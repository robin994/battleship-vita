#include "HiResPack.h"

#include "../port_log.h"

#include <libultraship/libultraship.h>
#include <ship/utils/filesystemtools/Directory.h>

// stb_image's implementation lives in libultraship's stb_impl.c (see
// libultraship/cmake/dependencies/common.cmake) — only include the header
// here to pick up the function declarations.
#include <stb_image.h>

// libzip — already on the include path via libultraship.h → classes.h →
// O2rArchive.h (the engine's .o2r loader is libzip-based). Used here so a
// pack can be dropped into mods/ as a .zip and read in place, with no
// extraction step (the distributed pack format on every platform).
#include <zip.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <list>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#if defined(__vita__)
#include <png.h>
#include <psp2/io/dirent.h>
#endif

namespace ssb64::hires {

namespace {

struct HashKeyHasher {
    size_t operator()(const HashKey& k) const noexcept {
        // FNV-1a over the four payload bytes of rgba8Crc is plenty — the
        // CRC32 itself already approximates uniform distribution; folding
        // (fmt|siz) avoids degenerate collisions when packs duplicate the
        // same RGBA8 image across multiple N64 formats.
        uint64_t h = 1469598103934665603ULL;
        for (int i = 0; i < 4; i++) {
            h ^= (k.rgba8Crc >> (i * 8)) & 0xFFu;
            h *= 1099511628211ULL;
        }
        h ^= (uint64_t)k.fmt << 32;
        h ^= (uint64_t)k.siz << 40;
        h *= 1099511628211ULL;
        return (size_t)h;
    }
};

// Where a parsed pack PNG lives. A pack entry is either a loose file on disk
// (member empty) or a member inside a .zip dropped in mods/ (member = the
// entry name inside the archive at `container`). Same HashKey grammar either
// way — only the decode source differs (stbi_load vs stbi_load_from_memory).
struct PackEntry {
    std::string container; // loose: the .png path; zip: the .zip path
    std::string member;    // empty for loose; entry name within the zip
    bool inZip() const noexcept { return !member.empty(); }
};

// Index of all parsed pack PNGs (loose + zip members). Lookup() reads it.
std::unordered_map<HashKey, PackEntry, HashKeyHasher> gIndex;

// Open zip handles, keyed by .zip path, kept alive for the process so we don't
// re-parse a pack's central directory on every first-decode. Single-threaded
// (the hook runs on the game/render thread), so one shared handle per zip is
// safe to reuse sequentially.
std::unordered_map<std::string, zip_t*> gOpenZips;

std::string gModsRoot;

// Per-process dedup so dump-mode writes each unique key at most once per
// run. Protects the disk from being hammered when the same texture cycles
// through the texture cache repeatedly.
std::unordered_set<uint64_t> gDumpedKeys;
std::string gDumpDir;
bool gDumpDirReady = false;

constexpr uint32_t kDumpMagic   = 0x44524853u; // 'SHRD' little-endian
constexpr uint32_t kDumpVersion = 1u;
struct DumpHeader {
    uint32_t magic;       // 'SHRD'
    uint32_t version;     // 1
    uint32_t width;       // post-mask/clamp pixel width
    uint32_t height;      // post-mask/clamp pixel height
    uint32_t bpl;         // source row stride in bytes
    uint32_t texelBytes;  // = bytesPerLine * height
    uint32_t paletteBytes;
    uint8_t  fmt;         // G_IM_FMT_*
    uint8_t  siz;         // G_IM_SIZ_*
    uint8_t  pad[6];
};
static_assert(sizeof(DumpHeader) == 36, "DumpHeader must be 36 bytes");

uint64_t SourceDumpDedupId(uint8_t fmt, uint8_t siz, uint32_t texelCrc, uint32_t palCrc) noexcept {
    return (((uint64_t)texelCrc) | ((uint64_t)palCrc << 32)) ^ ((uint64_t)fmt << 8) ^ ((uint64_t)siz << 16);
}

// CRC32-IEEE 802.3 (poly 0xEDB88320) — public-domain table-driven impl.
// Not Rice CRC32: that algorithm originated in GPL N64 plugins and is
// license-incompatible with this codebase. Pack PNGs intended for this
// port are named with this hash instead, produced offline by a separate
// (unbundled, GPL) conversion tool that decodes each source-byte dump to
// RGBA8 and computes the same plain CRC32-IEEE over the decoded pixels.
struct Crc32Table {
    uint32_t v[256];
    constexpr Crc32Table() : v{} {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int j = 0; j < 8; j++) c = (c >> 1) ^ (0xEDB88320u & -(c & 1u));
            v[i] = c;
        }
    }
};
const Crc32Table gCrcTbl;

