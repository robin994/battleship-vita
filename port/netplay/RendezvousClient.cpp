#include "RendezvousClient.h"

#include "../vita/VitaNetResolver.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

extern "C" void port_log(const char* fmt, ...);

namespace ssb64::netplay {

namespace {

constexpr auto kListInterval = std::chrono::seconds(3);
constexpr auto kConnectTimeout = std::chrono::seconds(8);
constexpr auto kListTimeout = std::chrono::seconds(8);
constexpr auto kPingInterval = std::chrono::seconds(5);
constexpr auto kResolveTtl = std::chrono::minutes(5);
constexpr std::size_t kMaxRx = 16384;

constexpr uint8_t kOpList = 0x01;
constexpr uint8_t kOpRegister = 0x02;
constexpr uint8_t kOpUpdate = 0x04;
constexpr uint8_t kOpPing = 0x05;
constexpr uint8_t kOpEntry = 0x81;
constexpr uint8_t kOpListEnd = 0x82;
constexpr uint8_t kOpRegistered = 0x83;

void putU16(std::vector<uint8_t>& b, uint16_t v) {
    b.push_back(static_cast<uint8_t>(v >> 8));
    b.push_back(static_cast<uint8_t>(v));
}
void putU32(std::vector<uint8_t>& b, uint32_t v) {
    b.push_back(static_cast<uint8_t>(v >> 24));
    b.push_back(static_cast<uint8_t>(v >> 16));
    b.push_back(static_cast<uint8_t>(v >> 8));
    b.push_back(static_cast<uint8_t>(v));
}
void putStr(std::vector<uint8_t>& b, const std::string& s) {
    const uint16_t n = static_cast<uint16_t>(std::min<std::size_t>(s.size(), 64));
    putU16(b, n);
    b.insert(b.end(), s.begin(), s.begin() + n);
}

std::vector<uint8_t> makeFrame(uint8_t op, const std::vector<uint8_t>& body) {
    std::vector<uint8_t> f;
    putU32(f, static_cast<uint32_t>(1 + body.size()));
    f.push_back(op);
    f.insert(f.end(), body.begin(), body.end());
    return f;
}

int takeFrame(std::vector<uint8_t>& rx, uint8_t& op, std::vector<uint8_t>& body) {
    if (rx.size() < 4) return 0;
    const uint32_t len = (uint32_t(rx[0]) << 24) | (uint32_t(rx[1]) << 16) |
                         (uint32_t(rx[2]) << 8) | uint32_t(rx[3]);
    if (len == 0 || len > 512) return -1;
    if (rx.size() < 4 + len) return 0;
    op = rx[4];
    body.assign(rx.begin() + 5, rx.begin() + 4 + len);
    rx.erase(rx.begin(), rx.begin() + 4 + static_cast<std::ptrdiff_t>(len));
    return 1;
}

struct FrameReader {
    const uint8_t* p;
    std::size_t n;
    std::size_t pos = 0;
    bool err = false;

    uint8_t u8() {
        if (err || pos + 1 > n) {
            err = true;
            return 0;
        }
        return p[pos++];
    }
    uint16_t u16() {
        const uint16_t hi = u8();
        const uint16_t lo = u8();
        return static_cast<uint16_t>((hi << 8) | lo);
    }
    uint32_t u32() {
        const uint32_t a = u8();
        const uint32_t b = u8();
        const uint32_t c = u8();
        const uint32_t d = u8();
        return (a << 24) | (b << 16) | (c << 8) | d;
    }
    std::string str() {
        const uint16_t len = u16();
        if (err || len > 64 || pos + len > n) {
            err = true;
            return {};
        }
        std::string s(reinterpret_cast<const char*>(p + pos), len);
        pos += len;
        return s;
    }
};

bool sendAll(transport::SocketHandle sock, const std::vector<uint8_t>& data) {
    std::size_t off = 0;
    while (off < data.size()) {
        const int r = transport::SendReliable(NetplayMode::Online, sock, data.data() + off, data.size() - off);
        if (r > 0) {
            off += static_cast<std::size_t>(r);
            continue;
        }
        if (r < 0 && transport::IsWouldBlock(r)) {
            return true;
        }
        return false;
    }
    return true;
}

} // namespace

RendezvousClient::~RendezvousClient() {
    Stop();
}

void RendezvousClient::SetServer(const std::string& host) {
    std::string trimmed = host;
    const std::size_t first = trimmed.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        trimmed.clear();
    } else {
        trimmed = trimmed.substr(first, trimmed.find_last_not_of(" \t\r\n") - first + 1);
    }

