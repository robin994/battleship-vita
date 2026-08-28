#include "VitaNetworkTransport.h"

#include "../port_log.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#ifdef __vita__
#include <psp2/pspnet_adhoc.h>
#include <psp2/pspnet_adhocctl.h>
#endif

namespace ssb64::netplay::transport {
namespace {

int sLastAdhocError = 0;
#ifdef __vita__
int sLastPeerListErrorLogged = 0;
#endif

bool IsAdhoc(NetplayMode mode) {
    return mode == NetplayMode::LocalAdhoc;
}

sockaddr_in ToNative(const SocketAddress& address) {
    sockaddr_in native{};
#ifdef __vita__
    // Match SceNet's native sockaddr size. vitaQuake avoids INADDR_ANY and
    // binds its LAN sockets to the address returned by sceNetCtl instead.
    native.sin_len = sizeof(native);
    native.sin_vport = 0;
#endif
    native.sin_family = AF_INET;
    native.sin_port = htons(address.port);
    native.sin_addr.s_addr = address.ipv4;
    return native;
}

bool ResolveBindAddress(const std::string& bindIp, uint16_t port, SocketAddress& out) {
    if (bindIp.empty() || bindIp == "0.0.0.0") {
        out = AnyAddress(port);
        return true;
    }
    return ParseIpv4(bindIp, port, out);
}

SocketAddress FromNative(const sockaddr_in& native) {
    SocketAddress address{};
    address.ipv4 = native.sin_addr.s_addr;
    address.port = ntohs(native.sin_port);
    address.isAdhoc = false;
    return address;
}

bool SetNonBlocking(SocketHandle socket) {
    const int flags = fcntl(socket, F_GETFL, 0);
    return flags >= 0 && fcntl(socket, F_SETFL, flags | O_NONBLOCK) == 0;
}

bool SetReuseAddress(SocketHandle socket) {
    int enabled = 1;
    return setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled)) == 0;
}

#ifdef __vita__
bool GetLocalAdhocMac(SceNetEtherAddr& mac) {
    const int result = sceNetAdhocctlGetEtherAddr(&mac);
    if (result < 0) {
        sLastAdhocError = result;
        return false;
    }
    return true;
}

SceNetEtherAddr ToAdhocMac(const SocketAddress& address) {
    SceNetEtherAddr mac{};
    std::memcpy(mac.data, address.mac.data(), address.mac.size());
    return mac;
}

SocketAddress FromAdhoc(const SceNetEtherAddr& mac, uint16_t port) {
    SocketAddress address{};
    std::memcpy(address.mac.data(), mac.data, address.mac.size());
    address.port = port;
    address.isAdhoc = true;
    return address;
}

bool QueryPtpState(SocketHandle socket, int& state) {
    std::array<SceNetAdhocPtpStat, 20> stats{};
    int length = static_cast<int>(sizeof(stats));
    const int result = sceNetAdhocGetPtpStat(&length, stats.data());
    if (result < 0) {
        sLastAdhocError = result;
        return false;
    }
    const auto* stat = stats.data();
    while (stat != nullptr) {
        if (stat->id == socket) {
            state = stat->state;
            sLastAdhocError = 0;
            return true;
        }
        stat = stat->next;
    }
    sLastAdhocError = SCE_ERROR_NET_ADHOC_INVALID_SOCKET_ID;
    return false;
}

bool QueryPdpLocalPort(SocketHandle socket, uint16_t& port) {
    std::array<SceNetAdhocPdpStat, 20> stats{};
    int length = static_cast<int>(sizeof(stats));
    const int result = sceNetAdhocGetPdpStat(&length, stats.data());
    if (result < 0) {
        sLastAdhocError = result;
        return false;
    }

    const auto* stat = stats.data();
    while (stat != nullptr) {
        if (stat->id == socket) {
            // PSPNet AdHoc reports PDP stat ports in network byte order, the
            // same representation vitaQuake stores in sockaddr_adhoc.
            port = sceNetNtohs(stat->lport);
            sLastAdhocError = 0;
            return true;
        }
        stat = stat->next;
    }

    sLastAdhocError = SCE_ERROR_NET_ADHOC_INVALID_SOCKET_ID;
    return false;
}
#endif

} // namespace

