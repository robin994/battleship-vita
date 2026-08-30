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
constexpr const char* kHostStageCVar = "gNetplay.HostStage";
constexpr const char* kHostStocksCVar = "gNetplay.HostStocks";
constexpr const char* kHostTimeCVar = "gNetplay.HostTime";
constexpr const char* kHostItemRateCVar = "gNetplay.HostItemRate";
constexpr const char* kHostItemTogglesCVar = "gNetplay.HostItemToggles";
constexpr const char* kHostTeamBattleCVar = "gNetplay.HostTeamBattle";
constexpr const char* kHostTeamAttackCVar = "gNetplay.HostTeamAttack";
constexpr const char* kHostDamageRatioCVar = "gNetplay.HostDamageRatio";
constexpr const char* kHostHandicapCVar = "gNetplay.HostHandicap";
constexpr const char* kJoinAddressCVar = "gNetplay.JoinAddress";
constexpr const char* kRendezvousServerCVar = "gNetplay.RendezvousServer";
constexpr int kAutoInputDelay = -1;
constexpr int kMaxInputDelay = 4;
constexpr int kHostRuleRandom = -1;
constexpr int kHostStageMax = 8;
constexpr int kHostStocksMin = 1;
constexpr int kHostStocksMax = 5;
constexpr int kHostTimeInfinite = 0;
constexpr int kHostTimeUnitMin = 2;
constexpr int kHostTimeUnitMax = 10;
constexpr int kHostItemRateMax = 5;
constexpr int kHostItemTogglesMask = 0x000FFFFF;
constexpr int kHostItemTogglesDefault = 0x000FFFFF;
constexpr int kHostDamageMin = 50;
constexpr int kHostDamageMax = 200;
constexpr int kHostDamageStep = 10;
constexpr int kHostDamageDefault = 100;
constexpr int kHostHandicapMax = 2;
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

