#include "virtual_display.h"

#include <bcrypt.h>
#include <cfgmgr32.h>

#include <algorithm>
#include <chrono>

#include "log.h"
#include "sst_display_control.h"

// Storage for the GUID declared with DEFINE_GUID in sst_display_control.h.
extern "C" const GUID GUID_DEVINTERFACE_SST_DISPLAY = {
    0x6c72fe6d, 0x78cc, 0x46e6, {0xbc, 0x0f, 0x23, 0x20, 0xe9, 0xc6, 0x43, 0x13}};

namespace sst {

VirtualDisplayDriver::~VirtualDisplayDriver() {
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    cv_.notify_all();
    if (keepalive_.joinable()) keepalive_.join();
    for (uint32_t id : std::vector<uint32_t>(monitors_)) RemoveMonitor(id);
    if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
}

bool VirtualDisplayDriver::Open() {
    ULONG size = 0;
    GUID guid = GUID_DEVINTERFACE_SST_DISPLAY;
    if (CM_Get_Device_Interface_List_SizeW(&size, &guid, nullptr, CM_GET_DEVICE_INTERFACE_LIST_PRESENT) !=
            CR_SUCCESS || size <= 1) {
        return false;
    }
    std::vector<wchar_t> list(size);
    if (CM_Get_Device_Interface_ListW(&guid, nullptr, list.data(), size, CM_GET_DEVICE_INTERFACE_LIST_PRESENT) !=
        CR_SUCCESS) {
        return false;
    }
    // The list is double-NUL terminated; the first entry is our device.
    // Display devices only grant read/write to administrators. Our IOCTLs are
    // FILE_ANY_ACCESS, so a handle without any access rights is enough.
    handle_ = CreateFileW(list.data(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (handle_ == INVALID_HANDLE_VALUE) {
        Log("display driver: cannot open (error %lu)", GetLastError());
        return false;
    }

    SST_VERSION_OUT version{};
    if (!Ioctl(IOCTL_SST_GET_VERSION, nullptr, 0, &version, sizeof(version)) ||
        version.Version != SST_DISPLAY_CONTROL_VERSION) {
        Log("display driver: version mismatch (driver %u, host %u)", version.Version, SST_DISPLAY_CONTROL_VERSION);
        CloseHandle(handle_);
        handle_ = INVALID_HANDLE_VALUE;
        return false;
    }
    if (BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(&owner_), sizeof(owner_), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
        owner_ = (uint64_t(GetCurrentProcessId()) << 32) ^ GetTickCount64();  // unique enough between our own hosts
    keepalive_ = std::thread([this] { KeepaliveLoop(); });
    return true;
}

bool VirtualDisplayDriver::Ioctl(DWORD code, const void* in, DWORD inSize, void* out, DWORD outSize) {
    DWORD returned = 0;
    return DeviceIoControl(handle_, code, const_cast<void*>(in), inSize, out, outSize, &returned, nullptr) != 0;
}

uint32_t VirtualDisplayDriver::AddMonitor(uint32_t width, uint32_t height, uint32_t refresh, uint32_t serial) {
    SST_ADD_MONITOR_IN in{width, height, refresh, serial, owner_};
    SST_ADD_MONITOR_OUT out{};
    // Right after boot the driver's adapter may still be starting.
    for (int attempt = 0; attempt < 20; ++attempt) {
        if (Ioctl(IOCTL_SST_ADD_MONITOR, &in, sizeof(in), &out, sizeof(out))) {
            std::lock_guard lock(mutex_);
            monitors_.push_back(out.MonitorId);
            return out.MonitorId;
        }
        if (GetLastError() != ERROR_NOT_READY) break;
        Sleep(250);
    }
    Log("display driver: adding a %ux%u monitor failed (error %lu)", width, height, GetLastError());
    return 0;
}

void VirtualDisplayDriver::RemoveMonitor(uint32_t id) {
    SST_REMOVE_MONITOR_IN in{id, owner_};
    Ioctl(IOCTL_SST_REMOVE_MONITOR, &in, sizeof(in), nullptr, 0);
    std::lock_guard lock(mutex_);
    monitors_.erase(std::remove(monitors_.begin(), monitors_.end(), id), monitors_.end());
}

void VirtualDisplayDriver::KeepaliveLoop() {
    std::unique_lock lock(mutex_);
    while (!stopping_) {
        cv_.wait_for(lock, std::chrono::seconds(1));
        if (stopping_) break;
        lock.unlock();
        SST_KEEPALIVE_IN in{owner_};
        Ioctl(IOCTL_SST_KEEPALIVE, &in, sizeof(in), nullptr, 0);
        lock.lock();
    }
}

}  // namespace sst
