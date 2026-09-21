# 问题与风险

> 本文档维护已知问题与风险记录，供 agent 参考。
> 只保留当前有效条目，不写成无限追加日志。
> 发现无关问题只记录风险，不顺手重构或扩大范围。

## 已知问题
<!-- 格式：状态 | 描述 | 影响范围 | 复现步骤 -->

| 状态 | 描述 | 影响范围 | 复现步骤 |
|------|------|----------|----------|
| 已修复 2026-09-16 | 急停线程触发一次即 return（第二次急停失效）、_getch 窃取控制台输入、空格误触急停 | 真机安全、控制台交互 | 见修复提交（estop_poll 重写为 PeekConsoleInput 常驻轮询，仅 ESC 触发） |
| 已修复 2026-09-16 | inZRange 校验原始 z，未含 Z_OFFSET，偏移过大时越界指令可下发 | 真机安全 | 菜单5设 Z_OFFSET=-95 后 dryrun 写字：现报"点越界 Z=-415"并拦截 |
| 已修复 2026-09-16 | sendPoint/packPoint5 忽略 Point.speed（全局 SPEED_LEVEL 打包），蘸墨"上慢下快"不生效 | 蘸墨动作质量 | dryrun 开蘸墨，TX 帧速度字节现 0x00/0x02 交替 |
| 已修复 2026-09-16 | calli_end 分界按原轨迹下标传入，去重收缩与书法段重采样后未更新，混合轨迹会把描边点当书法逐点发送 | 混合轨迹发送 | 自测：书法段(含0.2mm去重收缩)+描边段混合轨迹，描边 8 点正确批量 7+1=2 帧 |
| 已修复 2026-09-16 | 书法区去重间距 1.5mm→0.3mm（小于 0.6mm 重采样步长），密集笔迹不再在重采样前丢失细节 | 书法点密度 | 自测：0.4~0.6mm 点距落笔段，重采样后保留 0.9/2.0/3.0mm 点（旧逻辑仅剩 0/2.0） |
| 已确认 2026-09-16 | 设备 Z 可动范围实测 -320~-385（-320/-330/-340/-350/-385 均可动，-310 及以上 ACK 但不动）；写寄存器 0x0008 即触发运动，协议帧格式正确 | 全部 Z 相关运动 | 调试1单点实测；Z 层/行程已改为可配置（菜单19 + config.json） |
| 已修复 2026-09-16 | 菜单3复位不动：中心 Z=-300 在设备死区（高于-320），旧 inZRange(-410~-250) 校验过宽未拦截；Z_UP=-320 恰在可动边界 | 菜单3/全部抬笔动作 | Z_UP 调至 -325 留裕量；inZRange 改按实测行程校验 |
| 待确认 | HanziWriter 数据 y 轴向下、代码未翻转，若机械臂 Y+ 朝纸上方则字上下镜像 | 首次真机写字 | 真机写"十"字验证方向（调试2 L形测试） |
| 待确认 | 源码默认 g_enableDip=false，但比赛要求书法和国画均自主蘸墨至少一次 | 比赛合规性 | 启动程序查看默认菜单状态 |
| 待修复 | calib.json 没有接入实际加载流程，设备/安全边界仍来自硬编码（Z 行程已先行可配置化） | 运动安全和标定 | 选择菜单 8 只显示保留提示 |
| 部分确认 2026-09-16 | Modbus 协议：单点写寄存器已上机验证（设备 ACK 且执行运动）；批量7点帧、坐标方向/镜像、速度编码仍未验证 | 全部运动 | 调试5 方框批量 + 调试2 L形方向测试 |
| 待确认 | 长纸张自动输送尚无机构、定位和控制方案 | 纸张布局和比赛稳定性 | 先完成固定纸张版本，再做纸张输送样机 |
| 待修复 | 现有布局默认偏向 5 列，七个最小 60mm 字符和画面区域可能无法同时舒展 | 比赛构图和合规性 | 重新设计为按实际工作区计算的 4+3 或 3+4 布局 |
| 方案已形成，待实测 | 尚不能确认控制器是否提供真实当前位置、状态、报警和限位寄存器 | 测量结果可信度 | 先确认寄存器映射；未知地址不得盲读 |
| 方案已形成，待确认 | 自动拖纸机构可能被组委会视为额外执行机构 | 比赛合规性 | 赛前向组委会书面确认；固定纸张版作为保底 |

## 蓝牙翻页接入的坑（2026-09-20/21，实测踩过；下次别再走弯路）

