# 任务进度

> 本文档维护当前任务进度，供 agent 在开始非简单任务前读取。
> 只保留当前有效信息，不写成无限追加日志。
> 出现阶段完成、方向变化、失败尝试、准备结束会话时，必须更新本文件。

## 当前任务
**（已实现·离线验证通过·待上机 & 待视觉核验）GUI 字号/布局实时调整 Phase1（字号+网格，横排）**：并入「书写任务」页（不新开页），输入文字后可实时调字号/每行字数/上区占比/行距并在轨迹画布叠看版面预览。设计=自动/手动双模式，默认自动保持原行为零回归。实现：`robot_common.{h,cpp}` 新增手动全局 `g_layout_mode/g_lm_char_size/g_lm_cols/g_lm_top_ratio/g_lm_row_spacing` + `TextPlan.err/row_spacing` + `LayoutErr` 枚举（字号下限锁 60 比赛红线、上不封顶）；`hanzi.cpp` 新增 `plan_manual_layout`（按用户值严格排布+纯 fit 检查，放不下置 err 且**不自动缩放**），`prepare_layout_only` 按 `g_layout_mode` 分流；`gui_service.{h,cpp}` 新增 `set_layout_mode/char_size/cols/top_ratio/row_spacing` + `layout_preview`（不写审计的实时预检+计划字块世界坐标）+ cfg 五键持久化 + snapshot.cfg 暴露 + preflight 补 mode/row_spacing/err_code；`gui_win32.cpp` 书写页左栏加排版控件（`WLGeo` 共享几何防错位、"手动排版"复选框+应用/恢复自动）、`EN_CHANGE` 实时刷新、方案二把计划字块（紫点线框+自适应字形）叠画到 `DrawTrailPanel`（外接框并入 cells，任务中冻结不叠画），`IDC_BTN_WRITE` 加排版可行性闸门。验证：GUI+控制台 g++ 链接 RC=0；排版单元自测 20/20 PASS（自动基线不变、手动生效字号不被改、S=100 上不封顶、FONT_H/BAD_PARAM/GRID 失败码正确、不缩放、偏移顺序对）；`RobotGUI --dryrun` 启动冒烟 Responding=True 正常退出。未验证：书写页视觉叠画与 EN_CHANGE 端到端未交互跑（仅代码+编译+离线索测），建议关窗后用新 `RobotGUI.exe` 实操核验。改动文件：robot_common.{h,cpp}、hanzi.{h,cpp}、gui_service.{h,cpp}、gui_win32.cpp（Phase1 未改 robot_config.json，默认自动）。

**（已实现·离线验证通过·待上机）GUI "书写平面"页**（与主页/设备连接/书写任务同级，专调落笔 Z）。解决"书写平面低于桌面"。实现：新全局 `g_writing_plane_z/g_writing_plane_valid` + `robot_config.json` 持久化 + `snapshot["cfg"]` 暴露；`hanzi.cpp` 落笔 `zdown = valid ? g_writing_plane_z : zFor(三层)`（未保存前行为不变）；服务层 `gs::preview_writing_plane`(移 (0,0,Z) 悬停)/`gs::set_writing_plane`(校验+置位+保存)；GUI `ID_PAGE_PLANE` 页含 当前Z显示 / 新Z输入框 / 模拟悬停 / 保存并应用。整合编译 RC=0；单元验证 PASS（设 -365→落笔全 -365、过渡点合法；未设→heavy/normal 分层）。Z 语义为 raw（不含 Z_OFFSET，发送时叠加）；改动文件：robot_common.{h,cpp}、hanzi.cpp、gui_service.{h,cpp}、gui_win32.cpp、robot_config.json。

**（已归档、待上机）顺滑度方案 A/B + 逐点停止 + 节拍可配置 + dryrun 归因**
- 方案A 补偿式计时（`estimateMoveMs` 语义改"目标指令间隔"、去 60/35ms 走停地板、`wait_after_send` 扣掉串口/运动耗时）。方案B RDP(0.06mm) 抽稀（`resamplePolylineRDP`，直段塌两端、弯曲保点）。逐点停止（`estopLiftAt`+书法逐点/描边批量/回退逐点三处 `g_estop` 轮询）。节拍参数运行期可配置（`z_settle_ms` 等 5 键，`gs::cfg_load` 覆盖全局）。整合编译 RC=0。
- dryrun 归因（"书"，关描边）：prev 中位 186ms/min 62ms(地板钉死) → new 153ms/31ms → LOW 109ms/26%<50ms；顿挫大头是每笔起落 dwell+Z 沉降+冷启动，地板次之。RDP 对简单稀疏字不减点。字库缺失"找不到书.json"已定位非回归并派子代理下载 9574 字到 `D:/objects/hanzi-writer-data/data/`。
- 上一轮 Win32 GUI 已完成，本轮不动既有三页布局。

