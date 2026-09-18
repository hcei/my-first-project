// serial_port.cpp — SerialPort 实现（Modbus 帧构造与收发）
// 由 Robot.cpp（单文件版）拆分而来；实现逐行保真。
// ★GUI 阶段：sendPoint/sendPointRetry/sendPointsBatch7 增加设备事件钩子
//   （TX/RX 帧、ACK 校验结果、软件位姿更新时间戳入 logs/audit_*.jsonl）。
//   钩子在原发送路径完成后调用，不改变原打印/重试/位姿逻辑。
#include "serial_port.h"
#include "gui_service.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <thread>
#include <vector>

bool SerialPort::open(const std::wstring& portName, DWORD baud, BYTE parity, BYTE bytesize, BYTE stopbits) {
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

void SerialPort::close() { if (h_ != INVALID_HANDLE_VALUE) { CloseHandle(h_); h_ = INVALID_HANDLE_VALUE; } }

bool SerialPort::write(const uint8_t* buf, size_t len) {
    std::ostringstream oss; oss << "[TX] ";
    for (size_t i = 0; i < len; i++) oss << std::hex << std::uppercase << std::setw(2) << std::setfill('0') << int(buf[i]) << " ";
    if (g_dryRun) { wprintln(mb2w(oss.str())); return true; }
    log_line(mb2w(oss.str()));   // ★真机模式帧同样入日志（不上屏，避免刷屏）
    DWORD w = 0; if (!WriteFile(h_, buf, (DWORD)len, &w, nullptr)) return false;
    return (w == len);
}

bool SerialPort::read_exact(uint8_t* buf, size_t need, DWORD timeout_ms) {
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

bool SerialPort::read(uint8_t* buf, size_t expectLen, size_t& outLen) {
    if (g_dryRun) {
        std::vector<uint8_t> rx = { MB_SLAVE,MB_FUNC,(uint8_t)(REG_START >> 8),(uint8_t)(REG_START & 0xFF),
                                    (uint8_t)(REG_COUNT >> 8),(uint8_t)(REG_COUNT & 0xFF) };
        uint16_t crc = crc16_modbus(rx.data(), rx.size());
        rx.push_back(crc & 0xFF); rx.push_back((crc >> 8) & 0xFF);
        outLen = rx.size(); memcpy(buf, rx.data(), outLen);
        std::ostringstream oss; oss << "[RX] ";
        for (size_t i = 0; i < outLen; i++) oss << std::hex << std::uppercase << std::setw(2) << std::setfill('0') << int(buf[i]) << " ";
        wprintln(mb2w(oss.str())); return true;
    }
    bool ok = read_exact(buf, expectLen, 800);
    outLen = ok ? expectLen : 0; return ok;
}

bool SerialPort::sendPoint(const Point& p) {
    // ★修复：Z 越界校验包含 Z_OFFSET（此前校验原始 z，实际发送 z+偏移，偏移过大时校验失效）
    if (!inZRange(p.z + Z_OFFSET) || !inXYRange(p.x, p.y, g_devLimit)) {
        std::wstringstream ws; ws << L"[错误] 点越界 X=" << p.x << L" Y=" << p.y << L" Z=" << (p.z + Z_OFFSET)
            << L"（原始Z=" << p.z << L"，Z_OFFSET=" << Z_OFFSET << L"，设备可动范围 " << g_z_bottom << L"~" << g_z_top << L"）";
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

    bool response_ok = g_dryRun;
    uint8_t rx[8] = { 0 }; size_t rlen = 0;   // ★GUI 钩子复用（DRYUN 分支在下方填充）
    if (!g_dryRun) {
        bool ok = read(rx, 8, rlen);
        if (!ok || rlen != 8) wprintln(L"[警告] 未收到完整应答。");
        else {
            uint16_t crc_calc = crc16_modbus(rx, 6);
            uint16_t crc_rx = rx[6] | (uint16_t(rx[7]) << 8);
            if (crc_calc != crc_rx) wprintln(L"[警告] 应答CRC错误。");
            else if (rx[0] != MB_SLAVE || rx[1] != MB_FUNC ||
                     rx[2] != uint8_t(REG_START >> 8) || rx[3] != uint8_t(REG_START & 0xFF) ||
                     rx[4] != uint8_t(REG_COUNT >> 8) || rx[5] != uint8_t(REG_COUNT & 0xFF)) {
                wprintln(L"[警告] 应答地址、功能码或寄存器范围不匹配。");
            }
            else {
                response_ok = true;
                std::ostringstream oss; oss << "[RX] ";
                for (int i = 0; i < 8; i++) oss << std::hex << std::uppercase << std::setw(2) << std::setfill('0') << int(rx[i]) << " ";
                wprintln(mb2w(oss.str()));
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
        wprintln(mb2w(oss.str()));
    }

    if (!response_ok) return false;

    // ★GUI 钩子：帧 + ACK 结果入审计日志（DRYRUN 下即本函数打印的模拟应答）
    {
        uint8_t rxd[8] = { 0 }; size_t rxdlen = 0;
        if (g_dryRun) {
            uint8_t m[6] = { MB_SLAVE,MB_FUNC,(uint8_t)(REG_START >> 8),(uint8_t)(REG_START & 0xFF),
                            (uint8_t)(REG_COUNT >> 8),(uint8_t)(REG_COUNT & 0xFF) };
            uint16_t c = crc16_modbus(m, 6);
            memcpy(rxd, m, 6);
            rxd[6] = c & 0xFF; rxd[7] = (c >> 8) & 0xFF;
            rxdlen = 8;
        }
        else { memcpy(rxd, rx, rlen); rxdlen = rlen; }
        const char* err_code = "";
        if (!g_dryRun && rlen != 8) err_code = "timeout";
        gs::device::log_send_point(p, frame, idx, rxd, rxdlen, response_ok, response_ok, err_code);
    }

    // 仅在收到结构和 CRC 均有效的应答后更新位姿。
    update_pose_from(p);
    gs::device::note_pose_updated();
    return true;
}

bool SerialPort::sendPointRetry(const Point& p, int max_retries, int backoff_ms) {
    for (int a = 0; a <= max_retries; ++a) {
        gs::device::set_attempt(a + 1);
        if (sendPoint(p)) return true;
        wprintln(L"[警告] 单点发送失败，重试中...");
        if (!g_dryRun) std::this_thread::sleep_for(std::chrono::milliseconds(backoff_ms));
    }
    return false;
}

// —— 批量7点 —— //
void SerialPort::packPoint5(const Point& p, int16_t(&out5)[5]) {
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

bool SerialPort::sendPointsBatch7(const std::vector<Point>& pts7, int max_retries) {
    if (pts7.empty()) return true;
    size_t n = std::min<size_t>(pts7.size(), 7);
    for (size_t i = 0; i < n; ++i) {
        // ★修复：Z 校验同样包含 Z_OFFSET
        if (!inZRange(pts7[i].z + Z_OFFSET) || !inXYRange(pts7[i].x, pts7[i].y, g_devLimit)) {
            std::wstringstream ws; ws << L"[错误] 批量点越界 #" << i << L" X=" << pts7[i].x << L" Y=" << pts7[i].y << L" Z=" << (pts7[i].z + Z_OFFSET)
                << L"（原始Z=" << pts7[i].z << L"，Z_OFFSET=" << Z_OFFSET << L"，设备可动范围 " << g_z_bottom << L"~" << g_z_top << L"）";
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

    // max_retries 表示失败后的重试次数；即使传入 0，也必须至少执行一次发送。
    const int attempts = std::max(1, max_retries + 1);
    for (int attempt = 1; attempt <= attempts; ++attempt) {
        if (!g_dryRun) PurgeComm(h_, PURGE_RXCLEAR | PURGE_TXCLEAR);
        build();

        if (g_dryRun) {
            std::ostringstream oss; oss << "[TX-B7] ";
            for (size_t i = 0; i < idx; ++i) oss << std::hex << std::uppercase << std::setw(2) << std::setfill('0') << int(frame[i]) << " ";
            wprintln(mb2w(oss.str()));
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
        bool response_ok = g_dryRun;
        if (!g_dryRun) {
            uint16_t crc_calc = crc16_modbus(rx, 6);
            uint16_t crc_rx = rx[6] | (uint16_t(rx[7]) << 8);
            if (crc_calc != crc_rx) {
                wprintln(L"[警告] 批量应答CRC错误，重试...");
                std::this_thread::sleep_for(std::chrono::milliseconds(30));
                continue;
            }
            if (rx[0] != MB_SLAVE || rx[1] != 0x10 ||
                rx[2] != uint8_t(REG_BATCH7_START >> 8) || rx[3] != uint8_t(REG_BATCH7_START & 0xFF) ||
                rx[4] != uint8_t(REG_BATCH7_COUNT >> 8) || rx[5] != uint8_t(REG_BATCH7_COUNT & 0xFF)) {
                wprintln(L"[警告] 批量应答地址、功能码或寄存器范围不匹配，重试...");
                std::this_thread::sleep_for(std::chrono::milliseconds(30));
                continue;
            }
            response_ok = true;
        }
        if (!g_dryRun) {
            std::ostringstream oss; oss << "[RX-B7] ";
            for (int i = 0; i < 8; ++i) oss << std::hex << std::uppercase << std::setw(2) << std::setfill('0') << int(g_dryRun ? 0 : rx[i]) << " ";
            wprintln(mb2w(oss.str()));
        }

        // ★ 批量成功后，以该批的“最后一个点”作为当前位置
        if (!response_ok) return false;

        // ★GUI 钩子：批量帧 + ACK 结果入审计日志
        {
            const char* err_code = "";
            if (!g_dryRun && (!ok || rlen != 8)) err_code = "timeout";
            gs::device::log_send_batch7(pts7, n, frame, idx, rx, rlen,
                                        response_ok, response_ok, err_code, attempt);
        }
        update_pose_from(pts7[std::min(n, size_t(7)) - 1]);
        gs::device::note_pose_updated();
        return true;
    }
    wprintln(L"[错误] 批量发送连续失败，放弃该批。");
    return false;
}

// —— 0x03 读坐标：读 0x0008×5（X/Y/Z/A/V），返回 X/Y/Z（mm）——
// 手册 6.1 读帧示例 01 03 00 08 00 05 04 0B；从机响应 [01,03,0A, Xh Xl Yh Yl Zh Zl Ah Al Vh Vl, CL CH] 共 15 字节。
// 坐标以 0.1mm 编码（mm_to_dev10 的逆），故读回 ÷10 还原成 mm。
bool SerialPort::readPose(float& x, float& y, float& z) {
    if (g_dryRun) return false;                       // 无设备可读
    if (h_ == INVALID_HANDLE_VALUE) return false;      // 未连接
    // 请求帧：从机 + 0x03 + 起始(0x0008) + 个数(0x0005) + CRC
    uint8_t req[8];
    req[0] = MB_SLAVE; req[1] = 0x03;
    req[2] = uint8_t(REG_START >> 8); req[3] = uint8_t(REG_START & 0xFF);
    req[4] = uint8_t(REG_COUNT >> 8); req[5] = uint8_t(REG_COUNT & 0xFF);
    uint16_t rc = crc16_modbus(req, 6);
    req[6] = uint8_t(rc & 0xFF); req[7] = uint8_t((rc >> 8) & 0xFF);

    PurgeComm(h_, PURGE_RXCLEAR);
    if (!write(req, 8)) return false;
    Sleep(10);

    uint8_t rx[15] = { 0 };
    if (!read_exact(rx, 15, 800)) return false;
    if (rx[0] != MB_SLAVE || rx[1] != 0x03 || rx[2] != 0x0A) return false;   // 从机/功能码/字节数校验
    uint16_t crc_calc = crc16_modbus(rx, 13);
    uint16_t crc_rx = uint16_t(rx[13]) | (uint16_t(rx[14]) << 8);
    if (crc_calc != crc_rx) return false;

    auto rd16 = [&](int off) -> int16_t { return int16_t((uint16_t(rx[off]) << 8) | rx[off + 1]); };
    x = rd16(3) / 10.0f;   // reg0 = X
    y = rd16(5) / 10.0f;   // reg1 = Y
    z = rd16(7) / 10.0f;   // reg2 = Z
    return true;
}
