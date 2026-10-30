#include "channel.h"

#include <cstring>

#include "log.h"
#include "net.h"

namespace sst {

void Channel::Secure(std::unique_ptr<crypto::SealKey> tx, std::unique_ptr<crypto::SealKey> rx) {
    tx_ = std::move(tx);
    rx_ = std::move(rx);
}

bool Channel::Write(proto::MsgType type, const void* part1, size_t size1, const void* part2, size_t size2) {
    if (!tx_) return net::WriteMessage(s_, type, part1, size1, part2, size2);
    plain_.resize(1 + size1 + size2);
    plain_[0] = uint8_t(type);
    if (size1) memcpy(plain_.data() + 1, part1, size1);
    if (size2) memcpy(plain_.data() + 1 + size1, part2, size2);
    if (!tx_->Seal(plain_.data(), plain_.size(), sealed_)) return false;
    return net::WriteMessage(s_, proto::MsgType::Sealed, sealed_.data(), sealed_.size());
}

bool Channel::Read(proto::MsgType& type, std::vector<uint8_t>& payload) {
    if (!net::ReadMessage(s_, type, payload)) return false;
    if (!rx_) return true;
    std::vector<uint8_t> plain;
    if (type != proto::MsgType::Sealed || !rx_->Open(payload.data(), payload.size(), plain) || plain.empty()) {
        Log("net: a message from the tablet didn't check out; closing");
        return false;
    }
    type = proto::MsgType(plain[0]);
    payload.assign(plain.begin() + 1, plain.end());
    return true;
}

}  // namespace sst
