#include "engine.h"

#include <d3d11_4.h>
#include <iphlpapi.h>

#include <algorithm>
#include <chrono>
#include <ctime>

#include "adb.h"
#include "crypto.h"
#include "log.h"
#include "net.h"
#include "sst_display_control.h"

namespace sst {

namespace {

constexpr char kVirtualMonitorName[] = "ScreenFleet";

int64_t SteadySeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string ClockNow() {
    SYSTEMTIME t;
    GetLocalTime(&t);
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%02u:%02u", t.wHour, t.wMinute);
    return buf;
}

// Stable per tablet, so Windows remembers where you placed its monitor.
uint32_t DeviceSerial(const proto::DeviceInfo& info) {
    uint32_t h = 2166136261u;  // FNV-1a
    auto mix = [&](const void* p, size_t n) {
        for (size_t i = 0; i < n; ++i) h = (h ^ static_cast<const uint8_t*>(p)[i]) * 16777619u;
    };
    mix(info.model.data(), info.model.size());
    mix(&info.width, sizeof(info.width));
    mix(&info.height, sizeof(info.height));
    // two tablets of the same model need two monitors (tablets without an id keep their old serial)
    mix(info.deviceId.data(), info.deviceId.size());
    return h;
}

bool IsLoopback(const std::string& host) {
    return host == "localhost" || host == "::1" || host.rfind("127.", 0) == 0;
}

// Transcript pieces (PROTOCOL.md): labels as raw bytes, strings as on the wire.
crypto::Bytes Label(const char* s) { return crypto::Bytes(s, s + strlen(s)); }
crypto::Bytes Str(const std::string& s) {
    crypto::Bytes b{uint8_t(s.size() >> 8), uint8_t(s.size())};
    b.insert(b.end(), s.begin(), s.end());
    return b;
}
template <typename... Parts>
crypto::Bytes Cat(const Parts&... parts) {
    crypto::Bytes out;
    (out.insert(out.end(), std::begin(parts), std::end(parts)), ...);
    return out;
}

// One short line of printable text, at most 64 bytes, never half a UTF-8 character.
std::string CleanName(const std::string& in) {
    std::string name;
    bool cut = false;
    for (unsigned char c : in) {
        if (c < 0x20 || c == 0x7f) continue;
        if (name.size() == 64) {
            cut = true;
            break;
        }
        name.push_back(char(c));
    }
    if (cut) {
        while (!name.empty() && (static_cast<unsigned char>(name.back()) & 0xC0) == 0x80) name.pop_back();
        if (!name.empty() && static_cast<unsigned char>(name.back()) >= 0xC0) name.pop_back();
    }
    return name;
}

void ParseAddress(const std::string& address, std::string& host, uint16_t& port) {
    host = address;
    port = proto::kDefaultPort;
    auto colon = address.rfind(':');
    if (colon != std::string::npos && address.find(':') == colon) {
        host = address.substr(0, colon);
        try {
            port = uint16_t(std::stoi(address.substr(colon + 1)));
        } catch (...) {
        }
    }
}

// Waits until Windows has attached the newly added virtual monitor to the
// desktop: one of ours that wasn't there before (others may be streaming).
bool WaitForVirtualOutput(UINT width, UINT height, const std::vector<std::string>& before, DisplayOutput& found,
                          const std::atomic<bool>& stop) {
    for (int i = 0; i < 50 && !stop; ++i) {
        for (auto& o : EnumerateOutputs()) {
            bool isNew = std::find(before.begin(), before.end(), o.gdiName) == before.end();
            if (o.monitorName == kVirtualMonitorName && isNew && UINT(o.Width()) == width && UINT(o.Height()) == height) {
                found = o;
                return true;
            }
        }
        Sleep(100);
    }
    return false;
}

// Where to send the discovery query: each network's broadcast address, plus
// 255.255.255.255 (which Windows sends out of only one network).
std::vector<sockaddr_in> BroadcastTargets() {
    std::vector<sockaddr_in> out;
    auto add = [&](uint32_t ip) {
        for (auto& t : out)
            if (t.sin_addr.s_addr == htonl(ip)) return;
        sockaddr_in t{};
        t.sin_family = AF_INET;
        t.sin_port = htons(proto::kDefaultPort);
        t.sin_addr.s_addr = htonl(ip);
        out.push_back(t);
    };
    ULONG size = 16 * 1024;
    std::vector<uint8_t> buf(size);
    const ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    auto* list = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
    ULONG r = GetAdaptersAddresses(AF_INET, flags, nullptr, list, &size);
    if (r == ERROR_BUFFER_OVERFLOW) {
        buf.resize(size);
        list = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
        r = GetAdaptersAddresses(AF_INET, flags, nullptr, list, &size);
    }
    if (r == NO_ERROR) {
        for (auto* a = list; a; a = a->Next) {
            if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
            for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
                if (u->Address.lpSockaddr->sa_family != AF_INET) continue;
                ULONG len = u->OnLinkPrefixLength;
                if (len == 0 || len > 30) continue;
                uint32_t ip = ntohl(reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr)->sin_addr.s_addr);
                add(ip | ~(0xFFFFFFFFu << (32 - len)));
            }
        }
    }
    add(INADDR_BROADCAST);
    return out;
}

}  // namespace

GpuContext CreateDevice(const DisplayOutput& out) {
    GpuContext gpu;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    Check(D3D11CreateDevice(out.adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                            D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT, levels,
                            ARRAYSIZE(levels), D3D11_SDK_VERSION, &gpu.device, nullptr, &gpu.context),
          "D3D11CreateDevice");
    // The encoder uses the device from its own thread.
    ComPtr<ID3D11Multithread> mt;
    if (SUCCEEDED(gpu.context.As(&mt))) mt->SetMultithreadProtected(TRUE);
    gpu.adapterLuid = out.adapterLuid;
    return gpu;
}

std::unique_ptr<CaptureTarget> OpenCapture(const DisplayOutput& out) {
    Log("capturing %s %dx%d \"%s\" on %s", out.gdiName.c_str(), out.Width(), out.Height(), out.monitorName.c_str(),
        out.adapterName.c_str());
    auto target = std::make_unique<CaptureTarget>();
    target->gpu = CreateDevice(out);
    target->capture.Init(target->gpu.device.Get(), out.output.Get());
    // The first acquire returns the full current screen.
    for (int i = 0; i < 20 && target->capture.Acquire(100) != DesktopDuplicator::Result::NewFrame; ++i) {
    }
    if (!target->capture.Texture()) {
        // Nothing has been drawn yet (the monitor is asleep, or Windows is
        // showing the lock screen). Start black; real frames follow.
        Log("no picture yet; starting with a black frame");
        target->capture.EnsureTexture(UINT(out.Width()), UINT(out.Height()), true);
    }
    return target;
}

