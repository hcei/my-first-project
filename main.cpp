// Robot — 主程序入口（拆分版）
// 原单文件 Robot.cpp 已拆分为：robot_common / serial_port / motion / hanzi / polyline / main 六个编译单元，行为不变。
// 编译（MSVC）：见 Robot.vcxproj（cl /std:c++17 /utf-8）
// 编译（MinGW）：g++ -std=c++17 -g -Wall -Inlohmann robot_common.cpp serial_port.cpp motion.cpp hanzi.cpp polyline.cpp main.cpp -o Robot.exe
// 特性：GBK 控制台、DRYRUN、Modbus 逐点/批量7点、探边、多Z层、安全区、书法(上区) + 线稿(下区)
// 变更：1) 首划稳态：serial_pre_warm + 首落笔点重复发送（参数上调）
//      2) 蘸墨：两次蘸→十字抖→离液面上下抖5次（上慢下快）
//      3) ★新增位姿跟踪：全局记录“最后已知位置”，为首字“到位→等待→落笔”做准备
//      4) ★修复：批量7点成功后位姿更新用 pts7.back()
//      5) ★修复：急停线程常驻、仅ESC触发且不窃取按键；Z越界校验含Z_OFFSET；蘸墨逐点速度生效
//      6) ★工程化拆分：单文件 → 多模块（行为不变）

#include "robot_common.h"
#include "serial_port.h"
#include "motion.h"
#include "hanzi.h"
#include "polyline.h"
#include "shanshui_gen.hpp"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <direct.h>
#include <filesystem>
#include <iostream>
#include <limits>
#include <locale>
#include <sstream>
#include <thread>

// --------------------------- 主题初始化 ---------------------------
static void init_default_themes() {
    g_theme.name = THEME_NAME;
    g_theme.files.clear();
    g_theme.auto_fit = true;
    g_theme.drawZ = Z_DOWN_LIGHT;
    g_theme.speed = 1;
    g_theme.bottom_area = true;
    g_theme.top_ratio = TEXT_TOP_RATIO;
    g_theme.vgap_mm = V_GAP_BETWEEN;
}

