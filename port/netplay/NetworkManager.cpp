#include "NetworkManager.h"

#include "../port_log.h"
#include "../vita/VitaNetworkPlatform.h"
#ifdef __vita__
#include "../mods/VitaModLoader.h"
namespace ssb64 {
void MountModsDir();
void UnmountAllMods();
}
#endif

#include <libultraship/bridge/consolevariablebridge.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <utility>

namespace ssb64::netplay {
namespace {

#ifdef __vita__
bool sOnlineVanillaActive = false;

void SetOnlineVanillaRuntime(bool enabled) {
    if (sOnlineVanillaActive == enabled) return;

    if (enabled) {
        // Do not mutate per-mod enable preferences: ONLINE is a temporary
        // vanilla sandbox and the user's offline setup must come back intact.
        ssb64::mods::VitaModLoader::UnloadAll();
        ssb64::UnmountAllMods();
        sOnlineVanillaActive = true;
        port_log("[NETPLAY] ONLINE vanilla mode enabled; native mods unloaded and archives unmounted\n");
    } else {
        sOnlineVanillaActive = false;
        ssb64::MountModsDir();
        ssb64::mods::VitaModLoader::LoadAll();
        port_log("[NETPLAY] ONLINE vanilla mode disabled; offline mod setup restored\n");
    }
}
#endif

constexpr const char* kPlayerNameCVar = "gNetplay.PlayerName";
constexpr const char* kInputDelayCVar = "gNetplay.InputDelay";
constexpr const char* kShowStatsCVar = "gNetplay.ShowStats";
constexpr int kAutoInputDelay = -1;
constexpr int kMaxInputDelay = 4;
constexpr std::size_t kMaxUiPlayerNameChars = 12;
constexpr auto kWorkerPollInterval = std::chrono::milliseconds(10);
constexpr auto kPlatformPollInterval = std::chrono::milliseconds(500);

#ifndef BATTLESHIP_CURRENT_VERSION
#define BATTLESHIP_CURRENT_VERSION "dev"
#endif
constexpr const char* kNetplayBuildId = BATTLESHIP_CURRENT_VERSION;

std::string SanitizePlayerName(const std::string& input) {
    std::string out;
    out.reserve(std::min(input.size(), kMaxUiPlayerNameChars));
    bool previousSpace = false;

    for (unsigned char raw : input) {
        if (out.size() >= kMaxUiPlayerNameChars) break;
        const char c = static_cast<char>(std::toupper(raw));
        if (c >= 'A' && c <= 'Z') {
            out.push_back(c);
            previousSpace = false;
        } else if (c == ' ' && !out.empty() && !previousSpace) {
            out.push_back(' ');
            previousSpace = true;
        }
    }
    while (!out.empty() && out.back() == ' ') {
        out.pop_back();
    }
    if (out.empty()) {
        out = "PLAYER";
    }
    return out;
}

void CopyStringOut(const std::string& value, char* out, int outSize) {
    if (out == nullptr || outSize <= 0) return;
    const std::size_t count = std::min(value.size(), static_cast<std::size_t>(outSize - 1));
    if (count != 0) {
        std::memcpy(out, value.data(), count);
    }
    out[count] = '\0';
}

bool EncodeCssInputPayload(uint8_t playerId, uint32_t sequence, uint16_t buttons,
                           int8_t stickX, int8_t stickY, std::vector<uint8_t>& payload) {
    payload.clear();
    PayloadWriter writer(payload);
    return writer.U8(playerId) && writer.U32(sequence) && writer.U16(buttons) &&
           writer.S8(stickX) && writer.S8(stickY);
}

bool DecodeCssInputPayload(const std::vector<uint8_t>& payload, uint8_t& playerId,
                           uint32_t& sequence, uint16_t& buttons, int8_t& stickX, int8_t& stickY) {
    PayloadReader reader(payload.data(), payload.size());
    return reader.U8(playerId) && reader.U32(sequence) && reader.U16(buttons) &&
           reader.S8(stickX) && reader.S8(stickY) && reader.Empty() && playerId < kMaxPlayers;
}

bool EncodeCssSelectionPayload(uint8_t playerId, uint8_t fighterKind, uint8_t costume,
                               uint8_t shade, std::vector<uint8_t>& payload) {
    payload.clear();
    PayloadWriter writer(payload);
    return writer.U8(playerId) && writer.U8(fighterKind) && writer.U8(costume) && writer.U8(shade);
}

bool EncodeMatchConfigPayload(const PortNetplayMatchConfig& config, std::vector<uint8_t>& payload) {
    payload.clear();
    PayloadWriter writer(payload);
    if (config.player_count < 2 || config.player_count > kMaxPlayers ||
        !writer.U32(config.rng_seed) || !writer.U32(config.stage_kind) ||
        !writer.U32(config.stocks) || !writer.U32(config.time_limit) ||
        !writer.U32(config.item_switch) || !writer.U32(config.item_toggles) ||
        !writer.U8(config.game_type) || !writer.U8(config.game_rules) ||
        !writer.U8(config.is_team_battle) || !writer.U8(config.handicap) ||
        !writer.U8(config.is_team_attack) || !writer.U8(config.damage_ratio) ||
        !writer.U8(config.item_appearance_rate) || !writer.U8(config.is_not_teamshadows) ||
        !writer.U8(config.player_count)) {
        return false;
    }
    for (std::size_t i = 0; i < kMaxPlayers; ++i) {
        if (!writer.U8(config.player_kinds[i]) || !writer.U8(config.fighter_kinds[i]) ||
            !writer.U8(config.costumes[i]) || !writer.U8(config.teams[i]) ||
            !writer.U8(config.handicaps[i]) || !writer.U8(config.levels[i]) ||
            !writer.U8(config.shades[i])) {
            return false;
        }
    }
    return true;
}

bool DecodeMatchConfigPayload(const std::vector<uint8_t>& payload, PortNetplayMatchConfig& config) {
    PortNetplayMatchConfig decoded{};
    PayloadReader reader(payload.data(), payload.size());
    if (!reader.U32(decoded.rng_seed) || !reader.U32(decoded.stage_kind) ||
        !reader.U32(decoded.stocks) || !reader.U32(decoded.time_limit) ||
        !reader.U32(decoded.item_switch) || !reader.U32(decoded.item_toggles) ||
        !reader.U8(decoded.game_type) || !reader.U8(decoded.game_rules) ||
        !reader.U8(decoded.is_team_battle) || !reader.U8(decoded.handicap) ||
        !reader.U8(decoded.is_team_attack) || !reader.U8(decoded.damage_ratio) ||
        !reader.U8(decoded.item_appearance_rate) || !reader.U8(decoded.is_not_teamshadows) ||
        !reader.U8(decoded.player_count)) {
        return false;
    }
    if (decoded.player_count < 2 || decoded.player_count > kMaxPlayers) return false;
    for (std::size_t i = 0; i < kMaxPlayers; ++i) {
        if (!reader.U8(decoded.player_kinds[i]) || !reader.U8(decoded.fighter_kinds[i]) ||
            !reader.U8(decoded.costumes[i]) || !reader.U8(decoded.teams[i]) ||
            !reader.U8(decoded.handicaps[i]) || !reader.U8(decoded.levels[i]) ||
            !reader.U8(decoded.shades[i])) {
            return false;
        }
    }
    if (!reader.Empty()) return false;
    config = decoded;
    return true;
}

} // namespace

NetworkManager& NetworkManager::Instance() {
    static NetworkManager manager;
    return manager;
}

NetworkManager::~NetworkManager() {
    Shutdown();
}

void NetworkManager::LoadSettings() {
    std::lock_guard<std::mutex> lock(mMutex);
    if (mSettingsLoaded) return;

    const char* storedName = CVarGetString(kPlayerNameCVar, "PLAYER");
    mSettings.playerName = SanitizePlayerName(storedName != nullptr ? storedName : "PLAYER");
    mSettings.inputDelay = CVarGetInteger(kInputDelayCVar, kAutoInputDelay);
    if (mSettings.inputDelay < kAutoInputDelay || mSettings.inputDelay > kMaxInputDelay) {
        mSettings.inputDelay = kAutoInputDelay;
    }
    mSettings.showStats = CVarGetInteger(kShowStatsCVar, 0) != 0;
    mSettingsLoaded = true;
}

void NetworkManager::SaveSettings() {
    NetplaySettings settings;
    {
        std::lock_guard<std::mutex> lock(mMutex);
        settings = mSettings;
    }
    CVarSetString(kPlayerNameCVar, settings.playerName.c_str());
    CVarSetInteger(kInputDelayCVar, settings.inputDelay);
    CVarSetInteger(kShowStatsCVar, settings.showStats ? 1 : 0);
    CVarSave();
}

void NetworkManager::EnterMultiplayerMenu() {
    LoadSettings();
    if (State() == NetplayState::Error || State() == NetplayState::Disconnected) {
        ForceTransition(NetplayState::Offline, "menu reopened");
    }
    StartWorker();
}

void NetworkManager::LeaveMultiplayerMenu() {
    Enqueue(Command{CommandType::CancelActivity});
    StopWorker();
    mMode.store(NetplayMode::None, std::memory_order_release);
    ForceTransition(NetplayState::Offline, "left multiplayer menu");
}

void NetworkManager::Shutdown() {
    StopWorker();
    mMode.store(NetplayMode::None, std::memory_order_release);
    mState.store(NetplayState::Offline, std::memory_order_release);
}

void NetworkManager::StartWorker() {
    std::lock_guard<std::mutex> lock(mMutex);
    if (mWorkerRunning) return;

    mStopRequested = false;
    mWorkerRunning = true;
    try {
        mWorker = std::thread(&NetworkManager::WorkerMain, this);
    } catch (...) {
        mWorkerRunning = false;
        mLastError = "FAILED TO START NETWORK THREAD";
        mState.store(NetplayState::Error, std::memory_order_release);
        port_log("[NETPLAY] failed to create network worker thread\n");
    }
}

void NetworkManager::StopWorker() {
    std::thread worker;
    {
        std::lock_guard<std::mutex> lock(mMutex);
        if (!mWorkerRunning) return;
        mStopRequested = true;
        mWake.notify_all();
        worker = std::move(mWorker);
    }

    if (worker.joinable()) {
        worker.join();
    }

    std::lock_guard<std::mutex> lock(mMutex);
    mWorkerRunning = false;
    mStopRequested = false;
}

void NetworkManager::WorkerMain() {
    port_log("[NETPLAY] network worker started\n");
    platform::NetworkPlatformStatus platformStatus = platform::Initialize();
    if (!platformStatus.initialized) {
        SetError(platformStatus.errorCode, "network init");
    } else {
        mNetworkInitialized.store(true, std::memory_order_release);
        mNetworkConnected.store(platformStatus.connected, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(mMutex);
            mLocalIp = platformStatus.localIp;
            mLocalMac = platformStatus.localMac;
            mLastError.clear();
        }
    }

    auto nextPlatformPoll = std::chrono::steady_clock::now();
    for (;;) {
        {
            std::unique_lock<std::mutex> lock(mMutex);
            if (mWake.wait_for(lock, kWorkerPollInterval, [this] {
                    return mStopRequested || !mCommands.empty();
                }) && mStopRequested) {
                break;
            }
        }

        ProcessCommands();
        if (mNetworkInitialized.load(std::memory_order_acquire)) {
            PollNetworkServices();
        }

        const auto now = std::chrono::steady_clock::now();
        if (mNetworkInitialized.load(std::memory_order_acquire) && now >= nextPlatformPoll) {
            platformStatus = platform::Query();
            mNetworkConnected.store(platformStatus.connected, std::memory_order_release);
            {
                std::lock_guard<std::mutex> lock(mMutex);
                mLocalIp = platformStatus.localIp;
                if (!platformStatus.localMac.empty()) mLocalMac = platformStatus.localMac;
            }
            if (platformStatus.errorCode < 0 && State() != NetplayState::Error) {
                port_log("[NETPLAY] network status query failed code=0x%08X\n",
                         static_cast<unsigned int>(platformStatus.errorCode));
            }
            if (Mode() == NetplayMode::Online && !platformStatus.connected &&
                State() != NetplayState::Offline && State() != NetplayState::Disconnected &&
                State() != NetplayState::Error) {
                mLobby.Stop(RejectReason::None, false);
                mDiscovery.Stop();
                ForceTransition(NetplayState::Disconnected, "wifi disconnected");
            }
            nextPlatformPoll = now + kPlatformPollInterval;
        }
        PublishSnapshots();
    }

    mLobby.Stop(RejectReason::HostClosing, true);
    mDiscovery.Stop();
    PublishSnapshots();
    platform::Shutdown();
    mNetworkInitialized.store(false, std::memory_order_release);
    mNetworkConnected.store(false, std::memory_order_release);
    mAdhocInitialized.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(mMutex);
        mLocalIp.clear();
        mLocalMac.clear();
    }
    port_log("[NETPLAY] network worker stopped\n");
}

