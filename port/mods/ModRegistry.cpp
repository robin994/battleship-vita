/*
 * ModRegistry.cpp — see ModRegistry.h.
 *
 * Walks the archives the engine already mounted (ArchiveManager) rather
 * than re-scanning the filesystem: that way a "mod" is whatever the engine
 * actually has, manifest parsing is reused from the Archive layer
 * (GetManifest), and per-archive reads avoid the VFS path collision you'd
 * hit reading "manifest.json" globally when several mods each ship one.
 */
#include "ModRegistry.h"

#include <libultraship/libultraship.h> // Ship::Context
#include <ship/resource/ResourceManager.h>
#include <ship/resource/archive/ArchiveManager.h>
#include <ship/resource/archive/Archive.h> // Archive, ArchiveManifest, GetManifest
#ifndef DISABLE_SCRIPTING
#include <ship/scripting/ScriptLoader.h>   // GetLoadersInDependencyOrder
#endif
#ifdef __vita__
#include "VitaModLoader.h"
#endif

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <unordered_set>
#include <utility>

namespace ssb64::mods {
namespace {

bool CaseInsensitiveLess(const std::string& a, const std::string& b) {
    return std::lexicographical_compare(
        a.begin(), a.end(), b.begin(), b.end(),
        [](unsigned char x, unsigned char y) { return std::tolower(x) < std::tolower(y); });
}

/* Folder name, or archive filename without extension — a readable fallback
 * label when a mod's manifest carries no usable "name". */
std::string PathLabel(const std::string& archivePath) {
    const std::filesystem::path p(archivePath);
    std::string stem = p.stem().string();
    if (!stem.empty()) {
        return stem;
    }
    std::string fname = p.filename().string();
    return fname.empty() ? archivePath : fname;
}

} // namespace

std::vector<ModInfo> ModRegistry::Snapshot() {
    std::vector<ModInfo> mods;

    auto ctx = Ship::Context::GetInstance();
    if (!ctx) {
        return mods;
    }
    auto resourceManager = ctx->GetResourceManager();
    if (!resourceManager) {
        return mods;
    }
    auto archiveManager = resourceManager->GetArchiveManager();
    if (!archiveManager) {
        return mods;
    }
    auto archives = archiveManager->GetArchives();
    if (!archives) {
        return mods;
    }

    /* Names of mods that compiled and ran ModInit successfully. Keyed by
     * manifest name — the same key ScriptLoader uses internally. If the
     * loader is not up, every mod simply reports NotLoaded. */
    std::unordered_set<std::string> loaded;
#ifdef __vita__
    loaded = VitaModLoader::LoadedNames();
#elif !defined(DISABLE_SCRIPTING)
    if (auto scripting = ctx->GetScriptLoader()) {
        for (const auto& name : scripting->GetLoadersInDependencyOrder()) {
            loaded.insert(name);
        }
    }
#endif

    for (const auto& archive : *archives) {
        if (!archive) {
            continue;
        }
        /* A mounted archive is a managed mod iff it carries a manifest.json.
         * Excludes the base game + shader archives, which carry none. */
        if (!archive->HasFile("manifest.json")) {
            continue;
        }

        const Ship::ArchiveManifest& manifest = archive->GetManifest();

        ModInfo info;
        info.archivePath = archive->GetPath();
        info.version = manifest.Version;
        info.author = manifest.Author;
        info.description = manifest.Description;

        if (manifest.Name.empty()) {
            /* manifest.json exists but parsed to no name: malformed JSON or
             * a missing "name". Surface it instead of hiding it. */
            info.name = PathLabel(info.archivePath);
            info.state = ModState::InvalidManifest;
            info.note = "manifest.json is missing a \"name\" or could not be parsed";
        } else {
            info.name = manifest.Name;
#ifdef __vita__
            info.enabled = VitaModLoader::IsEnabled(manifest.Name);
            if (loaded.count(manifest.Name)) {
                info.state = ModState::Loaded;
                if (!info.enabled) {
                    info.note = "disable pending - press Apply / Reload or restart";
                }
            } else if (!info.enabled) {
                info.state = ModState::Disabled;
            } else {
                info.state = ModState::NotLoaded;
            }
            if (info.state == ModState::NotLoaded &&
                !manifest.Binaries.contains("vita") &&
                !manifest.Binaries.contains("vita_armv7")) {
                info.note = "no Vita native binary (manifest binaries.vita)";
            }
#else
            info.state = loaded.count(manifest.Name) ? ModState::Loaded : ModState::NotLoaded;
#endif
        }

        mods.push_back(std::move(info));
    }

    /* Loaded mods first, then case-insensitive by name — stable across
     * frames so the rendered list does not jump around. */
    std::sort(mods.begin(), mods.end(), [](const ModInfo& a, const ModInfo& b) {
        const int rankA = (a.state == ModState::Loaded) ? 0 :
                          (a.state == ModState::NotLoaded) ? 1 :
                          (a.state == ModState::Disabled) ? 2 : 3;
        const int rankB = (b.state == ModState::Loaded) ? 0 :
                          (b.state == ModState::NotLoaded) ? 1 :
                          (b.state == ModState::Disabled) ? 2 : 3;
        if (rankA != rankB) {
            return rankA < rankB;
        }
        return CaseInsensitiveLess(a.name, b.name);
    });

    return mods;
}

} // namespace ssb64::mods
