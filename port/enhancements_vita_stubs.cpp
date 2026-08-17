/**
 * enhancements_vita_stubs.cpp — Vita stubs for the desktop-only enhancement
 * subsystems (self-updater, Discord Rich Presence, libretro shader-pack
 * downloader). Their .cpp files are excluded from the Vita build the same
 * way upstream already excludes them for Android (curl self-updater,
 * discord-rpc link, libretro shader downloader all make no sense on a
 * console target), but gameloop.cpp/PortMenu.cpp call into them
 * unconditionally rather than through an Android-style #ifdef, so the
 * declarations in enhancements.h need real (no-op) definitions here.
 */

#include "enhancements/enhancements.h"
#include "port_window_icon.h"

namespace ssb64 {

void SetWindowIcon() {}

namespace enhancements {

void TickDiscordPresence() {}

void CheckForUpdatesAsync(bool) {}
void StartGameUpdate() {}
bool IsUpdateAvailable() { return false; }
bool IsDownloading() { return false; }
bool IsDownloadComplete() { return false; }
bool IsCheckingForUpdates() { return false; }
std::string GetUpdateStatus() { return ""; }
std::string GetDownloadStatus() { return ""; }
std::string GetLatestVersion() { return ""; }

void FetchShaderPackCatalogAsync() {}
void InstallSelectedShaderPackAsync(const std::vector<std::string>&) {}
void CancelShaderPackFlow() {}
ShaderPackPhase GetShaderPackPhase() { return ShaderPackPhase::Idle; }
std::vector<ShaderPackCandidate> GetShaderPackCandidates() { return {}; }
std::string GetShaderPackStatus() { return ""; }

} // namespace enhancements
} // namespace ssb64
