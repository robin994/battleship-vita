/**
 * fighter_registry.cpp - storage + accessors for the per-fkind dispatch
 * registry declared in fighter_registry.h.
 *
 * Vector-backed, one row per fkind. Vanilla rows seeded at boot from
 * decomp arrays via port_fighter_seed_vanilla (implemented decomp-side in
 * decomp/src/ft/ftport.c, which has access to the source arrays + types).
 * Synth rows registered by character mods at MOD_INIT.
 */

#include "fighter_registry.h"

#include <cstring>
#include <memory>
#include <vector>

namespace {

using FDPtr = std::unique_ptr<FighterDescriptor>;
std::vector<FDPtr> sRegistry;

/* Returns the row for fkind, or nullptr if out of range / unregistered. */
const FighterDescriptor *get(int fkind)
{
    if (fkind < 0) return nullptr;
    if (static_cast<size_t>(fkind) >= sRegistry.size()) return nullptr;
    return sRegistry[static_cast<size_t>(fkind)].get();
}

} /* namespace */

extern "C" {

void port_fighter_register(int fkind, const FighterDescriptor *src)
{
    if (fkind < 0 || src == nullptr) return;

    if (static_cast<size_t>(fkind) >= sRegistry.size()) {
        sRegistry.resize(static_cast<size_t>(fkind) + 1);
    }

    auto row = std::make_unique<FighterDescriptor>();
    std::memcpy(row.get(), src, sizeof(FighterDescriptor));
    sRegistry[static_cast<size_t>(fkind)] = std::move(row);
}

void port_fighter_unregister(int fkind)
{
    if (fkind < 0 || static_cast<size_t>(fkind) >= sRegistry.size()) return;
    sRegistry[static_cast<size_t>(fkind)].reset();
}

const FighterDescriptor *port_fighter_descriptor(int fkind)
{
    return get(fkind);
}

int port_fighter_parent_fkind(int fkind)
{
    if (const auto *r = get(fkind)) {
        if (r->parent_fkind >= 0) return r->parent_fkind;
    }
    return fkind;
}

struct FTData *port_fighter_data(int fkind)
{
    if (const auto *r = get(fkind)) return r->ft_data;
    return nullptr;
}

struct FTStatusDesc *port_fighter_special_descs(int fkind)
{
    if (const auto *r = get(fkind)) return r->special_descs;
    return nullptr;
}

int port_fighter_special_descs_count(int fkind)
{
    if (const auto *r = get(fkind)) return r->special_descs_count;
    return 0;
}

PortFTSpecialEnterFn port_fighter_special_handler(int fkind, int kind)
{
    if (kind < 0 || kind >= PORT_FIGHTER_SPECIAL_COUNT) return nullptr;

    if (const auto *r = get(fkind)) return r->special_handler[kind];
    return nullptr;
}

/* nFTCommonStatusEntryNull is the safe sentinel for "no entry animation"
 * (decomp value: 1). Hard-coded so the header stays decomp-agnostic. */
static constexpr int PORT_ENTRY_NULL_STATUS = 1;

int port_fighter_entry_appear(int fkind, int entry_id)
{
    if (entry_id != 0 && entry_id != 1) return PORT_ENTRY_NULL_STATUS;
    if (const auto *r = get(fkind)) return r->entry_appear_status[entry_id];
    return PORT_ENTRY_NULL_STATUS;
}

void port_fighter_entry_make_effect(int fkind, struct FTStruct *fp)
{
    if (const auto *r = get(fkind)) {
        if (r->entry_make_effect != nullptr) r->entry_make_effect(fp);
    }
}

struct FTOpeningDesc *port_fighter_opening_descs(int fkind)
{
    if (const auto *r = get(fkind)) return r->opening_descs;
    return nullptr;
}

struct FTCostume *port_fighter_costume_row(int fkind)
{
    if (const auto *r = get(fkind)) return r->costume_row;
    return nullptr;
}

float port_fighter_scale(int fkind)
{
    if (const auto *r = get(fkind)) return r->scale;
    return 1.0f;
}

int port_fighter_skeleton_col_anim_base(int fkind)
{
    if (const auto *r = get(fkind)) return r->skeleton_col_anim_base;
    return 0;
}

int port_fighter_down_bounce_fgm(int fkind)
{
    if (const auto *r = get(fkind)) return r->down_bounce_fgm;
    return 0;
}

int port_fighter_public_call_fgm(int fkind)
{
    if (const auto *r = get(fkind)) return r->public_call_fgm;
    return 0;
}

void *port_fighter_yoshi_egg_damage_coll(int fkind)
{
    if (const auto *r = get(fkind)) return r->yoshi_egg_damage_coll;
    return nullptr;
}

void *port_fighter_computer_attack_list(int fkind)
{
    if (const auto *r = get(fkind)) return r->computer_attack_list;
    return nullptr;
}

void port_fighter_for_each(PortFighterForEachFn cb, void *user)
{
    if (cb == nullptr) return;
    for (size_t i = 0; i < sRegistry.size(); ++i) {
        if (sRegistry[i] != nullptr) {
            cb(static_cast<int>(i), sRegistry[i].get(), user);
        }
    }
}

/* SR engine-extension accessors. Each looks up the fkind row and falls
 * back to a safe sentinel (0 / NULL / safe int) when no override is
 * registered. */

int port_fighter_shield_hitlag_skip(int fkind, struct GObj *fighter_gobj, int status_id)
{
    if (const auto *r = get(fkind)) {
        if (r->shield_hitlag_skip != nullptr) {
            return r->shield_hitlag_skip(fighter_gobj, status_id);
        }
    }
    return 0;
}

int port_fighter_ecb_override(int fkind, struct FTStruct *fp, int next_status_id,
                               float *out_upper, float *out_middle)
{
    if (fp == nullptr || out_upper == nullptr || out_middle == nullptr) return 0;
    /* ECB resizing is per-fighter: only the descriptor registered for THIS
     * fighter's fkind may rewrite its diamond. The old form looped over every
     * registered fighter and called each one's callback with this fp, so a mod
     * that hooked one character could resize unrelated/vanilla fighters. */
    if (const auto *r = get(fkind)) {
        if (r->ecb_override != nullptr) {
            return r->ecb_override(fp, next_status_id, out_upper, out_middle);
        }
    }
    return 0;
}

int port_fighter_kirby_hat_id(int fkind)
{
    if (const auto *r = get(fkind)) return r->kirby_hat_id;
    return 0;
}

/* Per-player pending custom hat id storage lives in KirbyHatEngine. These
 * forward through handlers the mod installs at MOD_INIT. */
static PortKirbySetPendingHatFn s_set_pending = nullptr;
static PortKirbyGetPendingHatFn s_get_pending = nullptr;

void port_kirby_register_pending_hat_handlers(PortKirbySetPendingHatFn set,
                                              PortKirbyGetPendingHatFn get)
{
    s_set_pending = set;
    s_get_pending = get;
}

void port_kirby_set_pending_hat(int player, int hat_id)
{
    if (s_set_pending) s_set_pending(player, hat_id);
}

int port_kirby_get_pending_hat(int player)
{
    return s_get_pending ? s_get_pending(player) : 0;
}

/* -1 = unset; ftMainSetStatus then uses the acting fighter's own fkind. Scoped
 * to a single synth-copied-special SetStatus call by the dispatch select fn. */
static int sKirbyCopySpecialFkind = -1;

void port_kirby_set_copy_special_fkind(int fkind)
{
    sKirbyCopySpecialFkind = fkind;
}

int port_kirby_get_copy_special_fkind(void)
{
    return sKirbyCopySpecialFkind;
}

int port_fighter_crowd_chant_fgm(int fkind)
{
    if (const auto *r = get(fkind)) return r->crowd_chant_fgm;
    return 0;
}

const char *port_fighter_action_string(int fkind, int action_id)
{
    const auto *r = get(fkind);
    if (r == nullptr) return nullptr;
    if (r->action_string_table == nullptr) return nullptr;
    int idx = action_id - r->action_string_base_action_id;
    if (idx < 0 || idx >= r->action_string_table_count) return nullptr;
    return r->action_string_table[idx];
}

void *port_fighter_ai_attack_prevent_routine(int fkind)
{
    if (const auto *r = get(fkind)) return r->ai_attack_prevent_routine;
    return nullptr;
}

void *port_fighter_ai_recovery_routine(int fkind)
{
    if (const auto *r = get(fkind)) return r->ai_recovery_routine;
    return nullptr;
}

void *port_fighter_ai_attack_weight_routine(int fkind)
{
    if (const auto *r = get(fkind)) return r->ai_attack_weight_routine;
    return nullptr;
}

int port_fighter_remix_1p_end_bgm(int fkind)
{
    if (const auto *r = get(fkind)) return r->remix_1p_end_bgm;
    return 0;
}

int port_fighter_remix_1p_ending_image_file_id(int fkind)
{
    if (const auto *r = get(fkind)) return r->remix_1p_ending_image_file_id;
    return 0;
}

const unsigned char *port_fighter_default_costumes(int fkind, int *out_count)
{
    if (const auto *r = get(fkind)) {
        if (out_count != nullptr) *out_count = r->default_costumes_count;
        return r->default_costumes;
    }
    if (out_count != nullptr) *out_count = 0;
    return nullptr;
}

int port_fighter_team_costume(int fkind, int team)
{
    if (team < 0 || team >= 4) return -1;
    if (const auto *r = get(fkind)) {
        unsigned char c = r->team_costume[team];
        return (c == 0xFFu) ? -1 : (int)c;
    }
    return -1;
}

const unsigned char *port_fighter_charge_smash_frames(int fkind)
{
    if (const auto *r = get(fkind)) return r->charge_smash_frames;
    return nullptr;
}

int port_fighter_costume_count(int fkind)
{
    if (const auto *r = get(fkind)) return r->costume_count;
    return 0;
}

float port_fighter_css_spotlight_scale(int fkind)
{
    if (const auto *r = get(fkind)) {
        if (r->css_spotlight_scale > 0.0f) return r->css_spotlight_scale;
    }
    return 1.5f;
}

static int cssResource(const FighterDescriptor *r, unsigned int file_id, unsigned int offset,
                       unsigned int *out_file_id, unsigned int *out_offset)
{
    if (r == nullptr || file_id == 0) return 0;
    if (out_file_id) *out_file_id = file_id;
    if (out_offset) *out_offset = offset;
    return 1;
}

int port_fighter_css_portrait(int fkind, unsigned int *out_file_id, unsigned int *out_offset)
{
    const auto *r = get(fkind);
    return cssResource(r, r ? r->css_portrait_file_id : 0u,
                       r ? r->css_portrait_offset : 0u, out_file_id, out_offset);
}

int port_fighter_css_name(int fkind, unsigned int *out_file_id, unsigned int *out_offset)
{
    const auto *r = get(fkind);
    return cssResource(r, r ? r->css_name_file_id : 0u,
                       r ? r->css_name_offset : 0u, out_file_id, out_offset);
}

int port_fighter_css_emblem(int fkind, unsigned int *out_file_id, unsigned int *out_offset)
{
    const auto *r = get(fkind);
    return cssResource(r, r ? r->css_emblem_file_id : 0u,
                       r ? r->css_emblem_offset : 0u, out_file_id, out_offset);
}

int port_fighter_css_portrait_flash(int fkind, unsigned int *out_file_id, unsigned int *out_offset)
{
    const auto *r = get(fkind);
    return cssResource(r, r ? r->css_portrait_flash_file_id : 0u,
                       r ? r->css_portrait_flash_offset : 0u, out_file_id, out_offset);
}

int port_fighter_intro_name(int fkind, unsigned int *out_file_id, unsigned int *out_offset)
{
    const auto *r = get(fkind);
    return cssResource(r, r ? r->intro_name_file_id : 0u,
                       r ? r->intro_name_offset : 0u, out_file_id, out_offset);
}

int port_fighter_announce_fgm(int fkind)
{
    if (const auto *r = get(fkind)) return r->announce_fgm;
    return 0;
}

PortFTProcFrameFn port_fighter_proc_frame(int fkind)
{
    if (const auto *r = get(fkind)) return r->proc_frame;
    return nullptr;
}

int port_fighter_forward_throw_status(int fkind)
{
    if (const auto *r = get(fkind)) return r->forward_throw_status_id;
    return -1;
}

int port_fighter_jab3_status(int fkind)
{
    if (const auto *r = get(fkind)) return r->jab3_status_id;
    return -1;
}

int port_fighter_custom_capture_action(int fkind)
{
    if (const auto *r = get(fkind)) return r->custom_capture_action;
    return 0;
}

int port_fighter_custom_capture_dk_interrupt(int fkind, struct FTStruct *grabber_fp)
{
    if (const auto *r = get(fkind)) {
        if (r->custom_capture_dk_interrupt != nullptr) {
            return r->custom_capture_dk_interrupt(grabber_fp);
        }
    }
    return 0;
}

int port_fighter_results_announce_fgm(int fkind)
{
    if (const auto *r = get(fkind)) return r->results_announce_fgm;
    return 0;
}

const char *port_fighter_results_name(int fkind)
{
    if (const auto *r = get(fkind)) return r->results_name;
    return nullptr;
}

float port_fighter_results_name_lx(int fkind)
{
    if (const auto *r = get(fkind)) return r->results_name_lx;
    return 0.0f;
}

float port_fighter_results_name_scale(int fkind)
{
    if (const auto *r = get(fkind)) return r->results_name_scale;
    return 1.0f;
}

float port_fighter_results_wins_lx(int fkind)
{
    if (const auto *r = get(fkind)) return r->results_wins_lx;
    return 0.0f;
}

/* CE-owned FTEmblemModels blob base, refreshed every scene reset. The emblem
 * model's internal Gfx/Vtx/texture pointer tokens are wiped + re-registered on
 * each lbRelocInitSetup, so the base buffer pointer is stable but the resolved
 * model is only valid after CE has reloaded for the current generation. CE
 * passes NULL here if the reload failed, which disables the emblem draw. */
static void *s_results_emblem_base = nullptr;

void port_set_results_emblem_base(void *base)
{
    s_results_emblem_base = base;
}

int port_fighter_results_emblem(int fkind, void **out_dobjdesc,
                                void **out_mobjsub, void **out_matanim)
{
    const auto *r = get(fkind);
    if (r == nullptr || r->results_emblem_valid == 0 ||
        s_results_emblem_base == nullptr)
    {
        return 0;
    }
    unsigned char *base = static_cast<unsigned char *>(s_results_emblem_base);
    if (out_dobjdesc) *out_dobjdesc = base + r->results_emblem_dobjdesc;
    if (out_mobjsub)  *out_mobjsub  = base + r->results_emblem_mobjsub;
    if (out_matanim)  *out_matanim  = base + r->results_emblem_matanim;
    return 1;
}

} /* extern "C" */
