// ScreenFleet virtual display driver (IddCx, UMDF).
// Based on Microsoft's IddSampleDriver (MIT License, Copyright (c) Microsoft
// Corporation). Monitors are added and removed on request by sst-host.exe.
#include "Driver.h"

#include <chrono>
#include <cstdarg>
#include <map>
#include <vector>

#include "Edid.h"

using namespace sst;
using Microsoft::WRL::ComPtr;

// Storage for the GUID declared in sst_display_control.h (initguid.h would
// also re-define every GUID in winioctl.h).
extern "C" const GUID GUID_DEVINTERFACE_SST_DISPLAY = {
    0x6c72fe6d, 0x78cc, 0x46e6, {0xbc, 0x0f, 0x23, 0x20, 0xe9, 0xc6, 0x43, 0x13}};

namespace {

void Trace(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    if (n < 0) n = int(strlen(buf));
    OutputDebugStringA("[SstDisplay] ");
    OutputDebugStringA(buf);
    OutputDebugStringA("\n");
}

// ParseMonitorDescription only gets the EDID, not the monitor object, so the
// mode for each monitor is looked up by the EDID serial number.
std::mutex g_modesMutex;
std::map<UINT32, Mode> g_modes;

void FillSignalInfo(DISPLAYCONFIG_VIDEO_SIGNAL_INFO& info, const Mode& m, bool monitorMode) {
    info.totalSize.cx = info.activeSize.cx = m.width;
    info.totalSize.cy = info.activeSize.cy = m.height;
    info.AdditionalSignalInfo.vSyncFreqDivider = monitorMode ? 0 : 1;
    info.AdditionalSignalInfo.videoStandard = 255;
    info.vSyncFreq.Numerator = m.refresh;
    info.vSyncFreq.Denominator = 1;
    info.hSyncFreq.Numerator = m.refresh * m.height;
    info.hSyncFreq.Denominator = 1;
    info.scanLineOrdering = DISPLAYCONFIG_SCANLINE_ORDERING_PROGRESSIVE;
    info.pixelRate = UINT64(m.refresh) * m.width * m.height;
}

IDDCX_MONITOR_MODE MakeMonitorMode(const Mode& m, IDDCX_MONITOR_MODE_ORIGIN origin) {
    IDDCX_MONITOR_MODE mode = {};
    mode.Size = sizeof(mode);
    mode.Origin = origin;
    FillSignalInfo(mode.MonitorVideoSignalInfo, m, true);
    return mode;
}

IDDCX_TARGET_MODE MakeTargetMode(const Mode& m) {
    IDDCX_TARGET_MODE mode = {};
    mode.Size = sizeof(mode);
    FillSignalInfo(mode.TargetVideoSignalInfo.targetVideoSignalInfo, m, false);
    return mode;
}

// Stable per serial, so Windows treats the same tablet as the same monitor.
GUID ContainerIdForSerial(UINT32 serial) {
    GUID g = {0x5e7c0b9a, 0x2f41, 0x4d6e, {0x9a, 0x1c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}};
    g.Data4[4] = BYTE(serial >> 24);
    g.Data4[5] = BYTE(serial >> 16);
    g.Data4[6] = BYTE(serial >> 8);
    g.Data4[7] = BYTE(serial);
    return g;
}

struct DeviceContextWrapper {
    DeviceContext* context;
    bool owner;  // the device object owns the context; the adapter only points to it
    void Cleanup() {
        if (owner) delete context;
        context = nullptr;
    }
};

struct MonitorContextWrapper {
    MonitorContext* context;
    void Cleanup() {
        delete context;
        context = nullptr;
    }
};

}  // namespace

WDF_DECLARE_CONTEXT_TYPE(DeviceContextWrapper);
WDF_DECLARE_CONTEXT_TYPE(MonitorContextWrapper);

#pragma region Direct3DDevice / SwapChainProcessor

HRESULT Direct3DDevice::Init() {
    HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
    if (FAILED(hr)) return hr;
    hr = factory->EnumAdapterByLuid(adapterLuid, IID_PPV_ARGS(&adapter));
    if (FAILED(hr)) return hr;
    return D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                             nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context);
}

