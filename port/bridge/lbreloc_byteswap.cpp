/**
 * lbreloc_byteswap.cpp — Big-endian to native byte-swap for reloc file blobs
 *
 * N64 reloc files are stored in big-endian byte order. On little-endian PC,
 * every multi-byte field is read incorrectly. This module performs a two-pass
 * byte-swap:
 *
 *   Pass 1: Blanket u32 swap of the entire blob. This correctly handles
 *           display list commands, float/int struct fields, reloc chain
 *           descriptors, 32bpp texture pixels, and zeros.
 *
 *   Pass 2: Parse the now-native-endian display list commands to identify
 *           vertex buffers and texture/palette regions, then apply targeted
 *           fixups for data types that u32 swap got wrong.
 *
 * Called from lbRelocLoadAndRelocFile() between memcpy and the reloc chain walk.
 */

#include "bridge/lbreloc_byteswap.h"
#include "resource/RelocPointerTable.h"
#include "resource/RelocFileTable.h"

#include <ship/utils/binarytools/endianness.h>
#include <spdlog/spdlog.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <iterator>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <vector>

extern "C" void port_log(const char *fmt, ...);
extern "C" int  portRelocFindContainingFile(const void *ptr, uintptr_t *out_base, size_t *out_size);
extern "C" int  portRelocFindFileIdAndBase(const void *ptr, uintptr_t *out_base);
extern "C" bool portRelocDescribePointer(const void *ptr, uintptr_t *out_base, size_t *out_size,
                                          unsigned int *out_file_id, const char **out_path);

// ============================================================
//  Stage audit (SSB64_STAGE_AUDIT=1)
// ============================================================
//
// Per-file counters for the byteswap + reloc-chain fixup pipeline.
// Reset at the top of portRelocByteSwapBlob; emitted as a single
// port_log line by portStageAuditEmitLoadSummary after the reloc
// chain walk completes. Helps identify stage files whose G_VTX /
// G_SETTIMG targets the DL scan skipped AND the chain walk failed
// to recover.

static int sStageAuditState = 0;  // 0=unchecked, 1=on, -1=off

struct StageAuditCounters {
	uint32_t vtx_scan_caught      = 0;
	uint32_t vtx_scan_skipped_seg = 0;
	uint32_t settimg_scan_caught  = 0;
	uint32_t settimg_scan_skipped_seg = 0;
	uint32_t chain_vtx_fixups     = 0;
	uint32_t chain_settimg_fixups = 0;
	uint32_t runtime_vtx_fixups   = 0;
	uint32_t runtime_tex_fixups   = 0;
};

static StageAuditCounters sStageAudit;
static uint64_t sStageAuditRuntimeVtxTotal = 0;
static uint64_t sStageAuditRuntimeTexTotal = 0;
/* Total dispatches regardless of reloc-file membership vs heap-only. */
static uint64_t sStageAuditRuntimeVtxDispatches = 0;
static uint64_t sStageAuditRuntimeVtxHeap = 0;
static uint64_t sStageAuditRuntimeTexDispatches = 0;
static uint64_t sStageAuditRuntimeTexHeap = 0;

static bool stage_audit_enabled()
{
	if (sStageAuditState == 0) {
		const char *e = std::getenv("SSB64_STAGE_AUDIT");
		sStageAuditState = (e != nullptr && e[0] == '1') ? 1 : -1;
	}
	return sStageAuditState == 1;
}

static inline void stage_audit_reset_per_file()
{
	sStageAudit = StageAuditCounters{};
}

/* Count u32 words at 8-byte-aligned (w0) positions whose high byte matches
 * the given opcode — diagnostic for F3D vs F3DEX2 discrimination. */
static uint32_t count_opcode_matches(const void *data, size_t size, uint8_t opcode)
{
	const uint32_t *words = static_cast<const uint32_t *>(data);
	size_t n = size / 4;
	uint32_t hits = 0;
	for (size_t i = 0; i + 1 < n; i += 2) {
		if (((words[i] >> 24) & 0xFF) == opcode) hits++;
	}
	return hits;
}

/* Same as count_opcode_matches but steps by 1 word, so it also catches
 * commands whose (w0, w1) pair lands at an odd word offset within the file. */
static uint32_t count_opcode_matches_any_phase(const void *data, size_t size, uint8_t opcode)
{
	const uint32_t *words = static_cast<const uint32_t *>(data);
	size_t n = size / 4;
	uint32_t hits = 0;
	for (size_t i = 0; i + 1 < n; i += 1) {
		if (((words[i] >> 24) & 0xFF) == opcode) hits++;
	}
	return hits;
}

extern "C" void portStageAuditEmitLoadSummary(unsigned int file_id, const char *path, size_t size)
{
	if (!stage_audit_enabled()) return;
	port_log("[STAGE_AUDIT] file=%3u size=0x%06zx vtx_scan=%u/%u settimg_scan=%u/%u chain=vtx:%u,st:%u path=%s\n",
	         file_id, size,
	         sStageAudit.vtx_scan_caught, sStageAudit.vtx_scan_skipped_seg,
	         sStageAudit.settimg_scan_caught, sStageAudit.settimg_scan_skipped_seg,
	         sStageAudit.chain_vtx_fixups, sStageAudit.chain_settimg_fixups,
	         path ? path : "(null)");
	stage_audit_reset_per_file();
}

/* Separate entry point for opcode-census diagnostic. Call after the load
 * summary from the bridge. */
extern "C" void portStageAuditEmitOpcodeCensus(unsigned int file_id, const char *path,
                                                const void *data, size_t size)
{
	if (!stage_audit_enabled() || data == nullptr || size < 8) return;
	/* F3D: G_VTX=0x01, G_MTX=0x01 is ALSO MTX-first in F3DEX2 — use the
	 * two cases that diverge most strongly to tell the formats apart.
	 * F3D     : G_VTX=0x01  G_ENDDL=0xB8  G_DL=0x06
	 * F3DEX2  : G_VTX=0x04  G_ENDDL=0xDF  G_DL=0x06
	 */
	uint32_t f3d_vtx    = count_opcode_matches(data, size, 0x01);
	uint32_t f3d_enddl  = count_opcode_matches(data, size, 0xB8);
	uint32_t f3dex2_vtx = count_opcode_matches(data, size, 0x04);
	uint32_t f3dex2_end = count_opcode_matches(data, size, 0xDF);
	/* Any-phase variants: step by 1 word rather than 2. If DL data is at
	 * an odd-word offset within the file, the even-phase scan misses every
	 * command. The difference between *_any and the scan counts flags that. */
	uint32_t f3d_vtx_any    = count_opcode_matches_any_phase(data, size, 0x01);
	uint32_t f3d_end_any    = count_opcode_matches_any_phase(data, size, 0xB8);
	uint32_t f3dex2_vtx_any = count_opcode_matches_any_phase(data, size, 0x04);
	uint32_t f3dex2_end_any = count_opcode_matches_any_phase(data, size, 0xDF);
	port_log("[STAGE_AUDIT_GBI] file=%3u f3d:vtx=%u(%u),end=%u(%u)  f3dex2:vtx=%u(%u),end=%u(%u)  path=%s\n",
	         file_id, f3d_vtx, f3d_vtx_any, f3d_enddl, f3d_end_any,
	         f3dex2_vtx, f3dex2_vtx_any, f3dex2_end, f3dex2_end_any,
	         path ? path : "(null)");

	/* Stage-file deep inspection: byte histogram + first bytes to understand layout. */
	if (path != nullptr && std::strstr(path, "reloc_stages/Stage") != nullptr) {
		const uint8_t *p = static_cast<const uint8_t *>(data);
		/* Byte-0 histogram: count occurrences at each (byte_offset % 4) position. */
		uint32_t byte0_hits[256] = {0};
		uint32_t byte3_hits[256] = {0};
		for (size_t i = 0; i + 3 < size; i += 4) {
			byte0_hits[p[i]]++;
			byte3_hits[p[i + 3]]++;
		}
		port_log("[STAGE_AUDIT_HIST] file=%3u  byte0: 0x04=%u 0xDF=%u 0xF3=%u 0xFD=%u 0xB8=%u  byte3: 0x04=%u 0xDF=%u 0xF3=%u 0xFD=%u 0xB8=%u\n",
		         file_id,
		         byte0_hits[0x04], byte0_hits[0xDF], byte0_hits[0xF3], byte0_hits[0xFD], byte0_hits[0xB8],
		         byte3_hits[0x04], byte3_hits[0xDF], byte3_hits[0xF3], byte3_hits[0xFD], byte3_hits[0xB8]);
		size_t dump = size < 128 ? size : 128;
		char hex[512];
		size_t pos = 0;
		for (size_t i = 0; i < dump && pos + 4 < sizeof(hex); i++) {
			pos += std::snprintf(hex + pos, sizeof(hex) - pos, "%02X", p[i]);
			if ((i & 3) == 3 && pos + 1 < sizeof(hex)) hex[pos++] = ' ';
		}
		hex[pos] = '\0';
		port_log("[STAGE_AUDIT_HEX] file=%3u first128=%s\n", file_id, hex);
	}
}

extern "C" void portStageAuditNoteRuntimeVtx(unsigned int num_vtx)
{
	if (!stage_audit_enabled()) return;
	sStageAuditRuntimeVtxTotal += num_vtx;
}

extern "C" void portStageAuditNoteRuntimeTex(unsigned int num_bytes)
{
	if (!stage_audit_enabled()) return;
	sStageAuditRuntimeTexTotal += num_bytes;
}

/* Per-file dispatch counters so we can see exactly which reloc files
 * supply vertex data during gameplay. */
static std::unordered_map<int, uint64_t> sStageAuditVtxPerFile;
static std::unordered_map<int, uint64_t> sStageAuditTexPerFile;

static void stage_audit_dump_per_file_vtx()
{
	/* Sort by count desc, emit as one line. */
	std::vector<std::pair<int, uint64_t>> v(sStageAuditVtxPerFile.begin(), sStageAuditVtxPerFile.end());
	std::sort(v.begin(), v.end(), [](auto &a, auto &b){ return a.second > b.second; });
	for (auto &p : v) {
		const char *path = (p.first >= 0) ? gRelocFileTable[p.first] : "(heap)";
		port_log("[STAGE_AUDIT] vtx_per_file: file=%4d hits=%llu path=%s\n",
		         p.first, (unsigned long long)p.second, path ? path : "(null)");
	}
}

extern "C" void portStageAuditNoteVtxDispatch(int in_reloc_file)
{
	if (!stage_audit_enabled()) return;
	sStageAuditRuntimeVtxDispatches++;
	if (!in_reloc_file) sStageAuditRuntimeVtxHeap++;
	if ((sStageAuditRuntimeVtxDispatches & 0xFFF) == 0) {
		port_log("[STAGE_AUDIT] runtime: vtx_disp=%llu vtx_heap=%llu vtx_fix=%llu  tex_disp=%llu tex_heap=%llu tex_fix=%llu\n",
		         (unsigned long long)sStageAuditRuntimeVtxDispatches,
		         (unsigned long long)sStageAuditRuntimeVtxHeap,
		         (unsigned long long)sStageAuditRuntimeVtxTotal,
		         (unsigned long long)sStageAuditRuntimeTexDispatches,
		         (unsigned long long)sStageAuditRuntimeTexHeap,
		         (unsigned long long)sStageAuditRuntimeTexTotal);
		stage_audit_dump_per_file_vtx();
	}
}

extern "C" void portStageAuditNoteVtxDispatchFile(int file_id)
{
	if (!stage_audit_enabled()) return;
	sStageAuditVtxPerFile[file_id]++;
}

extern "C" void portStageAuditNoteTexDispatchFile(int file_id)
{
	if (!stage_audit_enabled()) return;
	sStageAuditTexPerFile[file_id]++;
}

extern "C" void portStageAuditNoteTexDispatch(int in_reloc_file)
{
	if (!stage_audit_enabled()) return;
	sStageAuditRuntimeTexDispatches++;
	if (!in_reloc_file) sStageAuditRuntimeTexHeap++;
}

// ============================================================
//  Texture-fixup diagnostic log (SSB64_TEX_FIXUP_LOG=1)
// ============================================================
//
// One-line entry per byteswap-side texture fixup invocation. See
// portRelocTexFixupLog() in lbreloc_byteswap.h for the user-facing knobs.
// All entries are interleaved into debug_traces/tex_fixup.log so chain /
// pass2 / runtime / heap / sprite paths can be diffed and grepped together.
//
// Initialization is lazy and once-only. The log is opened on the first call
// after the env var has been read.

static int  sTexLogState = 0;        // 0=uninit, 1=enabled, -1=disabled
static FILE *sTexLogFile = nullptr;
static int  sTexLogFileFilter = -1;  // -1 = no filter

static void tex_log_init_once()
{
	if (sTexLogState != 0) return;
	const char *env = std::getenv("SSB64_TEX_FIXUP_LOG");
	if (env == nullptr || env[0] == '0' || env[0] == '\0') {
		sTexLogState = -1;
		return;
	}
	sTexLogFile = std::fopen("debug_traces/tex_fixup.log", "w");
	if (sTexLogFile == nullptr) {
		// Fall back to stderr so we never silently swallow the request.
		sTexLogFile = stderr;
	}
	const char *fenv = std::getenv("SSB64_TEX_FIXUP_LOG_FILE_ID");
	if (fenv != nullptr && fenv[0] != '\0') {
		sTexLogFileFilter = (int)std::strtol(fenv, nullptr, 0);
	}
	sTexLogState = 1;
	std::fprintf(sTexLogFile,
		"# tex_fixup.log — SSB64 texture byteswap-fixup trace\n"
		"# columns: [path] file=<id> off=0x<file_off> size=<bytes> "
		"fmt=<n> siz=<n> bpp=<n> first16=<hex> note=<...>\n");
	std::fflush(sTexLogFile);
}

static inline bool tex_log_enabled()
{
	if (sTexLogState == 0) tex_log_init_once();
	return sTexLogState == 1;
}

static void tex_log_first16(char *out, size_t out_sz, const void *data, size_t bytes)
{
	if (data == nullptr || bytes == 0 || out_sz < 3) {
		if (out_sz > 0) out[0] = '-';
		if (out_sz > 1) out[1] = '\0';
		return;
	}
	size_t n = bytes < 16 ? bytes : 16;
	const uint8_t *p = static_cast<const uint8_t *>(data);
	size_t pos = 0;
	for (size_t i = 0; i < n && pos + 3 < out_sz; i++) {
		std::snprintf(out + pos, out_sz - pos, "%02X", p[i]);
		pos += 2;
	}
	if (pos < out_sz) out[pos] = '\0';
}

// Centralized emit. Caller has already pre-formatted everything except the
// optional first16 hex (which we never want to compute when the log is off).
static void tex_log_emit(const char *path, int file_id,
                         uint32_t file_off, uint32_t size_bytes,
                         int fmt, int siz, int bpp,
                         const void *first_bytes,
                         const char *note)
{
	if (!tex_log_enabled()) return;
	if (sTexLogFileFilter >= 0 && file_id != sTexLogFileFilter) return;

	char hex[40];
	tex_log_first16(hex, sizeof(hex), first_bytes, size_bytes);

	char fmt_buf[16] = "?";
	if (fmt >= 0) std::snprintf(fmt_buf, sizeof(fmt_buf), "%d", fmt);
	char siz_buf[16] = "?";
	if (siz >= 0) std::snprintf(siz_buf, sizeof(siz_buf), "%d", siz);
	char bpp_buf[16] = "?";
	if (bpp >= 0) std::snprintf(bpp_buf, sizeof(bpp_buf), "%d", bpp);
	char file_buf[24];
	if (file_id < 0) std::snprintf(file_buf, sizeof(file_buf), "heap");
	else             std::snprintf(file_buf, sizeof(file_buf), "%d", file_id);

	std::fprintf(sTexLogFile,
		"[%-12s] file=%-6s off=0x%06X size=%-6u fmt=%s siz=%s bpp=%s first16=%s%s%s\n",
		path, file_buf, file_off, size_bytes,
		fmt_buf, siz_buf, bpp_buf, hex,
		note ? " note=" : "", note ? note : "");
	std::fflush(sTexLogFile);
}

// Public C-callable variadic logger declared in lbreloc_byteswap.h.
// Currently unused by external code but exposed so future hooks (e.g. an
// interpreter-side LoadBlock context line) can interleave entries into the
// same log without re-implementing the gating.
extern "C" void portRelocTexFixupLog(const char *fmt, ...)
{
	if (!tex_log_enabled()) return;
	std::va_list ap;
	va_start(ap, fmt);
	std::vfprintf(sTexLogFile, fmt, ap);
	va_end(ap);
	std::fflush(sTexLogFile);
}

