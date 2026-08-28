#pragma once

#include "../netplay/NetplayProtocol.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ssb64::netplay::transport {

using SocketHandle = int;
constexpr SocketHandle kInvalidSocket = -1;

struct SocketAddress {
    uint32_t ipv4 = 0; // network byte order
    std::array<uint8_t, 6> mac{};
    uint16_t port = 0; // host byte order
    bool isAdhoc = false;
};

enum class ConnectResult {
    Connected,
    InProgress,
    Failed,
};

int LastError();
bool IsWouldBlock(int result);
bool IsConnectPendingError(int errorCode);

SocketAddress AnyAddress(uint16_t port);
SocketAddress BroadcastAddress(NetplayMode mode, uint16_t port);
bool ParseIpv4(const std::string& ip, uint16_t port, SocketAddress& out);
bool ParseEndpoint(NetplayMode mode, const std::string& endpoint, uint16_t port, SocketAddress& out);
std::string ToString(const SocketAddress& address);
std::vector<SocketAddress> EnumerateAdhocPeers(uint16_t port);

SocketHandle CreateDatagram(NetplayMode mode, const char* name, const std::string& localEndpoint,
                            uint16_t bindPort, bool allowBroadcast);
int SendDatagram(NetplayMode mode, SocketHandle socket, const void* data, std::size_t size,
                 const SocketAddress& address);
int RecvDatagram(NetplayMode mode, SocketHandle socket, void* data, std::size_t size,
                 SocketAddress& address);
void CloseDatagram(NetplayMode mode, SocketHandle& socket);

SocketHandle CreateReliableListener(NetplayMode mode, const char* name, const std::string& localEndpoint,
                                    uint16_t bindPort, int backlog);
ConnectResult CreateReliableClient(NetplayMode mode, const char* name, const SocketAddress& address,
                                   uint16_t localPort, SocketHandle& socket);
SocketHandle AcceptReliable(NetplayMode mode, SocketHandle listener, SocketAddress& peer);
int SendReliable(NetplayMode mode, SocketHandle socket, const void* data, std::size_t size);
int RecvReliable(NetplayMode mode, SocketHandle socket, void* data, std::size_t size);
void CloseReliable(NetplayMode mode, SocketHandle& socket);

bool GetSocketError(NetplayMode mode, SocketHandle socket, int& errorCode);
bool IsConnected(NetplayMode mode, SocketHandle socket);

} // namespace ssb64::netplay::transport