int LastError() {
    return sLastAdhocError != 0 ? sLastAdhocError : errno;
}

bool IsWouldBlock(int result) {
#ifdef __vita__
    if (result == SCE_ERROR_NET_ADHOC_WOULD_BLOCK ||
        sLastAdhocError == SCE_ERROR_NET_ADHOC_WOULD_BLOCK) {
        return true;
    }
#else
    (void)result;
#endif
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINPROGRESS || errno == EALREADY;
}

bool IsConnectPendingError(int errorCode) {
#ifdef __vita__
    if (errorCode == SCE_ERROR_NET_ADHOC_WOULD_BLOCK || errorCode == SCE_ERROR_NET_ADHOC_BUSY) {
        return true;
    }
#endif
    return errorCode == EINPROGRESS || errorCode == EALREADY ||
           errorCode == EAGAIN || errorCode == EWOULDBLOCK;
}

SocketAddress AnyAddress(uint16_t port) {
    SocketAddress address{};
    address.ipv4 = htonl(INADDR_ANY);
    address.port = port;
    return address;
}

SocketAddress BroadcastAddress(NetplayMode mode, uint16_t port) {
    SocketAddress address{};
    address.port = port;
#ifndef __vita__
    // Desktop tests emulate the AdHoc session over localhost/BSD sockets.
    if (IsAdhoc(mode)) {
        address.ipv4 = htonl(INADDR_BROADCAST);
        return address;
    }
#endif
    address.isAdhoc = IsAdhoc(mode);
    if (address.isAdhoc) {
        address.mac.fill(0xFF);
    } else {
        address.ipv4 = htonl(INADDR_BROADCAST);
    }
    return address;
}

bool ParseIpv4(const std::string& ip, uint16_t port, SocketAddress& out) {
    in_addr native{};
    if (inet_pton(AF_INET, ip.c_str(), &native) != 1) {
        return false;
    }
    out.ipv4 = native.s_addr;
    out.port = port;
    out.isAdhoc = false;
    return true;
}

bool ParseEndpoint(NetplayMode mode, const std::string& endpoint, uint16_t port, SocketAddress& out) {
#ifndef __vita__
    if (IsAdhoc(mode)) return ParseIpv4(endpoint, port, out);
#endif
    if (!IsAdhoc(mode)) return ParseIpv4(endpoint, port, out);

    unsigned int octets[6]{};
    if (std::sscanf(endpoint.c_str(), "%2x:%2x:%2x:%2x:%2x:%2x",
                    &octets[0], &octets[1], &octets[2], &octets[3], &octets[4], &octets[5]) != 6) {
        return false;
    }
    out = SocketAddress{};
    for (std::size_t i = 0; i < out.mac.size(); ++i) {
        if (octets[i] > 0xFFU) return false;
        out.mac[i] = static_cast<uint8_t>(octets[i]);
    }
    out.port = port;
    out.isAdhoc = true;
    return true;
}

std::string ToString(const SocketAddress& address) {
    if (address.isAdhoc) {
        char text[18]{};
        std::snprintf(text, sizeof(text), "%02X:%02X:%02X:%02X:%02X:%02X",
                      address.mac[0], address.mac[1], address.mac[2],
                      address.mac[3], address.mac[4], address.mac[5]);
        return text;
    }
    char buffer[64]{};
    if (inet_ntop(AF_INET, &address.ipv4, buffer, sizeof(buffer)) == nullptr) {
        return {};
    }
    return buffer;
}

std::vector<SocketAddress> EnumerateAdhocPeers(uint16_t port) {
    std::vector<SocketAddress> result;
#ifdef __vita__
    std::array<SceNetAdhocctlPeerInfo, 16> peers{};
    int bytes = static_cast<int>(sizeof(peers));
    const int rc = sceNetAdhocctlGetPeerList(&bytes, peers.data());
    if (rc < 0) {
        sLastAdhocError = rc;
        if (rc != sLastPeerListErrorLogged) {
            port_log("[NETPLAY] AdHoc peer list query failed error=0x%08X\n",
                     static_cast<unsigned int>(rc));
            sLastPeerListErrorLogged = rc;
        }
        return result;
    }
    sLastPeerListErrorLogged = 0;

    SceNetEtherAddr localMac{};
    const bool haveLocalMac = GetLocalAdhocMac(localMac);
    const std::size_t count = std::min<std::size_t>(
        peers.size(), bytes > 0 ? static_cast<std::size_t>(bytes) / sizeof(SceNetAdhocctlPeerInfo) : 0U);
    result.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const SceNetEtherAddr& mac = peers[i].macAddr;
        if (haveLocalMac && std::memcmp(mac.data, localMac.data, sizeof(mac.data)) == 0) continue;

        bool allZero = true;
        for (uint8_t octet : mac.data) {
            if (octet != 0) {
                allZero = false;
                break;
            }
        }
        if (allZero) continue;
        result.push_back(FromAdhoc(mac, port));
    }
