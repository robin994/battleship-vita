#include "LobbySession.h"

#include "../port_log.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <string_view>

namespace ssb64::netplay {
namespace {

constexpr auto kHeartbeatInterval = std::chrono::milliseconds(500);
constexpr auto kPeerTimeout = std::chrono::milliseconds(5000);
constexpr auto kJoinTimeout = std::chrono::milliseconds(5000);
constexpr std::size_t kMaxTcpBuffer = 8192;
constexpr uint8_t kHeartbeatReplyFlag = 1U;

const char* RejectReasonText(RejectReason reason) {
    switch (reason) {
        case RejectReason::None: return "NONE";
        case RejectReason::ProtocolMismatch: return "PROTOCOL MISMATCH";
        case RejectReason::BuildMismatch: return "BUILD MISMATCH";
        case RejectReason::LobbyFull: return "LOBBY FULL";
        case RejectReason::MatchStarted: return "MATCH STARTED";
        case RejectReason::MalformedPacket: return "MALFORMED PACKET";
        case RejectReason::InvalidSession: return "INVALID SESSION";
        case RejectReason::InvalidPlayer: return "INVALID PLAYER";
        case RejectReason::HostClosing: return "HOST CLOSED LOBBY";
    }
    return "DISCONNECTED";
}

uint16_t PeekPayloadLength(const std::vector<uint8_t>& rx) {
    return static_cast<uint16_t>((static_cast<uint16_t>(rx[22]) << 8) | rx[23]);
}

} // namespace

const char* LobbySlotStateName(LobbySlotState state) {
    switch (state) {
        case LobbySlotState::Empty: return "EMPTY";
        case LobbySlotState::Connected: return "CONNECTED";
        case LobbySlotState::Ready: return "READY";
        case LobbySlotState::Disconnected: return "DISCONNECTED";
    }
    return "UNKNOWN";
}

LobbySession::LobbySession() {
    ResetView();
}

LobbySession::~LobbySession() {
    Stop(RejectReason::HostClosing, false);
}

void LobbySession::ResetView() {
    mView = LobbyView{};
    mSessionEvents.clear();
    for (uint8_t i = 0; i < kMaxPlayers; ++i) {
        mView.players[i].playerId = i;
    }
}

bool LobbySession::StartHost(NetplayMode mode, uint32_t sessionId, const std::string& hostName,
                             const std::string& buildId, const std::string& localEndpoint) {
    Stop(RejectReason::None, false);
    ResetView();
    mTransportMode = mode;

    mListener = transport::CreateReliableListener(mode, "bsnp_lobby_host", localEndpoint, kLobbyPort,
                                                  static_cast<int>(kMaxPlayers - 1));
    if (mListener == transport::kInvalidSocket) {
        mView.lastMessage = "FAILED TO OPEN LOBBY";
        port_log("[NETPLAY] lobby %s listen failed %s:%u error=0x%08X\n",
                 mode == NetplayMode::LocalAdhoc ? "PTP" : "TCP",
                 localEndpoint.empty() ? "0.0.0.0" : localEndpoint.c_str(), kLobbyPort,
                 static_cast<unsigned int>(transport::LastError()));
        return false;
    }

    mMode = Mode::Host;
    mBuildId = buildId;
    mLocalName = hostName;
    mView.isHost = true;
    mView.connected = true;
    mView.sessionId = sessionId;
    mView.localPlayerId = 0;
    mView.status = LobbyStatus::Open;
    mView.players[0].state = LobbySlotState::Connected;
    mView.players[0].name = hostName;
    mView.players[0].ip = localEndpoint;
    mView.lastMessage = "LOBBY OPEN";
    mCharacterSelectStartPending = false;

    port_log("[NETPLAY] lobby host opened %s=%s:%u session=%08X host=%s\n",
             mode == NetplayMode::LocalAdhoc ? "adhoc" : "tcp",
             localEndpoint.empty() ? "0.0.0.0" : localEndpoint.c_str(), kLobbyPort, sessionId, hostName.c_str());
    return true;
}

bool LobbySession::StartClient(NetplayMode mode, const std::string& hostEndpoint, uint32_t expectedSessionId,
                               const std::string& playerName, const std::string& buildId) {
    Stop(RejectReason::None, false);
    ResetView();
    mTransportMode = mode;

    transport::SocketAddress address{};
    if (!transport::ParseEndpoint(mode, hostEndpoint, kLobbyPort, address)) {
        mView.lastMessage = "INVALID HOST ADDRESS";
        return false;
    }

    const transport::ConnectResult result = transport::CreateReliableClient(
        mode, "bsnp_lobby_client", address, kLobbyPort, mServerPeer.socket);
    if (result == transport::ConnectResult::Failed) {
        port_log("[NETPLAY] lobby %s connect failed host=%s:%u error=0x%08X\n",
                 mode == NetplayMode::LocalAdhoc ? "PTP" : "TCP",
                 hostEndpoint.c_str(), kLobbyPort, static_cast<unsigned int>(transport::LastError()));
        transport::CloseReliable(mode, mServerPeer.socket);
        mView.lastMessage = "CONNECTION FAILED";
        return false;
    }

    mMode = Mode::Client;
    mBuildId = buildId;
    mLocalName = playerName;
    mExpectedSessionId = expectedSessionId;
    mServerPeer.address = address;
    mServerPeer.connectPending = result == transport::ConnectResult::InProgress;
    mServerPeer.connectedAt = Clock::now();
    mServerPeer.lastRx = Clock::now();
    mServerPeer.nextHeartbeatAt = Clock::now() + kHeartbeatInterval;
    mView.isHost = false;
    mView.connected = false;
    mView.sessionId = expectedSessionId;
    mView.lastMessage = "CONNECTING";
    mCharacterSelectStartPending = false;

    port_log("[NETPLAY] lobby connecting mode=%s host=%s:%u session=%08X\n",
             mode == NetplayMode::LocalAdhoc ? "ADHOC" : "ONLINE",
             hostEndpoint.c_str(), kLobbyPort, expectedSessionId);
    return true;
}

void LobbySession::Stop(RejectReason reason, bool notifyPeer) {
    if (mMode == Mode::Host) {
        if (notifyPeer) {
            std::vector<uint8_t> payload;
            PayloadWriter writer(payload);
            writer.U8(static_cast<uint8_t>(reason == RejectReason::None ? RejectReason::HostClosing : reason));
            for (Peer& peer : mHostPeers) {
                if (peer.socket != transport::kInvalidSocket && peer.joined) {
                    QueuePacket(peer, PacketType::Disconnect, 0, payload);
                    FlushPeer(peer);
                }
            }
        }
        for (Peer& peer : mHostPeers) {
                    transport::CloseReliable(mTransportMode, peer.socket);
            peer = Peer{};
        }
        transport::CloseReliable(mTransportMode, mListener);
    } else if (mMode == Mode::Client) {
        if (notifyPeer && mServerPeer.socket != transport::kInvalidSocket && mServerPeer.joined) {
            std::vector<uint8_t> payload;
            PayloadWriter writer(payload);
            writer.U8(static_cast<uint8_t>(reason));
            QueuePacket(mServerPeer, PacketType::LeaveSession, mView.localPlayerId, payload);
            FlushPeer(mServerPeer);
        }
        transport::CloseReliable(mTransportMode, mServerPeer.socket);
        mServerPeer = Peer{};
    }

    if (mMode != Mode::None) {
        port_log("[NETPLAY] lobby closed role=%s reason=%s\n",
                 mMode == Mode::Host ? "host" : "client", RejectReasonText(reason));
    }
    mMode = Mode::None;
    mView.connected = false;
    if (reason != RejectReason::None) {
        mView.lastMessage = RejectReasonText(reason);
    }
}

void LobbySession::Poll() {
    if (mMode == Mode::Host) PollHost();
    else if (mMode == Mode::Client) PollClient();
}

void LobbySession::AcceptClients() {
    for (int budget = 0; budget < 4; ++budget) {
        transport::SocketAddress address{};
        transport::SocketHandle socket = transport::AcceptReliable(mTransportMode, mListener, address);
        if (socket == transport::kInvalidSocket) {
            break;
        }

        if (ConnectedPlayerCount() >= kMaxPlayers) {
            Peer rejected{};
            rejected.socket = socket;
            rejected.address = address;
            RejectPeer(rejected, RejectReason::LobbyFull, "LOBBY FULL");
            transport::CloseReliable(mTransportMode, rejected.socket);
            continue;
        }

        Peer* freePeer = nullptr;
        for (Peer& peer : mHostPeers) {
            if (peer.socket == transport::kInvalidSocket) {
                freePeer = &peer;
                break;
            }
        }
        if (freePeer == nullptr) {
            port_log("[NETPLAY] rejected lobby connection endpoint=%s reason=no peer capacity\n",
                     transport::ToString(address).c_str());
            transport::CloseReliable(mTransportMode, socket);
            continue;
        }

        *freePeer = Peer{};
        freePeer->socket = socket;
        freePeer->address = address;
        freePeer->connectedAt = Clock::now();
        freePeer->lastRx = Clock::now();
        freePeer->nextHeartbeatAt = Clock::now() + kHeartbeatInterval;
        port_log("[NETPLAY] lobby %s accepted endpoint=%s\n",
                 mTransportMode == NetplayMode::LocalAdhoc ? "PTP" : "TCP",
                 transport::ToString(address).c_str());
    }
}

void LobbySession::PollHost() {
    AcceptClients();
    const auto now = Clock::now();

    for (std::size_t i = 0; i < mHostPeers.size(); ++i) {
        Peer& peer = mHostPeers[i];
        if (peer.socket == transport::kInvalidSocket) continue;

        if (!PollPeer(peer, true)) {
            RemovePeer(i, RejectReason::None, "CONNECTION LOST");
            continue;
        }
        if (!peer.joined && now - peer.connectedAt > kJoinTimeout) {
            RemovePeer(i, RejectReason::None, "JOIN TIMEOUT");
            continue;
        }
        if (peer.joined && now - peer.lastRx > kPeerTimeout) {
            port_log("[NETPLAY] player timeout slot=P%u ip=%s\n",
                     peer.playerId + 1, transport::ToString(peer.address).c_str());
            RemovePeer(i, RejectReason::None, "TIMEOUT");
            continue;
        }
        if (peer.joined && now >= peer.nextHeartbeatAt) {
            SendHeartbeat(peer);
        }
    }
    RefreshHostStatus();
}

void LobbySession::PollClient() {
    if (mServerPeer.socket == transport::kInvalidSocket) {
        Stop(RejectReason::None, false);
        mView.lastMessage = "CONNECTION LOST";
        return;
    }

    if (mServerPeer.connectPending) {
        if (transport::IsConnected(mTransportMode, mServerPeer.socket)) {
            mServerPeer.connectPending = false;
        } else {
        int socketError = 0;
        if (!transport::GetSocketError(mTransportMode, mServerPeer.socket, socketError)) {
            mView.lastMessage = "CONNECTION FAILED";
            Stop(RejectReason::None, false);
            return;
        }
        if (socketError != 0) {
            if (!transport::IsConnectPendingError(socketError)) {
                port_log("[NETPLAY] lobby connect completion failed error=%d\n", socketError);
                mView.lastMessage = "CONNECTION FAILED";
                Stop(RejectReason::None, false);
                return;
            }
        }
            if (Clock::now() - mServerPeer.connectedAt > kJoinTimeout) {
                mView.lastMessage = "CONNECTION TIMEOUT";
                Stop(RejectReason::None, false);
            }
            return;
        }
    }

    if (!mServerPeer.connectPending && !mServerPeer.joinSent) {
        std::vector<uint8_t> payload;
        PayloadWriter writer(payload);
        if (!writer.String(mBuildId, 24) || !writer.String(mLocalName, kMaxPlayerNameBytes)) {
            mView.lastMessage = "INVALID JOIN DATA";
            Stop(RejectReason::None, false);
            return;
        }
        QueuePacket(mServerPeer, PacketType::JoinRequest, 0xFF, payload);
        mServerPeer.joinSent = true;
        mView.lastMessage = "JOINING LOBBY";
    }

    if (!PollPeer(mServerPeer, false)) {
        port_log("[NETPLAY] host connection lost\n");
        if (mView.lastMessage.empty() || mView.lastMessage == "CONNECTING" ||
            mView.lastMessage == "JOINING LOBBY" || mView.lastMessage == "CONNECTED") {
            mView.lastMessage = "HOST DISCONNECTED";
        }
        Stop(RejectReason::None, false);
        return;
    }

    const auto now = Clock::now();
    if (!mServerPeer.joined && now - mServerPeer.connectedAt > kJoinTimeout) {
        mView.lastMessage = "JOIN TIMEOUT";
        Stop(RejectReason::None, false);
        return;
    }
    if (mServerPeer.joined && now - mServerPeer.lastRx > kPeerTimeout) {
        port_log("[NETPLAY] host heartbeat timeout\n");
        mView.lastMessage = "HOST TIMEOUT";
        Stop(RejectReason::None, false);
        return;
    }
    if (mServerPeer.joined && now >= mServerPeer.nextHeartbeatAt) {
        SendHeartbeat(mServerPeer);
    }
}

bool LobbySession::PollPeer(Peer& peer, bool hostSide) {
    if (!FlushPeer(peer)) return false;
    if (!ReceiveIntoPeer(peer)) return false;
    if (!DrainPackets(peer, hostSide)) return false;
    if (peer.remoteClosed) return false;
    return FlushPeer(peer);
}

bool LobbySession::ReceiveIntoPeer(Peer& peer) {
    std::array<uint8_t, 1536> buffer{};
    for (int budget = 0; budget < 8; ++budget) {
        const int received = transport::RecvReliable(mTransportMode, peer.socket, buffer.data(), buffer.size());
        if (received == 0) {
            peer.remoteClosed = true;
            return true;
        }
        if (received < 0) {
            if (transport::IsWouldBlock(received)) return true;
            port_log("[NETPLAY] reliable recv failed slot=%u error=0x%08X\n",
                     peer.playerId, static_cast<unsigned int>(transport::LastError()));
            return false;
        }
        if (peer.rx.size() + static_cast<std::size_t>(received) > kMaxTcpBuffer) {
            port_log("[NETPLAY] TCP receive buffer overflow slot=%u\n", peer.playerId);
            return false;
        }
        peer.rx.insert(peer.rx.end(), buffer.begin(), buffer.begin() + received);
        peer.lastRx = Clock::now();
    }
    return true;
}

bool LobbySession::DrainPackets(Peer& peer, bool hostSide) {
    int budget = 24;
    while (peer.rx.size() >= kWireHeaderBytes && budget-- > 0) {
        const uint16_t payloadLength = PeekPayloadLength(peer.rx);
        if (payloadLength > kMaxPayloadBytes) {
            port_log("[NETPLAY] malformed TCP payload length=%u slot=%u\n", payloadLength, peer.playerId);
            return false;
        }
        const std::size_t packetSize = kWireHeaderBytes + payloadLength;
        if (peer.rx.size() < packetSize) break;

        DecodedPacket packet;
        RejectReason reject = RejectReason::None;
        const bool decoded = DecodePacket(peer.rx.data(), packetSize, packet, &reject);
        peer.rx.erase(peer.rx.begin(), peer.rx.begin() + static_cast<std::ptrdiff_t>(packetSize));
        if (!decoded) {
            port_log("[NETPLAY] rejected TCP packet slot=%u reason=%s\n", peer.playerId, RejectReasonText(reject));
            if (hostSide && !peer.joined && reject == RejectReason::ProtocolMismatch) {
                RejectPeer(peer, reject, "PROTOCOL MISMATCH");
            }
            return false;
        }

        if (peer.joined) {
            if (packet.header.sessionId != mView.sessionId) {
                port_log("[NETPLAY] invalid session packet got=%08X expected=%08X\n",
                         packet.header.sessionId, mView.sessionId);
                return false;
            }
            const uint8_t expectedPlayerId = hostSide ? peer.playerId : 0;
            if (packet.header.playerId != expectedPlayerId) {
                port_log("[NETPLAY] invalid player packet got=%u expected=%u\n",
                         packet.header.playerId, expectedPlayerId);
                return false;
            }
        }

        if (packet.header.type == PacketType::Heartbeat) {
            HandleHeartbeat(peer, packet);
            continue;
        }
        if (hostSide) {
            if (!HandleHostPacket(peer, packet)) return false;
        } else {
            if (!HandleClientPacket(peer, packet)) return false;
        }
    }
    return true;
}

bool LobbySession::FlushPeer(Peer& peer) {
    while (!peer.tx.empty()) {
        std::vector<uint8_t>& packet = peer.tx.front();
        const std::size_t remaining = packet.size() - peer.txOffset;
        const int sent = transport::SendReliable(mTransportMode, peer.socket, packet.data() + peer.txOffset, remaining);
        if (sent < 0) {
            if (transport::IsWouldBlock(sent)) return true;
            port_log("[NETPLAY] reliable send failed slot=%u error=0x%08X\n",
                     peer.playerId, static_cast<unsigned int>(transport::LastError()));
            return false;
        }
        if (sent == 0) return false;
        peer.txOffset += static_cast<std::size_t>(sent);
        if (peer.txOffset == packet.size()) {
            peer.tx.pop_front();
            peer.txOffset = 0;
        }
    }
    return true;
}

void LobbySession::QueuePacket(Peer& peer, PacketType type, uint8_t playerId,
                               const std::vector<uint8_t>& payload, uint8_t flags) {
    PacketHeader header{};
    header.type = type;
    header.sessionId = (type == PacketType::JoinRequest) ? 0 : mView.sessionId;
    header.playerId = playerId;
    header.flags = flags;
    header.sequence = peer.nextSequence++;

    std::vector<uint8_t> packet;
    if (EncodePacket(header, payload, packet)) {
        peer.tx.push_back(std::move(packet));
    }
}

void LobbySession::Broadcast(PacketType type, uint8_t playerId, const std::vector<uint8_t>& payload) {
    for (Peer& peer : mHostPeers) {
        if (peer.socket != transport::kInvalidSocket && peer.joined) {
            QueuePacket(peer, type, playerId, payload);
        }
    }
}

void LobbySession::SendHeartbeat(Peer& peer) {
    const uint32_t nonce = peer.nextHeartbeat++;
    std::vector<uint8_t> payload;
    PayloadWriter writer(payload);
    if (!writer.U32(nonce)) return;

    const uint8_t sender = mMode == Mode::Host ? 0 : mView.localPlayerId;
    QueuePacket(peer, PacketType::Heartbeat, sender, payload, 0);
    peer.heartbeatSent[nonce] = Clock::now();
    peer.nextHeartbeatAt = Clock::now() + kHeartbeatInterval;
    if (peer.heartbeatSent.size() > 16) {
        for (auto it = peer.heartbeatSent.begin(); it != peer.heartbeatSent.end();) {
            if (Clock::now() - it->second > kPeerTimeout) it = peer.heartbeatSent.erase(it);
            else ++it;
        }
    }
}

void LobbySession::HandleHeartbeat(Peer& peer, const DecodedPacket& packet) {
    uint32_t nonce = 0;
    PayloadReader reader(packet.payload.data(), packet.payload.size());
    if (!reader.U32(nonce) || !reader.Empty()) return;

    if ((packet.header.flags & kHeartbeatReplyFlag) == 0) {
        std::vector<uint8_t> payload;
        PayloadWriter writer(payload);
        writer.U32(nonce);
        const uint8_t sender = mMode == Mode::Host ? 0 : mView.localPlayerId;
        QueuePacket(peer, PacketType::Heartbeat, sender, payload, kHeartbeatReplyFlag);
        return;
    }

    const auto it = peer.heartbeatSent.find(nonce);
    if (it == peer.heartbeatSent.end()) return;
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - it->second);
    const uint32_t sample = static_cast<uint32_t>(std::clamp<long long>(elapsed.count(), 0, 9999));
    const uint32_t previous = peer.pingMs;
    peer.pingMs = previous == 0 ? sample : ((previous * 3U) + sample) / 4U;
    const uint32_t delta = previous == 0 ? 0 : static_cast<uint32_t>(std::abs(static_cast<int>(sample) - static_cast<int>(previous)));
    peer.jitterMs = peer.jitterMs == 0 ? delta : ((peer.jitterMs * 3U) + delta) / 4U;
    peer.heartbeatSent.erase(it);

