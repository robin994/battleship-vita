# Vita: crash traced to a desynced `build/f/` copy, not a source bug

**Status: FIXED (build hygiene, no source change)**

## Symptom

`battleship.vpk` crashed on boot on both real Vita hardware and the Vita3K emulator,
always at the same point during early startup — right after the log lines:

```
SSB64: bootstrap archive (shaders) -> app0:/f3d.o2r
SSB64: calling InitResourceManager (bootstrap) ...
```

Two prior captures (`vita_lseek_crash_2026-08-17.txt` and
`vita_lseek_crash_2026-08-17_after-stack-heap-fix.txt`) both showed the crashing
thread's PC jump to `0x0` a few instructions into `Ship::Archive::Load()`
(`libultraship/src/ship/resource/archive/Archive.cpp:33`), immediately after the
call to the virtual `Open()`. The disassembly context in those dumps landed near a
`sceIoClose` export stub and an `fmt::throw_format_error` symbol, but per this
project's own coredump-tool caveat those are unreliable nearest-preceding-symbol
guesses, not the real call site — chasing them literally would have been a
dead end.

## Investigation

Vita3K was already installed locally with BattleShip set up (`ux0:app/SSB64VITA`)
and its own emulator trace log (`logs/SSB64VITA - [BattleShip].log`, distinct from
our `spdlog`/`port_log` sinks) showed the same failure signature: a virtual call
from `Archive::Load()` landing at `PC=0x0`, then the CPU free-running through
zeroed/unmapped memory, logging one `Invalid read` per word until the process was
killed — matching the two on-disk crash dumps closely enough to trust as the same
bug, and critically, this log was fresh enough to match the current build's
unstripped ELF (verified via `arm-vita-eabi-addr2line` resolving `LR` back to a
sane symbol, `Ship::Archive::Load()`).

Two temporary `sceClibPrintf` diagnostics were added around the `Open()` and
`LoadFile("version")` calls in `Archive::Load()` to bracket exactly which virtual
call was landing at a null vtable slot. After an **incremental** `make -f
Makefile.vita objects` → `prepare` → `build/battleship.vpk` cycle (adding only
those two printfs, no other source change) and a fresh Vita3K run, the crash did
not reproduce at all: `Open()` returned `1`, `LoadFile("version")` returned a
clean `nullptr` (no `version` file in the archive — expected, not a bug), and boot
proceeded through window init, first-run asset extraction, and into real gameplay
display-list processing with zero invalid memory accesses logged for the rest of
the run.

Reverting the diagnostics and doing one more from-scratch `objects` → `prepare` →
package cycle reproduced the clean boot again, twice.

## Root cause

Not a code defect. It matches the exact footgun this repo's own `CLAUDE.md`
documents under **PS Vita Port → Build**: `build/battleship.elf` links
`build/f/*.o`, which are *copies* of the real object files made only by the
`prepare` step's `%.ok:` rule. The `.vpk`(s) that produced both `vita_lseek_crash`
captures were built (or last linked) from a `build/f/` snapshot that predated a
source fix already present in `libultraship/src/ship/resource/archive/O2rArchive.cpp`
and/or `Archive.cpp` — `make objects` alone had recompiled the real `.o` files,
but the linker was still packaging stale copies from an earlier `prepare` run, so
the deployed binary silently didn't contain the fix that was already in source.
The in-memory `.o2r` loading path documented in `O2rArchive.cpp` (avoiding
`sceIoLseek32` entirely) was already correct in source at the time of both
crash captures; it just hadn't made it into the tested `.vpk`.

## Fix

No source change. The fix is procedural: always run `objects` → `prepare` →
`build/battleship.vpk` as one sequence (or verify with `strings
build/battleship.elf.unstripped.elf | grep <marker>`) before treating a real
hardware/emulator test as representative of current source, exactly as
`CLAUDE.md` already warns. Re-verified via `arm-vita-eabi-nm` that every
`O2rArchive`/`Archive` vtable slot and virtual-inheritance thunk resolves to
real code in the freshly linked ELF.

## How this was verified

- Vita3K (`Vita3K -z 0 -l 0 -r SSB64VITA`), eboot.bin/f3d.o2r copied directly into
  `ux0:app/SSB64VITA` (same swap-and-relaunch idea as the real-hardware
  vitacompanion loop, done against Vita3K's local `fs/` tree instead).
- Two full runs from a freshly-synced build: zero `Invalid read`/`Invalid write`/
  `PC is 0x0` log lines, vs. every prior capture showing the crash within ~1
  second of boot.
- One run was allowed to continue to completion: first-run `.o2r` re-extraction
  (recipe hash mismatch, expected — `SSB64_ASSET_RECIPE_HASH` was rebuilt
  locally) completed successfully, followed by real display-list rendering
  (`gcDrawDObj`) with no faults.
- Real hardware was not re-tested in this session — do that next with the
  freshly linked `build/battleship.vpk`, since Vita3K's boot divergence from
  real firmware is exactly what caused the original `sceIoLseek32` bug to only
  show up on hardware (see `CLAUDE.md`'s Vita3K caveat).