// --------------------------- 菜单动作 ---------------------------
static std::string make_autogen_path() {
    _mkdir("themes");
    std::time_t t = std::time(nullptr);
    std::tm tmv{};
#if defined(_WIN32)
    localtime_s(&tmv, &t);
#else
    tmv = *std::localtime(&t);
#endif
    char buf[64];
    std::snprintf(buf, sizeof(buf), "themes/auto_shanshui_%04d%02d%02d_%02d%02d%02d.json",
        tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    return std::string(buf);
}

static std::string find_latest_autogen_json(const std::string& dir = "themes") {
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

static void gotoCenter(SerialPort& sp) {
    Point p{ 0.f,0.f,Z_UP,false,(uint8_t)SPEED_LEVEL,"UP-CENTER" };
    sp.sendPointRetry(p);
}

static bool menu_write_and_draw_capture(SerialPort& sp) {
    wprint(L"输入汉字（支持整首诗句，长文本将采用逐字流式写入）：");
    std::string gbkline; std::getline(std::cin, gbkline);
    std::wstring wtext = mb2w(gbkline);

    TextPlan plan; std::wstring chars;
    if (!prepare_layout_only(wtext, plan, chars)) return false;

    // 写字
    if (!write_text_streamed(sp, chars, plan, SPEED_LEVEL, g_highQuality ? 160 : 120)) {
        wprintln(L"[错误] 流式写字异常终止。");
        return false;
    }

    // 写完后自动作画
    if (g_autoDraw) {
        std::string latest = find_latest_autogen_json("themes");
        if (latest.empty()) {
            wprintln(L"[提示] 未找到 themes/ 下 auto_shanshui_*.json，本次仅书写。");
            return true;
        }
        std::wstringstream ws; ws << L"[信息] 读取线稿JSON：" << mb2w(latest); wprintln(ws.str());
        std::vector<Point> polyTraj;
        if (drawPolylinesFromFile(latest, polyTraj)) {
            (void)transmitTrajectoryWithSplit(sp, polyTraj, 0);
        }
        else {
            wprintln(L"[警告] 线稿加载失败，跳过作画。");
        }
    }
    return true;
}

static void menu_set_ink_station() {
    wprint(L"输入蘸墨位 X(mm) Y(mm) Z(mm)：");
    float x = 0, y = 0, z = Z_MID;
    std::cin >> x >> y >> z;
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    g_ink = InkStation{ x,y,z,true };
    wprintln(L"[信息] 已记录蘸墨位。");
}

static void menu_set_speed() {
    wprint(L"输入速度档(1~6)：");
    int v = 3; std::cin >> v; std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    if (v<SPEED_MIN || v>SPEED_MAX) { wprintln(L"[错误] 超出范围。"); return; }
    SPEED_LEVEL = v;
    std::wstringstream ws; ws << L"[信息] 当前速度=" << SPEED_LEVEL; wprintln(ws.str());
}

static void menu_set_zoffset() {
    wprint(L"输入Z_OFFSET(mm，正值整体抬高)：");
    float z; std::cin >> z; std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    Z_OFFSET = z;
    std::wstringstream ws; ws << L"[信息] Z_OFFSET=" << Z_OFFSET << L" mm"; wprintln(ws.str());
}

static void menu_set_char_spacing() {
    wprint(L"输入字间距(mm，2~50)：");
    float sp; std::cin >> sp; std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    if (sp < CHAR_SPACING_MIN) sp = CHAR_SPACING_MIN;
    if (sp > 50.f) sp = 50.f;
    CHAR_SPACING = sp;
    std::wstringstream ws; ws << L"[信息] 字间距=" << CHAR_SPACING << L" mm"; wprintln(ws.str());
}

static void menu_probe_and_save() {
    // 简化：这里直接把当前 safeArea 打印；如需真实探边可按你的硬件逻辑补充
    std::wstringstream ws;
    ws << L"[信息] SAFE 区：x:[" << g_safeArea.xmin << L"," << g_safeArea.xmax << L"] y:[" << g_safeArea.ymin << L"," << g_safeArea.ymax << L"]";
    wprintln(ws.str());
}

static void menu_draw_only(SerialPort& sp) {
    wprint(L"输入线稿JSON路径（留空使用 themes/ 下最新）：");
    std::string p; std::getline(std::cin, p);
    std::string use = p.empty() ? find_latest_autogen_json("themes") : p;
    if (use.empty()) { wprintln(L"[提示] 未找到线稿JSON。"); return; }
    std::wstringstream ws; ws << L"[信息] 使用线稿JSON：" << mb2w(use); wprintln(ws.str());
    std::vector<Point> traj;
    if (!drawPolylinesFromFile(use, traj)) return;
    transmitTrajectoryWithSplit(sp, traj, 0);
}

static void menu_generate_shanshui() {
    wprint(L"输入描述文字（例如：千山鸟飞绝，万径人踪灭）：");
    std::string gbkline; std::getline(std::cin, gbkline);
    std::wstring wtext = mb2w(gbkline);
    std::string output = make_autogen_path();

    if (generate_shanshui_json(wtext, output)) {
        std::wstringstream ws; ws << L"[成功] 已生成：" << mb2w(output); wprintln(ws.str());
        g_theme.files = { output };
    }
    else {
        wprintln(L"[错误] 生成失败。");
    }
}

static void print_menu() {
    wprintln(L"================= 毛笔书写 + 2D 描边 控制台 =================");
    wprintln(L"1. 写字并自动 2D 描边（上方写字，下方作画：自动读取 themes/ 最新 JSON）");
    wprintln(L"2. 调整速度(v=1~6)");
    wprintln(L"3. 复位到中心");
    wprintln(L"4. 退出");
    wprintln(L"5. 调整Z_OFFSET（mm）");
    wprintln(L"6. 调整字间距（mm，默认12，可压到2）");
    wprintln(L"7. 打印当前SAFE区（示例）");
    wprintln(L"8. （保留）加载calib.json（如需可自行扩展）");
    wprintln(L"9. 开关：写字完成后是否自动2D描边");
    wprintln(L"10. 2D描边模式（单独运行，加载JSON；留空使用“最近自动生成”的路径）");
    wprintln(L"11. 开关：高质模式（书法优先品质，逐点更稳）");
    wprintln(L"12. （保留）容量评估（如需可自行扩展）");
    wprintln(L"13. 根据文字手动生成山水线稿JSON（调用 shanshui_gen.hpp）");
    wprintln(L"14. 设置蘸墨位（记录 X/Y/Z）");
    wprintln(L"15. 开关：蘸墨功能（默认关，用于排除蘸墨影响）");
    std::wstringstream ws;
    ws << L"[状态] DRYRUN=" << (g_dryRun ? L"ON" : L"OFF")
        << L" 速度=" << SPEED_LEVEL
        << L" 上区比例=" << TEXT_TOP_RATIO
        << L" 字间距=" << CHAR_SPACING
        << L" 自动描边=" << (g_autoDraw ? L"ON" : L"OFF")
        << L" 高质=" << (g_highQuality ? L"ON" : L"OFF")
        << L" 蘸墨=" << (g_enableDip ? L"ON" : L"OFF")
        << L" SAFE:[" << g_safeArea.xmin << L"," << g_safeArea.xmax << L"; " << g_safeArea.ymin << L"," << g_safeArea.ymax << L"]";
    wprintln(ws.str());
    wprint(L"选择：");
}

// --------------------------- 急停线程 ---------------------------
// ★修复：1) 线程常驻循环（此前触发一次即 return，导致第二次急停失效）
//        2) 用 PeekConsoleInput 探测，不再 _getch 窃取普通按键（避免菜单/文字输入丢字）
//        3) 仅 ESC 触发急停（移除空格：输入诗句时空格常见，易误触）
//        4) 触发后清空输入缓冲，防止同一 ESC 事件反复置位
static void estop_poll() {
    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
    while (true) {
        DWORD n = 0;
        if (GetNumberOfConsoleInputEvents(hIn, &n) && n > 0) {
            std::vector<INPUT_RECORD> recs(n);
            DWORD rd = 0;
            if (PeekConsoleInputW(hIn, recs.data(), n, &rd) && rd > 0) {
                for (DWORD i = 0; i < rd; ++i) {
                    if (recs[i].EventType == KEY_EVENT &&
                        recs[i].Event.KeyEvent.bKeyDown &&
                        recs[i].Event.KeyEvent.wVirtualKeyCode == VK_ESCAPE) {
                        g_estop = true;
                        FlushConsoleInputBuffer(hIn);
                        break;
                    }
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

// --------------------------- main ---------------------------
int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_GBK);
    SetConsoleCP(CP_GBK);
    setlocale(LC_ALL, ".936");

    for (int i = 1; i < argc; i++) if (std::string(argv[i]) == "--dryrun") g_dryRun = true;

    init_default_themes();

    SerialPort sp;
    if (!g_dryRun) {
        wprint(L"输入串口号（如 COM3）：");
        std::string com; std::getline(std::cin, com);
        std::wstring wcom = mb2w(com);
        std::wstring wpath = normalizeComName(wcom);
        if (!sp.open(wpath, BAUDRATE, EVENPARITY, 8, ONESTOPBIT)) {
            wprintln(L"[错误] 串口初始化失败，自动切换 DRYRUN。");
            g_dryRun = true;
        }
    }
    else {
        wprintln(L"[信息] DRYRUN 模式：不会打开串口/发送报文，仅打印帧与日志。");
    }

    std::thread tEstop(estop_poll); tEstop.detach();

    while (true) {
        print_menu();
        int sel = 0;
        if (!(std::cin >> sel)) { std::cin.clear(); std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n'); wprintln(L"[提示] 无效输入。"); continue; }
        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');

        if (sel == 1) {
            if (!menu_write_and_draw_capture(sp)) wprintln(L"[提示] 写字/作画未完成。");
        }
        else if (sel == 2) {
            menu_set_speed();
        }
        else if (sel == 3) {
            gotoCenter(sp);
            wprintln(L"[信息] 已复位到中心。");
        }
        else if (sel == 4) {
            wprintln(L"再见。"); break;
        }
        else if (sel == 5) {
            menu_set_zoffset();
        }
        else if (sel == 6) {
            menu_set_char_spacing();
        }
        else if (sel == 7) {
            menu_probe_and_save();
        }
        else if (sel == 8) {
            wprintln(L"[提示] 你可以在此处实现 calib.json 读取逻辑。");
        }
        else if (sel == 9) {
            g_autoDraw = !g_autoDraw;
            std::wstringstream ws; ws << L"[信息] 自动2D描边已" << (g_autoDraw ? L"开启" : L"关闭"); wprintln(ws.str());
        }
        else if (sel == 10) {
            menu_draw_only(sp);
        }
        else if (sel == 11) {
            g_highQuality = !g_highQuality;
            std::wstringstream ws; ws << L"[信息] 高质模式已" << (g_highQuality ? L"开启（更慢更稳，逐点发送）" : L"关闭（常规速度）"); wprintln(ws.str());
        }
        else if (sel == 12) {
            wprintln(L"[提示] 你可以在此处实现容量评估逻辑。");
        }
        else if (sel == 13) {
            menu_generate_shanshui();
        }
        else if (sel == 14) {
            menu_set_ink_station();
        }
        else if (sel == 15) {
            g_enableDip = !g_enableDip;
            std::wstringstream ws;
            ws << L"[信息] 蘸墨功能已" << (g_enableDip ? L"开启" : L"关闭（排除干扰模式）");
            wprintln(ws.str());
        }
        else {
            wprintln(L"[提示] 无此选项。");
        }

        if (g_estop) {
            wprintln(L"[信息] 已处理急停，标志复位。");
            g_estop = false;
        }
    }
    return 0;
}
