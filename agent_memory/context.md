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

## GUI 排版（自由拖拽 + 字体朝向）约定（2026-09-19 重构，取代旧网格方案，勿再走网格 UI）
- 书写页左栏**只保留「字号」+「书写方向(字体朝向)」两个数值/选择控件**（旧的 每行字数/上区占比/行距/手动/应用/恢复自动/竖排 网格 UI 已全部移除）。网格仅作**初始摆位**：输入文字后按字号在固定视野内自动居中排成网格，用户随后在画布上**逐字拖拽**定最终布局（所见即所得，写啥即摆啥）。
- **固定视野**（预览与书写共用、不再按数据自动缩放）：`gs::view_bounds()` = 四角标定外接框（4 角齐全时），否则回退可达框 **X±162 / Y±85**（真机实测硬极限，`gui_service.cpp` 常量 `REACH_X/REACH_Y`；注意 `g_devLimit` 仍是 ±180 校验用，二者不同、勿混）。拖拽夹取、磁吸、绘制 map 全以该框为准。
- **字体朝向** `g_glyph_orient`(0..3，随 config 持久化)：0=沿Y向下(0°正常)/1=沿Y向上(180°倒置)/2=沿X向上(90°CW)/3=沿X向下(90°CCW)。**仅旋转整字、不改变字块位置**。书写路径在 `run_task_thread` 用 `rotate_local` 绕字心 (S/2,S/2) 做整数旋转局部轨迹；预览用 `DrawRotatedGlyph`(GDI escapement)+字顶小标记。GUI 下拉 `IDC_COMBO_ORIENT`（CBN_SELCHANGE→`set_glyph_orient`）。若某向 CW/CCW 与预期相反，改 `rotate_local`/`esc[4]` 一处即可。
- **字形居中（所见即所得关键）**：`generateSingleCharTrajectory` 按最长边缩放到 S 且以 bbox 左下角对齐 (0,0)，扁字（一/二/三）会贴到字格底部→实写偏下、与居中预览框不符。修：`run_task_thread` 先求 local 轨迹 bbox，加偏移 `dcx=S/2-(minlx+maxlx)/2, dcy=S/2-(minly+maxly)/2` 把字形居中到 [0,S]² 再 `rotate_local`（填满格的字 dcx/dcy≈0 无影响）。预览 `DrawRotatedGlyph` 也改为“正立字形居中于框心 + `SetGraphicsMode(GM_ADVANCED)` 绕框心世界变换旋转”，勿用字面 escapement（90/270°会偏心）；MinGW 无 `ResetWorldTransform`，还原用 `SetWorldTransform` 存旧 XFORM。字顶朝向小标记已删（旋转字形本身可辨朝向，标记像多一竖被用户否掉）。
- 服务层自由布局状态（`gui_service.cpp` 匿名 ns，**不再走 hanzi 网格**）：`g_free_text`(宽字符，绑定键)/`g_free_cells`(每字左下角世界坐标)/`g_free_char_size`/`g_glyph_orient`。API：`view_bounds`、`set_free_char_size`(改字号→`compute_initial_grid` 重排复位)、`set_free_cell`(拖拽落点，夹取+cfg_save)、`set_glyph_orient`、`glyph_orient`、`reset_canvas`(清轨迹缓冲让可编辑叠画重现)。`layout_preview(text)`：**按文本绑定**——文本变则重排初始网格、否则复用已存坐标；返回 cells(世界坐标)+view+orient+valid(字号塞得进视野即 valid，放不下**不自动缩放**)。`prepare_free_layout` 供 `run_task_thread` 用自由坐标直出 TextPlan(text_area 原点 0、offsets=绝对坐标)。`preflight` 同步改走自由布局。**字号健壮性(2026-09-19)**：`compute_initial_grid` 改为**始终生成 n 个字块**(放不下则夹取/允许重叠，不再返回空→避免越界)。**允许字块互相重叠**：`grid_fits(n,S)` 只判“单字塞得进视野”(S≤视野宽/高)，字多排不下时不再判不可行而是重叠摆放、交给用户拖拽分开；仅当单字比视野还大才 valid=false。叠画显示与拖拽门控从 `g_prevValid` 改为 `!g_prevCells.empty()`(RefreshLayoutPreview 无论 valid 都取回 cells)，所以字号大到排不下时字块仍可见可拖。**字号输入务必分两段**（否则打字会被打断）：`EN_CHANGE`→`ApplyCharSizeField` 只在值 ≥60 时实时套用、**绝不回写文本**；`EN_KILLFOCUS`→`CommitCharSizeField` 才把值夹到 [60,500] 并回写生效值+提示。60mm 是比赛每字≥6×6cm 下限，故 <60 会被抬回 60（看着“没变”属正常，但输入过程不能被强制回写，否则打“80”会在首字符“8”就被夹回“60”再也打不出——这是踩过的坑）。
- config 新键：`free_text`/`free_cells`([{x,y}])/`free_char_size`/`glyph_orient`；snapshot.cfg 暴露 `free_char_size`/`glyph_orient`。旧 `layout_*`/`write_dir` 键与 hanzi `plan_manual_layout`/`plan_text_area_and_layout`/`prepare_layout_only` 仍在（控制台/回归用），GUI 已不调用。
- GUI 拖拽（`gui_win32.cpp`）：`TrailView g_tv` 缓存固定 map（`DrawTrailPanel` 每帧写、`tvMapX/Y`+`tvUnmapX/Y` 复用）；`CanDragCells`=书写页+视野就绪+`!g_prevCells.empty()`+`!task_active`+`commandedSize()==0`；`HitCell`/`SnapCell`(≈6px 磁吸到其它字边/中心+视野边/中心)。`WM_LBUTTONDOWN` 命中→SetCapture；`WM_MOUSEMOVE` 拖拽(磁吸+夹取)；`WM_LBUTTONUP`→`set_free_cell` 落库。可编辑叠画显示条件 = `!g_prevCells.empty() && !task_active && gs::trail::commandedSize()==0`（**允许重叠也照画**；跑过一次任务后隐藏，点「重置画布」再编辑）。`WRITE_PANEL_H` 372→300。
- 验证：g++ 链接 RC=0（GUI+控制台，无新 warning）；纯数学单测 29/29 PASS（网格居中/换行/夹取/字号超视野不可行/旋转四向保框且可逆/回退视野）；dryrun 启动 RESPONDING=True；Computer Use 实操核验：拖拽移动+落库、字体朝向仅旋转不改位、固定视野不缩放均通过。改动文件仅 `gui_service.{h,cpp}`+`gui_win32.cpp`（未碰 hanzi/gui_trail/serial_port，避让他窗口）。