#else
    (void)port;
#endif
    return result;
}

SocketHandle CreateOnlineUdpSocket(const char* name, const std::string& bindIp, uint16_t bindPort, bool allowBroadcast) {
    (void)name;
    SocketHandle socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket < 0) {
#ifdef __vita__
        port_log("[NETPLAY] UDP socket create failed name=%s port=%u error=%d\n",
                 name != nullptr ? name : "unnamed", static_cast<unsigned int>(bindPort), LastError());
#endif
        return kInvalidSocket;
    }

    if (!SetReuseAddress(socket) || !SetNonBlocking(socket)) {
#ifdef __vita__
        port_log("[NETPLAY] UDP socket options failed name=%s port=%u error=%d\n",
                 name != nullptr ? name : "unnamed", static_cast<unsigned int>(bindPort), LastError());
#endif
        ::close(socket);
        socket = kInvalidSocket;
        return kInvalidSocket;
    }

    if (allowBroadcast) {
        int enabled = 1;
#ifdef __vita__
        if (setsockopt(socket, SOL_SOCKET, SO_BROADCAST, &enabled, sizeof(enabled)) < 0) {
            port_log("[NETPLAY] UDP broadcast option failed name=%s port=%u error=%d\n",
                     name != nullptr ? name : "unnamed", static_cast<unsigned int>(bindPort), LastError());
#else
        if (setsockopt(socket, SOL_SOCKET, SO_BROADCAST, &enabled, sizeof(enabled)) < 0) {
#endif
            ::close(socket);
            socket = kInvalidSocket;
            return kInvalidSocket;
        }
    }

    SocketAddress bindAddress{};
    if (!ResolveBindAddress(bindIp, bindPort, bindAddress)) {
#ifdef __vita__
        port_log("[NETPLAY] UDP invalid bind address name=%s addr=%s:%u\n",
                 name != nullptr ? name : "unnamed", bindIp.c_str(), static_cast<unsigned int>(bindPort));
#endif
        ::close(socket);
        socket = kInvalidSocket;
        return kInvalidSocket;
    }
#ifdef __vita__
    const sockaddr_in native = ToNative(bindAddress);
    if (::bind(socket, reinterpret_cast<const sockaddr*>(&native), sizeof(native)) < 0) {
        const int bindError = LastError();
        port_log("[NETPLAY] UDP bind failed name=%s addr=%s:%u error=%d\n",
                 name != nullptr ? name : "unnamed",
                 bindIp.empty() ? "0.0.0.0" : bindIp.c_str(),
                 static_cast<unsigned int>(bindPort), bindError);
        ::close(socket);
        socket = kInvalidSocket;
        errno = bindError;
        return kInvalidSocket;
#else
    const sockaddr_in native = ToNative(bindAddress);
    if (::bind(socket, reinterpret_cast<const sockaddr*>(&native), sizeof(native)) < 0) {
#endif
        ::close(socket);
        socket = kInvalidSocket;
        return kInvalidSocket;
    }
    return socket;
}

SocketHandle CreateOnlineTcpListener(const char* name, const std::string& bindIp, uint16_t bindPort, int backlog) {
    (void)name;
    SocketHandle socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket < 0) {
#ifdef __vita__
        port_log("[NETPLAY] TCP socket create failed name=%s port=%u error=%d\n",
                 name != nullptr ? name : "unnamed", static_cast<unsigned int>(bindPort), LastError());
#endif
        return kInvalidSocket;
    }
    if (!SetReuseAddress(socket) || !SetNonBlocking(socket)) {
#ifdef __vita__
        port_log("[NETPLAY] TCP socket options failed name=%s port=%u error=%d\n",
                 name != nullptr ? name : "unnamed", static_cast<unsigned int>(bindPort), LastError());
#endif
        ::close(socket);
        socket = kInvalidSocket;
        return kInvalidSocket;
    }

    SocketAddress bindAddress{};
    if (!ResolveBindAddress(bindIp, bindPort, bindAddress)) {
#ifdef __vita__
        port_log("[NETPLAY] TCP invalid bind address name=%s addr=%s:%u\n",
                 name != nullptr ? name : "unnamed", bindIp.c_str(), static_cast<unsigned int>(bindPort));
#endif
        ::close(socket);
        socket = kInvalidSocket;
        return kInvalidSocket;
    }
#ifdef __vita__
    const sockaddr_in native = ToNative(bindAddress);
    if (::bind(socket, reinterpret_cast<const sockaddr*>(&native), sizeof(native)) < 0) {
        const int bindError = LastError();
        port_log("[NETPLAY] TCP bind failed name=%s addr=%s:%u error=%d\n",
                 name != nullptr ? name : "unnamed",
                 bindIp.empty() ? "0.0.0.0" : bindIp.c_str(),
                 static_cast<unsigned int>(bindPort), bindError);
        ::close(socket);
        socket = kInvalidSocket;
        errno = bindError;
        return kInvalidSocket;
    }
    if (::listen(socket, backlog) < 0) {
        port_log("[NETPLAY] TCP listen failed name=%s port=%u error=%d\n",
                 name != nullptr ? name : "unnamed", static_cast<unsigned int>(bindPort), LastError());
        ::close(socket);
        socket = kInvalidSocket;
        return kInvalidSocket;
    }
#else
    const sockaddr_in native = ToNative(bindAddress);
    if (::bind(socket, reinterpret_cast<const sockaddr*>(&native), sizeof(native)) < 0 ||
        ::listen(socket, backlog) < 0) {
        ::close(socket);
        socket = kInvalidSocket;
        return kInvalidSocket;
    }
#endif
    return socket;
}

