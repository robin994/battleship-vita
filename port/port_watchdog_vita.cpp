/**
 * port_watchdog_vita.cpp — Vita stub for port_watchdog.h.
 *
 * The desktop implementation (port_watchdog.cpp) dumps a signal-handler
 * backtrace via <sys/ucontext.h>, which VitaSDK doesn't provide. The
 * hang-detection watchdog is a dev diagnostic, not gameplay-critical, so
 * it's stubbed out here rather than ported - revisit if hangs need
 * debugging on-device.
 */

#include "port_watchdog.h"

void port_watchdog_init(void) {}
void port_watchdog_shutdown(void) {}

void port_watchdog_note_yield(void) {}
void port_watchdog_note_resume_start(int) {}
void port_watchdog_note_resume_end(int) {}
void port_watchdog_note_frame_end(void) {}

void port_dump_backtrace(void) {}
