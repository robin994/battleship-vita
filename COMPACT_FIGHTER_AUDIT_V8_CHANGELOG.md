# Compact Fighter Audit v8

- Reworked Mario/Fox model diagnostics so the logger is no longer flooded by per-joint/per-frame audit lines.
- Added one compact `FIGHTER_MODEL_CREATE_SUMMARY` per fighter creation with masks for present joints, expected/actual DLs, mismatches, invalid FTParts, modelpart changes, unregistered DLs, MObjs and a stable hash.
- Added one compact `FIGHTER_TREE_SUMMARY` on the first draw of each new fighter instance with present/reachable/DL/registered/hidden/parts masks and a stable hash.
- Added one compact `FIGHTER_DRAW_SUMMARY` on the first draw of each new fighter instance with traversal/submission/hidden/no-texture/no-DV/null-DL/unsupported-mode/unregistered/parts masks.
- Normal per-joint creation/tree/traverse/draw logs are removed; only anomaly detail lines remain.
- Added monotonically increasing per-fighter creation serials so repeated player-select previews in the same scene are distinguishable.
- Increased the async log queue from 128 to 256 slots.
- Added cumulative logger diagnostics: dropped lines, queue high-water mark and current queued lines.
- Forced NUL termination after formatting and when inserting a line into the async queue, preventing an overlong diagnostic line from making the writer read into adjacent memory.
- No rendering, relocation, Vtx, texture, stage or fighter behavior is changed by this patch; it is a low-noise diagnostic patch intended to identify the exact stage at which a complete model becomes partial.
