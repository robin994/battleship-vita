# Vita VBO scratch starvation and black screen

**Status:** Resolved on hardware. VBO churn, seek-free O2R streaming, the ABI
rebuild fix, the 41-program early shader pre-warm, and direct FB0 presentation
(bypassing `SDL_GL_SwapWindow` and the fixed 960x544 window/drawable size) are
all confirmed on hardware — video is now visible. The desktop transition-
capture workaround (off-screen `mGameFb` FBO composited via `ImGui::Image`)
remains disabled on Vita; re-enabling stage-transition snapshots needs a
Vita-native FB0 capture path, not yet implemented.

## Symptoms

The game booted far enough to play audio and process graphics tasks, but the
display remained black. Gameplay shader programs repeatedly failed to link and
Fast3D then skipped their draws because the resulting OpenGL program was zero.
Earlier builds could instead data-abort in `glLinkProgram()` or
`glProgramBinary()` after one of those failures.

The SceShaccCg messages (`fatal internal error line -1`) initially made valid
shader source look like the cause. The failures were actually correlated with
severe vitaGL memory pressure.

## Root cause

The Vita path treated `Interpreter::mBufVbo` as persistent GPU storage and
reserved a fixed 10 MiB block from vitaGL's circular scratch allocator every
frame. `Flush()` then advanced the pointer and `EndFrame()` replaced it with a
new 10 MiB allocation, orphaning the previous base pointer.

vitaGL's scratch pool is shared by the in-flight display buffers. With double
buffering, the fixed reservation consumed most of each frame's available
scratch space even though a populated Fast3D batch is at most about 96 KiB.
Garbage collection and allocation retries left too little usable memory for
SceShaccCg to compile and register runtime shaders.

`vglBufferData()` is copy-less on this vitaGL build, so passing the reusable CPU
staging buffer directly is not safe either: GXM can still be reading it after
Fast3D starts filling the next batch.

## Fix

- `Interpreter` now owns the same bounded CPU staging array as the desktop
  backend (`MAX_TRI_BUFFER * 32 * 3` floats) and frees it normally.
- Every Vita draw allocates only its populated byte count from
  `vglAllocFromScratch()`, copies that batch, and submits the scratch pointer to
  `vglBufferData()`. This preserves the GPU in-flight lifetime without a fixed
  10 MiB reservation.
- Rate-limited per-frame diagnostics report submitted bytes, draw count, peak
  scratch use, and dropped allocations.
- The local `vitaGL-nosplash` dependency now checks the compiled-program
  allocation and `sceGxmShaderPatcherRegisterProgram()` result before using the
  program. A failure is returned to `glLinkProgram()` instead of dereferencing
  or registering invalid memory.

The vitaGL change lives in the machine-local dependency at
`~/.local/opt/vitadb-deps/vitaGL-nosplash/source/custom_shaders.c`; that
directory is not a repository and must be patched again if the dependency is
replaced.

## Verification

The required Vita sequence completed successfully:

```text
make -f Makefile.vita objects -j4
make -f Makefile.vita prepare
make -f Makefile.vita build/battleship.vpk
```

The final unstripped ELF contains the VBO counters and the new vitaGL failure
messages. It does not contain the expensive full GLSL translation dump.

On hardware, a healthy run should show lines like:

```text
SSB64: Vita VBO frame=... bytes=... draws=... peak=... dropped_total=0
```

The acceptance gate is visible video with audio, no repeated `shader link
failed`, no SceShaccCg fatal internal error, and no VBO allocation drops. If
registration still fails, the new log identifies whether the compiled GXM
program allocation or shader-patcher registration exhausted memory.

## Real-hardware follow-up

The first build confirmed that the fixed 10 MiB VBO allocations disappeared
and audio stutter dropped substantially, but the display remained black. The
new logs separated two remaining issues:

- The manual shader pre-warm ran before `PortInit()`, so vitaGL had not created
  `gxm_shader_patcher` yet. Both compiled programs were valid, but registration
  returned `SCE_GXM_ERROR_INVALID_POINTER`. The pre-warm was removed because
  `InitWindow()` already compiles ImGui's shaders at the correct time.
