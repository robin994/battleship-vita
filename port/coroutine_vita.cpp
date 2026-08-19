/**
 * coroutine_vita.cpp — Vita coroutine backend built on the official SceFiber
 * API, replacing the earlier hand-rolled memalign()+raw-ARM-assembly-SP-swap
 * backend (coroutine_android.cpp's __vita__ branch + coroutine_armv7.S).
 *
 * Why: every kernel syscall made from code running on the old backend's
 * manually-swapped stack crashed on real hardware — confirmed for
 * sceKernelDelayThread, raw write(), and multiple newlib internals that
 * lazily create a kernel lock on first use (malloc's mmap_chunk() path for
 * large allocations, vsnprintf, mutex/promise-future). That stack was never
 * registered with the kernel as a real thread stack, so kernel calls made
 * from it fault. SceFiber is Sony's own supported primitive for exactly this
 * (cooperative execution contexts layered on a real thread) — RetroArch's
 * Vita port uses it for the same purpose (libretro-common's libco/scefiber.c)
 * and performs I/O from within it successfully, which is strong evidence
 * this class of bug doesn't apply here.
 *
 * SceFiber's API isn't a single symmetric swap like the old backend's — it's
 * three different calls depending on direction:
 *   sceFiberRun()          - start/resume a fiber, called from the plain
 *                            thread context (not from within another fiber).
 *   sceFiberSwitch()       - switch to a fiber, called from within another
 *                            fiber (fiber-to-fiber, no thread involved).
 *   sceFiberReturnToThread() - called from within a fiber to give control
 *                            back to the thread context that started this
 *                            chain via sceFiberRun().
 * Each PortCoroutine remembers which of "the real thread" or "another fiber"
 * last resumed it (callerCoroutine, nullptr = the real thread) so yield can
 * pick the matching call. This mirrors coroutine_android.cpp's caller_ctx
 * tracking — same problem (nested resumes must unwind to the right caller),
 * different underlying primitive.
 */

#if defined(__vita__)

#include "coroutine.h"
#include "port_log.h"
#include "port_watchdog.h"

#include <psp2/fiber.h>
#include <psp2/sysmodule.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <malloc.h>
#include <unistd.h>

/* SceFiber's context buffer plays the same role the old backend's raw stack
 * did (it's where the fiber's own execution state lives), so it keeps the
 * same floor and the same page-aligned memalign() allocation that backend
 * used — no guard page available on Vita either way (no sys/mman.h). */
#define MIN_CONTEXT_SIZE 32768

