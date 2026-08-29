#include "GameplaySession.h"

#include "../port_log.h"

#include <algorithm>
#include <limits>

namespace ssb64::netplay {
namespace {
constexpr auto kKeepaliveInterval = std::chrono::milliseconds(200);
constexpr std::size_t kReceiveBudget = 48;
}

bool GameplaySession::SameAddress(const transport::SocketAddress& a, const transport::SocketAddress& b) {
    if (a.isAdhoc != b.isAdhoc || a.port != b.port) return false;
    if (a.isAdhoc) return a.mac == b.mac;
    return a.ipv4 == b.ipv4;
}

void GameplaySession::ResetState() {
    mPeerAddresses = {};
    mPeerKnown.fill(false);
    mRecvSequences = {};
    for (auto& player : mRemoteSeen) player.fill(std::numeric_limits<uint32_t>::max());
    mLocalHistory = {};
    mLocalHistoryValid.fill(false);
    mLocalSequence = 1;
    mLatestLocalFrame = 0;
    mHaveLocalFrame = false;
    mNextKeepalive = Clock::now();
    {
        std::lock_guard<std::mutex> lock(mQueueMutex);
        mLocalRead = mLocalWrite = mLocalCount = 0;
        mRemoteRead = mRemoteWrite = mRemoteCount = 0;
    }
    mPayloadScratch.clear();
    mPacketScratch.clear();
    mPayloadScratch.reserve(128);
    mPacketScratch.reserve(kWireHeaderBytes + 128);
    mPacketsSent.store(0, std::memory_order_release);
    mPacketsReceived.store(0, std::memory_order_release);
    mPacketsDropped.store(0, std::memory_order_release);
    mSequenceGaps.store(0, std::memory_order_release);
    mDuplicates.store(0, std::memory_order_release);
    mOutOfOrder.store(0, std::memory_order_release);
    mRelayed.store(0, std::memory_order_release);
}

bool GameplaySession::Start(NetplayMode mode, bool isHost, uint32_t sessionId, uint8_t localPlayerId,
                            const std::string& hostEndpoint) {
    Stop();
    if ((mode != NetplayMode::LocalAdhoc && mode != NetplayMode::Online) ||
        sessionId == 0 || localPlayerId >= kMaxPlayers) {
        return false;
    }

    mMode = mode;
    mIsHost = isHost;
    mSessionId = sessionId;
    mLocalPlayerId = localPlayerId;
    ResetState();

    const uint16_t bindPort = isHost ? kGameplayPort : 0;
    mSocket = transport::CreateDatagram(mode, "bsnp_gameplay", {}, bindPort, false);
    if (mSocket == transport::kInvalidSocket) {
        port_log("[NETPLAY] gameplay transport open failed host=%d port=%u error=0x%08X\n",
                 isHost ? 1 : 0, static_cast<unsigned>(bindPort),
                 static_cast<unsigned>(transport::LastError()));
        Stop();
        return false;
    }

    if (!isHost) {
        if (!transport::ParseEndpoint(mode, hostEndpoint, kGameplayPort, mHostAddress)) {
            port_log("[NETPLAY] gameplay invalid host endpoint=%s\n", hostEndpoint.c_str());
            Stop();
            return false;
        }
        mHaveHostAddress = true;
        mPeerAddresses[0] = mHostAddress;
        mPeerKnown[0] = true;
    }

    mActive.store(true, std::memory_order_release);
    port_log("[NETPLAY] gameplay transport started mode=%s role=%s session=%08X local=P%u port=%u\n",
             mode == NetplayMode::LocalAdhoc ? "ADHOC/PDP" : "ONLINE/UDP",
             isHost ? "host" : "client", sessionId, localPlayerId + 1,
             static_cast<unsigned>(bindPort));
    return true;
}

void GameplaySession::Stop() {
    mActive.store(false, std::memory_order_release);
    if (mSocket != transport::kInvalidSocket) {
        transport::CloseDatagram(mMode, mSocket);
    }
    mSocket = transport::kInvalidSocket;
    mMode = NetplayMode::None;
    mIsHost = false;
    mSessionId = 0;
    mLocalPlayerId = 0xFF;
    mHaveHostAddress = false;
    ResetState();
}

void GameplaySession::SubmitLocalInput(uint32_t frame, uint16_t buttons, int8_t stickX, int8_t stickY) {
    if (!IsActive()) return;
    const GameplayFrameInput input{mLocalPlayerId, frame, buttons, stickX, stickY};
    std::lock_guard<std::mutex> lock(mQueueMutex);
    if (mLocalCount == kLocalQueueCapacity) {
        mLocalRead = (mLocalRead + 1) % kLocalQueueCapacity;
        --mLocalCount;
    }
    mLocalQueue[mLocalWrite] = input;
    mLocalWrite = (mLocalWrite + 1) % kLocalQueueCapacity;
    ++mLocalCount;
}

bool GameplaySession::PopRemoteInput(GameplayFrameInput& input) {
    std::lock_guard<std::mutex> lock(mQueueMutex);
    if (mRemoteCount == 0) return false;
    input = mRemoteQueue[mRemoteRead];
    mRemoteRead = (mRemoteRead + 1) % kRemoteQueueCapacity;
    --mRemoteCount;
    return true;
}

bool GameplaySession::PushRemote(const GameplayFrameInput& input) {
    if (input.playerId >= kMaxPlayers || input.playerId == mLocalPlayerId) return false;
    const std::size_t seenIndex = input.frame % kRemoteSeenCapacity;
    if (mRemoteSeen[input.playerId][seenIndex] == input.frame) return false;
    mRemoteSeen[input.playerId][seenIndex] = input.frame;

    std::lock_guard<std::mutex> lock(mQueueMutex);
    if (mRemoteCount == kRemoteQueueCapacity) {
        mRemoteRead = (mRemoteRead + 1) % kRemoteQueueCapacity;
        --mRemoteCount;
        mPacketsDropped.fetch_add(1, std::memory_order_relaxed);
    }
    mRemoteQueue[mRemoteWrite] = input;
    mRemoteWrite = (mRemoteWrite + 1) % kRemoteQueueCapacity;
    ++mRemoteCount;
    return true;
}

GameplayStats GameplaySession::Stats() const {
    GameplayStats stats{};
    stats.packetsSent = mPacketsSent.load(std::memory_order_acquire);
    stats.packetsReceived = mPacketsReceived.load(std::memory_order_acquire);
    stats.packetsDropped = mPacketsDropped.load(std::memory_order_acquire);
    stats.sequenceGaps = mSequenceGaps.load(std::memory_order_acquire);
    stats.duplicates = mDuplicates.load(std::memory_order_acquire);
    stats.outOfOrder = mOutOfOrder.load(std::memory_order_acquire);
    stats.relayed = mRelayed.load(std::memory_order_acquire);
    return stats;
}

bool GameplaySession::EncodeFramePayload(uint32_t ackSequence, uint32_t ackBits,
                                         const GameplayFrameInput* frames, std::size_t frameCount,
                                         std::vector<uint8_t>& payload) {
    if (frameCount > kRedundantInputs || (frameCount != 0 && frames == nullptr)) return false;
    payload.clear();
    PayloadWriter writer(payload);
    if (!writer.U32(ackSequence) || !writer.U32(ackBits) || !writer.U8(static_cast<uint8_t>(frameCount))) {
        return false;
    }
    for (std::size_t i = 0; i < frameCount; ++i) {
        if (!writer.U32(frames[i].frame) || !writer.U16(frames[i].buttons) ||
            !writer.S8(frames[i].stickX) || !writer.S8(frames[i].stickY)) {
            return false;
        }
    }
    return true;
}

bool GameplaySession::DecodeFramePayload(const std::vector<uint8_t>& payload, uint32_t& ackSequence,
                                         uint32_t& ackBits,
                                         std::array<GameplayFrameInput, kRedundantInputs>& frames,
                                         std::size_t& frameCount) {
    uint8_t count = 0;
    PayloadReader reader(payload.data(), payload.size());
    if (!reader.U32(ackSequence) || !reader.U32(ackBits) || !reader.U8(count) || count > kRedundantInputs) {
        return false;
    }
    frames = {};
    for (uint8_t i = 0; i < count; ++i) {
        if (!reader.U32(frames[i].frame) || !reader.U16(frames[i].buttons) ||
            !reader.S8(frames[i].stickX) || !reader.S8(frames[i].stickY)) {
            return false;
        }
    }
    if (!reader.Empty()) return false;
    frameCount = count;
    return true;
}

bool GameplaySession::TrackSequence(uint8_t playerId, uint32_t sequence) {
    if (playerId >= kMaxPlayers) return false;
    SequenceState& state = mRecvSequences[playerId];
    if (!state.initialized) {
        state.initialized = true;
        state.high = sequence;
        state.mask = 1U;
        return true;
    }
    if (sequence > state.high) {
        const uint32_t delta = sequence - state.high;
        if (delta > 1) mSequenceGaps.fetch_add(delta - 1, std::memory_order_relaxed);
        state.mask = delta >= 32 ? 1U : ((state.mask << delta) | 1U);
        state.high = sequence;
        return true;
    }

    const uint32_t delta = state.high - sequence;
    if (delta >= 32) {
        mDuplicates.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    const uint32_t bit = 1U << delta;
    if ((state.mask & bit) != 0) {
        mDuplicates.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    state.mask |= bit;
    mOutOfOrder.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool GameplaySession::IsExpectedSource(const transport::SocketAddress& source) const {
    if (mIsHost) return true;
    return mHaveHostAddress && SameAddress(source, mHostAddress);
}

bool GameplaySession::HandlePacket(const uint8_t* data, std::size_t size,
                                   const transport::SocketAddress& source) {
    if (!IsExpectedSource(source)) {
        mPacketsDropped.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    DecodedPacket packet;
    RejectReason reject = RejectReason::None;
    if (!DecodePacket(data, size, packet, &reject) || packet.header.type != PacketType::FrameInput ||
        packet.header.sessionId != mSessionId || packet.header.playerId >= kMaxPlayers ||
        packet.header.playerId == mLocalPlayerId) {
        mPacketsDropped.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    if (mIsHost) {
        if (packet.header.playerId == 0) {
            mPacketsDropped.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        if (!mPeerKnown[packet.header.playerId]) {
            mPeerAddresses[packet.header.playerId] = source;
            mPeerKnown[packet.header.playerId] = true;
            port_log("[NETPLAY] gameplay peer learned P%u endpoint=%s:%u\n",
                     packet.header.playerId + 1, transport::ToString(source).c_str(),
                     static_cast<unsigned>(source.port));
        } else if (!SameAddress(source, mPeerAddresses[packet.header.playerId])) {
            mPacketsDropped.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
    }

    uint32_t ackSequence = 0;
    uint32_t ackBits = 0;
    std::array<GameplayFrameInput, kRedundantInputs> frames{};
    std::size_t frameCount = 0;
    if (!DecodeFramePayload(packet.payload, ackSequence, ackBits, frames, frameCount)) {
        mPacketsDropped.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    for (std::size_t i = 1; i < frameCount; ++i) {
        if (frames[i - 1].frame >= frames[i].frame) {
            mPacketsDropped.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
    }
    if (frameCount != 0 && packet.header.frame != frames[frameCount - 1].frame) {
        mPacketsDropped.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    (void)ackSequence;
    (void)ackBits;

    const bool freshPacket = TrackSequence(packet.header.playerId, packet.header.sequence);
    mPacketsReceived.fetch_add(1, std::memory_order_relaxed);
    if (!freshPacket) return true;
    for (std::size_t i = 0; i < frameCount; ++i) {
        frames[i].playerId = packet.header.playerId;
        PushRemote(frames[i]);
    }

    if (mIsHost && frameCount != 0) {
        RelayPacket(data, size, packet.header.playerId);
    }
    return true;
}

void GameplaySession::ReceivePackets() {
    std::array<uint8_t, kWireHeaderBytes + kMaxPayloadBytes> buffer{};
    for (std::size_t budget = 0; budget < kReceiveBudget; ++budget) {
        transport::SocketAddress source{};
        const int received = transport::RecvDatagram(mMode, mSocket, buffer.data(), buffer.size(), source);
        if (received < 0) {
            if (!transport::IsWouldBlock(received)) {
                mPacketsDropped.fetch_add(1, std::memory_order_relaxed);
            }
            break;
        }
        if (received == 0) break;
        HandlePacket(buffer.data(), static_cast<std::size_t>(received), source);
    }
}

void GameplaySession::RelayPacket(const uint8_t* data, std::size_t size, uint8_t sourcePlayer) {
    for (uint8_t player = 1; player < kMaxPlayers; ++player) {
        if (player == sourcePlayer || !mPeerKnown[player]) continue;
        const int sent = transport::SendDatagram(mMode, mSocket, data, size, mPeerAddresses[player]);
        if (sent >= 0) {
            mRelayed.fetch_add(1, std::memory_order_relaxed);
            mPacketsSent.fetch_add(1, std::memory_order_relaxed);
        } else if (!transport::IsWouldBlock(sent)) {
            mPacketsDropped.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

void GameplaySession::DrainLocalQueue() {
    bool haveLatest = false;
    {
        std::lock_guard<std::mutex> lock(mQueueMutex);
        while (mLocalCount != 0) {
            const GameplayFrameInput latest = mLocalQueue[mLocalRead];
            mLocalRead = (mLocalRead + 1) % kLocalQueueCapacity;
            --mLocalCount;
            mLocalHistory[latest.frame % kHistoryCapacity] = latest;
            mLocalHistoryValid[latest.frame % kHistoryCapacity] = true;
            mLatestLocalFrame = latest.frame;
            mHaveLocalFrame = true;
            haveLatest = true;
        }
    }
    if (haveLatest) SendLocalBundle(false);
}

void GameplaySession::SendLocalBundle(bool allowEmpty) {
    std::array<GameplayFrameInput, kRedundantInputs> frames{};
    std::size_t frameCount = 0;
    if (mHaveLocalFrame) {
        const uint32_t oldest = mLatestLocalFrame >= (kRedundantInputs - 1)
            ? mLatestLocalFrame - static_cast<uint32_t>(kRedundantInputs - 1) : 0;
        for (uint32_t frame = oldest; frame <= mLatestLocalFrame; ++frame) {
            const std::size_t index = frame % kHistoryCapacity;
            if (!mLocalHistoryValid[index] || mLocalHistory[index].frame != frame) continue;
            frames[frameCount++] = mLocalHistory[index];
        }
    }
    if (frameCount == 0 && !allowEmpty) return;

    auto sendOne = [&](const transport::SocketAddress& destination, uint8_t ackPlayer) {
        uint32_t ackSequence = 0;
        uint32_t ackBits = 0;
        if (ackPlayer < kMaxPlayers && mRecvSequences[ackPlayer].initialized) {
            ackSequence = mRecvSequences[ackPlayer].high;
            ackBits = mRecvSequences[ackPlayer].mask;
        }
        if (!EncodeFramePayload(ackSequence, ackBits, frames.data(), frameCount, mPayloadScratch)) return;
        PacketHeader header{};
        header.type = PacketType::FrameInput;
        header.sessionId = mSessionId;
        header.playerId = mLocalPlayerId;
        header.sequence = mLocalSequence;
        header.frame = frameCount != 0 ? frames[frameCount - 1].frame : 0;
        if (!EncodePacket(header, mPayloadScratch, mPacketScratch)) return;
        const int sent = transport::SendDatagram(mMode, mSocket, mPacketScratch.data(), mPacketScratch.size(), destination);
        if (sent >= 0) {
            mPacketsSent.fetch_add(1, std::memory_order_relaxed);
        } else if (!transport::IsWouldBlock(sent)) {
            mPacketsDropped.fetch_add(1, std::memory_order_relaxed);
        }
    };

    if (mIsHost) {
        for (uint8_t player = 1; player < kMaxPlayers; ++player) {
            if (mPeerKnown[player]) sendOne(mPeerAddresses[player], player);
        }
    } else if (mHaveHostAddress) {
        sendOne(mHostAddress, 0);
    }
    ++mLocalSequence;
}

void GameplaySession::Poll() {
    if (!IsActive() || mSocket == transport::kInvalidSocket) return;
    ReceivePackets();
    DrainLocalQueue();
    const auto now = Clock::now();
    if (now >= mNextKeepalive) {
        SendLocalBundle(true);
        mNextKeepalive = now + kKeepaliveInterval;
    }
}

} // namespace ssb64::netplay
