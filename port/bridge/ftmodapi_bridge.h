#ifndef BATTLESHIP_FTMODAPI_BRIDGE_H
#define BATTLESHIP_FTMODAPI_BRIDGE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int port_mod_fighter_set_status(void *fighter_gobj, int status_id,
                                float frame_begin, float anim_speed,
                                uint32_t preserve_flags);
void port_mod_fighter_play_anim_events(void *fighter_gobj);
void port_mod_fighter_set_anim_speed(void *fighter_gobj, float anim_speed);
void port_mod_fighter_set_ground(void *fighter_gobj);
void port_mod_fighter_set_air(void *fighter_gobj);
void port_mod_fighter_set_wait_or_fall(void *fighter_gobj);
int port_mod_fighter_check_landing(void *fighter_gobj);
int port_mod_fighter_joint_world_position(void *fighter_gobj, int joint_id,
                                          float *out_x, float *out_y, float *out_z);
int port_mod_projectile_spawn_fflower(void *owner_gobj,
                                      float pos_x, float pos_y, float pos_z,
                                      float vel_x, float vel_y, float vel_z);

#ifdef __cplusplus
}
#endif

#endif
