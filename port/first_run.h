#pragma once

#include <string>

namespace ssb64 {

struct ExtractionResult {
    bool success = false;
    std::string outputPath;
    std::string error;
    std::string logPath;
};

/* Ensure BattleShip.o2r exists at `target_o2r_path`. If it doesn't, locate a
 * ROM (.z64 / .n64 / .v64), config.yml + yamls/us, and invoke the standalone
 * Torch executable to build the archive. */
// silent=true suppresses native SDL_ShowSimpleMessageBox popups. The
// ImGui wizard sets this so failures land in the wizard's own status
// line instead of an alien-looking second dialog.
ExtractionResult ExtractAssetsIfNeeded(const std::string& target_o2r_path, bool silent = false,
                                       const std::string& romOverridePath = {});

/* Drive an ImGui first-run wizard until either:
 *   - the user provides a ROM and extraction succeeds (returns true,
 *     target_o2r_path now exists), or
 *   - the user closes the window / picks Quit (returns false, caller
 *     should exit).
 *
 * Requires Window/Gui already initialized. Spins its own pre-gameloop
 * render loop using gui->StartDraw() / window->RunGuiOnly() / gui->EndDraw().
 *
 * lastAttempt: the result of the ExtractAssetsIfNeeded() call the caller
 * already made before deciding a wizard/screen is needed. Vita has no
 * Browse/Retry UI (see RunFirstRunWizard's __vita__ branch) - it just
 * displays lastAttempt.error verbatim and waits for Cross to exit, so the
 * caller's diagnosis (missing ROM vs. a real extraction failure) reaches
 * the user instead of a generic message.
 */
bool RunFirstRunWizard(const std::string& target_o2r_path,
                       const ExtractionResult& lastAttempt = {});

} // namespace ssb64