void NetworkManager::Enqueue(Command command) {
    StartWorker();
    {
        std::lock_guard<std::mutex> lock(mMutex);
        mCommands.push_back(std::move(command));
    }
    mWake.notify_all();
}

void NetworkManager::ProcessCommands() {
    for (;;) {
        Command command;
        {
            std::lock_guard<std::mutex> lock(mMutex);
            if (mCommands.empty()) break;
            command = std::move(mCommands.front());
            mCommands.pop_front();
        }

        if (command.type != CommandType::CancelActivity && command.type != CommandType::SetMode &&
            !ModeReady()) {
            {
                std::lock_guard<std::mutex> lock(mMutex);
                mLastError = Mode() == NetplayMode::LocalAdhoc ? "ADHOC NOT READY" : "WIFI DISCONNECTED";
            }
            ForceTransition(NetplayState::Disconnected, "network command without selected transport");
            continue;
        }

        switch (command.type) {
            case CommandType::SetMode: {
                mLobby.Stop(RejectReason::None, true);
                mDiscovery.Stop();
                if (command.mode == NetplayMode::LocalAdhoc) {
                    platform::NetworkPlatformStatus status = platform::InitializeAdhoc();
                    if (!status.adhocInitialized) {
                        mAdhocInitialized.store(false, std::memory_order_release);
                        SetError(status.errorCode, "adhoc init");
                        break;
                    }
                    mAdhocInitialized.store(true, std::memory_order_release);
                    {
                        std::lock_guard<std::mutex> lock(mMutex);
                        mLocalMac = status.localMac;
                        mLastError.clear();
                    }
                } else {
                    platform::ShutdownAdhoc();
                    mAdhocInitialized.store(false, std::memory_order_release);
                    std::lock_guard<std::mutex> lock(mMutex);
                    mLocalMac.clear();
                }
                mMode.store(command.mode, std::memory_order_release);
                ForceTransition(NetplayState::Offline,
                                command.mode == NetplayMode::LocalAdhoc ? "LOCAL ADHOC selected" :
                                command.mode == NetplayMode::Online ? "ONLINE selected" : "multiplayer mode cleared");
                port_log("[NETPLAY] mode selected=%s endpoint=%s\n",
                         command.mode == NetplayMode::LocalAdhoc ? "LOCAL_ADHOC" :
                         command.mode == NetplayMode::Online ? "ONLINE" : "NONE",
                         LocalEndpoint().c_str());
                break;
            }

            case CommandType::StartDiscovery:
                mLobby.Stop(RejectReason::None, true);
                mDiscovery.Stop();
                if (!mDiscovery.StartClient(Mode(), kNetplayBuildId)) {
                    SetError(transport::LastError(), "discovery init");
                    break;
                }
                ForceTransition(NetplayState::Discovering, "LAN discovery started");
                break;

            case CommandType::RefreshDiscovery:
                if (!mDiscovery.IsClientActive()) {
                    if (!mDiscovery.StartClient(Mode(), kNetplayBuildId)) {
                        SetError(transport::LastError(), "discovery refresh");
                        break;
                    }
                    ForceTransition(NetplayState::Discovering, "LAN discovery refreshed");
                }
                mDiscovery.RequestImmediateScan();
                break;

            case CommandType::HostLobby: {
                mLobby.Stop(RejectReason::None, true);
                mDiscovery.Stop();
                NetplaySettings settings;
                std::string localEndpoint;
                {
                    std::lock_guard<std::mutex> lock(mMutex);
                    settings = mSettings;
                    localEndpoint = Mode() == NetplayMode::LocalAdhoc ? mLocalMac : mLocalIp;
                }
                const uint32_t sessionId = MakeSessionId();
                if (!mLobby.StartHost(Mode(), sessionId, settings.playerName, kNetplayBuildId, localEndpoint)) {
                    SetError(transport::LastError(), "lobby host");
                    break;
                }
                const LobbyView lobby = mLobby.Snapshot();
                DiscoveryHostInfo info{};
                info.sessionId = sessionId;
                info.localIp = localEndpoint;
                info.hostName = settings.playerName;
                info.buildId = kNetplayBuildId;
                info.playerCount = mLobby.ConnectedPlayerCount();
                info.status = lobby.status;
                if (!mDiscovery.StartHost(Mode(), info)) {
                    mLobby.Stop(RejectReason::HostClosing, true);
                    SetError(transport::LastError(), "discovery host");
                    break;
                }
                ForceTransition(NetplayState::HostingLobby, "local lobby created");
                break;
            }

            case CommandType::JoinLobby: {
                mDiscovery.Stop();
                mLobby.Stop(RejectReason::None, true);
                NetplaySettings settings;
                {
                    std::lock_guard<std::mutex> lock(mMutex);
                    settings = mSettings;
                }
                if (!mLobby.StartClient(Mode(), command.hostIp, command.sessionId,
                                        settings.playerName, kNetplayBuildId)) {
                    {
                        std::lock_guard<std::mutex> lock(mMutex);
                        mLastError = mLobby.Snapshot().lastMessage;
                    }
                    ForceTransition(NetplayState::Disconnected, "lobby connect failed");
                    break;
                }
                ForceTransition(NetplayState::Connecting, "joining LAN lobby");
                break;
            }

            case CommandType::CancelActivity:
                mLobby.Stop(RejectReason::None, true);
                mDiscovery.Stop();
                ForceTransition(NetplayState::Offline, "network activity cancelled");
                break;

            case CommandType::SetReady:
                mLobby.SetLocalReady(command.ready);
                break;

            case CommandType::StartCharacterSelect:
                if (!mLobby.StartCharacterSelect()) {
                    port_log("[NETPLAY] start denied players=%u local_ready=%d\n",
                             mLobby.ConnectedPlayerCount(), mLobby.LocalReady() ? 1 : 0);
                }
                break;

            case CommandType::CssInput: {
                const LobbyView lobby = mLobby.Snapshot();
                if (State() != NetplayState::CharacterSelect || !lobby.connected ||
                    lobby.localPlayerId >= kMaxPlayers) {
                    break;
                }
                std::vector<uint8_t> payload;
                if (EncodeCssInputPayload(lobby.localPlayerId, command.sequence, command.buttons,
                                          command.stickX, command.stickY, payload)) {
                    mLobby.SendSessionMessage(PacketType::CharacterCursorInput, payload);
                }
                break;
            }

            case CommandType::CssLock:
            case CommandType::CssUnlock: {
                const LobbyView lobby = mLobby.Snapshot();
                if (State() != NetplayState::CharacterSelect || !lobby.connected ||
                    lobby.localPlayerId >= kMaxPlayers) {
                    break;
                }
                std::vector<uint8_t> payload;
                if (EncodeCssSelectionPayload(lobby.localPlayerId,
                                              command.type == CommandType::CssLock ? command.fighterKind : 0xFF,
                                              command.costume, command.shade, payload)) {
                    mLobby.SendSessionMessage(command.type == CommandType::CssLock
                                                  ? PacketType::CharacterLocked
                                                  : PacketType::CharacterUnlocked,
                                              payload);
                    port_log("[NETPLAY] CSS %s slot=P%u fighter=%u costume=%u shade=%u\n",
                             command.type == CommandType::CssLock ? "lock" : "unlock",
                             lobby.localPlayerId + 1, command.fighterKind, command.costume, command.shade);
                }
                break;
            }

            case CommandType::CommitMatchConfig: {
                if (State() != NetplayState::CharacterSelect || !mLobby.IsHost()) break;
                std::vector<uint8_t> payload;
                if (!EncodeMatchConfigPayload(command.matchConfig, payload)) {
                    port_log("[NETPLAY] rejected invalid host match configuration\n");
                    break;
                }
                {
                    std::lock_guard<std::mutex> lock(mMutex);
                    mMatchConfig = command.matchConfig;
                    mMatchConfigValid = true;
                    mLoadingReady.fill(false);
                    mLoadingReadySent = false;
                    mStartMatchReceived = false;
                    mMatchStartCountdown = -1;
                }
                mLobby.SendSessionMessage(PacketType::MatchConfiguration, payload);
                ForceTransition(NetplayState::LoadingMatch, "host match configuration committed");
                port_log("[NETPLAY] match config host stage=%u seed=%u players=%u\n",
                         command.matchConfig.stage_kind, command.matchConfig.rng_seed,
                         command.matchConfig.player_count);
                break;
            }

            case CommandType::LoadingReady: {
                if (State() != NetplayState::LoadingMatch) break;
                const LobbyView lobby = mLobby.Snapshot();
                if (!lobby.connected || lobby.localPlayerId >= kMaxPlayers) break;
                std::vector<uint8_t> payload;
                PayloadWriter writer(payload);
                if (!writer.U8(lobby.localPlayerId)) break;
                if (mLobby.IsHost()) {
                    std::lock_guard<std::mutex> lock(mMutex);
                    mLoadingReady[lobby.localPlayerId] = true;
                    mLoadingReadySent = true;
                } else if (mLobby.SendSessionMessage(PacketType::LoadingReady, payload)) {
                    std::lock_guard<std::mutex> lock(mMutex);
                    mLoadingReadySent = true;
                }
                port_log("[NETPLAY] loading ready local=P%u\n", lobby.localPlayerId + 1);
                break;
            }
        }
    }
}

