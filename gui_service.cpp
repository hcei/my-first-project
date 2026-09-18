// gui_service.cpp — GUI/CLI 共用控制服务层实现
// 实现依据 GUI_DESIGN_SPEC.md；行为边界见 gui_service.h 头注释。
// 关键设计：
//   - 全部状态存内存，snapshot() 只读拷贝，不落盘（GUI 面板周期刷新）。
//   - 审计日志 logs/audit_*.jsonl 逐条 flush；m_source 区分 GUI(HUMAN)/CLI(AGENT)。
//   - 服务函数不发 wprintln；错误经 std::string& err 返回（GBK 控制台/UTF-8 GUI 均可显示）。
//   - 任务线程统一为 detach 线程 + 原子标志；运动路径复用 motion.cpp 原函数，
//     task::progress 钩子在 motion.cpp 内更新，GUI 侧只读快照。
#include "gui_service.h"

#include <atomic>
#include <chrono>
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
std::string      g_task_stage = "空闲";      // 预热/蘸墨/书写/描边/空闲
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
        { "enable_dip", g_enableDip }, { "log", g_logEnable },
        { "char_size", ACTIVE_CHAR_SIZE }, { "plan_cols", g_plan_cols },
        { "pose_source", "软件位姿（最后有效 ACK）" },
        { "safe_area", { g_safeArea.xmin, g_safeArea.xmax, g_safeArea.ymin, g_safeArea.ymax } },
        { "z_up", Z_UP }, { "z_normal", Z_DOWN_NORMAL } };
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

bool connect(const std::string& port_name, std::string& err) {
    std::lock_guard<std::recursive_mutex> lk(g_mu);
    if (g_task_active) { err = "任务运行中，禁止修改连接"; return false; }
    session_id();
    if (g_port.is_open()) g_port.close();

    g_port_name = port_name;
    bool ok = g_port.open(normalizeComName(utf8_to_w(port_name)), BAUDRATE, EVENPARITY, 8, ONESTOPBIT);
    json ev = op_event("connect", nullptr);
    ev["parameters"] = { { "port", port_name }, { "params", "9600/8E1" } };
    ev["result"] = ok ? "ok" : "fail";
    if (!ok) ev["error_code"] = "open_failed";
    audit_locked(ev);
    if (ok) { g_consecutive_fail = 0; g_last_ack.clear(); g_last_comm = now_hms(); }
    else err = "串口打开失败（端口不存在/被占用）";
    return ok;
}

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

// ---------------- 写字任务 ----------------
json preflight(const std::string& utf8_text) {
    std::lock_guard<std::recursive_mutex> lk(g_mu);
    session_id();
    std::wstring wtext = utf8_to_w(utf8_text);
    TextPlan plan; std::wstring chars;
    bool ok = prepare_layout_only(wtext, plan, chars);
    json r;
    r["layout_ok"] = ok;
    if (ok) {
        r["char_count"] = (int)chars.size();
        r["cols"] = plan.cols;
        r["rows"] = plan.rows;
        r["char_size"] = plan.used_S;
        r["spacing"] = plan.used_sp;
        r["speed"] = SPEED_LEVEL;
        r["dry_run"] = g_dryRun;
        r["layout"] = "上区书写 / 下区绘画";
        r["text"] = w_to_utf8(chars);
    }
    else {
        r["error"] = "排版失败（字数过多或区域不足）";
    }
    json ev = op_event("task_preflight", nullptr);
    ev["parameters"] = { { "text", utf8_text } };
    ev["result"] = ok ? "ok" : "fail";
    if (!ok) ev["error_code"] = "layout";
    audit_locked(ev);
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

    // 步骤 2：布局（行首字符按换行分组）
    TextPlan plan; std::wstring chars;
    std::wstring wtext = utf8_to_w(text);
    std::vector<std::wstring> lines;
    {
        std::wstring cur;
        for (wchar_t c : wtext) {
            if (c == L'\n' || c == L'\r') { if (!cur.empty()) { lines.push_back(cur); cur.clear(); } }
            else cur.push_back(c);
        }
        if (!cur.empty()) lines.push_back(cur);
    }
    if (lines.empty()) { end_task(false, "failed", "no_text"); return; }

    std::wstring all_chars;
    std::vector<int> line_begin, line_len;
    for (auto& ln : lines) {
        std::wstring cs;
        for (wchar_t c : ln) if (isCJKOrPunct(c)) cs.push_back(c);
        if (cs.empty()) continue;
        line_begin.push_back((int)all_chars.size());
        line_len.push_back((int)cs.size());
        all_chars += cs;
    }
    if (all_chars.empty()) { end_task(false, "failed", "no_chars"); return; }
    if (!prepare_layout_only(all_chars, plan, chars)) { end_task(false, "failed", "layout"); return; }
    task::begin(chars, plan, "write");

    // 步骤 3：逐字流式书写（每字发送后更新进度）
    for (size_t ci = 0; ci < chars.size(); ++ci) {
        if (g_task_cancel) { end_task(false, "canceled", "estop"); return; }

        MMAHCharData ch;
        if (!loadCharData(chars[ci], ch)) {
            wprintln(L"[警告] 找不到字形数据，跳过：" + std::wstring(1, chars[ci]));
            task::char_done((int)ci);
            continue;
        }
        std::vector<Point> local;
        if (!generateSingleCharTrajectory(ch, SPEED_LEVEL, local, ACTIVE_CHAR_SIZE)) continue;

        std::vector<Point> one; one.reserve(local.size() + 2);
        const Offset& of = plan.offsets[(int)ci];
        for (const auto& p : local) {
            one.push_back(Point{ plan.text_area.xmin + of.x + p.x,
                                 plan.text_area.ymin + of.y + p.y,
                                 p.z, p.isPenDown, p.speed, p.zType });
        }
        task::traj_add_total(one.size());

        bool firstOfLine = false;
        for (size_t k = 0; k < line_begin.size(); ++k)
            if (line_begin[k] == (int)ci) { firstOfLine = true; break; }

        if ((ci == 0) || firstOfLine) {
            Point firstUp = one.front();
            firstUp.z = Z_UP;
            firstUp.isPenDown = false;
            firstUp.zType = (ci == 0) ? "UP-FIRST-ANCHOR" : "UP-LINE-ANCHOR";
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
        std::this_thread::sleep_for(std::chrono::milliseconds(g_highQuality ? 160 : 120));

        if (g_enableDip && ((int)(ci + 1) % 5 == 0) && ci + 1 < chars.size()) {
            task::stage("蘸墨");
            if (!do_dip_and_groom(g_port, g_ink)) { end_task(false, "failed", "dip"); return; }
        }
    }

    if (g_enableDip && ((int)chars.size() % 5 != 0)) {
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
        g_task_thread = std::thread([text = utf8_text]() {
            run_task_thread(text);
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
void toggle_auto_draw()    { if (!g_task_active) { g_autoDraw = !g_autoDraw; cfg_save(); } }
void toggle_high_quality() { if (!g_task_active) { g_highQuality = !g_highQuality; cfg_save(); } }
void toggle_enable_dip()   { if (!g_task_active) { g_enableDip = !g_enableDip; cfg_save(); } }
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
    j["ink"]          = { g_ink.x, g_ink.y, g_ink.z, g_ink.valid };
    j["z_layers"]     = { Z_UP, Z_MID, Z_PRE_DOWN, Z_DOWN_LIGHT, Z_DOWN_NORMAL, Z_DOWN_HEAVY };
    j["z_limits"]     = { g_z_top, g_z_bottom };
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
    while (g_task_active && wait < 3000) {
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