SwapChainProcessor::SwapChainProcessor(IDDCX_SWAPCHAIN swapChain, std::shared_ptr<Direct3DDevice> device,
                                       HANDLE newFrameEvent)
    : swapChain_(swapChain), device_(std::move(device)), newFrameEvent_(newFrameEvent) {
    terminateEvent_.Attach(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    thread_ = std::thread([this] { Run(); });
}

SwapChainProcessor::~SwapChainProcessor() {
    SetEvent(terminateEvent_.Get());
    if (thread_.joinable()) thread_.join();
}

void SwapChainProcessor::Run() {
    DWORD avTask = 0;
    HANDLE avHandle = AvSetMmThreadCharacteristicsW(L"Distribution", &avTask);
    RunCore();
    // Deleting the swap chain tells the OS to make a new one if still needed.
    WdfObjectDelete(reinterpret_cast<WDFOBJECT>(swapChain_));
    swapChain_ = nullptr;
    if (avHandle) AvRevertMmThreadCharacteristics(avHandle);
}

void SwapChainProcessor::RunCore() {
    ComPtr<IDXGIDevice> dxgiDevice;
    if (FAILED(device_->device.As(&dxgiDevice))) return;
    IDARG_IN_SWAPCHAINSETDEVICE setDevice = {};
    setDevice.pDevice = dxgiDevice.Get();
    if (FAILED(IddCxSwapChainSetDevice(swapChain_, &setDevice))) return;

    for (;;) {
        IDARG_OUT_RELEASEANDACQUIREBUFFER buffer = {};
        HRESULT hr = IddCxSwapChainReleaseAndAcquireBuffer(swapChain_, &buffer);
        if (hr == E_PENDING) {
            HANDLE waits[] = {newFrameEvent_, terminateEvent_.Get()};
            DWORD result = WaitForMultipleObjects(ARRAYSIZE(waits), waits, FALSE, 16);
            if (result == WAIT_OBJECT_0 || result == WAIT_TIMEOUT) continue;
            break;  // terminate or error
        }
        if (FAILED(hr)) break;  // swap chain abandoned, e.g. mode change

        // Release our reference right away; the host reads the desktop itself.
        ComPtr<IDXGIResource> surface;
        surface.Attach(buffer.MetaData.pSurface);
        surface.Reset();
        if (FAILED(IddCxSwapChainFinishedProcessingFrame(swapChain_))) break;
    }
}

void MonitorContext::AssignSwapChain(IDDCX_SWAPCHAIN swapChain, LUID renderAdapter, HANDLE newFrameEvent) {
    processor_.reset();
    auto device = std::make_shared<Direct3DDevice>(renderAdapter);
    if (FAILED(device->Init())) {
        // The OS retries with a new swap chain.
        WdfObjectDelete(reinterpret_cast<WDFOBJECT>(swapChain));
        return;
    }
    processor_ = std::make_unique<SwapChainProcessor>(swapChain, device, newFrameEvent);
}

#pragma endregion

#pragma region DeviceContext

DeviceContext::DeviceContext(WDFDEVICE device) : device_(device) {
    watchdog_ = std::thread([this] { Watchdog(); });
}

DeviceContext::~DeviceContext() {
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    cv_.notify_all();
    if (watchdog_.joinable()) watchdog_.join();
}

void DeviceContext::InitAdapter() {
    if (adapter_) return;  // D0Entry also runs after resume from sleep

    IDDCX_ADAPTER_CAPS caps = {};
    caps.Size = sizeof(caps);
    caps.MaxMonitorsSupported = SST_MAX_MONITORS;
    caps.EndPointDiagnostics.Size = sizeof(caps.EndPointDiagnostics);
    caps.EndPointDiagnostics.GammaSupport = IDDCX_FEATURE_IMPLEMENTATION_NONE;
    caps.EndPointDiagnostics.TransmissionType = IDDCX_TRANSMISSION_TYPE_OTHER;
    caps.EndPointDiagnostics.pEndPointFriendlyName = L"ScreenFleet";
    caps.EndPointDiagnostics.pEndPointManufacturerName = L"ScreenFleet";
    caps.EndPointDiagnostics.pEndPointModelName = L"ScreenFleet Virtual Display";
    IDDCX_ENDPOINT_VERSION version = {};
    version.Size = sizeof(version);
    version.MajorVer = 1;
    caps.EndPointDiagnostics.pFirmwareVersion = &version;
    caps.EndPointDiagnostics.pHardwareVersion = &version;

    WDF_OBJECT_ATTRIBUTES attr;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, DeviceContextWrapper);

    IDARG_IN_ADAPTER_INIT init = {};
    init.WdfDevice = device_;
    init.pCaps = &caps;
    init.ObjectAttributes = &attr;
    IDARG_OUT_ADAPTER_INIT out = {};
    NTSTATUS status = IddCxAdapterInitAsync(&init, &out);
    if (NT_SUCCESS(status)) {
        adapter_ = out.AdapterObject;
        auto* wrapper = WdfObjectGet_DeviceContextWrapper(out.AdapterObject);
        wrapper->context = this;
        wrapper->owner = false;
    } else {
        Trace("IddCxAdapterInitAsync failed 0x%08X", status);
    }
}

