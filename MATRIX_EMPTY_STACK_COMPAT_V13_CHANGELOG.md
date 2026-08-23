# v13 — Matrix empty-stack compatibility

Target: PS Vita / Fast3D opening-room regression after v12 runtime hardening.

## Symptom
v12 stabilised the intro, fighters and stages, but the first opening-room phase no longer visibly rendered Master Hand and the logo/text pass.

## Evidence
- BossModel loads successfully and its 3D manifest is stable.
- No v12 OOB guard fires in the captured run.
- During the first opening-room phase, Fast3D repeatedly ends tasks at modelview depth 2 / max depth 5 / DL depth 9.
- v12 changed the original matrix-pop semantics by forbidding the modelview stack from reaching depth 0.

## Root compatibility issue
The SSB64 multi-camera / DL-link flow can use depth 0 as a transient empty/reset state between camera passes. v12 treated the base matrix as permanently present and converted a 1 -> 0 pop into 1 -> 1. That prevents an out-of-bounds access, but can also leave the next camera inheriting the previous camera's modelview transform.

## v13 behaviour
- Preserve v12 bounds hardening, redzones and all non-matrix guards.
- Allow modelview depth 1 -> 0 again.
- Never dereference `modelview_matrix_stack[-1]`.
- While depth is 0, `MP_matrix` uses identity-modelview semantics (`MP = P`).
- The next modelview `G_MTX` safely reseeds slot 0 with identity before LOAD/MUL processing.
- PUSH on an empty stack cannot copy from slot -1.
- POP while already at depth 0 is treated as the real underflow and ignored.
- Positional-light and normal transforms are zero-depth-safe.
- `GFX_RSP_TASK_BOUNDS` no longer reports legal final depth 0/1 as an anomaly.

## New diagnostics
- `GFX_MTX_EMPTY_ENTER action=projection-only`
- `GFX_MTX_EMPTY_RESEED ... action=identity-base`
- `GFX_MTX_STACK_UNDERFLOW depth=0 action=ignore-pop`
- startup marker: `VITA_BOUNDS_HARDENING matrix-stack=safe-empty ...`

No reloc, token, Vtx, texture, fighter or stage-specific behaviour is changed.