// ============================================================
//  Texture PNG dump (SSB64_TEX_DUMP=1)
// ============================================================
//
// One-shot per texture, fired from chain_fixup_settimg and
// portFixupSpriteBitmapData immediately after the byteswap fix is applied.
// The data passed in is in N64 big-endian byte order — i.e. the same layout
// Fast3D's ImportTexture* readers consume — so what we decode here is what
// the renderer will eventually upload as RGBA8.
//
// The chain path doesn't know the texture's actual width/height (LOADBLOCK
// only encodes the total texel count), so for chain-path dumps we emit one
// TGA per plausible width in {8,16,32,64,128} that divides the pixel count
// evenly.  Sprite-path dumps have real (width_img × actualHeight) so they
// emit one image at the correct dimensions.
//
// Filename: tex_dump/f<file>_o<offset>_fmt<f>siz<s>_w<w>_h<h>.tga
//
// TGA format: uncompressed 32-bit BGRA, top-down.  Header is 18 bytes;
// supported by every image viewer.

static int sTexDumpState = 0;        // 0=uninit, 1=enabled, -1=disabled

static void tex_dump_init_once()
{
	if (sTexDumpState != 0) return;
	const char *env = std::getenv("SSB64_TEX_DUMP");
	if (env == nullptr || env[0] == '0' || env[0] == '\0') {
		sTexDumpState = -1;
		return;
	}
	// Best-effort directory create.  On Windows the binary's CWD is
	// build/Debug, so this lands at build/Debug/tex_dump/.
#if defined(_WIN32)
	std::system("if not exist tex_dump mkdir tex_dump");
#else
	std::system("mkdir -p tex_dump");
#endif
	sTexDumpState = 1;
}

static inline bool tex_dump_enabled()
{
	if (sTexDumpState == 0) tex_dump_init_once();
	return sTexDumpState == 1;
}

// Write one 32-bit RGBA texel buffer (already in r,g,b,a byte order) as a
// TGA image.  TGA uses BGRA byte order so we swizzle on the way out.
static void dump_tga_rgba(const char *path,
                          const uint8_t *rgba, uint32_t w, uint32_t h)
{
	FILE *f = std::fopen(path, "wb");
	if (f == nullptr) return;
	uint8_t hdr[18] = {0};
	hdr[2]  = 2;                  // uncompressed truecolor
	hdr[12] = (uint8_t)(w & 0xFF);
	hdr[13] = (uint8_t)((w >> 8) & 0xFF);
	hdr[14] = (uint8_t)(h & 0xFF);
	hdr[15] = (uint8_t)((h >> 8) & 0xFF);
	hdr[16] = 32;                 // 32 bpp
	hdr[17] = 0x28;               // top-down, 8 alpha bits
	std::fwrite(hdr, 1, 18, f);
	std::vector<uint8_t> bgra(static_cast<size_t>(w) * h * 4);
	for (size_t i = 0; i < (size_t)w * h; i++) {
		bgra[i * 4 + 0] = rgba[i * 4 + 2]; // B
		bgra[i * 4 + 1] = rgba[i * 4 + 1]; // G
		bgra[i * 4 + 2] = rgba[i * 4 + 0]; // R
		bgra[i * 4 + 3] = rgba[i * 4 + 3]; // A
	}
	std::fwrite(bgra.data(), 1, bgra.size(), f);
	std::fclose(f);
}

// Decode N64 texel data (in BE byte order, post fixup) into a flat RGBA8
// buffer.  Returns true if (fmt,siz) is supported and the byte count makes
// sense for the requested dimensions.  CI formats are decoded with a
// fabricated greyscale palette since we have no TLUT context here.
static bool decode_to_rgba8(const uint8_t *src, size_t src_bytes,
                            int fmt, int siz,
                            uint32_t width, uint32_t height,
                            std::vector<uint8_t> &out_rgba)
{
	size_t need_pixels = (size_t)width * height;
	out_rgba.assign(need_pixels * 4, 0);

	auto scale_5_8 = [](uint8_t v) -> uint8_t {
		return (uint8_t)((v << 3) | (v >> 2));
	};
	auto scale_4_8 = [](uint8_t v) -> uint8_t {
		return (uint8_t)((v << 4) | v);
	};
	auto scale_3_8 = [](uint8_t v) -> uint8_t {
		return (uint8_t)((v << 5) | (v << 2) | (v >> 1));
	};

	// siz: 0=4b, 1=8b, 2=16b, 3=32b
	if (siz == 3) {
		// RGBA32: 4 bytes per pixel, byte order R G B A
		size_t need = need_pixels * 4;
		if (need > src_bytes) return false;
		for (size_t i = 0; i < need_pixels; i++) {
			out_rgba[i * 4 + 0] = src[i * 4 + 0];
			out_rgba[i * 4 + 1] = src[i * 4 + 1];
			out_rgba[i * 4 + 2] = src[i * 4 + 2];
			out_rgba[i * 4 + 3] = src[i * 4 + 3];
		}
		return true;
	}
	if (siz == 2) {
		// 16-bit per pixel, BE.  fmt=0:RGBA5551, fmt=3:IA88, fmt=4:I16, others:RGBA5551
		size_t need = need_pixels * 2;
		if (need > src_bytes) return false;
		for (size_t i = 0; i < need_pixels; i++) {
			uint16_t c = ((uint16_t)src[i * 2] << 8) | src[i * 2 + 1];
			if (fmt == 3) {
				// IA88 (16b): hi=intensity, lo=alpha
				uint8_t I = (uint8_t)(c >> 8);
				uint8_t A = (uint8_t)(c & 0xFF);
				out_rgba[i * 4 + 0] = I;
				out_rgba[i * 4 + 1] = I;
				out_rgba[i * 4 + 2] = I;
				out_rgba[i * 4 + 3] = A;
			} else if (fmt == 4) {
				// I16 isn't a real RDP format — treat as IA88 fallback
				uint8_t I = (uint8_t)(c >> 8);
				out_rgba[i * 4 + 0] = I;
				out_rgba[i * 4 + 1] = I;
				out_rgba[i * 4 + 2] = I;
				out_rgba[i * 4 + 3] = 255;
			} else {
				// RGBA5551 (and CI16 falls back here)
				uint8_t r = (c >> 11) & 0x1F;
				uint8_t g = (c >>  6) & 0x1F;
				uint8_t b = (c >>  1) & 0x1F;
				uint8_t a = c & 1;
				out_rgba[i * 4 + 0] = scale_5_8(r);
				out_rgba[i * 4 + 1] = scale_5_8(g);
				out_rgba[i * 4 + 2] = scale_5_8(b);
				out_rgba[i * 4 + 3] = a ? 255 : 0;
			}
		}
		return true;
	}
	if (siz == 1) {
		// 8b per pixel.  fmt=2:CI8 (no palette here → grayscale),
		// fmt=3:IA44, fmt=4:I8
		if (need_pixels > src_bytes) return false;
		for (size_t i = 0; i < need_pixels; i++) {
			uint8_t v = src[i];
			if (fmt == 3) {
				uint8_t I = (v >> 4) & 0xF;
				uint8_t A = v & 0xF;
				out_rgba[i * 4 + 0] = scale_4_8(I);
				out_rgba[i * 4 + 1] = scale_4_8(I);
				out_rgba[i * 4 + 2] = scale_4_8(I);
				out_rgba[i * 4 + 3] = scale_4_8(A);
			} else {
				out_rgba[i * 4 + 0] = v;
				out_rgba[i * 4 + 1] = v;
				out_rgba[i * 4 + 2] = v;
				out_rgba[i * 4 + 3] = 255;
			}
		}
		return true;
	}
	if (siz == 0) {
		// 4b per pixel.  fmt=2:CI4 (no palette → grayscale),
		// fmt=3:IA31, fmt=4:I4
		if ((need_pixels + 1) / 2 > src_bytes) return false;
		for (size_t i = 0; i < need_pixels; i++) {
			uint8_t byte = src[i / 2];
			uint8_t nib = (i & 1) ? (byte & 0x0F) : (byte >> 4);
			if (fmt == 3) {
				uint8_t I = (nib >> 1) & 0x7;
				uint8_t A = nib & 1;
				out_rgba[i * 4 + 0] = scale_3_8(I);
				out_rgba[i * 4 + 1] = scale_3_8(I);
				out_rgba[i * 4 + 2] = scale_3_8(I);
				out_rgba[i * 4 + 3] = A ? 255 : 0;
			} else {
				uint8_t v = scale_4_8(nib);
				out_rgba[i * 4 + 0] = v;
				out_rgba[i * 4 + 1] = v;
				out_rgba[i * 4 + 2] = v;
				out_rgba[i * 4 + 3] = 255;
			}
		}
		return true;
	}
	return false;
}

// Dump a texture for which we know the width and height (sprite path).
static void tex_dump_known_dims(int file_id, uint32_t file_off,
                                int fmt, int siz, int bpp,
                                const uint8_t *src, size_t src_bytes,
                                uint32_t w, uint32_t h)
{
	if (!tex_dump_enabled()) return;
	std::vector<uint8_t> rgba;
	if (!decode_to_rgba8(src, src_bytes, fmt, siz, w, h, rgba)) return;
	char path[256];
	std::snprintf(path, sizeof(path),
	              "tex_dump/sprite_f%d_o0x%06X_fmt%dsiz%dbpp%d_w%u_h%u.tga",
	              file_id, file_off, fmt, siz, bpp, w, h);
	dump_tga_rgba(path, rgba.data(), w, h);
}

// Dump a chain-path texture across plausible widths.
static void tex_dump_chain(int file_id, uint32_t file_off,
                           int fmt, int siz, uint32_t bpp,
                           const uint8_t *src, size_t src_bytes)
{
	if (!tex_dump_enabled()) return;
	if (bpp == 0 || src_bytes == 0) return;

	size_t total_bits = src_bytes * 8;
	if (total_bits % bpp != 0) return;
	size_t pixels = total_bits / bpp;
	if (pixels == 0 || pixels > 256 * 256) return;

	static const uint32_t kCandidateWidths[] = {8, 16, 32, 64, 128, 256};
	int written = 0;
	for (uint32_t w : kCandidateWidths) {
		if ((size_t)w > pixels) break;
		if (pixels % w != 0) continue;
		uint32_t h = (uint32_t)(pixels / w);
		if (h > 256) continue;

		std::vector<uint8_t> rgba;
		if (!decode_to_rgba8(src, src_bytes, fmt, siz, w, h, rgba)) continue;
		char path[256];
		std::snprintf(path, sizeof(path),
		              "tex_dump/chain_f%d_o0x%06X_fmt%dsiz%dbpp%u_w%u_h%u.tga",
		              file_id, file_off, fmt, siz, bpp, w, h);
		dump_tga_rgba(path, rgba.data(), w, h);
		if (++written >= 4) break; // cap to keep file count manageable
	}
}

// Tracks which (base + offset) regions have been fixed to prevent
// double-fixup. Used by per-struct fixups (Sprite, Bitmap, MObjSub, etc.),
// pass2 vertex fixups, the chain-walk texture fixup, and the runtime
// lazy vertex fixup. All paths share this set so they don't undo each
// other's work. Cleared at scene change via portResetStructFixups.
static std::unordered_set<uintptr_t> sStructU16Fixups;

// Vertex fixups must not share idempotency state with Sprite/MObjSub/DObjDesc
// struct fixups.  A reused address can legitimately be interpreted as a
// different data type later in the scene; sharing one set lets an unrelated
// struct fixup suppress the first real G_VTX normalization at that address.
static std::unordered_set<uintptr_t> sVertexFixups;

// Tracks runtime texture bases that own at least one fixed word, plus the
// largest request size seen from that base. This preserves the old pass2/chain
// skip behavior for exact-base matches while sTexFixupRanges handles the actual
// overlap-safe idempotency.
static std::unordered_map<uintptr_t, unsigned int> sTexFixupExtent;

// Runtime texture/TLUT fixups can overlap even when they start at different
// addresses. CSS gate palettes are adjacent 0x28-byte blocks, but their Sprite
// requests a 512-byte TLUT load from each block. Track each u32 word so the
// second overlapping load skips words fixed by the first instead of BSWAPing
// them back to the wrong byte order.
//
// This used to be an unordered_set with one entry and one hash lookup per u32.
// Razor measured millions of lookups in a handful of frames: the prime-bucket
// modulo alone consumed most of the Fast3D command walk on Vita. Store merged,
// half-open byte ranges instead. Runtime requests are normally repeat loads of
// the same texture, so one O(log N) containment test now replaces thousands of
// hash lookups; partially-overlapping requests still swap only uncovered gaps.
static std::map<uintptr_t, uintptr_t> sTexFixupRanges;

static void tex_fixup_ranges_insert(uintptr_t begin, uintptr_t end)
{
	if (begin >= end)
		return;

	auto it = sTexFixupRanges.lower_bound(begin);
	if (it != sTexFixupRanges.begin())
	{
		auto prev = std::prev(it);
		if (prev->second >= begin)
			it = prev;
	}

	while (it != sTexFixupRanges.end() && it->first <= end)
	{
		if (it->second < begin)
		{
			++it;
			continue;
		}
		begin = std::min(begin, it->first);
		end = std::max(end, it->second);
		it = sTexFixupRanges.erase(it);
	}
	sTexFixupRanges.emplace(begin, end);
}

static bool tex_fixup_ranges_contains(uintptr_t begin, uintptr_t end)
{
	if (begin >= end)
		return true;
	auto it = sTexFixupRanges.upper_bound(begin);
	if (it == sTexFixupRanges.begin())
		return false;
	--it;
	return it->second >= end;
}

static void tex_fixup_ranges_evict(uintptr_t begin, uintptr_t end)
{
	if (begin >= end || sTexFixupRanges.empty())
		return;

	auto it = sTexFixupRanges.upper_bound(begin);
	if (it != sTexFixupRanges.begin())
		--it;

	while (it != sTexFixupRanges.end())
	{
		const uintptr_t range_begin = it->first;
		const uintptr_t range_end = it->second;
		if (range_end <= begin)
		{
			++it;
			continue;
		}
		if (range_begin >= end)
			break;

		if (range_begin < begin && range_end > end)
		{
			it->second = begin;
			sTexFixupRanges.emplace(end, range_end);
			break;
		}
		if (range_begin < begin)
		{
			it->second = begin;
			++it;
			continue;
		}
		if (range_end > end)
		{
			it = sTexFixupRanges.erase(it);
			sTexFixupRanges.emplace(end, range_end);
			break;
		}
		it = sTexFixupRanges.erase(it);
	}
}

// Tracks memory ranges of decoded struct arrays that runtime texture/palette
// BSWAPs must NOT touch. Certain N64 sprite files intentionally overlap the
// tail of a CI8 palette (which Fast3D loads as a fixed 512-byte block even if
// the actual palette is smaller) with the start of the bitmap array — on
// hardware the unused palette entries don't matter, but on the port the
// runtime-path BSWAP would corrupt the bitmap/sprite structs we already
// byte-swap-fixed up.
//
// Entries are (begin, end) pairs. Cleared at scene change via
// portResetStructFixups.
struct ProtectedRange { uintptr_t begin; uintptr_t end; };
static std::vector<ProtectedRange> sProtectedStructRanges;

// Separate tracking for post-decode 4c sprite deswizzle (portDeswizzleDecodedSprite4c).
// Needs its own set because portFixupSpriteBitmapData already inserted the same buf
// addresses during the pre-decode BSWAP32 pass.
static std::unordered_set<uintptr_t> sDeswizzle4cFixups;

/* Absolute addresses of every tokenized reloc chain slot currently live.
 * Texel data never contains chain slots, so a runtime texture fixup whose
 * range covers one is aimed at non-texture data (a stale or garbage
 * SETTIMG operand) and must be refused — applying it BSWAP32s live
 * descriptor and token words in place. Observed cascade: a DObjDesc array
 * in a fighter main file wholesale byte-reversed mid-scene, its sentinel
 * unrecognizable, item spawn walks running off the array into float data
 * → textureless objects, "impossible" token warnings (byte-swapped
 * tokens), and SIGSEGV in gcSetupCustomDObjsWithMObj. */
// Range queries dominate at runtime: a texture load needs the first live slot
// in [texture_begin, texture_end), not an exact-address lookup for every u32 in
// that range. An ordered set makes that a single lower_bound plus visits only
// actual slots. Exact lookups used by the load-time chain walk remain O(log N).
static std::set<uintptr_t> sChainSlotAddrs;

extern "C" void portRelocNoteChainSlot(const void *slot)
{
	sChainSlotAddrs.insert(reinterpret_cast<uintptr_t>(slot));
}

static void portRegisterProtectedStructRange(const void *begin, size_t size)
{
	if (begin == nullptr || size == 0) return;
	uintptr_t b = reinterpret_cast<uintptr_t>(begin);
	sProtectedStructRanges.push_back({ b, b + size });
}