// Plain CRC32-IEEE over a flat byte run. Used for the decoded-RGBA8 hash
// (post-decode, no row stride) and for hashing source bytes / palette
// bytes in dump-source mode.
uint32_t Crc32Bytes(const uint8_t* src, size_t bytes) noexcept {
    if (src == nullptr || bytes == 0) return 0;
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < bytes; i++) {
        crc = gCrcTbl.v[(crc ^ src[i]) & 0xFFu] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

// CRC32-IEEE over a row-strided source-byte image — kept for the
// dump-source helper, which still records source bytes so the offline
// conversion tool can pair Reloaded packs to our decoded-RGBA8 names.
uint32_t SourceTexelCrc(const uint8_t* src, int width, int height,
                        int size, int rowStride) noexcept {
    if (src == nullptr || width <= 0 || height <= 0 || rowStride <= 0) return 0;
    const int bytesPerLine = (width << size) >> 1;
    if (bytesPerLine <= 0) return 0;

    uint32_t crc = 0xFFFFFFFFu;
    for (int y = 0; y < height; y++) {
        const uint8_t* row = src + (size_t)y * rowStride;
        for (int x = 0; x < bytesPerLine; x++) {
            crc = gCrcTbl.v[(crc ^ row[x]) & 0xFFu] ^ (crc >> 8);
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

// Decoded-RGBA LRU. Insert always succeeds (the just-inserted entry is at
// the tail and is exempt from eviction); eviction only fires on subsequent
// Insert() calls, never during a single Lookup() — so the pointer returned
// from Lookup() is guaranteed valid through the immediately-following
// UploadTexture call.
class LruCache {
public:
    using Node = std::pair<HashKey, DecodedTexture>;

    explicit LruCache(size_t budgetBytes) : mBudget(budgetBytes) {}

    const DecodedTexture* Get(const HashKey& k) {
        auto it = mIndex.find(k);
        if (it == mIndex.end()) return nullptr;
        // Move the hit to MRU end.
        mList.splice(mList.end(), mList, it->second);
        return &it->second->second;
    }

    void Insert(const HashKey& k, DecodedTexture&& tex) {
        // If the same key already lives in the cache, drop the older entry.
        if (auto it = mIndex.find(k); it != mIndex.end()) {
            mBytes -= it->second->second.rgba.size();
            mList.erase(it->second);
            mIndex.erase(it);
        }
        size_t addBytes = tex.rgba.size();
        mList.emplace_back(k, std::move(tex));
        mIndex[k] = std::prev(mList.end());
        mBytes += addBytes;
        EvictToBudget();
    }

    // Re-point the budget after construction. The cache is built at static-init
    // time (before CVars load), so HiResPack::Init reads the
    // gHiResTextures.CacheBudgetMB CVar and calls this once config is up.
    void SetBudget(size_t budgetBytes) {
        mBudget = budgetBytes;
        EvictToBudget();
    }

    // Drop all decoded entries (called from Init so a re-scan with a changed
    // pack can't return a stale decoded texture on an LRU hit that predates
    // the new index).
    void Clear() {
        mList.clear();
        mIndex.clear();
        mBytes = 0;
    }

    size_t Bytes() const noexcept { return mBytes; }
    size_t Budget() const noexcept { return mBudget; }

private:
    // Evict from the LRU end (front) until back under budget. Never evict the
    // tail (most-recently inserted) — Lookup returns a pointer into it that
    // must stay valid through the immediately-following UploadTexture call.
    void EvictToBudget() {
        while (mBytes > mBudget && mList.size() > 1) {
            auto& front = mList.front();
            mBytes -= front.second.rgba.size();
            mIndex.erase(front.first);
            mList.pop_front();
        }
    }

    std::list<Node> mList;
    std::unordered_map<HashKey, std::list<Node>::iterator, HashKeyHasher> mIndex;
    size_t mBytes = 0;
    size_t mBudget;
};

// Initial budget from the platform default in HiResPack.h; HiResPack::Init
// re-points it from the gHiResTextures.CacheBudgetMB CVar once config loads.
LruCache gLru{ (size_t)kDefaultLruBudgetMB * 1024u * 1024u };

#if defined(__vita__)
uint64_t gVitaLruHits = 0;
uint64_t gVitaDecodeCount = 0;
uint64_t gVitaDecodeTotalUs = 0;
uint64_t gVitaDecodeMaxUs = 0;
#endif

bool IsHexDigit(char c) noexcept {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
}

bool ParseHex32(std::string_view s, uint32_t* out) noexcept {
    if (s.size() != 8) return false;
    uint32_t v = 0;
    for (char c : s) {
        if (!IsHexDigit(c)) return false;
        uint32_t d = (c <= '9') ? (c - '0') : ((c | 0x20) - 'a' + 10);
        v = (v << 4) | d;
    }
    *out = v;
    return true;
}

bool ParseSingleDigit(std::string_view s, uint8_t* out) noexcept {
    if (s.size() != 1 || s[0] < '0' || s[0] > '9') return false;
    *out = (uint8_t)(s[0] - '0');
    return true;
}

/* Parse a hires-pack filename into a HashKey.
 *
 *   <prefix>#<rgba8CRC8>#<fmt>#<siz>[_<anything>].png
 *
 * Returns nullopt on grammar mismatch. We accept any prefix (including
 * none) — the prefix is decorative.
 *
 * Examples that parse successfully:
 *   "SMASH BROTHERS#A1B2C3D4#0#2_all.png"   RGBA16 (decoded RGBA8 hash)
 *   "anything#cafe1234#3#0.png"             IA4 (no fmtTag suffix)
 *   "anything#cafe1234#3#0_anything.png"    IA4 with arbitrary fmtTag
 *
 * The fmt+siz fields are kept in the filename as a sanity check / human-
 * readable hint, but they're folded into the HashKey so two textures whose
 * decoded RGBA8 happens to collide across formats stay distinct.
 */
std::optional<HashKey> ParseFilename(std::string_view filename) {
    // Strip trailing ".png" (case-insensitive).
    if (filename.size() < 5) return std::nullopt;
    std::string_view ext = filename.substr(filename.size() - 4);
    bool isPng = (ext.size() == 4)
        && (ext[0] == '.')
        && ((ext[1] | 0x20) == 'p')
        && ((ext[2] | 0x20) == 'n')
        && ((ext[3] | 0x20) == 'g');
    if (!isPng) return std::nullopt;
    std::string_view stem = filename.substr(0, filename.size() - 4);

    // Optional trailing _<fmtTag>: strip everything from the rightmost '_'
    // forward IF it sits after the last '#' (so we don't eat underscores
    // inside the prefix).
    size_t lastHash = stem.rfind('#');
    size_t lastUnder = stem.rfind('_');
    if (lastUnder != std::string_view::npos && (lastHash == std::string_view::npos || lastUnder > lastHash)) {
        stem = stem.substr(0, lastUnder);
    }

    // Split the body on '#' from the right: <prefix>#<crc>#<fmt>#<siz>.
    size_t h3 = stem.rfind('#');
    if (h3 == std::string_view::npos) return std::nullopt;
    size_t h2 = stem.rfind('#', h3 - 1);
    if (h2 == std::string_view::npos) return std::nullopt;
    size_t h1 = stem.rfind('#', h2 - 1);
    if (h1 == std::string_view::npos) return std::nullopt;

    std::string_view crcField = stem.substr(h1 + 1, h2 - h1 - 1);
    std::string_view fmtField = stem.substr(h2 + 1, h3 - h2 - 1);
    std::string_view sizField = stem.substr(h3 + 1);

    HashKey key{};
    if (!ParseHex32(crcField, &key.rgba8Crc)) return std::nullopt;
    if (!ParseSingleDigit(fmtField, &key.fmt)) return std::nullopt;
    if (!ParseSingleDigit(sizField, &key.siz)) return std::nullopt;

    return key;
}

// Basename (after the last '/' or '\\') of a path or zip entry name.
std::string_view Basename(std::string_view p) noexcept {
    size_t slash = p.find_last_of("/\\");
    return slash == std::string_view::npos ? p : p.substr(slash + 1);
}

// True if `name` ends in ".<ext>" (case-insensitive). `ext` is 3 lowercase chars.
bool HasExt(std::string_view name, const char* ext) noexcept {
    if (name.size() < 4) return false;
    std::string_view e = name.substr(name.size() - 4);
    return e[0] == '.' && (e[1] | 0x20) == ext[0] && (e[2] | 0x20) == ext[1] && (e[3] | 0x20) == ext[2];
}

#if defined(__vita__)
// VitaSDK's std::filesystem::directory_entry::is_directory() can return EINVAL
// for otherwise valid ux0: paths (observed with Reloaded pack filenames).  The
// generic Directory::ListFiles() calls that status() path for every entry, so
// one bad result aborts the whole recursive scan.  Use SceIo's directory API on
// Vita instead: d_stat is populated by sceIoDread, so no extra status() syscall
// is needed and an unreadable subdirectory only skips that subtree.
std::vector<std::string> ListPackFilesVita(const std::string& root) {
    std::vector<std::string> files;
    std::vector<std::string> pending{root};
    size_t loggedErrors = 0;
    size_t suppressedErrors = 0;
    constexpr size_t kMaxLoggedErrors = 8;

    while (!pending.empty()) {
        std::string dir = std::move(pending.back());
        pending.pop_back();

        SceUID dfd = sceIoDopen(dir.c_str());
        if (dfd < 0) {
            if (loggedErrors < kMaxLoggedErrors) {
                port_log("HiResPack: cannot open directory %s (0x%08X) — skipping subtree\n",
                         dir.c_str(), (unsigned int)dfd);
                loggedErrors++;
            } else {
                suppressedErrors++;
            }
            continue;
        }

        for (;;) {
            SceIoDirent ent{};
            int rc = sceIoDread(dfd, &ent);
            if (rc <= 0) {
                if (rc < 0) {
                    if (loggedErrors < kMaxLoggedErrors) {
                        port_log("HiResPack: cannot read directory %s (0x%08X) — keeping entries scanned so far\n",
                                 dir.c_str(), (unsigned int)rc);
                        loggedErrors++;
                    } else {
                        suppressedErrors++;
                    }
                }
                break;
            }

            std::string_view name = ent.d_name;
            if (name == "." || name == "..") continue;

            std::string path = dir;
            if (!path.empty() && path.back() != '/') path.push_back('/');
            path.append(name.data(), name.size());

            if (SCE_S_ISDIR(ent.d_stat.st_mode) || SCE_SO_ISDIR(ent.d_stat.st_attr)) {
                pending.emplace_back(std::move(path));
            } else {
                files.emplace_back(std::move(path));
            }
        }

        sceIoDclose(dfd);
    }

    if (suppressedErrors != 0) {
        port_log("HiResPack: suppressed %u additional Vita directory scan error(s)\n",
                 (unsigned int)suppressedErrors);
    }
    return files;
}
#endif

// Insert a parsed entry, applying the "last scan wins" collision rule (matches
// the loose-file ordering) and updating stats. Shared by both scanners.
void IndexEntry(const HashKey& key, PackEntry entry, PackStats& stats) {
    auto it = gIndex.find(key);
    if (it == gIndex.end()) {
        gIndex.emplace(key, std::move(entry));
        stats.indexedTextures++;
    } else {
        it->second = std::move(entry); // last-scanned wins
        stats.collisions++;
    }
}

// Open a pack .zip read-only and cache the handle for the process lifetime so
// we don't re-parse its central directory on every first-decode. A null handle
// is cached too, so a broken zip isn't retried.
zip_t* OpenZipCached(const std::string& path) {
    if (auto it = gOpenZips.find(path); it != gOpenZips.end()) return it->second;
    int err = 0;
    zip_t* z = zip_open(path.c_str(), ZIP_RDONLY, &err);
    if (z == nullptr) {
        zip_error_t ze;
        zip_error_init_with_code(&ze, err);
        port_log("HiResPack: cannot open zip %s (%s)\n", path.c_str(), zip_error_strerror(&ze));
        zip_error_fini(&ze);
    }
    gOpenZips[path] = z;
    return z;
}

// Enumerate a .zip's entries and index every member whose basename matches the
// pack grammar. The central-directory read is one shot — far cheaper than the
// recursive directory walk a loose pack needs (and it sidesteps the per-file
// scoped-storage traversal cost on Android).
void ScanZip(const std::string& zipPath, PackStats& stats) {
    zip_t* z = OpenZipCached(zipPath);
    if (z == nullptr) return;
    zip_int64_t n = zip_get_num_entries(z, 0);
    if (n < 0) { // corrupt central directory; signed n makes the loop skip anyway
        port_log("HiResPack: zip_get_num_entries failed for %s — treating as empty\n",
                 zipPath.c_str());
        return;
    }
    for (zip_int64_t i = 0; i < n; i++) {
        const char* name = zip_get_name(z, i, 0);
        if (name == nullptr) continue;
        auto key = ParseFilename(Basename(name));
        if (!key) { stats.skippedFilenames++; continue; }
        IndexEntry(*key, PackEntry{zipPath, std::string(name)}, stats);
    }
}

// Upper bound on a single decoded zip member (the compressed PNG bytes we
// read into memory). A pack is untrusted input — without a cap a crafted or
// corrupt entry whose stat declares a huge uncompressed size would drive
// out.resize() into an uncaught std::bad_alloc (crash). Real pack PNGs top out
// around ~25 MB; 256 MB is generous headroom and stays well under INT_MAX so
// the later (int)bytes.size() cast for stbi_load_from_memory can't overflow.
constexpr uint64_t kMaxPackMemberBytes = 256ull * 1024ull * 1024ull;

// Read a zip member fully into `out`. Returns false on any libzip error, an
// out-of-range member size, or an allocation failure.
bool ReadZipMember(const std::string& zipPath, const std::string& member,
                   std::vector<uint8_t>& out) {
    zip_t* z = OpenZipCached(zipPath);
    if (z == nullptr) return false;
    zip_stat_t st;
    zip_stat_init(&st);
    if (zip_stat(z, member.c_str(), 0, &st) != 0 || !(st.valid & ZIP_STAT_SIZE)) return false;
    // Reject empty / absurd members before allocating from an untrusted stat.
    if (st.size == 0 || st.size > kMaxPackMemberBytes) {
        port_log("HiResPack: zip member %s size %llu out of range — skipping\n",
                 member.c_str(), (unsigned long long)st.size);
        return false;
    }
    zip_file_t* zf = zip_fopen(z, member.c_str(), 0);
    if (zf == nullptr) return false;
    try {
        out.resize((size_t)st.size);
    } catch (const std::bad_alloc&) {
        port_log("HiResPack: out of memory reading zip member %s (%llu bytes)\n",
                 member.c_str(), (unsigned long long)st.size);
        zip_fclose(zf);
        return false;
    }
    zip_int64_t rd = zip_fread(zf, out.data(), st.size);
    zip_fclose(zf);
    return rd >= 0 && (zip_uint64_t)rd == st.size;
}

#if defined(__vita__)
enum class VitaDecodeResult {
    Ok,
    Failed,
    Oversize,
    OutOfMemory,
};

// stb_image's PNG inflater is convenient but very slow on Vita's ARM CPU,
// especially for the 512-1024px textures used by Reloaded. libpng is already
// linked by the Vita build and delegates DEFLATE to zlib, which is much faster
// here. Decode directly into the LRU-owned RGBA vector to avoid stb's temporary
// RGBA allocation plus the second full-image copy previously done by Lookup().
VitaDecodeResult DecodePackEntryVita(const PackEntry& entry, DecodedTexture& tex,
                                     std::string& error) {
    png_image image{};
    image.version = PNG_IMAGE_VERSION;

    std::vector<uint8_t> zipBytes;
    int began = 0;
    if (entry.inZip()) {
        if (!ReadZipMember(entry.container, entry.member, zipBytes)) {
            error = "cannot read zip member";
            return VitaDecodeResult::Failed;
        }
        began = png_image_begin_read_from_memory(&image, zipBytes.data(), zipBytes.size());
    } else {
        began = png_image_begin_read_from_file(&image, entry.container.c_str());
    }

    if (!began) {
        error = image.message[0] ? image.message : "png begin-read failed";
        png_image_free(&image);
        return VitaDecodeResult::Failed;
    }

    if (image.width == 0 || image.height == 0 || image.width > 65535 || image.height > 65535) {
        error = "invalid dimensions";
        png_image_free(&image);
        return VitaDecodeResult::Failed;
    }

    const size_t texels = (size_t)image.width * (size_t)image.height;
    if (kMaxPackTexels != 0 && texels > kMaxPackTexels) {
        tex.w = (uint16_t)image.width;
        tex.h = (uint16_t)image.height;
        png_image_free(&image);
        return VitaDecodeResult::Oversize;
    }

    image.format = PNG_FORMAT_RGBA;
    try {
        tex.rgba.resize((size_t)PNG_IMAGE_SIZE(image));
    } catch (const std::bad_alloc&) {
        png_image_free(&image);
        return VitaDecodeResult::OutOfMemory;
    }

    if (!png_image_finish_read(&image, nullptr, tex.rgba.data(), 0, nullptr)) {
        error = image.message[0] ? image.message : "png finish-read failed";
        tex.rgba.clear();
        png_image_free(&image);
        return VitaDecodeResult::Failed;
    }

    tex.w = (uint16_t)image.width;
    tex.h = (uint16_t)image.height;
    png_image_free(&image);
    return VitaDecodeResult::Ok;
}
#endif

} // namespace

HiResPack& HiResPack::Get() {
    static HiResPack inst;
    return inst;
}

const char* HiResPack::ModsRoot() const noexcept {
    return gModsRoot.c_str();
}

bool HiResPack::Init() {
    mStats = {};
    gIndex.clear();
    gLru.Clear(); // drop decoded textures so a re-scan can't serve stale hits
#if defined(__vita__)
    gVitaLruHits = 0;
    gVitaDecodeCount = 0;
    gVitaDecodeTotalUs = 0;
    gVitaDecodeMaxUs = 0;
#endif
    // Close any zip handles from a prior Init before re-scanning.
    for (auto& [path, z] : gOpenZips) {
        if (z != nullptr) zip_close(z);
    }
    gOpenZips.clear();

    // Resolve <app-data>/mods alongside BattleShip.o2r and ssb64_save.bin.
    // Same convention as port_save.cpp.
    try {
        gModsRoot = Ship::Context::GetPathRelativeToAppDirectory("mods");
    } catch (...) {
        gModsRoot.clear();
        port_log("HiResPack: Ship::Context not ready; mods/ disabled\n");
        return false;
    }

    // Apply the decoded-RGBA8 LRU budget now that config (CVars) is loaded.
    // Platform default (HiResPack.h) unless the user overrode it; floored so a
    // too-small value can't make the cache thrash by re-decoding every miss.
    int budgetMB = CVarGetInteger("gHiResTextures.CacheBudgetMB", kDefaultLruBudgetMB);
    if (budgetMB < kMinLruBudgetMB) budgetMB = kMinLruBudgetMB;
    gLru.SetBudget((size_t)budgetMB * 1024u * 1024u);
    port_log("HiResPack: decoded-RGBA8 LRU budget = %d MB%s\n", budgetMB,
             kMaxPackTexels ? " (mobile per-texture upscale cap active)" : "");

    if (!Directory::Exists(gModsRoot)) {
        port_log("HiResPack: %s does not exist; create it and drop a pack inside to enable\n",
                 gModsRoot.c_str());
        return false;
    }

    // ListFiles' recursive iterator throws on a bad mods/ (locked subdir,
    // symlink loop, a file removed mid-walk) — catch it so a junk folder just
    // disables the pack instead of taking down boot.
    try {
        std::vector<std::string> files;
#if defined(__vita__)
        files = ListPackFilesVita(gModsRoot);
#else
        files = Directory::ListFiles(gModsRoot); // recursive
#endif
        std::sort(files.begin(), files.end()); // deterministic collision-winner

        for (const std::string& path : files) {
            mStats.scannedFiles++;
            std::string_view name = Basename(path);

            // A .zip pack is read in place: enumerate + index its members. This
            // is the distributed pack format — desktop users drop the zip into
            // mods/, and the Android importer copies the downloaded zip here.
            if (HasExt(name, "zip")) {
                ScanZip(path, mStats);
                continue;
            }

            // Otherwise index a loose .png (member empty → decoded via stbi_load).
            if (!HasExt(name, "png")) continue;
            auto key = ParseFilename(name);
            if (!key) {
                mStats.skippedFilenames++;
                continue;
            }
            IndexEntry(*key, PackEntry{path, std::string()}, mStats);
        }
    } catch (const std::exception& e) {
        port_log("HiResPack: cannot scan %s (%s); hi-res pack disabled this run\n",
                 gModsRoot.c_str(), e.what());
        gIndex.clear();
        return false;
    }

    port_log("HiResPack: scanned %s — %u files, %u indexed, %u unparsed, %u hash collisions, %u zip(s)\n",
             gModsRoot.c_str(),
             (unsigned int)mStats.scannedFiles, (unsigned int)mStats.indexedTextures,
             (unsigned int)mStats.skippedFilenames, (unsigned int)mStats.collisions, (unsigned int)gOpenZips.size());
    return true;
}

const DecodedTexture* HiResPack::Lookup(uint8_t fmt, uint8_t siz,
                                         const uint8_t* rgba8,
                                         uint16_t width, uint16_t height) {
    if (gIndex.empty() || rgba8 == nullptr || width == 0 || height == 0) return nullptr;

    mLookupStats.lookups++;

    HashKey key{};
    key.fmt = fmt;
    key.siz = siz;
    key.rgba8Crc = Crc32Bytes(rgba8, (size_t)width * height * 4);

    // Per-call diagnostic + hit/miss flag for the first 8 lookups — lets us
    // confirm the hook is firing on real boots without spamming the log
    // during normal play.
    bool will_hit = gIndex.find(key) != gIndex.end();
    if (mLookupStats.lookups <= 8) {
        port_log("HiResPack: lookup #%llu fmt=%u siz=%u w=%u h=%u "
                 "key=%08X#%X#%X %s\n",
                 (unsigned long long)mLookupStats.lookups,
                 fmt, siz, width, height,
                 key.rgba8Crc, key.fmt, key.siz,
                 will_hit ? "HIT" : "miss");
    }
    // Periodic log so the user can see coverage drift while playing — every
    // 64 cache misses (a few seconds of typical gameplay).
    if ((mLookupStats.lookups & 0x3F) == 0) {
        double rate = mLookupStats.lookups
            ? (100.0 * (double)mLookupStats.hits / (double)mLookupStats.lookups)
            : 0.0;
        port_log("HiResPack: %llu lookups, %llu hits (%.1f%%), %llu decode-fails, LRU=%u MB",
                 (unsigned long long)mLookupStats.lookups,
                 (unsigned long long)mLookupStats.hits,
                 rate,
                 (unsigned long long)mLookupStats.decodeFails,
                 (unsigned int)(gLru.Bytes() / (1024u * 1024u)));
#if defined(__vita__)
        const uint64_t avgUs = gVitaDecodeCount ? (gVitaDecodeTotalUs / gVitaDecodeCount) : 0;
        port_log(" lru-hits=%llu decodes=%llu decode-avg=%llums max=%llums\n",
                 (unsigned long long)gVitaLruHits,
                 (unsigned long long)gVitaDecodeCount,
                 (unsigned long long)(avgUs / 1000u),
                 (unsigned long long)(gVitaDecodeMaxUs / 1000u));
#else
        port_log("\n");
#endif
    }

    if (const DecodedTexture* hit = gLru.Get(key)) {
#if defined(__vita__)
        gVitaLruHits++;
#endif
        mLookupStats.hits++;
        return hit;
    }

    auto it = gIndex.find(key);
    if (it == gIndex.end()) {
        return nullptr;
    }

    const PackEntry& entry = it->second;
#if defined(__vita__)
    DecodedTexture tex;
    std::string decodeError;
    const auto decodeStart = std::chrono::steady_clock::now();
    VitaDecodeResult result = DecodePackEntryVita(entry, tex, decodeError);
    const auto decodeEnd = std::chrono::steady_clock::now();
    const uint64_t decodeUs = (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
                                  decodeEnd - decodeStart).count();
    gVitaDecodeCount++;
    gVitaDecodeTotalUs += decodeUs;
    gVitaDecodeMaxUs = std::max(gVitaDecodeMaxUs, decodeUs);

    if (result == VitaDecodeResult::Oversize) {
        port_log("HiResPack: %s decodes to %ux%u (> Vita %u-texel cap) - using native texture\n",
                 entry.container.c_str(), (unsigned int)tex.w, (unsigned int)tex.h,
                 (unsigned int)kMaxPackTexels);
        gIndex.erase(it);
        return nullptr;
    }
    if (result != VitaDecodeResult::Ok) {
        port_log("HiResPack: libpng decode failed for %s%s%s (%s)\n",
                 entry.container.c_str(), entry.inZip() ? " :: " : "",
                 entry.inZip() ? entry.member.c_str() : "",
                 result == VitaDecodeResult::OutOfMemory ? "out of memory" : decodeError.c_str());
        gIndex.erase(it);
        mLookupStats.decodeFails++;
        return nullptr;
    }

    gLru.Insert(key, std::move(tex));
    mLookupStats.hits++;
    return gLru.Get(key);
#else
    int w = 0, h = 0, ch = 0;
    uint8_t* raw = nullptr;
    if (entry.inZip()) {
        // Decode straight from the zip member — no extraction to disk.
        std::vector<uint8_t> bytes;
        if (ReadZipMember(entry.container, entry.member, bytes)) {
            raw = stbi_load_from_memory(bytes.data(), (int)bytes.size(), &w, &h, &ch, 4);
        }
    } else {
        raw = stbi_load(entry.container.c_str(), &w, &h, &ch, 4);
    }
    if (raw == nullptr || w <= 0 || h <= 0 || w > 65535 || h > 65535) {
        if (raw) stbi_image_free(raw);
        port_log("HiResPack: decode failed for %s%s%s (%s)\n",
                 entry.container.c_str(), entry.inZip() ? " :: " : "",
                 entry.inZip() ? entry.member.c_str() : "",
                 stbi_failure_reason() ? stbi_failure_reason() : "?");
        // Drop the entry so we don't keep retrying every cache miss.
        gIndex.erase(it);
        mLookupStats.decodeFails++;
        return nullptr;
    }

    // Mobile per-texture upscale cap (kMaxPackTexels = 0 → uncapped on desktop).
    // A single oversize PNG would blow the LRU budget (the just-inserted tail is
    // never evicted) and balloon the uncompressed GPU upload, so reject it and
    // fall back to the native texture. Drop the index entry so we don't re-decode
    // the same monster on every cache miss.
    if (kMaxPackTexels != 0 && (size_t)w * (size_t)h > kMaxPackTexels) {
        stbi_image_free(raw);
        port_log("HiResPack: %s decodes to %dx%d (> mobile %u-texel cap) — using native texture\n",
                 entry.container.c_str(), w, h, (unsigned)kMaxPackTexels);
        gIndex.erase(it);
        return nullptr;
    }

    DecodedTexture tex;
    tex.w = (uint16_t)w;
    tex.h = (uint16_t)h;
    tex.rgba.assign(raw, raw + (size_t)w * h * 4);
    stbi_image_free(raw);

    gLru.Insert(key, std::move(tex));
    mLookupStats.hits++;
    return gLru.Get(key);
#endif
}

void HiResPack::MaybeDumpSource(uint8_t fmt, uint8_t siz,
                                 const uint8_t* texels, uint16_t width, uint16_t height, uint32_t bpl,
                                 const uint8_t* palette, uint32_t paletteBytes) {
    // Cheap CVar check up front. The hook layer reads the same CVar for the
    // master enable; reading it here keeps dump-on toggling responsive.
    if (CVarGetInteger("gHiResTextures.DumpSource", 0) == 0) return;
    if (texels == nullptr || width == 0 || height == 0 || bpl == 0) return;

    const uint32_t texelCrc = SourceTexelCrc(texels, width, height, siz, (int)bpl);
    const uint32_t palCrc = (palette != nullptr && paletteBytes > 0)
                                ? Crc32Bytes(palette, paletteBytes)
                                : 0;

    uint64_t dedup = SourceDumpDedupId(fmt, siz, texelCrc, palCrc);
    if (!gDumpedKeys.insert(dedup).second) return; // already dumped this run

    if (!gDumpDirReady) {
        try {
            gDumpDir = Ship::Context::GetPathRelativeToAppDirectory("hires_dump");
        } catch (...) { gDumpDir = "hires_dump"; }
        std::error_code ec;
        std::filesystem::create_directories(gDumpDir, ec);
        gDumpDirReady = true;
        port_log("HiResPack: dump-source mode active, writing to %s\n", gDumpDir.c_str());
    }

    // Filename includes source-byte CRC + (optionally) palette CRC so the
    // offline conversion tool can pair each .bin with its Reloaded PNG by
    // looking up Rice CRC of the dumped texel bytes.
    char namebuf[96];
    if (palCrc != 0 || paletteBytes > 0) {
        std::snprintf(namebuf, sizeof(namebuf),
                      "ssb64src#%08X#%X#%X#%08X.bin",
                      texelCrc, fmt, siz, palCrc);
    } else {
        std::snprintf(namebuf, sizeof(namebuf),
                      "ssb64src#%08X#%X#%X.bin",
                      texelCrc, fmt, siz);
    }

    std::string path = gDumpDir + "/" + namebuf;
    FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) {
        port_log("HiResPack: dump-source open failed: %s\n", path.c_str());
        return;
    }

    DumpHeader hdr{};
    hdr.magic = kDumpMagic;
    hdr.version = kDumpVersion;
    hdr.width = width;
    hdr.height = height;
    hdr.bpl = bpl;
    const uint32_t bytesPerLine = (uint32_t)((width << siz) >> 1);
    hdr.texelBytes = bytesPerLine * height;
    hdr.paletteBytes = paletteBytes;
    hdr.fmt = fmt;
    hdr.siz = siz;

    std::fwrite(&hdr, sizeof(hdr), 1, f);
    // Texel bytes — write only the active rows (bytesPerLine each, row-by-
    // row at bpl stride). The conversion tool reads them as a contiguous
    // bytesPerLine*height blob, no stride.
    for (uint32_t y = 0; y < height; y++) {
        std::fwrite(texels + (size_t)y * bpl, 1, bytesPerLine, f);
    }
    if (paletteBytes > 0 && palette != nullptr) {
        std::fwrite(palette, 1, paletteBytes, f);
    }
    std::fclose(f);
}

} // namespace ssb64::hires
