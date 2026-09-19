# 项目上下文

> 本文档维护项目级上下文信息，供 agent 在开始非简单任务前读取。
> 只保留当前有效信息，不写成无限追加日志。
> 文件过长时先压缩为摘要，再归档旧内容到 agent_memory/archive/ 。

## 项目概述
基于自行组装的三自由度 Delta 机械臂，通过 Modbus RTU 控制刚性 3D 打印笔架上的动物毛毛笔完成现场书法和配套中国画。

## 技术栈
Windows / C++17 / Modbus RTU over RS485 / nlohmann::json。
构建：MSYS2 MinGW-w64 g++ 静态链接（`build.bat` 一键产出两个 exe）；`Robot.vcxproj` 保留 MSVC 路径但本机未安装 MSVC 工具链，实际以 MinGW 为准。

## 目录结构
- 控制台程序：`main.cpp` + `robot_common` / `serial_port` / `motion` / `hanzi` / `polyline`
- GUI 程序：`gui_win32.cpp`（Win32 入口，不进控制台工程）+ `gui_service.cpp/h`（GUI 与控制台共用的服务层：状态快照、配置、串口、任务线程、审计）
- 交付物：`Robot.exe`（控制台）、`RobotGUI.exe`（GUI）
- 文档：`README.md`（简介）、`使用说明.md`（构建/运行/上机注意事项，面向使用者）、`GUI_DESIGN_SPEC.md` + `gui_design_mockup.html`（GUI 设计依据）
- 配置与数据：`robot_config.json`、`calib.json`（未接入）、`shanshui_gen.hpp`、`themes/`、`D:/objects/hanzi-writer-data`（字库，由 `HANZI_BASE_DIR` 指定）
- 运行时产物：`logs/Robot_*.log`（仅控制台写）、`logs/audit_*.jsonl`、`gui_window.txt`、`last_task.txt`（后三者已 gitignore）

## 仓库与分支约定
- `origin` = https://github.com/hcei/my-first-project.git （本仓库），`upstream` = https://github.com/GuhuaiDawn/my-first-project.git （原作者）。
- 功能开发走新分支再推送，不直接推 `main`；本次 GUI 工作分支：`feat/gui-win32-assistant`。
- `gh` CLI 已登录（账号 hcei，https 协议）。
- **推送需要走本机代理**：`git push` 直连 github.com 会 `Recv failure: Connection was reset`；本机浏览器代理为 `127.0.0.1:7897`（WinINET 里 ProxyEnable=1），但 git/curl 不会自动使用它，WinHTTP 显示“直接访问”。可用一次性参数推送，不必写进 git 配置：
  `git -c http.proxy=http://127.0.0.1:7897 push -u origin <branch>`

## 关键约定
优先复用现有 C++ 控制流程；不得把协议假设当成已上机验证事实；涉及硬件运动必须先空载、限位和急停验证。原 CR-3040/G-code 方案仅作为需求和流程参考，不直接继承其机械、电控或夹具设计。