extern "C" {

struct PortCoroutine {
    SceFiber fiber;
    void   *context_mem;      /* memalign'd fiber context/stack buffer */
    size_t  context_size;
    void  (*entry)(void *);   /* user entry */
    void   *arg;               /* arg passed to entry */
    int     finished;          /* 1 once entry returns */
    PortCoroutine *caller;      /* who resumed us this cycle; nullptr = real thread */
};

static thread_local PortCoroutine *sCurrentCoroutine = nullptr;

static void port_coroutine_trampoline_scefiber(SceUInt32 argOnInitialize, SceUInt32 argOnRun) {
    (void)argOnRun;
    PortCoroutine *co = (PortCoroutine *)(uintptr_t)argOnInitialize;
    co->entry(co->arg);
    co->finished = 1;

    /* Permanent yield: give control back to whoever resumed us for the last
     * time. They will see the finished flag and never resume us again. */
    if (co->caller) {
        sceFiberSwitch(&co->caller->fiber, 0, nullptr);
    } else {
        sceFiberReturnToThread(0, nullptr);
    }

    /* If we got back here, the caller resumed a finished coroutine — caller
     * error (port_coroutine_resume guards against it normally). */
    fprintf(stderr, "SSB64: coroutine resumed after finish\n");
    abort();
}

/* ========================================================================= */
/*  Public API                                                               */
/* ========================================================================= */

void port_coroutine_init_main(void) {
    /* SceFiber isn't in the resident default module set — load it once,
     * before the first fiber is created. Safe to call more than once. */
    int ret = sceSysmoduleLoadModule(SCE_SYSMODULE_FIBER);
    if (ret < 0) {
        port_log("SSB64: sceSysmoduleLoadModule(SCE_SYSMODULE_FIBER) failed: 0x%08x\n", (unsigned)ret);
    }
}

PortCoroutine *port_coroutine_create(void (*entry)(void *), void *arg, size_t stack_size) {
    if (stack_size < MIN_CONTEXT_SIZE) {
        stack_size = MIN_CONTEXT_SIZE;
    }

    PortCoroutine *co = (PortCoroutine *)calloc(1, sizeof(PortCoroutine));
    if (!co) {
        return nullptr;
    }

    long ps_v = sysconf(_SC_PAGESIZE);
    size_t ps = (ps_v > 0) ? (size_t)ps_v : 4096;
    stack_size = (stack_size + ps - 1) & ~(ps - 1);

    void *ctxMem = memalign(ps, stack_size);
    if (!ctxMem) {
        free(co);
        return nullptr;
    }

    co->context_mem  = ctxMem;
    co->context_size = stack_size;
    co->entry        = entry;
    co->arg          = arg;
    co->finished     = 0;
    co->caller       = nullptr;

    int ret = _sceFiberInitializeImpl(&co->fiber, (char *)"PortCoroutine",
                                       port_coroutine_trampoline_scefiber,
                                       (SceUInt32)(uintptr_t)co,
                                       co->context_mem, (SceSize)co->context_size,
                                       nullptr);
    if (ret < 0) {
        port_log("SSB64: _sceFiberInitializeImpl failed: 0x%08x\n", (unsigned)ret);
        free(co->context_mem);
        free(co);
        return nullptr;
    }

    return co;
}

void port_coroutine_destroy(PortCoroutine *co) {
    if (!co) return;
    if (co == sCurrentCoroutine) {
        fprintf(stderr, "SSB64: port_coroutine_destroy on current coroutine\n");
        abort();
    }
    sceFiberFinalize(&co->fiber);
    free(co->context_mem);
    free(co);
}

void port_coroutine_resume(PortCoroutine *co) {
    if (!co || co->finished) return;

    /* Save the previous current so nested resumes restore correctly, and
     * remember it on the coroutine itself so its yield (or finish) knows
     * which SceFiber call gets control back to us. Same semantics as the
     * old backend's caller_ctx. Example: main resumes Thread5 (caller =
     * nullptr, uses sceFiberRun), Thread5 resumes a GObj coroutine (caller =
     * Thread5, uses sceFiberSwitch). When the GObj yields, control returns
     * to Thread5 via sceFiberSwitch; when Thread5 later yields, control
     * returns to main via sceFiberReturnToThread. */
    PortCoroutine *prev = sCurrentCoroutine;
    co->caller = prev;
    sCurrentCoroutine = co;

    if (prev == nullptr) {
        sceFiberRun(&co->fiber, 0, nullptr);
    } else {
        sceFiberSwitch(&co->fiber, 0, nullptr);
    }

    sCurrentCoroutine = prev;
}

void port_coroutine_yield(void) {
    PortCoroutine *co = sCurrentCoroutine;
    if (!co) {
        fprintf(stderr, "SSB64: port_coroutine_yield called outside coroutine\n");
        return;
    }
    port_watchdog_note_yield();

    PortCoroutine *caller = co->caller;
    if (caller) {
        sceFiberSwitch(&caller->fiber, 0, nullptr);
    } else {
        sceFiberReturnToThread(0, nullptr);
    }
    /* Returns here when resumed again; port_coroutine_resume() restores
     * sCurrentCoroutine on the resumer's side, not here. */
}

int port_coroutine_is_finished(PortCoroutine *co) {
    return co ? co->finished : 1;
}

int port_coroutine_in_coroutine(void) {
    return sCurrentCoroutine != nullptr;
}

} /* extern "C" */

#endif /* __vita__ */