SocketHandle CreateOnlineTcpSocket(const char* name) {
    (void)name;
    SocketHandle socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket < 0) return kInvalidSocket;
    if (!SetNonBlocking(socket)) {
        ::close(socket);
        socket = kInvalidSocket;
        return kInvalidSocket;
    }
    return socket;
}

ConnectResult ConnectOnline(SocketHandle socket, const SocketAddress& address) {
    const sockaddr_in native = ToNative(address);
    const int result = ::connect(socket, reinterpret_cast<const sockaddr*>(&native), sizeof(native));
    if (result == 0) return ConnectResult::Connected;
    return IsWouldBlock(result) ? ConnectResult::InProgress : ConnectResult::Failed;
}

SocketHandle AcceptOnline(SocketHandle listener, SocketAddress& peer) {
    sockaddr_in native{};
    socklen_t length = sizeof(native);
    SocketHandle socket = ::accept(listener, reinterpret_cast<sockaddr*>(&native), &length);
    if (socket < 0) return kInvalidSocket;
    if (!SetNonBlocking(socket)) {
        ::close(socket);
        socket = kInvalidSocket;
        return kInvalidSocket;
    }
    peer = FromNative(native);
    return socket;
}

int SendOnline(SocketHandle socket, const void* data, std::size_t size) {
    const unsigned int amount = static_cast<unsigned int>(std::min<std::size_t>(size, 0x7FFFFFFFU));
    return static_cast<int>(::send(socket, data, amount, 0));
}

int RecvOnline(SocketHandle socket, void* data, std::size_t size) {
    const unsigned int amount = static_cast<unsigned int>(std::min<std::size_t>(size, 0x7FFFFFFFU));
    return static_cast<int>(::recv(socket, data, amount, 0));
}

int SendToOnline(SocketHandle socket, const void* data, std::size_t size, const SocketAddress& address) {
    const unsigned int amount = static_cast<unsigned int>(std::min<std::size_t>(size, 0x7FFFFFFFU));
    const sockaddr_in native = ToNative(address);
    return static_cast<int>(::sendto(socket, data, amount, 0,
                                     reinterpret_cast<const sockaddr*>(&native), sizeof(native)));
}