void NetworkManager::PollNetworkServices() {
    mDiscovery.Poll();
    mLobby.Poll();

    LobbySessionEvent event;
    while (mLobby.PopSessionEvent(event)) {
        if (event.type == PacketType::CharacterCursorInput) {
            uint8_t playerId = 0xFF;
            uint32_t sequence = 0;
            uint16_t buttons = 0;
            int8_t stickX = 0;
            int8_t stickY = 0;
            if (!DecodeCssInputPayload(event.payload, playerId, sequence, buttons, stickX, stickY)) continue;
            if (mLobby.IsHost() && event.sourcePlayerId != playerId) continue;

            std::lock_guard<std::mutex> lock(mMutex);
            CssInputState& input = mCssInputs[playerId];
            if (input.valid && sequence <= input.sequence) continue;
            const uint16_t previous = input.buttons;
            input.tapPending |= static_cast<uint16_t>((buttons ^ previous) & buttons);
            input.releasePending |= static_cast<uint16_t>((buttons ^ previous) & previous);
            input.sequence = sequence;
            input.buttons = buttons;
            input.stickX = stickX;
            input.stickY = stickY;
            input.valid = true;
            continue;
        }

        if (event.type == PacketType::CharacterLocked || event.type == PacketType::CharacterUnlocked) {
            uint8_t playerId = 0xFF;
            uint8_t fighter = 0xFF;
            uint8_t costume = 0;
            uint8_t shade = 0;
            PayloadReader reader(event.payload.data(), event.payload.size());
            if (!reader.U8(playerId) || !reader.U8(fighter) || !reader.U8(costume) || !reader.U8(shade) ||
                !reader.Empty() || playerId >= kMaxPlayers) {
                continue;
            }
            if (mLobby.IsHost() && event.sourcePlayerId != playerId) continue;
            port_log("[NETPLAY] CSS remote %s slot=P%u fighter=%u costume=%u shade=%u\n",
                     event.type == PacketType::CharacterLocked ? "lock" : "unlock",
                     playerId + 1, fighter, costume, shade);
            continue;
        }

        if (event.type == PacketType::MatchConfiguration) {
            if (mLobby.IsHost()) continue;
            PortNetplayMatchConfig config{};
            if (!DecodeMatchConfigPayload(event.payload, config)) {
                port_log("[NETPLAY] malformed match configuration from host\n");
                continue;
            }
            {
                std::lock_guard<std::mutex> lock(mMutex);
                mMatchConfig = config;
                mMatchConfigValid = true;
                mLoadingReady.fill(false);
                mLoadingReadySent = false;
                mStartMatchReceived = false;
                mMatchStartCountdown = -1;
            }
            ForceTransition(NetplayState::LoadingMatch, "host match configuration received");
            port_log("[NETPLAY] match config client stage=%u seed=%u players=%u\n",
                     config.stage_kind, config.rng_seed, config.player_count);
            continue;
        }

        if (event.type == PacketType::LoadingReady) {
            if (!mLobby.IsHost()) continue;
            uint8_t playerId = 0xFF;
            PayloadReader reader(event.payload.data(), event.payload.size());
            if (!reader.U8(playerId) || !reader.Empty() || playerId >= kMaxPlayers ||
                event.sourcePlayerId != playerId) {
                continue;
            }
            {
                std::lock_guard<std::mutex> lock(mMutex);
                mLoadingReady[playerId] = true;
            }
            port_log("[NETPLAY] loading ready remote=P%u\n", playerId + 1);
            continue;
        }

        if (event.type == PacketType::StartMatch) {
            if (mLobby.IsHost()) continue;
            uint16_t countdown = 0;
            PayloadReader reader(event.payload.data(), event.payload.size());
            if (!reader.U16(countdown) || !reader.Empty() || countdown < 10 || countdown > 180) continue;
            {
                std::lock_guard<std::mutex> lock(mMutex);
                mStartMatchReceived = true;
                mMatchStartCountdown = countdown;
            }
            port_log("[NETPLAY] start match received countdown=%u\n", countdown);
            continue;
        }
    }

    if (mLobby.IsHost()) {
        const LobbyView lobby = mLobby.Snapshot();
        DiscoveryHostInfo info{};
        info.sessionId = lobby.sessionId;
        {
            std::lock_guard<std::mutex> lock(mMutex);
            info.localIp = Mode() == NetplayMode::LocalAdhoc ? mLocalMac : mLocalIp;
        }
        info.hostName = lobby.players[0].name;
        info.buildId = kNetplayBuildId;
        info.playerCount = mLobby.ConnectedPlayerCount();
        info.status = lobby.status;
        mDiscovery.SetHostInfo(info);
    }

    const NetplayState state = State();
    const LobbyView lobby = mLobby.Snapshot();
    if (state == NetplayState::Connecting && lobby.connected) {
        ForceTransition(NetplayState::ClientLobby, "join accepted");
    } else if ((state == NetplayState::Connecting || state == NetplayState::ClientLobby) &&
               !mLobby.IsActive()) {
        {
            std::lock_guard<std::mutex> lock(mMutex);
            mLastError = lobby.lastMessage.empty() ? "DISCONNECTED" : lobby.lastMessage;
        }
        ForceTransition(NetplayState::Disconnected, "lobby connection ended");
    }

    if (mLobby.ConsumeCharacterSelectStart()) {
        {
            std::lock_guard<std::mutex> lock(mMutex);
            mCssInputs = {};
            mMatchConfigValid = false;
            mLoadingReady.fill(false);
            mLoadingReadySent = false;
            mStartMatchReceived = false;
            mMatchStartCountdown = -1;
        }
        mCssLocalSequence.store(1, std::memory_order_release);
        ForceTransition(NetplayState::CharacterSelect, "lobby start synchronized");
    }

    if (State() == NetplayState::LoadingMatch && mLobby.IsHost()) {
        const LobbyView currentLobby = mLobby.Snapshot();
        bool allReady = currentLobby.connected;
        uint8_t expected = 0;
        {
            std::lock_guard<std::mutex> lock(mMutex);
            for (std::size_t i = 0; i < kMaxPlayers; ++i) {
                const LobbySlotState slotState = currentLobby.players[i].state;
                if (slotState != LobbySlotState::Connected && slotState != LobbySlotState::Ready) continue;
                ++expected;
                if (!mLoadingReady[i]) allReady = false;
            }
            if (mStartMatchReceived) allReady = false;
        }
        if (allReady && expected >= 2) {
            constexpr uint16_t kStartCountdownFrames = 60;
            std::vector<uint8_t> payload;
            PayloadWriter writer(payload);
            writer.U16(kStartCountdownFrames);
            if (mLobby.SendSessionMessage(PacketType::StartMatch, payload)) {
                std::lock_guard<std::mutex> lock(mMutex);
                mStartMatchReceived = true;
                mMatchStartCountdown = kStartCountdownFrames;
                port_log("[NETPLAY] loading barrier released players=%u countdown=%u\n",
                         expected, kStartCountdownFrames);
            }
        }
    }
}

