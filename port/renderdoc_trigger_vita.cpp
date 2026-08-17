/**
 * renderdoc_trigger_vita.cpp — Vita stub for renderdoc_trigger.h.
 *
 * RenderDoc is a desktop GPU-capture tool (renderdoc_app.h has no Vita
 * platform branch, and no RenderDoc client can attach to a Vita process
 * anyway) - stubbed out rather than ported.
 */

#include "renderdoc_trigger.h"

void portRenderDocInit(void) {}
void portRenderDocOnFrame(unsigned int) {}
void portRenderDocShutdown(void) {}
