# 任务进度

> 本文档维护当前任务进度，供 agent 在开始非简单任务前读取。
> 只保留当前有效信息，不写成无限追加日志。
> 出现阶段完成、方向变化、失败尝试、准备结束会话时，必须更新本文件。

## 当前任务
Win32 原生版“书画机械臂调试助手”已实现；UI 布局缺陷与“切页卡死”均已修复并核验，使用说明已补齐，代码已推送分支 `feat/gui-win32-assistant`。下一步是合并/评审与真机验证准备。

## 成功标准
`gui_win32.cpp` 三页（主页/设备连接/书写任务）可交互且版面正确（无字段溢出、无文字截断、无图形压字）；所有 GUI 操作经 `gs::` 服务层写入结构化 JSONL 审计（`source=GUI, actor=HUMAN`）；与控制台程序共用配置、运动安全校验和日志链路；未接入硬件状态显示为“未接入/协议待确认”。

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
- 排查中确认的注入链路结论：跨进程 `SendMessage` 传缓冲区（WM_SETTEXT / WM_GETTEXT）会挂死 GUI 主线程，**禁止使用**；跨进程 `PostMessage WM_COMMAND` 安全可用；跨进程 `PostMessage WM_CHAR` 不安全（同样导致未响应）；切页须用 DPI-aware 真实点击，`PostMessage WM_LBUTTONDOWN` 不可靠。

### UI 布局缺陷修复（2026-09-18，全部只改 `gui_win32.cpp`）
- 主页“设备信息”网格下标写反（`col=i/2, row=i%2`），实际排成 7 列 × 2 行，第 3 列起溢出面板并被右栏面板覆盖——这是“UI 错位”的主因，15 个字段原先只有 4 个可见。已改为 `col=i%2, row=i/2`。
- 一批固定文本框宽度不足造成的截断：面板标题“输入与任务预检”、右栏标签“最近通信”、“运行开关”参数标签“速度档／字间距／Z 偏移”、以及两个超长信息标签。已分别放宽标题框（200→300）、右栏标签（70→88）、参数标签（52→76），并把两个超长信息标签改成 4 字（实际位置／资源占用）、“坐标依据”取值统一为“软件位姿（最后有效 ACK）”。
- 书写页“任务进度”预览弧线超出预览框并压住底部“比赛要求”提示文字。已让图形区避开提示行，半径按可用高度推算。
- 主页“字间距／Z 偏移”输入框靠一次性标志 `s_cfgInit` 赋值，离开主页再返回后永久为空。已改为在 `CreateHomeControls` 内随控件创建赋值。
- 设备连接页说明条撑满页面剩余高度、单行字垂直居中浮在整块琥珀色空白里。已把说明条高度收紧到 72px。

### 操作核验（续，2026-09-18 复核）
- 三页版面逐页截图核验通过：截图前用 `RedrawWindow(RDW_UPDATENOW|RDW_INVALIDATE|RDW_ALLCHILDREN|RDW_FRAME)` + `UpdateWindow` 强制重绘，再用 `PrintWindow` 抓取，并以 md5 确认三页确实是三张不同画面（主机上 `CopyFromScreen` 会返回冻结帧，是上一轮“三页核验”证据失效的原因）。
- GUI 预检链路再次实测通过：切到书写页 → `PostMessage WM_COMMAND 1009` → 审计新增 `task_preflight result=ok`，`parameters.text="书"`、`source=GUI, actor=HUMAN, mode=DRYRUN`（`logs/audit_20260918_000443.jsonl` 等）。
- 上一轮遗留的“预检报请先输入文本。”已定性：不是逻辑缺陷，而是**从非项目目录启动**导致 `last_task.txt` 按 CWD 解析读不到（详见 bugs.md 与 context.md）；同目录启动可正常预载“书”。

## 待办
- 决定是否把 `last_task.txt` / `gui_window.txt` / `robot_config.json` / `logs/` 改成按可执行文件目录解析（当前按 CWD，从别的目录启动会失效；见 bugs.md）。
- 接入 `calib.json` 或设计带验证状态的 measured safe area，并统一作为运动硬拦截边界。
- 设计急停锁存/人工复位语义；确认控制器状态、报警、限位、当前位置寄存器。
- 解决 ACK 丢失后的重放风险，确认命令幂等/序号/查询语义；批量 7 点协议需真机验证。
- Hanzi/Polyline JSON 字段类型/有限性/文件大小/点数限制与容量评估。
- 新增 `robot_cli.exe` 非交互命令入口，复用同一服务层与审计链路（当前 `main.cpp` 仍是菜单式交互）。
- 上机验证 Z 深度、方向、速度、批量、蘸墨和急停；当前 `Z_DOWN_HEAVY=-388` 仍超出已确认 -385 的实测点。
- `logs/` 与 `last_task.txt` 会持续增长，后续可加清理/轮转策略。