void NetworkManager::PublishSnapshots() {
    std::vector<DiscoveredLobby> discovered = mDiscovery.Snapshot();
    LobbyView lobby = mLobby.Snapshot();
    std::lock_guard<std::mutex> lock(mMutex);
    mDiscoveredLobbies = std::move(discovered);
    mLobbyView = std::move(lobby);
}

uint32_t NetworkManager::MakeSessionId() const {
    const uint64_t ticks = static_cast<uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    std::string endpoint;
    {
        std::lock_guard<std::mutex> lock(mMutex);
        endpoint = Mode() == NetplayMode::LocalAdhoc ? mLocalMac : mLocalIp;
    }
    uint32_t hash = 2166136261U ^ static_cast<uint32_t>(ticks) ^ static_cast<uint32_t>(ticks >> 32);
    for (unsigned char c : endpoint) {
        hash ^= c;
        hash *= 16777619U;
    }
    return hash == 0 ? 1U : hash;
}

void NetworkManager::SetError(int code, const char* where) {
    char message[96];
    std::snprintf(message, sizeof(message), "%s 0X%08X", where != nullptr ? where : "NETWORK ERROR",
                  static_cast<unsigned int>(code));
    {
        std::lock_guard<std::mutex> lock(mMutex);
        mLastError = message;
    }
    mState.store(NetplayState::Error, std::memory_order_release);
    port_log("[NETPLAY] error where=%s code=0x%08X\n",
             where != nullptr ? where : "unknown", static_cast<unsigned int>(code));
}