    if (mMode == Mode::Host && peer.playerId < kMaxPlayers) {
        mView.players[peer.playerId].pingMs = peer.pingMs;
        mView.players[peer.playerId].jitterMs = peer.jitterMs;
    } else if (mMode == Mode::Client) {
        mView.players[0].pingMs = peer.pingMs;
        mView.players[0].jitterMs = peer.jitterMs;
    }
}

bool LobbySession::HandleHostPacket(Peer& peer, const DecodedPacket& packet) {
    if (!peer.joined) {
        return packet.header.type == PacketType::JoinRequest && HandleJoinRequest(peer, packet);
    }

    if (packet.header.type == PacketType::ReadyState) {
        uint8_t playerId = 0xFF;
        uint8_t ready = 0;
        PayloadReader reader(packet.payload.data(), packet.payload.size());
        if (!reader.U8(playerId) || !reader.U8(ready) || !reader.Empty() ||
            playerId != peer.playerId || ready > 1) {
            return false;
        }
        mView.players[playerId].state = ready ? LobbySlotState::Ready : LobbySlotState::Connected;
        BroadcastReady(playerId);
        port_log("[NETPLAY] ready slot=P%u ready=%u\n", playerId + 1, ready);
        return true;
    }

    switch (packet.header.type) {
        case PacketType::CharacterCursorInput:
        case PacketType::CharacterLocked:
        case PacketType::CharacterUnlocked:
            return QueueSessionEvent(peer, packet, true);
        case PacketType::LoadingReady:
        case PacketType::StateHash:
            return QueueSessionEvent(peer, packet, false);
        default:
            break;
    }

    if (packet.header.type == PacketType::LeaveSession || packet.header.type == PacketType::Disconnect) {
        return false;
    }
    return false;
}

