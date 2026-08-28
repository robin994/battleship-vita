#pragma once

/* Public fighter-registration surface for native Vita mods.
 *
 * Engine structures remain opaque here on purpose. Character mods that need
 * to construct FTData/FTStatusDesc values may include the matching decomp
 * headers, while the generic native-mod ABI depends only on this stable
 * descriptor layout. Any layout change requires a Vita mod ABI version bump.
 */

#ifdef __cplusplus
extern "C" {
#endif

struct FTData;
struct FTStatusDesc;
struct FTOpeningDesc;
struct FTCostume;
struct GObj;
struct FTStruct;

typedef void (*PortFTSpecialEnterFn)(struct GObj *);
typedef void (*PortFTProcFrameFn)(struct GObj *);
typedef void (*PortFTEntryMakeEffectFn)(struct FTStruct *);
typedef int  (*PortFTShieldHitlagSkipFn)(struct GObj *, int status_id);
typedef int  (*PortFTECBOverrideFn)(struct FTStruct *fp, int next_status_id,
                                    float *out_upper, float *out_middle);
typedef int  (*PortFTCaptureDkInterruptFn)(struct FTStruct *grabber_fp);

enum {
    PORT_FIGHTER_SPECIAL_N      = 0,
    PORT_FIGHTER_SPECIAL_HI     = 1,
    PORT_FIGHTER_SPECIAL_LW     = 2,
    PORT_FIGHTER_SPECIAL_AIR_N  = 3,
    PORT_FIGHTER_SPECIAL_AIR_HI = 4,
    PORT_FIGHTER_SPECIAL_AIR_LW = 5,
    PORT_FIGHTER_SPECIAL_COUNT  = 6
};

enum {
    PORT_FIGHTER_ENTRY_APPEAR_R = 0,
    PORT_FIGHTER_ENTRY_APPEAR_L = 1
};

typedef struct FighterDescriptor {
    struct FTData               *ft_data;
    struct FTStatusDesc         *special_descs;
    int                          special_descs_count;
    PortFTSpecialEnterFn         special_handler[PORT_FIGHTER_SPECIAL_COUNT];
    int                          entry_appear_status[2];
    PortFTEntryMakeEffectFn      entry_make_effect;
    struct FTOpeningDesc        *opening_descs;
    struct FTCostume            *costume_row;
    float                        scale;
    int                          skeleton_col_anim_base;
    void                        *yoshi_egg_damage_coll;
    int                          down_bounce_fgm;
    int                          public_call_fgm;
    void                        *computer_attack_list;
    int                          css_motion_special;
    void                        *css_attack1_motion_descs;
    float                        css_spotlight_scale;
    PortFTShieldHitlagSkipFn     shield_hitlag_skip;
    PortFTECBOverrideFn          ecb_override;
    int                          kirby_hat_id;
    int                          crowd_chant_fgm;
    const char *const           *action_string_table;
    int                          action_string_table_count;
    int                          action_string_base_action_id;
    void                        *ai_attack_prevent_routine;
    void                        *ai_recovery_routine;
    void                        *ai_attack_weight_routine;
    int                          remix_1p_end_bgm;
    int                          remix_1p_ending_image_file_id;
    const unsigned char         *default_costumes;
    int                          default_costumes_count;
    int                          costume_count;
    unsigned char                team_costume[4];
    const unsigned char         *charge_smash_frames;
    int                          custom_capture_action;
    PortFTCaptureDkInterruptFn   custom_capture_dk_interrupt;
    int                          results_announce_fgm;
    const char                  *results_name;
    float                        results_name_lx;
    float                        results_name_scale;
    float                        results_wins_lx;
    int                          results_emblem_valid;
    unsigned int                 results_emblem_dobjdesc;
    unsigned int                 results_emblem_mobjsub;
    unsigned int                 results_emblem_matanim;
    /* Optional CSS presentation resources. File IDs are resolved through the
     * normal reloc registry, so a mod can ship extended Remix UI files in its
     * own O2R without teaching the core about any specific character. A zero
     * file id leaves that presentation element unavailable. */
    unsigned int                 css_portrait_file_id;
    unsigned int                 css_portrait_offset;
    unsigned int                 css_name_file_id;
    unsigned int                 css_name_offset;
    unsigned int                 css_emblem_file_id;
    unsigned int                 css_emblem_offset;
    /* Optional selected-portrait flash. Smash Remix stores the white-flash
     * Sprite immediately after each expanded portrait block, but the core
     * accepts an explicit resource so other mods are not tied to that layout. */
    unsigned int                 css_portrait_flash_file_id;
    unsigned int                 css_portrait_flash_offset;
    /* Presentation used outside CSS. The 1P intro has a separate character
     * name file/layout from the VS character-select name textures. */
    unsigned int                 intro_name_file_id;
    unsigned int                 intro_name_offset;
    /* Character-select / 1P announcer FGM. May refer to an external PCM FGM
     * registered through the native-mod audio API. */
    int                          announce_fgm;
    /* Vanilla fighter whose presentation/behaviour tables may be reused when
     * an engine subsystem still has a fixed 12/27-row table. Vanilla rows set
     * this to their own fkind. Synthetic fighters should set it to the fighter
     * they derive from (for example a Yoshi-derived fighter uses Yoshi). */
    int                          parent_fkind;
    /* Optional runtime extensions. NULL/-1 preserves vanilla behavior. */
    PortFTProcFrameFn             proc_frame;
    int                           forward_throw_status_id;
    int                           jab3_status_id;
} FighterDescriptor;

#ifdef __cplusplus
}
#endif
