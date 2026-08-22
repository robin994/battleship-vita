# Root GFX Resource Lifetime v7

- Fixed the Vita display-list lifetime bug across scene transitions.
- Vita now consumes each submitted Fast3D display list immediately while its scene/resource heap is still alive.
- Removed the unsafe Vita window where `sPendingDisplayList` could survive `RESOURCE_FREE` / heap reuse before Fast3D consumed it.
- Added a monotonic reloc-resource lifetime generation.
- Deferred paths now bind each pending `Gfx*` to its submit generation and drop it if resource lifetime advances before consumption.
- Address reuse no longer makes an old pending display list look valid merely because a new resource occupies the same numeric range.
- Restores the intended synchronous Vita behavior documented by `osSpTaskStartGo`, while preserving Android's deferred main-thread path.
- Fixes the RCP cost-model timing on Vita: DL stats are now latched before `port_submit_display_list()` returns.
- No fighter, model, stage, or file-ID whitelist was added.
- New diagnostics: `GFX_LIFETIME_MODE`, `RESOURCE_LIFETIME_GENERATION`, `GFX_LIFETIME_FALLBACK_DROP`, `GFX_PENDING_REPLACE`.
