// Robot — 主程序入口（拆分版）
// 原单文件 Robot.cpp 已拆分为：robot_common / serial_port / motion / hanzi / polyline / main 六个编译单元，行为不变。
// 编译（MSVC）：见 Robot.vcxproj（cl /std:c++17 /utf-8）
// 编译（MinGW，静态链接，无外部DLL依赖，可独立拷贝运行）：g++ -std=c++17 -g -Wall -Inlohmann -static -static-libgcc -static-libstdc++ robot_common.cpp serial_port.cpp motion.cpp hanzi.cpp polyline.cpp main.cpp -o Robot.exe
// 特性：GBK 控制台、DRYRUN、Modbus 逐点/批量7点、探边、多Z层、安全区、书法(上区) + 线稿(下区)
// 变更：1) 首划稳态：serial_pre_warm + 首落笔点重复发送（参数上调）
//      2) 蘸墨：两次蘸→十字抖→离液面上下抖5次（上慢下快）
//      3) ★新增位姿跟踪：全局记录“最后已知位置”，为首字“到位→等待→落笔”做准备
//      4) ★修复：批量7点成功后位姿更新用 pts7.back()
//      5) ★修复：急停线程常驻、仅ESC触发且不窃取按键；Z越界校验含Z_OFFSET；蘸墨逐点速度生效
//      6) ★工程化拆分：单文件 → 多模块（行为不变）
//      7) ★新增调试菜单（菜单16）：单点发送/L形方向/Z步进标定/抬落笔循环/方框批量/急停演练/位姿查询
//      8) ★新增菜单17设置中心点（菜单3复位目标）、菜单18日志开关；每次运行自动记录 logs/Robot_时间.log（含全部输出与DRYRUN帧，带毫秒时间戳）
//      9) ★中心点扩展为 X/Y/Z 三轴；新增 robot_config.json 配置持久化——每次菜单操作后自动保存，启动时自动加载
//      10) ★Z 层按真机实测校准（可动范围 -320~-385，-310 以上不动）：Z 层/设备行程改为可配置全局并持久化，新增菜单19 Z 层校准；真机模式 TX 帧入日志

#include "robot_common.h"
#include "serial_port.h"
#include "motion.h"
#include "hanzi.h"
#include "polyline.h"
#include "gui_service.h"
#include "shanshui_gen.hpp"
#include "nlohmann/json.hpp"

#include <fstream>

using json = nlohmann::json;

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
// ★GUI 阶段：make_autogen_path/find_latest_autogen_json/save_config/load_config
//   已下沉到 gui_service（gs::make_autogen_path / gs::latest_autogen_json /
//   gs::cfg_save / gs::cfg_load），控制台与 GUI 共用同一实现与 robot_config.json。
static std::string make_autogen_path() {
    return gs::make_autogen_path();
}

static std::string find_latest_autogen_json(const std::string& dir) {
    return gs::latest_autogen_json(dir);
}

static void gotoCenter(SerialPort& sp) {
    Point p{ g_center_x, g_center_y, g_center_z, false, (uint8_t)SPEED_LEVEL, "UP-CENTER" };
    sp.sendPointRetry(p);
}

// --------------------------- 配置持久化（robot_config.json） ---------------------------
// 每次菜单操作后自动保存；启动时自动加载。保存失败的警告只提示不中断。
// ★GUI 阶段：实现已下沉到 gs::cfg_save/gs::cfg_load（含相同提示语），此处保留壳函数。
static void save_config() { gs::cfg_save(); }

static void load_config() { gs::cfg_load(); }

