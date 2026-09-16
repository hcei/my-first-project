// Robot_merged.cpp — 单文件可编译版（Part 1/2）
// 编译：cl /std:c++17 /utf-8 Robot_merged.cpp
// 特性：GBK 控制台、DRYRUN、Modbus 逐点/批量7点、探边、多Z层、安全区、书法(上区) + 线稿(下区)
// 变更：1) 首划稳态：serial_pre_warm + 首落笔点重复发送（参数上调）
//      2) 蘸墨：两次蘸→十字抖→离液面上下抖5次（上慢下快）
//      3) ★新增位姿跟踪：全局记录“最后已知位置”，为首字“到位→等待→落笔”做准备
//      4) ★修复：批量7点成功后位姿更新用 pts7.back()
//      5) ★修复：急停线程常驻、仅ESC触发且不窃取按键；Z越界校验含Z_OFFSET；蘸墨逐点速度生效

#define NOMINMAX
#include <windows.h>
#include <conio.h>
#include <tchar.h>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <deque>
#include <map>
#include <set>
#include <fstream>
#include <sstream>
#include <iostream>
#include <iomanip>
#include <algorithm>
#include <cmath>
#include <limits>
#include <chrono>
#include <thread>
#include <locale>
#include <ctime>
#include <direct.h>
#include <filesystem>

#include "nlohmann/json.hpp"
#include "shanshui_gen.hpp"
using json = nlohmann::json;

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

// 字号范围（mm）
#define SINGLE_CHAR_MIN 60.0f
#define SINGLE_CHAR_MAX 70.0f
static float ACTIVE_CHAR_SIZE = SINGLE_CHAR_MIN;

// 字间距（可压到2mm）
static float CHAR_SPACING = 12.0f;
static const float CHAR_SPACING_MIN = 0.5f;

// 上下区分割
static float TEXT_TOP_RATIO = 0.46f;
static const float V_GAP_BETWEEN = 1.0f; // 上下区域间隙

// Z 层（mm，负值向下）
static const float Z_UP = -320.0f;
static const float Z_MID = -350.0f;
static const float Z_PRE_DOWN = -363.0f;
static const float Z_DOWN_LIGHT = -382.0f;
static const float Z_DOWN_NORMAL = -385.0f;
static const float Z_DOWN_HEAVY = -388.0f;
static float       Z_OFFSET = 0.0f;  // 运行时整体偏移

// 速度档（1~6）
static int SPEED_LEVEL = 3;
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

// HanziWriter 数据根
static std::string HANZI_BASE_DIR = "D:/objects/hanzi-writer-data";

// 主题
static std::string THEME_NAME = "jiangxue";

// DRYRUN：不打开串口，不发报文
static bool g_dryRun = false;

// 急停
static volatile bool g_estop = false;

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
class SerialPort; // 前置
struct InkStation {
    float x{ 0 }, y{ 0 }, z{ Z_MID };
    bool valid{ false };
};
static InkStation g_ink;

// --------------------------- ★全局位姿跟踪（新增） ---------------------------
static bool  g_pose_init = false;
static Point g_last_pose = { 0.f,0.f,Z_UP,false,(uint8_t)SPEED_LEVEL,"POSE" };

static inline void update_pose_from(const Point& p) {
    g_last_pose = p; g_pose_init = true;
}

