// gui_win32.cpp — 书画机械臂调试助手 GUI（Win32 原生，无外部依赖）
// 页面与视觉依据 gui_design_mockup.html / GUI_DESIGN_SPEC.md：
//   左侧导航（主页/设备连接/书写任务）+ 紫色标题栏 + 底部状态栏。
// 三页功能（全部走 gs:: 服务层，source=GUI actor=HUMAN，自动写 JSONL 审计）：
//   主页：设备信息只读快照 + 快捷操作 + 运行开关
//   连接：串口枚举/连接/断开/刷新 + 诊断
//   书写：文本输入 + 预检 + 开始/停止 + 进度
// 限制：单实例；任务运行时快捷操作与配置置灰；急停常驻可用。
#include "gui_service.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cwctype>
#include <mutex>
#include <thread>
#include <string>
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <shellapi.h>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")

namespace gw {

// ---------------- 颜色（对应 HTML :root 变量） ----------------
static const COLORREF PURPLE      = RGB(181, 107, 215);
static const COLORREF PURPLE_DARK = RGB(123, 75, 179);
static const COLORREF PURPLE_SOFT = RGB(245, 233, 251);
static const COLORREF BLUE        = RGB(121, 200, 242);
static const COLORREF TEAL        = RGB(99, 196, 190);
static const COLORREF INK         = RGB(39, 48, 61);
static const COLORREF MUTED       = RGB(105, 115, 134);
static const COLORREF LINE        = RGB(220, 229, 239);
static const COLORREF BG          = RGB(247, 249, 252);
static const COLORREF DANGER      = RGB(230, 80, 104);
static const COLORREF OK          = RGB(41, 170, 85);
static const COLORREF WARN        = RGB(240, 163, 59);
static const COLORREF WHITE       = RGB(255, 255, 255);

// ---------------- 布局常量（对应 HTML 1494×1204 原型） ----------------
static const int WIN_W = 1494, WIN_H = 1204;
static const int TITLE_H = 45, FOOTER_H = 32, NAV_W = 212;
static const int MARGIN = 24, PANEL_PAD = 25;

// ★页面几何：绘制与子控件必须共用同一基准。
// OnPaint 传入的 pageRc 即页面内容区矩形；Draw* 与 Create*Controls 都由
// PageMetrics() 取得原点与可用尺寸，避免"绘制按客户区绝对坐标、控件按页面坐标"的错位。
static const int PAGE_X = NAV_W + 8;   // 页面内容区左边界（客户区坐标）
static const int PAGE_Y = TITLE_H + 4; // 页面内容区上边界
static const int PAGE_R = 6;           // 右侧留白
static const int HINT_H = 46;          // 顶部提示条高度
static const int PANEL_TOP = 66;       // 页面内第一排面板的 y（页面局部坐标）
static const int PANEL_GAP = 18;       // 面板行间距
static const int INFO_ROW_H = 28;      // 设备信息行高
static const int CTRL_PANEL_H = 280;   // 主页"快捷操作/运行开关"面板高度（内容高度，不拉伸）
static const int INFO_ROW_MAX = 44;    // 设备信息行距上限（多余高度均匀铺开，不留大片空白）
static const int INFO_LABEL_W = 88;    // 设备信息标签列宽（容纳 4 字标签）
static const int INFO_VALUE_DX = 94;   // 设备信息值列相对标签起点的偏移
static const int CONNECT_PANEL_H = 246;// 连接页面板高度
static const int WRITE_PANEL_H = 262;  // 书写页面板高度
static const int BTN_H = 40, BTN_PITCH = 52;
static const int CHK_H = 24, CHK_PITCH = 30;

// 由客户区尺寸构造页面矩形（供 Create*Controls 使用，与 OnPaint 的 pageRc 一致）
static RECT PageRectFromClient(int cw, int ch) {
    return RECT{ PAGE_X, PAGE_Y, cw - PAGE_R, ch - FOOTER_H };
}
// 主页"设备信息"面板高度：按可用高度自适应，保证下方控制面板完整可见
static int HomeInfoH(int H) {
    return H - PANEL_TOP - PANEL_GAP - CTRL_PANEL_H - 10;
}
// 设备信息行距：高度富余时均匀铺开（上限 INFO_ROW_MAX），避免面板下半大片空白
static int HomeRowH(int infoH, int lines) {
    int rows = (lines + 1) / 2;                        // 两列排布 → 行数
    if (rows < 1) rows = 1;
    int avail = infoH - 50 - 6;                        // 减去标题区与底部留白
    int h = avail / rows;
    if (h < INFO_ROW_H) h = INFO_ROW_H;
    if (h > INFO_ROW_MAX) h = INFO_ROW_MAX;
    return h;
}

// ---- 三页统一几何：绘制与子控件共用同一套计算结果 ----
// 单面板页（连接 / 书写）：左右两栏等宽
struct TwoColGeo {
    int x0, y0, W, H;        // 页面内容区
    int top;                 // 首排面板 y
    int leftX, rightX;       // 两栏面板左边界
    int colW;                // 每栏宽度
    int innerX;              // 左栏内容起点（面板 + 内边距）
    int innerW;              // 左栏内容宽度
    int panelH;
};
static TwoColGeo TwoColLayout(const RECT& rc, int panelH) {
    TwoColGeo g{};
    g.x0 = rc.left; g.y0 = rc.top;
    g.W = rc.right - rc.left; g.H = rc.bottom - rc.top;
    g.panelH = panelH;
    g.top = g.y0 + PANEL_TOP;
    g.colW = (g.W - MARGIN * 2 - PANEL_GAP) / 2;
    g.leftX = g.x0 + MARGIN;
    g.rightX = g.leftX + g.colW + PANEL_GAP;
    g.innerX = g.leftX + PANEL_PAD;
    g.innerW = g.colW - PANEL_PAD * 2;
    return g;
}

// 主页几何：信息面板 + 右列 + 底部"快捷操作 / 运行开关"
struct HomeGeo {
    int x0, y0, W, H;
    int top;
    int leftW, rightW, infoH;
    int rx;                  // 右列左边界
    int connH;               // 右列"设备连接"面板高度
    int cTop, cH;            // 底部控制面板
    int blW;                 // 快捷操作面板宽度
    int ctlX, ctlW;          // 运行开关面板
    int btnX, btnY, btnW;    // 快捷操作按钮起点与单列宽
    int chkX, chkW;          // 复选框列
    int spdX, spdW;          // 参数列（速度 / 字间距 / Z 偏移）
    // 参数列内部布局（相对 spdX；绘制与控件共用）
    int lblW, pbtnW, valW, editW, applyW;
    int rowY[3];             // 速度 / 字间距 / Z 偏移 三行的 y
};
static HomeGeo HomeLayout(const RECT& rc) {
    HomeGeo g{};
    g.x0 = rc.left; g.y0 = rc.top;
    g.W = rc.right - rc.left; g.H = rc.bottom - rc.top;
    g.top = g.y0 + PANEL_TOP;
    g.leftW = (int)((g.W - MARGIN * 2 - PANEL_GAP) * 1.55 / 2.35);
    g.rightW = g.W - MARGIN * 2 - PANEL_GAP - g.leftW;
    g.rx = g.x0 + MARGIN + g.leftW + PANEL_GAP;
    g.infoH = HomeInfoH(g.H);
    g.connH = 128;
    g.cTop = g.top + g.infoH + PANEL_GAP;
    g.cH = CTRL_PANEL_H;                       // 固定内容高度：控件排不满时留白在面板下方，避免面板内大面积空白
    if (g.cTop + g.cH > g.y0 + g.H - 10)       // 窗口过矮时允许内容向下延伸（顶部对齐），不压缩面板
        g.H = g.cTop + g.cH + 10 - g.y0;
    g.blW = (g.W - MARGIN * 2 - PANEL_GAP - 2) / 2;
    g.ctlX = g.x0 + MARGIN + g.blW + PANEL_GAP;
    g.ctlW = g.x0 + g.W - MARGIN - g.ctlX;
    // 快捷操作按钮：两列，面板内边距对齐
    g.btnX = g.x0 + MARGIN + PANEL_PAD;
    g.btnW = (g.blW - PANEL_PAD * 2 - 24) / 2;
    g.btnY = g.cTop + 52;
    // 运行开关：左列复选框，右列参数（标签在左、控件在右，同一行内依次排列）
    g.chkX = g.ctlX + PANEL_PAD;
    g.chkW = 296;
    g.spdX = g.chkX + g.chkW + 16;
    g.spdW = g.ctlX + g.ctlW - PANEL_PAD - g.spdX;
    // 参数列内部宽度：先给足，再按可用宽度收窄（窗口很窄时不溢出）
    g.lblW = 76; g.pbtnW = 32; g.valW = 72; g.editW = 72; g.applyW = 64;
    int need = g.lblW + 4 + g.editW + 6 + g.applyW;
    if (need > g.spdW) {
        int over = need - g.spdW;
        g.editW -= over / 2; g.applyW -= over - over / 2;
        if (g.editW < 44) g.editW = 44;
        if (g.applyW < 44) g.applyW = 44;
    }
    int need2 = g.lblW + g.pbtnW * 2 + g.valW;
    if (need2 > g.spdW) {
        g.valW -= need2 - g.spdW;
        if (g.valW < 40) g.valW = 40;
    }
    g.rowY[0] = g.cTop + 52;   // 速度
    g.rowY[1] = g.cTop + 92;   // 字间距
    g.rowY[2] = g.cTop + 130;  // Z 偏移
    return g;
}

// 控件 ID（1000+ 避免与 IDC_STATIC 冲突）
enum {
    IDC_BTN_CONNECT = 1001, IDC_BTN_DISCONNECT, IDC_BTN_REFRESH, IDC_COMBO_PORT,
    IDC_BTN_HEARTBEAT,
    IDC_BTN_RESET, IDC_BTN_CENTER, IDC_BTN_TESTPT, IDC_BTN_QUERY, IDC_BTN_REFRESHDEV,
    IDC_BTN_ESTOP, IDC_BTN_ESTOP_CLEAR,
    IDC_BTN_WRITE, IDC_BTN_STOP, IDC_EDIT_TEXT,
    IDC_BTN_SPEED_DEC, IDC_BTN_SPEED_INC, IDC_EDIT_SPACING, IDC_BTN_SPACING_APPLY,
    IDC_BTN_ZOFF_APPLY, IDC_EDIT_ZOFF,
    IDC_CHECK_DRY, IDC_CHECK_AUTODRAW, IDC_CHECK_HQ, IDC_CHECK_DIP, IDC_CHECK_LOG,
    IDC_EDIT_PREVIEW,
    ID_PAGE_HOME = 2001, ID_PAGE_CONNECT, ID_PAGE_WRITE,
    IDT_TIMER = 3001,
};

struct WndState {
    HWND hwnd = nullptr;
    int  page = ID_PAGE_HOME;          // 当前页
    int  navHover = -1;
    nlohmann::json snap;               // 最近一次快照
    std::wstring lastResult;           // 操作结果（顶部提示条）
    bool  resultIsErr = false;
    ULONGLONG lastAction = 0;
};
static WndState g_st;

// 只读文本行（主页信息表）
struct InfoLine { std::wstring label; std::wstring value; COLORREF color; };
static std::vector<InfoLine> g_infoLines;

// 子窗口句柄
static HWND g_comboPort = nullptr;
static HWND g_editText = nullptr;
static HWND g_editSpacing = nullptr;
static HWND g_editZoff = nullptr;
static HWND g_editPreview = nullptr;
static HWND g_btns[64] = {};            // ID 映射辅助
static HWND g_checks[16] = {};
static RECT g_speedRect{};              // 主页速度档显示区
static HFONT g_font18 = nullptr, g_fontBold20 = nullptr, g_font16 = nullptr;
static HBRUSH g_brPanel = nullptr, g_brBg = nullptr, g_brTitle = nullptr, g_brFooter = nullptr,
              g_brSoft = nullptr, g_brWhite = nullptr, g_brDangerSoft = nullptr, g_brWarn = nullptr;

static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

// ---------------- 小工具 ----------------
static std::wstring to_ws(const std::string& utf8) { return gs::utf8_to_w(utf8); }
static std::string   to_u8(const std::wstring& ws) { return gs::w_to_utf8(ws); }

static std::wstring fmt(const wchar_t* f, ...) {
    wchar_t buf[512];
    va_list ap; va_start(ap, f);
    _vsnwprintf_s(buf, 512, _TRUNCATE, f, ap);
    va_end(ap);
    return buf;
}
static std::wstring f1(float v, int prec = 1) {
    wchar_t buf[32];
    swprintf_s(buf, L"%.*f", prec, v);
    return buf;
}
static std::wstring FloatStr(float v) {
    // 避免出现 -0.0
    if (std::fabs(v) < 0.05f) v = 0.f;
    return f1(v, 1);
}

static void SetResult(const std::wstring& s, bool err = false) {
    g_st.lastResult = s;
    g_st.resultIsErr = err;
    g_st.lastAction = GetTickCount64();
    InvalidateRect(g_st.hwnd, nullptr, FALSE);
}

// 圆角矩形描边
static void rr(HDC dc, int x, int y, int w, int h, int r, COLORREF line) {
    HBRUSH br = CreateSolidBrush(line);
    HRGN g = CreateRoundRectRgn(x, y, x + w, y + h, r, r);
    FrameRgn(dc, g, br, 2, 2);   // FrameRgn 用画刷描边（等宽 2px）
    DeleteObject(g);
    DeleteObject(br);
}

// 通用：带边框白底面板
static void Panel(HDC dc, int x, int y, int w, int h) {
    RECT rc{ x, y, x + w, y + h };
    FillRect(dc, &rc, g_brWhite);
    rr(dc, x, y, w, h, 20, LINE);
}

// 静态文本
static void Text(HDC dc, const std::wstring& s, int x, int y, int w, int h,
                 COLORREF c, int size = 18, bool bold = false, UINT align = DT_LEFT) {
    HFONT f = CreateFontW(-(size * 96 / 72), 0, 0, 0, bold ? FW_BOLD : FW_NORMAL,
                          0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                          L"Microsoft YaHei");
    HFONT of = (HFONT)SelectObject(dc, f);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, c);
    RECT rc{ x, y, x + w, y + h };
    DrawTextW(dc, s.c_str(), -1, &rc, align | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(dc, of);
    DeleteObject(f);
}

// ---------------- 快照取值辅助 ----------------
static bool snapB(const char* obj, const char* key, bool dflt = false) {
    try { return g_st.snap.at(obj).at(key).get<bool>(); } catch (...) { return dflt; }
}
static std::string snapS(const char* obj, const char* key) {
    try { return g_st.snap.at(obj).at(key).get<std::string>(); } catch (...) { return ""; }
}
static double snapD(const char* obj, const char* key, double d = 0) {
    try { return g_st.snap.at(obj).at(key).get<double>(); } catch (...) { return d; }
}
static int snapI(const char* obj, const char* key, int d = 0) {
    try { return g_st.snap.at(obj).at(key).get<int>(); } catch (...) { return d; }
}
static bool snapTop(const char* key, bool d = false) {
    try { return g_st.snap.at(key).get<bool>(); } catch (...) { return d; }
}
static std::string snapTopS(const char* key) {
    try { return g_st.snap.at(key).get<std::string>(); } catch (...) { return ""; }
}

// 异步连接观察标记：点击“连接设备”后置位，由 WM_TIMER 轮询 snapshot()["connect"]，
// 待 pending 变 false 时刷新结果文本（open 在工作线程，UI 全程不阻塞）。
static bool g_connWatching = false;

// ---------------- 主页信息行构建 ----------------
static void BuildInfoLines() {
    g_infoLines.clear();
    bool conn = snapTop("connected");
    bool dry = snapTop("dry_run");
    bool poseValid = snapB("pose", "valid");
    bool taskActive = snapB("task", "active");
    bool estop = snapTop("estop");

    COLORREF cConn = dry ? WARN : (conn ? OK : DANGER);
    g_infoLines.push_back({ L"设备状态", dry ? L"DRYRUN（模拟）" : (conn ? L"已连接" : L"未连接"), cConn });
    g_infoLines.push_back({ L"连接类型", conn ? L"RS485 / Modbus RTU" : L"—", conn ? BLUE : MUTED });
    g_infoLines.push_back({ L"串口", to_ws(snapTopS("port")).empty() ? L"—" : to_ws(snapTopS("port")), INK });
    g_infoLines.push_back({ L"通信参数", conn ? L"9600 / 8E1" : L"—", conn ? BLUE : MUTED });

    if (poseValid) {
        std::wstring pose = fmt(L"X %s　Y %s　Z %s mm", FloatStr((float)snapD("pose", "x")).c_str(),
                                FloatStr((float)snapD("pose", "y")).c_str(), FloatStr((float)snapD("pose", "z")).c_str());
        g_infoLines.push_back({ L"当前坐标", pose, BLUE });
        g_infoLines.push_back({ L"坐标依据", L"软件位姿（最后有效 ACK）", WARN });
        g_infoLines.push_back({ L"笔状态", snapB("pose", "pen_down") ? L"落笔" : L"抬笔", snapB("pose", "pen_down") ? DANGER : OK });
    }
    else {
        g_infoLines.push_back({ L"当前坐标", L"—（尚未发送过点）", MUTED });
        g_infoLines.push_back({ L"坐标依据", L"软件位姿（最后有效 ACK）", WARN });
        g_infoLines.push_back({ L"笔状态", L"—", MUTED });
    }

    std::wstring mode = to_ws(snapTopS("mode"));
    std::wstring ackw = to_ws(snapS("comm", "ack"));
    g_infoLines.push_back({ L"当前模式", mode.empty() ? L"空闲" : mode, taskActive ? PURPLE_DARK : INK });
    g_infoLines.push_back({ L"急停状态", estop ? L"已触发（需人工复位）" : L"未触发", estop ? DANGER : OK });
    g_infoLines.push_back({ L"当前任务", taskActive ? L"书写任务运行中" : L"无任务", taskActive ? PURPLE_DARK : INK });
    g_infoLines.push_back({ L"最近响应", ackw == L"valid" ? L"ACK 有效" : (ackw.empty() ? L"—" : L"无应答"),
                            ackw == L"valid" ? OK : WARN });
    g_infoLines.push_back({ L"最近 TX", to_ws(snapS("comm", "tx")).empty() ? L"—" : to_ws(snapS("comm", "tx")), MUTED });
    g_infoLines.push_back({ L"连续失败", fmt(L"%d 次", snapI("comm", "fail")), snapI("comm", "fail") > 0 ? DANGER : OK });
    g_infoLines.push_back({ L"实际位置", L"未接入 / 协议待确认", MUTED });
    g_infoLines.push_back({ L"资源占用", L"未接入 / 协议待确认", MUTED });
}

// ---------------- 三个页面绘制 ----------------
static void DrawHome(HDC dc, RECT& rc) {
    HomeGeo g = HomeLayout(rc);

    // 顶部提示条（页面内容区整宽）
    Panel(dc, g.x0 + MARGIN, g.y0 + 10, g.W - MARGIN * 2, HINT_H);
    Text(dc, g_st.lastResult.empty() ? L"就绪。所有操作将记录审计日志（source=GUI, actor=HUMAN）。"
                                     : g_st.lastResult,
         g.x0 + MARGIN + PANEL_PAD - 5, g.y0 + 10, g.W - MARGIN * 2 - PANEL_PAD * 2, HINT_H,
         g_st.resultIsErr ? DANGER : PURPLE_DARK, 18, true);

    // 左栏：设备信息（两列排布）
    int infoX = g.x0 + MARGIN;
    Panel(dc, infoX, g.top, g.leftW, g.infoH);
    Text(dc, L"设备信息", infoX + PANEL_PAD, g.top + 6, 300, 34, INK, 24, true);
    int colW = (g.leftW - PANEL_PAD * 2) / 2;
    int y0 = g.top + 50;
    int rowH = HomeRowH(g.infoH, (int)g_infoLines.size());
    for (size_t i = 0; i < g_infoLines.size(); ++i) {
        int col = (int)(i % 2), row = (int)(i / 2);
        int x = infoX + PANEL_PAD + col * colW;
        int y = y0 + row * rowH;
        if (y + rowH > g.top + g.infoH - 6) break;
        Text(dc, g_infoLines[i].label, x, y, INFO_LABEL_W, rowH, INK, 16, true);
        Text(dc, g_infoLines[i].value, x + INFO_VALUE_DX, y, colW - INFO_VALUE_DX - 6, rowH,
             g_infoLines[i].color, 16);
    }

    // 右列：设备连接 + 设备资源
    Panel(dc, g.rx, g.top, g.rightW, g.connH);
    Text(dc, L"设备连接", g.rx + PANEL_PAD, g.top + 6, 300, 34, INK, 24, true);
    bool conn = snapTop("connected"); bool dry = snapTop("dry_run");
    int kvW = g.rightW - PANEL_PAD * 2 - 94;
    Text(dc, L"端口", g.rx + PANEL_PAD, g.top + 46, 88, 26, INK, 16, true);
    Text(dc, to_ws(snapTopS("port")).empty() ? L"—" : to_ws(snapTopS("port")),
         g.rx + PANEL_PAD + 94, g.top + 46, kvW, 26, INK, 16);
    Text(dc, L"状态", g.rx + PANEL_PAD, g.top + 74, 88, 26, INK, 16, true);
    Text(dc, dry ? L"DRYRUN" : (conn ? L"在线" : L"离线"), g.rx + PANEL_PAD + 94, g.top + 74,
         kvW, 26, dry ? WARN : (conn ? OK : DANGER), 16, true);
    Text(dc, L"最近通信", g.rx + PANEL_PAD, g.top + 102, 88, 24, INK, 16, true);
    std::wstring lc = to_ws(snapS("comm", "at"));
    Text(dc, lc.empty() ? L"—" : lc, g.rx + PANEL_PAD + 94, g.top + 102, kvW, 24, MUTED, 15);

    int resTop = g.top + g.connH + PANEL_GAP, resH = g.infoH - g.connH - PANEL_GAP;
    Panel(dc, g.rx, resTop, g.rightW, resH);
    Text(dc, L"设备资源", g.rx + PANEL_PAD, resTop + 6, 300, 34, INK, 24, true);
    int resW = g.rightW - PANEL_PAD * 2;
    Text(dc, L"内部存储 / 运行内存 / 电池", g.rx + PANEL_PAD, resTop + 46, resW, 26, MUTED, 15);
    Text(dc, L"未接入（协议未确认）", g.rx + PANEL_PAD, resTop + 72, resW, 26, MUTED, 15);
    Text(dc, L"审计日志：" + to_ws(snapTopS("jsonl")), g.rx + PANEL_PAD, resTop + 106,
         resW, 24, MUTED, 14);
    Text(dc, L"运行日志：" + to_ws(snapTopS("log")), g.rx + PANEL_PAD, resTop + 132,
         resW, 24, MUTED, 14);

    // 底部：快捷操作（左）+ 运行开关（右）
    Panel(dc, g.x0 + MARGIN, g.cTop, g.blW, g.cH);
    Text(dc, L"快捷操作", g.x0 + MARGIN + PANEL_PAD, g.cTop + 6, 300, 34, INK, 24, true);
    // 按钮由子窗口实现（见 CreateHomeControls），这里只画面板底色

    Panel(dc, g.ctlX, g.cTop, g.ctlW, g.cH);
    Text(dc, L"运行开关", g.ctlX + PANEL_PAD, g.cTop + 6, 300, 34, INK, 24, true);

    // 速度档（－ 值 ＋）+ 字间距 / Z 偏移标签（与控件对齐，坐标同源）
    Text(dc, L"速度档", g.spdX, g.rowY[0], g.lblW, 28, INK, 16, true);
    Text(dc, L"字间距", g.spdX, g.rowY[1], g.lblW, 26, INK, 16, true);
    Text(dc, L"Z 偏移", g.spdX, g.rowY[2], g.lblW, 26, INK, 16, true);
    Text(dc, fmt(L"%d", snapI("cfg", "speed", 3)),
         g_speedRect.left, g_speedRect.top, g_speedRect.right - g_speedRect.left, 28,
         PURPLE_DARK, 18, true, DT_CENTER);
}

static void DrawConnect(HDC dc, RECT& rc) {
    TwoColGeo g = TwoColLayout(rc, CONNECT_PANEL_H);

    Panel(dc, g.leftX, g.top, g.colW, g.panelH);
    Text(dc, L"串口连接", g.innerX, g.top + 6, 300, 34, INK, 24, true);
    // 串口下拉框与按钮为子窗口控件（见 CreateConnectControls），此处补静态标签
    Text(dc, L"协议固定：9600 / Even / 8 / 1（RS485 · Modbus RTU）",
         g.innerX, g.top + 196, g.innerW, 24, MUTED, 14);

    Panel(dc, g.rightX, g.top, g.colW, g.panelH);
    Text(dc, L"连接诊断", g.rightX + PANEL_PAD, g.top + 6, 300, 34, INK, 24, true);
    struct Diag { std::wstring v, k; COLORREF c; };
    bool conn = snapTop("connected"); bool dry = snapTop("dry_run");
    std::wstring ack = to_ws(snapS("comm", "ack"));
    Diag ds[6] = {
        { dry ? L"DRYRUN" : (conn ? L"在线" : L"离线"), L"串口句柄", dry ? WARN : (conn ? OK : DANGER) },
        { ack == L"valid" ? L"有效" : (ack.empty() ? L"—" : L"无"), L"最近 ACK", ack == L"valid" ? OK : WARN },
        { fmt(L"%d", snapI("comm", "fail")), L"连续失败", snapI("comm", "fail") > 0 ? DANGER : OK },
        { L"800 ms", L"读取超时", MUTED },
        { to_ws(snapS("comm", "at")).empty() ? L"—" : to_ws(snapS("comm", "at")), L"最近通信", MUTED },
        { to_ws(snapS("comm", "op")).empty() ? L"—" : to_ws(snapS("comm", "op")), L"最近操作", MUTED },
    };
    int chipGap = 10;
    int chipW = (g.innerW - chipGap * 2) / 3;
    int chipH = 70;
    for (int i = 0; i < 6; ++i) {
        int cx = g.rightX + PANEL_PAD + (i % 3) * (chipW + chipGap);
        int cy = g.top + 50 + (i / 3) * (chipH + chipGap);
        RECT chr{ cx, cy, cx + chipW, cy + chipH };
        FillRect(dc, &chr, g_brWhite);
        rr(dc, chr.left, chr.top, chipW, chipH, 12, LINE);
        Text(dc, ds[i].v, cx + 10, cy + 4, chipW - 20, 30, ds[i].c, 19, true);
        Text(dc, ds[i].k, cx + 10, cy + 34, chipW - 20, 24, MUTED, 14);
    }

    int noteTop = g.top + g.panelH + PANEL_GAP;
    int noteH = (g.y0 + g.H - 10) - noteTop;
    if (noteH < 60) noteH = 60;
    if (noteH > 72) noteH = 72;   // 说明条按内容高度收紧，避免整块空白
    Panel(dc, g.x0 + MARGIN, noteTop, g.W - MARGIN * 2, noteH);
    RECT note{ g.x0 + MARGIN + PANEL_PAD, noteTop + 12,
               g.x0 + g.W - MARGIN - PANEL_PAD, noteTop + noteH - 12 };
    FillRect(dc, &note, g_brWarn);
    Text(dc, L"当前协议实现可记录 TX/RX 帧和 ACK 校验结果；尚未确认控制器是否提供独立状态、报警和真实坐标寄存器。",
         note.left + 10, note.top, note.right - note.left - 20, note.bottom - note.top, RGB(129, 87, 28), 16);
}

static void DrawWrite(HDC dc, RECT& rc) {
    TwoColGeo g = TwoColLayout(rc, WRITE_PANEL_H);

    Panel(dc, g.leftX, g.top, g.colW, g.panelH);
    Text(dc, L"输入与任务预检", g.innerX, g.top + 6, 300, 34, INK, 24, true);

    Panel(dc, g.rightX, g.top, g.colW, g.panelH);
    Text(dc, L"任务进度", g.rightX + PANEL_PAD, g.top + 6, 300, 34, INK, 24, true);
    struct S { std::wstring v, k; COLORREF c; };
    bool active = snapB("task", "active");
    S ss[5] = {
        { to_ws(snapS("task", "stage")), L"当前阶段", active ? PURPLE_DARK : INK },
        { fmt(L"%d / %d", snapI("task", "chars_done"), snapI("task", "chars_total")), L"字符", INK },
        { fmt(L"%d / %d", snapI("task", "traj_done"), snapI("task", "traj_total")), L"轨迹点", INK },
        { snapB("task", "pen_down") ? L"落笔" : L"抬笔", L"笔状态", snapB("task", "pen_down") ? DANGER : OK },
        { snapI("task", "dip_done") > 0 ? fmt(L"%d 次", snapI("task", "dip_done")) : (snapB("cfg", "enable_dip") ? L"未执行" : L"未启用"), L"蘸墨", INK },
    };
    int chipGap = 8;
    int chipW = (g.innerW - chipGap * 4) / 5;
    int chipH = 64;
    for (int i = 0; i < 5; ++i) {
        int cx = g.rightX + PANEL_PAD + i * (chipW + chipGap);
        int cy = g.top + 50;
        RECT chr{ cx, cy, cx + chipW, cy + chipH };
        FillRect(dc, &chr, g_brWhite);
        rr(dc, chr.left, chr.top, chipW, chipH, 12, LINE);
        Text(dc, ss[i].v, cx + 8, cy + 2, chipW - 16, 28, ss[i].c, 17, true);
        Text(dc, ss[i].k, cx + 8, cy + 30, chipW - 16, 22, MUTED, 13);
    }

    // 预览区（占满面板剩余高度）
    int pvTop = g.top + 50 + chipH + 12;
    int pvBot = g.top + g.panelH - PANEL_PAD;
    if (pvBot - pvTop < 60) pvBot = pvTop + 60;
    RECT pv{ g.rightX + PANEL_PAD, pvTop, g.rightX + g.colW - PANEL_PAD, pvBot };
    FillRect(dc, &pv, g_brSoft);
    HPEN pen = CreatePen(PS_SOLID, 2, RGB(185, 201, 219));
    HPEN op = (HPEN)SelectObject(dc, pen);
    SelectObject(dc, op);
    // 图形区避开底部提示行，否则弧线会压在提示文字上
    int gfxTop = pv.top + 6, gfxBot = pv.bottom - 28;
    if (gfxBot - gfxTop < 40) gfxBot = gfxTop + 40;
    int cxm = (pv.left + pv.right) / 2, cym = (gfxTop + gfxBot) / 2;
    int rh = (gfxBot - gfxTop) / 2, rw = rh * 7 / 6;
    MoveToEx(dc, pv.left + 6, cym, nullptr); LineTo(dc, pv.right - 6, cym);
    MoveToEx(dc, cxm, gfxTop, nullptr); LineTo(dc, cxm, gfxBot);
    HPEN pp = CreatePen(PS_SOLID, 4, PURPLE);
    SelectObject(dc, pp);
    Arc(dc, cxm - rw, cym - rh, cxm + rw, cym + rh, 0, 0, 0, 0);
    SelectObject(dc, op); DeleteObject(pp); DeleteObject(pen);
    // 蘸墨合规提示
    Text(dc, L"比赛要求：书法与国画均需自主蘸墨至少一次（运行开关可启用蘸墨）",
         pv.left + 8, pv.bottom - 24, pv.right - pv.left - 16, 20, MUTED, 13);
}

// ---------------- 导航 ----------------
static const struct { const wchar_t* icon; const wchar_t* name; int id; } NAV[] = {
    { L"⌂", L"主页",        ID_PAGE_HOME },
    { L"▣", L"设备连接",    ID_PAGE_CONNECT },
    { L"✎", L"书写任务",    ID_PAGE_WRITE },
};
static const int NAV_N = 3;

static void DrawNav(HDC dc) {
    static HBRUSH brPurpleBar = CreateSolidBrush(PURPLE);
    int y = TITLE_H + 14;
    for (int i = 0; i < NAV_N; ++i) {
        int hh = 58;
        if (g_st.page == NAV[i].id) {
            RECT sel{ 0, y, NAV_W, y + hh };
            FillRect(dc, &sel, g_brSoft);
            RECT bar{ 0, y, 6, y + hh };
            FillRect(dc, &bar, brPurpleBar);
        }
        else if (g_st.navHover == i) {
            RECT hov{ 0, y, NAV_W, y + hh };
            FillRect(dc, &hov, g_brBg);
        }
        Text(dc, NAV[i].icon, 30, y, 30, hh, RGB(52, 52, 52), 21);
        Text(dc, NAV[i].name, 72, y, NAV_W - 80, hh, INK, 18, g_st.page == NAV[i].id);
        y += hh;
    }
}

// ---------------- 按钮子窗口（owner-draw 着色） ----------------
struct BtnDef { const wchar_t* text; int id; COLORREF color; };
static void MakeBtn(HWND parent, const BtnDef& d, int x, int y, int w, int h) {
    HWND b = CreateWindowW(L"BUTTON", d.text, WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_OWNERDRAW,
                           x, y, w, h, parent, (HMENU)(INT_PTR)d.id, nullptr, nullptr);
    SendMessage(b, WM_SETFONT, (WPARAM)g_fontBold20, TRUE);
    int slot = d.id - 1000;
    if (slot >= 0 && slot < 64) g_btns[slot] = b;
}

// owner-draw：圆角实底色按钮（对应 HTML .button）
static void DrawBtn(LPDRAWITEMSTRUCT di) {
    int slot = (int)(UINT_PTR)GetDlgCtrlID(di->hwndItem) - 1000;
    COLORREF base = BLUE;
    if (slot >= 0 && slot < 64) {
        switch (GetDlgCtrlID(di->hwndItem)) {
        case IDC_BTN_TESTPT: case IDC_BTN_CONNECT: case IDC_BTN_WRITE: base = TEAL; break;
        case IDC_BTN_ESTOP: case IDC_BTN_STOP: base = DANGER; break;
        case IDC_BTN_DISCONNECT: case IDC_BTN_RESET: case IDC_BTN_CENTER:
        case IDC_BTN_REFRESH: case IDC_BTN_REFRESHDEV: base = BLUE; break;
        default: base = PURPLE; break;
        }
    }
    bool hot = (di->itemState & ODS_FOCUS) != 0;
    if (di->itemState & ODS_SELECTED) base = RGB(GetRValue(base) * 3 / 4, GetGValue(base) * 3 / 4, GetBValue(base) * 3 / 4);
    HBRUSH br = CreateSolidBrush(base);
    RECT rc = di->rcItem;
    HRGN rgn = CreateRoundRectRgn(rc.left, rc.top, rc.right, rc.bottom, 12, 12);
    HBRUSH ob = (HBRUSH)SelectObject(di->hDC, br);
    FillRgn(di->hDC, rgn, br);
    SelectObject(di->hDC, ob);
    DeleteObject(rgn);
    DeleteObject(br);
    SetBkMode(di->hDC, TRANSPARENT);
    SetTextColor(di->hDC, WHITE);
    wchar_t txt[128];
    GetWindowTextW(di->hwndItem, txt, 128);
    HFONT of = (HFONT)SelectObject(di->hDC, g_fontBold20);
    RECT tr = rc;
    DrawTextW(di->hDC, txt, -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(di->hDC, of);
    (void)hot;
}

// ---------------- 主窗绘制 ----------------
static void OnPaint(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC screen = BeginPaint(hwnd, &ps);
    RECT crc; GetClientRect(hwnd, &crc);
    int w = crc.right, h = crc.bottom;

    // ★双缓冲：全部绘制先画进内存位图，最后一次 BitBlt 上屏，消除 500ms 定时重绘造成的闪烁
    HDC dc = CreateCompatibleDC(screen);
    HBITMAP hbmBuf = CreateCompatibleBitmap(screen, w, h);
    HBITMAP hbmOld = (HBITMAP)SelectObject(dc, hbmBuf);

    RECT full{ 0, 0, w, h };
    FillRect(dc, &full, g_brBg);

    // 内容区背景
    RECT content{ NAV_W, TITLE_H, w, h - FOOTER_H };
    FillRect(dc, &content, g_brBg);

    // 标题栏
    RECT tr{ 0, 0, w, TITLE_H };
    FillRect(dc, &tr, g_brTitle);
    Text(dc, L"书画机械臂调试助手", 18, 0, 400, TITLE_H, WHITE, 26, true);

    // 导航
    DrawNav(dc);

    // 页面（pageRc 与 Create*Controls 的 PageRectFromClient 完全一致）
    BuildInfoLines();   // ★每次绘制前重建信息行（快照由轮询/定时器更新）
    RECT pageRc = PageRectFromClient(w, h);
    if (g_st.page == ID_PAGE_HOME)        DrawHome(dc, pageRc);
    else if (g_st.page == ID_PAGE_CONNECT) DrawConnect(dc, pageRc);
    else                                    DrawWrite(dc, pageRc);

    // 底部状态栏
    RECT fr{ 0, h - FOOTER_H, w, h };
    FillRect(dc, &fr, g_brFooter);
    Text(dc, L"设备调试助手 v0.1", 18, h - FOOTER_H, 160, FOOTER_H, PURPLE, 15, true);
    bool estop = snapTop("estop");
    bool active = snapB("task", "active");
    std::wstring st = active ? L"任务运行中" : (estop ? L"急停已触发" : L"空闲");
    Text(dc, L"状态：" + st + (snapTop("dry_run") ? L"　|　DRYRUN" : L"") + L"　|　底层：Modbus RTU",
         200, h - FOOTER_H, w - 400, FOOTER_H, MUTED, 15);
    Text(dc, estop ? L"急停" : L"GUI", w - 70, h - FOOTER_H, 60, FOOTER_H, estop ? DANGER : MUTED, 15, true);

    // ★一次性上屏，释放双缓冲资源
    BitBlt(screen, 0, 0, w, h, dc, 0, 0, SRCCOPY);
    SelectObject(dc, hbmOld);
    DeleteObject(hbmBuf);
    DeleteDC(dc);
    EndPaint(hwnd, &ps);
}

// ---------------- 子控件创建（每页切换重建） ----------------
static void DestroyPageControls() {
    for (auto& b : g_btns) if (b) { DestroyWindow(b); b = nullptr; }
    for (auto& c : g_checks) if (c) { DestroyWindow(c); c = nullptr; }
    if (g_comboPort) { DestroyWindow(g_comboPort); g_comboPort = nullptr; }
    if (g_editText) { DestroyWindow(g_editText); g_editText = nullptr; }
    if (g_editSpacing) { DestroyWindow(g_editSpacing); g_editSpacing = nullptr; }
    if (g_editZoff) { DestroyWindow(g_editZoff); g_editZoff = nullptr; }
    if (g_editPreview) { DestroyWindow(g_editPreview); g_editPreview = nullptr; }
}

static HWND MakeCheck(HWND parent, int id, const wchar_t* text, int x, int y, int w, int h) {
    HWND c = CreateWindowW(L"BUTTON", text, WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                           x, y, w, h, parent, (HMENU)(INT_PTR)id, nullptr, nullptr);
    SendMessage(c, WM_SETFONT, (WPARAM)g_font18, TRUE);
    int idx = id - IDC_CHECK_DRY;
    if (idx >= 0 && idx < 16) g_checks[idx] = c;
    return c;
}

static void CreateHomeControls(HWND hwnd) {
    // 布局与 DrawHome 共用 HomeLayout()，保证面板与子控件严格对齐
    RECT crc; GetClientRect(hwnd, &crc);
    HomeGeo g = HomeLayout(PageRectFromClient(crc.right, crc.bottom));

    struct B { const wchar_t* t; int id; } bs[] = {
        { L"⏻ 系统复位", IDC_BTN_RESET },
        { L"↕ 回中心点", IDC_BTN_CENTER },
        { L"↯ 发送测试点", IDC_BTN_TESTPT },
        { L"▱ 查询位姿", IDC_BTN_QUERY },
        { L"↻ 刷新设备", IDC_BTN_REFRESHDEV },
        { L"■ 急停", IDC_BTN_ESTOP },
        { L"□ 解除急停", IDC_BTN_ESTOP_CLEAR },
    };
    for (size_t i = 0; i < sizeof(bs) / sizeof(bs[0]); ++i) {
        MakeBtn(hwnd, { bs[i].t, bs[i].id, PURPLE },
                 g.btnX + (int)(i % 2) * (g.btnW + 24),
                 g.btnY + (int)(i / 2) * BTN_PITCH, g.btnW, BTN_H);
    }

    int kx = g.chkX, ky = g.cTop + 52;
    bool dry = snapTop("dry_run");
    HWND c1 = MakeCheck(hwnd, IDC_CHECK_DRY, L"Dry Run（不连接真实串口）", kx, ky, g.chkW, CHK_H);
    SendMessage(c1, BM_SETCHECK, dry ? BST_CHECKED : BST_UNCHECKED, 0);
    HWND c2 = MakeCheck(hwnd, IDC_CHECK_AUTODRAW, L"自动描边（写字完成后作画）", kx, ky + CHK_PITCH, g.chkW, CHK_H);
    SendMessage(c2, BM_SETCHECK, snapB("cfg", "auto_draw") ? BST_CHECKED : BST_UNCHECKED, 0);
    HWND c3 = MakeCheck(hwnd, IDC_CHECK_HQ, L"高质模式（更慢更稳）", kx, ky + CHK_PITCH * 2, g.chkW, CHK_H);
    SendMessage(c3, BM_SETCHECK, snapB("cfg", "high_quality") ? BST_CHECKED : BST_UNCHECKED, 0);
    HWND c4 = MakeCheck(hwnd, IDC_CHECK_DIP, L"蘸墨功能", kx, ky + CHK_PITCH * 3, g.chkW, CHK_H);
    SendMessage(c4, BM_SETCHECK, snapB("cfg", "enable_dip") ? BST_CHECKED : BST_UNCHECKED, 0);
    HWND c5 = MakeCheck(hwnd, IDC_CHECK_LOG, L"运行日志记录", kx, ky + CHK_PITCH * 4, g.chkW, CHK_H);
    SendMessage(c5, BM_SETCHECK, snapB("cfg", "log") ? BST_CHECKED : BST_UNCHECKED, 0);

    // 速度 ± / 字间距 / Z 偏移（标签由 DrawHome 绘制，坐标同源）
    int sx = g.spdX;
    MakeBtn(hwnd, { L"－", IDC_BTN_SPEED_DEC, PURPLE }, sx + g.lblW + 4, g.rowY[0], g.pbtnW, 28);
    MakeBtn(hwnd, { L"＋", IDC_BTN_SPEED_INC, PURPLE },
            sx + g.lblW + 4 + g.pbtnW + g.valW, g.rowY[0], g.pbtnW, 28);
    g_speedRect = { sx + g.lblW + 4 + g.pbtnW, g.rowY[0], sx + g.lblW + 4 + g.pbtnW + g.valW, g.rowY[0] + 28 };
    g_editSpacing = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                    sx + g.lblW + 4, g.rowY[1], g.editW, 26, hwnd, (HMENU)(INT_PTR)IDC_EDIT_SPACING, nullptr, nullptr);
    SendMessage(g_editSpacing, WM_SETFONT, (WPARAM)g_font16, TRUE);
    SetWindowTextW(g_editSpacing, f1((float)snapD("cfg", "char_spacing", 1.0)).c_str());
    MakeBtn(hwnd, { L"应用", IDC_BTN_SPACING_APPLY, PURPLE },
            sx + g.lblW + 4 + g.editW + 6, g.rowY[1], g.applyW, 26);
    g_editZoff = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                 sx + g.lblW + 4, g.rowY[2], g.editW, 26, hwnd, (HMENU)(INT_PTR)IDC_EDIT_ZOFF, nullptr, nullptr);
    SendMessage(g_editZoff, WM_SETFONT, (WPARAM)g_font16, TRUE);
    SetWindowTextW(g_editZoff, f1((float)snapD("cfg", "z_offset", 0.0)).c_str());
    MakeBtn(hwnd, { L"应用", IDC_BTN_ZOFF_APPLY, PURPLE },
            sx + g.lblW + 4 + g.editW + 6, g.rowY[2], g.applyW, 26);
}

