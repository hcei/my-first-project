// motion.h — 运动轨迹处理与发送（去重/降采样/节拍估算/预热/蘸墨/流式书写）
// 由 Robot.cpp（单文件版）拆分而来；行为保持不变。
#pragma once
#include "robot_common.h"

class SerialPort; // 前置声明

// 重复点过滤。srcIndex 非空时输出各保留点在原轨迹中的下标（供分界映射）；
// calli_end_src 为原轨迹中书法段边界：原下标 < calli_end_src 的点用书法细间距去重。
std::vector<Point> filterDuplicatePoints(const std::vector<Point>& in,
                                         std::vector<size_t>* srcIndex = nullptr,
                                         size_t calli_end_src = 0);
void resamplePolyline(std::vector<Point>& pts, float minStep);

// —— 节奏估时 —— //
float speedLevelToXYmmPerSec(int level);
int get_Z_SETTLE_MS(bool isCalli);
int get_STROKE_BEGIN_DWELL_MS(bool isCalli);
int get_STROKE_END_DWELL_MS(bool isCalli);
int get_POINT_RATE_LIMIT_MS(bool isCalli);
int estimateMoveMs(const Point& prev, const Point& cur, bool isCalli);

// 角度
float angle_between(const Point& a, const Point& b, const Point& c);

// 串口预热（更新位姿）
void serial_pre_warm(SerialPort& sp);

// 发送轨迹：书法逐点 + 描边批量 + 首落笔加固 + 冷启动强节流
bool transmitTrajectoryWithSplit(SerialPort& sp, const std::vector<Point>& traj, size_t calli_end);

// 蘸墨流程（两次蘸→十字抖→离液面“上慢下快”5次）
bool do_dip_and_groom(SerialPort& sp, const InkStation& ink);

// ★抬笔预定位→等待（首字/每行首字用）
bool move_up_to_and_wait(SerialPort& sp, const Point& targetUp, bool isCalli);

// 逐字流式写字（预热；可选蘸墨；首字/每行首字预定位）
bool write_text_streamed(SerialPort& sp, const std::wstring& chars, const TextPlan& plan, int baseSpeed, int per_char_pause_ms = 120);
