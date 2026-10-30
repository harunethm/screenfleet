// Synthetic pointer devices need Windows 10 1809; the functions are looked up
// at run time so the host still starts (without touch) on older Windows.
#define NTDDI_VERSION 0x0A000006  // NTDDI_WIN10_RS5, for the declarations
#include "touch_input.h"

#include <algorithm>

#include "log.h"

namespace sst {

namespace {

using Clock = std::chrono::steady_clock;
namespace in = proto::input;

std::atomic<InputMode> g_mode{InputMode::Inject};

struct PointerApi {
    decltype(&CreateSyntheticPointerDevice) create = nullptr;
    decltype(&InjectSyntheticPointerInput) inject = nullptr;
    decltype(&DestroySyntheticPointerDevice) destroy = nullptr;
    bool Ok() const { return create && inject && destroy; }
};

const PointerApi& Api() {
    static const PointerApi api = [] {
        PointerApi a;
        if (HMODULE user32 = GetModuleHandleW(L"user32.dll")) {
            a.create = reinterpret_cast<decltype(a.create)>(GetProcAddress(user32, "CreateSyntheticPointerDevice"));
            a.inject = reinterpret_cast<decltype(a.inject)>(GetProcAddress(user32, "InjectSyntheticPointerInput"));
            a.destroy = reinterpret_cast<decltype(a.destroy)>(GetProcAddress(user32, "DestroySyntheticPointerDevice"));
        }
        return a;
    }();
    return api;
}

// Monitor positions and injected points in physical pixels, whatever the
// calling thread's DPI awareness was.
class PhysicalPixels {
public:
    PhysicalPixels() : old_(SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {}
    ~PhysicalPixels() {
        if (old_) SetThreadDpiAwarenessContext(old_);
    }
    PhysicalPixels(const PhysicalPixels&) = delete;
    PhysicalPixels& operator=(const PhysicalPixels&) = delete;

private:
    DPI_AWARENESS_CONTEXT old_;
};

std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring out(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), out.data(), n);
    return out;
}

constexpr auto kKeepAlive = std::chrono::milliseconds(100);
constexpr auto kRectMaxAge = std::chrono::seconds(1);
constexpr auto kMinMoveGap = std::chrono::milliseconds(4);

}  // namespace

void SetInputMode(InputMode mode) { g_mode = mode; }

TabletInput::TabletInput(std::string gdiName, std::string label, bool allowed)
    : gdiName_(std::move(gdiName)), label_(std::move(label)), allowed_(allowed) {}

TabletInput::~TabletInput() {
    std::lock_guard lock(mutex_);
    ReleaseAll();
    if (touchDevice_) Api().destroy(HSYNTHETICPOINTERDEVICE(touchDevice_));
    if (penDevice_) Api().destroy(HSYNTHETICPOINTERDEVICE(penDevice_));
}

void TabletInput::SetAllowed(bool allowed) {
    std::lock_guard lock(mutex_);
    if (allowed_ == allowed) return;
    if (!allowed) ReleaseAll();
    allowed_ = allowed;
    Log("touch from %s %s", label_.c_str(), allowed ? "allowed" : "turned off");
}

void TabletInput::OnEvent(const proto::InputEvent& event) {
    std::lock_guard lock(mutex_);
    if (!allowed_ || failed_) return;
    Apply(event);
}

void TabletInput::Tick() {
    std::lock_guard lock(mutex_);
    if (!allowed_ || failed_ || g_mode != InputMode::Inject) return;
    bool touching = pen_.touching;
    for (auto& p : touch_) touching |= p.present;
    if (!touching || Clock::now() - lastInject_ < kKeepAlive) return;
    PhysicalPixels px;
    // the same picture again: Windows sees the contacts are still there
    InjectTouch(touch_, false);
    if (pen_.touching) InjectPen(pen_, false);
}

bool TabletInput::RefreshRect() {
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    if (!EnumDisplaySettingsExW(Widen(gdiName_).c_str(), ENUM_CURRENT_SETTINGS, &dm, 0) || dm.dmPelsWidth == 0 ||
        dm.dmPelsHeight == 0) {
        haveRect_ = false;
        return false;
    }
    rect_ = {dm.dmPosition.x, dm.dmPosition.y, dm.dmPosition.x + LONG(dm.dmPelsWidth),
             dm.dmPosition.y + LONG(dm.dmPelsHeight)};
    haveRect_ = true;
    rectAt_ = Clock::now();
    return true;
}

