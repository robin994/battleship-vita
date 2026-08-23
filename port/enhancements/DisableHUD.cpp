#include "enhancements.h"

#include <libultraship/bridge/consolevariablebridge.h>

#ifdef __vita__
extern "C" void port_log(const char* fmt, ...);
#endif

constexpr const char* kDisableHUDCVar = "gEnhancements.DisableHUD";

extern "C" {
    bool port_enhancement_is_hud_disabled(void) {
        const bool disabled = CVarGetInteger(kDisableHUDCVar, 0) != 0;
#ifdef __vita__
        static bool logged = false;
        if (!logged) {
            logged = true;
            port_log("SSB64: HUD_CVAR disabled=%s key=%s\n", disabled ? "yes" : "no", kDisableHUDCVar);
        }
#endif
        return disabled;
    }
}

namespace ssb64 {
    namespace enhancements {

        const char* DisableHUDCVarName() {
            return kDisableHUDCVar;
        }

    } // namespace enhancements
} // namespace ssb64
