#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace sst {

// A tablet that has connected before (shown in the Tablets list).
struct KnownTablet {
    std::string id;        // the tablet's own id; empty for tablets from before ids
    std::string name;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t sdk = 0;
    std::string link;      // "USB" or "Wi-Fi"
    std::string address;   // Wi-Fi address it last connected from, if any
    int64_t lastSeen = 0;  // Unix time
    std::string pairKey;   // Wi-Fi pairing secret, DPAPI-encrypted (readable only by this Windows user); empty = not paired
    bool allowTouch = true;  // its touches and pen move this PC's pointer
};

struct Settings {
    std::string hostId;      // random, made once; tablets know this PC by it
    bool usb = true;
    bool wifi = true;
    std::vector<std::string> wifiAddresses;  // "192.168.1.42" or "host:port"
    std::string preset = "balanced";         // latency | balanced | sharp | custom
    uint32_t fps = 60;
    uint32_t bitrateMbps = 8;
    bool autoConnect = true;
    std::vector<KnownTablet> tablets;
};

struct Preset {
    const char* id;
    const char* name;
    uint32_t fps;
    uint32_t mbps;
};
inline constexpr Preset kPresets[] = {
    {"latency", "Low latency", 60, 5},
    {"balanced", "Balanced", 60, 8},
    {"sharp", "Sharp", 30, 12},
};

// %APPDATA%\ScreenFleet\settings.ini
Settings LoadSettings();
void SaveSettings(const Settings& s);

// "Start with Windows": a value under HKCU\...\Run pointing at this exe.
bool GetStartWithWindows();
void SetStartWithWindows(bool on);

std::string AndroidVersionName(uint32_t sdk);

}  // namespace sst
