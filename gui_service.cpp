// gui_service.cpp — GUI/CLI 共用控制服务层实现
// 实现依据 GUI_DESIGN_SPEC.md；行为边界见 gui_service.h 头注释。
// 关键设计：
//   - 全部状态存内存，snapshot() 只读拷贝，不落盘（GUI 面板周期刷新）。
//   - 审计日志 logs/audit_*.jsonl 逐条 flush；m_source 区分 GUI(HUMAN)/CLI(AGENT)。
//   - 服务函数不发 wprintln；错误经 std::string& err 返回（GBK 控制台/UTF-8 GUI 均可显示）。
//   - 任务线程统一为 detach 线程 + 原子标志；运动路径复用 motion.cpp 原函数，
//     task::progress 钩子在 motion.cpp 内更新，GUI 侧只读快照。
#include "gui_service.h"
#include "gui_trail.h"

#include <atomic>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <direct.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>

namespace gs {

// ---------------- 内部状态 ----------------
namespace {

std::recursive_mutex g_mu;            // 串口写 + 任务状态 + 审计文件（递归锁：session_id 等嵌套调用安全）

std::string g_source = "GUI";
std::string g_actor   = "HUMAN";
std::string g_session;
uint64_t    g_req_seq = 0;
uint64_t    g_op_seq  = 0;
std::string g_jsonl_path;
FILE*       g_jsonl   = nullptr;

SerialPort  g_port;
std::string g_port_name;               // 用户输入形式（如 COM3）

// 异步连接状态（open 在工作线程执行，避免 UI 线程内核阻塞）
std::atomic_bool g_connect_pending{ false };   // 工作线程正在打开串口
std::string      g_connect_state = "idle";     // idle/connecting/ok/fail（g_mu 保护）
std::string      g_connect_msg;                // 结果文本 UTF-8（g_mu 保护）

// 通信统计
std::string g_last_tx, g_last_rx;      // hex 大写带空格
std::string g_last_ack;                // valid / invalid / none
std::string g_last_op;                 // point / batch7 / ...
std::string g_last_err;
int         g_consecutive_fail = 0;
std::string g_last_comm;               // HH:MM:SS
std::string g_pose_ts;                 // 最后位姿更新时间
int         g_current_attempt = 1;

// 任务状态
std::atomic_bool g_task_active{ false };
std::atomic_bool g_task_cancel{ false };
std::thread      g_task_thread;
std::string      g_task_stage = "空闲";      // 预热/蘸墨/书写/翻页/描边/空闲
int              g_page_no = 0;               // 当前书写页 1-based（0=无任务/未开始）
int              g_page_total = 0;            // 任务总页数
int              g_chars_total = 0;
int              g_chars_done  = 0;
std::string      g_task_text;                 // UTF-8
std::string      g_task_chars;                // UTF-8（书写序列）
int              g_plan_cols = 0;
float            g_plan_char_size = 0;
float            g_plan_spacing = 0;
size_t           g_traj_total = 0;
size_t           g_traj_done  = 0;
bool             g_pen_down   = false;
bool             g_auto_draw_used = false;
std::string      g_task_error;
int              g_dip_count = 0;

// —— 自由拖拽排版 + 分页状态（GUI 书写页；仅本层使用，随 robot_config.json 持久化）—— //
// g_full_all  = 过滤后的全量有效文本（不截断；容量缩小时也不丢，放大后可无损恢复）；
// g_full_text = 当前实际参与书写的子串（= g_full_all 截断到容量），分页与书写的依据。
std::wstring             g_full_all;
std::wstring             g_full_text;               // 截断后的实际书写文本（宽字符）
std::vector<PageEntry>   g_pages;                   // 每页文本 + 每字左下角世界坐标
float                    g_free_char_size = SINGLE_CHAR_MIN;  // 全局字号 mm（≥60，各页共用）
int                      g_glyph_orient   = 0;      // 字体朝向 0..3（各页共用）
int                      g_page_chars  = 5;         // 每页字数 1~50
int                      g_page_count  = 1;         // 总页数 1~20
int                      g_page_turn_wait_ms = 3000;// 翻页模拟等待时长（蓝牙未接入）
int                      g_edit_page   = 0;         // 编辑页游标（空闲时 GUI 拖拽所在页）
std::atomic_int          g_write_page{ 0 };         // 书写页游标（任务线程正在写的页，UI 只读）
std::atomic_bool         g_task_finished{ false };  // 本进程至少完成/中止过一次任务（叠画门控）
PageTurnFn g_page_turn;   // 空 = 默认模拟等待（调用处兜底 page_turn_wait_locked）
constexpr float      REACH_X = 162.0f;            // 未标定四角时的可达回退半宽（真机实测 X±162）
constexpr float      REACH_Y = 85.0f;             // 未标定四角时的可达回退半高（真机实测 Y±85）

// 翻页模拟：持锁分片睡眠（锁序 g_mu → trail 不受影响；snapshot 亦等锁，UI 随 500ms 定时器
// 显示“翻页”阶段）。每 50ms 轮询取消/急停，命中即中止且不翻页（用户选定安全语义：立即中止）。
// ★必须在持有 g_mu 的线程调用（任务线程）。蓝牙接入后可被注入回调整体替代。
bool page_turn_wait_locked() {
    const int total = g_page_turn_wait_ms;
    int waited = 0;
    while (waited < total) {
        if (g_task_cancel || g_estop) return false;
        int chunk = total - waited < 50 ? total - waited : 50;
        std::this_thread::sleep_for(std::chrono::milliseconds(chunk));
        waited += chunk;
    }
    return !(g_task_cancel || g_estop);
}

std::string now_compact() {   // 20260917_101520
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tmv{}; localtime_s(&tmv, &t);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d%02d%02d_%02d%02d%02d",
        tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    return buf;
}
std::string now_iso() {       // 2026-09-17T10:15:20.123
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tmv{}; localtime_s(&tmv, &t);
    int ms = int(std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count() % 1000);
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03d",
        tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec, ms);
    return buf;
}
std::string now_hms() {
    std::time_t t = std::time(nullptr);
    std::tm tmv{}; localtime_s(&tmv, &t);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d", tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    return buf;
}

std::string hex_str(const uint8_t* p, size_t n) {
    std::ostringstream oss;
    for (size_t i = 0; i < n; ++i)
        oss << std::hex << std::uppercase << std::setw(2) << std::setfill('0') << int(p[i]) << " ";
    return oss.str();
}

std::string next_req() {
    char buf[32]; std::snprintf(buf, sizeof(buf), "req-%llu", (unsigned long long)++g_req_seq);
    return buf;
}

// 写审计事件（g_mu 已由调用方持有）
void audit_locked(const json& ev) {
    if (!g_jsonl) {
        _mkdir("logs");
        g_jsonl_path = "logs/audit_" + now_compact() + ".jsonl";
        g_jsonl = std::fopen(g_jsonl_path.c_str(), "a");
        if (g_jsonl) {
            std::fprintf(g_jsonl, "%s\n",
                json{ {"event", "session"}, {"timestamp", now_iso()},
                      {"session_id", g_session}, {"source", g_source}, {"actor", g_actor},
                      {"log_file", g_jsonl_path} }.dump().c_str());
        }
    }
    if (!g_jsonl) return;
    std::fprintf(g_jsonl, "%s\n", ev.dump().c_str());
    std::fflush(g_jsonl);
}

// operation 事件（request_id 绑定当前触发请求）
json op_event(const std::string& op, const Point* p) {
    json ev;
    ev["timestamp"]   = now_iso();
    ev["source"]      = g_source;
    ev["actor"]       = g_actor;
    ev["session_id"]  = g_session;
    ev["request_id"]  = next_req();
    ev["operation_id"]= std::string("op-") + now_compact() + "-" + std::to_string(++g_op_seq);
    ev["mode"]        = g_dryRun ? "DRYRUN" : (g_port.is_open() ? "REAL" : "OFFLINE");
    ev["operation"]   = op;
    if (p) ev["target_pose"] = { p->x, p->y, p->z, p->isPenDown, int(p->speed) };
    return ev;
}

} // anonymous namespace

// ---------------- 对外实现 ----------------
void set_source(const std::string& source, const std::string& actor) {
    std::lock_guard<std::recursive_mutex> lk(g_mu);
    g_source = source; g_actor = actor;
}
std::string session_id() {
    std::string sid;
    {
        std::lock_guard<std::recursive_mutex> lk(g_mu);
        if (g_session.empty()) g_session = "sess-" + now_compact();
        sid = g_session;
    }
    return sid;
}
void audit_write(const json& event) {
    std::lock_guard<std::recursive_mutex> lk(g_mu);
    json ev = event;
    if (!ev.contains("timestamp")) ev["timestamp"] = now_iso();
    if (!ev.contains("source"))    ev["source"] = g_source;
    if (!ev.contains("actor"))     ev["actor"]  = g_actor;
    if (!ev.contains("session_id"))ev["session_id"] = g_session;
    audit_locked(ev);
}
std::string jsonl_path() {
    std::lock_guard<std::recursive_mutex> lk(g_mu);
    if (g_jsonl_path.empty()) { _mkdir("logs"); g_jsonl_path = "logs/audit_" + now_compact() + ".jsonl"; }
    return g_jsonl_path;
}

