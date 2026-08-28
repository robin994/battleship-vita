#include "LanDiscovery.h"

#include "../port_log.h"

#include <algorithm>
#include <array>
#include <string_view>

namespace ssb64::netplay {
namespace {

constexpr auto kScanInterval = std::chrono::milliseconds(700);
constexpr auto kEntryLifetime = std::chrono::milliseconds(2500);
constexpr std::size_t kDiscoveryBufferBytes = kWireHeaderBytes + kMaxPayloadBytes;

std::string MakeEntryKey(const std::string& ip, uint32_t sessionId) {
    return ip + ":" + std::to_string(sessionId);
}

} // namespace

LanDiscovery::~LanDiscovery() {
    Stop();
}

bool LanDiscovery::StartClient(NetplayMode mode, const std::string& localBuildId) {
    if (mClientSocket != transport::kInvalidSocket) {
        return true;
    }
    mMode = mode;
    // Match vitaQuake's AdHoc layout: the control/discovery socket is opened
    // on an automatic local port, while only the host listener owns the fixed
    // discovery port. Replies are sent back to the source port reported by PDP.
    const uint16_t bindPort = 0;
    mClientSocket = transport::CreateDatagram(mode, "bsnp_discovery_client", {}, bindPort, true);
    if (mClientSocket == transport::kInvalidSocket) {
        port_log("[NETPLAY] discovery client socket failed error=%d\n", transport::LastError());
        return false;
    }
    mLocalBuildId = localBuildId;
    mEntries.clear();
    mNonceSentAt.clear();
    mNextScan = Clock::time_point{};
    mLoggedFirstBroadcast = false;
    mLastAdhocPeerCount = -1;
    port_log("[NETPLAY] %s discovery client opened port=%u -> broadcast:%u\n",
             mode == NetplayMode::LocalAdhoc ? "AdHoc" : "online",
             bindPort, kDiscoveryPort);
    return true;
}

bool LanDiscovery::StartHost(NetplayMode mode, const DiscoveryHostInfo& info) {
    mMode = mode;
    if (mHostSocket == transport::kInvalidSocket) {
        // Receive LAN broadcast packets on all local interfaces. Binding this
        // socket to the Vita's unicast Wi-Fi address can prevent delivery of
        // datagrams addressed to 255.255.255.255. The TCP lobby listener stays
        // bound to the concrete Wi-Fi IP.
        mHostSocket = transport::CreateDatagram(mode, "bsnp_discovery_host", {}, kDiscoveryPort, true);
        if (mHostSocket == transport::kInvalidSocket) {
            port_log("[NETPLAY] discovery host bind failed udp=%u error=%d\n",
                     kDiscoveryPort, transport::LastError());
            return false;
        }
        port_log("[NETPLAY] %s discovery host listening endpoint=%s port=%u\n",
                 mode == NetplayMode::LocalAdhoc ? "AdHoc" : "online",
                 info.localIp.empty() ? "0.0.0.0" : info.localIp.c_str(), kDiscoveryPort);
    }
    mLoggedFirstHostRequest = false;
    mLoggedFirstHostResponse = false;
    mHostInfo = info;
    return true;
}

void LanDiscovery::SetHostInfo(const DiscoveryHostInfo& info) {
    mHostInfo = info;
}

void LanDiscovery::Stop() {
    if (mClientSocket != transport::kInvalidSocket) {
        transport::CloseDatagram(mMode, mClientSocket);
        port_log("[NETPLAY] discovery client socket closed\n");
    }
    if (mHostSocket != transport::kInvalidSocket) {
        transport::CloseDatagram(mMode, mHostSocket);
        port_log("[NETPLAY] discovery host socket closed\n");
    }
    mEntries.clear();
    mNonceSentAt.clear();
}

void LanDiscovery::RequestImmediateScan() {
    mNextScan = Clock::time_point{};
}

void LanDiscovery::Poll() {
    if (mHostSocket != transport::kInvalidSocket) {
        PollHost();
    }
    if (mClientSocket != transport::kInvalidSocket) {
        PollClient();
        const auto now = Clock::now();
        if (mNextScan == Clock::time_point{} || now >= mNextScan) {
            SendDiscoveryRequest();
            mNextScan = now + kScanInterval;
        }
        ExpireEntries();
    }
}

void LanDiscovery::SendDiscoveryRequest() {
    const uint32_t nonce = mNextNonce++;
    std::vector<uint8_t> payload;
    PayloadWriter writer(payload);
    if (!writer.U32(nonce) || !writer.String(mLocalBuildId, 24)) {
        return;
    }

    PacketHeader header{};
    header.type = PacketType::DiscoveryRequest;
    header.sessionId = 0;
    header.playerId = 0xFF;
    header.sequence = mSequence++;

    std::vector<uint8_t> packet;
    if (!EncodePacket(header, payload, packet)) {
        return;
    }
    bool anySent = false;
    const transport::SocketAddress broadcast = transport::BroadcastAddress(mMode, kDiscoveryPort);
    const int sent = transport::SendDatagram(mMode, mClientSocket, packet.data(), packet.size(), broadcast);
    if (sent >= 0) {
        anySent = true;
        mNonceSentAt[nonce] = Clock::now();
        if (!mLoggedFirstBroadcast) {
            port_log("[NETPLAY] discovery first broadcast sent bytes=%d port=%u\n", sent, kDiscoveryPort);
            mLoggedFirstBroadcast = true;
        }
    } else if (!transport::IsWouldBlock(sent)) {
        port_log("[NETPLAY] discovery broadcast failed error=%d\n", transport::LastError());
    }

    if (mMode == NetplayMode::LocalAdhoc) {
        const std::vector<transport::SocketAddress> peers = transport::EnumerateAdhocPeers(kDiscoveryPort);
        if (static_cast<int>(peers.size()) != mLastAdhocPeerCount) {
            mLastAdhocPeerCount = static_cast<int>(peers.size());
            port_log("[NETPLAY] AdHoc discovery peer list count=%d\n", mLastAdhocPeerCount);
            for (const auto& peer : peers) {
                port_log("[NETPLAY] AdHoc discovery peer mac=%s\n", transport::ToString(peer).c_str());
            }
        }
        for (const auto& peer : peers) {
            const int peerSent = transport::SendDatagram(mMode, mClientSocket, packet.data(), packet.size(), peer);
            if (peerSent >= 0) {
                anySent = true;
            } else if (!transport::IsWouldBlock(peerSent)) {
                port_log("[NETPLAY] AdHoc discovery unicast failed peer=%s error=0x%08X\n",
                         transport::ToString(peer).c_str(),
                         static_cast<unsigned int>(transport::LastError()));
            }
        }
    }

    if (anySent) {
        mNonceSentAt[nonce] = Clock::now();
    }

    if (mNonceSentAt.size() > 16) {
        const auto now = Clock::now();
        for (auto it = mNonceSentAt.begin(); it != mNonceSentAt.end();) {
            if (now - it->second > kEntryLifetime) it = mNonceSentAt.erase(it);
            else ++it;
        }
    }
}

void LanDiscovery::PollHost() {
    std::array<uint8_t, kDiscoveryBufferBytes> buffer{};
    for (int budget = 0; budget < 12; ++budget) {
        transport::SocketAddress source{};
        const int received = transport::RecvDatagram(mMode, mHostSocket, buffer.data(), buffer.size(), source);
        if (received < 0) {
            if (!transport::IsWouldBlock(received)) {
                port_log("[NETPLAY] discovery host recv failed error=%d\n", transport::LastError());
            }
            break;
        }
        if (received == 0) break;

        DecodedPacket request;
        RejectReason reject = RejectReason::None;
        if (!DecodePacketAnyVersion(buffer.data(), static_cast<std::size_t>(received), request, &reject) ||
            request.header.type != PacketType::DiscoveryRequest) {
            continue;
        }
        if (!mLoggedFirstHostRequest) {
            port_log("[NETPLAY] discovery host received first request from=%s bytes=%d\n",
                     transport::ToString(source).c_str(), received);
            mLoggedFirstHostRequest = true;
        }

        uint32_t nonce = 0;
        std::string_view requesterBuild;
        PayloadReader reader(request.payload.data(), request.payload.size());
        if (!reader.U32(nonce) || !reader.String(requesterBuild, 24) || !reader.Empty()) {
            continue;
        }

        std::vector<uint8_t> payload;
        PayloadWriter writer(payload);
        if (!writer.U32(nonce) ||
            !writer.U16(kLobbyPort) ||
            !writer.U8(mHostInfo.playerCount) ||
            !writer.U8(mHostInfo.maxPlayers) ||
            !writer.U8(static_cast<uint8_t>(mHostInfo.status)) ||
            !writer.String(mHostInfo.buildId, 24) ||
            !writer.String(mHostInfo.hostName, kMaxPlayerNameBytes)) {
            continue;
        }

        PacketHeader header{};
        header.type = PacketType::DiscoveryResponse;
        header.sessionId = mHostInfo.sessionId;
        header.playerId = 0;
        header.sequence = mSequence++;

        std::vector<uint8_t> response;
        if (!EncodePacket(header, payload, response)) continue;
        const int sent = transport::SendDatagram(mMode, mHostSocket, response.data(), response.size(), source);
        if ((sent >= 0) && !mLoggedFirstHostResponse) {
            port_log("[NETPLAY] discovery host sent first response to=%s bytes=%d\n",
                     transport::ToString(source).c_str(), sent);
            mLoggedFirstHostResponse = true;
        }
    }
}

void LanDiscovery::PollClient() {
    std::array<uint8_t, kDiscoveryBufferBytes> buffer{};
    for (int budget = 0; budget < 24; ++budget) {
        transport::SocketAddress source{};
        const int received = transport::RecvDatagram(mMode, mClientSocket, buffer.data(), buffer.size(), source);
        if (received < 0) {
            if (!transport::IsWouldBlock(received)) {
                port_log("[NETPLAY] discovery client recv failed error=%d\n", transport::LastError());
            }
            break;
        }
        if (received == 0) break;

        DecodedPacket response;
        RejectReason reject = RejectReason::None;
        if (!DecodePacketAnyVersion(buffer.data(), static_cast<std::size_t>(received), response, &reject) ||
            response.header.type != PacketType::DiscoveryResponse || response.header.sessionId == 0) {
            continue;
        }

        uint32_t nonce = 0;
        uint16_t lobbyPort = 0;
        uint8_t players = 0;
        uint8_t maxPlayers = 0;
        uint8_t statusRaw = 0;
        std::string_view build;
        std::string_view hostName;
        PayloadReader reader(response.payload.data(), response.payload.size());
        if (!reader.U32(nonce) || !reader.U16(lobbyPort) ||
            !reader.U8(players) || !reader.U8(maxPlayers) || !reader.U8(statusRaw) ||
            !reader.String(build, 24) || !reader.String(hostName, kMaxPlayerNameBytes) || !reader.Empty()) {
            continue;
        }
        if (lobbyPort != kLobbyPort || maxPlayers == 0 || maxPlayers > kMaxPlayers || players > maxPlayers ||
            statusRaw > static_cast<uint8_t>(LobbyStatus::InGame)) {
            continue;
        }

        const auto sentIt = mNonceSentAt.find(nonce);
        if (sentIt == mNonceSentAt.end()) continue;
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - sentIt->second);

        DiscoveredLobby lobby{};
        lobby.sessionId = response.header.sessionId;
        lobby.hostIp = transport::ToString(source);
        lobby.hostName.assign(hostName.begin(), hostName.end());
        lobby.buildId.assign(build.begin(), build.end());
        lobby.protocolVersion = response.header.protocolVersion;
        lobby.playerCount = players;
        lobby.maxPlayers = maxPlayers;
        lobby.status = static_cast<LobbyStatus>(statusRaw);
        lobby.pingMs = static_cast<uint32_t>(std::clamp<long long>(elapsed.count(), 0, 9999));
        lobby.compatible = lobby.protocolVersion == kProtocolVersion && lobby.buildId == mLocalBuildId;

        const std::string key = MakeEntryKey(lobby.hostIp, lobby.sessionId);
        auto [it, inserted] = mEntries.emplace(key, TimedLobby{});
        const bool changed = inserted || it->second.lobby.playerCount != lobby.playerCount ||
                             it->second.lobby.status != lobby.status ||
                             it->second.lobby.compatible != lobby.compatible;
        it->second.lobby = lobby;
        it->second.lastSeen = Clock::now();
        if (inserted || changed) {
            port_log("[NETPLAY] lobby found host=%s ip=%s players=%u/%u ping=%ums build=%s status=%s compatible=%d\n",
                     lobby.hostName.c_str(), lobby.hostIp.c_str(), lobby.playerCount, lobby.maxPlayers,
                     lobby.pingMs, lobby.buildId.c_str(), LobbyStatusName(lobby.status), lobby.compatible ? 1 : 0);
        }
    }
}

void LanDiscovery::ExpireEntries() {
    const auto now = Clock::now();
    for (auto it = mEntries.begin(); it != mEntries.end();) {
        if (now - it->second.lastSeen > kEntryLifetime) it = mEntries.erase(it);
        else ++it;
    }
}

std::vector<DiscoveredLobby> LanDiscovery::Snapshot() const {
    std::vector<DiscoveredLobby> result;
    result.reserve(mEntries.size());
    for (const auto& [key, entry] : mEntries) {
        (void)key;
        result.push_back(entry.lobby);
    }
    std::sort(result.begin(), result.end(), [](const DiscoveredLobby& a, const DiscoveredLobby& b) {
        if (a.compatible != b.compatible) return a.compatible > b.compatible;
        if (a.status != b.status) return static_cast<int>(a.status) < static_cast<int>(b.status);
        if (a.pingMs != b.pingMs) return a.pingMs < b.pingMs;
        return a.hostName < b.hostName;
    });
    return result;
}

} // namespace ssb64::netplay
