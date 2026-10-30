// ScreenFleet wire protocol (see protocol/PROTOCOL.md).
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace sst::proto {

constexpr uint32_t kMagic = 0x53535442;  // "SSTB"
constexpr uint16_t kVersion = 1;
constexpr uint16_t kDefaultPort = 27183;
constexpr size_t kHeaderSize = 5;  // u8 type + u32 length
constexpr uint32_t kMaxPayload = 8 * 1024 * 1024;

enum class MsgType : uint8_t {
    Hello = 1,
    DeviceInfo = 2,
    StreamStart = 3,
    VideoFrame = 4,
    RequestKeyFrame = 5,
    Ping = 6,
    Pong = 7,
    Bye = 8,
    DeviceStats = 9,
    // pairing and encrypted sessions over Wi-Fi (PROTOCOL.md)
    PairStart = 10,
    PairReply = 11,
    PairReveal = 12,
    PairConfirm = 13,
    PairDone = 14,
    NotPaired = 15,
    SessionReply = 16,
    SessionConfirm = 17,
    Sealed = 18,
    InputEvent = 19,  // touch and pen, device -> host
};

namespace hello_flags {
constexpr uint8_t kSecure = 0x01;  // the host wants an encrypted session (Wi-Fi)
}  // namespace hello_flags

enum class Codec : uint8_t { H264 = 1 };

namespace frame_flags {
constexpr uint8_t kKeyFrame = 0x01;
constexpr uint8_t kConfig = 0x02;
}  // namespace frame_flags

namespace stream_flags {
constexpr uint8_t kExtend = 0x01;  // the tablet is a new monitor (else it mirrors one)
}  // namespace stream_flags

namespace device_flags {
constexpr uint32_t kTouch = 0x01;
constexpr uint32_t kPen = 0x02;
constexpr uint32_t kFlushFrames = 0x04;  // the decoder holds frames: send a few repeats after each change
}  // namespace device_flags

// DeviceInfo / discovery value for "the user never asked to connect".
constexpr uint32_t kNoConnectRequest = 0xFFFFFFFF;

// Discovery over UDP on kDefaultPort (see PROTOCOL.md).
constexpr uint32_t kDiscoveryQuery = 0x53535451;  // "SSTQ"
constexpr uint32_t kDiscoveryReply = 0x53535452;  // "SSTR"
constexpr size_t kDiscoveryQuerySize = 256;       // replies are never larger than this
namespace discovery_flags {
constexpr uint8_t kBusy = 0x01;
}  // namespace discovery_flags

// Size of the fixed VideoFrame header that precedes the bitstream data.
constexpr size_t kVideoFrameHeaderSize = 1 + 8;

class Writer {
public:
    void U8(uint8_t v) { buf.push_back(v); }
    void U16(uint16_t v) { U8(uint8_t(v >> 8)); U8(uint8_t(v)); }
    void U32(uint32_t v) { U16(uint16_t(v >> 16)); U16(uint16_t(v)); }
    void U64(uint64_t v) { U32(uint32_t(v >> 32)); U32(uint32_t(v)); }
    void Str(const std::string& s) {
        U16(uint16_t(s.size()));
        buf.insert(buf.end(), s.begin(), s.end());
    }
    void Raw(const uint8_t* p, size_t n) { buf.insert(buf.end(), p, p + n); }

    std::vector<uint8_t> buf;
};

// Reads big-endian values; on underflow returns zeros and Ok() becomes false.
class Reader {
public:
    Reader(const uint8_t* data, size_t size) : p_(data), n_(size) {}
    explicit Reader(const std::vector<uint8_t>& v) : Reader(v.data(), v.size()) {}

    uint8_t U8() { return Need(1) ? p_[pos_++] : 0; }
    uint16_t U16() { uint16_t hi = U8(); return uint16_t((hi << 8) | U8()); }
    uint32_t U32() { uint32_t hi = U16(); return (hi << 16) | U16(); }
    uint64_t U64() { uint64_t hi = U32(); return (hi << 32) | U32(); }
    std::string Str() {
        uint16_t len = U16();
        if (!Need(len)) return {};
        std::string s(reinterpret_cast<const char*>(p_ + pos_), len);
        pos_ += len;
        return s;
    }
    void Raw(uint8_t* out, size_t n) {
        if (!Need(n)) return;
        std::copy(p_ + pos_, p_ + pos_ + n, out);
        pos_ += n;
    }

    bool Ok() const { return ok_; }
    size_t Remaining() const { return n_ - pos_; }
    const uint8_t* Current() const { return p_ + pos_; }

private:
    bool Need(size_t count) {
        if (!ok_ || n_ - pos_ < count) {
            ok_ = false;
            return false;
        }
        return true;
    }

    const uint8_t* p_;
    size_t n_;
    size_t pos_ = 0;
    bool ok_ = true;
};

struct Hello {
    uint32_t magic = kMagic;
    uint16_t version = kVersion;
    std::string hostName;  // shown on the tablet ("Connected to DESKTOP-PC")
    // appended for encrypted sessions (Wi-Fi)
    uint8_t flags = 0;     // hello_flags
    std::string hostId;
    uint8_t epk[65] = {};  // per-connection ECDH key
    uint8_t nonce[32] = {};
};

struct DeviceInfo {
    uint16_t version = kVersion;
    uint16_t width = 0;
    uint16_t height = 0;
    uint16_t densityDpi = 0;
    uint16_t sdkInt = 0;
    uint16_t maxFps = 60;
    uint32_t flags = 0;
    std::string model;
    // appended after v1's first release; absent from older devices
    uint32_t connectRequestAge = kNoConnectRequest;  // seconds since the user tapped Connect on the device
    std::string deviceId;                            // random per install
};

struct StreamStart {
    Codec codec = Codec::H264;
    uint16_t width = 0;
    uint16_t height = 0;
    uint16_t fps = 60;
    uint32_t bitrate = 0;
    uint8_t flags = 0;  // stream_flags
};

inline std::vector<uint8_t> Encode(const Hello& m) {
    Writer w;
    w.U32(m.magic);
    w.U16(m.version);
    w.Str(m.hostName);
    if (m.flags & hello_flags::kSecure) {
        w.U8(m.flags);
        w.Str(m.hostId);
        w.Raw(m.epk, sizeof(m.epk));
        w.Raw(m.nonce, sizeof(m.nonce));
    }
    return w.buf;
}

inline bool Decode(const std::vector<uint8_t>& p, Hello& m) {
    Reader r(p);
    m.magic = r.U32();
    m.version = r.U16();
    bool ok = r.Ok();
    if (r.Remaining()) m.hostName = r.Str();  // appended after v1's first release
    return ok;
}

inline std::vector<uint8_t> Encode(const DeviceInfo& m) {
    Writer w;
    w.U16(m.version);
    w.U16(m.width);
    w.U16(m.height);
    w.U16(m.densityDpi);
    w.U16(m.sdkInt);
    w.U16(m.maxFps);
    w.U32(m.flags);
    w.Str(m.model);
    w.U32(m.connectRequestAge);
    w.Str(m.deviceId);
    return w.buf;
}

inline bool Decode(const std::vector<uint8_t>& p, DeviceInfo& m) {
    Reader r(p);
    m.version = r.U16();
    m.width = r.U16();
    m.height = r.U16();
    m.densityDpi = r.U16();
    m.sdkInt = r.U16();
    m.maxFps = r.U16();
    m.flags = r.U32();
    m.model = r.Str();
    bool ok = r.Ok();
    if (ok && r.Remaining() >= 4) {
        m.connectRequestAge = r.U32();
        if (r.Remaining()) m.deviceId = r.Str();
        if (!r.Ok()) m.deviceId.clear();
    }
    return ok;
}

inline std::vector<uint8_t> Encode(const StreamStart& m) {
    Writer w;
    w.U8(uint8_t(m.codec));
    w.U16(m.width);
    w.U16(m.height);
    w.U16(m.fps);
    w.U32(m.bitrate);
    w.U8(m.flags);
    return w.buf;
}

inline bool Decode(const std::vector<uint8_t>& p, StreamStart& m) {
    Reader r(p);
    m.codec = Codec(r.U8());
    m.width = r.U16();
    m.height = r.U16();
    m.fps = r.U16();
    m.bitrate = r.U32();
    bool ok = r.Ok();
    if (r.Remaining()) m.flags = r.U8();
    return ok;
}

// Sent by the device about once a second while streaming.
struct DeviceStats {
    uint32_t decodeUs = 0;  // average time from queueing a frame to it being decoded; 0 = unknown
    uint16_t framesShown = 0;  // frames rendered in the last second
};

inline bool Decode(const std::vector<uint8_t>& p, DeviceStats& m) {
    Reader r(p);
    m.decodeUs = r.U32();
    m.framesShown = r.U16();
    return r.Ok();
}

// Touch and pen: a snapshot of every pointer on the tablet (PROTOCOL.md).
namespace input {
constexpr size_t kMaxPointers = 10;
constexpr uint8_t kMaxId = 31;
constexpr uint16_t kMaxPressure = 1024;
constexpr uint8_t kCanceled = 0x01;  // InputEvent flags
enum class Tool : uint8_t { Finger = 1, Pen = 2 };
constexpr uint8_t kTouching = 0x01;  // Pointer state
constexpr uint8_t kBarrel = 0x02;
constexpr uint8_t kEraser = 0x04;
}  // namespace input

struct InputPointer {
    uint8_t id = 0;
    input::Tool tool = input::Tool::Finger;
    uint8_t state = 0;
    uint16_t x = 0, y = 0;  // 0..65535 across the picture
    uint16_t pressure = 0;  // pen: 0..1024
};

struct InputEvent {
    uint8_t flags = 0;
    uint8_t count = 0;
    InputPointer pointers[input::kMaxPointers];
};

// Strict: anything out of the rules in PROTOCOL.md makes the whole message invalid.
inline bool Decode(const std::vector<uint8_t>& p, InputEvent& m) {
    Reader r(p);
    m.flags = r.U8();
    m.count = r.U8();
    if (!r.Ok() || m.count > input::kMaxPointers) return false;
    if ((m.flags & input::kCanceled) && m.count) return false;
    uint32_t ids = 0;
    int pens = 0;
    for (uint8_t i = 0; i < m.count; ++i) {
        InputPointer& q = m.pointers[i];
        q.id = r.U8();
        uint8_t tool = r.U8();
        q.state = r.U8();
        q.x = r.U16();
        q.y = r.U16();
        q.pressure = r.U16();
        if (!r.Ok() || q.id > input::kMaxId || (ids & (1u << q.id))) return false;
        ids |= 1u << q.id;
        if (tool == uint8_t(input::Tool::Finger)) {
            if (!(q.state & input::kTouching)) return false;  // fingers can't hover
            q.state = input::kTouching;
            q.pressure = 0;
        } else if (tool == uint8_t(input::Tool::Pen)) {
            if (++pens > 1) return false;
            q.state &= input::kTouching | input::kBarrel | input::kEraser;
            q.pressure = std::min(q.pressure, input::kMaxPressure);
        } else {
            return false;
        }
        q.tool = input::Tool(tool);
    }
    return true;
}

inline std::vector<uint8_t> Encode(const InputEvent& m) {
    Writer w;
    w.U8(m.flags);
    w.U8(m.count);
    for (uint8_t i = 0; i < m.count && i < input::kMaxPointers; ++i) {
        const InputPointer& q = m.pointers[i];
        w.U8(q.id);
        w.U8(uint8_t(q.tool));
        w.U8(q.state);
        w.U16(q.x);
        w.U16(q.y);
        w.U16(q.pressure);
    }
    return w.buf;
}

// A device's answer to the discovery query.
struct DiscoveryReply {
    uint16_t version = 0;
    uint16_t tcpPort = 0;
    uint8_t flags = 0;  // discovery_flags
    uint16_t width = 0;
    uint16_t height = 0;
    uint16_t sdkInt = 0;
    uint32_t connectRequestAge = kNoConnectRequest;
    std::string model;
    std::string deviceId;
    std::string busyWith;  // the host it streams to
};

inline std::vector<uint8_t> EncodeDiscoveryQuery() {
    Writer w;
    w.U32(kDiscoveryQuery);
    w.U16(kVersion);
    w.buf.resize(kDiscoveryQuerySize, 0);
    return w.buf;
}

inline bool Decode(const uint8_t* data, size_t size, DiscoveryReply& m) {
    Reader r(data, size);
    if (r.U32() != kDiscoveryReply) return false;
    m.version = r.U16();
    m.tcpPort = r.U16();
    m.flags = r.U8();
    m.width = r.U16();
    m.height = r.U16();
    m.sdkInt = r.U16();
    m.connectRequestAge = r.U32();
    m.model = r.Str();
    m.deviceId = r.Str();
    m.busyWith = r.Str();
    return r.Ok();
}

inline void EncodeVideoFrameHeader(uint8_t flags, uint64_t ptsUs, uint8_t out[kVideoFrameHeaderSize]) {
    out[0] = flags;
    for (int i = 0; i < 8; ++i) out[1 + i] = uint8_t(ptsUs >> (56 - 8 * i));
}

}  // namespace sst::proto
