#!/usr/bin/env bash
# build-vita.sh — build the PS Vita VPK (Makefile.vita wrapper).
#
# Usage:
#   scripts/build-vita.sh              # incremental: objects -> prepare -> vpk
#   scripts/build-vita.sh fast         # same as above (explicit)
#   scripts/build-vita.sh full         # clean + full rebuild from scratch
#   scripts/build-vita.sh package      # re-link + re-package only, no compile
#
# Environment overrides:
#   JOBS=4        compile parallelism for `objects` (default 4; the M1 16GB
#                 host pins fans/swap beyond that — see CLAUDE.md)
#   VITASDK=...   toolchain root (default /usr/local/vitasdk)
#
# Notes baked in from hard-won experience (CLAUDE.md / vita handoff):
#   - `prepare` is MANDATORY and runs SERIALLY: its %.ok rule numbers the
#     copies under build/f/N.o with a make-time counter ($(eval gen_num=...))
#     that is not parallel-safe. A parallel prepare can double-assign slots,
#     silently dropping objects -> undefined references at link.
#   - Skipping prepare produces a valid-looking VPK containing STALE code.
#     This script never lets you skip it on fast/full paths.
#   - Use `full` after changing global macros (SPDLOG_ACTIVE_LEVEL, NDEBUG,
#     CFLAGS/CXXFLAGS): plain `objects` won't recompile TUs make doesn't know
#     are affected.

set -euo pipefail

MODE="${1:-fast}"
case "$MODE" in
	fast|full|package) ;;
	*)
		echo "usage: $0 [fast|full|package]" >&2
		exit 2
		;;
esac

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

VITASDK="${VITASDK:-/usr/local/vitasdk}"
JOBS="${JOBS:-4}"
export VITASDK
export PATH="$VITASDK/bin:/usr/bin:/bin:/usr/sbin:/sbin"

MAKE="make -f Makefile.vita"

die() { echo "build-vita: ERROR: $*" >&2; exit 1; }
step() { echo "==> $*"; }

[ -x "$VITASDK/bin/arm-vita-eabi-g++" ] || die "toolchain not found at $VITASDK (set VITASDK=...)"
[ -f /Users/robin994/.local/opt/vitadb-deps/vitaGL-nosplash/libvitaGL.a ] \
	|| die "vendored vitaGL not found (~/.local/opt/vitadb-deps/vitaGL-nosplash) - see CLAUDE.md"
[ -f Makefile.vita ] || die "run from the repo root (Makefile.vita not found)"

if [ "$MODE" = "package" ]; then
	step "re-packaging from existing artifacts (no compile)"
	$MAKE all -j"$JOBS"
else
	if [ "$MODE" = "full" ]; then
		step "clean (full rebuild requested)"
		$MAKE clean
	fi

	step "compile (objects -j$JOBS)"
	$MAKE objects -j"$JOBS"

	step "stage objects into build/f (prepare, serial)"
	$MAKE prepare

	step "link + package vpk"
	$MAKE all -j"$JOBS"
fi

VPK=build/battleship.vpk
[ -f "$VPK" ] || die "$VPK missing after build"

echo
step "artifacts:"
ls -lh "$VPK" build/eboot.bin build/battleship.elf.unstripped.elf build/f3d.o2r 2>/dev/null || true

# Stale-artifact guard: if any in-tree object is newer than what we linked,
# something raced the build (parallel session?) or prepare dropped a slot.
NEWER=$(find port decomp/src libultraship/src torch/src libgfxd -name '*.o' -newer "$VPK" 2>/dev/null | head -3)
if [ -n "$NEWER" ]; then
	echo
	echo "WARNING: objects newer than the VPK (concurrent build or missed staging?):" >&2
	echo "$NEWER" >&2
fi

echo
echo "OK: $VPK ready."