    std::string name = trimmed;
    uint16_t port = kRendezvousPort;
    const std::size_t colon = trimmed.rfind(':');
    if (colon != std::string::npos && trimmed.find(':') == colon) {
        const std::string portStr = trimmed.substr(colon + 1);
        if (!portStr.empty() && portStr.find_first_not_of("0123456789") == std::string::npos) {
            const long p = std::strtol(portStr.c_str(), nullptr, 10);
            if (p > 0 && p < 65536) {
                name = trimmed.substr(0, colon);
                port = static_cast<uint16_t>(p);
            }
        }
    }

    if (name == mHost && port == mServerPort) return;
    mHost = name;
    mServerPort = port;
    mResolvedIp = 0;
    mResolvedAt = Clock::time_point{};
}

void RendezvousClient::StartList(const std::string& buildId) {
    if (mHost.empty()) return;
    mBuildId = buildId;
    if (mMode == Mode::Hosting) CloseHost();
    mMode = Mode::Listing;
    mFatal = false;
    mNextList = Clock::now();
}

void RendezvousClient::RequestList() {
    if (mMode == Mode::Listing) mNextList = Clock::now();
}

void RendezvousClient::StartHost(const std::string& hostName, const std::string& buildId, uint8_t maxPlayers,
                                 uint16_t lobbyPort, uint16_t gameplayPort, uint32_t bsnpSessionId) {
    if (mHost.empty()) return;
    if (mMode == Mode::Listing) CloseList();
    mMode = Mode::Hosting;
    mBuildId = buildId;
    mHostName = hostName;
    mHostMax = (maxPlayers < 2 || maxPlayers > 4) ? static_cast<uint8_t>(kMaxPlayers) : maxPlayers;
    mHostLobbyPort = lobbyPort;
    mHostGameplayPort = gameplayPort;
    mHostBsnpSession = bsnpSessionId;
    mPlayers = 1;
    mStatus = 0;
    mSentPlayers = 0xFF;
    mSentStatus = 0xFF;
    mRegistered = false;
    mRegisterSent = false;
    mFatal = false;
    mLobbyId = 0;
    mHostStartedAt = Clock::time_point{};
    CloseHost();
    mHostStartedAt = Clock::now();
}

void RendezvousClient::UpdateStatus(uint8_t players, uint8_t status) {
    mPlayers = players;
    mStatus = status;
}

void RendezvousClient::Stop() {
    CloseList();
    CloseHost();
    mMode = Mode::Idle;
    mLobbies.clear();
    mPendingList.clear();
    mFatal = false;
    mLastMessage.clear();
}

bool RendezvousClient::Resolve() {
    const Clock::time_point now = Clock::now();
    if (mResolvedIp != 0 && now - mResolvedAt < kResolveTtl) return true;
    uint32_t ip = 0;
    if (!ResolveHostV4(mHost, ip)) {
        mResolvedIp = 0;
        mLastMessage = "SERVER NAME LOOKUP FAILED";
        return false;
    }
    mResolvedIp = ip;
    mResolvedAt = now;
    return true;
}

transport::SocketHandle RendezvousClient::DialServer(const char* name, Clock::time_point& startedAt,
                                                     bool& connecting) {
    if (!Resolve()) return transport::kInvalidSocket;
    transport::SocketAddress addr{};
    addr.ipv4 = mResolvedIp;
    addr.port = mServerPort;
    addr.isAdhoc = false;

    transport::SocketHandle sock = transport::kInvalidSocket;
    const transport::ConnectResult r =
        transport::CreateReliableClient(NetplayMode::Online, name, addr, 0, sock);
    if (r == transport::ConnectResult::Failed) {
        mLastMessage = "SERVER UNREACHABLE";
        if (sock != transport::kInvalidSocket) transport::CloseReliable(NetplayMode::Online, sock);
        return transport::kInvalidSocket;
    }
    connecting = (r == transport::ConnectResult::InProgress);
    startedAt = Clock::now();
    return sock;
}

