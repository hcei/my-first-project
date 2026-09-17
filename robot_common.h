// robot_common.h — 公共常量、数据结构、全局状态声明与基础工具
// 由 Robot.cpp（单文件版）拆分而来；行为保持不变。
// 注意：可变全局一律 extern 声明（定义在 robot_common.cpp），
//       避免 static 全局在多编译单元下各自成副本的隐性 bug。
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

class SerialPort; // 前置声明

// --------------------------- 常量区 ---------------------------
static const int    CP_GBK = 936;

// 物理极限（mm）
static const float DEV_X_MIN = -180.0f;
static const float DEV_X_MAX = 180.0f;
static const float DEV_Y_MIN = -180.0f;
static const float DEV_Y_MAX = 180.0f;

// 初始安全区
static const float SAFE_INIT_XMIN = -165.0f;
static const float SAFE_INIT_XMAX = 165.0f;
static const float SAFE_INIT_YMIN = -170.0f;
static const float SAFE_INIT_YMAX = 110.0f;

// 字号范围（mm）（原为宏，改 constexpr，语义不变）
constexpr float SINGLE_CHAR_MIN = 60.0f;
constexpr float SINGLE_CHAR_MAX = 70.0f;
extern float ACTIVE_CHAR_SIZE;

// 字间距（可压到2mm）
extern float CHAR_SPACING;
static const float CHAR_SPACING_MIN = 0.5f;

// 上下区分割
extern float TEXT_TOP_RATIO;
static const float V_GAP_BETWEEN = 1.0f; // 上下区域间隙

// Z 层（mm，负值向下）——★2026-09-16 真机实测校准：设备可动范围约 -320~-385，
// -310 及以上不动（ACK 但不执行）。全部改为可配置全局，随 robot_config.json 持久化。
extern float Z_UP;          // 抬笔（原 -320 恰在可动边界，调至 -325 留裕量）
extern float Z_MID;         // 中位
extern float Z_PRE_DOWN;    // 预压
extern float Z_DOWN_LIGHT;  // 轻触
extern float Z_DOWN_NORMAL; // 常规书写
extern float Z_DOWN_HEAVY;  // 重压（点）
extern float      Z_OFFSET; // 运行时整体偏移
extern float g_z_top;       // ★设备最上可动 Z（实测 -320 可动、-310 不动）
extern float g_z_bottom;    // ★设备最下可动 Z（下限保护，-385 已实测可动）

// 速度档（1~6）
extern int SPEED_LEVEL;
static const int SPEED_MIN = 1;
static const int SPEED_MAX = 6;

// 发送基本延时
static const int DELAY_MS = 35;

// 高质模式节拍
static const int   POINT_RATE_LIMIT_MS_BASE = 60;
static const int   Z_SETTLE_MS_BASE = 120;
static const int   STROKE_BEGIN_DWELL_MS_BASE = 70;
static const int   STROKE_END_DWELL_MS_BASE = 90;
static const float RESAMPLE_STEP_MM_BASE = 1.2f;

// 探边
static const float PROBE_STEP_DEFAULT = 5.0f;
static const float PROBE_BACKOFF_DEFAULT = 10.0f;

// Modbus
static const DWORD    BAUDRATE = CBR_9600;
static const uint8_t  MB_SLAVE = 0x01;
static const uint8_t  MB_FUNC = 0x10;
static const uint16_t REG_START = 0x0008;
static const uint16_t REG_COUNT = 0x0005;

// 批量7点寄存器
static const uint16_t REG_BATCH7_START = 0x0064;
static const uint16_t REG_BATCH7_COUNT = 0x0023;
static const uint8_t  REG_BATCH7_BYTES = 0x46;

// 布局/边距
static const float SAFE_MARGIN = 2.0f;

// —— 首落笔稳态（参数上调，更保守） —— //
static const int PREWARM_MIN_GOOD_RESP = 3;        // 预热阶段连续成功阈值
static const int PREWARM_MAX_PROBES = 12;          // 最多探测次数
static const int FIRST_DOWN_DUP_SEND_PAUSE = 220;  // 首次落笔重复发送之间的停顿(ms)
static const int FIRST_DOWN_EXTRA_DWELL_MS = 240;  // 首次落笔额外沉稳(ms)

// --------------------------- 数据结构 ---------------------------
struct Point {
    float x{ 0 }, y{ 0 }, z{ Z_UP };
    bool isPenDown{ false };
    uint8_t speed{ 3 };
    std::string zType;
};

