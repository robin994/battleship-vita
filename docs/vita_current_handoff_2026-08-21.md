# PS Vita current development handoff (2026-08-21)

This is the canonical handoff for continuing the real-hardware PS Vita port.
Read `CLAUDE.md` first, then this file. Where an older Vita note conflicts with
this file, this file describes the current branch and wins. Historical bug
documents remain useful as evidence, but some of their intermediate shader-cache
experiments have since been superseded.

## Bootstrap prompt for the next Claude session

Copy this into a new session:

```text
Work in /Users/robin994/Documents/Code/NeoVitaDB/battleship-vita, the canonical
robin994 PS Vita fork. Read CLAUDE.md and
docs/vita_current_handoff_2026-08-21.md completely before changing anything.
Preserve the confirmed 60 FPS texture-fixup optimization at commit b25239a.
There is an uncommitted port/port.cpp change that expands the early Vita shader
prewarm from 59 to 62 programs; do not discard it. Its build succeeded but it
still needs real-hardware validation for the Mario character-select cursor and
Pikachu rendering defect. Analyze the newest ssb64.log and vitaGL.log before
editing. Do not enable vitaGL's own HAVE_SHADER_CACHE: the current design uses
Fast3D's validated linked-program cache under
ux0:data/battleship/shader_cache. Build with Makefile.vita and always run the
objects -> prepare -> all/package sequence so build/f does not contain stale
objects. Treat Vita3K as diagnostic evidence only; completion requires a real
Vita test. Keep logging rate-limited because stdout/system logging destroys
performance on the devkit.
```

## Repository and branch snapshot

- Repository: `robin994/battleship-vita`
- Local path: `/Users/robin994/Documents/Code/NeoVitaDB/battleship-vita`
- Outer branch: `main`
- Outer HEAD: `b25239a9c425366946cd4bedf5acb568f790a5e3`
- `origin/main` was at the same commit when this handoff was written.
- `decomp`: `52ae67403bc57b64ef390151b862ffb57fc72980`, branch
  `vita-compat-fixes`
- `libultraship`: `f7470bc6e8cc4a9797a57c8912248611cf9bd6ac`, branch
  `vita-rinne-merge`
- `torch`: `2bdb7847da252f2ddd69fe4a7cf5dac6837db874`, branch
  `vita-compat-fixes`

At handoff time the only uncommitted functional source change is
`port/port.cpp`: three observed shader pairs were appended to
`kVitaEarlyShaders`. The documentation files added/updated by this handoff will
also appear dirty until committed. Never reset or discard these changes merely
to obtain a clean tree.

## Confirmed current baseline

The following is confirmed on a real Vita, not inferred from Vita3K:

- Video and audio render.
- Menus, loading screens, fighter models, textures, and a complete match have
  worked on hardware in the recent baseline.
- Direct rendering to vitaGL framebuffer 0 and `vglSwapBuffers(GL_FALSE)` are
  required. `SDL_GL_SwapWindow()` does not present vitaGL's framebuffer with
  the linked VitaSDK SDL build.
- The Vita window/drawable size must remain forced to `960x544`.
- Vita uses no MSAA and no post-processing in this path.
- The VBO path allocates and copies only the populated draw size from vitaGL's
  scratch allocator. Reintroducing the old fixed 10 MiB per-frame scratch
  reservation starves the shader compiler and causes GC/allocation churn.
- The current memory knobs are a 4 MiB GXM parameter buffer, an 8 MiB circular
  pool, and double buffering. Change one memory variable at a time and validate
  on hardware.
- Commit `b25239a` removed the CPU bottleneck in
  `portRelocFixupTextureAtRuntime`; the user then measured the target 60 FPS in
  the tested gameplay path.

The 60 FPS result is strong evidence for the optimization, but it is not yet a
full-game regression pass. The intro and every character/stage combination have
not all been reprofiled after the latest changes.

## Why `portRelocFixupTextureAtRuntime` was slow

The supplied PS Vita Razor capture showed the renderer spending almost the
entire selected interval below `portRelocFixupTextureAtRuntime`:

- `portRelocFixupTextureAtRuntime`: about 1.13 s inclusive in the selected
  capture range.
- `std::_Hashtable::find(unsigned int)`: about 1.09 s.
- `__aeabi_uidivmod` / unsigned division: about 0.79 s.
- The old implementation performed millions of `unordered_set<uintptr_t>`
  lookups to decide, one 32-bit word at a time, whether texture bytes had
  already been swapped.

Commit `b25239a` replaced that representation with:

- `sTexFixupRanges`: a merged `std::map<uintptr_t, uintptr_t>` of half-open
  byte ranges `[begin, end)`.