// ---------------- device 钩子 ----------------
namespace device {

void log_send_point(const Point& p, const uint8_t* tx, size_t txlen,
                    const uint8_t* rx, size_t rxlen,
                    bool ack_valid, bool ok, const char* error_code) {
    std::lock_guard<std::recursive_mutex> lk(g_mu);
    g_last_tx = hex_str(tx, txlen);
    g_last_rx = hex_str(rx, rxlen);
    g_last_op = "point";
    g_last_ack = ack_valid ? "valid" : "none";
    g_last_err = error_code ? error_code : "";
    g_last_comm = now_hms();
    if (ok) g_consecutive_fail = 0;
    else if (!g_dryRun) ++g_consecutive_fail;

    json ev = op_event("send_point", &p);
    ev["tx_frame"]      = g_last_tx;
    ev["rx_frame"]      = g_last_rx;
    ev["device_response"] = ok ? "ACK" : (g_dryRun ? "DRY" : "NO-RESP");
    ev["ack_valid"]     = ack_valid;
    ev["result"]        = ok ? "ok" : "fail";
    ev["retry_count"]   = g_current_attempt - 1;
    if (!g_last_err.empty()) ev["error_code"] = g_last_err;
    ev["software_pose"] = { g_last_pose.x, g_last_pose.y, g_last_pose.z, g_last_pose.isPenDown };
    if (g_pose_init) ev["pose_ts"] = g_pose_ts;
    audit_locked(ev);
    if (g_task_active) trail::addCommanded(p);   // ★实时轨迹：仅任务运行期记录已下发点（软件位姿）
}

void log_send_batch7(const std::vector<Point>& pts, size_t n,
                     const uint8_t* tx, size_t txlen,
                     const uint8_t* rx, size_t rxlen,
                     bool ack_valid, bool ok, const char* error_code, int attempt) {
    std::lock_guard<std::recursive_mutex> lk(g_mu);
    g_last_tx = hex_str(tx, txlen);
    g_last_rx = hex_str(rx, rxlen);
    g_last_op = "batch7";
    g_last_ack = ack_valid ? "valid" : (ok ? "valid" : "none");
    g_last_err = error_code ? error_code : "";
    g_last_comm = now_hms();
    if (ok) g_consecutive_fail = 0;
    else if (!g_dryRun) ++g_consecutive_fail;

    const Point* p = pts.empty() ? nullptr : &pts[std::min(n, size_t(7)) - 1];
    json ev = op_event("send_batch7", p);
    json arr = json::array();
    for (size_t i = 0; i < n && i < 7; ++i)
        arr.push_back({ pts[i].x, pts[i].y, pts[i].z, pts[i].isPenDown, int(pts[i].speed) });
    ev["parameters"]   = { { "points", arr } };
    ev["tx_frame"]     = g_last_tx;
    ev["rx_frame"]     = g_last_rx;
    ev["device_response"] = g_dryRun ? "DRY" : (ok ? "ACK" : "NO-RESP");
    ev["ack_valid"]    = ack_valid;
    ev["result"]       = ok ? "ok" : "fail";
    ev["retry_count"]  = attempt - 1;
    if (!g_last_err.empty()) ev["error_code"] = g_last_err;
    ev["software_pose"] = { g_last_pose.x, g_last_pose.y, g_last_pose.z, g_last_pose.isPenDown };
    if (g_pose_init) ev["pose_ts"] = g_pose_ts;
    audit_locked(ev);
    if (g_task_active) trail::addCommandedBatch(pts, n);   // ★实时轨迹：描边/批量点同样记录
}

void note_pose_updated() {
    std::lock_guard<std::recursive_mutex> lk(g_mu);
    g_pose_ts = now_hms();
}
void set_attempt(int attempt) {
    std::lock_guard<std::recursive_mutex> lk(g_mu);
    g_current_attempt = std::max(1, attempt);
}
std::string pose_ts() {
    std::lock_guard<std::recursive_mutex> lk(g_mu);
    return g_pose_ts;
}
json last_comm() {
    std::lock_guard<std::recursive_mutex> lk(g_mu);
    return json{ { "tx", g_last_tx }, { "rx", g_last_rx }, { "ack", g_last_ack },
                 { "op", g_last_op }, { "err", g_last_err }, { "at", g_last_comm },
                 { "fail", g_consecutive_fail } };
}

} // namespace device

// ---------------- task 钩子 ----------------
namespace task {

void begin(const std::wstring& chars, const TextPlan& plan, const char* kind) {
    std::lock_guard<std::recursive_mutex> lk(g_mu);
    g_chars_total = (int)chars.size();
    g_chars_done = 0;
    g_traj_total = g_traj_done = 0;
    g_pen_down = false;
    g_plan_cols = plan.cols;
    g_plan_char_size = plan.used_S;
    g_plan_spacing = plan.used_sp;
    g_task_chars = w_to_utf8(chars);
    g_task_error.clear();
    (void)kind;
}
// 分页任务：每页开始。页内“字符 x/y、轨迹点”计数复位（GUI 显示当前页进度），并记录页码。
void page_begin(int page_idx0, int page_total, int chars_in_page) {
    std::lock_guard<std::recursive_mutex> lk(g_mu);
    g_page_no = page_idx0 + 1;
    g_page_total = page_total;
    g_chars_total = chars_in_page;
    g_chars_done = 0;
    g_traj_total = g_traj_done = 0;
    g_pen_down = false;
}
void stage(const char* label) {
    std::lock_guard<std::recursive_mutex> lk(g_mu);
    g_task_stage = label;
}
void char_done(int idx) {
    std::lock_guard<std::recursive_mutex> lk(g_mu);
    if (idx >= 0) g_chars_done = idx + 1;
}
void traj_add_total(size_t n) {
    std::lock_guard<std::recursive_mutex> lk(g_mu);
    g_traj_total += n;
}
void traj_add_done(size_t n, bool pen_down) {
    std::lock_guard<std::recursive_mutex> lk(g_mu);
    g_traj_done += n;
    g_pen_down = pen_down;
}
void dip_done() {
    std::lock_guard<std::recursive_mutex> lk(g_mu);
    ++g_dip_count;
}
void end(bool ok, const char* state, const char* error) {
    std::lock_guard<std::recursive_mutex> lk(g_mu);
    g_task_error = error ? error : "";
    json ev = op_event("task_end", nullptr);
    ev["parameters"] = { { "text", g_task_text } };
    ev["result"] = ok ? "ok" : state;
    if (!g_task_error.empty()) ev["error_code"] = g_task_error;
    ev["progress"] = { { "chars", g_chars_done }, { "chars_total", g_chars_total },
                       { "traj", g_traj_done }, { "traj_total", g_traj_total } };
    audit_locked(ev);
}
void reset() {
    std::lock_guard<std::recursive_mutex> lk(g_mu);
    g_task_stage = "空闲"; g_chars_total = g_chars_done = 0;
    g_page_no = g_page_total = 0;
    g_traj_total = g_traj_done = 0; g_pen_down = false;
    g_task_error.clear(); g_task_chars.clear(); g_task_text.clear();
    g_dip_count = 0;
}

} // namespace task

json snapshot() {
    std::lock_guard<std::recursive_mutex> lk(g_mu);
    json j;
    j["timestamp"] = now_iso();
    j["dry_run"] = g_dryRun;
    j["connected"] = g_port.is_open();
    j["port"] = g_port_name;
    if (g_port.is_open()) {
        j["protocol"] = "RS485 / Modbus RTU";
        j["params"] = "9600 / 8E1";
    }
    j["connect"] = json{
        { "pending", (bool)g_connect_pending },
        { "state", g_connect_state },
        { "message", g_connect_msg } };
    j["pose"] = json{ { "x", g_last_pose.x }, { "y", g_last_pose.y }, { "z", g_last_pose.z },
                      { "pen_down", g_last_pose.isPenDown },
                      { "valid", g_pose_init }, { "ts", g_pose_ts } };
    j["comm"] = json{ { "tx", g_last_tx }, { "rx", g_last_rx }, { "ack", g_last_ack },
                      { "err", g_last_err }, { "at", g_last_comm },
                      { "fail", g_consecutive_fail }, { "op", g_last_op } };
    j["mode"] = g_task_active ? g_task_stage : (g_estop ? std::string("急停") : std::string("空闲"));
    j["estop"] = (bool)g_estop;
    j["session"] = g_session;
    j["jsonl"] = g_jsonl_path;
    j["log"] = g_logPath;
    j["task"] = json{
        { "active", (bool)g_task_active },
        { "stage", g_task_stage },
        { "page_no", g_page_no }, { "page_total", g_page_total },
        { "display_page", g_task_active ? (int)g_write_page.load() : g_edit_page },
        { "chars_done", g_chars_done }, { "chars_total", g_chars_total },
        { "chars", g_task_chars },
        { "traj_done", g_traj_done }, { "traj_total", g_traj_total },
        { "pen_down", g_pen_down },
        { "auto_draw", g_autoDraw }, { "enable_dip", g_enableDip },
        { "dip_done", g_dip_count },
        { "text", g_task_text },
        { "error", g_task_error } };
    j["cfg"] = json{
        { "speed", SPEED_LEVEL }, { "char_spacing", CHAR_SPACING },
        { "z_offset", Z_OFFSET },
        { "auto_draw", g_autoDraw }, { "high_quality", g_highQuality },
        { "enable_dip", g_enableDip }, { "enable_dunbi", g_enableDunbi }, { "log", g_logEnable },
        { "char_size", ACTIVE_CHAR_SIZE }, { "plan_cols", g_plan_cols },
        { "pose_source", "软件位姿（最后有效 ACK）" },
        { "safe_area", { g_safeArea.xmin, g_safeArea.xmax, g_safeArea.ymin, g_safeArea.ymax } },
        { "z_up", Z_UP }, { "z_normal", Z_DOWN_NORMAL },
        { "writing_plane_z", g_writing_plane_z }, { "writing_plane_valid", g_writing_plane_valid },
        { "layout_mode", g_layout_mode }, { "layout_char_size", g_lm_char_size },
        { "layout_cols", g_lm_cols }, { "layout_top_ratio", g_lm_top_ratio },
        { "layout_row_spacing", g_lm_row_spacing }, { "write_dir", g_write_dir },
        { "free_char_size", g_free_char_size }, { "glyph_orient", g_glyph_orient },
        { "page_chars", g_page_chars }, { "page_count", g_page_count },
        { "edit_page", g_edit_page }, { "page_turn_wait_ms", g_page_turn_wait_ms },
        { "task_finished", (bool)g_task_finished },
        { "text_total", (int)g_full_all.size() },
        { "text_written", (int)g_full_text.size() } };
    j["memory"] = "未接入";
    j["storage"] = "未接入";
    j["battery"] = "未接入";
    return j;
}

