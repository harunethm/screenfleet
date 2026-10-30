#include "main_window.h"

#include <commctrl.h>
#include <uxtheme.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <ctime>
#include <filesystem>

#include "../driver_info.h"
#include "../log.h"
#include "tray_icon.h"

namespace sst::app {

namespace {

constexpr wchar_t kClassName[] = L"ScreenFleetWindow";
// DIPs. The window opens wide enough for three columns on a 2K monitor and
// falls back to two, then one, when made narrower.
constexpr float kDefaultWidth = 1320;
constexpr float kDefaultHeight = 1000;
constexpr float kMinWidth = 440;      // one 408-wide column
constexpr float kMargin = 16;
constexpr float kColumnGap = 16;
constexpr float kPad = 16;            // card padding
constexpr float kGap = 12;
constexpr float kCardRadius = 8;
constexpr float kControlRadius = 6;
constexpr float kControlHeight = 36;
constexpr float kChartHeight = 132;

enum TrayCommand : UINT { kTrayHead = 1, kTraySub, kTrayOpen, kTrayAction, kTrayQuit };

std::wstring W(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring out(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), out.data(), n);
    return out;
}

std::wstring Dims(uint32_t w, uint32_t h) { return std::to_wstring(w) + L" × " + std::to_wstring(h); }

std::wstring Fixed1(double v) {
    wchar_t b[32];
    swprintf_s(b, L"%.1f", v);
    return b;
}

const wchar_t* kWifi = L"Wi‑Fi";

D2D1_RECT_F Rect(float x, float y, float w, float h) { return {x, y, x + w, y + h}; }

bool Inside(const D2D1_RECT_F& r, float x, float y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }

std::wstring LastSeen(int64_t t) {
    if (!t) return L"";
    time_t tt = time_t(t);
    tm local{};
    localtime_s(&local, &tt);
    time_t now = time(nullptr);
    tm today{};
    localtime_s(&today, &now);
    wchar_t b[32];
    if (local.tm_year == today.tm_year && local.tm_yday == today.tm_yday)
        wcsftime(b, 32, L"%H:%M", &local);
    else
        wcsftime(b, 32, L"%d %b", &local);
    return b;
}

std::wstring LinkName(const std::string& link) { return link == "Wi-Fi" ? kWifi : W(link); }

}  // namespace

MainWindow::~MainWindow() {
    UpdateTray(false);
    if (editFont_) DeleteObject(editFont_);
    if (editBrush_) DeleteObject(editBrush_);
    for (HICON i : {windowIcon_, smallIcon_, trayIcon_})
        if (i) DestroyIcon(i);
}

bool MainWindow::Create(HINSTANCE instance, bool visible) {
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d_.GetAddressOf()))) return false;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory5),
                                   reinterpret_cast<IUnknown**>(dwrite_.GetAddressOf())))) {
        return false;
    }
    if (!fonts_.Init(dwrite_.Get())) return false;
    canvas_ = std::make_unique<Canvas>(dwrite_.Get(), &fonts_);
    const float dashes[] = {3, 2};
    d2d_->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT,
                                                        D2D1_LINE_JOIN_MITER, 10, D2D1_DASH_STYLE_CUSTOM, 0),
                            dashes, 2, &dashed_);

    // GDI needs the font too, for the address box (a native EDIT control).
    for (int id : {101, 102}) {
        HRSRC res = FindResourceW(instance, MAKEINTRESOURCEW(id), RT_RCDATA);
        DWORD count = 0;
        HGLOBAL data = res ? LoadResource(instance, res) : nullptr;
        if (data) AddFontMemResourceEx(LockResource(data), SizeofResource(instance, res), nullptr, &count);
    }

    wchar_t name[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD len = ARRAYSIZE(name);
    if (GetComputerNameW(name, &len)) pcName_ = Narrow(name);
    auto outputs = PhysicalOutputs();
    if (!outputs.empty()) gpuName_ = outputs[0].adapterName;
    startWithWindows_ = GetStartWithWindows();
    status_ = engine_.Status();
    settings_ = engine_.GetSettings();
    driverInfoKnown_ = QueryDriverInfo(driverVersion_, driverDate_);

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);

    dark_ = AppsUseDarkMode();
    SetMenusDark(dark_);
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_THICKFRAME |
                  WS_CLIPCHILDREN;
    hwnd_ = CreateWindowExW(0, kClassName, L"ScreenFleet", style, CW_USEDEFAULT, CW_USEDEFAULT, 480, 820, nullptr,
                            nullptr, instance, this);
    if (!hwnd_) return false;
    dpi_ = GetDpiForWindow(hwnd_);

    edit_ = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 0, 0, 10, 10, hwnd_,
                            nullptr, instance, nullptr);
    SendMessageW(edit_, EM_SETCUEBANNER, FALSE, reinterpret_cast<LPARAM>(L"192.168.1.42"));
    SetWindowSubclass(edit_, EditProc, 1, reinterpret_cast<DWORD_PTR>(this));
    pairEdit_ = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL | ES_NUMBER, 0, 0, 10, 10, hwnd_,
                                nullptr, instance, nullptr);
    SendMessageW(pairEdit_, EM_SETCUEBANNER, FALSE, reinterpret_cast<LPARAM>(L"123456"));
    SendMessageW(pairEdit_, EM_SETLIMITTEXT, 6, 0);
    SetWindowSubclass(pairEdit_, EditProc, 1, reinterpret_cast<DWORD_PTR>(this));
    UpdateEditFont();
    ApplyTheme();

    // Size the client area in DIPs (smaller on small screens) and centre it.
    RECT r{0, 0, ToPx(kDefaultWidth), ToPx(kDefaultHeight)};
    AdjustWindowRectExForDpi(&r, style, FALSE, 0, dpi_);
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    int w = std::min<int>(r.right - r.left, work.right - work.left - 24);
    int h = std::min<int>(r.bottom - r.top, work.bottom - work.top - 24);
    SetWindowPos(hwnd_, nullptr, work.left + (work.right - work.left - w) / 2, work.top + (work.bottom - work.top - h) / 2,
                 w, h, SWP_NOZORDER | SWP_NOACTIVATE);

    windowIcon_ = CreateMarkIcon(GetSystemMetricsForDpi(SM_CXICON, dpi_), dark_);
    smallIcon_ = CreateMarkIcon(GetSystemMetricsForDpi(SM_CXSMICON, dpi_), dark_);
    SendMessageW(hwnd_, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(windowIcon_));
    SendMessageW(hwnd_, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(smallIcon_));

    taskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");
    UpdateTray(true);
    if (visible) Show();
    return true;
}

void MainWindow::PostStatusChanged() {
    // Coalesce: the engine may report many times a second.
    if (!statusPending_.exchange(true)) PostMessageW(hwnd_, kMsgStatus, 0, 0);
}