bool LobbySession::HandleClientPacket(Peer& peer, const DecodedPacket& packet) {
    if (!peer.joined) {
        if (packet.header.type == PacketType::JoinReject) {
            uint8_t reasonRaw = static_cast<uint8_t>(RejectReason::MalformedPacket);
            std::string_view message;
            PayloadReader reader(packet.payload.data(), packet.payload.size());
            if (reader.U8(reasonRaw) && reader.String(message, 48)) {
                mView.lastMessage.assign(message.begin(), message.end());
            } else {
                mView.lastMessage = "JOIN REJECTED";
            }
            port_log("[NETPLAY] join rejected reason=%u message=%s\n", reasonRaw, mView.lastMessage.c_str());
            return false;
        }
        if (packet.header.type != PacketType::JoinAccept) return false;
        if (mExpectedSessionId != 0 && packet.header.sessionId != mExpectedSessionId) {
            mView.lastMessage = "SESSION CHANGED";
            return false;
        }
        mView.sessionId = packet.header.sessionId;
        if (!ApplyLobbySnapshotPayload(packet.payload)) {
            return false;
        }
        peer.joined = true;
        peer.playerId = mView.localPlayerId;
        mView.connected = true;
        mView.lastMessage = "CONNECTED";
        port_log("[NETPLAY] joined lobby session=%08X slot=P%u host=%s\n",
                 mView.sessionId, mView.localPlayerId + 1, transport::ToString(peer.address).c_str());
        return true;
    }

    if (packet.header.type == PacketType::PlayerJoined) {
        uint8_t playerId = 0xFF;
        uint8_t stateRaw = 0;
        std::string_view name;
        PayloadReader reader(packet.payload.data(), packet.payload.size());
        if (!reader.U8(playerId) || !reader.U8(stateRaw) ||
            !reader.String(name, kMaxPlayerNameBytes) || !reader.Empty() ||
            playerId >= kMaxPlayers || stateRaw > static_cast<uint8_t>(LobbySlotState::Ready)) return false;
        mView.players[playerId].state = static_cast<LobbySlotState>(stateRaw);
        mView.players[playerId].name.assign(name.begin(), name.end());
        return true;
    }

    if (packet.header.type == PacketType::PlayerLeft) {
        uint8_t playerId = 0xFF;
        uint8_t reasonRaw = 0;
        PayloadReader reader(packet.payload.data(), packet.payload.size());
        if (!reader.U8(playerId) || !reader.U8(reasonRaw) || !reader.Empty() || playerId >= kMaxPlayers) return false;
        mView.players[playerId].state = LobbySlotState::Disconnected;
        mView.players[playerId].pingMs = 0;
        mView.players[playerId].jitterMs = 0;
        return true;
    }

    if (packet.header.type == PacketType::ReadyState) {
        uint8_t playerId = 0xFF;
        uint8_t ready = 0;
        PayloadReader reader(packet.payload.data(), packet.payload.size());
        if (!reader.U8(playerId) || !reader.U8(ready) || !reader.Empty() || playerId >= kMaxPlayers || ready > 1) return false;
        mView.players[playerId].state = ready ? LobbySlotState::Ready : LobbySlotState::Connected;
        return true;
    }

    if (packet.header.type == PacketType::StartCharacterSelect) {
        mView.status = LobbyStatus::Starting;
        mCharacterSelectStartPending = true;
        mView.lastMessage = "STARTING";
        port_log("[NETPLAY] host started character select\n");
        return true;
    }

    switch (packet.header.type) {
        case PacketType::CharacterCursorInput:
        case PacketType::CharacterLocked:
        case PacketType::CharacterUnlocked:
        case PacketType::MatchConfiguration:
        case PacketType::StartMatch:
        case PacketType::StateHash:
        case PacketType::MatchResult:
        case PacketType::Rematch:
        case PacketType::ReturnToCharacterSelect:
        case PacketType::LeaveSession:
            return QueueSessionEvent(peer, packet, false);
        default:
            break;
    }

    if (packet.header.type == PacketType::Disconnect) {
        uint8_t reasonRaw = static_cast<uint8_t>(RejectReason::HostClosing);
        PayloadReader reader(packet.payload.data(), packet.payload.size());
        reader.U8(reasonRaw);
        mView.lastMessage = RejectReasonText(static_cast<RejectReason>(reasonRaw));
        return false;
    }
    return false;
}

