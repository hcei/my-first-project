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
#include <functional>

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
void page_begin(int page_idx0, int page_total, int chars_in_page);  // 分页任务：每页开始，页内字符/轨迹计数复位
void stage(const char* label);      // 预热/蘸墨/书写/翻页/描边/空闲
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

// —— 自由拖拽排版（GUI 书写页 2026-09-19 重构；2026-09-20 升级为分页）—— //
// 网格仅作“初始摆位”，用户可在固定视野（四角标定框，未标定回退可达 X±162/Y±85）内
// 逐字拖拽定位；拖拽结果按“每页一份”持久化，书写时所见即所得。
// 视野矩形（世界 mm，x0<x1、y0<y1）：优先四角外接框，否则可达回退框。
// 分页模型：有效文本 = 输入前 min(总字数, 每页字数×总页数) 字（超出截断不参与书写），
// 按每页字数顺序切页；每页拥有独立字块坐标（同一世界坐标系，翻页走纸量=版面高、坐标重合）。
struct PageEntry {
    std::wstring          text;    // 该页有效汉字序列
    std::vector<Offset>   cells;   // 每字左下角世界坐标，size 与 text 一致
};
void view_bounds(float& x0, float& y0, float& x1, float& y1);
bool set_free_char_size(float mm);         // 设定全局字号（≥60）；仅给缺坐标的页补初始网格
bool set_free_cell(int idx, float x, float y);  // 拖拽更新【当前编辑页】第 idx 字左下角（夹取在视野内）
bool set_glyph_orient(int o);              // 字体朝向 0=沿y向下(0°)/1=沿y向上(180°)/2=沿x向上(90°CW)/3=沿x向下(90°CCW)
int  glyph_orient();                       // 读取当前字体朝向
void reset_canvas();                       // 清空实时轨迹缓冲，使可编辑字块叠画重新出现
// —— 分页参数与页游标 —— //
bool set_page_chars(int n);                // 每页字数 1~50 → 重新切页（文本未变的页保留拖拽坐标）
int  page_chars();
bool set_page_count(int n);                // 总页数 1~20 → 重新切页
int  page_count();
bool set_edit_page(int idx0);              // 切换编辑页（任务运行中拒绝）
int  edit_page();
int  page_entry_count();                   // 当前分页结果页数（== page_count，无文本时为 0）
bool page_entry(int idx0, PageEntry& out); // 取某页文本+字块（世界坐标）
void get_page_cells(int idx0, std::vector<Offset>& out);
void text_capacity(int& total_chars, int& written_chars);  // 过滤后总字数 / 截断后实际书写字数
int  display_page();                       // 任务中=书写页；空闲=编辑页（GUI 画布/翻页条显示依据）
bool page_drag_enabled();                  // 空闲 + 显示页==编辑页 + 无历史轨迹 → 允许拖拽
// —— 翻页预留接口（蓝牙模块暂不接入）—— //
// 每写完一页：抬笔 → 调用该回调（入参=即将书写的页码 1-based 与总页数，返回 false=翻页失败）
// → 清空实时轨迹 → 写下一页。默认实现为模拟：等待 page_turn_wait_ms 后返回成功。
using PageTurnFn = std::function<bool(int page_no_1based, int page_total)>;
void set_page_turn_handler(PageTurnFn fn); // 注入真实蓝牙翻页实现（传 nullptr 恢复默认模拟）
bool set_page_turn_wait_ms(int ms);        // 模拟等待时长 500~60000ms，持久化
int  page_turn_wait_ms();

// —— 蓝牙翻页（2026-09-21 接入，见 ble_motor.h）—— //
// 真信号 = 板子回 DONE（闭合环，不是定时器猜）。链路 = BLE GATT 透传（FFE0/FFE1）。
// 三个参数都做成 GUI 可调并持久化；关闭蓝牙翻页时自动回退到上面的模拟等待。
//
// ★翻页档位上限 PAGE_TURN_GEAR_MAX：必须与下位机固件 Hardware/Motor.h 的
//   MOTOR_GEAR_MAX 保持一致，改一处就得同步改另一处。
//   2026-09-23：执行器电机由 130 换成 TT 减速电机(1:48)，档位总数 50 -> 20。
constexpr int PAGE_TURN_GEAR_MAX = 20;
void page_turn_install();                  // 注册翻页回调（Run() 启动时调一次即可）
void page_turn_shutdown();                 // 退出前优雅收尾（断开 GATT + 停工作线程）
bool set_page_turn_ble(bool on);           // 启用/停用蓝牙翻页（持久化）
bool page_turn_ble();
bool set_page_turn_gear(int gear);         // 档位 0~PAGE_TURN_GEAR_MAX（持久化）
int  page_turn_gear();
bool set_page_turn_run_ms(int ms);         // 每次转动时长 100~600000ms（持久化）
int  page_turn_run_ms();
bool set_ble_address(const std::string& addr12);  // 12 位十六进制（可带冒号）
std::string ble_address();
// —— 蓝牙状态 / 手动连接（GUI 用）—— //
std::string ble_status_line();             // 例：已连接 / 未连接 / 异常：服务被其他程序占用
bool ble_connect(std::string& err);        // 发起连接（非阻塞，返回是否已受理）
void ble_disconnect();
bool ble_available();                      // 本机 WinRT 蓝牙是否可用
bool set_point_fixed_ms(int ms);           // 直线段(横/竖)每点固定开销 C(ms) 0~300；自动配平 z_settle
int  point_fixed_ms();
bool set_point_fixed_curve_ms(int ms);     // 曲线段(撇/捺/弯钩)每点固定开销 C(ms) 0~300
int  point_fixed_curve_ms();
bool set_rdp_tol_mm(float mm);             // 直线段 RDP 抽稀容差(mm) 0.02~3.0
float rdp_tol_mm();
bool set_rdp_tol_curve_mm(float mm);       // 曲线段 RDP 抽稀容差(mm) 0.02~3.0
float rdp_tol_curve_mm();
bool set_z_settle_ms(int ms);              // Z 过渡沉降时间(ms) 0~3000；直设不调 sync_z_settle，但改任一 C 框会被重配平为 107-min(C直,C弯)
int  z_settle_ms();
bool set_stroke_begin_ms(int ms);          // 笔画起点(落笔)额外驻留(ms) 0~3000；直设、不进 sync_z_settle 配平（与 R5 去掉的伪 80ms 同位，供"封口/粘连"双赢调参）
int  stroke_begin_ms();
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