int RecvFromOnline(SocketHandle socket, void* data, std::size_t size, SocketAddress& address) {
    const unsigned int amount = static_cast<unsigned int>(std::min<std::size_t>(size, 0x7FFFFFFFU));
    sockaddr_in native{};
    socklen_t length = sizeof(native);
    const int result = static_cast<int>(::recvfrom(socket, data, amount, 0,
                                                   reinterpret_cast<sockaddr*>(&native), &length));
    if (result >= 0) address = FromNative(native);
    return result;
}

bool GetOnlineSocketError(SocketHandle socket, int& errorCode) {
    errorCode = 0;
    socklen_t length = sizeof(errorCode);
    return getsockopt(socket, SOL_SOCKET, SO_ERROR, &errorCode, &length) == 0;
}

bool IsOnlineConnected(SocketHandle socket) {
    sockaddr_in native{};
    socklen_t length = sizeof(native);
    return ::getpeername(socket, reinterpret_cast<sockaddr*>(&native), &length) == 0;
}

void CloseOnline(SocketHandle& socket) {
    if (socket == kInvalidSocket) return;
    ::close(socket);
    socket = kInvalidSocket;
}

SocketHandle CreateDatagram(NetplayMode mode, const char* name, const std::string& localEndpoint,
                            uint16_t bindPort, bool allowBroadcast) {
    sLastAdhocError = 0;
    if (!IsAdhoc(mode)) {
        return CreateOnlineUdpSocket(name, localEndpoint, bindPort, allowBroadcast);
    }

#ifdef __vita__
    (void)localEndpoint;
    (void)allowBroadcast;
    SceNetEtherAddr localMac{};
    if (!GetLocalAdhocMac(localMac)) return kInvalidSocket;
    const int socket = sceNetAdhocPdpCreate(&localMac, bindPort, 0x4000, 0);
    if (socket < 0) {
        sLastAdhocError = socket;
        port_log("[NETPLAY] AdHoc PDP create failed name=%s port=%u error=0x%08X\n",
                 name != nullptr ? name : "unnamed", static_cast<unsigned int>(bindPort),
                 static_cast<unsigned int>(socket));
        return kInvalidSocket;
    }
    uint16_t actualPort = bindPort;
    const bool haveActualPort = QueryPdpLocalPort(socket, actualPort);
    port_log("[NETPLAY] AdHoc PDP opened name=%s requested_port=%u actual_port=%u%s\n",
             name != nullptr ? name : "unnamed", static_cast<unsigned int>(bindPort),
             static_cast<unsigned int>(actualPort), haveActualPort ? "" : " (stat unavailable)");
    return socket;
#else
    return CreateOnlineUdpSocket(name, localEndpoint, bindPort, allowBroadcast);
#endif
}

int SendDatagram(NetplayMode mode, SocketHandle socket, const void* data, std::size_t size,
                 const SocketAddress& address) {
    sLastAdhocError = 0;
    if (!IsAdhoc(mode)) return SendToOnline(socket, data, size, address);
#ifdef __vita__
    if (size > SCE_NET_ADHOC_PDP_MTU) {
        sLastAdhocError = SCE_ERROR_NET_ADHOC_INVALID_DATALEN;
        return sLastAdhocError;
    }
    const SceNetEtherAddr mac = ToAdhocMac(address);
    // ScePspnetAdhoc uses network byte order for the remote PDP port. This is
    // also how vitaQuake builds its broadcast sockaddr before PdpSend().
    const SceUShort16 remotePort = sceNetHtons(address.port);
    const int result = sceNetAdhocPdpSend(socket, &mac, remotePort, data,
                                          static_cast<int>(size), 0, SCE_NET_ADHOC_F_NONBLOCK);
    if (result < 0) {
        sLastAdhocError = result;
        return result;
    }
    return static_cast<int>(size);
#else
    return SendToOnline(socket, data, size, address);
#endif
}

