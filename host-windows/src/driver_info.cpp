// Kept apart from virtual_display.cpp: devpkey.h needs initguid.h, which
// clashes with the winioctl.h that the driver's control header pulls in.
#include <windows.h>
#include <initguid.h>
#include <devpkey.h>
#include <cfgmgr32.h>

#include <vector>

#include "driver_info.h"
#include "log.h"

extern "C" const GUID GUID_DEVINTERFACE_SST_DISPLAY;  // defined in virtual_display.cpp

namespace sst {

bool QueryDriverInfo(std::string& version, std::string& date) {
    GUID guid = GUID_DEVINTERFACE_SST_DISPLAY;
    ULONG size = 0;
    if (CM_Get_Device_Interface_List_SizeW(&size, &guid, nullptr, CM_GET_DEVICE_INTERFACE_LIST_PRESENT) !=
            CR_SUCCESS || size <= 1) {
        return false;
    }
    std::vector<wchar_t> list(size);
    if (CM_Get_Device_Interface_ListW(&guid, nullptr, list.data(), size, CM_GET_DEVICE_INTERFACE_LIST_PRESENT) !=
        CR_SUCCESS) {
        return false;
    }

    wchar_t instance[512];
    ULONG bytes = sizeof(instance);
    DEVPROPTYPE type = 0;
    if (CM_Get_Device_Interface_PropertyW(list.data(), &DEVPKEY_Device_InstanceId, &type,
                                          reinterpret_cast<PBYTE>(instance), &bytes, 0) != CR_SUCCESS) {
        return false;
    }
    DEVINST node = 0;
    if (CM_Locate_DevNodeW(&node, instance, CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS) return false;

    wchar_t buf[128];
    bytes = sizeof(buf);
    if (CM_Get_DevNode_PropertyW(node, &DEVPKEY_Device_DriverVersion, &type, reinterpret_cast<PBYTE>(buf), &bytes,
                                 0) == CR_SUCCESS) {
        version = Narrow(buf);
    }
    FILETIME ft{};
    bytes = sizeof(ft);
    if (CM_Get_DevNode_PropertyW(node, &DEVPKEY_Device_DriverDate, &type, reinterpret_cast<PBYTE>(&ft), &bytes, 0) ==
        CR_SUCCESS) {
        SYSTEMTIME st{};
        FileTimeToSystemTime(&ft, &st);
        static const char* months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                       "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
        char d[32];
        std::snprintf(d, sizeof(d), "%u %s %u", st.wDay, months[(st.wMonth + 11) % 12], st.wYear);
        date = d;
    }
    return true;
}

}  // namespace sst
