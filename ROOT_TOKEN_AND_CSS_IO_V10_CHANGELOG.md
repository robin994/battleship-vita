# v10 — Token namespace + CSS PNG I/O

## Fixed

- Preserved the v9 stage-geometry fix and moved relocation tokens from `0x40-0x7F` to `0x80-0x9F`.
- v9's `0x40-0x7F` range overlapped real OTR display-list opcodes (`0x40`, `0x42`, `0x43`, `0x44`), so raw relocation words could be misread as valid graphics commands in intro/effect display lists.
- New layout: `[3-bit tag=100][12-bit generation][17-bit index]`.
- Full 4095 generations per slot retained; up to 131071 simultaneous relocation slots.
- Existing stale-token, ownership, resource-lifetime and synchronous Vita display-list guards retained.
- CSS stage-select PNG loading no longer treats `sceIoGetstat().st_size` as authoritative.
- Vita CSS PNG files are now opened once and streamed with repeated `sceIoRead()` calls until EOF; short reads are handled correctly.
- No `sceIoLseek()` is used by the CSS PNG loader.
- Existing PNG signature/CRC diagnostics and ROM-sprite fallback remain intact.

## New diagnostics

- `RELOC_FIXUP_MODE ... token_namespace=0x80-0x9F`
- `CSS stage assets: PNG_IO_MODE vita=fd-stream-until-eof stat=advisory`
