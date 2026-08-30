#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace ssb64::netplay {

constexpr uint32_t kProtocolMagic = 0x42534E50U; // "BSNP"
constexpr uint16_t kProtocolVersion = 2;
// Keep the Vita LAN sockets in the same conventional range used by Quake
// ports rather than at the very top of the 16-bit port space.
constexpr uint16_t kDiscoveryPort = 26040;
constexpr uint16_t kLobbyPort = 26041;
constexpr uint16_t kGameplayPort = 26042;
constexpr uint16_t kRendezvousPort = 26050;
constexpr std::size_t kMaxPlayers = 4;
constexpr std::size_t kMaxPlayerNameBytes = 16;
constexpr std::size_t kMaxPayloadBytes = 768;
constexpr std::size_t kWireHeaderBytes = 28;

// All values are explicit protocol values. Never serialize this enum or any
// C/C++ struct directly; NetplayProtocol.cpp emits fixed-width big-endian
// fields so Vita/desktop padding and host endian are irrelevant.
enum class PacketType : uint16_t {
    DiscoveryRequest = 1,
    DiscoveryResponse,
    JoinRequest,
    JoinAccept,
    JoinReject,
    PlayerJoined,
    PlayerLeft,
    ReadyState,
    StartCharacterSelect,
    CharacterCursorInput,
    CharacterLocked,
    CharacterUnlocked,
    MatchConfiguration,
    LoadingReady,
    StartMatch,
    FrameInput,
    Heartbeat,
    Disconnect,
    StateHash,
    MatchResult,
    Rematch,
    ReturnToCharacterSelect,
    LeaveSession,
    LobbyRules,
    ReturnToLobby,
};

enum class NetplayState : uint8_t {
    Offline = 0,
    Discovering,
    Connecting,
    HostingLobby,
    ClientLobby,
    CharacterSelect,
    LoadingMatch,
    InMatch,
    Results,
    Disconnected,
    Error,
};

// User-facing connection type.  LOCAL_ADHOC uses the Vita's native
// ScePspnetAdhoc MAC/PDP/PTP stack and never requires an infrastructure AP.
// ONLINE keeps the existing IP socket transport and can later grow remote
// matchmaking/NAT traversal without changing the game/session protocol.
enum class NetplayMode : uint8_t {
    None = 0,
    LocalAdhoc,
    Online,
};

enum class LobbyStatus : uint8_t {
    Open = 0,
    Full,
    Starting,
    InGame,
};

enum class RejectReason : uint8_t {
    None = 0,
    ProtocolMismatch,
    BuildMismatch,
    LobbyFull,
    MatchStarted,
    MalformedPacket,
    InvalidSession,
    InvalidPlayer,
    HostClosing,
};

enum class MatchResultReason : uint8_t {
    Completed = 0,
    NoContest,
    PeerDisconnected,
    DesyncAbort,
};

struct MatchResultPayload {
    uint32_t matchId = 0;
    uint32_t finalFrame = 0;
    uint8_t winner = 0xFF;
    MatchResultReason reason = MatchResultReason::Completed;
    std::array<uint8_t, kMaxPlayers> placements{0xFF, 0xFF, 0xFF, 0xFF};
    std::array<int8_t, kMaxPlayers> stocksRemaining{-1, -1, -1, -1};
    bool hasFinalHash = false;
    uint64_t finalHash = 0;
};

struct RematchPayload {
    uint32_t matchId = 0;
    uint32_t rngSeed = 0;
};

struct PacketHeader {
    uint16_t protocolVersion = kProtocolVersion;
    PacketType type = PacketType::Heartbeat;
    uint32_t sessionId = 0;
    uint8_t playerId = 0xFF;
    uint8_t flags = 0;
    uint32_t sequence = 0;
    uint32_t frame = 0;
    uint16_t payloadLength = 0;
};

struct DecodedPacket {
    PacketHeader header{};
    std::vector<uint8_t> payload;
};

// Encode/decode the common wire envelope. The checksum covers every header
// field except the checksum field itself plus the exact payload bytes.
bool EncodePacket(const PacketHeader& header, const uint8_t* payload,
                  std::size_t payloadSize, std::vector<uint8_t>& out);
bool EncodePacket(const PacketHeader& header, const std::vector<uint8_t>& payload,
                  std::vector<uint8_t>& out);
bool DecodePacket(const uint8_t* data, std::size_t size, DecodedPacket& out,
                  RejectReason* rejectReason = nullptr);
// Discovery uses this variant only to surface incompatible protocol versions
// in the lobby list. It still validates magic, packet type, payload bounds and
// checksum; callers must never use it to accept gameplay/control traffic.
bool DecodePacketAnyVersion(const uint8_t* data, std::size_t size, DecodedPacket& out,
                            RejectReason* rejectReason = nullptr);

uint32_t ProtocolChecksum(const uint8_t* data, std::size_t size,
                          const uint8_t* payload, std::size_t payloadSize);
const char* PacketTypeName(PacketType type);
const char* StateName(NetplayState state);
const char* LobbyStatusName(LobbyStatus status);
bool EncodeMatchResultPayload(const MatchResultPayload& result, std::vector<uint8_t>& payload);
bool DecodeMatchResultPayload(const std::vector<uint8_t>& payload, MatchResultPayload& result);
bool EncodeRematchPayload(const RematchPayload& rematch, std::vector<uint8_t>& payload);
bool DecodeRematchPayload(const std::vector<uint8_t>& payload, RematchPayload& rematch);

// Helpers for explicit payload serialization. They perform bounds checks and
// keep protocol code independent from host endian/alignment.
class PayloadWriter {
public:
    explicit PayloadWriter(std::vector<uint8_t>& bytes) : mBytes(bytes) {}

    bool U8(uint8_t value);
    bool U16(uint16_t value);
    bool U32(uint32_t value);
    bool S8(int8_t value);
    bool String(std::string_view value, std::size_t maxBytes = kMaxPlayerNameBytes);

private:
    bool Reserve(std::size_t count) const;
    std::vector<uint8_t>& mBytes;
};

class PayloadReader {
public:
    PayloadReader(const uint8_t* bytes, std::size_t size) : mBytes(bytes), mSize(size) {}

    bool U8(uint8_t& value);
    bool U16(uint16_t& value);
    bool U32(uint32_t& value);
    bool S8(int8_t& value);
    bool String(std::string_view& value, std::size_t maxBytes = kMaxPlayerNameBytes);
    bool Empty() const { return mOffset == mSize; }
    std::size_t Remaining() const { return mSize - mOffset; }

private:
    bool Take(std::size_t count, const uint8_t*& begin);
    const uint8_t* mBytes = nullptr;
    std::size_t mSize = 0;
    std::size_t mOffset = 0;
};

} // namespace ssb64::netplay
