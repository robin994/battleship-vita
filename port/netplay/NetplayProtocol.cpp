#include "NetplayProtocol.h"

#include <algorithm>
#include <cstring>

namespace ssb64::netplay {
namespace {

constexpr std::size_t kChecksumOffset = 24;
constexpr uint32_t kFnvOffset = 2166136261U;
constexpr uint32_t kFnvPrime = 16777619U;

void WriteU16(std::vector<uint8_t>& out, uint16_t value) {
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFFU));
    out.push_back(static_cast<uint8_t>(value & 0xFFU));
}

void WriteU32(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>((value >> 24) & 0xFFU));
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xFFU));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFFU));
    out.push_back(static_cast<uint8_t>(value & 0xFFU));
}

uint16_t ReadU16(const uint8_t* ptr) {
    return static_cast<uint16_t>((static_cast<uint16_t>(ptr[0]) << 8) |
                                 static_cast<uint16_t>(ptr[1]));
}

uint32_t ReadU32(const uint8_t* ptr) {
    return (static_cast<uint32_t>(ptr[0]) << 24) |
           (static_cast<uint32_t>(ptr[1]) << 16) |
           (static_cast<uint32_t>(ptr[2]) << 8) |
           static_cast<uint32_t>(ptr[3]);
}

uint32_t Accumulate(uint32_t hash, const uint8_t* data, std::size_t size) {
    for (std::size_t i = 0; i < size; ++i) {
        hash ^= data[i];
        hash *= kFnvPrime;
    }
    return hash;
}

bool IsKnownPacketType(uint16_t raw) {
    return raw >= static_cast<uint16_t>(PacketType::DiscoveryRequest) &&
           raw <= static_cast<uint16_t>(PacketType::ReturnToLobby);
}

} // namespace

uint32_t ProtocolChecksum(const uint8_t* data, std::size_t size,
                          const uint8_t* payload, std::size_t payloadSize) {
    uint32_t hash = kFnvOffset;
    if (data != nullptr && size != 0) {
        hash = Accumulate(hash, data, size);
    }
    if (payload != nullptr && payloadSize != 0) {
        hash = Accumulate(hash, payload, payloadSize);
    }
    return hash;
}

bool EncodePacket(const PacketHeader& header, const uint8_t* payload,
                  std::size_t payloadSize, std::vector<uint8_t>& out) {
    if (payloadSize > kMaxPayloadBytes || payloadSize > 0xFFFFU ||
        (payloadSize != 0 && payload == nullptr)) {
        return false;
    }

    out.clear();
    out.reserve(kWireHeaderBytes + payloadSize);
    WriteU32(out, kProtocolMagic);
    WriteU16(out, header.protocolVersion);
    WriteU16(out, static_cast<uint16_t>(header.type));
    WriteU32(out, header.sessionId);
    out.push_back(header.playerId);
    out.push_back(header.flags);
    WriteU32(out, header.sequence);
    WriteU32(out, header.frame);
    WriteU16(out, static_cast<uint16_t>(payloadSize));

    // Checksum is patched after the payload has been appended.
    WriteU32(out, 0);
    if (payloadSize != 0) {
        out.insert(out.end(), payload, payload + payloadSize);
    }

    const uint32_t checksum = ProtocolChecksum(out.data(), kChecksumOffset,
                                                payloadSize ? out.data() + kWireHeaderBytes : nullptr,
                                                payloadSize);
    out[kChecksumOffset + 0] = static_cast<uint8_t>((checksum >> 24) & 0xFFU);
    out[kChecksumOffset + 1] = static_cast<uint8_t>((checksum >> 16) & 0xFFU);
    out[kChecksumOffset + 2] = static_cast<uint8_t>((checksum >> 8) & 0xFFU);
    out[kChecksumOffset + 3] = static_cast<uint8_t>(checksum & 0xFFU);
    return true;
}

