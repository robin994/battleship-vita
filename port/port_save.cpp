#include "port_save.h"
#include "port_log.h"

#include <libultraship/libultraship.h>
#include <SDL2/SDL.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <mutex>
#include <string>

#ifdef __vita__
#include <fcntl.h>
#include <unistd.h>
#endif

// Where the SRAM file lives, in priority order:
//
//   1. $SSB64_SAVE_PATH                  — explicit override (debug / CI).
//   2. Ship::Context::GetPathRelativeToAppDirectory("ssb64_save.bin")
//      — same convention as BattleShip.o2r and BattleShip.cfg.json:
//         * NON_PORTABLE=ON                                 → OS app-data dir
//             macOS    ~/Library/Application Support/BattleShip/
//             Linux    $XDG_DATA_HOME/BattleShip/  (or ~/.local/share/...)
//             Windows  %APPDATA%\BattleShip\
//         * NON_PORTABLE=OFF                                → cwd (portable)
//         * SHIP_HOME=<dir>                                 → that dir wins on macOS/Linux
//   3. SDL_GetPrefPath fallback                              — only if Ship::Context
//                                                             hasn't been constructed yet
//                                                             (shouldn't happen — SRAM
//                                                             I/O runs after PortInit —
//                                                             but defensive).
//   4. "ssb64_save.bin" in cwd                               — last-resort.

namespace {

std::once_flag gPathOnce;
std::string gSavePath;
std::mutex gSaveMutex;

void resolveSavePath()
{
    if (const char *override = std::getenv("SSB64_SAVE_PATH")) {
        gSavePath = override;
        return;
    }
    try {
        // Mirrors how BattleShip.o2r / BattleShip.cfg.json / logs/*.log
        // are located. Honors SHIP_HOME and the NON_PORTABLE build flag.
        gSavePath = Ship::Context::GetPathRelativeToAppDirectory("ssb64_save.bin");
        if (!gSavePath.empty()) {
            return;
        }
    } catch (...) {
        // Ship::Context not yet alive — fall through.
    }
    if (char *p = SDL_GetPrefPath(NULL, "BattleShip")) {
        gSavePath = std::string(p) + "ssb64_save.bin";
        SDL_free(p);
        return;
    }
    gSavePath = "ssb64_save.bin"; // last-resort cwd
}

const std::string &savePath()
{
    std::call_once(gPathOnce, resolveSavePath);
    return gSavePath;
}

bool saveRangeValid(uintptr_t offset, size_t size)
{
    return offset <= PORT_SAVE_SIZE && size <= (PORT_SAVE_SIZE - offset);
}

#ifdef __vita__
int writeAllFd(int fd, const void *src, size_t size)
{
    const unsigned char *bytes = static_cast<const unsigned char *>(src);
    size_t done = 0;
    while (done < size) {
        const ssize_t wrote = write(fd, bytes + done, size - done);
        if (wrote <= 0) return -1;
        done += static_cast<size_t>(wrote);
    }
    return 0;
}

int padFdToOffset(int fd, uintptr_t offset)
{
    off_t cur = lseek(fd, 0, SEEK_END);
    if (cur < 0) return -1;

    static const unsigned char zeros[256] = {};
    while (static_cast<uintptr_t>(cur) < offset) {
        size_t chunk = offset - static_cast<uintptr_t>(cur);
        if (chunk > sizeof(zeros)) chunk = sizeof(zeros);
        if (writeAllFd(fd, zeros, chunk) != 0) return -1;
        cur += static_cast<off_t>(chunk);
    }
    return 0;
}

int writeRangeFd(int fd, uintptr_t offset, const void *src, size_t size)
{
    if (lseek(fd, static_cast<off_t>(offset), SEEK_SET) < 0) return -1;
    return writeAllFd(fd, src, size);
}
#else
FILE *openSaveForUpdate(const char *path)
{
    FILE *f = std::fopen(path, "r+b");
    if (f == NULL) {
        f = std::fopen(path, "w+b");
    }
    return f;
}

int padToOffset(FILE *f, uintptr_t offset)
{
    if (std::fseek(f, 0, SEEK_END) != 0) return -1;
    long cur = std::ftell(f);
    if (cur < 0) return -1;

    static const char zeros[256] = {0};
    while (cur < (long)offset) {
        size_t chunk = (size_t)((long)offset - cur);
        if (chunk > sizeof(zeros)) chunk = sizeof(zeros);
        if (std::fwrite(zeros, 1, chunk, f) != chunk) return -1;
        cur += (long)chunk;
    }
    return 0;
}

int writeRange(FILE *f, uintptr_t offset, const void *src, size_t size)
{
    if (std::fseek(f, (long)offset, SEEK_SET) != 0) return -1;
    return std::fwrite(src, 1, size, f) == size ? 0 : -1;
}
#endif

} // namespace

extern "C" const char *port_save_get_path(void)
{
    return savePath().c_str();
}

