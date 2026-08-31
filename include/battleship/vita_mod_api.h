#ifndef BATTLESHIP_VITA_MOD_API_H
#define BATTLESHIP_VITA_MOD_API_H

#include <stddef.h>
#include <stdint.h>
#include <battleship/vita_mod_fighter.h>
#include <battleship/vita_mod_reloc.h>
#include <battleship/vita_mod_audio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BATTLESHIP_VITA_MOD_MAGIC 0x42534D44u /* 'BSMD' */
#define BATTLESHIP_VITA_MOD_ABI_VERSION 9u

typedef uint32_t BattleShipVitaModHandle;

typedef struct BattleShipVitaModAPI {
    uint32_t abi_version;
    uint32_t struct_size;

    void (*log)(BattleShipVitaModHandle mod, const char *message);

    int (*asset_size)(BattleShipVitaModHandle mod, const char *path, uint32_t *out_size);
    int (*asset_read)(BattleShipVitaModHandle mod, const char *path,
                      void *dst, uint32_t dst_size, uint32_t *out_size);

    /* Fighter registrations are owned by the calling mod handle. The host
     * snapshots any previous row and restores it automatically when the
     * module is unloaded, so hot reload never leaves callbacks pointing into
     * an unloaded SUPRX. */
    int (*fighter_register)(BattleShipVitaModHandle mod, int fkind,
                            const struct FighterDescriptor *descriptor);
    int (*fighter_unregister)(BattleShipVitaModHandle mod, int fkind);
    const struct FighterDescriptor *(*fighter_descriptor)(int fkind);

    /* Reloc state is owned by the module handle as well. The host tracks
     * destination ranges and explicit tokens so unloading a SUPRX cannot
     * leave live reloc tokens pointing into freed module/heap memory. */
    void (*reloc_load_private)(BattleShipVitaModHandle mod,
                               void *ram_dst, uint32_t dst_size,
                               const void *src_bytes, uint32_t src_size,
                               uint16_t reloc_intern_offset);
    uint32_t (*reloc_register_pointer)(BattleShipVitaModHandle mod, void *ptr);
    int (*reloc_unregister_pointer)(BattleShipVitaModHandle mod, uint32_t token);
    void *(*reloc_resolve_pointer)(uint32_t token);

    /* Register reloc resources that physically live inside this mod's O2R.
     * resource_path is copied by the host during this call. Last registration
     * wins for a file id; unload restores any previous mod/vanilla provider. */
    int (*reloc_files_register)(BattleShipVitaModHandle mod,
                                const BattleShipVitaModRelocResource *resources,
                                uint32_t count);
    int (*reloc_file_unregister)(BattleShipVitaModHandle mod, uint32_t file_id);

    /* Register FGM ids backed by PCM assets inside this mod's O2R. This lets
     * external fighters keep their original Remix FGM ids without replacing
     * BattleShip's vanilla N64 sound bank. Registrations are owner-scoped and
     * automatically removed on module unload. */
    int (*audio_fgm_register)(BattleShipVitaModHandle mod,
                              const BattleShipVitaModAudioResource *resources,
                              uint32_t count);
    int (*audio_fgm_unregister)(BattleShipVitaModHandle mod, uint16_t fgm_id);

    /* Generic fighter-runtime services for freestanding zero-import modules. */
    int (*fighter_set_status)(void *fighter_gobj, int status_id,
                              float frame_begin, float anim_speed,
                              uint32_t preserve_flags);
    void (*fighter_play_anim_events)(void *fighter_gobj);
    void (*fighter_set_anim_speed)(void *fighter_gobj, float anim_speed);
    void (*fighter_set_ground)(void *fighter_gobj);
    void (*fighter_set_air)(void *fighter_gobj);
    void (*fighter_set_wait_or_fall)(void *fighter_gobj);
    int (*fighter_check_landing)(void *fighter_gobj);
    int (*fighter_joint_world_position)(void *fighter_gobj, int joint_id,
                                        float *out_x, float *out_y, float *out_z);
    int (*projectile_spawn_builtin)(BattleShipVitaModHandle mod,
                                    uint32_t projectile_kind,
                                    void *owner_gobj,
                                    float pos_x, float pos_y, float pos_z,
                                    float vel_x, float vel_y, float vel_z);
    /* Generalized Mario-style special-up collision path. Native fighter mods
     * supply their own landing lag while the host retains collision/cliff
     * ownership and the exact engine transition semantics. */
    void (*fighter_special_hi_map)(void *fighter_gobj, float landing_lag);
} BattleShipVitaModAPI;

enum {
    BATTLESHIP_VITA_BUILTIN_PROJECTILE_FFLOWER_FLAME = 1u
};

typedef struct BattleShipVitaModContext {
    uint32_t magic;
    uint32_t abi_version;
    uint32_t struct_size;
    BattleShipVitaModHandle mod_handle;
    const BattleShipVitaModAPI *api;
} BattleShipVitaModContext;

#ifdef __cplusplus
}
#endif

#endif /* BATTLESHIP_VITA_MOD_API_H */
