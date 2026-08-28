#ifdef __vita__

#include "VitaModLoader.h"

#include "../fighter_registry.h"
#include "../port_log.h"
#include "../../include/battleship/vita_mod_api.h"
#include "../resource/ModRelocRegistry.h"
#include "../resource/RelocPointerTable.h"
#include "../audio/mod_audio.h"

#include "bridge/ftmodapi_bridge.h"

#include <libultraship/libultraship.h>
#include <libultraship/bridge/consolevariablebridge.h>
#include <ship/resource/File.h>
#include <ship/resource/ResourceManager.h>
#include <ship/resource/archive/Archive.h>
#include <ship/resource/archive/ArchiveManager.h>

#include <psp2/kernel/modulemgr.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

extern "C" void portRelocLoadFileFromBytesPrivate(
    void *ram_dst, unsigned int dst_size, const void *src_bytes,
    unsigned int src_size, unsigned short reloc_intern_offset);
extern "C" void portRelocEvictFileId(unsigned int file_id);

namespace ssb64::mods {
namespace {

struct FighterRollback {
    int fkind = -1;
    bool hadPrevious = false;
    FighterDescriptor previous{};
    BattleShipVitaModHandle previousOwner = 0;
};

struct RelocRange {
    void *base = nullptr;
    uint32_t size = 0;
};

struct LoadedVitaMod {
    BattleShipVitaModHandle handle = 0;
    std::string name;
    std::string cachePath;
    std::shared_ptr<Ship::Archive> archive;
    SceUID moduleId = -1;
    BattleShipVitaModContext context{};
    std::vector<FighterRollback> fighterRollbacks;
    std::vector<uint32_t> relocTokens;
    std::vector<RelocRange> relocRanges;
    bool unloading = false;
};

std::recursive_mutex sMutex;
std::vector<std::unique_ptr<LoadedVitaMod>> sLoaded;
std::unordered_map<int, BattleShipVitaModHandle> sFighterOwners;
BattleShipVitaModHandle sNextHandle = 1;

LoadedVitaMod *FindByHandle(BattleShipVitaModHandle handle) {
    for (auto &mod : sLoaded) {
        if (mod && mod->handle == handle) {
            return mod.get();
        }
    }
    return nullptr;
}

void ApiLog(BattleShipVitaModHandle handle, const char *message) {
    std::lock_guard<std::recursive_mutex> lock(sMutex);
    LoadedVitaMod *mod = FindByHandle(handle);
    port_log("[vita-mod:%s] %s\n",
             mod ? mod->name.c_str() : "unknown",
             message ? message : "");
}

int ApiAssetSize(BattleShipVitaModHandle handle, const char *path, uint32_t *outSize) {
    if (!path || !outSize) {
        return -1;
    }
    std::lock_guard<std::recursive_mutex> lock(sMutex);
    LoadedVitaMod *mod = FindByHandle(handle);
    if (!mod || !mod->archive) {
        return -2;
    }
    auto file = mod->archive->LoadFile(path);
    if (!file || !file->IsLoaded || !file->Buffer) {
        return -3;
    }
    *outSize = static_cast<uint32_t>(file->Buffer->size());
    return 0;
}

int ApiAssetRead(BattleShipVitaModHandle handle, const char *path,
                 void *dst, uint32_t dstSize, uint32_t *outSize) {
    if (!path || !dst) {
        return -1;
    }
    std::lock_guard<std::recursive_mutex> lock(sMutex);
    LoadedVitaMod *mod = FindByHandle(handle);
    if (!mod || !mod->archive) {
        return -2;
    }
    auto file = mod->archive->LoadFile(path);
    if (!file || !file->IsLoaded || !file->Buffer) {
        return -3;
    }
    const size_t size = file->Buffer->size();
    if (size > dstSize) {
        if (outSize) {
            *outSize = static_cast<uint32_t>(size);
        }
        return -4;
    }
    std::memcpy(dst, file->Buffer->data(), size);
    if (outSize) {
        *outSize = static_cast<uint32_t>(size);
    }
    return 0;
}

auto FindFighterRollback(LoadedVitaMod *mod, int fkind) {
    return std::find_if(mod->fighterRollbacks.begin(), mod->fighterRollbacks.end(),
                        [fkind](const FighterRollback &entry) { return entry.fkind == fkind; });
}

void RestoreFighter(LoadedVitaMod *mod, std::vector<FighterRollback>::iterator rollback) {
    if (!mod || rollback == mod->fighterRollbacks.end()) {
        return;
    }

    if (rollback->hadPrevious) {
        port_fighter_register(rollback->fkind, &rollback->previous);
    } else {
        port_fighter_unregister(rollback->fkind);
    }

    if (rollback->previousOwner != 0) {
        sFighterOwners[rollback->fkind] = rollback->previousOwner;
    } else {
        sFighterOwners.erase(rollback->fkind);
    }
    mod->fighterRollbacks.erase(rollback);
}

void RestoreAllFighters(LoadedVitaMod *mod) {
    if (!mod) {
        return;
    }
    while (!mod->fighterRollbacks.empty()) {
        auto rollback = std::prev(mod->fighterRollbacks.end());
        const auto owner = sFighterOwners.find(rollback->fkind);
        if (owner != sFighterOwners.end() && owner->second != mod->handle) {
            /* This should only be possible if callers unload modules out of
             * dependency/reverse-load order. Do not clobber a newer owner. */
            port_log("SSB64: Vita mod '%s' cannot restore fkind=%d: owned by handle=%u\n",
                     mod->name.c_str(), rollback->fkind, owner->second);
            mod->fighterRollbacks.erase(rollback);
            continue;
        }
        RestoreFighter(mod, rollback);
    }
}

int ApiFighterRegister(BattleShipVitaModHandle handle, int fkind,
                       const FighterDescriptor *descriptor) {
    if (fkind < 0 || !descriptor) {
        return -1;
    }
    std::lock_guard<std::recursive_mutex> lock(sMutex);
    LoadedVitaMod *mod = FindByHandle(handle);
    if (!mod) {
        return -2;
    }

    auto rollback = FindFighterRollback(mod, fkind);
    if (rollback == mod->fighterRollbacks.end()) {
        FighterRollback entry;
        entry.fkind = fkind;
        if (const FighterDescriptor *previous = port_fighter_descriptor(fkind)) {
            entry.hadPrevious = true;
            entry.previous = *previous;
        }
        if (const auto owner = sFighterOwners.find(fkind); owner != sFighterOwners.end()) {
            entry.previousOwner = owner->second;
        }
        mod->fighterRollbacks.push_back(entry);
    }

    port_fighter_register(fkind, descriptor);
    sFighterOwners[fkind] = handle;
    return 0;
}

int ApiFighterUnregister(BattleShipVitaModHandle handle, int fkind) {
    std::lock_guard<std::recursive_mutex> lock(sMutex);
    LoadedVitaMod *mod = FindByHandle(handle);
    if (!mod) {
        return -1;
    }
    auto rollback = FindFighterRollback(mod, fkind);
    if (rollback == mod->fighterRollbacks.end()) {
        return -2;
    }
    const auto owner = sFighterOwners.find(fkind);
    if (owner == sFighterOwners.end() || owner->second != handle) {
        return -3;
    }
    RestoreFighter(mod, rollback);
    return 0;
}

void ApiRelocLoadPrivate(BattleShipVitaModHandle handle,
                         void *ramDst, uint32_t dstSize,
                         const void *srcBytes, uint32_t srcSize,
                         uint16_t relocInternOffset) {
    if (!ramDst || dstSize == 0 || !srcBytes || srcSize == 0) {
        return;
    }
    std::lock_guard<std::recursive_mutex> lock(sMutex);
    LoadedVitaMod *mod = FindByHandle(handle);
    if (!mod) {
        return;
    }

    portRelocLoadFileFromBytesPrivate(ramDst, dstSize, srcBytes, srcSize, relocInternOffset);

    const auto duplicate = std::find_if(mod->relocRanges.begin(), mod->relocRanges.end(),
        [&](const RelocRange &range) { return range.base == ramDst && range.size == dstSize; });
    if (duplicate == mod->relocRanges.end()) {
        mod->relocRanges.push_back({ramDst, dstSize});
    }
}

uint32_t ApiRelocRegisterPointer(BattleShipVitaModHandle handle, void *ptr) {
    if (!ptr) {
        return 0;
    }
    std::lock_guard<std::recursive_mutex> lock(sMutex);
    LoadedVitaMod *mod = FindByHandle(handle);
    if (!mod) {
        return 0;
    }
    const uint32_t token = portRelocRegisterPointer(ptr);
    if (token != 0) {
        mod->relocTokens.push_back(token);
    }
    return token;
}

int ApiRelocUnregisterPointer(BattleShipVitaModHandle handle, uint32_t token) {
    std::lock_guard<std::recursive_mutex> lock(sMutex);
    LoadedVitaMod *mod = FindByHandle(handle);
    if (!mod) {
        return -1;
    }
    const auto it = std::find(mod->relocTokens.begin(), mod->relocTokens.end(), token);
    if (it == mod->relocTokens.end()) {
        return -2;
    }
    const int invalidated = portRelocUnregisterPointer(token);
    mod->relocTokens.erase(it);
    return invalidated ? 0 : -3;
}

int ApiRelocFilesRegister(BattleShipVitaModHandle handle,
                          const BattleShipVitaModRelocResource *resources,
                          uint32_t count) {
    if ((!resources && count != 0) || count > 8192u) {
        return -1;
    }

    std::lock_guard<std::recursive_mutex> lock(sMutex);
    LoadedVitaMod *mod = FindByHandle(handle);
    if (!mod || !mod->archive || mod->unloading) {
        return -2;
    }

    /* Fail atomically before touching the provider stack. This catches bad
     * generated paths at module_start instead of much later in a match. */
    for (uint32_t i = 0; i < count; ++i) {
        const auto &resource = resources[i];
        if (!resource.resource_path || resource.resource_path[0] == '\0') {
            return -3;
        }
        if (!mod->archive->HasFile(resource.resource_path)) {
            port_log("SSB64: Vita mod '%s' reloc registration missing file_id=%u path=%s\n",
                     mod->name.c_str(), resource.file_id, resource.resource_path);
            return -4;
        }
    }

    for (uint32_t i = 0; i < count; ++i) {
        const auto &resource = resources[i];
        if (!RegisterModRelocResource(handle, resource.file_id,
                                      resource.resource_path, resource.flags)) {
            /* Inputs were already validated; reaching this is a registry
             * invariant failure. Remove this owner's entire partial table. */
            const auto affected = UnregisterModRelocOwner(handle);
            for (uint32_t fileId : affected) {
                portRelocEvictFileId(fileId);
            }
            return -5;
        }
        portRelocEvictFileId(resource.file_id);
    }

    port_log("SSB64: Vita mod '%s' registered %u reloc resources\n",
             mod->name.c_str(), count);
    return 0;
}

int ApiRelocFileUnregister(BattleShipVitaModHandle handle, uint32_t fileId) {
    std::lock_guard<std::recursive_mutex> lock(sMutex);
    LoadedVitaMod *mod = FindByHandle(handle);
    if (!mod) {
        return -1;
    }
    if (!UnregisterModRelocResource(handle, fileId)) {
        return -2;
    }
    portRelocEvictFileId(fileId);
    return 0;
}

int ApiAudioFgmRegister(BattleShipVitaModHandle handle,
                        const BattleShipVitaModAudioResource *resources,
                        uint32_t count) {
    if ((!resources && count != 0) || count > 512u) return -1;

    std::lock_guard<std::recursive_mutex> lock(sMutex);
    LoadedVitaMod *mod = FindByHandle(handle);
    if (!mod || !mod->archive || mod->unloading) return -2;

    for (uint32_t i = 0; i < count; ++i) {
        const auto &resource = resources[i];
        if (!resource.resource_path || resource.resource_path[0] == '\0' ||
            resource.sample_rate != 32000u ||
            (resource.channels != 1u && resource.channels != 2u) ||
            !mod->archive->HasFile(resource.resource_path)) {
            return -3;
        }
        auto file = mod->archive->LoadFile(resource.resource_path);
        if (!file || !file->IsLoaded || !file->Buffer || file->Buffer->empty() ||
            ((file->Buffer->size() & 1u) != 0)) {
            return -4;
        }
    }

    for (uint32_t i = 0; i < count; ++i) {
        const auto &resource = resources[i];
        auto file = mod->archive->LoadFile(resource.resource_path);
        if (portModAudioRegisterPCM(handle, &resource,
                                    file->Buffer->data(), file->Buffer->size()) != 0) {
            portModAudioUnregisterOwner(handle);
            return -5;
        }
    }
    port_log("SSB64: Vita mod '%s' registered %u PCM FGMs\n",
             mod->name.c_str(), count);
    return 0;
}

int ApiAudioFgmUnregister(BattleShipVitaModHandle handle, uint16_t fgmId) {
    std::lock_guard<std::recursive_mutex> lock(sMutex);
    if (FindByHandle(handle) == nullptr) return -1;
    return portModAudioUnregisterFGM(handle, fgmId);
}

int ApiFighterSetStatus(void *fighterGObj, int statusId,
                        float frameBegin, float animSpeed, uint32_t preserveFlags) {
    return port_mod_fighter_set_status(fighterGObj, statusId, frameBegin, animSpeed, preserveFlags);
}

void ApiFighterPlayAnimEvents(void *fighterGObj) {
    port_mod_fighter_play_anim_events(fighterGObj);
}

void ApiFighterSetAnimSpeed(void *fighterGObj, float animSpeed) {
    port_mod_fighter_set_anim_speed(fighterGObj, animSpeed);
}

void ApiFighterSetGround(void *fighterGObj) {
    port_mod_fighter_set_ground(fighterGObj);
}

void ApiFighterSetAir(void *fighterGObj) {
    port_mod_fighter_set_air(fighterGObj);
}

void ApiFighterSetWaitOrFall(void *fighterGObj) {
    port_mod_fighter_set_wait_or_fall(fighterGObj);
}

int ApiFighterCheckLanding(void *fighterGObj) {
    return port_mod_fighter_check_landing(fighterGObj);
}

int ApiFighterJointWorldPosition(void *fighterGObj, int jointId,
                                 float *outX, float *outY, float *outZ) {
    return port_mod_fighter_joint_world_position(fighterGObj, jointId, outX, outY, outZ);
}

int ApiProjectileSpawnBuiltin(BattleShipVitaModHandle handle, uint32_t projectileKind,
                              void *ownerGObj,
                              float posX, float posY, float posZ,
                              float velX, float velY, float velZ) {
    if (!ownerGObj) return -1;
    {
        std::lock_guard<std::recursive_mutex> lock(sMutex);
        if (FindByHandle(handle) == nullptr) return -2;
    }
    if (projectileKind != BATTLESHIP_VITA_BUILTIN_PROJECTILE_FFLOWER_FLAME) return -3;
    return port_mod_projectile_spawn_fflower(ownerGObj, posX, posY, posZ, velX, velY, velZ);
}

void UnregisterOwnedRelocFiles(LoadedVitaMod *mod) {
    if (!mod) {
        return;
    }
    const auto affected = UnregisterModRelocOwner(mod->handle);
    for (uint32_t fileId : affected) {
        portRelocEvictFileId(fileId);
    }
}

void InvalidateOwnedRelocs(LoadedVitaMod *mod) {
    if (!mod) {
        return;
    }
    for (const uint32_t token : mod->relocTokens) {
        portRelocUnregisterPointer(token);
    }
    mod->relocTokens.clear();
    for (const RelocRange &range : mod->relocRanges) {
        portRelocInvalidateRange(range.base, range.size);
    }
    mod->relocRanges.clear();
}

const BattleShipVitaModAPI kApi = {
    BATTLESHIP_VITA_MOD_ABI_VERSION,
    sizeof(BattleShipVitaModAPI),
    ApiLog,
    ApiAssetSize,
    ApiAssetRead,
    ApiFighterRegister,
    ApiFighterUnregister,
    port_fighter_descriptor,
    ApiRelocLoadPrivate,
    ApiRelocRegisterPointer,
    ApiRelocUnregisterPointer,
    portRelocResolvePointer,
    ApiRelocFilesRegister,
    ApiRelocFileUnregister,
    ApiAudioFgmRegister,
    ApiAudioFgmUnregister,
    ApiFighterSetStatus,
    ApiFighterPlayAnimEvents,
    ApiFighterSetAnimSpeed,
    ApiFighterSetGround,
    ApiFighterSetAir,
    ApiFighterSetWaitOrFall,
    ApiFighterCheckLanding,
    ApiFighterJointWorldPosition,
    ApiProjectileSpawnBuiltin,
};

std::string SanitizeFilename(const std::string &name) {
    std::string out;
    out.reserve(name.size());
    for (unsigned char c : name) {
        if (std::isalnum(c) || c == '-' || c == '_') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('_');
        }
    }
    return out.empty() ? "mod" : out;
}

uint32_t HashModName(const std::string &name) {
    uint32_t hash = 2166136261u;
    for (unsigned char c : name) {
        hash ^= c;
        hash *= 16777619u;
    }
    return hash;
}

std::string EnabledCVarKey(const std::string &name) {
    return "gVitaMods.Enabled." + SanitizeFilename(name) + "." + std::to_string(HashModName(name));
}

bool IsModEnabledPreference(const std::string &name) {
    const std::string key = EnabledCVarKey(name);
    return CVarGetInteger(key.c_str(), 1) != 0;
}

bool WriteModuleToCache(const std::shared_ptr<Ship::Archive> &archive,
                        const std::string &binaryPath,
                        const std::string &cachePath) {
    auto file = archive->LoadFile(binaryPath);
    if (!file || !file->IsLoaded || !file->Buffer || file->Buffer->empty()) {
        return false;
    }

    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(cachePath).parent_path(), ec);
    if (ec) {
        return false;
    }

