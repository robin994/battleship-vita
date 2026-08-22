#!/usr/bin/env python3
"""
derive_stage_assets.py — Extract CSS stage assets from baserom.us.z64.

For each stage in the STAGES manifest this script:
  1. Extracts the reloc file from baserom.us.z64 (using the VPK0/reloc logic
     shared with debug_tools/reloc_extract/reloc_extract.py).
  2. Parses the Sprite struct at the specified byte offset (big-endian ROM layout).
  3. Resolves each Bitmap's texel-buffer pointer using the SSB64 RELOC linked-list
     encoding (see "RELOC internal-pointer resolver" section below).
  4. Un-swizzles the N64 TMEM odd-row XOR4 pattern for 16bpp textures.
  5. Stitches all bitmap strips into one full RGBA image.
  6. Saves <output_dir>/<name>_background.png  (full resolution, e.g. 300x220).
  7. Saves <output_dir>/<name>_small.png       (48x36 LANCZOS downscale).
  8. If the stage entry has a "name_text" key: synthesizes a 96x10 nameplate
     from glyphs segmented out of the ROM's own MNMaps stage-name sprites, then
     saves it as <output_dir>/<name>_name.png.  No host/system font is used.

Usage:
  python3 tools/derive_stage_assets.py <baserom.z64> <output_dir>

The output PNGs are loaded at runtime by port/css_icons/port_css_stage_assets.cpp
via stb_image. They are NOT committed to the repo — they are derived from the
user's own baserom at build time by the CMake pipeline.

Adding a new stage:
  Append an entry to the STAGES list below and re-run (CMake will re-run
  automatically because derive_stage_assets.py is listed as a DEPENDS).
"""

import struct
import sys
from pathlib import Path
from PIL import Image

# ---------------------------------------------------------------------------
# Import the VPK0 decoder and RELOC extractor from the existing debug tool.
# We add the debug_tools/reloc_extract directory to the path and import the
# relevant functions directly so we don't duplicate the logic.
# ---------------------------------------------------------------------------
_SCRIPT_DIR = Path(__file__).resolve().parent
_REPO_ROOT   = _SCRIPT_DIR.parent
sys.path.insert(0, str(_REPO_ROOT / "debug_tools" / "reloc_extract"))
from reloc_extract import extract_file  # type: ignore  # noqa: E402

# ---------------------------------------------------------------------------
# Stage manifest.
#
# To add a new port-introduced stage:
#   1. Append a dict with these keys:
#        name        — filename stem (e.g. "brinstar")
#        gkind       — GRKind enum value (for runtime lookup table)
#        reloc_file  — file index inside the ROM's RELOC table
#        sprite_off  — byte offset of the Sprite struct inside that file
#        name_text   — (optional) string to render as a synthetic nameplate PNG.
#                      Only needed when no ROM-shipped nameplate exists for this
#                      stage (e.g. nGRKindLast/Final Destination). Stages that
#                      ship a ROM nameplate should omit this key.
#   2. Re-run this script (or trigger a CMake rebuild — it lists this file
#      and baserom.us.z64 as DEPENDS so it re-runs automatically).
#   3. The new <name>_background.png and <name>_small.png appear in the
#      output directory and are picked up by portCSSGetStageBackgroundSprite()
#      and portCSSGetStageIconSprite() at runtime via the same gkind lookup.
#   4. If name_text is set, <name>_name.png also appears and is picked up
#      by portCSSGetStageNameSprite() at runtime.
# ---------------------------------------------------------------------------
STAGES = [
    {
        "name":       "final_destination",   # filename stem
        "gkind":      16,                    # nGRKindLast
        "reloc_file": 96,                    # file 96 = StageLastFile1
        "sprite_off": 0x26c88,              # dStageLastBackground_0x26c88
        "name_text":  "FINAL DESTINATION",  # synthetic nameplate (no ROM sprite)
        "emblem_src": {
            "reloc_file": 345,              # llMasterHandIconFileID = 0x159
            "sprite_off": 0x2b8,            # llMasterHandIconFTEmblemSprite = 0x2b8
        },
    },
    {
        # nGRKindMetal — Meta Crystal (Metal Mario boss stage). The stage data
        # file 0x62 ships a full 300x220 CSS wallpaper at the same 0x26c88
        # offset every Stage*FileID uses; the matching map file (0x10d) carries
        # collision data, not visuals.
        "name":       "metal_cavern",
        "gkind":      13,                    # nGRKindMetal
        "reloc_file": 0x62,                  # ll_98_FileID — Meta Crystal stage data
        "sprite_off": 0x26c88,
        "name_text":  "METAL CAVERN",        # no ROM nameplate exists; synthesize
        # No emblem_src — the FD emblem path is still stubbed off in
        # mnMapsMakeEmblem pending IA4 debug; new stages take the same skip.
    },
    {
        # nGRKindZako — Duel Zone (Polygon Fighters arena). Same wallpaper-in-
        # stage-data-file pattern as Metal Cavern; stage data file is 0x61.
        "name":       "battlefield",
        "gkind":      14,                    # nGRKindZako
        "reloc_file": 0x61,                  # ll_97_FileID — Duel Zone stage data
        "sprite_off": 0x26c88,
        "name_text":  "BATTLEFIELD",
    },
]

# ---------------------------------------------------------------------------
# Nameplate PNG dimensions — derived from ROM IA4 nameplate sprites in
# reloc file 30 (MNMaps), e.g. PeachsCastleText at offset 0x1f8:
#   width=96, height=10, bmfmt=4 (G_IM_FMT_IA), bmsiz=0 (G_IM_SIZ_4b=IA4)
# The SObj caller forces red=green=blue=0x00 (black), so only the alpha channel
# matters at runtime.  The PNG is stored as RGBA with black opaque pixels on a
# transparent background.
# ---------------------------------------------------------------------------
NAME_W = 96
NAME_H = 10

# Vanilla Stage Select nameplates live in reloc file 30 (MNMaps).  They are
# pre-rendered IA4 masks, not a runtime font.  Build a small glyph atlas from
# these ROM-owned labels so port-only stage names inherit the exact same pixel
# style instead of Pillow's unrelated default bitmap font.  Labels containing
# apostrophes are intentionally omitted: the punctuation can form a separate
# alpha run and complicate deterministic segmentation, while the remaining
# labels already cover every letter needed by BATTLEFIELD / FINAL DESTINATION
# and every METAL CAVERN letter except V.
NAMEPLATE_RELOC_FILE = 30
NAMEPLATE_SOURCES = [
    ("SECTOR Z",          0x438),
    ("CONGO JUNGLE",      0x678),
    ("PLANET ZEBES",      0x8B8),
    ("HYRULE CASTLE",     0xB10),
    ("SAFFRON CITY",      0xF98),
    ("MUSHROOM KINGDOM", 0x11D8),
    ("DREAM LAND",       0x1418),
]

