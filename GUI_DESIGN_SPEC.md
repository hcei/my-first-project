# 书画机械臂调试助手 GUI 设计文档

## 1. 交付范围

本文件与 `gui_design_mockup.html` 配套，描述静态 GUI 原型的页面结构、状态边界、日志协议和 `robot_cli.exe` 方向。原型只用于确认布局和信息层级，不连接真实设备，不执行运动。

本轮 HTML 原型只覆盖前三个页面：设备总览、设备连接、书写任务。后文列出的其他页面属于后续 GUI 规划和功能映射，不代表已经在 HTML 中制作完成。

目标窗口比例参考用户提供的 1494 x 1204 截图，不要求移动端或窄屏适配。

## 2. 视觉与布局

- 顶部紫色窗口标题栏，标题为“书画机械臂调试助手”。
- 左侧固定导航，宽度约 212 px。
- 主内容使用浅灰背景、白色面板、细蓝灰边框和较小圆角。
- 紫色用于品牌和状态强调，浅蓝色用于常规操作，青绿色用于安全可执行操作，红色只用于急停、停止和错误。
- 首页优先展示连接状态、最后已知位姿、当前软件模式、任务状态、最近 ACK 和运行开关。
- 所有页面保持同一左侧导航与底部状态栏。

## 3. 页面清单

### 3.0 本轮 HTML 原型覆盖范围

`gui_design_mockup.html` 当前只提供以下三个静态页面，用于确认桌面窗口比例、导航层级、信息密度和核心工作流布局：

1. 设备总览。
2. 设备连接。
3. 书写任务。

页面中的状态、时间戳、TX/RX、ACK 和日志内容均为静态模拟数据。

### 3.1 主页 / 设备总览

展示：

- 串口连接状态、端口、Modbus RTU 参数。
- 最后一次有效 ACK 后的软件位姿：X、Y、Z。
- 位姿来源标记：软件目标/最后有效 ACK；不得标成“真实当前位置”。
- 当前软件阶段：空闲、预热、预定位、写字、绘画、蘸墨、复位、急停。
- 当前任务、字符进度、轨迹进度、笔状态。
- Dry Run、自动描边、高质模式、蘸墨功能、速度档、字间距。
- Z 层、Z 行程、中心点和 SAFE 区摘要。
- 快捷操作：复位、回中心点、测试点、查询最后已知位姿、刷新设备、急停。

### 3.2 设备连接

只模拟前三项连接功能：

- 串口选择。
- 连接/断开。
- 刷新串口。

同时展示最近通信时间、ACK 状态、连续失败次数、读取超时和最近连接错误。

### 3.3 书写任务

对应主菜单“写字并自动 2D 描边”的实际流程：

- 输入汉字或诗句。
- 任务预检。
- 显示字号、布局、速度、估算时长、字形数据状态。
- 开始、停止和保存任务配置。
- 展示字符、轨迹点、阶段、笔状态和蘸墨状态。

### 3.4 后续页面规划（本轮未制作）

以下页面用于承接 `robot.exe` 的完整功能，不属于当前 HTML 原型：

#### 2D 描边

对应主菜单“2D 描边模式”：

- 选择或显示线稿 JSON。
- 显示线稿路径数量和轨迹点数。
- 显示批量 7 点发送、失败回退逐点的发送策略。
- 轨迹预览、开始和停止。

#### 蘸墨流程

对应蘸墨位设置和蘸墨开关：

- 显示蘸墨位 X/Y/Z 和有效性。
- 显示两次蘸墨、十字抖动、5 次上下抖的流程。
- 显示上慢下快速度策略。
- 设置、测试、启用、停止。

#### 人工调试

对应现有调试子菜单：

1. 单点发送。
2. L 形方向测试。
3. Z 轴步进标定。
4. 抬落笔循环。
5. 方框批量测试。
6. 急停演练。
7. 查询最后已知位姿。
8. 循环单点。

另设独立急停入口。人工调试产生的日志必须记录 `source=GUI`、`actor=HUMAN`。

#### Z 层与安全区

对应中心点、Z 层校准和 Z 行程范围设置：

- Z 上限/下限。
- 抬笔、中位、预压、轻触、常规书写、重压。
- Z_OFFSET。
- 中心点。
- 设备 XY 极限和 SAFE 区。
- 标定来源和验证状态。

