#include "focus.h"

#include "libultraship/bridge/consolevariablebridge.h"
#include "hooks/Events.h"
#include "port_log.h"

namespace ssb64 {

static float sSavedMasterVolume = -1.0f;

static void OnWindowFocus(IEvent* raw) {
    auto* ev = reinterpret_cast<WindowFocusEvent*>(raw);

    if (!ev->Focused) {
        if (sSavedMasterVolume < 0.0f && CVarGetInteger("gSettings.FocusControl.MuteOnFocusLoss", 0)) {
            sSavedMasterVolume = CVarGetFloat("gSettings.Audio.MasterVolume", 1.0f);
            CVarSetFloat("gSettings.Audio.MasterVolume", 0.0f);
        }
    } else {
        if (sSavedMasterVolume >= 0.0f) {
            CVarSetFloat("gSettings.Audio.MasterVolume", sSavedMasterVolume);
            sSavedMasterVolume = -1.0f;
        }
    }
}

void RegisterFocusListener() {
    port_log("SSB64: RegisterFocusListener — start\n");
    REGISTER_LISTENER(WindowFocusEvent, EVENT_PRIORITY_NORMAL, OnWindowFocus);
    port_log("SSB64: RegisterFocusListener — done\n");
}

} // namespace ssb64