# Vanilla stage labels do not contain V.  Use the ROM's MNCommonFonts V only
# as a last-resort single-glyph seed for METAL CAVERN; it is resized with
# nearest-neighbour and thresholded to the same 1-bit alpha mask.
COMMON_FONTS_RELOC_FILE = 33
COMMON_FONTS_V_SPRITE_OFF = 0xC10

# CSS thumbnail dimensions — same for all stage icons (matches N64 CSS format).
ICON_W = 48
ICON_H = 36

# FT emblem target dimensions — matches every llFTEmblemSprites* sprite in file 20
# (width=64, height=48, bmfmt=4/IA, bmsiz=0/IA4, verified empirically).
EMBLEM_W = 64
EMBLEM_H = 48

# ---------------------------------------------------------------------------
# Sprite struct / Bitmap struct sizes (bytes).
# ---------------------------------------------------------------------------
SPRITE_SIZE = 68
BITMAP_SIZE = 16

# ---------------------------------------------------------------------------
# Big-endian field readers.
# ---------------------------------------------------------------------------


def _be_s16(data: bytes, off: int) -> int:
    return struct.unpack_from(">h", data, off)[0]


def _be_u32(data: bytes, off: int) -> int:
    return struct.unpack_from(">I", data, off)[0]


def _be_u8(data: bytes, off: int) -> int:
    return data[off]


# ---------------------------------------------------------------------------
# Sprite struct parser (big-endian ROM layout, total 68 bytes).
#
# Field offsets (from port_css_fd_icon.cpp header comment):
#   0x00  s16 x, y
#   0x04  s16 width, height
#   0x08  f32 scalex
#   0x0C  f32 scaley
#   0x10  s16 expx, expy
#   0x14  u16 attr, s16 zdepth
#   0x18  u8  red, green, blue, alpha
#   0x1C  s16 startTLUT, nTLUT
#   0x20  u32 LUT  (reloc token)
#   0x24  s16 istart, istep
#   0x28  s16 nbitmaps, ndisplist
#   0x2C  s16 bmheight, bmHreal
#   0x30  u8  bmfmt, u8 bmsiz, pad, pad
#   0x34  u32 bitmap pointer (reloc token)
#   0x38  u32 rsp_dl
#   0x3C  u32 rsp_dl_next
#   0x40  s16 frac_s, frac_t
#
# Bitmap struct (16 bytes, big-endian ROM layout):
#   0x00  s16 width, width_img
#   0x04  s16 s, t
#   0x08  u32 buf  (reloc token)
#   0x0C  s16 actualHeight, LUToffset
# ---------------------------------------------------------------------------


def parse_sprite(file_data: bytes, sprite_off: int) -> dict:
    """
    Parse the Sprite struct at sprite_off inside file_data (big-endian).
    Returns a dict with the fields we care about.
    """
    d = file_data
    o = sprite_off
    sp = {
        "x":         _be_s16(d, o + 0x00),
        "y":         _be_s16(d, o + 0x02),
        "width":     _be_s16(d, o + 0x04),
        "height":    _be_s16(d, o + 0x06),
        "nbitmaps":  _be_s16(d, o + 0x28),
        "ndisplist": _be_s16(d, o + 0x2A),
        "bmheight":  _be_s16(d, o + 0x2C),
        "bmHreal":   _be_s16(d, o + 0x2E),
        "bmfmt":     _be_u8(d, o + 0x30),
        "bmsiz":     _be_u8(d, o + 0x31),
        "sprite_off": sprite_off,
    }
    return sp


def parse_bitmap(file_data: bytes, bm_off: int) -> dict:
    """Parse one Bitmap struct at bm_off (big-endian)."""
    d = file_data
    o = bm_off
    bm = {
        "width":        _be_s16(d, o + 0x00),
        "width_img":    _be_s16(d, o + 0x02),
        "s":            _be_s16(d, o + 0x04),
        "t":            _be_s16(d, o + 0x06),
        "buf_raw":      _be_u32(d, o + 0x08),
        "actualHeight": _be_s16(d, o + 0x0C),
        "LUToffset":    _be_s16(d, o + 0x0E),
        "off": o,
    }
    return bm


# ---------------------------------------------------------------------------
# RELOC internal-pointer resolver.
#
# After VPK0 decompression the file bytes contain pre-relocation data.
# The N64 OS would patch each internal-reloc slot by adding the KSEG0 load
# address; we see the raw pre-patch values from reloc_extract.py.
#
# Encoding of each u32 internal-reloc slot (big-endian):
#   high 16 bits — word index of next reloc entry in the linked list
#                  (0xFFFF = end of list)
#   low  16 bits — (file_relative_byte_offset / 4) for the pointed-to data
#
# To recover the file-relative byte offset of the pointed-to data:
#   file_off = (raw_u32 & 0xFFFF) * 4
#
# Empirically verified against file 96 (FD background, 44-bitmap RGBA16 sprite):
#   - Sprite.bitmap ptr at sprite+0x34: raw=0xFFFF9A72 -> 0x9A72*4=0x269C8
#     (which is the Bitmap array start in that file)
#   - Bitmap[0].buf at bm+0x08: raw=0x9A780002 -> 0x0002*4=0x8
#     (start of first texel strip, 8 bytes after the file start for alignment)
# ---------------------------------------------------------------------------


def resolve_reloc_ptr(file_data: bytes, field_off: int) -> int:
    """
    Read the 4-byte big-endian reloc pointer at field_off and return the
    file-relative byte offset it encodes: (raw_u32 & 0xFFFF) * 4.
    """
    raw = _be_u32(file_data, field_off)
    return (raw & 0xFFFF) * 4