// --------------------------- 字符编码工具 ---------------------------
static std::string w2gbk(const std::wstring& ws) {
    if (ws.empty()) return {};
    int len = WideCharToMultiByte(CP_GBK, 0, ws.c_str(), (int)ws.size(), nullptr, 0, nullptr, nullptr);
    std::string out(len, '\0');
    WideCharToMultiByte(CP_GBK, 0, ws.c_str(), (int)ws.size(), out.data(), len, nullptr, nullptr);
    return out;
}
static std::wstring mb2w(const std::string& s) {
    if (s.empty()) return L"";
    int wlen = MultiByteToWideChar(CP_GBK, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring ws(wlen, L'\0');
    MultiByteToWideChar(CP_GBK, 0, s.c_str(), (int)s.size(), ws.data(), wlen);
    return ws;
}
static void wprintln(const std::wstring& ws) { std::cout << w2gbk(ws) << "\n"; }
static void wprint(const std::wstring& ws) { std::cout << w2gbk(ws); }

// --------------------------- 小工具 ---------------------------
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
static inline bool inZRange(float z) { return (z >= -410.0f && z <= -250.0f); }
static inline bool isCJKOrPunct(wchar_t c) {
    if (c == L' ' || c == L'\t' || c == L'\r' || c == L'\n') return false;
    if ((c >= 0x4E00 && c <= 0x9FFF) || (c >= 0x3000 && c <= 0x303F) || (c >= 0xFF00 && c <= 0xFFEF)) return true;
    return false;
}
static std::wstring normalizeComName(const std::wstring& in) {
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
static uint16_t crc16_modbus(const uint8_t* data, size_t len) {
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

// 前置声明（Part 2 实现）
static bool transmitTrajectoryWithSplit(SerialPort& sp, const std::vector<Point>& traj, size_t calli_end);
static bool drawPolylinesFromFile(const std::string& path, std::vector<Point>& traj);
// —— 供 SerialPort 早期引用的前置全局声明 —— 
extern WorkArea g_devLimit;
extern WorkArea g_safeArea;


// --------------------------- 串口封装 ---------------------------
class SerialPort {
public:
    SerialPort() :h_(INVALID_HANDLE_VALUE) {}
    ~SerialPort() { close(); }

    bool open(const std::wstring& portName, DWORD baud = BAUDRATE, BYTE parity = EVENPARITY, BYTE bytesize = 8, BYTE stopbits = ONESTOPBIT) {
        if (g_dryRun) return true;
        close();
        h_ = CreateFileW(portName.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h_ == INVALID_HANDLE_VALUE) { wprintln(L"[错误] 串口打开失败。"); return false; }
        DCB dcb{}; dcb.DCBlength = sizeof(DCB);
        if (!GetCommState(h_, &dcb)) { wprintln(L"[错误] 获取串口状态失败。"); close(); return false; }
        dcb.BaudRate = baud; dcb.Parity = parity; dcb.fParity = TRUE;
        dcb.ByteSize = bytesize; dcb.StopBits = stopbits;
        dcb.fBinary = TRUE; dcb.fDtrControl = DTR_CONTROL_ENABLE; dcb.fRtsControl = RTS_CONTROL_ENABLE;
        if (!SetCommState(h_, &dcb)) { wprintln(L"[错误] 设置串口参数失败。"); close(); return false; }
        COMMTIMEOUTS to{}; to.ReadIntervalTimeout = 20; to.ReadTotalTimeoutConstant = 800; to.WriteTotalTimeoutConstant = 1200;
        if (!SetCommTimeouts(h_, &to)) { wprintln(L"[错误] 设置串口超时失败。"); close(); return false; }
        SetupComm(h_, 1024, 1024);
        PurgeComm(h_, PURGE_RXCLEAR | PURGE_TXCLEAR);
        return true;
    }
    void close() { if (h_ != INVALID_HANDLE_VALUE) { CloseHandle(h_); h_ = INVALID_HANDLE_VALUE; } }

    bool write(const uint8_t* buf, size_t len) {
        if (g_dryRun) {
            std::ostringstream oss; oss << "[TX] ";
            for (size_t i = 0; i < len; i++) oss << std::hex << std::uppercase << std::setw(2) << std::setfill('0') << int(buf[i]) << " ";
            std::cout << oss.str() << "\n"; return true;
        }
        DWORD w = 0; if (!WriteFile(h_, buf, (DWORD)len, &w, nullptr)) return false;
        return (w == len);
    }

    bool read_exact(uint8_t* buf, size_t need, DWORD timeout_ms) {
        if (g_dryRun) return false;
        DWORD start = GetTickCount(); size_t got = 0;
        while (got < need) {
            DWORD r = 0;
            if (!ReadFile(h_, buf + got, (DWORD)(need - got), &r, nullptr)) return false;
            got += r;
            if (got >= need) return true;
            if (GetTickCount() - start > timeout_ms) break;
            Sleep(2);
        }
        return (got >= need);
    }

    bool read(uint8_t* buf, size_t expectLen, size_t& outLen) {
        if (g_dryRun) {
            std::vector<uint8_t> rx = { MB_SLAVE,MB_FUNC,(uint8_t)(REG_START >> 8),(uint8_t)(REG_START & 0xFF),
                                        (uint8_t)(REG_COUNT >> 8),(uint8_t)(REG_COUNT & 0xFF) };
            uint16_t crc = crc16_modbus(rx.data(), rx.size());
            rx.push_back(crc & 0xFF); rx.push_back((crc >> 8) & 0xFF);
            outLen = rx.size(); memcpy(buf, rx.data(), outLen);
            std::ostringstream oss; oss << "[RX] ";
            for (size_t i = 0; i < outLen; i++) oss << std::hex << std::uppercase << std::setw(2) << std::setfill('0') << int(buf[i]) << " ";
            std::cout << oss.str() << "\n"; return true;
        }
        bool ok = read_exact(buf, expectLen, 800);
        outLen = ok ? expectLen : 0; return ok;
    }

    bool sendPoint(const Point& p) {
        // ★修复：Z 越界校验包含 Z_OFFSET（此前校验原始 z，实际发送 z+偏移，偏移过大时校验失效）
        if (!inZRange(p.z + Z_OFFSET) || !inXYRange(p.x, p.y, g_devLimit)) {
            std::wstringstream ws; ws << L"[错误] 点越界 X=" << p.x << L" Y=" << p.y << L" Z=" << (p.z + Z_OFFSET);
            wprintln(ws.str()); return false;
        }
        // ★修复：优先使用点自带的速度档 p.speed（1~6），蘸墨的"上慢下快"才能生效
        int lvl = (p.speed >= 1 && p.speed <= (uint8_t)SPEED_MAX) ? (int)p.speed : SPEED_LEVEL;
        uint8_t speedHigh = (uint8_t)std::max(0, std::min(9, lvl - 1));
        uint8_t suctionLow = 0x00;
        uint16_t vReg = (uint16_t(speedHigh) << 8) | suctionLow;

        int16_t x10 = mm_to_dev10(p.x);
        int16_t y10 = mm_to_dev10(p.y);
        int16_t z10 = mm_to_dev10(p.z + Z_OFFSET);
        int16_t a10 = 0; int16_t v10 = (int16_t)vReg;

        uint8_t frame[256]; size_t idx = 0;
        auto push8 = [&](uint8_t v) { frame[idx++] = v; };
        auto push16 = [&](uint16_t v) { frame[idx++] = (uint8_t)(v >> 8); frame[idx++] = (uint8_t)(v & 0xFF); };

        push8(MB_SLAVE); push8(MB_FUNC);
        push16(REG_START); push16(REG_COUNT); push8(uint8_t(REG_COUNT * 2));
        auto pushS16 = [&](int16_t sv) { push16((uint16_t)sv); };
        pushS16(x10); pushS16(y10); pushS16(z10); pushS16(a10); pushS16(v10);
        uint16_t crc = crc16_modbus(frame, idx);
        push8(crc & 0xFF); push8((crc >> 8) & 0xFF);

        if (!g_dryRun) PurgeComm(h_, PURGE_RXCLEAR | PURGE_TXCLEAR);
        if (!write(frame, idx)) return false;
        if (!g_dryRun) Sleep(10);

        if (!g_dryRun) {
            uint8_t rx[8]; size_t rlen = 0; bool ok = read(rx, 8, rlen);
            if (!ok || rlen != 8) wprintln(L"[警告] 未收到完整应答。");
            else {
                uint16_t crc_calc = crc16_modbus(rx, 6);
                uint16_t crc_rx = rx[6] | (uint16_t(rx[7]) << 8);
                if (crc_calc != crc_rx) wprintln(L"[警告] 应答CRC错误。");
                else {
                    std::ostringstream oss; oss << "[RX] ";
                    for (int i = 0; i < 8; i++) oss << std::hex << std::uppercase << std::setw(2) << std::setfill('0') << int(rx[i]) << " ";
                    std::cout << oss.str() << "\n";
                }
            }
        }
        else {
            uint8_t rxm[6] = { MB_SLAVE,MB_FUNC,(uint8_t)(REG_START >> 8),(uint8_t)(REG_START & 0xFF),
                            (uint8_t)(REG_COUNT >> 8),(uint8_t)(REG_COUNT & 0xFF) };
            uint16_t crcm = crc16_modbus(rxm, 6);
            std::ostringstream oss; oss << "[RX] ";
            for (int i = 0; i < 6; i++) oss << std::hex << std::uppercase << std::setw(2) << std::setfill('0') << int(rxm[i]) << " ";
            oss << std::setw(2) << (crcm & 0xFF) << " " << std::setw(2) << ((crcm >> 8) & 0xFF) << " ";
            std::cout << oss.str() << "\n";
        }

        // ★ 成功发送后，更新当前位置（抬/落都记录；Z 含偏移）
        update_pose_from(p);
        return true;
    }

    bool sendPointRetry(const Point& p, int max_retries = 2, int backoff_ms = 25) {
        for (int a = 0; a <= max_retries; ++a) {
            if (sendPoint(p)) return true;
            wprintln(L"[警告] 单点发送失败，重试中...");
            if (!g_dryRun) std::this_thread::sleep_for(std::chrono::milliseconds(backoff_ms));
        }
        return false;
    }

    // —— 批量7点 —— //
    static void packPoint5(const Point& p, int16_t(&out5)[5]) {
        int16_t x10 = mm_to_dev10(p.x);
        int16_t y10 = mm_to_dev10(p.y);
        int16_t z10 = mm_to_dev10(p.z + Z_OFFSET);
        int16_t a10 = 0;
        // ★修复：同 sendPoint，优先使用点自带速度档
        int lvl = (p.speed >= 1 && p.speed <= (uint8_t)SPEED_MAX) ? (int)p.speed : SPEED_LEVEL;
        uint8_t speedHigh = (uint8_t)std::max(0, std::min(9, lvl - 1));
        uint8_t suctionLow = 0x00;
        int16_t v10 = (int16_t)((uint16_t(speedHigh) << 8) | suctionLow);
        out5[0] = x10; out5[1] = y10; out5[2] = z10; out5[3] = a10; out5[4] = v10;
    }

    bool sendPointsBatch7(const std::vector<Point>& pts7, int max_retries = 3) {
        if (pts7.empty()) return true;
        size_t n = std::min<size_t>(pts7.size(), 7);
        for (size_t i = 0; i < n; ++i) {
            // ★修复：Z 校验同样包含 Z_OFFSET
            if (!inZRange(pts7[i].z + Z_OFFSET) || !inXYRange(pts7[i].x, pts7[i].y, g_devLimit)) {
                std::wstringstream ws; ws << L"[错误] 批量点越界 #" << i << L" X=" << pts7[i].x << L" Y=" << pts7[i].y << L" Z=" << (pts7[i].z + Z_OFFSET);
                wprintln(ws.str()); return false;
            }
        }
        uint8_t frame[256]; size_t idx = 0;
        auto push8 = [&](uint8_t v) { frame[idx++] = v; };
        auto push16 = [&](uint16_t v) { frame[idx++] = (uint8_t)(v >> 8); frame[idx++] = (uint8_t)(v & 0xFF); };

        auto build = [&]() {
            idx = 0;
            push8(MB_SLAVE); push8(0x10);
            push16(REG_BATCH7_START);
            push16(REG_BATCH7_COUNT);
            push8(REG_BATCH7_BYTES);
            int16_t reg5[5];
            for (size_t i = 0; i < 7; ++i) {
                const Point& p = pts7[std::min(i, n - 1)];
                packPoint5(p, reg5);
                for (int k = 0; k < 5; ++k) push16((uint16_t)reg5[k]);
            }
            uint16_t crc = crc16_modbus(frame, idx);
            push8(crc & 0xFF); push8((crc >> 8) & 0xFF);
            };

        for (int attempt = 1; attempt <= max_retries; ++attempt) {
            if (!g_dryRun) PurgeComm(h_, PURGE_RXCLEAR | PURGE_TXCLEAR);
            build();

            if (g_dryRun) {
                std::ostringstream oss; oss << "[TX-B7] ";
                for (size_t i = 0; i < idx; ++i) oss << std::hex << std::uppercase << std::setw(2) << std::setfill('0') << int(frame[i]) << " ";
                std::cout << oss.str() << "\n";
            }
            if (!write(frame, idx)) {
                wprintln(L"[警告] 批量写失败，重试...");
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                continue;
            }
            if (!g_dryRun) Sleep(12);

            uint8_t rx[8]; size_t rlen = 0;
            bool ok = g_dryRun ? true : read(rx, 8, rlen);
            if (!ok || (!g_dryRun && rlen != 8)) {
                wprintln(L"[警告] 批量未收到完整应答，重试...");
                std::this_thread::sleep_for(std::chrono::milliseconds(30));
                continue;
            }
            if (!g_dryRun) {
                uint16_t crc_calc = crc16_modbus(rx, 6);
                uint16_t crc_rx = rx[6] | (uint16_t(rx[7]) << 8);
                if (crc_calc != crc_rx) {
                    wprintln(L"[警告] 批量应答CRC错误，重试...");
                    std::this_thread::sleep_for(std::chrono::milliseconds(30));
                    continue;
                }
            }
            if (!g_dryRun) {
                std::ostringstream oss; oss << "[RX-B7] ";
                for (int i = 0; i < 8; ++i) oss << std::hex << std::uppercase << std::setw(2) << std::setfill('0') << int(g_dryRun ? 0 : rx[i]) << " ";
                std::cout << oss.str() << "\n";
            }

            // ★ 批量成功后，以该批的“最后一个点”作为当前位置
            update_pose_from(pts7.back());
            return true;
        }
        wprintln(L"[错误] 批量发送连续失败，放弃该批。");
        return false;
    }

private:
    HANDLE h_;
};

// --------------------------- 过滤与降采样 ---------------------------
static std::vector<Point> filterDuplicatePoints(const std::vector<Point>& in) {
    std::vector<Point> out; out.reserve(in.size());
    auto nearEq = [](float a, float b, float eps) { return std::fabs(a - b) <= eps; };
    for (size_t i = 0; i < in.size(); ++i) {
        if (out.empty()) { out.push_back(in[i]); continue; }
        const Point& prev = out.back();
        float xyEps = 1.5f;
        float zEps = in[i].isPenDown ? 0.6f : 1.5f;
        if (nearEq(prev.x, in[i].x, xyEps) && nearEq(prev.y, in[i].y, xyEps) && nearEq(prev.z, in[i].z, zEps)
            && prev.isPenDown == in[i].isPenDown && prev.speed == in[i].speed) continue;
        out.push_back(in[i]);
    }
    return out;
}

static void resamplePolyline(std::vector<Point>& pts, float minStep) {
    if (pts.size() <= 2) return;
    std::vector<Point> out; out.reserve(pts.size());
    auto dist2d = [](const Point& a, const Point& b) {
        float dx = a.x - b.x, dy = a.y - b.y; return std::sqrt(dx * dx + dy * dy);
        };
    out.push_back(pts.front());
    Point lastKeep = pts.front();
    for (size_t i = 1; i < pts.size() - 1; ++i) {
        if (dist2d(lastKeep, pts[i]) >= minStep) { out.push_back(pts[i]); lastKeep = pts[i]; }
    }
    out.push_back(pts.back());
    pts.swap(out);
}

// —— 节奏估时 —— //
static bool g_highQuality = true;
static float RESAMPLE_STEP_MM_CALLI = 0.6f;
static float RESAMPLE_STEP_MM_DRAW = 1.2f;

static float speedLevelToXYmmPerSec(int level) {
    static const float table_normal[7] = { 0,40,60,80,110,140,180 };
    static const float table_quality[7] = { 0,28,42,56,80,100,128 };
    level = std::max(SPEED_MIN, std::min(SPEED_MAX, level));
    return g_highQuality ? table_quality[level] : table_normal[level];
}
static int get_Z_SETTLE_MS(bool isCalli) { return (g_highQuality && isCalli) ? 160 : Z_SETTLE_MS_BASE; }
static int get_STROKE_BEGIN_DWELL_MS(bool isCalli) { return (g_highQuality && isCalli) ? 110 : STROKE_BEGIN_DWELL_MS_BASE; }
static int get_STROKE_END_DWELL_MS(bool isCalli) { return (g_highQuality && isCalli) ? 120 : STROKE_END_DWELL_MS_BASE; }
static int get_POINT_RATE_LIMIT_MS(bool isCalli) { return (g_highQuality && isCalli) ? 90 : POINT_RATE_LIMIT_MS_BASE; }

static int estimateMoveMs(const Point& prev, const Point& cur, bool isCalli) {
    float vx = speedLevelToXYmmPerSec(SPEED_LEVEL);
    float dx = cur.x - prev.x, dy = cur.y - prev.y;
    float dxy = std::sqrt(dx * dx + dy * dy);
    int t_xy = (vx > 1e-3f) ? int(std::ceil(dxy / vx * 1000.f)) : 0;

    bool zDownNow = cur.isPenDown;
    bool zDownPrev = prev.isPenDown;
    int extra = 0;
    if (!zDownPrev && zDownNow) extra += get_STROKE_BEGIN_DWELL_MS(isCalli);
    if (zDownPrev && !zDownNow) extra += get_STROKE_END_DWELL_MS(isCalli);
    if (std::fabs(cur.z - prev.z) > 1.0f) extra += get_Z_SETTLE_MS(isCalli);

    int base = std::max(get_POINT_RATE_LIMIT_MS(isCalli), t_xy);
    base = std::max(base, DELAY_MS);
    return base + extra;
}

// 角度
static float angle_between(const Point& a, const Point& b, const Point& c) {
    float ux = a.x - b.x, uy = a.y - b.y;
    float vx = c.x - b.x, vy = c.y - b.y;
    float lu = std::sqrt(ux * ux + uy * uy), lv = std::sqrt(vx * vx + vy * vy);
    if (lu < 1e-6f || lv < 1e-6f) return 0.f;
    float cosv = (ux * vx + uy * vy) / (lu * lv);
    cosv = std::max(-1.0f, std::min(1.0f, cosv));
    return std::acos(cosv);
}

// --------------------------- 串口预热（更新位姿） ---------------------------
static void serial_pre_warm(SerialPort& sp) {
    wprintln(L"[预热] 开始...");
    Point center_up{ 0.f,0.f,Z_UP,false,(uint8_t)SPEED_LEVEL,"UP-WARM" };
    int good = 0, tries = 0;
    while (tries < PREWARM_MAX_PROBES && good < PREWARM_MIN_GOOD_RESP) {
        if (sp.sendPointRetry(center_up)) ++good;
        else good = 0;
        ++tries;
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
    }
    update_pose_from(center_up); // ★记录已到位
    if (good >= PREWARM_MIN_GOOD_RESP) wprintln(L"[预热] 成功，进入稳定态。");
    else wprintln(L"[预热] 未完全达到阈值，继续按正常流程发送。");
}

// --------------------------- 轨迹发送（声明，实现在 Part 2） ---------------------------
static bool transmitTrajectoryWithSplit(SerialPort& sp, const std::vector<Point>& traj, size_t calli_end);

// ---------- 后续：Hanzi JSON / 折线JSON / 布局 / 蘸墨 / 菜单 / main 等在 Part 2 ----------
// =========================== Part 2 / 2 ===========================

// --------------------------- 发送轨迹：书法逐点 + 描边批量 + 首落笔加固 + 冷启动强节流 ---------------------------
static bool transmitTrajectoryWithSplit(SerialPort& sp, const std::vector<Point>& traj, size_t calli_end)
{
    if (traj.empty()) return true;

    // 去重（防止重复点）
    std::vector<Point> filtered = filterDuplicatePoints(traj);

    // 按段降采样：书法更密、描边稍稀
    auto resample_range = [&](size_t beg, size_t end, float step) {
        if (end <= beg) return;
        size_t i = beg;
        std::vector<Point> out; out.reserve(end - beg + 16);
        while (i < end) {
            bool pen = filtered[i].isPenDown;
            size_t j = i;
            std::vector<Point> seg;
            for (; j < end && filtered[j].isPenDown == pen; ++j) seg.push_back(filtered[j]);
            if (pen) resamplePolyline(seg, step);
            out.insert(out.end(), seg.begin(), seg.end());
            i = j;
        }
        filtered.erase(filtered.begin() + beg, filtered.begin() + end);
        filtered.insert(filtered.begin() + beg, out.begin(), out.end());
        };

    calli_end = std::min(calli_end, filtered.size());
    if (calli_end > 0) resample_range(0, calli_end, 0.6f);   // RESAMPLE_STEP_MM_CALLI（在 Part 1 已定义为 0.6）
    if (calli_end < filtered.size()) resample_range(calli_end, filtered.size(), 1.2f); // RESAMPLE_STEP_MM_DRAW（1.2）

    const float corner_theta_rad = 75.0f * 3.1415926f / 180.0f;
    const int   corner_dwell_ms = 40;
    const int   hard_corner_ms = 80;

    // 冷启动：本次调用开始时间
    auto t_start = std::chrono::steady_clock::now();

    auto sleep_move = [&](const Point* prev, const Point& cur, bool isCalli) {
        int ms = prev ? estimateMoveMs(*prev, cur, isCalli)
            : std::max(get_POINT_RATE_LIMIT_MS(isCalli), DELAY_MS);
        // 冷启动首秒强限速（设备刚“醒”时更保守）
        auto ms_from_start = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t_start).count();
        if (ms_from_start < 1000) ms = std::max(ms, 150);
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        };

    const Point* lastSent = nullptr;
    bool firstDownDuplicated = false; // 仅“本次调用”的首个落笔点加固

    size_t i = 0;
    while (i < filtered.size()) {
        bool inCalli = (i < calli_end);

        if (g_estop) {
            wprintln(L"[急停] 停止。抬笔复位。");
            Point up = filtered[i]; up.z = Z_UP; up.isPenDown = false; up.zType = "UP-ESTOP";
            sp.sendPointRetry(up);
            return false;
        }

        if (!filtered[i].isPenDown) {
            // 抬笔段：逐点发送（用于定位）
            const Point& p = filtered[i];
            if (!sp.sendPointRetry(p)) return false;
            sleep_move(lastSent, p, inCalli);
            lastSent = &filtered[i];
            ++i;
            continue;
        }

        // —— 落笔段 —— //
        if (inCalli) {
            // 书法段：逐点发送；首落笔点重复一次并停顿
            size_t j = i; while (j < filtered.size() && filtered[j].isPenDown) ++j; // [i, j)

            for (size_t k = i; k < j; ++k) {
                const Point& p = filtered[k];

                // 首落笔加固：检测“由抬到落”的第一个点，且尚未加固过
                bool isFirstDownThisCall = (!firstDownDuplicated) && ((k == 0) || !filtered[k - 1].isPenDown);
                if (isFirstDownThisCall) {
                    // 第一次落笔：重复发送两次，中间停顿；再额外沉稳
                    if (!sp.sendPointRetry(p)) return false;
                    sleep_move(lastSent, p, true);
                    std::this_thread::sleep_for(std::chrono::milliseconds(FIRST_DOWN_DUP_SEND_PAUSE));

                    if (!sp.sendPointRetry(p)) return false;
                    std::this_thread::sleep_for(std::chrono::milliseconds(FIRST_DOWN_EXTRA_DWELL_MS));

                    // 再额外保持一小段，确保压力建立
                    std::this_thread::sleep_for(std::chrono::milliseconds(120));

                    lastSent = &filtered[k];
                    firstDownDuplicated = true;
                    continue;
                }

                if (!sp.sendPointRetry(p)) return false;

                // 角点轻微停顿
                if (k >= 1 && k + 1 < j) {
                    float ang = angle_between(filtered[k - 1], filtered[k], filtered[k + 1]);
                    if (ang > corner_theta_rad) std::this_thread::sleep_for(std::chrono::milliseconds(hard_corner_ms));
                    else if (ang > corner_theta_rad * 0.6f) std::this_thread::sleep_for(std::chrono::milliseconds(corner_dwell_ms));
                }

                sleep_move(lastSent, p, true);
                lastSent = &filtered[k];
            }
            i = j; // 下一段
        }
        else {
            // 描边段：尽量批量每7点；失败回退到逐点
            size_t j = i; while (j < filtered.size() && filtered[j].isPenDown) ++j; // [i,j)
            size_t len = j - i, k = 0;
            while (k < len) {
                size_t chunk = std::min<size_t>(7, len - k);
                std::vector<Point> batch(filtered.begin() + i + k, filtered.begin() + i + k + chunk);

                if (!sp.sendPointsBatch7(batch)) {
                    // 回退逐点 —— 用 filtered 的下标，保证 lastSent 指针稳定
                    for (size_t t = 0; t < batch.size(); ++t) {
                        const Point& p_ref = filtered[i + k + t];  // 指向 filtered 中的真实元素
                        if (!sp.sendPointRetry(p_ref)) return false;
                        sleep_move(lastSent, p_ref, false);
                        lastSent = &p_ref;                          // 指向稳定存储
                    }
                }
                else {
                    const Point& plast = filtered[i + k + chunk - 1];
                    sleep_move(lastSent, plast, false);
                    lastSent = &plast;
                }
                k += chunk;
            }
            i = j;
        }
    }
    return true;
}

// --------------------------- HanziWriter JSON 解析与生成 ---------------------------
static void bboxOfStrokes(const std::vector<MMAHStroke>& ss, float& minx, float& maxx, float& miny, float& maxy) {
    minx = miny = std::numeric_limits<float>::infinity();
    maxx = maxy = -std::numeric_limits<float>::infinity();
    for (const auto& s : ss) for (const auto& p : s.centerLine) {
        minx = std::min(minx, p.first); maxx = std::max(maxx, p.first);
        miny = std::min(miny, p.second); maxy = std::max(maxy, p.second);
    }
    if (!std::isfinite(minx)) { minx = maxx = miny = maxy = 0.f; }
}

static MMAHCharData parseHanziWriterJson(const std::string& path, wchar_t wch) {
    MMAHCharData ret; ret.charStr = std::wstring(1, wch);
    std::ifstream ifs(path);
    if (!ifs) { std::wstringstream ws; ws << L"[错误] 打开汉字JSON失败：" << mb2w(path); wprintln(ws.str()); return ret; }
    json j; try { ifs >> j; }
    catch (...) { wprintln(L"[错误] 解析汉字JSON失败。"); return ret; }
    if (!j.contains("medians") || !j["medians"].is_array()) { wprintln(L"[错误] 汉字JSON缺少 medians。"); return ret; }

    int order = 0;
    for (auto& st : j["medians"]) {
        if (!st.is_array() || st.empty()) continue;
        MMAHStroke s; s.order = ++order;
        for (auto& seg : st) {
            if (!seg.is_array() || seg.size() < 2) continue;
            float x = float(seg[0].get<double>());
            float y = float(seg[1].get<double>());
            s.centerLine.emplace_back(x, y);
        }
        if (s.centerLine.size() >= 2) {
            auto a = s.centerLine.front(), b = s.centerLine.back();
            float dx = b.first - a.first, dy = b.second - a.second;
            if (std::fabs(dx) > std::fabs(dy) * 1.5f) s.type = "heng";
            else if (std::fabs(dy) > std::fabs(dx) * 1.5f) s.type = "shu";
            else if (s.centerLine.size() <= 3) s.type = "dot";
            else s.type = "other";
        }
        else s.type = "dot";
        if (!s.centerLine.empty()) ret.strokes.push_back(std::move(s));
    }
    ret.isValid = !ret.strokes.empty();
    return ret;
}

static bool loadCharData(wchar_t wc, MMAHCharData& out) {
    std::string gbk1;
    {
        std::wstring one(1, wc);
        gbk1 = w2gbk(one);
        if (gbk1.empty()) {
            std::ostringstream oss; oss << std::hex << std::uppercase << int((unsigned)wc);
            gbk1 = "U" + oss.str();
        }
    }
    std::string path = HANZI_BASE_DIR + "/data/" + gbk1 + ".json";
    out = parseHanziWriterJson(path, wc);
    return out.isValid;
}

static bool generateSingleCharTrajectory(const MMAHCharData& ch, int baseSpeed, std::vector<Point>& traj, float size_mm) {
    if (!ch.isValid) return false;
    float minx, maxx, miny, maxy; bboxOfStrokes(ch.strokes, minx, maxx, miny, maxy);
    float w = std::max(1.0f, maxx - minx);
    float h = std::max(1.0f, maxy - miny);
    float S = size_mm / std::max(w, h);
    auto toLocal = [&](std::pair<float, float> p) {
        return std::pair<float, float>{ (p.first - minx)* S, (p.second - miny)* S };
        };
    auto zFor = [&](const std::string& t)->float {
        if (t == "dot")  return Z_DOWN_HEAVY;
        if (t == "heng") return Z_DOWN_NORMAL;
        if (t == "shu")  return Z_DOWN_NORMAL;
        return Z_DOWN_LIGHT;
        };

    for (const auto& s : ch.strokes) {
        if (s.centerLine.empty()) continue;
        auto p0 = toLocal(s.centerLine.front());
        float zdown = zFor(s.type);
        traj.push_back(Point{ p0.first, p0.second, Z_UP,       false,(uint8_t)baseSpeed,"UP" });
        traj.push_back(Point{ p0.first, p0.second, Z_MID,      false,(uint8_t)baseSpeed,"MID" });
        traj.push_back(Point{ p0.first, p0.second, Z_PRE_DOWN, false,(uint8_t)baseSpeed,"PRE" });
        traj.push_back(Point{ p0.first, p0.second, zdown,      true, (uint8_t)baseSpeed,s.type });
        for (size_t k = 1; k < s.centerLine.size(); ++k) {
            auto pk = toLocal(s.centerLine[k]);
            traj.push_back(Point{ pk.first, pk.second, zdown, true,(uint8_t)baseSpeed,s.type });
        }
        auto pe = toLocal(s.centerLine.back());
        traj.push_back(Point{ pe.first, pe.second, Z_PRE_DOWN, false,(uint8_t)baseSpeed,"PRE" });
        traj.push_back(Point{ pe.first, pe.second, Z_MID,      false,(uint8_t)baseSpeed,"MID" });
    }
    // 末尾抬笔，方便分字
    if (!traj.empty()) {
        Point last = traj.back(); last.z = Z_UP; last.isPenDown = false; last.zType = "UP-ENDCHAR";
        traj.push_back(last);
    }
    return true;
}

// --------------------------- 折线 JSON（作画） ---------------------------
static bool loadPolylinesFromJson(const std::string& path, std::vector<PolyPath2D>& polys, float& mm_per_unit, float& user_scale) {
    std::ifstream ifs(path);
    if (!ifs) { std::wstringstream ws; ws << L"[错误] 打开线稿JSON失败：" << mb2w(path); wprintln(ws.str()); return false; }
    json j; try { ifs >> j; }
    catch (...) { wprintln(L"[错误] 解析线稿JSON失败。"); return false; }
    std::string units = j.value("units", "mm");
    user_scale = float(j.value("scale", 1.0));
    if (units == "mm") mm_per_unit = 1.0f;
    else if (units == "px") mm_per_unit = float(j.value("mm_per_unit", 0.264583));
    else { wprintln(L"[警告] 未知units，按mm处理。"); mm_per_unit = 1.0f; }

    if (!j.contains("polylines") || !j["polylines"].is_array()) { wprintln(L"[错误] 线稿JSON缺少 polylines。"); return false; }
    for (auto& pl : j["polylines"]) {
        if (!pl.contains("points") || !pl["points"].is_array()) continue;
        PolyPath2D p; p.closed = pl.value("closed", false);
        for (auto& pt : pl["points"]) {
            if (!pt.is_array() || pt.size() < 2) continue;
            float x = float(pt[0].get<double>()) * mm_per_unit * user_scale;
            float y = float(pt[1].get<double>()) * mm_per_unit * user_scale;
            p.pts.emplace_back(x, y);
        }
        if (p.pts.size() >= 2) polys.push_back(std::move(p));
    }
    return !polys.empty();
}

static void bboxOfPolys(const std::vector<PolyPath2D>& polys, float& minx, float& maxx, float& miny, float& maxy) {
    minx = miny = std::numeric_limits<float>::infinity();
    maxx = maxy = -std::numeric_limits<float>::infinity();
    for (const auto& p : polys) for (const auto& q : p.pts) {
        minx = std::min(minx, q.first); maxx = std::max(maxx, q.first);
        miny = std::min(miny, q.second); maxy = std::max(maxy, q.second);
    }
    if (!std::isfinite(minx)) { minx = maxx = miny = maxy = 0.f; }
}

static void fitPolylinesToArea(std::vector<PolyPath2D>& polys, const WorkArea& area) {
    if (polys.empty()) return;
    float minx, maxx, miny, maxy; bboxOfPolys(polys, minx, maxx, miny, maxy);
    float w = std::max(1.0f, maxx - minx), h = std::max(1.0f, maxy - miny);
    float targetW = (area.xmax - area.xmin) - SAFE_MARGIN * 2;
    float targetH = (area.ymax - area.ymin) - SAFE_MARGIN * 2;
    float s = std::min(targetW / w, targetH / h);
    float ox = area.xmin + SAFE_MARGIN - minx * s + (targetW - w * s) * 0.5f;
    float oy = area.ymin + SAFE_MARGIN - miny * s + (targetH - h * s) * 0.5f;
    for (auto& p : polys) for (auto& q : p.pts) { q.first = q.first * s + ox; q.second = q.second * s + oy; }
}

static void generatePolylineTrajectory(const std::vector<PolyPath2D>& polys, std::vector<Point>& traj, int speed, float drawZ) {
    for (const auto& p : polys) {
        if (p.pts.size() < 2) continue;
        auto s = p.pts.front();
        traj.push_back(Point{ s.first, s.second, Z_UP,       false,(uint8_t)speed,"UP" });
        traj.push_back(Point{ s.first, s.second, Z_MID,      false,(uint8_t)speed,"MID" });
        traj.push_back(Point{ s.first, s.second, Z_PRE_DOWN, false,(uint8_t)speed,"PRE" });
        traj.push_back(Point{ s.first, s.second, drawZ,      true, (uint8_t)speed,"DRAW" });
        for (size_t i = 1; i < p.pts.size(); ++i) {
            traj.push_back(Point{ p.pts[i].first, p.pts[i].second, drawZ, true,(uint8_t)speed,"DRAW" });
        }
        if (p.closed) traj.push_back(Point{ s.first, s.second, drawZ, true,(uint8_t)speed,"DRAW" });
        auto e = p.pts.back();
        traj.push_back(Point{ e.first, e.second, Z_PRE_DOWN, false,(uint8_t)speed,"PRE" });
        traj.push_back(Point{ e.first, e.second, Z_MID,      false,(uint8_t)speed,"MID" });
        traj.push_back(Point{ e.first, e.second, Z_UP,       false,(uint8_t)speed,"UP" });
    }
}

static bool drawPolylinesFromFile(const std::string& path, std::vector<Point>& traj) {
    std::vector<PolyPath2D> polys; float mm_per_unit = 1.0f, user_scale = 1.0f;
    if (!loadPolylinesFromJson(path, polys, mm_per_unit, user_scale)) return false;

    // 放到底部区域
    WorkArea area = { SAFE_INIT_XMIN, SAFE_INIT_XMAX, SAFE_INIT_YMIN, SAFE_INIT_YMAX };
    float totalH = area.ymax - area.ymin;
    float bottom_ymin = area.ymin + SAFE_MARGIN;
    float bottom_ymax = area.ymin + totalH * (1.0f - TEXT_TOP_RATIO) - V_GAP_BETWEEN * 0.5f - SAFE_MARGIN;
    if (bottom_ymax < bottom_ymin + 10.0f) bottom_ymax = bottom_ymin + std::max(10.0f, totalH * 0.2f);
    WorkArea bottom{ area.xmin + SAFE_MARGIN, area.xmax - SAFE_MARGIN, bottom_ymin, bottom_ymax };

    fitPolylinesToArea(polys, bottom);
    generatePolylineTrajectory(polys, traj, /*speed*/1, /*drawZ*/Z_DOWN_LIGHT);
    return true;
}

// --------------------------- 全局状态定义 ---------------------------
// 注意：Part 1 中有 extern 声明，因此这里必须使用外部链接（不可 static）
WorkArea g_devLimit{ DEV_X_MIN, DEV_X_MAX, DEV_Y_MIN, DEV_Y_MAX };
WorkArea g_safeArea{ SAFE_INIT_XMIN, SAFE_INIT_XMAX, SAFE_INIT_YMIN, SAFE_INIT_YMAX };

// 主题与运行时参数
static DrawTheme g_theme;
static bool g_autoDraw = true;

// 蘸墨总开关（默认关闭，先排除干扰）
static bool g_enableDip = false;

// --------------------------- 安全区/布局 ---------------------------
static bool layout_fit(int n, float W, float H, float S, float sp, int cols, int& out_rows) {
    if (cols <= 0) return false;
    out_rows = (n + cols - 1) / cols;
    float full_w = cols * S + (cols - 1) * sp;
    float full_h = out_rows * S + (out_rows - 1) * sp;
    return (full_w <= W + 1e-3f) && (full_h <= H + 1e-3f);
}

static TextPlan plan_text_area_and_layout(
    int n, const WorkArea& safe, float init_top_ratio,
    float vgap_mm, float sp_init)
{
    TextPlan tp;
    if (n <= 0) {
        tp.ok = true; tp.cols = 5; tp.rows = 0; tp.used_S = SINGLE_CHAR_MAX; tp.used_sp = sp_init; tp.top_ratio = init_top_ratio; tp.text_area = safe;
        return tp;
    }

    // 在上区划出文本区
    for (float ratio = init_top_ratio; ratio <= 0.90f + 1e-6f; ratio += 0.02f) {
        float totalH = safe.ymax - safe.ymin;
        float totalW = safe.xmax - safe.xmin;
        float textH = totalH * ratio - vgap_mm * 0.5f;
        float textW = totalW - SAFE_MARGIN * 2.0f;
        if (textH <= 0 || textW <= 0) continue;

        for (int colsTry : {5, 4}) {
            for (float S = SINGLE_CHAR_MAX; S >= SINGLE_CHAR_MIN - 1e-3f; S -= 1.0f) {
                for (float sp = sp_init; sp >= CHAR_SPACING_MIN - 1e-3f; sp -= 1.0f) {
                    int rows = 0;
                    if (!layout_fit(n, textW, textH, S, sp, colsTry, rows)) continue;
                    tp.ok = true; tp.cols = colsTry; tp.rows = rows; tp.used_S = S; tp.used_sp = sp; tp.top_ratio = ratio;
                    float text_ymax = safe.ymax - vgap_mm * 0.5f;
                    float text_ymin = text_ymax - textH;
                    tp.text_area = WorkArea{ safe.xmin + SAFE_MARGIN, safe.xmin + SAFE_MARGIN + textW, text_ymin, text_ymax };

                    tp.offsets.clear(); tp.offsets.reserve(n);
                    for (int i = 0; i < n; ++i) {
                        int r = i / tp.cols, c = i % tp.cols;
                        float ox = c * (S + sp);
                        float oy = textH - S - r * (S + sp);
                        tp.offsets.push_back(Offset{ ox,oy });
                    }
                    return tp;
                }
            }
        }
    }
    tp.ok = false; return tp;
}

static bool prepare_layout_only(const std::wstring& wtext_in, TextPlan& plan_out, std::wstring& chars_out) {
    chars_out.clear();
    for (wchar_t c : wtext_in) if (isCJKOrPunct(c)) chars_out.push_back(c);
    if (chars_out.empty()) { wprintln(L"[错误] 无有效汉字。"); return false; }
    plan_out = plan_text_area_and_layout((int)chars_out.size(), g_safeArea, TEXT_TOP_RATIO, V_GAP_BETWEEN, CHAR_SPACING);
    if (!plan_out.ok) {
        wprintln(L"[错误] 排版失败。尝试：减少字数/提高上区比例/减小字号/减小字距。");
        return false;
    }
    ACTIVE_CHAR_SIZE = plan_out.used_S;
    TEXT_TOP_RATIO = plan_out.top_ratio;
    CHAR_SPACING = plan_out.used_sp;
    return true;
}

// --------------------------- 蘸墨流程（两次蘸→十字抖→离液面“上慢下快”5次） ---------------------------
static bool do_dip_and_groom(SerialPort& sp, const InkStation& ink) {
    if (!g_enableDip) return true;          // 总开关：默认关闭
    if (!ink.valid) {
        wprintln(L"[提示] 未设置蘸墨位（菜单14）。跳过蘸墨。");
        return true;
    }
    const uint8_t SLOW = 1;
    const uint8_t FAST = (uint8_t)std::min(6, std::max(2, SPEED_LEVEL));
    const float dx = 8.0f, dy = 8.0f; // 十字抖动幅度

    std::vector<Point> traj; traj.reserve(128);

    // 到位（抬笔）
    traj.push_back(Point{ ink.x, ink.y, Z_UP,  false, SLOW, "UP-INK" });
    traj.push_back(Point{ ink.x, ink.y, Z_MID, false, SLOW, "MID-INK" });

    // —— 两次蘸 —— //
    for (int t = 0; t < 2; ++t) {
        traj.push_back(Point{ ink.x, ink.y, ink.z, false, SLOW, "DIP" });
        traj.push_back(Point{ ink.x, ink.y, Z_MID, false, SLOW, "RISE" });
    }

    // —— 十字抖动（在 Z_MID 附近）—— //
    traj.push_back(Point{ ink.x - dx, ink.y,     Z_MID, false, SLOW, "GROOM-X" });
    traj.push_back(Point{ ink.x + dx, ink.y,     Z_MID, false, SLOW, "GROOM-X" });
    traj.push_back(Point{ ink.x,      ink.y,     Z_MID, false, SLOW, "GROOM-C" });
    traj.push_back(Point{ ink.x,      ink.y - dy,Z_MID, false, SLOW, "GROOM-Y" });
    traj.push_back(Point{ ink.x,      ink.y + dy,Z_MID, false, SLOW, "GROOM-Y" });
    traj.push_back(Point{ ink.x,      ink.y,     Z_MID, false, SLOW, "GROOM-C" });

    // —— 离液面后上下抖 5 次：上慢下快 —— //
    for (int k = 0; k < 5; ++k) {
        traj.push_back(Point{ ink.x, ink.y, Z_MID, false, SLOW, "SHAKE-UP-SLOW" });   // 上拉慢
        traj.push_back(Point{ ink.x, ink.y, ink.z, false, FAST, "SHAKE-DOWN-FAST" }); // 迅速下
    }
    // 结束抬笔
    traj.push_back(Point{ ink.x, ink.y, Z_UP, false, SLOW, "UP-END" });

    // 实际发送（作为“描边”逻辑：不需要角点停顿，calli_end=0）
    return transmitTrajectoryWithSplit(sp, traj, 0);
}

// --------------------------- ★抬笔预定位→等待（首字/每行首字用） ---------------------------
static bool move_up_to_and_wait(SerialPort& sp, const Point& targetUp, bool isCalli) {
    // 1) 先抬笔到目标XY（保证安全）
    Point up = targetUp;
    up.z = Z_UP;
    up.isPenDown = false;
    up.zType = "UP-PREPOS";
    if (!sp.sendPointRetry(up)) return false;

    // 2) 估算到位时间：若没有历史pose，就按 0,0,UP 估
    Point prev = g_pose_init ? g_last_pose : Point{ 0.f,0.f,Z_UP,false,(uint8_t)SPEED_LEVEL,"POSE0" };
    float vx = speedLevelToXYmmPerSec(SPEED_LEVEL);
    float dx = up.x - prev.x, dy = up.y - prev.y;
    float dxy = std::sqrt(dx * dx + dy * dy);
    int t_xy = (vx > 1e-3f) ? int(std::ceil(dxy / vx * 1000.f)) : 0;

    // 3) 加冗余：远距离给更大guard；再加 Z settle
    int guard = (dxy > 20.f ? 600 : 250) + get_Z_SETTLE_MS(isCalli);
    {
        std::wstringstream ws;
        ws << L"[预定位] dXY=" << dxy << L"mm 估计t=" << t_xy << L"ms + guard=" << guard << L"ms";
        wprintln(ws.str());
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(std::max(t_xy, get_POINT_RATE_LIMIT_MS(isCalli)) + guard));

    update_pose_from(up);
    return true;
}

// --------------------------- 逐字流式写字（预热；可选蘸墨；首字/每行首字预定位） ---------------------------
static bool write_text_streamed(SerialPort& sp, const std::wstring& chars, const TextPlan& plan, int baseSpeed, int per_char_pause_ms = 120) {
    // 串口预热
    serial_pre_warm(sp);

    // 开写前可选蘸墨（默认关闭，用菜单15开启）
    if (g_enableDip) {
        do_dip_and_groom(sp, g_ink);
    }

    for (size_t i = 0; i < chars.size(); ++i) {
        if (g_estop) return false;

        MMAHCharData ch;
        if (!loadCharData(chars[i], ch)) {
            std::wstringstream ws; ws << L"[警告] 找不到字形数据，跳过：" << std::wstring(1, chars[i]);
            wprintln(ws.str());
            continue;
        }

        std::vector<Point> local;
        if (!generateSingleCharTrajectory(ch, baseSpeed, local, ACTIVE_CHAR_SIZE)) continue;

        // 加上版面偏移
        std::vector<Point> one; one.reserve(local.size() + 2);
        const Offset of = plan.offsets[(int)i];
        for (const auto& p : local) {
            one.push_back(Point{
                plan.text_area.xmin + of.x + p.x,
                plan.text_area.ymin + of.y + p.y,
                p.z, p.isPenDown, p.speed, p.zType
                });
        }

        // ★ 首字 或 每行首字：抬笔预定位→等待，再写整字
        if (i == 0 || (plan.cols > 0 && (int)i % plan.cols == 0)) {
            if (!one.empty()) {
                Point firstUp = one.front();
                firstUp.z = Z_UP;
                firstUp.isPenDown = false;
                firstUp.zType = (i == 0) ? "UP-FIRST-ANCHOR" : "UP-LINE-ANCHOR";
                if (!move_up_to_and_wait(sp, firstUp, /*isCalli*/true)) return false;
                // 额外沉稳一点（开启蘸墨时更久）
                std::this_thread::sleep_for(std::chrono::milliseconds(g_enableDip ? 400 : 200));
            }
        }

        // 发送（书法全部算 calli 段）
        if (!transmitTrajectoryWithSplit(sp, one, one.size())) return false;

        // 每写5个字再蘸一次（开启时）
        if (g_enableDip && (((int)(i + 1)) % 5 == 0)) {
            do_dip_and_groom(sp, g_ink);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(per_char_pause_ms));
    }

    // 收尾：若总字数不是5的倍数且开启蘸墨，再补一次
    if (g_enableDip && (((int)chars.size() % 5) != 0)) {
        do_dip_and_groom(sp, g_ink);
    }
    return true;
}

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