bool RendezvousClient::ReadFrames(transport::SocketHandle& sock, std::vector<uint8_t>& rx) {
    std::array<uint8_t, 1536> buf{};
    for (int budget = 0; budget < 8; ++budget) {
        const int received =
            transport::RecvReliable(NetplayMode::Online, sock, buf.data(), buf.size());
        if (received == 0) return false;
        if (received < 0) {
            if (transport::IsWouldBlock(received)) return true;
            return false;
        }
        if (rx.size() + static_cast<std::size_t>(received) > kMaxRx) return false;
        rx.insert(rx.end(), buf.begin(), buf.begin() + received);
    }
    return true;
}

void RendezvousClient::CloseList() {
    if (mListSocket != transport::kInvalidSocket) {
        transport::CloseReliable(NetplayMode::Online, mListSocket);
    }
    mListSocket = transport::kInvalidSocket;
    mListConnecting = false;
    mListSent = false;
    mListRx.clear();
    mPendingList.clear();
}

void RendezvousClient::CloseHost() {
    if (mHostSocket != transport::kInvalidSocket) {
        transport::CloseReliable(NetplayMode::Online, mHostSocket);
    }
    mHostSocket = transport::kInvalidSocket;
    mHostConnecting = false;
    mRegisterSent = false;
    mRegistered = false;
    mHostRx.clear();
}

void RendezvousClient::Poll() {
    if (mMode == Mode::Listing) PollList();
    else if (mMode == Mode::Hosting) PollHost();
}

void RendezvousClient::PollList() {
    const Clock::time_point now = Clock::now();

    if (mListSocket == transport::kInvalidSocket) {
        if (now < mNextList) return;
        mListSocket = DialServer("bsnp_rdv_list", mListStartedAt, mListConnecting);
        if (mListSocket == transport::kInvalidSocket) {
            mNextList = now + kListInterval;
            return;
        }
        mListRx.clear();
        mListSent = false;
        mPendingList.clear();
    }

    if (now - mListStartedAt > kListTimeout) {
        CloseList();
        mNextList = now + kListInterval;
        return;
    }

    if (mListConnecting) {
        int sockErr = 0;
        if (transport::GetSocketError(NetplayMode::Online, mListSocket, sockErr) && sockErr != 0) {
            CloseList();
            mNextList = now + kListInterval;
            return;
        }
        if (!transport::IsConnected(NetplayMode::Online, mListSocket)) return;
        mListConnecting = false;
    }

    if (!mListSent) {
        std::vector<uint8_t> body;
        putStr(body, std::string());
        putStr(body, mBuildId);
        if (!sendAll(mListSocket, makeFrame(kOpList, body))) {
            CloseList();
            mNextList = now + kListInterval;
            return;
        }
        mListSent = true;
    }

    const bool listAlive = ReadFrames(mListSocket, mListRx);

    uint8_t op = 0;
    std::vector<uint8_t> fb;
    for (;;) {
        const int r = takeFrame(mListRx, op, fb);
        if (r == 0) break;
        if (r < 0) {
            CloseList();
            mNextList = now + kListInterval;
            return;
        }
        if (op == kOpEntry) {
            FrameReader fr{fb.data(), fb.size()};
            DiscoveredLobby lobby{};
            const uint32_t lobbyId = fr.u32();
            lobby.hostIp = fr.str();
            const uint16_t lobbyPort = fr.u16();
            const uint16_t gameplayPort = fr.u16();
            lobby.sessionId = fr.u32();
            lobby.hostName = fr.str();
            lobby.playerCount = fr.u8();
            lobby.maxPlayers = fr.u8();
            const uint8_t status = fr.u8();
            lobby.buildId = fr.str();
            if (fr.err || lobby.hostIp.empty() || lobbyPort != kLobbyPort ||
                gameplayPort != kGameplayPort || lobby.sessionId == 0) {
                continue;
            }
            (void)lobbyId;
            lobby.protocolVersion = kProtocolVersion;
            lobby.status = (status == 0) ? LobbyStatus::Open : LobbyStatus::InGame;
            lobby.pingMs = 0;
            lobby.compatible = (lobby.buildId == mBuildId);
            mPendingList.push_back(std::move(lobby));
        } else if (op == kOpListEnd) {
            mLobbies = mPendingList;
            mPendingList.clear();
            CloseList();
            mNextList = now + kListInterval;
            return;
        }
    }

    if (!listAlive) {
        CloseList();
        mNextList = now + kListInterval;
    }
}