// ---------------- 串口连接 ----------------
// 串口枚举：只做查询，绝不打开端口。
// 打开一个无响应的 COM 端口会让调用线程在内核里长时间阻塞（实测 COM4 阻塞 5s、COM5 无限阻塞），
// 本函数在 GUI 中由 UI 线程调用，一旦阻塞整个界面就“未响应”，因此禁止用 CreateFile 探测。
static bool comDigitsOk(const std::wstring& w) {
    if (w.size() <= 3) return false;
    for (size_t i = 3; i < w.size(); ++i)
        if (w[i] < L'0' || w[i] > L'9') return false;
    return true;
}
std::vector<std::string> list_serial_ports() {
    std::set<std::string> out;
    // 1) DOS 设备名列表（纯查询；缓冲区不足时按需扩容重试）
    std::vector<WCHAR> buf(8192);
    DWORD n = 0;
    for (int attempt = 0; attempt < 5; ++attempt) {
        n = QueryDosDeviceW(nullptr, buf.data(), (DWORD)buf.size());
        if (n > 0 && n < buf.size()) break;
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) { n = 0; break; }
        buf.resize(buf.size() * 2);
    }
    if (n > 0) {
        const WCHAR* p = buf.data();
        while (*p) {
            std::wstring w(p);
            if (w.rfind(L"COM", 0) == 0 && comDigitsOk(w)) out.insert("COM" + std::to_string(_wtoi(w.c_str() + 3)));
            p += w.size() + 1;
        }
    }
    // 2) 注册表补充：部分虚拟串口不会出现在 DOS 设备列表中
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DEVICEMAP\\SERIALCOMM", 0, KEY_READ, &k) == ERROR_SUCCESS) {
        for (DWORD i = 0; ; ++i) {
            WCHAR name[512]; BYTE data[512];
            DWORD nl = 512, dl = sizeof(data), ty = 0;
            if (RegEnumValueW(k, i, name, &nl, nullptr, &ty, data, &dl) != ERROR_SUCCESS) break;
            if ((ty == REG_SZ || ty == REG_EXPAND_SZ) && dl >= sizeof(WCHAR)) {
                std::wstring com((const WCHAR*)data);
                if (com.rfind(L"COM", 0) == 0 && comDigitsOk(com)) out.insert("COM" + std::to_string(_wtoi(com.c_str() + 3)));
            }
        }
        RegCloseKey(k);
    }
    return std::vector<std::string>(out.begin(), out.end());
}

// 异步连接：open（蓝牙虚拟口会在 CreateFileW 内核阻塞）放工作线程，打开期间不持 g_mu。
void connect_async(const std::string& port_name) {
    // 前置校验在 UI 线程持锁快速完成；open 与写结果分离，绝不跨 open 持锁。
    {
        std::lock_guard<std::recursive_mutex> lk(g_mu);
        if (g_task_active) {                       // 任务运行中禁止改连接，立即给失败结果
            g_connect_pending = false;
            g_connect_state = "fail";
            g_connect_msg = "任务运行中，禁止修改连接";
            return;
        }
        if (g_connect_pending) return;             // 已有连接建立中，忽略重复点击（防叠加挂起线程）
        session_id();                              // 确保会话头（持锁调用）
        g_connect_pending = true;
        g_connect_state = "connecting";
        g_connect_msg.clear();
        g_port_name = port_name;                   // 先记录目标口，供面板显示“连接中”
    }
    std::thread([port_name]() {
        // ★阻塞点：此处不持 g_mu，UI 线程与 500ms snapshot() 不受影响
        bool ok = g_port.open(normalizeComName(utf8_to_w(port_name)),
                              BAUDRATE, EVENPARITY, 8, ONESTOPBIT);
        std::lock_guard<std::recursive_mutex> lk(g_mu);   // 仅记录结果/审计时持锁
        json ev = op_event("connect", nullptr);
        ev["parameters"] = { { "port", port_name }, { "params", "9600/8E1" } };
        ev["result"] = ok ? "ok" : "fail";
        if (!ok) ev["error_code"] = "open_failed";
        audit_locked(ev);
        if (ok) {
            g_consecutive_fail = 0; g_last_ack.clear(); g_last_comm = now_hms();
            g_connect_state = "ok";
            g_connect_msg = "已连接 " + port_name + "（9600/8E1）。";
        } else {
            g_port_name.clear();
            g_connect_state = "fail";
            g_connect_msg = "串口打开失败（端口不存在/被占用/蓝牙虚拟口不可连接）";
        }
        g_connect_pending = false;
    }).detach();
}
bool connect_pending() { return g_connect_pending; }

void disconnect() {
    std::lock_guard<std::recursive_mutex> lk(g_mu);
    if (g_task_active) return;
    bool was = g_port.is_open();
    g_port.close();
    g_port_name.clear();
    if (was) {
        json ev = op_event("disconnect", nullptr);
        ev["result"] = "ok";
        audit_locked(ev);
    }
}
bool is_connected() {
    return g_port.is_open();
}
SerialPort& port() { return g_port; }

// ---------------- 快捷操作 ----------------
std::wstring query_pose_line() {
    if (!g_pose_init) return L"[位姿] 本次运行尚未成功发送过点，无已知位姿。";
    std::wstringstream ws;
    ws << L"[位姿] X=" << g_last_pose.x << L" Y=" << g_last_pose.y << L" Z=" << g_last_pose.z
        << (g_last_pose.isPenDown ? L" [落笔]" : L" [抬笔]")
        << L" 来源=" << mb2w(g_last_pose.zType);
    return ws.str();
}

bool go_center(std::string& err) {
    std::lock_guard<std::recursive_mutex> lk(g_mu);
    if (g_task_active) { err = "任务运行中，请先停止任务"; return false; }
    session_id();
    bool ok = g_port.sendPointRetry(Point{ g_center_x, g_center_y, g_center_z, false,
                                            (uint8_t)SPEED_LEVEL, "UP-CENTER" });
    err = ok ? "" : "复位发送失败（坐标越界或无应答）";
    return ok;
}

bool send_test_point(std::string& err) {
    std::lock_guard<std::recursive_mutex> lk(g_mu);
    if (g_task_active) { err = "任务运行中，请先停止任务"; return false; }
    session_id();
    float x = g_center_x + 30.f, y = g_center_y - 30.f, z = Z_UP;
    if (!inXYRange(x, y, g_devLimit) || !inZRange(z + Z_OFFSET)) { err = "测试点越界"; return false; }
    bool ok = g_port.sendPointRetry(Point{ x, y, z, false, (uint8_t)SPEED_LEVEL, "TEST" });
    err = ok ? "" : "测试点发送失败";
    return ok;
}

bool heartbeat(std::string& err) {
    std::lock_guard<std::recursive_mutex> lk(g_mu);
    if (g_task_active) { err = "任务运行中"; return false; }
    if (!g_dryRun && !g_port.is_open()) { err = "串口未连接"; return false; }
    session_id();
    bool ok = g_port.sendPointRetry(Point{ g_center_x, g_center_y, Z_UP, false,
                                            (uint8_t)SPEED_LEVEL, "HEARTBEAT" });
    err = ok ? "" : "心跳发送失败";
    return ok;
}

// —— 书写平面：把机械臂移到 (0,0,z) 悬停，供目视确认笔尖实际接触高度（不书写）—— //
bool preview_writing_plane(float z, std::string& err) {
    std::lock_guard<std::recursive_mutex> lk(g_mu);
    if (g_task_active) { err = "任务运行中，请先停止任务"; return false; }
    if (!g_dryRun && !g_port.is_open()) { err = "串口未连接"; return false; }
    if (!std::isfinite(z)) { err = "Z 无效"; return false; }
    if (!inXYRange(0.f, 0.f, g_devLimit) || !inZRange(z + Z_OFFSET)) {
        std::ostringstream os; os << "Z 越界（有效 " << g_z_bottom << "~" << g_z_top
            << "，含偏移后 Z=" << (z + Z_OFFSET) << "）";
        err = os.str(); return false;
    }
    session_id();
    bool ok = g_port.sendPointRetry(Point{ 0.f, 0.f, z, false, (uint8_t)SPEED_LEVEL, "PLANE-PROBE" });
    err = ok ? "" : "悬停点发送失败";
    return ok;
}

// —— 书写平面：保存即生效并持久化；后续书写所有落笔点用该固定 Z —— //
bool set_writing_plane(float z, std::string& err) {
    if (g_task_active) { err = "任务运行中，请先停止任务"; return false; }
    if (!std::isfinite(z)) { err = "Z 无效"; return false; }
    if (!inZRange(z + Z_OFFSET)) {
        std::ostringstream os; os << "Z 越界（有效 " << g_z_bottom << "~" << g_z_top
            << "，含偏移后 Z=" << (z + Z_OFFSET) << "）";
        err = os.str(); return false;
    }
    {
        std::lock_guard<std::recursive_mutex> lk(g_mu);
        session_id();
        g_writing_plane_z = z;
        g_writing_plane_valid = true;
        json ev = op_event("set_writing_plane", nullptr);
        ev["parameters"] = { { "z", z } };
        ev["result"] = "ok";
        audit_locked(ev);
    }
    cfg_save();
    return true;
}

// —— 四角标定：抬笔移到 (x,y) 预览某角（真机会移动到该处；DRYRUN 仅打帧）—— //
bool preview_corner(float x, float y, std::string& err) {
    std::lock_guard<std::recursive_mutex> lk(g_mu);
    if (g_task_active) { err = "任务运行中，请先停止任务"; return false; }
    if (!g_dryRun && !g_port.is_open()) { err = "串口未连接"; return false; }
    if (!std::isfinite(x) || !std::isfinite(y)) { err = "坐标无效"; return false; }
    // 预览落在“已固定的书写平面”高度（笔尖触纸，便于核对角位）；未设平面则用抬笔高度。
    float z = g_writing_plane_valid ? g_writing_plane_z : Z_UP;
    if (!inXYRange(x, y, g_devLimit) || !inZRange(z + Z_OFFSET)) {
        std::ostringstream os; os << "预览点越界（X " << g_devLimit.xmin << "~" << g_devLimit.xmax
            << "，Y " << g_devLimit.ymin << "~" << g_devLimit.ymax
            << "，含偏移后 Z=" << (z + Z_OFFSET) << " 有效 " << g_z_bottom << "~" << g_z_top << "）";
        err = os.str(); return false;
    }
    session_id();
    bool ok = g_port.sendPointRetry(Point{ x, y, z, false, (uint8_t)SPEED_LEVEL, "CORNER-PREVIEW" });
    err = ok ? "" : "预览移动失败（无应答）";
    return ok;
}

