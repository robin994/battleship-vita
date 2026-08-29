#pragma once

#include "GameplaySession.h"
#include "LanDiscovery.h"
#include "LobbySession.h"
#include "NetplayProtocol.h"
#include "netplay_bridge.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ssb64::netplay {

struct NetplaySettings {
    std::string playerName = "PLAYER";
    int inputDelay = -1; // -1 = AUTO, otherwise 0..4 frames
    bool showStats = false;
};

class NetworkManager {
public:
    static NetworkManager& Instance();

    NetworkManager(const NetworkManager&) = delete;
    NetworkManager& operator=(const NetworkManager&) = delete;

    void EnterMultiplayerMenu();
    void LeaveMultiplayerMenu();
    void Shutdown();

    NetplayState State() const { return mState.load(std::memory_order_acquire); }
    NetplayMode Mode() const { return mMode.load(std::memory_order_acquire); }
    bool NetworkInitialized() const { return mNetworkInitialized.load(std::memory_order_acquire); }
    bool NetworkConnected() const { return mNetworkConnected.load(std::memory_order_acquire); }
    bool AdhocInitialized() const { return mAdhocInitialized.load(std::memory_order_acquire); }
    std::string LocalIp() const;
    std::string LocalEndpoint() const;
    std::string LastError() const;

    NetplaySettings Settings() const;
    void SetPlayerName(const std::string& name);
    void SetInputDelay(int frames);
    void SetShowStats(bool enabled);
    void ResetSettings();
    void SetMode(NetplayMode mode);
    bool ModeReady() const;

    void StartDiscovery();
    void RefreshDiscovery();
    void HostLobby();
    bool JoinDiscoveredLobby(std::size_t index);
    void CancelNetworkActivity();
    void ToggleLocalReady();
    void RequestStartCharacterSelect();
    void SubmitCssInput(uint16_t buttons, int8_t stickX, int8_t stickY);
    bool ConsumeCssInput(uint8_t playerId, uint16_t& buttons, uint16_t& buttonTap,
                         uint16_t& buttonRelease, int8_t& stickX, int8_t& stickY);
    void NotifyCssLock(uint8_t fighterKind, uint8_t costume, uint8_t shade);
    void NotifyCssUnlock();
    void CommitMatchConfig(const PortNetplayMatchConfig& config);
    bool GetMatchConfig(PortNetplayMatchConfig& config) const;
    void NotifyLoadingReady();
    bool MatchGateTick();
    void SubmitStateHash(uint32_t frame, uint64_t hash);
    uint32_t DeterminismMismatchCount() const;
    bool GameplayActive() const;
    int ResolvedInputDelay() const { return mResolvedInputDelay.load(std::memory_order_acquire); }
    void SubmitGameplayInput(uint32_t frame, uint16_t buttons, int8_t stickX, int8_t stickY);
    void AbortGameplayDesync(uint32_t mismatchFrame, uint32_t currentFrame, uint8_t reason);
    void NotifyMatchFinished(const MatchResultPayload& result);
    void RequestResultsRematch();
    void RequestResultsCharacterSelect();
    void RequestResultsLeave();
    uint32_t ResultMismatchCount() const;
    bool ConsumeGameplayInput(GameplayFrameInput& input);
    GameplayStats GameplayTransportStats() const { return mGameplay.Stats(); }
    void GameplayLatency(uint32_t& pingMs, uint32_t& jitterMs) const;

    std::vector<DiscoveredLobby> DiscoveredLobbies() const;
    LobbyView Lobby() const;

    // Centralized transition point. Later lobby/gameplay modules use this
    // instead of independent boolean flags.
    bool Transition(NetplayState expected, NetplayState next, const char* reason);
    void ForceTransition(NetplayState next, const char* reason);

private:
    NetworkManager() = default;
    ~NetworkManager();

    void LoadSettings();
    void SaveSettings();
    void StartWorker();
    void StopWorker();
    void WorkerMain();
    void SetError(int code, const char* where);
    void ProcessCommands();
    void PollNetworkServices();
    void PublishSnapshots();
    uint32_t MakeSessionId() const;
    bool StartGameplayTransport(int forcedDelay = -1);
    int ResolveInputDelay(const LobbyView& lobby) const;
    void ResetRoundState(bool clearMatchConfig, bool clearCssState);
    uint32_t NextRematchSeed(uint32_t nextMatchId) const;
    uint32_t GameplaySessionId(uint32_t lobbySessionId) const;
    static bool MatchResultsEquivalent(const MatchResultPayload& local,
                                       const MatchResultPayload& authoritative);

    enum class CommandType {
        StartDiscovery,
        RefreshDiscovery,
        HostLobby,
        JoinLobby,
        CancelActivity,
        SetReady,
        StartCharacterSelect,
        CssInput,
        CssLock,
        CssUnlock,
        CommitMatchConfig,
        LoadingReady,
        StateHash,
        GameplayAbort,
        MatchFinished,
        ResultsRematch,
        ResultsCharacterSelect,
        ResultsLeave,
        SetMode,
    };

    struct Command {
        CommandType type = CommandType::CancelActivity;
        std::string hostIp;
        uint32_t sessionId = 0;
        bool ready = false;
        uint32_t sequence = 0;
        uint16_t buttons = 0;
        int8_t stickX = 0;
        int8_t stickY = 0;
        uint8_t fighterKind = 0;
        uint8_t costume = 0;
        uint8_t shade = 0;
        uint32_t frame = 0;
        uint64_t stateHash = 0;
        MatchResultPayload matchResult{};
        PortNetplayMatchConfig matchConfig{};
        NetplayMode mode = NetplayMode::None;
    };