void RendezvousClient::PollHost() {
    const Clock::time_point now = Clock::now();

    if (mHostSocket == transport::kInvalidSocket) {
        if (mFatal) return;
        mHostSocket = DialServer("bsnp_rdv_host", mHostStartedAt, mHostConnecting);
        if (mHostSocket == transport::kInvalidSocket) {
            mFatal = true;
            return;
        }
        mHostRx.clear();
        mRegisterSent = false;
        mRegistered = false;
    }

    if (!mRegistered && now - mHostStartedAt > kConnectTimeout) {
        CloseHost();
        mFatal = true;
        mLastMessage = "SERVER UNREACHABLE";
        return;
    }

    if (mHostConnecting) {
        int sockErr = 0;
        if (transport::GetSocketError(NetplayMode::Online, mHostSocket, sockErr) && sockErr != 0) {
            CloseHost();
            mFatal = true;
            mLastMessage = "SERVER UNREACHABLE";
            return;
        }
        if (!transport::IsConnected(NetplayMode::Online, mHostSocket)) return;
        mHostConnecting = false;
    }

    if (!mRegisterSent) {
        std::vector<uint8_t> body;
        putStr(body, std::string());
        putStr(body, mBuildId);
        putStr(body, mHostName);
        body.push_back(mHostMax);
        putU16(body, mHostLobbyPort);
        putU16(body, mHostGameplayPort);
        putU32(body, mHostBsnpSession);
        if (!sendAll(mHostSocket, makeFrame(kOpRegister, body))) {
            CloseHost();
            mFatal = true;
            mLastMessage = "SERVER UNREACHABLE";
            return;
        }
        mRegisterSent = true;
    }

    const bool hostAlive = ReadFrames(mHostSocket, mHostRx);

    uint8_t op = 0;
    std::vector<uint8_t> fb;
    for (;;) {
        const int r = takeFrame(mHostRx, op, fb);
        if (r == 0) break;
        if (r < 0) {
            CloseHost();
            mFatal = true;
            mLastMessage = "SERVER PROTOCOL ERROR";
            return;
        }
        if (op == kOpRegistered) {
            FrameReader fr{fb.data(), fb.size()};
            mLobbyId = fr.u32();
            mRegistered = true;
            mNextPing = now + kPingInterval;
            mLastMessage = "LOBBY PUBLISHED";
            port_log("[NETPLAY] rendezvous registered lobby=%u\n", mLobbyId);
        }
    }

    if (!hostAlive) {
        CloseHost();
        mFatal = true;
        mLastMessage = mRegistered ? "SERVER CONNECTION LOST" : "SERVER UNREACHABLE";
        return;
    }

    if (!mRegistered) return;

    if (mSentPlayers != mPlayers || mSentStatus != mStatus) {
        std::vector<uint8_t> body;
        body.push_back(mPlayers);
        body.push_back(mStatus);
        if (!sendAll(mHostSocket, makeFrame(kOpUpdate, body))) {
            CloseHost();
            mFatal = true;
            mLastMessage = "SERVER CONNECTION LOST";
            return;
        }
        mSentPlayers = mPlayers;
        mSentStatus = mStatus;
    }

    if (now >= mNextPing) {
        if (!sendAll(mHostSocket, makeFrame(kOpPing, {}))) {
            CloseHost();
            mFatal = true;
            mLastMessage = "SERVER CONNECTION LOST";
            return;
        }
        mNextPing = now + kPingInterval;
    }
}

} // namespace ssb64::netplay