## 下一步 / 待办（新增或承接）
- **Phase2：书写方向 已完成（离线验证过·待视觉核验）**——`g_write_dir`(0横排/1竖排右起)+config 键+`set_write_dir`+snapshot+preflight/`layout_preview` 回显 dir；hanzi 抽出方向感知 `grid_extents`/`fill_offsets`，`plan_manual_layout` 与 `plan_text_area_and_layout` 加 `dir` 参（`dir=0` 逐点保持旧行为；竖排右起=列内从上到下、列从右往左，fit 轴向交换）；自动模式竖排候选改偏好短列 `{4,3,2,5}`（横排仍 `{5,4}`）；GUI 书写页加“竖排(右起)”复选框 `g_chkVert`（独立于自动/手动），状态框随方向显示网格。编译链接 RC=0 无新 warning；方向单测 19/19 PASS（竖排字序、轴向交换、自动短列候选、横排回归）；p2 GUI 启动 Responding=True。未验证：书写页视觉叠画与方向端到端仍需关旧实例后用新 `RobotGUI.exe` 实操核验。
- 运行态提醒：`robot_config.json` 当前是某次手动模式测试写入的状态（`layout_mode:1/layout_cols:1/layout_top_ratio:0.9`，且无 `write_dir` 键——由 Phase1 期构建保存），非源码改动；如需回到干净默认，可勾“恢复自动”或在 config 里改。
- **可达极限已探明 + 实验已撤销（2026-09-19）**：把 `dev_limit` 临时放宽到 ±180 真机测试，臂仍**稳定停在 X±162 / Y±85** → 限制在**控制器/机构侧，非软件**。随后按用户要求**撤销整个可达边界实验回原版**：`g_devLimit` 恢复固定 ±180，删除 `cfg_save/cfg_load` 的 `dev_limit`、`snapshot.cfg.dev_limit`、`robot_config.json` 的 `dev_limit` 键；重编 RC=0。保留四角标定/轨迹面板/结果提示。**结论留存**：真机上电可达 X±162/Y±85（断电可手推越过），疑控制器工作区软限位（手册 ±180 但固件设小）；软件无解，需问厂商放参数或改机械。
- **速度编码疑似反向**：`serial_port.cpp` 发 `level-1` 与 `speedLevelToXYmmPerSec` 方向相反，手册 `00 最快`；上机先定真实"档→mm/s"方向再决定是否反转映射。
- **auto_draw 描边走 `0x0064` 是抓放宏（手册明确）**：真机会把"画山水"当抓放动作，风险高。修：把描边发送路径由 `sendPointsBatch7` 改回逐点 `sendPointRetry`；在改完前保持 `auto_draw=false`。
- **停止按钮 GUI 侧差异未定位**：静态看 `abort_task` 与 `request_estop` 都置 `g_estop`，用户报"停止没反应/急停有效"；上机若仍复现，看提示文本与 `task_abort` 事件确认是没触发还是段延迟。
- **HanziWriter y 轴方向**：项目未翻转 y，若真机字上下颠倒，是翻转字形 y 的问题（`bugs.md` 有记）。
- 上机阶段 3（判定控制器运动模型）由用户执行中。

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