// —— 四角标定：固定第 i 个角并持久化（robot_config.json，下次开 GUI 沿用）—— //
bool save_corner(int i, float x, float y, std::string& err) {
    if (i < 0 || i >= (int)trail::kCorners) { err = "角序号无效"; return false; }
    if (!std::isfinite(x) || !std::isfinite(y)) { err = "坐标无效"; return false; }
    if (!inXYRange(x, y, g_devLimit)) { err = "坐标越界，未保存"; return false; }
    {
        std::lock_guard<std::recursive_mutex> lk(g_mu);
        session_id();
        trail::setCorner(i, x, y);
        json ev = op_event("corner_save", nullptr);
        ev["parameters"] = { { "index", i }, { "x", x }, { "y", y } };
        ev["result"] = "ok";
        audit_locked(ev);
    }
    cfg_save();
    return true;
}

void clear_corners() {
    {
        std::lock_guard<std::recursive_mutex> lk(g_mu);
        session_id();
        trail::clearCorners();
        json ev = op_event("corner_clear", nullptr);
        ev["result"] = "ok";
        audit_locked(ev);
    }
    cfg_save();
}

void request_estop() {
    g_estop = true;
    std::lock_guard<std::recursive_mutex> lk(g_mu);
    json ev = op_event("estop", nullptr);
    ev["result"] = "triggered";
    audit_locked(ev);
}
void clear_estop() {
    g_estop = false;
    std::lock_guard<std::recursive_mutex> lk(g_mu);
    json ev = op_event("estop_clear", nullptr);
    ev["result"] = "ok";
    audit_locked(ev);
}
bool abort_task() {
    if (!g_task_active) return false;
    g_task_cancel = true;
    g_estop = true;
    std::lock_guard<std::recursive_mutex> lk(g_mu);
    json ev = op_event("task_abort", nullptr);
    ev["result"] = "requested";
    audit_locked(ev);
    return true;
}
bool task_canceled() { return g_task_cancel; }

// 前向声明：分页自由布局（定义见下方“自由拖拽排版 + 分页”段），供 preflight 复用。
static std::wstring filter_page_chars(const std::wstring& wtext);
static void ensure_page_grid(PageEntry& e, float S);
static void rebuild_pages(const std::wstring& full);
static bool prepare_page_plan(const PageEntry& e, TextPlan& plan);
static bool grid_fits(int n, float S);

// ---------------- 写字任务 ----------------
// 预检（分页）：容量截断 → 重新切页（文本未变的页保留拖拽坐标）→ 逐页校验“有字且塞得进视野”。
json preflight(const std::string& utf8_text) {
    std::lock_guard<std::recursive_mutex> lk(g_mu);
    session_id();
    std::wstring all = filter_page_chars(utf8_to_w(utf8_text));
    int cap = g_page_chars * g_page_count;
    g_full_all  = all;
    g_full_text = all.substr(0, std::min((size_t)cap, all.size()));
    rebuild_pages(g_full_text);
    int written = (int)g_full_text.size();
    float x0, y0, x1, y1; view_bounds(x0, y0, x1, y1);
    json r;
    r["mode"] = "free";
    r["orient"] = g_glyph_orient;
    r["view"] = json{ { "x0", x0 }, { "y0", y0 }, { "x1", x1 }, { "y1", y1 } };
    r["page_total"]  = g_page_count;
    r["written_pages"] = (written + g_page_chars - 1) / g_page_chars;
    r["page_chars"]  = g_page_chars;
    r["text_total"]  = (int)all.size();
    r["truncated"]   = (int)all.size() > cap;
    bool has = written > 0;
    bool fits = true;
    int  bad_page = 0;
    int  wPages = (written + g_page_chars - 1) / g_page_chars;   // 有字要写的页数
    for (int k = 0; k < wPages && k < (int)g_pages.size(); ++k) {
        const auto& e = g_pages[k];
        if (e.text.empty()) { fits = false; if (!bad_page) bad_page = (int)k + 1; break; }
        if (!grid_fits((int)e.text.size(), g_free_char_size)) { fits = false; if (!bad_page) bad_page = (int)k + 1; }
    }
    r["layout_ok"] = has && fits;
    if (has && fits) {
        r["char_count"]   = (int)g_pages.size();
        r["chars_planned"]= written;
        r["char_size"]    = g_free_char_size;
        r["speed"]        = SPEED_LEVEL;
        r["dry_run"]      = g_dryRun;
        r["layout"]       = "自由拖拽排版 · 分页（所见即所得）";
        r["text"]         = w_to_utf8(g_full_text.substr(0, (size_t)written));
    }
    else {
        r["error"] = !has ? "无有效汉字" : "第 " + std::to_string(bad_page) + " 页排版不可行（页内无字或字号超出视野）";
        r["err_code"] = !has ? LAY_NO_CHARS : LAY_GRID;
    }
    json ev = op_event("task_preflight", nullptr);
    ev["parameters"] = { { "text", utf8_text }, { "mode", r["mode"] }, { "orient", g_glyph_orient },
                         { "page_chars", g_page_chars }, { "page_count", g_page_count } };
    ev["result"] = (has && fits) ? "ok" : "fail";
    if (!(has && fits)) { ev["error_code"] = "layout"; ev["err_code"] = r.value("err_code", 0); }
    audit_locked(ev);
    return r;
}

// ---------------- 自由拖拽排版 + 分页（GUI 书写页） ----------------
// 固定视野：优先四角外接框（gs::trail），未标定（角不足 4）回退可达框 X±162/Y±85。

// 过滤出全量有效字符序列：按行分组（\n/\r），行内仅保留汉字/标点，再按行顺序拼接。
// 与旧 run_task_thread 的行过滤语义一致（不处理行间空段；光标等非有效字符被丢弃）。
static std::wstring filter_page_chars(const std::wstring& wtext) {
    std::wstring out;
    std::vector<std::wstring> lines;
    std::wstring cur;
    for (wchar_t c : wtext) {
        if (c == L'\n' || c == L'\r') { if (!cur.empty()) { lines.push_back(cur); cur.clear(); } }
        else cur.push_back(c);
    }
    if (!cur.empty()) lines.push_back(cur);
    for (auto& ln : lines)
        for (wchar_t c : ln) if (isCJKOrPunct(c)) out.push_back(c);
    return out;
}

void view_bounds(float& x0, float& y0, float& x1, float& y1) {
    float mnx, mny, mxx, mxy;
    if (trail::cornersBounds(mnx, mny, mxx, mxy) && trail::cornerCount() >= 4) {
        x0 = mnx; y0 = mny; x1 = mxx; y1 = mxy;
    } else {
        x0 = -REACH_X; y0 = -REACH_Y; x1 = REACH_X; y1 = REACH_Y;
    }
    if (x1 < x0) std::swap(x0, x1);
    if (y1 < y0) std::swap(y0, y1);
}

// 把字块左下角夹取到视野内（保证整块 S×S 落在 [x0,x1]×[y0,y1]）。
static void clamp_cell(float& x, float& y, float S, float x0, float y0, float x1, float y1) {
    float lo_x = x0, hi_x = x1 - S, lo_y = y0, hi_y = y1 - S;
    if (hi_x < lo_x) hi_x = lo_x;                 // 字比视野还大：贴左边
    if (hi_y < lo_y) hi_y = lo_y;
    x = clampf(x, lo_x, hi_x);
    y = clampf(y, lo_y, hi_y);
}

// 可行性判定：**允许字块互相重叠**——只要单个字块塞得进固定视野（字号 ≤ 视野宽/高）即算可行。
// 字多到排不下时不再判不可行，而是重叠摆放、交给用户拖拽分开；仅当单字比视野还大才不可行。
static bool grid_fits(int n, float S) {
    if (n <= 0) return true;
    float x0, y0, x1, y1; view_bounds(x0, y0, x1, y1);
    float vw = x1 - x0, vh = y1 - y0;
    return (S > 0 && S <= vw + 1e-3f && S <= vh + 1e-3f);
}

// 初始网格摆位：按字号 S 在视野内宽优先换行、整体居中；**始终生成 n 个字块**（放不下则夹取到
// 视野内、允许重叠），是否真放得下由 grid_fits 判定。S 非法时兜底最小字号，保证 out.size()==n。
static void compute_initial_grid(int n, float S, std::vector<Offset>& out) {
    out.clear();
    if (n <= 0) return;
    if (!(S > 0)) S = SINGLE_CHAR_MIN;
    float x0, y0, x1, y1; view_bounds(x0, y0, x1, y1);
    float vw = x1 - x0, vh = y1 - y0;
    const float g = 2.0f;
    int per = (int)std::floor((vw + g) / (S + g)); if (per < 1) per = 1; if (per > n) per = n;
    int rows = (n + per - 1) / per;
    float gridW = per * S + (per - 1) * g;
    float gridH = rows * S + (rows - 1) * g;
    float ox = x0 + std::max(0.f, (vw - gridW) / 2.0f);
    float topY = y1 - std::max(0.f, (vh - gridH) / 2.0f);
    out.reserve(n);
    for (int i = 0; i < n; ++i) {
        int r = i / per, c = i % per;
        float cx = ox + c * (S + g);
        float cy = topY - S - r * (S + g);           // 左下角 y
        clamp_cell(cx, cy, S, x0, y0, x1, y1);
        out.push_back(Offset{ cx, cy });
    }
}

// 把字块中心按朝向做整数旋转（方框 [0,S]×[0,S]，绕中心 (S/2,S/2)，y 轴向上）。
// orient: 0=0°、1=180°、2=90°CW、3=90°CCW。旋转后仍落在同一 S×S 方框内。
static void rotate_local(float& x, float& y, float S, int orient) {
    switch (orient) {
    case 1:  x = S - x; y = S - y; break;            // 180°
    case 2:  { float nx = S - y, ny = x; x = nx; y = ny; } break;  // 90° 顺时针
    case 3:  { float nx = y, ny = S - x; x = nx; y = ny; } break;  // 90° 逆时针
    default: break;                                  // 0°：不变
    }
}

