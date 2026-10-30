#pragma once

#include <winsock2.h>

#include <cstdint>
#include <memory>
#include <vector>

#include "crypto.h"
#include "sst/protocol.h"

namespace sst {

// One connection to a tablet. Plain until Secure() is called (encrypted
// Wi-Fi sessions); from then on every message goes out and comes in as
// Sealed. Reading and writing may happen on two threads at once; writes from
// several threads need the caller's lock, as with net::WriteMessage.
class Channel {
public:
    explicit Channel(SOCKET s) : s_(s) {}

    SOCKET Socket() const { return s_; }
    bool IsSecure() const { return tx_ != nullptr; }
    void Secure(std::unique_ptr<crypto::SealKey> tx, std::unique_ptr<crypto::SealKey> rx);

    bool Write(proto::MsgType type, const void* part1 = nullptr, size_t size1 = 0, const void* part2 = nullptr,
               size_t size2 = 0);
    bool Read(proto::MsgType& type, std::vector<uint8_t>& payload);

private:
    SOCKET s_;
    std::unique_ptr<crypto::SealKey> tx_, rx_;
    std::vector<uint8_t> plain_, sealed_;  // write side only
};

}  // namespace sst