def resolve_bitmap_offsets(file_data: bytes, sp: dict) -> list:
    """
    Locate the Bitmap array (via the sprite's bitmap ptr reloc field) and
    resolve each bitmap's buf field to a file-relative byte offset.
    Returns a list of dicts (one per bitmap) with an added 'buf_off' key.
    """
    sprite_off = sp["sprite_off"]
    nbitmaps   = sp["nbitmaps"]

    # Resolve Bitmap array location from sprite+0x34 (bitmap ptr reloc field).
    bm_array_start = resolve_reloc_ptr(file_data, sprite_off + 0x34)

    bitmaps = []
    for i in range(nbitmaps):
        bm_off = bm_array_start + i * BITMAP_SIZE
        bm = parse_bitmap(file_data, bm_off)
        # Resolve each bitmap's texel buf pointer.
        bm["buf_off"] = resolve_reloc_ptr(file_data, bm_off + 0x08)
        bitmaps.append(bm)

    return bitmaps


# ---------------------------------------------------------------------------
# TMEM odd-row un-swizzle for 16bpp (RGBA16 / IA16) textures.
#
# The N64 RDP pre-swizzles RGBA16/IA16 texture data stored in DRAM so that
# LOAD_BLOCK (dxt=0) can stream it into TMEM without stalls. For 16bpp, odd
# rows (strip-local row index within each bitmap) have the two 4-byte halves
# of each 8-byte qword swapped (XOR4 addressing). Fast3D reads texels linearly,
# so we must un-swap before writing pixels.
#
# Reference: docs/bugs/sprite_texel_tmem_swizzle_2026-04-10.md
# ---------------------------------------------------------------------------


def unswizzle_rgba16_strip(raw: bytes, width: int, height: int) -> bytes:
    """
    Un-swizzle one bitmap strip of RGBA16 data (width x height pixels,
    2 bytes per pixel, strip-local row indexing).

    For each odd row, within every 8-byte qword swap the two 4-byte halves.
    Returns the corrected bytes.
    """
    row_bytes = width * 2
    out = bytearray(raw)
    for row in range(height):
        if row % 2 == 0:
            continue  # even rows are fine
        row_start = row * row_bytes
        qword = 0
        while qword * 8 < row_bytes:
            q_off = row_start + qword * 8
            # Swap the two 4-byte halves within this 8-byte qword.
            a = out[q_off:q_off + 4]
            b = out[q_off + 4:q_off + 8]
            out[q_off:q_off + 4] = b
            out[q_off + 4:q_off + 8] = a
            qword += 1
    return bytes(out)


# ---------------------------------------------------------------------------
# RGBA16 -> RGBA8888 pixel decoder.
#
# N64 RGBA16 layout: R5 G5 B5 A1 (big-endian u16, MSB = R bit 4).
#   bits 15..11 = R5, bits 10..6 = G5, bits 5..1 = B5, bit 0 = A1
# ---------------------------------------------------------------------------


def rgba16_to_rgba8888(raw: bytes) -> bytes:
    """
    Convert a big-endian RGBA16 buffer to 8-bit RGBA.
    Returns bytes of length (len(raw) / 2) * 4.
    """
    npixels = len(raw) // 2
    out = bytearray(npixels * 4)
    for i in range(npixels):
        word = (raw[i * 2] << 8) | raw[i * 2 + 1]
        r5 = (word >> 11) & 0x1F
        g5 = (word >>  6) & 0x1F
        b5 = (word >>  1) & 0x1F
        a1 = word & 0x01
        out[i * 4 + 0] = (r5 << 3) | (r5 >> 2)  # expand 5 -> 8 bits
        out[i * 4 + 1] = (g5 << 3) | (g5 >> 2)
        out[i * 4 + 2] = (b5 << 3) | (b5 >> 2)
        out[i * 4 + 3] = 255 if a1 else 0
    return bytes(out)


# ---------------------------------------------------------------------------
# IA4 -> RGBA8888 pixel decoder.
#
# N64 IA4 layout: 4 bits per pixel, packed two-per-byte (big-endian nibble order):
#   high nibble of byte i -> pixel 2*i    (3-bit intensity, 1-bit alpha)
#   low  nibble of byte i -> pixel 2*i+1
#   bits 3..1 = I3, bit 0 = A1
#
# Alpha: 0 -> fully transparent, 1 -> fully opaque.
# Intensity: expanded 3-bit -> 8-bit: I8 = (I3 << 5) | (I3 << 2) | (I3 >> 1)
#   equivalent to round(I3 / 7.0 * 255).
#
# On the N64 the IA4 alpha channel equals the intensity when rendered with a
# combine mode that pipes intensity to alpha.  For CSS emblem use we emit
# (I, I, I, A) per pixel — the caller (mnmaps.c SObj path) overrides RGB
# channels to the franchise colour, leaving only alpha from the texture.
# ---------------------------------------------------------------------------


def unswizzle_ia4_strip(raw: bytes, width_img: int, height: int) -> bytes:
    """Undo the N64 sprite-library TMEM odd-row swizzle for IA4 data.

    The runtime Vita path performs the same transform in
    portFixupSpriteBitmapData(): 4b/8b/16b sprite LOAD_BLOCK data swaps the
    two 4-byte halves of each 8-byte group on odd rows.  ROM bytes are already
    in big-endian texel order, so unlike the runtime bridge we do *not* BSWAP32
    here; we only undo the pre-swizzle before linear pixel decoding.
    """
    row_bytes = (width_img + 1) // 2
    out = bytearray(raw)
    if row_bytes < 8:
        return bytes(out)
    for row in range(1, height, 2):
        base = row * row_bytes
        for g in range(0, row_bytes - 7, 8):
            a = out[base + g:base + g + 4]
            b = out[base + g + 4:base + g + 8]
            out[base + g:base + g + 4] = b
            out[base + g + 4:base + g + 8] = a
    return bytes(out)


def ia4_to_rgba8888(raw: bytes, width: int, height: int, width_img: int | None = None) -> bytes:
    """
    Convert an N64 IA4 buffer to 8-bit RGBA.

    Each byte encodes two pixels (high nibble = left pixel, low nibble = right).
    The source stride in the ROM is padded to 32-bit alignment for LOAD_BLOCK
    (naturalWidth = round_up(width, 8) in nibbles, i.e. round_up(width, 8)//2
    bytes per row).  We read exactly (width+1)//2 * height bytes (the actual
    pixel data without alignment padding) unless the caller provides the padded
    stride — since parse_bitmap returns bitmap.width_img (the DMA-padded
    naturalWidth), we use that to compute the row stride.

    Args:
        raw:    Raw IA4 bytes (may include right-edge padding nibbles per row).
        width:  Rendered pixel width (the sprite's width field).
        height: Rendered pixel height.
    Returns:
        bytes of length width * height * 4 (RGBA8888).
    """
    # IA4 packs 2 pixels per byte.  The DMA/source row stride is width_img,
    # while width is only the authored/rendered width.  They often match, but
    # using width here corrupts sprites with padded source rows.
    stride_pixels = width if width_img is None else width_img
    row_stride = (stride_pixels + 1) // 2
    out = bytearray(width * height * 4)
    for row in range(height):
        for col in range(width):
            byte_idx = row * row_stride + col // 2
            nibble = (raw[byte_idx] >> 4) if (col % 2 == 0) else (raw[byte_idx] & 0x0F)
            i3 = (nibble >> 1) & 0x07
            a1 = nibble & 0x01
            # Expand 3-bit intensity to 8-bit: replicate MSBs into lower bits.
            i8 = (i3 << 5) | (i3 << 2) | (i3 >> 1)
            a8 = 255 if a1 else 0
            px = (row * width + col) * 4
            out[px + 0] = i8
            out[px + 1] = i8
            out[px + 2] = i8
            out[px + 3] = a8
    return bytes(out)