static void menu_set_center() {
    wprintln(L"输入中心点 X Y Z（mm；Z=复位高度，建议抬笔 -320）/ Set center X Y Z：");
    float x = 0, y = 0, z = Z_UP;
    if (!(std::cin >> x >> y >> z)) { std::cin.clear(); std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n'); wprintln(L"[提示] 输入无效。"); return; }
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    if (!inXYRange(x, y, g_devLimit)) { wprintln(L"[错误] XY 超出设备极限，未设置。/ XY out of device limits."); return; }
    if (!inZRange(z)) { wprintln(L"[错误] Z 超出范围(-410~-250)，未设置。/ Z out of range."); return; }
    g_center_x = x; g_center_y = y; g_center_z = z;
    std::wstringstream ws;
    ws << L"[信息] 中心点已设置：(" << g_center_x << L", " << g_center_y << L", " << g_center_z << L")，菜单3复位到该点。";
    wprintln(ws.str());
}

static void menu_log_toggle() {
    g_logEnable = !g_logEnable;
    std::wstringstream ws;
    ws << L"[信息] 日志记录已" << (g_logEnable ? L"开启" : L"关闭")
        << L"；文件：" << mb2w(g_logPath);
    wprintln(ws.str());
}

// ★菜单19：Z 层校准——按设备实测，输入两个锚点（抬笔Z/书写Z），自动推算其余层
static void menu_calibrate_z_layers() {
    wprintln(L"输入 抬笔Z 书写Z（mm；实测可动范围约 -320~-385，两值至少差 20）/ Set liftZ writeZ：");
    float up = 0, wr = 0;
    if (!(std::cin >> up >> wr)) { std::cin.clear(); std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n'); wprintln(L"[提示] 输入无效。"); return; }
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    if (!inZRange(up) || !inZRange(wr)) { wprintln(L"[错误] 超出设备 Z 可动范围（菜单里可看 g_z_top/g_z_bottom），未设置。"); return; }
    if (wr >= up - 20.0f) { wprintln(L"[错误] 书写Z 至少比抬笔Z 深 20mm，未设置。"); return; }
    Z_UP = up;
    Z_DOWN_NORMAL = wr;
    Z_MID = up - 30.0f;             if (!inZRange(Z_MID))         Z_MID = (up + wr) * 0.5f;
    Z_PRE_DOWN = wr + 22.0f;        if (!inZRange(Z_PRE_DOWN))    Z_PRE_DOWN = (Z_MID + wr) * 0.5f;
    Z_DOWN_LIGHT = wr + 3.0f;
    Z_DOWN_HEAVY = wr - 3.0f;       if (!inZRange(Z_DOWN_HEAVY))  Z_DOWN_HEAVY = wr;
    std::wstringstream ws;
    ws << L"[信息] Z 层已校准：抬笔=" << Z_UP << L" 中位=" << Z_MID << L" 预压=" << Z_PRE_DOWN
        << L" 轻触=" << Z_DOWN_LIGHT << L" 书写=" << Z_DOWN_NORMAL << L" 重压=" << Z_DOWN_HEAVY;
    wprintln(ws.str());
}

// ★菜单20：自定义设备 Z 行程范围（上限/下限）——inZRange 校验与菜单17/19 都按此范围
// 注意：上限改大只是解除软件拦截，设备对死区点仍会"ACK 但不动"，以实际反应为准。
static void menu_set_z_limits() {
    std::wstringstream hint;
    hint << L"输入 设备Z上限 设备Z下限（mm；当前 " << g_z_top << L" " << g_z_bottom
        << L"；上限=能动的最高点，可自行试探）/ Set Z top bottom：";
    wprintln(hint.str());
    float top = 0, bot = 0;
    if (!(std::cin >> top >> bot)) { std::cin.clear(); std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n'); wprintln(L"[提示] 输入无效。"); return; }
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    if (bot > top) { wprintln(L"[错误] 下限必须 ≤ 上限，未设置。"); return; }
    if (top > -200.0f || bot > -200.0f || top < -450.0f || bot < -450.0f) {
        wprintln(L"[错误] 超出合理范围(-450~-200)，未设置。"); return;
    }
    g_z_top = top; g_z_bottom = bot;
    std::wstringstream ws;
    ws << L"[信息] 设备 Z 行程已更新：上限=" << g_z_top << L" 下限=" << g_z_bottom;
    wprintln(ws.str());
    if (!inZRange(Z_UP))            wprintln(L"[警告] 当前抬笔层超出新范围，请用菜单19重新校准！");
    if (!inZRange(Z_DOWN_NORMAL))   wprintln(L"[警告] 当前书写层超出新范围，请用菜单19重新校准！");
    if (!inZRange(g_center_z))      wprintln(L"[警告] 当前中心点 Z 超出新范围，请用菜单17重新设置！");
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
            if (!transmitTrajectoryWithSplit(sp, polyTraj, 0)) {
                wprintln(L"[错误] 线稿发送失败，作画未完成。");
                return false;
            }
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
    if (!(std::cin >> x >> y >> z)) {
        std::cin.clear();
        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        wprintln(L"[提示] 输入无效，保持原蘸墨位。");
        return;
    }
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)
        || !inXYRange(x, y, g_devLimit) || !inZRange(z + Z_OFFSET)) {
        wprintln(L"[错误] 蘸墨位坐标无效或越界，未设置。");
        return;
    }
    g_ink = InkStation{ x,y,z,true };
    wprintln(L"[信息] 已记录蘸墨位。");
}

static void menu_set_speed() {
    wprint(L"输入速度档(1~6)：");
    int v = 0;
    if (!(std::cin >> v)) {
        std::cin.clear();
        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        wprintln(L"[提示] 输入无效，保持原速度。");
        return;
    }
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    if (v<SPEED_MIN || v>SPEED_MAX) { wprintln(L"[错误] 超出范围。"); return; }
    SPEED_LEVEL = v;
    std::wstringstream ws; ws << L"[信息] 当前速度=" << SPEED_LEVEL; wprintln(ws.str());
}

static void menu_set_zoffset() {
    wprint(L"输入Z_OFFSET(mm，正值整体抬高)：");
    float z = 0;
    if (!(std::cin >> z)) {
        std::cin.clear();
        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        wprintln(L"[提示] 输入无效，保持原 Z_OFFSET。");
        return;
    }
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    if (!std::isfinite(z) || z < -100.0f || z > 100.0f) {
        wprintln(L"[错误] Z_OFFSET 必须是有限数值且范围为 -100~100，未设置。");
        return;
    }
    Z_OFFSET = z;
    std::wstringstream ws; ws << L"[信息] Z_OFFSET=" << Z_OFFSET << L" mm"; wprintln(ws.str());
}

static void menu_set_char_spacing() {
    wprint(L"输入字间距(mm，2~50)：");
    float sp = 0;
    if (!(std::cin >> sp)) {
        std::cin.clear();
        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        wprintln(L"[提示] 输入无效，保持原字间距。");
        return;
    }
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    if (!std::isfinite(sp)) {
        wprintln(L"[提示] 输入必须是有限数值，保持原字间距。");
        return;
    }
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

// ★输入失败的统一处理（2026-09-24 修复“非控制台 stdin 下主循环空转刷日志”）
//   背景：stdin 被重定向/关闭时 std::cin 立即返回 EOF。此前失败分支一律
//   clear + ignore + continue，而 ignore 在 EOF 上什么都吃不到，于是 while(true)
//   无任何延时地空转：实测 12 秒重打印近万遍菜单、写出 15MB 日志
//   （2026-09-18 另两次各 10~13MB）。
//   返回 true = 调用方应立即退出循环；false = 已清错，可继续下一次读取。
static int s_badInputStreak = 0;
static const int BAD_INPUT_LIMIT = 1000;
static bool input_fail_should_exit(const wchar_t* where) {
    if (std::cin.eof()) {
        std::wstringstream ws;
        ws << L"[提示] 输入流已结束（stdin 被关闭/重定向，位置：" << where
           << L"）：为避免空转刷日志，程序退出。";
        wprintln(ws.str());
        return true;
    }
    std::cin.clear();
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    if (++s_badInputStreak > BAD_INPUT_LIMIT) {
        std::wstringstream ws;
        ws << L"[提示] 连续 " << BAD_INPUT_LIMIT << L" 次无效输入（位置：" << where
           << L"），疑似输入源异常，程序退出。";
        wprintln(ws.str());
        return true;
    }
    wprintln(L"[提示] 无效输入。");
    return false;
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
    wprintln(L"16. 调试菜单（上机分步验证：单点/方向/标定/批量/急停）");
    wprintln(L"17. 设置中心点（X/Y/Z，菜单3复位目标，默认 0,0,-325）");
    wprintln(L"18. 开关：日志记录（logs/Robot_时间.log，含全部输出与时间戳）");
    wprintln(L"19. Z 层校准（输入抬笔Z 书写Z，自动推算中位/预压/轻触/重压）");
    wprintln(L"20. 设置设备 Z 行程范围（上限/下限，自定义可动边界）");
    wprintln(L"21. 开关：顿笔（起笔/收笔按压停顿 + 点画深压；关闭后仅写每字 medians 骨架）");
    std::wstringstream ws;
    ws << L"[状态] DRYRUN=" << (g_dryRun ? L"ON" : L"OFF")
        << L" 速度=" << SPEED_LEVEL
        << L" 上区比例=" << TEXT_TOP_RATIO
        << L" 字间距=" << CHAR_SPACING
        << L" 自动描边=" << (g_autoDraw ? L"ON" : L"OFF")
        << L" 高质=" << (g_highQuality ? L"ON" : L"OFF")
        << L" 蘸墨=" << (g_enableDip ? L"ON" : L"OFF")
        << L" 顿笔=" << (g_enableDunbi ? L"ON" : L"OFF")
        << L" 中心点=(" << g_center_x << L"," << g_center_y << L"," << g_center_z << L")"
        << L" 抬笔=" << Z_UP << L" 书写=" << Z_DOWN_NORMAL
        << L" 日志=" << (g_logEnable ? L"ON" : L"OFF")
        << L" SAFE:[" << g_safeArea.xmin << L"," << g_safeArea.xmax << L"; " << g_safeArea.ymin << L"," << g_safeArea.ymax << L"]";
    wprintln(ws.str());
    wprintln(L"选择(0-21) / Select：");
}

// --------------------------- 急停线程 ---------------------------
// ★修复：1) 线程常驻循环（此前触发一次即 return，导致第二次急停失效）
//        2) 用 PeekConsoleInput 探测，不再 _getch 窃取普通按键（避免菜单/文字输入丢字）
//        3) 仅 ESC 触发急停（移除空格：输入诗句时空格常见，易误触）
//        4) 触发后清空输入缓冲，防止同一 ESC 事件反复置位
static void estop_poll() {
    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
    while (!g_estop_stop.load()) {
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

// --------------------------- 调试菜单（上机分步验证辅助） ---------------------------
// 所有动作都走 SerialPort 正常发送路径：DRYRUN 下仅打印帧（可查协议），
// 真机下实际运动。每项对应一个真机验证点：通信/方向镜像/Z标定/Z动作/批量协议/急停。

static void dbg_query_pose() {
    if (!g_pose_init) { wprintln(L"[位姿] 本次运行尚未成功发送过点，无已知位姿。"); return; }
    std::wstringstream ws;
    ws << L"[位姿] X=" << g_last_pose.x << L" Y=" << g_last_pose.y << L" Z=" << g_last_pose.z
        << (g_last_pose.isPenDown ? L" [落笔]" : L" [抬笔]")
        << L" 来源=" << mb2w(g_last_pose.zType);
    wprintln(ws.str());
}

static void dbg_single_point(SerialPort& sp) {
    wprint(L"输入 X Y Z 落笔(1/0)，如 0 -40 -385 1：");
    float x, y, z; int pen = 0;
    if (!(std::cin >> x >> y >> z >> pen)) { std::cin.clear(); std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n'); wprintln(L"[提示] 输入无效。"); return; }
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    Point p{ x, y, z, pen != 0, (uint8_t)SPEED_LEVEL, "DBG-POINT" };
    bool ok = sp.sendPointRetry(p);
    wprintln(ok ? L"[成功] 单点已发送并收到有效应答。" : L"[失败] 发送被拒（坐标越界）或无应答，见上方日志。");
    dbg_query_pose();
}

static void dbg_direction_test(SerialPort& sp) {
    wprint(L"输入中心 X Y 边长，如 0 -40 20：");
    float cx, cy, s;
    if (!(std::cin >> cx >> cy >> s)) { std::cin.clear(); std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n'); wprintln(L"[提示] 输入无效。"); return; }
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    if (!std::isfinite(cx) || !std::isfinite(cy) || !std::isfinite(s) || s <= 0.0f
        || !inXYRange(cx - s * 0.5f, cy - s * 0.5f, g_devLimit)
        || !inXYRange(cx + s * 0.5f, cy + s * 0.5f, g_devLimit)) {
        wprintln(L"[错误] L 形参数无效或超出设备 XY 范围。");
        return;
    }
    wprintln(L"[提示] 将落笔画 L 形（先竖后横，逐点慢速）。预期：竖笔在左、横笔在底；若呈镜像（Γ/┐）则轴方向与代码假设相反。");
    float x0 = cx - s * 0.5f, y0 = cy + s * 0.5f;
    std::vector<Point> traj;
    traj.push_back(Point{ x0, y0, Z_UP, false, (uint8_t)1, "DIR-UP" });
    traj.push_back(Point{ x0, y0, Z_MID, false, (uint8_t)1, "DIR-MID" });
    traj.push_back(Point{ x0, y0, Z_PRE_DOWN, false, (uint8_t)1, "DIR-PRE" });
    traj.push_back(Point{ x0, y0, Z_DOWN_NORMAL, true, (uint8_t)1, "DIR-shu" });
    traj.push_back(Point{ x0, y0 - s, Z_DOWN_NORMAL, true, (uint8_t)1, "DIR-shu" });
    traj.push_back(Point{ x0, y0 - s, Z_PRE_DOWN, false, (uint8_t)1, "DIR-PRE" });
    traj.push_back(Point{ x0, y0 - s, Z_MID, false, (uint8_t)1, "DIR-MID" });
    traj.push_back(Point{ x0, y0 - s, Z_DOWN_NORMAL, true, (uint8_t)1, "DIR-heng" });
    traj.push_back(Point{ x0 + s, y0 - s, Z_DOWN_NORMAL, true, (uint8_t)1, "DIR-heng" });
    traj.push_back(Point{ x0 + s, y0 - s, Z_MID, false, (uint8_t)1, "DIR-MID" });
    traj.push_back(Point{ x0 + s, y0 - s, Z_UP, false, (uint8_t)1, "DIR-UP" });
    transmitTrajectoryWithSplit(sp, traj, traj.size());   // 全部按书法逐点慢发
    wprintln(L"[完成] L 形已发送。对照纸上形状判断方向/镜像。");
}

static void dbg_z_calibrate(SerialPort& sp) {
    wprint(L"输入标定点 X Y，如 0 -40：");
    float x, y;
    if (!(std::cin >> x >> y)) { std::cin.clear(); std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n'); wprintln(L"[提示] 输入无效。"); return; }
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    wprintln(L"[提示] 标定前建议先用主菜单5把 Z_OFFSET 设为 0。从 Z=-363 起每次下压 2mm（每次先抬到 -350 再压，防刮纸）；笔尖刚触纸时输入 q。请手扶急停。");
    Point up{ x, y, Z_UP, false, (uint8_t)1, "CAL-UP" };
    if (!sp.sendPointRetry(up)) { wprintln(L"[失败] 无法到达标定点。"); return; }
    float z = Z_PRE_DOWN; bool touched = false;
    while (z >= -408.0f) {
        Point down{ x, y, z, true, (uint8_t)1, "CAL-DOWN" };
        if (!sp.sendPointRetry(down)) { wprintln(L"[提示] 下压点越界被拒，标定结束。"); break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(800));
        std::wstringstream ws; ws << L"已压到 Z=" << z << L"，触纸输 q，回车下一档："; wprint(ws.str());
        std::string ans;
        if (!std::getline(std::cin, ans)) break;
        if (ans == "q" || ans == "Q") { touched = true; break; }
        Point rise{ x, y, Z_MID, false, (uint8_t)1, "CAL-RISE" };
        sp.sendPointRetry(rise);
        z -= 2.0f;
    }
    Point up2{ x, y, Z_UP, false, (uint8_t)1, "CAL-END" };
    sp.sendPointRetry(up2);
    if (touched) {
        std::wstringstream ws;
        ws << L"[建议] 触纸 Z=" << z << L" → 轻触纸(Z=" << Z_DOWN_LIGHT << L")建议 Z_OFFSET=" << (z + Z_DOWN_LIGHT)
            << L"；常规书写(Z=" << Z_DOWN_NORMAL << L")建议 Z_OFFSET=" << (z + Z_DOWN_NORMAL) << L"。用主菜单5设置后，可用调试1复验。";
        wprintln(ws.str());
    }
    else {
        wprintln(L"[提示] 未记录触纸点（到达下限或中途退出）。");
    }
}

static void dbg_pen_cycle(SerialPort& sp) {
    wprint(L"输入 X Y 次数，如 0 -40 5：");
    float x, y; int n = 5;
    if (!(std::cin >> x >> y >> n)) { std::cin.clear(); std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n'); wprintln(L"[提示] 输入无效。"); return; }
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    if (!std::isfinite(x) || !std::isfinite(y) || !inXYRange(x, y, g_devLimit)
        || n < 1 || n > 50) { wprintln(L"[提示] 坐标无效/越界，或次数不在 1~50。"); return; }
    for (int i = 1; i <= n && !g_estop; ++i) {
        Point d{ x, y, Z_DOWN_NORMAL, true, (uint8_t)1, "CYC-DOWN" };
        Point u{ x, y, Z_UP, false, (uint8_t)1, "CYC-UP" };
        if (!sp.sendPointRetry(d)) { wprintln(L"[失败] 落笔点被拒，终止。"); return; }
        std::this_thread::sleep_for(std::chrono::milliseconds(600));
        if (!sp.sendPointRetry(u)) { wprintln(L"[失败] 抬笔点被拒，终止。"); return; }
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        std::wstringstream ws; ws << L"[循环] 第 " << i << L"/" << n << L" 次抬落完成。"; wprintln(ws.str());
    }
}

static void dbg_square_batch(SerialPort& sp) {
    wprint(L"输入中心 X Y 边长，如 0 -40 10：");
    float cx, cy, s;
    if (!(std::cin >> cx >> cy >> s)) { std::cin.clear(); std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n'); wprintln(L"[提示] 输入无效。"); return; }
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    if (!std::isfinite(cx) || !std::isfinite(cy) || !std::isfinite(s) || s <= 0.0f
        || !inXYRange(cx - s * 0.5f, cy - s * 0.5f, g_devLimit)
        || !inXYRange(cx + s * 0.5f, cy + s * 0.5f, g_devLimit)) {
        wprintln(L"[错误] 方框参数无效或超出设备 XY 范围。");
        return;
    }
    std::vector<Point> traj;
    float h = s * 0.5f;
    traj.push_back(Point{ cx - h, cy + h, Z_UP, false, (uint8_t)1, "SQ-UP" });
    traj.push_back(Point{ cx - h, cy + h, Z_DOWN_LIGHT, true, (uint8_t)1, "SQ" });
    auto edge = [&](float x1, float y1, float x2, float y2) {
        float dx = x2 - x1, dy = y2 - y1;
        int n = std::max(1, (int)std::ceil(std::sqrt(dx * dx + dy * dy) / 2.0f));
        for (int k = 1; k <= n; ++k)
            traj.push_back(Point{ x1 + dx * k / n, y1 + dy * k / n, Z_DOWN_LIGHT, true, (uint8_t)1, "SQ" });
        };
    edge(cx - h, cy + h, cx + h, cy + h);
    edge(cx + h, cy + h, cx + h, cy - h);
    edge(cx + h, cy - h, cx - h, cy - h);
    edge(cx - h, cy - h, cx - h, cy + h);
    wprintln(L"[提示] 方框走描边模式（批量7点协议，每2mm一点）。DRYRUN 可查 [TX-B7] 帧；真机观察是否闭合、有无批量警告。");
    transmitTrajectoryWithSplit(sp, traj, 0);
    wprintln(L"[完成] 方框发送结束。");
}

static void dbg_estop_drill(SerialPort& sp) {
    wprint(L"输入往返 Y（如 -40），回车开始：");
    float y;
    if (!(std::cin >> y)) { std::cin.clear(); std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n'); wprintln(L"[提示] 输入无效。"); return; }
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    wprintln(L"[演练] 抬笔在 X=-20↔20 缓慢往返（速度2，最多约20秒），请随机按 ESC 测急停。");
    bool hit = false;
    for (int i = 0; i < 40 && !hit; ++i) {
        float x = (i % 2 == 0) ? -20.0f : 20.0f;
        Point p{ x, y, Z_MID, false, (uint8_t)2, "DRILL" };
        if (!sp.sendPointRetry(p)) { wprintln(L"[失败] 发送异常，终止演练。"); hit = true; break; }
        for (int k = 0; k < 10 && !g_estop; ++k)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (g_estop) hit = true;
    }
    Point up{ 0.f, y, Z_UP, false, (uint8_t)1, "DRILL-UP" };
    sp.sendPointRetry(up);
    if (g_estop) {
        wprintln(L"[急停] 已触发，机械臂已抬笔复位。");
        g_estop = false;
    }
    else {
        wprintln(L"[演练] 往返完成未触发急停，可再次运行并按 ESC。");
    }
}

// ★调试8：循环单点（jog 模式）——逐行输入 X Y Z 落笔(1/0) 立即执行，q 退出。
// 等价于连续使用调试1；越界点跳过不重试；ESC 急停后自动抬笔退出。
static void dbg_jog_loop(SerialPort& sp) {
    wprintln(L"[循环单点] 每行输入：X Y Z 落笔(1/0)  （落笔可省略，默认抬笔）");
    wprintln(L"[循环单点] 每行立即执行；输入 q 回车退出；ESC 急停中止。");
    while (!g_estop) {
        wprintln(L"JOG> (输入 X Y Z 落笔(1/0)，q=退出)");
        std::string line;
        if (!std::getline(std::cin, line)) break;
        // 去首尾空白
        size_t b = line.find_first_not_of(" \t\r\n");
        if (b == std::string::npos) { wprintln(L"[提示] 空行。格式：X Y Z 落笔(1/0)，或 q 退出。"); continue; }
        size_t e = line.find_last_not_of(" \t\r\n");
        line = line.substr(b, e - b + 1);
        if (line == "q" || line == "Q") break;
        std::istringstream iss(line);
        float x = 0, y = 0, z = 0; int pen = 0;
        if (!(iss >> x >> y >> z)) { wprintln(L"[提示] 格式：X Y Z 落笔(1/0)，或 q 退出。"); continue; }
        iss >> pen;   // 可选第4个参数
        if (!inZRange(z + Z_OFFSET) || !inXYRange(x, y, g_devLimit)) {
            std::wstringstream ws;
            ws << L"[错误] 越界，已跳过（有效Z范围 " << g_z_bottom << L"~" << g_z_top
                << L"，当前含偏移后 Z=" << (z + Z_OFFSET) << L"）";
            wprintln(ws.str());
            continue;
        }
        Point p{ x, y, z, pen != 0, (uint8_t)SPEED_LEVEL, "JOG" };
        sp.sendPointRetry(p);
        dbg_query_pose();
    }
    if (g_estop) {
        float sx = g_pose_init ? g_last_pose.x : g_center_x;
        float sy = g_pose_init ? g_last_pose.y : g_center_y;
        Point up{ sx, sy, Z_UP, false, (uint8_t)1, "JOG-ESTOP-UP" };
        sp.sendPointRetry(up);
        wprintln(L"[急停] 循环单点已中止并抬笔。");
    }
    wprintln(L"[循环单点] 已退出。");
}

static void menu_debug(SerialPort& sp) {
    while (true) {
        wprintln(L"================= 调试菜单（上机分步验证） =================");
        wprintln(L"1. 单点发送（验证串口通信/坐标到达）");
        wprintln(L"2. L形方向测试（验证 X/Y 方向与镜像）");
        wprintln(L"3. Z 轴步进标定（找触纸Z，给出 Z_OFFSET 建议）");
        wprintln(L"4. 抬落笔循环（验证 Z 轴动作与应答稳定性）");
        wprintln(L"5. 方框批量测试（验证批量7点协议与闭合度）");
        wprintln(L"6. 急停演练（运行中按 ESC）");
        wprintln(L"7. 查询最后已知位姿");
        wprintln(L"8. 循环单点（逐行输入 X Y Z 落笔(1/0) 立即执行，q 退出）");
        wprintln(L"0. 返回主菜单");
        wprintln(L"调试选择(0-8) / Select：");
        int sel = 0;
        // ★输入失败：EOF / 连续无效过多 -> 退出循环（此前 continue 会空转刷日志）
        if (!(std::cin >> sel)) { if (input_fail_should_exit(L"调试菜单")) break; continue; }
        s_badInputStreak = 0;
        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        if (sel == 0) break;
        else if (sel == 1) dbg_single_point(sp);
        else if (sel == 2) dbg_direction_test(sp);
        else if (sel == 3) dbg_z_calibrate(sp);
        else if (sel == 4) dbg_pen_cycle(sp);
        else if (sel == 5) dbg_square_batch(sp);
        else if (sel == 6) dbg_estop_drill(sp);
        else if (sel == 7) dbg_query_pose();
        else if (sel == 8) dbg_jog_loop(sp);
        else wprintln(L"[提示] 无此选项。");
        if (g_estop) { wprintln(L"[信息] 已处理急停，标志复位。"); g_estop = false; }
    }
}

// ★黑窗问题修复记录：经实测，部分控制台主机（Win11 双击启动）不渲染“不带换行符”的控制台写入，
//   导致“输入串口号：”“选择：”等提示不可见，程序看似黑窗卡死（实际在等输入）。
//   因此：1) wprint/wprintln 全部改为 printf 直写 stdout 并立即 fflush；
//        2) wprint 统一按整行输出（补换行）；3) main 中 stdout 设为无缓冲。
// --------------------------- main ---------------------------
int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);   // 输出即时上屏：防止提示滞留缓冲导致"黑窗等输入"
    SetConsoleOutputCP(CP_GBK);
    SetConsoleCP(CP_GBK);
    setlocale(LC_ALL, ".936");
    log_init();   // ★本次运行日志（在首条输出前初始化，横幅即入日志）
    wprintln(L"[启动] Robot 控制台已启动。");

    for (int i = 1; i < argc; i++) if (std::string(argv[i]) == "--dryrun") g_dryRun = true;

    init_default_themes();
    load_config();   // ★自动加载上次保存的配置（中心点/Z_OFFSET/速度/字间距/蘸墨位/开关）

    SerialPort sp;
    if (!g_dryRun) {
        wprintln(L"输入串口号（如 COM99）/ Enter COM port (e.g. COM99)：");
        std::string com; std::getline(std::cin, com);
        std::wstring wcom = mb2w(com);
        std::wstring wpath = normalizeComName(wcom);
        {
            std::wstringstream ws;
            ws << L"[信息] 正在打开 " << wpath << L" ... / Opening port...（若长时间停在此行：端口不存在/被占用/驱动异常）";
            wprintln(ws.str());
        }
        if (!sp.open(wpath, BAUDRATE, EVENPARITY, 8, ONESTOPBIT)) {
            wprintln(L"[错误] 串口打开失败，自动切换 DRYRUN。/ Open failed -> switched to DRYRUN.");
            g_dryRun = true;
        }
    }
    else {
        wprintln(L"[信息] DRYRUN 模式：不会打开串口/发送报文，仅打印帧与日志。");
    }

    g_estop_stop = false;
    std::thread tEstop(estop_poll);
    gs::set_source("CONSOLE", "HUMAN");   // ★GUI 阶段：控制台来源标记（审计日志用）

    while (true) {
        print_menu();
        int sel = 0;
        // ★输入失败：EOF（stdin 关闭/重定向）或连续无效过多 -> 退出，避免空转刷日志
        if (!(std::cin >> sel)) { if (input_fail_should_exit(L"主菜单")) break; continue; }
        s_badInputStreak = 0;
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
        else if (sel == 16) {
            menu_debug(sp);
        }
        else if (sel == 17) {
            menu_set_center();
        }
        else if (sel == 18) {
            menu_log_toggle();
        }
        else if (sel == 19) {
            menu_calibrate_z_layers();
        }
        else if (sel == 20) {
            menu_set_z_limits();
        }
        else if (sel == 21) {
            g_enableDunbi = !g_enableDunbi;
            std::wstringstream ws;
            ws << L"[信息] 顿笔已" << (g_enableDunbi ? L"开启（起收笔按压·点画深压）" : L"关闭（仅写每字 medians 骨架）");
            wprintln(ws.str());
        }
        else {
            wprintln(L"[提示] 无此选项。");
        }

        save_config();   // ★每次菜单操作后自动保存配置（robot_config.json）

        if (g_estop) {
            wprintln(L"[信息] 已处理急停，标志复位。");
            g_estop = false;
        }
    }
    g_estop_stop = true;
    if (tEstop.joinable()) tEstop.join();
    gs::shutdown();   // ★GUI 阶段：关闭串口/审计文件
    return 0;
}
