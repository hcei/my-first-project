// robot_common.cpp — 全局状态定义与基础工具实现
// 由 Robot.cpp（单文件版）拆分而来；初值与实现逐行保真。
#include "robot_common.h"

#include <cwctype>
#include <iostream>
#include <sstream>

// --------------------------- 全局状态定义（初值照抄原文件） ---------------------------
float ACTIVE_CHAR_SIZE = SINGLE_CHAR_MIN;
float CHAR_SPACING = 12.0f;
float TEXT_TOP_RATIO = 0.46f;
float Z_OFFSET = 0.0f;  // 运行时整体偏移
int   SPEED_LEVEL = 3;

std::string HANZI_BASE_DIR = "D:/objects/hanzi-writer-data"; // HanziWriter 数据根
std::string THEME_NAME = "jiangxue";                          // 主题

bool  g_dryRun = false;        // DRYRUN：不打开串口，不发报文
volatile bool g_estop = false; // 急停

InkStation g_ink;

bool  g_pose_init = false;     // ★全局位姿跟踪
Point g_last_pose = { 0.f,0.f,Z_UP,false,(uint8_t)SPEED_LEVEL,"POSE" };

// 注意：原文件中即采用外部链接定义，此处保持一致
WorkArea g_devLimit{ DEV_X_MIN, DEV_X_MAX, DEV_Y_MIN, DEV_Y_MAX };
WorkArea g_safeArea{ SAFE_INIT_XMIN, SAFE_INIT_XMAX, SAFE_INIT_YMIN, SAFE_INIT_YMAX };

DrawTheme g_theme;             // 主题与运行时参数
bool  g_autoDraw = true;
bool  g_enableDip = false;     // 蘸墨总开关（默认关闭，先排除干扰）
bool  g_highQuality = true;

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
void wprintln(const std::wstring& ws) { std::cout << w2gbk(ws) << "\n"; }
void wprint(const std::wstring& ws) { std::cout << w2gbk(ws); }

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