// 重新切页：full 为**全量**有效文本（不截断；超出容量的页 text 为空），按 g_page_chars 顺序
// 切分成 g_page_count 页。同下标且文本一致的旧页保留其拖拽坐标（改页数/每页字数不丢摆位）。
static void rebuild_pages(const std::wstring& full) {
    std::vector<PageEntry> old;
    old.swap(g_pages);
    g_pages.clear();
    int n = (int)full.size();
    // 旧全文与各字所在旧页/页内下标（按首次出现位置建立映射，供字符级继承）
    std::wstring old_all;
    for (const auto& oe : old) old_all += oe.text;
    auto inherit_cell = [&](wchar_t ch, int fallback_idx) -> Offset {
        size_t p = old_all.find(ch);
        if (p == std::wstring::npos) return Offset{ 1e30f, 1e30f };   // 找不到→哨兵，交补网格
        // 定位该字在旧页中的坐标
        size_t acc = 0;
        for (const auto& oe : old) {
            if (p < acc + oe.text.size()) {
                size_t j = p - acc;
                if (j < oe.cells.size()) return oe.cells[j];
                break;
            }
            acc += oe.text.size();
        }
        (void)fallback_idx;
        return Offset{ 1e30f, 1e30f };
    };
    int used = 0;
    for (int k = 0; k < g_page_count; ++k) {
        PageEntry e;
        int take = std::max(0, std::min(g_page_chars, n - used));
        e.text = take > 0 ? full.substr((size_t)used, (size_t)take) : std::wstring();
        used += take;
        // ①同下标同文本 → 整页沿用旧坐标
        if (k < (int)old.size() && !e.text.empty()
            && old[k].text == e.text && old[k].cells.size() == e.text.size()) {
            e.cells = old[k].cells;
        }
        // ②文本变了（页数/每页字数/内容调整）→ 按字继承旧坐标；有任一字继承不到则整页重排，
        //   保证同页坐标风格一致（不做半继承的怪布局）。
        else if (!e.text.empty() && !old.empty()) {
            std::vector<Offset> inh;
            bool ok = true;
            for (size_t i = 0; i < e.text.size() && ok; ++i) {
                Offset c = inherit_cell(e.text[i], (int)i);
                if (c.x > 1e29f) ok = false;
                else inh.push_back(c);
            }
            if (ok && (int)inh.size() == (int)e.text.size()) e.cells = std::move(inh);
        }
        g_pages.push_back(std::move(e));
    }
    // 所有有字页统一补初始网格：任意页切过去即可拖拽（set_free_cell 不再因 cells 未生成而失败）
    for (auto& e : g_pages) ensure_page_grid(e, g_free_char_size);
    if (g_edit_page >= (int)g_pages.size()) g_edit_page = g_pages.empty() ? 0 : (int)g_pages.size() - 1;
    if (g_edit_page < 0) g_edit_page = 0;
}

// 页内缺字块坐标则按字号补初始网格（覆盖“文本变了”与“字号变了但 cells 仍在”两种情形）。
static void ensure_page_grid(PageEntry& e, float S) {
    if (e.text.empty()) { e.cells.clear(); return; }
    if (e.cells.size() != e.text.size())
        compute_initial_grid((int)e.text.size(), S, e.cells);
}

// 供 run_task_thread：按页生成 TextPlan（offsets 即该页每字左下角世界绝对坐标）。
// cells 数量不符时兜底补初始网格（任务期间 GUI 被冻结，理论不可达，纯防御）。
static bool prepare_page_plan(const PageEntry& e, TextPlan& plan) {
    if (e.text.empty()) return false;
    float x0, y0, x1, y1; view_bounds(x0, y0, x1, y1);
    std::vector<Offset> cells = e.cells;
    if (cells.size() != e.text.size())
        compute_initial_grid((int)e.text.size(), g_free_char_size, cells);
    plan = TextPlan();
    plan.ok = true;
    plan.used_S = g_free_char_size;
    plan.cols = (int)e.text.size(); plan.rows = 1;
    plan.dir = 0;
    plan.text_area = WorkArea{ 0.f, 0.f, 0.f, 0.f };   // 原点零：offsets 已是绝对世界坐标
    plan.offsets.clear();
    for (size_t i = 0; i < e.text.size(); ++i) {
        float cx = cells[i].x, cy = cells[i].y;
        clamp_cell(cx, cy, g_free_char_size, x0, y0, x1, y1);
        plan.offsets.push_back(Offset{ cx, cy });
    }
    return true;
}

bool set_free_char_size(float mm) {
    if (g_task_active) return false;
    if (!std::isfinite(mm)) return false;
    if (mm < SINGLE_CHAR_MIN) mm = SINGLE_CHAR_MIN;   // 60mm 比赛红线
    if (mm > 500.f) mm = 500.f;
    g_free_char_size = mm;
    // 分页场景：改字号**不再复位**已摆好的字块（避免毁掉多页手工布局），
    // 只给缺坐标的页补初始网格；越界的旧坐标由渲染/书写路径夹取回视野。
    for (auto& e : g_pages) ensure_page_grid(e, mm);
    cfg_save();
    return true;
}
bool set_free_cell(int idx, float x, float y) {
    if (g_task_active) return false;
    if (idx < 0 || g_edit_page < 0 || g_edit_page >= (int)g_pages.size()) return false;
    auto& e = g_pages[g_edit_page];
    if (idx >= (int)e.cells.size()) return false;
    if (!std::isfinite(x) || !std::isfinite(y)) return false;
    float x0, y0, x1, y1; view_bounds(x0, y0, x1, y1);
    clamp_cell(x, y, g_free_char_size, x0, y0, x1, y1);
    e.cells[idx] = Offset{ x, y };
    cfg_save();
    return true;
}
bool set_glyph_orient(int o) {
    if (g_task_active) return false;
    if (o < 0 || o > 3) return false;
    g_glyph_orient = o;
    cfg_save();
    return true;
}
int  glyph_orient() { return g_glyph_orient; }
void reset_canvas() {
    trail::reset();
    g_task_finished = false;    // 重置画布 → 可编辑叠画重新出现
}

// ---------------- 分页参数 / 页游标 ----------------
bool set_page_chars(int n) {
    if (g_task_active) return false;
    if (n < 1 || n > 50) return false;
    g_page_chars = n;
    int cap = g_page_chars * g_page_count;
    g_full_text = g_full_all.substr(0, std::min((size_t)cap, g_full_all.size()));
    rebuild_pages(g_full_text);          // 文本不变的页保留坐标
    cfg_save();
    return true;
}
int page_chars() { return g_page_chars; }
bool set_page_count(int n) {
    if (g_task_active) return false;
    if (n < 1 || n > 20) return false;
    g_page_count = n;
    int cap = g_page_chars * g_page_count;
    g_full_text = g_full_all.substr(0, std::min((size_t)cap, g_full_all.size()));
    rebuild_pages(g_full_text);
    cfg_save();
    return true;
}
int page_count() { return g_page_count; }
bool set_edit_page(int idx0) {
    if (g_task_active) return false;
    if (idx0 < 0 || idx0 >= (int)g_pages.size()) return false;
    g_edit_page = idx0;
    return true;
}
int edit_page() { return g_edit_page; }
int page_entry_count() { return (int)g_pages.size(); }
bool page_entry(int idx0, PageEntry& out) {
    if (idx0 < 0 || idx0 >= (int)g_pages.size()) return false;
    out = g_pages[idx0];
    return true;
}
void get_page_cells(int idx0, std::vector<Offset>& out) {
    out.clear();
    if (idx0 < 0 || idx0 >= (int)g_pages.size()) return;
    out = g_pages[idx0].cells;
}
void text_capacity(int& total_chars, int& written_chars) {
    int cap = g_page_chars * g_page_count;
    total_chars  = (int)g_full_all.size();
    written_chars = std::min(total_chars, cap);
}
int  display_page() { return g_task_active ? (int)g_write_page.load() : g_edit_page; }
bool page_drag_enabled() {
    return !g_task_active && display_page() == g_edit_page && trail::commandedSize() == 0;
}

// ---------------- 翻页预留接口（蓝牙未接入，默认模拟等待） ----------------
void set_page_turn_handler(PageTurnFn fn) {
    g_page_turn = fn ? std::move(fn) : PageTurnFn([](int, int) { return page_turn_wait_locked(); });
}
bool set_page_turn_wait_ms(int ms) {
    if (g_task_active) return false;
    if (ms < 500 || ms > 60000) return false;
    g_page_turn_wait_ms = ms;
    cfg_save();
    return true;
}
int page_turn_wait_ms() { return g_page_turn_wait_ms; }

// 实时排版预览：不写审计；据当前文本重切页（文本未变的页保留拖拽坐标），
// 返回【当前编辑页】的叠画字块与分页信息，供 GUI 书写页画布/翻页条即时刷新。
json layout_preview(const std::string& utf8_text) {
    std::lock_guard<std::recursive_mutex> lk(g_mu);
    std::wstring all = filter_page_chars(utf8_to_w(utf8_text));
    int cap = g_page_chars * g_page_count;
    g_full_all  = all;
    g_full_text = all.substr(0, std::min((size_t)cap, all.size()));
    rebuild_pages(g_full_text);
    int written = (int)g_full_text.size();
    float x0, y0, x1, y1; view_bounds(x0, y0, x1, y1);
    float S = g_free_char_size;

    json r;
    r["mode"]        = "free";
    r["orient"]      = g_glyph_orient;
    r["page"]        = g_pages.empty() ? 0 : g_edit_page;
    r["page_total"]  = (int)g_pages.size();
    r["written_pages"] = (written + g_page_chars - 1) / g_page_chars;
    r["page_chars"]  = g_page_chars;
    r["text_total"]  = (int)all.size();
    r["chars_planned"] = written;
    r["char_size"]   = S;
    r["truncated"]   = (int)all.size() > cap;
    r["view"]        = json{ { "x0", x0 }, { "y0", y0 }, { "x1", x1 }, { "y1", y1 } };
    json cells = json::array();

    if (g_pages.empty() || g_edit_page >= (int)g_pages.size()) {
        r["valid"] = false;
        r["partial"] = false;
        r["err_code"] = LAY_NO_CHARS;
        r["char_count"] = 0;
        r["cells"] = cells;
        return r;
    }
    auto& e = g_pages[g_edit_page];
    ensure_page_grid(e, S);                       // 本页缺坐标则补初始网格
    // 坐标统一夹取回视野（字号变大等情形下保持与书写路径一致，所见即所得）。
    for (auto& c : e.cells) clamp_cell(c.x, c.y, S, x0, y0, x1, y1);
    r["char_count"] = (int)e.text.size();
    // partial = 本页在“有字页”范围之外（容量外的空页，仅可浏览不可书写）；末页不满额不算。
    r["partial"]    = (g_edit_page >= (written + g_page_chars - 1) / g_page_chars);
    bool fits = grid_fits((int)e.text.size(), S);
    for (size_t i = 0; i < e.text.size() && i < e.cells.size(); ++i) {
        std::wstring one(1, e.text[i]);
        cells.push_back(json{
            { "x", e.cells[i].x }, { "y", e.cells[i].y }, { "s", S },
            { "idx", (int)i }, { "ch", w_to_utf8(one) } });
    }
    r["cells"] = cells;
    r["valid"] = fits && !e.text.empty();
    r["err_code"] = e.text.empty() ? LAY_NO_CHARS : (fits ? LAY_OK : LAY_GRID);
    return r;
}