// F3DEX2 GBI opcodes (from gbi.h with F3DEX_GBI_2 defined)
#define GBI_VTX         0x01
#define GBI_DL          0xDE
#define GBI_ENDDL       0xDF
#define GBI_SETTIMG     0xFD
#define GBI_LOADBLOCK   0xF3
#define GBI_LOADTLUT    0xF0

// Texture size constants (bits per pixel)
#define G_IM_SIZ_4b     0
#define G_IM_SIZ_8b     1
#define G_IM_SIZ_16b    2
#define G_IM_SIZ_32b    3

// Segment ID used for intra-file display list references
#define FILE_SEGMENT_ID 0x0E

// Fixup types for marked regions
enum RegionFixup
{
	FIXUP_VERTEX,       // Per-Vtx: rotate16 words 0-2, bswap32 word 3
	FIXUP_TEX_BYTES,    // 4bpp/8bpp texture: undo u32 swap (bswap32 back)
	FIXUP_TEX_U16,      // 16bpp texture or palette: rotate16 each word
	// 32bpp: no fixup needed (u32 swap was correct)
};

struct FixupRegion
{
	uint32_t byte_offset;
	uint32_t byte_size;
	RegionFixup type;
};

// ============================================================
//  Pass 1: Blanket u32 swap
// ============================================================

static void pass1_swap_u32(void *data, size_t size)
{
	uint32_t *words = static_cast<uint32_t *>(data);
	size_t count = size / 4;

	for (size_t i = 0; i < count; i++)
	{
		words[i] = BSWAP32(words[i]);
	}
}

// ============================================================
//  Pass 2: DL-guided fixup scan
// ============================================================

static void scan_display_lists(const uint32_t *words, size_t num_words,
                               size_t file_size, std::vector<FixupRegion> &regions)
{
	// Pending texture image state from the most recent gDPSetTextureImage
	bool has_pending_tex = false;
	uint32_t pending_tex_offset = 0;
	uint32_t pending_tex_siz = 0;

	for (size_t i = 0; i + 1 < num_words; i += 2)
	{
		uint32_t w0 = words[i];
		uint32_t w1 = words[i + 1];
		uint8_t opcode = (w0 >> 24) & 0xFF;

		switch (opcode)
		{
		case GBI_VTX:
		{
			uint32_t num_vtx = (w0 >> 12) & 0xFF;
			uint8_t seg = (w1 >> 24) & 0xFF;
			uint32_t offset = w1 & 0x00FFFFFF;
			size_t offset_sz = static_cast<size_t>(offset);
			size_t vtx_bytes = static_cast<size_t>(num_vtx) * 16;

			if (seg == FILE_SEGMENT_ID && num_vtx > 0 &&
			    offset_sz <= file_size && vtx_bytes <= (file_size - offset_sz))
			{
				regions.push_back({offset, num_vtx * 16, FIXUP_VERTEX});
				if (stage_audit_enabled()) sStageAudit.vtx_scan_caught++;
			}
			else if (stage_audit_enabled() && num_vtx > 0)
			{
				sStageAudit.vtx_scan_skipped_seg++;
			}
			break;
		}

		case GBI_SETTIMG:
		{
			uint8_t seg = (w1 >> 24) & 0xFF;
			uint32_t siz = (w0 >> 19) & 0x03;

			if (seg == FILE_SEGMENT_ID)
			{
				pending_tex_offset = w1 & 0x00FFFFFF;
				pending_tex_siz = siz;
				has_pending_tex = true;
				if (stage_audit_enabled()) sStageAudit.settimg_scan_caught++;
			}
			else
			{
				has_pending_tex = false;
				if (stage_audit_enabled()) sStageAudit.settimg_scan_skipped_seg++;
			}
			break;
		}

		case GBI_LOADBLOCK:
		{
			if (!has_pending_tex)
				break;

			uint32_t lrs = (w1 >> 12) & 0xFFF;
			uint32_t num_texels = lrs + 1;
			uint32_t bpp = 0;

			switch (pending_tex_siz)
			{
			case G_IM_SIZ_4b:  bpp = 4;  break;
			case G_IM_SIZ_8b:  bpp = 8;  break;
			case G_IM_SIZ_16b: bpp = 16; break;
			case G_IM_SIZ_32b: bpp = 32; break;
			}

			if (bpp == 0)
				break;

			uint32_t tex_bytes = (num_texels * bpp + 7) / 8;
			// Align to 4 bytes for u32 word processing
			tex_bytes = (tex_bytes + 3) & ~3u;

			size_t pending_offset_sz = static_cast<size_t>(pending_tex_offset);
			if (pending_offset_sz > file_size)
			{
				has_pending_tex = false;
				break;
			}
			if (static_cast<size_t>(tex_bytes) > (file_size - pending_offset_sz))
				tex_bytes = static_cast<uint32_t>(file_size - pending_offset_sz);

			RegionFixup fixup;
			switch (pending_tex_siz)
			{
			case G_IM_SIZ_4b:
			case G_IM_SIZ_8b:
				fixup = FIXUP_TEX_BYTES;
				break;
			case G_IM_SIZ_16b:
				fixup = FIXUP_TEX_U16;
				break;
			default:
				// 32b: u32 swap was correct, no fixup
				goto skip_tex;
			}

			regions.push_back({pending_tex_offset, tex_bytes, fixup});
		skip_tex:
			has_pending_tex = false;
			break;
		}

		case GBI_LOADTLUT:
		{
			if (!has_pending_tex)
				break;

			uint32_t count = ((w1 >> 14) & 0x3FF) + 1;
			uint32_t palette_bytes = count * 2;
			// Align to 4 bytes
			palette_bytes = (palette_bytes + 3) & ~3u;

			size_t pending_offset_sz = static_cast<size_t>(pending_tex_offset);
			if (pending_offset_sz > file_size)
			{
				has_pending_tex = false;
				break;
			}
			if (static_cast<size_t>(palette_bytes) > (file_size - pending_offset_sz))
				palette_bytes = static_cast<uint32_t>(file_size - pending_offset_sz);

			regions.push_back({pending_tex_offset, palette_bytes, FIXUP_TEX_U16});
			has_pending_tex = false;
			break;
		}

		default:
			break;
		}
	}
}


// Vita-safe pre-fix pass: only recognize structurally valid F3DEX2 G_VTX
// commands.  Unlike the legacy pass2 this does not infer SETTIMG/LOADBLOCK or
// TLUT regions from arbitrary blob bytes, so palette/texture data cannot be
// mutated merely because it happens to resemble a GBI command pair.
static __attribute__((unused)) void scan_vertex_lists_strict(const uint32_t *words, size_t num_words,
                                     size_t file_size, std::vector<FixupRegion> &regions)
{
	const uint8_t *bytes = reinterpret_cast<const uint8_t *>(words);
	for (size_t i = 0; i + 1 < num_words; i += 2)
	{
		uint32_t w0 = words[i];
		uint32_t w1 = words[i + 1];
		if (((w0 >> 24) & 0xFFu) != GBI_VTX) continue;

		// F3DEX2 G_VTX structural validation, matching chain_fixup_vertex().
		if ((w0 & 0x00F00000u) != 0) continue; // reserved bits [23:20]
		if ((w0 & 0x1u) != 0) continue;        // encoded end index is even
		uint32_t num_vtx = (w0 >> 12) & 0xFFu;
		uint32_t end_idx = (w0 & 0xFFEu) >> 1;
		if (num_vtx == 0 || num_vtx > 32) continue;
		if (end_idx > 32 || end_idx < num_vtx) continue;

		uint8_t seg = (uint8_t)(w1 >> 24);
		uint32_t offset = w1 & 0x00FFFFFFu;
		if (seg != FILE_SEGMENT_ID) continue;
		size_t total_bytes = (size_t)num_vtx * 16u;
		if ((size_t)offset > file_size || total_bytes > file_size - (size_t)offset) continue;
		if ((offset & 0x3u) != 0) continue;

		// Content sanity check on the first vertex in post-pass1 form.
		// This rejects the overwhelmingly common false-positive 0x01 words in
		// animation/struct data before any in-place mutation happens.
		const uint32_t *region = reinterpret_cast<const uint32_t *>(bytes + offset);
		int16_t ob0 = (int16_t)((region[0] >> 16) & 0xFFFFu);
		int16_t ob1 = (int16_t)(region[0] & 0xFFFFu);
		int16_t ob2 = (int16_t)((region[1] >> 16) & 0xFFFFu);
		uint16_t flag = (uint16_t)(region[1] & 0xFFFFu);
		const int kMaxCoord = 16384;
		if (ob0 < -kMaxCoord || ob0 > kMaxCoord ||
		    ob1 < -kMaxCoord || ob1 > kMaxCoord ||
		    ob2 < -kMaxCoord || ob2 > kMaxCoord || flag != 0) continue;

		regions.push_back({offset, num_vtx * 16u, FIXUP_VERTEX});
	}
}

// ============================================================
//  Fixup application
// ============================================================

// Rotate each u32 word by 16 bits: converts u32-swapped u16 pairs
// to correctly u16-swapped data.
// After u32 swap, BE bytes [A B C D] became [D C B A].
// rotate16 produces [B A D C] which is correct u16 LE for (AB, CD).
static inline uint32_t rotate16(uint32_t w)
{
	return (w << 16) | (w >> 16);
}

static void apply_fixup_vertex(uint32_t *words, size_t num_words)
{
	// Vtx is 4 u32 words (16 bytes):
	//   Word 0: s16 ob[0] | s16 ob[1]   -> rotate16
	//   Word 1: s16 ob[2] | u16 flag     -> rotate16
	//   Word 2: s16 tc[0] | s16 tc[1]    -> rotate16
	//   Word 3: u8 cn[0-3]               -> bswap32 (restore byte order)
	for (size_t i = 0; i + 3 < num_words; i += 4)
	{
		words[i + 0] = rotate16(words[i + 0]);
		words[i + 1] = rotate16(words[i + 1]);
		words[i + 2] = rotate16(words[i + 2]);
		words[i + 3] = BSWAP32(words[i + 3]);
	}
}

static void apply_fixup_tex_bytes(uint32_t *words, size_t num_words)
{
	// 4bpp/8bpp texture data: byte-granular pixels.
	// u32 swap reversed byte order within each 4-byte group.
	// Undo by swapping back to original byte order.
	for (size_t i = 0; i < num_words; i++)
	{
		words[i] = BSWAP32(words[i]);
	}
}

static void apply_fixup_tex_u16(uint32_t *words, size_t num_words)
{
	// 16bpp texture/palette data: u16 pixel values stored in N64 BE byte order.
	// Pass1 BSWAP32 reversed the byte order within each u32; Fast3D's
	// ImportTextureRgba16 reads texels as `(addr[0] << 8) | addr[1]` (BE u16),
	// so we must restore the original BE byte layout.  BSWAP32 undoes pass1.
	//
	// Earlier this used rotate16 which only swapped the two 16-bit halves —
	// that's correct for `[s16][s16]` vertex pair fields but wrong for a stream
	// of BE-read u16 texels.  Most non-sprite RGBA16 textures (room walls, etc.)
	// surfaced as all-near-black pixels until this was switched to BSWAP32.
	for (size_t i = 0; i < num_words; i++)
	{
		words[i] = BSWAP32(words[i]);
	}
}

static void apply_fixups(void *data, size_t file_size,
                         const std::vector<FixupRegion> &regions,
                         unsigned int file_id)
{
	uint8_t *bytes = static_cast<uint8_t *>(data);

	for (const auto &region : regions)
	{
		size_t start = static_cast<size_t>(region.byte_offset);
		size_t len = static_cast<size_t>(region.byte_size);

		// Bounds check using size_t math to prevent 32-bit overflow.
		if (start > file_size || len > (file_size - start))
			continue;

		// We reinterpret as u32 words, so both start and length must be word-aligned.
		if ((start & 3) != 0)
			continue;
		len &= ~static_cast<size_t>(3);
		if (len == 0)
			continue;

		uint32_t *region_words = reinterpret_cast<uint32_t *>(
			bytes + start);
		size_t num_words = len / 4;

		switch (region.type)
		{
		case FIXUP_VERTEX:
			// Strict per-Vtx idempotency BEFORE mutation.  The same Vtx array can
			// be referenced by many G_VTX commands (including overlapping subranges);
			// the legacy pass2 applied the whole region first and only registered it
			// afterwards, so a duplicate region could rotate/bswap the vertex twice.
			{
				uintptr_t base = reinterpret_cast<uintptr_t>(region_words);
				size_t n_vtx = num_words / 4;
				for (size_t v = 0; v < n_vtx; v++) {
					uintptr_t key = base + v * 16;
					if (sVertexFixups.count(key)) continue;
					sVertexFixups.insert(key);
					apply_fixup_vertex(region_words + v * 4, 4);
				}
			}
			break;
		case FIXUP_TEX_BYTES:
			apply_fixup_tex_bytes(region_words, num_words);
			tex_log_emit("pass2.bytes", (int)file_id,
			             (uint32_t)start, (uint32_t)len,
			             -1, -1, -1, region_words, "4b/8b");
			break;
		case FIXUP_TEX_U16:
			apply_fixup_tex_u16(region_words, num_words);
			tex_log_emit("pass2.u16", (int)file_id,
			             (uint32_t)start, (uint32_t)len,
			             -1, -1, 16, region_words, "16b/tlut");
			break;
		}
	}
}

// ============================================================
//  Public API
// ============================================================

extern "C" void portRelocByteSwapBlob(void *data, size_t size, unsigned int file_id)
{
	if (data == nullptr || size < 4)
		return;

	if (stage_audit_enabled()) stage_audit_reset_per_file();

	if (stage_audit_enabled() && file_id == 104) {
		const uint8_t *p = static_cast<const uint8_t *>(data);
		port_log("[STAGE_AUDIT_104_LOAD] pre_pass1 bytes=%02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X\n",
		         p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7],
		         p[8], p[9], p[10], p[11], p[12], p[13], p[14], p[15]);
	}

	// Pass 1: blanket u32 swap
	pass1_swap_u32(data, size);

	if (stage_audit_enabled() && file_id == 104) {
		const uint8_t *p = static_cast<const uint8_t *>(data);
		port_log("[STAGE_AUDIT_104_LOAD] post_pass1 bytes=%02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X\n",
		         p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7],
		         p[8], p[9], p[10], p[11], p[12], p[13], p[14], p[15]);
	}

#if defined(__vita__)
	/*
	 * Vita: do not mutate vertex or texture payloads by scanning the raw blob.
	 * Reloc-file G_VTX operands are still chain-encoded at this point, so a
	 * blob scan cannot reliably identify the real arrays.  Vertex byte-order
	 * normalization is deferred to GfxSpVertex(), where the running interpreter
	 * has already proven that the resolved pointer is a real Vtx array.
	 * Texture/TLUT normalization likewise remains lazy at the actual load site.
	 */
	static bool sVitaLazyOnlyModeLogged = false;
	if (!sVitaLazyOnlyModeLogged)
	{
		port_log("SSB64: RELOC_FIXUP_MODE vita vertex=post-reloc-manifest-strict chain-vtx=disabled texture=lazy token_namespace=0x40-0x7F live-command-disambiguation\n");
		sVitaLazyOnlyModeLogged = true;
	}
#else
	// Pass 2: legacy DL-guided fixup scan.  This is intentionally disabled
	// on Vita because it cannot distinguish arbitrary blob data from GBI.
	size_t num_words = size / 4;
	const uint32_t *words = static_cast<const uint32_t *>(data);

	std::vector<FixupRegion> regions;
	scan_display_lists(words, num_words, size, regions);

	if (!regions.empty())
	{
		apply_fixups(data, size, regions, file_id);
	}
#endif
}

// ============================================================
//  Struct u16 fixup — rotate16 for u16 fields in ROM structs
// ============================================================

extern "C" void portFixupStructU16(void *base, unsigned int byte_offset, unsigned int num_words)
{
	uintptr_t key = reinterpret_cast<uintptr_t>(base) + byte_offset;
	if (sStructU16Fixups.count(key))
		return;
	sStructU16Fixups.insert(key);

	uint32_t *words = reinterpret_cast<uint32_t *>(
		static_cast<uint8_t *>(base) + byte_offset);
	for (unsigned int i = 0; i < num_words; i++)
	{
		words[i] = (words[i] << 16) | (words[i] >> 16);
	}
}

// Undo the blanket pass1 u32 byteswap for a single-u8-inside-a-u32 field.
// After pass1, a BE ROM u8 at struct offset N lives at native offset (N|3);
// reversing the containing u32 word puts the u8 back at offset N where the
// C struct definition expects to read it.  Idempotent via sStructU16Fixups.
extern "C" void portFixupStructU32(void *base, unsigned int byte_offset, unsigned int num_words)
{
	uintptr_t key = reinterpret_cast<uintptr_t>(base) + byte_offset;
	if (sStructU16Fixups.count(key))
		return;
	sStructU16Fixups.insert(key);

	uint32_t *words = reinterpret_cast<uint32_t *>(
		static_cast<uint8_t *>(base) + byte_offset);
	for (unsigned int i = 0; i < num_words; i++)
	{
		words[i] = BSWAP32(words[i]);
	}
}