### 书写顿挫修复（2026-09-18，方案A 补偿节拍 + 方案B RDP 抽稀）
- 根因：书法落笔段是“逐点绝对定位 + 发完再固定空等”，每点 `sleep≥POINT_RATE_LIMIT_MS(60ms)` 叠加在 9600 串口单帧往返（~42ms）之上，且每个 0.6mm 途经点都是一次到点全停 → 沿笔画一路“点刹”。全项目原无插补/前瞻/缓冲。
- 方案A（`motion.cpp` `estimateMoveMs` / `transmitTrajectoryWithSplit`）：`estimateMoveMs` 语义改为“相邻两指令的目标间隔”，去掉 60ms/DELAY 走停地板，只留 `MIN_POINT_INTERVAL_MS(12)` 下限；发送循环改用 `wait_after_send(t0,…)` 补偿式计时（串口+运动耗时计入间隔，只补睡到目标节拍），拐角停顿折进间隔不再单独叠加。抬落笔/Z 沉降等物理停顿保留。新增可调常量 `MIN_POINT_INTERVAL_MS`、`COLD_START_MIN_MS`（`robot_common.h`）。
- 方案B（`motion.cpp` `resamplePolylineRDP` + `resample_range`）：书法落笔段改用 Ramer–Douglas–Peucker 按最大弦高偏差 `RESAMPLE_DEV_MM_CALLI(0.06mm)` 抽稀，直段塌成两端点（一笔到底）、弯曲与拐角自动保点；迭代实现防深递归。描边段沿用定步长 `RESAMPLE_STEP_MM_DRAW(1.2)` 不变。（曾用“转角自适应步长”方案，经曲线探针证伪——密集平滑曲线被误判为直线过度抽稀，已弃用改 RDP。）
- 已验证（离线单测，`Robot.exe`/`RobotGUI.exe` `build.bat` 重编通过 RC=0）：完美直线 121→2 点、偏差 0；R=2~200mm 圆弧 RDP 后最大垂直偏差恒 ≤0.06mm 且半径越小保留越密（急弯 1.8 点/mm vs 近直 0.15 点/mm）；纯 XY 落笔点目标间隔 60→12ms、抬笔→落笔仍 202ms、100mm 定位仍按 ~1250ms。
- 未验证（须上机）：真实控制器对相邻指令是“到点全停”还是“可在运动中接受新目标连续走/缓冲”——决定 A 能消除死等但能否根除每点顿挫的上限；9600 波特率单帧 ~22ms 是硬吞吐天花板；方案C（书法落笔改走 batch7 队列）是否更顺滑，需真机小样验证。上机前先空载、限位、手触急停。

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

- 2026-09-18：书写中"停止任务"不跟手的 motion 侧加固——`transmitTrajectoryWithSplit` 原来只在外层段边界查 `g_estop`，一笔之内/描边整段批量循环不查→段中途停不下。新增 `estopLiftAt` 抬笔复位小函数，并在书法逐点 for、描边批量 while 每块、回退逐点 each 三处轮询 `g_estop`，命中即抬笔返回（`motion.cpp`）。注：急停与"停止任务"(`abort_task`)都置 `g_estop`，本改动让二者都能逐点打断；"停止按钮点了没反应 vs 急停有效"的 GUI 侧差异仍待上机确认（见 bugs.md）。整合编译 RC=0。
- 2026-09-18：本机缺字库致"找不到书.json"——非回归（hanzi.cpp/HANZI_BASE_DIR 未改），派子代理下载 HanziWriter 常用字库到 `D:/objects/hanzi-writer-data/data/`。
- 2026-09-18：节拍参数运行期可配置——新增全局 `g_z_settle_ms/g_stroke_begin_ms/g_stroke_end_ms/g_cold_start_min_ms/g_min_point_interval_ms`（默认取原 *_BASE 常量），`get_*`/`estimateMoveMs`/`wait_after_send` 改读全局，`gs::cfg_save/cfg_load` 增对应 5 键（`z_settle_ms` 等，0~3000ms 限幅），改 `robot_config.json` 重启即生效、无需重编（改 `robot_common.{h,cpp}`+`motion.cpp`+`gui_service.cpp`）。dryrun 归因（"书"，关描边）：prev 中位 186ms/最短 62ms(60ms 地板钉死)→ new(A/B) 153ms/31ms → LOW(压dwell) 109ms/26%<50ms；结论：顿挫大头是每笔 Z 沉降+起收笔 dwell+冷启动（省~20%），60ms 地板是次因（A 省~9%），RDP 对稀疏简单字不减点。LOW 版为测量极端值、勿上机（墨淡无锋）。

- 2026-09-19（本窗口）：实现「书写任务」页**字号/布局/方向 + 实时预览**（Phase1 字号/网格 + Phase2 竖排右起），自动/手动双模式默认自动零回归，参数经 `robot_config.json` 持久化、控制台共用。改动文件：`robot_common.{h,cpp}`（6 全局 + TextPlan.err/row_spacing/dir + LayoutErr 枚举）、`hanzi.{h,cpp}`（`plan_manual_layout` + 方向感知 `grid_extents`/`fill_offsets`，`prepare_layout_only` 分流）、`gui_service.{h,cpp}`（5 setter + `set_write_dir` + `layout_preview` + cfg/snapshot/preflight 扩展）、`gui_win32.cpp`（书写页 `WLGeo` 排版控件 + `g_chkManual`/`g_chkVert` + `EN_CHANGE` 实时刷新 + 计划字块叠画进 `DrawTrailPanel` + 开始书写可行性闸门）。**收尾修复**：叠画显示条件加 `gs::trail::commandedSize()==0`——原 `!task_active()` 会让任务跑完后计划字块重新显示、与保留的真实轨迹重叠冲突，现只在首次书写前的规划阶段叠画。验证：`g++` 链接 RC=0 无新 warning；排版/方向单元自测 Phase1 20/20 + Phase2 19/19 全 PASS；`RobotGUI --dryrun` 启动 `Responding=True`；正式 `Robot.exe`/`RobotGUI.exe` 已覆盖。未验证：书写页视觉叠画与方向端到端仍待关旧实例后实操核验（用户已反馈一次重叠问题并据此修复）。注：`robot_config.json` 的 M 是用户用 Phase1 GUI 测试手动模式写入的运行态（mode1/cols1/top0.9，无 write_dir 键），非源码改动。

