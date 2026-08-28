/*
 * ModRegistry.h — read-only inventory of the engine's mounted mods.
 *
 * Phase 1 of the in-game Mod Manager (see docs/mod_manager_menu_plan.md).
 * This is the *model* behind the Mods menu panel: a pure, side-effect-free
 * query that enumerates the mods the engine currently has mounted, reads
 * their manifest metadata, and marks which ones actually loaded.
 *
 * It deliberately knows nothing about ImGui — the menu renders a snapshot.
 * That keeps mod discovery testable and reusable independently of the UI.
 *
 * Source of truth: a mounted archive (folder or .o2r/.otr/.zip) is treated
 * as a mod iff it carries a manifest.json at its root — exactly how
 * MountModsDir (port.cpp) recognises a mod folder. The base game and shader
 * archives carry no manifest.json and are therefore excluded automatically.
 *
 * "Loaded" is keyed by the manifest name, which is the same key
 * ScriptLoader stores its loaded modules under (mLoadedScripts[Name]).
 *
 * Desktop uses ScriptLoader/TCC for loaded state. Vita uses VitaModLoader's
 * native-module state, so this inventory remains available even though the
 * desktop scripting backend stays disabled there.
 */
#pragma once

#include <string>
#include <vector>

namespace ssb64::mods {

/* Lifecycle state of an installed mod, as the user should see it. */
enum class ModState {
    Loaded,          /* compiled + ModInit ran: live in the engine        */
    NotLoaded,       /* mounted/installed but not currently loaded         */
    Disabled,        /* installed but disabled by the user                  */
    InvalidManifest, /* mounted, but manifest.json is unnamed/unparseable  */
};

/* A single installed mod, flattened for display. Plain value type. */
struct ModInfo {
    std::string name;        /* manifest "name" (or a path-derived fallback) */
    std::string version;     /* manifest "version"                           */
    std::string author;      /* manifest "author"                            */
    std::string description; /* manifest "description"                       */
    std::string archivePath; /* absolute path of the mounted folder / .o2r   */
    bool enabled = true;      /* persisted Vita load preference                */
    ModState state = ModState::NotLoaded;
    std::string note; /* human-readable detail; set for InvalidManifest */
};

class ModRegistry {
  public:
    /*
     * Snapshot the currently-mounted mods. Pure query: no side effects,
     * never throws. Returns an empty vector if the resource/archive
     * subsystems are not up yet (e.g. called too early in boot).
     *
     * Result is ordered deterministically: loaded mods first, then
     * case-insensitively by name — so the list does not reshuffle between
     * frames when the menu re-renders.
     */
    static std::vector<ModInfo> Snapshot();
};

} // namespace ssb64::mods
