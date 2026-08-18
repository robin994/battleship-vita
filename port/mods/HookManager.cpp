#include "HookManager.h"
#include "SymbolResolver.h"
#include "../port_log.h"

#include <funchook.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/mman.h>
#include <cstdlib>
#endif

#include <algorithm>
#include <cstdint>
#include <cstring>

namespace ssb64::mods {

std::mutex HookManager::sMutex;
std::vector<HookManager::HookRecord> HookManager::sHooks;
std::map<void *, HookManager::ChainState> HookManager::sChains;
std::string HookManager::sCurrentOwner;
bool HookManager::sInitialized = false;

/* ------------------------------------------------------------------ */
/*  Portable double-indirect trampoline thunk                          */
/*                                                                     */
/*  A thunk is two allocations:                                        */
/*    - an executable stub that jumps to *cell                         */
/*    - a malloc'd `cell` (one void*) holding the live jump target     */
/*                                                                     */
/*  The stub is written once then flipped to read+exec and NEVER       */
/*  rewritten, so it stays W^X (works under macOS hardened runtime     */
/*  with no MAP_JIT dance). Re-pointing a thunk = writing the always-  */
/*  writable cell, which SetThunkTarget does. The stub→cell mapping    */
/*  is kept in sThunkCells so Free/SetTarget don't have to decode the  */
/*  stub. All callers hold sMutex.                                     */
/* ------------------------------------------------------------------ */

static std::map<void *, void **> sThunkCells; /* stub -> heap cell */

#if defined(__aarch64__) || defined(_M_ARM64)
/* 20-byte arm64 stub:
 *   LDR  x16, #12        ; x16 = literal at stub+12 (the cell address)
 *   LDR  x16, [x16]      ; x16 = *cell
 *   BR   x16
 *   <8-byte cell address literal>
 */
static const size_t kStubSize = 20;
static void EmitStub(unsigned char *p, void **cell) {
    uint32_t insns[3] = {
        0x58000070u, /* LDR  x16, #12  (imm19 = 12/4 = 3, Rt = x16) */
        0xF9400210u, /* LDR  x16, [x16]                              */
        0xD61F0200u, /* BR   x16                                     */
    };
    std::memcpy(p + 0, &insns[0], 4);
    std::memcpy(p + 4, &insns[1], 4);
    std::memcpy(p + 8, &insns[2], 4);
    std::memcpy(p + 12, &cell, sizeof(void *));
}
#else
/* 12-byte x86-64 stub:
 *   48 B8 <cell:8>      movabs rax, cell
 *   FF 20               jmp    [rax]        ; jmp *cell
 */
static const size_t kStubSize = 12;
static void EmitStub(unsigned char *p, void **cell) {
    p[0] = 0x48;
    p[1] = 0xB8;
    std::memcpy(p + 2, &cell, sizeof(void *));
    p[10] = 0xFF;
    p[11] = 0x20;
}
#endif

static void *AllocExec(size_t len) {
#if defined(_WIN32)
    return VirtualAlloc(nullptr, len, MEM_COMMIT | MEM_RESERVE,
                        PAGE_EXECUTE_READWRITE);
#else
    void *m = mmap(nullptr, len, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return (m == MAP_FAILED) ? nullptr : m;
#endif
}

static bool MakeExec(void *mem, size_t len) {
#if defined(_WIN32)
    (void)mem;
    (void)len; /* already PAGE_EXECUTE_READWRITE */
    return true;
#else
    if (mprotect(mem, len, PROT_READ | PROT_EXEC) != 0) {
        return false;
    }
    __builtin___clear_cache((char *)mem, (char *)mem + len);
    return true;
#endif
}

static void FreeExec(void *mem, size_t len) {
#if defined(_WIN32)
    (void)len;
    VirtualFree(mem, 0, MEM_RELEASE);
#else
    munmap(mem, len);
#endif
}

void *HookManager::AllocThunk(void *initial_target) {
    void **cell = (void **)std::malloc(sizeof(void *));
    if (cell == nullptr) {
        return nullptr;
    }
    *cell = initial_target;

    void *stub = AllocExec(kStubSize);
    if (stub == nullptr) {
        std::free(cell);
        return nullptr;
    }
    EmitStub((unsigned char *)stub, cell);
    if (!MakeExec(stub, kStubSize)) {
        FreeExec(stub, kStubSize);
        std::free(cell);
        return nullptr;
    }
    sThunkCells[stub] = cell;
    return stub;
}

void HookManager::SetThunkTarget(void *thunk, void *new_target) {
    if (thunk == nullptr) {
        return;
    }
    auto it = sThunkCells.find(thunk);
    if (it != sThunkCells.end()) {
        *(it->second) = new_target;
    }
}

void HookManager::FreeThunk(void *thunk) {
    if (thunk == nullptr) {
        return;
    }
    auto it = sThunkCells.find(thunk);
    if (it == sThunkCells.end()) {
        return;
    }
    std::free(it->second);
    FreeExec(thunk, kStubSize);
    sThunkCells.erase(it);
}

/* ------------------------------------------------------------------ */
/*  funchook backend helpers                                           */
/* ------------------------------------------------------------------ */

/* Detour `target` (real engine address) to `replacement`. On success
 * returns the funchook handle and writes the call-original trampoline
 * to *orig_out. On failure returns nullptr and logs. */
static funchook_t *BuildHandle(const char *sym, void *target,
                               void *replacement, void **orig_out) {
    funchook_t *fh = funchook_create();
    if (fh == nullptr) {
        port_log("[mods] InstallHook(%s): funchook_create failed\n", sym);
        return nullptr;
    }
    void *call_orig = target; /* in: addr to hook — out: trampoline */
    int rv = funchook_prepare(fh, &call_orig, replacement);
    if (rv != FUNCHOOK_ERROR_SUCCESS) {
        port_log("[mods] InstallHook(%s): funchook_prepare failed: %s\n", sym,
                 funchook_error_message(fh));
        funchook_destroy(fh);
        return nullptr;
    }
    rv = funchook_install(fh, 0);
    if (rv != FUNCHOOK_ERROR_SUCCESS) {
        port_log("[mods] InstallHook(%s): funchook_install failed: %s\n", sym,
                 funchook_error_message(fh));
        funchook_destroy(fh);
        return nullptr;
    }
    *orig_out = call_orig;
    return fh;
}

static void DestroyHandle(funchook_t *fh) {
    if (fh == nullptr) {
        return;
    }
    funchook_uninstall(fh, 0);
    funchook_destroy(fh);
}

bool HookManager::Init() {
    std::lock_guard<std::mutex> lock(sMutex);
    /* funchook needs no global init; arming is bookkeeping only. */
    sInitialized = true;
    return true;
}

void HookManager::Shutdown() {
    std::lock_guard<std::mutex> lock(sMutex);
    if (!sInitialized) {
        return;
    }
    for (auto &kv : sChains) {
        ChainState &st = kv.second;
        DestroyHandle((funchook_t *)st.fh);
        st.fh = nullptr;
        for (void *t : st.chain_trampolines) {
            FreeThunk(t);
        }
        /* inner_thunk is the first chain_trampoline only when chain.size
         * never grew; once it does, inner_thunk may have been reassigned
         * to a higher trampoline during mid-chain removals. Free it if
         * it wasn't already swept above. */
        if (st.inner_thunk != nullptr &&
            sThunkCells.find(st.inner_thunk) != sThunkCells.end()) {
            FreeThunk(st.inner_thunk);
        }
    }
    sChains.clear();
    sHooks.clear();
    sInitialized = false;
}

int HookManager::InstallHook(const char *symbol_name, void *replacement,
                             void **original_out) {
    if (original_out != nullptr) {
        *original_out = nullptr;
    }
    if (symbol_name == nullptr || replacement == nullptr) {
        port_log("[mods] InstallHook: bad args (sym=%p repl=%p)\n",
                 (const void *)symbol_name, replacement);
        return 1;
    }

    std::lock_guard<std::mutex> lock(sMutex);
    if (!sInitialized) {
        port_log("[mods] InstallHook(%s): not initialized\n", symbol_name);
        return 1;
    }

    void *target = SymbolResolver::Resolve(symbol_name);
    if (target == nullptr) {
        port_log("[mods] InstallHook(%s): symbol not found in debug info\n",
                 symbol_name);
        return 1;
    }

    auto chain_it = sChains.find(target);
    if (chain_it == sChains.end()) {
        /* First hook on this target. funchook detours the engine
         * address to `replacement`; its trampoline runs original code.
         * inner_thunk wraps that trampoline so subsequent installs can
         * re-point "the bottom of the chain" without the first mod
         * recompiling. */
        void *orig_call = nullptr;
        funchook_t *fh = BuildHandle(symbol_name, target, replacement,
                                     &orig_call);
        if (fh == nullptr) {
            return 1;
        }
        void *inner_thunk = AllocThunk(orig_call);
        if (inner_thunk == nullptr) {
            port_log("[mods] InstallHook(%s): inner thunk alloc failed\n",
                     symbol_name);
            DestroyHandle(fh);
            return 1;
        }
        ChainState st;
        st.fh = fh;
        st.orig_call = orig_call;
        st.inner_thunk = inner_thunk;
        st.chain.push_back(replacement);
        st.chain_owners.push_back(sCurrentOwner);
        st.chain_trampolines.push_back(inner_thunk);
        sChains[target] = st;

        sHooks.push_back({std::string(symbol_name), target, sCurrentOwner});
        if (original_out != nullptr) {
            *original_out = inner_thunk;
        }
        port_log("[mods] hooked %s @ %p (trampoline=%p, chain=1, owner=%s)\n",
                 symbol_name, target, inner_thunk,
                 sCurrentOwner.empty() ? "(unowned)" : sCurrentOwner.c_str());
        return 0;
    }

    /* Subsequent hook on an already-detoured target. funchook has no
     * incremental API, so destroy the handle and rebuild it with the
     * new mod's function as the outermost detour. The new mod's
     * trampoline jumps to the previous outermost replacement (which
     * has its own trampoline walking inward). inner_thunk is re-pointed
     * at the fresh funchook trampoline so the chain bottom still
     * reaches original engine code. */
    ChainState &st = chain_it->second;
    void *prev_outer = st.chain.back();

    /* Allocate the new chain trampoline BEFORE any destructive teardown.
     * If it fails we return with the existing handle and chain fully
     * intact (target still correctly detoured to prev_outer). Doing this
     * after DestroyHandle/BuildHandle (the old order) left the target
     * detoured to `replacement` with *original_out never set — an
     * untracked live detour with a null "original" the chain didn't even
     * record, so the mod would crash on call-original and the previous
     * chain was silently bypassed. */
    void *new_trampoline = AllocThunk(prev_outer);
    if (new_trampoline == nullptr) {
        port_log("[mods] InstallHook(%s): chain thunk alloc failed (chain left intact)\n",
                 symbol_name);
        return 1;
    }

    DestroyHandle((funchook_t *)st.fh);
    st.fh = nullptr;

    void *new_orig_call = nullptr;
    funchook_t *fh = BuildHandle(symbol_name, chain_it->first, replacement,
                                 &new_orig_call);
    if (fh == nullptr) {
        /* funchook couldn't re-detour the target. It is now un-hooked
         * entirely; drop the chain (and the trampoline just allocated)
         * so the engine runs clean rather than leaving dangling thunks. */
        FreeThunk(new_trampoline);
        for (void *t : st.chain_trampolines) {
            FreeThunk(t);
        }
        sChains.erase(chain_it);
        return 1;
    }
    st.fh = fh;
    st.orig_call = new_orig_call;
    SetThunkTarget(st.inner_thunk, new_orig_call);

    st.chain.push_back(replacement);
    st.chain_owners.push_back(sCurrentOwner);
    st.chain_trampolines.push_back(new_trampoline);
    sHooks.push_back({std::string(symbol_name), target, sCurrentOwner});
    if (original_out != nullptr) {
        *original_out = new_trampoline;
    }
    port_log("[mods] chained %s @ %p (trampoline=%p, chain=%u, owner=%s)\n",
             symbol_name, target, new_trampoline, (unsigned int)st.chain.size(),
             sCurrentOwner.empty() ? "(unowned)" : sCurrentOwner.c_str());
    return 0;
}

void HookManager::SetCurrentOwner(const char *owner_id) {
    std::lock_guard<std::mutex> lock(sMutex);
    sCurrentOwner = (owner_id != nullptr) ? owner_id : "";
}

void HookManager::ClearCurrentOwner() {
    std::lock_guard<std::mutex> lock(sMutex);
    sCurrentOwner.clear();
}

int HookManager::UninstallHooksForOwner(const char *owner_id) {
    if (owner_id == nullptr) {
        return 0;
    }
    std::lock_guard<std::mutex> lock(sMutex);
    if (!sInitialized) {
        return 0;
    }
    std::string oid(owner_id);
    int removed = 0;

    /* Walk every chain top-down, removing all entries owned by oid.
     * Three cases per entry:
     *   - is top + chain.size==1: destroy the funchook handle, free
     *     inner_thunk, the chain disappears.
     *   - is top + chain.size>1:  rebuild the handle detouring to
     *     chain[i-1] (the new outermost).
     *   - is mid-chain:           re-point chain[i+1]'s trampoline
     *     thunk to skip past us. If we're the bottom (i==0), the
     *     entry above us inherits inner_thunk's role. */
    for (auto chain_it = sChains.begin(); chain_it != sChains.end();) {
        ChainState &st = chain_it->second;

        bool any_removed = true;
        while (any_removed) {
            any_removed = false;
            for (int i = (int)st.chain.size() - 1; i >= 0; --i) {
                if (st.chain_owners[i] != oid) {
                    continue;
                }
                void *removed_tramp = st.chain_trampolines[i];
                bool is_top = (i == (int)st.chain.size() - 1);
                bool is_bottom = (i == 0);

                if (is_top && st.chain.size() == 1) {
                    DestroyHandle((funchook_t *)st.fh);
                    st.fh = nullptr;
                    if (st.inner_thunk != nullptr) {
                        FreeThunk(st.inner_thunk);
                        st.inner_thunk = nullptr;
                    }
                } else if (is_top) {
                    void *new_top = st.chain[i - 1];
                    DestroyHandle((funchook_t *)st.fh);
                    st.fh = nullptr;
                    void *new_orig_call = nullptr;
                    funchook_t *fh = BuildHandle("(uninstall)",
                                                 chain_it->first, new_top,
                                                 &new_orig_call);
                    if (fh != nullptr) {
                        st.fh = fh;
                        st.orig_call = new_orig_call;
                        SetThunkTarget(st.inner_thunk, new_orig_call);
                    } else {
                        port_log("[mods] uninstall %p (owner=%s): rebuild "
                                 "failed — chain broken\n",
                                 chain_it->first, oid.c_str());
                    }
                    if (removed_tramp != st.inner_thunk) {
                        FreeThunk(removed_tramp);
                    }
                } else {
                    /* Mid-chain: re-point chain[i+1]'s trampoline thunk
                     * to skip our entry. If we're the bottom, the
                     * upstairs thunk now points at orig_call (the
                     * funchook trampoline → original engine code) and
                     * itself becomes the new inner_thunk. */
                    void *new_target =
                        is_bottom ? st.orig_call : st.chain[i - 1];
                    SetThunkTarget(st.chain_trampolines[i + 1], new_target);

                    if (is_bottom) {
                        if (st.inner_thunk != nullptr) {
                            FreeThunk(st.inner_thunk);
                        }
                        st.inner_thunk = st.chain_trampolines[i + 1];
                    } else {
                        if (removed_tramp != st.inner_thunk) {
                            FreeThunk(removed_tramp);
                        }
                    }
                }

                st.chain.erase(st.chain.begin() + i);
                st.chain_owners.erase(st.chain_owners.begin() + i);
                st.chain_trampolines.erase(st.chain_trampolines.begin() + i);

                port_log(
                    "[mods] uninstalled %p[%d] (owner=%s, %s, chain=%u)\n",
                    chain_it->first, i, oid.c_str(),
                    is_top ? (st.chain.empty() ? "chain emptied" : "was top")
                           : (is_bottom ? "was bottom (mid-chain skip)"
                                        : "mid-chain"),
                    (unsigned int)st.chain.size());
                removed++;
                any_removed = true;
                break; /* restart after vector mutation */
            }
        }

        if (st.chain.empty()) {
            chain_it = sChains.erase(chain_it);
        } else {
            ++chain_it;
        }
    }

    sHooks.erase(std::remove_if(sHooks.begin(), sHooks.end(),
                                [&oid](const HookRecord &h) {
                                    return h.owner_id == oid;
                                }),
                 sHooks.end());

    return removed;
}

} // namespace ssb64::mods
