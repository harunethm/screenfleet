#include "theme.h"

#include <dwmapi.h>

namespace sst::app {

namespace {

Theme MakeLight() {
    Theme t;
    t.dark = false;
    t.ground = Hex(0xf2f4f7);
    t.surface = Hex(0xffffff);
    t.raised = Hex(0xeeeeee);
    t.active = Hex(0xe9ebee);
    t.border = Hex(0xe6e6e6);
    t.rule = Hex(0xf0f0f0);
    t.control = Hex(0xd4d4d4);
    t.text = Hex(0x0a0a0a);
    t.muted = Hex(0x5f6368);
    t.faint = Hex(0x6b6b6b);
    t.primary = Hex(0x141414);
    t.primaryHover = Hex(0x2e2e2e);
    t.primaryInk = Hex(0xffffff);
    t.secondary = Hex(0xe5e5e5);
    t.secondaryHover = Hex(0xd4d4d4);
    t.positive = Hex(0x247b50);
    t.negative = Hex(0xc4242a);
    t.warning = Hex(0x92400e);
    t.chart = Hex(0x1d5fbf);
    t.grid = Hex(0xe5e5e5);
    t.pillPositiveBg = Hex(0xdcfae5);
    t.pillPositiveInk = Hex(0x04250e);
    t.pillNegativeBg = Hex(0xffedea);
    t.pillNegativeInk = Hex(0x831c1e);
    t.pillWarningBg = Hex(0xfffbeb);
    t.pillWarningInk = Hex(0x92400e);
    t.pillNeutralBg = Hex(0xeeeeee);
    t.pillNeutralInk = Hex(0x3d3d3d);
    t.noticeBorder = Hex(0xfde68a);
    t.deskMonitor = Hex(0xffffff);
    t.deskTablet = Hex(0x141414);
    return t;
}

Theme MakeDark() {
    Theme t;
    t.dark = true;
    t.ground = Hex(0x111111);
    t.surface = Hex(0x1a1a1a);
    t.raised = Hex(0x262626);
    t.active = Hex(0x2a2a2a);
    t.border = Hex(0x2e2e2e);
    t.rule = Hex(0x242424);
    t.control = Hex(0x4c4c4c);
    t.text = Hex(0xffffff);
    t.muted = Hex(0xb4b4b4);
    t.faint = Hex(0x949494);
    t.primary = Hex(0xffffff);
    t.primaryHover = Hex(0xe0e0e0);
    t.primaryInk = Hex(0x0a0a0a);
    t.secondary = Hex(0x2e2e2e);
    t.secondaryHover = Hex(0x3a3a3a);
    t.positive = Hex(0x3fc07a);
    t.negative = Hex(0xe5566f);
    t.warning = Hex(0xf0b45a);
    t.chart = Hex(0x5b93e6);
    t.grid = Hex(0x2b2b2b);
    t.pillPositiveBg = Hex(0x172a1d);
    t.pillPositiveInk = Hex(0x3fc07a);
    t.pillNegativeBg = Hex(0x2c1a20);
    t.pillNegativeInk = Hex(0xe5566f);
    t.pillWarningBg = Hex(0x2a2214);
    t.pillWarningInk = Hex(0xf0b45a);
    t.pillNeutralBg = Hex(0x262626);
    t.pillNeutralInk = Hex(0xd0d0d0);
    t.noticeBorder = Hex(0x4a3a1a);
    t.deskMonitor = Hex(0x262626);
    t.deskTablet = Hex(0xffffff);
    return t;
}

bool ReadPersonalize(const wchar_t* value) {
    DWORD data = 1, size = sizeof(data);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", value,
                 RRF_RT_REG_DWORD, nullptr, &data, &size);
    return data == 0;  // 0 = dark
}

}  // namespace

D2D1_COLOR_F Hex(uint32_t rgb, float alpha) {
    return D2D1::ColorF(((rgb >> 16) & 0xff) / 255.0f, ((rgb >> 8) & 0xff) / 255.0f, (rgb & 0xff) / 255.0f, alpha);
}

COLORREF ToColorRef(const D2D1_COLOR_F& c) {
    return RGB(BYTE(c.r * 255 + 0.5f), BYTE(c.g * 255 + 0.5f), BYTE(c.b * 255 + 0.5f));
}

const Theme& GetTheme(bool dark) {
    static const Theme light = MakeLight();
    static const Theme darkTheme = MakeDark();
    return dark ? darkTheme : light;
}

bool AppsUseDarkMode() { return ReadPersonalize(L"AppsUseLightTheme"); }
bool SystemUsesDarkMode() { return ReadPersonalize(L"SystemUsesLightTheme"); }

void ApplyTitleBarTheme(HWND hwnd, bool dark) {
    BOOL value = dark;
    DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &value, sizeof(value));
}

void SetMenusDark(bool dark) {
    static HMODULE uxtheme = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!uxtheme) return;
    using SetPreferredAppMode = int(WINAPI*)(int);
    using FlushMenuThemes = void(WINAPI*)();
    auto setMode = reinterpret_cast<SetPreferredAppMode>(GetProcAddress(uxtheme, MAKEINTRESOURCEA(135)));
    auto flush = reinterpret_cast<FlushMenuThemes>(GetProcAddress(uxtheme, MAKEINTRESOURCEA(136)));
    if (setMode) setMode(dark ? 2 /* ForceDark */ : 3 /* ForceLight */);
    if (flush) flush();
}

}  // namespace sst::app