bool NetworkManager::Transition(NetplayState expected, NetplayState next, const char* reason) {
    NetplayState current = expected;
    if (!mState.compare_exchange_strong(current, next, std::memory_order_acq_rel)) {
        port_log("[NETPLAY] transition rejected current=%s expected=%s next=%s reason=%s\n",
                 StateName(current), StateName(expected), StateName(next),
                 reason != nullptr ? reason : "none");
        return false;
    }
    port_log("[NETPLAY] state %s -> %s reason=%s\n", StateName(expected), StateName(next),
             reason != nullptr ? reason : "none");
    return true;
}

void NetworkManager::ForceTransition(NetplayState next, const char* reason) {
    const NetplayState previous = mState.exchange(next, std::memory_order_acq_rel);
    if (previous != next) {
        port_log("[NETPLAY] state %s -> %s reason=%s\n", StateName(previous), StateName(next),
                 reason != nullptr ? reason : "none");
    }
}

std::string NetworkManager::LocalIp() const {
    std::lock_guard<std::mutex> lock(mMutex);
    return mLocalIp;
}

std::string NetworkManager::LocalEndpoint() const {
    std::lock_guard<std::mutex> lock(mMutex);
    return Mode() == NetplayMode::LocalAdhoc ? mLocalMac : mLocalIp;
}

std::string NetworkManager::LastError() const {
    std::lock_guard<std::mutex> lock(mMutex);
    return mLastError;
}

NetplaySettings NetworkManager::Settings() const {
    std::lock_guard<std::mutex> lock(mMutex);
    return mSettings;
}

void NetworkManager::SetPlayerName(const std::string& name) {
    LoadSettings();
    {
        std::lock_guard<std::mutex> lock(mMutex);
        mSettings.playerName = SanitizePlayerName(name);
    }
    SaveSettings();
}

void NetworkManager::SetInputDelay(int frames) {
    LoadSettings();
    if (frames < kAutoInputDelay || frames > kMaxInputDelay) {
        frames = kAutoInputDelay;
    }
    {
        std::lock_guard<std::mutex> lock(mMutex);
        mSettings.inputDelay = frames;
    }
    SaveSettings();
}

void NetworkManager::SetShowStats(bool enabled) {
    LoadSettings();
    {
        std::lock_guard<std::mutex> lock(mMutex);
        mSettings.showStats = enabled;
    }
    SaveSettings();
}