int RecvDatagram(NetplayMode mode, SocketHandle socket, void* data, std::size_t size,
                 SocketAddress& address) {
    sLastAdhocError = 0;
    if (!IsAdhoc(mode)) return RecvFromOnline(socket, data, size, address);
#ifdef __vita__
    SceNetEtherAddr source{};
    SceUShort16 sourcePort = 0;
    int length = static_cast<int>(std::min<std::size_t>(size, SCE_NET_ADHOC_PDP_MTU));
    const int result = sceNetAdhocPdpRecv(socket, &source, &sourcePort, data, &length,
                                          0, SCE_NET_ADHOC_F_NONBLOCK);
    if (result < 0) {
        sLastAdhocError = result;
        return result;
    }
    // Keep SocketAddress ports in host byte order; PDP reports the sender port
    // in network byte order.
    address = FromAdhoc(source, sceNetNtohs(sourcePort));
    return length;
#else
    return RecvFromOnline(socket, data, size, address);
#endif
}

void CloseDatagram(NetplayMode mode, SocketHandle& socket) {
    if (socket == kInvalidSocket) return;
    if (!IsAdhoc(mode)) {
        CloseOnline(socket);
        return;
    }
#ifdef __vita__
    const int result = sceNetAdhocPdpDelete(socket, 0);
    if (result < 0) sLastAdhocError = result;
    socket = kInvalidSocket;
#else
    CloseOnline(socket);
#endif
}

SocketHandle CreateReliableListener(NetplayMode mode, const char* name, const std::string& localEndpoint,
                                    uint16_t bindPort, int backlog) {
    sLastAdhocError = 0;
    if (!IsAdhoc(mode)) {
        return CreateOnlineTcpListener(name, localEndpoint, bindPort, backlog);
    }
#ifdef __vita__
    (void)localEndpoint;
    SceNetEtherAddr localMac{};
    if (!GetLocalAdhocMac(localMac)) return kInvalidSocket;
    const int socket = sceNetAdhocPtpListen(&localMac, bindPort, 0x4000, 100000, 10, backlog, 0);
    if (socket < 0) {
        sLastAdhocError = socket;
        port_log("[NETPLAY] AdHoc PTP listen failed name=%s port=%u error=0x%08X\n",
                 name != nullptr ? name : "unnamed", static_cast<unsigned int>(bindPort),
                 static_cast<unsigned int>(socket));
        return kInvalidSocket;
    }
    port_log("[NETPLAY] AdHoc PTP listening port=%u\n", static_cast<unsigned int>(bindPort));
    return socket;
#else
    return CreateOnlineTcpListener(name, localEndpoint, bindPort, backlog);
#endif
}

ConnectResult CreateReliableClient(NetplayMode mode, const char* name, const SocketAddress& address,
                                   uint16_t localPort, SocketHandle& socket) {
    sLastAdhocError = 0;
    socket = kInvalidSocket;
    if (!IsAdhoc(mode)) {
        socket = CreateOnlineTcpSocket(name);
        if (socket == kInvalidSocket) return ConnectResult::Failed;
        const ConnectResult result = ConnectOnline(socket, address);
        if (result == ConnectResult::Failed) CloseOnline(socket);
        return result;
    }
#ifdef __vita__
    (void)name;
    SceNetEtherAddr localMac{};
    if (!GetLocalAdhocMac(localMac)) return ConnectResult::Failed;
    const SceNetEtherAddr remoteMac = ToAdhocMac(address);
    socket = sceNetAdhocPtpOpen(&localMac, localPort, &remoteMac, address.port,
                                0x4000, 100000, 10, 0);
    if (socket < 0) {
        sLastAdhocError = socket;
        socket = kInvalidSocket;
        return ConnectResult::Failed;
    }
    const int result = sceNetAdhocPtpConnect(socket, 0, SCE_NET_ADHOC_F_NONBLOCK);
    if (result == 0) return ConnectResult::Connected;
    if (result == SCE_ERROR_NET_ADHOC_WOULD_BLOCK || result == SCE_ERROR_NET_ADHOC_BUSY) {
        sLastAdhocError = result;
        return ConnectResult::InProgress;
    }
    sLastAdhocError = result;
    sceNetAdhocPtpClose(socket, 0);
    socket = kInvalidSocket;
    return ConnectResult::Failed;
#else
    (void)localPort;
    socket = CreateOnlineTcpSocket(name);
    if (socket == kInvalidSocket) return ConnectResult::Failed;
    const ConnectResult result = ConnectOnline(socket, address);
    if (result == ConnectResult::Failed) CloseOnline(socket);
    return result;
#endif
}