static void CreateConnectControls(HWND hwnd) {
    RECT crc; GetClientRect(hwnd, &crc);
    TwoColGeo g = TwoColLayout(PageRectFromClient(crc.right, crc.bottom), CONNECT_PANEL_H);

    g_comboPort = CreateWindowW(L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                                g.innerX, g.top + 46, g.innerW, 400, hwnd, (HMENU)(INT_PTR)IDC_COMBO_PORT, nullptr, nullptr);
    SendMessage(g_comboPort, WM_SETFONT, (WPARAM)g_font18, TRUE);
    auto ports = gs::list_serial_ports();
    for (auto& p : ports) {
        std::wstring wp = to_ws(p);
        SendMessageW(g_comboPort, CB_ADDSTRING, 0, (LPARAM)wp.c_str());
    }
    // 波特率/校验为协议已确认的固定值（9600/8E1），以静态文本提示，见 DrawConnect

    int by = g.top + 100;
    int bbw = (g.innerW - 24) / 2;
    MakeBtn(hwnd, { L"连接设备", IDC_BTN_CONNECT, TEAL }, g.innerX, by, bbw, BTN_H);
    MakeBtn(hwnd, { L"断开连接", IDC_BTN_DISCONNECT, BLUE }, g.innerX + bbw + 24, by, bbw, BTN_H);
    MakeBtn(hwnd, { L"刷新串口", IDC_BTN_REFRESH, BLUE }, g.innerX, by + BTN_PITCH, bbw, BTN_H);
    MakeBtn(hwnd, { L"发送心跳", IDC_BTN_HEARTBEAT, PURPLE }, g.innerX + bbw + 24, by + BTN_PITCH, bbw, BTN_H);

    // 提示文本（静态绘制）
}

