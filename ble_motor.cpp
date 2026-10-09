// ble_motor.cpp — BLE 翻页链路实现（纯 WinRT，单工作线程）
//
// 详见 ble_motor.h 顶部的选型说明。这里只记几个实现上的硬约束：
//   1) MinGW 的 MIDL 头里，运行类（GattCharacteristic 等）是 opaque 的，统一按
//      IInspectable* 承接再 QueryInterface 到 IXxx 接口。
//   2) IAsyncOperation<X*> 在 ABI 上等价于 IAsyncOperation<IX*>，因此异步等待一律
//      轮询 IAsyncInfo::get_Status，不实现任何 COM 委托。
//   3) 通知委托 ITypedEventHandler<GattCharacteristic*, GattValueChangedEventArgs*>
//      经 AggregateType 展开后，Invoke 的实参是【接口】：
//         Invoke(IGattCharacteristic*, IGattValueChangedEventArgs*)
//      —— 写成运行类会 override 不上，这是最容易踩的坑。
//   4) 特征枚举必须用 GetCharacteristicsWithCacheModeAsync(Uncached)：
//      服务缓存可能是冷的（返回 0 个特征）。
//   5) 一定要 OpenAsync(SharedReadAndWrite)：只做 RequestAccessAsync 不够。

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#define WINVER       0x0A00
#define _WIN32_WINNT 0x0A00
#define NTDDI_VERSION 0x0A000000

#include "ble_motor.h"

#include <sdkddkver.h>
#include <windows.h>
#include <roapi.h>
#include <winstring.h>
#include <inspectable.h>
#include <asyncinfo.h>
#include <robuffer.h>
#include <windows.foundation.h>
#include <windows.foundation.collections.h>
#include <windows.storage.streams.h>
#include <windows.security.cryptography.h>
#include <windows.devices.bluetooth.h>
#include <windows.devices.bluetooth.genericattributeprofile.h>
#include <windows.devices.enumeration.h>

#include <atomic>
#include <condition_variable>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace ABI::Windows::Foundation;
using namespace ABI::Windows::Foundation::Collections;
using namespace ABI::Windows::Devices::Bluetooth;
using namespace ABI::Windows::Devices::Bluetooth::GenericAttributeProfile;
using namespace ABI::Windows::Storage::Streams;
using namespace ABI::Windows::Security::Cryptography;

namespace blem {

// ================================================================ WinRT 小工具

namespace {

// DEFINE_GUID 在未定义 INITGUID 时只有声明 → 自带所需 IID
const GUID kIID_IAsyncInfo =
    { 0x00000036, 0x0000, 0x0000, { 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 } };
const GUID kIID_IBufferByteAccess =
    { 0x905a0fef, 0xbc53, 0x11df, { 0x8c, 0x49, 0x00, 0x1e, 0x4f, 0xc6, 0x86, 0xda } };

const GUID kUuidFfe0 =
    { 0x0000ffe0, 0x0000, 0x1000, { 0x80, 0x00, 0x00, 0x80, 0x5f, 0x9b, 0x34, 0xfb } };
const GUID kUuidFfe1 =
    { 0x0000ffe1, 0x0000, 0x1000, { 0x80, 0x00, 0x00, 0x80, 0x5f, 0x9b, 0x34, 0xfb } };

HSTRING mk_hstr(const wchar_t* s)
{
    HSTRING h = nullptr;
    WindowsCreateString(s, (UINT32)wcslen(s), &h);
    return h;
}

// opaque 运行类指针 → 目标接口（失败返回 nullptr；opaque 指针仍需自行 Release）
template <class TIface>
TIface* as_iface(void* opaque)
{
    if (!opaque) return nullptr;
    TIface* out = nullptr;
    HRESULT hr = reinterpret_cast<IInspectable*>(opaque)->QueryInterface(__uuidof(TIface), (void**)&out);
    return SUCCEEDED(hr) ? out : nullptr;
}

void rel_opaque(void* opaque)
{
    if (opaque) reinterpret_cast<IUnknown*>(opaque)->Release();
}

// 轮询等待异步完成（不做 COM 委托）；out 可为 nullptr
template <class TRes>
HRESULT async_wait(IAsyncOperation<TRes>* op, TRes* out, int timeout_ms)
{
    if (!op) return E_POINTER;
    IAsyncInfo* info = nullptr;
    HRESULT hr = op->QueryInterface(kIID_IAsyncInfo, (void**)&info);
    if (FAILED(hr) || !info) return FAILED(hr) ? hr : E_NOINTERFACE;

    DWORD t0 = GetTickCount();
    hr = E_PENDING;
    for (;;) {
        AsyncStatus st = Started;
        HRESULT h2 = info->get_Status(&st);
        if (FAILED(h2)) { hr = h2; break; }
        if (st != Started) {
            if (st == Completed) hr = out ? op->GetResults(out) : S_OK;
            else {
                HRESULT ec = E_FAIL;
                info->get_ErrorCode(&ec);
                hr = (st == Canceled) ? HRESULT_FROM_WIN32(ERROR_CANCELLED) : ec;
            }
            break;
        }
        if ((int)(GetTickCount() - t0) > timeout_ms) {
            hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
            info->Cancel();
            break;
        }
        Sleep(15);
    }
    info->Release();
    return hr;
}

bool buffer_bytes(IBuffer* buf, const uint8_t** data, uint32_t* len)
{
    if (!buf) return false;
    Windows::Storage::Streams::IBufferByteAccess* acc = nullptr;
    if (FAILED(buf->QueryInterface(kIID_IBufferByteAccess, (void**)&acc)) || !acc) return false;
    byte* p = nullptr;
    bool ok = SUCCEEDED(acc->Buffer(&p));
    acc->Release();
    if (!ok) return false;
    UINT32 n = 0;
    if (FAILED(buf->get_Length(&n))) return false;
    *data = p;
    *len = n;
    return true;
}

// ---------------------------------------------------------------- 接收行缓冲

// 通知回调发生在 WinRT 线程，只往队列里塞；工作线程再取。
struct RxQueue {
    std::mutex               mu;
    std::string              partial;
    std::deque<std::string>  lines;