void NetworkManager::ResetSettings() {
    LoadSettings();
    {
        std::lock_guard<std::mutex> lock(mMutex);
        mSettings = NetplaySettings{};
    }
    SaveSettings();
    port_log("[NETPLAY] settings reset\n");
}

void NetworkManager::SetMode(NetplayMode mode) {
    if (mode != NetplayMode::LocalAdhoc && mode != NetplayMode::Online && mode != NetplayMode::None) return;
    Command command{};
    command.type = CommandType::SetMode;
    command.mode = mode;
    Enqueue(std::move(command));
}

bool NetworkManager::ModeReady() const {
    const NetplayMode mode = Mode();
    if (mode == NetplayMode::LocalAdhoc) {
        return mNetworkInitialized.load(std::memory_order_acquire) &&
               mAdhocInitialized.load(std::memory_order_acquire) &&
               platform::IsAdhocConnectionReady();
    }
    if (mode == NetplayMode::Online) {
        return mNetworkInitialized.load(std::memory_order_acquire) &&
               mNetworkConnected.load(std::memory_order_acquire);
    }
    return false;
}

void NetworkManager::StartDiscovery() {
    Enqueue(Command{CommandType::StartDiscovery});
}

void NetworkManager::RefreshDiscovery() {
    Enqueue(Command{CommandType::RefreshDiscovery});
}

void NetworkManager::HostLobby() {
    Enqueue(Command{CommandType::HostLobby});
}

bool NetworkManager::JoinDiscoveredLobby(std::size_t index) {
    DiscoveredLobby lobby;
    {
        std::lock_guard<std::mutex> lock(mMutex);
        if (index >= mDiscoveredLobbies.size()) return false;
        lobby = mDiscoveredLobbies[index];
    }
    if (!lobby.compatible || lobby.status != LobbyStatus::Open ||
        lobby.playerCount >= lobby.maxPlayers || lobby.hostIp.empty() || lobby.sessionId == 0) {
        return false;
    }

    Command command{};
    command.type = CommandType::JoinLobby;
    command.hostIp = lobby.hostIp;
    command.sessionId = lobby.sessionId;
    Enqueue(std::move(command));
    return true;
}

void NetworkManager::CancelNetworkActivity() {
    Enqueue(Command{CommandType::CancelActivity});
}

void NetworkManager::ToggleLocalReady() {
    LobbyView lobby = Lobby();
    if (!lobby.connected || lobby.localPlayerId >= kMaxPlayers) return;
    const bool isReady = lobby.players[lobby.localPlayerId].state == LobbySlotState::Ready;
    Command command{};
    command.type = CommandType::SetReady;
    command.ready = !isReady;
    Enqueue(std::move(command));
}

void NetworkManager::RequestStartCharacterSelect() {
    Enqueue(Command{CommandType::StartCharacterSelect});
}

void NetworkManager::SubmitCssInput(uint16_t buttons, int8_t stickX, int8_t stickY) {
    if (State() != NetplayState::CharacterSelect) return;
    Command command{};
    command.type = CommandType::CssInput;
    command.sequence = mCssLocalSequence.fetch_add(1, std::memory_order_acq_rel);
    command.buttons = buttons;
    command.stickX = stickX;
    command.stickY = stickY;
    Enqueue(std::move(command));
}

bool NetworkManager::ConsumeCssInput(uint8_t playerId, uint16_t& buttons, uint16_t& buttonTap,
                                     uint16_t& buttonRelease, int8_t& stickX, int8_t& stickY) {
    if (playerId >= kMaxPlayers) return false;
    std::lock_guard<std::mutex> lock(mMutex);
    CssInputState& input = mCssInputs[playerId];
    if (!input.valid) return false;
    buttons = input.buttons;
    buttonTap = input.tapPending;
    buttonRelease = input.releasePending;
    stickX = input.stickX;
    stickY = input.stickY;
    input.tapPending = 0;
    input.releasePending = 0;
    return true;
}

void NetworkManager::NotifyCssLock(uint8_t fighterKind, uint8_t costume, uint8_t shade) {
    if (State() != NetplayState::CharacterSelect) return;
    Command command{};
    command.type = CommandType::CssLock;
    command.fighterKind = fighterKind;
    command.costume = costume;
    command.shade = shade;
    Enqueue(std::move(command));
}

void NetworkManager::NotifyCssUnlock() {
    if (State() != NetplayState::CharacterSelect) return;
    Command command{};
    command.type = CommandType::CssUnlock;
    Enqueue(std::move(command));
}

void NetworkManager::CommitMatchConfig(const PortNetplayMatchConfig& config) {
    if (State() != NetplayState::CharacterSelect) return;
    Command command{};
    command.type = CommandType::CommitMatchConfig;
    command.matchConfig = config;
    Enqueue(std::move(command));
}

bool NetworkManager::GetMatchConfig(PortNetplayMatchConfig& config) const {
    std::lock_guard<std::mutex> lock(mMutex);
    if (!mMatchConfigValid) return false;
    config = mMatchConfig;
    return true;
}

void NetworkManager::NotifyLoadingReady() {
    if (State() != NetplayState::LoadingMatch) return;
    {
        std::lock_guard<std::mutex> lock(mMutex);
        if (mLoadingReadySent) return;
    }
    Command command{};
    command.type = CommandType::LoadingReady;
    Enqueue(std::move(command));
}

bool NetworkManager::MatchGateTick() {
    const NetplayState state = State();
    if (state != NetplayState::LoadingMatch && state != NetplayState::InMatch) return true;

    bool release = false;
    {
        std::lock_guard<std::mutex> lock(mMutex);
        if (!mStartMatchReceived || mMatchStartCountdown < 0) return false;
        if (mMatchStartCountdown > 0) {
            --mMatchStartCountdown;
            return false;
        }
        release = true;
    }
    if (release && state == NetplayState::LoadingMatch) {
        ForceTransition(NetplayState::InMatch, "synchronized match countdown complete");
        port_log("[NETPLAY] synchronized match execution released\n");
    }
    return true;
}

std::vector<DiscoveredLobby> NetworkManager::DiscoveredLobbies() const {
    std::lock_guard<std::mutex> lock(mMutex);
    return mDiscoveredLobbies;
}

LobbyView NetworkManager::Lobby() const {
    std::lock_guard<std::mutex> lock(mMutex);
    return mLobbyView;
}

} // namespace ssb64::netplay

