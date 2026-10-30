#include "net.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

#include "log.h"

namespace sst::net {

WinsockInit::WinsockInit() {
    WSADATA data;
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) throw std::runtime_error("WSAStartup failed");
}

WinsockInit::~WinsockInit() { WSACleanup(); }

SOCKET Connect(const std::string& host, uint16_t port) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* result = nullptr;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &result) != 0) return INVALID_SOCKET;

    SOCKET s = INVALID_SOCKET;
    for (addrinfo* ai = result; ai; ai = ai->ai_next) {
        s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s == INVALID_SOCKET) continue;
        if (connect(s, ai->ai_addr, int(ai->ai_addrlen)) == 0) break;
        closesocket(s);
        s = INVALID_SOCKET;
    }
    freeaddrinfo(result);
    if (s != INVALID_SOCKET) TuneSocket(s);
    return s;
}

SOCKET ConnectTimeout(const std::string& host, uint16_t port, int timeoutMs) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* result = nullptr;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &result) != 0) return INVALID_SOCKET;
    SOCKET s = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    if (s != INVALID_SOCKET) {
        u_long nonBlocking = 1;
        ioctlsocket(s, FIONBIO, &nonBlocking);
        connect(s, result->ai_addr, int(result->ai_addrlen));
        fd_set writable, failed;
        FD_ZERO(&writable);
        FD_ZERO(&failed);
        FD_SET(s, &writable);
        FD_SET(s, &failed);
        timeval tv{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
        bool ok = select(0, nullptr, &writable, &failed, &tv) == 1 && FD_ISSET(s, &writable);
        nonBlocking = 0;
        ioctlsocket(s, FIONBIO, &nonBlocking);
        if (!ok) {
            closesocket(s);
            s = INVALID_SOCKET;
        }
    }
    freeaddrinfo(result);
    if (s != INVALID_SOCKET) TuneSocket(s);
    return s;
}

void TuneSocket(SOCKET s) {
    BOOL noDelay = TRUE;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));
    // A small send buffer keeps frames from piling up (and adding latency)
    // when the link is slower than the encoder.
    int sendBuf = 256 * 1024;
    setsockopt(s, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&sendBuf), sizeof(sendBuf));
    // A peer that stops reading must not block the sender forever: the send
    // fails after 5 s and the session ends.
    DWORD sendTimeout = 5000;
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&sendTimeout), sizeof(sendTimeout));
}

void SetRecvTimeout(SOCKET s, DWORD ms) {
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&ms), sizeof(ms));
}

bool SendAll(SOCKET s, const void* data, size_t size) {
    auto* p = static_cast<const char*>(data);
    while (size > 0) {
        int n = send(s, p, int(std::min<size_t>(size, 1 << 20)), 0);
        if (n <= 0) return false;
        p += n;
        size -= size_t(n);
    }
    return true;
}

bool RecvAll(SOCKET s, void* data, size_t size) {
    auto* p = static_cast<char*>(data);
    while (size > 0) {
        int n = recv(s, p, int(std::min<size_t>(size, 1 << 20)), 0);
        if (n <= 0) return false;
        p += n;
        size -= size_t(n);
    }
    return true;
}

bool WriteMessage(SOCKET s, proto::MsgType type, const void* part1, size_t size1, const void* part2,
                  size_t size2) {
    size_t length = size1 + size2;
    if (length > proto::kMaxPayload) return false;

    // Header and small first part go out in one send() call.
    uint8_t head[proto::kHeaderSize + 64];
    head[0] = uint8_t(type);
    head[1] = uint8_t(length >> 24);
    head[2] = uint8_t(length >> 16);
    head[3] = uint8_t(length >> 8);
    head[4] = uint8_t(length);
    size_t headSize = proto::kHeaderSize;
    if (size1 <= sizeof(head) - proto::kHeaderSize) {
        if (size1) memcpy(head + headSize, part1, size1);
        headSize += size1;
        size1 = 0;
    }
    if (!SendAll(s, head, headSize)) return false;
    if (size1 && !SendAll(s, part1, size1)) return false;
    if (size2 && !SendAll(s, part2, size2)) return false;
    return true;
}

bool ReadMessage(SOCKET s, proto::MsgType& type, std::vector<uint8_t>& payload) {
    uint8_t head[proto::kHeaderSize];
    if (!RecvAll(s, head, sizeof(head))) return false;
    uint32_t length = (uint32_t(head[1]) << 24) | (uint32_t(head[2]) << 16) | (uint32_t(head[3]) << 8) | head[4];
    if (length > proto::kMaxPayload) {
        Log("net: message too large (%u bytes)", length);
        return false;
    }
    type = proto::MsgType(head[0]);
    payload.resize(length);
    return length == 0 || RecvAll(s, payload.data(), length);
}

}  // namespace sst::net
