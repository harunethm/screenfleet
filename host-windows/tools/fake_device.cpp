// sst-fake-device: pretends to be the Android app. Listens like the app does,
// answers the handshake, saves the received video to a file and prints
// statistics, so the host can be tested without a tablet.
#include <winsock2.h>
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "log.h"
#include "net.h"

using namespace sst;
using Clock = std::chrono::steady_clock;

namespace {

uint64_t NowUs() {
    return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count());
}

void ServeOne(SOCKET s, const proto::DeviceInfo& info, const std::string& outPath, int keyEvery) {
    proto::MsgType type;
    std::vector<uint8_t> payload;
    proto::Hello hello;
    if (!net::ReadMessage(s, type, payload) || type != proto::MsgType::Hello || !proto::Decode(payload, hello) ||
        hello.magic != proto::kMagic) {
        Log("bad hello");
        return;
    }
    Log("host says hello, protocol v%u", hello.version);
    auto infoBytes = proto::Encode(info);
    net::WriteMessage(s, proto::MsgType::DeviceInfo, infoBytes.data(), infoBytes.size());

    proto::StreamStart start;
    if (!net::ReadMessage(s, type, payload) || type != proto::MsgType::StreamStart || !proto::Decode(payload, start)) {
        Log("bad stream start");
        return;
    }
    Log("stream: codec %u, %ux%u @ %u fps, %u kbit/s", unsigned(start.codec), start.width, start.height, start.fps,
        start.bitrate / 1000);

    FILE* out = nullptr;
    if (!outPath.empty() && fopen_s(&out, outPath.c_str(), "wb") != 0) out = nullptr;

    std::mutex sendMutex;
    std::atomic<bool> running{true};
    // Pings measure the round trip; key frame requests exercise that path.
    std::thread pinger([&] {
        int tick = 0;
        while (running) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            if (!running) break;
            ++tick;
            proto::Writer w;
            w.U64(NowUs());
            std::lock_guard lock(sendMutex);
            if (!net::WriteMessage(s, proto::MsgType::Ping, w.buf.data(), w.buf.size())) break;
            if (keyEvery > 0 && tick % keyEvery == 0) net::WriteMessage(s, proto::MsgType::RequestKeyFrame);
        }
    });

    uint64_t frames = 0, keyFrames = 0, bytes = 0, totalFrames = 0;
    auto lastStats = Clock::now();
    double lastRttMs = -1;
    while (net::ReadMessage(s, type, payload)) {
        if (type == proto::MsgType::VideoFrame) {
            proto::Reader r(payload);
            uint8_t flags = r.U8();
            r.U64();  // pts
            if (!r.Ok()) break;
            if (totalFrames == 0 && !(flags & proto::frame_flags::kKeyFrame)) Log("warning: first frame is not a key frame");
            if (out) fwrite(r.Current(), 1, r.Remaining(), out);
            ++frames;
            ++totalFrames;
            bytes += r.Remaining();
            if (flags & proto::frame_flags::kKeyFrame) ++keyFrames;
        } else if (type == proto::MsgType::Pong) {
            proto::Reader r(payload);
            lastRttMs = double(NowUs() - r.U64()) / 1000.0;
        } else if (type == proto::MsgType::Bye) {
            Log("host said goodbye");
            break;
        }
        auto now = Clock::now();
        double secs = std::chrono::duration<double>(now - lastStats).count();
        if (secs >= 2) {
            Log("received %.1f fps, %.0f kbit/s, %llu key frames, rtt %.2f ms", frames / secs, bytes * 8 / secs / 1000,
                keyFrames, lastRttMs);
            frames = keyFrames = bytes = 0;
            lastStats = now;
        }
    }
    running = false;
    shutdown(s, SD_BOTH);
    pinger.join();
    if (out) fclose(out);
    Log("connection closed after %llu frames", totalFrames);
}

}  // namespace

int main(int argc, char** argv) {
    proto::DeviceInfo info;
    info.width = 1280;
    info.height = 800;
    info.densityDpi = 160;
    info.sdkInt = 19;
    info.maxFps = 60;
    info.flags = proto::device_flags::kTouch | proto::device_flags::kPen;
    info.model = "fake-device";
    uint16_t port = proto::kDefaultPort;
    std::string outPath;
    int keyEvery = 0;
    bool once = false;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--size") {
            std::string v = next();
            auto x = v.find('x');
            info.width = uint16_t(std::stoi(v.substr(0, x)));
            info.height = uint16_t(std::stoi(v.substr(x + 1)));
        } else if (a == "--port") port = uint16_t(std::stoi(next()));
        else if (a == "--out") outPath = next();
        else if (a == "--max-fps") info.maxFps = uint16_t(std::stoi(next()));
        else if (a == "--key-every") keyEvery = std::stoi(next());
        else if (a == "--once") once = true;
        else if (a == "--name") info.model = next();
        else if (a == "--id") info.deviceId = next();
        else {
            std::fprintf(stderr,
                         "Usage: sst-fake-device [--size WxH] [--port N] [--out FILE.h264] [--max-fps N]\n"
                         "                       [--key-every SECONDS] [--once] [--name NAME] [--id ID]\n");
            return 2;
        }
    }

    net::WinsockInit winsock;
    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    if (bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || listen(listener, 1) != 0) {
        Log("cannot listen on 127.0.0.1:%u", port);
        return 1;
    }
    Log("fake device %ux%u listening on 127.0.0.1:%u", info.width, info.height, port);
    do {
        SOCKET s = accept(listener, nullptr, nullptr);
        if (s == INVALID_SOCKET) break;
        net::TuneSocket(s);
        Log("host connected");
        ServeOne(s, info, outPath, keyEvery);
        closesocket(s);
    } while (!once);
    closesocket(listener);
    return 0;
}
