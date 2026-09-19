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

// 四角标定：用户依次输入纸面四角的设备坐标（mm，与轨迹点同一坐标系、Y+ 同向）。
// 每角可先“预览”（机械臂抬笔移到该点）再“保存”（固定下来，随配置持久化）。
struct Corner { float x{ 0 }, y{ 0 }; bool valid{ false }; };
enum { kCorners = 4 };

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

// —— 四角标定（持久化经 gs::cfg_save/cfg_load 的 "corners" 数组）——
void   setCorner(int i, float x, float y);        // 置第 i 个角为有效（越界索引/非有限值忽略）
void   clearCorner(int i);
void   clearCorners();
Corner corner(int i);                              // i 0..3；越界返回 invalid
int    cornerCount();                             // 已保存（有效）角数
bool   cornersBounds(float& minx, float& miny, float& maxx, float& maxy);  // 有效角外接框；有则 true
void   getCorners(Corner out[kCorners]);          // 供绘制读取
nlohmann::json calibToJson();                      // 供 cfg_save（写 "corners"）
void           calibFromJson(const nlohmann::json& j);   // 供 cfg_load

}} // namespace gs::trail
