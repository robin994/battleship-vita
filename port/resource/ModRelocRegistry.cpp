#include "ModRelocRegistry.h"

#include <algorithm>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace ssb64::mods {
namespace {

struct Entry {
    uint32_t owner = 0;
    const char *path = nullptr;
    uint32_t flags = 0;
};

std::mutex sMutex;
std::unordered_map<uint32_t, std::vector<Entry>> sByFileId;

/* Intern paths for process lifetime. References/pointers to unordered_set
 * elements survive rehash, and we never erase them, so lbReloc range records
 * can safely retain the returned c_str() after a mod unregisters. */
std::unordered_set<std::string> sPathPool;

const char *InternPath(const char *path) {
    auto [it, inserted] = sPathPool.emplace(path ? path : "");
    (void)inserted;
    return it->c_str();
}

} // namespace

bool RegisterModRelocResource(uint32_t owner, uint32_t fileId,
                              const char *path, uint32_t flags) {
    if (owner == 0 || !path || path[0] == '\0') {
        return false;
    }

    std::lock_guard<std::mutex> lock(sMutex);
    auto &stack = sByFileId[fileId];

    /* Re-registering from the same module is an update, not a second stack
     * frame. Move it to the top so normal last-registration-wins semantics
     * remain deterministic. */
    stack.erase(std::remove_if(stack.begin(), stack.end(),
                               [owner](const Entry &entry) { return entry.owner == owner; }),
                stack.end());
    stack.push_back({owner, InternPath(path), flags});
    return true;
}

bool UnregisterModRelocResource(uint32_t owner, uint32_t fileId) {
    std::lock_guard<std::mutex> lock(sMutex);
    auto found = sByFileId.find(fileId);
    if (found == sByFileId.end()) {
        return false;
    }

    auto &stack = found->second;
    const size_t oldSize = stack.size();
    stack.erase(std::remove_if(stack.begin(), stack.end(),
                               [owner](const Entry &entry) { return entry.owner == owner; }),
                stack.end());
    const bool changed = stack.size() != oldSize;
    if (stack.empty()) {
        sByFileId.erase(found);
    }
    return changed;
}

std::vector<uint32_t> UnregisterModRelocOwner(uint32_t owner) {
    std::vector<uint32_t> affected;
    std::lock_guard<std::mutex> lock(sMutex);

    for (auto it = sByFileId.begin(); it != sByFileId.end();) {
        auto &stack = it->second;
        const size_t oldSize = stack.size();
        stack.erase(std::remove_if(stack.begin(), stack.end(),
                                   [owner](const Entry &entry) { return entry.owner == owner; }),
                    stack.end());
        if (stack.size() != oldSize) {
            affected.push_back(it->first);
        }
        if (stack.empty()) {
            it = sByFileId.erase(it);
        } else {
            ++it;
        }
    }

    std::sort(affected.begin(), affected.end());
    return affected;
}

bool FindModRelocResource(uint32_t fileId, ModRelocResourceView *out) {
    std::lock_guard<std::mutex> lock(sMutex);
    const auto found = sByFileId.find(fileId);
    if (found == sByFileId.end() || found->second.empty()) {
        return false;
    }

    if (out) {
        const Entry &entry = found->second.back();
        out->owner = entry.owner;
        out->fileId = fileId;
        out->path = entry.path;
        out->flags = entry.flags;
    }
    return true;
}

} // namespace ssb64::mods