| 状态 | 描述 | 影响范围 | 复现步骤 / 判据 |
|------|------|----------|----------|
| **已定位·环境问题** | **`OpenAsync` 返回 `GattOpenStatus_SharingViolation(4)` = 模块被别的程序占着**。BLE 外设同时只服务一个中心设备；被占用时它**停止广播**（`BleakScanner` 扫不到），但 Windows 仍按配对缓存回答 `ConnectionStatus=Connected`，极具迷惑性 | 蓝牙翻页全部功能 | `OpenAsync`/`GattCharacteristicsResult.Status=3(AccessDenied)`。查法：`tasklist` + PowerShell `Get-CimInstance Win32_Process` 看命令行。本次是用户自己的 `motor_ble_gui.py` 还开着 |
| **已确认·不要再用** | **Win32 老 GATT API 收不了也发不了**：`BluetoothGATTGetServices/GetCharacteristics` 读**缓存**能成功，但 `BluetoothGATTSetCharacteristicValue` 与 `BluetoothGATTRegisterEvent` **一律立刻返回 E_FAIL(0x80070001)**。即使 WinRT 已把链路拉起（`ConnectionStatus=Connected`）也一样 —— 两套栈不能共用设备 | 任何走 Win32 GATT 的方案 | 纯 Win32 PoC 实测：服务/特征枚举 OK，订阅与写入 5 次重试全 E_FAIL、耗时 0ms |
| **已确认·不要再用** | `BluetoothGATTGetServices/GetCharacteristics` 带 `BLUETOOTH_GATT_FLAG_FORCE_READ_FROM_DEVICE` 在本机返回 **E_INVALIDARG(0x80070057)** | Win32 GATT 路径 | 同上；改用 `BLUETOOTH_GATT_FLAG_NONE` 才成功（但写入仍失败，见上条） |
| **已确认·必守** | **特征枚举必须 `GetCharacteristicsWithCacheModeAsync(Uncached)`**：服务缓存可能是冷的，`GetCharacteristicsAsync()` 直接返回 **0 个特征** | 找不到 FFE1 | 纯 WinRT 首版就是这样：`FFE0 下 0 个特征` |
| **已确认·必守** | **只做 `RequestAccessAsync` 不够，必须再 `OpenAsync(GattSharingMode_SharedReadAndWrite)`**；且它的返回状态就是「模块现在能不能用」的直接判据 | 链路可用性 | `RequestAccess` 给 Allowed，但 `OpenAsync` 给 SharingViolation |
| **已确认·必守** | MinGW g++ 里用 WinRT：`ITypedEventHandler<GattCharacteristic*, GattValueChangedEventArgs*>` 经 `AggregateType` 展开后，**`Invoke` 的实参是接口 `IGattCharacteristic*`/`IGattValueChangedEventArgs*`**，写成运行类**编译期 override 不上**（报 does not override） | 通知订阅（闭环回包） | 直接照 `ble_motor.cpp` 里的 `ValueChangedHandler` 抄 |
| **已确认·必守** | `DEFINE_GUID` 未定义 `INITGUID` 时**只有声明没有实体** → `IID_IAsyncInfo`/`IID_IBufferByteAccess` 链接期 undefined。**自带 GUID 常量**最省事（见 `ble_motor.cpp`） | 链接 | `undefined reference to IID_IAsyncInfo` |
| **已确认·必守** | MinGW 的 `windows.devices.bluetooth.h` **只前置声明 `IBluetoothLEDevice3`**（没定义），拿不到 `GetGattServicesAsync`。可用 `IBluetoothLEDevice::GetGattService(uuid,&svc)` + `get_GattServices()` 替代 | 服务枚举 | 编译期 `invalid use of incomplete type IBluetoothLEDevice3` |
| **已修复 2026-09-21** | GUI 关闭时崩溃：`ble_motor` 的工作线程若是**可 join 的静态 `std::thread`**，静态销毁阶段会打 `terminate called without an active exception` 并崩溃 | 程序退出 | 改为 `detach()` + `g_started/g_workerDone` 显式收尾，并在 `WM_DESTROY` 调 `gs::page_turn_shutdown()`。正常关闭（`CloseMainWindow`）已实测干净退出 |
| **非缺陷·勿误判** | `timeout N ./RobotGUI.exe` 被强杀时 stderr 会出现 `terminate called without an active exception` —— 这是**改动前就存在**的：`gui_win32.cpp` 的 `g_poll` 在消息循环期间是可 join 的。正常关闭路径会 `join()`，无此问题 | 仅强杀场景 | 用 `CloseMainWindow()` 复测即无 |

**本机蓝牙模块事实（省得重新体检）**：模块自称 HC-05，**实为 BLE 模块**（固件 `hc05V2.3_le`），Windows **永远不建 COM 口**（SPP 实例 0 个，枚举在 `BTHLE\DEV_21F6473AD889`）；地址 **`21:F6:47:3A:D8:89`**；GATT 服务 **FFE0**，特征 **FFE1=0x0010(write+notify)** / FFE2=0x0013(write only)；**FFE1 不可读**（所以只能靠 notify 收回包）。PC 端一线通吃：Python 用 `bleak`，C++ 用本工程 `ble_motor.{h,cpp}`。

**能耗提示**：`BluetoothLEDevice` 对象活着 = 链路活着，所以**建链一次后要常驻复用**（首次约十几秒，之后一次翻页只要几百毫秒）。翻页回调里已按此实现；断链后下次翻页会自动重连。

