# v11 — Vita host-pointer / reloc-token separation + G_MTX safety

## Fixed

- Reverted the relocation token tag from v10 `0x80..0x9F` to the v9-safe `0x40..0x7F` range.
  - Vita host pointers such as `0x87xxxxxx` / `0x88xxxxxx` can no longer be mistaken for stale relocation tokens.
  - Keeps 12-bit per-slot generations (4095 reuse generations) and restores 18-bit token indices (262143 live slots).
- Fixed intro/OTR command disambiguation without moving the token namespace into Vita RAM space.
  - The 3D manifest no longer rejects every `w0` in the token namespace.
  - A display-list command word is treated as reloc pointer data only when `portRelocTryResolvePointer(w0)` proves it is a currently-live token.
  - Real OTR opcodes in `0x40..0x44` remain valid commands.
- Added a defensive NULL guard in `Fast::Interpreter::GfxSpMatrix()`.
  - An unresolved matrix now logs `GFX_MTX_NULL_DROP` (rate-limited on Vita) and drops that matrix command instead of dereferencing address zero.

## Preserved

- v9 stage-reload / generation protection.
- v10 Vita CSS PNG `open -> read-until-EOF` loader; no `sceIoLseek` reintroduced.
- Strict per-resource 3D manifest ownership and synchronous Vita display-list submission.

## Diagnostics expected

- `RELOC_FIXUP_MODE ... token_namespace=0x40-0x7F live-command-disambiguation`
- The startup/N64 logo must no longer emit `RELOC_TOKEN_STALE_DROP` for `0x87xxxxxx` host pointers.
- `EFCommonEffects1/2/3` should return to the pre-v9 manifest counts seen in the healthy trace: `14 / 35 / 34` valid DLs rather than `9 / 25 / 29`.
- `GFX_MTX_NULL_DROP` should normally remain at zero; if it appears, it identifies a separate unresolved matrix pointer without crashing the process.