bool EncodeMatchConfigPayload(uint32_t matchId, const PortNetplayMatchConfig& config,
                              std::vector<uint8_t>& payload) {
    payload.clear();
    PayloadWriter writer(payload);
    if (matchId == 0 || config.player_count < 2 || config.player_count > kMaxPlayers ||
        !writer.U32(matchId) || !writer.U32(config.rng_seed) || !writer.U32(config.stage_kind) ||
        !writer.U32(config.stocks) || !writer.U32(config.time_limit) ||
        !writer.U32(config.time_seconds) ||
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

bool DecodeMatchConfigPayload(const std::vector<uint8_t>& payload, uint32_t& matchId,
                              PortNetplayMatchConfig& config) {
    PortNetplayMatchConfig decoded{};
    uint32_t decodedMatchId = 0;
    PayloadReader reader(payload.data(), payload.size());
    if (!reader.U32(decodedMatchId) || decodedMatchId == 0 ||
        !reader.U32(decoded.rng_seed) || !reader.U32(decoded.stage_kind) ||
        !reader.U32(decoded.stocks) || !reader.U32(decoded.time_limit) ||
        !reader.U32(decoded.time_seconds) ||
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
    matchId = decodedMatchId;
    config = decoded;
    return true;
}

bool EncodeStateHashPayload(uint32_t frame, uint64_t hash, std::vector<uint8_t>& payload) {
    payload.clear();
    PayloadWriter writer(payload);
    return writer.U32(frame) && writer.U32(static_cast<uint32_t>(hash >> 32)) &&
           writer.U32(static_cast<uint32_t>(hash));
}

bool DecodeStateHashPayload(const std::vector<uint8_t>& payload, uint32_t& frame, uint64_t& hash) {
    uint32_t high = 0;
    uint32_t low = 0;
    PayloadReader reader(payload.data(), payload.size());
    if (!reader.U32(frame) || !reader.U32(high) || !reader.U32(low) || !reader.Empty()) return false;
    hash = (static_cast<uint64_t>(high) << 32) | low;
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

    mSettings.hostStage = CVarGetInteger(kHostStageCVar, kHostRuleRandom);
    if (mSettings.hostStage != kHostRuleRandom &&
        (mSettings.hostStage < 0 || mSettings.hostStage > kHostStageMax)) {
        mSettings.hostStage = kHostRuleRandom;
    }
    mSettings.hostStocks = CVarGetInteger(kHostStocksCVar, kHostRuleRandom);
    if (mSettings.hostStocks != kHostRuleRandom &&
        (mSettings.hostStocks < kHostStocksMin || mSettings.hostStocks > kHostStocksMax)) {
        mSettings.hostStocks = kHostRuleRandom;
    }
    mSettings.hostTimeUnits = CVarGetInteger(kHostTimeCVar, kHostRuleRandom);
    if (mSettings.hostTimeUnits != kHostRuleRandom && mSettings.hostTimeUnits != kHostTimeInfinite &&
        (mSettings.hostTimeUnits < kHostTimeUnitMin || mSettings.hostTimeUnits > kHostTimeUnitMax)) {
        mSettings.hostTimeUnits = kHostRuleRandom;
    }

    mSettings.hostItemRate = CVarGetInteger(kHostItemRateCVar, 0);
    if (mSettings.hostItemRate != kHostRuleRandom &&
        (mSettings.hostItemRate < 0 || mSettings.hostItemRate > kHostItemRateMax)) {
        mSettings.hostItemRate = 0;
    }
    mSettings.hostItemToggles = CVarGetInteger(kHostItemTogglesCVar, kHostItemTogglesDefault) & kHostItemTogglesMask;
    mSettings.hostTeamBattle = CVarGetInteger(kHostTeamBattleCVar, 0) != 0 ? 1 : 0;
    mSettings.hostTeamAttack = CVarGetInteger(kHostTeamAttackCVar, 0) != 0 ? 1 : 0;
    mSettings.hostDamageRatio = CVarGetInteger(kHostDamageRatioCVar, kHostDamageDefault);
    if (mSettings.hostDamageRatio != kHostRuleRandom &&
        (mSettings.hostDamageRatio < kHostDamageMin || mSettings.hostDamageRatio > kHostDamageMax ||
         (mSettings.hostDamageRatio % kHostDamageStep) != 0)) {
        mSettings.hostDamageRatio = kHostDamageDefault;
    }
    mSettings.hostHandicap = CVarGetInteger(kHostHandicapCVar, 0);
    if (mSettings.hostHandicap < 0 || mSettings.hostHandicap > kHostHandicapMax) {
        mSettings.hostHandicap = 0;
    }

    const char* storedJoin = CVarGetString(kJoinAddressCVar, "");
    mSettings.joinAddress = storedJoin != nullptr ? storedJoin : "";

    const char* storedRendezvous = CVarGetString(kRendezvousServerCVar, "");
    mSettings.rendezvousServer = storedRendezvous != nullptr ? storedRendezvous : "";

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
    CVarSetInteger(kHostStageCVar, settings.hostStage);
    CVarSetInteger(kHostStocksCVar, settings.hostStocks);
    CVarSetInteger(kHostTimeCVar, settings.hostTimeUnits);
    CVarSetInteger(kHostItemRateCVar, settings.hostItemRate);
    CVarSetInteger(kHostItemTogglesCVar, settings.hostItemToggles);
    CVarSetInteger(kHostTeamBattleCVar, settings.hostTeamBattle);
    CVarSetInteger(kHostTeamAttackCVar, settings.hostTeamAttack);
    CVarSetInteger(kHostDamageRatioCVar, settings.hostDamageRatio);
    CVarSetInteger(kHostHandicapCVar, settings.hostHandicap);
    CVarSetString(kJoinAddressCVar, settings.joinAddress.c_str());
    CVarSetString(kRendezvousServerCVar, settings.rendezvousServer.c_str());
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
        PushHostRulesToLobby();

        if (Mode() != NetplayMode::LocalAdhoc && mAdhocInitialized.load(std::memory_order_acquire)) {
            platform::ShutdownAdhoc();
            mAdhocInitialized.store(false, std::memory_order_release);
            {
                std::lock_guard<std::mutex> lock(mMutex);
                mLocalMac.clear();
            }
        }

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
                mGameplay.Stop();
                mLobby.Stop(RejectReason::None, false);
                mDiscovery.Stop();
                mRendezvous.Stop();
                ForceTransition(NetplayState::Disconnected, "wifi disconnected");
            }
            nextPlatformPoll = now + kPlatformPollInterval;
        }
        PublishSnapshots();
    }

    mGameplay.Stop();
    mLobby.Stop(RejectReason::HostClosing, true);
    mDiscovery.Stop();
    mRendezvous.Stop();
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

void NetworkManager::ResetRoundState(bool clearMatchConfig, bool clearCssState) {
    std::lock_guard<std::mutex> lock(mMutex);
    if (clearCssState) {
        mCssInputs = {};
        mCssLocalSequence.store(1, std::memory_order_release);
    }
    if (clearMatchConfig) {
        mMatchConfig = {};
        mMatchConfigValid = false;
    }
    mLoadingReady.fill(false);
    mLoadingReadySent = false;
    mStartMatchReceived = false;
    mMatchStartCountdown = -1;
    mLocalStateHashes = {};
    mRemoteStateHashes = {};
    mDeterminismMismatchCount = 0;
    mFirstDeterminismMismatchFrame = UINT32_MAX;
    mMatchParticipantMask = 0;
    mLocalMatchResult = {};
    mAuthoritativeMatchResult = {};
    mLocalMatchResultValid = false;
    mAuthoritativeMatchResultValid = false;
    mResultMismatchCount = 0;
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
            command.type != CommandType::ResultsLeave &&
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
                mGameplay.Stop();
                mLobby.Stop(RejectReason::None, true);
                mDiscovery.Stop();
                mRendezvous.Stop();
                ResetRoundState(true, true);
                {
                    std::lock_guard<std::mutex> lock(mMutex);
                    mMatchId = 0;
                    mResultMismatchCount = 0;
                }
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
                    platform::PrepareAdhocConnectionDialog();
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

            case CommandType::StartDiscovery: {
                mGameplay.Stop();
                mLobby.Stop(RejectReason::None, true);
                mDiscovery.Stop();
                mRendezvous.Stop();
                ResetRoundState(true, true);
                if (!mDiscovery.StartClient(Mode(), kNetplayBuildId)) {
                    SetError(transport::LastError(), "discovery init");
                    break;
                }
                std::string rendezvous;
                {
                    std::lock_guard<std::mutex> lock(mMutex);
                    rendezvous = mSettings.rendezvousServer;
                }
                if (Mode() == NetplayMode::Online && !rendezvous.empty()) {
                    mRendezvous.SetServer(rendezvous);
                    mRendezvous.StartList(kNetplayBuildId);
                }
                ForceTransition(NetplayState::Discovering, "LAN discovery started");
                break;
            }

            case CommandType::RefreshDiscovery:
                if (!mDiscovery.IsClientActive()) {
                    if (!mDiscovery.StartClient(Mode(), kNetplayBuildId)) {
                        SetError(transport::LastError(), "discovery refresh");
                        break;
                    }
                    ForceTransition(NetplayState::Discovering, "LAN discovery refreshed");
                }
                mDiscovery.RequestImmediateScan();
                mRendezvous.RequestList();
                break;

            case CommandType::HostLobby: {
                mGameplay.Stop();
                mLobby.Stop(RejectReason::None, true);
                mDiscovery.Stop();
                mRendezvous.Stop();
                ResetRoundState(true, true);
                NetplaySettings settings;
                std::string localEndpoint;
                {
                    std::lock_guard<std::mutex> lock(mMutex);
                    settings = mSettings;
                    localEndpoint = Mode() == NetplayMode::LocalAdhoc ? mLocalMac : mLocalIp;
                }
                const uint32_t sessionId = MakeSessionId();
                mGameplayHostEndpoint = localEndpoint;
                {
                    std::lock_guard<std::mutex> lock(mMutex);
                    mMatchId = 0;
                    mResultMismatchCount = 0;
                }
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
                if (Mode() == NetplayMode::Online && !settings.rendezvousServer.empty()) {
                    mRendezvous.SetServer(settings.rendezvousServer);
                    mRendezvous.StartHost(settings.playerName, kNetplayBuildId,
                                          static_cast<uint8_t>(kMaxPlayers), kLobbyPort, kGameplayPort,
                                          sessionId);
                }
                ForceTransition(NetplayState::HostingLobby, "local lobby created");
                break;
            }

            case CommandType::JoinLobby: {
                mGameplay.Stop();
                mDiscovery.Stop();
                mRendezvous.Stop();
                mLobby.Stop(RejectReason::None, true);
                ResetRoundState(true, true);
                NetplaySettings settings;
                {
                    std::lock_guard<std::mutex> lock(mMutex);
                    settings = mSettings;
                }
                mGameplayHostEndpoint = command.hostIp;
                {
                    std::lock_guard<std::mutex> lock(mMutex);
                    mMatchId = 0;
                    mResultMismatchCount = 0;
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
                mGameplay.Stop();
                mLobby.Stop(RejectReason::None, true);
                mDiscovery.Stop();
                mRendezvous.Stop();
                ResetRoundState(true, true);
                {
                    std::lock_guard<std::mutex> lock(mMutex);
                    mMatchId = 0;
                }
                ForceTransition(NetplayState::Offline, "network activity cancelled");
                break;

            case CommandType::GameplayAbort: {
                const NetplayState abortState = State();
                if (abortState != NetplayState::InMatch && abortState != NetplayState::LoadingMatch &&
                    abortState != NetplayState::CharacterSelect) {
                    break;
                }
                {
                    std::lock_guard<std::mutex> lock(mMutex);
                    mLastError = "DESYNC — RETURNED TO LOBBY";
                }
                port_log("[NETPLAY] desync recovery: returning to lobby mismatch=%u current=%u reason=%u\n",
                         command.frame, command.sequence, static_cast<unsigned>(command.fighterKind));
                ApplyReturnToLobby(true, false);
                break;
            }

            case CommandType::MatchFinished: {
                const NetplayState finishState = State();
                if ((finishState != NetplayState::InMatch && finishState != NetplayState::Results) ||
                    (finishState == NetplayState::Results && mLobby.IsHost())) {
                    break;
                }
                mGameplay.Stop();
                MatchResultPayload result = command.matchResult;
                bool resultMismatch = false;
                bool alreadyAuthoritative = false;
                {
                    std::lock_guard<std::mutex> lock(mMutex);
                    result.matchId = mMatchId;
                    mLocalMatchResult = result;
                    mLocalMatchResultValid = true;
                    alreadyAuthoritative = mAuthoritativeMatchResultValid;
                    if (alreadyAuthoritative &&
                        !MatchResultsEquivalent(result, mAuthoritativeMatchResult)) {
                        ++mResultMismatchCount;
                        resultMismatch = true;
                    }
                }
                if (resultMismatch) {
                    port_log("[NETPLAY][DET] FINAL RESULT MISMATCH match=%u local_frame=%u\n",
                             result.matchId, result.finalFrame);
                }
                if (mLobby.IsHost()) {
                    std::vector<uint8_t> payload;
                    if (!EncodeMatchResultPayload(result, payload) ||
                        !mLobby.SendSessionMessage(PacketType::MatchResult, payload)) {
                        std::lock_guard<std::mutex> lock(mMutex);
                        mLastError = "MATCH RESULT SEND FAILED";
                        ForceTransition(NetplayState::Error, "match result encode/send failed");
                        break;
                    }
                    {
                        std::lock_guard<std::mutex> lock(mMutex);
                        mAuthoritativeMatchResult = result;
                        mAuthoritativeMatchResultValid = true;
                    }
                    ForceTransition(NetplayState::Results, "host result broadcast");
                    port_log("[NETPLAY] result match=%u frame=%u winner=P%u reason=%u hash=%08X%08X\n",
                             result.matchId, result.finalFrame,
                             result.winner < kMaxPlayers ? result.winner + 1U : 0U,
                             static_cast<unsigned>(result.reason),
                             static_cast<uint32_t>(result.finalHash >> 32),
                             static_cast<uint32_t>(result.finalHash));
                } else if (!alreadyAuthoritative) {
                    port_log("[NETPLAY] local result ready match=%u frame=%u; awaiting host result\n",
                             result.matchId, result.finalFrame);
                }
                break;
            }

            case CommandType::ResultsRematch: {
                if (State() != NetplayState::Results || !mLobby.IsHost()) break;
                uint32_t nextMatchId;
                PortNetplayMatchConfig config;
                {
                    std::lock_guard<std::mutex> lock(mMutex);
                    if (!mMatchConfigValid) break;
                    nextMatchId = mMatchId + 1U;
                    if (nextMatchId == 0) nextMatchId = 1;
                    config = mMatchConfig;
                }
                config.rng_seed = NextRematchSeed(nextMatchId);
                const RematchPayload rematch{nextMatchId, config.rng_seed};
                std::vector<uint8_t> payload;
                if (!EncodeRematchPayload(rematch, payload) ||
                    !mLobby.SendSessionMessage(PacketType::Rematch, payload)) {
                    break;
                }
                ResetRoundState(false, false);
                {
                    std::lock_guard<std::mutex> lock(mMutex);
                    mMatchId = nextMatchId;
                    mMatchConfig = config;
                    mMatchConfigValid = true;
                }
                ForceTransition(NetplayState::LoadingMatch, "host requested rematch");
                port_log("[NETPLAY] rematch match=%u seed=%u\n", nextMatchId, config.rng_seed);
                break;
            }

            case CommandType::ResultsCharacterSelect: {
                if (State() != NetplayState::Results || !mLobby.IsHost()) break;
                uint32_t matchId;
                {
                    std::lock_guard<std::mutex> lock(mMutex);
                    matchId = mMatchId;
                }
                std::vector<uint8_t> payload;
                PayloadWriter writer(payload);
                if (!writer.U32(matchId) ||
                    !mLobby.SendSessionMessage(PacketType::ReturnToCharacterSelect, payload)) {
                    break;
                }
                ResetRoundState(true, true);
                ForceTransition(NetplayState::CharacterSelect, "host returned to character select");
                port_log("[NETPLAY] results return CSS match=%u\n", matchId);
                break;
            }

            case CommandType::ReturnToLobby: {
                const NetplayState st = State();
                if (st != NetplayState::CharacterSelect && st != NetplayState::LoadingMatch &&
                    st != NetplayState::InMatch && st != NetplayState::Results) {
                    break;
                }
                ApplyReturnToLobby(true, command.ready);
                break;
            }

            case CommandType::ResultsLeave: {
                if (State() == NetplayState::Offline || State() == NetplayState::Discovering ||
                    State() == NetplayState::Connecting) {
                    break;
                }
                mGameplay.Stop();
                mLobby.SendSessionMessage(PacketType::LeaveSession, {});
                mLobby.Poll();
                mLobby.Stop(RejectReason::None, false);
                mDiscovery.Stop();
                mRendezvous.Stop();
                ResetRoundState(true, true);
                {
                    std::lock_guard<std::mutex> lock(mMutex);
                    mMatchId = 0;
                    mResultMismatchCount = 0;
                }
                ForceTransition(NetplayState::Offline, "left results session");
                break;
            }

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
                uint32_t nextMatchId;
                {
                    std::lock_guard<std::mutex> lock(mMutex);
                    nextMatchId = mMatchId + 1U;
                    if (nextMatchId == 0) nextMatchId = 1;
                }
                std::vector<uint8_t> payload;
                if (!EncodeMatchConfigPayload(nextMatchId, command.matchConfig, payload)) {
                    port_log("[NETPLAY] rejected invalid host match configuration\n");
                    break;
                }
                ResetRoundState(false, false);
                {
                    std::lock_guard<std::mutex> lock(mMutex);
                    mMatchId = nextMatchId;
                    mMatchConfig = command.matchConfig;
                    mMatchConfigValid = true;
                }
                mLobby.SendSessionMessage(PacketType::MatchConfiguration, payload);
                ForceTransition(NetplayState::LoadingMatch, "host match configuration committed");
                port_log("[NETPLAY] match config host match=%u stage=%u seed=%u players=%u\n",
                         nextMatchId, command.matchConfig.stage_kind, command.matchConfig.rng_seed,
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

            case CommandType::StateHash: {
                if (State() != NetplayState::InMatch) break;
                const LobbyView lobby = mLobby.Snapshot();
                if (!lobby.connected || lobby.localPlayerId >= kMaxPlayers) break;

                {
                    std::lock_guard<std::mutex> lock(mMutex);
                    const std::size_t index = command.frame % kStateHashHistory;
                    mLocalStateHashes[index] = StateHashSample{command.frame, command.stateHash, true};
                    for (std::size_t player = 0; player < kMaxPlayers; ++player) {
                        if (player == lobby.localPlayerId) continue;
                        StateHashSample& remote = mRemoteStateHashes[player][index];
                        if (!remote.valid || remote.frame != command.frame) continue;
                        if (remote.hash != command.stateHash) {
                            ++mDeterminismMismatchCount;
                            if (mFirstDeterminismMismatchFrame == UINT32_MAX) {
                                mFirstDeterminismMismatchFrame = command.frame;
                                port_log("[NETPLAY][DET] FIRST STATE HASH MISMATCH frame=%u remote=P%u local=%08X%08X remote=%08X%08X\n",
                                         command.frame, static_cast<unsigned>(player + 1),
                                         static_cast<uint32_t>(command.stateHash >> 32), static_cast<uint32_t>(command.stateHash),
                                         static_cast<uint32_t>(remote.hash >> 32), static_cast<uint32_t>(remote.hash));
                            }
                        }
                        remote.valid = false;
                    }
                }

                std::vector<uint8_t> payload;
                if (EncodeStateHashPayload(command.frame, command.stateHash, payload)) {
                    mLobby.SendSessionMessage(PacketType::StateHash, payload);
                }
                break;
            }
        }
    }
}

void NetworkManager::PollNetworkServices() {
    mGameplay.Poll();
    mDiscovery.Poll();
    mLobby.Poll();
    mRendezvous.Poll();

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
            uint32_t matchId = 0;
            if (!DecodeMatchConfigPayload(event.payload, matchId, config)) {
                port_log("[NETPLAY] malformed match configuration from host\n");
                continue;
            }
            {
                std::lock_guard<std::mutex> lock(mMutex);
                if (matchId <= mMatchId) continue;
            }
            ResetRoundState(false, false);
            {
                std::lock_guard<std::mutex> lock(mMutex);
                mMatchId = matchId;
                mMatchConfig = config;
                mMatchConfigValid = true;
            }
            ForceTransition(NetplayState::LoadingMatch, "host match configuration received");
            port_log("[NETPLAY] match config client match=%u stage=%u seed=%u players=%u\n",
                     matchId, config.stage_kind, config.rng_seed, config.player_count);
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
            uint8_t hostInputDelay = 0;
            PayloadReader reader(event.payload.data(), event.payload.size());
            if (!reader.U16(countdown) || !reader.U8(hostInputDelay) || !reader.Empty() ||
                countdown < 10 || countdown > 180 || hostInputDelay > kMaxInputDelay) {
                continue;
            }
            if (!mGameplay.IsActive() && !StartGameplayTransport(hostInputDelay)) {
                SetError(transport::LastError(), "gameplay transport");
                continue;
            }
            {
                std::lock_guard<std::mutex> lock(mMutex);
                mStartMatchReceived = true;
                mMatchStartCountdown = countdown;
            }
            port_log("[NETPLAY] start match received countdown=%u host_delay=%u\n", countdown, hostInputDelay);
            continue;
        }

        if (event.type == PacketType::MatchResult) {
            if (mLobby.IsHost() ||
                (State() != NetplayState::InMatch && State() != NetplayState::Results)) {
                continue;
            }
            MatchResultPayload result{};
            if (!DecodeMatchResultPayload(event.payload, result)) continue;
            bool mismatch = false;
            {
                std::lock_guard<std::mutex> lock(mMutex);
                if (result.matchId != mMatchId) continue;
                if (mLocalMatchResultValid &&
                    !MatchResultsEquivalent(mLocalMatchResult, result)) {
                    ++mResultMismatchCount;
                    mismatch = true;
                }
                mAuthoritativeMatchResult = result;
                mAuthoritativeMatchResultValid = true;
            }
            if (mismatch) {
                port_log("[NETPLAY][DET] FINAL RESULT MISMATCH match=%u frame=%u winner=P%u\n",
                         result.matchId, result.finalFrame,
                         result.winner < kMaxPlayers ? result.winner + 1U : 0U);
            }
            mGameplay.Stop();
            ForceTransition(NetplayState::Results, "authoritative match result received");
            continue;
        }

        if (event.type == PacketType::Rematch) {
            if (mLobby.IsHost() || State() != NetplayState::Results) continue;
            RematchPayload rematch{};
            if (!DecodeRematchPayload(event.payload, rematch)) continue;
            PortNetplayMatchConfig config{};
            {
                std::lock_guard<std::mutex> lock(mMutex);
                if (!mMatchConfigValid || rematch.matchId != (mMatchId + 1U)) continue;
                config = mMatchConfig;
            }
            config.rng_seed = rematch.rngSeed;
            ResetRoundState(false, false);
            {
                std::lock_guard<std::mutex> lock(mMutex);
                mMatchId = rematch.matchId;
                mMatchConfig = config;
                mMatchConfigValid = true;
            }
            ForceTransition(NetplayState::LoadingMatch, "host requested rematch");
            port_log("[NETPLAY] rematch received match=%u seed=%u\n",
                     rematch.matchId, rematch.rngSeed);
            continue;
        }

        if (event.type == PacketType::ReturnToCharacterSelect) {
            if (mLobby.IsHost() || State() != NetplayState::Results) continue;
            uint32_t matchId = 0;
            PayloadReader reader(event.payload.data(), event.payload.size());
            if (!reader.U32(matchId) || !reader.Empty()) continue;
            {
                std::lock_guard<std::mutex> lock(mMutex);
                if (matchId != mMatchId) continue;
            }
            ResetRoundState(true, true);
            ForceTransition(NetplayState::CharacterSelect, "host returned to character select");
            continue;
        }

        if (event.type == PacketType::LeaveSession) {
            mGameplay.Stop();
            mLobby.Stop(RejectReason::None, false);
            mDiscovery.Stop();
            ResetRoundState(true, true);
            {
                std::lock_guard<std::mutex> lock(mMutex);
                mMatchId = 0;
                mResultMismatchCount = 0;
                mLastError = "HOST LEFT SESSION";
            }
            ForceTransition(NetplayState::Offline, "host left results session");
            continue;
        }

        if (event.type == PacketType::ReturnToLobby) {
            uint8_t toCss = 0;
            PayloadReader reader(event.payload.data(), event.payload.size());
            if (!reader.U8(toCss)) toCss = 0;
            ApplyReturnToLobby(false, toCss != 0);
            continue;
        }

        if (event.type == PacketType::StateHash) {
            if (State() != NetplayState::InMatch || event.sourcePlayerId >= kMaxPlayers) continue;
            uint32_t frame = 0;
            uint64_t remoteHash = 0;
            if (!DecodeStateHashPayload(event.payload, frame, remoteHash)) continue;

            const LobbyView currentLobby = mLobby.Snapshot();
            if (event.sourcePlayerId == currentLobby.localPlayerId) continue;

            std::lock_guard<std::mutex> lock(mMutex);
            const std::size_t index = frame % kStateHashHistory;
            const StateHashSample& local = mLocalStateHashes[index];
            if (local.valid && local.frame == frame) {
                if (local.hash != remoteHash) {
                    ++mDeterminismMismatchCount;
                    if (mFirstDeterminismMismatchFrame == UINT32_MAX) {
                        mFirstDeterminismMismatchFrame = frame;
                        port_log("[NETPLAY][DET] FIRST STATE HASH MISMATCH frame=%u remote=P%u local=%08X%08X remote=%08X%08X\n",
                                 frame, event.sourcePlayerId + 1,
                                 static_cast<uint32_t>(local.hash >> 32), static_cast<uint32_t>(local.hash),
                                 static_cast<uint32_t>(remoteHash >> 32), static_cast<uint32_t>(remoteHash));
                    }
                }
            } else {
                mRemoteStateHashes[event.sourcePlayerId][index] = StateHashSample{frame, remoteHash, true};
            }
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

        if (mRendezvous.Hosting()) {
            const NetplayState st = State();
            const uint8_t rvStatus = (st == NetplayState::HostingLobby) ? 0 : 1;
            mRendezvous.UpdateStatus(mLobby.ConnectedPlayerCount(), rvStatus);
        }
    }

    const NetplayState state = State();
    const LobbyView lobby = mLobby.Snapshot();
    if (state == NetplayState::Connecting && lobby.connected) {
        ForceTransition(NetplayState::ClientLobby, "join accepted");
    } else if ((state == NetplayState::Connecting || state == NetplayState::ClientLobby ||
                state == NetplayState::CharacterSelect || state == NetplayState::LoadingMatch ||
                state == NetplayState::InMatch || state == NetplayState::Results) &&
               !mLobby.IsActive()) {
        mGameplay.Stop();
        {
            std::lock_guard<std::mutex> lock(mMutex);
            mLastError = lobby.lastMessage.empty() ? "DISCONNECTED" : lobby.lastMessage;
        }
        ForceTransition(NetplayState::Disconnected, "lobby connection ended");
    }

    if (State() == NetplayState::InMatch && mLobby.IsActive()) {
        uint8_t expectedMask = 0;
        uint8_t connectedMask = 0;
        {
            std::lock_guard<std::mutex> lock(mMutex);
            expectedMask = mMatchParticipantMask;
        }
        for (std::size_t player = 0; player < kMaxPlayers; ++player) {
            if (lobby.players[player].state == LobbySlotState::Connected ||
                lobby.players[player].state == LobbySlotState::Ready) {
                connectedMask |= static_cast<uint8_t>(1U << player);
            }
        }
        if (expectedMask != 0 && (connectedMask & expectedMask) != expectedMask) {
            std::vector<uint8_t> payload;
            PayloadWriter writer(payload);
            writer.U8(static_cast<uint8_t>(MatchResultReason::PeerDisconnected));
            if (mLobby.IsHost()) mLobby.SendSessionMessage(PacketType::Disconnect, payload);
            mGameplay.Stop();
            {
                std::lock_guard<std::mutex> lock(mMutex);
                mLastError = "PLAYER DISCONNECTED DURING MATCH";
            }
            ForceTransition(NetplayState::Disconnected, "match participant disconnected");
        }
    }

    if (mLobby.ConsumeCharacterSelectStart()) {
        ResetRoundState(true, true);
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
            if (!mGameplay.IsActive() && !StartGameplayTransport()) {
                SetError(transport::LastError(), "gameplay transport");
                return;
            }
            constexpr uint16_t kStartCountdownFrames = 60;
            const uint8_t hostInputDelay = static_cast<uint8_t>(
                std::clamp(mResolvedInputDelay.load(std::memory_order_acquire), 0, kMaxInputDelay));
            std::vector<uint8_t> payload;
            PayloadWriter writer(payload);
            writer.U16(kStartCountdownFrames);
            writer.U8(hostInputDelay);
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

void NetworkManager::ApplyReturnToLobby(bool localInitiated, bool toCss) {
    const NetplayState current = State();
    if (!toCss && (current == NetplayState::HostingLobby || current == NetplayState::ClientLobby)) {
        return;
    }
    mGameplay.Stop();
    std::vector<uint8_t> payload;
    PayloadWriter writer(payload);
    writer.U8(toCss ? 1U : 0U);
    if (mLobby.IsHost()) {
        mLobby.SendSessionMessage(PacketType::ReturnToLobby, payload);
        if (!toCss) mLobby.ReopenLobby();
        ResetRoundState(true, true);
        {
            std::lock_guard<std::mutex> lock(mMutex);
            mMatchId = 0;
            mResultMismatchCount = 0;
        }
        ForceTransition(toCss ? NetplayState::CharacterSelect : NetplayState::HostingLobby,
                        toCss ? "returned to character select" : "returned to host lobby");
        return;
    }
    if (localInitiated) {
        mLobby.SendSessionMessage(PacketType::ReturnToLobby, payload);
    }
    if (!toCss) mLobby.ReopenLobby();
    ResetRoundState(true, true);
    {
        std::lock_guard<std::mutex> lock(mMutex);
        mMatchId = 0;
        mResultMismatchCount = 0;
    }
    ForceTransition(toCss ? NetplayState::CharacterSelect : NetplayState::ClientLobby,
                    localInitiated ? "requested round return" : "host round return");
}

void NetworkManager::PushHostRulesToLobby() {
    if (!mLobby.IsHost()) return;
    LoadSettings();
    LobbyRuleSet rules;
    {
        std::lock_guard<std::mutex> lock(mMutex);
        rules.stage = mSettings.hostStage;
        rules.stocks = mSettings.hostStocks;
        rules.timeUnits = mSettings.hostTimeUnits;
        rules.itemRate = mSettings.hostItemRate;
        rules.teamBattle = mSettings.hostTeamBattle;
        rules.teamAttack = mSettings.hostTeamAttack;
        rules.damageRatio = mSettings.hostDamageRatio;
        rules.handicap = mSettings.hostHandicap;
    }
    mLobby.SetHostRules(rules);
}

void NetworkManager::PublishSnapshots() {
    std::vector<DiscoveredLobby> discovered = mDiscovery.Snapshot();
    for (DiscoveredLobby& board : mRendezvous.Lobbies()) {
        bool dup = false;
        for (const DiscoveredLobby& lan : discovered) {
            if (lan.hostIp == board.hostIp && lan.sessionId == board.sessionId) {
                dup = true;
                break;
            }
        }
        if (!dup) discovered.push_back(std::move(board));
    }
    LobbyView lobby = mLobby.Snapshot();
    std::lock_guard<std::mutex> lock(mMutex);
    mDiscoveredLobbies = std::move(discovered);
    mLobbyView = std::move(lobby);
}

int NetworkManager::ResolveInputDelay(const LobbyView& lobby) const {
    NetplaySettings settings;
    {
        std::lock_guard<std::mutex> lock(mMutex);
        settings = mSettings;
    }
    if (settings.inputDelay >= 0) return std::clamp(settings.inputDelay, 0, kMaxInputDelay);

    uint32_t maxPing = 0;
    uint32_t maxJitter = 0;
    for (const LobbyPlayerView& player : lobby.players) {
        if (player.playerId == lobby.localPlayerId) continue;
        if (player.state != LobbySlotState::Connected && player.state != LobbySlotState::Ready) continue;
        maxPing = std::max(maxPing, player.pingMs);
        maxJitter = std::max(maxJitter, player.jitterMs);
    }
    if (maxPing == 0) {
        return Mode() == NetplayMode::LocalAdhoc ? 1 : 2;
    }
    const uint32_t oneWayBudgetMs = ((maxPing + 1U) / 2U) + (maxJitter * 2U) + 2U;
    int frames = static_cast<int>((oneWayBudgetMs + 16U) / 17U);
    if (Mode() == NetplayMode::LocalAdhoc) return std::clamp(frames, 1, 2);
    return std::clamp(frames, 1, kMaxInputDelay);
}

bool NetworkManager::StartGameplayTransport(int forcedDelay) {
    const LobbyView lobby = mLobby.Snapshot();
    if (!lobby.connected || lobby.sessionId == 0 || lobby.localPlayerId >= kMaxPlayers) return false;
    const int delay = (forcedDelay >= 0) ? std::clamp(forcedDelay, 0, kMaxInputDelay) : ResolveInputDelay(lobby);
    const std::string endpoint = mLobby.IsHost() ? LocalEndpoint() : mGameplayHostEndpoint;
    if (!mLobby.IsHost() && endpoint.empty()) return false;
    const uint32_t gameplaySessionId = GameplaySessionId(lobby.sessionId);
    if (!mGameplay.Start(Mode(), mLobby.IsHost(), gameplaySessionId, lobby.localPlayerId, endpoint)) return false;
    uint8_t participantMask = 0;
    for (std::size_t player = 0; player < kMaxPlayers; ++player) {
        if (lobby.players[player].state == LobbySlotState::Connected ||
            lobby.players[player].state == LobbySlotState::Ready) {
            participantMask |= static_cast<uint8_t>(1U << player);
        }
    }
    {
        std::lock_guard<std::mutex> lock(mMutex);
        mMatchParticipantMask = participantMask;
    }
    mResolvedInputDelay.store(delay, std::memory_order_release);
    port_log("[NETPLAY] gameplay match=%u transport_session=%08X players=0x%02X delay=%d setting=%d mode=%s\n",
             mMatchId, gameplaySessionId, participantMask, delay, Settings().inputDelay,
             Mode() == NetplayMode::LocalAdhoc ? "ADHOC" : "ONLINE");
    return true;
}

uint32_t NetworkManager::GameplaySessionId(uint32_t lobbySessionId) const {
    uint32_t matchId;
    {
        std::lock_guard<std::mutex> lock(mMutex);
        matchId = mMatchId;
    }
    uint32_t value = lobbySessionId ^ (matchId * 0x9E3779B9U);
    value ^= value >> 16;
    value *= 0x85EBCA6BU;
    value ^= value >> 13;
    return value == 0 ? 1U : value;
}

uint32_t NetworkManager::NextRematchSeed(uint32_t nextMatchId) const {
    uint32_t seed;
    uint32_t sessionId;
    {
        std::lock_guard<std::mutex> lock(mMutex);
        seed = mMatchConfig.rng_seed;
        sessionId = mLobbyView.sessionId;
    }
    uint32_t next = seed ^ sessionId ^ (nextMatchId * 0x9E3779B9U);
    next ^= next << 13;
    next ^= next >> 17;
    next ^= next << 5;
    if (next == 0 || next == seed) next = seed + 0x6D2B79F5U + nextMatchId;
    return next == 0 ? 1U : next;
}

bool NetworkManager::MatchResultsEquivalent(const MatchResultPayload& local,
                                            const MatchResultPayload& authoritative) {
    if (local.matchId != authoritative.matchId || local.finalFrame != authoritative.finalFrame ||
        local.winner != authoritative.winner || local.reason != authoritative.reason ||
        local.placements != authoritative.placements ||
        local.stocksRemaining != authoritative.stocksRemaining) {
        return false;
    }
    return !authoritative.hasFinalHash ||
           (local.hasFinalHash && local.finalHash == authoritative.finalHash);
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

namespace {
std::string SanitizeRendezvousHost(const std::string& in) {
    std::string out;
    for (char c : in) {
        if (out.size() >= 128) break;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '.' || c == '-' || c == ':') {
            out.push_back(c);
        }
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}
} // namespace

void NetworkManager::SetRendezvousServer(const std::string& host) {
    LoadSettings();
    {
        std::lock_guard<std::mutex> lock(mMutex);
        mSettings.rendezvousServer = SanitizeRendezvousHost(host);
    }
    SaveSettings();
}

std::string NetworkManager::RendezvousServer() const {
    std::lock_guard<std::mutex> lock(mMutex);
    return mSettings.rendezvousServer;
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

void NetworkManager::SetHostStage(int stage) {
    LoadSettings();
    if (stage != kHostRuleRandom && (stage < 0 || stage > kHostStageMax)) {
        stage = kHostRuleRandom;
    }
    {
        std::lock_guard<std::mutex> lock(mMutex);
        mSettings.hostStage = stage;
    }
    SaveSettings();
}

void NetworkManager::SetHostStocks(int stocks) {
    LoadSettings();
    if (stocks != kHostRuleRandom && (stocks < kHostStocksMin || stocks > kHostStocksMax)) {
        stocks = kHostRuleRandom;
    }
    {
        std::lock_guard<std::mutex> lock(mMutex);
        mSettings.hostStocks = stocks;
    }
    SaveSettings();
}

void NetworkManager::SetHostTimeUnits(int units) {
    LoadSettings();
    if (units != kHostRuleRandom && units != kHostTimeInfinite &&
        (units < kHostTimeUnitMin || units > kHostTimeUnitMax)) {
        units = kHostRuleRandom;
    }
    {
        std::lock_guard<std::mutex> lock(mMutex);
        mSettings.hostTimeUnits = units;
    }
    SaveSettings();
}

void NetworkManager::SetHostItemRate(int rate) {
    LoadSettings();
    if (rate != kHostRuleRandom && (rate < 0 || rate > kHostItemRateMax)) {
        rate = 0;
    }
    {
        std::lock_guard<std::mutex> lock(mMutex);
        mSettings.hostItemRate = rate;
    }
    SaveSettings();
}

void NetworkManager::SetHostItemToggles(int mask) {
    LoadSettings();
    mask &= kHostItemTogglesMask;
    {
        std::lock_guard<std::mutex> lock(mMutex);
        mSettings.hostItemToggles = mask;
    }
    SaveSettings();
}

void NetworkManager::SetHostTeamBattle(int enabled) {
    LoadSettings();
    {
        std::lock_guard<std::mutex> lock(mMutex);
        mSettings.hostTeamBattle = enabled != 0 ? 1 : 0;
    }
    SaveSettings();
}

void NetworkManager::SetHostTeamAttack(int enabled) {
    LoadSettings();
    {
        std::lock_guard<std::mutex> lock(mMutex);
        mSettings.hostTeamAttack = enabled != 0 ? 1 : 0;
    }
    SaveSettings();
}

void NetworkManager::SetHostDamageRatio(int ratio) {
    LoadSettings();
    if (ratio != kHostRuleRandom &&
        (ratio < kHostDamageMin || ratio > kHostDamageMax || (ratio % kHostDamageStep) != 0)) {
        ratio = kHostDamageDefault;
    }
    {
        std::lock_guard<std::mutex> lock(mMutex);
        mSettings.hostDamageRatio = ratio;
    }
    SaveSettings();
}

void NetworkManager::SetHostHandicap(int mode) {
    LoadSettings();
    if (mode < 0 || mode > kHostHandicapMax) {
        mode = 0;
    }
    {
        std::lock_guard<std::mutex> lock(mMutex);
        mSettings.hostHandicap = mode;
    }
    SaveSettings();
}

int NetworkManager::BattleTimeSeconds() const {
    std::lock_guard<std::mutex> lock(mMutex);
    return mMatchConfigValid ? static_cast<int>(mMatchConfig.time_seconds) : 0;
}

bool NetworkManager::BattleIsTimed() const {
    const NetplayState s = State();
    if (s != NetplayState::LoadingMatch && s != NetplayState::InMatch) return false;
    std::lock_guard<std::mutex> lock(mMutex);
    return mMatchConfigValid && mMatchConfig.time_seconds != 0;
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

bool NetworkManager::JoinByAddress(const std::string& ip) {
    transport::SocketAddress parsed;
    if (!transport::ParseIpv4(ip, kLobbyPort, parsed)) {
        std::lock_guard<std::mutex> lock(mMutex);
        mLastError = "INVALID IP ADDRESS";
        return false;
    }
    LoadSettings();
    {
        std::lock_guard<std::mutex> lock(mMutex);
        mSettings.joinAddress = ip;
    }
    SaveSettings();

    Command command{};
    command.type = CommandType::JoinLobby;
    command.hostIp = ip;
    command.sessionId = 0;
    Enqueue(std::move(command));
    port_log("[NETPLAY] direct connect requested host=%s\n", ip.c_str());
    return true;
}

std::string NetworkManager::JoinAddress() const {
    std::lock_guard<std::mutex> lock(mMutex);
    return mSettings.joinAddress;
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

void NetworkManager::SubmitStateHash(uint32_t frame, uint64_t hash) {
    if (State() != NetplayState::InMatch) return;
    Command command{};
    command.type = CommandType::StateHash;
    command.frame = frame;
    command.stateHash = hash;
    Enqueue(std::move(command));
}

uint32_t NetworkManager::DeterminismMismatchCount() const {
    std::lock_guard<std::mutex> lock(mMutex);
    return mDeterminismMismatchCount;
}

bool NetworkManager::GameplayActive() const {
    return State() == NetplayState::InMatch && mGameplay.IsActive();
}

void NetworkManager::SubmitGameplayInput(uint32_t frame, uint16_t buttons, int8_t stickX, int8_t stickY) {
    if (!GameplayActive()) return;
    mGameplay.SubmitLocalInput(frame, buttons, stickX, stickY);
}

void NetworkManager::AbortGameplayDesync(uint32_t mismatchFrame, uint32_t currentFrame, uint8_t reason) {
    if (!GameplayActive()) return;
    Command command{};
    command.type = CommandType::GameplayAbort;
    command.frame = mismatchFrame;
    command.sequence = currentFrame;
    command.fighterKind = reason;
    Enqueue(std::move(command));
}

void NetworkManager::NotifyMatchFinished(const MatchResultPayload& result) {
    if (State() != NetplayState::InMatch && State() != NetplayState::Results) return;
    Command command{};
    command.type = CommandType::MatchFinished;
    command.matchResult = result;
    Enqueue(std::move(command));
}

void NetworkManager::RequestResultsRematch() {
    if (State() != NetplayState::Results) return;
    Enqueue(Command{CommandType::ResultsRematch});
}

void NetworkManager::RequestResultsCharacterSelect() {
    if (State() != NetplayState::Results) return;
    Enqueue(Command{CommandType::ResultsCharacterSelect});
}

void NetworkManager::RequestReturnToLobby(bool toCss) {
    const NetplayState state = State();
    if (state != NetplayState::CharacterSelect && state != NetplayState::LoadingMatch &&
        state != NetplayState::InMatch && state != NetplayState::Results) {
        return;
    }
    Command command{};
    command.type = CommandType::ReturnToLobby;
    command.ready = toCss;
    Enqueue(command);
}

void NetworkManager::RequestResultsLeave() {
    const NetplayState state = State();
    if (state != NetplayState::Results && state != NetplayState::Disconnected &&
        state != NetplayState::Error && state != NetplayState::InMatch &&
        state != NetplayState::CharacterSelect && state != NetplayState::LoadingMatch) {
        return;
    }
    Enqueue(Command{CommandType::ResultsLeave});
}

uint32_t NetworkManager::ResultMismatchCount() const {
    std::lock_guard<std::mutex> lock(mMutex);
    return mResultMismatchCount;
}

bool NetworkManager::ConsumeGameplayInput(GameplayFrameInput& input) {
    if (!GameplayActive()) return false;
    return mGameplay.PopRemoteInput(input);
}

void NetworkManager::GameplayLatency(uint32_t& pingMs, uint32_t& jitterMs) const {
    pingMs = 0;
    jitterMs = 0;
    const LobbyView lobby = Lobby();
    for (const LobbyPlayerView& player : lobby.players) {
        if (player.playerId == lobby.localPlayerId) continue;
        if (player.state != LobbySlotState::Connected && player.state != LobbySlotState::Ready) continue;
        pingMs = std::max(pingMs, player.pingMs);
        jitterMs = std::max(jitterMs, player.jitterMs);
    }
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

int port_netplay_join_address(const char* ip) {
    if (ip == nullptr) return 0;
    return ssb64::netplay::NetworkManager::Instance().JoinByAddress(ip) ? 1 : 0;
}

void port_netplay_get_join_address(char* out, int out_size) {
    ssb64::netplay::CopyStringOut(ssb64::netplay::NetworkManager::Instance().JoinAddress(), out, out_size);
}

void port_netplay_set_rendezvous_server(const char* host) {
    ssb64::netplay::NetworkManager::Instance().SetRendezvousServer(host != nullptr ? host : "");
}

void port_netplay_get_rendezvous_server(char* out, int out_size) {
    ssb64::netplay::CopyStringOut(ssb64::netplay::NetworkManager::Instance().RendezvousServer(), out, out_size);
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

int port_netplay_hostrules_get_stage(void) {
    return ssb64::netplay::NetworkManager::Instance().Settings().hostStage;
}

void port_netplay_hostrules_set_stage(int stage) {
    ssb64::netplay::NetworkManager::Instance().SetHostStage(stage);
}

int port_netplay_hostrules_get_stocks(void) {
    return ssb64::netplay::NetworkManager::Instance().Settings().hostStocks;
}

void port_netplay_hostrules_set_stocks(int stocks) {
    ssb64::netplay::NetworkManager::Instance().SetHostStocks(stocks);
}

int port_netplay_hostrules_get_time(void) {
    return ssb64::netplay::NetworkManager::Instance().Settings().hostTimeUnits;
}

void port_netplay_hostrules_set_time(int units) {
    ssb64::netplay::NetworkManager::Instance().SetHostTimeUnits(units);
}

int port_netplay_hostrules_get_item_rate(void) {
    return ssb64::netplay::NetworkManager::Instance().Settings().hostItemRate;
}

void port_netplay_hostrules_set_item_rate(int rate) {
    ssb64::netplay::NetworkManager::Instance().SetHostItemRate(rate);
}

int port_netplay_hostrules_get_item_toggles(void) {
    return ssb64::netplay::NetworkManager::Instance().Settings().hostItemToggles;
}

void port_netplay_hostrules_set_item_toggles(int mask) {
    ssb64::netplay::NetworkManager::Instance().SetHostItemToggles(mask);
}

int port_netplay_hostrules_get_team_battle(void) {
    return ssb64::netplay::NetworkManager::Instance().Settings().hostTeamBattle;
}

void port_netplay_hostrules_set_team_battle(int enabled) {
    ssb64::netplay::NetworkManager::Instance().SetHostTeamBattle(enabled);
}

int port_netplay_hostrules_get_team_attack(void) {
    return ssb64::netplay::NetworkManager::Instance().Settings().hostTeamAttack;
}

void port_netplay_hostrules_set_team_attack(int enabled) {
    ssb64::netplay::NetworkManager::Instance().SetHostTeamAttack(enabled);
}

int port_netplay_hostrules_get_damage_ratio(void) {
    return ssb64::netplay::NetworkManager::Instance().Settings().hostDamageRatio;
}

void port_netplay_hostrules_set_damage_ratio(int ratio) {
    ssb64::netplay::NetworkManager::Instance().SetHostDamageRatio(ratio);
}

int port_netplay_hostrules_get_handicap(void) {
    return ssb64::netplay::NetworkManager::Instance().Settings().hostHandicap;
}

void port_netplay_hostrules_set_handicap(int mode) {
    ssb64::netplay::NetworkManager::Instance().SetHostHandicap(mode);
}

int port_netplay_battle_time_seconds(void) {
    return ssb64::netplay::NetworkManager::Instance().BattleTimeSeconds();
}

int port_netplay_battle_is_timed(void) {
    return ssb64::netplay::NetworkManager::Instance().BattleIsTimed() ? 1 : 0;
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

int port_netplay_ime_begin(const char* title, const char* initial, int max_len, int mode) {
    return ssb64::netplay::platform::BeginImeDialog(title != nullptr ? title : "",
                                                    initial != nullptr ? initial : "",
                                                    max_len > 0 ? static_cast<uint32_t>(max_len) : 0U,
                                                    mode == 1) ? 1 : 0;
}

void port_netplay_ime_tick(void) {
    ssb64::netplay::platform::UpdateImeDialog();
}

int port_netplay_ime_state(void) {
    return static_cast<int>(ssb64::netplay::platform::GetImeState());
}

int port_netplay_ime_result(char* out, int out_size) {
    std::string value;
    ssb64::netplay::platform::GetImeResult(value);
    if (out == nullptr || out_size <= 0) return static_cast<int>(value.size());
    const std::size_t copy = value.size() < static_cast<std::size_t>(out_size - 1)
                                 ? value.size()
                                 : static_cast<std::size_t>(out_size - 1);
    std::memcpy(out, value.data(), copy);
    out[copy] = '\0';
    return static_cast<int>(copy);
}

void port_netplay_ime_cancel(void) {
    ssb64::netplay::platform::CancelImeDialog();
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

int port_netplay_lobby_get_rule_stage(void) {
    return ssb64::netplay::NetworkManager::Instance().Lobby().ruleStage;
}

int port_netplay_lobby_get_rule_stocks(void) {
    return ssb64::netplay::NetworkManager::Instance().Lobby().ruleStocks;
}

int port_netplay_lobby_get_rule_time(void) {
    return ssb64::netplay::NetworkManager::Instance().Lobby().ruleTimeUnits;
}

int port_netplay_lobby_get_rule_item_rate(void) {
    return ssb64::netplay::NetworkManager::Instance().Lobby().ruleItemRate;
}

int port_netplay_lobby_get_rule_team_battle(void) {
    return ssb64::netplay::NetworkManager::Instance().Lobby().ruleTeamBattle;
}

int port_netplay_lobby_get_rule_team_attack(void) {
    return ssb64::netplay::NetworkManager::Instance().Lobby().ruleTeamAttack;
}

int port_netplay_lobby_get_rule_damage_ratio(void) {
    return ssb64::netplay::NetworkManager::Instance().Lobby().ruleDamageRatio;
}

int port_netplay_lobby_get_rule_handicap(void) {
    return ssb64::netplay::NetworkManager::Instance().Lobby().ruleHandicap;
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

void port_netplay_submit_state_hash(uint32_t frame, uint32_t hash_high, uint32_t hash_low) {
    const uint64_t hash = (static_cast<uint64_t>(hash_high) << 32) | hash_low;
    ssb64::netplay::NetworkManager::Instance().SubmitStateHash(frame, hash);
}

uint32_t port_netplay_get_determinism_mismatch_count(void) {
    return ssb64::netplay::NetworkManager::Instance().DeterminismMismatchCount();
}

int port_netplay_gameplay_active(void) {
    return ssb64::netplay::NetworkManager::Instance().GameplayActive() ? 1 : 0;
}

int port_netplay_gameplay_get_input_delay(void) {
    return ssb64::netplay::NetworkManager::Instance().ResolvedInputDelay();
}

int port_netplay_gameplay_slot_connected(int slot) {
    if (slot < 0 || slot >= static_cast<int>(ssb64::netplay::kMaxPlayers)) return 0;
    const auto lobby = ssb64::netplay::NetworkManager::Instance().Lobby();
    const auto state = lobby.players[static_cast<std::size_t>(slot)].state;
    return state == ssb64::netplay::LobbySlotState::Connected ||
           state == ssb64::netplay::LobbySlotState::Ready ? 1 : 0;
}

void port_netplay_gameplay_submit_input(uint32_t frame, uint16_t buttons, int8_t stick_x, int8_t stick_y) {
    ssb64::netplay::NetworkManager::Instance().SubmitGameplayInput(frame, buttons, stick_x, stick_y);
}

void port_netplay_gameplay_abort_desync(uint32_t mismatch_frame, uint32_t current_frame, int reason) {
    if (reason < 0) reason = 0;
    if (reason > 255) reason = 255;
    ssb64::netplay::NetworkManager::Instance().AbortGameplayDesync(
        mismatch_frame, current_frame, static_cast<uint8_t>(reason));
}

void port_netplay_gameplay_match_finished(const PortNetplayMatchResult* source) {
    if (source == nullptr) return;
    ssb64::netplay::MatchResultPayload result{};
    result.finalFrame = source->final_frame;
    result.winner = source->winner;
    if (source->reason <= PORT_NETPLAY_RESULT_DESYNC_ABORT) {
        result.reason = static_cast<ssb64::netplay::MatchResultReason>(source->reason);
    } else {
        result.reason = ssb64::netplay::MatchResultReason::NoContest;
    }
    for (std::size_t player = 0; player < ssb64::netplay::kMaxPlayers; ++player) {
        result.placements[player] = source->placements[player];
        result.stocksRemaining[player] = source->stocks_remaining[player];
    }
    result.hasFinalHash = source->has_final_hash != 0;
    result.finalHash = (static_cast<uint64_t>(source->final_hash_high) << 32) |
                       source->final_hash_low;
    ssb64::netplay::NetworkManager::Instance().NotifyMatchFinished(result);
}

void port_netplay_results_rematch(void) {
    ssb64::netplay::NetworkManager::Instance().RequestResultsRematch();
}

void port_netplay_results_character_select(void) {
    ssb64::netplay::NetworkManager::Instance().RequestResultsCharacterSelect();
}

void port_netplay_return_to_lobby(void) {
    ssb64::netplay::NetworkManager::Instance().RequestReturnToLobby(false);
}

void port_netplay_ingame_return_css(void) {
    ssb64::netplay::NetworkManager::Instance().RequestReturnToLobby(true);
}

void port_netplay_ingame_leave(void) {
    ssb64::netplay::NetworkManager::Instance().RequestResultsLeave();
}

void port_netplay_results_leave(void) {
    ssb64::netplay::NetworkManager::Instance().RequestResultsLeave();
}

uint32_t port_netplay_results_mismatch_count(void) {
    return ssb64::netplay::NetworkManager::Instance().ResultMismatchCount();
}

int port_netplay_gameplay_consume_input(int* player, uint32_t* frame, uint16_t* buttons,
                                        int8_t* stick_x, int8_t* stick_y) {
    ssb64::netplay::GameplayFrameInput input{};
    if (!ssb64::netplay::NetworkManager::Instance().ConsumeGameplayInput(input)) return 0;
    if (player) *player = static_cast<int>(input.playerId);
    if (frame) *frame = input.frame;
    if (buttons) *buttons = input.buttons;
    if (stick_x) *stick_x = input.stickX;
    if (stick_y) *stick_y = input.stickY;
    return 1;
}

void port_netplay_gameplay_get_transport_stats(uint32_t* ping_ms, uint32_t* jitter_ms,
                                                uint32_t* packets_sent, uint32_t* packets_received,
                                                uint32_t* packets_dropped, uint32_t* sequence_gaps,
                                                uint32_t* duplicates, uint32_t* out_of_order) {
    auto& manager = ssb64::netplay::NetworkManager::Instance();
    uint32_t ping = 0;
    uint32_t jitter = 0;
    manager.GameplayLatency(ping, jitter);
    const auto stats = manager.GameplayTransportStats();
    if (ping_ms) *ping_ms = ping;
    if (jitter_ms) *jitter_ms = jitter;
    if (packets_sent) *packets_sent = stats.packetsSent;
    if (packets_received) *packets_received = stats.packetsReceived;
    if (packets_dropped) *packets_dropped = stats.packetsDropped;
    if (sequence_gaps) *sequence_gaps = stats.sequenceGaps;
    if (duplicates) *duplicates = stats.duplicates;
    if (out_of_order) *out_of_order = stats.outOfOrder;
}

uint64_t port_netplay_monotonic_us(void) {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

} // extern "C"