## 风险记录
<!-- 格式：可能性 | 影响 | 描述 | 缓解措施 -->

| 可能性 | 影响 | 描述 | 缓解措施 |
|--------|------|------|----------|
| 高 | 中 | 仓库已跟踪 x64/ 构建产物（历史遗留），.gitignore 只能阻止新增 | 需所有者确认后执行 git rm -r --cached x64 |
| 中 | 高 | 急停在 sleep_for 期间不响应，仅在下个发送点检查 | 上真机时保持手触硬件急停/电源 |
| 高 | 高 | 正式比赛开始后禁止人工干预，蘸墨流程失败将导致判零或作品中断 | 蘸墨动作必须纳入自动流程并在赛前反复验证 |
| 高 | 高 | 20 分钟正式执行时间较短，通信响应和点数过多会拖慢书写 | 预先生成轨迹，优先使用已验证的 7 点批量协议 |
| 中 | 高 | 规则要求每字至少 6x6 cm，不能沿用小字号测试参数 | 布局算法固定最小字号为 60x60mm，并按试题字数重新排版 |
| 中 | 高 | 比赛试题现场抽签，不能只准备单一固定作品 | 预置规范笔顺字形和主题化画面模板，现场仅输入/选择抽到的内容 |
| 高 | 高 | 纸张输送打滑会导致整幅作品坐标漂移，且无人工纠偏机会 | 采用刚性纸张载台、机械基准、单向索引和输送后校验；未经连续测试不用于正式比赛 |
| 中 | 高 | 自动拖纸机构可能被裁判理解为额外执行机构，规则未明确说明 | 赛前向组委会书面确认；默认比赛版本优先采用固定纸张 |

## 本轮审查补充
| 状态 | 描述 | 影响范围 | 复现步骤 |
|------|------|----------|----------|
| 已修复 2026-09-17 | 预定位发送后才读取 `g_last_pose`，导致 `dXY` 恒为 0；作画发送结果和蘸墨结果被忽略；菜单部分输入失败后继续使用无效值 | 到位节拍、任务完成判断、配置安全 | 静态审查发现；已分别传播返回值、发送前保存旧位姿并统一拒绝无效输入 |
| 已修复 2026-09-17 | 预热固定使用 `(0,0)`，调试 L 形/方框/抬落笔循环缺少有限性和 XY 边界校验 | 预热定位、真机调试安全 | 静态审查发现；已使用配置中心点并增加参数校验 |
| 待验证 | ACK 丢失/超时后自动重试可能重放已执行动作；批量末批填充语义、控制器到位语义未知 | 真机运动安全、节拍 | 需协议文档、抓包和上机验证，不能由静态代码确认 |
| 待修复 | 急停未形成锁存故障状态，软件仍会尝试抬笔并可清除标志；`calib.json` 未接入统一运动硬拦截 | 急停恢复、安全边界 | 需明确硬件急停/人工复位和标定数据格式后实现 |
| 待修复 | Hanzi/Polyline JSON 字段类型和有限性校验不足；缺字会跳过；无任务点数/时间预算 | 输入可靠性、比赛完整性、GUI 预检 | 构造异常 JSON 或缺失字形可触发；后续增加预检 |

## GUI 阶段（2026-09-17）
| 状态 | 描述 | 影响范围 | 复现步骤 |
|------|------|----------|----------|
| 已修复 2026-09-17 | `gs::preflight` 先持有 `g_mu` 再调用同样加锁的 `session_id()`，`std::mutex` 非递归 → 同线程二次加锁永久阻塞，GUI 主线程“未响应”；`connect`/`go_center`/`send_test_point`/`heartbeat`/`start_write` 五处同源 | GUI 预检、全部快捷操作、任务启动 | limit：写文本后点“预检任务”即挂死；`_t3.cpp` 直调 `gs::preflight` 8 秒超时复现；gdb attach 栈停在 `pthread_mutex_lock`。修复：`g_mu` 改为 `std::recursive_mutex`（31 处 `lock_guard` 同步替换） |
| 已确认 2026-09-17 | 跨进程 `SendMessage` 传缓冲区（WM_SETTEXT / WM_GETTEXT）会挂死 GUI 主线程 | 自动化测试脚本 | `SendMessageTimeout(WM_NULL)` 3 秒超时 + `Responding=False`；结论：只允许 `PostMessage`，禁止跨进程 `SendMessage` 传字符串 |
| 已确认 2026-09-17 | 跨进程 `PostMessage WM_CHAR` 同样触发 GUI 未响应；非 DPI-aware 进程投递 WM_LBUTTONDOWN 无法切页 | 自动化测试脚本 | 连续两次实测（PID 12600/44008）；切页改用 DPI-aware 真实点击 |
| 待修复 2026-09-17 | GUI 未限制单实例；未处理串口在任务中途被拔出；`logs/` 与 `last_task.txt` 无轮转/清理 | 长期运行稳定性 | 静态审查发现；需在实现阶段补单实例互斥与日志轮转 |
| 待确认 2026-09-17 | GUI 仅在 150% DPI 下核验；100%/其他缩放下的控件布局与导航命中未验证 | 界面可用性 | 调整系统缩放后启动 `RobotGUI.exe` 复核三页 |