- One containment lookup for the normal repeated-load case.
- Gap-only byte swapping for partially overlapping texture/TLUT requests.
- Interval splitting/eviction when resource memory is reused.
- An ordered `std::set` for `sChainSlotAddrs`, so a texture range query visits
  only actual chain slots rather than testing every word.

These correctness properties are load-bearing. Do not replace the ranges with
the old per-word hash set to fix a visual problem. Preserve:

1. Half-open interval semantics.
2. Swap-only-uncovered-gaps behavior for overlapping TLUT loads.
3. Range splitting on partial eviction.
4. Protection of decoded struct ranges.
5. Clamping at live relocation-chain slots.
6. Removal of stale chain slots when their token no longer resolves.

The latest pre-fix log shows long stretches of 100% texture-cache hits in the
character-select scene. That evidence does not support blaming the Mario cursor
or Pikachu defect on the texture cache before the shader failure is retested.

## Current unresolved rendering defect

### User-visible symptom

With the build containing commit `b25239a`, gameplay reaches approximately
60 FPS, but in the VS character-select screen:

- selecting Mario can make the cursor fail to render;
- Pikachu can render incorrectly.

### Evidence in the latest logs

The latest logs in `build/` were captured from the 59-program build, before the
current three-pair patch:

- `build/ssb64.log`
- `build/vitaGL.log`

The early prewarm itself was healthy:

```text
EARLY_SHADER_PREWARM begin count=59
EARLY_SHADER_PREWARM complete success=59 failed=0 elapsed_ms=777
```

Scene 16 is `nSCKindPlayersVS`, the VS character-select screen. In that scene,
the log records both shader stages failing and Fast3D dropping the unlinked
program:

```text
VGLDIAG compile_shader FAILED type=35633
VGLDIAG compile_shader FAILED type=35632
shader link failed, shader_id0=020D020D01080108 shader_id1=FFFFFFFFFFFF0112
```

At this point newlib had grown to roughly 114.4 MiB with only 2.88 MiB free.
vitaGL/Shark reports a generic internal compiler error, but the same shader
family compiles during the early heap window. The evidence therefore supports
late heap pressure/fragmentation, not invalid generated GLSL.

Two more previously unseen programs failed later in the match:

```text
shader_id0=0000000001082821 shader_id1=FFFFFFFFFFFF0001
shader_id0=0000000080002821 shader_id1=FFFFFFFFFFFF0021
```

The texture-cache reports surrounding character select are healthy: repeated
windows have approximately 34,900 lookups, all hits, zero stale entries, and no
evictions. Therefore the next experiment is shader coverage, not a texture
cache rewrite.

### Current patch awaiting hardware validation

`port/port.cpp` appends these exact pairs to `kVitaEarlyShaders`:

```cpp
{ 0x020D020D01080108ULL, 0xFFFFFFFFFFFF0112ULL },
{ 0x0000000001082821ULL, 0xFFFFFFFFFFFF0001ULL },
{ 0x0000000080002821ULL, 0xFFFFFFFFFFFF0021ULL },
```

The prewarm count is now 62. This patch has compiled, linked, and packaged
successfully, but it has not yet been tested on real hardware. Do not describe
the Mario/Pikachu issue as fixed until the device proves it.

Expected log gates for the next test:

```text
EARLY_SHADER_PREWARM begin count=62
EARLY_SHADER_PREWARM complete success=62 failed=0
```

After that there should be no `shader link failed` for any of the three pairs.
The visual gates are: Mario's selection cursor remains visible, Pikachu is
complete/correct, and gameplay still holds the prior performance level.

## Shader-cache architecture: current truth

The current design uses Fast3D's validated linked-program binary cache from
libultraship commit `f7470bc6`. It does **not** use vitaGL's per-stage
`HAVE_SHADER_CACHE` implementation.

- Device directory: `ux0:data/battleship/shader_cache`
- File key: shader IDs plus cache ABI marker.
- Header: fixed-width, versioned, and includes both shader IDs, generated
  vertex/fragment source hashes, payload hash, binary format, binary size, and
  `numFloats`.
- Reader rejects truncated, stale, oversized, hash-mismatched, or unlinked
  entries and regenerates only the invalid exact key.
- Writer uses a temporary file and rename so partial cache files are not treated
  as complete programs.
- `glProgramBinary()` is used only after validation, followed by an explicit
  `GL_LINK_STATUS` check.

Do not expect files under `ux0:data/shader_cache/SSB64VITA/v0`; that was the old
vitaGL cache experiment. Do not enable `HAVE_SHADER_CACHE=1` in vitaGL while the
Fast3D cache is active.

Deleting the Fast3D cache is not a routine troubleshooting step. A cache clear
forces expensive recompilation and can obscure whether the warm startup path
works. Clear only the exact rejected entry, or clear everything only for an
explicit cold-cache validation.

