#pragma once

#include <stdint.h>

typedef struct PortNetplayMatchConfig {
    uint32_t rng_seed;
    uint32_t stage_kind;
    uint32_t stocks;
    uint32_t time_limit;
    uint32_t time_seconds;
    uint32_t item_switch;
    uint32_t item_toggles;
    uint8_t game_type;
    uint8_t game_rules;
    uint8_t is_team_battle;
    uint8_t handicap;
    uint8_t is_team_attack;
    uint8_t damage_ratio;
    uint8_t item_appearance_rate;
    uint8_t is_not_teamshadows;
    uint8_t player_count;
    uint8_t player_kinds[4];
    uint8_t fighter_kinds[4];
    uint8_t costumes[4];
    uint8_t teams[4];
    uint8_t handicaps[4];
    uint8_t levels[4];
    uint8_t shades[4];
} PortNetplayMatchConfig;

typedef struct PortNetplayMatchResult {
    uint32_t final_frame;
    uint8_t winner;
    uint8_t placements[4];
    int8_t stocks_remaining[4];
    uint8_t reason;
    uint8_t has_final_hash;
    uint32_t final_hash_high;
    uint32_t final_hash_low;
} PortNetplayMatchResult;

enum {
    PORT_NETPLAY_RESULT_COMPLETED = 0,
    PORT_NETPLAY_RESULT_NO_CONTEST,
    PORT_NETPLAY_RESULT_PEER_DISCONNECTED,
    PORT_NETPLAY_RESULT_DESYNC_ABORT
};

enum {
    PORT_NETPLAY_STATE_OFFLINE = 0,
    PORT_NETPLAY_STATE_DISCOVERING,
    PORT_NETPLAY_STATE_CONNECTING,
    PORT_NETPLAY_STATE_HOSTING_LOBBY,
    PORT_NETPLAY_STATE_CLIENT_LOBBY,
    PORT_NETPLAY_STATE_CHARACTER_SELECT,
    PORT_NETPLAY_STATE_LOADING_MATCH,
    PORT_NETPLAY_STATE_IN_MATCH,
    PORT_NETPLAY_STATE_RESULTS,
    PORT_NETPLAY_STATE_DISCONNECTED,
    PORT_NETPLAY_STATE_ERROR
};

enum {
    PORT_NETPLAY_MODE_NONE = 0,
    PORT_NETPLAY_MODE_LOCAL_ADHOC,
    PORT_NETPLAY_MODE_ONLINE
};

enum {
    PORT_NETPLAY_ADHOC_DIALOG_INACTIVE = 0,
    PORT_NETPLAY_ADHOC_DIALOG_AWAITING,
    PORT_NETPLAY_ADHOC_DIALOG_RUNNING,
    PORT_NETPLAY_ADHOC_DIALOG_CONNECTED,
    PORT_NETPLAY_ADHOC_DIALOG_CANCELED,
    PORT_NETPLAY_ADHOC_DIALOG_ERROR
};

#ifdef __cplusplus
extern "C" {
#endif

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
int port_netplay_hostrules_get_stage(void);
void port_netplay_hostrules_set_stage(int stage);
int port_netplay_hostrules_get_stocks(void);
void port_netplay_hostrules_set_stocks(int stocks);
int port_netplay_hostrules_get_time(void);
void port_netplay_hostrules_set_time(int units);
int port_netplay_battle_time_seconds(void);
int port_netplay_battle_is_timed(void);
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
int port_netplay_lobby_get_rule_stage(void);
int port_netplay_lobby_get_rule_stocks(void);
int port_netplay_lobby_get_rule_time(void);
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
int port_netplay_css_active(void);
int port_netplay_css_is_host(void);
int port_netplay_css_get_local_player(void);
int port_netplay_css_slot_connected(int slot);
void port_netplay_css_submit_input(uint16_t buttons, int8_t stick_x, int8_t stick_y);
int port_netplay_css_consume_input(int slot, uint16_t* buttons, uint16_t* button_tap,
                                  uint16_t* button_release, int8_t* stick_x, int8_t* stick_y);
void port_netplay_css_notify_lock(int fighter_kind, int costume, int shade);
void port_netplay_css_notify_unlock(void);
void port_netplay_css_host_commit_match(const PortNetplayMatchConfig* config);
int port_netplay_get_match_config(PortNetplayMatchConfig* out_config);
void port_netplay_loading_ready(void);
int port_netplay_match_gate_tick(void);
void port_netplay_submit_state_hash(uint32_t frame, uint32_t hash_high, uint32_t hash_low);
uint32_t port_netplay_get_determinism_mismatch_count(void);
int port_netplay_gameplay_active(void);
int port_netplay_gameplay_get_input_delay(void);
int port_netplay_gameplay_slot_connected(int slot);
void port_netplay_gameplay_submit_input(uint32_t frame, uint16_t buttons, int8_t stick_x, int8_t stick_y);
void port_netplay_gameplay_abort_desync(uint32_t mismatch_frame, uint32_t current_frame, int reason);
void port_netplay_gameplay_match_finished(const PortNetplayMatchResult* result);
void port_netplay_results_rematch(void);
void port_netplay_results_character_select(void);
void port_netplay_results_leave(void);
void port_netplay_return_to_lobby(void);
void port_netplay_ingame_return_css(void);
void port_netplay_ingame_leave(void);
uint32_t port_netplay_results_mismatch_count(void);
int port_netplay_gameplay_consume_input(int* player, uint32_t* frame, uint16_t* buttons,
                                        int8_t* stick_x, int8_t* stick_y);
void port_netplay_gameplay_get_transport_stats(uint32_t* ping_ms, uint32_t* jitter_ms,
                                                uint32_t* packets_sent, uint32_t* packets_received,
                                                uint32_t* packets_dropped, uint32_t* sequence_gaps,
                                                uint32_t* duplicates, uint32_t* out_of_order);
uint64_t port_netplay_monotonic_us(void);

#ifdef __cplusplus
}
#endif