# ---------------------------------------------------------------------------
# ROM-style nameplate glyph extraction / renderer.
# ---------------------------------------------------------------------------


def decode_ia4_sprite(file_data: bytes, sprite_off: int) -> Image.Image:
    """Decode a reloc-file IA4 Sprite into an RGBA PIL image."""
    sp = parse_sprite(file_data, sprite_off)
    if sp["bmfmt"] != 4 or sp["bmsiz"] != 0:
        raise ValueError(
            f"sprite 0x{sprite_off:X}: expected IA4 (fmt=4/siz=0), "
            f"got fmt={sp['bmfmt']} siz={sp['bmsiz']}"
        )

    w = sp["width"]
    h = sp["height"]
    rows = []
    for bm in resolve_bitmap_offsets(file_data, sp):
        strip_w = bm["width"]
        strip_w_img = bm["width_img"] if bm["width_img"] > 0 else strip_w
        strip_h = bm["actualHeight"]
        rendered_rows = min(sp["bmheight"], strip_h)
        row_stride = (strip_w_img + 1) // 2
        raw_bytes = row_stride * strip_h
        buf_off = bm["buf_off"]
        if buf_off < 0 or buf_off + raw_bytes > len(file_data):
            raise ValueError(
                f"IA4 sprite 0x{sprite_off:X}: bitmap data 0x{buf_off:X}+0x{raw_bytes:X} "
                f"outside reloc file (0x{len(file_data):X})"
            )
        raw_strip = file_data[buf_off:buf_off + raw_bytes]
        raw_strip = unswizzle_ia4_strip(raw_strip, strip_w_img, strip_h)
        rgba = ia4_to_rgba8888(raw_strip, strip_w, strip_h, strip_w_img)
        row_bytes = strip_w * 4
        for row in range(rendered_rows):
            # Nameplate sprites are full-width strips; crop any DMA padding to
            # the authored Sprite.width before appending the row.
            rows.append(rgba[row * row_bytes:row * row_bytes + w * 4])

    if len(rows) < h:
        rows.extend([bytes(w * 4)] * (h - len(rows)))
    return Image.frombytes("RGBA", (w, h), b"".join(rows[:h]))


def _alpha_runs(img: Image.Image) -> list:
    """Return contiguous x-ranges containing at least one visible pixel."""
    alpha = img.getchannel("A")
    runs = []
    run_start = None
    for x in range(img.width):
        occupied = any(alpha.getpixel((x, y)) >= 128 for y in range(img.height))
        if occupied and run_start is None:
            run_start = x
        elif not occupied and run_start is not None:
            runs.append((run_start, x))
            run_start = None
    if run_start is not None:
        runs.append((run_start, img.width))
    return runs


def _glyph_mask(img: Image.Image, x0: int, x1: int) -> Image.Image:
    """Crop one glyph run and normalize it to black + 1-bit alpha."""
    glyph = img.crop((x0, 0, x1, img.height)).convert("RGBA")
    alpha = glyph.getchannel("A").point(lambda a: 255 if a >= 128 else 0)
    out = Image.new("RGBA", glyph.size, (0, 0, 0, 0))
    out.putalpha(alpha)
    return out


def _trim_glyph_alpha(img: Image.Image) -> Image.Image:
    """Trim transparent side columns while preserving the 10px baseline."""
    bbox = img.getchannel("A").getbbox()
    if bbox is None:
        return Image.new("RGBA", (1, img.height), (0, 0, 0, 0))
    return img.crop((bbox[0], 0, bbox[2], img.height))


def _glyph_similarity(a: Image.Image, b: Image.Image) -> float:
    """Binary-alpha similarity for two ROM glyph samples.

    Repeated letters in MNMaps use the same raster glyph but tight kerning
    can crop a transparent -- or even inked -- edge column (see CONGO
    JUNGLE's 'L': its only raw run is just the vertical stroke, the foot
    entirely lost to neighbouring-glyph contention).  Comparing trimmed
    masks with a small horizontal shift, as before, handles the common
    off-by-a-column case; on top of that, also score containment -- does
    the *entire* smaller glyph's ink sit inside the larger one's ink at some
    alignment?  This is what lets a later, fuller sample for the same
    letter be recognised as consistent with (and safely replace, via the
    widest-sample rule in ``build_rom_nameplate_glyphs``) an earlier
    truncated one, without resizing either glyph or guessing a shape.  A
    pure containment match is capped below a pure ink-for-ink match so a
    pixel-identical pair still wins any comparison.
    """
    a = _trim_glyph_alpha(a)
    b = _trim_glyph_alpha(b)
    if a.height != b.height:
        return 0.0

    aset = {
        (x, y)
        for y in range(a.height)
        for x in range(a.width)
        if a.getchannel("A").getpixel((x, y)) >= 128
    }
    bbase = {
        (x, y)
        for y in range(b.height)
        for x in range(b.width)
        if b.getchannel("A").getpixel((x, y)) >= 128
    }
    if not aset or not bbase:
        return 0.0

    small, large = (aset, bbase) if len(aset) <= len(bbase) else (bbase, aset)
    iou = 0.0
    contain = 0.0
    shift = 2 if abs(a.width - b.width) > 2 else 1
    for dx in range(-shift, shift + 1):
        bset = {(x + dx, y) for x, y in bbase}
        union = aset | bset
        if union:
            iou = max(iou, len(aset & bset) / len(union))
        shifted_small = {(x + dx, y) for x, y in small}
        contain = max(contain, len(shifted_small & large) / len(shifted_small))
    # Containment alone is gameable by extreme size mismatches: a
    # near-empty sliver trivially sits "inside" almost anything, and a huge
    # blob trivially "contains" almost anything small.  Require the two
    # glyphs to be within a plausible size ratio of each other -- a
    # genuinely truncated sample (CONGO JUNGLE's 'L' losing its foot) is
    # still a large fraction of the real glyph's ink, not a sliver of it.
    if len(small) < 0.4 * len(large):
        contain = 0.0
    return max(iou, contain * 0.92)