### 切页卡死修复（2026-09-18，`gui_service.cpp`）
- 现象：切到“设备连接”页后界面立刻无响应，CPU 0%（阻塞而非忙等），进程 `taskkill /F` 都杀不掉。
- 定位：gdb attach 被系统拒绝，改用临时文件探针（各阶段写 `_trace.log`）逐段收敛，确认卡在 `gs::list_serial_ports()`；再在其内部打点，定位到 `CreateFileW("\\.\COM5")` 永久阻塞（COM4 也阻塞 5.2s）。触发链：`QueryDosDeviceW(nullptr, names, 4096)` 因本机 DOS 设备名列表长 47349 字符而返回 0/err=122 → 走兜底分支 → 逐个打开 COM1..32 探测 → 本机 6 个端口全是蓝牙虚拟串口（BthModem），打开即阻塞。
- 修复：`list_serial_ports()` 改为只查询不打开——`QueryDosDeviceW` 按 `ERROR_INSUFFICIENT_BUFFER` 扩容重试 + 注册表 `SERIALCOMM` 补充，删除 `CreateFileW` 探测分支；另加纯数字校验过滤 “COMDB” 之类误解析。
- 核验：正常模式与 `--dryrun` 各连续切页 9 次全部 RESPONSIVE；“刷新串口”按钮即时返回并正确列出 COM3/4/5/6/8/9；`--dryrun` 下书写链 `task_preflight(ok) → task_start → 7×send_point + 20×send_batch7 → task_abort → task_end(canceled/estop)` 全部落盘且全程响应；控制台 `Robot.exe --dryrun` 菜单 4 正常退出码 0。
- 未修（已记录）：点“连接设备”仍在 UI 线程打开串口（选到蓝牙虚拟口会同样卡死），需改成工作线程 + 打开期间不持 `g_mu`，见 bugs.md。

## 阶段变更记录
- 2026-09-16：完成比赛/协议/设计方案核对及第一轮源码修复。
- 2026-09-17：完成 Terra 第二轮审查；修复错误传播、预定位估时、输入和调试几何校验；建立 CodeGraph 和 GUI 状态边界。
- 2026-09-17：完成编译、dryrun、CodeGraph 同步和差异检查。
- 2026-09-17：根据用户确认收敛为前三页静态 HTML 原型，补齐设计文档并完成差异检查。
- 2026-09-17：完成 Win32 GUI 实现（gui_service/gui_win32）、修复 preflight 递归锁死锁、完成 GUI 操作核验与控制台帧级回归，新增 build.bat。
- 2026-09-18：修复主页设备信息网格下标写反（“UI 错位”主因）及一批文字截断／图形压字／参数框空白缺陷，三页逐页重抓截图核验通过；定性“请先输入文本。”为 CWD 相对路径问题。
- 2026-09-18：修复“切到设备连接页卡死”——串口枚举误用 `CreateFileW` 打开蓝牙虚拟串口导致 UI 线程内核阻塞；改为纯查询枚举。同期未修：点“连接设备”仍在 UI 线程打开端口（见 bugs.md）。
- 2026-09-18：新增 `使用说明.md`（构建/运行/GUI 各页/控制台菜单/配置与审计/上机安全/已知限制），README 加入指引；提交并推送分支 `feat/gui-win32-assistant`。
- 2026-09-18：修复打开 GUI 后整体/文字闪烁——`OnPaint` 改内存位图双缓冲 + 一次 `BitBlt` 上屏，主窗口加 `WS_CLIPCHILDREN`（仅改 `gui_win32.cpp`）；`build.bat` 重编通过，`--dryrun` 启动 `Responding=True` 并优雅退出。
- 2026-09-18：修复“连接设备”在 UI 线程打开串口导致蓝牙口卡死（与切页卡死同源）——`gs::connect` 改异步 `connect_async`，open 移到 detach 工作线程且期间不持 `g_mu`，结果经 `snapshot()["connect"]` 由 `WM_TIMER` 回报，`g_connect_pending` 防重复点击（改 `gui_service.{h,cpp}`+`gui_win32.cpp`）；`build.bat` 重编通过，`--dryrun` 启动 `Responding=True` 并优雅退出。蓝牙口真实阻塞路径未做自动化实测（跨进程驱动风险高），需上机手动验证。

## 验证说明
- 已验证：GUI 预检/开始书写/停止三条操作路径的审计事件落盘正确；控制台 DRYRUN 帧序列与拆分前基线逐帧一致；`build.bat` 一次产出两个 exe 并冒烟通过。
- 已验证（2026-09-18）：三页截图三张互不相同且逐页目视检查通过；GUI 预检再次实测 `task_preflight result=ok`。本轮只改 `gui_win32.cpp`（`git status` 仅此一个文件），未触及控制台与共享服务层，故未重跑帧级回归。
- 已验证：`RobotGUI.exe --dryrun` 冒烟（控制台 `Robot.exe --dryrun` 菜单 4 正常退出码 0）。
- 已知环境限制：`Robot.exe` 交互式写字需约 8 分钟（单字 3749 点 DRYRUN），本轮以 5 分钟窗口截取前段对比；控制台 stdin 重定向在 EOF 后不退出，需显式输入菜单 4。
- 已知环境限制：本机 `CopyFromScreen` 截图会返回冻结帧，核验必须“强制重绘 + PrintWindow + md5 比对”三步并用。
- 未验证：真实硬件（无设备接入），所有运动均为 DRYRUN 模拟帧；GUI 未在 100% 缩放与高 DPI 之外的分辨率下核验布局；未从非项目目录启动 GUI 的路径解析问题尚未修（见 bugs.md）。