#pragma once

#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

namespace sst {

// Talks to the ScreenFleet display driver: adds and removes virtual
// monitors and keeps them alive. If this process dies, the driver removes
// its monitors after a few seconds.
class VirtualDisplayDriver {
public:
    ~VirtualDisplayDriver();

    // False if the driver isn't installed (or is a different version).
    bool Open();
    bool IsOpen() const { return handle_ != INVALID_HANDLE_VALUE; }

    // Returns the monitor id, or 0 on failure.
    uint32_t AddMonitor(uint32_t width, uint32_t height, uint32_t refresh, uint32_t serial);
    void RemoveMonitor(uint32_t id);

private:
    bool Ioctl(DWORD code, const void* in, DWORD inSize, void* out, DWORD outSize);
    void KeepaliveLoop();

    HANDLE handle_ = INVALID_HANDLE_VALUE;
    uint64_t owner_ = 0;  // random per process: the driver times out each host's monitors on their own
    std::vector<uint32_t> monitors_;
    std::thread keepalive_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool stopping_ = false;
};

}  // namespace sst
