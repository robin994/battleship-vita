#pragma once

#include <cstdint>
#include <vector>

namespace ssb64::mods {

enum : uint32_t {
    MOD_RELOC_FIGHTER_FIGATREE = 1u << 0,
};

struct ModRelocResourceView {
    uint32_t owner = 0;
    uint32_t fileId = 0;
    const char *path = nullptr;
    uint32_t flags = 0;
};

/* Last registration wins. Registrations are stacked by file id so unloading
 * an overriding mod exposes the previous owner's resource automatically. */
bool RegisterModRelocResource(uint32_t owner, uint32_t fileId,
                              const char *path, uint32_t flags);
bool UnregisterModRelocResource(uint32_t owner, uint32_t fileId);

/* Removes every row owned by `owner` and returns the affected file ids. The
 * caller uses that list to evict already-published lbReloc/status-buffer data. */
std::vector<uint32_t> UnregisterModRelocOwner(uint32_t owner);

bool FindModRelocResource(uint32_t fileId, ModRelocResourceView *out);

} // namespace ssb64::mods