    std::ofstream out(cachePath, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        return false;
    }
    out.write(file->Buffer->data(), static_cast<std::streamsize>(file->Buffer->size()));
    return out.good();
}

void LoadArchive(const std::shared_ptr<Ship::Archive> &archive) {
    if (!archive || !archive->HasFile("manifest.json")) {
        return;
    }

    const auto &manifest = archive->GetManifest();
    if (manifest.Name.empty()) {
        return;
    }
    if (!IsModEnabledPreference(manifest.Name)) {
        port_log("SSB64: Vita mod disabled -> %s\n", manifest.Name.c_str());
        return;
    }
    {
        std::lock_guard<std::recursive_mutex> lock(sMutex);
        if (std::any_of(sLoaded.begin(), sLoaded.end(), [&](const auto &entry) {
                return entry && entry->name == manifest.Name;
            })) {
            return;
        }
    }

    auto binary = manifest.Binaries.find("vita");
    if (binary == manifest.Binaries.end()) {
        binary = manifest.Binaries.find("vita_armv7");
    }
    if (binary == manifest.Binaries.end() || binary->second.empty()) {
        port_log("SSB64: Vita mod '%s' mounted but has no binaries.vita entry\n",
                 manifest.Name.c_str());
        return;
    }

    auto mod = std::make_unique<LoadedVitaMod>();
    mod->name = manifest.Name;
    mod->archive = archive;
    mod->cachePath = Ship::Context::GetPathRelativeToAppDirectory(
        "mod_cache/" + SanitizeFilename(manifest.Name) + ".suprx");
    if (!WriteModuleToCache(archive, binary->second, mod->cachePath)) {
        port_log("SSB64: Vita mod '%s' failed to extract '%s'\n",
                 manifest.Name.c_str(), binary->second.c_str());
        return;
    }

    LoadedVitaMod *raw = nullptr;
    BattleShipVitaModHandle handle = 0;
    {
        std::lock_guard<std::recursive_mutex> lock(sMutex);
        /* A rescan may have raced the extraction step. Recheck before the
         * module becomes visible to ABI callbacks. */
        if (std::any_of(sLoaded.begin(), sLoaded.end(), [&](const auto &entry) {
                return entry && entry->name == manifest.Name;
            })) {
            return;
        }
        mod->handle = sNextHandle++;
        mod->context.magic = BATTLESHIP_VITA_MOD_MAGIC;
        mod->context.abi_version = BATTLESHIP_VITA_MOD_ABI_VERSION;
        mod->context.struct_size = sizeof(BattleShipVitaModContext);
        mod->context.mod_handle = mod->handle;
        mod->context.api = &kApi;
        handle = mod->handle;
        raw = mod.get();
        sLoaded.push_back(std::move(mod));
    }

    int startStatus = 0;
    /* Do not hold sMutex here. module_start is third-party code and is
     * expected to call back into the ABI, whose entrypoints take sMutex. */
    const SceUID moduleId = sceKernelLoadStartModule(
        raw->cachePath.c_str(), sizeof(raw->context), &raw->context,
        0, nullptr, &startStatus);
    std::lock_guard<std::recursive_mutex> lock(sMutex);
    raw = FindByHandle(handle);
    if (!raw) {
        return;
    }
    if (moduleId < 0 || startStatus < 0) {
        port_log("SSB64: Vita mod '%s' load failed module=0x%08x start=0x%08x\n",
                 raw->name.c_str(), static_cast<unsigned int>(moduleId),
                 static_cast<unsigned int>(startStatus));
        if (moduleId >= 0) {
            const int unloadRc = sceKernelUnloadModule(moduleId, 0, nullptr);
            if (unloadRc < 0) {
                port_log("SSB64: Vita mod '%s' failed-start cleanup could not unload uid=0x%08x rc=0x%08x\n",
                         raw->name.c_str(), static_cast<unsigned int>(moduleId),
                         static_cast<unsigned int>(unloadRc));
            }
        }
        RestoreAllFighters(raw);
        UnregisterOwnedRelocFiles(raw);
        portModAudioUnregisterOwner(raw->handle);
        InvalidateOwnedRelocs(raw);
        const auto it = std::find_if(sLoaded.begin(), sLoaded.end(),
            [handle](const auto &entry) { return entry && entry->handle == handle; });
        if (it != sLoaded.end()) {
            sLoaded.erase(it);
        }
        return;
    }

    raw->moduleId = moduleId;
    port_log("SSB64: Vita mod loaded -> %s (uid=0x%08x)\n",
             raw->name.c_str(), static_cast<unsigned int>(raw->moduleId));
}

} // namespace