    void feed(const uint8_t* d, uint32_t n)
    {
        std::lock_guard<std::mutex> lk(mu);
        partial.append((const char*)d, n);
        size_t p;
        while ((p = partial.find('\n')) != std::string::npos) {
            std::string ln = partial.substr(0, p);
            partial.erase(0, p + 1);
            if (!ln.empty() && ln.back() == '\r') ln.pop_back();
            if (!ln.empty()) lines.push_back(ln);
        }
        if (partial.size() > 512) partial.clear();   // 无换行兜底
    }
    bool pop(std::string& out)
    {
        std::lock_guard<std::mutex> lk(mu);
        if (lines.empty()) return false;
        out = lines.front();
        lines.pop_front();
        return true;
    }
    void clear()
    {
        std::lock_guard<std::mutex> lk(mu);
        lines.clear();
        partial.clear();
    }
};

RxQueue g_rx;

// ---------------------------------------------------------------- 通知委托
//
// ★ Invoke 的实参是【接口类型】（AggregateType 展开），不是运行类。
class ValueChangedHandler
    : public ITypedEventHandler<GattCharacteristic*, GattValueChangedEventArgs*>
{
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        typedef ITypedEventHandler<GattCharacteristic*, GattValueChangedEventArgs*> Self;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, __uuidof(Self))) {
            *ppv = static_cast<Self*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&ref_); }
    ULONG STDMETHODCALLTYPE Release() override
    {
        LONG n = InterlockedDecrement(&ref_);
        if (n == 0) delete this;
        return (ULONG)n;
    }
    HRESULT STDMETHODCALLTYPE Invoke(IGattCharacteristic* /*sender*/,
                                     IGattValueChangedEventArgs* args) override
    {
        if (!args) return S_OK;
        IBuffer* buf = nullptr;
        if (FAILED(args->get_CharacteristicValue(&buf)) || !buf) return S_OK;
        const uint8_t* p = nullptr;
        uint32_t n = 0;
        if (buffer_bytes(buf, &p, &n) && n) g_rx.feed(p, n);
        buf->Release();
        return S_OK;
    }
protected:
    virtual ~ValueChangedHandler() {}
private:
    LONG ref_{ 1 };
};

// ================================================================ 共享状态

std::mutex              g_qMu;          // 请求队列
std::condition_variable g_qCv;
bool                    g_quit = false;
bool                    g_reqConnect = false;
bool                    g_reqDisconnect = false;

