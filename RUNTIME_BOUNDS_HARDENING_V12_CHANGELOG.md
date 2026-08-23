# Runtime Bounds Hardening v12

## Why this build exists

The v11 log shows Peach's Castle resources loading consistently while the visible result can still fail:

- `ExternDataBank106` is repeatedly committed READY with the same manifest: `roots=134 valid_dls=26 rejected=82 vtx_ranges=40 normalized_vertices=262`.
- No `RESOURCE_INTEGRITY_MISMATCH`, `DL_REJECT_WALKED_PAST`, `VTX_COMMON_REJECT`, `DLBuffer OVERFLOW`, `DynamicBuffer OVERFLOW`, `GFX_MTX_NULL_DROP`, or `RELOC_TOKEN_STALE_DROP` was observed in the failing run.

That moves the investigation downstream from resource loading into Fast3D/RSP runtime state.

## Confirmed unsafe runtime paths found in source

### 1. Model-view matrix stack underflow

`SpReset()` initializes `modelview_matrix_stack_size = 1`, and many hot paths unconditionally index `stack_size - 1`.

Before v12, `GfxSpPopMatrix()` allowed depth `1 -> 0`. A subsequent matrix/lighting/vertex operation could therefore access `modelview_matrix_stack[-1]`, i.e. memory immediately before the 11-entry matrix array.

v12:

- never allows POP below depth 1;
- repairs an already-corrupt depth to the valid range before `GfxSpMatrix()` accesses it;
- logs `GFX_MTX_STACK_UNDERFLOW`, `GFX_MTX_STACK_OVERFLOW`, and `GFX_MTX_DEPTH_REPAIR`.

### 2. Unchecked vertex-cache indices

`loaded_vertices` has `MAX_VERTICES + 4` entries (68 on this build), but `GfxSpTri1()` and `GfxSpModifyVertex()` indexed it without a bounds check. The custom OTR branch-Z path also decoded a 12-bit vertex index into a `uint8_t` and indexed the same array.

v12 drops invalid accesses and logs `GFX_VTX_INDEX_OOB` with contexts `TRI`, `MODIFY`, or `BRANCH_Z`.

### 3. Unchecked segment-table writes

`mSegmentPointers` has 16 entries. Both F3D and F3DEX2 `G_MW_SEGMENT` handlers wrote `mSegmentPointers[offset / 4]` without validating that the resulting index is 0..15.

v12 drops invalid writes and logs `GFX_SEGMENT_INDEX_OOB`.

### 4. Unbounded light count

`current_lights` contains `MAX_LIGHTS + 1` entries, but `G_MW_NUMLIGHT` could set `current_num_lights` to an arbitrary value. Lighting then iterates and indexes using that count.

v12 clamps the decoded count to `1..MAX_LIGHTS+1` and logs `GFX_LIGHT_COUNT_OOB`.

### 5. Null MOVEMEM sources / look-at bounds

F3D/F3DEX2 MOVEMEM handlers copied from `data` without a null check. The F3DEX2 look-at path could also derive an index outside the two-entry look-at array from a malformed offset.

v12 drops these accesses and logs `GFX_MOVEMEM_NULL` / `GFX_LIGHT_INDEX_OOB`.

### 6. DL buffer overflow was detected too late to be safe

The existing `syTaskmanCheckBufferLengths()` only checks the raw display-list heads after a draw/update pass. If a raw `gSYTaskmanDLHeads[i]++` sequence ran past its logical allocation, adjacent scene memory could already have been overwritten before `DLBuffer OVERFLOW` was printed.

On Vita only, v12 physically reserves an 8 KiB redzone after every taskman display-list buffer and RDP output buffer while preserving the original logical lengths. Therefore:

- existing overflow detection still fires at the original boundary;
- a modest overflow is contained in slack instead of immediately corrupting the next allocation;
- `DLBuffer HIGH_WATER` reports buffers reaching >=90% of their logical capacity.

This is containment/diagnostic hardening, not an excuse to keep an undersized DL buffer if an overflow is actually observed.

## Task-level diagnostics

`GFX_RSP_TASK_BOUNDS` is emitted (bounded) when a top-level Fast3D task ends with an unbalanced model-view stack or reaches unusually deep nested display-list execution.

Startup marker:

`VITA_BOUNDS_HARDENING matrix-stack vertex-index segment-index light-count movemem-null dl-redzone=8192`

## What was intentionally NOT changed

- relocation-token namespace / v11 live-command disambiguation;
- resource manifests / resource ownership;
- Vtx byte-order policy;
- stage-specific or fighter-specific behavior;
- texture cache behavior;
- stage-select PNG loader;
- renderer draw/shader policy.

The patch is generic Vita runtime hardening. There are no stage IDs, fighter IDs, or per-resource whitelists in the fix logic.

## What to look for on hardware

The highest-value markers are:

- `GFX_MTX_STACK_UNDERFLOW`
- `GFX_MTX_STACK_OVERFLOW`
- `GFX_MTX_DEPTH_REPAIR`
- `GFX_VTX_INDEX_OOB`
- `GFX_SEGMENT_INDEX_OOB`
- `GFX_LIGHT_COUNT_OOB`
- `GFX_MOVEMEM_NULL`
- `DLBuffer HIGH_WATER`
- `DLBuffer OVERFLOW`
- `GFX_RSP_TASK_BOUNDS`

If one of these appears immediately before the intro/stage/UI breaks, it gives a concrete runtime corruption path rather than another resource-level hypothesis.