SocketHandle AcceptReliable(NetplayMode mode, SocketHandle listener, SocketAddress& peer) {
    sLastAdhocError = 0;
    if (!IsAdhoc(mode)) return AcceptOnline(listener, peer);
#ifdef __vita__
    SceNetEtherAddr source{};
    SceUShort16 sourcePort = 0;
    const int socket = sceNetAdhocPtpAccept(listener, &source, &sourcePort, 0, SCE_NET_ADHOC_F_NONBLOCK);
    if (socket < 0) {
        sLastAdhocError = socket;
        return kInvalidSocket;
    }
    peer = FromAdhoc(source, sourcePort);
    return socket;
#else
    return AcceptOnline(listener, peer);
#endif
}

int SendReliable(NetplayMode mode, SocketHandle socket, const void* data, std::size_t size) {
    sLastAdhocError = 0;
    if (!IsAdhoc(mode)) return SendOnline(socket, data, size);
#ifdef __vita__
    int length = static_cast<int>(std::min<std::size_t>(size, 0x7FFFFFFFU));
    const int result = sceNetAdhocPtpSend(socket, data, &length, 0, SCE_NET_ADHOC_F_NONBLOCK);
    if (result < 0) {
        sLastAdhocError = result;
        return result;
    }
    return length;
#else
    return SendOnline(socket, data, size);
#endif
}

int RecvReliable(NetplayMode mode, SocketHandle socket, void* data, std::size_t size) {
    sLastAdhocError = 0;
    if (!IsAdhoc(mode)) return RecvOnline(socket, data, size);
#ifdef __vita__
    int length = static_cast<int>(std::min<std::size_t>(size, 0x7FFFFFFFU));
    const int result = sceNetAdhocPtpRecv(socket, data, &length, 0, SCE_NET_ADHOC_F_NONBLOCK);
    if (result == SCE_ERROR_NET_ADHOC_DISCONNECTED || result == SCE_ERROR_NET_ADHOC_SOCKET_DELETED) {
        return 0;
    }
    if (result < 0) {
        sLastAdhocError = result;
        return result;
    }
    return length;
#else
    return RecvOnline(socket, data, size);
#endif
}

void CloseReliable(NetplayMode mode, SocketHandle& socket) {
    if (socket == kInvalidSocket) return;
    if (!IsAdhoc(mode)) {
        CloseOnline(socket);
        return;
    }
#ifdef __vita__
    const int result = sceNetAdhocPtpClose(socket, 0);
    if (result < 0) sLastAdhocError = result;
    socket = kInvalidSocket;
#else
    CloseOnline(socket);
#endif
}

bool GetSocketError(NetplayMode mode, SocketHandle socket, int& errorCode) {
    if (!IsAdhoc(mode)) return GetOnlineSocketError(socket, errorCode);
#ifdef __vita__
    int state = SCE_NET_ADHOC_PTP_STATE_CLOSED;
    if (!QueryPtpState(socket, state)) {
        errorCode = LastError();
        return false;
    }
    if (state == SCE_NET_ADHOC_PTP_STATE_ESTABLISHED) {
        errorCode = 0;
    } else if (state == SCE_NET_ADHOC_PTP_STATE_SYN_SENT || state == SCE_NET_ADHOC_PTP_STATE_SYN_RCVD) {
        errorCode = SCE_ERROR_NET_ADHOC_WOULD_BLOCK;
    } else {
        errorCode = SCE_ERROR_NET_ADHOC_NOT_CONNECTED;
    }
    return true;
#else
    return GetOnlineSocketError(socket, errorCode);
#endif
}

bool IsConnected(NetplayMode mode, SocketHandle socket) {
    if (!IsAdhoc(mode)) return IsOnlineConnected(socket);
#ifdef __vita__
    int state = SCE_NET_ADHOC_PTP_STATE_CLOSED;
    return QueryPtpState(socket, state) && state == SCE_NET_ADHOC_PTP_STATE_ESTABLISHED;
#else
    return IsOnlineConnected(socket);
#endif
}

} // namespace ssb64::netplay::transport