struct WorkArea {
    float xmin{ SAFE_INIT_XMIN }, xmax{ SAFE_INIT_XMAX };
    float ymin{ SAFE_INIT_YMIN }, ymax{ SAFE_INIT_YMAX };
};

struct PolyPath2D {
    std::vector<std::pair<float, float>> pts;
    bool closed{ false };
};

struct MMAHStroke {
    std::vector<std::pair<float, float>> centerLine;
    std::string type;
    int order{ 0 };
};
struct MMAHCharData {
    std::wstring charStr;
    std::vector<MMAHStroke> strokes;
    bool isValid{ false };
};

struct Offset { float x{ 0 }; float y{ 0 }; };

struct DrawTheme {
    std::string name;
    std::vector<std::string> files;
    bool auto_fit{ true };
    float drawZ{ Z_DOWN_LIGHT };
    int speed{ 1 };
    bool bottom_area{ true };
    float top_ratio{ TEXT_TOP_RATIO };
    float vgap_mm{ V_GAP_BETWEEN };
};

struct TextPlan {
    bool ok{ false };
    int cols{ 5 };
    int rows{ 1 };
    float used_S{ SINGLE_CHAR_MIN };
    float used_sp{ CHAR_SPACING };
    float top_ratio{ TEXT_TOP_RATIO };
    WorkArea text_area;
    std::vector<Offset> offsets;
};

// --------------------------- Ink Station (蘸墨位) ---------------------------
struct InkStation {
    float x{ 0 }, y{ 0 }, z{ Z_MID };
    bool valid{ false };
};

// --------------------------- 全局状态（extern 声明，定义于 robot_common.cpp） ---------------------------
extern std::string HANZI_BASE_DIR;   // HanziWriter 数据根
extern std::string THEME_NAME;       // 主题
extern bool  g_dryRun;               // DRYRUN：不打开串口，不发报文
extern std::atomic_bool g_estop;     // 急停
extern std::atomic_bool g_estop_stop;// 急停轮询线程停止标志
extern InkStation g_ink;
extern bool  g_pose_init;            // ★全局位姿跟踪
extern Point g_last_pose;
extern WorkArea g_devLimit;
extern WorkArea g_safeArea;
extern DrawTheme g_theme;
extern bool  g_autoDraw;
extern bool  g_enableDip;            // 蘸墨总开关（默认关闭，先排除干扰）
extern bool  g_highQuality;          // 高质模式
extern float g_center_x;             // ★中心点（菜单3复位目标；默认 0,0，菜单17可设）
extern float g_center_y;
extern float g_center_z;             // ★中心点复位高度（默认 Z_UP，菜单17可设）
extern std::string g_logPath;        // ★本次运行日志路径（log_init 生成）
extern bool        g_logEnable;      // ★日志记录开关（默认开，菜单18切换）

// --------------------------- 内联小工具 ---------------------------
static inline void update_pose_from(const Point& p) {
    g_last_pose = p; g_pose_init = true;
}

static inline float clampf(float v, float a, float b) { return std::min(std::max(v, a), b); }
static inline int16_t mm_to_dev10(float v) {
    float scaled = std::round(v * 10.0f);
    if (scaled > 32760.f)  scaled = 32760.f;
    if (scaled < -32760.f) scaled = -32760.f;
    return (int16_t)scaled;
}
static inline bool inXYRange(float x, float y, const WorkArea& w) {
    return (x >= w.xmin && x <= w.xmax && y >= w.ymin && y <= w.ymax);
}
bool inZRange(float z);   // ★改为函数：按设备实测行程 g_z_bottom~g_z_top 校验
static inline bool isCJKOrPunct(wchar_t c) {
    if (c == L' ' || c == L'\t' || c == L'\r' || c == L'\n') return false;
    if ((c >= 0x4E00 && c <= 0x9FFF) || (c >= 0x3000 && c <= 0x303F) || (c >= 0xFF00 && c <= 0xFFEF)) return true;
    return false;
}

// --------------------------- 基础工具（实现在 robot_common.cpp） ---------------------------
std::string w2gbk(const std::wstring& ws);
std::wstring mb2w(const std::string& s);
void wprintln(const std::wstring& ws);
void wprint(const std::wstring& ws);
std::wstring normalizeComName(const std::wstring& in);

// 日志：main 启动时调用一次，生成 logs/Robot_时间.log；
// 之后所有 wprintln/wprint 输出（含 DRYRUN 帧）自动带毫秒时间戳写入
void log_init();
void log_line(const std::wstring& ws);   // ★仅写日志不上屏（真机模式 TX 帧记录用）

// CRC16(Modbus)
uint16_t crc16_modbus(const uint8_t* data, size_t len);