std::vector<DisplayOutput> PhysicalOutputs() {
    std::vector<DisplayOutput> out;
    for (auto& o : EnumerateOutputs())
        if (o.monitorName != kVirtualMonitorName) out.push_back(o);
    return out;
}

EngineStatus EngineStatus::For(size_t i) const {
    EngineStatus s = *this;
    if (i >= tablets.size()) return s;
    const TabletStatus& t = tablets[i];
    s.state = t.state;
    s.key = t.key;
    s.address = t.address;
    s.needsPair = t.needsPair;
    s.device = t.device;
    s.link = t.link;
    s.sdk = t.sdk;
    s.width = t.width;
    s.height = t.height;
    s.refresh = t.refresh;
    s.startedAt = t.startedAt;
    s.endedAt = t.endedAt;
    s.steps = t.steps;
    s.error = t.error;
    s.touch = t.touch;
    s.pen = t.pen;
    s.allowTouch = t.allowTouch;
    s.fps = t.fps;
    s.mbps = t.mbps;
    s.decodeMs = t.decodeMs;
    return s;
}

StreamEngine::~StreamEngine() { Stop(); }

void StreamEngine::Start(const Settings& settings, bool persist, Notify notify, std::wstring adbPath) {
    settings_ = settings;
    persist_ = persist;
    if (settings_.hostId.empty()) {
        auto id = crypto::Random(16);
        settings_.hostId = crypto::Hex(id.data(), id.size());
        if (persist_) SaveSettings(settings_);
    }
    notify_ = std::move(notify);
    adb_ = FindAdb(adbPath);
    status_.adbFound = !adb_.empty();
    userStopped_ = !settings.autoConnect;
    status_.paused = userStopped_;
    status_.driverInstalled = driver_.Open();
    RefreshDisplays();
    thread_ = std::thread([this] { Loop(); });
    discovery_ = std::thread([this] { DiscoveryLoop(); });
}