def _zip_validates(img: Image.Image, runs: list, chars: list) -> bool:
    """True if zipping ``runs`` 1:1 onto ``chars`` is plausible on its face.

    Deliberately does *not* compare against ``atlas`` here.  Pixel-similarity
    is the right tool for choosing between ambiguous *candidate*
    segmentations (see ``_segment_nameplate_runs``), but this font has
    genuine, harmless inter-label style variance -- e.g. 'O' and 'E' score a
    perfect 1.0 across labels while a correctly-segmented 'C' can legitimately
    score as low as ~0.3 against another correctly-segmented 'C' -- so using
    it to gate an *already count-matched* zip produces false-positive
    rejections.  Width is a far more robust signal for "is this actually one
    character": every confirmed-correct single glyph across every MNMaps
    label tops out around 7px (uppercase O), so a run wider than that,
    regardless of how it happens to score, essentially never is one
    character.  This alone is what catches MUSHROOM KINGDOM's 16px/12px
    merged runs.
    """
    if len(runs) != len(chars):
        return False
    return all((x1 - x0) <= 8 for x0, x1 in runs)


def _split_candidate_boundaries(runs: list) -> list:
    """Candidate x-positions where a character boundary may fall.

    Always includes every run's start/end (genuine ink/transparent
    transitions from ``_alpha_runs``).  Also includes every interior column
    of any run wider than 8px -- the widest confirmed single ROM glyph seen
    across every MNMaps label (uppercase O, ~7px) -- since only an
    anomalously wide run can plausibly hold more than one character.  This
    keeps the search small while staying pixel-driven: it does not assume
    *which* run is wrong or *where* inside it the true boundary sits, only
    that a run wider than any real single glyph is worth searching.
    """
    bounds = set()
    for x0, x1 in runs:
        bounds.add(x0)
        bounds.add(x1)
        if (x1 - x0) > 8:
            bounds.update(range(x0 + 1, x1))
    return sorted(bounds)