static void CreateWriteControls(HWND hwnd) {
    RECT crc; GetClientRect(hwnd, &crc);
    TwoColGeo g = TwoColLayout(PageRectFromClient(crc.right, crc.bottom), WRITE_PANEL_H);

    g_editText = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                 WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | WS_VSCROLL,
                                 g.innerX, g.top + 46, g.innerW, 84, hwnd, (HMENU)(INT_PTR)IDC_EDIT_TEXT, nullptr, nullptr);
    SendMessage(g_editText, WM_SETFONT, (WPARAM)g_font18, TRUE);
    std::string last = gs::load_last_task_text();
    if (!last.empty()) SetWindowTextW(g_editText, to_ws(last).c_str());

    int y = g.top + 46 + 84 + 12;
    int bbw = (g.innerW - 24 * 2) / 3;
    struct B { const wchar_t* t; int id; } bs[] = {
        { L"预检任务", IDC_BTN_QUERY },
        { L"开始书写", IDC_BTN_WRITE },
        { L"停止任务", IDC_BTN_STOP },
    };
    for (size_t i = 0; i < 3; ++i)
        MakeBtn(hwnd, { bs[i].t, bs[i].id, PURPLE }, g.innerX + (int)i * (bbw + 24), y, bbw, BTN_H);

    // 预检结果区（只读 EDIT）
    int prevTop = y + BTN_H + 12;
    int prevH = g.top + g.panelH - PANEL_PAD - prevTop;
    if (prevH < 48) prevH = 48;
    g_editPreview = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                    WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | WS_VSCROLL,
                                    g.innerX, prevTop, g.innerW, prevH, hwnd, (HMENU)(INT_PTR)IDC_EDIT_PREVIEW, nullptr, nullptr);
    SendMessage(g_editPreview, WM_SETFONT, (WPARAM)g_font16, TRUE);
    SetWindowTextW(g_editPreview, L"点击“预检任务”查看字号、布局与速度估算。");
}