bool LobbySession::HandleJoinRequest(Peer& peer, const DecodedPacket& packet) {
    if (packet.header.sessionId != 0 || packet.header.playerId != 0xFF || mView.status != LobbyStatus::Open) {
        RejectPeer(peer, mView.status == LobbyStatus::Open ? RejectReason::MalformedPacket : RejectReason::MatchStarted,
                   mView.status == LobbyStatus::Open ? "INVALID JOIN" : "MATCH STARTED");
        return false;
    }

    std::string_view build;
    std::string_view name;
    PayloadReader reader(packet.payload.data(), packet.payload.size());
    if (!reader.String(build, 24) || !reader.String(name, kMaxPlayerNameBytes) || !reader.Empty() || name.empty()) {
        RejectPeer(peer, RejectReason::MalformedPacket, "INVALID JOIN DATA");
        return false;
    }
    if (build != mBuildId) {
        RejectPeer(peer, RejectReason::BuildMismatch, "BUILD MISMATCH");
        port_log("[NETPLAY] build mismatch remote=%.*s local=%s\n",
                 static_cast<int>(build.size()), build.data(), mBuildId.c_str());
        return false;
    }

    const int slot = FindFreeSlot();
    if (slot < 0) {
        RejectPeer(peer, RejectReason::LobbyFull, "LOBBY FULL");
        return false;
    }

    peer.playerId = static_cast<uint8_t>(slot);
    peer.joined = true;
    peer.lastRx = Clock::now();
    mView.players[slot].state = LobbySlotState::Connected;
    mView.players[slot].name.assign(name.begin(), name.end());
    mView.players[slot].ip = transport::ToString(peer.address);
    mView.players[slot].pingMs = 0;
    mView.players[slot].jitterMs = 0;
    RefreshHostStatus();

    const std::vector<uint8_t> snapshot = MakeLobbySnapshotPayload(peer.playerId);
    QueuePacket(peer, PacketType::JoinAccept, 0, snapshot);
    BroadcastPlayerJoined(peer.playerId);
    port_log("[NETPLAY] player joined slot=P%u name=%s ip=%s players=%u/%u\n",
             peer.playerId + 1, mView.players[slot].name.c_str(), mView.players[slot].ip.c_str(),
             ConnectedPlayerCount(), static_cast<unsigned>(kMaxPlayers));
    return true;
}