bool EncodePacket(const PacketHeader& header, const std::vector<uint8_t>& payload,
                  std::vector<uint8_t>& out) {
    return EncodePacket(header, payload.empty() ? nullptr : payload.data(), payload.size(), out);
}

static bool DecodePacketInternal(const uint8_t* data, std::size_t size, DecodedPacket& out,
                                 RejectReason* rejectReason, bool requireCurrentVersion) {
    auto reject = [rejectReason](RejectReason reason) {
        if (rejectReason != nullptr) {
            *rejectReason = reason;
        }
        return false;
    };

    if (data == nullptr || size < kWireHeaderBytes) {
        return reject(RejectReason::MalformedPacket);
    }

    const uint32_t magic = ReadU32(data + 0);
    const uint16_t version = ReadU16(data + 4);
    const uint16_t rawType = ReadU16(data + 6);
    const uint16_t payloadLength = ReadU16(data + 22);
    const uint32_t checksum = ReadU32(data + kChecksumOffset);

    if (magic != kProtocolMagic || !IsKnownPacketType(rawType)) {
        return reject(RejectReason::MalformedPacket);
    }
    if (requireCurrentVersion && version != kProtocolVersion) {
        return reject(RejectReason::ProtocolMismatch);
    }
    if (payloadLength > kMaxPayloadBytes ||
        size != kWireHeaderBytes + static_cast<std::size_t>(payloadLength)) {
        return reject(RejectReason::MalformedPacket);
    }

    const uint8_t* payload = payloadLength ? data + kWireHeaderBytes : nullptr;
    const uint32_t expected = ProtocolChecksum(data, kChecksumOffset, payload, payloadLength);
    if (checksum != expected) {
        return reject(RejectReason::MalformedPacket);
    }

    out.header.protocolVersion = version;
    out.header.type = static_cast<PacketType>(rawType);
    out.header.sessionId = ReadU32(data + 8);
    out.header.playerId = data[12];
    out.header.flags = data[13];
    out.header.sequence = ReadU32(data + 14);
    out.header.frame = ReadU32(data + 18);
    out.header.payloadLength = payloadLength;
    out.payload.assign(payloadLength ? payload : data + kWireHeaderBytes,
                       payloadLength ? payload + payloadLength : data + kWireHeaderBytes);

    if (rejectReason != nullptr) {
        *rejectReason = RejectReason::None;
    }
    return true;
}

bool DecodePacket(const uint8_t* data, std::size_t size, DecodedPacket& out,
                  RejectReason* rejectReason) {
    return DecodePacketInternal(data, size, out, rejectReason, true);
}

bool DecodePacketAnyVersion(const uint8_t* data, std::size_t size, DecodedPacket& out,
                            RejectReason* rejectReason) {
    return DecodePacketInternal(data, size, out, rejectReason, false);
}

bool PayloadWriter::Reserve(std::size_t count) const {
    return count <= kMaxPayloadBytes && mBytes.size() <= kMaxPayloadBytes - count;
}

bool PayloadWriter::U8(uint8_t value) {
    if (!Reserve(1)) return false;
    mBytes.push_back(value);
    return true;
}

bool PayloadWriter::U16(uint16_t value) {
    if (!Reserve(2)) return false;
    WriteU16(mBytes, value);
    return true;
}

bool PayloadWriter::U32(uint32_t value) {
    if (!Reserve(4)) return false;
    WriteU32(mBytes, value);
    return true;
}

bool PayloadWriter::S8(int8_t value) {
    return U8(static_cast<uint8_t>(value));
}

bool PayloadWriter::String(std::string_view value, std::size_t maxBytes) {
    const std::size_t length = std::min(value.size(), maxBytes);
    if (length > 0xFFU || !Reserve(1 + length)) return false;
    mBytes.push_back(static_cast<uint8_t>(length));
    mBytes.insert(mBytes.end(), value.begin(), value.begin() + static_cast<std::ptrdiff_t>(length));
    return true;
}

bool PayloadReader::Take(std::size_t count, const uint8_t*& begin) {
    if (count > Remaining()) return false;
    begin = mBytes + mOffset;
    mOffset += count;
    return true;
}