## GUI 布局缺陷（2026-09-18 修复）
| 状态 | 描述 | 影响范围 | 复现步骤 |
|------|------|----------|----------|
| 已修复 2026-09-18 | 主页“设备信息”网格下标写反（`col=i/2, row=i%2`），实际排成 7 列 × 2 行；第 3 列起全部溢出面板右侧并被后绘制的右栏面板盖住，15 个字段只有 4 个可见 | 主页设备信息面板 | 打开主页即见（字段跑到面板外／被遮挡）。修复：改为 `col=i%2, row=i/2`，与同文件快捷操作按钮的写法一致 |
| 已修复 2026-09-18 | 文本宽度超出固定文本框导致截断：“输入与任务预检”→ “输入与任务…”；“最近通信”→“最近…”；“速度档／字间距／Z 偏移”→“速…／字…／Z …”；“控制器真实位置”（147px）、“存储 / 内存 / 电池”（169px）放不进 96px 标签列 | 主页、书写页、设备连接页 | 目视三页即见。修复：标题框 200→300、右栏标签 70→88、参数标签 52→76；两个超长信息标签改为 4 字（实际位置／资源占用）；“坐标依据”取值统一为“软件位姿（最后有效 ACK）”（原带时间戳的写法 384px 排不下） |
| 已修复 2026-09-18 | 书写页“任务进度”预览弧线（140×120）超出预览框并与底部“比赛要求”提示文字重叠，遮挡“自主蘸墨”等字样 | 书写页预览区 | 目视书写页即见。修复：图形区避开底部提示行（`gfxBot = pv.bottom - 28`），半径按可用高度推算 |
| 已修复 2026-09-18 | 主页“字间距／Z 偏移”输入框靠一次性标志 `s_cfgInit` 在 500ms 计时器里赋值；离开主页再返回时控件已重建而标志已置位 → 输入框永久空白 | 主页运行开关参数列 | 启动 GUI → 切到别的页 → 切回主页，两个输入框为空。修复：初值改在 `CreateHomeControls` 内随控件创建赋值，并删除一次性标志 |
| 已修复 2026-09-18 | 设备连接页底部说明条高度撑满页面剩余高度，单行文字在其中垂直居中，表现为整块琥珀色空白里浮着一行字 | 设备连接页 | 目视设备连接页即见。修复：`noteH` 上限收紧到 72px，说明条按内容成条 |

## GUI 运行时状态文件的 CWD 依赖（2026-09-18 发现，未改动）
| 状态 | 描述 | 影响范围 | 复现步骤 |
|------|------|----------|----------|
| 待确认 2026-09-18 | `load_last_task_text()` / `save_last_task_text()` / `load_window_size()` / `set_window_size()` / `cfg_load()` / `cfg_save()` 以及审计的 `logs/` 全部按进程 CWD 解析相对路径；从非项目目录启动时读不到上次任务与配置，审计日志也会静默写到别处 | 配置持久化、审计链路完整性 | 从 `D:/Code/华五` 启动 `RobotGUI.exe`（此时 CWD 不是项目目录）：书写页输入框为空、预检报“请先输入文本。”；同目录启动则正常预载“书”。修复方向：按可执行文件目录解析这些路径——涉及控制台与 GUI 共享的服务层，需先确认是否要一并改动 |