void LobbySession::RejectPeer(Peer& peer, RejectReason reason, const char* message) {
    std::vector<uint8_t> payload;
    PayloadWriter writer(payload);
    writer.U8(static_cast<uint8_t>(reason));
    writer.String(message != nullptr ? message : RejectReasonText(reason), 48);
    QueuePacket(peer, PacketType::JoinReject, 0, payload);
    FlushPeer(peer);
    port_log("[NETPLAY] join rejected ip=%s reason=%s\n",
             transport::ToString(peer.address).c_str(), RejectReasonText(reason));
}

void LobbySession::RemovePeer(std::size_t index, RejectReason reason, const char* message) {
    Peer& peer = mHostPeers[index];
    if (peer.socket == transport::kInvalidSocket) return;
    const uint8_t playerId = peer.playerId;
    const bool wasJoined = peer.joined && playerId < kMaxPlayers;
    const std::string ip = transport::ToString(peer.address);
    transport::CloseReliable(mTransportMode, peer.socket);
    peer = Peer{};

    if (wasJoined) {
        mView.players[playerId].state = LobbySlotState::Disconnected;
        mView.players[playerId].pingMs = 0;
        mView.players[playerId].jitterMs = 0;
        BroadcastPlayerLeft(playerId, reason);
        port_log("[NETPLAY] player left slot=P%u ip=%s reason=%s detail=%s\n",
                 playerId + 1, ip.c_str(), RejectReasonText(reason), message != nullptr ? message : "none");
        RefreshHostStatus();
    }
}