NTSTATUS DeviceContext::AddMonitor(const SST_ADD_MONITOR_IN& request, UINT32* monitorId) {
    if (!adapterReady_) return STATUS_DEVICE_NOT_READY;
    if (request.Width < 320 || request.Width > 4095 || request.Height < 240 || request.Height > 4095 ||
        request.RefreshRate < 24 || request.RefreshRate > 240) {
        return STATUS_INVALID_PARAMETER;
    }

    std::lock_guard lock(mutex_);
    Slot* slot = nullptr;
    UINT connector = 0;
    for (UINT i = 0; i < SST_MAX_MONITORS; ++i) {
        if (!slots_[i].monitor) {
            slot = &slots_[i];
            connector = i;
            break;
        }
    }
    if (!slot) return STATUS_INSUFFICIENT_RESOURCES;

    Mode mode{request.Width, request.Height, request.RefreshRate};
    {
        std::lock_guard modesLock(g_modesMutex);
        g_modes[request.Serial] = mode;
    }
    EdidBlock edid = MakeEdid(mode.width, mode.height, mode.refresh, request.Serial);

    WDF_OBJECT_ATTRIBUTES attr;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, MonitorContextWrapper);
    // Free the monitor's context with the monitor (the sample this driver is
    // based on never did, leaking one context per monitor).
    attr.EvtCleanupCallback = [](WDFOBJECT object) {
        if (auto* wrapper = WdfObjectGet_MonitorContextWrapper(object)) wrapper->Cleanup();
    };
    IDDCX_MONITOR_INFO info = {};
    info.Size = sizeof(info);
    info.MonitorType = DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INDIRECT_WIRED;
    info.ConnectorIndex = connector;
    info.MonitorDescription.Size = sizeof(info.MonitorDescription);
    info.MonitorDescription.Type = IDDCX_MONITOR_DESCRIPTION_TYPE_EDID;
    info.MonitorDescription.DataSize = UINT(edid.size());
    info.MonitorDescription.pData = edid.data();
    info.MonitorContainerId = ContainerIdForSerial(request.Serial);

    IDARG_IN_MONITORCREATE create = {};
    create.ObjectAttributes = &attr;
    create.pMonitorInfo = &info;
    IDARG_OUT_MONITORCREATE created = {};
    NTSTATUS status = IddCxMonitorCreate(adapter_, &create, &created);
    if (!NT_SUCCESS(status)) {
        Trace("IddCxMonitorCreate failed 0x%08X", status);
        return status;
    }
    WdfObjectGet_MonitorContextWrapper(created.MonitorObject)->context = new MonitorContext(mode);

    IDARG_OUT_MONITORARRIVAL arrival = {};
    status = IddCxMonitorArrival(created.MonitorObject, &arrival);
    if (!NT_SUCCESS(status)) {
        Trace("IddCxMonitorArrival failed 0x%08X", status);
        WdfObjectDelete(reinterpret_cast<WDFOBJECT>(created.MonitorObject));
        return status;
    }

    slot->monitor = created.MonitorObject;
    slot->id = nextId_++;
    slot->serial = request.Serial;
    slot->owner = request.Owner;
    slot->lastKeepalive = GetTickCount64();
    *monitorId = slot->id;
    Trace("monitor %u added: %ux%u @ %u Hz, serial %08X", slot->id, mode.width, mode.height, mode.refresh,
          request.Serial);
    return STATUS_SUCCESS;
}

