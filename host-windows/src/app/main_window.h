#pragma once

#include <windows.h>
#include <d2d1.h>
#include <dwrite_3.h>
#include <shellapi.h>
#include <wrl/client.h>

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "../engine.h"
#include "canvas.h"
#include "theme.h"

namespace sst::app {

// The one window of the tray app, drawn with Direct2D to match the approved
// mockup, plus the tray icon and its menu.
class MainWindow {
public:
    static constexpr UINT kMsgStatus = WM_APP + 1;  // the engine's status changed
    static constexpr UINT kMsgTray = WM_APP + 2;    // tray icon events
    static constexpr UINT kMsgShow = WM_APP + 3;    // a second instance asked us to show

    explicit MainWindow(StreamEngine& engine) : engine_(engine) {}
    ~MainWindow();

    bool Create(HINSTANCE instance, bool visible);
    // Thread-safe: called by the engine on its own threads.
    void PostStatusChanged();

private:
    enum class Act {
        None, Disconnect, Connect, Forget, InstallDriver, RepairDriver, UninstallDriver, DisplaySettings, Usb, Wifi,
        Address, AddressConnect, Preset, FpsSelect, Bitrate, Startup, AutoConnect, DisconnectTablet, ConnectTablet,
        PairNearby, PairKnown, PairCard, PairCode, PairSubmit, PairCancel, PairDismiss,
        AllowTouch,
    };
    struct Hit {
        D2D1_RECT_F r;
        Act act;
        int option;
        bool disabled;
    };
    struct Key {
        Act act = Act::None;
        int option = -1;
        bool operator==(const Key& o) const { return act == o.act && option == o.option; }
    };
    enum class ButtonStyle { Primary, Secondary, Outline, DangerText };

    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK EditProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp);

    // drawing
    bool EnsureTarget();
    void Paint();
    void Layout();
    float ViewWidth() const;
    float StatusCard(float y);
    float Kpis(float x, float y, float w);
    float Chart(float x, float y, float w);
    float Desk(float x, float y, float w);
    float TabletsCard(float y);
    float PairingCard(float y);
    float ConnectionCard(float y);
    float QualityCard(float y);
    float StartupCard(float y);
    float GroupTitle(const wchar_t* text, float y);
    float Row(float x, float y, float w, const std::wstring& label, const std::wstring& desc, D2D1_SIZE_F control,
              const std::function<void(float, float)>& draw);
    void CardBackground(size_t mark, float top, float bottom);
    void RowRule(float x, float y, float w);

    // controls
    D2D1_SIZE_F PillSize(const std::wstring& text) const;
    void Pill(const std::wstring& text, D2D1_COLOR_F bg, D2D1_COLOR_F ink, float x, float y);
    float Button(Act act, const std::wstring& text, ButtonStyle style, float x, float y, bool disabled = false,
                 int option = -1);
    D2D1_SIZE_F SegmentedSize(const std::vector<std::wstring>& options) const;
    void Segmented(Act act, const std::vector<std::wstring>& options, int selected, float x, float y,
                   int disabledOption = -1);
    void Toggle(Act act, bool on, float x, float y, int option = -1);
    void Select(Act act, const std::wstring& text, float x, float y, float w, bool disabled);
    void Slider(Act act, float value, float min, float max, float x, float y, float w);
    void Link(Act act, const std::wstring& text, Font font, float x, float y);
    void AddHit(D2D1_RECT_F r, Act act, int option = -1, bool disabled = false);

    // input
    const Hit* HitAt(float x, float y) const;
    void Activate(const Key& key, float x = 0);
    void MoveFocus(int delta);
    void ScrollBy(float dy);
    void ScrollIntoView(const D2D1_RECT_F& r);
    void ApplySettings(const Settings& s, bool restart);
    void SetBitrateFromX(float x);
    void ShowSelectMenu(Act act);
    void ShowTrayMenu();
    void RunDriverScript(const wchar_t* script);

    // window & tray
    void Show();
    void ApplyTheme();
    void UpdateTray(bool add);
    void PositionEdit();
    void UpdateEditFont();
    float ToDip(int px) const { return px * 96.0f / float(dpi_); }
    int ToPx(float dip) const { return int(dip * float(dpi_) / 96.0f + 0.5f); }
    const Theme& T() const { return GetTheme(dark_); }

    StreamEngine& engine_;
    HWND hwnd_ = nullptr;
    HWND edit_ = nullptr;       // IP address
    HWND pairEdit_ = nullptr;   // pairing code
    HFONT editFont_ = nullptr;
    HBRUSH editBrush_ = nullptr;
    HICON windowIcon_ = nullptr;
    HICON smallIcon_ = nullptr;
    HICON trayIcon_ = nullptr;
    UINT taskbarCreated_ = 0;
    UINT dpi_ = 96;
    bool dark_ = false;
    std::atomic<bool> statusPending_{false};

    ComPtr<ID2D1Factory> d2d_;
    ComPtr<ID2D1HwndRenderTarget> rt_;
    ComPtr<ID2D1SolidColorBrush> brush_;
    ComPtr<ID2D1StrokeStyle> dashed_;
    ComPtr<IDWriteFactory5> dwrite_;
    Fonts fonts_;
    std::unique_ptr<Canvas> canvas_;
    std::vector<Hit> hits_;
    D2D1_RECT_F editRect_{};
    D2D1_RECT_F pairEditRect_{};

    EngineStatus status_;          // while a tablet's card is drawn: that tablet's view (EngineStatus::For)
    int card_ = -1;                // the tablet whose card is being drawn; -1 = the only card
    int compact_ = 0;              // 1: several status cards side by side, without the desk drawing
    Settings settings_;
    bool startWithWindows_ = false;
    bool driverInfoKnown_ = false;
    std::string driverVersion_, driverDate_;
    std::string pcName_, gpuName_;

    Key hover_, pressed_, focus_;
    bool focusVisible_ = false;
    bool dragging_ = false;
    float colX_ = 16, colW_ = 408;  // the column the card being drawn goes in
    float scrollY_ = 0;
    float contentHeight_ = 0;
};

}  // namespace sst::app
