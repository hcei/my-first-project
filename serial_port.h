// serial_port.h — 串口封装与 Modbus 帧发送（逐点 / 批量7点）
// 由 Robot.cpp（单文件版）拆分而来；行为保持不变。
// 含修复：Z 越界校验包含 Z_OFFSET；发送优先使用点自带速度档 p.speed。
#pragma once
#include "robot_common.h"

class SerialPort {
public:
    SerialPort() :h_(INVALID_HANDLE_VALUE) {}
    ~SerialPort() { close(); }

    bool open(const std::wstring& portName, DWORD baud = BAUDRATE, BYTE parity = EVENPARITY, BYTE bytesize = 8, BYTE stopbits = ONESTOPBIT);
    void close();

    bool write(const uint8_t* buf, size_t len);
    bool read_exact(uint8_t* buf, size_t need, DWORD timeout_ms);
    bool read(uint8_t* buf, size_t expectLen, size_t& outLen);

    bool sendPoint(const Point& p);
    bool sendPointRetry(const Point& p, int max_retries = 2, int backoff_ms = 25);

    // —— 批量7点 —— //
    static void packPoint5(const Point& p, int16_t(&out5)[5]);
    bool sendPointsBatch7(const std::vector<Point>& pts7, int max_retries = 3);

private:
    HANDLE h_;
};