## 书写节拍与真机告警（2026-09-18 沉淀）
- 节拍可运行期配置：`robot_config.json` 新增 5 键 `z_settle_ms / stroke_begin_ms / stroke_end_ms / cold_start_min_ms / min_point_interval_ms`（默认 120/70/90/150/12），由 `gs::cfg_load` 覆盖全局 `g_*`，改后重启即生效、无需重编；真机扫参就调这些。
- dryrun 归因：顿挫大头是每笔 Z 沉降+起收笔 dwell+冷启动（压这些省 ~20%），60ms 走停地板是次因（方案A 省 ~9%）；RDP 只对密采样/长直笔画减点，简单稀疏字不减点。
- 顿笔独立开关 `g_enableDunbi`（默认 true=保持现状，仿 `g_enableDip`）：持久化键 `enable_dunbi`、snapshot `cfg.enable_dunbi`、GUI 主页复选框（`IDC_CHECK_DUNBI`，idx5，画在蘸墨下方）、控制台菜单 21。关闭时：①`get_Z_SETTLE_MS/get_STROKE_BEGIN/END_DWELL_MS` 一律返回 0（含高质模式），首落笔"重压加固"块跳过（`isFirstDownThisCall` 增加 `&& g_enableDunbi`）→ 落笔过渡间隔从 ~202ms 降到 min_interval(12ms)；②`hanzi.cpp::zFor` 的点画 dot 由 `Z_DOWN_HEAVY` 改 `Z_DOWN_NORMAL`，与书写平面无关。**不动** medians 骨架几何、不删分层下刀(UP→MID→PRE→DOWN)、不删拐角 dwell。单元验证 verify_dunbi.exe 全 PASS。真机待验证：关闭后是否更顺滑、小字点画是否不再糊成墨点。
- ⚠️ 速度编码疑似反向（待真机确认）：手册 `0x0008` 的 V 高字节 0~9 中 **00 最快**，但 `serial_port.cpp` 发 `level-1`（level 越大数值越大=越慢），与 `speedLevelToXYmmPerSec`（level 越大越快）相反；上机前先定"档→mm/s"真实方向再决定是否反转。
- ⚠️ 描边/作画真机不安全：`auto_draw` 走 `0x0064` 批量，但手册明确 `0x0064` 是**固定抓放宏**（抓上/抓/抓上/放上/放/放上/等待），非任意轨迹；真机联调期保持 `auto_draw=false`，作画须先把描边改回逐点 `0x0008`。

## GUI 排版（字号/布局/方向）约定（2026-09-19 实现，勿重复）
- 排版 = **自动/手动双模式 + 书写方向**，入口在「书写任务」页左栏（不新开页）。默认自动＝沿用 `plan_text_area_and_layout` 旧搜索，行为零回归。
- 全局（`robot_common.{h,cpp}`，均随 `robot_config.json` 持久化、`gs::cfg_load` 覆盖）：`g_layout_mode`(0/1)、`g_lm_char_size`(≥60 上不封顶)、`g_lm_cols`(每线字数)、`g_lm_top_ratio`(0.10~0.95)、`g_lm_row_spacing`(行距,独立于 CHAR_SPACING)、`g_write_dir`(0横排左起/1竖排右起·列内上到下·列从右往左)。`TextPlan` 加 `err`(LayoutErr 码)/`row_spacing`/`dir`。
- hanzi：抽出方向感知共享函数 `grid_extents(n,per,S,along,cross,dir→gw/gh/nLines)` 与 `fill_offsets(...)`；`plan_manual_layout(...,dir)` 与 `plan_text_area_and_layout(...,dir)` 都走它们，`prepare_layout_only` 按 `g_layout_mode` 分流、传 `g_write_dir`。**关键约束**：fit 基准是 `g_safeArea`（设备安全区），不是 paper 盒（`paper.valid=false` 也能判）；字号下限 60 比赛红线；手动放不下**报 `err` 不自动缩放**；竖排右起是**宽高轴向交换**；自动模式竖排候选改 `{4,3,2,5}` 偏好短列（横排仍 `{5,4}`）。
- 服务层（`gui_service.{h,cpp}`）：`set_layout_mode/char_size/cols/top_ratio/row_spacing`、`set_write_dir`（均 `g_task_active` 拒绝+`cfg_save`）；`layout_preview(text)` 返回不写审计的实时预检明细 + `cells`（计划字块世界坐标 x/y/s/ch）供叠画；`preflight` 补 `mode/dir/row_spacing/err_code`；snapshot.cfg 暴露上述键。
- GUI（`gui_win32.cpp`）：书写页 `WLGeo` 共享几何放 4 输入框 + `g_chkManual`(手动) + `g_chkVert`(竖排) + 应用/恢复自动；`EN_CHANGE`→`ApplyLayoutFields`+`RefreshLayoutPreview`（`g_laySuppress` 防回环）；计划字块叠画进 `DrawTrailPanel`，**显示条件 `g_prevValid && !task_active && gs::trail::commandedSize()==0`**（一旦任务跑过有轨迹就不再叠画，避免与真实轨迹重叠）；`IDC_BTN_WRITE` 用 `g_prevValid` 做可行性闸门。

