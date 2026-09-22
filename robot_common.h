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

// —— 书写平面（GUI“书写平面”页设置；随 robot_config.json 持久化）—— //
// g_writing_plane_valid=false 时书写沿用原 Z_DOWN_HEAVY/NORMAL/LIGHT 三层深度；
// 用户在页面点“保存”后置 true，此后所有落笔点的 Z = g_writing_plane_z（固定平面）。
extern float g_writing_plane_z;       // 落笔接触深度（raw，不含 Z_OFFSET）
extern bool  g_writing_plane_valid;   // 是否已由用户设定并持久化

// —— 手动排版（GUI「书写任务」页排版控件设置；随 robot_config.json 持久化）—— //
// g_layout_mode=0 自动：沿用 plan_text_area_and_layout 的自动搜索（默认，行为不变）；
// g_layout_mode=1 手动：按下面四项严格排布（字号上不封顶，仅受安全区文本框 fit 约束），
// 放不下时 prepare_layout_only 直接置 ok=false 并回 err 原因码，绝不自动缩小。
extern int   g_layout_mode;        // 0=自动, 1=手动
extern float g_lm_char_size;       // 字号 mm（≥60 比赛红线，上不封顶）
extern int   g_lm_cols;            // 每行字数
extern float g_lm_top_ratio;       // 上区占比（文本区占安全区高度比例 0.10~0.95）
extern float g_lm_row_spacing;     // 行间距 mm（独立于列内 CHAR_SPACING）
// 书写方向：0=横排左起（默认，现状）；1=竖排右起（列内从上到下、列与列从右往左，传统书法）。
// 独立于自动/手动模式，决定 TextPlan.offsets 的字序映射。随 robot_config.json 持久化。
extern int   g_write_dir;

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

// —— 方案A（节拍重整）：真机主要安全节拍旋钮 —— //
// MIN_POINT_INTERVAL_MS：落笔逐点最小“指令间隔”。旧逻辑每点固定 sleep≥60ms（在串口往返之上
//   再叠加死等），是书写顿挫的主因之一；现改为补偿式计时（串口+运动时间计入间隔），此值仅作
//   纯 XY 直段点的下限保护。真机若出现拐角切角/失步，优先上调本值。
// COLD_START_MIN_MS：每次下发调用开始的首秒内，指令间隔不小于此值（设备刚“醒”时更保守）。
static const int   MIN_POINT_INTERVAL_MS = 12;
static const int   COLD_START_MIN_MS = 150;
// POINT_FIXED_MS_BASE：每个落笔点的固定开销 C(ms)——碎段加减速/伺服整定在地板之上的常数项，
//   由 estimateMoveMs 计入 base = max(地板, d/v + C)。历史由拐角 bug 隐式兜着，现显式化、可扫参。
static const int   POINT_FIXED_MS_BASE = 80;
// RDP_TOL_MM_BASE：书法段 RDP 抽稀容差(mm)。越大→点越少→停顿越少(治顿挫)；拐角/小结构天然保点。
//   注：point_fixed_ms / rdp_tol_mm = 直线段(横/竖)组；*_curve = 曲线段(撇/捺/弯钩)组。
static const float RDP_TOL_MM_BASE = 0.35f;
static const int   POINT_FIXED_CURVE_MS_BASE = 55;
static const float RDP_TOL_CURVE_MM_BASE = 0.15f;

// —— 运行期可调节拍（随 robot_config.json 持久化，改后重启生效；默认取上面的 *_BASE 常量）—— //
// 真机扫参用：把每笔 Z 沉降 / 起收笔 dwell / 冷启动 / 逐点下限做成可配置，无需重编译。
extern int g_z_settle_ms;            // 每笔 Z 沉降(ms)
extern int g_stroke_begin_ms;        // 落笔起笔 dwell(ms)
extern int g_stroke_end_ms;          // 收笔 dwell(ms)
extern int g_cold_start_min_ms;      // 冷启动首秒指令间隔下限(ms)
extern int g_min_point_interval_ms;  // 落笔逐点最小指令间隔(ms)
extern int g_point_fixed_ms;         // 每落笔点固定开销 C(ms)，计入 estimateMoveMs 的 base
extern float g_rdp_tol_mm;           // 书法段 RDP 抽稀容差(mm)，config 可调
extern int g_point_fixed_curve_ms;   // 曲线段(撇/捺/弯钩)每点固定开销 C(ms)
extern float g_rdp_tol_curve_mm;     // 曲线段 RDP 抽稀容差(mm)

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
    uint8_t strokeKind{ 0 };   // 落笔段类型：0=直线(横/竖)→用 point_fixed_ms/rdp_tol_mm；1=曲线(撇/捺/弯钩)→用 *_curve
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

// 排版失败原因码（仅手动模式产生；0=成功）
enum LayoutErr {
    LAY_OK = 0,
    LAY_NO_CHARS = 1,     // 无有效汉字
    LAY_FONT_W = 2,       // 单字宽度超过文本区宽（字号过大/列数过多）
    LAY_FONT_H = 3,       // 单字高度超过文本区高（字号过大/上区占比过小）
    LAY_GRID = 4,         // 整体网格超出文本区（减少字数/增大上区占比/调小行距）
    LAY_BAD_PARAM = 5,    // 参数非法
};

struct TextPlan {
    bool ok{ false };
    int cols{ 5 };
    int rows{ 1 };
    float used_S{ SINGLE_CHAR_MIN };
    float used_sp{ CHAR_SPACING };
    float row_spacing{ 0.0f };   // 实际行间距 mm；0 表示沿用 used_sp（自动排版保持旧行为）
    float top_ratio{ TEXT_TOP_RATIO };
    int err{ LAY_OK };           // 排版失败原因码
    int dir{ 0 };                // 生效书写方向：0=横排左起, 1=竖排右起
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
extern bool  g_enableDunbi;          // 顿笔总开关（默认开启=保持现状；关闭则去掉起收笔/沉降/首点重压停顿且点画等深，仅写骨架）
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
