#include "session.h"

#include <algorithm>
#include <array>
#include <chrono>

#include "converter.h"
#include "encoder.h"
#include "net.h"

namespace sst {

using Clock = std::chrono::steady_clock;

FileSink::FileSink(const std::string& path) {
    if (fopen_s(&file_, path.c_str(), "wb") != 0) file_ = nullptr;
}

FileSink::~FileSink() {
    if (file_) fclose(file_);
}

void FileSink::OnFrame(const uint8_t* data, size_t size, int64_t, bool) {
    if (file_) fwrite(data, 1, size, file_);
}

NetSink::NetSink(Channel& channel, StatsFn onStats, InputFn onInput)
    : channel_(channel), onStats_(std::move(onStats)), onInput_(std::move(onInput)) {
    receiver_ = std::thread([this] { ReceiveLoop(); });
}

NetSink::~NetSink() {
    ok_ = false;
    shutdown(channel_.Socket(), SD_BOTH);  // unblocks recv() in the receiver thread
    if (receiver_.joinable()) receiver_.join();
}

void NetSink::OnFrame(const uint8_t* data, size_t size, int64_t ptsUs, bool keyFrame) {
    if (!ok_) return;
    uint8_t header[proto::kVideoFrameHeaderSize];
    proto::EncodeVideoFrameHeader(keyFrame ? proto::frame_flags::kKeyFrame : 0, uint64_t(ptsUs), header);
    std::lock_guard lock(sendMutex_);
    if (!channel_.Write(proto::MsgType::VideoFrame, header, sizeof(header), data, size)) {
        Log("connection lost while sending");
        ok_ = false;
        return;
    }
    ++frames_;
    bytes_ += size;
    anyFrame_ = true;
}

void NetSink::SendBye() {
    std::lock_guard lock(sendMutex_);
    channel_.Write(proto::MsgType::Bye);
}

void NetSink::SendPing() {
    if (!ok_) return;
    proto::Writer w;
    w.U64(uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                       std::chrono::steady_clock::now().time_since_epoch())
                       .count()));
    std::lock_guard lock(sendMutex_);
    if (!channel_.Write(proto::MsgType::Ping, w.buf.data(), w.buf.size())) ok_ = false;
}

void NetSink::ReceiveLoop() {
    proto::MsgType type;
    std::vector<uint8_t> payload;
    while (ok_ && channel_.Read(type, payload)) {
        switch (type) {
        case proto::MsgType::RequestKeyFrame:
            keyFrameRequested_ = true;
            break;
        case proto::MsgType::Ping: {
            std::lock_guard lock(sendMutex_);
            channel_.Write(proto::MsgType::Pong, payload.data(), payload.size());
            break;
        }
        case proto::MsgType::Bye:
            Log("device said goodbye");
            byeReceived_ = true;
            ok_ = false;
            break;
        case proto::MsgType::DeviceStats: {
            proto::DeviceStats stats;
            if (onStats_ && proto::Decode(payload, stats)) onStats_(stats);
            break;
        }
        case proto::MsgType::InputEvent: {
            proto::InputEvent input;
            if (!proto::Decode(payload, input)) {
                // dropped; logged a few times, not once per message
                if (++badInput_ <= 3 || badInput_ % 1000 == 0) Log("dropped %u malformed input message(s)", badInput_);
                break;
            }
            if (onInput_) onInput_(input);
            break;
        }
        default:
            break;  // ignore unknown messages (forward compatibility)
        }
    }
    if (ok_) Log("connection closed by device");
    ok_ = false;
}