## 实时轨迹与四角标定约定（2026-09-19 实现）
- 模块 `gui_trail.{h,cpp}`（`gs::trail`）：存已下发轨迹 `Cpt{x,y,pen}`、实测点 `Apt{x,y}`、四角 `Corner[4]`；自带独立 `std::mutex`，**锁序恒 `g_mu → trail`**，内部绝不回调 `gs::`（避免反向死锁）。GUI 用 `epoch()` 判任务 reset、游标增量 `fetchCommanded/fetchActual` 取数。
- 轨迹**仅在 `g_task_active` 时记录**（挂在 `device::log_send_point`/`log_send_batch7` 钩子），回中心/心跳/测试点等快捷操作不进图。
- 实测点：`SerialPort::readPose(x,y,z)` 走 Modbus **0x03 读 0x0008×5**（回帧 15 字节，坐标 0.1mm 编码 ÷10）；**只能在任务线程调用**（半双工单总线 + 反馈延迟 ≤300ms，且 `SerialPort` 由单写者占用），现每字采一次。**读回是实际位置还是最后写值待上机确认**。
- 四角标定：用户依次输入四角**设备 mm 坐标**（与轨迹点同系），每角「预览」/「保存」；持久化到 `robot_config.json` 的 `"corners"`（数组 {x,y,valid}），下次开 GUI 沿用，「清除标定」复位。绘图视口优先按已下发轨迹、无轨迹有角→按角框、都无→`g_devLimit`；四角齐全时**按绕质心极角排序连线**成简单四边形（防输入顺序导致交叉）。
- `gs::preview_corner(x,y)`：抬笔移到该角，**真机会物理移动**（属硬件动作）——任务中拒绝、未连接拒绝、`inXYRange`+`inZRange` 越界拦截、DRYRUN 仅打帧；Z 用**已固定书写平面** `g_writing_plane_z`（valid 时）否则 `Z_UP`。`save_corner` 校验后写角+审计 `corner_save`+`cfg_save`。
- **可达边界（已撤销实验，回原版）**：曾把 `g_devLimit` 做成 config `"dev_limit"` 可配置并全局收紧到 X±162/Y±85，后经真机验证**已撤销回原版**——`g_devLimit` 恢复固定 ±180（无 config 键、无 snapshot 项），`sendPoint`/`preview_corner`/`save_corner` 按 ±180 校验。**探明的硬事实（保留）**：真机上电可达极限就是 **X±162 / Y±85**，且断电可手推越过 → 限制在**控制器/机构侧，非软件**（疑控制器工作区软限位，手册 ±180 但固件设小），软件无法扩，需找厂商放参数或改机械；超出该框的点控制器会静默夹到边界。**四角预览/保存/清除结果在轨迹面板标题行显示**（`SetResult` 顶部横幅只画在主页）——此 UI 保留。

## 审计日志约定（已实现）
- GUI 操作写 `logs/audit_<yyyyMMdd_HHmmss>.jsonl`：首行 `event=session`，随后每操作一行，`fflush` 逐条落盘。
- 字段：`source`（GUI/CONSOLE）、`actor`（HUMAN）、`mode`（DRYRUN/REAL/OFFLINE）、`operation`、`operation_id`、`request_id`、`session_id`、`parameters`、`result`、`error_code`；发送类事件另带 `tx_frame`/`rx_frame`/`device_response`/`ack_valid`/`retry_count`/`software_pose`。
- 事件类型：session / connect / disconnect / send_point / send_batch7 / task_preflight / task_start / task_abort / task_end / estop / estop_clear。

## 并发与锁约定（重要）
- `gs::` 内部单把 `std::recursive_mutex g_mu` 保护串口写、任务状态、审计文件；必须保持递归锁，因为 `session_id()`、`op_event()`、`audit_write()` 等会在已持锁路径被复用（改回 `std::mutex` 会让 `preflight` 死锁）。
- 任务线程 `detach`，用 `std::atomic` 的 `g_task_cancel` / `g_estop` 协作取消；`abort_task()` 经急停通道，抬笔后结束。