POINT TabletInput::Map(uint16_t x, uint16_t y) const {
    // 0..65535 across the picture onto the monitor's pixels, never outside it
    const int64_t w = rect_.right - rect_.left, h = rect_.bottom - rect_.top;
    return {LONG(rect_.left + (int64_t(x) * (w - 1) + 32767) / 65535),
            LONG(rect_.top + (int64_t(y) * (h - 1) + 32767) / 65535)};
}

void TabletInput::Apply(const proto::InputEvent& event) {
    const bool canceled = (event.flags & in::kCanceled) != 0;
    bool anyBefore = pen_.present;
    for (auto& p : touch_) anyBefore |= p.present;

    PhysicalPixels px;
    // where the monitor is: looked up as a gesture starts (it may have been
    // moved); lifting everything only needs the last positions
    if (event.count && (!anyBefore || !haveRect_ || Clock::now() - rectAt_ > kRectMaxAge) && !RefreshRect()) {
        if (anyBefore) ReleaseAll();  // count 0: doesn't come back here
        return;
    }

    Pointer touch[in::kMaxId + 1];
    Pointer pen;
    for (uint8_t i = 0; i < event.count; ++i) {
        const proto::InputPointer& q = event.pointers[i];
        Pointer& p = q.tool == in::Tool::Pen ? pen : touch[q.id];
        p.present = true;
        p.touching = (q.state & in::kTouching) != 0;
        p.state = q.state;
        p.pt = Map(q.x, q.y);
        p.pressure = q.pressure;
    }

    // At most ~250 moves a second reach Windows: a snapshot that only moves
    // pointers right after the last one is skipped (the next one carries
    // the newer positions). Fingers arriving or leaving always go through.
    bool onlyMoves = !canceled && pen.present == pen_.present && pen.touching == pen_.touching &&
                     pen.state == pen_.state;
    for (int id = 0; id <= in::kMaxId && onlyMoves; ++id) onlyMoves = touch[id].present == touch_[id].present;
    if (onlyMoves && Clock::now() - lastInject_ < kMinMoveGap) return;

    if (g_mode == InputMode::Log) {
        for (int id = 0; id <= in::kMaxId; ++id) {
            if (touch[id].present != touch_[id].present)
                Log("input %s: finger %d %s at %ld,%ld", label_.c_str(), id,
                    touch[id].present ? "down" : (canceled ? "canceled" : "up"),
                    (touch[id].present ? touch[id] : touch_[id]).pt.x, (touch[id].present ? touch[id] : touch_[id]).pt.y);
        }
        if (pen.present != pen_.present || pen.touching != pen_.touching) {
            const Pointer& p = pen.present ? pen : pen_;
            Log("input %s: pen %s at %ld,%ld pressure %u", label_.c_str(),
                pen.touching ? "down" : pen.present ? "hover" : (canceled ? "canceled" : "away"), p.pt.x, p.pt.y,
                pen.pressure);
        }
        std::copy(std::begin(touch), std::end(touch), std::begin(touch_));
        pen_ = pen;
        lastInject_ = Clock::now();
        return;
    }

    InjectTouch(touch, canceled);
    InjectPen(pen, canceled);
}