extern "C" void portResetStructFixups(void)
{
	sStructU16Fixups.clear();
	sVertexFixups.clear();
	sTexFixupExtent.clear();
	sTexFixupRanges.clear();
	sProtectedStructRanges.clear();
	sDeswizzle4cFixups.clear();
	sChainSlotAddrs.clear();
}

extern "C" void portEvictStructFixupsInRange(const void *begin, size_t size)
{
	if (begin == nullptr || size == 0)
		return;

	uintptr_t lo = reinterpret_cast<uintptr_t>(begin);
	uintptr_t hi = lo + size;

	auto evict_set = [&](std::unordered_set<uintptr_t> &s) {
		for (auto it = s.begin(); it != s.end(); ) {
			if (*it >= lo && *it < hi) it = s.erase(it);
			else ++it;
		}
	};
	evict_set(sStructU16Fixups);
	evict_set(sVertexFixups);
	evict_set(sDeswizzle4cFixups);
	tex_fixup_ranges_evict(lo, hi);
	for (auto it = sChainSlotAddrs.lower_bound(lo);
	     it != sChainSlotAddrs.end() && *it < hi; ) {
		it = sChainSlotAddrs.erase(it);
	}

	for (auto it = sTexFixupExtent.begin(); it != sTexFixupExtent.end(); ) {
		if (it->first >= lo && it->first < hi) it = sTexFixupExtent.erase(it);
		else ++it;
	}

	sProtectedStructRanges.erase(
		std::remove_if(sProtectedStructRanges.begin(), sProtectedStructRanges.end(),
		               [&](const ProtectedRange &r) {
		                   return r.begin < hi && r.end > lo;
		               }),
		sProtectedStructRanges.end());
}

// For raw texel blobs loaded via a runtime-built SETTIMG (where pass2's
// DL scan never saw the SETTIMG inside a stored display list) — apply
// BSWAP32 again to restore N64 BE byte order that pass1 destroyed.
// Tracked by base so repeat calls on the same blob are no-ops.
extern "C" void portFixupRawTextureBSWAP32(void *base, size_t bytes)
{
	if (base == nullptr || bytes == 0)
		return;

	uintptr_t key = reinterpret_cast<uintptr_t>(base);
	if (sStructU16Fixups.count(key))
		return;
	sStructU16Fixups.insert(key);

	// Round down to full u32s — pass1 only touched 4-byte-aligned words,
	// so any trailing tail bytes were never swapped and shouldn't be now.
	size_t num_words = bytes / 4;
	uint32_t *words = static_cast<uint32_t *>(base);
	for (size_t i = 0; i < num_words; i++)
	{
		words[i] = BSWAP32(words[i]);
	}
}

// ============================================================
//  Chain-walk fixup (textures and vertices)
// ============================================================
//
// Pass2's seg==0x0E filter only catches a tiny minority of in-file textures
// and vertices.  SSB64 model DLs reference these via real-pointer slots that
// the reloc chain rewrites at load time.  Pass2 sees those slots BEFORE
// rewrite — they contain chain encoding (random-looking high bytes), not
// seg=0x0E references, so they get missed.
//
// Workaround: during the chain walk, for each chain entry whose preceding
// cmd is a G_SETTIMG or G_VTX, apply the appropriate fixup using the chain
// encoding's words_num field as the in-file target offset.  The chain knows
// the truth because it's an explicit list of "slots that need fixup" —
// no false positives possible.
//
// Args:
//   file_base       — start of the file in PC memory (post-pass1)
//   file_size       — size of the file blob
//   slot_byte_off   — byte offset of the cmd's w1 slot (the slot the
//                     chain entry refers to)
//   target_byte_off — byte offset of the target data within the same file
//                     (computed from chain entry's words_num * 4)
//
// Returns 1 if the slot was at a G_SETTIMG/G_VTX and a fixup was applied;
// 0 otherwise.

static int chain_fixup_settimg(void *file_base, size_t file_size,
                                unsigned int slot_byte_off,
                                unsigned int target_byte_off,
                                uint32_t w0)
{
	const uint8_t *file_bytes = static_cast<const uint8_t *>(file_base);

	uint32_t fmt = (w0 >> 21) & 0x07;
	uint32_t siz = (w0 >> 19) & 0x03;
	(void)fmt;

	// Walk forward up to 8 cmds (64 bytes) to find G_LOADBLOCK or G_LOADTLUT.
	uint32_t loadblock_w1 = 0;
	uint32_t loadtlut_w1  = 0;
	int      found_load   = 0;
	for (int step = 1; step <= 8; step++)
	{
		size_t walk_off = (size_t)slot_byte_off - 4 + (size_t)(step * 8);
		if (walk_off + 8 > file_size) break;
		uint32_t walk_w0 = *(const uint32_t *)(file_bytes + walk_off);
		uint32_t walk_w1 = *(const uint32_t *)(file_bytes + walk_off + 4);
		uint8_t  walk_op = (uint8_t)(walk_w0 >> 24);
		if (walk_op == GBI_LOADBLOCK)
		{
			loadblock_w1 = walk_w1;
			found_load = 1;
			break;
		}
		if (walk_op == GBI_LOADTLUT)
		{
			loadtlut_w1 = walk_w1;
			found_load = 2;
			break;
		}
		if (walk_op == GBI_SETTIMG) return 0;
	}
	if (!found_load) return 0;

	uint32_t tex_bytes = 0;
	if (found_load == 1)
	{
		uint32_t lrs = (loadblock_w1 >> 12) & 0xFFF;
		uint32_t num_texels = lrs + 1;
		uint32_t bpp = (siz == G_IM_SIZ_4b)  ? 4
		             : (siz == G_IM_SIZ_8b)  ? 8
		             : (siz == G_IM_SIZ_16b) ? 16
		             : (siz == G_IM_SIZ_32b) ? 32
		             : 0;
		if (bpp == 0) return 0;
		tex_bytes = (num_texels * bpp + 7) / 8;
	}
	else
	{
		uint32_t count = ((loadtlut_w1 >> 14) & 0x3FF) + 1;
		tex_bytes = count * 2;
	}
	tex_bytes = (tex_bytes + 3) & ~3u;
	if (tex_bytes == 0) return 0;
	if ((size_t)target_byte_off + tex_bytes > file_size)
	{
		tex_bytes = (uint32_t)(file_size - target_byte_off);
		tex_bytes &= ~3u;
		if (tex_bytes == 0) return 0;
	}

	uintptr_t fixup_key = reinterpret_cast<uintptr_t>(file_base) + (uintptr_t)target_byte_off;
	if (sStructU16Fixups.count(fixup_key)) return 1;
	sStructU16Fixups.insert(fixup_key);

	uint32_t *region = (uint32_t *)((uint8_t *)file_base + target_byte_off);
	size_t num_words = tex_bytes / 4;

	// All formats need BSWAP32 to restore N64 BE byte order:
	//  - 4b/8b: per-byte data; Fast3D reads bytes in their original order
	//  - 16b: pixels are BE u16; Fast3D reads `(addr[0] << 8) | addr[1]`
	//  - 32b: pixels are BE [R,G,B,A]; Fast3D's ImportTextureRgba32 reads
	//         addr[0]=R, addr[1]=G, addr[2]=B, addr[3]=A
	// Pass1 BSWAP32 reversed the byte order within each u32, so a second
	// BSWAP32 restores the original layout for ALL of these formats.
	//
	// Issue #4 (BTT/BTP arrow palette): we must record this fix in
	// sTexFixupExtent and sTexFixupRanges too. Without those, the runtime
	// LOADTLUT path's early-skip — which fires when a target appears in
	// sStructU16Fixups but NOT sTexFixupExtent — locks out runtime fixup
	// on this target. That matters when the runtime LOADTLUT loads MORE
	// bytes than chain just fixed (e.g. a count=3 partial palette where
	// chain only saw a count=1 LOADTLUT in its 8-cmd window): bytes
	// beyond the chain-fixed range stay in pass1-BSWAP state and the
	// arrow's palette[2] reads as 0xC17B (magenta) instead of 0x0001
	// (black). With these two records, the runtime path now passes the
	// early-skip and falls through to the range-difference path, which skips
	// already-fixed spans via sTexFixupRanges and BSWAPs the rest.
	uintptr_t reg_base = reinterpret_cast<uintptr_t>(region);
	for (size_t i = 0; i < num_words; i++) {
		region[i] = BSWAP32(region[i]);
	}
	tex_fixup_ranges_insert(reg_base, reg_base + tex_bytes);
	auto extent_it = sTexFixupExtent.find(reg_base);
	if (extent_it == sTexFixupExtent.end() || extent_it->second < tex_bytes)
		sTexFixupExtent[reg_base] = tex_bytes;

	// Diagnostic: record what was just fixed (chain-walk path).
	if (tex_log_enabled() || tex_dump_enabled()) {
		uintptr_t fb_addr = reinterpret_cast<uintptr_t>(file_base);
		int chain_file_id = portRelocFindFileIdAndBase(
			reinterpret_cast<const void *>(fb_addr), nullptr);
		uint32_t bpp_val = (siz == G_IM_SIZ_4b)  ? 4u
		                 : (siz == G_IM_SIZ_8b)  ? 8u
		                 : (siz == G_IM_SIZ_16b) ? 16u
		                 : (siz == G_IM_SIZ_32b) ? 32u
		                 : 0u;
		int fmt_val = (int)((w0 >> 21) & 0x07);
		const char *note = (found_load == 2) ? "tlut" : "loadblock";
		tex_log_emit("chain.settimg", chain_file_id,
		             target_byte_off, tex_bytes,
		             fmt_val, (int)siz, (int)bpp_val,
		             region, note);

		// Dump the texture as one or more TGA images so we can identify
		// it visually.  Skips palettes (loadtlut) since they're 1-D LUTs
		// not viewable as images.
		if (found_load == 1) {
			tex_dump_chain(chain_file_id, target_byte_off,
			               fmt_val, (int)siz, bpp_val,
			               reinterpret_cast<const uint8_t *>(region),
			               (size_t)tex_bytes);
		}
	}

	if (stage_audit_enabled()) sStageAudit.chain_settimg_fixups++;
	return 1;
}

static __attribute__((unused)) int chain_fixup_vertex(void *file_base, size_t file_size,
                               unsigned int slot_byte_off,
                               unsigned int target_byte_off,
                               uint32_t w0)
{
	(void)slot_byte_off;
	// G_VTX layout in F3DEX2 (matches gbi_decoder.c decoding):
	//   w0[31:24] = 0x01  (G_VTX opcode)
	//   w0[23:20] = 0     (reserved)
	//   w0[19:12] = num_vtx (n), 8 bits, practically 1..32
	//   w0[11:1]  = (v0 + n) << 1, end vertex slot index in TMEM
	//   w0[0]     = 0     (must be even because of << 1)
	//   w1        = vertex array address
	//
	// Each Vtx is 16 bytes:
	//   word 0: s16 ob[0]  | s16 ob[1]   → rotate16
	//   word 1: s16 ob[2]  | u16 flag    → rotate16
	//   word 2: s16 tc[0]  | s16 tc[1]   → rotate16
	//   word 3: u8  cn[0..3]              → bswap32 (restore byte order)
	//
	// STRICT VALIDATION: opcode 0x01 is too common to trust by itself.
	// Many non-cmd u32 words start with 0x01.  Reject anything where the
	// reserved/structural bits don't match the spec.

	// bits [23:20] must be zero (reserved)
	if ((w0 & 0x00F00000) != 0) return 0;
	// bit [0] must be zero (the count is multiplied by 2)
	if ((w0 & 0x1) != 0) return 0;

	uint32_t num_vtx = (w0 >> 12) & 0xFF;
	uint32_t end_idx = (w0 & 0xFFE) >> 1;

	// Sanity-check num_vtx and the destination range.  Real game DLs use
	// 1..32 vertices per G_VTX (Vtx buffer is 32 entries on N64).
	if (num_vtx == 0 || num_vtx > 32) return 0;
	if (end_idx > 32 || end_idx < num_vtx) return 0;

	size_t total_bytes = (size_t)num_vtx * 16;
	if ((size_t)target_byte_off + total_bytes > file_size) return 0;

	uint32_t *region = (uint32_t *)((uint8_t *)file_base + target_byte_off);

	// Content-based sanity check on the FIRST vertex.
	// Post-pass1 word 0 (high 16 bits) = original BE ob[0] (s16),
	// (low 16 bits) = original BE ob[1] (s16). Word 1 likewise has
	// ob[2] and flag. Real game vertex coords observed up to ±4200,
	// flag is always 0. Reject anything with much larger coords or
	// nonzero flag — those are false-positive struct-field chain
	// entries whose preceding 4 bytes happen to look like a G_VTX cmd.
	{
		int16_t ob0 = (int16_t)((region[0] >> 16) & 0xFFFF);
		int16_t ob1 = (int16_t)(region[0] & 0xFFFF);
		int16_t ob2 = (int16_t)((region[1] >> 16) & 0xFFFF);
		uint16_t flag = (uint16_t)(region[1] & 0xFFFF);
		const int kMaxCoord = 16384;
		if (ob0 < -kMaxCoord || ob0 > kMaxCoord) return 0;
		if (ob1 < -kMaxCoord || ob1 > kMaxCoord) return 0;
		if (ob2 < -kMaxCoord || ob2 > kMaxCoord) return 0;
		if (flag != 0) return 0;
	}

	// Per-vertex idempotency (matches portRelocFixupVertexAtRuntime).
	uintptr_t target_addr = reinterpret_cast<uintptr_t>(file_base) + (uintptr_t)target_byte_off;
	if (stage_audit_enabled()) {
		int tgt_file = portRelocFindFileIdAndBase((const void *)target_addr, nullptr);
		int src_file = portRelocFindFileIdAndBase(file_base, nullptr);
		static uint32_t sChainTrace = 0;
		if (tgt_file == 104 && sChainTrace < 30) {
			sChainTrace++;
			port_log("[STAGE_AUDIT_CHAIN] chain_fixup_vertex src=file%d tgt=file%d target_off=0x%x num_vtx=%u\n",
			         src_file, tgt_file, target_byte_off, num_vtx);
		}
	}
	for (uint32_t i = 0; i < num_vtx; i++)
	{
		uintptr_t vtx_key = target_addr + (uintptr_t)i * 16;
		if (sVertexFixups.count(vtx_key)) continue;
		sVertexFixups.insert(vtx_key);

		region[i * 4 + 0] = (region[i * 4 + 0] << 16) | (region[i * 4 + 0] >> 16);
		region[i * 4 + 1] = (region[i * 4 + 1] << 16) | (region[i * 4 + 1] >> 16);
		region[i * 4 + 2] = (region[i * 4 + 2] << 16) | (region[i * 4 + 2] >> 16);
		region[i * 4 + 3] = BSWAP32(region[i * 4 + 3]);
	}
	if (stage_audit_enabled()) sStageAudit.chain_vtx_fixups++;
	return 1;
}

/*
 * High-confidence G_VTX context validation for the reloc-chain pre-fix path.
 * A candidate must sit inside a short coherent GBI command run before its
 * target is normalized in-place. This preserves early host-order Vtx for
 * game-side readers/copies while rejecting struct data that only resembles
 * a vertex command.
 */
/*
 * Resource-class gate for chain-time vertex normalization. Fighter model
 * blobs are mixed containers (DObj/MObj descriptors + display lists + Vtx).
 * Heuristically mutating them in-place can turn a false-positive G_VTX into
 * persistent model/display-list corruption that survives into the match.
 *
 * Fighter models therefore stay immutable and use the typed copy-decode in
 * GfxSpVertex. Early normalization is reserved for shared geometry banks and
 * movie geometry that game-side code may inspect/copy before Fast3D dispatch.
 * This is class-based, never a file-id whitelist.
 */
static bool chain_vtx_resource_allows_early_fix(const void *file_base)
{
	uintptr_t base = 0;
	size_t size = 0;
	unsigned int file_id = 0xFFFFFFFFu;
	const char *path = nullptr;
	if (!portRelocDescribePointer(file_base, &base, &size, &file_id, &path) || path == nullptr)
		return false;

	if (std::strstr(path, "reloc_fighters_main/") != nullptr &&
	    std::strstr(path, "Model") != nullptr)
		return false;

	if (std::strstr(path, "reloc_extern_data/ExternDataBank") != nullptr)
		return true;
	if (std::strstr(path, "reloc_movies/") != nullptr)
		return true;

	return false;
}

