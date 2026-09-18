// gui_trail.h — 实时轨迹调试：已下发轨迹（软件位姿）+ 真机 0x03 实测点 + 纸张边界
// 供 GUI 书写页“实时轨迹”面板使用。
//   - 写入方：gui_service 在发送钩子（log_send_point / log_send_batch7）内调用，仅任务运行时记录；
//     实测点由任务线程 readPose 采样后调用 addActual（单写者，绝不在 UI 线程读串口）。
//   - 读取方：GUI 在 UI 线程按 epoch 判断是否全量重置、否则用游标增量取数。
// 并发约定：本模块自带独立 mutex，锁序恒为 gs::g_mu → trail（addCommanded* 在 g_mu 下持本锁），
//   本模块内部绝不回调任何 gs:: 函数，避免与 g_mu 形成反向锁序而死锁。
#pragma once
#include "robot_common.h"
#include "nlohmann/json.hpp"
#include <cstdint>
#include <vector>

namespace gs { namespace trail {

struct Cpt { float x{ 0 }, y{ 0 }; uint8_t pen{ 0 }; };   // 已下发轨迹点；pen=1 落笔、0 抬笔移动
struct Apt { float x{ 0 }, y{ 0 }; };                      // 真机 0x03 实测采样点（mm）

struct PaperBox {
    float cx{ 0 }, cy{ 0 };   // 纸张中心（默认 = 底盘中心点，mm）
    float w{ 0 },  h{ 0 };    // 纸张宽 × 高（mm）
    float dx{ 0 }, dy{ 0 };   // 微调偏移（mm）
    bool  valid{ false };     // 是否已设定（false 时绘图按数据外接框自动缩放）
    float xmin() const { return cx + dx - w / 2.0f; }
    float xmax() const { return cx + dx + w / 2.0f; }
    float ymin() const { return cy + dy - h / 2.0f; }
    float ymax() const { return cy + dy + h / 2.0f; }
};

// —— 任务生命周期 ——
void reset();                 // 清空轨迹并 epoch++（UI 据此把游标重置为全量）
uint64_t epoch();             // 复位次数

// —— 写入（gui_service 发送钩子调用，仅任务运行期）——
void addCommanded(const Point& p);
void addCommandedBatch(const std::vector<Point>& pts, size_t n);
void addActual(float x, float y);

// —— 读取（UI 线程增量）——
// since 为上次返回的总量游标；把 [since, 当前) 的新点追加进 out，返回新的总量游标。
uint64_t fetchCommanded(uint64_t since, std::vector<Cpt>& out);
uint64_t fetchActual(uint64_t since, std::vector<Apt>& out);
bool     truncated();         // 是否因超过上限丢弃了后续下发点
size_t   commandedSize();
size_t   actualSize();
void     bounds(float& minx, float& miny, float& maxx, float& maxy);   // 下发点外接框；无数据回退设备极限

// —— 纸张边界（持久化经 gs::cfg_save/cfg_load 的 "paper" 子对象）——
PaperBox paper();
void     setPaper(const PaperBox& pb);              // 仅更新内存与 valid；持久化由调用方另调 gs::cfg_save()
nlohmann::json paperToJson();                        // 供 cfg_save
void           paperFromJson(const nlohmann::json& j);   // 供 cfg_load

}} // namespace gs::trail
