#pragma once

#ifdef __vita__

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_set>

namespace Ship {
class IResource;
}

namespace ssb64::mods {

class VitaModLoader {
  public:
    static void LoadAll();
    static void UnloadAll();
    static void ReloadAll();
    static bool IsEnabled(const std::string &name);
    static void SetEnabled(const std::string &name, bool enabled);
    static std::unordered_set<std::string> LoadedNames();
    static std::shared_ptr<Ship::IResource> LoadOwnedResource(uint32_t handle,
                                                              const std::string &path);
};

} // namespace ssb64::mods

#endif
