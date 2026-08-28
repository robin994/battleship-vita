#pragma once

#include <stdint.h>

typedef struct PortNetplayMatchConfig {
    uint32_t rng_seed;
    uint32_t stage_kind;
    uint32_t stocks;
    uint32_t time_limit;
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

#ifdef __cplusplus
}
#endif
