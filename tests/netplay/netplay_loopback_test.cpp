#include "../../port/netplay/LanDiscovery.h"
#include "../../port/netplay/GameplaySession.h"
#include "../../port/netplay/LobbySession.h"
#include "../../port/netplay/NetplayProtocol.h"
#include "../../port/netplay/RendezvousClient.h"
#include "../../port/vita/VitaNetworkTransport.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <atomic>
#include <cstdint>
#include <mutex>
#define PORT 1
#define _LANGUAGE_C_PLUS_PLUS 1
#include <sys/netinput.h>
#include <sys/netrollback.h>
#include <sys/netsync.h>

#include <array>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <thread>
#include <vector>

extern "C" void port_log(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    std::vfprintf(stderr, fmt, args);
    va_end(args);
}

namespace {

using namespace ssb64::netplay;
using Clock = std::chrono::steady_clock;

[[noreturn]] void Fail(const char* message) {
    std::fprintf(stderr, "[FAIL] %s\n", message);
    std::exit(1);
}

void Require(bool condition, const char* message) {
    if (!condition) Fail(message);
}

template <typename Predicate, typename Tick>
bool WaitFor(Predicate predicate, Tick tick, int timeoutMs = 2500) {
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
    while (Clock::now() < deadline) {
        tick();
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}

void PollAll(LobbySession& host, std::vector<LobbySession*>& clients) {
    host.Poll();
    for (LobbySession* client : clients) client->Poll();
}

void TestProtocolValidation() {
    std::vector<uint8_t> payload;
    PayloadWriter writer(payload);
    Require(writer.U32(0x12345678U) && writer.String("TEST", 16), "payload writer");

    PacketHeader header{};
    header.type = PacketType::Heartbeat;
    header.sessionId = 0x10203040U;
    header.playerId = 2;
    header.sequence = 77;
    std::vector<uint8_t> packet;
    Require(EncodePacket(header, payload, packet), "packet encode");

    DecodedPacket decoded;
    RejectReason reject = RejectReason::None;
    Require(DecodePacket(packet.data(), packet.size(), decoded, &reject), "packet decode");
    Require(decoded.header.sessionId == header.sessionId && decoded.header.playerId == 2,
            "header roundtrip");

    packet.back() ^= 0x5A;
    Require(!DecodePacket(packet.data(), packet.size(), decoded, &reject) &&
            reject == RejectReason::MalformedPacket, "checksum rejection");

    header.protocolVersion = kProtocolVersion + 1;
    Require(EncodePacket(header, payload, packet), "version mismatch encode");
    Require(!DecodePacket(packet.data(), packet.size(), decoded, &reject) &&
            reject == RejectReason::ProtocolMismatch, "protocol mismatch rejection");
    Require(DecodePacketAnyVersion(packet.data(), packet.size(), decoded, &reject) &&
            decoded.header.protocolVersion == kProtocolVersion + 1,
            "discovery can inspect incompatible protocol");
}

uint64_t MixTraceValue(uint64_t hash, uint64_t value) {
    hash ^= value;
    hash *= 1099511628211ULL;
    return hash;
}

SYNetSyncDigest AdvanceSyntheticSimulation(uint32_t frame, uint32_t input, uint64_t& state) {
    state = MixTraceValue(state, static_cast<uint64_t>(input) | (static_cast<uint64_t>(frame) << 32));
    SYNetSyncDigest digest{};
    digest.frame = frame;
    digest.rng_hash = MixTraceValue(state, 0x10);
    digest.battle_hash = MixTraceValue(state, 0x20);
    for (std::size_t player = 0; player < 4; ++player) {
        digest.fighter_hash[player] = MixTraceValue(state, 0x30 + player);
    }
    digest.stage_hash = MixTraceValue(state, 0x40);
    digest.map_hash = MixTraceValue(state, 0x50);
    digest.item_hash = MixTraceValue(state, 0x60);
    digest.weapon_hash = MixTraceValue(state, 0x70);
    digest.total_hash = MixTraceValue(state, 0x80);
    return digest;
}

void TestDeterministicTraceComparison() {
    constexpr std::size_t kFrames = 300;
    std::array<uint32_t, kFrames> recordedInputs{};
    for (std::size_t frame = 0; frame < kFrames; ++frame) {
        recordedInputs[frame] = static_cast<uint32_t>((frame * 1103515245ULL + 12345ULL) >> 16);
    }

    std::array<SYNetSyncDigest, kFrames> simulationA{};
    std::array<SYNetSyncDigest, kFrames> simulationB{};
    uint64_t stateA = 0xCBF29CE484222325ULL;
    uint64_t stateB = 0xCBF29CE484222325ULL;
    for (std::size_t frame = 0; frame < kFrames; ++frame) {
        simulationA[frame] = AdvanceSyntheticSimulation(static_cast<uint32_t>(frame), recordedInputs[frame], stateA);
        simulationB[frame] = AdvanceSyntheticSimulation(static_cast<uint32_t>(frame), recordedInputs[frame], stateB);
        Require(syNetSyncDigestEqual(&simulationA[frame], &simulationB[frame]) != FALSE,
                "deterministic record/reset/replay trace");
    }

    simulationB[137].item_hash ^= 1U;
    simulationB[137].total_hash ^= 1U;
    std::size_t firstDivergentFrame = kFrames;
    for (std::size_t frame = 0; frame < kFrames; ++frame) {
        if (syNetSyncDigestEqual(&simulationA[frame], &simulationB[frame]) == FALSE) {
            firstDivergentFrame = frame;
            break;
        }
    }
    Require(firstDivergentFrame == 137, "determinism first divergent frame");
    Require(std::string(syNetSyncDigestFirstMismatch(&simulationA[137], &simulationB[137])) == "items",
            "determinism first divergent category");

    SYNetSyncDigest category = simulationA[20];
    category.fighter_hash[2] ^= 1U;
    category.total_hash ^= 1U;
    Require(std::string(syNetSyncDigestFirstMismatch(&simulationA[20], &category)) == "fighter2",
            "determinism fighter category diagnostic");
}

void TestRollbackPolicy() {
    Require(syNetRollbackClassifyMismatch(95, 100, FALSE, TRUE) == nSYNetRollbackDecisionPerform,
            "rollback request at six-frame window");
    Require(syNetRollbackClassifyMismatch(94, 100, FALSE, TRUE) == nSYNetRollbackDecisionAbort,
            "late input beyond rollback window aborts");
    Require(syNetRollbackClassifyMismatch(99, 100, FALSE, FALSE) == nSYNetRollbackDecisionAbort,
            "missing rollback snapshot aborts");
    Require(syNetRollbackClassifyMismatch(95, 100, TRUE, FALSE) == nSYNetRollbackDecisionNone,
            "terminal match suppresses rollback");
    Require(syNetRollbackClassifyMismatch(101, 100, FALSE, TRUE) == nSYNetRollbackDecisionNone,
            "future mismatch ignored");
}

void TestConfirmedInputBarrier() {
    constexpr uint32_t participants = 0x03U;
    Require(syNetInputConfirmedBarrierReady(0, 2, participants, 0x01U, 0) != FALSE,
            "input-delay neutral frame zero releases");
    Require(syNetInputConfirmedBarrierReady(1, 2, participants, 0x01U, 0) != FALSE,
            "input-delay neutral frame one releases");
    Require(syNetInputConfirmedBarrierReady(2, 2, participants, 0x01U, 0) == FALSE,
            "missing remote input stalls logical frame");
    Require(syNetInputConfirmedBarrierReady(2, 2, participants, 0x03U, 0) != FALSE,
            "confirmed remote input releases logical frame");
    Require(syNetInputConfirmedBarrierReady(8, 2, 0x0FU, 0x07U, 0) == FALSE,
            "all remote participants required");
    Require(syNetInputConfirmedBarrierReady(8, 2, 0x0FU, 0x0FU, 0) != FALSE,
            "four-player confirmed barrier releases");
}

void TestDiscovery() {
    LanDiscovery hostDiscovery;
    LanDiscovery clientDiscovery;
    DiscoveryHostInfo info{};
    info.sessionId = 0xAABBCCDDU;
    info.hostName = "HOST";
    info.buildId = "1.2";
    info.playerCount = 1;
    info.status = LobbyStatus::Open;

    Require(hostDiscovery.StartHost(NetplayMode::Online, info), "discovery host start");
    Require(clientDiscovery.StartClient(NetplayMode::Online, "1.2"), "discovery client start");
    clientDiscovery.RequestImmediateScan();

    const bool found = WaitFor(
        [&] { return !clientDiscovery.Snapshot().empty(); },
        [&] {
            clientDiscovery.Poll();
            hostDiscovery.Poll();
            clientDiscovery.Poll();
        }, 2200);
    Require(found, "LAN discovery response");
    const auto entries = clientDiscovery.Snapshot();
    Require(entries[0].sessionId == info.sessionId && entries[0].compatible,
            "discovery payload/compatibility");
    hostDiscovery.Stop();
    clientDiscovery.Stop();
}

void TestLobbyFourPlayersAndStart() {
    LobbySession host;
    LobbySession c1;
    LobbySession c2;
    LobbySession c3;
    std::vector<LobbySession*> clients;

    Require(host.StartHost(NetplayMode::Online, 0x11223344U, "HOST", "1.2", "127.0.0.1"), "host lobby start");
    Require(host.ConnectedPlayerCount() == 1 && !host.CanHostStart(),
            "host-only lobby cannot start");

    Require(c1.StartClient(NetplayMode::Online, "127.0.0.1", 0x11223344U, "ONE", "1.2"), "client 1 start");
    clients.push_back(&c1);
    Require(WaitFor([&] {
        return host.ConnectedPlayerCount() == 2 && c1.Snapshot().connected;
    }, [&] { PollAll(host, clients); }), "host + 1 client join");
    Require(!host.CanHostStart(), "two players still require ready state");

    Require(c2.StartClient(NetplayMode::Online, "127.0.0.1", 0x11223344U, "TWO", "1.2"), "client 2 start");
    clients.push_back(&c2);
    Require(WaitFor([&] {
        return host.ConnectedPlayerCount() == 3 && c2.Snapshot().connected;
    }, [&] { PollAll(host, clients); }), "host + 2 clients join");

    Require(c3.StartClient(NetplayMode::Online, "127.0.0.1", 0x11223344U, "THREE", "1.2"), "client 3 start");
    clients.push_back(&c3);

    Require(WaitFor([&] {
        return host.ConnectedPlayerCount() == 4 && c1.Snapshot().connected &&
               c2.Snapshot().connected && c3.Snapshot().connected;
    }, [&] { PollAll(host, clients); }), "host + 3 clients join");

    Require(c1.Snapshot().localPlayerId == 1 && c2.Snapshot().localPlayerId == 2 &&
            c3.Snapshot().localPlayerId == 3, "stable P1-P4 slot assignment");
    Require(host.Snapshot().status == LobbyStatus::Full, "lobby full status");

    host.SetLocalReady(true);
    c1.SetLocalReady(true);
    c2.SetLocalReady(true);
    c3.SetLocalReady(true);
    Require(WaitFor([&] { return host.CanHostStart(); }, [&] { PollAll(host, clients); }),
            "all ready enables host start");
    Require(host.StartCharacterSelect(), "host start command");
    bool c1Started = false;
    bool c2Started = false;
    bool c3Started = false;
    Require(WaitFor([&] {
        c1Started = c1Started || c1.ConsumeCharacterSelectStart();
        c2Started = c2Started || c2.ConsumeCharacterSelectStart();
        c3Started = c3Started || c3.ConsumeCharacterSelectStart();
        return c1Started && c2Started && c3Started;
    }, [&] { PollAll(host, clients); }), "start character select broadcast");

    std::vector<uint8_t> cssInput;
    PayloadWriter cssWriter(cssInput);
    Require(cssWriter.U8(1) && cssWriter.U32(7) && cssWriter.U16(0x8000) &&
            cssWriter.S8(42) && cssWriter.S8(-17), "CSS input encode");
    Require(c1.SendSessionMessage(PacketType::CharacterCursorInput, cssInput), "CSS input send");

    bool hostSawCss = false;
    bool c2SawCss = false;
    Require(WaitFor([&] {
        LobbySessionEvent event;
        while (host.PopSessionEvent(event)) {
            if (event.type == PacketType::CharacterCursorInput && event.sourcePlayerId == 1 &&
                event.payload == cssInput) hostSawCss = true;
        }
        while (c2.PopSessionEvent(event)) {
            if (event.type == PacketType::CharacterCursorInput && event.payload == cssInput) c2SawCss = true;
        }
        return hostSawCss && c2SawCss;
    }, [&] { PollAll(host, clients); }), "CSS client input host relay");

    std::vector<uint8_t> matchConfig{0x11, 0x22, 0x33, 0x44};
    Require(host.SendSessionMessage(PacketType::MatchConfiguration, matchConfig), "match config broadcast");
    bool c1SawConfig = false;
    Require(WaitFor([&] {
        LobbySessionEvent event;
        while (c1.PopSessionEvent(event)) {
            if (event.type == PacketType::MatchConfiguration && event.payload == matchConfig) c1SawConfig = true;
        }
        return c1SawConfig;
    }, [&] { PollAll(host, clients); }), "match config reaches client");

    std::vector<uint8_t> loadingReady;
    PayloadWriter readyWriter(loadingReady);
    Require(readyWriter.U8(1), "loading ready encode");
    Require(c1.SendSessionMessage(PacketType::LoadingReady, loadingReady), "loading ready send");
    bool hostSawLoadingReady = false;
    Require(WaitFor([&] {
        LobbySessionEvent event;
        while (host.PopSessionEvent(event)) {
            if (event.type == PacketType::LoadingReady && event.sourcePlayerId == 1 &&
                event.payload == loadingReady) hostSawLoadingReady = true;
        }
        return hostSawLoadingReady;
    }, [&] { PollAll(host, clients); }), "loading ready reaches host");

    std::vector<uint8_t> startMatch;
    PayloadWriter startWriter(startMatch);
    Require(startWriter.U16(60) && startWriter.U8(2), "start match encode");
    Require(host.SendSessionMessage(PacketType::StartMatch, startMatch), "start match broadcast");
    bool c3SawStart = false;
    Require(WaitFor([&] {
        LobbySessionEvent event;
        while (c3.PopSessionEvent(event)) {
            if (event.type == PacketType::StartMatch && event.payload == startMatch) c3SawStart = true;
        }
        return c3SawStart;
    }, [&] { PollAll(host, clients); }), "start match reaches client");

    std::vector<uint8_t> stateHash;
    PayloadWriter hashWriter(stateHash);
    Require(hashWriter.U32(120) && hashWriter.U32(0x11223344U) && hashWriter.U32(0x55667788U),
            "state hash encode");
    Require(c1.SendSessionMessage(PacketType::StateHash, stateHash), "state hash client send");
    bool hostSawHash = false;
    Require(WaitFor([&] {
        LobbySessionEvent event;
        while (host.PopSessionEvent(event)) {
            if (event.type == PacketType::StateHash && event.sourcePlayerId == 1 &&
                event.payload == stateHash) hostSawHash = true;
        }
        return hostSawHash;
    }, [&] { PollAll(host, clients); }), "state hash reaches host");

    Require(host.SendSessionMessage(PacketType::StateHash, stateHash), "state hash host send");
    bool c3SawHash = false;
    Require(WaitFor([&] {
        LobbySessionEvent event;
        while (c3.PopSessionEvent(event)) {
            if (event.type == PacketType::StateHash && event.payload == stateHash) c3SawHash = true;
        }
        return c3SawHash;
    }, [&] { PollAll(host, clients); }), "state hash reaches client");

    c2.Stop(RejectReason::None, true);
    Require(WaitFor([&] {
        return host.Snapshot().players[2].state == LobbySlotState::Disconnected;
    }, [&] { PollAll(host, clients); }), "client leave propagation");

    host.Stop(RejectReason::HostClosing, true);
    Require(WaitFor([&] { return !c1.IsActive() && !c3.IsActive(); }, [&] {
        c1.Poll();
        c3.Poll();
    }), "host close propagation");
}

void TestBuildMismatch() {
    LobbySession host;
    LobbySession client;
    std::vector<LobbySession*> clients{&client};
    Require(host.StartHost(NetplayMode::Online, 0x55667788U, "HOST", "1.2", "127.0.0.1"), "mismatch host start");
    Require(client.StartClient(NetplayMode::Online, "127.0.0.1", 0x55667788U, "OLD", "0.9"), "mismatch client start");
    Require(WaitFor([&] { return !client.IsActive(); }, [&] { PollAll(host, clients); }),
            "build mismatch disconnect");
    Require(client.Snapshot().lastMessage == "BUILD MISMATCH", "build mismatch reason");
    host.Stop(RejectReason::HostClosing, false);
}

void TestLobbyFullReject() {
    LobbySession host;
    LobbySession c1;
    LobbySession c2;
    LobbySession c3;
    LobbySession extra;
    std::vector<LobbySession*> clients{&c1, &c2, &c3};
    Require(host.StartHost(NetplayMode::Online, 0xABCDEF01U, "HOST", "1.2", "127.0.0.1"), "full host start");
    Require(c1.StartClient(NetplayMode::Online, "127.0.0.1", 0xABCDEF01U, "ONE", "1.2"), "full c1");
    Require(c2.StartClient(NetplayMode::Online, "127.0.0.1", 0xABCDEF01U, "TWO", "1.2"), "full c2");
    Require(c3.StartClient(NetplayMode::Online, "127.0.0.1", 0xABCDEF01U, "THREE", "1.2"), "full c3");
    Require(WaitFor([&] { return host.ConnectedPlayerCount() == 4; }, [&] { PollAll(host, clients); }),
            "fill lobby");

    Require(extra.StartClient(NetplayMode::Online, "127.0.0.1", 0xABCDEF01U, "EXTRA", "1.2"), "extra connect attempt");
    Require(WaitFor([&] { return !extra.IsActive(); }, [&] {
        PollAll(host, clients);
        extra.Poll();
    }), "full lobby extra rejected");
    Require(extra.Snapshot().lastMessage == "LOBBY FULL" ||
            extra.Snapshot().lastMessage == "HOST DISCONNECTED",
            "full lobby rejection reason");
    host.Stop(RejectReason::HostClosing, false);
    for (LobbySession* client : clients) client->Stop(RejectReason::None, false);
}

void TestHeartbeatTimeout() {
    LobbySession host;
    LobbySession client;
    std::vector<LobbySession*> clients{&client};
    Require(host.StartHost(NetplayMode::Online, 0x13572468U, "HOST", "1.2", "127.0.0.1"), "timeout host start");
    Require(client.StartClient(NetplayMode::Online, "127.0.0.1", 0x13572468U, "CLIENT", "1.2"), "timeout client start");
    Require(WaitFor([&] { return client.Snapshot().connected; }, [&] { PollAll(host, clients); }),
            "timeout client joined");

    // Stop polling the client so it sends no heartbeat/replies. The host must
    // detect the silent peer without any blocking network call.
    const auto deadline = Clock::now() + std::chrono::milliseconds(5400);
    while (Clock::now() < deadline) {
        host.Poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    Require(host.Snapshot().players[1].state == LobbySlotState::Disconnected,
            "heartbeat timeout marks disconnected");
    host.Stop(RejectReason::HostClosing, false);
    client.Stop(RejectReason::None, false);
}

void TestGameplayInputTransport() {
    GameplaySession host;
    GameplaySession client;
    constexpr uint32_t session = 0x4D350001U;

    Require(host.Start(NetplayMode::Online, true, session, 0, ""), "gameplay host start");
    Require(client.Start(NetplayMode::Online, false, session, 1, "127.0.0.1"), "gameplay client start");

    for (uint32_t frame = 10; frame < 15; ++frame) {
        client.SubmitLocalInput(frame, static_cast<uint16_t>(0x0100U + frame),
                                static_cast<int8_t>(frame), static_cast<int8_t>(-static_cast<int>(frame)));
    }
    std::array<bool, 5> seen{};
    Require(WaitFor([&] {
        GameplayFrameInput input{};
        while (host.PopRemoteInput(input)) {
            if (input.playerId == 1 && input.frame >= 10 && input.frame < 15)
                seen[input.frame - 10] = true;
        }
        for (bool value : seen) if (!value) return false;
        return true;
    }, [&] { client.Poll(); host.Poll(); }), "gameplay redundant client inputs reach host");

    host.SubmitLocalInput(15, 0xBEEF, 33, -22);
    GameplayFrameInput hostInput{};
    Require(WaitFor([&] {
        GameplayFrameInput input{};
        while (client.PopRemoteInput(input)) {
            if (input.playerId == 0 && input.frame == 15) { hostInput = input; return true; }
        }
        return false;
    }, [&] { host.Poll(); client.Poll(); }), "gameplay host input reaches client");
    Require(hostInput.buttons == 0xBEEF && hostInput.stickX == 33 && hostInput.stickY == -22,
            "gameplay input values roundtrip");

    std::array<GameplayFrameInput, GameplaySession::kRedundantInputs> frames{};
    frames[0] = GameplayFrameInput{2, 100, 0x1234, 12, -7};
    std::vector<uint8_t> payload;
    Require(GameplaySession::EncodeFramePayload(77, 0xA5A5A5A5U, frames.data(), 1, payload),
            "gameplay payload encode");
    uint32_t ack = 0, ackBits = 0;
    std::size_t count = 0;
    std::array<GameplayFrameInput, GameplaySession::kRedundantInputs> decoded{};
    Require(GameplaySession::DecodeFramePayload(payload, ack, ackBits, decoded, count) &&
            ack == 77 && ackBits == 0xA5A5A5A5U && count == 1 && decoded[0].frame == 100 &&
            decoded[0].buttons == 0x1234 && decoded[0].stickX == 12 && decoded[0].stickY == -7,
            "gameplay ack/redundancy payload roundtrip");

    host.Stop();
    client.Stop();
}

void SendRawGameplayPacket(transport::SocketHandle socket, const transport::SocketAddress& destination,
                           uint32_t session, uint32_t sequence,
                           const GameplayFrameInput* frames, std::size_t frameCount) {
    std::vector<uint8_t> payload;
    std::vector<uint8_t> packet;
    Require(GameplaySession::EncodeFramePayload(0, 0, frames, frameCount, payload), "raw gameplay payload");
    PacketHeader header{};
    header.type = PacketType::FrameInput;
    header.sessionId = session;
    header.playerId = 1;
    header.sequence = sequence;
    header.frame = frameCount != 0 ? frames[frameCount - 1].frame : 0;
    Require(EncodePacket(header, payload, packet), "raw gameplay packet");
    Require(transport::SendDatagram(NetplayMode::Online, socket, packet.data(), packet.size(), destination) >= 0,
            "raw gameplay send");
}

void TestGameplayLossDuplicateAndReorder() {
    GameplaySession host;
    constexpr uint32_t session = 0x4D350002U;
    Require(host.Start(NetplayMode::Online, true, session, 0, ""), "gameplay sequence host start");

    const auto sender = transport::CreateDatagram(NetplayMode::Online, "sequence_test", {}, 0, false);
    Require(sender != transport::kInvalidSocket, "gameplay sequence sender start");
    transport::SocketAddress destination{};
    Require(transport::ParseIpv4("127.0.0.1", kGameplayPort, destination), "gameplay host address");

    const GameplayFrameInput frame100{1, 100, 0x0100, 1, -1};
    const std::array<GameplayFrameInput, 2> recovered{{
        GameplayFrameInput{1, 101, 0x0101, 2, -2},
        GameplayFrameInput{1, 102, 0x0102, 3, -3},
    }};
    const GameplayFrameInput frame103{1, 103, 0x0103, 4, -4};

    SendRawGameplayPacket(sender, destination, session, 1, &frame100, 1);
    SendRawGameplayPacket(sender, destination, session, 1, &frame100, 1);
    SendRawGameplayPacket(sender, destination, session, 3, recovered.data(), recovered.size());
    SendRawGameplayPacket(sender, destination, session, 2, &frame103, 1);

    std::array<uint32_t, 4> counts{};
    Require(WaitFor([&] {
        GameplayFrameInput input{};
        while (host.PopRemoteInput(input)) {
            if (input.frame >= 100 && input.frame <= 103) ++counts[input.frame - 100];
        }
        const GameplayStats stats = host.Stats();
        return stats.duplicates >= 1 && stats.sequenceGaps >= 1 && stats.outOfOrder >= 1 &&
               counts[0] == 1 && counts[1] == 1 && counts[2] == 1 && counts[3] == 1;
    }, [&] { host.Poll(); }), "gameplay duplicate/loss/reorder accounting");

    host.Stop();
    constexpr uint32_t nextSession = session + 1U;
    Require(host.Start(NetplayMode::Online, true, nextSession, 0, ""),
            "gameplay transport restarts for next match");
    const GameplayFrameInput staleFrame{1, 104, 0x0BAD, 5, -5};
    const GameplayFrameInput nextFrame{1, 0, 0x600D, 6, -6};
    SendRawGameplayPacket(sender, destination, session, 4, &staleFrame, 1);
    SendRawGameplayPacket(sender, destination, nextSession, 1, &nextFrame, 1);
    bool sawStale = false;
    bool sawNext = false;
    Require(WaitFor([&] {
        GameplayFrameInput input{};
        while (host.PopRemoteInput(input)) {
            if (input.frame == staleFrame.frame) sawStale = true;
            if (input.frame == nextFrame.frame && input.buttons == nextFrame.buttons) sawNext = true;
        }
        return sawNext;
    }, [&] { host.Poll(); }), "new match accepts reset frame and sequence");
    Require(!sawStale && host.Stats().packetsDropped >= 1,
            "new match rejects stale session packet");

    auto senderToClose = sender;
    transport::CloseDatagram(NetplayMode::Online, senderToClose);
    host.Stop();
}

void TestPostMatchControlLifecycle() {
    LobbySession host;
    LobbySession client;
    constexpr uint32_t session = 0x4D360001U;
    Require(host.StartHost(NetplayMode::Online, session, "HOST", "1.2", "127.0.0.1"),
            "lifecycle host start");
    Require(client.StartClient(NetplayMode::Online, "127.0.0.1", session, "CLIENT", "1.2"),
            "lifecycle client start");
    Require(WaitFor([&] { return client.Snapshot().connected; }, [&] { host.Poll(); client.Poll(); }),
            "lifecycle client joined");

    MatchResultPayload result{};
    result.matchId = 1;
    result.finalFrame = 3600;
    result.winner = 0;
    result.placements = {0, 1, 0xFF, 0xFF};
    result.stocksRemaining = {2, -1, -1, -1};
    result.hasFinalHash = true;
    result.finalHash = 0x1122334455667788ULL;
    std::vector<uint8_t> payload;
    Require(EncodeMatchResultPayload(result, payload), "match result encode");
    MatchResultPayload decoded{};
    Require(DecodeMatchResultPayload(payload, decoded) && decoded.matchId == result.matchId &&
            decoded.finalFrame == result.finalFrame && decoded.winner == result.winner &&
            decoded.placements == result.placements && decoded.stocksRemaining == result.stocksRemaining &&
            decoded.finalHash == result.finalHash, "match result roundtrip");
    std::vector<uint8_t> malformed = payload;
    malformed.pop_back();
    Require(!DecodeMatchResultPayload(malformed, decoded), "truncated match result rejected");
    Require(host.SendSessionMessage(PacketType::MatchResult, payload), "match result broadcast");
    Require(WaitFor([&] {
        LobbySessionEvent event;
        while (client.PopSessionEvent(event)) {
            if (event.type == PacketType::MatchResult) return event.payload == payload;
        }
        return false;
    }, [&] { host.Poll(); client.Poll(); }), "client reaches results control");

    RematchPayload rematch{2, 0xAABBCCDDU};
    Require(EncodeRematchPayload(rematch, payload), "rematch encode");
    Require(host.SendSessionMessage(PacketType::Rematch, payload), "rematch broadcast");
    Require(WaitFor([&] {
        LobbySessionEvent event;
        while (client.PopSessionEvent(event)) {
            RematchPayload received{};
            if (event.type == PacketType::Rematch && DecodeRematchPayload(event.payload, received)) {
                return received.matchId == 2 && received.rngSeed != result.finalHash;
            }
        }
        return false;
    }, [&] { host.Poll(); client.Poll(); }), "rematch uses new identity and seed");

    std::vector<uint8_t> loadingReady;
    PayloadWriter loadingWriter(loadingReady);
    Require(loadingWriter.U8(1) && client.SendSessionMessage(PacketType::LoadingReady, loadingReady),
            "rematch loading ready send");
    Require(WaitFor([&] {
        LobbySessionEvent event;
        while (host.PopSessionEvent(event)) {
            if (event.type == PacketType::LoadingReady && event.sourcePlayerId == 1)
                return event.payload == loadingReady;
        }
        return false;
    }, [&] { host.Poll(); client.Poll(); }), "rematch loading barrier ready");

    std::vector<uint8_t> startMatch;
    PayloadWriter startWriter(startMatch);
    Require(startWriter.U16(60) && startWriter.U8(2) &&
                host.SendSessionMessage(PacketType::StartMatch, startMatch),
            "rematch start broadcast");
    Require(WaitFor([&] {
        LobbySessionEvent event;
        while (client.PopSessionEvent(event)) {
            if (event.type == PacketType::StartMatch) return event.payload == startMatch;
        }
        return false;
    }, [&] { host.Poll(); client.Poll(); }), "rematch starts second match");

    result.matchId = 2;
    result.finalFrame = 2400;
    result.finalHash = 0x8877665544332211ULL;
    Require(EncodeMatchResultPayload(result, payload) &&
            host.SendSessionMessage(PacketType::MatchResult, payload), "second match result broadcast");
    Require(WaitFor([&] {
        LobbySessionEvent event;
        while (client.PopSessionEvent(event)) {
            MatchResultPayload received{};
            if (event.type == PacketType::MatchResult &&
                DecodeMatchResultPayload(event.payload, received)) return received.matchId == 2;
        }
        return false;
    }, [&] { host.Poll(); client.Poll(); }), "second match reaches results");

    std::vector<uint8_t> action;
    PayloadWriter actionWriter(action);
    Require(actionWriter.U32(2), "return CSS encode");
    Require(host.SendSessionMessage(PacketType::ReturnToCharacterSelect, action), "return CSS broadcast");
    Require(WaitFor([&] {
        LobbySessionEvent event;
        while (client.PopSessionEvent(event)) {
            if (event.type == PacketType::ReturnToCharacterSelect) return event.payload == action;
        }
        return false;
    }, [&] { host.Poll(); client.Poll(); }), "return CSS reaches client");

    std::vector<uint8_t> characterLock;
    PayloadWriter lockWriter(characterLock);
    Require(lockWriter.U8(1) && lockWriter.U8(9) && lockWriter.U8(1) && lockWriter.U8(0) &&
            client.SendSessionMessage(PacketType::CharacterLocked, characterLock),
            "new CSS fighter lock send");
    Require(WaitFor([&] {
        LobbySessionEvent event;
        while (host.PopSessionEvent(event)) {
            if (event.type == PacketType::CharacterLocked && event.sourcePlayerId == 1)
                return event.payload == characterLock;
        }
        return false;
    }, [&] { host.Poll(); client.Poll(); }), "new CSS fighter lock received");

    const std::vector<uint8_t> nextConfig{0, 0, 0, 3, 0xCA, 0xFE, 0xBA, 0xBE};
    Require(host.SendSessionMessage(PacketType::MatchConfiguration, nextConfig),
            "post-CSS match config broadcast");
    Require(WaitFor([&] {
        LobbySessionEvent event;
        while (client.PopSessionEvent(event)) {
            if (event.type == PacketType::MatchConfiguration) return event.payload == nextConfig;
        }
        return false;
    }, [&] { host.Poll(); client.Poll(); }), "post-CSS new match configured");

    std::vector<uint8_t> nextLoadingReady;
    PayloadWriter nextLoadingWriter(nextLoadingReady);
    Require(nextLoadingWriter.U8(1) &&
            client.SendSessionMessage(PacketType::LoadingReady, nextLoadingReady),
            "post-CSS loading ready send");
    Require(WaitFor([&] {
        LobbySessionEvent event;
        while (host.PopSessionEvent(event)) {
            if (event.type == PacketType::LoadingReady && event.sourcePlayerId == 1)
                return event.payload == nextLoadingReady;
        }
        return false;
    }, [&] { host.Poll(); client.Poll(); }), "post-CSS loading barrier ready");

    std::vector<uint8_t> nextStartMatch;
    PayloadWriter nextStartWriter(nextStartMatch);
    Require(nextStartWriter.U16(60) && nextStartWriter.U8(2) &&
            host.SendSessionMessage(PacketType::StartMatch, nextStartMatch),
            "post-CSS start broadcast");
    Require(WaitFor([&] {
        LobbySessionEvent event;
        while (client.PopSessionEvent(event)) {
            if (event.type == PacketType::StartMatch) return event.payload == nextStartMatch;
        }
        return false;
    }, [&] { host.Poll(); client.Poll(); }), "post-CSS starts third match");

    result.matchId = 3;
    result.finalFrame = 1800;
    result.finalHash = 0x0123456789ABCDEFULL;
    Require(EncodeMatchResultPayload(result, payload) &&
            host.SendSessionMessage(PacketType::MatchResult, payload), "third match result broadcast");
    Require(WaitFor([&] {
        LobbySessionEvent event;
        while (client.PopSessionEvent(event)) {
            MatchResultPayload received{};
            if (event.type == PacketType::MatchResult &&
                DecodeMatchResultPayload(event.payload, received)) return received.matchId == 3;
        }
        return false;
    }, [&] { host.Poll(); client.Poll(); }), "post-CSS match reaches results");

    Require(host.SendSessionMessage(PacketType::LeaveSession, {}), "leave broadcast");
    Require(WaitFor([&] {
        LobbySessionEvent event;
        while (client.PopSessionEvent(event)) if (event.type == PacketType::LeaveSession) return true;
        return false;
    }, [&] { host.Poll(); client.Poll(); }), "leave reaches client");
    host.Stop();
    client.Stop();
}

void TestAdhocModeSessionFallback() {
    LobbySession host;
    LobbySession client;
    std::vector<LobbySession*> clients{&client};

    // Desktop has no ScePspnetAdhoc stack, so the transport intentionally
    // maps LOCAL_ADHOC to loopback sockets. This still exercises the mode
    // dispatch and keeps all lobby/session logic transport-independent.
    Require(host.StartHost(NetplayMode::LocalAdhoc, 0xAD0C0001U, "HOST", "1.2", "127.0.0.1"),
            "adhoc fallback host start");
    Require(client.StartClient(NetplayMode::LocalAdhoc, "127.0.0.1", 0xAD0C0001U, "CLIENT", "1.2"),
            "adhoc fallback client start");
    Require(WaitFor([&] { return client.Snapshot().connected && host.ConnectedPlayerCount() == 2; },
                    [&] { PollAll(host, clients); }),
            "adhoc fallback join");
    host.SetLocalReady(true);
    client.SetLocalReady(true);
    Require(WaitFor([&] { return host.CanHostStart(); }, [&] { PollAll(host, clients); }),
            "adhoc fallback ready");
    Require(host.StartCharacterSelect(), "adhoc fallback start CSS");
    Require(WaitFor([&] { return client.ConsumeCharacterSelectStart(); }, [&] { PollAll(host, clients); }),
            "adhoc fallback CSS broadcast");
    host.Stop(RejectReason::HostClosing, false);
    client.Stop(RejectReason::None, false);
}

// --- rendezvous lobby-board mock + test ---------------------------------------

class MockBoard {
public:
    void Start() {
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        Require(fd >= 0, "mock board socket");
        int one = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in sa{};
        sa.sin_family = AF_INET;
        sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        sa.sin_port = 0;
        Require(::bind(fd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) == 0, "mock board bind");
        Require(::listen(fd, 8) == 0, "mock board listen");
        socklen_t len = sizeof(sa);
        ::getsockname(fd, reinterpret_cast<sockaddr*>(&sa), &len);
        mPort = ntohs(sa.sin_port);
        mListen.store(fd);
        mAccept = std::thread([this] { AcceptLoop(); });
    }

    uint16_t Port() const { return mPort; }

    void Stop() {
        mStop.store(true);
        const int fd = mListen.exchange(-1);
        if (fd >= 0) {
            ::shutdown(fd, SHUT_RDWR);
            ::close(fd);
        }
        if (mAccept.joinable()) mAccept.join();
        for (std::thread& t : mWorkers) {
            if (t.joinable()) t.join();
        }
    }

private:
    struct Rec {
        uint32_t id;
        std::string ip;
        uint16_t lobbyPort;
        uint16_t gameplayPort;
        uint32_t bsnp;
        std::string name;
        uint8_t players;
        uint8_t max;
        uint8_t status;
        std::string build;
        bool alive;
    };

    static bool ReadN(int fd, uint8_t* buf, std::size_t n) {
        std::size_t got = 0;
        while (got < n) {
            const ssize_t r = ::recv(fd, buf + got, n - got, 0);
            if (r <= 0) return false;
            got += static_cast<std::size_t>(r);
        }
        return true;
    }
    static bool ReadFrame(int fd, uint8_t& op, std::vector<uint8_t>& body) {
        uint8_t lenb[4];
        if (!ReadN(fd, lenb, 4)) return false;
        const uint32_t len = (uint32_t(lenb[0]) << 24) | (uint32_t(lenb[1]) << 16) |
                             (uint32_t(lenb[2]) << 8) | lenb[3];
        if (len == 0 || len > 512) return false;
        std::vector<uint8_t> buf(len);
        if (!ReadN(fd, buf.data(), len)) return false;
        op = buf[0];
        body.assign(buf.begin() + 1, buf.end());
        return true;
    }
    static void WriteFrame(int fd, uint8_t op, const std::vector<uint8_t>& body) {
        std::vector<uint8_t> f;
        const uint32_t total = 1 + static_cast<uint32_t>(body.size());
        f.push_back(uint8_t(total >> 24));
        f.push_back(uint8_t(total >> 16));
        f.push_back(uint8_t(total >> 8));
        f.push_back(uint8_t(total));
        f.push_back(op);
        f.insert(f.end(), body.begin(), body.end());
        ::send(fd, f.data(), f.size(), 0);
    }

    struct Reader {
        const uint8_t* p;
        std::size_t n;
        std::size_t pos = 0;
        uint8_t u8() { return pos < n ? p[pos++] : 0; }
        uint16_t u16() {
            const uint16_t a = u8();
            const uint16_t b = u8();
            return uint16_t((a << 8) | b);
        }
        uint32_t u32() {
            const uint32_t a = u8(), b = u8(), c = u8(), d = u8();
            return (a << 24) | (b << 16) | (c << 8) | d;
        }
        std::string str() {
            const uint16_t l = u16();
            std::string s;
            for (uint16_t i = 0; i < l && pos < n; ++i) s.push_back(char(p[pos++]));
            return s;
        }
    };
    static void PutU16(std::vector<uint8_t>& b, uint16_t v) {
        b.push_back(uint8_t(v >> 8));
        b.push_back(uint8_t(v));
    }
    static void PutU32(std::vector<uint8_t>& b, uint32_t v) {
        b.push_back(uint8_t(v >> 24));
        b.push_back(uint8_t(v >> 16));
        b.push_back(uint8_t(v >> 8));
        b.push_back(uint8_t(v));
    }
    static void PutStr(std::vector<uint8_t>& b, const std::string& s) {
        PutU16(b, uint16_t(s.size()));
        b.insert(b.end(), s.begin(), s.end());
    }

    void AcceptLoop() {
        while (!mStop.load()) {
            const int listen = mListen.load();
            if (listen < 0) return;
            const int fd = ::accept(listen, nullptr, nullptr);
            if (fd < 0) return;
            mWorkers.emplace_back([this, fd] { Handle(fd); });
        }
    }

    void Handle(int fd) {
        uint8_t op = 0;
        std::vector<uint8_t> body;
        if (!ReadFrame(fd, op, body)) {
            ::close(fd);
            return;
        }
        Reader r{body.data(), body.size()};
        if (op == 0x02) { // REGISTER
            r.str();
            const std::string build = r.str();
            const std::string name = r.str();
            const uint8_t max = r.u8();
            const uint16_t lp = r.u16();
            const uint16_t gp = r.u16();
            const uint32_t bsnp = r.u32();
            uint32_t id;
            {
                std::lock_guard<std::mutex> lock(mMu);
                id = ++mNextId;
                mLobbies.push_back({id, "127.0.0.1", lp, gp, bsnp, name, 1, max, 0, build, true});
            }
            std::vector<uint8_t> reply;
            PutU32(reply, id);
            WriteFrame(fd, 0x83, reply);
            for (;;) {
                if (!ReadFrame(fd, op, body)) break;
                if (op == 0x05) {
                    WriteFrame(fd, 0x06, {});
                } else if (op == 0x04) {
                    std::lock_guard<std::mutex> lock(mMu);
                    for (Rec& rec : mLobbies) {
                        if (rec.id == id && body.size() >= 2) {
                            rec.players = body[0];
                            rec.status = body[1];
                        }
                    }
                }
            }
            {
                std::lock_guard<std::mutex> lock(mMu);
                for (Rec& rec : mLobbies) {
                    if (rec.id == id) rec.alive = false;
                }
            }
            ::close(fd);
        } else if (op == 0x01) { // LIST
            r.str();
            const std::string build = r.str();
            std::vector<Rec> snap;
            {
                std::lock_guard<std::mutex> lock(mMu);
                for (const Rec& rec : mLobbies) {
                    if (rec.alive && (build.empty() || rec.build == build)) snap.push_back(rec);
                }
            }
            for (const Rec& rec : snap) {
                std::vector<uint8_t> e;
                PutU32(e, rec.id);
                PutStr(e, rec.ip);
                PutU16(e, rec.lobbyPort);
                PutU16(e, rec.gameplayPort);
                PutU32(e, rec.bsnp);
                PutStr(e, rec.name);
                e.push_back(rec.players);
                e.push_back(rec.max);
                e.push_back(rec.status);
                PutStr(e, rec.build);
                WriteFrame(fd, 0x81, e);
            }
            WriteFrame(fd, 0x82, {});
            ::close(fd);
        } else {
            ::close(fd);
        }
    }

    std::atomic<int> mListen{-1};
    uint16_t mPort = 0;
    std::atomic<bool> mStop{false};
    std::thread mAccept;
    std::vector<std::thread> mWorkers;
    std::mutex mMu;
    std::vector<Rec> mLobbies;
    uint32_t mNextId = 0x1000;
};

void TestRendezvousBoard() {
    MockBoard board;
    board.Start();
    const std::string endpoint = "127.0.0.1:" + std::to_string(board.Port());

    RendezvousClient host;
    host.SetServer(endpoint);
    host.StartHost("HOSTX", "1.3", 2, kLobbyPort, kGameplayPort, 0xABCD1234U);
    Require(WaitFor([&] { return host.HostRegistered(); }, [&] { host.Poll(); }),
            "rendezvous host register");

    RendezvousClient client;
    client.SetServer(endpoint);
    client.StartList("1.3");
    Require(WaitFor([&] { return !client.Lobbies().empty(); },
                    [&] { host.Poll(); client.Poll(); }, 3000),
            "rendezvous list fetch");

    const std::vector<DiscoveredLobby> lobbies = client.Lobbies();
    Require(lobbies.size() == 1, "rendezvous one lobby");
    Require(lobbies[0].hostIp == "127.0.0.1", "rendezvous lobby ip");
    Require(lobbies[0].sessionId == 0xABCD1234U, "rendezvous lobby session id");
    Require(lobbies[0].hostName == "HOSTX", "rendezvous lobby name");
    Require(lobbies[0].compatible, "rendezvous lobby compatible");
    Require(lobbies[0].playerCount == 1, "rendezvous lobby player count");

    RendezvousClient other;
    other.SetServer(endpoint);
    other.StartList("9.9");
    Require(WaitFor([&] {
        other.RequestList();
        host.Poll();
        other.Poll();
        return true;
    }, [&] {}, 500), "rendezvous other-build tick");
    Require(other.Lobbies().empty(), "rendezvous build filter");
    other.Stop();

    host.UpdateStatus(2, 1);
    Require(WaitFor([&] {
        client.RequestList();
        host.Poll();
        client.Poll();
        const std::vector<DiscoveredLobby> l = client.Lobbies();
        return !l.empty() && l[0].playerCount == 2 && l[0].status == LobbyStatus::InGame;
    }, [&] {}, 4000), "rendezvous status update");

    host.Stop();
    Require(WaitFor([&] {
        client.RequestList();
        client.Poll();
        return client.Lobbies().empty();
    }, [&] {}, 5000), "rendezvous lobby removed on host stop");

    client.Stop();
    board.Stop();
}

} // namespace

int main() {
    TestProtocolValidation();
    TestDeterministicTraceComparison();
    TestRollbackPolicy();
    TestConfirmedInputBarrier();
    TestDiscovery();
    TestLobbyFourPlayersAndStart();
    TestBuildMismatch();
    TestLobbyFullReject();
    TestHeartbeatTimeout();
    TestGameplayInputTransport();
    TestGameplayLossDuplicateAndReorder();
    TestPostMatchControlLifecycle();
    TestAdhocModeSessionFallback();
    TestRendezvousBoard();
    std::puts("netplay loopback tests: PASS");
    return 0;
}