// ---------------- 事件处理 ----------------
static std::wstring GetEditW(HWND e) {
    int len = GetWindowTextLengthW(e);
    std::wstring s(len + 1, L'\0');
    GetWindowTextW(e, &s[0], len + 1);
    s.resize(len);
    return s;
}

static void OnCommand(HWND hwnd, int id, HWND ctl, int code) {
    (void)ctl; (void)code;
    std::string err;
    switch (id) {
    case IDC_BTN_CONNECT: {
        wchar_t buf[64] = { 0 };
        int sel = (int)SendMessageW(g_comboPort, CB_GETCURSEL, 0, 0);
        if (sel == CB_ERR) { SetResult(L"请先选择串口。", true); return; }
        SendMessageW(g_comboPort, CB_GETLBTEXT, sel, (LPARAM)buf);
        gs::set_dry_run(false);
        // ★异步连接：open 在工作线程执行，UI 立即返回，不再因蓝牙虚拟口阻塞
        gs::connect_async(to_u8(buf));
        g_connWatching = true;
        SetResult(fmt(L"正在连接 %s …", buf));
        break;
    }
    case IDC_BTN_DISCONNECT:
        gs::disconnect();
        SetResult(L"已断开串口。");
        break;
    case IDC_BTN_REFRESH: {
        SendMessageW(g_comboPort, CB_RESETCONTENT, 0, 0);
        auto ports = gs::list_serial_ports();
        for (auto& p : ports)
            SendMessageW(g_comboPort, CB_ADDSTRING, 0, (LPARAM)to_ws(p).c_str());
        SetResult(fmt(L"已刷新串口列表（%d 个可用）。", (int)ports.size()));
        break;
    }
    case IDC_BTN_HEARTBEAT:
        if (gs::heartbeat(err)) SetResult(L"心跳已发送（抬笔点至中心）。");
        else SetResult(fmt(L"心跳失败：%s", to_ws(err).c_str()), true);
        break;
    case IDC_BTN_RESET:
        if (!gs::is_connected() && !snapTop("dry_run")) { SetResult(L"串口未连接。", true); break; }
        if (gs::go_center(err)) SetResult(L"已发送回中心点指令（抬笔）。");
        else SetResult(fmt(L"复位失败：%s", to_ws(err).c_str()), true);
        break;
    case IDC_BTN_CENTER:
        if (!gs::is_connected() && !snapTop("dry_run")) { SetResult(L"串口未连接。", true); break; }
        if (gs::go_center(err)) SetResult(L"已复位到中心。");
        else SetResult(fmt(L"复位失败：%s", to_ws(err).c_str()), true);
        break;
    case IDC_BTN_TESTPT:
        if (gs::send_test_point(err)) SetResult(L"测试点已发送（抬笔安全点）。");
        else SetResult(fmt(L"测试点失败：%s", to_ws(err).c_str()), true);
        break;
    case IDC_BTN_QUERY: {
        std::wstring p = gs::query_pose_line();
        if (g_st.page == ID_PAGE_WRITE && g_editPreview) {
            std::wstring r = p + L"\r\n审计日志：" + to_ws(gs::jsonl_path());
            SetWindowTextW(g_editPreview, r.c_str());
        }
        SetResult(p);
        break;
    }
    case IDC_BTN_REFRESHDEV:
        SetResult(L"已刷新设备状态。");
        break;
    case IDC_BTN_ESTOP:
        gs::request_estop();
        SetResult(L"急停已触发：任务停止、软件锁存；真机请同时按硬件急停/断电。", true);
        break;
    case IDC_BTN_ESTOP_CLEAR:
        gs::clear_estop();
        SetResult(L"急停标志已清除（软件层）。");
        break;
    case IDC_BTN_WRITE: {
        if (gs::task_active()) { SetResult(L"任务已在运行。", true); break; }
        std::wstring t = GetEditW(g_editText);
        if (t.empty()) { SetResult(L"请输入要书写的汉字或诗句。", true); break; }
        if (gs::start_write(to_u8(t), err)) SetResult(L"书写任务已启动（预热→书写→可选蘸墨→自动描边）。");
        else SetResult(fmt(L"启动失败：%s", to_ws(err).c_str()), true);
        break;
    }
    case IDC_BTN_STOP:
        if (gs::abort_task()) SetResult(L"已请求停止任务（经急停通道，抬起笔后结束）。", true);
        else SetResult(L"当前没有运行中的任务。");
        break;
    case IDC_BTN_SPEED_DEC:
        if (gs::set_speed(snapI("cfg", "speed", 3) - 1)) SetResult(L"速度已调低一档。");
        else SetResult(L"速度已是最低档。");
        break;
    case IDC_BTN_SPEED_INC:
        if (gs::set_speed(snapI("cfg", "speed", 3) + 1)) SetResult(L"速度已调高一档。");
        else SetResult(L"速度已是最高档。");
        break;
    case IDC_BTN_SPACING_APPLY: {
        std::wstring v = GetEditW(g_editSpacing);
        float f = (float)_wtof(v.c_str());
        if (gs::set_char_spacing(f)) SetResult(fmt(L"字间距已设为 %s mm。", f1(f).c_str()));
        else SetResult(L"字间距输入无效（需 0.5~50）。", true);
        break;
    }
    case IDC_BTN_ZOFF_APPLY: {
        std::wstring v = GetEditW(g_editZoff);
        float f = (float)_wtof(v.c_str());
        if (gs::set_z_offset(f)) SetResult(fmt(L"Z_OFFSET 已设为 %s mm。", f1(f).c_str()));
        else SetResult(L"Z_OFFSET 输入无效（需 -100~100）。", true);
        break;
    }
    case IDC_CHECK_DRY: {
        bool on = (SendMessage(g_checks[id - IDC_CHECK_DRY], BM_GETCHECK, 0, 0) == BST_CHECKED);
        if (!gs::set_dry_run(on)) {
            SendMessage(g_checks[id - IDC_CHECK_DRY], BM_SETCHECK, on ? BST_UNCHECKED : BST_CHECKED, 0);
            SetResult(L"任务运行中，不能切换 Dry Run。", true);
        }
        else SetResult(on ? L"Dry Run 已开启（不连接串口，仅打印帧）。" : L"Dry Run 已关闭。");
        break;
    }
    case IDC_CHECK_AUTODRAW:
        gs::toggle_auto_draw(); SetResult(L"自动描边开关已切换。"); break;
    case IDC_CHECK_HQ:
        gs::toggle_high_quality(); SetResult(L"高质模式开关已切换。"); break;
    case IDC_CHECK_DIP:
        gs::toggle_enable_dip(); SetResult(L"蘸墨功能开关已切换。"); break;
    case IDC_CHECK_LOG:
        gs::set_log_enable(SendMessage(g_checks[id - IDC_CHECK_DRY], BM_GETCHECK, 0, 0) == BST_CHECKED);
        SetResult(L"运行日志开关已切换。");
        break;
    default: break;
    }
    InvalidateRect(hwnd, nullptr, FALSE);
}

