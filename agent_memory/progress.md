# 任务进度

> 本文档维护当前任务进度，供 agent 在开始非简单任务前读取。
> 只保留当前有效信息，不写成无限追加日志。
> 出现阶段完成、方向变化、失败尝试、准备结束会话时，必须更新本文件。

## 当前任务
Win32 原生版“书画机械臂调试助手”已实现并完成操作核验；下一步是收尾（提交）与真机验证准备。

## 成功标准
`gui_win32.cpp` 三页（主页/设备连接/书写任务）可交互；所有 GUI 操作经 `gs::` 服务层写入结构化 JSONL 审计（`source=GUI, actor=HUMAN`）；与控制台程序共用配置、运动安全校验和日志链路；未接入硬件状态显示为“未接入/协议待确认”。

## 范围边界
本轮只做 GUI 与审计链路，不改控制器协议、不改运动学；真机运动参数（Z 深度、批量 7 点、蘸墨）仍需上机验证，不得据 DRYRUN 结果视为已确认。

## 停止条件
<!-- 什么情况下应暂停并汇报 -->

## 已完成
### GUI 实现（gui_win32.cpp / gui_service.cpp / gui_service.h）
- 三页 Win32 GUI：主页（只读状态快照 + 快捷操作 + 运行开关）、设备连接（串口枚举/连接/断开/刷新）、书写任务（文本输入 + 预检 + 开始/停止 + 进度）；WM_PAINT GDI owner-draw，紫色标题栏 + 左侧导航 + 底部状态栏，与 `gui_design_mockup.html` 视觉一致。
- 服务层 `gs::`：统一状态快照 `snapshot()`、配置读写 `robot_config.json`（与控制台共用）、串口连接、写字任务线程（detach + 原子取消）、急停 latching 标志、结构化 JSONL 审计（会话头 + 每事件 flush）。
- 审计事件覆盖：session / connect / disconnect / send_point / send_batch7 / task_preflight / task_start / task_abort / task_end / estop / estop_clear，全部带 `source`、`actor`、`mode`、`operation_id`、`request_id`。
- 构建：新增 `build.bat`（MSYS2 MinGW g++，静态链接）一次产出 `Robot.exe` 与 `RobotGUI.exe`；`Robot.vcxproj` 已加入 `gui_service.cpp/h`（`gui_win32.cpp` 作为 GUI 独立入口，不进控制台工程）。
- 运行时状态文件 `gui_window.txt`（窗口尺寸）、`last_task.txt`（上次任务文本）已加入 `.gitignore`。

### 操作核验（全部通过）
- 预检：写入 `last_task.txt` → 启动 GUI → 切页预载文本 → PostMessage WM_COMMAND 1009 → 审计出现 `task_preflight result=ok`，界面显示“排版可行：1 字，1 列 × 1 行，字号 60.0 mm，字间距 1.0 mm，速度 3 档”。
- 开始书写（DRYRUN）：WM_COMMAND 1013 → 审计出现 `task_start` 并持续增长 `send_point` / `send_batch7`，界面进度条与阶段（预热→书写→描边）实时刷新。
- 停止任务：WM_COMMAND 1014 → 审计出现 `task_abort` + `task_end result=canceled error_code=estop`，界面回到空闲、按钮重新可用。
- 控制台回归：同一批源码重建的 `Robot.exe` 与拆分前基线 `_robot_base.exe` 对比，DRYRUN TX/RX 帧前 1736 条逐帧一致。

### 已修复缺陷（本轮）
- **`gs::preflight` 递归加锁导致 GUI 主线程死锁**（详见 bugs.md）：`preflight` 先持有 `g_mu` 再调用同样加锁的 `session_id()`；MinGW pthread 下 std::mutex 非递归，主线程永久阻塞，表现为窗口“未响应”。已把 `g_mu` 改为 `std::recursive_mutex`（31 处 `lock_guard` 同步替换）。
- 排查中确认的注入链路结论：跨进程 `SendMessage` 传缓冲区（WM_SETTEXT / WM_GETTEXT）会挂死 GUI 主线程，**禁止使用**；跨进程 `PostMessage WM_COMMAND` 安全可用；跨进程 `PostMessage WM_CHAR` 不安全（同样导致未响应）；非 DPI-aware 进程投递 WM_LBUTTONDOWN 无法切页，须用 DPI-aware 真实点击。

## 待办
- 接入 `calib.json` 或设计带验证状态的 measured safe area，并统一作为运动硬拦截边界。
- 设计急停锁存/人工复位语义；确认控制器状态、报警、限位、当前位置寄存器。
- 解决 ACK 丢失后的重放风险，确认命令幂等/序号/查询语义；批量 7 点协议需真机验证。
- Hanzi/Polyline JSON 字段类型/有限性/文件大小/点数限制与容量评估。
- 新增 `robot_cli.exe` 非交互命令入口，复用同一服务层与审计链路（当前 `main.cpp` 仍是菜单式交互）。
- 上机验证 Z 深度、方向、速度、批量、蘸墨和急停；当前 `Z_DOWN_HEAVY=-388` 仍超出已确认 -385 的实测点。
- `logs/` 与 `last_task.txt` 会持续增长，后续可加清理/轮转策略。

## 阶段变更记录
- 2026-09-16：完成比赛/协议/设计方案核对及第一轮源码修复。
- 2026-09-17：完成 Terra 第二轮审查；修复错误传播、预定位估时、输入和调试几何校验；建立 CodeGraph 和 GUI 状态边界。
- 2026-09-17：完成编译、dryrun、CodeGraph 同步和差异检查。
- 2026-09-17：根据用户确认收敛为前三页静态 HTML 原型，补齐设计文档并完成差异检查。
- 2026-09-17：完成 Win32 GUI 实现（gui_service/gui_win32）、修复 preflight 递归锁死锁、完成 GUI 操作核验与控制台帧级回归，新增 build.bat。

## 验证说明
- 已验证：GUI 预检/开始书写/停止三条操作路径的审计事件落盘正确；控制台 DRYRUN 帧序列与拆分前基线逐帧一致；`build.bat` 一次产出两个 exe 并冒烟通过。
- 已验证：`RobotGUI.exe --dryrun` 冒烟（控制台 `Robot.exe --dryrun` 菜单 4 正常退出码 0）。
- 已知环境限制：`Robot.exe` 交互式写字需约 8 分钟（单字 3749 点 DRYRUN），本轮以 5 分钟窗口截取前段对比；控制台 stdin 重定向在 EOF 后不退出，需显式输入菜单 4。
- 未验证：真实硬件（无设备接入），所有运动均为 DRYRUN 模拟帧；GUI 未在 100% 缩放与高 DPI 之外的分辨率下核验布局。