#### 运行日志

实时列表展示每次 GUI 或 CLI 操作，至少包括：

```text
timestamp
source              GUI | CLI
actor               HUMAN | AGENT
session_id
request_id
operation_id
mode
operation
parameters
software_pose
target_pose
tx_frame
rx_frame
device_response
ack_valid
result
retry_count
error_code
```

原有文本日志可以继续保留，但 GUI/CLI 新接口应优先写结构化 JSONL，确保 agent 能准确区分调试方并复盘请求。

## 4. 已确认的状态能力

### 当前代码可直接提供

- 串口打开、关闭结果。
- Dry Run 状态。
- 当前配置：速度档、字间距、自动描边、高质模式、蘸墨开关。
- 中心点、蘸墨位、Z 层和 Z 行程范围。
- SAFE 区和设备 XY 软件边界。
- 最近 TX 帧。
- 最近 RX 帧。
- ACK 是否通过长度、CRC、设备地址、功能码和寄存器范围校验。
- 最后一次成功发送点或批量最后一点的软件位姿。
- 急停软件标志。
- 写字、绘画、蘸墨、预热、预定位等可由控制流程推断的阶段。
- 当前字符、轨迹点和批次进度，前提是任务执行器向状态模型报告进度。

### 当前代码不能确认

- 控制器真实当前位置回报。
- 独立的控制器运行模式寄存器。
- 电池、电量、存储、运行内存。
- 控制器报警码。
- 硬件限位状态。
- 硬件急停锁存和人工复位语义。

这些字段可以在 GUI 中显示为“未接入/协议待确认”，不能用软件位姿或 ACK 推断为真实硬件状态。

## 5. GUI 与 CLI 边界

### GUI

- 面向人工调试。
- 提供可视化连接、配置、任务、状态、日志和急停。
- 所有动作写入 `source=GUI`、`actor=HUMAN`。
- GUI 不直接解析控制器私有状态；通过统一控制服务/状态模型读取。

### CLI

建议保留底层控制逻辑，新增非交互入口并输出机器可读 JSON：

```text
robot_cli.exe status --port COM3 --json
robot_cli.exe debug point --x 0 --y 0 --z -325 --pen-up --json
robot_cli.exe debug pose --json
robot_cli.exe debug direction --json
robot_cli.exe debug z-calibrate --lift-z -325 --write-z -385 --json
robot_cli.exe debug pen-cycle --json
robot_cli.exe debug square-batch --json
robot_cli.exe debug estop --json
robot_cli.exe task preflight --text "..." --json
robot_cli.exe task write --text "..." --json
robot_cli.exe task draw --file themes/example.json --json
robot_cli.exe config show --json
robot_cli.exe config set --key speed --value 3 --json
```

建议：

- 原 `Robot.exe` 可暂时保留作为兼容入口。
- 新入口命名为 `robot_cli.exe`。
- 不建议让 agent 依赖交互式菜单或控制台提示。
- `stdout` 输出 JSON 结果，`stderr` 输出诊断文本。
- 退出码表示成功、参数错误、设备错误、急停或预检失败。
- 每次请求带 `request_id`；每次设备动作带 `operation_id`。
- CLI 与 GUI 复用同一套配置、运动安全校验、日志和状态快照。

## 6. 后续实现顺序

1. 抽取统一 `DeviceStateSnapshot`、`OperationEvent` 和 `AuditLogEvent` 数据结构。
2. 将串口 TX/RX、ACK 校验、重试、软件位姿更新接入结构化日志。
3. 把控制台菜单动作包装成可复用服务函数。
4. 新增 `robot_cli.exe` 非交互命令解析和 JSON 输出。
5. GUI 调用同一服务层，避免 GUI 和 CLI 各自复制运动逻辑。
6. 等协议确认后再接入真实位置、报警和限位寄存器。
7. 真机验证急停锁存、ACK 丢失重放风险、批量协议末批语义和 Z 行程。

## 7. 原型限制

HTML 中的状态、时间戳、TX/RX 和日志均为静态示例。它们用于对齐界面信息层级，不代表当前设备实时值，也不代表控制器已经支持相应寄存器。
