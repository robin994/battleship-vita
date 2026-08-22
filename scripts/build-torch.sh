#!/usr/bin/env bash
# build-torch.sh — compile just the torch/ (Torch asset-reloc engine, the
# same source this game's RelocFactory/naudio factories come from) +
# libgfxd/ (vendored GBI-disassembly dependency Torch's factories use)
# subset of object files, via Makefile.vita.
#
# Note: this is NOT the standalone host `torch` executable used to extract
# BattleShip.o2r from a ROM (see scripts/extract-vita-data.sh for that — a
# separate CMake host build of the same torch/ submodule). This script
# compiles Torch's *Vita-target* object files, which get linked directly
# into battleship.elf (see Makefile.vita's TORCH_SOURCES) to provide the
# runtime reloc/asset-resolution logic the game itself calls into.
#
# One of a 4-script split for this Vita build:
#   build-decomp.sh         — decomp/**
#   build-torch.sh           — this file (torch/** + libgfxd/**)
#   build-libultraship.sh    — libultraship/** + imgui/** (its GUI dependency)
#   build-vpk.sh             — everything else (port/**, debug_tools/**,
#                              vendor/**) + prepare + link + package the VPK
#
# Makefile.vita still compiles all of these into ONE battleship.elf — there
# are no per-project static libs to build separately. These scripts just
# narrow *which* object files get (re)compiled on a given run, by asking
# make itself (via a forced dry run) which .o targets currently live under
# this project's source directories — so the file list can never drift out
# of sync with Makefile.vita's own TORCH_SOURCES/GFXD_SOURCES definitions.
#
# Usage:
#   scripts/build-torch.sh
#
# Environment overrides:
#   JOBS      compile parallelism (default 4)
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
fail() { echo "build-torch: ERROR: $*" >&2; exit 1; }

[ -x "$VITASDK/bin/arm-vita-eabi-g++" ] || fail "toolchain not found at $VITASDK (set VITASDK=...)"
[ -f Makefile.vita ] || fail "run from the repo root (Makefile.vita not found)"

step "resolving torch/ + libgfxd/ object list from Makefile.vita"
OBJS=()
while IFS= read -r line; do OBJS+=("$line"); done < <($MAKE -Bn objects 2>/dev/null | grep -oE -- '-o [^ ]+\.o ' | awk '{print $2}' | grep -E '^(torch|libgfxd)/')
[ "${#OBJS[@]}" -gt 0 ] || fail "no torch/libgfxd objects found in Makefile.vita's source list"

step "compiling ${#OBJS[@]} torch/libgfxd object file(s) (-j$JOBS)"
$MAKE "${OBJS[@]}" -j"$JOBS"

echo
step "OK: torch/ + libgfxd/ objects up to date (${#OBJS[@]} files)"
echo "Next: scripts/build-vpk.sh to link + package (after the other two project scripts, if any of them changed too)."
