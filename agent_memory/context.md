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
- 配置与数据：`robot_config.json`、`calib.json`（未接入）、`shanshui_gen.hpp`、`themes/`、`D:/objects/hanzi-writer-data`（字库，由 `HANZI_BASE_DIR` 指定）
- 运行时产物：`logs/Robot_*.log`、`logs/audit_*.jsonl`、`gui_window.txt`、`last_task.txt`（后三者已 gitignore）

## 关键约定
优先复用现有 C++ 控制流程；不得把协议假设当成已上机验证事实；涉及硬件运动必须先空载、限位和急停验证。原 CR-3040/G-code 方案仅作为需求和流程参考，不直接继承其机械、电控或夹具设计。

## 审计日志约定（已实现）
- GUI 操作写 `logs/audit_<yyyyMMdd_HHmmss>.jsonl`：首行 `event=session`，随后每操作一行，`fflush` 逐条落盘。
- 字段：`source`（GUI/CONSOLE）、`actor`（HUMAN）、`mode`（DRYRUN/REAL/OFFLINE）、`operation`、`operation_id`、`request_id`、`session_id`、`parameters`、`result`、`error_code`；发送类事件另带 `tx_frame`/`rx_frame`/`device_response`/`ack_valid`/`retry_count`/`software_pose`。
- 事件类型：session / connect / disconnect / send_point / send_batch7 / task_preflight / task_start / task_abort / task_end / estop / estop_clear。

## 并发与锁约定（重要）
- `gs::` 内部单把 `std::recursive_mutex g_mu` 保护串口写、任务状态、审计文件；必须保持递归锁，因为 `session_id()`、`op_event()`、`audit_write()` 等会在已持锁路径被复用（改回 `std::mutex` 会让 `preflight` 死锁）。
- 任务线程 `detach`，用 `std::atomic` 的 `g_task_cancel` / `g_estop` 协作取消；`abort_task()` 经急停通道，抬笔后结束。

## 跨进程驱动 GUI 的安全边界（调试经验）
- 可用：`PostMessage(hwnd, WM_COMMAND, id, 0)` —— 已用于预检 1009 / 开始书写 1013 / 停止 1014 / 急停 1011。
- 禁用：跨进程 `SendMessage` 传缓冲区（WM_SETTEXT / WM_GETTEXT）—— 会挂死 GUI 主线程。
- 不安全：跨进程 `PostMessage WM_CHAR` —— 同样导致 GUI 未响应。
- 切页必须用 DPI-aware 的真实点击（GUI 以 150% DPI 运行，非 aware 进程坐标被虚拟化，投递 WM_LBUTTONDOWN 无效）。
- 文本输入的正路：预先把文本写入 `last_task.txt`，GUI 创建书写页时经 `gs::load_last_task_text()` 预载进输入框。

## 已知约束
比赛要求动物毛毛笔；正文每字至少 6x6 cm；按规范笔顺；书法和国画均需自主蘸墨至少一次，且蘸笔、提笔、沾墨动作要明显；一台机器人连接一台电脑；正式自动执行 20 分钟，期间不得远程或人工干预；场地桌面 1.5x1.2 m；文字现场输入且为常用汉字；纸张可自定但不能超出桌面。