- When gameplay shader creation began, newlib had only roughly 30 KiB free.
  vitaGL still reserved its default 32 MiB circular pool even though exact-size
  VBO submission no longer needs it. The pool is now 8 MiB, the GXM parameter
  buffer is reduced from 6 to 4 MiB (default is 16 MiB), and double buffering
  remains enabled. This makes roughly 26 MiB more internal-pool memory
  available as a fallback to the runtime compiler.

The next hardware log must contain
`vitaGL config param=4MiB circular=8MiB display_buffers=2` before judging this
iteration. A stale VPK will not contain that marker.

That build confirmed the pool changes but still failed every gameplay shader.
At the first failure, newlib had only about 34 KiB free while vitaGL still had
about 30.9 MiB in RAM, 27.3 MiB in SLOW, and 9.2 MiB in BUDGET. vitaShaRK was
configured with `vglMalloc`, which prioritizes the exhausted external/newlib
heap and therefore never reached those internal pools.

The local vitaGL dependency now gives vitaShaRK a dedicated allocator wrapper.
During `shark_init()` it still uses the external heap because `vgl_mem_init()`
has not run yet. Once the vitaGL heaps exist, runtime shader allocations prefer
RAM, SLOW, BUDGET, and VRAM in that order, using the external heap only as the
last fallback. A hardware run of this iteration must contain:

```text
vitaShaRK runtime allocations now prefer vitaGL internal pools.
```

The next hardware run showed that the allocator marker was present, but only
about 1 KiB per shader attempt moved through the vitaShaRK callback. The heavy
allocations inside SceShaccCg still depend on the general process heap, so all
gameplay programs remained zero.

The structural fix is now implemented in `O2rArchive`: a custom libzip source
keeps one raw descriptor open, represents ZIP seeks as in-memory offset
updates, and reads with `sceIoPread()`. This preserves the earlier workaround
for the real-hardware `sceIoLseek32` crash while removing the permanent 12 MiB
`mArchiveBuffer` allocation. The source owns and closes its descriptor through
libzip's `ZIP_SOURCE_FREE` callback.

The next hardware log must contain:

```text
SSB64: Vita O2R source=pread path=... size=...
```

It must not contain `Vita O2R pread failed` or `unexpected EOF`. At the first
gameplay shader attempt, the `newlib ... free=` field should be higher by
approximately the archive size. The final acceptance criteria remain nonzero
gameplay `VGLDIAG` program pointers and nonzero VBO bytes/draws with
`dropped_total=0`.

## Incremental-build ABI crash

The first VPK built after removing `mArchiveBuffer` crashed before the new
libzip source was reached. The coredump stopped in
`ArchiveManager::AddArchive(const std::string&)`; `DFAR=0x30707065` and
`R0=0x30707061` were path bytes from `app0:` interpreted as pointers.

This was not a failure in `sceIoPread()`. `ArchiveManager.o` was stale because
the Vita makefile tracked only source timestamps, while `O2rArchive.o` had the
new, smaller class layout. Since `Archive` is a virtual base, the stale code
looked for `enable_shared_from_this` at the old offset and crashed while
constructing the archive, before `O2rArchive::Open()`.

The unused `mArchiveBuffer` members are restored as empty ABI-compatibility
fields; they no longer own archive data. `Makefile.vita` now builds and includes
`-MMD -MP` dependency files, and a full forced rebuild synchronized every
translation unit. Generated `.d` files are ignored by git but retained locally
for future incremental builds.

The next device boot must proceed from
`calling InitResourceManager (bootstrap)` to:

```text
SSB64: Vita O2R source=pread path=app0:/f3d.o2r size=...
```

A later marker must report the main `BattleShip.o2r` archive. There must be no
data abort in `ArchiveManager::AddArchive()` and no `app0` path bytes in a fault
address. Only after these gates pass can the shader-memory result be evaluated.

## Early shader timing proof

The first post-`InitWindow()` Fast3D self-test compiled both stages and linked
successfully before `BattleShip.o2r` and the game resources were loaded:

```text
SSB64: EARLY_SHADER_TEST result=SUCCESS ...
```

The same run later failed every newly encountered program once newlib usage had
grown from roughly 68 MiB to 107--115 MiB. The successful early program was
reused by the renderer and changed the VBO diagnostic from zero activity to
`bytes=840 draws=3`, proving that a valid cached Fast3D program reaches the draw
path. The screen remained black because one program was insufficient for the
scene and all other programs were still compiled too late.

The Vita startup now precompiles the 41 unique shader pairs observed across the
full hardware boot and attract-mode captures immediately after `InitWindow()`.
Successful programs remain in `GfxRenderingAPIOGL`'s in-memory map; the unsafe
on-disk `glProgramBinary()` cache stays disabled. The next hardware log must
contain:

```text
SSB64: EARLY_SHADER_PREWARM begin count=41 ...
SSB64: EARLY_SHADER_PREWARM complete success=41 failed=0 ...
```

Any later `shader link failed` line identifies a pair not yet present in the
pre-warm set and should be added before evaluating rendering completeness.

## Direct-display experiment after successful pre-warm

The next hardware run completed the entire pre-warm set (`41/41`), emitted no
later shader-link failures, and submitted real Fast3D work (`50232` VBO bytes,
`37` draws at frame 240, with zero dropped draws). The persistent black screen
is therefore downstream of shader compilation and geometry submission.

The desktop transition-capture workaround was still forcing every Vita frame
through `mGameFb`, followed by an `ImGui::Image` copy of that framebuffer's
texture into display framebuffer 0. The Vita build now bypasses that unproven
off-screen sampling path and draws directly into vitaGL framebuffer 0. It also
pins MSAA to 1, disables post-processing, and consistently uses the physical
Vita height of 544 pixels rather than the previous SDL-only 545 value.

The next hardware log must contain:

```text
SSB64: Vita present experiment v3 path=direct-fb0 size=960x544 sdl_dimensions=forced swap=vglSwapBuffers msaa=1 postprocess=0
SSB64: Vita present frame=... path=direct-fb0 window=960x544 ...
```

If video becomes visible, the failure was the off-screen FBO-to-ImGui texture
composition. Stage-transition snapshot effects remain disabled on Vita until
FB0 capture is implemented without reintroducing that per-frame path. If the
screen remains black while the log says `direct-fb0`, the next target is the
vitaGL/SDL swap path itself, not Fast3D shader generation.

The first direct-FB0 hardware run exposed a more specific failure before swap:

```text
SSB64: Vita present frame=120 path=direct-fb0 window=0x0 render=32x32
viewport=(60,60 32x32) ...
```

VitaSDK SDL2 returned zero from its window/drawable-size queries. ImGui then
never created the full-screen dockspace and left `Main Game` at its 32x32
fallback size; Fast3D correctly followed that invalid viewport. Both the SDL
window backend and ImGui SDL backend now use the fixed vitaGL display size of
960x544 on Vita. The corrected run must report `window=960x544` and a game
viewport close to the full display instead of 32x32.

The corrected hardware run did report the full `960x544` window, render size,
and viewport, but the display remained black. The build links VitaSDK's stock
`libSDL2.a`, whose Vita driver presents its own sceGxm `SDL_Renderer` surface;
it is not the Northfear SDL vitaGL backend. This port initializes vitaGL
directly, so `SDL_GL_SwapWindow()` was not presenting the surface Fast3D drew
into. Vita now ends each frame with `vglSwapBuffers(GL_FALSE)` instead. A small
nine-point FB0 sample is logged before swap at frames 3, 120, 240, and so on.
The next run must contain both:

```text
SSB64: Vita swap frame=... backend=vglSwapBuffers common_dialog=0
SSB64: Vita FB0 sample frame=... draw_fbo=0 ... nonblack=.../9 ... glerr_after=0000
```

`nonblack > 0` proves that FB0 already contains image data before presentation;
`glerr_after=0000` proves the diagnostic readback itself was accepted.