void VitaModLoader::LoadAll() {
    auto context = Ship::Context::GetInstance();
    if (!context || !context->GetResourceManager()) {
        return;
    }
    auto manager = context->GetResourceManager()->GetArchiveManager();
    if (!manager) {
        return;
    }
    auto archives = manager->GetArchives();
    if (!archives) {
        return;
    }
    for (const auto &archive : *archives) {
        LoadArchive(archive);
    }
}

void VitaModLoader::UnloadAll() {
    std::vector<BattleShipVitaModHandle> handles;
    {
        std::lock_guard<std::recursive_mutex> lock(sMutex);
        for (size_t i = sLoaded.size(); i-- > 0;) {
            LoadedVitaMod *mod = sLoaded[i].get();
            if (!mod || mod->moduleId < 0 || mod->unloading) {
                continue;
            }
            mod->unloading = true;
            handles.push_back(mod->handle);
        }
    }

    for (const BattleShipVitaModHandle handle : handles) {
        SceUID moduleId = -1;
        BattleShipVitaModContext *context = nullptr;
        std::string name;
        {
            std::lock_guard<std::recursive_mutex> lock(sMutex);
            LoadedVitaMod *mod = FindByHandle(handle);
            if (!mod) {
                continue;
            }
            moduleId = mod->moduleId;
            context = &mod->context;
            name = mod->name;
        }

        int stopStatus = 0;
        /* Same rule as module_start: module_stop may invoke ABI calls. */
        const int rc = sceKernelStopUnloadModule(
            moduleId, sizeof(*context), context,
            0, nullptr, &stopStatus);
        port_log("SSB64: Vita mod unload -> %s rc=0x%08x stop=0x%08x\n",
                 name.c_str(), static_cast<unsigned int>(rc),
                 static_cast<unsigned int>(stopStatus));

        std::lock_guard<std::recursive_mutex> lock(sMutex);
        LoadedVitaMod *mod = FindByHandle(handle);
        if (!mod) {
            continue;
        }
        if (rc >= 0) {
            RestoreAllFighters(mod);
            UnregisterOwnedRelocFiles(mod);
            portModAudioUnregisterOwner(mod->handle);
            InvalidateOwnedRelocs(mod);
            const auto it = std::find_if(sLoaded.begin(), sLoaded.end(),
                [handle](const auto &entry) { return entry && entry->handle == handle; });
            if (it != sLoaded.end()) {
                sLoaded.erase(it);
            }
        } else {
            mod->unloading = false;
        }
    }
}