std::mutex              g_stMu;         // 状态/文本
std::condition_variable g_stCv;
bool                    g_initialized = false;
bool                    g_avail = false;
St                      g_state = St::Stopped;
std::string             g_err;
std::string             g_reply;
std::string             g_addrHex = "21F6473AD889";
int                     g_connTimeout = 30000;
int                     g_replyTimeout = 8000;

std::thread             g_worker;
// 注意：g_worker 一律 detach，绝不留下可 join 的线程。
// 否则进程退出时它的析构会在静态销毁阶段触发 std::terminate —— 表现就是
// 关闭窗口时打印 "terminate called without an active exception" 并崩溃。
// 优雅收尾改用下面的 g_started / g_workerDone 配合 stop() 显式完成。
bool                    g_started = false;    // 受 g_stMu 保护
bool                    g_workerDone = false; // 受 g_stMu 保护

// —— 以下仅工作线程访问 ——
IBluetoothLEDevice*     w_dev = nullptr;
IGattDeviceService*     w_svc = nullptr;
IGattCharacteristic*    w_chr = nullptr;
ValueChangedHandler*    w_handler = nullptr;
EventRegistrationToken  w_tok{};

struct Job {
    std::string            line, want, got, err;
    int                    timeout_ms = 0;
    std::function<bool()>  canceled;
    bool                   ok = false;
    bool                   done = false;
    std::mutex             mu;
    std::condition_variable cv;
};
std::shared_ptr<Job>    g_job;          // 待执行（受 g_qMu 保护）

void set_state(St s, const std::string& err = std::string())
{
    std::lock_guard<std::mutex> lk(g_stMu);
    g_state = s;
    if (!err.empty()) g_err = err;
    if (s == St::Ready) g_err.clear();
    g_stCv.notify_all();
}

void set_reply(const std::string& line)
{
    std::lock_guard<std::mutex> lk(g_stMu);
    g_reply = line;
}

// 结束一个 job
void finish(const std::shared_ptr<Job>& j, bool ok, const std::string& got, const std::string& err)
{
    {
        std::lock_guard<std::mutex> lk(j->mu);
        j->ok = ok;
        j->got = got;
        j->err = err;
        j->done = true;
    }
    j->cv.notify_all();
}

// 记录并结束失败
void fail(const std::shared_ptr<Job>& j, const std::string& err)
{
    set_state(St::Error, err);
    finish(j, false, std::string(), err);
}

// ================================================================ 工作线程内部操作

void wr_close()
{
    if (w_chr && w_handler) w_chr->remove_ValueChanged(w_tok);
    if (w_handler) { w_handler->Release(); w_handler = nullptr; }
    if (w_chr)     { w_chr->Release();     w_chr = nullptr; }
    if (w_svc)     { w_svc->Release();     w_svc = nullptr; }
    if (w_dev)     { w_dev->Release();     w_dev = nullptr; }
}

// 把 12 位十六进制串解析成 UINT64 蓝牙地址
bool parse_addr(const std::string& s, uint64_t& out)
{
    uint64_t v = 0;
    int n = 0;
    for (char c : s) {
        if (c == ':' || c == '-' || c == ' ') continue;
        int d = (c >= '0' && c <= '9') ? c - '0'
              : (c >= 'a' && c <= 'f') ? c - 'a' + 10
              : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
        if (d < 0) return false;
        v = (v << 4) | (uint64_t)d;
        if (++n > 12) return false;
    }
    if (n != 12) return false;
    out = v;
    return true;
}