static bool chain_vtx_has_dl_context(const uint8_t *file_bytes, size_t file_size,
                                     unsigned int slot_byte_off)
{
	if (file_bytes == nullptr || slot_byte_off < 4) return false;
	const size_t cmd_off = (size_t)slot_byte_off - 4u;
	if ((cmd_off & 0x7u) != 0 || cmd_off + 8u > file_size) return false;

	auto plausible_opcode = [](uint8_t op) -> bool {
		if (op <= 0x08u) return true;
		if (op >= 0xD3u && op <= 0xDFu) return true;
		if (op >= 0xE4u) return true;
		if (op == 0xC0u) return true;
		return false;
	};

	auto strong_opcode = [](uint8_t op) -> bool {
		return op == 0x05u || op == 0x06u || op == 0x07u ||
		       op == GBI_DL || op == GBI_ENDDL ||
		       op == GBI_SETTIMG || op == GBI_LOADBLOCK || op == GBI_LOADTLUT ||
		       op == 0xF5u || op == 0xF2u || op == 0xFCu || op == 0xD9u;
	};

	unsigned int valid_after = 0;
	unsigned int strong_after = 0;
	for (unsigned int i = 1; i <= 6; ++i)
	{
		const size_t off = cmd_off + (size_t)i * 8u;
		if (off + 8u > file_size) break;
		const uint32_t next_w0 = *reinterpret_cast<const uint32_t *>(file_bytes + off);
		const uint8_t op = static_cast<uint8_t>(next_w0 >> 24);
		if (!plausible_opcode(op)) break;

		if (op == GBI_VTX)
		{
			if ((next_w0 & 0x00F00000u) != 0 || (next_w0 & 1u) != 0) break;
			const uint32_t n = (next_w0 >> 12) & 0xFFu;
			const uint32_t end_idx = (next_w0 & 0xFFEu) >> 1;
			if (n == 0 || n > 32 || end_idx > 32 || end_idx < n) break;
		}

		++valid_after;
		if (strong_opcode(op)) ++strong_after;
		if (op == GBI_ENDDL) break;
	}
	return valid_after >= 2u && strong_after >= 1u;
}

extern "C" int portRelocFixupTextureFromChain(void *file_base, size_t file_size,
                                              unsigned int slot_byte_off,
                                              unsigned int target_byte_off)
{
	if (file_base == nullptr || slot_byte_off < 4 ||
	    (size_t)(slot_byte_off + 4) > file_size ||
	    (size_t)target_byte_off >= file_size)
	{
		return 0;
	}

	const uint8_t *file_bytes = static_cast<const uint8_t *>(file_base);

	/* The w0 candidate at slot-4 may be a TOKEN this same walk already wrote:
	 * chain entries are frequently adjacent words, so the "command" preceding
	 * this slot can be the previous entry's freshly-tokenized slot. Legacy
	 * generation-in-high-bits tokens could therefore become 0x01 (G_VTX) or
	 * 0xFD (SETTIMG) after enough reloads and be parsed as commands. v10 pins tokens to 0x80..0x9F, which is outside every command
	 * opcode band used by the port and outside N64 segment IDs. Keep this live-slot guard as defense in depth. */
	{
		uintptr_t w0_addr = (uintptr_t)(file_bytes + slot_byte_off) - 4;
		auto it = sChainSlotAddrs.find(w0_addr);
		if (it != sChainSlotAddrs.end())
		{
			extern void *portRelocTryResolvePointer(uint32_t token);
			uint32_t w0_word = *(const uint32_t *)w0_addr;
			if (w0_word != 0 && portRelocTryResolvePointer(w0_word) != nullptr)
			{
				static unsigned int sW0TokenSkips = 0;
				if (sW0TokenSkips < 16)
				{
					sW0TokenSkips++;
					port_log("SSB64: chainFixup SKIP w0-is-chain-token base=%p slot_off=0x%x w0=0x%08X\n",
					         file_base, slot_byte_off, w0_word);
				}
				return 0;
			}
			sChainSlotAddrs.erase(it);
		}
	}

	// The cmd's w0 is at slot_byte_off - 4, w1 is at slot_byte_off.
	uint32_t w0 = *(const uint32_t *)(file_bytes + slot_byte_off - 4);
	uint8_t opcode = (uint8_t)(w0 >> 24);

	if (opcode == GBI_SETTIMG)
	{
		return chain_fixup_settimg(file_base, file_size, slot_byte_off,
		                           target_byte_off, w0);
	}
	if (opcode == GBI_VTX)
	{
		if ((slot_byte_off & 0x7) != 4) return 0;

#if defined(__vita__)
		// Never mutate Vtx payloads from a reloc-chain predecessor heuristic.
		// The chain only establishes pointer provenance. After both relocation
		// walks finish, portRelocFinalize3DVertexManifest() follows live tokens
		// into validated packed display lists and normalizes only real G_VTX
		// targets before the resource is published.
		int file_id = portRelocFindFileIdAndBase(file_base, nullptr);
		uint32_t num_vtx = (w0 >> 12) & 0xFFu;
		static unsigned int sDeferredVertexTrace = 0;
		if (sDeferredVertexTrace < 64u)
		{
			port_log("SSB64: RELOC_VTX_CHAIN_DEFER file=%d slot=0x%x target=0x%x n=%u reason=post-reloc-manifest\n",
			         file_id, slot_byte_off, target_byte_off, num_vtx);
			++sDeferredVertexTrace;
		}
		return 0;
#else
		return chain_fixup_vertex(file_base, file_size, slot_byte_off, target_byte_off, w0);
#endif
	}
	return 0;
}


// ============================================================
//  Post-relocation 3D vertex manifest (Vita)
// ============================================================
//
// Vtx payloads are not type-safe under the blanket u32 endian pass. The old
// Vita chain fix guessed their type from the word before a relocation slot,
// making normalization dependent on chain order and nearby token values.
//
// This pass runs after intern+extern relocation has completed. Candidate DL
// roots come only from live relocation tokens. A root is accepted only when it
// parses as a bounded packed GBI list with valid opcodes and a real terminator.
// G_DL edges are followed recursively ONLY while they remain inside the same
// resource. A finalizer must never mutate an external dependency: dependencies
// have their own load/finalization transaction. Only owner-local G_VTX targets
// reached through the validated graph are normalized in-place. Runtime-built
// lists still fall back to the non-destructive GfxSpVertex decode path.

#if defined(__vita__)

struct ManifestVtxRange
{
	uintptr_t address;
	size_t bytes;
};

static bool manifest_opcode_plausible(uint8_t op)
{
	if (op <= 0x0Fu) return true;
	if (op >= 0xD3u) return true;
	if (op == 0xC0u) return true;
	return false;
}

static bool manifest_decode_vtx_command(uint32_t w0, unsigned int *out_num_vtx)
{
	if (((w0 >> 24) & 0xFFu) != GBI_VTX) return false;
	if ((w0 & 0x00F00000u) != 0) return false;
	if ((w0 & 0x1u) != 0) return false;

	const uint32_t num_vtx = (w0 >> 12) & 0xFFu;
	const uint32_t end_idx = (w0 & 0xFFEu) >> 1;
	if (num_vtx == 0 || num_vtx > 32u) return false;
	if (end_idx > 32u || end_idx < num_vtx) return false;

	if (out_num_vtx != nullptr) *out_num_vtx = num_vtx;
	return true;
}

static bool manifest_owner_contains(uintptr_t owner_base, size_t owner_size,
                                    const void *ptr, size_t bytes = 1u)
{
	if (ptr == nullptr || owner_size == 0u || bytes == 0u) return false;
	const uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
	if (addr < owner_base) return false;
	const size_t off = (size_t)(addr - owner_base);
	return off <= owner_size && bytes <= owner_size - off;
}

static bool manifest_resolve_resource_pointer(uint32_t raw,
                                              uintptr_t owner_base,
                                              size_t owner_size,
                                              void **out_ptr)
{
	extern void *portRelocTryResolvePointer(uint32_t token);
	extern int portRelocIsPointerToken(uint32_t token);

	void *resolved = raw != 0 ? portRelocTryResolvePointer(raw) : nullptr;

	/* v10: if this value belongs to the reloc-token namespace but no longer
	 * resolves, it is stale. Never reinterpret it as a segment-0E address. */
	if (resolved == nullptr && raw != 0 && portRelocIsPointerToken(raw))
		return false;

	if (resolved == nullptr)
	{
		const uint8_t seg = (uint8_t)(raw >> 24);
		const uint32_t off = raw & 0x00FFFFFFu;
		if (seg == FILE_SEGMENT_ID && (size_t)off < owner_size)
		{
			resolved = reinterpret_cast<void *>(owner_base + (uintptr_t)off);
		}
	}

	if (resolved == nullptr) return false;

	/* A resolved token may legitimately point at an external dependency.
	 * Resolution is allowed here so the parser can classify that edge, but
	 * ownership is enforced by the caller.  v5 accidentally followed these
	 * edges and normalized dependency memory after that dependency was READY. */
	if (out_ptr != nullptr) *out_ptr = resolved;
	return true;
}

static bool manifest_validate_vtx_range(void *ptr, unsigned int num_vtx,
                                        uintptr_t owner_base, size_t owner_size,
                                        ManifestVtxRange *out_range)
{
	if (ptr == nullptr || num_vtx == 0 || num_vtx > 32u) return false;

	const uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
	const size_t bytes = (size_t)num_vtx * 16u;

	if ((addr & 0x3u) != 0) return false;
	if (!manifest_owner_contains(owner_base, owner_size, ptr, bytes)) return false;

	if (out_range != nullptr)
	{
		out_range->address = addr;
		out_range->bytes = bytes;
	}
	return true;
}

struct ManifestDlResult
{
	bool valid = false;
	std::vector<ManifestVtxRange> vtx_ranges;
	std::vector<void *> sublists;
	unsigned int external_vtx_refs = 0;
	unsigned int external_dl_refs = 0;
};

static ManifestDlResult manifest_parse_packed_dl(void *candidate,
                                                  uintptr_t owner_base,
                                                  size_t owner_size)
{
	ManifestDlResult result;
	if (candidate == nullptr) return result;

	/* Critical v6 invariant: a resource finalizer may inspect and mutate only
	 * bytes owned by that exact resource.  Never follow a token into an extern
	 * dependency merely because it happens to parse as a valid display list. */
	if (!manifest_owner_contains(owner_base, owner_size, candidate, 8u))
		return result;

	const uintptr_t start = reinterpret_cast<uintptr_t>(candidate);
	const size_t start_off = (size_t)(start - owner_base);
	if ((start_off & 0x7u) != 0 || start_off + 8u > owner_size)
		return result;

	const size_t max_commands = std::min<size_t>((owner_size - start_off) / 8u, 512u);
	unsigned int meaningful = 0;
	bool terminated = false;
	bool saw_graphics = false;

	for (size_t i = 0; i < max_commands; ++i)
	{
		const uint32_t *cmd = reinterpret_cast<const uint32_t *>(start + i * 8u);
		const uint32_t w0 = cmd[0];
		const uint32_t w1 = cmd[1];
		const uint8_t op = (uint8_t)(w0 >> 24);

		/* v11: 0x40..0x44 are legitimate OTR command opcodes, so namespace-only
		 * classification is not valid for a display-list command word. Reject w0
		 * as pointer data only when it is a currently-live relocation token. This
		 * preserves v9's host-pointer-safe token namespace without hiding real OTR
		 * commands used by the intro/effects display lists. */
		extern void *portRelocTryResolvePointer(uint32_t token);
		if (portRelocTryResolvePointer(w0) != nullptr)
			return ManifestDlResult{};

		if (!manifest_opcode_plausible(op))
			return ManifestDlResult{};

		if (w0 != 0 || w1 != 0) ++meaningful;

		if (op == GBI_VTX)
		{
			unsigned int num_vtx = 0;
			if (!manifest_decode_vtx_command(w0, &num_vtx))
				return ManifestDlResult{};

			void *vtx_ptr = nullptr;
			if (manifest_resolve_resource_pointer(w1, owner_base, owner_size, &vtx_ptr))
			{
				if (manifest_owner_contains(owner_base, owner_size, vtx_ptr,
				                            (size_t)num_vtx * 16u))
				{
					ManifestVtxRange range{};
					if (!manifest_validate_vtx_range(vtx_ptr, num_vtx,
					                                 owner_base, owner_size, &range))
						return ManifestDlResult{};
					result.vtx_ranges.push_back(range);
				}
				else
				{
					/* The dependency owns this Vtx and will normalize it in its own
					 * finalization transaction. Runtime typed decode remains the
					 * fallback if that dependency has no discoverable stored DL. */
					++result.external_vtx_refs;
				}
				saw_graphics = true;
			}
		}
		else if (op == 0x05u || op == 0x06u || op == 0x07u)
		{
			saw_graphics = true;
		}
		else if (op == GBI_DL)
		{
			saw_graphics = true;
			void *sub = nullptr;
			if (manifest_resolve_resource_pointer(w1, owner_base, owner_size, &sub))
			{
				if (manifest_owner_contains(owner_base, owner_size, sub, 8u))
					result.sublists.push_back(sub);
				else
					++result.external_dl_refs;
			}

			const bool no_push = ((w0 >> 16) & 1u) != 0;
			if (no_push)
			{
				terminated = true;
				break;
			}
		}
		else if (op == GBI_ENDDL)
		{
			terminated = true;
			break;
		}
	}

	result.valid = terminated && meaningful >= 2u && saw_graphics;
	if (!result.valid)
	{
		result.vtx_ranges.clear();
		result.sublists.clear();
		result.external_vtx_refs = 0;
		result.external_dl_refs = 0;
	}
	return result;
}

extern "C" void portRelocFinalize3DVertexManifest(void *file_base, size_t file_size,
                                                   unsigned int file_id,
                                                   const unsigned int *reloc_slot_offsets,
                                                   size_t reloc_slot_count)
{
	if (file_base == nullptr || file_size < 8u ||
	    reloc_slot_offsets == nullptr || reloc_slot_count == 0)
		return;

	const uintptr_t base = reinterpret_cast<uintptr_t>(file_base);
	std::vector<void *> queue;
	queue.reserve(reloc_slot_count + 16u);

	extern void *portRelocTryResolvePointer(uint32_t token);
	for (size_t i = 0; i < reloc_slot_count; ++i)
	{
		const size_t slot_off = (size_t)reloc_slot_offsets[i];
		if (slot_off > file_size || sizeof(uint32_t) > file_size - slot_off)
			continue;

		const uint32_t token = *reinterpret_cast<const uint32_t *>(base + slot_off);
		void *target = token != 0 ? portRelocTryResolvePointer(token) : nullptr;
		if (target != nullptr && manifest_owner_contains(base, file_size, target, 8u))
			queue.push_back(target);
	}

	std::unordered_set<uintptr_t> visited_dl;
	std::map<uintptr_t, size_t> ranges;
	unsigned int valid_dls = 0;
	unsigned int rejected_roots = 0;
	unsigned int external_vtx_refs = 0;
	unsigned int external_dl_refs = 0;

	for (size_t q = 0; q < queue.size() && q < 4096u; ++q)
	{
		void *candidate = queue[q];
		const uintptr_t key = reinterpret_cast<uintptr_t>(candidate);
		if (!visited_dl.insert(key).second) continue;

		ManifestDlResult parsed = manifest_parse_packed_dl(candidate, base, file_size);
		if (!parsed.valid)
		{
			++rejected_roots;
			continue;
		}

		++valid_dls;
		external_vtx_refs += parsed.external_vtx_refs;
		external_dl_refs += parsed.external_dl_refs;
		for (const ManifestVtxRange &r : parsed.vtx_ranges)
		{
			auto it = ranges.find(r.address);
			if (it == ranges.end() || it->second < r.bytes)
				ranges[r.address] = r.bytes;
		}
		for (void *sub : parsed.sublists)
			if (sub != nullptr) queue.push_back(sub);
	}

	unsigned int normalized_ranges = 0;
	unsigned int normalized_vertices = 0;
	unsigned int already_normalized = 0;

	for (const auto &entry : ranges)
	{
		const uintptr_t addr = entry.first;
		const size_t bytes = entry.second;
		if ((bytes % 16u) != 0) continue;

		uint32_t *region = reinterpret_cast<uint32_t *>(addr);
		const size_t count = bytes / 16u;
		for (size_t i = 0; i < count; ++i)
		{
			const uintptr_t vtx_key = addr + i * 16u;
			if (sVertexFixups.count(vtx_key) != 0)
			{
				++already_normalized;
				continue;
			}

			sVertexFixups.insert(vtx_key);
			region[i * 4u + 0u] = rotate16(region[i * 4u + 0u]);
			region[i * 4u + 1u] = rotate16(region[i * 4u + 1u]);
			region[i * 4u + 2u] = rotate16(region[i * 4u + 2u]);
			region[i * 4u + 3u] = BSWAP32(region[i * 4u + 3u]);
			++normalized_vertices;
		}
		++normalized_ranges;
	}

	if (valid_dls > 0u || normalized_vertices > 0u)
	{
		port_log("SSB64: RESOURCE_3D_MANIFEST file_id=%u roots=%u valid_dls=%u rejected=%u "
		         "vtx_ranges=%u normalized_vertices=%u prehost_vertices=%u "
		         "external_vtx=%u external_dl=%u ownership=strict\n",
		         file_id, (unsigned int)reloc_slot_count, valid_dls, rejected_roots,
		         normalized_ranges, normalized_vertices, already_normalized,
		         external_vtx_refs, external_dl_refs);
	}
}