## 分页 + 翻页约定（2026-09-20）
一页写完→向翻页机构发信号（当前为模拟等待，蓝牙未接入）→等翻页→抬笔清实时轨迹→写下一页。用户四项决策：①**每页独立布局**可逐字拖拽；②翻页走纸量=版面高、**下一页字块与当前页世界坐标重合**（不做偏移补偿，简化为同坐标系复用）；③超容量**只写前 每页字数×总页数 字**（截断，不报错阻断）；④翻页等待期点停止/急停**立即中止不翻页**。
- **服务层分页模型**（`gui_service.cpp` 匿名 ns）：`g_full_all`=过滤后全量有效文本（**不截断**，缩小容量后再放大可无损恢复）；`g_full_text`=截断到容量的实际书写文本；`g_pages`=每页 `gs::PageEntry{std::wstring text; std::vector<Offset> cells}`；`g_page_chars`(1~50)/`g_page_count`(1~20)；`g_edit_page`(空闲时 GUI 拖拽页) 与 `g_write_page`(atomic，任务线程书写页) 双游标；`display_page()`=任务中取书写页否则编辑页；`page_drag_enabled()`=空闲+显示页==编辑页+`commandedSize()==0`。
- **`rebuild_pages(full)` 三级坐标继承**（防改页数/每页字数/容量恢复丢摆位）：①新页 k 与旧页 k 同文本→整页沿用；②`same_order`(旧有字页序列==新前缀)时按文本认领未占用旧页；③按字在旧全文首次出现位置继承该字旧坐标（整页每字都能继承才用，否则留 `ensure_page_grid` 补初始网格）。`layout_preview`/`preflight`/`run_task_thread` 均 `filter_page_chars`→`g_full_all`→截断 `g_full_text`→`rebuild_pages`。`partial`=编辑页超出有字页范围（容量外空页），有字末页不满额不算 partial。
- **翻页接口**：`gs::PageTurnFn=std::function<bool(int page_no_1based,int total)>`；`set_page_turn_handler(fn)` 注入真实蓝牙（传 nullptr 恢复默认）；默认 `page_turn_wait_locked()` **必须在持 g_mu 的任务线程调用**，分片 50ms 睡眠并轮询 `g_task_cancel||g_estop`，命中返回 false。`set_page_turn_wait_ms(500~60000)` 持久化。GUI `Run()` 有注入锚点注释。
- **`run_task_thread` 多页循环**：预热→(蘸墨)→逐页{`prepare_page_plan(页)`→`task::page_begin`→逐字(居中/旋转/发送，每页首字抬笔预定位锚点，跨页全局 `gci%5` 蘸墨)→页尾补蘸→非末页:抬笔到末字上方→审计`page_turn`→`g_page_turn()`→成功则 `g_write_page=下一页`+`trail::reset()`，失败/取消则 canceled}→末页收尾蘸墨→(描边)。审计事件 `page_turn`(发信号,带 turn_to_page/wait_ms)/`page_turn_result`(ok/canceled/turn_failed)。
- **config 键**：`page_chars/page_count/page_turn_wait_ms/page_text`(截断后)/`page_text_full`(全量)/`page_cells`(嵌套 `[{text,cells:[{x,y}]}]`)；`cfg_load` 兼容旧单页键 `free_text/free_cells` 自动迁移为第 1 页。snapshot.task 加 `page_no/page_total/display_page`，cfg 加 `page_chars/page_count/edit_page/page_turn_wait_ms/task_finished/text_total/text_written`。
- **GUI**（`gui_win32.cpp`）：左栏第三行「每页字数」「总页数」整数框（`ApplyIntField`/`CommitIntField` 两段式，同字号教训）+ 截断提示 STATIC；轨迹面板顶加**翻页条** `◀ 第 k/N 页（本次书写 M 页）▶ …翻页等待(ms)[输入]`（`WritePlotGeo` 新增 strip/btnPrev/btnNext/pgLab/waitLab/waitEd 几何，`WRITE_PANEL_H` 300→346；**页码标签 `pgLabW=480`、等待标签 `waitLabW=150`**——初值 360/120 太窄，`Text` 的 `DT_END_ELLIPSIS` 会把“本次书写 2/2 页”“翻页等待(ms)”截成省略号，2026-09-20 加宽修复）；`RefreshLayoutPreview` 缓存 `g_pgEdit/g_pgTotal/g_pgWritten/g_pgPartial` 并在任务结束瞬间刷新；`IDC_BTN_PAGE_PREV/NEXT` 空闲切编辑页(任务中拒绝)。
- **验证**：`verify_pages` 单测 78/78（切页/截断/容量恢复不丢字/每页隔离/三级继承/旧键迁移/漂移重排/preflight/翻页成功+失败+默认模拟等待+7.4 等待期 abort 立即中止停第1页）；dryrun 审计链完整；Computer Use 实操切页/隔离/翻页后轨迹清空重播所见即所得。**待真机**：走纸精度是否=版面高（否则加偏移参数）、翻页后 `g_last_pose` 不变前提、真实蓝牙替换模拟、拖纸时臂抬笔避让安全性。

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