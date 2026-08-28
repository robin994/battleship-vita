#pragma once

#include "NetplayProtocol.h"
#include "../vita/VitaNetworkTransport.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace ssb64::netplay {

struct DiscoveredLobby {
    uint32_t sessionId = 0;
    std::string hostIp;
    std::string hostName;
    std::string buildId;
    uint16_t protocolVersion = 0;
    uint8_t playerCount = 0;
    uint8_t maxPlayers = static_cast<uint8_t>(kMaxPlayers);
    LobbyStatus status = LobbyStatus::Open;
    uint32_t pingMs = 0;
    bool compatible = false;
};

struct DiscoveryHostInfo {
    uint32_t sessionId = 0;
    std::string localIp;
    std::string hostName;
    std::string buildId;
    uint8_t playerCount = 1;
    uint8_t maxPlayers = static_cast<uint8_t>(kMaxPlayers);
    LobbyStatus status = LobbyStatus::Open;
};

class LanDiscovery {
public:
    LanDiscovery() = default;
    ~LanDiscovery();

    bool StartClient(NetplayMode mode, const std::string& localBuildId);
    bool StartHost(NetplayMode mode, const DiscoveryHostInfo& info);
    void SetHostInfo(const DiscoveryHostInfo& info);
    void Stop();

    void Poll();
    void RequestImmediateScan();
    std::vector<DiscoveredLobby> Snapshot() const;
    bool IsClientActive() const { return mClientSocket != transport::kInvalidSocket; }
    bool IsHostActive() const { return mHostSocket != transport::kInvalidSocket; }

private:
    using Clock = std::chrono::steady_clock;

    void PollClient();
    void PollHost();
    void SendDiscoveryRequest();
    void ExpireEntries();

    transport::SocketHandle mClientSocket = transport::kInvalidSocket;
    transport::SocketHandle mHostSocket = transport::kInvalidSocket;
    NetplayMode mMode = NetplayMode::Online;
    std::string mLocalBuildId;
    DiscoveryHostInfo mHostInfo{};
    uint32_t mNextNonce = 1;
    uint32_t mSequence = 1;
    Clock::time_point mNextScan{};
    bool mLoggedFirstBroadcast = false;
    bool mLoggedFirstHostRequest = false;
    bool mLoggedFirstHostResponse = false;
    int mLastAdhocPeerCount = -1;
    std::unordered_map<uint32_t, Clock::time_point> mNonceSentAt;

    struct TimedLobby {
        DiscoveredLobby lobby;
        Clock::time_point lastSeen{};
    };
    std::unordered_map<std::string, TimedLobby> mEntries;
};

} // namespace ssb64::netplay
