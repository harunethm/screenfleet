#pragma once

// The NTSTATUS codes come from ntstatus.h (via wudfwdm.h), not windows.h.
#define WIN32_NO_STATUS
#include <windows.h>
#undef WIN32_NO_STATUS
#include <bugcodes.h>
#pragma warning(push)
// Warnings inside the WDK headers: forward-declared enums, struct padding.
#pragma warning(disable : 4471 4324)
#include <wudfwdm.h>
#include <wdf.h>
#include <iddcx.h>
#pragma warning(pop)

#include <avrt.h>
#include <d3d11_2.h>
#include <dxgi1_5.h>
#include <wrl.h>

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

#include "sst_display_control.h"

namespace sst {

struct Mode {
    UINT32 width = 0;
    UINT32 height = 0;
    UINT32 refresh = 60;
};

// D3D11 device on the GPU that renders the virtual monitor's desktop.
struct Direct3DDevice {
    explicit Direct3DDevice(LUID adapterLuid) : adapterLuid(adapterLuid) {}
    HRESULT Init();

    LUID adapterLuid;
    Microsoft::WRL::ComPtr<IDXGIFactory5> factory;
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
};

// Takes frames from the OS for one monitor and hands them straight back.
// The host captures the monitor with Desktop Duplication, so the driver
// doesn't need the pixels; it only has to keep the swap chain moving.
class SwapChainProcessor {
public:
    SwapChainProcessor(IDDCX_SWAPCHAIN swapChain, std::shared_ptr<Direct3DDevice> device, HANDLE newFrameEvent);
    ~SwapChainProcessor();

private:
    void Run();
    void RunCore();

    IDDCX_SWAPCHAIN swapChain_;
    std::shared_ptr<Direct3DDevice> device_;
    HANDLE newFrameEvent_;
    Microsoft::WRL::Wrappers::Event terminateEvent_;
    std::thread thread_;
};

class MonitorContext {
public:
    explicit MonitorContext(const Mode& mode) : mode(mode) {}

    void AssignSwapChain(IDDCX_SWAPCHAIN swapChain, LUID renderAdapter, HANDLE newFrameEvent);
    void UnassignSwapChain() { processor_.reset(); }

    const Mode mode;

private:
    std::unique_ptr<SwapChainProcessor> processor_;
};

// One per driver device: owns the IddCx adapter and the monitors the host
// asked for.
class DeviceContext {
public:
    explicit DeviceContext(WDFDEVICE device);
    ~DeviceContext();

    void InitAdapter();
    void OnAdapterReady() { adapterReady_ = true; }

    NTSTATUS AddMonitor(const SST_ADD_MONITOR_IN& request, UINT32* monitorId);
    NTSTATUS RemoveMonitor(UINT32 monitorId, UINT64 owner);
    void Keepalive(UINT64 owner);

private:
    struct Slot {
        IDDCX_MONITOR monitor = nullptr;
        UINT32 id = 0;
        UINT32 serial = 0;
        UINT64 owner = 0;            // the host that added it
        ULONGLONG lastKeepalive = 0; // from that host
    };

    void RemoveSlot(Slot& slot);
    void Watchdog();

    WDFDEVICE device_;
    IDDCX_ADAPTER adapter_ = nullptr;
    std::atomic<bool> adapterReady_{false};

    std::mutex mutex_;
    std::condition_variable cv_;
    Slot slots_[SST_MAX_MONITORS];
    UINT32 nextId_ = 1;
    bool stopping_ = false;
    std::thread watchdog_;
};

}  // namespace sst