extern "C" int port_save_read(uintptr_t offset, void *dst, size_t size)
{
    if (size == 0) return 0;
    if (dst == NULL || !saveRangeValid(offset, size)) return -1;

    std::lock_guard<std::mutex> lock(gSaveMutex);
    std::memset(dst, 0, size);

#ifdef __vita__
    const int fd = open(savePath().c_str(), O_RDONLY);
    if (fd < 0) return 0;
    if (lseek(fd, static_cast<off_t>(offset), SEEK_SET) < 0) {
        close(fd);
        return 0;
    }

    unsigned char *bytes = static_cast<unsigned char *>(dst);
    size_t done = 0;
    int rc = 0;
    while (done < size) {
        const ssize_t got = read(fd, bytes + done, size - done);
        if (got == 0) break;
        if (got < 0) {
            rc = -1;
            break;
        }
        done += static_cast<size_t>(got);
    }
    if (close(fd) != 0) rc = -1;
    return rc;
#else
    FILE *f = std::fopen(savePath().c_str(), "rb");
    if (f == NULL) {
        return 0; // missing file -> zero-filled, treated as fresh SRAM
    }
    if (std::fseek(f, (long)offset, SEEK_SET) != 0) {
        std::fclose(f);
        return 0; // short file -> zero-filled
    }
    (void)std::fread(dst, 1, size, f);
    std::fclose(f);
    return 0;
#endif
}

extern "C" int port_save_write(uintptr_t offset, const void *src, size_t size)
{
    if (size == 0) return 0;
    if (src == NULL || !saveRangeValid(offset, size)) return -1;

    std::lock_guard<std::mutex> lock(gSaveMutex);

    const char *path = savePath().c_str();

#ifdef __vita__
    const int fd = open(path, O_WRONLY | O_CREAT, 0666);
    if (fd < 0) {
        port_log("SSB64 Save: open(%s) for raw write failed\n", path);
        return -1;
    }

    int rc = padFdToOffset(fd, offset);
    if (rc == 0) rc = writeRangeFd(fd, offset, src, size);
    if (close(fd) != 0) rc = -1;
    if (rc != 0) {
        port_log("SSB64 Save: raw write failed at offset 0x%x size=%u\n",
                 (unsigned int)offset, (unsigned int)size);
    }
    return rc;
#else
    FILE *f = openSaveForUpdate(path);
    if (f == NULL) {
        port_log("SSB64 Save: open(%s) for write failed\n", path);
        return -1;
    }

    if (padToOffset(f, offset) != 0 || writeRange(f, offset, src, size) != 0) {
        port_log("SSB64 Save: write failed at offset 0x%x size=%u\n",
                 (unsigned int)offset, (unsigned int)size);
        std::fclose(f);
        return -1;
    }
    int close_rc = 0;
    if (std::fflush(f) != 0) close_rc = -1;
    if (std::fclose(f) != 0) close_rc = -1;
    if (close_rc != 0) {
        port_log("SSB64 Save: flush/close failed at offset 0x%x\n", (unsigned int)offset);
        return -1;
    }
    return 0;
#endif
}

extern "C" int port_save_write_pair(uintptr_t offset_a, uintptr_t offset_b, const void *src, size_t size)
{
    if (size == 0) return 0;
    if (src == NULL || !saveRangeValid(offset_a, size) || !saveRangeValid(offset_b, size)) return -1;

    std::lock_guard<std::mutex> lock(gSaveMutex);
    const char *path = savePath().c_str();
#ifdef __vita__
    const int fd = open(path, O_WRONLY | O_CREAT, 0666);
    if (fd < 0) {
        port_log("SSB64 Save: open(%s) for raw paired write failed\n", path);
        return -1;
    }

    const uintptr_t max_offset = std::max(offset_a, offset_b);
    int rc = padFdToOffset(fd, max_offset);
    if (rc == 0) rc = writeRangeFd(fd, offset_a, src, size);
    if (rc == 0) rc = writeRangeFd(fd, offset_b, src, size);
    if (close(fd) != 0) rc = -1;

    if (rc != 0) {
        port_log("SSB64 Save: raw paired write failed offsets=0x%x/0x%x size=%u\n",
                 (unsigned int)offset_a, (unsigned int)offset_b, (unsigned int)size);
    }
    return rc;
#else
    FILE *f = openSaveForUpdate(path);
    if (f == NULL) {
        port_log("SSB64 Save: open(%s) for paired write failed\n", path);
        return -1;
    }

    const uintptr_t max_offset = std::max(offset_a, offset_b);
    int rc = padToOffset(f, max_offset);
    if (rc == 0) rc = writeRange(f, offset_a, src, size);
    if (rc == 0) rc = writeRange(f, offset_b, src, size);
    if (rc == 0 && std::fflush(f) != 0) rc = -1;
    if (std::fclose(f) != 0) rc = -1;

    if (rc != 0) {
        port_log("SSB64 Save: paired write failed offsets=0x%x/0x%x size=%u\n",
                 (unsigned int)offset_a, (unsigned int)offset_b, (unsigned int)size);
    }
    return rc;
#endif
}