void StreamEngine::Stop() {
    quit_ = true;
    {
        std::lock_guard lock(mutex_);
        wake_ = true;
        for (auto& slot : slots_) {
            slot->quit = true;
            slot->stopSession = true;
        }
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    if (discovery_.joinable()) discovery_.join();
}

EngineStatus StreamEngine::Status() const {
    std::lock_guard lock(mutex_);
    EngineStatus st = status_;
    for (const auto& slot : slots_)
        if (slot->active) st.tablets.push_back(slot->status);
    for (const auto& [key, at] : held_) st.held.push_back(key);
    st.pairing = pairing_;
    // the single-tablet fields: the first streaming, else connecting, else lost
    for (EngineState want : {EngineState::Streaming, EngineState::Connecting, EngineState::Error}) {
        for (size_t i = 0; i < st.tablets.size(); ++i) {
            if (st.tablets[i].state == want) {
                return st.For(i);
            }
        }
    }
    st.state = EngineState::Waiting;
    return st;
}

Settings StreamEngine::GetSettings() const {
    std::lock_guard lock(mutex_);
    return settings_;
}

void StreamEngine::SetSettings(const Settings& settings, bool restart) {
    {
        std::lock_guard lock(mutex_);
        settings_ = settings;
        if (persist_) SaveSettings(settings_);
        if (restart) {
            for (auto& slot : slots_) {
                if (slot->active && slot->status.state != EngineState::Error) {
                    slot->restart = true;
                    slot->stopSession = true;
                }
            }
        }
        wake_ = true;
    }
    RefreshDisplays();
    cv_.notify_all();
    Changed();
}

void StreamEngine::Connect() {
    {
        std::lock_guard lock(mutex_);
        userStopped_ = false;
        status_.paused = false;
        held_.clear();
        wake_ = true;
    }
    cv_.notify_all();
    Changed();
}

void StreamEngine::Disconnect() {
    {
        std::lock_guard lock(mutex_);
        userStopped_ = true;
        status_.paused = true;
        for (auto& slot : slots_) {
            slot->stopSession = true;
            if (slot->status.state == EngineState::Error) slot->active = false;
        }
        wake_ = true;
    }
    cv_.notify_all();
    Changed();
}

void StreamEngine::DisconnectTablet(const std::string& key) {
    {
        std::lock_guard lock(mutex_);
        held_[key] = SteadySeconds();
        for (auto& slot : slots_) {
            if (slot->status.key != key) continue;
            slot->stopSession = true;
            if (slot->status.state == EngineState::Error) slot->active = false;
        }
    }
    cv_.notify_all();
    Changed();
}

void StreamEngine::ConnectTablet(const std::string& key) {
    {
        std::lock_guard lock(mutex_);
        held_.erase(key);
    }
    cv_.notify_all();
    Changed();
}

void StreamEngine::Pair(const std::string& address, const std::string& name) {
    {
        std::lock_guard lock(mutex_);
        if (pairing_.active && pairing_.stage != PairStage::Done && pairing_.stage != PairStage::Failed) return;
        pairing_ = {};
        pairing_.active = true;
        pairing_.address = address;
        pairing_.name = name;
        pairCode_.clear();
        pairCancel_ = false;
        auto& list = settings_.wifiAddresses;
        if (std::find(list.begin(), list.end(), address) == list.end()) list.insert(list.begin(), address);
        settings_.wifi = true;
        userStopped_ = false;
        status_.paused = false;
        if (persist_) SaveSettings(settings_);
        wake_ = true;
    }
    cv_.notify_all();
    Changed();
}

void StreamEngine::SubmitPairCode(const std::string& code) {
    {
        std::lock_guard lock(mutex_);
        if (!pairing_.active || pairing_.stage != PairStage::EnterCode) return;
        pairCode_.clear();
        for (char c : code)
            if (c >= '0' && c <= '9') pairCode_.push_back(c);
    }
    cv_.notify_all();
}

void StreamEngine::CancelPairing() {
    {
        std::lock_guard lock(mutex_);
        pairCancel_ = true;
        if (pairing_.stage == PairStage::Connecting) pairing_.active = false;
    }
    cv_.notify_all();
    Changed();
}

void StreamEngine::DismissPairing() {
    {
        std::lock_guard lock(mutex_);
        if (pairing_.stage == PairStage::Done || pairing_.stage == PairStage::Failed) pairing_ = {};
    }
    Changed();
}

void StreamEngine::PairFailed(const std::string& message) {
    {
        std::lock_guard lock(mutex_);
        pairing_.stage = PairStage::Failed;
        pairing_.message = message;
    }
    Log("pairing: %s", message.c_str());
    Changed();
}

void StreamEngine::Forget(const std::string& key) {
    {
        std::lock_guard lock(mutex_);
        auto& t = settings_.tablets;
        t.erase(std::remove_if(t.begin(), t.end(),
                               [&](const KnownTablet& k) { return k.id.empty() ? k.name == key : k.id == key; }),
                t.end());
        if (persist_) SaveSettings(settings_);
        held_.erase(key);
        for (auto& slot : slots_)
            if (slot->active && slot->status.state == EngineState::Error && slot->status.key == key) slot->active = false;
    }
    Changed();
}

bool StreamEngine::AllowTouch(const std::string& key) const {
    std::lock_guard lock(mutex_);
    for (auto& t : settings_.tablets)
        if (t.id.empty() ? t.name == key : t.id == key) return t.allowTouch;
    return true;  // default for a tablet that streams (USB, or paired over Wi-Fi)
}

void StreamEngine::SetAllowTouch(const std::string& key, bool allow) {
    {
        std::lock_guard lock(mutex_);
        for (auto& t : settings_.tablets)
            if (t.id.empty() ? t.name == key : t.id == key) t.allowTouch = allow;
        if (persist_) SaveSettings(settings_);
        for (auto& slot : slots_) {
            if (slot->status.key != key) continue;
            slot->status.allowTouch = allow;
            if (slot->input) slot->input->SetAllowed(allow);
        }
    }
    Changed();
}

void StreamEngine::RefreshDisplays() {
    auto outputs = PhysicalOutputs();
    std::lock_guard lock(mutex_);
    if (outputs.empty()) return;
    status_.mainName = outputs[0].monitorName.empty() ? outputs[0].gdiName : outputs[0].monitorName;
    status_.mainWidth = uint32_t(outputs[0].Width());
    status_.mainHeight = uint32_t(outputs[0].Height());
}

void StreamEngine::Changed() {
    if (notify_) notify_();
}

void StreamEngine::SleepFor(int ms) {
    std::unique_lock lock(mutex_);
    cv_.wait_for(lock, std::chrono::milliseconds(ms), [this] { return wake_ || quit_.load(); });
    wake_ = false;
}

// Sleeps up to ms; false if the slot should end.
bool StreamEngine::SlotSleep(Slot& slot, int ms) {
    std::unique_lock lock(mutex_);
    cv_.wait_for(lock, std::chrono::milliseconds(ms), [&] { return slot.quit.load() || quit_.load(); });
    return !slot.quit && !quit_;
}

std::string StreamEngine::KeyOf(const proto::DeviceInfo& info) {
    return info.deviceId.empty() ? info.model : info.deviceId;
}

// The manager: finds USB devices and Wi-Fi addresses and keeps one slot per place.
void StreamEngine::Loop() {
    while (!quit_) {
        bool driverOk = driver_.IsOpen() || driver_.Open();
        bool paused, usb;
        {
            std::lock_guard lock(mutex_);
            status_.driverInstalled = driverOk;
            status_.paused = userStopped_;
            paused = userStopped_;
            usb = settings_.usb;
        }
        std::vector<std::string> serials;
        if (!paused && usb && !adb_.empty()) serials = AdbDevices(adb_);
        UpdateSlots(serials);
        Changed();
        SleepFor(paused ? 1000 : 1500);
    }
    std::list<std::unique_ptr<Slot>> all;
    {
        std::lock_guard lock(mutex_);
        for (auto& slot : slots_) {
            slot->quit = true;
            slot->stopSession = true;
        }
        all.swap(slots_);
    }
    cv_.notify_all();
    for (auto& slot : all)
        if (slot->thread.joinable()) slot->thread.join();
}

void StreamEngine::UpdateSlots(const std::vector<std::string>& usbSerials) {
    std::list<std::unique_ptr<Slot>> ending;
    {
        std::lock_guard lock(mutex_);
        const bool paused = userStopped_;
        auto wantedAddress = [&](const std::string& a) {
            return settings_.wifi && std::find(settings_.wifiAddresses.begin(), settings_.wifiAddresses.end(), a) !=
                                         settings_.wifiAddresses.end();
        };
        for (auto it = slots_.begin(); it != slots_.end();) {
            Slot& slot = **it;
            bool keep;
            if (!slot.serial.empty()) {
                bool listed = std::find(usbSerials.begin(), usbSerials.end(), slot.serial) != usbSerials.end();
                slot.missing = listed ? 0 : slot.missing + 1;
                // adb's list can blink (its server restarting): a streaming
                // device is only dropped by the session failing, an idle one
                // after three misses; a lost one stays to show the error
                bool busy = slot.active && slot.status.state != EngineState::Error;
                keep = settings_.usb && !paused && (listed || busy || slot.missing < 3 || slot.active);
            } else {
                keep = wantedAddress(slot.address) && !paused;
            }
            if (keep) {
                ++it;
                continue;
            }
            slot.quit = true;
            slot.stopSession = true;
            ending.push_back(std::move(*it));
            it = slots_.erase(it);
        }
        if (!paused) {
            auto add = [&](std::unique_ptr<Slot> slot) {
                Slot* raw = slot.get();
                slots_.push_back(std::move(slot));
                raw->thread = std::thread([this, raw] { SlotLoop(*raw); });
            };
            for (const auto& serial : usbSerials) {
                bool have = std::any_of(slots_.begin(), slots_.end(), [&](auto& s) { return s->serial == serial; });
                if (have) continue;
                auto slot = std::make_unique<Slot>();
                slot->serial = serial;
                // a local port per device for its adb forward, 27200-27299
                for (int tries = 0; tries < 100; ++tries) {
                    uint16_t port = nextLocalPort_;
                    nextLocalPort_ = uint16_t(nextLocalPort_ >= 27299 ? 27200 : nextLocalPort_ + 1);
                    bool used =
                        std::any_of(slots_.begin(), slots_.end(), [&](auto& s) { return s->localPort == port; });
                    if (!used) {
                        slot->localPort = port;
                        break;
                    }
                }
                if (slot->localPort) add(std::move(slot));
            }
            if (settings_.wifi) {
                for (const auto& address : settings_.wifiAddresses) {
                    bool have =
                        std::any_of(slots_.begin(), slots_.end(), [&](auto& s) { return s->address == address; });
                    if (have) continue;
                    auto slot = std::make_unique<Slot>();
                    slot->address = address;
                    add(std::move(slot));
                }
            }
        }
    }
    if (ending.empty()) return;
    cv_.notify_all();
    for (auto& slot : ending)
        if (slot->thread.joinable()) slot->thread.join();
    Changed();
}

// One place (a USB device or a Wi-Fi address): connect, stream, repeat.
void StreamEngine::SlotLoop(Slot& slot) {
    // Media Foundation and D3D need COM on this thread.
    const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    const bool usb = !slot.serial.empty();
    bool forwarded = false;
    // a head start for USB, for a tablet that is on both when the app starts
    if (!usb) SlotSleep(slot, 1500);
    while (!slot.quit && !quit_) {
        {
            // a new try: an earlier Stop or settings change has done its work
            std::lock_guard lock(mutex_);
            if (userStopped_) break;
            slot.stopSession = false;
        }
        SOCKET s = INVALID_SOCKET;
        bool secure = false;
        if (usb) {
            if (!forwarded) forwarded = AdbForward(adb_, slot.serial, slot.localPort, proto::kDefaultPort);
            if (forwarded) s = net::ConnectTimeout("127.0.0.1", slot.localPort, 700);
        } else {
            // a tablet that says it is streaming (to this PC over USB, or to
            // another PC) won't answer until it is free
            bool busy;
            {
                std::lock_guard lock(mutex_);
                busy = std::any_of(status_.nearby.begin(), status_.nearby.end(),
                                   [&](const NearbyTablet& n) { return n.address == slot.address && n.busy; });
            }
            if (busy) {
                if (!SlotSleep(slot, 2000)) break;
                continue;
            }
            std::string host;
            uint16_t port;
            ParseAddress(slot.address, host, port);
            // Wi-Fi is encrypted; loopback (the test tools) is treated like USB
            secure = !IsLoopback(host);
            s = net::ConnectTimeout(host, port, 700);
        }
        Channel ch(s);
        bool pairNow;
        {
            std::lock_guard lock(mutex_);
            pairNow = !usb && pairing_.active && pairing_.stage == PairStage::Connecting &&
                      pairing_.address == slot.address;
        }
        if (pairNow) {
            if (s != INVALID_SOCKET) RunPairing(slot, ch);
            else PairFailed("Couldn't reach the tablet at " + slot.address + ". Is ScreenFleet open on it?");
            if (s != INVALID_SOCKET) closesocket(s);
            if (!SlotSleep(slot, 500)) break;
            continue;
        }
        proto::DeviceInfo info;
        SessionConfig config;
        HandshakeResult result = s == INVALID_SOCKET ? HandshakeResult::Failed : Handshake(ch, secure, info, config);
        if (result == HandshakeResult::NotPaired || result == HandshakeResult::OldApp) {
            ch.Write(proto::MsgType::Bye);
            closesocket(s);
            {
                std::lock_guard lock(mutex_);
                slot.failures = 0;
                slot.active = true;
                slot.status = {};
                slot.status.key = info.deviceId.empty() ? info.model : info.deviceId;
                slot.status.address = slot.address;
                slot.status.state = EngineState::Error;
                slot.status.device = info.model.empty() ? slot.address : info.model;
                slot.status.link = "Wi-Fi";
                slot.status.endedAt = ClockNow();
                slot.status.needsPair = result == HandshakeResult::NotPaired;
                slot.status.error = result == HandshakeResult::NotPaired
                                        ? "Not paired with this PC yet. Press Pair, then type the code the tablet shows."
                                        : "Update ScreenFleet on this tablet: this version can't encrypt Wi-Fi.";
            }
            Changed();
            if (!SlotSleep(slot, 3000)) break;
            continue;
        }
        if (result != HandshakeResult::Ok) {
            if (s != INVALID_SOCKET) closesocket(s);
            // With adb forward, connect() succeeds even when the app isn't
            // running; forward again next time in case the device rebooted.
            forwarded = false;
            int failures;
            {
                std::lock_guard lock(mutex_);
                failures = ++slot.failures;
            }
            // a device without the app open is asked less often after a while
            if (!SlotSleep(slot, failures < 10 ? 1000 : 3000)) break;
            continue;
        }
        const std::string key = KeyOf(info);
        const std::string link = usb ? "USB" : "Wi-Fi";
        bool go;
        {
            std::lock_guard lock(mutex_);
            slot.failures = 0;
            slot.lastKey = key;
            // the same tablet over its other link (USB and Wi-Fi at once) is skipped
            go = !userStopped_ && !slot.quit &&
                 std::find(streamingKeys_.begin(), streamingKeys_.end(), key) == streamingKeys_.end();
            // on both: USB wins (faster, and private) while its link works
            if (go && !usb)
                go = !std::any_of(slots_.begin(), slots_.end(), [&](const auto& other) {
                    return !other->serial.empty() && other->lastKey == key && other->failures < 3 && !other->quit;
                });
            // disconnected here: only if the tablet asked to connect after that
            auto hold = held_.find(key);
            if (go && hold != held_.end()) {
                bool asked = info.connectRequestAge != proto::kNoConnectRequest &&
                             SteadySeconds() - int64_t(info.connectRequestAge) > hold->second;
                if (asked) {
                    Log("%s asked to connect from the tablet", info.model.c_str());
                    held_.erase(hold);
                } else {
                    go = false;
                }
            }
            std::string refusal;
            if (go && !driver_.IsOpen())
                refusal = "The virtual display driver isn't installed (Startup and driver).";
            else if (go && streamingKeys_.size() >= SST_MAX_MONITORS)
                refusal = "Up to " + std::to_string(SST_MAX_MONITORS) + " tablets can be monitors at once.";
            if (!refusal.empty()) {
                go = false;
                slot.active = true;
                slot.status = {};
                slot.status.key = key;
                slot.status.state = EngineState::Error;
                slot.status.device = info.model;
                slot.status.link = link;
                slot.status.sdk = info.sdkInt;
                slot.status.width = config.width;
                slot.status.height = config.height;
                slot.status.endedAt = ClockNow();
                slot.status.error = refusal;
            }
            if (go) streamingKeys_.push_back(key);
        }
        if (!go) {
            ch.Write(proto::MsgType::Bye);
            closesocket(s);
            if (!SlotSleep(slot, 2000)) break;
            continue;
        }
        RememberTablet(info, key, link, slot.address);
        if (SendStart(ch, config)) RunDeviceSession(slot, ch, info, config);
        closesocket(s);
        {
            std::lock_guard lock(mutex_);
            streamingKeys_.erase(std::remove(streamingKeys_.begin(), streamingKeys_.end(), key), streamingKeys_.end());
        }
        Changed();
        if (!SlotSleep(slot, 500)) break;
    }
    if (com) CoUninitialize();
}

// Every two seconds: ask the network who is there and keep the list of
// tablets that answered in the last six. Listing only: a tablet is
// connected to when the user adds it (Wi-Fi has no pairing yet, so a reply
// is never trusted to move a known tablet to a new address).
void StreamEngine::DiscoveryLoop() {
    SOCKET u = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (u == INVALID_SOCKET) return;
    BOOL yes = TRUE;
    setsockopt(u, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&yes), sizeof(yes));
    sockaddr_in local{};
    local.sin_family = AF_INET;
    bind(u, reinterpret_cast<sockaddr*>(&local), sizeof(local));
    const auto query = proto::EncodeDiscoveryQuery();
    using Clock = std::chrono::steady_clock;
    std::map<std::string, std::pair<NearbyTablet, Clock::time_point>> seen;

    while (!quit_) {
        bool on;
        {
            std::lock_guard lock(mutex_);
            on = settings_.wifi && !userStopped_;
        }
        if (on) {
            for (const auto& target : BroadcastTargets())
                sendto(u, reinterpret_cast<const char*>(query.data()), int(query.size()), 0,
                       reinterpret_cast<const sockaddr*>(&target), sizeof(target));
            const auto until = Clock::now() + std::chrono::milliseconds(1500);
            while (!quit_ && Clock::now() < until) {
                fd_set readable;
                FD_ZERO(&readable);
                FD_SET(u, &readable);
                timeval tv{0, 200 * 1000};
                if (select(0, &readable, nullptr, nullptr, &tv) != 1) continue;
                uint8_t buf[512];
                sockaddr_in from{};
                int fromLen = sizeof(from);
                int n = recvfrom(u, reinterpret_cast<char*>(buf), sizeof(buf), 0, reinterpret_cast<sockaddr*>(&from),
                                 &fromLen);
                proto::DiscoveryReply reply;
                if (n <= 0 || !proto::Decode(buf, size_t(n), reply) || !reply.tcpPort) continue;
                char ip[INET_ADDRSTRLEN] = {};
                inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
                NearbyTablet t;
                t.address = ip;
                if (reply.tcpPort != proto::kDefaultPort) t.address += ":" + std::to_string(reply.tcpPort);
                t.name = CleanName(reply.model);
                if (t.name.empty()) t.name = "Tablet";
                t.id = CleanName(reply.deviceId);
                t.width = reply.width;
                t.height = reply.height;
                t.sdk = reply.sdkInt;
                t.busy = (reply.flags & proto::discovery_flags::kBusy) != 0;
                t.busyWith = CleanName(reply.busyWith);
                if (seen.size() < 64 || seen.count(t.address)) seen[t.address] = {t, Clock::now()};
            }
        } else {
            seen.clear();
        }

        std::vector<NearbyTablet> list;
        for (auto it = seen.begin(); it != seen.end();) {
            if (Clock::now() - it->second.second > std::chrono::seconds(6)) {
                it = seen.erase(it);
            } else {
                list.push_back(it->second.first);
                ++it;
            }
        }
        bool changed;
        bool moved = false;
        {
            std::lock_guard lock(mutex_);
            // A paired tablet that got a new address (DHCP): follow it. Safe now
            // that every Wi-Fi session proves the tablet holds the pairing secret;
            // someone else claiming its id only gets a failed session.
            for (const auto& n : list) {
                if (n.id.empty()) continue;
                for (auto& k : settings_.tablets) {
                    if (k.id != n.id || k.pairKey.empty() || k.address.empty() || k.address == n.address) continue;
                    auto& addrs = settings_.wifiAddresses;
                    auto old = std::find(addrs.begin(), addrs.end(), k.address);
                    if (old == addrs.end() || std::find(addrs.begin(), addrs.end(), n.address) != addrs.end()) continue;
                    Log("%s moved from %s to %s", k.name.c_str(), k.address.c_str(), n.address.c_str());
                    *old = n.address;
                    k.address = n.address;
                    moved = true;
                }
            }
            if (moved && persist_) SaveSettings(settings_);
            for (auto& t : list)
                t.added = std::find(settings_.wifiAddresses.begin(), settings_.wifiAddresses.end(), t.address) !=
                          settings_.wifiAddresses.end();
            changed = list.size() != status_.nearby.size();
            for (size_t i = 0; !changed && i < list.size(); ++i) {
                const auto &a = list[i], &b = status_.nearby[i];
                changed = a.address != b.address || a.name != b.name || a.busy != b.busy || a.added != b.added ||
                          a.busyWith != b.busyWith;
            }
            status_.nearby = std::move(list);
        }
        if (changed || moved) Changed();
        for (int i = 0; i < 5 && !quit_; ++i) Sleep(100);
    }
    closesocket(u);
}

