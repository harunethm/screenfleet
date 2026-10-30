#include "settings.h"

#include <windows.h>
#include <shlobj.h>

#include <string>

#include "log.h"

namespace sst {

namespace {

constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"ScreenFleet";

std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring out(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), out.data(), n);
    return out;
}

std::wstring SettingsPath() {
    PWSTR appData = nullptr;
    std::wstring path;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appData))) {
        path = std::wstring(appData) + L"\\ScreenFleet";
        CoTaskMemFree(appData);
        CreateDirectoryW(path.c_str(), nullptr);
        path += L"\\settings.ini";
    }
    return path;
}

class Ini {
public:
    explicit Ini(std::wstring path) : path_(std::move(path)) {}

    std::string Get(const wchar_t* section, const wchar_t* key, const char* def = "") const {
        wchar_t buf[1024];
        GetPrivateProfileStringW(section, key, Widen(def).c_str(), buf, ARRAYSIZE(buf), path_.c_str());
        return Narrow(buf);
    }
    int64_t GetInt(const wchar_t* section, const wchar_t* key, int64_t def) const {
        std::string v = Get(section, key);
        if (v.empty()) return def;
        try {
            return std::stoll(v);
        } catch (...) {
            return def;
        }
    }
    bool GetBool(const wchar_t* section, const wchar_t* key, bool def) const { return GetInt(section, key, def) != 0; }

    void Set(const wchar_t* section, const wchar_t* key, const std::string& v) const {
        WritePrivateProfileStringW(section, key, Widen(v).c_str(), path_.c_str());
    }
    void SetInt(const wchar_t* section, const wchar_t* key, int64_t v) const { Set(section, key, std::to_string(v)); }
    void ClearSection(const wchar_t* section) const {
        WritePrivateProfileStringW(section, nullptr, nullptr, path_.c_str());
    }

private:
    std::wstring path_;
};

}  // namespace

Settings LoadSettings() {
    Settings s;
    std::wstring path = SettingsPath();
    if (path.empty()) return s;
    Ini ini(path);
    s.hostId = ini.Get(L"host", L"id");
    s.usb = ini.GetBool(L"connect", L"usb", true);
    s.wifi = ini.GetBool(L"connect", L"wifi", true);
    s.autoConnect = ini.GetBool(L"connect", L"auto", true);
    for (int i = 0; i < 16; ++i) {
        std::string a = ini.Get(L"connect", (L"address" + std::to_wstring(i)).c_str());
        if (!a.empty()) s.wifiAddresses.push_back(a);
    }
    s.preset = ini.Get(L"quality", L"preset", "balanced");
    s.fps = uint32_t(ini.GetInt(L"quality", L"fps", 60));
    s.bitrateMbps = uint32_t(ini.GetInt(L"quality", L"bitrate", 8));
    for (int i = 0; i < 16; ++i) {
        std::wstring sec = L"tablet" + std::to_wstring(i);
        KnownTablet t;
        t.name = ini.Get(sec.c_str(), L"name");
        if (t.name.empty()) continue;
        t.id = ini.Get(sec.c_str(), L"id");
        t.width = uint32_t(ini.GetInt(sec.c_str(), L"width", 0));
        t.height = uint32_t(ini.GetInt(sec.c_str(), L"height", 0));
        t.sdk = uint32_t(ini.GetInt(sec.c_str(), L"sdk", 0));
        t.link = ini.Get(sec.c_str(), L"link", "USB");
        t.address = ini.Get(sec.c_str(), L"address");
        t.lastSeen = ini.GetInt(sec.c_str(), L"lastSeen", 0);
        t.pairKey = ini.Get(sec.c_str(), L"pairKey");
        t.allowTouch = ini.GetBool(sec.c_str(), L"allowTouch", true);
        s.tablets.push_back(t);
    }
    return s;
}

void SaveSettings(const Settings& s) {
    std::wstring path = SettingsPath();
    if (path.empty()) return;
    Ini ini(path);
    ini.ClearSection(L"display");  // mirror settings from before; Windows' own settings do that now
    ini.Set(L"host", L"id", s.hostId);
    ini.ClearSection(L"connect");
    ini.SetInt(L"connect", L"usb", s.usb);
    ini.SetInt(L"connect", L"wifi", s.wifi);
    ini.SetInt(L"connect", L"auto", s.autoConnect);
    for (size_t i = 0; i < s.wifiAddresses.size() && i < 16; ++i)
        ini.Set(L"connect", (L"address" + std::to_wstring(i)).c_str(), s.wifiAddresses[i]);
    ini.Set(L"quality", L"preset", s.preset);
    ini.SetInt(L"quality", L"fps", s.fps);
    ini.SetInt(L"quality", L"bitrate", s.bitrateMbps);
    for (int i = 0; i < 16; ++i) ini.ClearSection((L"tablet" + std::to_wstring(i)).c_str());
    for (size_t i = 0; i < s.tablets.size() && i < 16; ++i) {
        std::wstring sec = L"tablet" + std::to_wstring(i);
        const KnownTablet& t = s.tablets[i];
        ini.Set(sec.c_str(), L"name", t.name);
        if (!t.id.empty()) ini.Set(sec.c_str(), L"id", t.id);
        ini.SetInt(sec.c_str(), L"width", t.width);
        ini.SetInt(sec.c_str(), L"height", t.height);
        ini.SetInt(sec.c_str(), L"sdk", t.sdk);
        ini.Set(sec.c_str(), L"link", t.link);
        ini.Set(sec.c_str(), L"address", t.address);
        ini.SetInt(sec.c_str(), L"lastSeen", t.lastSeen);
        if (!t.pairKey.empty()) ini.Set(sec.c_str(), L"pairKey", t.pairKey);
        ini.SetInt(sec.c_str(), L"allowTouch", t.allowTouch);
    }
}

bool GetStartWithWindows() {
    wchar_t buf[MAX_PATH * 2];
    DWORD size = sizeof(buf);
    return RegGetValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS;
}

void SetStartWithWindows(bool on) {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) return;
    if (on) {
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring cmd = L"\"" + std::wstring(exe) + L"\" --tray";
        RegSetValueExW(key, kRunValue, 0, REG_SZ, reinterpret_cast<const BYTE*>(cmd.c_str()),
                       DWORD((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(key, kRunValue);
    }
    RegCloseKey(key);
}

std::string AndroidVersionName(uint32_t sdk) {
    switch (sdk) {
    case 19: return "4.4";
    case 20: return "4.4W";
    case 21: return "5.0";
    case 22: return "5.1";
    case 23: return "6";
    case 24: return "7.0";
    case 25: return "7.1";
    case 26: return "8.0";
    case 27: return "8.1";
    case 28: return "9";
    case 29: return "10";
    case 30: return "11";
    case 31: return "12";
    case 32: return "12L";
    case 33: return "13";
    case 34: return "14";
    case 35: return "15";
    case 36: return "16";
    default: return sdk ? "API " + std::to_string(sdk) : "";
    }
}

}  // namespace sst