void TabletInput::InjectTouch(const Pointer (&next)[in::kMaxId + 1], bool canceled) {
    bool any = false;
    for (int id = 0; id <= in::kMaxId; ++id) any |= next[id].present || touch_[id].present;
    if (!any) return;
    if (!touchDevice_ && !failed_) {
        if (Api().Ok()) touchDevice_ = Api().create(PT_TOUCH, ULONG(in::kMaxPointers), POINTER_FEEDBACK_DEFAULT);
        if (!touchDevice_) {
            failed_ = true;
            Log("touch: no synthetic touch device (error %lu); touch needs Windows 10 1809 or newer", GetLastError());
            return;
        }
    }
    if (!touchDevice_) return;

    auto entry = [](int id, POINT pt, POINTER_FLAGS flags) {
        POINTER_TYPE_INFO info{};
        info.type = PT_TOUCH;
        POINTER_TOUCH_INFO& t = info.touchInfo;
        t.pointerInfo.pointerType = PT_TOUCH;
        t.pointerInfo.pointerId = UINT32(id);
        t.pointerInfo.ptPixelLocation = pt;
        t.pointerInfo.pointerFlags = flags;
        t.touchMask = TOUCH_MASK_CONTACTAREA;
        t.rcContact = {pt.x - 2, pt.y - 2, pt.x + 2, pt.y + 2};
        return info;
    };
    auto inject = [&](const POINTER_TYPE_INFO* frame, UINT32 n) {
        if (n && !Api().inject(HSYNTHETICPOINTERDEVICE(touchDevice_), frame, n)) {
            static std::atomic<int> logged{0};
            if (logged++ < 5) Log("touch: Windows refused the input (error %lu)", GetLastError());
        }
    };
    const POINTER_FLAGS held = POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT;

    // Two frames when fingers both leave and arrive, so a frame never holds
    // more contacts than the device has: first the lifts, then the new ones.
    POINTER_TYPE_INFO frame[in::kMaxId + 1];
    UINT32 n = 0;
    bool lifts = false, arrivals = false;
    for (int id = 0; id <= in::kMaxId; ++id) {
        lifts |= touch_[id].present && !next[id].present;
        arrivals |= next[id].present && !touch_[id].present;
    }
    if (lifts) {
        for (int id = 0; id <= in::kMaxId; ++id) {
            if (!touch_[id].present) continue;
            if (next[id].present)
                frame[n++] = entry(id, next[id].pt, held | POINTER_FLAG_UPDATE);
            else
                frame[n++] = entry(id, touch_[id].pt, POINTER_FLAG_UP | (canceled ? POINTER_FLAG_CANCELED : 0));
        }
        inject(frame, n);
    }
    if (arrivals || !lifts) {
        n = 0;
        for (int id = 0; id <= in::kMaxId; ++id) {
            if (!next[id].present) continue;
            frame[n++] = entry(id, next[id].pt, held | (touch_[id].present ? POINTER_FLAG_UPDATE : POINTER_FLAG_DOWN));
        }
        inject(frame, n);
    }
    for (int id = 0; id <= in::kMaxId; ++id) touch_[id] = next[id];
    lastInject_ = Clock::now();
}

void TabletInput::InjectPen(const Pointer& next, bool canceled) {
    if (!next.present && !pen_.present) return;
    if (!penDevice_ && !failed_) {
        if (Api().Ok()) penDevice_ = Api().create(PT_PEN, 1, POINTER_FEEDBACK_DEFAULT);
        if (!penDevice_) {
            failed_ = true;
            Log("touch: no synthetic pen device (error %lu); the pen needs Windows 10 1809 or newer", GetLastError());
            return;
        }
    }
    if (!penDevice_) return;

    auto send = [&](const Pointer& p, POINTER_FLAGS flags) {
        POINTER_TYPE_INFO info{};
        info.type = PT_PEN;
        POINTER_PEN_INFO& pen = info.penInfo;
        pen.pointerInfo.pointerType = PT_PEN;
        pen.pointerInfo.pointerId = 1;
        pen.pointerInfo.ptPixelLocation = p.pt;
        pen.pointerInfo.pointerFlags = flags;
        if (p.state & in::kBarrel) pen.penFlags |= PEN_FLAG_BARREL;
        if (p.state & in::kEraser) pen.penFlags |= PEN_FLAG_INVERTED | (p.touching ? PEN_FLAG_ERASER : 0);
        pen.penMask = PEN_MASK_PRESSURE;
        // a touching pen without pressure (the tablet has none) still draws
        pen.pressure = p.touching ? std::max<UINT32>(p.pressure, p.pressure ? 1 : 512) : 0;
        if (!Api().inject(HSYNTHETICPOINTERDEVICE(penDevice_), &info, 1)) {
            static std::atomic<int> logged{0};
            if (logged++ < 5) Log("pen: Windows refused the input (error %lu)", GetLastError());
        }
    };
    const POINTER_FLAGS inRange = POINTER_FLAG_INRANGE;

    if (!next.present) {
        if (pen_.touching) send(pen_, POINTER_FLAG_UP | (canceled ? POINTER_FLAG_CANCELED : 0));
        else send(pen_, POINTER_FLAG_UPDATE);  // left the screen
    } else if (next.touching) {
        if (!pen_.present) send({true, false, next.state, next.pt, 0}, inRange | POINTER_FLAG_UPDATE);
        send(next, inRange | POINTER_FLAG_INCONTACT | (pen_.touching ? POINTER_FLAG_UPDATE : POINTER_FLAG_DOWN));
    } else {
        send(next, inRange | (pen_.touching ? POINTER_FLAG_UP : POINTER_FLAG_UPDATE));
    }
    pen_ = next;
    lastInject_ = Clock::now();
}

void TabletInput::ReleaseAll() {
    bool any = pen_.present;
    for (auto& p : touch_) any |= p.present;
    if (!any) return;
    proto::InputEvent cancel;
    cancel.flags = in::kCanceled;
    Apply(cancel);
}

}  // namespace sst