#else

extern "C" void portRelocFinalize3DVertexManifest(void *, size_t, unsigned int,
                                                   const unsigned int *, size_t)
{
}

#endif

// ============================================================
//  Lazy runtime fixup (Option A)
// ============================================================
//
// Called from the interpreter's gfx_vtx_handler_f3dex2 with the resolved
// vertex array address and the cmd's num_vtx.  If the address is inside a
// reloc file (i.e., not heap-built data), apply the per-Vtx byte permutation
// idempotently.
//
// Why this is correct: only data the running interpreter actually treats as
// vertices reaches this function — there's no guessing.  Each target's "type"
// is unambiguously vertex because a real G_VTX cmd just dispatched it.

// Lazy texture byte-order fixup at G_LOADBLOCK / G_LOADTLUT execute time.
// Called from libultraship's GfxDpLoadBlock and GfxDpLoadTLUT once both
// the texture address (set by G_SETTIMG) and the texel count are known.
//
// This catches textures referenced by RUNTIME-BUILT DLs that pass2 doesn't
// see (no in-file SETTIMG/LOADBLOCK pair) and that the chain walk also
// doesn't see (the slot was looked up via an MObjSub field, not a chain
// entry whose preceding cmd is SETTIMG).  Fighter material textures are
// the most common example: lbCommonAddMObjForFighterPartsDObj walks the
// fighter's MObjSub array at draw time and builds the SETTIMG/LOADBLOCK
// pairs in heap, never embedding them in any file DL.
//
// Fast3D's texel readers expect bytes in N64 BE byte order:
//   4b/8b: per-byte data, addr[0] is the first pixel
//   16b: pixels are BE u16, read via `(addr[0] << 8) | addr[1]`
//   32b: pixels are BE [R,G,B,A], read addr[0]=R, addr[1]=G, ...
// Pass1 BSWAP32 reversed the byte order within each u32 word, so we apply
// BSWAP32 again to restore the original layout.
//
// Idempotent at word granularity so overlapping runtime loads from different
// starts cannot undo each other.
extern "C" void portRelocFixupTextureAtRuntime(const void *addr, unsigned int num_bytes)
{
	if (addr == nullptr || num_bytes == 0) return;

	// Only fix data that lives inside a reloc file blob.  Heap-built textures
	// (procedurally generated, copied from elsewhere, etc.) are already in
	// correct host byte order.
	uintptr_t fileBase = 0;
	size_t    fileSize = 0;
	int in_reloc_file = portRelocFindContainingFile(addr, &fileBase, &fileSize);
	portStageAuditNoteTexDispatch(in_reloc_file);
	if (!in_reloc_file)
	{
		// Log heap textures so we can spot any geometry texture that
		// somehow lives outside any reloc file (the "third path" case).
		if (tex_log_enabled()) {
			char heap_note[40];
			std::snprintf(heap_note, sizeof(heap_note),
			              "addr=0x%016llX",
			              (unsigned long long)reinterpret_cast<uintptr_t>(addr));
			tex_log_emit("runtime.heap", -1,
			             0u, num_bytes, -1, -1, -1, addr, heap_note);
		}
		return;
	}

	uintptr_t target = reinterpret_cast<uintptr_t>(addr);
	size_t    target_offset = target - fileBase;
	if (target_offset >= fileSize)
	{
		return;
	}
	size_t available_bytes = fileSize - target_offset;
	if ((size_t)num_bytes > available_bytes)
	{
		num_bytes = (unsigned int)available_bytes;
	}

	// Clip the fix range so it can't enter a previously-registered protected
	// struct array (bitmap array, sprite, mobjsub, ...). These are already in
	// native byte order and a blind BSWAP32 pass would corrupt them. The
	// typical offender is a CI8 palette load that Fast3D treats as a fixed
	// 512-byte read even when the actual palette is smaller; the extra bytes
	// happen to overlap the bitmap/sprite struct region on sprite files.
	{
		uintptr_t clip_end = target + num_bytes;
		for (const auto &r : sProtectedStructRanges)
		{
			if (r.begin >= clip_end) continue;
			if (r.end <= target) continue;
			// Overlap. Only clip if the protected range starts *after* target,
			// so we still fix the prefix that lives outside the struct. If
			// the protected range starts at or before target, shrink num_bytes
			// to zero — the whole range overlaps a struct and we should skip it.
			if (r.begin > target)
			{
				if (r.begin < clip_end)
					clip_end = r.begin;
			}
			else
			{
				clip_end = target;
				break;
			}
		}
		num_bytes = (clip_end > target) ? (unsigned int)(clip_end - target) : 0u;
	}

	// Round UP to next 4-byte boundary, then clamp to file bounds. Issue #4:
	// a CI4 LOADTLUT with count=3 calls in with num_bytes=6, which under the
	// old `&= ~3u` round-DOWN became 4 — leaving the second 4-byte word
	// (containing palette[2]'s low byte) in pass1-BSWAP state. The N64
	// LOADTLUT only copies 6 bytes into TMEM, but pass1 mangled the whole
	// containing word, so we must un-mangle the whole word for the loaded
	// portion to read correctly. Bounds-clamped because rounding up can't
	// validly extend past file end.
	num_bytes = (unsigned int)(((size_t)num_bytes + 3u) & ~(size_t)3u);
	if ((size_t)num_bytes > available_bytes)
		num_bytes = (unsigned int)(available_bytes & ~(size_t)3u);
	if (num_bytes == 0) return;

	// If pass2 or the chain walk already fixed this exact base and runtime has
	// never seen it, keep the old conservative skip. Runtime-owned fixups below
	// are tracked per word to handle overlapping TLUT/LOADBLOCK ranges.
	if (sTexFixupExtent.find(target) == sTexFixupExtent.end() &&
	    sStructU16Fixups.count(target)) {
		if (tex_log_enabled()) {
			int rt_file_id = portRelocFindFileIdAndBase(addr, nullptr);
			tex_log_emit("runtime.skip", rt_file_id,
			             (uint32_t)target_offset, num_bytes,
			             -1, -1, -1, addr, "already-fixed");
		}
		return;
	}

	/* The renderer emits LOADBLOCK/LOADTILE for the same texture on nearly
	 * every frame. If this whole request is already in native byte order, stop
	 * here: the old per-word hash loop was still rechecking every u32 on every
	 * load and dominated the Vita CPU trace. */
	if (tex_fixup_ranges_contains(target, target + num_bytes))
		return;

	/* Per-file trace budget (2 lines/file, whole array ~4 KiB) instead of one
	 * shared global counter. A global counter of 32 was entirely exhausted by
	 * whichever handful of files loaded first (intro movies/fighters), so
	 * stage files and later-loaded resources never got a single trace line —
	 * hiding exactly the resources under investigation. */
	{
		uintptr_t descBase = 0;
		size_t descSize = 0;
		unsigned int descFileId = 0;
		const char *descPath = nullptr;
		int described = portRelocDescribePointer(addr, &descBase, &descSize, &descFileId, &descPath);
		static uint8_t sRuntimeTexTraceCountByFile[RELOC_FILE_COUNT];
		if (descFileId < RELOC_FILE_COUNT && sRuntimeTexTraceCountByFile[descFileId] < 2)
		{
			sRuntimeTexTraceCountByFile[descFileId]++;
			/* %zx silently prints as the literal characters "zx" on this
			 * VitaSDK newlib build (its vsnprintf doesn't support the 'z'
			 * length modifier) - every size_t field showed up as "0xzx"
			 * instead of a real value. size_t is 32-bit on this target, so
			 * %x with an explicit cast is equivalent and actually works. */
			port_log("SSB64: runtimeTexFix addr=%p req=0x%x fileBase=%p fileSize=0x%x off=0x%x num=0x%x desc=%d file=%u descBase=%p descSize=0x%x path=%s\n",
			         addr, num_bytes, (void*)fileBase, (unsigned int)fileSize, (unsigned int)target_offset, num_bytes,
			         described, descFileId, (void*)descBase,
			         (unsigned int)descSize, descPath ? descPath : "(unknown)");
		}
	}

	/* Clamp the swap at the first tokenized reloc chain slot in the range.
	 *
	 * N64 LoadBlock/LoadTLUT legitimately over-read past the true texel
	 * span (fixed block sizes); on hardware the extra bytes only landed in
	 * TMEM and were never written back. This fixup MUTATES source memory,
	 * so swapping the over-read tail byte-reverses live tokens and struct
	 * words in place — observed as fighter-file DObjDesc arrays wholesale
	 * BSWAP32'd (tokens included), cascading into textureless objects,
	 * impossible-generation token warnings, and SIGSEGV in the desc walk.
	 *
	 * Texel data never contains chain slots, so the first slot marks the
	 * end of trustworthy texture bytes: swap up to it, leave the rest.
	 * The clamped tail stays in its live (already correct) byte order and
	 * the GPU merely samples a few nonsense over-read texels the original
	 * hardware also displayed as garbage-in-TMEM.
	 *
	 * Verified case (2026-08-21): reloc_menus/MNPlayersCommon,
	 * GateMan1PLUT (file offset 0x103f8) via RedCardSprite's
	 * gDPLoadTLUT(nTLUT=256, ...) — clamps at 0xa0/0x200 requested. Parsed
	 * the raw Sprite struct straight from the ROM: RedCardSprite is CI4
	 * (bmsiz=G_IM_SIZ_4b), so only its first 16 palette entries (32 bytes)
	 * are ever indexable; the clamp already keeps 5x that. The excluded
	 * tail is RedCardSprite's own Sprite struct (its .LUT/.bitmap reloc
	 * fields land at +0xd8/+0xec) — a second, unrelated over-read, not
	 * lost texel data. Before assuming a CLAMPED hit here is the cause of
	 * a visual bug, parse the Sprite/Bitmap's bmsiz the same way and
	 * confirm the kept byte count is actually short of what that siz can
	 * sample — most hits will look exactly like this one. */
	{
		extern void *portRelocTryResolvePointer(uint32_t token);
		uintptr_t scan_end = target + num_bytes;
		auto slot_it = sChainSlotAddrs.lower_bound(target);
		while (slot_it != sChainSlotAddrs.end() && *slot_it < scan_end)
		{
			const uintptr_t w = *slot_it;
			/* Registry entries can outlive their file epoch: a reload at
			 * the same address (force-heap rewind, bump reuse) evicts only
			 * the NEW file's smaller range, leaving corpse addresses from
			 * the old file's tail. A live slot must hold a token that
			 * actually resolves; anything else is a corpse — drop it and
			 * keep the texture swap intact (clamping on a corpse truncates
			 * real texels and renders the texture garbled). */
			uint32_t slot_word = *reinterpret_cast<const uint32_t *>(w);
			if (slot_word == 0 || portRelocTryResolvePointer(slot_word) == nullptr)
			{
				slot_it = sChainSlotAddrs.erase(slot_it);
				continue;
			}
			/* Same per-file dedup rationale as the runtimeTexFix trace above:
			 * one hot resource re-clamped every frame (e.g. a menu texture
			 * drawn continuously) used to burn the entire shared 64-line
			 * budget on itself, hiding whether any other file — a stage,
			 * an intro model — ever clamps at all. */
			int cl_file_id = portRelocFindFileIdAndBase(addr, nullptr);
			static uint8_t sClampLogCountByFile[RELOC_FILE_COUNT];
			if (cl_file_id >= 0 && (unsigned int)cl_file_id < RELOC_FILE_COUNT &&
			    sClampLogCountByFile[cl_file_id] < 3)
			{
				sClampLogCountByFile[cl_file_id]++;
				port_log("SSB64: runtimeTexFix CLAMPED addr=%p num=0x%x->0x%x file=%d — "
				         "live reloc chain slot at %p bounds the texel span\n",
				         addr, num_bytes, (unsigned int)(w - target), cl_file_id, (void *)w);
			}
			num_bytes = (unsigned int)(w - target);
			break;
		}
		if (num_bytes == 0)
			return;
	}

	const uintptr_t fix_end = target + num_bytes;
	const size_t num_words = num_bytes / 4;
	size_t fixed_words = 0;
	uintptr_t cursor = target;
	auto range_it = sTexFixupRanges.upper_bound(cursor);
	if (range_it != sTexFixupRanges.begin())
	{
		auto prev = std::prev(range_it);
		if (prev->second > cursor)
			range_it = prev;
	}

	/* Swap only gaps not covered by an earlier chain/runtime fixup. The map is
	 * kept merged, so traversal is linear in the number of overlapping ranges
	 * (normally zero or one), not in the number of texture words. */
	while (cursor < fix_end)
	{
		while (range_it != sTexFixupRanges.end() && range_it->second <= cursor)
			++range_it;

		if (range_it == sTexFixupRanges.end() || range_it->first >= fix_end)
		{
			for (uintptr_t word = cursor; word < fix_end; word += 4)
			{
				uint32_t *p = reinterpret_cast<uint32_t *>(word);
				*p = BSWAP32(*p);
				fixed_words++;
			}
			break;
		}

		if (range_it->first > cursor)
		{
			const uintptr_t gap_end = std::min(fix_end, range_it->first);
			for (uintptr_t word = cursor; word < gap_end; word += 4)
			{
				uint32_t *p = reinterpret_cast<uint32_t *>(word);
				*p = BSWAP32(*p);
				fixed_words++;
			}
			cursor = gap_end;
			continue;
		}

		cursor = std::min(fix_end, range_it->second);
		++range_it;
	}
	const size_t skipped_words = num_words - fixed_words;

	if (fixed_words == 0)
	{
		if (tex_log_enabled()) {
			int rt_file_id = portRelocFindFileIdAndBase(addr, nullptr);
			tex_log_emit("runtime.skip", rt_file_id,
			             (uint32_t)target_offset, num_bytes,
			             -1, -1, -1, addr, "already-fixed");
		}
		return;
	}
	tex_fixup_ranges_insert(target, fix_end);

	sStructU16Fixups.insert(target);
	auto extent_it = sTexFixupExtent.find(target);
	if (extent_it == sTexFixupExtent.end() || extent_it->second < num_bytes)
		sTexFixupExtent[target] = num_bytes;

	if (tex_log_enabled()) {
		int rt_file_id = portRelocFindFileIdAndBase(addr, nullptr);
		char note[64];
		std::snprintf(note, sizeof(note), "fixed=%u skipped=%u",
		              (unsigned int)fixed_words, (unsigned int)skipped_words);
		tex_log_emit("runtime.fix", rt_file_id,
		             (uint32_t)target_offset, num_bytes,
		             -1, -1, -1, reinterpret_cast<uint32_t *>(target), note);
	}

	portStageAuditNoteRuntimeTex((unsigned int)(fixed_words * 4));
}

extern "C" int portRelocDecodeVerticesForRuntime(const void *addr, unsigned int num_vtx,
                                                    void *out_vertices, size_t out_size)
{
	if (addr == nullptr || out_vertices == nullptr || num_vtx == 0) return -1;

	uintptr_t fileBase = 0;
	size_t fileSize = 0;
	int in_reloc_file = portRelocFindContainingFile(addr, &fileBase, &fileSize);
	portStageAuditNoteVtxDispatch(in_reloc_file);
	int rt_file_id = in_reloc_file ? portRelocFindFileIdAndBase(addr, nullptr) : -1;
	portStageAuditNoteVtxDispatchFile(rt_file_id);
	if (!in_reloc_file) return 0;

	const uintptr_t target = reinterpret_cast<uintptr_t>(addr);
	const size_t target_offset = target - fileBase;
	const size_t total_bytes = (size_t)num_vtx * 16u;
	if (out_size < total_bytes) return -1;
	if (target_offset > fileSize || total_bytes > fileSize - target_offset) return -1;

	std::memcpy(out_vertices, addr, total_bytes);
	uint32_t *region = reinterpret_cast<uint32_t *>(out_vertices);
	unsigned int decoded_here = 0;
	unsigned int already_host = 0;
	for (unsigned int i = 0; i < num_vtx; i++)
	{
		const uintptr_t vtx_key = target + (uintptr_t)i * 16u;
		if (sVertexFixups.count(vtx_key) != 0)
		{
			++already_host;
			continue;
		}
		region[i * 4 + 0] = rotate16(region[i * 4 + 0]);
		region[i * 4 + 1] = rotate16(region[i * 4 + 1]);
		region[i * 4 + 2] = rotate16(region[i * 4 + 2]);
		region[i * 4 + 3] = BSWAP32(region[i * 4 + 3]);
		++decoded_here;
	}
	portStageAuditNoteRuntimeVtx(decoded_here);

	if (stage_audit_enabled())
	{
		static unsigned int sDecodeTrace = 0;
		if (sDecodeTrace < 64u)
		{
			const int16_t *ob = reinterpret_cast<const int16_t *>(out_vertices);
			port_log("SSB64: VTX_RUNTIME_DECODE file=%d off=0x%x n=%u decoded=%u prehost=%u post_ob=(%d,%d,%d)\n",
			         rt_file_id, (unsigned int)target_offset, num_vtx,
			         decoded_here, already_host,
			         (int)ob[0], (int)ob[1], (int)ob[2]);
			++sDecodeTrace;
		}
	}

	return 1;
}

