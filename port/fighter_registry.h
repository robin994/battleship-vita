/**
 * fighter_registry.h - port-side per-fighter dispatch table.
 *
 * Decomp tables like dFTManagerDataFiles[] / dFTMainSpecialStatusDescs[] /
 * dFTCommonSpecialNStatusList[] are sized for the 27 vanilla FTKind values.
 * Synth fighters added by character mods (Crash, Banjo, etc.) have fkind
 * values past nFTKindEnumCount, so any unredirected `dFooArr[fkind]` site
 * OOBs and either crashes or pulls in adjacent data.
 *
 * This registry is the single redirection point. Every per-fkind dispatch
 * site in the decomp gets PORT-gated to read through one of the accessors
 * below. Vanilla rows are seeded once at boot from the decomp arrays; mods
 * call port_fighter_register() at MOD_INIT to add synth rows.
 */

#ifndef PORT_FIGHTER_REGISTRY_H
#define PORT_FIGHTER_REGISTRY_H

#include <battleship/vita_mod_fighter.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Register or replace a fighter's row. Resizes if fkind exceeds current
 * capacity. Caller-owned descriptor is shallow-copied. Registration is
 * MOD_INIT only -- not thread-safe for concurrent reads. */
void  port_fighter_register(int fkind, const FighterDescriptor *src);

/* Remove one row without shrinking the registry. Used by native-mod lifetime
 * management to roll back a synthetic fighter after its SUPRX is unloaded. */
void  port_fighter_unregister(int fkind);

/* Returns NULL if fkind out of range / unregistered. Field-specific
 * accessors below return safe defaults instead. */
const FighterDescriptor *port_fighter_descriptor(int fkind);

struct FTData              *port_fighter_data(int fkind);
struct FTStatusDesc        *port_fighter_special_descs(int fkind);
int                         port_fighter_special_descs_count(int fkind);
PortFTSpecialEnterFn        port_fighter_special_handler(int fkind, int kind);
int                         port_fighter_entry_appear(int fkind, int entry_id);
void                        port_fighter_entry_make_effect(int fkind, struct FTStruct *fp);
struct FTOpeningDesc       *port_fighter_opening_descs(int fkind);
struct FTCostume           *port_fighter_costume_row(int fkind);
float                       port_fighter_scale(int fkind);
int                         port_fighter_skeleton_col_anim_base(int fkind);
int                         port_fighter_down_bounce_fgm(int fkind);
int                         port_fighter_public_call_fgm(int fkind);
void                       *port_fighter_yoshi_egg_damage_coll(int fkind);
void                       *port_fighter_computer_attack_list(int fkind);

/* SR engine-extension accessors. */
int                         port_fighter_shield_hitlag_skip(int fkind,
                                                             struct GObj *fighter_gobj,
                                                             int status_id);
int                         port_fighter_ecb_override(int fkind,
                                                       struct FTStruct *fp,
                                                       int next_status_id,
                                                       float *out_upper,
                                                       float *out_middle);
int                         port_fighter_kirby_hat_id(int fkind);

/* Per-player "pending custom hat id" carried from the inhale-eat path to the
 * copy-apply paths. The decomp stores only the copy POWER fkind in
 * status_vars.kirby.specialn.copy_id, then re-derives the hat at apply time as
 * copy[copy_id].copy_modelpart_id. That re-derivation collides for a synth: a
 * synth's power fkind is its parent (Crash -> Mario), so copy[Mario] yields
 * Mario's vanilla hat (0x0C) instead of the synth's custom hat (0x2A). When
 * Kirby eats a synth, the eat path records the synth's hat id here; the apply
 * sites (ftKirbySpecialNCopyInitCopyVars, ftManagerMakeFighter) read it back
 * and use the custom hat id when it is set (>= 0x0F). hat_id 0 clears it.
 *
 * Vanilla copies never set this (eat path only records for a KHE-resolved
 * synth), so vanilla behavior is unchanged. Indexed by FTStruct.player.
 *
 * The slot itself lives in KirbyHatEngine; these forward through handlers the
 * mod installs at MOD_INIT via port_kirby_register_pending_hat_handlers. Before
 * the mod registers (or after it exits) set is a no-op and get returns 0. */
typedef void (*PortKirbySetPendingHatFn)(int player, int hat_id);
typedef int  (*PortKirbyGetPendingHatFn)(int player);
void                        port_kirby_register_pending_hat_handlers(PortKirbySetPendingHatFn set,
                                                                     PortKirbyGetPendingHatFn get);
void                        port_kirby_set_pending_hat(int player, int hat_id);
int                         port_kirby_get_pending_hat(int player);

