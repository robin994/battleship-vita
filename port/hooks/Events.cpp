/*
 * Events.cpp — Allocates the storage for BattleShip's event IDs and
 * registers each event with libultraship's EventSystem at engine init.
 *
 * Allocation: this is the ONE translation unit that defines
 * INIT_EVENT_IDS before #include'ing Events.h. Every other file that
 * fires or listens for an event sees an extern declaration only.
 *
 * Registration: PortRegisterEvents() must be called ONCE after
 * sContext->InitEventSystem() succeeds. It allocates a runtime EventID
 * for each declared event so REGISTER_LISTENER / CALL_EVENT can dispatch.
 */
#define INIT_EVENT_IDS
#include "hooks/Events.h"

#include "libultraship/bridge/eventsbridge.h"

#include "port_log.h"

extern "C" void PortRegisterEvents(void) {
    port_log("SSB64: PortRegisterEvents — start\n");
    REGISTER_EVENT(GamePreUpdateEvent);
    port_log("SSB64:   GamePreUpdateEvent\n");
    REGISTER_EVENT(GamePostUpdateEvent);
    port_log("SSB64:   GamePostUpdateEvent\n");
    REGISTER_EVENT(DisplayPreUpdateEvent);
    port_log("SSB64:   DisplayPreUpdateEvent\n");
    REGISTER_EVENT(DisplayPostUpdateEvent);
    port_log("SSB64:   DisplayPostUpdateEvent\n");
    REGISTER_EVENT(EngineRenderMenubarEvent);
    port_log("SSB64:   EngineRenderMenubarEvent\n");
    REGISTER_EVENT(EngineRenderModsEvent);
    port_log("SSB64:   EngineRenderModsEvent\n");

    REGISTER_EVENT(FighterEnvColorQueryEvent);
    port_log("SSB64:   FighterEnvColorQueryEvent\n");
    REGISTER_EVENT(FighterDamageDirectionApplyEvent);
    port_log("SSB64:   FighterDamageDirectionApplyEvent\n");
    REGISTER_EVENT(FighterHitboxSlotResetEvent);
    port_log("SSB64:   FighterHitboxSlotResetEvent\n");
    REGISTER_EVENT(FighterParentKindResolveEvent);
    port_log("SSB64:   FighterParentKindResolveEvent\n");
    REGISTER_EVENT(FighterRapidJabStatusQueryEvent);
    port_log("SSB64:   FighterRapidJabStatusQueryEvent\n");

    REGISTER_EVENT(WindowFocusEvent);
    port_log("SSB64:   WindowFocusEvent\n");
    port_log("SSB64: PortRegisterEvents — done\n");
}
