#pragma once

#include "NetplayProtocol.h"
#include "../vita/VitaNetworkTransport.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace ssb64::netplay {

struct GameplayFrameInput {
    uint8_t playerId = 0xFF;
    uint32_t frame = 0;
    uint16_t buttons = 0;
    int8_t stickX = 0;
    int8_t stickY = 0;
};

struct GameplayStats {
    uint32_t packetsSent = 0;
    uint32_t packetsReceived = 0;
    uint32_t packetsDropped = 0;
    uint32_t sequenceGaps = 0;
    uint32_t duplicates = 0;
    uint32_t outOfOrder = 0;
    uint32_t relayed = 0;
};

// Worker-owned PDP/UDP gameplay transport. The game thread only touches the
// fixed-size input queues below; all socket syscalls stay in Poll().
class GameplaySession {
public:
    static constexpr std::size_t kRedundantInputs = 5;

    bool Start(NetplayMode mode, bool isHost, uint32_t sessionId, uint8_t localPlayerId,
               const std::string& hostEndpoint);
    void Stop();
    void Poll();

    bool IsActive() const { return mActive.load(std::memory_order_acquire); }
    void SubmitLocalInput(uint32_t frame, uint16_t buttons, int8_t stickX, int8_t stickY);
    bool PopRemoteInput(GameplayFrameInput& input);
    GameplayStats Stats() const;

    static bool EncodeFramePayload(uint32_t ackSequence, uint32_t ackBits,
                                   const GameplayFrameInput* frames, std::size_t frameCount,
                                   std::vector<uint8_t>& payload);
    static bool DecodeFramePayload(const std::vector<uint8_t>& payload, uint32_t& ackSequence,
                                   uint32_t& ackBits, std::array<GameplayFrameInput, kRedundantInputs>& frames,
                                   std::size_t& frameCount);

private:
    using Clock = std::chrono::steady_clock;
    static constexpr std::size_t kLocalQueueCapacity = 128;
    static constexpr std::size_t kRemoteQueueCapacity = 512;
    static constexpr std::size_t kHistoryCapacity = 32;
    static constexpr std::size_t kRemoteSeenCapacity = 64;

    struct SequenceState {
        uint32_t high = 0;
        uint32_t mask = 0;
        bool initialized = false;
    };

    void ResetState();
    void ReceivePackets();
    bool HandlePacket(const uint8_t* data, std::size_t size, const transport::SocketAddress& source);
    void DrainLocalQueue();
    void SendLocalBundle(bool allowEmpty);
    void RelayPacket(const uint8_t* data, std::size_t size, uint8_t sourcePlayer);
    bool TrackSequence(uint8_t playerId, uint32_t sequence);
    bool IsExpectedSource(const transport::SocketAddress& source) const;
    static bool SameAddress(const transport::SocketAddress& a, const transport::SocketAddress& b);
    bool PushRemote(const GameplayFrameInput& input);

    NetplayMode mMode = NetplayMode::None;
    bool mIsHost = false;
    uint32_t mSessionId = 0;
    uint8_t mLocalPlayerId = 0xFF;
    transport::SocketHandle mSocket = transport::kInvalidSocket;
    transport::SocketAddress mHostAddress{};
    bool mHaveHostAddress = false;
    std::array<transport::SocketAddress, kMaxPlayers> mPeerAddresses{};
    std::array<bool, kMaxPlayers> mPeerKnown{};
    std::array<SequenceState, kMaxPlayers> mRecvSequences{};
    std::array<std::array<uint32_t, kRemoteSeenCapacity>, kMaxPlayers> mRemoteSeen{};

    std::array<GameplayFrameInput, kHistoryCapacity> mLocalHistory{};
    std::array<bool, kHistoryCapacity> mLocalHistoryValid{};
    uint32_t mLocalSequence = 1;
    uint32_t mLatestLocalFrame = 0;
    bool mHaveLocalFrame = false;
    Clock::time_point mNextKeepalive{};

    mutable std::mutex mQueueMutex;
    std::array<GameplayFrameInput, kLocalQueueCapacity> mLocalQueue{};
    std::size_t mLocalRead = 0;
    std::size_t mLocalWrite = 0;
    std::size_t mLocalCount = 0;
    std::array<GameplayFrameInput, kRemoteQueueCapacity> mRemoteQueue{};
    std::size_t mRemoteRead = 0;
    std::size_t mRemoteWrite = 0;
    std::size_t mRemoteCount = 0;

    std::vector<uint8_t> mPayloadScratch;
    std::vector<uint8_t> mPacketScratch;
    std::atomic<bool> mActive{false};
    std::atomic<uint32_t> mPacketsSent{0};
    std::atomic<uint32_t> mPacketsReceived{0};
    std::atomic<uint32_t> mPacketsDropped{0};
    std::atomic<uint32_t> mSequenceGaps{0};
    std::atomic<uint32_t> mDuplicates{0};
    std::atomic<uint32_t> mOutOfOrder{0};
    std::atomic<uint32_t> mRelayed{0};
};

} // namespace ssb64::netplay