bool PayloadReader::U8(uint8_t& value) {
    const uint8_t* ptr;
    if (!Take(1, ptr)) return false;
    value = ptr[0];
    return true;
}

bool PayloadReader::U16(uint16_t& value) {
    const uint8_t* ptr;
    if (!Take(2, ptr)) return false;
    value = ReadU16(ptr);
    return true;
}

bool PayloadReader::U32(uint32_t& value) {
    const uint8_t* ptr;
    if (!Take(4, ptr)) return false;
    value = ReadU32(ptr);
    return true;
}

bool PayloadReader::S8(int8_t& value) {
    uint8_t raw;
    if (!U8(raw)) return false;
    value = static_cast<int8_t>(raw);
    return true;
}

bool PayloadReader::String(std::string_view& value, std::size_t maxBytes) {
    uint8_t length;
    if (!U8(length) || length > maxBytes) return false;
    const uint8_t* ptr;
    if (!Take(length, ptr)) return false;
    value = std::string_view(reinterpret_cast<const char*>(ptr), length);
    return true;
}

bool EncodeMatchResultPayload(const MatchResultPayload& result, std::vector<uint8_t>& payload) {
    if (result.matchId == 0 || (result.winner != 0xFF && result.winner >= kMaxPlayers) ||
        static_cast<uint8_t>(result.reason) > static_cast<uint8_t>(MatchResultReason::DesyncAbort)) {
        return false;
    }
    payload.clear();
    PayloadWriter writer(payload);
    if (!writer.U32(result.matchId) || !writer.U32(result.finalFrame) || !writer.U8(result.winner) ||
        !writer.U8(static_cast<uint8_t>(result.reason)) || !writer.U8(result.hasFinalHash ? 1 : 0)) {
        return false;
    }
    for (std::size_t player = 0; player < kMaxPlayers; ++player) {
        if ((result.placements[player] != 0xFF && result.placements[player] >= kMaxPlayers) ||
            result.stocksRemaining[player] < -1 || result.stocksRemaining[player] > 99 ||
            !writer.U8(result.placements[player]) || !writer.S8(result.stocksRemaining[player])) {
            payload.clear();
            return false;
        }
    }
    return writer.U32(static_cast<uint32_t>(result.finalHash >> 32)) &&
           writer.U32(static_cast<uint32_t>(result.finalHash));
}

bool DecodeMatchResultPayload(const std::vector<uint8_t>& payload, MatchResultPayload& result) {
    MatchResultPayload decoded{};
    uint8_t reason = 0;
    uint8_t hasHash = 0;
    uint32_t hashHigh = 0;
    uint32_t hashLow = 0;
    PayloadReader reader(payload.data(), payload.size());
    if (!reader.U32(decoded.matchId) || !reader.U32(decoded.finalFrame) || !reader.U8(decoded.winner) ||
        !reader.U8(reason) || !reader.U8(hasHash) || decoded.matchId == 0 ||
        (decoded.winner != 0xFF && decoded.winner >= kMaxPlayers) ||
        reason > static_cast<uint8_t>(MatchResultReason::DesyncAbort) || hasHash > 1) {
        return false;
    }
    for (std::size_t player = 0; player < kMaxPlayers; ++player) {
        if (!reader.U8(decoded.placements[player]) || !reader.S8(decoded.stocksRemaining[player]) ||
            (decoded.placements[player] != 0xFF && decoded.placements[player] >= kMaxPlayers) ||
            decoded.stocksRemaining[player] < -1 || decoded.stocksRemaining[player] > 99) {
            return false;
        }
    }
    if (!reader.U32(hashHigh) || !reader.U32(hashLow) || !reader.Empty()) return false;
    decoded.reason = static_cast<MatchResultReason>(reason);
    decoded.hasFinalHash = hasHash != 0;
    decoded.finalHash = (static_cast<uint64_t>(hashHigh) << 32) | hashLow;
    result = decoded;
    return true;
}