// 供 run_task_thread 使用
static void end_task(bool ok, const char* state, const char* error) {
    task::end(ok, state, error);
}

static void run_task_thread(std::string text) {
    // 步骤 1：串口预热
    task::stage("预热");
    serial_pre_warm(g_port);
    if (g_task_cancel) { end_task(false, "canceled", "estop"); return; }

    if (g_enableDip) {
        task::stage("蘸墨");
        if (!do_dip_and_groom(g_port, g_ink)) { end_task(false, "failed", "dip"); return; }
        if (g_task_cancel) { end_task(false, "canceled", "estop"); return; }
    }

    // 步骤 2：分页布局。过滤→按容量截断（超出部分本次不写）→切页；每页独立字块（世界坐标重合）。
    std::wstring all = filter_page_chars(utf8_to_w(text));
    int cap = g_page_chars * g_page_count;
    g_full_all  = all;
    g_full_text = all.substr(0, std::min((size_t)cap, all.size()));
    rebuild_pages(g_full_text);
    const int written = (int)g_full_text.size();
    if (written <= 0) { end_task(false, "failed", "no_chars"); return; }
    task::begin(g_full_text, TextPlan(), "write");
    const int   nPages = std::min((int)g_pages.size(), (written + g_page_chars - 1) / g_page_chars);
    const float S = g_free_char_size;              // 全局字号（自由布局，各页共用）
    const int   orient = g_glyph_orient;           // 字体朝向（整字旋转，仅改朝向不改位置）
    int gci = 0;                                   // 跨页全局字序（蘸墨“每 5 字”节奏依据）

    // 步骤 3：逐页 → 逐字流式书写
    for (int pi = 0; pi < nPages; ++pi) {
        PageEntry e;
        if (!page_entry(pi, e) || e.text.empty()) { end_task(false, "failed", "layout"); return; }
        TextPlan plan;
        if (!prepare_page_plan(e, plan)) { end_task(false, "failed", "layout"); return; }
        const std::wstring& chars = e.text;        // 本页字符序列
        g_write_page = pi;                         // GUI 画布/翻页条跟随显示当前书写页
        task::page_begin(pi, nPages, (int)chars.size());

        for (size_t ci = 0; ci < chars.size(); ++ci) {
            if (g_task_cancel) { end_task(false, "canceled", "estop"); return; }

            MMAHCharData ch;
            if (!loadCharData(chars[ci], ch)) {
                wprintln(L"[警告] 找不到字形数据，跳过：" + std::wstring(1, chars[ci]));
                task::char_done((int)ci);
                ++gci;
                continue;
            }
            std::vector<Point> local;
            if (!generateSingleCharTrajectory(ch, SPEED_LEVEL, local, S)) {
                Point up = Point{ plan.offsets[(int)ci].x, plan.offsets[(int)ci].y, Z_UP, false,
                                  (uint8_t)SPEED_LEVEL, "UP-BADGLYPH" };
                g_port.sendPointRetry(up);
                end_task(false, "failed", "glyph");
                return;
            }

            // 把字形局部外接框居中到 S×S 字格：generateSingleCharTrajectory 按最长边缩放到 S 并以
            // bbox 左下角对齐 (0,0)，扁字（一/二/三）会贴到字格底部 → 与“居中”的预览框不一致、实写偏下。
            // 这里补一个居中偏移，使实际落笔与拖拽字格所见即所得（填满格的字 dx/dy≈0，无影响）。
            float minlx = 1e30f, maxlx = -1e30f, minly = 1e30f, maxly = -1e30f;
            for (const auto& p : local) {
                minlx = std::min(minlx, p.x); maxlx = std::max(maxlx, p.x);
                minly = std::min(minly, p.y); maxly = std::max(maxly, p.y);
            }
            if (!(maxlx >= minlx && maxly >= minly)) { minlx = maxlx = minly = maxly = 0.f; }
            const float dcx = S / 2 - (minlx + maxlx) / 2;
            const float dcy = S / 2 - (minly + maxly) / 2;

            std::vector<Point> one; one.reserve(local.size() + 2);
            const Offset& of = plan.offsets[(int)ci];   // 该字左下角世界绝对坐标（本页）
            for (const auto& p : local) {
                float lx = p.x + dcx, ly = p.y + dcy;   // 先居中到字格 [0,S]×[0,S]
                rotate_local(lx, ly, S, orient);        // 再按朝向绕字心旋转
                one.push_back(Point{ of.x + lx, of.y + ly,
                                     p.z, p.isPenDown, p.speed, p.zType });
            }
            task::traj_add_total(one.size());

            // 每页首字：先抬笔预定位到该字起点上方（页与页之间同样需要，翻页后从纸上空白区起步）
            if (ci == 0) {
                Point firstUp = one.front();
                firstUp.z = Z_UP;
                firstUp.isPenDown = false;
                firstUp.zType = "UP-FIRST-ANCHOR";
                if (!move_up_to_and_wait(g_port, firstUp, true)) { end_task(false, "failed", "prepos"); return; }
                std::this_thread::sleep_for(std::chrono::milliseconds(g_enableDip ? 400 : 200));
            }

            task::stage("书写");
            if (!transmitTrajectoryWithSplit(g_port, one, one.size())) {
                if (g_task_cancel) { end_task(false, "canceled", "estop"); return; }
                end_task(false, "failed", "send");
                return;
            }
            task::char_done((int)ci);
            ++gci;

            // ★实时轨迹：真机每字采一次 0x03 实测坐标（任务线程独占串口，readPose 安全穿插）
            if (!g_dryRun && g_port.is_open()) {
                float ax = 0, ay = 0, az = 0;
                if (g_port.readPose(ax, ay, az)) trail::addActual(ax, ay);
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(g_highQuality ? 160 : 120));

            // 跨页全局“每 5 字”蘸墨（gci 为已写字数；本页已无后续字时留给页尾补蘸）
            if (g_enableDip && gci % 5 == 0 && gci < written) {
                task::stage("蘸墨");
                if (!do_dip_and_groom(g_port, g_ink)) { end_task(false, "failed", "dip"); return; }
            }
        }   // ← 本页逐字循环结束

        // 页尾补蘸：全局计数不满 5 的倍数且后面还有页要写（保持原“书写结束前至少一次蘸墨”语义）
        if (g_enableDip && gci % 5 != 0 && pi + 1 < nPages) {
            task::stage("蘸墨");
            if (!do_dip_and_groom(g_port, g_ink)) { end_task(false, "failed", "dip"); return; }
        }

        // —— 翻页（仅非末页）：抬笔 → 通知 GUI 切页 → 蓝牙信号(当前为模拟等待) → 清轨迹 —— //
        if (pi + 1 < nPages) {
            {
                std::lock_guard<std::recursive_mutex> lk(g_mu);
                // 抬笔到当页末字正上方，避免拖纸时笔尖蹭纸
                const Offset& lastOfs = plan.offsets.back();
                Point up = Point{ lastOfs.x + S / 2, lastOfs.y + S / 2, Z_UP, false,
                                  (uint8_t)SPEED_LEVEL, "UP-PAGETURN" };
                if (g_port.sendPointRetry(up)) task::traj_add_done(1, false);
                session_id();
                json ev = op_event("page_turn", nullptr);
                ev["parameters"] = { { "turn_to_page", pi + 2 }, { "page_total", nPages },
                                     { "wait_ms", g_page_turn_wait_ms } };
                ev["turn_to_page"] = pi + 2;
                audit_locked(ev);
                task::stage("翻页");
            }
            const bool turnOk = g_page_turn ? g_page_turn(pi + 2, nPages) : page_turn_wait_locked();
            std::string turnErr;
            if (turnOk) {
                g_write_page = pi + 1;             // 翻页成功：GUI 画布切到下一页
                trail::reset();                    // 实时轨迹只展示当前页 → 换页后从空白重播
            }
            else {
                turnErr = (g_task_cancel || g_estop) ? "canceled" : "turn_failed";
            }
            {
                std::lock_guard<std::recursive_mutex> lk(g_mu);
                json ev = op_event("page_turn_result", nullptr);
                ev["parameters"] = { { "turn_to_page", pi + 2 }, { "page_total", nPages } };
                ev["result"] = turnOk ? "ok" : turnErr;
                audit_locked(ev);
            }
            if (!turnOk) {
                if (g_task_cancel || g_estop) { end_task(false, "canceled", "estop"); return; }
                end_task(false, "failed", "page_turn");
                return;
            }
        }
    }   // ← 页循环结束

    // 末页收尾蘸墨（全局计数不满 5 的倍数；与旧行为一致）
    if (g_enableDip && written % 5 != 0) {
        task::stage("蘸墨");
        if (!do_dip_and_groom(g_port, g_ink)) { end_task(false, "failed", "dip"); return; }
    }

    // 步骤 4：自动描边（下区绘画）
    if (g_autoDraw) {
        task::stage("描边");
        std::string latest = latest_autogen_json("themes");
        if (latest.empty()) {
            wprintln(L"[提示] 未找到 themes/ 下 auto_shanshui_*.json，本次仅书写。");
        }
        else {
            std::wstringstream ws; ws << L"[信息] 读取线稿JSON：" << mb2w(latest); wprintln(ws.str());
            std::vector<Point> polyTraj;
            if (!drawPolylinesFromFile(latest, polyTraj)) {
                wprintln(L"[警告] 线稿加载失败，跳过作画。");
            }
            else {
                task::traj_add_total(polyTraj.size());
                if (!transmitTrajectoryWithSplit(g_port, polyTraj, 0)) {
                    if (g_task_cancel) { end_task(false, "canceled", "estop"); return; }
                    end_task(false, "failed", "draw");
                    return;
                }
            }
        }
    }
    end_task(true, "done", "");
}

