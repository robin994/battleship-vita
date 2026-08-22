#!/usr/bin/env bash
# build-vpk.sh — compile everything NOT covered by the other 3 project
# scripts (port/**, debug_tools/**, vendor/**: this project's own porting
# glue code), then link and package build/battleship.vpk.
#
# One of a 4-script split for this Vita build:
#   build-decomp.sh         — decomp/**
#   build-torch.sh           — torch/** + libgfxd/**
#   build-libultraship.sh    — libultraship/** + imgui/**
#   build-vpk.sh             — this file: the rest, + prepare + link + package
#
# Typical full-build sequence:
#   scripts/build-decomp.sh
#   scripts/build-torch.sh
#   scripts/build-libultraship.sh
#   scripts/build-vpk.sh
# (order between the first three doesn't matter; each only touches its own
# object files). Running build-vpk.sh alone is also fine — it compiles
# whatever's left, same as the others, before packaging.
#
# `prepare` is MANDATORY and runs SERIALLY: its %.ok rule numbers the copies
# under build/f/N.o with a make-time counter that is not parallel-safe. A
# parallel prepare can double-assign slots, silently dropping objects ->
# undefined references at link. Skipping prepare produces a valid-looking
# VPK containing STALE code — this script never lets you skip it.
#
# Usage:
#   scripts/build-vpk.sh
#
# Environment overrides:
#   JOBS      compile/link parallelism (default 4)
#   VITASDK   toolchain root (default /usr/local/vitasdk)

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

VITASDK="${VITASDK:-/usr/local/vitasdk}"
JOBS="${JOBS:-4}"
export VITASDK
export PATH="$VITASDK/bin:/usr/bin:/bin:/usr/sbin:/sbin"

MAKE="make -f Makefile.vita"

step() { echo "==> $*"; }
fail() { echo "build-vpk: ERROR: $*" >&2; exit 1; }

[ -x "$VITASDK/bin/arm-vita-eabi-g++" ] || fail "toolchain not found at $VITASDK (set VITASDK=...)"
[ -f /Users/robin994/.local/opt/vitadb-deps/vitaGL-nosplash/libvitaGL.a ] \
	|| fail "vendored vitaGL not found (~/.local/opt/vitadb-deps/vitaGL-nosplash) - see CLAUDE.md"
[ -f Makefile.vita ] || fail "run from the repo root (Makefile.vita not found)"

step "resolving remaining (port/, debug_tools/, vendor/, ...) object list from Makefile.vita"
OBJS=()
while IFS= read -r line; do OBJS+=("$line"); done < <($MAKE -Bn objects 2>/dev/null | grep -oE -- '-o [^ ]+\.o ' | awk '{print $2}' | grep -vE '^(decomp|torch|libgfxd|libultraship|imgui)/')
if [ "${#OBJS[@]}" -gt 0 ]; then
	step "compiling ${#OBJS[@]} remaining object file(s) (-j$JOBS)"
	$MAKE "${OBJS[@]}" -j"$JOBS"
else
	step "no remaining objects outside decomp/torch/libgfxd/libultraship/imgui — skipping"
fi

step "staging objects into build/f (prepare, serial)"
$MAKE prepare

step "link + package vpk"
$MAKE all -j"$JOBS"

VPK=build/battleship.vpk
[ -f "$VPK" ] || fail "$VPK missing after build"

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
