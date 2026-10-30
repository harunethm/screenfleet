#pragma once

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>

#include "sst/protocol.h"

namespace sst {

// What the host does with touches from tablets.
enum class InputMode {
    Inject,  // move the PC's pointer (normal)
    Log,     // only log what arrives (tests: nothing is clicked)
};
void SetInputMode(InputMode mode);

// Touches and pen from one tablet, played on that tablet's own monitor.
// Fingers go to a synthetic touch device, the pen to a synthetic pen device
// (Windows 10 1809 and newer), both made on first use. The tablet sends a
// snapshot of its pointers each time; down / move / up come from comparing
// it with the previous one. Thread-safe.
//
// Windows doesn't let input reach windows of elevated (admin) programs
// unless this one is elevated too (UIPI).
class TabletInput {
public:
    // gdiName: the tablet's monitor (\\.\DISPLAY5), looked up again when
    // a gesture starts, so moving monitors around in Windows works.
    TabletInput(std::string gdiName, std::string label, bool allowed);
    ~TabletInput();

    void OnEvent(const proto::InputEvent& event);
    // Off: what is pressed is lifted (canceled) and further events are ignored.
    void SetAllowed(bool allowed);
    // Every ~100 ms: keeps a held finger or pen alive (Windows cancels
    // contacts it hears nothing about for a while).
    void Tick();

private:
    struct Pointer {
        bool present = false;
        bool touching = false;
        uint8_t state = 0;
        POINT pt{};
        uint16_t pressure = 0;
    };

    bool RefreshRect();
    POINT Map(uint16_t x, uint16_t y) const;
    void Apply(const proto::InputEvent& event);
    void InjectTouch(const Pointer (&next)[proto::input::kMaxId + 1], bool canceled);
    void InjectPen(const Pointer& next, bool canceled);
    void ReleaseAll();

    const std::string gdiName_;
    const std::string label_;
    std::mutex mutex_;
    bool allowed_;
    RECT rect_{};
    bool haveRect_ = false;
    std::chrono::steady_clock::time_point rectAt_{};
    void* touchDevice_ = nullptr;  // HSYNTHETICPOINTERDEVICE
    void* penDevice_ = nullptr;
    bool failed_ = false;          // the devices couldn't be made; don't keep trying
    Pointer touch_[proto::input::kMaxId + 1];
    Pointer pen_;
    std::chrono::steady_clock::time_point lastInject_{};
};

}  // namespace sst