extern "C" void portRelocFixupVertexAtRuntime(const void *addr, unsigned int num_vtx)
{
	if (addr == nullptr || num_vtx == 0 || num_vtx > 32) return;

	// Only fix data that lives inside a reloc file blob.  Heap-built vertex
	// arrays (e.g., dynamic UI sprites) are constructed by game code with
	// already-correct byte order on the host LE side.
	uintptr_t fileBase = 0;
	size_t    fileSize = 0;
	int in_reloc_file = portRelocFindContainingFile(addr, &fileBase, &fileSize);
	portStageAuditNoteVtxDispatch(in_reloc_file);
	{
		int rt_file_id = in_reloc_file ? portRelocFindFileIdAndBase(addr, nullptr) : -1;
		portStageAuditNoteVtxDispatchFile(rt_file_id);
	}
	if (!in_reloc_file)
	{
		return;
	}

	uintptr_t target = reinterpret_cast<uintptr_t>(addr);
	size_t target_offset = target - fileBase;
	size_t total_bytes = (size_t)num_vtx * 16;
	if (target_offset + total_bytes > fileSize) return;

	// PER-VERTEX idempotency.  Game code can re-load OVERLAPPING sub-ranges
	// of the same vertex array (e.g. cmd 1 loads vertices 0..15 at addr A,
	// cmd 2 loads vertices 1..14 at addr A+16).  Address-keyed tracking
	// would treat these as distinct keys and double-fix the overlap region.
	// Instead we mark each individual 16-byte Vtx; only un-marked vertices
	// get fixed.
	uint32_t *region = reinterpret_cast<uint32_t *>(target);
	unsigned int fixed_here = 0;
	unsigned int skipped_here = 0;
	for (unsigned int i = 0; i < num_vtx; i++)
	{
		uintptr_t vtx_key = target + (uintptr_t)i * 16;
		if (sVertexFixups.count(vtx_key)) { skipped_here++; continue; }
		sVertexFixups.insert(vtx_key);
		fixed_here++;

		// word 0: s16 ob[0] | s16 ob[1] → rotate16
		region[i * 4 + 0] = (region[i * 4 + 0] << 16) | (region[i * 4 + 0] >> 16);
		// word 1: s16 ob[2] | u16 flag → rotate16
		region[i * 4 + 1] = (region[i * 4 + 1] << 16) | (region[i * 4 + 1] >> 16);
		// word 2: s16 tc[0] | s16 tc[1] → rotate16
		region[i * 4 + 2] = (region[i * 4 + 2] << 16) | (region[i * 4 + 2] >> 16);
		// word 3: u8 cn[0..3] → bswap32 to restore byte order
		region[i * 4 + 3] = BSWAP32(region[i * 4 + 3]);
	}
	if (fixed_here > 0) portStageAuditNoteRuntimeVtx(fixed_here);

	/* Focused trace on ExternDataBank104 to see who's marking its vertices. */
	if (stage_audit_enabled()) {
		int rt_file_id = portRelocFindFileIdAndBase(addr, nullptr);
		if (rt_file_id == 104) {
			static uint32_t sBank104Trace = 0;
			sBank104Trace++;
			if (sBank104Trace <= 5) {
				const uint8_t *p = (const uint8_t *)addr;
				port_log("[STAGE_AUDIT_104] call#%u addr=%p num_vtx=%u fixed=%u skipped=%u offset=0x%x bytes=%02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X\n",
				         sBank104Trace, addr, num_vtx, fixed_here, skipped_here,
				         (unsigned int)(size_t)(target - fileBase),
				         p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7],
				         p[8], p[9], p[10], p[11], p[12], p[13], p[14], p[15]);
			}
		}
	}

	// Post-fix trace for the resources that reproduce the remaining visual bug.
	// This is intentionally emitted after the per-Vtx idempotent normalization,
	// so the coordinates reflect exactly what GfxSpVertex will consume.
	{
		int rt_file_id = portRelocFindFileIdAndBase(addr, nullptr);
		if (rt_file_id == 106 || rt_file_id == 108 || rt_file_id == 296 || rt_file_id == 313 || rt_file_id == 317)
		{
			static unsigned int sRuntimeTrace106 = 0;
			static unsigned int sRuntimeTrace108 = 0;
			static unsigned int sRuntimeTrace296 = 0;
			static unsigned int sRuntimeTrace313 = 0;
			static unsigned int sRuntimeTrace317 = 0;
			unsigned int *trace_count = (rt_file_id == 106) ? &sRuntimeTrace106 :
			                            (rt_file_id == 108) ? &sRuntimeTrace108 :
			                            (rt_file_id == 296) ? &sRuntimeTrace296 :
			                            (rt_file_id == 313) ? &sRuntimeTrace313 : &sRuntimeTrace317;
			if (*trace_count < 24u)
			{
				const int16_t *ob = reinterpret_cast<const int16_t *>(addr);
				port_log("SSB64: VTX_RUNTIME_FIX file=%d off=0x%x n=%u fixed=%u skipped=%u post_ob=(%d,%d,%d)\n",
				         rt_file_id, (unsigned int)target_offset, num_vtx, fixed_here, skipped_here,
				         (int)ob[0], (int)ob[1], (int)ob[2]);
				++(*trace_count);
			}
		}
	}
}

// ============================================================
//  Sprite / Bitmap / MObjSub — struct-level byte-swap fixups
// ============================================================
//
// After the blanket u32 swap, u32 and f32 fields are correct but
// u16-pair words and u8-quad words are garbled.
//
// rotate16: fixes two u16 values packed in one u32 word
// bswap32:  undoes the blanket swap for a u8-quad word
//
// Each function is idempotent: tracked by the same sStructU16Fixups set.

static void fixup_rotate16(uint32_t *word)
{
	*word = (*word << 16) | (*word >> 16);
}

static void fixup_bswap32(uint32_t *word)
{
	*word = BSWAP32(*word);
}

// Fixup for a u32 word laid out as [u16 a][u8 b][u8 c] in original BE memory.
// Pass1's blanket BSWAP32 leaves the bytes as [c, b, a_lo, a_hi] which makes
// a_lo/a_hi appear in the wrong slots: reading `u16 a` from offset 0 yields
// (b << 8) | c, and reading `u8 b`/`u8 c` from offsets 2/3 yields a_lo/a_hi.
// Neither rotate16 nor a second bswap32 produces the correct LE layout for
// all three fields simultaneously, so we permute the bytes directly.
static void fixup_u16_u8u8(uint32_t *word)
{
	uint8_t *p = reinterpret_cast<uint8_t *>(word);
	uint8_t b0 = p[0];
	uint8_t b1 = p[1];
	uint8_t b2 = p[2];
	uint8_t b3 = p[3];
	// Have (post-pass1): [c, b, a_lo, a_hi] = [b0, b1, b2, b3]
	// Want (LE struct):  [a_lo, a_hi, b, c] = [b2, b3, b1, b0]
	p[0] = b2;
	p[1] = b3;
	p[2] = b1;
	p[3] = b0;
}

extern "C" void portFixupSprite(void *sprite)
{
	if (sprite == NULL)
		return;

	uintptr_t key = reinterpret_cast<uintptr_t>(sprite);
	if (sStructU16Fixups.count(key))
		return;
	sStructU16Fixups.insert(key);

	uint32_t *w = static_cast<uint32_t *>(sprite);

	// Sprite layout (17 words = 68 bytes):
	//  Word  Offset  Fields                   Fixup
	//  0     0x00    s16 x, s16 y             rotate16
	//  1     0x04    s16 width, s16 height    rotate16
	//  2     0x08    f32 scalex               (ok)
	//  3     0x0C    f32 scaley               (ok)
	//  4     0x10    s16 expx, s16 expy       rotate16
	//  5     0x14    u16 attr, s16 zdepth     rotate16
	//  6     0x18    u8 r,g,b,a               bswap32
	//  7     0x1C    s16 startTLUT, s16 nTLUT rotate16
	//  8     0x20    u32 LUT (token)          (ok)
	//  9     0x24    s16 istart, s16 istep    rotate16
	//  10    0x28    s16 nbitmaps, s16 ndisplist rotate16
	//  11    0x2C    s16 bmheight, s16 bmHreal rotate16
	//  12    0x30    u8 bmfmt, u8 bmsiz, pad  bswap32
	//  13    0x34    u32 bitmap (token)        (ok)
	//  14    0x38    u32 rsp_dl (token)        (ok)
	//  15    0x3C    u32 rsp_dl_next (token)   (ok)
	//  16    0x40    s16 frac_s, s16 frac_t   rotate16

	fixup_rotate16(&w[0]);   // x, y
	fixup_rotate16(&w[1]);   // width, height
	// w[2], w[3]: f32 scalex, scaley — ok
	fixup_rotate16(&w[4]);   // expx, expy
	fixup_rotate16(&w[5]);   // attr, zdepth
	fixup_bswap32(&w[6]);    // rgba
	fixup_rotate16(&w[7]);   // startTLUT, nTLUT
	// w[8]: u32 LUT — ok
	fixup_rotate16(&w[9]);   // istart, istep
	fixup_rotate16(&w[10]);  // nbitmaps, ndisplist
	fixup_rotate16(&w[11]);  // bmheight, bmHreal
	fixup_bswap32(&w[12]);   // bmfmt, bmsiz, pad
	// w[13..15]: u32 tokens — ok
	fixup_rotate16(&w[16]);  // frac_s, frac_t
}

extern "C" void portFixupBitmap(void *bitmap)
{
	if (bitmap == NULL)
		return;

	uintptr_t key = reinterpret_cast<uintptr_t>(bitmap);
	if (sStructU16Fixups.count(key))
		return;
	sStructU16Fixups.insert(key);

	uint32_t *w = static_cast<uint32_t *>(bitmap);

	// Bitmap layout (4 words = 16 bytes):
	//  Word  Offset  Fields                        Fixup
	//  0     0x00    s16 width, s16 width_img      rotate16
	//  1     0x04    s16 s, s16 t                  rotate16
	//  2     0x08    u32 buf (token)               (ok)
	//  3     0x0C    s16 actualHeight, s16 LUToffset rotate16

	fixup_rotate16(&w[0]);
	fixup_rotate16(&w[1]);
	// w[2]: u32 buf — ok
	fixup_rotate16(&w[3]);
}

extern "C" void portFixupBitmapArray(void *bitmaps, unsigned int count)
{
	if (bitmaps == NULL || count == 0)
		return;

	// Protect the bitmap array bytes from runtime texture/palette BSWAP
	// over-reads. N64 sprite files sometimes sit a full 512-byte CI8 palette
	// load on top of the bitmap array (the useful palette entries fit before
	// the array; the tail past the array start is effectively garbage). See
	// the wallpaper handling in mvOpeningRun for the concrete case.
	portRegisterProtectedStructRange(bitmaps, (size_t)count * 16);

	uint8_t *ptr = static_cast<uint8_t *>(bitmaps);
	for (unsigned int i = 0; i < count; i++)
	{
		portFixupBitmap(ptr + i * 16);
	}
}