def _segment_nameplate_runs(
    img: Image.Image, chars: list, atlas: dict, text: str, sprite_off: int
) -> list | None:
    """Find the best-supported partition of the label into len(chars) runs.

    Vanilla MNMaps labels normally have one alpha run per character, but two
    independent failure modes exist in the real ROM data: a tightly-kerned
    pair whose rasters touch with no separating transparent column (too few
    runs -- see SAFFRON CITY's O+N), and a single glyph containing its own
    fully-transparent internal column (too many runs -- see DREAM LAND's R).
    A label can even contain one of each simultaneously (see MUSHROOM
    KINGDOM's "ROO" touching pair and separate KINGDOM-tail split, which
    happened to cancel out to the *correct* raw run count and so passed the
    naive count check while silently mis-segmenting several letters).

    Rather than special-case each shape of error, this is one dynamic
    program over candidate boundary columns (see
    ``_split_candidate_boundaries``) that partitions the label into exactly
    ``len(chars)`` regions while maximizing agreement with already-trusted
    ROM glyph templates in ``atlas``.  Merging two runs into one character
    and splitting one run into two characters are both just different
    choices of which candidate boundaries to use -- no separate merge/split
    code paths are needed.

    A character with no template yet in ``atlas`` contributes no score to
    the search (it neither helps nor penalizes a candidate boundary), so its
    boundary is decided purely by its *neighbours'* agreement with ROM
    truth; see the unknown-character guard below for how much of that is
    required before an unseen glyph is trusted at all.

    Returns None (never guesses) unless every already-known character in the
    winning partition matches its ROM template strongly (>=0.72) and the
    label average is high (>=0.84) -- the same conservative bar the
    single-purpose recovery code used before this was generalized.
    """
    raw_runs = _alpha_runs(img)
    if not raw_runs:
        return None
    n_chars = len(chars)
    bounds = _split_candidate_boundaries(raw_runs)
    if len(bounds) < n_chars + 1:
        return None
    n_b = len(bounds)

    NEG = float("-inf")
    # dp[k][j] = best cumulative similarity placing the first k characters
    # using boundaries bounds[0..j], with character k-1 ending at bounds[j].
    dp = [[NEG] * n_b for _ in range(n_chars + 1)]
    known_count = [[0] * n_b for _ in range(n_chars + 1)]
    back = [[-1] * n_b for _ in range(n_chars + 1)]
    dp[0][0] = 0.0
    sim_cache = {}

    def region_score(ch, i, j):
        x0, x1 = bounds[i], bounds[j]
        key = (x0, x1, ch)
        if key in sim_cache:
            return sim_cache[key]
        glyph = _glyph_mask(img, x0, x1)
        if glyph.getchannel("A").getbbox() is None:
            sim_cache[key] = None
        elif ch not in atlas:
            sim_cache[key] = (0.0, False)
        else:
            sim_cache[key] = (_glyph_similarity(glyph, atlas[ch]), True)
        return sim_cache[key]

    for k in range(1, n_chars + 1):
        ch = chars[k - 1]
        for j in range(k, n_b):
            best_val, best_known, best_i = NEG, 0, -1
            for i in range(k - 1, j):
                if dp[k - 1][i] == NEG:
                    continue
                scored = region_score(ch, i, j)
                if scored is None:
                    continue
                sim, is_known = scored
                cand = dp[k - 1][i] + sim
                if cand > best_val:
                    best_val = cand
                    best_known = known_count[k - 1][i] + (1 if is_known else 0)
                    best_i = i
            dp[k][j] = best_val
            known_count[k][j] = best_known
            back[k][j] = best_i

    end_j = n_b - 1
    if dp[n_chars][end_j] == NEG:
        return None

    boundary_idx = [end_j]
    j = end_j
    for k in range(n_chars, 0, -1):
        i = back[k][j]
        if i < 0:
            return None
        boundary_idx.append(i)
        j = i
    boundary_idx.reverse()
    seg_runs = [(bounds[boundary_idx[k]], bounds[boundary_idx[k + 1]]) for k in range(n_chars)]

    sims = []
    for ch, (x0, x1) in zip(chars, seg_runs):
        glyph = _glyph_mask(img, x0, x1)
        if glyph.getchannel("A").getbbox() is None:
            return None
        sims.append(_glyph_similarity(glyph, atlas[ch]) if ch in atlas else None)

    known_sims = [s for s in sims if s is not None]
    unknown_count = len(sims) - len(known_sims)
    if not known_sims:
        return None
    min_sim = min(known_sims)
    avg_sim = sum(known_sims) / len(known_sims)
    if min_sim < 0.72 or avg_sim < 0.84:
        return None
    # An unknown glyph's boundary rests entirely on its known neighbours;
    # refuse to lean on that for more than a third of the label at once.
    if unknown_count > max(1, n_chars // 3):
        return None

    print(
        f"[{text}] segmented via glyph-atlas cross-validation: "
        f"runs={seg_runs} min_sim={min_sim:.3f} avg_sim={avg_sim:.3f}"
        + (f" ({unknown_count} newly-learned glyph(s))" if unknown_count else ""),
        file=sys.stderr,
    )
    return seg_runs


def build_rom_nameplate_glyphs(rom: bytes) -> dict:
    """
    Segment vanilla 96x10 MNMaps stage labels into a reusable glyph atlas.

    A naive one-alpha-run-per-character zip is trusted only when its run
    count matches and every run is a plausible single-glyph width (see
    ``_zip_validates`` -- pixel similarity is deliberately *not* used to
    gate this, since this font has real, harmless inter-label style
    variance, e.g. 'O'/'E' score a perfect 1.0 across labels while a
    correctly-segmented 'C' can legitimately score ~0.3 against another
    correctly-segmented 'C'; gating on that produced false-positive
    rejections).  Whenever the naive zip is not plausible,
    ``_segment_nameplate_runs`` searches for a fully cross-validated
    partition instead (touching pairs, internally-split glyphs, or both at
    once -- see its docstring).

    Source labels are *not* rendered output (only the STAGES entries with a
    ``name_text`` key are, and none of them are MNMaps labels), so a label
    is only a means of harvesting reusable letter shapes.  If no whole-label
    segmentation can be fully validated (this happens for real: MUSHROOM
    KINGDOM's S/H/R region renders unusually narrow in a way no other label
    corroborates, which is a separate phenomenon from the O+N/R-split cases
    this function does resolve), fall back to harvesting only the
    individual glyphs from the naive zip whose width is itself plausible,
    and skip the rest -- never guess a boundary, but also do not let one
    label's unrelated ambiguity block every other glyph it would otherwise
    safely contribute.  A character that no label ever safely resolves
    stays out of the atlas; ``render_name_png`` fails loudly if a stage
    that actually needs it is missing it.
    """
    file_data = extract_file(rom, NAMEPLATE_RELOC_FILE)
    atlas = {}

    def harvest(ch, x0, x1):
        # Never add an implausibly wide sample to the atlas, regardless of
        # which path produced it -- an unknown character inside a DP
        # recovery scores 0 (neutral) and so has no similarity-based
        # protection of its own; width is the backstop for that case too.
        if (x1 - x0) > 8:
            return
        glyph = _glyph_mask(img, x0, x1)
        # Prefer the widest sample for repeated letters; it is less likely
        # to have been tightly kerned/cropped by a neighbouring glyph.
        if ch not in atlas or glyph.width > atlas[ch].width:
            atlas[ch] = glyph

    for text, sprite_off in NAMEPLATE_SOURCES:
        img = decode_ia4_sprite(file_data, sprite_off)
        runs = _alpha_runs(img)
        chars = [c for c in text if c != " "]

        if _zip_validates(img, runs, chars):
            for ch, (x0, x1) in zip(chars, runs):
                harvest(ch, x0, x1)
            continue

        recovered = _segment_nameplate_runs(img, chars, atlas, text, sprite_off)
        if recovered is not None:
            for ch, (x0, x1) in zip(chars, recovered):
                harvest(ch, x0, x1)
            continue

        if len(runs) != len(chars):
            raise ValueError(
                f"MNMaps nameplate {text!r} @0x{sprite_off:X}: found {len(runs)} "
                f"glyph runs, expected {len(chars)}; no glyph-atlas-validated "
                f"segmentation found. Refusing host-font fallback."
            )
        skipped = [ch for ch, (x0, x1) in zip(chars, runs) if (x1 - x0) > 8]
        print(
            f"[{text}] no fully-validated segmentation found; harvesting only "
            f"the {len(chars) - len(skipped)} plausible-width glyph(s) from "
            f"the naive zip, skipping {skipped} (implausibly wide run(s))",
            file=sys.stderr,
        )
        for ch, (x0, x1) in zip(chars, runs):
            if (x1 - x0) <= 8:
                harvest(ch, x0, x1)

    # The vanilla stage names contain no V.  Seed only that missing letter from
    # the game's own menu font rather than from a host font.
    if "V" not in atlas:
        fonts = extract_file(rom, COMMON_FONTS_RELOC_FILE)
        v = decode_ia4_sprite(fonts, COMMON_FONTS_V_SPRITE_OFF)
        bbox = v.getchannel("A").getbbox()
        if bbox is not None:
            v = _glyph_mask(v, bbox[0], bbox[2])
            target_h = max(1, NAME_H - 2)
            if v.height != target_h:
                target_w = max(1, round(v.width * target_h / v.height))
                v = v.resize((target_w, target_h), Image.Resampling.NEAREST)
                canvas = Image.new("RGBA", (target_w, NAME_H), (0, 0, 0, 0))
                canvas.alpha_composite(v, (0, (NAME_H - target_h) // 2))
                v = canvas
            atlas["V"] = v

    return atlas


def render_name_png(text: str, output_path: Path, glyphs: dict) -> None:
    """Compose text from ROM-derived stage-name glyphs on a 96x10 canvas."""
    missing = sorted({ch for ch in text if ch != " " and ch not in glyphs})
    if missing:
        raise ValueError(
            f"Cannot render {text!r}: ROM-style glyph atlas is missing {missing}"
        )

    # A one-pixel gap reproduces the tight spacing of the pre-rendered stage
    # labels.  Word spacing is deliberately only three pixels because the 96px
    # plate is narrow and FINAL DESTINATION is longer than any vanilla label.
    pieces = []
    for ch in text:
        if ch == " ":
            pieces.append((None, 3))
        else:
            pieces.append((glyphs[ch], glyphs[ch].width))

    width = sum(w for _, w in pieces)
    letter_count = sum(1 for g, _ in pieces if g is not None)
    width += max(0, letter_count - 1)  # one pixel between adjacent letters

    work = Image.new("RGBA", (max(1, width), NAME_H), (0, 0, 0, 0))
    x = 0
    for idx, (glyph, advance) in enumerate(pieces):
        if glyph is None:
            x += advance
            continue
        y = (NAME_H - glyph.height) // 2
        work.alpha_composite(glyph, (x, y))
        x += advance
        # Add spacing only when another non-space glyph follows eventually.
        if any(g is not None for g, _ in pieces[idx + 1:]):
            x += 1

    bbox = work.getchannel("A").getbbox()
    if bbox is not None:
        work = work.crop((bbox[0], 0, bbox[2], NAME_H))

    # Long port-only names may exceed the fixed 96px N64 plate.  Preserve the
    # pixel-art appearance with nearest-neighbour horizontal compression only.
    max_text_w = NAME_W - 2
    if work.width > max_text_w:
        work = work.resize((max_text_w, NAME_H), Image.Resampling.NEAREST)

    img = Image.new("RGBA", (NAME_W, NAME_H), (0, 0, 0, 0))
    img.alpha_composite(work, ((NAME_W - work.width) // 2, 0))

    output_path.parent.mkdir(parents=True, exist_ok=True)
    img.save(output_path)
    print(
        f"[{text}] Saved ROM-style nameplate: {output_path} "
        f"({NAME_W}x{NAME_H}, {output_path.stat().st_size} bytes)",
        file=sys.stderr,
    )


# ---------------------------------------------------------------------------
# Emblem PNG extractor.
#
# Reads the source IA4 sprite from the reloc file specified in emblem_src,
# upscales it to EMBLEM_W x EMBLEM_H via LANCZOS, and saves an RGBA PNG.
#
# Target dimensions (EMBLEM_W=64, EMBLEM_H=48) are hard-coded because every
# llFTEmblemSprites* sprite in reloc file 20 is 64x48 IA4 (verified).
# ---------------------------------------------------------------------------


def extract_emblem(stage: dict, rom: bytes, output_dir: Path) -> None:
    """
    Extract and upscale the stage emblem sprite to EMBLEM_W x EMBLEM_H RGBA PNG.

    stage["emblem_src"] must contain:
        reloc_file: int  — reloc file index containing the source sprite
        sprite_off: int  — byte offset of the Sprite struct within that file
    """
    name = stage["name"]
    src  = stage["emblem_src"]
    reloc_file = src["reloc_file"]
    sprite_off = src["sprite_off"]

    print(
        f"[{name}] Extracting emblem from reloc file {reloc_file} "
        f"offset 0x{sprite_off:X}...",
        file=sys.stderr,
    )
    file_data = extract_file(rom, reloc_file)
    print(f"[{name}] Emblem file decompressed to {len(file_data)} bytes.", file=sys.stderr)

    sp = parse_sprite(file_data, sprite_off)
    src_w     = sp["width"]
    src_h     = sp["height"]
    nbitmaps  = sp["nbitmaps"]
    bmfmt     = sp["bmfmt"]
    bmsiz     = sp["bmsiz"]

    print(
        f"[{name}] Emblem source sprite: {src_w}x{src_h}, {nbitmaps} bitmap(s), "
        f"bmfmt={bmfmt} (4=IA), bmsiz={bmsiz} (0=IA4)",
        file=sys.stderr,
    )

    if bmfmt != 4 or bmsiz != 0:
        print(
            f"[{name}] WARNING: emblem sprite bmfmt={bmfmt}/bmsiz={bmsiz}, "
            f"expected IA4 (fmt=4, siz=0).  Decode may be wrong.",
            file=sys.stderr,
        )

    # Resolve bitmap array and get the single bitmap's buf.
    bitmaps = resolve_bitmap_offsets(file_data, sp)
    if not bitmaps:
        raise ValueError(f"[{name}] Emblem sprite has no bitmaps.")

    bm      = bitmaps[0]
    buf_off = bm["buf_off"]
    # IA4: width_img is the DMA/source row width; width is only how many
    # texels are drawn.  Use width_img for source addressing.
    source_width = bm["width_img"] if bm["width_img"] > 0 else bm["width"]
    row_stride = (source_width + 1) // 2
    raw_bytes  = row_stride * src_h

    if buf_off < 0 or buf_off + raw_bytes > len(file_data):
        raise ValueError(
            f"[{name}] Emblem bitmap buf_off=0x{buf_off:X} out of range "
            f"(file_data={len(file_data):#X}, raw_bytes={raw_bytes})"
        )

    raw_strip = file_data[buf_off:buf_off + raw_bytes]

    # Sprite-library IA4 data is pre-swizzled for TMEM LOAD_BLOCK. Mirror the
    # runtime Vita sprite fixup before decoding it linearly.
    raw_strip = unswizzle_ia4_strip(raw_strip, source_width, src_h)

    # Decode IA4 -> RGBA8888.
    rgba8 = ia4_to_rgba8888(raw_strip, src_w, src_h, source_width)

    # Build PIL image and upscale to EMBLEM_W x EMBLEM_H via LANCZOS.
    src_img = Image.frombytes("RGBA", (src_w, src_h), rgba8)
    emblem_img = src_img.resize((EMBLEM_W, EMBLEM_H), Image.LANCZOS)

    # The runtime packs this emblem to IA4 using the alpha channel as a
    # silhouette mask, matching the ROM FT emblem format exactly. SObj-side
    # colour modulation (0x5C/0x22/0x00 franchise brown) does the tinting at
    # draw time, just like the 9 ROM stage emblems — so no per-channel bake
    # here.

    emblem_path = output_dir / f"{name}_emblem.png"
    output_dir.mkdir(parents=True, exist_ok=True)
    emblem_img.save(emblem_path)

    filesize = emblem_path.stat().st_size
    print(
        f"[{name}] Saved emblem:    {emblem_path} "
        f"({src_w}x{src_h} -> {EMBLEM_W}x{EMBLEM_H}, {filesize} bytes)",
        file=sys.stderr,
    )


# ---------------------------------------------------------------------------
# Main extraction logic for a single stage.
# ---------------------------------------------------------------------------


def extract_stage(stage: dict, rom: bytes, output_dir: Path, nameplate_glyphs: dict) -> None:
    name       = stage["name"]
    reloc_file = stage["reloc_file"]
    sprite_off = stage["sprite_off"]

    print(f"[{name}] Extracting reloc file {reloc_file}...", file=sys.stderr)
    file_data = extract_file(rom, reloc_file)
    print(f"[{name}] Decompressed to {len(file_data)} bytes.", file=sys.stderr)

    # Parse the Sprite struct.
    sp = parse_sprite(file_data, sprite_off)
    w         = sp["width"]
    h         = sp["height"]
    nbitmaps  = sp["nbitmaps"]
    bmheight  = sp["bmheight"]
    bmsiz     = sp["bmsiz"]   # 2 -> G_IM_SIZ_16b (RGBA16)

    print(
        f"[{name}] Sprite: {w}x{h}, {nbitmaps} bitmaps, bmheight={bmheight}, "
        f"bmHreal={sp['bmHreal']}, bmfmt={sp['bmfmt']}, bmsiz={bmsiz}",
        file=sys.stderr,
    )

    if bmsiz != 2:
        print(
            f"[{name}] WARNING: bmsiz={bmsiz} (expected 2 for RGBA16). "
            f"Only RGBA16 is supported; output may be wrong.",
            file=sys.stderr,
        )

    # Resolve bitmap buffer offsets.
    bitmaps = resolve_bitmap_offsets(file_data, sp)

    # Decode each bitmap strip into RGBA rows.
    rows_rgba = []
    for i, bm in enumerate(bitmaps):
        strip_w     = bm["width"]
        strip_h     = bm["actualHeight"]
        buf_off     = bm["buf_off"]
        strip_bytes = strip_w * strip_h * 2  # 2 bytes per RGBA16 pixel

        if buf_off < 0 or buf_off + strip_bytes > len(file_data):
            raise ValueError(
                f"[{name}] bitmap[{i}].buf_off=0x{buf_off:X} out of range "
                f"(file_data length=0x{len(file_data):X}, strip_bytes={strip_bytes})"
            )

        raw_strip = file_data[buf_off:buf_off + strip_bytes]

        # Un-swizzle TMEM odd-row XOR4 pattern (16bpp, strip-local indexing).
        unswizzled = unswizzle_rgba16_strip(raw_strip, strip_w, strip_h)

        # Decode RGBA16 BE -> RGBA8888.
        rgba = rgba16_to_rgba8888(unswizzled)

        # Split into individual rows and accumulate (only bmheight rendered rows).
        row_bytes = strip_w * 4  # 4 bytes per pixel in RGBA8888
        rendered_rows = min(bmheight, strip_h)
        for row in range(rendered_rows):
            rows_rgba.append(rgba[row * row_bytes:(row + 1) * row_bytes])

    # Sanity check.
    if len(rows_rgba) != h:
        print(
            f"[{name}] WARNING: assembled {len(rows_rgba)} rows, expected {h}. "
            f"Output may be cropped or padded.",
            file=sys.stderr,
        )

    # Build a PIL Image from the decoded rows.
    img_data = b"".join(rows_rgba[:h])
    img = Image.frombytes("RGBA", (w, h), img_data)

    # Save full-resolution background PNG.
    bg_path = output_dir / f"{name}_background.png"
    output_dir.mkdir(parents=True, exist_ok=True)
    img.save(bg_path)
    bg_size = bg_path.stat().st_size
    print(
        f"[{name}] Saved background: {bg_path} ({w}x{h}, {bg_size} bytes)",
        file=sys.stderr,
    )

    # Downscale to 48x36 for the CSS icon thumbnail.
    # Use a center-crop if the aspect ratio doesn't match 4:3, then LANCZOS resize.
    target_w, target_h = ICON_W, ICON_H
    src_aspect = w / h
    tgt_aspect = target_w / target_h

    if abs(src_aspect - tgt_aspect) > 0.01:
        # Center-crop to match target aspect ratio.
        if src_aspect > tgt_aspect:
            # Image is wider -> crop width.
            new_w = int(h * tgt_aspect)
            left  = (w - new_w) // 2
            img_crop = img.crop((left, 0, left + new_w, h))
        else:
            # Image is taller -> crop height.
            new_h = int(w / tgt_aspect)
            top   = (h - new_h) // 2
            img_crop = img.crop((0, top, w, top + new_h))
    else:
        img_crop = img

    img_small = img_crop.resize((target_w, target_h), Image.LANCZOS)
    small_path = output_dir / f"{name}_small.png"
    img_small.save(small_path)
    small_size = small_path.stat().st_size
    print(
        f"[{name}] Saved icon:       {small_path} ({target_w}x{target_h}, {small_size} bytes)",
        file=sys.stderr,
    )

    # Render synthetic nameplate PNG if the stage has a name_text key.
    name_text = stage.get("name_text")
    if name_text:
        name_path = output_dir / f"{name}_name.png"
        render_name_png(name_text, name_path, nameplate_glyphs)

    # Extract and upscale emblem PNG if the stage has an emblem_src key.
    emblem_src = stage.get("emblem_src")
    if emblem_src:
        extract_emblem(stage, rom, output_dir)


# ---------------------------------------------------------------------------
# CLI entry point.
# ---------------------------------------------------------------------------


def main(argv: list) -> int:
    if len(argv) != 2:
        print(
            f"Usage: {Path(__file__).name} <baserom.z64> <output_dir>",
            file=sys.stderr,
        )
        return 2

    rom_path = Path(argv[0])
    out_dir  = Path(argv[1])

    if not rom_path.exists():
        print(f"ERROR: ROM not found: {rom_path}", file=sys.stderr)
        return 1

    rom = rom_path.read_bytes()
    if rom[:4] != bytes([0x80, 0x37, 0x12, 0x40]):
        print(
            f"WARNING: ROM header is not .z64 ({rom[:4].hex()}). "
            "Continuing anyway.",
            file=sys.stderr,
        )

    try:
        nameplate_glyphs = build_rom_nameplate_glyphs(rom)
        print(
            "ROM-style CSS glyph atlas: " + "".join(sorted(nameplate_glyphs.keys())),
            file=sys.stderr,
        )
    except Exception as exc:
        print(f"ERROR building ROM-style stage-name glyph atlas: {exc}", file=sys.stderr)
        return 1

    errors = 0
    for stage in STAGES:
        try:
            extract_stage(stage, rom, out_dir, nameplate_glyphs)
        except Exception as exc:
            print(f"ERROR [{stage['name']}]: {exc}", file=sys.stderr)
            errors += 1

    if errors:
        print(f"\n{errors} stage(s) failed.", file=sys.stderr)
        return 1

    print(f"\nAll {len(STAGES)} stage(s) extracted successfully.", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