// 预检（书写页“预检任务”按钮专用路径）
static void DoPreflight(HWND hwnd) {
    if (!g_editText) return;
    std::wstring t = GetEditW(g_editText);
    if (t.empty()) { SetResult(L"请先输入文本。", true); return; }
    nlohmann::json r = gs::preflight(to_u8(t));
    std::wstring out;
    if (r.value("layout_ok", false)) {
        out = fmt(L"排版可行：%d 字，%d 列 × %d 行，字号 %s mm，字间距 %s mm，速度 %d 档。\r\n布局：上区书写 / 下区绘画。%s\r\n（预检不发送任何运动指令）",
                  r.value("char_count", 0), r.value("cols", 0), r.value("rows", 0),
                  f1((float)r.value("char_size", 0.0)).c_str(),
                  f1((float)r.value("spacing", 0.0)).c_str(),
                  r.value("speed", 3),
                  r.value("dry_run", false) ? L"当前 Dry Run。" : L"当前真机模式。");
        SetResult(L"预检通过。");
    }
    else {
        out = L"排版失败：字数过多或区域不足。\r\n";
        SetResult(L"预检失败。", true);
    }
    if (g_editPreview) SetWindowTextW(g_editPreview, out.c_str());
    InvalidateRect(hwnd, nullptr, FALSE);
}

