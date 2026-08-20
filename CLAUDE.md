# SSB64 PC Port — Claude Session Context

PC port of Super Smash Bros. 64 built from the complete decompilation at github.com/VetriTheRetri/ssb-decomp-re. Target integration: libultraship (LUS) + Torch asset pipeline.

## Documentation

Detailed reference material lives under `docs/`. Read the file that matches the task before touching code. When looking for a topic not listed here, run `ls docs/` and `ls docs/bugs/` to see what's available.

| Topic | File |
|-------|------|
| Project status, ROM info, dependencies, source tree layout | `docs/architecture.md` |
| C type system, decomp naming prefixes, code style, macros | `docs/c_conventions.md` |
| RDRAM / RSP / RDP / GBI / audio / threading / controller / endianness | `docs/n64_reference.md` |
| CMake build, reloc stub regen, runtime logs, LP64 compat notes | `docs/build_and_tooling.md` |
| GBI trace capture (port + M64P plugin) and `gbi_diff.py` usage | `docs/debug_gbi_trace.md` |
| IDO BE bitfield layout audit (compile + rabbitizer disasm to verify port struct bit positions) | `docs/debug_ido_bitfield_layout.md` |
| Resolved bugs (index + per-bug root cause / fix write-ups) | `docs/bugs/README.md` |

Ongoing investigations and handoff notes are loose `.md` files at the top level of `docs/` — check there before starting work on rendering, collision, or animation issues so you don't duplicate prior effort.

When you fix a new significant bug, add an entry under `docs/bugs/` using the slug pattern `<topic>_<YYYY-MM-DD>.md` and link it from `docs/bugs/README.md`.

---

## GitHub Issue Access

When asked to inspect GitHub issues, prefer the GitHub connector. If issue tools
are not visible yet, first run tool discovery for "GitHub issue fetch/view" so
the connector exposes `_fetch_issue` and `_fetch_issue_comments`, then fetch with
`repository_full_name: "JRickey/BattleShip"`.

The local GitHub CLI is also authenticated as `JRickey` and has admin access to
`JRickey/BattleShip`. If the human-formatted `gh issue view` output is blank or
unreliable, use the JSON/template path instead:

```bash
gh issue view <number> -R JRickey/BattleShip --json number,title,state,author,body,url,comments,labels
```

Known-good check from 2026-06-01: issue #209 and its comments were accessible
through both the connector and `gh --json`; #209 had no comments at that time.

---

## Parallel Sessions — Worktree Workflow

Multiple Claude windows working in the same checkout will clobber each other's source edits and build outputs. **Every parallel session works in its own git worktree.**

### Spinning up a new worktree

```bash
./scripts/new-worktree.sh <slug>           # configure only (fast)
./scripts/new-worktree.sh <slug> --build   # configure + full Debug compile
./scripts/new-worktree.sh <slug> --base some-branch --release
```

Output lands at `.claude/worktrees/<slug>` on branch `agent/<slug>`. The script:
1. Creates the worktree and branch.
2. Symlinks `baserom.us.z64` (gitignored, too large to duplicate).
3. **Independently clones `libultraship`, `torch`, and `decomp`** from the main tree's local submodule checkouts (picks up pinned SHAs that may not be pushed to the forks yet), then resets each submodule's `origin` to whatever URL the main tree's submodule uses — usually SSH so pushes work.
4. Regenerates gitignored codegen (`reloc_data.h`, `yamls/us/reloc_*.yml`, credits encodings).
5. Runs `cmake -B build` inside the worktree (and compiles if `--build` given).

### What this gives you

- **Full edit authority everywhere** — any file under `decomp/src/`, `decomp/include/`, `port/`, `libultraship/`, `torch/` is fair game. Submodule checkouts are real independent clones, not symlinks.
- **Zero collision** with other windows on source, build artifacts, or submodule state.
- **Normal git flow for submodule changes**:
  1. Edit and commit inside `<worktree>/decomp/`, `<worktree>/libultraship/`, or `<worktree>/torch/`.
  2. Push to the fork: `git -C <worktree>/<sm> push origin <branch>`.
     - `decomp` → `port-patches` on `JRickey/ssb-decomp-re`
     - `libultraship` → `ssb64` on `JRickey/libultraship`
     - `torch` → `ssb64` on `JRickey/Torch`
  3. In the outer worktree, bump the submodule pointer: `git add <sm> && git commit -m "Bump <sm>: <summary>"`.
  4. When the outer branch lands on main, the pointer update goes with it.