void wr_open()
{
    wr_close();
    set_state(St::Connecting);
    g_rx.clear();

    std::string addr;
    {
        std::lock_guard<std::mutex> lk(g_stMu);
        addr = g_addrHex;
    }
    uint64_t addrVal = 0;
    if (!parse_addr(addr, addrVal)) { set_state(St::Error, "蓝牙地址格式不对（需 12 位十六进制）"); return; }

    int connMs;
    {
        std::lock_guard<std::mutex> lk(g_stMu);
        connMs = g_connTimeout;
    }

    // ---- 1) 建链 ----
    HSTRING cls = mk_hstr(L"Windows.Devices.Bluetooth.BluetoothLEDevice");
    IBluetoothLEDeviceStatics* statics = nullptr;
    HRESULT hr = RoGetActivationFactory(cls, __uuidof(IBluetoothLEDeviceStatics), (void**)&statics);
    WindowsDeleteString(cls);
    if (FAILED(hr) || !statics) { set_state(St::Error, "取 BluetoothLEDevice 工厂失败"); return; }

    IAsyncOperation<IBluetoothLEDevice*>* op = nullptr;
    hr = statics->FromBluetoothAddressAsync(
            addrVal, reinterpret_cast<IAsyncOperation<BluetoothLEDevice*>**>(&op));
    statics->Release();
    if (FAILED(hr) || !op) { set_state(St::Error, "FromBluetoothAddressAsync 提交失败"); return; }

    hr = async_wait(op, &w_dev, connMs);
    op->Release();
    if (FAILED(hr) || !w_dev) {
        w_dev = nullptr;
        set_state(St::Error, "建链失败（模块是否已开机？是否被手机连着？）");
        return;
    }

    // 诊断用：把各阶段的真实状态码留下来，失败时一并报出。
    // （教训：GattCommunicationStatus_Unreachable 曾被笼统报成"服务未广播"，
    //   把"模块没上电"误导向"模块没广播"，排查方向完全跑偏。）
    int link_status = -1;   // BluetoothConnectionStatus
    int open_status = -1;   // GattOpenStatus
    int comm_status = -1;   // GattCommunicationStatus（最后一次枚举）
    int char_count = -1;    // 最后一次枚举到的特征个数
    {
        BluetoothConnectionStatus lcs = BluetoothConnectionStatus_Disconnected;
        if (SUCCEEDED(w_dev->get_ConnectionStatus(&lcs))) link_status = (int)lcs;
    }

    // ---- 2) 取 FFE0 服务（服务缓存可能还没填充 → 重试）----
    for (int i = 0; i < 6 && !w_svc; ++i) {
        hr = w_dev->GetGattService(kUuidFfe0, &w_svc);
        if (FAILED(hr) || !w_svc) { w_svc = nullptr; Sleep(500); }
    }
    if (!w_svc) { set_state(St::Error, "取不到 FFE0 服务（先在手机端连一次可刷新缓存）"); return; }

    // ---- 3) 权限 + 打开服务 ----
    IGattDeviceService3* s3 = as_iface<IGattDeviceService3>(w_svc);
    if (!s3) { set_state(St::Error, "服务不支持 IGattDeviceService3"); return; }

    {
        IAsyncOperation<ABI::Windows::Devices::Enumeration::DeviceAccessStatus>* rap = nullptr;
        if (SUCCEEDED(s3->RequestAccessAsync(&rap)) && rap) {
            ABI::Windows::Devices::Enumeration::DeviceAccessStatus as =
                ABI::Windows::Devices::Enumeration::DeviceAccessStatus_Unspecified;
            async_wait(rap, &as, connMs);
            rap->Release();
            if (as != ABI::Windows::Devices::Enumeration::DeviceAccessStatus_Allowed) {
                s3->Release();
                set_state(St::Error, "系统拒绝访问该蓝牙设备（检查配对状态）");
                return;
            }
        }
    }

    // OpenAsync 的状态直接反映"模块现在能不能用"：
    //   Success/AlreadyOpened = 好；SharingViolation = 被别的程序占着（常见：另一份蓝牙工具还开着）
    {
        IAsyncOperation<GattOpenStatus>* oap = nullptr;
        if (SUCCEEDED(s3->OpenAsync(GattSharingMode_SharedReadAndWrite, &oap)) && oap) {
            GattOpenStatus os = GattOpenStatus_Unspecified;
            hr = async_wait(oap, &os, connMs);
            oap->Release();
            open_status = (int)os;
            if (FAILED(hr) || (os != GattOpenStatus_Success && os != GattOpenStatus_AlreadyOpened)) {
                s3->Release();
                if (os == GattOpenStatus_SharingViolation)
                    set_state(St::Error, "服务被其他程序占用（关掉别的蓝牙工具/手机连接后重试）");
                else if (os == GattOpenStatus_AccessDenied)
                    set_state(St::Error, "打开服务被拒绝（权限/配对问题）");
                else
                    set_state(St::Error, "打开 GATT 服务失败（模块可能未广播/未连接）");
                return;
            }
        }
    }

    // ---- 4) 枚举特征（必须 Uncached，缓存可能是冷的）----
    for (int attempt = 0; attempt < 3 && !w_chr; ++attempt) {
        IAsyncOperation<IInspectable*>* cop = nullptr;
        hr = s3->GetCharacteristicsWithCacheModeAsync(
                BluetoothCacheMode_Uncached,
                reinterpret_cast<IAsyncOperation<GattCharacteristicsResult*>**>(&cop));
        if (FAILED(hr) || !cop) break;

        IInspectable* cresRaw = nullptr;
        hr = async_wait(cop, &cresRaw, g_connTimeout);
        cop->Release();
        if (FAILED(hr) || !cresRaw) { Sleep(700); continue; }

        IGattCharacteristicsResult* cres = as_iface<IGattCharacteristicsResult>(cresRaw);
        rel_opaque(cresRaw);
        if (!cres) break;

        GattCommunicationStatus cst = GattCommunicationStatus_Unreachable;
        cres->get_Status(&cst);

        IVectorView<GattCharacteristic*>* vec = nullptr;
        HRESULT h2 = cres->get_Characteristics(&vec);
        cres->Release();
        if (FAILED(h2) || !vec) break;

        UINT32 cnt = 0;
        vec->get_Size(&cnt);
        for (UINT32 i = 0; i < cnt; ++i) {
            IGattCharacteristic* ic = nullptr;
            if (FAILED(vec->GetAt(i, &ic)) || !ic) continue;
            GUID g{};
            ic->get_Uuid(&g);
            if (memcmp(&g, &kUuidFfe1, sizeof(GUID)) == 0 && !w_chr) w_chr = ic;
            else ic->Release();
        }
        vec->Release();

        comm_status = (int)cst;
        char_count = (int)cnt;
        if (!w_chr) Sleep(700);
    }
    s3->Release();

    if (!w_chr) {
        const char* hint;
        if (comm_status == 1)        hint = "无线层够不到模块：检查模块供电 / 是否被手机连着";
        else if (comm_status == 2)   hint = "通信协议错误：模块固件异常，建议断电重启模块";
        else if (comm_status == 3)   hint = "读取被系统拒绝：权限或配对有问题";
        else if (comm_status == 0)   hint = "状态正常但特征列表为空：模块服务表异常";
        else                          hint = "枚举特征未取到结果";
        char buf[320];
        snprintf(buf, sizeof(buf),
                 "找不到 FFE1 特征 —— %s [link=%d open=%d comm=%d n=%d]",
                 hint, link_status, open_status, comm_status, char_count);
        set_state(St::Error, buf);
        return;
    }

    // ---- 5) 订阅通知（闭环靠它收 DONE）----
    w_handler = new ValueChangedHandler();
    HRESULT hreg = w_chr->add_ValueChanged(w_handler, &w_tok);
    if (FAILED(hreg)) {
        wr_close();
        set_state(St::Error, "订阅通知失败");
        return;
    }

    IGattCharacteristic3* c3 = as_iface<IGattCharacteristic3>(w_chr);
    if (c3) {
        IAsyncOperation<IInspectable*>* nop = nullptr;
        hr = c3->WriteClientCharacteristicConfigurationDescriptorWithResultAsync(
                GattClientCharacteristicConfigurationDescriptorValue_Notify,
                reinterpret_cast<IAsyncOperation<GattWriteResult*>**>(&nop));
        c3->Release();
        if (SUCCEEDED(hr) && nop) {
            IInspectable* wrRaw = nullptr;
            hr = async_wait(nop, &wrRaw, connMs);
            nop->Release();
            if (SUCCEEDED(hr) && wrRaw) {
                IGattWriteResult* wr = as_iface<IGattWriteResult>(wrRaw);
                rel_opaque(wrRaw);
                if (wr) {
                    GattCommunicationStatus wst = GattCommunicationStatus_Unreachable;
                    wr->get_Status(&wst);
                    wr->Release();
                    if (wst != GattCommunicationStatus_Success) {
                        wr_close();
                        set_state(St::Error, "开启通知失败");
                        return;
                    }
                }
            }
        }
    }

    set_state(St::Ready);
}