## 串口枚举与端口打开（重要安全约定）
- **枚举串口只许查询，不许打开**：`gs::list_serial_ports()` 仅用 `QueryDosDeviceW`（按 `ERROR_INSUFFICIENT_BUFFER` 扩容重试）+ 注册表 `HKLM\HARDWARE\DEVICEMAP\SERIALCOMM`。禁止用 `CreateFileW` 逐个探测——本机 6 个注册串口全是蓝牙虚拟串口（`\Device\BthModemN`），打开未连接的蓝牙口会在内核里阻塞（实测 COM4 5.2s、COM5 无限），而该函数由 UI 线程调用。
- 本机环境：DOS 设备名列表约 47349 字符，4096 WCHAR 缓冲不够；串口为 COM3/4/5/6/8/9（全部 BthModem）。
- 打开串口属同类风险：`gs::connect()` 已改为异步 `gs::connect_async()`——把 `SerialPort::open()`（蓝牙虚拟口会在 `CreateFileW` 内核无限阻塞）放到 detach 工作线程，**打开期间不持 `g_mu`**，结果经 `snapshot()["connect"]`（`pending`/`state`/`message`）由 UI `WM_TIMER` 轮询回报。**规则：任何串口 open 都不得在 UI 线程、也不得在持 `g_mu` 时进行**，否则一并冻结界面与 500ms `snapshot()`。

## 跨进程驱动 GUI 的安全边界（调试经验）
- 可用：`PostMessage(hwnd, WM_COMMAND, id, 0)` —— 已用于预检 1009 / 开始书写 1013 / 停止 1014 / 急停 1011。
- 禁用：跨进程 `SendMessage` 传缓冲区（WM_SETTEXT / WM_GETTEXT）—— 会挂死 GUI 主线程。
- 不安全：跨进程 `PostMessage WM_CHAR` —— 同样导致 GUI 未响应。
- 切页用 DPI-aware 的真实点击（`SetCursorPos` + `mouse_event`）稳定可靠；`PostMessage WM_LBUTTONDOWN` 客户区坐标点击会时灵时不灵，不要作为切页依据（2026-09-18 复核）。
- 文本输入的正路：预先把文本写入 `last_task.txt`，GUI 创建书写页时经 `gs::load_last_task_text()` 预载进输入框。
- 截图核验：本机 `CopyFromScreen` 会返回冻结帧（多次不同页面截出同一 md5），`PrintWindow` 单独用也会拿到旧帧。可靠做法是先 `RedrawWindow(h, 0, 0, RDW_INVALIDATE|RDW_UPDATENOW|RDW_ALLCHILDREN|RDW_FRAME)` + `UpdateWindow`，再 `PrintWindow(h, hdc, 0)`；每次截图后用 md5 比对确认内容确实变化，别用重复帧当核验证据。
- 判定 GUI 是否卡死：`SendMessageTimeoutW(hwnd, WM_NULL, 0, 0, SMTO_ABORTIFHUNG, 3000, &r)`——返回 0 即无响应；配合 `Get-Process` 看 CPU 增量区分“忙等”（CPU 高）与“阻塞”（CPU 0）。注意 `IsHungAppWindow` 偏粗，启动期会误报。
- 排查阻塞位置：gdb attach 在本机被拒（拒绝访问），改用**临时文件探针**（在各阶段 `fopen("_trace.log","a")+fprintf+fclose` 打点），复现后看最后一条打点；定位完必须删除探针。
- 串口下拉框（CBS_DROPDOWNLIST）不选中时显示为空是正常表现，不代表没枚举到；用 `CB_GETCOUNT`/`CB_GETLBTEXT` 跨进程查询实际条目（`CB_GETCOUNT` 无缓冲区，跨进程发送安全）。

## 运行时状态文件的路径约定（须从项目目录启动）
- `gui_window.txt`、`last_task.txt`、`robot_config.json`、`logs/` 全部是相对进程 CWD 的路径：从别的目录启动 `RobotGUI.exe` 会读不到上次任务与配置，审计日志也会写到别处（详见 bugs.md）。当前约定：**在项目根目录启动 GUI/控制台**。

## 已知约束
比赛要求动物毛毛笔；正文每字至少 6x6 cm；按规范笔顺；书法和国画均需自主蘸墨至少一次，且蘸笔、提笔、沾墨动作要明显；一台机器人连接一台电脑；正式自动执行 20 分钟，期间不得远程或人工干预；场地桌面 1.5x1.2 m；文字现场输入且为常用汉字；纸张可自定但不能超出桌面。