// ---------------- 状态轮询线程 → 定时刷新 ----------------
static std::atomic_bool g_run{ true };
static std::thread g_poll;
static void PollThread() {
    while (g_run) {
        g_st.snap = gs::snapshot();
        if (g_st.hwnd) InvalidateRect(g_st.hwnd, nullptr, FALSE);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}

// ---------------- 窗口过程 ----------------
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        g_st.hwnd = hwnd;
        g_st.snap = gs::snapshot();
        CreateHomeControls(hwnd);
        SetTimer(hwnd, IDT_TIMER, 500, nullptr);
        return 0;
    }
    case WM_TIMER:
        if (wp == IDT_TIMER) {
            g_st.snap = gs::snapshot();
            // ★异步连接完成回报：pending 由 true 转 false 时刷新结果（UI 全程未阻塞）
            if (g_connWatching && !snapB("connect", "pending")) {
                g_connWatching = false;
                std::string m = snapS("connect", "message");
                std::string state = snapS("connect", "state");
                SetResult(m.empty() ? L"连接已结束。" : to_ws(m), state == "fail");
            }
            // 配置同步到复选框（任务后自动归位）
            static bool s_lastTask = false;
            bool act = snapB("task", "active");
            if (s_lastTask && !act) {
                if (g_checks[0]) SendMessage(g_checks[0], BM_SETCHECK, snapTop("dry_run") ? BST_CHECKED : BST_UNCHECKED, 0);
                if (g_checks[1]) SendMessage(g_checks[1], BM_SETCHECK, snapB("cfg", "auto_draw") ? BST_CHECKED : BST_UNCHECKED, 0);
                if (g_checks[2]) SendMessage(g_checks[2], BM_SETCHECK, snapB("cfg", "high_quality") ? BST_CHECKED : BST_UNCHECKED, 0);
                if (g_checks[3]) SendMessage(g_checks[3], BM_SETCHECK, snapB("cfg", "enable_dip") ? BST_CHECKED : BST_UNCHECKED, 0);
            }
            s_lastTask = act;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        break;
    case WM_COMMAND: {
        int id = LOWORD(wp);
        // 书写页“预检任务”与主页“查询位姿”共用 ID，按页面分流
        if (id == IDC_BTN_QUERY && g_st.page == ID_PAGE_WRITE) { DoPreflight(hwnd); return 0; }
        OnCommand(hwnd, id, (HWND)lp, HIWORD(wp));
        return 0;
    }
    case WM_DRAWITEM: {
        DRAWITEMSTRUCT* di = (DRAWITEMSTRUCT*)lp;
        if (di && (di->CtlType == ODT_BUTTON)) { DrawBtn(di); return TRUE; }
        return 0;
    }
    case WM_LBUTTONDOWN: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        if (x < NAV_W && y > TITLE_H) {
            int idx = (y - TITLE_H - 14) / 58;
            if (idx >= 0 && idx < NAV_N) {
                int np = NAV[idx].id;
                if (np != g_st.page) {
                    g_st.page = np;
                    DestroyPageControls();
                    if (np == ID_PAGE_HOME) CreateHomeControls(hwnd);
                    else if (np == ID_PAGE_CONNECT) CreateConnectControls(hwnd);
                    else CreateWriteControls(hwnd);
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                return 0;
            }
        }
        break;
    }
    case WM_MOUSEMOVE: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        int hover = -1;
        if (x < NAV_W && y > TITLE_H) {
            int idx = (y - TITLE_H - 14) / 58;
            if (idx >= 0 && idx < NAV_N) hover = idx;
        }
        if (hover != g_st.navHover) { g_st.navHover = hover; InvalidateRect(hwnd, nullptr, FALSE); }
        break;
    }
    case WM_SIZE:
        // 窗口尺寸变化：重建当前页控件，保持布局与绘制一致
        if (g_st.hwnd) {
            DestroyPageControls();
            if (g_st.page == ID_PAGE_HOME) CreateHomeControls(hwnd);
            else if (g_st.page == ID_PAGE_CONNECT) CreateConnectControls(hwnd);
            else CreateWriteControls(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_CTLCOLORSTATIC:
        return (LRESULT)GetStockObject(WHITE_BRUSH);
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        OnPaint(hwnd);
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, IDT_TIMER);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------- 入口 ----------------
int Run() {
    // 高 DPI
    HINSTANCE hInst = GetModuleHandleW(nullptr);
    SetProcessDPIAware();

    // 初始化服务层：GUI 来源 = HUMAN
    gs::set_source("GUI", "HUMAN");
    gs::cfg_load();
    g_st.snap = gs::snapshot();

    g_brPanel = CreateSolidBrush(WHITE);
    g_brBg = CreateSolidBrush(BG);
    g_brTitle = CreateSolidBrush(PURPLE);
    g_brFooter = CreateSolidBrush(RGB(247, 233, 252));
    g_brSoft = CreateSolidBrush(PURPLE_SOFT);
    g_brWhite = CreateSolidBrush(WHITE);
    g_brWarn = CreateSolidBrush(RGB(255, 247, 233));
    g_brDangerSoft = CreateSolidBrush(RGB(255, 235, 238));

    g_font18 = CreateFontW(-18, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                           CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei");
    g_fontBold20 = CreateFontW(-20, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                               CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei");
    g_font16 = CreateFontW(-16, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                           CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei");

    WNDCLASSW wc{};
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = g_brBg;
    wc.lpszClassName = L"RobotGuiWnd";
    RegisterClassW(&wc);

    // 记忆窗口尺寸（默认 1494×1204 原型比例）
    int w = WIN_W, h = WIN_H;
    gs::load_window_size(w, h);
    if (w < 1000 || h < 700) { w = WIN_W; h = WIN_H; }
    RECT ad; SystemParametersInfoW(SPI_GETWORKAREA, 0, &ad, 0);
    int cw = w, ch = h;
    if (ad.right - ad.left < w + 16) cw = (int)(ad.right - ad.left - 16);
    if (ad.bottom - ad.top < h + 16) ch = (int)(ad.bottom - ad.top - 16);
    RECT wr{ 0, 0, cw, ch };
    AdjustWindowRect(&wr, WS_OVERLAPPEDWINDOW, FALSE);

    HWND hwnd = CreateWindowW(wc.lpszClassName, L"书画机械臂调试助手",
                              WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                              (ad.right - (wr.right - wr.left)) / 2, (ad.bottom - (wr.bottom - wr.top)) / 2,
                              wr.right - wr.left, wr.bottom - wr.top,
                              nullptr, nullptr, hInst, nullptr);
    if (!hwnd) return 1;
    g_st.hwnd = hwnd;
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    g_run = true;
    g_poll = std::thread(PollThread);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    g_run = false;
    if (g_poll.joinable()) g_poll.join();

    // 记忆窗口尺寸
    RECT cr; GetClientRect(hwnd, &cr);
    gs::set_window_size(cr.right, cr.bottom);

    gs::shutdown();
    return (int)msg.wParam;
}

} // namespace gw

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    // --dryrun 命令行支持（与控制台一致）
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; i < argc; i++) {
        if (wcscmp(argv[i], L"--dryrun") == 0) g_dryRun = true;
    }
    LocalFree(argv);
    return gw::Run();
}