## GUI 切页卡死（2026-09-18 已修复）
| 状态 | 描述 | 影响范围 | 复现步骤 |
|------|------|----------|----------|
| 已修复 2026-09-18 | **切到“设备连接”页后整个界面卡死（UI 线程永久阻塞，CPU 0%，进程无法用 taskkill /F 结束）**。根因在 `gs::list_serial_ports()`：① `QueryDosDeviceW(nullptr, names, 4096)` 在本机返回 0 且 `GetLastError=122`（ERROR_INSUFFICIENT_BUFFER）——本机 DOS 设备名列表长 47349 字符，8KB 缓冲区根本不够，于是走进 ② 的兜底分支；② 兜底分支用 `CreateFileW("\\\\.\\COM1..32")` 逐个**打开**端口探测，而本机注册的 6 个串口 COM3/4/5/6/8/9 全是蓝牙虚拟串口（注册表 `\Device\BthModemN`），**打开未连接的蓝牙串口会在内核里阻塞**（实测 COM4 阻塞 5.2s、COM5 无限阻塞）。该函数由 UI 线程在 `CreateConnectControls` 中调用，一阻塞界面就“未响应” | 设备连接页（切页即卡死）、刷新串口按钮 | 启动 `RobotGUI.exe` → 点左侧“设备连接”：窗口立即无响应。用文件探针逐段定位到 `cc:list_ports_begin` 之后不再推进，再在 `list_serial_ports()` 内打点确认卡在 `CreateFileW("\\.\COM5")`。修复：改为**只查询、不打开端口**——`QueryDosDeviceW` 按 `ERROR_INSUFFICIENT_BUFFER` 自动扩容重试（8K→16K→32K→64K），并补充注册表 `HKLM\HARDWARE\DEVICEMAP\SERIALCOMM` 枚举；彻底删除 `CreateFileW` 探测分支。同时给 DOS 设备名加了纯数字校验，避免 “COMDB” 之类被 `_wtoi` 转成 “COM0” |
| 已修复 2026-09-18 | **点“连接设备”曾会在 UI 线程上打开串口**（与切页卡死同源）。原 `gs::connect` 在持 `g_mu` 情况下 `g_port.open()`，选到蓝牙虚拟串口时 `CreateFileW` 在内核无限阻塞 → UI 线程卡死且 `g_mu` 被长期持有、连 500ms `snapshot()` 计时器一起阻塞。修复：改为异步 `gs::connect_async(port)`——前置校验（任务运行中/重复点击）在 UI 线程持锁快速完成后，把 `g_port.open()` 放到 detach 工作线程、**打开期间不持 `g_mu`**；线程仅在写审计与结果时重新持锁。连接状态经 `snapshot()["connect"]`（`pending`/`state`/`message`）回报，UI 点击后显示“正在连接…”，由 `WM_TIMER` 轮询到 `pending=false` 时刷新结果。`g_connect_pending` 原子标志防重复点击叠加多个挂起线程。仅改 `gui_service.{h,cpp}`+`gui_win32.cpp` | 设备连接页“连接设备”按钮 | 本机 6 个端口全是蓝牙虚拟串口；改后即便选中也会立即返回、界面保持响应（结果稍后经定时器回报）。`build.bat` 重编通过，`--dryrun` 启动 `Responding=True` 并优雅退出 |
| 环境备注 2026-09-18 | 本机因上述阻塞残留了 3 个 `RobotGUI.exe` 僵尸进程（PID 36508/36732/47116，0 MB、无窗口、`taskkill /F` 报“拒绝访问”），是卡在内核态无法终止的残留，重启系统才会清掉。排查时若 `FindWindow` 找到的是这类残留窗口会误判，应先确认窗口所属 PID | 排障环境 | `tasklist /FI "IMAGENAME eq RobotGUI.exe"` 可见；用 `EnumWindows` 枚举 `RobotGuiWnd` 类窗口并打印 PID 可区分 |
| 待确认 2026-09-18 | GUI 的“运行日志记录”开关只写 `robot_config.json`（供控制台读取），GUI 自身不创建 `logs/Robot_*.log`，只写审计 JSONL；因此在 GUI 里勾选该开关看不到任何日志文件变化 | 主页运行开关、日志 | 勾选后查看 `logs/` 无新增 `Robot_*.log`；`g_logPath` 仅由控制台启动流程赋值 |

## GUI 打开后整体/文字闪烁（2026-09-18 已修复）
| 状态 | 描述 | 影响范围 | 复现步骤 |
|------|------|----------|----------|
| 已修复 2026-09-18 | 打开 GUI 后界面与文字持续闪烁。两处根因叠加：① `OnPaint` 把背景 `FillRect`、标题栏、导航、页面、页脚及全部 `Text()` 直接画在 `BeginPaint` 屏幕 DC 上，无双缓冲，而 500ms 定时器每次 `InvalidateRect(g_st.hwnd,nullptr,FALSE)` 全屏重绘 → 背景先闪再叠字；② 主窗口创建样式仅 `WS_OVERLAPPEDWINDOW`，缺 `WS_CLIPCHILDREN`，父窗全屏 `FillRect` 覆盖按钮/输入框/下拉框子控件区域，子控件随之重绘。修复：`OnPaint` 改双缓冲（`CreateCompatibleDC`+`CreateCompatibleBitmap` 画完后一次 `BitBlt` 上屏，并释放内存 DC/位图）；`CreateWindowW` 加 `WS_CLIPCHILDREN`。`WM_ERASEBKGND return 1` 与 FALSE 擦除本已正确，未动 | GUI 主页/设备连接/书写三页整体 | 打开 `RobotGUI.exe` 即见闪烁；`build.bat` 重编后 `--dryrun` 启动，`Responding=True`、收 `WM_CLOSE` 优雅退出，闪烁消除 |

