#include "outputs.h"

#include <vector>

namespace sst {

namespace {

// Looks up the EDID monitor name for a GDI device name like \\.\DISPLAY1.
std::wstring MonitorFriendlyName(const wchar_t* gdiName) {
    UINT32 pathCount = 0, modeCount = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS) return {};
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr) !=
        ERROR_SUCCESS) {
        return {};
    }

    for (UINT32 i = 0; i < pathCount; ++i) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
        source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source.header.size = sizeof(source);
        source.header.adapterId = paths[i].sourceInfo.adapterId;
        source.header.id = paths[i].sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS) continue;
        if (wcscmp(source.viewGdiDeviceName, gdiName) != 0) continue;

        DISPLAYCONFIG_TARGET_DEVICE_NAME target{};
        target.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        target.header.size = sizeof(target);
        target.header.adapterId = paths[i].targetInfo.adapterId;
        target.header.id = paths[i].targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&target.header) != ERROR_SUCCESS) continue;
        return target.monitorFriendlyDeviceName;
    }
    return {};
}

}  // namespace

std::vector<DisplayOutput> EnumerateOutputs() {
    ComPtr<IDXGIFactory1> factory;
    Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "CreateDXGIFactory1");

    std::vector<DisplayOutput> result;
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT a = 0; factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a) {
        DXGI_ADAPTER_DESC1 adesc{};
        adapter->GetDesc1(&adesc);

        ComPtr<IDXGIOutput> output;
        for (UINT o = 0; adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; ++o) {
            DXGI_OUTPUT_DESC odesc{};
            output->GetDesc(&odesc);
            if (!odesc.AttachedToDesktop) continue;

            DisplayOutput d;
            d.index = int(result.size());
            d.gdiName = Narrow(odesc.DeviceName);
            d.monitorName = Narrow(MonitorFriendlyName(odesc.DeviceName));
            d.adapterName = Narrow(adesc.Description);
            d.adapterLuid = adesc.AdapterLuid;
            d.desktopRect = odesc.DesktopCoordinates;
            d.adapter = adapter;
            d.output = output;
            result.push_back(std::move(d));
        }
    }
    return result;
}

}  // namespace sst
