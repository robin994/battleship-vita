# v20 — Strict model/resource preflight

Baseline: v19 (functional runtime restored to the known-good v13 code path).

## Goal

Replace permissive/partial model publication with a generic fail-closed validation pipeline.
No stage IDs, fighter IDs, file IDs, or per-resource whitelists are used.

## 1. Reloc transaction preflight

Before `RESOURCE_TRANSACTION_COMMIT ... READY`, every relocation slot produced by the current load is checked:

- slot is 32-bit aligned and inside the owner resource;
- token is non-zero and resolves through the live reloc token table;
- resolved target belongs to a registered reloc resource range;
- external targets already belong to a committed/READY dependency;
- the only not-ready range allowed is the resource currently being constructed.

A failure emits:

`RESOURCE_PREFLIGHT_REJECT ... reason=... action=pristine-reload|quarantine`

The current resource is retracted from status/range/cache registries and is never published READY.
The loader retries the same immutable source bytes exactly once. If validation or the semantic
integrity signature still fails, the `file_id + source hash` pair is quarantined for the session.
Later requests for the same source fail immediately; replacing the source bytes clears the dynamic
quarantine. The former `accept-after-max-retries` path has been removed.

An undersized destination is rejected instead of truncating the resource. The private reloc loader
uses the same complete-or-reject allocation policy.

Successful transactions may emit the bounded marker:

`RESOURCE_PREFLIGHT_PASS ...`

## 2. Atomic DObj/model preflight

`gcSetupCommonDObjs`, `gcSetupCustomDObjs`, and `gcSetupCustomDObjsWithMObj` now validate the complete descriptor stream before creating the first DObj.

Validation includes:

- descriptor bounds when the stream lives in a reloc resource;
- bounded scan (max 1024 descriptors) and required terminator;
- DObj depth/index bounds;
- required parent availability;
- every non-null DL token must resolve;
- consumer-driven payload typing: a descriptor consumed by a direct renderer is
  validated as `Gfx[]`, while a link renderer is validated as a bounded
  `DObjDLLink[]` with legal list IDs, a required `{ 4, NULL }` terminator and a
  valid display-list graph for every non-null link;
- animation-only descriptor trees resolve their tokens without misclassifying
  their payload as a direct display list.

This removes the old failure mode where setup could create half a tree and then return/skip a bad node, leaving a visually incomplete fighter or stage alive. The three setup APIs now return `sb32`; central effect, item, weapon, ground, transition and immediate-root consumers eject the rejected GObj and propagate failure instead of dereferencing an absent root.

This distinction fixes the post-N64-logo regression where valid opening-room
`DObjDLLink` tables were parsed as Fast3D commands, setup returned false, and the
caller still registered `gcDrawDObjTreeDLLinksForGObj` with a null DObj root.
The resulting first instruction `ldrb [r0, #0x54]` produced `DFAR=0x54`.

All standard direct/link GObj draw wrappers now also reject and hide a null-root
or invalid complete tree before emitting model commands. This is a final safety
boundary; setup-time validation remains the publication gate.

Markers:

- `MODEL_PREFLIGHT_PASS ...`
- `MODEL_PREFLIGHT_REJECT ... action=reject-tree`
- `MODEL_DL_PREFLIGHT_REJECT ...`
- `MODEL_DLLINK_PREFLIGHT_REJECT ...`
- `MODEL_QUARANTINE ...`
- `MODEL_DRAW_REJECT ...`

## 3. Strict packed display-list graph validation

Before a reloc-backed packed DL is widened for Fast3D, its static graph is validated recursively.

Checks include:

- root/sub-DL 8-byte alignment and containing file bounds;
- only opcodes implemented by the current F3DEX2/RDP port are accepted;
- command word may not itself be a live reloc token;
- `G_VTX` encoding, destination range and vertex span;
- static `G_DL` target alignment/range and recursive sub-list validation;
- stale DL tokens are rejected;
- `G_MTX` target alignment and 64-byte span;
- `G_SETTIMG` target must resolve to a valid resource byte (or a legitimate deferred N64 segment);
- list must terminate naturally by `G_ENDDL` or `gSPBranchList`;
- the physical containing-resource boundary limits each list and the 16,384-command graph budget prevents runaway validation.

The old 512-command per-list ceiling was removed after checking vanilla resources: `LBTransitionStar`
contains a valid 548-command list and `LBTransitionSudare2` a valid 549-command list, both naturally
terminated. They now pass without weakening the graph-wide safety budget.

Segment 0x0E `G_DL` is deferred by static preflight because owner bytes at the
same offset may be palette/vertex data that accidentally resemble commands. At
execution, a live segment-E binding is authoritative; only when it is unbound
may a bounded structural check select an owner-local packed list. The
file-52/MVCommon prefix whitelist and the unchecked in-file fallback were
removed. If neither target exists, the whole graphics task is aborted.

The opcode check now enumerates the handlers actually installed by the interpreter. It no longer accepts the broad `0xE4..0xFF` range (notably unhandled `0xEA`, `0xEB`, and `0xF1`).

## 4. No more silent truncate-and-render

Previously `portNormalizeDisplayListPointer()` handled an invalid opcode by stopping the copy and appending a synthetic `G_ENDDL`. That made malformed/misclassified DLs executable as shorter lists and is a direct generic mechanism for producing "model/stage is missing pieces" without crashing.

v20 removes that policy:

- preflight failure => `nullptr` / reject;
- invalid opcode during widening after a successful preflight => internal mismatch + reject;
- no synthetic `G_ENDDL` is appended;
- `gSPBranchList` is treated as a real terminal boundary while widening.

Nested preflight failure aborts the task instead of pushing a null DL pointer into the execution stack.

## 5. Validation cache / lifetime

Successful packed-DL preflight results are cached by source pointer and reloc lifetime generation. Any arena/force-heap reuse bumps the generation, so a persistent root whose graph referenced a recycled external resource is revalidated even when the root address itself did not move.

## 6. Runtime fail-closed behavior and diagnostics

- stale-token `G_DL`, unresolved segment-0x0E calls, walked-past sub-DLs and nested normalization failures abort the complete graphics task rather than skipping one command/subtree;
- rejection logs are bounded and include owner `file_id`/path, resource base/size, DL offset and command index, opcode, `w0`, `w1`, target and reason;
- no file, stage, fighter or resource-specific allow/deny list remains in the v20 validator.

## Deliberately unchanged

- v9 reloc-token namespace fix;
- v11 host-pointer/live-command disambiguation;
- v12 bounds/redzones;
- v13 safe-empty matrix-stack semantics;
- vertex byte-order policy;
- texture cache/fixup policy;
- stage/fighter selection logic;
- CSS runtime path from the v13/v19 baseline.

## Scope / limitation

This validates structural and memory correctness before a model/resource is published or executed. It cannot prove artistic/semantic correctness (for example, whether a valid vertex has the intended coordinate), but it prevents the port from accepting structurally invalid data and rendering only the prefix that happened to look valid.
