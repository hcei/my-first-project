// gui_service.h — GUI/CLI 共用控制服务层
// 对应 GUI_DESIGN_SPEC.md 第 6 节实现顺序 1~3 步：
//   1) DeviceStateSnapshot（snapshot）/ OperationEvent（device 钩子）/ AuditLogEvent（JSONL）
//   2) 串口 TX/RX、ACK 校验、软件位姿更新接入结构化日志
//   3) 把控制台菜单动作包装成不依赖 stdin/stdout 的可复用服务函数
// 行为边界：
//   - 本层不向 stdout 新增任何输出（保证控制台入口回归输出不变）；
//     cfg_load/cfg_save 保留原控制台提示语。
//   - 任务运行期间拒绝新的运动请求（单一串口写线程原则）。
//   - 未接入的硬件状态（真实位置/报警/限位/电池/存储）只标“未接入”，
//     不得用软件位姿或 ACK 推断（SPEC §4）。
#pragma once
#include "robot_common.h"
#include "serial_port.h"
#include "motion.h"
#include "hanzi.h"
#include "polyline.h"
#include "nlohmann/json.hpp"
#include <string>
#include <vector>

namespace gs {

using json = nlohmann::json;

// ---------------- 会话与调试来源（SPEC §5：GUI=HUMAN，CLI=AGENT） ----------------
void set_source(const std::string& source, const std::string& actor);
std::string session_id();

// ---------------- 结构化审计日志（logs/audit_时间.jsonl） ----------------
void audit_write(const json& event);
std::string jsonl_path();

// ---------------- 底层设备事件（serial_port.cpp 钩子调用；不打印） ----------------
namespace device {
void log_send_point(const Point& p, const uint8_t* tx, size_t txlen,
                    const uint8_t* rx, size_t rxlen,
                    bool ack_valid, bool ok, const char* error_code);
void log_send_batch7(const std::vector<Point>& pts, size_t n,
                     const uint8_t* tx, size_t txlen,
                     const uint8_t* rx, size_t rxlen,
                     bool ack_valid, bool ok, const char* error_code, int attempt);
void note_pose_updated();          // 直接 update_pose_from 的位置补充位姿时间戳
void set_attempt(int attempt);     // sendPointRetry 传入当前重试序号
std::string pose_ts();
json last_comm();
} // namespace device

// ---------------- 任务进度钩子（motion.cpp 调用；仅内存计数，不打印） ----------------
namespace task {
void begin(const std::wstring& chars, const TextPlan& plan, const char* kind);
void stage(const char* label);      // 预热/蘸墨/书写/描边/空闲
void char_done(int idx);
void traj_add_total(size_t n);
void traj_add_done(size_t n, bool pen_down);
void dip_done();                    // 蘸墨流程成功完成一次（比赛合规提示）
void end(bool ok, const char* state, const char* error);   // state: done/failed/canceled
void reset();
} // namespace task

// ---------------- 状态快照（SPEC §4；未接入字段固定“未接入”） ----------------
json snapshot();

// ---------------- 串口连接 ----------------
std::vector<std::string> list_serial_ports();
// 异步连接：把可能无限阻塞的串口 open（蓝牙虚拟口在 CreateFileW 内核阻塞）放到工作线程，
// 打开期间不持有 g_mu，UI 线程立即返回；结果经 snapshot()["connect"] 回报。
void connect_async(const std::string& port_name);
bool connect_pending();             // 是否仍有连接建立中
void disconnect();
bool is_connected();
SerialPort& port();                 // 全进程唯一串口实例（控制台与 GUI 共用）

// ---------------- 快捷操作 ----------------
std::wstring query_pose_line();     // 最后已知软件位姿（不发送）
bool go_center(std::string& err);   // 复位到中心（= 控制台菜单3）
bool send_test_point(std::string& err);
bool heartbeat(std::string& err);   // 重发抬笔心跳点
void request_estop();               // 置急停（软件锁存）；无任务时后台补一次抬笔
void clear_estop();
bool abort_task();                  // 请求停止当前任务（经急停通道）

// ---------------- 写字任务（对应控制台菜单1：写字 + 可选自动描边） ----------------
json preflight(const std::string& utf8_text);
bool start_write(const std::string& utf8_text, std::string& err);
bool task_active();
json task_info();

// ---------------- 配置（改动即保存 robot_config.json） ----------------
bool set_speed(int v);
bool set_char_spacing(float v);
bool set_z_offset(float v);
// —— 手动排版（旧网格模式；保留供控制台/回归，GUI 书写页已改用下方自由布局）—— //
bool set_layout_mode(int mode);            // 0=自动, 1=手动
bool set_layout_char_size(float mm);       // 字号 mm（≥60）
bool set_layout_cols(int cols);            // 每行字数
bool set_layout_top_ratio(float ratio);    // 上区占比 0.10~0.95
bool set_layout_row_spacing(float mm);     // 行间距 mm
bool set_write_dir(int dir);               // 书写方向 0=横排左起, 1=竖排右起
json layout_preview(const std::string& utf8_text);  // 实时预检明细 + 叠画计划字块（世界坐标）

// —— 自由拖拽排版（GUI 书写页 2026-09-19 重构）—— //
// 网格仅作“初始摆位”，用户可在固定视野（四角标定框，未标定回退可达 X±162/Y±85）内
// 逐字拖拽定位；拖拽结果按文本绑定持久化，书写时所见即所得。
// 视野矩形（世界 mm，x0<x1、y0<y1）：优先四角外接框，否则可达回退框。
void view_bounds(float& x0, float& y0, float& x1, float& y1);
bool set_free_char_size(float mm);         // 设定全局字号（≥60）→ 重算初始网格摆位（复位拖拽）
bool set_free_cell(int idx, float x, float y);  // 拖拽更新第 idx 字左下角世界坐标（夹取在视野内）
bool set_glyph_orient(int o);              // 字体朝向 0=沿y向下(0°)/1=沿y向上(180°)/2=沿x向上(90°CW)/3=沿x向下(90°CCW)
int  glyph_orient();                       // 读取当前字体朝向
void reset_canvas();                       // 清空实时轨迹缓冲，使可编辑字块叠画重新出现
bool preview_writing_plane(float z, std::string& err);  // 移到 (0,0,z) 悬停，确认书写高度（不书写）
bool set_writing_plane(float z, std::string& err);      // 保存书写平面 Z：生效 + 持久化（后续书写统一用该 Z）
// —— 四角标定（实时轨迹面板）：预览=抬笔移到该角(真机移动)；保存=固定并持久化；清除=复位 —— //
bool preview_corner(float x, float y, std::string& err);
bool save_corner(int i, float x, float y, std::string& err);
void clear_corners();
void toggle_auto_draw();
void toggle_high_quality();
void toggle_enable_dip();
void toggle_enable_dunbi();
bool set_dry_run(bool on);          // 任务运行中返回 false
void set_log_enable(bool on);

// ---------------- 配置持久化（控制台与 GUI 共用同一 robot_config.json） ----------------
bool cfg_save();
void cfg_load();
std::string make_autogen_path();    // themes/auto_shanshui_时间.json
std::string latest_autogen_json(const std::string& dir = "themes");
bool save_last_task_text(const std::string& utf8_text);
std::string load_last_task_text();

// ---------------- GUI 窗口尺寸记忆 ----------------
bool load_window_size(int& w, int& h);
void set_window_size(int w, int h);

// ---------------- 退出清理（GUI 退出前调用；控制台可选） ----------------
void shutdown();

// ---------------- 编码工具（GUI 侧 UTF-8；控制台侧仍用 GBK 的 w2gbk/mb2w） ----------------
std::wstring utf8_to_w(const std::string& s);
std::string w_to_utf8(const std::wstring& ws);

} // namespace gs