// 发一行（纯 ASCII，自动补 \r\n）
bool wr_write_line(const std::string& line)
{
    if (!w_chr) return false;

    HSTRING cls = mk_hstr(L"Windows.Security.Cryptography.CryptographicBuffer");
    ICryptographicBufferStatics* cbs = nullptr;
    HRESULT hr = RoGetActivationFactory(cls, __uuidof(ICryptographicBufferStatics), (void**)&cbs);
    WindowsDeleteString(cls);
    if (FAILED(hr) || !cbs) return false;

    std::string payload = line;
    if (payload.size() < 2 || payload.substr(payload.size() - 2) != "\r\n") payload += "\r\n";

    std::wstring w(payload.begin(), payload.end());
    HSTRING hc = nullptr;
    WindowsCreateString(w.c_str(), (UINT32)w.size(), &hc);
    IBuffer* buf = nullptr;
    hr = cbs->ConvertStringToBinary(hc, BinaryStringEncoding_Utf8, &buf);
    WindowsDeleteString(hc);
    cbs->Release();
    if (FAILED(hr) || !buf) return false;

    IGattCharacteristic3* c3 = as_iface<IGattCharacteristic3>(w_chr);
    if (!c3) { buf->Release(); return false; }

    IAsyncOperation<IInspectable*>* op = nullptr;
    hr = c3->WriteValueWithResultAsync(
            buf, reinterpret_cast<IAsyncOperation<GattWriteResult*>**>(&op));
    c3->Release();
    buf->Release();
    if (FAILED(hr) || !op) return false;

    IInspectable* wrRaw = nullptr;
    hr = async_wait(op, &wrRaw, 10000);
    op->Release();
    if (FAILED(hr) || !wrRaw) return false;

    IGattWriteResult* wr = as_iface<IGattWriteResult>(wrRaw);
    rel_opaque(wrRaw);
    if (!wr) return false;
    GattCommunicationStatus st = GattCommunicationStatus_Unreachable;
    wr->get_Status(&st);
    wr->Release();
    return st == GattCommunicationStatus_Success;
}

