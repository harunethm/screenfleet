#pragma once

#include <winsock2.h>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "capture.h"
#include "channel.h"
#include "outputs.h"
#include "session.h"
#include "settings.h"
#include "sst/protocol.h"
#include "touch_input.h"
#include "virtual_display.h"

namespace sst {

GpuContext CreateDevice(const DisplayOutput& out);

// A display being captured, with the GPU device that captures it.
struct CaptureTarget {
    GpuContext gpu;
    DesktopDuplicator capture;
};
std::unique_ptr<CaptureTarget> OpenCapture(const DisplayOutput& out);

// Real monitors only (our own virtual monitors are left out).
std::vector<DisplayOutput> PhysicalOutputs();

enum class EngineState { Waiting, Connecting, Streaming, Error };

// One tablet the engine is connected to (or lost).
struct TabletStatus {
    std::string key;               // the tablet's id (or its name, for older tablets)
    std::string address;           // Wi-Fi address, if it came that way
    bool needsPair = false;        // Wi-Fi: it isn't paired with this PC yet
    EngineState state = EngineState::Connecting;
    std::string device;
    std::string link;              // "USB" or "Wi-Fi"
    uint32_t sdk = 0, width = 0, height = 0, refresh = 0;
    std::string startedAt, endedAt;  // "13:41"
    std::vector<std::string> steps;  // connecting progress; the last one is in progress
    std::string error;
    bool touch = false;            // the tablet sends touches (its "Touch controls the PC" is on)
    bool pen = false;              // ... and has a pen
    bool allowTouch = true;        // this PC acts on them (per tablet, in settings)

    std::vector<int> fps;          // frames sent per second, oldest first, up to 60
    double mbps = 0;               // average of the last 5 seconds
    double decodeMs = -1;          // reported by the tablet; -1 = unknown
};

// A tablet that answered on the local network (Wi-Fi discovery).
struct NearbyTablet {
    std::string address;           // "192.168.1.42" (or "host:port" off the default port)
    std::string name;
    std::string id;
    uint32_t width = 0, height = 0, sdk = 0;
    bool busy = false;             // streaming to some PC
    std::string busyWith;
    bool added = false;            // its address is in the Wi-Fi list
};

// Pairing a tablet over Wi-Fi (one at a time).
enum class PairStage { Connecting, EnterCode, WaitTablet, Done, Failed };
struct PairingStatus {
    bool active = false;
    std::string address;
    std::string name;
    PairStage stage = PairStage::Connecting;
    std::string message;           // for Done and Failed
};

struct EngineStatus {
    // The fields from `state` to `decodeMs` describe one tablet: the first
    // one streaming (else the first connected or lost), so a window that
    // shows a single tablet keeps working. `tablets` has all of them.
    EngineState state = EngineState::Waiting;
    bool paused = false;           // waiting for the user to press Start
    bool driverInstalled = false;
    bool adbFound = false;
    std::string mainName;          // the main monitor, for the desk drawing
    uint32_t mainWidth = 0, mainHeight = 0;

    std::string key;
    std::string address;           // Wi-Fi address of that tablet, if any
    bool needsPair = false;        // it answered "not paired"
    std::string device;            // the current (or last) tablet
    std::string link;              // "USB" or "Wi-Fi"
    uint32_t sdk = 0, width = 0, height = 0, refresh = 0;
    std::string startedAt, endedAt;  // "13:41"
    std::vector<std::string> steps;  // connecting progress; the last one is in progress
    std::string error;
    bool touch = false, pen = false, allowTouch = true;  // as in TabletStatus

    std::vector<int> fps;          // frames sent per second, oldest first, up to 60
    double mbps = 0;               // average of the last 5 seconds
    double decodeMs = -1;          // reported by the tablet; -1 = unknown

    std::vector<TabletStatus> tablets;
    std::vector<std::string> held;   // keys of tablets disconnected here, until reconnected
    std::vector<NearbyTablet> nearby;
    PairingStatus pairing;

    // This status with the single-tablet fields taken from tablets[i].
    EngineStatus For(size_t i) const;
};

// Finds tablets and runs one streaming session per tablet (up to the four
// monitors the driver offers), each on its own thread. All public methods
// are thread-safe; `notify` is called from engine threads whenever the
// status changes.
class StreamEngine {
public:
    using Notify = std::function<void()>;