LRESULT CALLBACK MainWindow::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    MainWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        self = static_cast<MainWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    return self ? self->Handle(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT CALLBACK MainWindow::EditProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR ref) {
    auto* self = reinterpret_cast<MainWindow*>(ref);
    const bool code = hwnd == self->pairEdit_;
    if (msg == WM_KEYDOWN && wp == VK_RETURN) {
        self->Activate({code ? Act::PairSubmit : Act::AddressConnect, -1});
        return 0;
    }
    if (msg == WM_KEYDOWN && wp == VK_TAB) {
        SetFocus(self->hwnd_);
        self->focusVisible_ = true;
        self->MoveFocus(GetKeyState(VK_SHIFT) < 0 ? -1 : 1);
        return 0;
    }
    if (msg == WM_CHAR && (wp == VK_RETURN || wp == VK_TAB)) return 0;
    if (msg == WM_SETFOCUS) {
        self->focus_ = {code ? Act::PairCode : Act::Address, -1};
        InvalidateRect(self->hwnd_, nullptr, FALSE);
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

LRESULT MainWindow::Handle(UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == taskbarCreated_ && taskbarCreated_) {
        UpdateTray(true);
        return 0;
    }
    switch (msg) {
    case WM_PAINT:
        Paint();
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        if (rt_) rt_->Resize({LOWORD(lp), HIWORD(lp)});
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    case WM_GETMINMAXINFO: {
        auto* mm = reinterpret_cast<MINMAXINFO*>(lp);
        RECT r{0, 0, ToPx(kMinWidth), ToPx(360)};
        AdjustWindowRectExForDpi(&r, DWORD(GetWindowLongW(hwnd_, GWL_STYLE)), FALSE, 0, dpi_);
        mm->ptMinTrackSize = {r.right - r.left, r.bottom - r.top};
        return 0;
    }
    case WM_DPICHANGED: {
        dpi_ = HIWORD(wp);
        auto* r = reinterpret_cast<RECT*>(lp);
        SetWindowPos(hwnd_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        if (rt_) rt_->SetDpi(float(dpi_), float(dpi_));
        UpdateEditFont();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    }
    case WM_SETTINGCHANGE:
        if (lp && wcscmp(reinterpret_cast<const wchar_t*>(lp), L"ImmersiveColorSet") == 0) {
            dark_ = AppsUseDarkMode();
            ApplyTheme();
            UpdateTray(true);
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return 0;
    case WM_CTLCOLOREDIT: {
        HDC dc = reinterpret_cast<HDC>(wp);
        SetTextColor(dc, ToColorRef(T().text));
        SetBkColor(dc, ToColorRef(T().surface));
        return reinterpret_cast<LRESULT>(editBrush_);
    }
    case WM_MOUSEMOVE: {
        float x = ToDip(GET_X_LPARAM(lp)), y = ToDip(GET_Y_LPARAM(lp)) + scrollY_;
        if (dragging_) {
            SetBitrateFromX(x);
            return 0;
        }
        const Hit* h = HitAt(x, y);
        Key k = h && !h->disabled ? Key{h->act, h->option} : Key{};
        if (!(k == hover_)) {
            hover_ = k;
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd_, 0};
        TrackMouseEvent(&tme);
        return 0;
    }
    case WM_MOUSELEAVE:
        hover_ = {};
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT && hover_.act != Act::None) {
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return TRUE;
        }
        break;
    case WM_LBUTTONDOWN: {
        SetFocus(hwnd_);
        focusVisible_ = false;
        float x = ToDip(GET_X_LPARAM(lp)), y = ToDip(GET_Y_LPARAM(lp)) + scrollY_;
        const Hit* h = HitAt(x, y);
        if (h && !h->disabled) {
            pressed_ = {h->act, h->option};
            focus_ = pressed_;
            if (h->act == Act::Bitrate) {
                dragging_ = true;
                SetCapture(hwnd_);
                SetBitrateFromX(x);
            }
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    }
    case WM_LBUTTONUP: {
        float x = ToDip(GET_X_LPARAM(lp)), y = ToDip(GET_Y_LPARAM(lp)) + scrollY_;
        if (dragging_) {
            dragging_ = false;
            ReleaseCapture();
            ApplySettings(settings_, true);
            pressed_ = {};
            return 0;
        }
        const Hit* h = HitAt(x, y);
        Key k = pressed_;
        pressed_ = {};
        if (h && !h->disabled && Key{h->act, h->option} == k) Activate(k, x);
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    }
    case WM_MOUSEWHEEL:
        ScrollBy(-GET_WHEEL_DELTA_WPARAM(wp) / 120.0f * 48);
        return 0;
    case WM_KEYDOWN:
        switch (wp) {
        case VK_TAB:
            focusVisible_ = true;
            MoveFocus(GetKeyState(VK_SHIFT) < 0 ? -1 : 1);
            return 0;
        case VK_SPACE:
        case VK_RETURN:
            if (focus_.act != Act::None) {
                focusVisible_ = true;
                Activate(focus_);
            }
            return 0;
        case VK_LEFT:
        case VK_RIGHT:
            if (focus_.act == Act::Bitrate) {
                Settings s = settings_;
                int step = wp == VK_RIGHT ? 1 : -1;
                s.bitrateMbps = uint32_t(std::clamp<int>(int(s.bitrateMbps) + step, 2, 20));
                s.preset = "custom";
                ApplySettings(s, true);
            } else if (focus_.option >= 0) {
                // move within a segmented control
                int next = focus_.option + (wp == VK_RIGHT ? 1 : -1);
                for (const Hit& h : hits_)
                    if (h.act == focus_.act && h.option == next && !h.disabled) {
                        focus_.option = next;
                        Activate(focus_);
                        break;
                    }
            }
            return 0;
        case VK_UP:
            ScrollBy(-48);
            return 0;
        case VK_DOWN:
            ScrollBy(48);
            return 0;
        case VK_ESCAPE:
            ShowWindow(hwnd_, SW_HIDE);
            return 0;
        }
        break;
    case WM_CLOSE:
        // Closing the window keeps streaming in the tray; Quit is in the tray menu.
        ShowWindow(hwnd_, SW_HIDE);
        return 0;
    case WM_DESTROY:
        UpdateTray(false);
        PostQuitMessage(0);
        return 0;
    case kMsgStatus: {
        statusPending_ = false;
        bool hadDriver = status_.driverInstalled;
        bool wantedCode = status_.pairing.active && status_.pairing.stage == PairStage::EnterCode;
        status_ = engine_.Status();
        if (!wantedCode && status_.pairing.active && status_.pairing.stage == PairStage::EnterCode) {
            SetWindowTextW(pairEdit_, L"");
            Show();
            SetFocus(pairEdit_);
        }
        settings_ = engine_.GetSettings();
        if (hadDriver != status_.driverInstalled) driverInfoKnown_ = QueryDriverInfo(driverVersion_, driverDate_);
        UpdateTray(false);  // refresh the tooltip
        if (IsWindowVisible(hwnd_)) InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    }
    case kMsgShow:
        Show();
        return 0;
    case kMsgTray:
        switch (LOWORD(lp)) {
        case NIN_SELECT:
        case NIN_KEYSELECT:
            if (IsWindowVisible(hwnd_) && GetForegroundWindow() == hwnd_)
                ShowWindow(hwnd_, SW_HIDE);
            else
                Show();
            return 0;
        case WM_CONTEXTMENU:
            ShowTrayMenu();
            return 0;
        }
        return 0;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

// ---------------------------------------------------------------- window & tray

void MainWindow::Show() {
    ShowWindow(hwnd_, SW_SHOWNORMAL);
    SetForegroundWindow(hwnd_);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::ApplyTheme() {
    ApplyTitleBarTheme(hwnd_, dark_);
    SetMenusDark(dark_);
    if (editBrush_) DeleteObject(editBrush_);
    editBrush_ = CreateSolidBrush(ToColorRef(T().surface));
    for (HWND e : {edit_, pairEdit_}) {
        SetWindowTheme(e, dark_ ? L"DarkMode_Explorer" : L"Explorer", nullptr);
        InvalidateRect(e, nullptr, TRUE);
    }
}

void MainWindow::UpdateTray(bool add) {
    NOTIFYICONDATAW nid{sizeof(nid)};
    nid.hWnd = hwnd_;
    nid.uID = 1;
    if (!add && !IsWindow(hwnd_)) return;
    std::wstring tip = L"ScreenFleet";
    if (status_.tablets.size() > 1) {
        tip += L" · " + std::to_wstring(status_.tablets.size()) + L" tablets";
    } else if (status_.state == EngineState::Streaming) {
        int fps = status_.fps.empty() ? 0 : status_.fps.back();
        tip += L" · " + W(status_.device) + L" · " + std::to_wstring(fps) + L" fps";
    } else if (status_.state == EngineState::Error) {
        tip += L" · " + W(status_.device) + L" disconnected";
    }
    wcsncpy_s(nid.szTip, tip.c_str(), _TRUNCATE);
    if (add) {
        bool white = SystemUsesDarkMode();
        if (trayIcon_) DestroyIcon(trayIcon_);
        trayIcon_ = CreateMarkIcon(GetSystemMetricsForDpi(SM_CXSMICON, dpi_), white);
        nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
        nid.uCallbackMessage = kMsgTray;
        nid.hIcon = trayIcon_;
        Shell_NotifyIconW(NIM_DELETE, &nid);
        Shell_NotifyIconW(NIM_ADD, &nid);
        nid.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &nid);
    } else {
        nid.uFlags = NIF_TIP | NIF_SHOWTIP;
        Shell_NotifyIconW(NIM_MODIFY, &nid);
    }
}

void MainWindow::ShowTrayMenu() {
    HMENU menu = CreatePopupMenu();
    std::wstring head, sub, action;
    switch (status_.state) {
    case EngineState::Streaming: {
        head = status_.tablets.size() > 1 ? std::to_wstring(status_.tablets.size()) + L" tablets" : W(status_.device);
        int fps = status_.fps.empty() ? 0 : status_.fps.back();
        sub = std::to_wstring(fps) + L" fps · " + Fixed1(status_.mbps) + L" Mbit/s";
        break;
    }
    case EngineState::Connecting:
        head = L"Connecting to " + W(status_.device);
        sub = LinkName(status_.link);
        break;
    case EngineState::Error:
        head = W(status_.device) + L" disconnected";
        sub = W(status_.endedAt) + L" · retrying";
        break;
    default:
        head = status_.paused ? L"Stopped" : L"No tablet connected";
        sub = status_.paused ? L"Not looking for tablets" : std::wstring(L"Looking on USB and ") + kWifi;
        break;
    }
    action = status_.paused ? L"Start" : L"Stop";
    AppendMenuW(menu, MF_STRING | MF_GRAYED, kTrayHead, head.c_str());
    AppendMenuW(menu, MF_STRING | MF_GRAYED, kTraySub, sub.c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kTrayOpen, L"Open ScreenFleet");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kTrayAction, action.c_str());
    AppendMenuW(menu, MF_STRING, kTrayQuit, L"Quit");
    SetMenuDefaultItem(menu, kTrayOpen, FALSE);

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd_);
    UINT cmd = UINT(TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, hwnd_, nullptr));
    DestroyMenu(menu);
    switch (cmd) {
    case kTrayOpen:
        Show();
        break;
    case kTrayAction:
        if (status_.paused) engine_.Connect();
        else engine_.Disconnect();
        break;
    case kTrayQuit:
        DestroyWindow(hwnd_);
        break;
    }
}

void MainWindow::UpdateEditFont() {
    if (editFont_) DeleteObject(editFont_);
    editFont_ = CreateFontW(-MulDiv(14, int(dpi_), 96), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Geist");
    for (HWND e : {edit_, pairEdit_}) SendMessageW(e, WM_SETFONT, reinterpret_cast<WPARAM>(editFont_), TRUE);
}

void MainWindow::PositionEdit() {
    auto place = [&](HWND edit, const D2D1_RECT_F& r) {
        if (r.right <= r.left) {
            ShowWindow(edit, SW_HIDE);
            return;
        }
        // Inside our drawn field border, vertically centred on one text line.
        float textH = 20;
        float x = r.left + 10, w = r.right - r.left - 20;
        float y = r.top + (kControlHeight - textH) / 2 - scrollY_;
        SetWindowPos(edit, nullptr, ToPx(x), ToPx(y), ToPx(w), ToPx(textH), SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    };
    place(edit_, editRect_);
    place(pairEdit_, pairEditRect_);
}

// ---------------------------------------------------------------- painting

bool MainWindow::EnsureTarget() {
    if (rt_) return true;
    RECT rc;
    GetClientRect(hwnd_, &rc);
    auto props = D2D1::RenderTargetProperties();
    props.dpiX = props.dpiY = float(dpi_);
    if (FAILED(d2d_->CreateHwndRenderTarget(props, D2D1::HwndRenderTargetProperties(hwnd_, {UINT32(rc.right), UINT32(rc.bottom)}),
                                            &rt_))) {
        return false;
    }
    rt_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    rt_->CreateSolidColorBrush(T().text, &brush_);
    return true;
}

void MainWindow::Paint() {
    PAINTSTRUCT ps;
    BeginPaint(hwnd_, &ps);
    if (EnsureTarget()) {
        Layout();
        RECT rc;
        GetClientRect(hwnd_, &rc);
        float viewH = ToDip(rc.bottom);
        scrollY_ = std::clamp(scrollY_, 0.0f, std::max(0.0f, contentHeight_ - viewH));

        rt_->BeginDraw();
        rt_->Clear(T().ground);
        canvas_->Render(rt_.Get(), brush_.Get(), dashed_.Get(), scrollY_);

        // keyboard focus ring
        if (focusVisible_ && focus_.act != Act::None && focus_.act != Act::Address && focus_.act != Act::PairCode) {
            for (const Hit& h : hits_) {
                if (h.act == focus_.act && h.option == focus_.option) {
                    brush_->SetColor(T().chart);
                    D2D1_RECT_F r{h.r.left - 2, h.r.top - 2 - scrollY_, h.r.right + 2, h.r.bottom + 2 - scrollY_};
                    rt_->DrawRoundedRectangle({r, 7, 7}, brush_.Get(), 2);
                    break;
                }
            }
        }
        // scroll indicator
        if (contentHeight_ > viewH + 1) {
            float trackH = viewH - 8;
            float barH = std::max(32.0f, trackH * viewH / contentHeight_);
            float barY = 4 + (trackH - barH) * scrollY_ / (contentHeight_ - viewH);
            brush_->SetColor(T().control);
            float viewW = ToDip(rc.right);
            rt_->FillRoundedRectangle({{viewW - 6, barY, viewW - 3, barY + barH}, 1.5f, 1.5f}, brush_.Get());
        }
        if (rt_->EndDraw() == D2DERR_RECREATE_TARGET) {
            rt_.Reset();
            brush_.Reset();
        }
        PositionEdit();
    }
    EndPaint(hwnd_, &ps);
}

void MainWindow::Layout() {
    canvas_->Reset();
    hits_.clear();
    editRect_ = {};
    pairEditRect_ = {};
    // Top: the status cards side by side, one per tablet (up to three a row).
    // Below: Tablets with Connection settings under it | Quality | Startup
    // and driver. Narrower windows use two columns, then one. Maximized,
    // everything stays together at the top centre.
    const float viewW = ViewWidth();
    const int columns = viewW >= 1100 ? 3 : viewW >= 820 ? 2 : 1;
    const float maxW = 1600;
    const float rowW = std::min(viewW, maxW) - 2 * kMargin;
    const float left = std::max(kMargin, (viewW - std::min(viewW, maxW)) / 2 + kMargin);
    const float top = -16;  // sections start with a group title

    const EngineStatus all = status_;
    const size_t cards = std::max<size_t>(1, all.tablets.size());
    const size_t perRow = std::min<size_t>(cards, columns >= 3 ? 3 : columns >= 2 ? 2 : 1);
    // several cards side by side: no desk drawing, so they stay short
    compact_ = all.tablets.size() > 1 ? 1 : 0;
    colX_ = left;
    colW_ = rowW;
    float y = GroupTitle(L"Status", top);
    const float cardW = std::floor((rowW - float(perRow - 1) * kColumnGap) / float(perRow));
    for (size_t i = 0; i < cards; i += perRow) {
        float rowBottom = y;
        for (size_t j = 0; j < perRow && i + j < cards; ++j) {
            colX_ = left + float(j) * (cardW + kColumnGap);
            colW_ = cardW;
            if (all.tablets.size() > 1) {
                status_ = all.For(i + j);
                card_ = int(i + j);
            }
            rowBottom = std::max(rowBottom, StatusCard(y));
        }
        y = rowBottom + kGap;
    }
    status_ = all;
    card_ = -1;
    const float sectionTop = y - kGap + top + 8;

    float widths[3] = {rowW, 0, 0};
    if (columns == 3) {
        widths[0] = std::floor((rowW - 2 * kColumnGap) * 0.38f);
        widths[1] = std::floor((rowW - 2 * kColumnGap - widths[0]) / 2);
        widths[2] = rowW - 2 * kColumnGap - widths[0] - widths[1];
    } else if (columns == 2) {
        widths[0] = std::floor((rowW - kColumnGap) / 2);
        widths[1] = rowW - kColumnGap - widths[0];
    }
    float bottom = y;
    int col = 0;
    float colY = sectionTop;
    auto place = [&](int c) {
        float x = left;
        for (int k = 0; k < c; ++k) x += widths[k] + kColumnGap;
        colX_ = x;
        colW_ = widths[c];
    };
    // which column each section goes in, by layout
    auto next = [&](int c) {
        bottom = std::max(bottom, colY);
        if (c != col) {
            col = c;
            colY = sectionTop;
        }
        place(c);
    };
    if (columns == 3) {
        next(0); colY = TabletsCard(colY); colY = ConnectionCard(colY);
        next(1); colY = QualityCard(colY);
        next(2); colY = StartupCard(colY);
    } else if (columns == 2) {
        next(0); colY = TabletsCard(colY); colY = ConnectionCard(colY);
        next(1); colY = QualityCard(colY); colY = StartupCard(colY);
    } else {
        next(0);
        colY = TabletsCard(colY);
        colY = ConnectionCard(colY);
        colY = QualityCard(colY);
        colY = StartupCard(colY);
    }
    std::wstring foot = L"ScreenFleet 1.0.0";
    if (!pcName_.empty()) foot += L" · " + W(pcName_);
    if (!gpuName_.empty()) foot += L" · " + W(gpuName_);
    colY += 12;
    colY += canvas_->Text(foot, Font::Small, colX_ + 4, colY, colW_ - 8, T().muted);
    contentHeight_ = std::max(bottom, colY) + 16;
}

float MainWindow::ViewWidth() const {
    RECT rc;
    GetClientRect(hwnd_, &rc);
    return std::max(kMinWidth, ToDip(rc.right));
}

void MainWindow::CardBackground(size_t mark, float top, float bottom) {
    // inserted in reverse order: the border goes above the fill
    canvas_->InsertStrokeRound(mark, Rect(colX_, top, colW_, bottom - top), kCardRadius, T().border);
    canvas_->InsertFillRound(mark, Rect(colX_, top, colW_, bottom - top), kCardRadius, T().surface);
}

void MainWindow::RowRule(float x, float y, float w) { canvas_->Line(x, y + 0.5f, x + w, y + 0.5f, T().rule); }

float MainWindow::GroupTitle(const wchar_t* text, float y) {
    y += 20;
    canvas_->Text(text, Font::Section, colX_ + 4, y, colW_, T().text);
    return y + 24 + 8;
}

float MainWindow::Row(float x, float y, float w, const std::wstring& label, const std::wstring& desc,
                      D2D1_SIZE_F control, const std::function<void(float, float)>& draw) {
    float textW = w - control.width - 12;
    TextBox l = canvas_->Measure(label, Font::BodyMedium, textW);
    TextBox d;
    if (!desc.empty()) d = canvas_->Measure(desc, Font::Small, textW);
    float textH = l.height + (desc.empty() ? 0 : 2 + d.height);
    float h = std::max(52.0f, std::max(textH, control.height) + 20);
    float ty = y + (h - textH) / 2;
    canvas_->Text(l, x, ty, T().text);
    if (!desc.empty()) canvas_->Text(d, x, ty + l.height + 2, T().muted);
    draw(x + w - control.width, y + (h - control.height) / 2);
    return h;
}

// ---------------------------------------------------------------- the status card

float MainWindow::StatusCard(float top) {
    const Theme& t = T();
    size_t mark = canvas_->Mark();
    float x = colX_ + kPad, w = colW_ - 2 * kPad, y = top + kPad;
    const EngineState state = status_.state;
    const bool streaming = state == EngineState::Streaming;
    const std::wstring device = W(status_.device);
    const std::wstring link = LinkName(status_.link);
    const std::wstring mainName = W(status_.mainName.empty() ? "your monitor" : status_.mainName);

    // title, subline and the state pill
    std::wstring title, sub, pill;
    D2D1_COLOR_F pillBg = t.pillNeutralBg, pillInk = t.pillNeutralInk;
    switch (state) {
    case EngineState::Waiting:
        title = L"No tablet connected";
        if (status_.paused) sub = L"Stopped. Press Start to look for tablets.";
        else if (settings_.usb && settings_.wifi) sub = std::wstring(L"Looking on USB and ") + kWifi;
        else if (settings_.usb) sub = L"Looking on USB";
        else if (settings_.wifi) sub = std::wstring(L"Looking on ") + kWifi;
        else sub = std::wstring(L"USB and ") + kWifi + L" are both off";
        pill = status_.paused ? L"Stopped" : L"Waiting";
        break;
    case EngineState::Connecting:
        title = device;
        sub = link + L" · Android " + W(AndroidVersionName(status_.sdk));
        pill = L"Connecting";
        break;
    case EngineState::Streaming:
        title = device;
        sub = link + L" · " + Dims(status_.width, status_.height) + L" at " + std::to_wstring(status_.refresh) +
              L" Hz · since " + W(status_.startedAt);
        pill = L"Streaming";
        pillBg = t.pillPositiveBg;
        pillInk = t.pillPositiveInk;
        break;
    case EngineState::Error:
        title = device;
        if (status_.needsPair) {
            sub = link + L" · " + W(status_.address);
            pill = L"Not paired";
            break;
        }
        sub = status_.startedAt.empty() ? link + L" · " + W(status_.endedAt)
                                        : L"Streamed " + W(status_.startedAt) + L"–" + W(status_.endedAt) + L" over " + link;
        pill = L"Disconnected";
        pillBg = t.pillNegativeBg;
        pillInk = t.pillNegativeInk;
        break;
    }
    D2D1_SIZE_F ps = PillSize(pill);
    Pill(pill, pillBg, pillInk, x + w - ps.width, y + 3);
    y += canvas_->Text(title, Font::Title, x, y, w - ps.width - 12, t.text, false);
    y += 2 + canvas_->Text(sub, Font::Body, x, y + 2, w - ps.width - 12, t.muted);
    // A wide card (one tablet across the window) puts the desk beside the rest.
    const float headerBottom = y;
    const float fullW = w;
    const bool split = !compact_ && fullW >= 860;
    if (split) w = std::floor(fullW * 0.56f);

    auto steps = [&](const std::vector<std::wstring>& lines, size_t doneCount) {
        y += 12;
        for (size_t i = 0; i < lines.size(); ++i) {
            D2D1_COLOR_F c = i < doneCount ? t.muted : t.text;
            canvas_->Text(std::to_wstring(i + 1) + L".", Font::Body, x, y, 20, c, false, true);
            y += canvas_->Text(lines[i], Font::Body, x + 20, y, w - 20, c) + 6;
        }
        y -= 6;
    };

    if (state == EngineState::Waiting && !status_.paused) {
        steps({L"Open ScreenFleet on the tablet.",
               std::wstring(L"Plug it into this PC with a USB cable, or add it under Network (") + kWifi + L").",
               L"It appears here and becomes a new monitor."},
              0);
    } else if (state == EngineState::Connecting) {
        std::vector<std::wstring> lines;
        for (auto& s : status_.steps) lines.push_back(W(s));
        steps(lines, lines.empty() ? 0 : lines.size() - 1);
    }

    if (state == EngineState::Connecting || streaming) y = Kpis(x, y + 16, w);
    if (streaming && compact_ < 2) {
        y += 16;
        canvas_->Text(L"Frames sent, last 60 s", Font::BodyMedium, x, y, w, t.text);
        TextBox note = canvas_->Measure(L"Only changes are sent", Font::Small, w);
        canvas_->Text(note, x + w - note.width, y + 3, t.muted);
        y = Chart(x, y + 24, w);
    }

    if (streaming) {
        // touch from this tablet (it can be turned off on either side)
        y += 12;
        RowRule(x, y, w);
        std::wstring desc;
        if (!status_.touch)
            desc = L"Off on the tablet (Touch controls the PC, in its menu).";
        else if (status_.allowTouch)
            desc = std::wstring(status_.pen ? L"Fingers and pen" : L"Fingers") +
                   L" on the tablet move this PC’s pointer on its monitor. Not in apps run as administrator.";
        else
            desc = L"Touches on the tablet are ignored.";
        y += Row(x, y, w, L"Touch controls this PC", desc, {40, 20}, [&](float cx, float cy) {
            Toggle(Act::AllowTouch, status_.allowTouch, cx, cy, card_);
        });
    }

    if (state == EngineState::Error) {
        y += 12;
        canvas_->StrokeEllipse(x + 8, y + 10, 6.5f, 6.5f, t.negative, 1.5f);
        canvas_->Line(x + 8, y + 6.5f, x + 8, y + 10.5f, t.negative, 1.6f);
        canvas_->FillEllipse(x + 8, y + 13, 0.9f, 0.9f, t.negative);
        std::wstring text = W(status_.error);
        if (!status_.needsPair && status_.error.find("Update") == std::string::npos)
            text += L" It reconnects on its own when the tablet is back.";
        y += canvas_->Text(text, Font::Body, x + 24, y, w - 24, t.text);
    }

    bool showNotice = !status_.driverInstalled;
    bool adbMissing = settings_.usb && !status_.adbFound;
    auto notice = [&](const std::wstring& text) {
        y += 12;
        TextBox b = canvas_->Measure(text, Font::Small, w - 24);
        D2D1_RECT_F r = Rect(x, y, w, b.height + 20);
        canvas_->FillRound(r, kControlRadius, t.pillWarningBg);
        canvas_->StrokeRound(r, kControlRadius, t.noticeBorder);
        canvas_->Text(b, x + 12, y + 10, t.pillWarningInk);
        y += b.height + 20;
    };
    if (showNotice)
        notice(L"The virtual display driver isn’t installed. The tablet becomes a monitor through it; installing "
               L"asks Windows for permission once.");
    if (adbMissing)
        notice(L"USB needs adb.exe from Android platform-tools. Install it, or connect over " + std::wstring(kWifi) + L".");

    // the desk, drawn to scale (one tablet only; with several the cards stay short)
    if (split) {
        float dx = x + w + 32;
        y = std::max(y, Desk(dx, headerBottom + 20, fullW - w - 32));
    } else if (!compact_) {
        y += 16;
        RowRule(x, y, w);
        y = Desk(x, y + 16, w);
    }
    w = fullW;

    // actions: Start or Stop is always there, so sharing can be switched off in any state
    float bx = x, by = y + 16;
    if (status_.paused) {
        Button(Act::Connect, L"Start", ButtonStyle::Primary, bx, by);
    } else if (state == EngineState::Error && status_.needsPair) {
        bx += Button(Act::PairCard, L"Pair", ButtonStyle::Primary, bx, by, false, card_) + 8;
        if (card_ <= 0) Button(Act::Disconnect, card_ == 0 ? L"Stop all" : L"Stop", ButtonStyle::Secondary, bx, by);
    } else if (card_ > 0) {
        // further tablets' cards: Stop (for all of them) is on the first card
        if (state == EngineState::Error) {
            bx += Button(Act::Connect, L"Retry now", ButtonStyle::Primary, bx, by) + 8;
            Button(Act::Forget, L"Forget this tablet", ButtonStyle::Secondary, bx, by, false, card_);
        } else {
            Button(Act::DisconnectTablet, L"Disconnect", ButtonStyle::Secondary, bx, by, false, card_);
        }
    } else if (streaming) {
        bx += Button(Act::Disconnect, card_ == 0 ? L"Stop all" : L"Stop", ButtonStyle::Primary, bx, by) + 8;
        if (card_ == 0) Button(Act::DisconnectTablet, L"Disconnect", ButtonStyle::Secondary, bx, by, false, 0);
    } else if (state == EngineState::Error) {
        bx += Button(Act::Connect, L"Retry now", ButtonStyle::Primary, bx, by) + 8;
        bx += Button(Act::Disconnect, card_ == 0 ? L"Stop all" : L"Stop", ButtonStyle::Secondary, bx, by) + 8;
        Button(Act::Forget, L"Forget this tablet", ButtonStyle::Secondary, bx, by, false, card_);
    } else if (showNotice && state == EngineState::Waiting) {
        bx += Button(Act::InstallDriver, L"Install driver", ButtonStyle::Primary, bx, by) + 8;
        Button(Act::Disconnect, L"Stop", ButtonStyle::Outline, bx, by);
    } else {
        Button(Act::Disconnect, L"Stop", ButtonStyle::Outline, bx, by);
    }
    y = by + kControlHeight;

    float bottom = y + kPad;
    CardBackground(mark, top, bottom);
    return bottom;
}

float MainWindow::Kpis(float x, float y, float w) {
    const Theme& t = T();
    RowRule(x, y, w);
    float colW = w / 3;
    bool loading = status_.state != EngineState::Streaming;
    int fps = status_.fps.empty() ? 0 : status_.fps.back();
    // the lead figure averages the last 5 seconds, like the bitrate
    if (status_.fps.size() >= 2) {
        size_t n = std::min<size_t>(5, status_.fps.size());
        int sum = 0;
        for (size_t i = status_.fps.size() - n; i < status_.fps.size(); ++i) sum += status_.fps[i];
        fps = int(std::lround(double(sum) / double(n)));
    }
    std::wstring decode = L"–";
    if (status_.decodeMs >= 0)
        decode = status_.decodeMs < 10 ? Fixed1(status_.decodeMs) : std::to_wstring(std::lround(status_.decodeMs));
    struct Cell {
        const wchar_t* label;
        std::wstring value;
        const wchar_t* unit;
        Font font;
    } cells[] = {
        {L"Frame rate", std::to_wstring(fps), L"fps", Font::KpiLead},
        {L"Bitrate", Fixed1(status_.mbps), L"Mbit/s", Font::Kpi},
        {L"Decode on tablet", decode, status_.decodeMs >= 0 ? L"ms" : L"", Font::Kpi},
    };
    float h = 0;
    for (int i = 0; i < 3; ++i) {
        float cx = x + colW * float(i) + (i ? 12.0f : 0.0f);
        if (i) canvas_->Line(x + colW * float(i) + 0.5f, y, x + colW * float(i) + 0.5f, y + 70, t.rule);
        float cy = y + 12;
        cy += canvas_->Text(cells[i].label, Font::Small, cx, cy, colW - 12, t.muted, false);
        if (loading) {
            canvas_->FillRound(Rect(cx, cy + 6, 64, 24), 4, t.raised);
            h = std::max(h, cy + 36 - y);
            continue;
        }
        TextBox v = canvas_->Measure(cells[i].value, cells[i].font, colW, false, true);
        canvas_->Text(v, cx, cy, t.text);
        if (*cells[i].unit) {
            TextBox u = canvas_->Measure(cells[i].unit, Font::Body, colW, false);
            canvas_->Text(u, cx + v.width + 3, cy + v.baseline - u.baseline, t.muted);
        }
        h = std::max(h, cy + v.height - y);
    }
    return y + h;
}

float MainWindow::Chart(float x, float y, float w) {
    const Theme& t = T();
    const auto& v = status_.fps;
    const float labelRow = 16, plotH = compact_ ? 44.0f : 92.0f, lane = 24;
    float px = x + lane, pw = w - lane;
    float py = y + labelRow;
    int cap = std::max<int>(60, int(status_.refresh));
    int top = ((std::max(cap, v.empty() ? 0 : *std::max_element(v.begin(), v.end())) + 19) / 20) * 20;
    for (int tick = 0; tick <= top; tick += top / 3) {
        float ty = py + plotH - plotH * float(tick) / float(top);
        canvas_->Line(px, ty + 0.5f, px + pw, ty + 0.5f, t.grid);
        TextBox l = canvas_->Measure(std::to_wstring(tick), Font::Small, lane, false, true);
        canvas_->Text(l, x + lane - 6 - l.width, ty - l.height / 2, t.muted);
    }
    auto slotX = [&](size_t slot) { return px + pw * float(slot) / 59.0f; };
    size_t offset = 60 - std::min<size_t>(60, v.size());
    // a still screen: the longest run of 3+ seconds with almost no frames
    size_t bestStart = 0, bestLen = 0;
    for (size_t i = 0; i < v.size();) {
        size_t j = i;
        while (j < v.size() && v[j] <= 5) ++j;
        if (j - i > bestLen) {
            bestLen = j - i;
            bestStart = i;
        }
        i = j + 1;
    }
    if (bestLen >= 3) {
        float x0 = slotX(offset + bestStart), x1 = slotX(offset + bestStart + bestLen - 1);
        D2D1_COLOR_F span = t.text;
        span.a = t.dark ? 0.12f : 0.06f;
        canvas_->FillRect({x0, py, x1, py + plotH}, span);
        TextBox l = canvas_->Measure(L"Screen still", Font::Small, 200, false);
        float lx = std::clamp((x0 + x1) / 2 - l.width / 2, px, px + pw - l.width);
        canvas_->Text(l, lx, y, t.muted);
    }
    if (v.size() >= 2) {
        std::vector<D2D1_POINT_2F> pts;
        for (size_t i = 0; i < v.size(); ++i)
            pts.push_back({slotX(offset + i), py + plotH - plotH * float(v[i]) / float(top)});
        canvas_->Poly(std::move(pts), t.chart, 2);
    }
    float ly = py + plotH + 6;
    const size_t at[] = {0, 15, 30, 45, 59};
    for (size_t slot : at) {
        std::wstring text = slot == 59 ? L"now" : L"−" + std::to_wstring(59 - slot) + L" s";
        TextBox l = canvas_->Measure(text, Font::Small, 80, false, true);
        float lx = std::clamp(slotX(slot) - l.width / 2, px, px + pw - l.width);
        canvas_->Text(l, lx, ly, t.muted);
    }
    return ly + 16;
}

float MainWindow::Desk(float x, float y, float w) {
    const Theme& t = T();
    const EngineState state = status_.state;
    float mwPx = float(status_.mainWidth ? status_.mainWidth : 2560);
    float mhPx = float(status_.mainHeight ? status_.mainHeight : 1440);
    uint32_t twPx = status_.width, thPx = status_.height;
    std::wstring tabletName = W(status_.device);
    if (!twPx && !settings_.tablets.empty()) {
        twPx = settings_.tablets.front().width;
        thPx = settings_.tablets.front().height;
    }
    if (!twPx) {
        twPx = 1280;
        thPx = 800;
    }
    const float gap = 6;
    float s = (w - gap) / (mwPx + float(twPx));
    float mw = mwPx * s, mh = mhPx * s, tw = float(twPx) * s, th = float(thPx) * s;
    float tx = x + mw + gap;

    canvas_->FillRound(Rect(x, y, mw, mh), 2, t.deskMonitor);
    canvas_->StrokeRound(Rect(x, y, mw, mh), 2, t.control);
    canvas_->Text(W(status_.mainName) + L" · " + Dims(uint32_t(mwPx), uint32_t(mhPx)), Font::Tiny, x + 10,
                  y + mh - 22, mw - 20, t.muted, false, true);

    bool labels = false;
    D2D1_COLOR_F labelInk = t.surface;
    if (state == EngineState::Waiting) {
        canvas_->StrokeRound(Rect(tx, y, tw, th), 2, t.control, 1, true);
    } else if (state == EngineState::Error) {
        canvas_->StrokeRound(Rect(tx, y, tw, th), 2, t.control);
        labels = true;
        labelInk = t.muted;
    } else {
        canvas_->FillRound(Rect(tx, y, tw, th), 2, t.deskTablet);
        labels = true;
    }
    if (labels && !tabletName.empty()) {
        canvas_->Text(tabletName, Font::Tiny, tx + 8, y + 6, tw - 12, labelInk, false);
        canvas_->Text(Dims(twPx, thPx), Font::Tiny, tx + 8, y + 20, tw - 12, labelInk, false, true);
    }
    y += mh + 10;

    std::wstring head, sub;
    bool link = false;
    if (state == EngineState::Waiting) {
        head = L"The tablet will appear here";
        sub = L"As a new monitor to the right of " + W(status_.mainName);
    } else if (state == EngineState::Connecting) {
        head = L"Adding a monitor on the right";
        sub = Dims(status_.width, status_.height) + L" at " + std::to_wstring(status_.refresh) + L" Hz";
    } else if (state == EngineState::Error) {
        head = L"The tablet’s monitor was removed";
        sub = L"Its windows moved back to " + W(status_.mainName);
    } else {
        head = L"Right of " + W(status_.mainName);
        sub = L"Move it, or duplicate a monitor on it, in Windows display settings";
        link = true;
    }
    y += canvas_->Text(head, Font::BodyMedium, x, y, w, t.text);
    if (link) {
        Link(Act::DisplaySettings, sub, Font::Small, x, y + 2);
        y += 18;
    } else {
        y += 2 + canvas_->Text(sub, Font::Small, x, y + 2, w, t.muted);
    }
    return y;
}

// ---------------------------------------------------------------- settings cards

float MainWindow::PairingCard(float y) {
    const Theme& t = T();
    const PairingStatus& p = status_.pairing;
    y = GroupTitle(L"Pairing", y);
    float x = colX_ + kPad, w = colW_ - 2 * kPad;
    size_t mark = canvas_->Mark();
    float top = y, ry = y + kPad;
    std::wstring name = W(p.name.empty() ? p.address : p.name);
    ry += canvas_->Text(name, Font::BodyMedium, x, ry, w, t.text) + 4;
    auto text = [&](const std::wstring& s, D2D1_COLOR_F c) { ry += canvas_->Text(s, Font::Body, x, ry, w, c) + 12; };
    float bx = x;
    switch (p.stage) {
    case PairStage::Connecting:
        text(L"Connecting to the tablet… ScreenFleet must be open on it.", t.muted);
        Button(Act::PairCancel, L"Cancel", ButtonStyle::Outline, bx, ry);
        break;
    case PairStage::EnterCode: {
        text(L"The tablet shows a 6-digit code. Type it here, then tap Pair on the tablet too.", t.muted);
        float bw = canvas_->Measure(L"Pair", Font::BodyMedium, 200, false).width + 28;
        float fw = std::min(200.0f, w - bw - 8);
        pairEditRect_ = Rect(x, ry, fw, kControlHeight);
        canvas_->FillRound(pairEditRect_, kControlRadius, t.surface);
        canvas_->StrokeRound(pairEditRect_, kControlRadius, focus_.act == Act::PairCode ? t.text : t.control);
        AddHit(pairEditRect_, Act::PairCode);
        Button(Act::PairSubmit, L"Pair", ButtonStyle::Primary, x + fw + 8, ry);
        ry += kControlHeight + 12;
        Button(Act::PairCancel, L"Cancel", ButtonStyle::Outline, bx, ry);
        break;
    }
    case PairStage::WaitTablet:
        text(L"Code accepted. Now tap Pair on the tablet.", t.text);
        Button(Act::PairCancel, L"Cancel", ButtonStyle::Outline, bx, ry);
        break;
    case PairStage::Done:
        text(W(p.message), t.positive);
        Button(Act::PairDismiss, L"Close", ButtonStyle::Outline, bx, ry);
        break;
    case PairStage::Failed:
        text(W(p.message), t.negative);
        Button(Act::PairDismiss, L"Close", ButtonStyle::Outline, bx, ry);
        break;
    }
    ry += kControlHeight + kPad;
    CardBackground(mark, top, ry);
    return ry;
}

float MainWindow::TabletsCard(float y) {
    const Theme& t = T();
    if (status_.pairing.active) y = PairingCard(y);
    y = GroupTitle(L"Tablets", y);
    float x = colX_ + kPad, w = colW_ - 2 * kPad;
    size_t mark = canvas_->Mark();
    float top = y, ry = y;

    // one row: status dot, name, a line about it, and on the right its size or a button
    auto row = [&](const std::wstring& nameText, const std::wstring& meta, bool dot, bool on, const std::wstring& right,
                   Act act, int option) {
        bool button = act != Act::None;
        TextBox dims = canvas_->Measure(right, Font::Small, 120, false, true);
        float rightW = button ? canvas_->Measure(right, Font::BodyMedium, 200, false).width + 28 : dims.width;
        float indent = dot ? 20.0f : 0.0f;
        float textW = w - indent - rightW - 12;
        TextBox name = canvas_->Measure(nameText, Font::BodyMedium, textW, false);
        TextBox m = canvas_->Measure(meta, Font::Small, textW);
        float h = std::max(52.0f, name.height + 2 + m.height + 20);
        float ty = ry + (h - name.height - 2 - m.height) / 2;
        if (dot) canvas_->FillEllipse(x + 4, ry + h / 2, 4, 4, on ? t.positive : t.control);
        canvas_->Text(name, x + indent, ty, t.text);
        canvas_->Text(m, x + indent, ty + name.height + 2, t.muted);
        if (button)
            Button(act, right, ButtonStyle::Outline, x + w - rightW, ry + (h - kControlHeight) / 2, false, option);
        else
            canvas_->Text(dims, x + w - dims.width, ry + (h - dims.height) / 2, t.muted);
        ry += h;
    };
    auto known = [&](const KnownTablet& k, bool firstRow) {
        if (!firstRow) RowRule(x - kPad, ry, colW_);
        const TabletStatus* live = nullptr;
        for (const TabletStatus& ts : status_.tablets)
            if (k.id.empty() ? ts.key == k.name : ts.key == k.id) live = &ts;
        bool connected = live && live->state != EngineState::Error;
        const std::string key = k.id.empty() ? k.name : k.id;
        bool held = !live && std::find(status_.held.begin(), status_.held.end(), key) != status_.held.end();
        std::wstring meta;
        if (held) {
            meta = L"Disconnected here · or tap Connect on the tablet";
        } else if (connected) {
            meta = L"Connected · " + LinkName(live->link) + L" · Android " + W(AndroidVersionName(k.sdk));
        } else if (live) {
            meta = L"Lost at " + W(live->endedAt) + L" · " + LinkName(k.link);
        } else {
            meta = L"Last seen " + LastSeen(k.lastSeen) + L" · Android " + W(AndroidVersionName(k.sdk));
        }
        const int index = int(&k - settings_.tablets.data());
        const bool unpaired = k.link != "USB" && k.pairKey.empty() && !connected;
        if (held) row(W(k.name), meta, true, false, L"Connect", Act::ConnectTablet, index);
        else if (unpaired && !k.address.empty()) row(W(k.name), meta + L" · not paired", true, false, L"Pair", Act::PairKnown, index);
        else row(W(k.name), meta, true, connected, Dims(k.width, k.height), Act::None, -1);
    };
    auto subhead = [&](const wchar_t* text) {
        ry += 14;
        ry += canvas_->Text(text, Font::SmallMedium, x, ry, w, t.muted) + 2;
    };
    auto empty = [&](const std::wstring& text) {
        ry += 6;
        ry += canvas_->Text(text, Font::Small, x, ry, w, t.muted) + 14;
    };

    // Cable: tablets that last connected over USB
    subhead(L"Cable");
    bool any = false;
    for (const KnownTablet& k : settings_.tablets) {
        if (k.link != "USB") continue;
        known(k, !any);
        any = true;
    }
    if (!any) empty(L"Plug in a tablet with USB debugging on and ScreenFleet open.");

    // Network: tablets that last connected over Wi-Fi, then new ones found on the network
    RowRule(x - kPad, ry, colW_);
    subhead(L"Network");
    any = false;
    for (const KnownTablet& k : settings_.tablets) {
        if (k.link == "USB") continue;
        known(k, !any);
        any = true;
    }
    for (size_t i = 0; i < status_.nearby.size(); ++i) {
        const NearbyTablet& n = status_.nearby[i];
        bool isKnown = std::any_of(settings_.tablets.begin(), settings_.tablets.end(),
                                   [&](const KnownTablet& k) { return !n.id.empty() && k.id == n.id; });
        if (isKnown) continue;
        if (any) RowRule(x - kPad, ry, colW_);
        std::wstring meta = W(n.address);
        if (n.sdk) meta += L" · Android " + W(AndroidVersionName(n.sdk));
        if (n.busy) meta += L" · streaming" + (n.busyWith.empty() ? std::wstring() : L" to " + W(n.busyWith));
        row(W(n.name), meta, true, false, L"Pair", Act::PairNearby, int(i));
        any = true;
    }
    if (!any)
        empty(settings_.wifi ? std::wstring(L"Tablets with ScreenFleet open on this ") + kWifi +
                                   L" network appear here (Android 5 and newer)."
                             : std::wstring(kWifi) + L" is off in Connection settings.");
    ry += 4;
    CardBackground(mark, top, ry);
    return ry;
}

float MainWindow::ConnectionCard(float y) {
    const Theme& t = T();
    y = GroupTitle(L"Connection settings", y);
    float x = colX_ + kPad, w = colW_ - 2 * kPad;
    size_t mark = canvas_->Mark();
    float top = y, ry = y;
    ry += Row(x, ry, w, kWifi, L"Android 5 and newer, on the same network as this PC.", {40, 20},
              [&](float cx, float cy) { Toggle(Act::Wifi, settings_.wifi, cx, cy); });
    RowRule(x - kPad, ry, colW_);
    ry += Row(x, ry, w, L"Cable", L"USB, Android 4.4 and newer. Needs USB debugging on the tablet.", {40, 20},
              [&](float cx, float cy) { Toggle(Act::Usb, settings_.usb, cx, cy); });
    RowRule(x - kPad, ry, colW_);
    ry += 10;
    ry += canvas_->Text(L"IP address", Font::BodyMedium, x, ry, w, t.text) + 2;
    ry += canvas_->Text(L"For a tablet that isn’t listed under Network.", Font::Small, x, ry, w, t.muted) + 8;
    float bw = canvas_->Measure(L"Connect", Font::BodyMedium, 200, false).width + 28;
    float fw = w - bw - 8;
    editRect_ = Rect(x, ry, fw, kControlHeight);
    canvas_->FillRound(editRect_, kControlRadius, t.surface);
    canvas_->StrokeRound(editRect_, kControlRadius, focus_.act == Act::Address ? t.text : t.control);
    AddHit(editRect_, Act::Address);
    Button(Act::AddressConnect, L"Connect", ButtonStyle::Outline, x + fw + 8, ry);
    ry += kControlHeight;
    if (!settings_.wifiAddresses.empty()) {
        std::wstring list = L"Tries ";
        for (size_t i = 0; i < settings_.wifiAddresses.size(); ++i)
            list += (i ? L", " : L"") + W(settings_.wifiAddresses[i]);
        ry += 6 + canvas_->Text(list, Font::Small, x, ry + 6, w, t.muted);
    }
    ry += 12;
    CardBackground(mark, top, ry);
    return ry;
}

float MainWindow::QualityCard(float y) {
    const Theme& t = T();
    y = GroupTitle(L"Quality", y);
    size_t mark = canvas_->Mark();
    float top = y, x = colX_ + kPad, w = colW_ - 2 * kPad;
    float ry = y + 10;

    std::vector<std::wstring> names;
    int selected = -1;
    for (int i = 0; i < 3; ++i) {
        names.push_back(W(kPresets[i].name));
        if (settings_.preset == kPresets[i].id) selected = i;
    }
    Segmented(Act::Preset, names, selected, x, ry);
    ry += SegmentedSize(names).height + 8;
    std::wstring out = selected >= 0 ? names[size_t(selected)] + L" sends up to " + std::to_wstring(kPresets[selected].fps) +
                                           L" fps at " + std::to_wstring(kPresets[selected].mbps) + L" Mbit/s."
                                     : L"Custom: up to " + std::to_wstring(settings_.fps) + L" fps at " +
                                           std::to_wstring(settings_.bitrateMbps) + L" Mbit/s.";
    ry += canvas_->Text(out + L" Changes apply right away.", Font::Small, x, ry, w, t.muted) + 10;
    RowRule(x - kPad, ry, colW_);
    ry += Row(x, ry, w, L"Frame rate", L"Up to what the tablet’s screen shows.", {120, kControlHeight},
              [&](float cx, float cy) { Select(Act::FpsSelect, std::to_wstring(settings_.fps) + L" fps", cx, cy, 120, false); });
    RowRule(x - kPad, ry, colW_);
    ry += 10;
    ry += canvas_->Text(L"Bitrate", Font::BodyMedium, x, ry, w, t.text) + 8;
    Slider(Act::Bitrate, float(settings_.bitrateMbps), 2, 20, x, ry, w);
    ry += 20 + 12;
    CardBackground(mark, top, ry);
    return ry;
}

float MainWindow::StartupCard(float y) {
    const Theme& t = T();
    y = GroupTitle(L"Startup and driver", y);
    size_t mark = canvas_->Mark();
    float top = y, x = colX_ + kPad, w = colW_ - 2 * kPad;
    float ry = y;
    ry += Row(x, ry, w, L"Start with Windows", L"Waits in the tray until a tablet appears.", {40, 20},
              [&](float cx, float cy) { Toggle(Act::Startup, startWithWindows_, cx, cy); });
    RowRule(x - kPad, ry, colW_);
    ry += Row(x, ry, w, L"Connect automatically", L"Starts streaming as soon as a tablet with the app open is found.",
              {40, 20}, [&](float cx, float cy) { Toggle(Act::AutoConnect, settings_.autoConnect, cx, cy); });
    RowRule(x - kPad, ry, colW_);

    bool installed = status_.driverInstalled;
    std::wstring desc = L"Not installed. The tablet becomes a monitor through it.";
    if (installed) {
        desc = L"Installed";
        if (!driverVersion_.empty()) desc += L" · version " + W(driverVersion_);
        if (!driverDate_.empty()) desc += L" · " + W(driverDate_);
    }
    std::wstring pill = installed ? L"Working" : L"Not installed";
    D2D1_SIZE_F ps = PillSize(pill);
    ry += Row(x, ry, w, L"Virtual display driver", desc, ps, [&](float cx, float cy) {
        Pill(pill, installed ? t.pillPositiveBg : t.pillWarningBg, installed ? t.pillPositiveInk : t.pillWarningInk, cx, cy);
    });
    float bx = x;
    if (installed) {
        bx += Button(Act::RepairDriver, L"Repair", ButtonStyle::Outline, bx, ry) + 8;
        Button(Act::UninstallDriver, L"Uninstall", ButtonStyle::DangerText, bx, ry);
    } else {
        Button(Act::InstallDriver, L"Install", ButtonStyle::Outline, bx, ry);
    }
    ry += kControlHeight + 14;
    CardBackground(mark, top, ry);
    return ry;
}

// ---------------------------------------------------------------- controls

void MainWindow::AddHit(D2D1_RECT_F r, Act act, int option, bool disabled) {
    hits_.push_back({r, act, option, disabled});
}

D2D1_SIZE_F MainWindow::PillSize(const std::wstring& text) const {
    return {canvas_->Measure(text, Font::SmallMedium, 300, false).width + 16, 22};
}

void MainWindow::Pill(const std::wstring& text, D2D1_COLOR_F bg, D2D1_COLOR_F ink, float x, float y) {
    TextBox b = canvas_->Measure(text, Font::SmallMedium, 300, false);
    canvas_->FillRound(Rect(x, y, b.width + 16, 22), 11, bg);
    canvas_->Text(b, x + 8, y + (22 - b.height) / 2, ink);
}

float MainWindow::Button(Act act, const std::wstring& text, ButtonStyle style, float x, float y, bool disabled,
                        int option) {
    const Theme& t = T();
    TextBox b = canvas_->Measure(text, Font::BodyMedium, 300, false);
    float pad = style == ButtonStyle::DangerText ? 8.0f : 14.0f;
    float w = b.width + 2 * pad;
    D2D1_RECT_F r = Rect(x, y, w, kControlHeight);
    bool hover = hover_ == Key{act, option} && !disabled;
    bool down = pressed_ == Key{act, option} && !disabled;
    float dy = down ? 1.0f : 0.0f;
    D2D1_COLOR_F ink = t.text;
    switch (style) {
    case ButtonStyle::Primary:
        canvas_->FillRound(r, kControlRadius, disabled ? t.raised : hover ? t.primaryHover : t.primary);
        ink = disabled ? t.muted : t.primaryInk;
        break;
    case ButtonStyle::Secondary:
        canvas_->FillRound(r, kControlRadius, disabled ? t.raised : hover ? t.secondaryHover : t.secondary);
        if (disabled) ink = t.muted;
        break;
    case ButtonStyle::Outline:
        canvas_->FillRound(r, kControlRadius, hover ? t.raised : t.surface);
        canvas_->StrokeRound(r, kControlRadius, t.control);
        break;
    case ButtonStyle::DangerText:
        if (hover) canvas_->FillRound(r, kControlRadius, t.pillNegativeBg);
        ink = t.negative;
        break;
    }
    canvas_->Text(b, x + pad, y + (kControlHeight - b.height) / 2 + dy, ink);
    AddHit(r, act, option, disabled);
    return w;
}

D2D1_SIZE_F MainWindow::SegmentedSize(const std::vector<std::wstring>& options) const {
    float w = 4;
    for (auto& o : options) w += canvas_->Measure(o, Font::BodyMedium, 300, false).width + 24 + 2;
    return {w - 2, 32};
}

void MainWindow::Segmented(Act act, const std::vector<std::wstring>& options, int selected, float x, float y,
                           int disabledOption) {
    const Theme& t = T();
    D2D1_SIZE_F size = SegmentedSize(options);
    canvas_->FillRound(Rect(x, y, size.width, size.height), kControlRadius, t.raised);
    float bx = x + 2;
    for (int i = 0; i < int(options.size()); ++i) {
        bool sel = i == selected;
        bool disabled = i == disabledOption;
        TextBox b = canvas_->Measure(options[size_t(i)], sel ? Font::BodyMedium : Font::Body, 300, false);
        float bw = canvas_->Measure(options[size_t(i)], Font::BodyMedium, 300, false).width + 24;
        D2D1_RECT_F r = Rect(bx, y + 2, bw, 28);
        if (sel) canvas_->FillRound(r, 5, t.dark ? Hex(0x3a3a3a) : t.surface);
        D2D1_COLOR_F ink = sel ? t.text : (hover_ == Key{act, i} && !disabled ? t.text : t.muted);
        if (disabled) ink.a = 0.5f;
        canvas_->Text(b, bx + (bw - b.width) / 2, y + 2 + (28 - b.height) / 2, ink);
        AddHit(r, act, i, disabled);
        bx += bw + 2;
    }
}

void MainWindow::Toggle(Act act, bool on, float x, float y, int option) {
    const Theme& t = T();
    D2D1_RECT_F r = Rect(x, y, 40, 20);
    canvas_->FillRound(r, 10, on ? t.primary : t.control);
    float kx = on ? x + 27 : x + 10;
    canvas_->FillEllipse(kx, y + 10, 7, 7, on ? t.primaryInk : Hex(0xffffff));
    AddHit(r, act, option);
}

void MainWindow::Select(Act act, const std::wstring& text, float x, float y, float w, bool disabled) {
    const Theme& t = T();
    D2D1_RECT_F r = Rect(x, y, w, kControlHeight);
    bool hover = hover_ == Key{act, -1} && !disabled;
    canvas_->FillRound(r, kControlRadius, disabled ? t.raised : hover ? t.raised : t.surface);
    canvas_->StrokeRound(r, kControlRadius, t.control);
    TextBox b = canvas_->Measure(text, Font::Body, w - 40, false, true);
    canvas_->Text(b, x + 10, y + (kControlHeight - b.height) / 2, disabled ? t.muted : t.text);
    float cx = x + w - 16, cy = y + kControlHeight / 2;
    canvas_->Poly({{cx - 4, cy - 2}, {cx, cy + 2}, {cx + 4, cy - 2}}, t.muted, 1.5f);
    AddHit(r, act, -1, disabled);
}

void MainWindow::Slider(Act act, float value, float min, float max, float x, float y, float w) {
    const Theme& t = T();
    const float outW = 84;
    float tw = w - outW - 12;
    float frac = (value - min) / (max - min);
    float cy = y + 10;
    canvas_->FillRound(Rect(x, cy - 2, tw, 4), 2, t.raised);
    canvas_->FillRound(Rect(x, cy - 2, tw * frac, 4), 2, t.primary);
    canvas_->FillEllipse(x + tw * frac, cy, 8, 8, t.primary);
    canvas_->FillEllipse(x + tw * frac, cy, 3, 3, t.primaryInk);
    TextBox b = canvas_->Measure(std::to_wstring(int(value)) + L" Mbit/s", Font::Body, outW, false, true,
                                 DWRITE_TEXT_ALIGNMENT_TRAILING);
    canvas_->Text(b, x + w - outW, y, t.text);
    AddHit(Rect(x - 8, y - 4, tw + 16, 28), act);
}

void MainWindow::Link(Act act, const std::wstring& text, Font font, float x, float y) {
    const Theme& t = T();
    TextBox b = canvas_->Measure(text, font, 380, false);
    canvas_->Text(b, x, y, t.text);
    float uy = y + b.baseline + 2.5f;
    canvas_->Line(x, uy, x + b.width, uy, hover_ == Key{act, -1} ? t.text : t.control);
    AddHit(Rect(x, y, b.width, b.height), act);
}

// ---------------------------------------------------------------- input

const MainWindow::Hit* MainWindow::HitAt(float x, float y) const {
    for (const Hit& h : hits_)
        if (Inside(h.r, x, y)) return &h;
    return nullptr;
}

void MainWindow::MoveFocus(int delta) {
    // one stop per control; a segmented control is one stop (arrows move inside it)
    std::vector<Key> stops;
    for (const Hit& h : hits_) {
        if (h.disabled) continue;
        Key k{h.act, h.option};
        if (h.option >= 0) {
            bool seen = std::any_of(stops.begin(), stops.end(), [&](const Key& s) { return s.act == h.act; });
            if (seen) continue;
            // the stop for a segmented control is its selected option, if any
            for (const Hit& o : hits_)
                if (o.act == h.act && o.option >= 0) {
                    k = {o.act, o.option};
                    if (&o == &h) break;
                }
        }
        stops.push_back(k);
    }
    if (stops.empty()) return;
    int i = -1;
    for (int j = 0; j < int(stops.size()); ++j)
        if (stops[size_t(j)].act == focus_.act) i = j;
    i = (i + delta + int(stops.size())) % int(stops.size());
    focus_ = stops[size_t(i)];
    if (focus_.act == Act::Address || focus_.act == Act::PairCode) {
        SetFocus(focus_.act == Act::Address ? edit_ : pairEdit_);
    }
    for (const Hit& h : hits_)
        if (h.act == focus_.act && h.option == focus_.option) ScrollIntoView(h.r);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::ScrollBy(float dy) {
    scrollY_ += dy;
    RECT rc;
    GetClientRect(hwnd_, &rc);
    scrollY_ = std::clamp(scrollY_, 0.0f, std::max(0.0f, contentHeight_ - ToDip(rc.bottom)));
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::ScrollIntoView(const D2D1_RECT_F& r) {
    RECT rc;
    GetClientRect(hwnd_, &rc);
    float viewH = ToDip(rc.bottom);
    if (r.top - 16 < scrollY_) scrollY_ = r.top - 16;
    else if (r.bottom + 16 > scrollY_ + viewH) scrollY_ = r.bottom + 16 - viewH;
}

void MainWindow::ApplySettings(const Settings& s, bool restart) {
    settings_ = s;
    engine_.SetSettings(s, restart);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::SetBitrateFromX(float x) {
    for (const Hit& h : hits_) {
        if (h.act != Act::Bitrate) continue;
        float left = h.r.left + 8, width = h.r.right - h.r.left - 16;
        float frac = std::clamp((x - left) / width, 0.0f, 1.0f);
        uint32_t v = uint32_t(std::lround(2 + frac * 18));
        if (v != settings_.bitrateMbps) {
            settings_.bitrateMbps = v;
            settings_.preset = "custom";
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        break;
    }
}

void MainWindow::ShowSelectMenu(Act act) {
    HMENU menu = CreatePopupMenu();
    for (UINT fps : {30u, 60u})
        AppendMenuW(menu, MF_STRING | (settings_.fps == fps ? MF_CHECKED : 0), 200 + fps,
                    (std::to_wstring(fps) + L" fps").c_str());
    D2D1_RECT_F r{};
    for (const Hit& h : hits_)
        if (h.act == act) r = h.r;
    POINT pt{ToPx(r.left), ToPx(r.bottom - scrollY_ + 4)};
    ClientToScreen(hwnd_, &pt);
    UINT cmd = UINT(TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y, hwnd_, nullptr));
    DestroyMenu(menu);
    if (!cmd) return;
    Settings s = settings_;
    s.fps = cmd - 200;
    s.preset = "custom";
    ApplySettings(s, true);
}

void MainWindow::RunDriverScript(const wchar_t* script) {
    // The script ends up running with admin rights, so it is only taken from
    // two exact places: a "driver" folder next to the exe (an installed copy),
    // or the source tree's driver folder (host-windows\build\ScreenFleet.exe).
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::filesystem::path dir = std::filesystem::path(exe).parent_path();
    std::filesystem::path found;
    for (auto candidate : {dir / L"driver" / script, dir.parent_path().parent_path() / L"driver" / script}) {
        std::error_code ec;
        if (std::filesystem::is_regular_file(candidate, ec)) {
            found = candidate;
            break;
        }
    }
    if (found.empty()) {
        MessageBoxW(hwnd_, (std::wstring(L"Couldn’t find ") + script + L" next to ScreenFleet.").c_str(),
                    L"ScreenFleet", MB_ICONWARNING);
        return;
    }
    wchar_t system[MAX_PATH];
    GetSystemDirectoryW(system, MAX_PATH);
    std::wstring powershell = std::wstring(system) + L"\\WindowsPowerShell\\v1.0\\powershell.exe";
    std::wstring args = L"-NoProfile -ExecutionPolicy Bypass -File \"" + found.wstring() + L"\"";
    ShellExecuteW(hwnd_, L"open", powershell.c_str(), args.c_str(), found.parent_path().c_str(), SW_SHOWNORMAL);
}

void MainWindow::Activate(const Key& key, float x) {
    Settings s = settings_;
    switch (key.act) {
    case Act::Disconnect:
        engine_.Disconnect();
        break;
    case Act::Connect:
        engine_.Connect();
        break;
    case Act::DisconnectTablet:
        if (key.option >= 0 && size_t(key.option) < status_.tablets.size())
            engine_.DisconnectTablet(status_.tablets[size_t(key.option)].key);
        else
            engine_.DisconnectTablet(status_.key);
        break;
    case Act::ConnectTablet:
        if (key.option >= 0 && size_t(key.option) < settings_.tablets.size()) {
            const KnownTablet& k = settings_.tablets[size_t(key.option)];
            engine_.ConnectTablet(k.id.empty() ? k.name : k.id);
        }
        break;
    case Act::PairNearby:
        if (key.option >= 0 && size_t(key.option) < status_.nearby.size())
            engine_.Pair(status_.nearby[size_t(key.option)].address, status_.nearby[size_t(key.option)].name);
        break;
    case Act::PairKnown:
        if (key.option >= 0 && size_t(key.option) < settings_.tablets.size())
            engine_.Pair(settings_.tablets[size_t(key.option)].address, settings_.tablets[size_t(key.option)].name);
        break;
    case Act::PairCard:
        if (key.option >= 0 && size_t(key.option) < status_.tablets.size())
            engine_.Pair(status_.tablets[size_t(key.option)].address, status_.tablets[size_t(key.option)].device);
        else if (!status_.address.empty())
            engine_.Pair(status_.address, status_.device);
        break;
    case Act::PairCode:
        SetFocus(pairEdit_);
        break;
    case Act::PairSubmit: {
        wchar_t buf[16];
        GetWindowTextW(pairEdit_, buf, ARRAYSIZE(buf));
        if (!buf[0]) {
            SetFocus(pairEdit_);
            break;
        }
        engine_.SubmitPairCode(Narrow(buf));
        SetWindowTextW(pairEdit_, L"");
        break;
    }
    case Act::PairCancel:
        engine_.CancelPairing();
        break;
    case Act::PairDismiss:
        engine_.DismissPairing();
        break;
    case Act::AllowTouch:
        if (key.option >= 0 && size_t(key.option) < status_.tablets.size()) {
            const TabletStatus& t = status_.tablets[size_t(key.option)];
            engine_.SetAllowTouch(t.key, !t.allowTouch);
        } else {
            engine_.SetAllowTouch(status_.key, !status_.allowTouch);
        }
        break;
    case Act::Forget:
        if (key.option >= 0 && size_t(key.option) < status_.tablets.size()) engine_.Forget(status_.tablets[size_t(key.option)].key);
        else engine_.Forget(status_.key);
        break;
    case Act::FpsSelect:
        ShowSelectMenu(key.act);
        break;
    case Act::Usb:
        s.usb = !s.usb;
        ApplySettings(s, false);
        break;
    case Act::Wifi:
        s.wifi = !s.wifi;
        ApplySettings(s, false);
        break;
    case Act::AutoConnect:
        s.autoConnect = !s.autoConnect;
        ApplySettings(s, false);
        break;
    case Act::Address:
        SetFocus(edit_);
        break;
    case Act::AddressConnect: {
        wchar_t buf[128];
        GetWindowTextW(edit_, buf, ARRAYSIZE(buf));
        std::string address = Narrow(buf);
        address.erase(std::remove(address.begin(), address.end(), ' '), address.end());
        if (address.empty()) {
            SetFocus(edit_);
            break;
        }
        auto& list = s.wifiAddresses;
        list.erase(std::remove(list.begin(), list.end(), address), list.end());
        list.insert(list.begin(), address);
        s.wifi = true;
        ApplySettings(s, false);
        SetWindowTextW(edit_, L"");
        engine_.Connect();
        break;
    }
    case Act::Preset:
        if (key.option >= 0 && key.option < 3) {
            const Preset& p = kPresets[key.option];
            s.preset = p.id;
            s.fps = p.fps;
            s.bitrateMbps = p.mbps;
            ApplySettings(s, true);
        }
        break;
    case Act::Bitrate:
        SetBitrateFromX(x);
        break;
    case Act::Startup:
        startWithWindows_ = !startWithWindows_;
        SetStartWithWindows(startWithWindows_);
        InvalidateRect(hwnd_, nullptr, FALSE);
        break;
    case Act::InstallDriver:
    case Act::RepairDriver:
        RunDriverScript(L"install.ps1");
        break;
    case Act::UninstallDriver:
        if (MessageBoxW(hwnd_,
                        L"Uninstall the ScreenFleet display driver?\n\nTablets can't be used as monitors until you "
                        L"install it again.",
                        L"ScreenFleet", MB_ICONQUESTION | MB_YESNO | MB_DEFBUTTON2) == IDYES) {
            RunDriverScript(L"uninstall.ps1");
        }
        break;
    case Act::DisplaySettings:
        ShellExecuteW(hwnd_, L"open", L"ms-settings:display", nullptr, nullptr, SW_SHOWNORMAL);
        break;
    case Act::None:
        break;
    }
}

}  // namespace sst::app