// Sprite layout offsets used by the texel-data fixup. We do not include
// PR/sp.h here (to keep the bridge translation unit lean), so we hard-code
// the field offsets matching the static_assert(sizeof(Sprite) == 68).
//
//   sprite[0x28]  s16 nbitmaps
//   sprite[0x32]  u8  bmsiz                (within the u8 quad word at 0x30)
//   sprite[0x34]  u32 bitmap (token)
//
//   bitmap[0x00]  s16 width        (unused for size — see width_img)
//   bitmap[0x02]  s16 width_img
//   bitmap[0x08]  u32 buf (token)
//   bitmap[0x0C]  s16 actualHeight
//
// Sizes match decomp Sprite/Bitmap structs after portFixupSprite/portFixupBitmap
// have run (i.e. fields readable as native LE).
extern "C" void portFixupSpriteBitmapData(void *sprite_v, void *bitmaps_v)
{
	if (sprite_v == NULL || bitmaps_v == NULL)
		return;

	// Also protect the 68-byte Sprite struct from runtime-texture over-reads
	// for the same reason as the bitmap array above.
	portRegisterProtectedStructRange(sprite_v, 68);

	uint8_t *sprite = static_cast<uint8_t *>(sprite_v);
	uint8_t *bitmaps = static_cast<uint8_t *>(bitmaps_v);

	int16_t nbitmaps = *reinterpret_cast<int16_t *>(sprite + 0x28);
	uint8_t bmfmt    = *reinterpret_cast<uint8_t  *>(sprite + 0x30);
	uint8_t bmsiz    = *reinterpret_cast<uint8_t  *>(sprite + 0x31);

	if (nbitmaps <= 0)
		return;

	// G_IM_SIZ_ values: 4b=0, 8b=1, 16b=2, 32b=3, 4c=4 (compressed-4b)
	//
	// Fast3D's texel readers (ImportTextureRgba16/Rgba32/CI4/CI8/etc.) all
	// read texture bytes in N64 big-endian order — for example
	//   RGBA16: `(addr[0] << 8) | addr[1]`
	//   RGBA32: `r=addr[0], g=addr[1], b=addr[2], a=addr[3]`
	// So texel data must be left in the original BE byte order from the file.
	// Pass1 BSWAP32 destroys that order; another BSWAP32 restores it.
	//
	// 32bpp used to be skipped here on the assumption "pass1 byteswap is
	// already correct because the reader is per-channel byte-by-byte".  That
	// reasoning is wrong: pass1's per-u32 byte reverse turns each pixel's
	// `[R G B A]` into `[A B G R]` in PC memory, so the per-byte reader sees
	// channel-reversed RGBA32.  Symptom: HAL Labs dog (a 32bpp sprite) renders
	// with R↔A and G↔B swapped — brown becomes a different color entirely.
	// chain_fixup_settimg already had the same wrong assumption removed; this
	// is the matching fix on the sprite path.
	int bpp;
	switch (bmsiz)
	{
	case 0: bpp = 4;  break;   // 4b
	case 1: bpp = 8;  break;   // 8b
	case 2: bpp = 16; break;   // 16b
	case 3: bpp = 32; break;   // 32b
	case 4: bpp = 4;  break;   // 4c (compressed-4b: byte-granular like 4b)
	default:
		return;
	}

	for (int i = 0; i < nbitmaps; i++)
	{
		uint8_t *bm = bitmaps + (i * 16);
		int16_t  bm_width     = *reinterpret_cast<int16_t *>(bm + 0x00);
		int16_t  width_img    = *reinterpret_cast<int16_t *>(bm + 0x02);
		uint32_t buf_token    = *reinterpret_cast<uint32_t *>(bm + 0x08);
		int16_t  actualHeight = *reinterpret_cast<int16_t *>(bm + 0x0C);

		void *buf = portRelocTryResolvePointer(buf_token);
		if (buf == NULL || width_img <= 0 || actualHeight <= 0)
			continue;

		// Idempotency for the BSWAP32 pass: skip if pass2 (or a prior
		// sprite fixup call) already restored the byte order.
		uintptr_t key = reinterpret_cast<uintptr_t>(buf);
		bool bswap_already_done = sStructU16Fixups.count(key) != 0;
		if (!bswap_already_done)
		{
			sStructU16Fixups.insert(key);

			size_t num_texels = static_cast<size_t>(width_img) *
			                    static_cast<size_t>(actualHeight);
			size_t tex_bytes = (num_texels * bpp + 7) / 8;
			// Word-align so we can iterate as u32
			tex_bytes = (tex_bytes + 3) & ~size_t{3};

			uint32_t *words = static_cast<uint32_t *>(buf);
			size_t num_words = tex_bytes / 4;

			// Undo pass1 BSWAP32 to restore N64 BE byte order.
			for (size_t j = 0; j < num_words; j++)
				words[j] = BSWAP32(words[j]);
		}

		if (!bswap_already_done && (tex_log_enabled() || tex_dump_enabled())) {
			size_t log_tex_bytes = (static_cast<size_t>(width_img) *
			                        static_cast<size_t>(actualHeight) * bpp + 7) / 8;
			log_tex_bytes = (log_tex_bytes + 3) & ~size_t{3};
			uintptr_t bm_base = 0;
			int sprite_file_id = portRelocFindFileIdAndBase(buf, &bm_base);
			uint32_t off = (sprite_file_id >= 0)
				? (uint32_t)(reinterpret_cast<uintptr_t>(buf) - bm_base)
				: 0u;
			char note[64];
			std::snprintf(note, sizeof(note),
			              "sprite bm=%d w=%d wi=%d h=%d",
			              i, (int)bm_width, (int)width_img, (int)actualHeight);
			tex_log_emit("sprite.bitmap", sprite_file_id,
			             off, (uint32_t)log_tex_bytes,
			             -1, (int)bmsiz, bpp, static_cast<uint32_t *>(buf), note);
			tex_dump_known_dims(sprite_file_id, off,
			                    (int)bmfmt, (int)bmsiz, bpp,
			                    reinterpret_cast<const uint8_t *>(buf),
			                    (size_t)log_tex_bytes,
			                    (uint32_t)width_img,
			                    (uint32_t)actualHeight);
		}

		// N64 RDP TMEM line swizzle: textures stored in DRAM are pre-swizzled
		// to avoid TMEM bank conflicts when sampled. lbCommonDrawSObjBitmap
		// LOAD_BLOCKs the sprite and relies on the RDP's odd-row TMEM
		// address XOR to unscramble at sample time; Fast3D reads DRAM
		// linearly, so we have to un-swizzle in software.
		//
		// The swap granularity is bit-depth dependent:
		//
		//   4b/8b/16b: LoadBlock uses G_IM_SIZ_16b (per the *_LOAD_BLOCK
		//              macros in gbi.h), so the sampler's XOR-0x4 on the
		//              byte address lands as "swap the two 4-byte halves
		//              of each 8-byte qword" on odd rows.
		//
		//   32b:       LoadBlock uses G_IM_SIZ_32b. The LOAD_BLOCK with
		//              dxt=0 interleaves each 4-byte pixel across the
		//              low/high TMEM banks (2 bytes per bank), so each
		//              texel consumes 2 TMEM words instead of one. The
		//              sampler's odd-row XOR still acts at the same TMEM
		//              word granularity, which at the DRAM level means
		//              "swap the two 8-byte halves of each 16-byte group"
		//              — i.e. swap 2 RGBA32 pixels with the next 2.
		//              Empirically verified by offline comparison of the
		//              post-BSWAP32 TGA dumps (tex_dump/sprite_f167_* for
		//              SMASH, _f19_* for fighter-select portraits) against
		//              the original artwork: XOR-4 (1-pixel swap, which
		//              used to be the unconditional behaviour for all
		//              bpp) scrambled 32bpp sprites by swapping adjacent
		//              pixels on every odd row; XOR-16 (2-pixel pair swap)
		//              produces clean images.
		//
		// 4c (compressed) is excluded because the on-disk data is 2bpp
		// compressed source; the decoder runs AFTER this fixup and
		// produces 4bpp data that needs its own post-decode deswizzle
		// (portDeswizzleDecodedSprite4c below).
		//
		// Uses sDeswizzle4cFixups (separate from the BSWAP set) so that
		// textures whose byte-swap was already handled by pass2 still get
		// the deswizzle applied.
		if (bpp > 0 && bmsiz != 4 &&
		    width_img > 0 && actualHeight > 0 &&
		    !sDeswizzle4cFixups.count(key))
		{
			sDeswizzle4cFixups.insert(key);

			size_t group_size = (bpp == 32) ? 16 : 8;
			size_t half       = group_size / 2;

			// row_bytes = DRAM bytes per pixel row.
			size_t row_bytes = ((size_t)width_img * (size_t)bpp + 7) / 8;

			// Require the row to hold at least one full group. The inner
			// loop only processes complete groups (`g + group_size <=
			// row_bytes`), so any trailing bytes past the last full group
			// are left untouched.
			if (row_bytes >= group_size)
			{
				uint8_t *bytes = static_cast<uint8_t *>(buf);
				for (int row = 1; row < actualHeight; row += 2)
				{
					uint8_t *row_p = bytes + (size_t)row * row_bytes;
					for (size_t g = 0; g + group_size <= row_bytes; g += group_size)
					{
						uint8_t tmp[8]; // max half is 8 (for group_size=16)
						std::memcpy(tmp, row_p + g, half);
						std::memcpy(row_p + g, row_p + g + half, half);
						std::memcpy(row_p + g + half, tmp, half);
					}
				}
			}
		}

		// Note: the fighter-intro "MARIO" title-card smear and the
		// title-screen border right-edge slice used to be patched
		// here (ce89700 zeroed, then a later revision replicated the
		// edge pixel).  Both were workarounds for a Fast3D upload bug
		// where the GPU texture was uploaded at the TMEM stride
		// (width_img) but the UV normalization used the SetTileSize
		// clamp (width), stretching the texture slightly.  The real
		// fix is in libultraship — see ImportTexture* clamp to
		// texture_tile.lrs — so we no longer need to touch the
		// trailing cols from the port side.
	}
}

extern "C" void portDeswizzleDecodedSprite4c(void *sprite_v, void *bitmaps_v)
{
	if (sprite_v == NULL || bitmaps_v == NULL)
		return;

	uint8_t *sprite = static_cast<uint8_t *>(sprite_v);
	uint8_t *bitmaps = static_cast<uint8_t *>(bitmaps_v);

	int16_t nbitmaps = *reinterpret_cast<int16_t *>(sprite + 0x28);
	if (nbitmaps <= 0)
		return;

	for (int i = 0; i < nbitmaps; i++)
	{
		uint8_t *bm = bitmaps + (i * 16);
		int16_t  width_img    = *reinterpret_cast<int16_t *>(bm + 0x02);
		uint32_t buf_token    = *reinterpret_cast<uint32_t *>(bm + 0x08);
		int16_t  actualHeight = *reinterpret_cast<int16_t *>(bm + 0x0C);

		void *buf = portRelocTryResolvePointer(buf_token);
		if (buf == NULL || width_img <= 0 || actualHeight <= 0)
			continue;

		uintptr_t key = reinterpret_cast<uintptr_t>(buf);
		if (sDeswizzle4cFixups.count(key))
			continue;
		sDeswizzle4cFixups.insert(key);

		// After decode, data is 4bpp: row_bytes = width_img / 2
		size_t row_bytes = (size_t)width_img / 2;
		if (row_bytes < 8)
			continue;

		uint8_t *bytes = static_cast<uint8_t *>(buf);
		for (int row = 1; row < actualHeight; row += 2)
		{
			uint8_t *row_p = bytes + (size_t)row * row_bytes;
			for (size_t qw = 0; qw + 8 <= row_bytes; qw += 8)
			{
				uint8_t tmp[4];
				std::memcpy(tmp, row_p + qw, 4);
				std::memcpy(row_p + qw, row_p + qw + 4, 4);
				std::memcpy(row_p + qw + 4, tmp, 4);
			}
		}
	}
}

extern "C" void portFixupMObjSub(void *mobjsub)
{
	if (mobjsub == NULL)
		return;

	uintptr_t key = reinterpret_cast<uintptr_t>(mobjsub);
	if (sStructU16Fixups.count(key))
		return;
	sStructU16Fixups.insert(key);

	uint32_t *w = static_cast<uint32_t *>(mobjsub);

	// MObjSub layout (30 words = 0x78 bytes):
	//  Word  Offset  Fields                           Fixup
	//  0     0x00    u16 pad00, u8 fmt, u8 siz        bswap32 (mixed u16+u8)
	//  1     0x04    u32 sprites (token)               (ok)
	//  2     0x08    u16 unk08, u16 unk0A              rotate16
	//  3     0x0C    u16 unk0C, u16 unk0E              rotate16
	//  4     0x10    s32 unk10                          (ok)
	//  5-10  0x14    f32 trau..unk28                    (ok)
	//  11    0x2C    u32 palettes (token)               (ok)
	//  12    0x30    u16 flags, u8 block_fmt, u8 block_siz  bswap32
	//  13    0x34    u16 block_dxt, u16 unk36           rotate16
	//  14    0x38    u16 unk38, u16 unk3A               rotate16
	//  15-18 0x3C    f32 scrollu..unk48                 (ok)
	//  19    0x4C    u32 unk4C                          (ok)
	//  20    0x50    SYColorPack primcolor (u8 rgba)    bswap32
	//  21    0x54    u8 prim_l, u8 prim_m, u8[2] pad   bswap32
	//  22    0x58    SYColorPack envcolor               bswap32
	//  23    0x5C    SYColorPack blendcolor             bswap32
	//  24    0x60    SYColorPack light1color            bswap32
	//  25    0x64    SYColorPack light2color            bswap32
	//  26-29 0x68    s32 unk68..unk74                   (ok)

	fixup_bswap32(&w[0]);    // pad00 + fmt + siz (pad16 unused, u8 fields ok)
	// w[1]: sprites token — ok
	fixup_rotate16(&w[2]);   // unk08, unk0A
	fixup_rotate16(&w[3]);   // unk0C, unk0E
	// w[4..11]: s32/f32/u32 — ok
	fixup_u16_u8u8(&w[12]);  // u16 flags + u8 block_fmt + u8 block_siz
	fixup_rotate16(&w[13]);  // block_dxt, unk36
	fixup_rotate16(&w[14]);  // unk38, unk3A
	// w[15..19]: f32/u32 — ok
	fixup_bswap32(&w[20]);   // primcolor
	fixup_bswap32(&w[21]);   // prim_l, prim_m, pad
	fixup_bswap32(&w[22]);   // envcolor
	fixup_bswap32(&w[23]);   // blendcolor
	fixup_bswap32(&w[24]);   // light1color
	fixup_bswap32(&w[25]);   // light2color
	// w[26..29]: s32 — ok
}

extern "C" void *portFixupFTTexturePartContainer(void *container)
{
	/* FTTexturePartContainer = `FTTexturePart textureparts[2]` where each
	 * entry is `{u8 joint_id; u8 detail[2];}` = 3 bytes; total sizeof = 6.
	 * After pass1 BSWAP32 the u8 fields read as the wrong byte (post-pass1
	 * byte 0 == original byte 3, etc.).
	 *
	 * Earlier attempts at a fix wrote *into* the file image. That broke
	 * for fighters whose `attr->textureparts_container` points at memory
	 * shared with adjacent reloc tokens (the C struct claim of always-6
	 * container bytes does NOT hold for every fighter — some have token-
	 * shaped data at offsets 4-7 of the resolved address). Writing the
	 * "fixed" bytes back over those tokens crashed `ftMainSetStatus`
	 * downstream when `PORT_RESOLVE` was called on the now-mutated token.
	 *
	 * Non-destructive approach: return a static-storage corrected COPY of
	 * the 6 container bytes. The original file image is never modified, so
	 * adjacent token data is untouched and downstream `PORT_RESOLVE` calls
	 * on those tokens still work. Caller dereferences the returned pointer
	 * for `textureparts[0..1]` reads — that data is correct.
	 *
	 * The static copy is single-instance: callers must not interleave
	 * lookups for two different containers without consuming the result of
	 * the first. ftparam.c's three call sites each take a single snapshot
	 * inside one function — safe. */
	if (container == NULL)
		return NULL;

	static uint8_t sFixed[8];   /* 8 bytes for container alignment safety */
	uint8_t *src = static_cast<uint8_t *>(container);
	sFixed[0] = src[3];   /* bswap of word 0 (bytes 0..3) */
	sFixed[1] = src[2];
	sFixed[2] = src[1];
	sFixed[3] = src[0];
	sFixed[4] = src[7];   /* pass1 moved original byte 4 → position 7 */
	sFixed[5] = src[6];   /* pass1 moved original byte 5 → position 6 */
	sFixed[6] = 0;        /* trailing pad (not read by C struct) */
	sFixed[7] = 0;
	return sFixed;
}

extern "C" void portFixupFTAttributes(void *attr)
{
	if (attr == NULL)
		return;

	uintptr_t key = reinterpret_cast<uintptr_t>(attr);
	if (sStructU16Fixups.count(key))
		return;
	sStructU16Fixups.insert(key);

	uint32_t *w = static_cast<uint32_t *>(attr);

	// FTAttributes layout (0x348 bytes = 210 words):
	// Words 0x00..0x2C: f32/s32 physics fields — ok
	// Words 0x27..0x2B: MPObjectColl (4 f32) + Vec2f (2 f32) — ok
	//
	//  Word  Offset  Fields                               Fixup
	//  0x2D  0x0B4   u16 dead_fgm_ids[0], [1]            rotate16
	//  0x2E  0x0B8   u16 deadup_sfx, u16 damage_sfx      rotate16
	//  0x2F  0x0BC   u16 smash_sfx[0], smash_sfx[1]      rotate16
	//  0x30  0x0C0   u16 smash_sfx[2], pad                rotate16
	// Words 0x31..0x38: FTItemPickup (8 f32) — ok
	//  0x39  0x0E4   u16 itemthrow_vel_scale, damage_scale rotate16
	//  0x3A  0x0E8   u16 heavyget_sfx, pad                rotate16
	// Word 0x3B: f32 halo_size — ok
	//  0x3C  0x0F0   SYColorRGBA shade_color[0]           bswap32
	//  0x3D  0x0F4   SYColorRGBA shade_color[1]           bswap32
	//  0x3E  0x0F8   SYColorRGBA shade_color[2]           bswap32
	//  0x3F  0x0FC   SYColorRGBA fog_color                bswap32
	// Word 0x40: is_have_* bitfields — handled by #if IS_BIG_ENDIAN in FTAttributes decl
	// Words 0x41..end: DamageCollDescs (s32/f32), pointers (u32 tokens) — ok

	fixup_rotate16(&w[0x2D]);  // dead_fgm_ids[0], [1]
	fixup_rotate16(&w[0x2E]);  // deadup_sfx, damage_sfx
	fixup_rotate16(&w[0x2F]);  // smash_sfx[0], smash_sfx[1]
	fixup_rotate16(&w[0x30]);  // smash_sfx[2], pad
	fixup_rotate16(&w[0x39]);  // itemthrow_vel_scale, itemthrow_damage_scale
	fixup_rotate16(&w[0x3A]);  // heavyget_sfx, pad
	fixup_bswap32(&w[0x3C]);   // shade_color[0] rgba
	fixup_bswap32(&w[0x3D]);   // shade_color[1] rgba
	fixup_bswap32(&w[0x3E]);   // shade_color[2] rgba
	fixup_bswap32(&w[0x3F]);   // fog_color rgba
}

/* =========================================================================
 * portMarkSyntheticSprite — pre-register a port-built Sprite so that all
 * byteswap/fixup passes become no-ops when the game calls
 * lbCommonMakeSObjForGObj → portFixupSprite / portFixupBitmapArray /
 * portFixupSpriteBitmapData on it.
 *
 * The caller must have already set all struct fields in native LE order and
 * stored pixel buffers in Big-endian RGBA16 linear (no TMEM odd-row swizzle).
 * ========================================================================= */
extern "C" void portMarkSyntheticSprite(void *sprite, void *bitmaps,
                                         unsigned int nbitmaps,
                                         void *const *buf_ptrs)
{
	if (sprite)
		sStructU16Fixups.insert(reinterpret_cast<uintptr_t>(sprite));

	if (bitmaps)
	{
		// Register each Bitmap's base address so portFixupBitmapArray's
		// per-entry portFixupBitmap calls are no-ops.
		// portFixupBitmapArray's portRegisterProtectedStructRange call will
		// still execute — it adds the bitmap array to the BSWAP-protection
		// list, which is harmless for synthetic sprites.
		uint8_t *bm = static_cast<uint8_t *>(bitmaps);
		for (unsigned int i = 0; i < nbitmaps; i++)
			sStructU16Fixups.insert(reinterpret_cast<uintptr_t>(bm + i * 16));
	}

	// Register each pixel buffer in both tracking sets so:
	//   sStructU16Fixups → portFixupSpriteBitmapData skips BSWAP32 pass
	//   sDeswizzle4cFixups → portFixupSpriteBitmapData skips deswizzle pass
	if (buf_ptrs)
	{
		for (unsigned int i = 0; i < nbitmaps; i++)
		{
			if (buf_ptrs[i] == nullptr) continue;
			uintptr_t key = reinterpret_cast<uintptr_t>(buf_ptrs[i]);
			sStructU16Fixups.insert(key);
			sDeswizzle4cFixups.insert(key);
		}
	}
}