    ~StreamEngine();

    // persist: save settings changes (and remembered tablets) to disk.
    void Start(const Settings& settings, bool persist, Notify notify, std::wstring adbPath = {});
    void Stop();

    EngineStatus Status() const;
    Settings GetSettings() const;
    // restart: end the current sessions so the change applies right away.
    void SetSettings(const Settings& settings, bool restart);

    void Connect();     // Start: look for tablets again (also reconnects held ones)
    void Disconnect();  // Stop: end all sessions and wait until Connect()
    // One tablet: end its session and leave it alone until it is reconnected
    // here or from the tablet itself (Connect on the tablet, after this).
    void DisconnectTablet(const std::string& key);
    void ConnectTablet(const std::string& key);
    void Forget(const std::string& key);
    // Whether the tablet's touches and pen move this PC's pointer (applies at once).
    void SetAllowTouch(const std::string& key, bool allow);
    bool AllowTouch(const std::string& key) const;

    // Pairing: the tablet shows a code, the user types it here
    // (SubmitPairCode) and taps Pair on the tablet.
    void Pair(const std::string& address, const std::string& name);
    void SubmitPairCode(const std::string& code);
    void CancelPairing();
    void DismissPairing();  // hide a finished or failed pairing

private:
    // One place a tablet can be reached: a USB device (by adb serial) or a
    // Wi-Fi address. Each has its own thread that connects and streams.
    struct Slot {
        std::string serial;        // USB
        uint16_t localPort = 0;    // USB: the adb forward's port on this PC
        std::string address;       // Wi-Fi
        std::thread thread;
        std::atomic<bool> quit{false};
        std::atomic<bool> stopSession{false};
        bool restart = false;      // the session ends for new settings
        int missing = 0;           // USB: polls in a row the device wasn't listed
        int failures = 0;          // tries in a row that found no app
        std::string lastKey;       // the tablet found here last
        bool active = false;       // `status` is shown (connecting, streaming or lost)
        TabletStatus status;
        TabletInput* input = nullptr;  // while streaming
    };

    void Loop();
    void SlotLoop(Slot& slot);
    void DiscoveryLoop();
    enum class HandshakeResult { Ok, Failed, NotPaired, OldApp };
    // secure: an encrypted session (Wi-Fi). On NotPaired/OldApp, info.model
    // and info.deviceId say which tablet it was.
    HandshakeResult Handshake(Channel& ch, bool secure, proto::DeviceInfo& info, SessionConfig& config);
    bool SendStart(Channel& ch, const SessionConfig& config);
    void RunDeviceSession(Slot& slot, Channel& ch, const proto::DeviceInfo& info, const SessionConfig& config);
    void RunPairing(Slot& slot, Channel& ch);
    void PairFailed(const std::string& message);
    void RememberTablet(const proto::DeviceInfo& info, const std::string& key, const std::string& link,
                        const std::string& address);
    void UpdateSlots(const std::vector<std::string>& usbSerials);
    void RefreshDisplays();
    void Changed();
    void SleepFor(int ms);
    bool SlotSleep(Slot& slot, int ms);
    static std::string KeyOf(const proto::DeviceInfo& info);

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    Settings settings_;
    EngineStatus status_;          // global fields only; tablets come from the slots
    bool persist_ = false;
    Notify notify_;
    std::wstring adb_;
    VirtualDisplayDriver driver_;
    std::mutex monitorMutex_;      // one monitor is added (and found) at a time

    std::list<std::unique_ptr<Slot>> slots_;
    std::map<std::string, int64_t> held_;  // key -> when it was disconnected here (steady seconds)
    PairingStatus pairing_;
    std::string pairCode_;         // typed by the user, while pairing_ is at EnterCode
    bool pairCancel_ = false;
    std::vector<std::string> streamingKeys_;  // tablets with a session, so a second link to one is skipped
    uint16_t nextLocalPort_ = 27200;

    std::thread thread_;
    std::thread discovery_;
    std::atomic<bool> quit_{false};
    bool wake_ = false;
    bool userStopped_ = false;
};

}  // namespace sst