bool start_write(const std::string& utf8_text, std::string& err) {
    if (g_task_active) { err = "已有任务在运行"; return false; }
    if (!g_dryRun && !g_port.is_open()) { err = "串口未连接"; return false; }
    {
        std::lock_guard<std::recursive_mutex> lk(g_mu);
        session_id();
        g_task_text = utf8_text;
    }
    save_last_task_text(utf8_text);
    {
        std::lock_guard<std::recursive_mutex> lk(g_mu);
        g_task_cancel = false;
        json ev = op_event("task_start", nullptr);
        ev["parameters"] = { { "text", utf8_text } };
        audit_locked(ev);
        g_task_active = true;
        g_write_page = 0;                 // 任务固定从第 1 页开始
        g_task_finished = false;
        trail::reset();                 // ★新任务开始：清空上一任务的实时轨迹
        g_task_thread = std::thread([text = utf8_text]() {
            run_task_thread(text);
            g_task_finished = true;       // 先立“已结束”标志，再清 active，避免 GUI 空窗误恢复编辑
            g_task_active = false;
        });
        g_task_thread.detach();
    }
    err.clear();
    return true;
}
bool task_active() { return g_task_active; }
json task_info() {
    return snapshot()["task"];
}

// ---------------- 配置 ----------------
bool set_speed(int v) {
    if (g_task_active) return false;
    if (v < SPEED_MIN || v > SPEED_MAX) return false;
    SPEED_LEVEL = v; cfg_save(); return true;
}
bool set_char_spacing(float v) {
    if (g_task_active) return false;
    if (!std::isfinite(v)) return false;
    if (v < CHAR_SPACING_MIN) v = CHAR_SPACING_MIN;
    if (v > 50.f) v = 50.f;
    CHAR_SPACING = v; cfg_save(); return true;
}
bool set_z_offset(float v) {
    if (g_task_active) return false;
    if (!std::isfinite(v) || v < -100.0f || v > 100.0f) return false;
    Z_OFFSET = v; cfg_save(); return true;
}
bool set_layout_mode(int mode) {
    if (g_task_active) return false;
    if (mode != 0 && mode != 1) return false;
    g_layout_mode = mode; cfg_save(); return true;
}
bool set_layout_char_size(float v) {
    if (g_task_active) return false;
    if (!std::isfinite(v)) return false;
    if (v < SINGLE_CHAR_MIN) v = SINGLE_CHAR_MIN;   // 60mm 比赛红线
    if (v > 500.f) v = 500.f;
    g_lm_char_size = v; cfg_save(); return true;
}
bool set_layout_cols(int v) {
    if (g_task_active) return false;
    if (v < 1 || v > 200) return false;
    g_lm_cols = v; cfg_save(); return true;
}
bool set_layout_top_ratio(float v) {
    if (g_task_active) return false;
    if (!std::isfinite(v)) return false;
    v = clampf(v, 0.10f, 0.95f);
    g_lm_top_ratio = v; cfg_save(); return true;
}
bool set_layout_row_spacing(float v) {
    if (g_task_active) return false;
    if (!std::isfinite(v)) return false;
    v = clampf(v, 0.0f, 50.0f);
    g_lm_row_spacing = v; cfg_save(); return true;
}
bool set_write_dir(int dir) {
    if (g_task_active) return false;
    if (dir != 0 && dir != 1) return false;
    g_write_dir = dir; cfg_save(); return true;
}
void toggle_auto_draw()    { if (!g_task_active) { g_autoDraw = !g_autoDraw; cfg_save(); } }
void toggle_high_quality() { if (!g_task_active) { g_highQuality = !g_highQuality; cfg_save(); } }
void toggle_enable_dip()   { if (!g_task_active) { g_enableDip = !g_enableDip; cfg_save(); } }
void toggle_enable_dunbi() { if (!g_task_active) { g_enableDunbi = !g_enableDunbi; cfg_save(); } }
bool set_dry_run(bool on) {
    if (g_task_active) return false;
    g_dryRun = on; return true;
}
void set_log_enable(bool on) { g_logEnable = on; }

// ---------------- 配置持久化（供控制台/CLI 复用） ----------------
bool cfg_save() {
    json j;
    j["center"]       = { g_center_x, g_center_y, g_center_z };
    j["z_offset"]     = Z_OFFSET;
    j["speed"]        = SPEED_LEVEL;
    j["char_spacing"] = CHAR_SPACING;
    j["auto_draw"]    = g_autoDraw;
    j["high_quality"] = g_highQuality;
    j["enable_dip"]   = g_enableDip;
    j["enable_dunbi"] = g_enableDunbi;
    j["ink"]          = { g_ink.x, g_ink.y, g_ink.z, g_ink.valid };
    j["z_layers"]     = { Z_UP, Z_MID, Z_PRE_DOWN, Z_DOWN_LIGHT, Z_DOWN_NORMAL, Z_DOWN_HEAVY };
    j["z_limits"]     = { g_z_top, g_z_bottom };
    j["z_settle_ms"]           = g_z_settle_ms;
    j["stroke_begin_ms"]       = g_stroke_begin_ms;
    j["stroke_end_ms"]         = g_stroke_end_ms;
    j["cold_start_min_ms"]     = g_cold_start_min_ms;
    j["min_point_interval_ms"] = g_min_point_interval_ms;
    j["writing_plane_z"]       = g_writing_plane_z;
    j["writing_plane_valid"]   = g_writing_plane_valid;
    j["layout_mode"]           = g_layout_mode;
    j["layout_char_size"]      = g_lm_char_size;
    j["layout_cols"]           = g_lm_cols;
    j["layout_top_ratio"]      = g_lm_top_ratio;
    j["layout_row_spacing"]    = g_lm_row_spacing;
    j["write_dir"]             = g_write_dir;
    // —— 自由拖拽排版 + 分页（每页一份字块坐标；世界坐标系各页重合）—— //
    j["free_char_size"]        = g_free_char_size;
    j["glyph_orient"]          = g_glyph_orient;
    j["page_chars"]            = g_page_chars;
    j["page_count"]            = g_page_count;
    j["page_turn_wait_ms"]     = g_page_turn_wait_ms;
    j["page_text"]             = w_to_utf8(g_full_text);    // 实际书写子串（截断后）
    j["page_text_full"]        = w_to_utf8(g_full_all);     // 全量文本（容量恢复后不丢字）
    {
        json pages = json::array();
        for (const auto& e : g_pages) {
            json cells = json::array();
            for (const auto& c : e.cells) cells.push_back(json{ { "x", c.x }, { "y", c.y } });
            pages.push_back(json{ { "text", w_to_utf8(e.text) }, { "cells", cells } });
        }
        j["page_cells"] = pages;
    }
    j["corners"]      = trail::calibToJson();     // ★实时轨迹：四角标定持久化（下次开 GUI 沿用）
    std::ofstream ofs("robot_config.json");
    if (!ofs) { wprintln(L"[警告] 配置保存失败：robot_config.json 无法写入。"); return false; }
    ofs << j.dump(2);
    return true;
}

