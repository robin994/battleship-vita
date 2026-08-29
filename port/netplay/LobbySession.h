#pragma once

#include "NetplayProtocol.h"
#include "../vita/VitaNetworkTransport.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

namespace ssb64::netplay {

enum class LobbySlotState : uint8_t {
    Empty = 0,
    Connected,
    Ready,
    Disconnected,
};

struct LobbyPlayerView {
    uint8_t playerId = 0xFF;
    LobbySlotState state = LobbySlotState::Empty;
    std::string name;
    std::string ip;
    uint32_t pingMs = 0;
    uint32_t jitterMs = 0;
};

struct LobbyView {
    bool isHost = false;
    bool connected = false;
    uint32_t sessionId = 0;
    uint8_t localPlayerId = 0xFF;
    LobbyStatus status = LobbyStatus::Open;
    std::array<LobbyPlayerView, kMaxPlayers> players{};
    std::string lastMessage;
    int8_t ruleStage = -1;
    int8_t ruleStocks = -1;
    int8_t ruleTimeUnits = -1;
};

struct LobbySessionEvent {
    PacketType type = PacketType::Heartbeat;
    uint8_t sourcePlayerId = 0xFF;
    std::vector<uint8_t> payload;
};

class LobbySession {
public:
    LobbySession();
    ~LobbySession();

    bool StartHost(NetplayMode mode, uint32_t sessionId, const std::string& hostName, const std::string& buildId,
                   const std::string& localEndpoint);
    bool StartClient(NetplayMode mode, const std::string& hostEndpoint, uint32_t expectedSessionId,
                     const std::string& playerName, const std::string& buildId);
    void Stop(RejectReason reason = RejectReason::None, bool notifyPeer = true);
    void Poll();

    bool SetLocalReady(bool ready);
    bool LocalReady() const;
    void SetHostRules(int stage, int stocks, int timeUnits);
    void ReopenLobby();
    bool CanHostStart() const;
    bool StartCharacterSelect();
    bool ConsumeCharacterSelectStart();
    bool SendSessionMessage(PacketType type, const std::vector<uint8_t>& payload);
    bool PopSessionEvent(LobbySessionEvent& event);

    LobbyView Snapshot() const { return mView; }
    bool IsActive() const { return mMode != Mode::None; }
    bool IsHost() const { return mMode == Mode::Host; }
    uint8_t ConnectedPlayerCount() const;

private:
    using Clock = std::chrono::steady_clock;
    enum class Mode { None, Host, Client };

    struct Peer {
        transport::SocketHandle socket = transport::kInvalidSocket;
        transport::SocketAddress address{};
        uint8_t playerId = 0xFF;
        bool joined = false;
        bool connectPending = false;
        bool joinSent = false;
        bool remoteClosed = false;
        std::vector<uint8_t> rx;
        std::deque<std::vector<uint8_t>> tx;
        std::size_t txOffset = 0;
        uint32_t nextSequence = 1;
        uint32_t nextHeartbeat = 1;
        Clock::time_point connectedAt{};
        Clock::time_point lastRx{};
        Clock::time_point nextHeartbeatAt{};
        std::unordered_map<uint32_t, Clock::time_point> heartbeatSent;
        uint32_t pingMs = 0;
        uint32_t jitterMs = 0;
    };

    void ResetView();
    void PollHost();
    void PollClient();
    void AcceptClients();
    bool PollPeer(Peer& peer, bool hostSide);
    bool ReceiveIntoPeer(Peer& peer);
    bool DrainPackets(Peer& peer, bool hostSide);
    bool FlushPeer(Peer& peer);
    void QueuePacket(Peer& peer, PacketType type, uint8_t playerId,
                     const std::vector<uint8_t>& payload = {}, uint8_t flags = 0);
    void Broadcast(PacketType type, uint8_t playerId, const std::vector<uint8_t>& payload = {});
    void SendHeartbeat(Peer& peer);
    void HandleHeartbeat(Peer& peer, const DecodedPacket& packet);

    bool HandleHostPacket(Peer& peer, const DecodedPacket& packet);
    bool HandleClientPacket(Peer& peer, const DecodedPacket& packet);
    bool HandleJoinRequest(Peer& peer, const DecodedPacket& packet);
    void RejectPeer(Peer& peer, RejectReason reason, const char* message);
    void RemovePeer(std::size_t index, RejectReason reason, const char* message);
    int FindFreeSlot() const;
    void RefreshHostStatus();

    std::vector<uint8_t> MakeLobbySnapshotPayload(uint8_t assignedPlayer) const;
    std::vector<uint8_t> MakeLobbyRulesPayload() const;
    bool ApplyLobbySnapshotPayload(const std::vector<uint8_t>& payload);
    void BroadcastPlayerJoined(uint8_t playerId);
    void BroadcastPlayerLeft(uint8_t playerId, RejectReason reason);
    void BroadcastReady(uint8_t playerId);
    bool QueueSessionEvent(const Peer& peer, const DecodedPacket& packet, bool relayFromClient);

    Mode mMode = Mode::None;
    NetplayMode mTransportMode = NetplayMode::Online;
    transport::SocketHandle mListener = transport::kInvalidSocket;
    std::array<Peer, kMaxPlayers - 1> mHostPeers{};
    Peer mServerPeer{};
    LobbyView mView{};
    std::string mBuildId;
    std::string mLocalName;
    uint32_t mExpectedSessionId = 0;
    bool mCharacterSelectStartPending = false;
    std::deque<LobbySessionEvent> mSessionEvents;
};

const char* LobbySlotStateName(LobbySlotState state);

} // namespace ssb64::netplay