/* "Active copied-special fkind" override. When Kirby copies a SYNTH fighter
 * (copy_id >= nFTKindEnumCount), SR runs the synth's own neutral-B routine on
 * Kirby (e.g. CrashNSP.ground_initial_), which puts Kirby into the SYNTH's
 * special action id (>= 0xDC). That action's status descriptor lives in the
 * SYNTH's special_descs table, not Kirby's, so ftMainSetStatus must key its
 * special-desc lookup off the synth fkind for the duration of that call. The
 * copy-special dispatch (ftKirbySpecialN/AirNSetStatusSelect) sets this to the
 * copy_id right before invoking the synth handler and clears it (0/Null) after.
 * ftMainSetStatus consults it; when unset (default) the lookup uses fp->fkind
 * so Kirby's own specials and vanilla copies are unaffected. */
void                        port_kirby_set_copy_special_fkind(int fkind);
int                         port_kirby_get_copy_special_fkind(void);

int                         port_fighter_crowd_chant_fgm(int fkind);
const char                 *port_fighter_action_string(int fkind, int action_id);
void                       *port_fighter_ai_attack_prevent_routine(int fkind);
void                       *port_fighter_ai_recovery_routine(int fkind);
void                       *port_fighter_ai_attack_weight_routine(int fkind);
int                         port_fighter_remix_1p_end_bgm(int fkind);
int                         port_fighter_remix_1p_ending_image_file_id(int fkind);
const unsigned char        *port_fighter_default_costumes(int fkind, int *out_count);
int                         port_fighter_team_costume(int fkind, int team);
const unsigned char        *port_fighter_charge_smash_frames(int fkind);
int                         port_fighter_costume_count(int fkind);
float                       port_fighter_css_spotlight_scale(int fkind);
int                         port_fighter_css_portrait(int fkind,
                                                      unsigned int *out_file_id,
                                                      unsigned int *out_offset);
int                         port_fighter_css_name(int fkind,
                                                  unsigned int *out_file_id,
                                                  unsigned int *out_offset);
int                         port_fighter_css_emblem(int fkind,
                                                    unsigned int *out_file_id,
                                                    unsigned int *out_offset);
int                         port_fighter_css_portrait_flash(int fkind,
                                                            unsigned int *out_file_id,
                                                            unsigned int *out_offset);
int                         port_fighter_intro_name(int fkind,
                                                   unsigned int *out_file_id,
                                                   unsigned int *out_offset);
int                         port_fighter_announce_fgm(int fkind);
int                         port_fighter_parent_fkind(int fkind);
PortFTProcFrameFn           port_fighter_proc_frame(int fkind);
int                         port_fighter_forward_throw_status(int fkind);
int                         port_fighter_jab3_status(int fkind);

/* SR custom capture-action accessors. _action returns the grabbed-opponent
 * action override for a grabber fkind (0 = none, use vanilla CapturePulled).
 * _dk_interrupt runs the grabber's break-out-skip routine and returns nonzero
 * to suppress the grabbed opponent's mash-out (0 if unregistered = vanilla). */
int                         port_fighter_custom_capture_action(int fkind);
int                         port_fighter_custom_capture_dk_interrupt(int fkind,
                                                                     struct FTStruct *grabber_fp);

/* VS-results accessors. For a synth fkind the decomp results consumers read
 * these instead of indexing the 12-entry vanilla tables. _name returns NULL
 * if the synth registered no results data (caller skips / surfaces the gap;
 * it never falls back to the parent). */
int                         port_fighter_results_announce_fgm(int fkind);
const char                 *port_fighter_results_name(int fkind);
float                       port_fighter_results_name_lx(int fkind);
float                       port_fighter_results_name_scale(int fkind);
float                       port_fighter_results_wins_lx(int fkind);

/* VS-results spinning emblem (SR winner-logo). CE publishes the current base
 * of the loaded FTEmblemModels blob via port_set_results_emblem_base after
 * every scene reset (the blob's internal pointer tokens are generation-scoped,
 * so the base must be refreshed each time). For a synth that registered emblem
 * offsets, port_fighter_results_emblem resolves the three model pointers from
 * (base + offset) and returns 1; it returns 0 (drawing nothing) if the synth
 * registered no emblem or the base has not been published. */
void                        port_set_results_emblem_base(void *base);
int                         port_fighter_results_emblem(int fkind,
                                                        void **out_dobjdesc,
                                                        void **out_mobjsub,
                                                        void **out_matanim);

/* Walk every registered fkind in ascending order. Used by figatree-heap
 * sizing and other "iterate every fighter" loops that previously walked
 * the vanilla array length. */
typedef void (*PortFighterForEachFn)(int fkind, const FighterDescriptor *desc, void *user);
void  port_fighter_for_each(PortFighterForEachFn cb, void *user);

/* Seeds slots [0, 27) from the vanilla decomp arrays. Called once at
 * PortInit. Safe to call again -- existing synth rows past 27 are kept,
 * vanilla rows are overwritten. */
void  port_fighter_seed_vanilla(void);

#ifdef __cplusplus
}
#endif

#endif /* PORT_FIGHTER_REGISTRY_H */