void wr_exec(const std::shared_ptr<Job>& j)
{
    St cur;
    {
        std::lock_guard<std::mutex> lk(g_stMu);
        cur = g_state;
    }
    if (cur != St::Ready) { fail(j, "蓝牙未连接"); return; }

    g_rx.clear();
    if (!wr_write_line(j->line)) { fail(j, "指令写入失败（链路可能已断）"); return; }

    if (j->want.empty()) { finish(j, true, std::string(), std::string()); return; }

    DWORD t0 = GetTickCount();
    for (;;) {
        if (j->canceled && j->canceled()) { finish(j, false, std::string(), "canceled"); return; }

        std::string ln;
        while (g_rx.pop(ln)) {
            set_reply(ln);
            // 板子报错：立即失败
            if (ln.rfind("ERR", 0) == 0) { fail(j, ln); return; }
            if (ln.find(j->want) != std::string::npos) { finish(j, true, ln, std::string()); return; }
        }

        if ((int)(GetTickCount() - t0) > j->timeout_ms) {
            set_state(St::Error, "等待回包超时（板子没回 " + j->want + "）");
            finish(j, false, std::string(), "timeout waiting for " + j->want);
            return;
        }
        Sleep(15);
    }
}

// ================================================================ 工作线程

void worker_main()
{
    HRESULT hr = RoInitialize(RO_INIT_MULTITHREADED);
    bool okInit = SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;   // 已在本线程其它套间也算可用
    {
        std::lock_guard<std::mutex> lk(g_stMu);
        g_avail = okInit;
        g_initialized = true;
        g_state = okInit ? St::Disconnected : St::Error;
        if (!okInit) g_err = "WinRT 初始化失败（RoInitialize）";
        g_stCv.notify_all();
    }

    for (;;) {
        std::shared_ptr<Job> job;
        bool doConnect = false, doDisconnect = false;
        {
            std::unique_lock<std::mutex> lk(g_qMu);
            g_qCv.wait(lk, [] { return g_quit || g_reqConnect || g_reqDisconnect || g_job; });
            if (g_quit) break;
            if (g_reqConnect)    { g_reqConnect = false;    doConnect = true; }
            if (g_reqDisconnect) { g_reqDisconnect = false; doDisconnect = true; }
            if (g_job && !doConnect && !doDisconnect) { job = g_job; g_job.reset(); }
        }

        if (doDisconnect) wr_close();
        if (doDisconnect) set_state(St::Disconnected);
        if (doConnect) wr_open();

        if (job) {
            // 连接请求与 exec 同时到达时，先把连接做完
            bool waitingConnect;
            {
                std::lock_guard<std::mutex> lk(g_qMu);
                waitingConnect = g_reqConnect;
            }
            if (waitingConnect) {
                std::lock_guard<std::mutex> lk(g_qMu);
                g_job = job;          // 放回去，下一轮再执行
                g_qCv.notify_all();
            } else {
                wr_exec(job);
            }
        }
    }

    wr_close();
    if (okInit) RoUninitialize();

    {
        std::lock_guard<std::mutex> lk(g_stMu);
        g_state = St::Stopped;
        g_workerDone = true;
        g_stCv.notify_all();
    }
}

}   // namespace

