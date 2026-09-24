// robot_common.cpp — 全局状态定义与基础工具实现
// 由 Robot.cpp（单文件版）拆分而来；初值与实现逐行保真。
#include "robot_common.h"

#include <chrono>
#include <cstdio>
#include <cwctype>
#include <direct.h>
#include <ctime>
#include <iostream>
#include <sstream>

// --------------------------- 全局状态定义（初值照抄原文件） ---------------------------
float ACTIVE_CHAR_SIZE = SINGLE_CHAR_MIN;
float CHAR_SPACING = 12.0f;
float TEXT_TOP_RATIO = 0.46f;
float Z_OFFSET = 0.0f;  // 运行时整体偏移
int   SPEED_LEVEL = 3;

// ★Z 层（按 2026-09-16 真机实测校准：-320~-385 可动，-310 以上不动）
float Z_UP = -325.0f;           // 抬笔（原 -320 恰在边界，留 5mm 裕量）
float Z_MID = -350.0f;          // 中位
float Z_PRE_DOWN = -363.0f;     // 预压
float Z_DOWN_LIGHT = -382.0f;   // 轻触
float Z_DOWN_NORMAL = -385.0f;  // 常规书写（实测可动）
float Z_DOWN_HEAVY = -388.0f;   // 重压（比书写深 3mm，真机验证后再用）
float g_z_top = -320.0f;        // 设备最上可动 Z（实测）
float g_z_bottom = -410.0f;     // 设备最下可动 Z（下限保护，-385 以下未实测）

// —— 书写平面（默认未设定→书写沿用三层深度；GUI 保存后置 valid 并持久化）—— //
float g_writing_plane_z = -385.0f;   // 落笔接触深度（raw，不含 Z_OFFSET）
bool  g_writing_plane_valid = false;

// —— 手动排版（默认自动，行为不变；GUI 切手动后置 1 并持久化）—— //
int   g_layout_mode    = 0;        // 0=自动, 1=手动
float g_lm_char_size   = SINGLE_CHAR_MIN; // 60
int   g_lm_cols        = 5;
float g_lm_top_ratio   = 0.46f;
float g_lm_row_spacing = 8.0f;
int   g_write_dir      = 0;        // 0=横排左起(默认现状), 1=竖排右起

std::string HANZI_BASE_DIR = "D:/objects/hanzi-writer-data"; // HanziWriter 数据根
std::string THEME_NAME = "jiangxue";                          // 主题

bool  g_dryRun = false;        // DRYRUN：不打开串口，不发报文
std::atomic_bool g_estop = false; // 急停
std::atomic_bool g_estop_stop = false;

InkStation g_ink;

bool  g_pose_init = false;     // ★全局位姿跟踪
Point g_last_pose = { 0.f,0.f,Z_UP,false,(uint8_t)SPEED_LEVEL,"POSE" };

// 注意：原文件中即采用外部链接定义，此处保持一致
WorkArea g_devLimit{ DEV_X_MIN, DEV_X_MAX, DEV_Y_MIN, DEV_Y_MAX };
WorkArea g_safeArea{ SAFE_INIT_XMIN, SAFE_INIT_XMAX, SAFE_INIT_YMIN, SAFE_INIT_YMAX };

DrawTheme g_theme;             // 主题与运行时参数
bool  g_autoDraw = true;
bool  g_enableDip = false;     // 蘸墨总开关（默认关闭，先排除干扰）
bool  g_enableDunbi = true;    // 顿笔总开关（默认开启=保持现状；关闭后书写仅走 medians 骨架，去掉压笔停顿与点画深压）
bool  g_highQuality = true;

float g_center_x = 0.f;        // ★中心点（菜单3复位目标）
float g_center_y = 0.f;
float g_center_z = Z_UP;       // ★中心点复位高度（默认抬笔高度）
std::string g_logPath;         // ★本次运行日志路径
bool        g_logEnable = true;

// —— 运行期可调节拍（默认值取常量；由 gs::cfg_load 从 robot_config.json 覆盖）—— //
int g_z_settle_ms           = Z_SETTLE_MS_BASE;          // 120
int g_stroke_begin_ms       = STROKE_BEGIN_DWELL_MS_BASE;// 70
int g_stroke_end_ms         = STROKE_END_DWELL_MS_BASE;  // 90
int g_cold_start_min_ms     = COLD_START_MIN_MS;         // 150
int g_min_point_interval_ms = MIN_POINT_INTERVAL_MS;     // 12
int g_point_fixed_ms        = POINT_FIXED_MS_BASE;       // 80  每落笔点固定开销 C
float g_rdp_tol_mm          = RDP_TOL_MM_BASE;           // 0.35 直线段 RDP 抽稀容差(mm)
int g_point_fixed_curve_ms  = POINT_FIXED_CURVE_MS_BASE; // 55  曲线段每点固定开销 C
float g_rdp_tol_curve_mm    = RDP_TOL_CURVE_MM_BASE;     // 0.15 曲线段 RDP 抽稀容差(mm)

