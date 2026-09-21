// ble_motor.h — 蓝牙翻页电机链路（BLE GATT 透传）
//
// 背景与选型（实测结论，勿再走回头路）：
//   * 本机这块 "HC-05" 实为 BLE 模块（固件 hc05V2.3_le），Windows 永远不建 COM 口，
//     所以不能用串口；走 GATT 透传：服务 FFE0 / 特征 FFE1（write + notify）。
//   * Win32 老 API（BluetoothGATT*）不能用：读走缓存能成功，但
//     BluetoothGATTSetCharacteristicValue / BluetoothGATTRegisterEvent 一律立刻返回
//     E_FAIL(0x80070001)——它无法接管 WinRT 建立的链路。
//   * 因此【全部用 WinRT】：建链用 BluetoothLEDevice.FromBluetoothAddressAsync，
//     收发用 IGattCharacteristic3::WriteValueWithResultAsync + IGattCharacteristic::add_ValueChanged。
//
// 线程模型：
//   所有 WinRT 调用都收敛到本模块内部唯一的工作线程（进 MTA），
//   外部（GUI 线程 / 任务线程）只通过 exec() 提交请求并等待结果，
//   避免 WinRT 单线程套间问题，也避免 GUI 被长阻塞。
//
// 闭环语义：
//   exec("RUN30,3000", "DONE", ...) —— 发指令 → 等板子回 DONE（真回包，不是定时器猜测）。
//   板子的 OK/ERR 也会被捕获：见 ERR 前缀立即失败。
#pragma once

#include <string>
#include <functional>

namespace blem {

// ---------------- 生命周期（进程内一次） ----------------
void start();                 // 起后台工作线程（幂等；可在 Run() 里调用）
void stop();                  // 停止工作线程并断开链路
bool available();             // 本机 WinRT 是否可用（start 后有效）

// ---------------- 配置（线程安全，随时可改） ----------------
void set_address_hex(const std::string& addr);   // 12 位十六进制，如 "21F6473AD889"
std::string address_hex();
void set_connect_timeout_ms(int ms);             // 建链超时（默认 30000）
void set_reply_timeout_ms(int ms);               // 等 DONE 的兜底超时（默认 8000）

// ---------------- 状态（GUI 轮询用；纯 ASCII 文本） ----------------
enum class St { Stopped, Disconnected, Connecting, Ready, Error };
St state();
const char* state_text();      // "未启动"/"未连接"/"连接中"/"已连接"/"异常"（UTF-8）
std::string last_error();      // 最近一次失败原因（UTF-8，纯 ASCII 内容）
std::string last_reply();      // 最近一次收到的整行回包

// ---------------- 连接控制（非阻塞，结果看 state()） ----------------
void request_connect();
void request_disconnect();

// ---------------- 核心：发一行并等待期望回包（闭环） ----------------
// line     : 不含 \r\n 的指令，须纯 ASCII，如 "RUN30,3000"
// want     : 期望出现的子串（如 "DONE"）；传空 = 只发不等回包
// timeout_ms: 等待 want 的毫秒数
// canceled : 可选中止回调（每 ~15ms 询问一次）；返回 true 立即放弃
// got      : 命中行（如 "DONE 30 3000"）
// err      : 失败原因（UTF-8）
// 返回 true 表示收到期望回包（闭环成立）
bool exec(const std::string& line, const std::string& want, int timeout_ms,
          const std::function<bool()>& canceled,
          std::string& got, std::string& err);

}   // namespace blem