void RunSession(GpuContext& gpu, DesktopDuplicator& capture, const SessionConfig& config, VideoSink& sink,
                const std::atomic<bool>& quit) {
    VideoConverter converter;
    UINT srcWidth = capture.Width();
    UINT srcHeight = capture.Height();
    converter.Init(gpu.device.Get(), gpu.context.Get(), srcWidth, srcHeight, config.width, config.height);

    // The encoder may still be reading a texture while we fill the next one.
    std::array<ComPtr<ID3D11Texture2D>, 6> ring;
    for (auto& tex : ring) tex = converter.CreateOutputTexture();
    size_t ringIndex = 0;

    std::atomic<uint64_t> bytesOut{0};
    std::atomic<uint32_t> framesOut{0};
    H264Encoder encoder;
    H264Encoder::Config encConfig{config.width, config.height, config.fps, config.bitrate};
    encoder.Init(gpu.device.Get(), gpu.adapterLuid, encConfig,
                 [&](const uint8_t* data, size_t size, int64_t ptsUs, bool keyFrame) {
                     bytesOut += size;
                     ++framesOut;
                     sink.OnFrame(data, size, ptsUs, keyFrame);
                 });

    const auto interval = std::chrono::microseconds(1'000'000 / config.fps);
    const auto start = Clock::now();
    auto lastEncode = start - interval;
    auto lastStats = start;
    bool dirty = true;  // the capture texture already holds the current screen
    bool forceKeyFrame = true;
    // Repeats of the unchanged picture once the screen has been still for a
    // frame slot, a few ms apart: some decoders only show a frame once newer
    // ones arrive. While the screen keeps changing, real frames do that and no
    // repeats are sent (they would overload such a decoder). They don't move
    // the frame cadence, so a real change right after isn't held back.
    const auto trailingGap = std::chrono::milliseconds(3);
    int trailing = 0;
    bool trailingFrame = false;
    auto nextTrailing = start;

    while (!quit && sink.Ok()) {
        auto now = Clock::now();
        if (config.maxSeconds > 0 && now - start >= std::chrono::duration<double>(config.maxSeconds)) break;

        if (sink.TakeKeyFrameRequest()) {
            forceKeyFrame = true;
            dirty = true;
        }

        // Wait for a screen change, but no longer than until the next frame slot.
        auto untilSlotUs = std::chrono::duration_cast<std::chrono::microseconds>(lastEncode + interval - now).count();
        UINT timeoutMs = dirty ? UINT(std::max<int64_t>(0, (untilSlotUs + 999) / 1000))
                               : UINT(std::chrono::duration_cast<std::chrono::milliseconds>(interval).count());
        if (trailing > 0 && !dirty) {
            auto untilTrailing = std::chrono::duration_cast<std::chrono::milliseconds>(nextTrailing - now).count();
            timeoutMs = UINT(std::clamp<int64_t>(untilTrailing, 0, timeoutMs));
        }
        if (capture.Acquire(timeoutMs) == DesktopDuplicator::Result::NewFrame) {
            if (capture.Width() != srcWidth || capture.Height() != srcHeight) {
                srcWidth = capture.Width();
                srcHeight = capture.Height();
                converter.Init(gpu.device.Get(), gpu.context.Get(), srcWidth, srcHeight, config.width,
                               config.height);
            }
            dirty = true;
            trailingFrame = false;
        }
        if (config.constantRate) dirty = true;

        now = Clock::now();
        if (!dirty && trailing > 0 && now >= nextTrailing) {
            dirty = true;
            trailingFrame = true;
            --trailing;
        }
        if (dirty && (trailingFrame || now - lastEncode >= interval)) {
            ID3D11Texture2D* target = ring[ringIndex].Get();
            ringIndex = (ringIndex + 1) % ring.size();
            converter.Convert(capture.Texture(), target);
            int64_t ptsUs = std::chrono::duration_cast<std::chrono::microseconds>(now - start).count();
            if (!encoder.Encode(target, ptsUs, forceKeyFrame)) {
                Log("encoder stopped");
                break;
            }
            forceKeyFrame = false;
            dirty = false;
            if (trailingFrame) {
                trailingFrame = false;
                nextTrailing = now + trailingGap;
            } else {
                trailing = config.trailingFrames;
                nextTrailing = now + interval;
                // Keep a steady cadence; after an idle period, restart from now.
                lastEncode = (now - lastEncode < 2 * interval) ? lastEncode + interval : now;
            }
        }

        if (now - lastStats >= std::chrono::seconds(5)) {
            double secs = std::chrono::duration<double>(now - lastStats).count();
            Log("stats: %.1f fps, %.0f kbit/s", framesOut.exchange(0) / secs, bytesOut.exchange(0) * 8 / secs / 1000);
            lastStats = now;
        }
    }
    encoder.Shutdown();
}

}  // namespace sst