static FILE* g_logFile = nullptr;

// ★设备 Z 行程校验（按实测范围，config 可调）
bool inZRange(float z) { return z >= g_z_bottom && z <= g_z_top; }

// --------------------------- 日志 ---------------------------
void log_init() {
    _mkdir("logs");
    std::time_t t = std::time(nullptr);
    std::tm tmv{};
    localtime_s(&tmv, &t);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "logs/Robot_%04d%02d%02d_%02d%02d%02d.log",
        tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    g_logPath = buf;
    g_logFile = std::fopen(g_logPath.c_str(), "a");
    if (g_logFile) {
        std::fprintf(g_logFile, "==== Robot 运行日志 %04d-%02d-%02d %02d:%02d:%02d ====\n",
            tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
            tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
        std::fflush(g_logFile);
    }
}

// 每条输出带毫秒时间戳写入日志（g_logEnable 关闭时跳过）
static void log_write(const std::string& gbk_line) {
    if (!g_logEnable || !g_logFile) return;
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tmv{};
    localtime_s(&tmv, &t);
    int ms = int(std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count() % 1000);
    std::fprintf(g_logFile, "[%02d:%02d:%02d.%03d] %s\n",
        tmv.tm_hour, tmv.tm_min, tmv.tm_sec, ms, gbk_line.c_str());
    std::fflush(g_logFile);
}

// --------------------------- 字符编码工具 ---------------------------
std::string w2gbk(const std::wstring& ws) {
    if (ws.empty()) return {};
    int len = WideCharToMultiByte(CP_GBK, 0, ws.c_str(), (int)ws.size(), nullptr, 0, nullptr, nullptr);
    std::string out(len, '\0');
    WideCharToMultiByte(CP_GBK, 0, ws.c_str(), (int)ws.size(), out.data(), len, nullptr, nullptr);
    return out;
}
std::wstring mb2w(const std::string& s) {
    if (s.empty()) return L"";
    int wlen = MultiByteToWideChar(CP_GBK, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring ws(wlen, L'\0');
    MultiByteToWideChar(CP_GBK, 0, s.c_str(), (int)s.size(), ws.data(), wlen);
    return ws;
}
// ★输出通道说明（黑窗/吞字问题的最终修复）：
//   1) 控制台（真窗口）：一律 WriteConsoleW 直写 UTF-16——与代码页/字体无关，
//      彻底解决"部分控制台主机不渲染 GBK 中文/无换行写入"的问题；
//   2) 重定向（文件/管道）：输出 GBK 字节（供测试脚本比对），行为与旧版一致；
//   3) wprint 统一按整行输出（补换行）。
static bool is_console_handle(HANDLE h) {
    DWORD mode = 0;
    return h != INVALID_HANDLE_VALUE && GetConsoleMode(h, &mode);
}
static void console_write_line(const std::wstring& ws) {
    log_write(w2gbk(ws));   // ★所有输出统一入日志（含 DRYRUN 帧）
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (is_console_handle(h)) {
        DWORD written = 0;
        if (!ws.empty()) WriteConsoleW(h, ws.c_str(), (DWORD)ws.size(), &written, nullptr);
        WriteConsoleW(h, L"\n", 1, &written, nullptr);
    }
    else {
        std::string s = w2gbk(ws);
        printf("%s\n", s.c_str());
        fflush(stdout);
    }
}
void wprintln(const std::wstring& ws) { console_write_line(ws); }
void wprint(const std::wstring& ws) { console_write_line(ws); }

// 仅写日志、不上屏（真机模式 TX 帧记录：避免刷屏，但日志可完整复盘）
void log_line(const std::wstring& ws) {
    if (ws.empty()) return;
    log_write(w2gbk(ws));
}

std::wstring normalizeComName(const std::wstring& in) {
    if (in.empty()) return L"\\\\.\\COM1";
    if (in.rfind(L"\\\\.\\", 0) == 0) return in;
    std::wstring s = in;
    s.erase(std::remove_if(s.begin(), s.end(), [](wchar_t ch) { return iswspace(ch); }), s.end());
    if (s.size() >= 3) {
        std::wstring head = s.substr(0, 3);
        for (auto& ch : head) ch = (wchar_t)towupper(ch);
        if (head == L"COM") return std::wstring(L"\\\\.\\") + s;
    }
    return std::wstring(L"\\\\.\\COM") + s;
}

// CRC16(Modbus)
uint16_t crc16_modbus(const uint8_t* data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 0x0001) crc = (crc >> 1) ^ 0xA001;
            else           crc = (crc >> 1);
        }
    }
    return crc;
}