void DeviceContext::RemoveSlot(Slot& slot) {
    if (!slot.monitor) return;
    NTSTATUS status = IddCxMonitorDeparture(slot.monitor);
    Trace("monitor %u removed (0x%08X)", slot.id, status);
    // Forget its mode unless another monitor uses the same serial, so the
    // table can't grow without bound.
    bool shared = false;
    for (const auto& other : slots_)
        if (&other != &slot && other.monitor && other.serial == slot.serial) shared = true;
    if (!shared) {
        std::lock_guard modesLock(g_modesMutex);
        g_modes.erase(slot.serial);
    }
    slot = Slot{};
}

NTSTATUS DeviceContext::RemoveMonitor(UINT32 monitorId, UINT64 owner) {
    std::lock_guard lock(mutex_);
    for (auto& slot : slots_) {
        if (slot.monitor && slot.id == monitorId && slot.owner == owner) {
            RemoveSlot(slot);
            return STATUS_SUCCESS;
        }
    }
    return STATUS_NOT_FOUND;
}

void DeviceContext::Keepalive(UINT64 owner) {
    std::lock_guard lock(mutex_);
    ULONGLONG now = GetTickCount64();
    for (auto& slot : slots_)
        if (slot.monitor && slot.owner == owner) slot.lastKeepalive = now;
}

void DeviceContext::Watchdog() {
    std::unique_lock lock(mutex_);
    while (!stopping_) {
        cv_.wait_for(lock, std::chrono::seconds(1));
        if (stopping_) break;
        // Each host's monitors on their own: a crashed host's monitors go even
        // while another host keeps its own alive.
        ULONGLONG now = GetTickCount64();
        for (auto& slot : slots_) {
            if (slot.monitor && now - slot.lastKeepalive >= SST_KEEPALIVE_TIMEOUT_MS) {
                Trace("host %016llX stopped responding", slot.owner);
                RemoveSlot(slot);
            }
        }
    }
}

#pragma endregion

#pragma region Driver callbacks

extern "C" DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD SstDeviceAdd;
EVT_WDF_DEVICE_D0_ENTRY SstDeviceD0Entry;
EVT_IDD_CX_DEVICE_IO_CONTROL SstIoDeviceControl;
EVT_IDD_CX_ADAPTER_INIT_FINISHED SstAdapterInitFinished;
EVT_IDD_CX_ADAPTER_COMMIT_MODES SstAdapterCommitModes;
EVT_IDD_CX_PARSE_MONITOR_DESCRIPTION SstParseMonitorDescription;
EVT_IDD_CX_MONITOR_GET_DEFAULT_DESCRIPTION_MODES SstMonitorGetDefaultModes;
EVT_IDD_CX_MONITOR_QUERY_TARGET_MODES SstMonitorQueryModes;
EVT_IDD_CX_MONITOR_ASSIGN_SWAPCHAIN SstMonitorAssignSwapChain;
EVT_IDD_CX_MONITOR_UNASSIGN_SWAPCHAIN SstMonitorUnassignSwapChain;

extern "C" BOOL WINAPI DllMain(HINSTANCE, UINT, LPVOID) { return TRUE; }

_Use_decl_annotations_
extern "C" NTSTATUS DriverEntry(PDRIVER_OBJECT driverObject, PUNICODE_STRING registryPath) {
    WDF_DRIVER_CONFIG config;
    WDF_DRIVER_CONFIG_INIT(&config, SstDeviceAdd);
    WDF_OBJECT_ATTRIBUTES attr;
    WDF_OBJECT_ATTRIBUTES_INIT(&attr);
    return WdfDriverCreate(driverObject, registryPath, &attr, &config, WDF_NO_HANDLE);
}