Some comments in older documents, and the historical wording near the prewarm
array in `port.cpp`, still say the on-disk program-binary cache is disabled.
That wording predates commit `a3b7c5f`; the validated Fast3D cache described
above is the current implementation.

## vitaGL dependency constraints

BattleShip links the machine-local library at:

```text
/Users/robin994/.local/opt/vitadb-deps/vitaGL-nosplash/libvitaGL.a
```

Its source directory is not tracked by this repository. It contains important
hardening that can be lost if the directory is replaced:

- `glLinkProgram()` must handle a NULL result from
  `shark_compile_shader_extended()` before calling any SceGxm program query.
- Compiled-program allocation and shader-patcher registration failures must
  return a clean link failure instead of dereferencing invalid state.
- Cache serialization must never run for a failed/null program.

The null-check changed the original hard data abort inside SceGxm into a
recoverable shader-link failure. It does not make late shader compilation
reliable; early prewarm is still required.

Before rebuilding or replacing vitaGL, inspect the local source and preserve
those patches. Always clean vitaGL before changing compile flags because its
Makefile otherwise reuses objects built with the old flags. After rebuilding
the external `libvitaGL.a`, relink BattleShip explicitly; the outer Makefile
cannot detect that the archive changed.

## Build workflow

Use `Makefile.vita`, never the desktop CMake target. On this host use the
explicit VitaSDK environment so a shell with a reduced PATH still finds the
toolchain:

```sh
cd /Users/robin994/Documents/Code/NeoVitaDB/battleship-vita

env VITASDK=/usr/local/vitasdk \
  PATH=/usr/local/vitasdk/bin:/usr/bin:/bin:/usr/sbin:/sbin \
  make -f Makefile.vita objects -j4

env VITASDK=/usr/local/vitasdk \
  PATH=/usr/local/vitasdk/bin:/usr/bin:/bin:/usr/sbin:/sbin \
  make -f Makefile.vita prepare

env VITASDK=/usr/local/vitasdk \
  PATH=/usr/local/vitasdk/bin:/usr/bin:/bin:/usr/sbin:/sbin \
  make -f Makefile.vita all -j4

git diff --check
```

`prepare` is mandatory. The final linker consumes copies under `build/f/`, not
the newly compiled `.o.k` objects directly. Skipping `prepare` can produce a
valid-looking VPK that silently contains older code.

Use a full clean rebuild after changing global macros, flags, or an ABI-visible
header. Ordinary source changes can use the incremental sequence above. Keep
parallelism at `-j4` on this host unless memory pressure is known to be safe.

Current successfully generated artifacts are:

- `build/battleship.vpk`
- `build/eboot.bin`
- `build/battleship.elf.unstripped.elf`

The current VPK was packaged on 2026-08-21 at approximately 01:44 local time
and contains the 62-program source patch. Build warnings about mixed enum ABI
already existed; the build completed successfully. Keep the unstripped ELF that
matches every tested VPK for coredump symbolization.

## Real-hardware deploy and log collection

Ask the user for the Vita's current IP; do not assume an address from an older
session. With vitacompanion running:

```sh
export BATTLESHIP_VITA_IP="<current Vita IP>"
printf 'destroy\n' | nc -w3 "$BATTLESHIP_VITA_IP" 1338
curl -s -T build/eboot.bin \
  "ftp://$BATTLESHIP_VITA_IP:1337/ux0:/app/SSB64VITA/eboot.bin"
printf 'launch SSB64VITA\n' | nc -w3 "$BATTLESHIP_VITA_IP" 1338
```

Primary logs:

- `ux0:data/battleship/ssb64.log` -> local `build/ssb64.log`
- `ux0:data/vitaGL.log` -> local `build/vitaGL.log`
- `ux0:data/battleship/logs/BattleShip.log` for libultraship/spdlog when needed

Read both `ssb64.log` and `vitaGL.log`. The first identifies scene, shader IDs,
cache health, and performance windows; the second contains Shark/vitaGL compiler
and allocation failures.

Avoid high-frequency `printf`, `sceClibPrintf`, spdlog, or per-draw file logs on
the devkit. System output for an invalid syscall/semaphore can itself dominate
the frame and asset-loading time. Add only bounded/rate-limited diagnostics and
remove them after the question is answered.

## Decision tree after the next device run

### A. Prewarm does not report 62 programs

The device is running a stale binary. Check the VPK/eboot timestamp, repeat
`objects -> prepare -> all`, and verify deployment. Do not diagnose rendering
from that run.

### B. Prewarm is 62/62 and the three shader failures disappear

Check Mario and Pikachu visually and confirm the frame rate. If they are fixed,
commit the `port.cpp` addition with the device log evidence and update this file
and `docs/bugs/vita_vbo_scratch_shader_starvation_2026-08-20.md` to say the
62-program coverage is hardware-confirmed.