// ================================================================ 公开接口

void start()
{
    {
        std::lock_guard<std::mutex> lk(g_stMu);
        if (g_started) return;          // 幂等
        g_started = true;
        g_workerDone = false;
    }
    {
        std::lock_guard<std::mutex> lk(g_qMu);
        g_quit = false;
    }
    g_worker = std::thread(worker_main);
    g_worker.detach();                  // ★必须 detach：见 g_worker 上方注释

    // 等工作线程回报可用性（最多 3 秒）
    std::unique_lock<std::mutex> sl(g_stMu);
    g_stCv.wait_for(sl, std::chrono::seconds(3), [] { return g_initialized; });
}

void stop()
{
    {
        std::lock_guard<std::mutex> lk(g_stMu);
        if (!g_started) return;
    }
    {
        std::lock_guard<std::mutex> lk(g_qMu);
        g_quit = true;
        g_reqConnect = g_reqDisconnect = false;
        g_job.reset();
    }
    g_qCv.notify_all();

    // 等工作线程真的收尾（它内部会断开链路 + RoUninitialize）
    std::unique_lock<std::mutex> sl(g_stMu);
    g_stCv.wait_for(sl, std::chrono::seconds(4), [] { return g_workerDone; });
    g_started = false;
    g_initialized = false;
    g_avail = false;
}

bool available()
{
    std::lock_guard<std::mutex> lk(g_stMu);
    return g_avail;
}

void set_address_hex(const std::string& addr)
{
    std::lock_guard<std::mutex> lk(g_stMu);
    g_addrHex = addr;
}

std::string address_hex()
{
    std::lock_guard<std::mutex> lk(g_stMu);
    return g_addrHex;
}

void set_connect_timeout_ms(int ms)
{
    if (ms < 1000) ms = 1000;
    std::lock_guard<std::mutex> lk(g_stMu);
    g_connTimeout = ms;
}

void set_reply_timeout_ms(int ms)
{
    if (ms < 100) ms = 100;
    std::lock_guard<std::mutex> lk(g_stMu);
    g_replyTimeout = ms;
}

St state()
{
    std::lock_guard<std::mutex> lk(g_stMu);
    return g_state;
}

const char* state_text()
{
    switch (state()) {
    case St::Stopped:      return "未启动";
    case St::Disconnected: return "未连接";
    case St::Connecting:   return "连接中";
    case St::Ready:        return "已连接";
    case St::Error:        return "异常";
    }
    return "未知";
}

std::string last_error()
{
    std::lock_guard<std::mutex> lk(g_stMu);
    return g_err;
}

std::string last_reply()
{
    std::lock_guard<std::mutex> lk(g_stMu);
    return g_reply;
}

void request_connect()
{
    {
        std::lock_guard<std::mutex> lk(g_qMu);
        g_reqConnect = true;
    }
    g_qCv.notify_all();
}

void request_disconnect()
{
    {
        std::lock_guard<std::mutex> lk(g_qMu);
        g_reqDisconnect = true;
    }
    g_qCv.notify_all();
}

bool exec(const std::string& line, const std::string& want, int timeout_ms,
          const std::function<bool()>& canceled,
          std::string& got, std::string& err)
{
    auto j = std::make_shared<Job>();
    j->line = line;
    j->want = want;
    j->timeout_ms = timeout_ms;
    j->canceled = canceled;

    {
        std::lock_guard<std::mutex> lk(g_stMu);
        if (!g_started) { err = "蓝牙模块未启动"; return false; }
    }
    {
        std::lock_guard<std::mutex> lk(g_qMu);
        if (g_job) { err = "已有蓝牙指令在执行"; return false; }
        g_job = j;
    }
    g_qCv.notify_all();

    std::unique_lock<std::mutex> lk(j->mu);
    j->cv.wait(lk, [&] { return j->done; });
    got = j->got;
    err = j->err;
    return j->ok;
}

}   // namespace blem