void cfg_load() {
    std::ifstream ifs("robot_config.json");
    if (!ifs) return;
    try {
        json j; ifs >> j;
        if (j.contains("z_limits") && j["z_limits"].is_array() && j["z_limits"].size() == 2) {
            float top = j["z_limits"][0].get<float>(), bot = j["z_limits"][1].get<float>();
            if (std::isfinite(top) && std::isfinite(bot) && bot <= top && bot >= -500.0f && top <= -200.0f) {
                g_z_top = top; g_z_bottom = bot;
            }
        }
        if (j.contains("z_offset")) {
            float v = j["z_offset"].get<float>();
            if (std::isfinite(v) && v >= -100.0f && v <= 100.0f) Z_OFFSET = v;
        }
        if (j.contains("speed")) {
            int v = j["speed"].get<int>();
            if (v >= SPEED_MIN && v <= SPEED_MAX) SPEED_LEVEL = v;
        }
        if (j.contains("char_spacing")) {
            float v = j["char_spacing"].get<float>();
            if (std::isfinite(v) && v >= CHAR_SPACING_MIN && v <= 50.0f) CHAR_SPACING = v;
        }
        if (j.contains("auto_draw"))    g_autoDraw    = j["auto_draw"].get<bool>();
        if (j.contains("high_quality")) g_highQuality = j["high_quality"].get<bool>();
        if (j.contains("enable_dip"))   g_enableDip   = j["enable_dip"].get<bool>();
        if (j.contains("enable_dunbi")) g_enableDunbi = j["enable_dunbi"].get<bool>();
        {
            auto clampi = [](long long v, long long lo, long long hi){ return v < lo ? (int)lo : (v > hi ? (int)hi : (int)v); };
            auto geti = [&](const char* k, int cur)->int{
                if (!j.contains(k) || !j[k].is_number()) return cur;
                return clampi(j[k].get<long long>(), 0, 3000);
                };
            g_z_settle_ms           = geti("z_settle_ms",           g_z_settle_ms);
            g_stroke_begin_ms       = geti("stroke_begin_ms",       g_stroke_begin_ms);
            g_stroke_end_ms         = geti("stroke_end_ms",         g_stroke_end_ms);
            g_cold_start_min_ms     = geti("cold_start_min_ms",     g_cold_start_min_ms);
            g_min_point_interval_ms = geti("min_point_interval_ms", g_min_point_interval_ms);
        }
        if (j.contains("writing_plane_z") && j["writing_plane_z"].is_number()) {
            float z = j["writing_plane_z"].get<float>();
            if (std::isfinite(z) && inZRange(z)) {
                g_writing_plane_z = z;
                g_writing_plane_valid = j.value("writing_plane_valid", false);
            }
        }
        if (j.contains("layout_mode") && j["layout_mode"].is_number_integer()) {
            int m = j["layout_mode"].get<int>();
            if (m == 0 || m == 1) g_layout_mode = m;
        }
        if (j.contains("layout_char_size") && j["layout_char_size"].is_number()) {
            float v = j["layout_char_size"].get<float>();
            if (std::isfinite(v) && v >= SINGLE_CHAR_MIN && v <= 500.0f) g_lm_char_size = v;
        }
        if (j.contains("layout_cols") && j["layout_cols"].is_number_integer()) {
            int v = j["layout_cols"].get<int>();
            if (v >= 1 && v <= 200) g_lm_cols = v;
        }
        if (j.contains("layout_top_ratio") && j["layout_top_ratio"].is_number()) {
            float v = j["layout_top_ratio"].get<float>();
            if (std::isfinite(v)) g_lm_top_ratio = clampf(v, 0.10f, 0.95f);
        }
        if (j.contains("layout_row_spacing") && j["layout_row_spacing"].is_number()) {
            float v = j["layout_row_spacing"].get<float>();
            if (std::isfinite(v)) g_lm_row_spacing = clampf(v, 0.0f, 50.0f);
        }
        if (j.contains("write_dir") && j["write_dir"].is_number_integer()) {
            int d = j["write_dir"].get<int>();
            if (d == 0 || d == 1) g_write_dir = d;
        }
        // —— 自由拖拽排版 + 分页 —— //
        if (j.contains("free_char_size") && j["free_char_size"].is_number()) {
            float v = j["free_char_size"].get<float>();
            if (std::isfinite(v) && v >= SINGLE_CHAR_MIN && v <= 500.0f) g_free_char_size = v;
        }
        if (j.contains("glyph_orient") && j["glyph_orient"].is_number_integer()) {
            int o = j["glyph_orient"].get<int>();
            if (o >= 0 && o <= 3) g_glyph_orient = o;
        }
        if (j.contains("page_chars") && j["page_chars"].is_number_integer()) {
            int v = j["page_chars"].get<int>();
            if (v >= 1 && v <= 50) g_page_chars = v;
        }
        if (j.contains("page_count") && j["page_count"].is_number_integer()) {
            int v = j["page_count"].get<int>();
            if (v >= 1 && v <= 20) g_page_count = v;
        }
        if (j.contains("page_turn_wait_ms") && j["page_turn_wait_ms"].is_number_integer()) {
            int v = j["page_turn_wait_ms"].get<int>();
            if (v >= 500 && v <= 60000) g_page_turn_wait_ms = v;
        }
        {
            auto parseCells = [](const json& arr) {
                std::vector<Offset> cells;
                if (!arr.is_array()) return cells;
                for (auto& e : arr) {
                    if (!e.is_object()) continue;
                    float x = e.value("x", 0.0f), y = e.value("y", 0.0f);
                    if (std::isfinite(x) && std::isfinite(y)) cells.push_back(Offset{ x, y });
                }
                return cells;
            };
            bool loaded = false;
            if (j.contains("page_cells") && j["page_cells"].is_array()) {
                std::vector<PageEntry> pages;
                bool bad = false;
                for (auto& ep : j["page_cells"]) {
                    if (!ep.is_object() || !ep.contains("text") || !ep["text"].is_string()) { bad = true; break; }
                    PageEntry e;
                    e.text = utf8_to_w(ep["text"].get<std::string>());
                    e.cells = parseCells(ep.value("cells", json::array()));
                    if (e.cells.size() != e.text.size()) { bad = true; break; }   // 数据漂移→整组弃用重排
                    pages.push_back(std::move(e));
                }
                if (!bad) {
                    g_pages = std::move(pages);
                    // 实际书写文本 = 各页文本拼接（page_text 仅作空页情形的兜底，绝不相加以防双份）
                    g_full_text.clear();
                    for (const auto& e : g_pages) g_full_text += e.text;
                    if (g_full_text.empty() && j.contains("page_text") && j["page_text"].is_string())
                        g_full_text = utf8_to_w(j["page_text"].get<std::string>());
                    g_full_all = j.contains("page_text_full") && j["page_text_full"].is_string()
                               ? utf8_to_w(j["page_text_full"].get<std::string>())
                               : g_full_text;
                    if (g_full_all.size() < g_full_text.size()) g_full_all = g_full_text;
                    loaded = true;
                }
            }
            // 旧版单页键迁移：free_text/free_cells → 第 1 页（不丢用户既有摆位）
            if (!loaded && j.contains("free_text") && j["free_text"].is_string()) {
                std::wstring ft = utf8_to_w(j["free_text"].get<std::string>());
                std::vector<Offset> fc = parseCells(j.value("free_cells", json::array()));
                if (!ft.empty() && fc.size() == ft.size()) {
                    g_pages.clear();
                    g_pages.push_back(PageEntry{ ft, fc });
                    g_full_text = ft;
                    g_full_all  = ft;
                    if (g_page_count < 1) g_page_count = 1;
                }
                loaded = true;   // 旧键存在即视为已处理，不再走空态
            }
            if (!loaded) { g_pages.clear(); g_full_text.clear(); g_full_all.clear(); }
            // 按当前容量重新截断并切页（文本一致的页保留坐标）
            {
                int cap0 = g_page_chars * g_page_count;
                g_full_text = g_full_all.substr(0, std::min((size_t)cap0, g_full_all.size()));
                if (!g_full_text.empty()) rebuild_pages(g_full_text);
                else g_pages.clear();
            }
            if (g_edit_page >= (int)g_pages.size()) g_edit_page = 0;
        }
        if (j.contains("ink") && j["ink"].is_array() && j["ink"].size() == 4) {
            g_ink = InkStation{ j["ink"][0].get<float>(), j["ink"][1].get<float>(),
                                j["ink"][2].get<float>(), j["ink"][3].get<bool>() };
        }
        if (j.contains("z_layers") && j["z_layers"].is_array() && j["z_layers"].size() == 6) {
            float up = j["z_layers"][0].get<float>(), mid = j["z_layers"][1].get<float>();
            float pre = j["z_layers"][2].get<float>(), lig = j["z_layers"][3].get<float>();
            float nor = j["z_layers"][4].get<float>(), hea = j["z_layers"][5].get<float>();
            if (std::isfinite(up) && std::isfinite(mid) && std::isfinite(pre) && std::isfinite(lig)
                && std::isfinite(nor) && std::isfinite(hea)
                && inZRange(up) && inZRange(mid) && inZRange(pre) && inZRange(lig) && inZRange(nor) && inZRange(hea)
                && up > mid && mid > pre && pre > lig && lig > nor && nor > hea) {
                Z_UP = up; Z_MID = mid; Z_PRE_DOWN = pre; Z_DOWN_LIGHT = lig; Z_DOWN_NORMAL = nor; Z_DOWN_HEAVY = hea;
            }
        }
        if (j.contains("center") && j["center"].is_array() && j["center"].size() == 3) {
            float x = j["center"][0].get<float>();
            float y = j["center"][1].get<float>();
            float z = j["center"][2].get<float>();
            if (std::isfinite(x) && std::isfinite(y) && std::isfinite(z)
                && inXYRange(x, y, g_devLimit) && inZRange(z + Z_OFFSET)) {
                g_center_x = x; g_center_y = y; g_center_z = z;
            }
        }
        if (j.contains("corners")) trail::calibFromJson(j["corners"]);   // ★实时轨迹：沿用上次四角标定
        wprintln(L"[信息] 已加载配置 robot_config.json。");
    }
    catch (...) { wprintln(L"[警告] 配置文件解析失败，使用默认设置。"); }
}

std::string make_autogen_path() {
    _mkdir("themes");
    return "themes/auto_shanshui_" + now_compact() + ".json";
}

std::string latest_autogen_json(const std::string& dir) {
    namespace fs = std::filesystem;
    try {
        if (!fs::exists(dir) || !fs::is_directory(dir)) return {};
        fs::path best; fs::file_time_type best_t{}; bool found = false;
        for (const auto& e : fs::directory_iterator(dir)) {
            if (!e.is_regular_file()) continue;
            auto p = e.path(); auto fname = p.filename().string();
            if (fname.rfind("auto_shanshui_", 0) == 0 && fname.size() >= 20 && fname.rfind(".json") == fname.size() - 5) {
                auto t = fs::last_write_time(p);
                if (!found || t > best_t) { best_t = t; best = p; found = true; }
            }
        }
        return found ? best.string() : std::string{};
    }
    catch (...) { return {}; }
}

bool save_last_task_text(const std::string& utf8_text) {
    std::ofstream ofs("last_task.txt");
    if (!ofs) return false;
    ofs << utf8_text;
    return true;
}
std::string load_last_task_text() {
    std::ifstream ifs("last_task.txt");
    if (!ifs) return "";
    std::string s((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    return s;
}

bool load_window_size(int& w, int& h) {
    std::ifstream ifs("gui_window.txt");
    if (!ifs) return false;
    if (ifs >> w >> h) return true;
    return false;
}
void set_window_size(int w, int h) {
    std::ofstream ofs("gui_window.txt");
    if (ofs) ofs << w << " " << h;
}

void shutdown() {
    if (g_task_active) abort_task();
    int wait = 0;
    while (g_task_active && wait < 6000) {   // 翻页模拟等待可达数秒，放宽收尾上限
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        wait += 50;
    }
    g_port.close();
    std::lock_guard<std::recursive_mutex> lk(g_mu);
    if (g_jsonl) { std::fclose(g_jsonl); g_jsonl = nullptr; }
}

// ---------------- 编码工具 ----------------
std::wstring utf8_to_w(const std::string& s) {
    if (s.empty()) return L"";
    int wlen = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring ws(wlen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), ws.data(), wlen);
    return ws;
}
std::string w_to_utf8(const std::wstring& ws) {
    if (ws.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), (int)ws.size(), nullptr, 0, nullptr, nullptr);
    std::string s(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), (int)ws.size(), s.data(), len, nullptr, nullptr);
    return s;
}

} // namespace gs