## 书写顿挫（写字一顿一顿，2026-09-18 修复，待上机确认）
| 状态 | 描述 | 影响范围 | 复现步骤 |
|------|------|----------|----------|
| 已修复(离线) 2026-09-18 | 机械臂书写沿笔画一路“点刹”。根因：书法落笔段 `motion.cpp:transmitTrajectoryWithSplit` 为“逐点绝对定位 + 发完再 `sleep_move` 固定空等”，每点 `estimateMoveMs` 取 `max(POINT_RATE_LIMIT_MS=60, t_xy, DELAY=35)` 作为额外 sleep，叠加在 9600 串口单帧往返(~42ms)之上，而 ACK 只代表寄存器收到不代表到位——机械臂在每 0.6mm 途经点减速到停、干等、再起步。全项目原无任何插补/前瞻/轨迹缓冲 | 真机与 dryrun 书写节拍、观感与耗时（单字数千点→数分钟） | 修复：方案A 补偿式计时——`estimateMoveMs` 改为返回“相邻指令目标间隔”，去掉 60/35ms 走停地板（仅留 `MIN_POINT_INTERVAL_MS=12` 下限），发送循环用 `wait_after_send(t0,…)` 从发送前时刻补偿等待到目标间隔（串口+运动耗时计入），拐角停顿折进间隔；保留抬落笔/Z 沉降等物理停顿。方案B——书法落笔段用 `resamplePolylineRDP`（Ramer–Douglas–Peucker，容差 `RESAMPLE_DEV_MM_CALLI=0.06mm`）误差有界抽稀，直段塌成两端点、弯曲与拐角保点；描边段仍定步长 1.2。**已离线验证**（`build.bat` RC=0 + 单测）：直线 121→2 点偏差0；R2~200mm 弧最大偏差恒≤0.06且急弯更密；纯点间隔 60→12ms。**未验证(须上机)**：控制器对相邻指令是到点全停还是可运动中接新目标连续走/缓冲，决定 A 的顺滑上限；9600 单帧 ~22ms 为硬吞吐天花板；方案C（书法走 batch7 队列）待真机小样。上机前空载/限位/手触急停 |
| 已否决 2026-09-18 | 方案B 曾用“转角自适应步长”（`resamplePolylineAdaptive`，按相邻点夹角在 min~max 取步长）：曲线探针证伪——HanziWriter 笔顺中线逐点转角极小，30mm 乃至 2mm 半径急弯的密采样点被判成“几乎直线”，仍按 ~2mm 大步长抽稀，会损失笔形保真 | 书写保真度 | 已删除该函数，改用误差有界的 RDP（方案B 现实现）。若日后要“直段稀疏+曲线致密”之外的需求，勿再走逐点夹角启发式，应以几何偏差为准 |

## 实时轨迹 / 四角标定（2026-09-19，须上机验证）
| 状态 | 描述 | 影响范围 | 复现步骤 |
|------|------|----------|----------|
| 待上机 2026-09-19 | `readPose` 用 0x03 读 0x0008×5，读回值语义未确认——是控制器实际位置、还是最后写入的目标值（若是后者，实测点=下发点，绿点叠加失去意义）。手册 6.1 只说“对坐标进行读操作”，未界定 | 实测点叠加的可信度、方向/丢步诊断 | 真机连接后：预览某角→readPose 看读回是否等于刚下发点；手动挪臂（若有 jog）后读回是否变化 |
| 待上机 2026-09-19 | `preview_corner` 抬笔移到角点用固定书写平面 Z（`g_writing_plane_z`），修“Y=-100 到不了”（原用 Z_UP 顶部 XY 收窄）。真实可达边界、以及移动路径是否会蹭纸，均须空载实测 | 四角预览可用性、真机安全 | 真机空载：逐角预览，确认能到位且不撞限位/蹭纸；手触急停 |
| 已验证(dryrun) 2026-09-19 | 四角标定 UI：角标签完整、值回填、连线绕质心极角排序成矩形（非八字）、提示不压字——`build.bat` RC=0 + `RobotGUI --dryrun` 截图核验 | 书写页四角面板 | 开 GUI→书写任务→看实时轨迹面板。注：DRYRUN 不移动臂、无实测绿点，仅能验 UI 与几何 |

## 分页 + 翻页（2026-09-20，须上机验证）
| 状态 | 描述 | 影响范围 | 复现步骤 |
|------|------|----------|----------|
| 待上机 2026-09-20 | 翻页走纸量假设“精确=版面高、下一页字块与当前页世界坐标重合”（用户确认）。若真机卷筒打滑/走纸量偏差，第 2 页起整页会累积偏移 | 多页书写落点准确性 | 真机两页：第 1 页写完翻页后，量第 2 页实际落点与预览框偏差；若偏则回头加“翻页偏移”参数（当前未做） |
| 待上机 2026-09-20 | 翻页后软件位姿 `g_last_pose` 不变（坐标重合前提下成立）；真机拖纸后臂的**物理**位置未变但**纸**动了，故复用坐标正确——前提须真机验证拖纸机构不动臂、且臂在翻页时已抬笔到安全高度 | 翻页安全、多页衔接 | 真机空载两页：翻页瞬间观察臂是否抬笔、拖纸时笔尖是否蹭纸 |
| 待接入 2026-09-20 | 翻页当前为**模拟等待**（`page_turn_wait_locked` 分片睡眠 `page_turn_wait_ms`），未接真实蓝牙模块。真实实现须在 `gui_win32.cpp::Run()` 锚点 `gs::set_page_turn_handler(真实信号+等完成回调)`；回调返回 false=翻页失败→任务 failed(page_turn) | 比赛真机翻页 | 蓝牙模块就绪后替换 handler；协议/超时/失败重试待定 |
| 已验证(dryrun+单测) 2026-09-20 | 翻页等待期点停止/急停立即中止不翻页（用户选定安全语义）：`verify_pages` 7.4 设 60s 等待、进入翻页阶段后 abort，实测 <5s 退出、停在第 1 页、第 2 页不写、error=estop | 翻页期安全 | 见 verify_pages.cpp 7.4（临时文件已移出项目，存 workspace .bak） |