_Use_decl_annotations_
NTSTATUS SstDeviceAdd(WDFDRIVER, PWDFDEVICE_INIT deviceInit) {
    WDF_PNPPOWER_EVENT_CALLBACKS power;
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&power);
    power.EvtDeviceD0Entry = SstDeviceD0Entry;
    WdfDeviceInitSetPnpPowerEventCallbacks(deviceInit, &power);

    IDD_CX_CLIENT_CONFIG idd;
    IDD_CX_CLIENT_CONFIG_INIT(&idd);
    idd.EvtIddCxDeviceIoControl = SstIoDeviceControl;
    idd.EvtIddCxAdapterInitFinished = SstAdapterInitFinished;
    idd.EvtIddCxParseMonitorDescription = SstParseMonitorDescription;
    idd.EvtIddCxMonitorGetDefaultDescriptionModes = SstMonitorGetDefaultModes;
    idd.EvtIddCxMonitorQueryTargetModes = SstMonitorQueryModes;
    idd.EvtIddCxAdapterCommitModes = SstAdapterCommitModes;
    idd.EvtIddCxMonitorAssignSwapChain = SstMonitorAssignSwapChain;
    idd.EvtIddCxMonitorUnassignSwapChain = SstMonitorUnassignSwapChain;
    NTSTATUS status = IddCxDeviceInitConfig(deviceInit, &idd);
    if (!NT_SUCCESS(status)) return status;

    WDF_OBJECT_ATTRIBUTES attr;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, DeviceContextWrapper);
    attr.EvtCleanupCallback = [](WDFOBJECT object) {
        if (auto* wrapper = WdfObjectGet_DeviceContextWrapper(object)) wrapper->Cleanup();
    };
    WDFDEVICE device = nullptr;
    status = WdfDeviceCreate(&deviceInit, &attr, &device);
    if (!NT_SUCCESS(status)) return status;

    // The host finds the driver through this interface.
    status = WdfDeviceCreateDeviceInterface(device, &GUID_DEVINTERFACE_SST_DISPLAY, nullptr);
    if (!NT_SUCCESS(status)) return status;

    status = IddCxDeviceInitialize(device);
    auto* wrapper = WdfObjectGet_DeviceContextWrapper(device);
    wrapper->context = new DeviceContext(device);
    wrapper->owner = true;
    return status;
}

