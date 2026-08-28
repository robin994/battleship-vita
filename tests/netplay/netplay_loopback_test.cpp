#include "../../port/netplay/LanDiscovery.h"
#include "../../port/netplay/LobbySession.h"
#include "../../port/netplay/NetplayProtocol.h"
#include "../../port/vita/VitaNetworkTransport.h"

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
    Require(startWriter.U16(60), "start match encode");
    Require(host.SendSessionMessage(PacketType::StartMatch, startMatch), "start match broadcast");
    bool c3SawStart = false;
    Require(WaitFor([&] {
        LobbySessionEvent event;
        while (c3.PopSessionEvent(event)) {
            if (event.type == PacketType::StartMatch && event.payload == startMatch) c3SawStart = true;
        }
        return c3SawStart;
    }, [&] { PollAll(host, clients); }), "start match reaches client");

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

} // namespace

int main() {
    TestProtocolValidation();
    TestDiscovery();
    TestLobbyFourPlayersAndStart();
    TestBuildMismatch();
    TestLobbyFullReject();
    TestHeartbeatTimeout();
    TestAdhocModeSessionFallback();
    std::puts("netplay loopback tests: PASS");
    return 0;
}