### C. Prewarm is 62/62 but a new `shader link failed` pair appears

Record the exact `shader_id0` and `shader_id1`, scene number, heap free value,
and visual symptom. If the generated sources are otherwise valid and the
failure happens only late, add that exact pair to the early list, rebuild, and
test. Do not broadly generate speculative combinations: each extra program
costs startup time and memory.

### D. Prewarm is 62/62, no shader failure occurs, but Mario/Pikachu is still wrong

Only then reopen the texture/relocation path. Keep `b25239a` intact and add
targeted, bounded evidence around the affected CSS draw:

1. Confirm whether Fast3D obtains a non-null `ShaderProgram` for the affected
   draw and whether the draw is actually submitted.
2. Capture the texture-cache key, texture address/range, format/size, palette
   address/range, hit/stale result, and scene for only the first few affected
   draws.
3. Check whether the requested runtime fixup range is fully contained in
   `sTexFixupRanges`, partially overlaps it, or is clamped at a live chain slot.
4. Verify resource reload calls `portEvictStructFixupsInRange()` for the reused
   memory before treating an old range as still fixed.
5. Compare the source bytes and decoded upload, not just the final framebuffer.

Do not restore per-word hashing. If an interval correctness bug is proven, fix
the interval insertion/eviction/gap logic directly and construct a minimal
overlap test or diagnostic reproduction.

### E. Crash instead of a visual defect

Fetch the new `psp2core-*.psp2dmp` and preserve the exact matching
`build/battleship.velf` and `build/battleship.elf.unstripped.elf`. Resolve
addresses with the static ELF base as described in `CLAUDE.md`; do not trust
vita-parse-core's nearest-symbol label by itself.

## Regression rules

Do not reintroduce any of the following:

- Per-word `unordered_set` lookups in the runtime texture fixup.
- Fixed 10 MiB VBO scratch allocations per frame.
- `SDL_GL_SwapWindow()` as the Vita presenter.
- Off-screen `mGameFb` composition on every Vita frame without a proven native
  capture design.
- vitaGL `HAVE_SHADER_CACHE=1` alongside the Fast3D linked-program cache.
- The old unvalidated raw `glProgramBinary()` file format.
- Synchronous flush-per-log-line or large stdout diagnostic streams.
- `%zu`, `%zx`, or other `%z` formats with VitaSDK newlib.
- A lower `_newlib_heap_size_user` based only on theory or Vita3K behavior.
- Multiple memory-pool changes in one hardware experiment.
- Claiming a fix from Vita3K without real-hardware evidence.
- Packaging without `make -f Makefile.vita prepare`.

Do not delete the user's shader cache, assets, logs, or generated archives
without explicit authorization. Prefer copying logs aside before a new run.

## Acceptance checklist for the current issue

- [ ] Device log identifies the build as the 62-program prewarm build.
- [ ] `EARLY_SHADER_PREWARM complete success=62 failed=0`.
- [ ] No late failure for any of the three new shader pairs.
- [ ] Mario selection cursor renders and remains visible.
- [ ] Pikachu renders completely and correctly.
- [ ] Loading screens and previously corrected models remain correct.
- [ ] No VBO allocation drops.
- [ ] Texture cache remains healthy without a stale-entry storm.
- [ ] Tested gameplay remains at the prior approximately 60 FPS target.
- [ ] Warm-cache startup remains within the 5--10 second black-screen target.
- [ ] Only after those gates: commit, document the hardware proof, and push if
      the user authorizes it.

## High-value files

- `port/port.cpp`: Vita initialization, early shader list, direct-FB0 policy.
- `port/bridge/lbreloc_byteswap.cpp`: relocation and runtime texture fixups.
- `libultraship/src/fast/backends/gfx_opengl.cpp`: generated shaders and the
  validated Fast3D program-binary cache.
- `libultraship/src/fast/interpreter.cpp`: Vita memory knobs, VBO submission,
  and frame presentation.
- `libultraship/src/fast/Fast3dWindow.cpp`: early shader self-test hook.
- `Makefile.vita`: dependency paths, flags, object staging, link/package rules.
- `port/port_log.c`: non-blocking bounded logging implementation.
- `docs/bugs/vita_vbo_scratch_shader_starvation_2026-08-20.md`: historical
  evidence for the black-screen/shader-memory investigation; read as a timeline,
  not as the final cache configuration.
- `docs/bugs/texcache_stale_identity_aliasing_2026-07-30.md`: texture-cache and
  relocation correctness invariants.
- `build/ssb64.log` and `build/vitaGL.log`: latest hardware evidence; always
  check their timestamps before assuming they correspond to the newest VPK.