_Use_decl_annotations_
NTSTATUS SstDeviceD0Entry(WDFDEVICE device, WDF_POWER_DEVICE_STATE) {
    WdfObjectGet_DeviceContextWrapper(device)->context->InitAdapter();
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
VOID SstIoDeviceControl(WDFDEVICE device, WDFREQUEST request, size_t, size_t, ULONG code) {
    DeviceContext* ctx = WdfObjectGet_DeviceContextWrapper(device)->context;
    NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
    ULONG_PTR bytes = 0;

    switch (code) {
    case IOCTL_SST_GET_VERSION: {
        SST_VERSION_OUT* out = nullptr;
        status = WdfRequestRetrieveOutputBuffer(request, sizeof(*out), reinterpret_cast<PVOID*>(&out), nullptr);
        if (NT_SUCCESS(status)) {
            out->Version = SST_DISPLAY_CONTROL_VERSION;
            bytes = sizeof(*out);
        }
        break;
    }
    case IOCTL_SST_ADD_MONITOR: {
        SST_ADD_MONITOR_IN* in = nullptr;
        SST_ADD_MONITOR_OUT* out = nullptr;
        status = WdfRequestRetrieveInputBuffer(request, sizeof(*in), reinterpret_cast<PVOID*>(&in), nullptr);
        if (NT_SUCCESS(status))
            status = WdfRequestRetrieveOutputBuffer(request, sizeof(*out), reinterpret_cast<PVOID*>(&out), nullptr);
        if (NT_SUCCESS(status)) {
            // METHOD_BUFFERED: input and output share one buffer, so copy first.
            SST_ADD_MONITOR_IN copy = *in;
            UINT32 id = 0;
            status = ctx->AddMonitor(copy, &id);
            if (NT_SUCCESS(status)) {
                out->MonitorId = id;
                bytes = sizeof(*out);
            }
        }
        break;
    }
    case IOCTL_SST_REMOVE_MONITOR: {
        SST_REMOVE_MONITOR_IN* in = nullptr;
        status = WdfRequestRetrieveInputBuffer(request, sizeof(*in), reinterpret_cast<PVOID*>(&in), nullptr);
        if (NT_SUCCESS(status)) status = ctx->RemoveMonitor(in->MonitorId, in->Owner);
        break;
    }
    case IOCTL_SST_KEEPALIVE: {
        SST_KEEPALIVE_IN* in = nullptr;
        status = WdfRequestRetrieveInputBuffer(request, sizeof(*in), reinterpret_cast<PVOID*>(&in), nullptr);
        if (NT_SUCCESS(status)) ctx->Keepalive(in->Owner);
        break;
    }
    default:
        break;
    }
    WdfRequestCompleteWithInformation(request, status, bytes);
}

_Use_decl_annotations_
NTSTATUS SstAdapterInitFinished(IDDCX_ADAPTER adapter, const IDARG_IN_ADAPTER_INIT_FINISHED* args) {
    if (NT_SUCCESS(args->AdapterInitStatus)) {
        WdfObjectGet_DeviceContextWrapper(adapter)->context->OnAdapterReady();
        Trace("adapter ready");
    } else {
        Trace("adapter init failed 0x%08X", args->AdapterInitStatus);
    }
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS SstAdapterCommitModes(IDDCX_ADAPTER, const IDARG_IN_COMMITMODES*) {
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS SstParseMonitorDescription(const IDARG_IN_PARSEMONITORDESCRIPTION* in,
                                    IDARG_OUT_PARSEMONITORDESCRIPTION* out) {
    if (in->MonitorDescription.DataSize < 128 || !in->MonitorDescription.pData) return STATUS_INVALID_PARAMETER;
    UINT32 serial = EdidSerial(static_cast<const uint8_t*>(in->MonitorDescription.pData));
    Mode mode;
    {
        std::lock_guard lock(g_modesMutex);
        auto it = g_modes.find(serial);
        if (it == g_modes.end()) return STATUS_INVALID_PARAMETER;
        mode = it->second;
    }

    out->MonitorModeBufferOutputCount = 1;
    if (in->MonitorModeBufferInputCount < 1)
        return in->MonitorModeBufferInputCount > 0 ? STATUS_BUFFER_TOO_SMALL : STATUS_SUCCESS;
    in->pMonitorModes[0] = MakeMonitorMode(mode, IDDCX_MONITOR_MODE_ORIGIN_MONITORDESCRIPTOR);
    out->PreferredMonitorModeIdx = 0;
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS SstMonitorGetDefaultModes(IDDCX_MONITOR monitor, const IDARG_IN_GETDEFAULTDESCRIPTIONMODES* in,
                                   IDARG_OUT_GETDEFAULTDESCRIPTIONMODES* out) {
    // Only used for monitors without an EDID; ours always have one.
    const Mode& mode = WdfObjectGet_MonitorContextWrapper(monitor)->context->mode;
    out->DefaultMonitorModeBufferOutputCount = 1;
    if (in->DefaultMonitorModeBufferInputCount > 0) {
        in->pDefaultMonitorModes[0] = MakeMonitorMode(mode, IDDCX_MONITOR_MODE_ORIGIN_DRIVER);
        out->PreferredMonitorModeIdx = 0;
    }
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS SstMonitorQueryModes(IDDCX_MONITOR monitor, const IDARG_IN_QUERYTARGETMODES* in,
                              IDARG_OUT_QUERYTARGETMODES* out) {
    const Mode& mode = WdfObjectGet_MonitorContextWrapper(monitor)->context->mode;
    out->TargetModeBufferOutputCount = 1;
    if (in->TargetModeBufferInputCount >= 1) in->pTargetModes[0] = MakeTargetMode(mode);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS SstMonitorAssignSwapChain(IDDCX_MONITOR monitor, const IDARG_IN_SETSWAPCHAIN* in) {
    WdfObjectGet_MonitorContextWrapper(monitor)->context->AssignSwapChain(in->hSwapChain, in->RenderAdapterLuid,
                                                                           in->hNextSurfaceAvailable);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS SstMonitorUnassignSwapChain(IDDCX_MONITOR monitor) {
    WdfObjectGet_MonitorContextWrapper(monitor)->context->UnassignSwapChain();
    return STATUS_SUCCESS;
}

#pragma endregion