    void Enqueue(Command command);

    mutable std::mutex mMutex;
    std::condition_variable mWake;
    std::thread mWorker;
    bool mWorkerRunning = false;
    bool mStopRequested = false;
    bool mSettingsLoaded = false;

    std::atomic<NetplayState> mState{NetplayState::Offline};
    std::atomic<NetplayMode> mMode{NetplayMode::None};
    std::atomic<bool> mNetworkInitialized{false};
    std::atomic<bool> mNetworkConnected{false};
    std::atomic<bool> mAdhocInitialized{false};
    std::string mLocalIp;
    std::string mLocalMac;
    std::string mLastError;
    NetplaySettings mSettings{};
    std::deque<Command> mCommands;
    std::vector<DiscoveredLobby> mDiscoveredLobbies;
    LobbyView mLobbyView{};

    struct CssInputState {
        uint32_t sequence = 0;
        uint16_t buttons = 0;
        uint16_t tapPending = 0;
        uint16_t releasePending = 0;
        int8_t stickX = 0;
        int8_t stickY = 0;
        bool valid = false;
    };
    std::array<CssInputState, kMaxPlayers> mCssInputs{};
    std::atomic<uint32_t> mCssLocalSequence{1};
    PortNetplayMatchConfig mMatchConfig{};
    bool mMatchConfigValid = false;
    std::array<bool, kMaxPlayers> mLoadingReady{};
    bool mLoadingReadySent = false;
    bool mStartMatchReceived = false;
    int mMatchStartCountdown = -1;

    struct StateHashSample {
        uint32_t frame = 0;
        uint64_t hash = 0;
        bool valid = false;
    };
    static constexpr std::size_t kStateHashHistory = 64;
    std::array<StateHashSample, kStateHashHistory> mLocalStateHashes{};
    std::array<std::array<StateHashSample, kStateHashHistory>, kMaxPlayers> mRemoteStateHashes{};
    uint32_t mDeterminismMismatchCount = 0;
    uint32_t mFirstDeterminismMismatchFrame = UINT32_MAX;
    uint32_t mMatchId = 0;
    uint8_t mMatchParticipantMask = 0;
    MatchResultPayload mLocalMatchResult{};
    MatchResultPayload mAuthoritativeMatchResult{};
    bool mLocalMatchResultValid = false;
    bool mAuthoritativeMatchResultValid = false;
    uint32_t mResultMismatchCount = 0;
    std::string mGameplayHostEndpoint;
    std::atomic<int> mResolvedInputDelay{1};

    // Worker-thread-owned network services. Socket I/O never runs on the game
    // or render thread; the UI only sees copies published under mMutex.
    LanDiscovery mDiscovery;
    LobbySession mLobby;
    GameplaySession mGameplay;
};

} // namespace ssb64::netplay

extern "C" {

void port_netplay_enter_menu(void);
void port_netplay_leave_menu(void);
int port_netplay_get_state(void);
int port_netplay_network_initialized(void);
int port_netplay_network_connected(void);
void port_netplay_get_local_ip(char* out, int out_size);
void port_netplay_get_last_error(char* out, int out_size);
void port_netplay_get_player_name(char* out, int out_size);
void port_netplay_set_player_name(const char* name);
int port_netplay_get_input_delay(void);
void port_netplay_set_input_delay(int frames);
int port_netplay_get_show_stats(void);
void port_netplay_set_show_stats(int enabled);
void port_netplay_reset_settings(void);
void port_netplay_set_mode(int mode);
int port_netplay_get_mode(void);
int port_netplay_mode_ready(void);
void port_netplay_adhoc_dialog_tick(void);
int port_netplay_adhoc_dialog_state(void);
int port_netplay_common_dialog_active(void);
void port_netplay_start_discovery(void);
void port_netplay_refresh_discovery(void);
void port_netplay_host_lobby(void);
int port_netplay_join_discovered_lobby(int index);
void port_netplay_cancel_activity(void);
int port_netplay_get_discovery_count(void);
int port_netplay_get_discovery_lobby(int index, char* host_name, int host_name_size,
                                     char* host_ip, int host_ip_size, char* build_id, int build_id_size,
                                     int* players, int* max_players, int* ping_ms, int* protocol_version,
                                     int* status, int* compatible);
int port_netplay_lobby_is_host(void);
int port_netplay_lobby_is_connected(void);
int port_netplay_lobby_get_status(void);
int port_netplay_lobby_get_local_player(void);
int port_netplay_lobby_get_player_count(void);
int port_netplay_lobby_get_slot(int slot, char* player_name, int player_name_size,
                                int* slot_state, int* ping_ms, int* jitter_ms);
int port_netplay_lobby_local_ready(void);
int port_netplay_lobby_can_start(void);
void port_netplay_lobby_toggle_ready(void);
void port_netplay_lobby_start(void);
void port_netplay_get_lobby_message(char* out, int out_size);
int port_netplay_get_protocol_version(void);
void port_netplay_get_build_id(char* out, int out_size);
void port_netplay_submit_state_hash(uint32_t frame, uint32_t hash_high, uint32_t hash_low);
uint32_t port_netplay_get_determinism_mismatch_count(void);

}