- 2026-09-19：书写页新增**实时轨迹面板**（`gui_trail.{h,cpp}` 新文件 + `serial_port::readPose` 0x03 读坐标 + `gui_service` 钩子接线 + `gui_win32` 面板）。数据源 hybrid：任务期把已下发点（软件位姿）记为稠密轨迹（紫落笔/灰抬笔），真机每字 `readPose` 采样实测点（绿点）叠加，用于暴露方向/丢步；DRYRUN 无实测点。轨迹仅在 `g_task_active` 时记录，快捷操作不污染。改动文件：`serial_port.{h,cpp}`、`gui_service.{h,cpp}`、`gui_trail.{h,cpp}`(新)、`gui_win32.cpp`、`build.bat`(COMMON 加 gui_trail.cpp)。
- 2026-09-19：边界标定从"纸张长宽"改为**四角标定**（用户依次输入四角设备坐标，每角"预览/保存"）。`gs::trail` 存 `Corner[4]`，持久化 `robot_config.json` 的 `corners`；`gs::preview_corner`(抬笔移到该角，真机移动/DRYRUN 打帧)、`save_corner`(校验+审计+存)、`clear_corners`。修 4 问题：①角标签截断→`lblW 30→46`；②预览改用固定书写平面 Z（`g_writing_plane_valid?g_writing_plane_z:Z_UP`，非 Z_UP）；③`Y=-100` 到不了实为 Z_UP 顶部 XY 收窄，随②解决；④四角连线按绕质心极角排序，消除"八字"交叉成规整矩形。构建 RC=0；**已 dryrun 截图核验**：角标签完整、角值回填、橙框为矩形、提示不压字。**未验证(须上机)**：预览真实移动与可达边界、readPose 实测点语义(0x0008 读回是实际位置还是最后写值)。

- 2026-09-19：可达 XY 边界**可配置 + 全局拦截**。发现控制器把 Y 实际夹在约 ±85（比手册 ±180 紧，`y=-100` 发得出但臂不动=静默夹取）。做法：`g_devLimit` 从 `robot_config.json` `"dev_limit"`[xmin,xmax,ymin,ymax] 读写（`cfg_load` 最先应用、绝对 ±180 内校验），当前设 **X±162/Y±85**；`sendPoint`/`preview_corner`/`save_corner`/回中心/测试点据此统一 fail-fast。另修：四角预览/保存结果原走 `SetResult`（只画主页）在书写页不可见 → 改在轨迹面板标题行右侧显示。构建 RC=0；**dryrun 已验**：`y=-100` 预览被拦（本会话无下发/无新审计文件）、`y=-80` 放行且提示"已移到(160,-80)"可见。**注意**：收紧后超出可达框的书写点会被拒（排版须落在可达框内；`g_safeArea` 默认 Y 到 110 已超出，待后续对齐）。真机预览可达性仍待上机。

## 验证说明
- 已验证：GUI 预检/开始书写/停止三条操作路径的审计事件落盘正确；控制台 DRYRUN 帧序列与拆分前基线逐帧一致；`build.bat` 一次产出两个 exe 并冒烟通过。
- 已验证（2026-09-18）：三页截图三张互不相同且逐页目视检查通过；GUI 预检再次实测 `task_preflight result=ok`。本轮只改 `gui_win32.cpp`（`git status` 仅此一个文件），未触及控制台与共享服务层，故未重跑帧级回归。
- 已验证：`RobotGUI.exe --dryrun` 冒烟（控制台 `Robot.exe --dryrun` 菜单 4 正常退出码 0）。
- 已知环境限制：`Robot.exe` 交互式写字需约 8 分钟（单字 3749 点 DRYRUN），本轮以 5 分钟窗口截取前段对比；控制台 stdin 重定向在 EOF 后不退出，需显式输入菜单 4。
- 已知环境限制：本机 `CopyFromScreen` 截图会返回冻结帧，核验必须“强制重绘 + PrintWindow + md5 比对”三步并用。
- 未验证：真实硬件（无设备接入），所有运动均为 DRYRUN 模拟帧；GUI 未在 100% 缩放与高 DPI 之外的分辨率下核验布局；未从非项目目录启动 GUI 的路径解析问题尚未修（见 bugs.md）。