bool EncodeRematchPayload(const RematchPayload& rematch, std::vector<uint8_t>& payload) {
    if (rematch.matchId == 0 || rematch.rngSeed == 0) return false;
    payload.clear();
    PayloadWriter writer(payload);
    return writer.U32(rematch.matchId) && writer.U32(rematch.rngSeed);
}

bool DecodeRematchPayload(const std::vector<uint8_t>& payload, RematchPayload& rematch) {
    RematchPayload decoded{};
    PayloadReader reader(payload.data(), payload.size());
    if (!reader.U32(decoded.matchId) || !reader.U32(decoded.rngSeed) || !reader.Empty() ||
        decoded.matchId == 0 || decoded.rngSeed == 0) {
        return false;
    }
    rematch = decoded;
    return true;
}

const char* PacketTypeName(PacketType type) {
    switch (type) {
        case PacketType::DiscoveryRequest: return "DISCOVERY_REQUEST";
        case PacketType::DiscoveryResponse: return "DISCOVERY_RESPONSE";
        case PacketType::JoinRequest: return "JOIN_REQUEST";
        case PacketType::JoinAccept: return "JOIN_ACCEPT";
        case PacketType::JoinReject: return "JOIN_REJECT";
        case PacketType::PlayerJoined: return "PLAYER_JOINED";
        case PacketType::PlayerLeft: return "PLAYER_LEFT";
        case PacketType::ReadyState: return "READY_STATE";
        case PacketType::StartCharacterSelect: return "START_CHARACTER_SELECT";
        case PacketType::CharacterCursorInput: return "CHARACTER_CURSOR_INPUT";
        case PacketType::CharacterLocked: return "CHARACTER_LOCKED";
        case PacketType::CharacterUnlocked: return "CHARACTER_UNLOCKED";
        case PacketType::MatchConfiguration: return "MATCH_CONFIGURATION";
        case PacketType::LoadingReady: return "LOADING_READY";
        case PacketType::StartMatch: return "START_MATCH";
        case PacketType::FrameInput: return "FRAME_INPUT";
        case PacketType::Heartbeat: return "HEARTBEAT";
        case PacketType::Disconnect: return "DISCONNECT";
        case PacketType::StateHash: return "STATE_HASH";
        case PacketType::MatchResult: return "MATCH_RESULT";
        case PacketType::Rematch: return "REMATCH";
        case PacketType::ReturnToCharacterSelect: return "RETURN_TO_CHARACTER_SELECT";
        case PacketType::LeaveSession: return "LEAVE_SESSION";
        case PacketType::LobbyRules: return "LOBBY_RULES";
        case PacketType::ReturnToLobby: return "RETURN_TO_LOBBY";
    }
    return "UNKNOWN";
}

const char* StateName(NetplayState state) {
    switch (state) {
        case NetplayState::Offline: return "OFFLINE";
        case NetplayState::Discovering: return "DISCOVERING";
        case NetplayState::Connecting: return "CONNECTING";
        case NetplayState::HostingLobby: return "HOSTING_LOBBY";
        case NetplayState::ClientLobby: return "CLIENT_LOBBY";
        case NetplayState::CharacterSelect: return "CHARACTER_SELECT";
        case NetplayState::LoadingMatch: return "LOADING_MATCH";
        case NetplayState::InMatch: return "IN_MATCH";
        case NetplayState::Results: return "RESULTS";
        case NetplayState::Disconnected: return "DISCONNECTED";
        case NetplayState::Error: return "ERROR";
    }
    return "UNKNOWN";
}

const char* LobbyStatusName(LobbyStatus status) {
    switch (status) {
        case LobbyStatus::Open: return "OPEN";
        case LobbyStatus::Full: return "FULL";
        case LobbyStatus::Starting: return "STARTING";
        case LobbyStatus::InGame: return "IN GAME";
    }
    return "UNKNOWN";
}

} // namespace ssb64::netplay