// Pairing (PROTOCOL.md): commit, exchange keys, show the same code on both
// sides, the user types it here and taps Pair on the tablet.
void StreamEngine::RunPairing(Slot& slot, Channel& ch) {
    const SOCKET s = ch.Socket();
    std::string hostId, hostName;
    {
        std::lock_guard lock(mutex_);
        hostId = settings_.hostId;
    }
    wchar_t pcName[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD pcNameLen = ARRAYSIZE(pcName);
    if (GetComputerNameW(pcName, &pcNameLen)) hostName = Narrow(pcName);

    crypto::EcdhKey eph;
    if (!eph.Ok()) return PairFailed("Couldn't make a key.");
    crypto::Bytes nH = crypto::Random(32);
    crypto::Key32 commit = crypto::Sha256(Cat(eph.Public(), nH));
    proto::Writer start;
    start.Str(hostId);
    start.Str(hostName);
    start.Raw(commit.data(), commit.size());
    if (!ch.Write(proto::MsgType::PairStart, start.buf.data(), start.buf.size()))
        return PairFailed("Lost the connection to the tablet.");

    net::SetRecvTimeout(s, 10000);
    proto::MsgType type;
    std::vector<uint8_t> payload;
    if (!ch.Read(type, payload)) return PairFailed("The tablet didn't answer. Is it showing its waiting screen?");
    if (type == proto::MsgType::Bye) return PairFailed("The tablet is busy (streaming?) or said no.");
    if (type != proto::MsgType::PairReply) return PairFailed("The tablet's app is too old to pair. Update it.");
    proto::Reader r(payload);
    std::string deviceId = r.Str();
    std::string model = CleanName(r.Str());
    crypto::PublicKey pkD{};
    uint8_t nD[32];
    r.Raw(pkD.data(), pkD.size());
    r.Raw(nD, sizeof(nD));
    if (!r.Ok() || deviceId.empty()) return PairFailed("The tablet sent something unexpected.");

    proto::Writer reveal;
    reveal.Raw(eph.Public().data(), eph.Public().size());
    reveal.Raw(nH.data(), nH.size());
    if (!ch.Write(proto::MsgType::PairReveal, reveal.buf.data(), reveal.buf.size()))
        return PairFailed("Lost the connection to the tablet.");
    crypto::Key32 z{};
    if (!eph.Agree(pkD, z)) return PairFailed("The tablet sent an invalid key.");
    auto th = crypto::Sha256(Cat(Label("SST pair v1"), Str(hostId), Str(deviceId), eph.Public(), pkD, nH, nD));
    crypto::Bytes t(th.begin(), th.end());
    auto codeHash = crypto::Sha256(Cat(Label("SST code"), t));
    uint32_t codeValue = ((uint32_t(codeHash[0]) << 24) | (uint32_t(codeHash[1]) << 16) | (uint32_t(codeHash[2]) << 8) |
                          codeHash[3]) % 1000000;
    char code[8];
    snprintf(code, sizeof(code), "%06u", codeValue);
    crypto::Bytes k = crypto::Hkdf(t, crypto::Bytes(z.begin(), z.end()), "SST pairing key", 32);
    SecureZeroMemory(z.data(), z.size());

    // the user types the code the tablet shows
    std::string typed;
    {
        std::unique_lock lock(mutex_);
        pairing_.stage = PairStage::EnterCode;
        pairing_.name = model;
        pairCode_.clear();
        Changed();
        bool done = cv_.wait_for(lock, std::chrono::seconds(120), [&] {
            return !pairCode_.empty() || pairCancel_ || quit_ || slot.quit;
        });
        typed = pairCode_;
        if (!done || pairCancel_ || quit_ || slot.quit) typed.clear();
    }
    if (typed.empty()) {
        ch.Write(proto::MsgType::Bye);
        return PairFailed("Pairing cancelled.");
    }
    if (typed.size() != 6 || !crypto::Equal(reinterpret_cast<const uint8_t*>(typed.data()),
                                            reinterpret_cast<const uint8_t*>(code), 6)) {
        ch.Write(proto::MsgType::Bye);
        return PairFailed("That code didn't match. Press Pair to try again.");
    }
    auto confirm = crypto::HmacSha256(k.data(), k.size(), Cat(Label("host"), t));
    if (!ch.Write(proto::MsgType::PairConfirm, confirm.data(), confirm.size()))
        return PairFailed("Lost the connection to the tablet.");
    {
        std::lock_guard lock(mutex_);
        pairing_.stage = PairStage::WaitTablet;
    }
    Changed();

    // now the tablet's user taps Pair
    net::SetRecvTimeout(s, 120000);
    if (!ch.Read(type, payload)) return PairFailed("The tablet didn't confirm in time.");
    if (type == proto::MsgType::Bye) return PairFailed("Cancelled on the tablet.");
    auto expect = crypto::HmacSha256(k.data(), k.size(), Cat(Label("device"), t));
    if (type != proto::MsgType::PairDone || payload.size() != 32 || !crypto::Equal(expect.data(), payload.data(), 32))
        return PairFailed("The tablet's confirmation didn't check out.");

    std::string protectedKey = crypto::Protect(k);
    SecureZeroMemory(k.data(), k.size());
    if (protectedKey.empty()) return PairFailed("Windows couldn't store the pairing.");
    {
        std::lock_guard lock(mutex_);
        auto& list = settings_.tablets;
        auto it = std::find_if(list.begin(), list.end(), [&](const KnownTablet& kt) { return kt.id == deviceId; });
        if (it == list.end()) {
            list.push_back({});
            it = list.end() - 1;
            it->id = CleanName(deviceId);
        }
        it->name = model.empty() ? "Tablet" : model;
        it->link = "Wi-Fi";
        it->address = slot.address;
        it->pairKey = protectedKey;
        it->lastSeen = int64_t(std::time(nullptr));
        if (persist_) SaveSettings(settings_);
        pairing_.stage = PairStage::Done;
        pairing_.message = "Paired with " + it->name + ". It connects in a moment.";
        slot.active = false;  // drop the "not paired" card
    }
    Log("paired with %s", model.c_str());
    Changed();
}

// Hello -> DeviceInfo; the caller decides whether StreamStart (or Bye) follows.
StreamEngine::HandshakeResult StreamEngine::Handshake(Channel& ch, bool secure, proto::DeviceInfo& info,
                                                      SessionConfig& config) {
    const SOCKET s = ch.Socket();
    proto::Hello helloMsg;
    wchar_t pcName[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD pcNameLen = ARRAYSIZE(pcName);
    if (GetComputerNameW(pcName, &pcNameLen)) helloMsg.hostName = Narrow(pcName);
    std::unique_ptr<crypto::EcdhKey> eph;
    if (secure) {
        eph = std::make_unique<crypto::EcdhKey>();
        if (!eph->Ok()) return HandshakeResult::Failed;
        helloMsg.flags = proto::hello_flags::kSecure;
        helloMsg.hostId = GetSettings().hostId;
        std::copy(eph->Public().begin(), eph->Public().end(), helloMsg.epk);
        auto nonce = crypto::Random(32);
        std::copy(nonce.begin(), nonce.end(), helloMsg.nonce);
    }
    auto hello = proto::Encode(helloMsg);
    if (!ch.Write(proto::MsgType::Hello, hello.data(), hello.size())) return HandshakeResult::Failed;

    net::SetRecvTimeout(s, 3000);
    proto::MsgType type;
    std::vector<uint8_t> payload;
    // With adb forward, connect() succeeds even when the app isn't running;
    // the connection is then closed right away and this read fails.
    if (!ch.Read(type, payload)) return HandshakeResult::Failed;

    if (secure) {
        if (type == proto::MsgType::DeviceInfo) {
            proto::Decode(payload, info);
            info.model = CleanName(info.model);
            info.deviceId = CleanName(info.deviceId);
            Log("%s answered without encryption (old app); not streaming over Wi-Fi", info.model.c_str());
            return HandshakeResult::OldApp;
        }
        if (type == proto::MsgType::NotPaired) {
            proto::Reader r(payload);
            info.deviceId = CleanName(r.Str());
            info.model = CleanName(r.Str());
            return HandshakeResult::NotPaired;
        }
        if (type != proto::MsgType::SessionReply) {
            Log("unexpected reply from device");
            return HandshakeResult::Failed;
        }
        proto::Reader r(payload);
        std::string deviceId = r.Str();
        crypto::PublicKey epkD{};
        uint8_t nD[32], macD[32];
        r.Raw(epkD.data(), epkD.size());
        r.Raw(nD, sizeof(nD));
        r.Raw(macD, sizeof(macD));
        if (!r.Ok()) return HandshakeResult::Failed;
        // the pairing secret for this tablet
        crypto::Bytes k;
        std::string model;
        {
            std::lock_guard lock(mutex_);
            for (const auto& t : settings_.tablets) {
                if (!t.id.empty() && t.id == deviceId && !t.pairKey.empty()) {
                    crypto::Unprotect(t.pairKey, k);
                    model = t.name;
                }
            }
        }
        info.deviceId = CleanName(deviceId);
        info.model = model;
        if (k.size() != 32) return HandshakeResult::NotPaired;  // it knows this PC, but this PC forgot it
        crypto::Key32 z{};
        if (!eph->Agree(epkD, z)) return HandshakeResult::Failed;
        crypto::Bytes t = [&] {
            auto h = crypto::Sha256(Cat(Label("SST session v1"), Str(helloMsg.hostId), Str(deviceId), eph->Public(),
                                        epkD, helloMsg.nonce, nD));
            return crypto::Bytes(h.begin(), h.end());
        }();
        crypto::Bytes keys = crypto::Hkdf(t, Cat(k, z), "SST session keys", 96);
        SecureZeroMemory(k.data(), k.size());
        SecureZeroMemory(z.data(), z.size());
        auto expectD = crypto::HmacSha256(keys.data() + 64, 32, Cat(Label("device"), t));
        if (!crypto::Equal(expectD.data(), macD, 32)) {
            Log("tablet %s failed the session check (different pairing?)", info.deviceId.c_str());
            return HandshakeResult::NotPaired;
        }
        auto macH = crypto::HmacSha256(keys.data() + 64, 32, Cat(Label("host"), t));
        if (!ch.Write(proto::MsgType::SessionConfirm, macH.data(), macH.size())) return HandshakeResult::Failed;
        ch.Secure(std::make_unique<crypto::SealKey>(keys.data(), 1), std::make_unique<crypto::SealKey>(keys.data() + 32, 2));
        SecureZeroMemory(keys.data(), keys.size());
        if (!ch.Read(type, payload)) return HandshakeResult::Failed;
    }
    net::SetRecvTimeout(s, 0);
    if (type != proto::MsgType::DeviceInfo || !proto::Decode(payload, info)) {
        Log("unexpected reply from device");
        return HandshakeResult::Failed;
    }
    if (info.version != proto::kVersion) {
        Log("device speaks protocol v%u, this host speaks v%u; please update both", info.version, proto::kVersion);
        return HandshakeResult::Failed;
    }
    // Names come from the network: they end up in settings.ini, the window
    // and the tray tooltip, so keep them to one short line of printable text.
    info.model = CleanName(info.model);
    if (info.model.empty()) info.model = "Tablet";
    info.deviceId = CleanName(info.deviceId);
    if (info.width < 16 || info.height < 16 || info.width > 4096 || info.height > 4096) {
        Log("device reported an invalid screen size %ux%u", info.width, info.height);
        return HandshakeResult::Failed;
    }
    Log("device: \"%s\", Android API %u, %ux%u @ %u dpi, max %u fps", info.model.c_str(), info.sdkInt, info.width,
        info.height, info.densityDpi, info.maxFps);

    Settings snap = GetSettings();
    config.width = info.width & ~1u;
    config.height = info.height & ~1u;
    config.fps = std::min<UINT>(snap.fps, info.maxFps ? info.maxFps : snap.fps);
    config.bitrate = snap.bitrateMbps * 1'000'000;
    config.trailingFrames = (info.flags & proto::device_flags::kFlushFrames) ? 3 : 0;
    return HandshakeResult::Ok;
}

bool StreamEngine::SendStart(Channel& ch, const SessionConfig& config) {
    proto::StreamStart start;
    start.width = uint16_t(config.width);
    start.height = uint16_t(config.height);
    start.fps = uint16_t(config.fps);
    start.bitrate = config.bitrate;
    start.flags = proto::stream_flags::kExtend;  // always: the tablet is a monitor of its own
    auto startBytes = proto::Encode(start);
    return ch.Write(proto::MsgType::StreamStart, startBytes.data(), startBytes.size());
}

void StreamEngine::RememberTablet(const proto::DeviceInfo& info, const std::string& key, const std::string& link,
                                  const std::string& address) {
    std::lock_guard lock(mutex_);
    auto& list = settings_.tablets;
    // by id; a tablet remembered before ids existed is matched by name once and gets its id
    auto it = std::find_if(list.begin(), list.end(), [&](const KnownTablet& t) { return !t.id.empty() && t.id == key; });
    if (it == list.end())
        it = std::find_if(list.begin(), list.end(),
                          [&](const KnownTablet& t) { return t.id.empty() && t.name == info.model; });
    if (it == list.end()) {
        list.push_back({});
        it = list.end() - 1;
    }
    it->id = info.deviceId;
    it->name = info.model;
    it->width = info.width;
    it->height = info.height;
    it->sdk = info.sdkInt;
    it->link = link;
    if (!address.empty()) it->address = address;
    it->lastSeen = int64_t(std::time(nullptr));
    if (persist_) SaveSettings(settings_);
}

void StreamEngine::RunDeviceSession(Slot& slot, Channel& ch, const proto::DeviceInfo& info, const SessionConfig& config) {
    const std::string link = slot.serial.empty() ? "Wi-Fi" : "USB";
    {
        std::lock_guard lock(mutex_);
        slot.restart = false;
        slot.active = true;
        TabletStatus& st = slot.status;
        st = {};
        st.key = KeyOf(info);
        st.address = slot.address;
        st.state = EngineState::Connecting;
        st.device = info.model;
        st.link = link;
        st.sdk = info.sdkInt;
        st.width = config.width;
        st.height = config.height;
        st.refresh = config.fps;
        st.touch = (info.flags & proto::device_flags::kTouch) != 0;
        st.pen = (info.flags & proto::device_flags::kPen) != 0;
        st.steps = {"Found " + info.model + " on " + link};
    }
    const bool allowTouch = AllowTouch(KeyOf(info));
    {
        std::lock_guard lock(mutex_);
        slot.status.allowTouch = allowTouch;
    }
    Changed();

    auto addStep = [&](const std::string& step) {
        {
            std::lock_guard lock(mutex_);
            slot.status.steps.push_back(step);
        }
        Changed();
    };

    std::string failure;
    uint32_t monitorId = 0;
    DisplayOutput output;
    {
        // one at a time, so each session finds its own new monitor
        std::lock_guard add(monitorMutex_);
        std::vector<std::string> before;
        for (auto& o : EnumerateOutputs())
            if (o.monitorName == kVirtualMonitorName) before.push_back(o.gdiName);
        monitorId = driver_.AddMonitor(config.width, config.height, config.fps, DeviceSerial(info));
        if (!monitorId) {
            failure = "The display driver could not add a monitor.";
        } else {
            addStep("Added a " + std::to_string(config.width) + " × " + std::to_string(config.height) + " monitor");
            if (!WaitForVirtualOutput(config.width, config.height, before, output, slot.stopSession))
                failure = "The new monitor did not appear. Is Windows set to Extend? Press Win+P.";
        }
    }

    // Touches go to this tablet's own monitor. Only now, in a stream with
    // a tablet that is on USB or paired and encrypted over Wi-Fi.
    std::unique_ptr<TabletInput> input;
    if (failure.empty()) {
        input = std::make_unique<TabletInput>(output.gdiName, info.model, allowTouch);
        std::lock_guard lock(mutex_);
        slot.input = input.get();
        input->SetAllowed(slot.status.allowTouch);  // changed meanwhile
    }
    NetSink sink(
        ch,
        [this, &slot](const proto::DeviceStats& stats) {
            std::lock_guard lock(mutex_);
            slot.status.decodeMs = stats.decodeUs ? stats.decodeUs / 1000.0 : -1;
        },
        [&input](const proto::InputEvent& event) {
            if (input) input->OnEvent(event);
        });

    if (failure.empty()) {
        addStep("Waiting for the first picture…");
        // Once a second: frame rate and bitrate for the window, and the switch
        // from Connecting to Streaming when the first frame has gone out.
        std::atomic<bool> samplerDone{false};
        std::thread sampler([&] {
            std::vector<uint64_t> bytesHistory;
            int tick = 0;
            while (!samplerDone) {
                for (int i = 0; i < 10 && !samplerDone; ++i) {
                    Sleep(100);
                    if (input) input->Tick();
                }
                if (++tick % 2 == 0) sink.SendPing();
                uint32_t frames = 0;
                uint64_t bytes = 0;
                sink.TakeCounters(frames, bytes);
                {
                    std::lock_guard lock(mutex_);
                    TabletStatus& st = slot.status;
                    if (st.state == EngineState::Connecting && sink.AnyFrameSent()) {
                        st.state = EngineState::Streaming;
                        st.startedAt = ClockNow();
                        st.steps.clear();
                    }
                    if (st.state == EngineState::Streaming) {
                        st.fps.push_back(int(frames));
                        if (st.fps.size() > 60) st.fps.erase(st.fps.begin());
                        bytesHistory.push_back(bytes);
                        if (bytesHistory.size() > 5) bytesHistory.erase(bytesHistory.begin());
                        uint64_t sum = 0;
                        for (auto b : bytesHistory) sum += b;
                        st.mbps = double(sum) * 8 / 1e6 / double(bytesHistory.size());
                    }
                }
                Changed();
            }
        });

        try {
            auto target = OpenCapture(output);
            Log("streaming started (%s)", info.model.c_str());
            RunSession(target->gpu, target->capture, config, sink, slot.stopSession);
        } catch (const std::exception& e) {
            Log("session error: %s", e.what());
            failure = e.what();
        }
        samplerDone = true;
        sampler.join();
    }

    bool byUser, restart;
    {
        std::lock_guard lock(mutex_);
        byUser = userStopped_ || quit_ || slot.quit || held_.count(KeyOf(info));
        restart = slot.restart;
    }
    if ((byUser || restart) && sink.Ok()) sink.SendBye();
    {
        std::lock_guard lock(mutex_);
        slot.input = nullptr;
    }
    // lift anything still pressed before the monitor goes away
    if (input) input->SetAllowed(false);
    if (monitorId) driver_.RemoveMonitor(monitorId);

    {
        std::lock_guard lock(mutex_);
        TabletStatus& st = slot.status;
        st.endedAt = ClockNow();
        st.steps.clear();
        if (byUser || restart || (failure.empty() && sink.ByeReceived())) {
            slot.active = false;
            st.state = EngineState::Waiting;
        } else {
            st.state = EngineState::Error;
            st.error = failure.empty() ? "Lost the connection to " + info.model + " at " + st.endedAt + "." : failure;
        }
    }
    Log("streaming stopped (%s)", info.model.c_str());
    Changed();
}

}  // namespace sst