extern "C" {

void port_netplay_enter_menu(void) {
    ssb64::netplay::NetworkManager::Instance().EnterMultiplayerMenu();
}

void port_netplay_leave_menu(void) {
    ssb64::netplay::NetworkManager::Instance().LeaveMultiplayerMenu();
#ifdef __vita__
    ssb64::netplay::SetOnlineVanillaRuntime(false);
#endif
}

int port_netplay_get_state(void) {
    return static_cast<int>(ssb64::netplay::NetworkManager::Instance().State());
}

int port_netplay_network_initialized(void) {
    return ssb64::netplay::NetworkManager::Instance().NetworkInitialized() ? 1 : 0;
}

int port_netplay_network_connected(void) {
    return ssb64::netplay::NetworkManager::Instance().NetworkConnected() ? 1 : 0;
}

void port_netplay_get_local_ip(char* out, int out_size) {
    ssb64::netplay::CopyStringOut(ssb64::netplay::NetworkManager::Instance().LocalEndpoint(), out, out_size);
}

void port_netplay_get_last_error(char* out, int out_size) {
    ssb64::netplay::CopyStringOut(ssb64::netplay::NetworkManager::Instance().LastError(), out, out_size);
}

void port_netplay_get_player_name(char* out, int out_size) {
    ssb64::netplay::CopyStringOut(ssb64::netplay::NetworkManager::Instance().Settings().playerName, out, out_size);
}

void port_netplay_set_player_name(const char* name) {
    ssb64::netplay::NetworkManager::Instance().SetPlayerName(name != nullptr ? name : "PLAYER");
}

int port_netplay_get_input_delay(void) {
    return ssb64::netplay::NetworkManager::Instance().Settings().inputDelay;
}

void port_netplay_set_input_delay(int frames) {
    ssb64::netplay::NetworkManager::Instance().SetInputDelay(frames);
}

int port_netplay_get_show_stats(void) {
    return ssb64::netplay::NetworkManager::Instance().Settings().showStats ? 1 : 0;
}

void port_netplay_set_show_stats(int enabled) {
    ssb64::netplay::NetworkManager::Instance().SetShowStats(enabled != 0);
}

void port_netplay_reset_settings(void) {
    ssb64::netplay::NetworkManager::Instance().ResetSettings();
}

void port_netplay_set_mode(int mode) {
    using ssb64::netplay::NetplayMode;
    if (mode == static_cast<int>(NetplayMode::LocalAdhoc)) {
#ifdef __vita__
        ssb64::netplay::SetOnlineVanillaRuntime(false);
#endif
        ssb64::netplay::NetworkManager::Instance().SetMode(NetplayMode::LocalAdhoc);
    } else if (mode == static_cast<int>(NetplayMode::Online)) {
#ifdef __vita__
        ssb64::netplay::SetOnlineVanillaRuntime(true);
#endif
        ssb64::netplay::NetworkManager::Instance().SetMode(NetplayMode::Online);
    } else {
#ifdef __vita__
        ssb64::netplay::SetOnlineVanillaRuntime(false);
#endif
        ssb64::netplay::NetworkManager::Instance().SetMode(NetplayMode::None);
    }
}

int port_netplay_get_mode(void) {
    return static_cast<int>(ssb64::netplay::NetworkManager::Instance().Mode());
}

int port_netplay_mode_ready(void) {
    return ssb64::netplay::NetworkManager::Instance().ModeReady() ? 1 : 0;
}

void port_netplay_adhoc_dialog_tick(void) {
    using ssb64::netplay::NetplayMode;
    using ssb64::netplay::platform::AdhocConnectionState;

    auto& manager = ssb64::netplay::NetworkManager::Instance();
    if (manager.Mode() != NetplayMode::LocalAdhoc || !manager.AdhocInitialized()) return;

    AdhocConnectionState state = ssb64::netplay::platform::GetAdhocConnectionState();
    if (state == AdhocConnectionState::AwaitingDialog) {
        ssb64::netplay::platform::BeginAdhocConnectionDialog();
    } else if (state == AdhocConnectionState::Running) {
        ssb64::netplay::platform::UpdateAdhocConnectionDialog();
    }
}

int port_netplay_adhoc_dialog_state(void) {
    return static_cast<int>(ssb64::netplay::platform::GetAdhocConnectionState());
}

int port_netplay_common_dialog_active(void) {
    return ssb64::netplay::platform::IsCommonDialogActive() ? 1 : 0;
}

void port_netplay_start_discovery(void) {
    ssb64::netplay::NetworkManager::Instance().StartDiscovery();
}

void port_netplay_refresh_discovery(void) {
    ssb64::netplay::NetworkManager::Instance().RefreshDiscovery();
}

void port_netplay_host_lobby(void) {
    ssb64::netplay::NetworkManager::Instance().HostLobby();
}

int port_netplay_join_discovered_lobby(int index) {
    if (index < 0) return 0;
    return ssb64::netplay::NetworkManager::Instance().JoinDiscoveredLobby(
               static_cast<std::size_t>(index)) ? 1 : 0;
}

void port_netplay_cancel_activity(void) {
    ssb64::netplay::NetworkManager::Instance().CancelNetworkActivity();
}

int port_netplay_get_discovery_count(void) {
    return static_cast<int>(ssb64::netplay::NetworkManager::Instance().DiscoveredLobbies().size());
}

int port_netplay_get_discovery_lobby(int index, char* host_name, int host_name_size,
                                     char* host_ip, int host_ip_size, char* build_id, int build_id_size,
                                     int* players, int* max_players, int* ping_ms, int* protocol_version,
                                     int* status, int* compatible) {
    if (index < 0) return 0;
    const auto lobbies = ssb64::netplay::NetworkManager::Instance().DiscoveredLobbies();
    if (static_cast<std::size_t>(index) >= lobbies.size()) return 0;
    const auto& lobby = lobbies[static_cast<std::size_t>(index)];
    ssb64::netplay::CopyStringOut(lobby.hostName, host_name, host_name_size);
    ssb64::netplay::CopyStringOut(lobby.hostIp, host_ip, host_ip_size);
    ssb64::netplay::CopyStringOut(lobby.buildId, build_id, build_id_size);
    if (players) *players = lobby.playerCount;
    if (max_players) *max_players = lobby.maxPlayers;
    if (ping_ms) *ping_ms = static_cast<int>(lobby.pingMs);
    if (protocol_version) *protocol_version = static_cast<int>(lobby.protocolVersion);
    if (status) *status = static_cast<int>(lobby.status);
    if (compatible) *compatible = lobby.compatible ? 1 : 0;
    return 1;
}

int port_netplay_lobby_is_host(void) {
    return ssb64::netplay::NetworkManager::Instance().Lobby().isHost ? 1 : 0;
}

int port_netplay_lobby_is_connected(void) {
    return ssb64::netplay::NetworkManager::Instance().Lobby().connected ? 1 : 0;
}

int port_netplay_lobby_get_status(void) {
    return static_cast<int>(ssb64::netplay::NetworkManager::Instance().Lobby().status);
}

int port_netplay_lobby_get_local_player(void) {
    return static_cast<int>(ssb64::netplay::NetworkManager::Instance().Lobby().localPlayerId);
}

int port_netplay_lobby_get_player_count(void) {
    const auto lobby = ssb64::netplay::NetworkManager::Instance().Lobby();
    int count = 0;
    for (const auto& player : lobby.players) {
        if (player.state == ssb64::netplay::LobbySlotState::Connected ||
            player.state == ssb64::netplay::LobbySlotState::Ready) {
            ++count;
        }
    }
    return count;
}

int port_netplay_lobby_get_slot(int slot, char* player_name, int player_name_size,
                                int* slot_state, int* ping_ms, int* jitter_ms) {
    if (slot < 0 || slot >= static_cast<int>(ssb64::netplay::kMaxPlayers)) return 0;
    const auto lobby = ssb64::netplay::NetworkManager::Instance().Lobby();
    const auto& player = lobby.players[static_cast<std::size_t>(slot)];
    ssb64::netplay::CopyStringOut(player.name, player_name, player_name_size);
    if (slot_state) *slot_state = static_cast<int>(player.state);
    if (ping_ms) *ping_ms = static_cast<int>(player.pingMs);
    if (jitter_ms) *jitter_ms = static_cast<int>(player.jitterMs);
    return 1;
}

int port_netplay_lobby_local_ready(void) {
    const auto lobby = ssb64::netplay::NetworkManager::Instance().Lobby();
    if (lobby.localPlayerId >= ssb64::netplay::kMaxPlayers) return 0;
    return lobby.players[lobby.localPlayerId].state == ssb64::netplay::LobbySlotState::Ready ? 1 : 0;
}

int port_netplay_lobby_can_start(void) {
    const auto lobby = ssb64::netplay::NetworkManager::Instance().Lobby();
    if (!lobby.isHost || !lobby.connected) return 0;
    int players = 0;
    for (const auto& player : lobby.players) {
        if (player.state == ssb64::netplay::LobbySlotState::Connected) return 0;
        if (player.state == ssb64::netplay::LobbySlotState::Ready) ++players;
    }
    return players >= 2 ? 1 : 0;
}

void port_netplay_lobby_toggle_ready(void) {
    ssb64::netplay::NetworkManager::Instance().ToggleLocalReady();
}

void port_netplay_lobby_start(void) {
    ssb64::netplay::NetworkManager::Instance().RequestStartCharacterSelect();
}

void port_netplay_get_lobby_message(char* out, int out_size) {
    ssb64::netplay::CopyStringOut(ssb64::netplay::NetworkManager::Instance().Lobby().lastMessage,
                                  out, out_size);
}

int port_netplay_get_protocol_version(void) {
    return static_cast<int>(ssb64::netplay::kProtocolVersion);
}

void port_netplay_get_build_id(char* out, int out_size) {
    ssb64::netplay::CopyStringOut(ssb64::netplay::kNetplayBuildId, out, out_size);
}

int port_netplay_css_active(void) {
    return ssb64::netplay::NetworkManager::Instance().State() == ssb64::netplay::NetplayState::CharacterSelect ? 1 : 0;
}

int port_netplay_css_is_host(void) {
    return ssb64::netplay::NetworkManager::Instance().Lobby().isHost ? 1 : 0;
}

int port_netplay_css_get_local_player(void) {
    const auto lobby = ssb64::netplay::NetworkManager::Instance().Lobby();
    return lobby.localPlayerId < ssb64::netplay::kMaxPlayers ? static_cast<int>(lobby.localPlayerId) : -1;
}

int port_netplay_css_slot_connected(int slot) {
    if (slot < 0 || slot >= static_cast<int>(ssb64::netplay::kMaxPlayers)) return 0;
    const auto lobby = ssb64::netplay::NetworkManager::Instance().Lobby();
    const auto state = lobby.players[static_cast<std::size_t>(slot)].state;
    return state == ssb64::netplay::LobbySlotState::Connected || state == ssb64::netplay::LobbySlotState::Ready ? 1 : 0;
}

void port_netplay_css_submit_input(uint16_t buttons, int8_t stick_x, int8_t stick_y) {
    ssb64::netplay::NetworkManager::Instance().SubmitCssInput(buttons, stick_x, stick_y);
}

int port_netplay_css_consume_input(int slot, uint16_t* buttons, uint16_t* button_tap,
                                  uint16_t* button_release, int8_t* stick_x, int8_t* stick_y) {
    if (slot < 0 || slot >= static_cast<int>(ssb64::netplay::kMaxPlayers)) return 0;
    uint16_t held = 0;
    uint16_t tap = 0;
    uint16_t released = 0;
    int8_t x = 0;
    int8_t y = 0;
    if (!ssb64::netplay::NetworkManager::Instance().ConsumeCssInput(
            static_cast<uint8_t>(slot), held, tap, released, x, y)) {
        return 0;
    }
    if (buttons) *buttons = held;
    if (button_tap) *button_tap = tap;
    if (button_release) *button_release = released;
    if (stick_x) *stick_x = x;
    if (stick_y) *stick_y = y;
    return 1;
}

void port_netplay_css_notify_lock(int fighter_kind, int costume, int shade) {
    if (fighter_kind < 0 || fighter_kind > 255 || costume < 0 || costume > 255 || shade < 0 || shade > 255) return;
    ssb64::netplay::NetworkManager::Instance().NotifyCssLock(
        static_cast<uint8_t>(fighter_kind), static_cast<uint8_t>(costume), static_cast<uint8_t>(shade));
}

void port_netplay_css_notify_unlock(void) {
    ssb64::netplay::NetworkManager::Instance().NotifyCssUnlock();
}

void port_netplay_css_host_commit_match(const PortNetplayMatchConfig* config) {
    if (config == nullptr) return;
    ssb64::netplay::NetworkManager::Instance().CommitMatchConfig(*config);
}

int port_netplay_get_match_config(PortNetplayMatchConfig* out_config) {
    if (out_config == nullptr) return 0;
    return ssb64::netplay::NetworkManager::Instance().GetMatchConfig(*out_config) ? 1 : 0;
}

void port_netplay_loading_ready(void) {
    ssb64::netplay::NetworkManager::Instance().NotifyLoadingReady();
}

int port_netplay_match_gate_tick(void) {
    return ssb64::netplay::NetworkManager::Instance().MatchGateTick() ? 1 : 0;
}

} // extern "C"