int LobbySession::FindFreeSlot() const {
    for (std::size_t i = 1; i < kMaxPlayers; ++i) {
        if (mView.players[i].state == LobbySlotState::Empty ||
            mView.players[i].state == LobbySlotState::Disconnected) {
            bool occupiedByPeer = false;
            for (const Peer& peer : mHostPeers) {
                if (peer.joined && peer.playerId == i) {
                    occupiedByPeer = true;
                    break;
                }
            }
            if (!occupiedByPeer) return static_cast<int>(i);
        }
    }
    return -1;
}

void LobbySession::RefreshHostStatus() {
    if (mMode != Mode::Host || mView.status == LobbyStatus::Starting || mView.status == LobbyStatus::InGame) return;
    mView.status = ConnectedPlayerCount() >= kMaxPlayers ? LobbyStatus::Full : LobbyStatus::Open;
}

std::vector<uint8_t> LobbySession::MakeLobbySnapshotPayload(uint8_t assignedPlayer) const {
    std::vector<uint8_t> payload;
    PayloadWriter writer(payload);
    if (!writer.U8(assignedPlayer) || !writer.U8(static_cast<uint8_t>(mView.status))) return {};
    for (const LobbyPlayerView& player : mView.players) {
        if (!writer.U8(static_cast<uint8_t>(player.state)) ||
            !writer.String(player.name, kMaxPlayerNameBytes)) return {};
    }
    return payload;
}

