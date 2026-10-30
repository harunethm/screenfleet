#pragma once

#include <winsock2.h>
#include <ws2tcpip.h>

#include <cstdint>
#include <string>
#include <vector>

#include "sst/protocol.h"

namespace sst::net {

// RAII for WSAStartup/WSACleanup.
class WinsockInit {
public:
    WinsockInit();
    ~WinsockInit();
};

// Connects to host:port. Returns INVALID_SOCKET on failure.
SOCKET Connect(const std::string& host, uint16_t port);
// Same, giving up after timeoutMs (for addresses that may not answer).
SOCKET ConnectTimeout(const std::string& host, uint16_t port, int timeoutMs);

// Configures a connected socket for low-latency streaming.
void TuneSocket(SOCKET s);

void SetRecvTimeout(SOCKET s, DWORD ms);

bool SendAll(SOCKET s, const void* data, size_t size);
bool RecvAll(SOCKET s, void* data, size_t size);

// Sends header + part1 + part2 as one message. Not thread-safe: callers that
// send from several threads must hold a lock.
bool WriteMessage(SOCKET s, proto::MsgType type, const void* part1 = nullptr, size_t size1 = 0,
                  const void* part2 = nullptr, size_t size2 = 0);
bool ReadMessage(SOCKET s, proto::MsgType& type, std::vector<uint8_t>& payload);

}  // namespace sst::net
