#pragma once

#include <windows.h>
#include <d2d1.h>

#include <cstdint>

namespace sst::app {

// Colour tokens, the same as the Android app's (Ui.java).
struct Theme {
    bool dark = false;
    D2D1_COLOR_F ground, surface, raised, active, border, rule, control;
    D2D1_COLOR_F text, muted, faint;
    D2D1_COLOR_F primary, primaryHover, primaryInk, secondary, secondaryHover;
    D2D1_COLOR_F positive, negative, warning, chart, grid;
    D2D1_COLOR_F pillPositiveBg, pillPositiveInk, pillNegativeBg, pillNegativeInk;
    D2D1_COLOR_F pillWarningBg, pillWarningInk, pillNeutralBg, pillNeutralInk, noticeBorder;
    D2D1_COLOR_F deskMonitor, deskTablet;
};

D2D1_COLOR_F Hex(uint32_t rgb, float alpha = 1.0f);
COLORREF ToColorRef(const D2D1_COLOR_F& c);
const Theme& GetTheme(bool dark);

// Windows' "app mode" setting (Settings > Personalization > Colors).
bool AppsUseDarkMode();
// The taskbar's mode, for the tray icon's colour.
bool SystemUsesDarkMode();
void ApplyTitleBarTheme(HWND hwnd, bool dark);
// Lets standard popup menus follow dark mode (undocumented uxtheme exports;
// silently does nothing where they're missing).
void SetMenusDark(bool dark);

}  // namespace sst::app