## 蓝牙翻页闭环验收 —— 2026-09-20 深夜（状态码定因，勿再凭 OpenAsync 猜）
| 状态 | 描述 | 影响范围 | 复现步骤 |
|------|------|----------|----------|
| **✓ 已闭环 2026-09-21**（2026-09-20 定因） | **真正原因是「模块在空口上不可达」＝模块没上电**——**9-21 用户上电后一次跑通，软件零改动**，定因得到验证。，不是软件、不是程序占用。三重证据：①`bleak` 主动扫描 14s / 25s 两次都扫不到它（只扫到 4 个无关设备）；②`poc_winrt.exe`：`RequestAccessAsync=Allowed`、`OpenAsync status=1 Success`，但每次特征枚举耗 **7.75 s** 且 `GattCommunicationStatus=1 Unreachable`；③`ble_selftest.exe` 诊断 `[link=0 open=1 comm=1 n=0]`，`link=0` 即 `ConnectionStatus=Disconnected`（排除“幽灵链路”）。另：主板**未接本机**（无任何串口设备）。 | 翻页真机验收 | 给模块上电后先跑 `tmp/ble_poc/scan.py`，目标应出现在列表里 |
| 已修 2026-09-20 | `ble_motor.cpp` 原先把 `Unreachable` 笼统报成“找不到 FFE1 特征（模块未就绪或服务未广播）”，**把排查方向带偏（往“服务没广播”查，实际是没上电）**。现按 `GattCommunicationStatus` 分诊并在信息里附 `[link= open= comm= n=]`。 | 故障定位效率 | 看 `ble_motor.cpp` `wr_open()` 尾部；跑 `ble_selftest.exe` 即可见 |
| 误判留档 2026-09-20 | **不要只看 `OpenAsync` 就下结论**：它可能从本地缓存返回 `Success`，而空口根本没连上（本次就发生过）。必须同时看 `link`（ConnectionStatus）与 `comm`（GattCommunicationStatus）。 | 排查方法 | — |
| 环境提醒 2026-09-20 | 本机 `bleak` 3.0.2 **只装在系统 Python**（`C:\Users\zby\AppData\Local\Programs\Python\Python313\python.exe`），WorkBuddy 隔离 venv 里没有 → 跑 `scan.py`/`ref_bleak.py` 要用系统解释器。 | 参照测试 | `pip list` 查 `bleak` |

## 蓝牙翻页验收通过留档 —— 2026-09-21
| 状态 | 描述 | 影响范围 | 复现步骤 |
|------|------|----------|----------|
| ✓ 已通过 2026-09-21 | 真机闭环：`scan.py` 15s 扫到 `21:F6:47:3A:D8:89 rssi=-65 HC-05`；`ble_selftest.exe` → 建链 **2.30 s** → 发 `RUN30,3000` → **3.33 s** 收到 `DONE 30 3000`，EXIT=0。 | 翻页真信号链路 | 模块上电后跑 `tmp/ble_poc/ble_selftest.exe` |
| 判据留档 | **可达时建链约 2.3 s；不可达时约 25 s 才报错**（`comm=1 Unreachable`）。两条时间量级差 10 倍，可当作「模块在不在」的快速旁证。 | 快速排障 | 对比 `ble_selftest.exe` 的 `[2]` 耗时 |

## 蓝牙「扫描假阴性」——模块已被 Windows 攥住链路（2026-09-21 晚，实锤）