void VitaModLoader::ReloadAll() {
    UnloadAll();
    LoadAll();
}

bool VitaModLoader::IsEnabled(const std::string &name) {
    return IsModEnabledPreference(name);
}

void VitaModLoader::SetEnabled(const std::string &name, bool enabled) {
    if (name.empty()) {
        return;
    }
    const std::string key = EnabledCVarKey(name);
    CVarSetInteger(key.c_str(), enabled ? 1 : 0);
    CVarSave();
    port_log("SSB64: Vita mod preference -> %s enabled=%d\n",
             name.c_str(), enabled ? 1 : 0);
}

std::unordered_set<std::string> VitaModLoader::LoadedNames() {
    std::lock_guard<std::recursive_mutex> lock(sMutex);
    std::unordered_set<std::string> result;
    for (const auto &mod : sLoaded) {
        if (mod && mod->moduleId >= 0) {
            result.insert(mod->name);
        }
    }
    return result;
}

std::shared_ptr<Ship::IResource> VitaModLoader::LoadOwnedResource(uint32_t handle,
                                                                  const std::string &path) {
    std::shared_ptr<Ship::Archive> archive;
    {
        std::lock_guard<std::recursive_mutex> lock(sMutex);
        LoadedVitaMod *mod = FindByHandle(handle);
        if (!mod || !mod->archive || mod->unloading) {
            return nullptr;
        }
        archive = mod->archive;
    }

    auto context = Ship::Context::GetInstance();
    if (!context || !context->GetResourceManager() || !archive->HasFile(path)) {
        return nullptr;
    }

    /* Native mod reloc providers must resolve in the archive that registered
     * them. A global path lookup is ambiguous once several mods are mounted,
     * and can also miss archives added after the base ResourceManager was
     * initialized. ResourceIdentifier keeps both cache ownership and archive
     * routing explicit. */
    Ship::ResourceIdentifier identifier(path, static_cast<uintptr_t>(handle), archive);
    return context->GetResourceManager()->LoadResource(identifier, true);
}

} // namespace ssb64::mods

#endif /* __vita__ */
