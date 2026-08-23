# ROOT_RELOC_TOKEN_NAMESPACE_V9

## Root cause targeted
Repeated stage-select reloads reused relocation slots and advanced their per-slot generation. The legacy token encoded generation directly in the high 12 bits, so the token's high byte changed over time and could enter values that look like N64/F3DEX2 data (for example G_VTX, SETTIMG, G_DL, or segment 0E). This made parsing/resolution dependent on reload count even when the O2R bytes were identical.

Observed in the 2026-08-23 zapping log: the same ExternDataBank108 load keeps the same source, 225 roots, 68 Vtx ranges and 321 normalized vertices, but later changes from 37 valid display lists to 36 with no resource-integrity mismatch.

## Fixes
- Replaced generation-dependent raw token layout with a tagged namespace:
  - bits 31..30: fixed `01`
  - bits 29..18: 12-bit per-slot generation
  - bits 17..0: 18-bit slot index
  - every token is therefore in `0x40000000..0x7FFFFFFF`.
- `0x40..0x7F` high bytes cannot be N64 segment IDs and are outside the packed F3DEX2 opcode ranges used by the port.
- Preserved 4095 generations per slot; capacity is 64K initially and can grow to 256K entries.
- Added `portRelocIsPointerToken()` so stale tokens remain identifiable even after they stop resolving.
- `Interpreter::SegAddr()` now drops a stale tagged token instead of reinterpreting it as a segmented address or raw host pointer.
- `G_DL` explicitly rejects stale tagged tokens instead of installing a null/invalid branch target.
- Post-reloc 3D manifest refuses tagged token words as GBI commands and never falls back from a stale token to segment-0E resolution.
- Existing chain token guard retained as defense in depth.
- Startup mode log now includes `token_namespace=0x40-0x7F`.

## Validation
- `RelocPointerTable.cpp`: Vita-mode syntax check passed.
- `lbreloc_byteswap.cpp`: Vita-mode syntax check passed.
- Token stress test: 4095 per-slot generations all remained inside high-byte range `0x40..0x7F`.
- Capacity-growth test: 70,000 simultaneously registered pointers resolved correctly across the 64K -> 128K growth boundary.
- Full `interpreter.cpp` host syntax pass is blocked by the container's missing SDL2 development headers; the modified blocks were separately inspected and use existing declarations/types.
