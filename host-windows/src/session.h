#pragma once

#include <d3d11.h>
#include <winsock2.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "capture.h"
#include "channel.h"
#include "com_util.h"
#include "sst/protocol.h"

namespace sst {

// Where encoded frames go: the device over TCP, or a file for testing.
class VideoSink {
public:
    virtual ~VideoSink() = default;
    // Called on the encoder thread.
    virtual void OnFrame(const uint8_t* data, size_t size, int64_t ptsUs, bool keyFrame) = 0;
    virtual bool Ok() const = 0;
    // True once after the device asked for a key frame.
    virtual bool TakeKeyFrameRequest() { return false; }
};

class FileSink : public VideoSink {
public:
    explicit FileSink(const std::string& path);
    ~FileSink() override;
    void OnFrame(const uint8_t* data, size_t size, int64_t ptsUs, bool keyFrame) override;
    bool Ok() const override { return file_ != nullptr; }

private:
    FILE* file_ = nullptr;
};

// Sends frames to the device and answers its requests (key frame, ping).
class NetSink : public VideoSink {
public:
    using StatsFn = std::function<void(const proto::DeviceStats&)>;
    using InputFn = std::function<void(const proto::InputEvent&)>;

    explicit NetSink(Channel& channel, StatsFn onStats = nullptr, InputFn onInput = nullptr);
    ~NetSink() override;
    void OnFrame(const uint8_t* data, size_t size, int64_t ptsUs, bool keyFrame) override;
    bool Ok() const override { return ok_; }
    bool TakeKeyFrameRequest() override { return keyFrameRequested_.exchange(false); }

    void SendBye();
    // Heartbeat: frames only flow when the screen changes, so the tablet needs
    // this to tell a still screen from a PC that went away.
    void SendPing();
    bool ByeReceived() const { return byeReceived_; }
    // Frames and bytes sent since the last call.
    void TakeCounters(uint32_t& frames, uint64_t& bytes) {
        frames = frames_.exchange(0);
        bytes = bytes_.exchange(0);
    }
    bool AnyFrameSent() const { return anyFrame_; }

private:
    void ReceiveLoop();

    Channel& channel_;
    StatsFn onStats_;
    InputFn onInput_;
    uint32_t badInput_ = 0;  // malformed InputEvents (receiver thread only)
    std::mutex sendMutex_;
    std::atomic<bool> ok_{true};
    std::atomic<bool> keyFrameRequested_{false};
    std::atomic<bool> byeReceived_{false};
    std::atomic<bool> anyFrame_{false};
    std::atomic<uint32_t> frames_{0};
    std::atomic<uint64_t> bytes_{0};
    std::thread receiver_;
};

struct GpuContext {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    LUID adapterLuid{};
};

struct SessionConfig {
    UINT width = 1280;
    UINT height = 800;
    UINT fps = 60;
    UINT bitrate = 8'000'000;
    bool constantRate = false;  // encode every interval even if nothing changed (testing)
    int trailingFrames = 0;     // repeats after each change, for decoders that hold frames back
    double maxSeconds = 0;      // 0 = until the sink fails or quit is set
};

// Captures, converts, encodes and delivers frames until the sink fails, the
// time limit passes or `quit` becomes true.
void RunSession(GpuContext& gpu, DesktopDuplicator& capture, const SessionConfig& config, VideoSink& sink,
                const std::atomic<bool>& quit);

}  // namespace sst
