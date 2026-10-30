// Control interface between sst-host.exe and the ScreenFleet display driver.
// The host opens the driver's device interface and sends these IOCTLs.
#pragma once

#include <windows.h>
#include <winioctl.h>

// {6C72FE6D-78CC-46E6-BC0F-2320E9C64313}
DEFINE_GUID(GUID_DEVINTERFACE_SST_DISPLAY,
    0x6c72fe6d, 0x78cc, 0x46e6, 0xbc, 0x0f, 0x23, 0x20, 0xe9, 0xc6, 0x43, 0x13);

// 2: monitors belong to the host that added them (Owner), each host's are
//    kept alive and removed on their own.
#define SST_DISPLAY_CONTROL_VERSION 2u

// out: SST_VERSION_OUT
#define IOCTL_SST_GET_VERSION    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x900, METHOD_BUFFERED, FILE_ANY_ACCESS)
// in: SST_ADD_MONITOR_IN, out: SST_ADD_MONITOR_OUT
#define IOCTL_SST_ADD_MONITOR    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x901, METHOD_BUFFERED, FILE_ANY_ACCESS)
// in: SST_REMOVE_MONITOR_IN
#define IOCTL_SST_REMOVE_MONITOR CTL_CODE(FILE_DEVICE_UNKNOWN, 0x902, METHOD_BUFFERED, FILE_ANY_ACCESS)
// in: SST_KEEPALIVE_IN. Must arrive at least every SST_KEEPALIVE_TIMEOUT_MS
// while a host has monitors, otherwise the driver removes that host's
// monitors (it crashed or hangs); other hosts' monitors stay.
#define IOCTL_SST_KEEPALIVE      CTL_CODE(FILE_DEVICE_UNKNOWN, 0x903, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define SST_KEEPALIVE_TIMEOUT_MS 5000u
#define SST_MAX_MONITORS 4u

typedef struct SST_VERSION_OUT {
    UINT32 Version;
} SST_VERSION_OUT;

typedef struct SST_ADD_MONITOR_IN {
    UINT32 Width;
    UINT32 Height;
    UINT32 RefreshRate;  // Hz
    UINT32 Serial;       // stable per device, so Windows remembers its layout
    UINT64 Owner;        // random per host process
} SST_ADD_MONITOR_IN;

typedef struct SST_ADD_MONITOR_OUT {
    UINT32 MonitorId;
} SST_ADD_MONITOR_OUT;

typedef struct SST_REMOVE_MONITOR_IN {
    UINT32 MonitorId;
    UINT64 Owner;        // only the host that added a monitor can remove it
} SST_REMOVE_MONITOR_IN;

typedef struct SST_KEEPALIVE_IN {
    UINT64 Owner;
} SST_KEEPALIVE_IN;
