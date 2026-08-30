#pragma once

#include "LanDiscovery.h"
#include "NetplayProtocol.h"
#include "../vita/VitaNetworkTransport.h"

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace ssb64::netplay {

// Worker-thread-only client for the self-hosted lobby board (server/matchmaker).
// It never carries gameplay traffic: it publishes the local lobby (host) or
// fetches the open-lobby list (client) so the existing DIRECT-IP join path can
// dial the host directly.
class RendezvousClient {
public:
    RendezvousClient() = default;
    ~RendezvousClient();

    RendezvousClient(const RendezvousClient&) = delete;
    RendezvousClient& operator=(const RendezvousClient&) = delete;

    void SetServer(const std::string& host);
    bool HasServer() const { return !mHost.empty(); }

    void StartList(const std::string& buildId);
    void RequestList();
    void StartHost(const std::string& hostName, const std::string& buildId, uint8_t maxPlayers,
                   uint16_t lobbyPort, uint16_t gameplayPort, uint32_t bsnpSessionId);
    void UpdateStatus(uint8_t players, uint8_t status);
    void Stop();

    void Poll();

    std::vector<DiscoveredLobby> Lobbies() const { return mLobbies; }
    bool Hosting() const { return mMode == Mode::Hosting; }
    bool HostRegistered() const { return mMode == Mode::Hosting && mRegistered; }
    std::string PublicIp() const { return mMode == Mode::Hosting ? mPublicIp : std::string(); }
    bool Fatal() const { return mFatal; }
    std::string LastMessage() const { return mLastMessage; }

private:
    using Clock = std::chrono::steady_clock;
    enum class Mode { Idle, Listing, Hosting };

    bool Resolve();
    transport::SocketHandle DialServer(const char* name, Clock::time_point& startedAt, bool& connecting);
    void PollList();
    void PollHost();
    void CloseList();
    void CloseHost();
    bool ReadFrames(transport::SocketHandle& sock, std::vector<uint8_t>& rx);

    std::string mHost;
    uint16_t mServerPort = kRendezvousPort;
    uint32_t mResolvedIp = 0;
    Clock::time_point mResolvedAt{};

    Mode mMode = Mode::Idle;
    std::string mBuildId;
    bool mFatal = false;
    std::string mLastMessage;

    transport::SocketHandle mListSocket = transport::kInvalidSocket;
    bool mListConnecting = false;
    bool mListSent = false;
    std::vector<uint8_t> mListRx;
    Clock::time_point mListStartedAt{};
    Clock::time_point mNextList{};
    std::vector<DiscoveredLobby> mPendingList;
    std::vector<DiscoveredLobby> mLobbies;

    transport::SocketHandle mHostSocket = transport::kInvalidSocket;
    bool mHostConnecting = false;
    bool mRegisterSent = false;
    bool mRegistered = false;
    std::vector<uint8_t> mHostRx;
    std::string mHostName;
    uint8_t mHostMax = static_cast<uint8_t>(kMaxPlayers);
    uint16_t mHostLobbyPort = 0;
    uint16_t mHostGameplayPort = 0;
    uint32_t mHostBsnpSession = 0;
    uint32_t mLobbyId = 0;
    std::string mPublicIp;
    uint8_t mPlayers = 1;
    uint8_t mStatus = 0;
    uint8_t mSentPlayers = 0xFF;
    uint8_t mSentStatus = 0xFF;
    Clock::time_point mHostStartedAt{};
    Clock::time_point mNextPing{};
};

} // namespace ssb64::netplay