bool LobbySession::ApplyLobbySnapshotPayload(const std::vector<uint8_t>& payload) {
    uint8_t assigned = 0xFF;
    uint8_t statusRaw = 0;
    PayloadReader reader(payload.data(), payload.size());
    if (!reader.U8(assigned) || !reader.U8(statusRaw) || assigned >= kMaxPlayers ||
        statusRaw > static_cast<uint8_t>(LobbyStatus::InGame)) return false;

    std::array<LobbyPlayerView, kMaxPlayers> players{};
    for (uint8_t i = 0; i < kMaxPlayers; ++i) {
        uint8_t stateRaw = 0;
        std::string_view name;
        if (!reader.U8(stateRaw) || !reader.String(name, kMaxPlayerNameBytes) ||
            stateRaw > static_cast<uint8_t>(LobbySlotState::Disconnected)) return false;
        players[i].playerId = i;
        players[i].state = static_cast<LobbySlotState>(stateRaw);
        players[i].name.assign(name.begin(), name.end());
    }
    if (!reader.Empty()) return false;

    mView.localPlayerId = assigned;
    mView.status = static_cast<LobbyStatus>(statusRaw);
    mView.players = std::move(players);
    return true;
}

void LobbySession::BroadcastPlayerJoined(uint8_t playerId) {
    if (playerId >= kMaxPlayers) return;
    std::vector<uint8_t> payload;
    PayloadWriter writer(payload);
    writer.U8(playerId);
    writer.U8(static_cast<uint8_t>(mView.players[playerId].state));
    writer.String(mView.players[playerId].name, kMaxPlayerNameBytes);
    Broadcast(PacketType::PlayerJoined, 0, payload);
}