### Merging back to main

The outer worktree is a normal branch (`agent/<slug>`). Merge or PR it into `main` like any other branch. Submodule pointer bumps ride along in the commits.

### Cleanup

```bash
git worktree remove .claude/worktrees/<slug>
git branch -D agent/<slug>
```

Stale worktrees under `.claude/worktrees/` from past sessions are fine to remove — check `git worktree list` and prune anything you don't recognize.

### Gotchas

- **Never use relative `build` paths in Bash tool calls** — Claude Code resets cwd between `Bash` calls. `cmake --build build` from the project root builds the main tree, not the worktree. Always use absolute paths: `cmake --build <worktree>/build ...`.
- **Cap build parallelism at `-j 4` on the M1 16 GB machine.** Bare `-j` lets make spawn one clang per logical core; libultraship's Debug C++ TUs hold 1–2 GB resident each, so 8 in parallel push the laptop into swap and pin the fans for the whole compile. Always: `cmake --build <worktree>/build --target ssb64 -j 4`. Push to `-j 6` only when you know nothing else heavy is open. Worktree first-builds are full from-scratch (no shared cache with main tree), so this matters most the first time.
- The binary loads `BattleShip.o2r` (ROM-derived, user-extracted) and `f3d.o2r` (shaders) from its CWD at launch; without them it exits with `archive ... does not exist`. `new-worktree.sh` symlinks both from the main tree's `build/` into the worktree's `build/`. If the main tree has never been extracted, run `cmake --build <main-tree>/build --target ExtractAssets` there first so the symlinks resolve. (`ssb64.o2r` was an early-development port-asset archive; it is no longer produced or loaded — the build never contains ROM-derived data beyond the user's own first-run `BattleShip.o2r`.)

---

## PS Vita Port

Real-hardware Vita bring-up lives on `main`/`vita-rinne-merge` branches (not upstream `JRickey/BattleShip`).
Repos:
- Outer tree: `robin994/battleship-vita` (`main`), remote name `vita-origin`.
- `libultraship` submodule: `robin994/libultraship-vita` (`vita-rinne-merge`), remote name `vita-fork` (also `origin` inside the submodule).
- `decomp` submodule: `robin994/ssb-decomp-re-vita` (`vita-compat-fixes`).
- `torch` submodule: unmodified, still `JRickey/Torch` (`ssb64`).

### Build

Uses `Makefile.vita`, not the CMake build the rest of this doc describes. Needs VitaSDK (`VITASDK` env var, default `/usr/local/vitasdk`) and a locally-built vitaGL at `~/.local/opt/vitadb-deps/vitaGL-nosplash` (see below — **not** the stock vitasdk-packaged vitaGL).

```sh
make -f Makefile.vita objects -j$(sysctl -n hw.ncpu)   # compile
make -f Makefile.vita prepare                           # copy build/f/*.o.k -> build/f/*.o
make -f Makefile.vita build/battleship.vpk               # link + package
```

**The `prepare` step is not optional and is easy to forget.** `build/battleship.elf` links `build/f/*.o`, which are *copies* of the real `.o` files made only by `prepare`'s `%.ok:` rule. Running `objects` alone recompiles the real object files but the linker still picks up stale copies in `build/f/` — you'll get a vpk that silently doesn't contain your change. After touching any source, always re-run `prepare` before packaging, or just always run the three commands in sequence. When in doubt, verify with `strings build/battleship.elf.unstripped.elf | grep <a string unique to your edit>` before deploying.

Output artifacts all land under `build/` (gitignored): `battleship.elf`, `battleship.elf.unstripped.elf` (keep for `addr2line`), `battleship.velf`, `eboot.bin`, `battleship.vpk`, plus `build/f3d.o2r` (zipped shader archive, rebuilt from `libultraship/src/fast/shaders/*/*`).

Full clean rebuild (`make -f Makefile.vita clean` then `objects`) is required whenever a **macro visible to many translation units** changes — `SPDLOG_ACTIVE_LEVEL`, `NDEBUG`, anything in the global `CFLAGS`/`CXXFLAGS` in `Makefile.vita`. `make objects` alone won't recompile files make doesn't think are affected. A normal incremental `objects` is fine for ordinary source edits.

### vitaGL: which build, and how to rebuild it

`Makefile.vita` links `-L$(VITAGL_NOSPLASH_DIR)` against `~/.local/opt/vitadb-deps/vitaGL-nosplash/libvitaGL.a` — a locally vendored source checkout, not vitasdk's packaged vitaGL. To rebuild it with specific flags:

```sh
cd ~/.local/opt/vitadb-deps/vitaGL-nosplash
make clean
make HAVE_PTHREAD=1 LOG_ERRORS=1 HAVE_GLSL_TEXTURE_SIZE=1 INDICES_SPEEDHACK=1 NO_SPLASHSCREEN=1 HAVE_SHADER_CACHE=1 -j$(sysctl -n hw.ncpu)
```

Gotchas found this session:
- **`HAVE_PTHREAD` is structurally broken in this vendored source**: `gxm.c`'s garbage-collector-thread creation checks `#ifdef HAVE_PTRHEAD` — letters transposed (should be `HAVE_PTHREAD`) — so passing `HAVE_PTHREAD=1` compiles the flag into `CFLAGS` but the check never matches; the GC thread always falls through to `sceKernelCreateThread`. Not yet fixed upstream in this vendor copy; fix the typo in `source/gxm.c` if `pthread`-based GC threading is ever actually needed.
- **`VGL_GIT_HASH` must be supplied**: `source/splashscreen.c` does `const char *commit_hash = "#" VGL_GIT_HASH;` with no fallback. This vendored copy isn't a git checkout, so the Makefile now has `CFLAGS += -DVGL_GIT_HASH='"local-nosplash-build"'` added permanently near the top — don't remove it or the build fails on that one file.
- **Flags don't retroactively apply to existing `.o` files.** Always `make clean` before changing flags (`HAVE_PTHREAD`, `LOG_ERRORS`, `NO_SPLASHSCREEN`, etc.) — verified by symbol inspection (`arm-vita-eabi-nm libvitaGL.a | grep <symbol only present under that flag>`) that a plain `make FLAG=1` reused stale objects and silently didn't apply the flag.
- **Keep `HAVE_SHADER_CACHE=1` enabled for the Vita port.** The mandatory early Fast3D prewarm otherwise recompiles all observed programs on every launch. vitaGL stores successful custom shader GXPs below `ux0:data/shader_cache/<titleid>/v0/{v,f}`. Its cache-write path must check `s->prog`, the serialized buffer/size, and the `sceIoOpen()` result before writing; a Shark compile failure must never be serialized.
- **Release startup target is 5–10 seconds, including a clean install.** After one complete cold hardware run, copy `ux0:data/shader_cache/SSB64VITA/v0/{v,f}` to `port/vita_shader_cache/v0/{v,f}` without renaming the GXP files. `Makefile.vita` then packages them at `app0:/shader_cache/v0/{v,f}` and the renderer selects that read-only cache before `vglInitWithCustomThreshold()`. Do not substitute Vita3K's `shaderlog/*.gxp`: those are emulator pipeline dumps, not vitaGL's serialized custom-shader cache.
- After rebuilding vitaGL, BattleShip's own `battleship.elf` needs relinking (delete `build/battleship.elf*` and re-run the link step) — `make objects` won't detect that the external `.a` changed.
- `~/.local/opt/vitadb-deps/vitaGL-nosplash` is a filesystem path specific to this dev machine, not tracked by any repo here — if it's missing, ask the user where their vendored vitaGL lives before assuming a fresh checkout is safe to `make clean`.

### Deploy / test on real hardware — vitacompanion

Real-hardware iteration uses [`vitacompanion`](https://github.com/devnoname120/vitacompanion) (taiHEN plugin, FTP on port 1337 + command server on port 1338), **not** a physical USB/install cycle. Get the device's current IP from the user — it's not fixed across sessions/networks. Loop:

```sh
echo "destroy" | nc -w3 $VITA_IP 1338
curl -s -T build/eboot.bin ftp://$VITA_IP:1337/ux0:/app/SSB64VITA/eboot.bin
echo "launch SSB64VITA" | nc -w3 $VITA_IP 1338
```

Then poll logs over FTP (see next section) rather than waiting on a live `nc -kl 9999` listener — the live listener is *less* reliable than reading the log files directly, since our own logging pipeline can lag behind a fast network capture.

### Logging

Two independent, decoupled log sinks exist on Vita — **check both** when diagnosing a real-hardware issue:

1. **`ux0:data/battleship/ssb64.log`** — our own `port_log()` (see `port/port_log.c`). Writes via a raw fd + `write(2)` on a dedicated background thread (deliberately bypasses libc stdio — see below). Always use `sceClibPrintf`/`port_log()`, never raw `printf`/`fprintf` to stdout for anything meant to survive a crash.
2. **`ux0:data/battleship/logs/BattleShip.log`** — libultraship's own `spdlog` sink (rotating file + console). **Two independent gates control whether anything appears here, and both must be right or the file silently stays empty:**
   - **Compile-time**: `SPDLOG_ACTIVE_LEVEL` in `Makefile.vita`'s `CFLAGS`. This is *not* the runtime log level — it decides which `SPDLOG_*` macro call-sites exist in the binary at all (level below the threshold compiles to nothing, `(void)0`). Was `6` (`SPDLOG_LEVEL_OFF`) for a long time, silently deleting every log call in the engine including all `SPDLOG_INFO`/`SPDLOG_TRACE` diagnostics in `ResourceManager`/`ArchiveManager`. Now `2` (`SPDLOG_LEVEL_INFO`); `SPDLOG_TRACE`/`SPDLOG_DEBUG` call sites (level 0/1) are still compiled out at this setting.
   - **Runtime**: `Context::InitLogging()`'s `releaseBuildLogLevel` param, called from `port.cpp`'s `PortInit()` with no args by default → defaults to `spdlog::level::warn`, filtering out INFO/DEBUG/TRACE regardless of what got compiled in. Pass an explicit level (`sContext->InitLogging(spdlog::level::debug, spdlog::level::info)`) to see more.
   - Vita's `flush_on()` policy is separately set to `spdlog::level::err` (not `info`) in `Context.cpp` — `async_logger::flush()` blocks the calling thread until the write completes, and `flush_on(info)` combined with ArchiveManager's per-resource INFO logging turned every routine log line into a real-hardware microSD stall. Don't casually flip this back to `info`/`trace` on real hardware without expecting stalls; it's fine as a throwaway diagnostic build but revert after.

### `port_log()` architecture (`port/port_log.c`)

Two independent queues, two independent writer threads (one for `sceClibPrintf`, one for the file), each writing via a **raw fd + `write(2)`**, not buffered stdio (`fopen`/`fputs`/`fflush`). This was a deliberate fix, not the original design — buffered stdio's internal ~1KB flush boundary reproducibly hung the whole process on real hardware the first time it filled (proven via coredump: main thread parked inside `fflush -> __sfvwrite_r -> __swrite -> sceIoWrite`). If you ever see a real-hardware "hang" that always happens at the same log content length, suspect the same class of bug and check whether something is using buffered C stdio for anything meant to run continuously.

Don't reintroduce buffered file I/O into this path, and don't add a synchronous flush-every-line policy without a real reason — both were tried and both reproduce hangs on this hardware/media.

### `printf`/`vsnprintf` `%z` is broken on this VitaSDK toolchain

**Never use `%zu`/`%zx`/`%zd`/etc. (the `size_t` length modifier) in any `port_log()`, `fprintf`, `snprintf`, or `ImGui::Text` call.** VitaSDK's newlib `vsnprintf` doesn't implement the `z` length modifier — it prints the literal characters `z`/`x`/`u` and, critically, **does not consume the corresponding variadic argument**. Every argument after the first `%z*` in that call shifts by one for the rest of the format string. Where a later `%s` or `%p` ends up consuming an unrelated/garbage value, this can (and did, repeatedly, on real hardware and under Vita3K) crash inside `strlen()` reading a near-null pointer. `ImGui::Text` is equally affected — it calls the platform's `vsnprintf` too (`IMGUI_USE_STB_SPRINTF` is not defined in this build).

Use `%u`/`%x`/`%d` with an explicit `(unsigned int)`/`(int)` cast instead — `size_t` is 32-bit on this ARM32 target, so no information is lost. All occurrences compiled into the Vita build were fixed as of 2026-08; if you add a new `port_log`/`fprintf`/`ImGui::Text` call with a `size_t`, `.size()`, or similar 64-on-other-platforms value, cast it — don't reach for `%z`.

### `sceUserMainThreadStackSize` / `_newlib_heap_size_user`

Both are weak-symbol overrides VitaSDK's toolchain reads by exact name from the final ELF (see `port/port.cpp`, right above `main()`). Neither was ever set before this session, and a real-hardware coredump showed the crashing thread's entire stack mapping was **16KB total**, with ~4.5KB left below SP at a fairly shallow call depth (libzip → newlib stdio → a syscall stub). Now: `sceUserMainThreadStackSize = SCE_KERNEL_4MiB`, `_newlib_heap_size_user = SCE_KERNEL_256MiB`.

**Don't lower the heap value without retesting on real hardware.** 192MiB was tried (to leave more room for GXM/vitaGL, per a memory-pressure suggestion) and produced a hard, early data-abort crash — `malloc()` (`_malloc_r` → `operator new` → a `shared_ptr` control-block constructor) handed back a pointer into memory that then faulted on first write, i.e. the reduced heap block itself came back unusable very early in `InitResourceManager`. `_newlib_heap_size_user` is an **up-front reservation** (`_init_vita_heap` grabs the whole block at startup), not a ceiling — every byte here is a byte GXM/vitaGL's own allocator can't have, so there's a real tradeoff, but 256MiB is the last value confirmed to boot.

Other memory-pressure knobs (from Rinnegatamante) that are already applied: `vglSetParamBufferSize(6 * 1024 * 1024)` and `vglUseTripleBuffering(0)` (double- instead of triple-buffering) in `libultraship/src/fast/interpreter.cpp`, right before `vglInitWithCustomThreshold`. Anything more aggressive (further heap reduction, VBO/texture cache size cuts) should be tested individually on real hardware, not assumed safe from Vita3K behavior alone (see below).

### `.o2r` archives are loaded fully into RAM on Vita, not streamed from disk

`libultraship/src/ship/resource/archive/O2rArchive.cpp`'s `Open()` has a Vita-only path (`#ifdef __vita__`): the whole archive file is read into a `std::vector<uint8_t>` up front via raw `sceIoOpen`/`sceIoGetstat`/`sceIoRead` (chunked, no `lseek` at all), then handed to libzip as an in-memory buffer source (`zip_source_buffer_create` + `zip_open_from_source`). This replaces the normal `zip_open(path, ...)`, which goes through libzip's stdio-based file source.

**Why:** a real-hardware coredump repeatedly showed the main thread taking a genuine fault inside `sceIoLseek32`, reached via `_zip_stdio_op_seek → _fseeko_r → __sseek → _lseek_r → sceIoLseek32`, always at the exact same archive byte offset — fully deterministic across relaunches, but the identical binary ran fine under the Vita3K emulator (rendered 100+ frames), meaning this is a real-firmware/libzip/lseek interaction that HLE emulation doesn't reproduce. As of the last real-hardware test in this session, this specific crash was **not yet confirmed fixed** — the in-memory-load change was deployed but hadn't cleared a boot past that point yet when the session ended; treat it as the current best lead, not a proven fix, until re-verified.

**Consequence: archives are read-only on Vita.** `O2rArchive::WriteFile()` fails loudly (logs an error, returns `false`) on `__vita__` instead of reopening a disk-backed handle — nothing in the boot path writes to an archive, so this was a deliberate simplification, not an oversight. If a future feature needs to write `.o2r` files on Vita, this will need a real implementation (e.g. write into `mArchiveBuffer` and re-source, or fall back to a disk-backed zip source for that one call).

`mZipArchive` must be initialized to `nullptr` in the constructor (it wasn't, originally) — `Open()`'s early-return-on-failure paths can leave it untouched, and the destructor's `Close()` only skips `zip_close()` when it's null; otherwise it hands a garbage pointer to `zip_close()` and takes a data abort.

### `NDEBUG` is a temporary diagnostic, not a real fix

`Makefile.vita`'s `CFLAGS` currently has `-DNDEBUG`, disabling all `assert()`s. This was added to get past a real bug, not to permanently disable assertions: ImGui's `AddFontDefault()` decompresses and parses its bundled `ProggyClean.ttf` via stb_truetype, which fires a genuine internal consistency assert (`output_ctx.num_vertices == count_ctx.num_vertices` in `stbtt__GetGlyphShapeT2`) on real hardware — a real vertex-count-mismatch bug in glyph outline extraction, not yet root-caused. Worse, the assert's own failure-message printing path itself crashes (`_svfprintf_r`), so before `NDEBUG` the process just took a raw, unexplained fault at that point with no diagnostic ever reaching the log. `NDEBUG` unblocks boot past that point but doesn't fix the underlying mismatch — if font rendering ever looks visually wrong (garbled default-font glyphs), this is the suspect. Don't remove `-DNDEBUG` without either fixing the stb_truetype bug or re-confirming this specific assert no longer fires.

### Coredump analysis (`psp2dmp`)

Real-hardware crashes generate Sony's own firmware coredumps (`ux0:data/psp2core-*.psp2dmp` — device-global, not per-app, mixed in with dumps from any other homebrew tested on the same device). Fetch over the vitacompanion FTP server and parse with [`xyzz/vita-parse-core`](https://github.com/xyzz/vita-parse-core) (Python 3):

```sh
python3 main.py <coredump>.psp2dmp build/battleship.velf
```

**Critical gotcha**: the tool's printed offsets are `battleship.elf@1 + 0xNNNNNN` — a *runtime* offset from the module's load base, not a directly-addr2line-able address. Before calling `arm-vita-eabi-addr2line -f -C -e build/battleship.elf.unstripped.elf <addr>`, add the ELF's own static link base (`0x81000000` for this build) to that offset. Feeding the raw runtime address (or the raw offset) directly gives silently-wrong-but-plausible-looking symbol names.

Also **don't trust the tool's own inline disassembly nearest-symbol labels** (e.g. a crash address labeled `<sceIoClose-0x81803f68>`) — these are "nearest preceding export" guesses and were repeatedly wrong this session (a `strlen` crash showed as `fmt::throw_format_error` this way). Always independently resolve PC/LR through `addr2line` on the corrected static address, and treat any resolved symbol pointing at newlib/libstdc++ internals (`_svfprintf_r`, `_malloc_r`, `operator new`, etc.) as "who called this" rather than "this is the bug" — walk one more frame up via the stack dump or LR to find the actual call site in our own code.

### Vita3K emulator as a secondary (not equivalent) test target

The user can also test builds under the [Vita3K](https://vita3k.org) emulator, which produces much more verbose crash diagnostics (register dumps, `Invalid read/write` memory-access warnings with exact addresses, live `sceClibPrintf` capture) than real hardware's terse coredumps. **Useful for finding bugs, but boot behavior meaningfully diverges from real hardware** — a build that runs 100+ frames cleanly on Vita3K crashed deterministically on real hardware at boot (the `sceIoLseek32` issue above). Treat "works on Vita3K" as evidence a given code path *can* run correctly, not as proof it works on real hardware — always confirm real-hardware behavior separately before considering something fixed.

---

## Agent Directives

### Pre-Work

1. **THE "STEP 0" RULE**: Before any structural refactor on a file >300 LOC, first remove dead code, unused exports, unused imports, and debug logs. Commit cleanup separately.

2. **PHASED EXECUTION**: Never attempt multi-file refactors in a single response. Break work into phases. Complete Phase 1, run verification, wait for approval before Phase 2. Max 5 files per phase.

### Code Quality

3. **THE SENIOR DEV OVERRIDE**: If architecture is flawed, state is duplicated, or patterns are inconsistent — propose and implement structural fixes. Ask: "What would a senior, experienced, perfectionist dev reject in code review?" Fix all of it.

4. **FORCED VERIFICATION**: Do not report a task complete until you have run the build and fixed all errors. If no build is configured yet, state that explicitly.

5. **DECOMP PRESERVATION — preserve behavior, not byte-matching**: The decomp describes the *game*, not the build. Keep IDO idioms (goto, odd casts, temp variables) that encode original N64 semantics — those are load-bearing and must not be "modernized." But don't preserve **compiler compat shims** (warning suppressions, permissive flags, header shortcuts) that hurt port stability just to avoid touching decomp source. If a suppressed diagnostic is masking real bugs on modern LP64 toolchains (e.g., `-Wno-implicit-function-declaration` silently truncating 64-bit pointer returns to `int`), fix the root cause — add the missing include, wrap a port fix in `#ifdef PORT`, or adjust the decomp file itself — rather than keeping the suppression. **Accuracy to game behavior > accuracy to ROM bytes.** When choosing between stability and ROM-matching, choose stability and document the deviation in `docs/bugs/`.

### Context Management

6. **SUB-AGENT SWARMING**: For tasks touching >5 independent files, launch parallel sub-agents. Each agent gets its own context window.

7. **CONTEXT DECAY AWARENESS**: After 10+ messages, re-read any file before editing. Do not trust memory of file contents.

8. **FILE READ BUDGET**: For files over 500 LOC, use offset and limit parameters to read in chunks.

9. **EDIT INTEGRITY**: Before every edit, re-read the file. After editing, verify the change applied correctly. Never batch >3 edits to the same file without a verification read.

10. **NO SEMANTIC SEARCH**: When renaming or changing any function/type/variable, search separately for: direct calls, type references, string literals, dynamic references, re-exports, and tests.