| 状态 | 描述 | 影响范围 | 复现步骤 / 判据 |
|------|------|----------|----------|
| **已定位·判据修正** | 模块**与 PC 已配对**（`HKLM\SYSTEM\CurrentControlSet\Services\BTHPORT\Parameters\Devices` 下有 `21f6473ad889` 链接密钥记录）。对已配对的 BLE 设备 **Windows 会常驻保持 LE 链路**；而 BLE 外设**一旦被连上就停止广播** → **`BleakScanner` 扫不到、bleak 按地址连报 `DeviceNotFound(30s)`，可 WinRT 按地址 0.06s 拿到设备、0.36s 枚举完 GATT、写特征与收通知全部正常、电机真转。** | 「判模块在不在」的全部诊断 | 跑 `tmp/ble_poc/winrt_probe.py`：应见 `connection_status = Connected (1)`、`name='HC-05'`、Uncached 服务 3 个（1800/1801/FFE0） |
| **判据（背下来）** | **扫描阳性 ⇒ 可信；扫描阴性 ⇒ 不可信。** 决定性判据是「能否拿到设备对象 / 建链多快」：拿到 = 在（`Connected`/`Disconnected` **都算在**）；返回 `null` = 不在。建链耗时：链路已存在 **0.1~0.6s** ／ 刚在广播 **2~3s** ／ 不在 **空等 25s**。 | 排障 | 见上 |
| **纠正 2026-09-20 的结论** | 当时把「`scan.py` 扫不到 ⇒ 模块没上电」当成定论并写进了技能与清单。**9-21 晚出现真实反例**：扫不到，但 WinRT 直连 **0.56s** 就跑通闭环。→「扫不到」只能说明**空口上没在广播**，**不能**推断不在。 | 方法论 | 对比本节与上一节 |
| **想恢复广播** | Windows「设置 → 蓝牙和其他设备」里把 `HC-05X` **删除设备**（取消配对）→ 立刻恢复广播；或给模块断电重启（只那次有效，之后还会被重新连上）。 | 手机 App 要连它时 | — |
| **非缺陷·须知晓** | **`g_dryRun` 管不到翻页**：翻页走 BLE，不是串口。所以「Dry Run + 勾蓝牙翻页」= 机械臂不动、**电机真转**。 | Dry Run 语义 | 勾 Dry Run + 勾蓝牙翻页，写两页 → 电机转 3s |

- 新增工具：`tmp/ble_poc/winrt_probe.py`（Python `winrt-*` 直读链路状态，**不发运动指令**）。
- 注：该枚举 `GattCacheMode` 在 Python 投影里没导出名字，`get_gatt_services_with_cache_mode_async` 直接传数值 **1**（=Uncached）。
- 本机 `bleak` 3.0.2 装在系统 Python，`winrt-*` 是它的依赖 → 因此系统 Python 里也能直接调 WinRT。
## UI 子控件跨页残留：按钮「跑到别的页上」（2026-09-21 晚 已修）

| 状态 | 描述 | 影响范围 | 复现步骤 |
|---|---|---|---|
| **已修** | `MakeBtn()` 用 `g_btns[64]`（下标 = 控件ID − 1000，且有 `slot < 64` 判断）登记按钮句柄，`DestroyPageControls()` 只销毁这个数组里的句柄。**ID 一旦 ≥ 1064 就登记不上 → 永远销毁不掉**。当前枚举里 `IDC_BTN_PAGE_NEXT = 1064`（▶ 下一页）、`IDC_BTN_BLECONN = 1067`（连接蓝牙）正好越界。 | 全部页面：这两个按钮会盖在其它页面上（用户看到的「按键错位、出现在不该出现的页」） | 打开 GUI → 进「书写任务」→ 切到「主页 / 设备连接 / 书写平面」→ ▶ 与「连接蓝牙」仍在原坐标；**每次切页还会再攒一对**（旧进程实测主页从 33→49→51 个直接子控件） |

判据（跨进程实测，`tmp/ble_poc/probe_children.exe` 枚举直接子控件）：

| 页面 | 旧版子控件数 | 修好后 |
|---|---|---|
| 主页 | 49 → 51（越切越多） | **19** |
| 设备连接 | 35 | **5** |
| 书写任务 | 63 → 65 | **35** |
| 书写平面 | 33 | **3** |

修法（`gui_win32.cpp`）：
- 删掉 `g_btns[64]` 记账，`DestroyPageControls(HWND)` 改为**枚举主窗口的直接子窗口并全部销毁**（先收集再销毁；只取直接子窗口，不递归以免动到 COMBOBOX 内部的编辑框/列表框）。新增控件自动覆盖，不会再漏。
- 顺带修 `DrawBtn()` 里同源的 `slot < 64` 判断：它让 `IDC_BTN_BLECONN` 跳过整个 switch、拿到默认 BLUE 而不是 TEAL。
- 顺带按**实测字宽**修「蓝牙翻页」行尺寸：`连接蓝牙` 需要 80px，原按钮只有 70px → 被 `DT_END_ELLIPSIS` 截成「连接…」；`档位(0~50)：` 需 115px（原 56）、`时长(ms)：` 需 95px（原 86）也都在截断。

### ★探针工具的两个必知坑（下次别再踩）
1. **探针必须 `SetProcessDPIAware()`**：本程序调了 `SetProcessDPIAware()`，窗口坐标是物理像素；DPI 不感知的探针拿到的坐标会被 Windows 虚拟化（本机实测缩到 **2/3**），不仅位置偏小，**模拟点击还会落到错位的导航项上**，把「没残留」误判成「有残留」。
2. **控制台打印中文会乱码**：探针结果写 UTF-8 文件再读，别指望 stdout。