void LobbySession::BroadcastPlayerLeft(uint8_t playerId, RejectReason reason) {
    std::vector<uint8_t> payload;
    PayloadWriter writer(payload);
    writer.U8(playerId);
    writer.U8(static_cast<uint8_t>(reason));
    Broadcast(PacketType::PlayerLeft, 0, payload);
}

void LobbySession::BroadcastReady(uint8_t playerId) {
    if (playerId >= kMaxPlayers) return;
    std::vector<uint8_t> payload;
    PayloadWriter writer(payload);
    writer.U8(playerId);
    writer.U8(mView.players[playerId].state == LobbySlotState::Ready ? 1 : 0);
    Broadcast(PacketType::ReadyState, 0, payload);
}

bool LobbySession::SetLocalReady(bool ready) {
    if (mMode == Mode::Host) {
        mView.players[0].state = ready ? LobbySlotState::Ready : LobbySlotState::Connected;
        BroadcastReady(0);
        port_log("[NETPLAY] host ready=%d\n", ready ? 1 : 0);
        return true;
    }
    if (mMode != Mode::Client || !mServerPeer.joined || mView.localPlayerId >= kMaxPlayers) return false;

    mView.players[mView.localPlayerId].state = ready ? LobbySlotState::Ready : LobbySlotState::Connected;
    std::vector<uint8_t> payload;
    PayloadWriter writer(payload);
    writer.U8(mView.localPlayerId);
    writer.U8(ready ? 1 : 0);
    QueuePacket(mServerPeer, PacketType::ReadyState, mView.localPlayerId, payload);
    return true;
}

bool LobbySession::LocalReady() const {
    return mView.localPlayerId < kMaxPlayers &&
           mView.players[mView.localPlayerId].state == LobbySlotState::Ready;
}

uint8_t LobbySession::ConnectedPlayerCount() const {
    uint8_t count = 0;
    for (const LobbyPlayerView& player : mView.players) {
        if (player.state == LobbySlotState::Connected || player.state == LobbySlotState::Ready) ++count;
    }
    return count;
}

bool LobbySession::CanHostStart() const {
    if (mMode != Mode::Host || ConnectedPlayerCount() < 2 || mView.status == LobbyStatus::Starting) return false;
    for (const LobbyPlayerView& player : mView.players) {
        if (player.state == LobbySlotState::Connected) return false;
    }
    return true;
}

bool LobbySession::StartCharacterSelect() {
    if (!CanHostStart()) return false;
    mView.status = LobbyStatus::Starting;
    mView.lastMessage = "STARTING";
    Broadcast(PacketType::StartCharacterSelect, 0);
    mCharacterSelectStartPending = true;
    port_log("[NETPLAY] host starting character select players=%u\n", ConnectedPlayerCount());
    return true;
}

bool LobbySession::SendSessionMessage(PacketType type, const std::vector<uint8_t>& payload) {
    if (!mView.connected || mMode == Mode::None) return false;

    if (mMode == Mode::Host) {
        Broadcast(type, 0, payload);
        return true;
    }
    if (mServerPeer.socket == transport::kInvalidSocket || !mServerPeer.joined ||
        mView.localPlayerId >= kMaxPlayers) {
        return false;
    }
    QueuePacket(mServerPeer, type, mView.localPlayerId, payload);
    return true;
}

bool LobbySession::PopSessionEvent(LobbySessionEvent& event) {
    if (mSessionEvents.empty()) return false;
    event = std::move(mSessionEvents.front());
    mSessionEvents.pop_front();
    return true;
}

bool LobbySession::QueueSessionEvent(const Peer& peer, const DecodedPacket& packet, bool relayFromClient) {
    uint8_t source = packet.header.playerId;

    if (mMode == Mode::Host) {
        if (peer.playerId >= kMaxPlayers || source != peer.playerId) return false;
    } else {
        if (packet.header.playerId != 0) return false;
        source = 0;
    }

    if (relayFromClient && mMode == Mode::Host) {
        if (packet.payload.empty() || packet.payload[0] != peer.playerId) return false;
        Broadcast(packet.header.type, 0, packet.payload);
    }

    if (mSessionEvents.size() >= 128) {
        mSessionEvents.pop_front();
    }
    mSessionEvents.push_back(LobbySessionEvent{packet.header.type, source, packet.payload});
    return true;
}

bool LobbySession::ConsumeCharacterSelectStart() {
    const bool pending = mCharacterSelectStartPending;
    mCharacterSelectStartPending = false;
    return pending;
}

} // namespace ssb64::netplay
