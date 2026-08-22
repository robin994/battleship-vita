#!/usr/bin/env bash
# build-libultraship.sh — compile just the libultraship/ (Fast3D renderer,
# resource manager, audio/controller layers) + imgui/ (its bundled GUI
# dependency, see LUS_INCLUDES in Makefile.vita) subset of object files,
# via Makefile.vita.
#
# One of a 4-script split for this Vita build:
#   build-decomp.sh         — decomp/**
#   build-torch.sh           — torch/** + libgfxd/**
#   build-libultraship.sh    — this file (libultraship/** + imgui/**)
#   build-vpk.sh             — everything else (port/**, debug_tools/**,
#                              vendor/**) + prepare + link + package the VPK
#
# Makefile.vita still compiles all of these into ONE battleship.elf — there
# are no per-project static libs to build separately. These scripts just
# narrow *which* object files get (re)compiled on a given run, by asking
# make itself (via a forced dry run) which .o targets currently live under
# this project's source directories — so the file list can never drift out
# of sync with Makefile.vita's own LUS_SOURCES/CPPFILES definitions.
#
# Usage:
#   scripts/build-libultraship.sh
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
fail() { echo "build-libultraship: ERROR: $*" >&2; exit 1; }

[ -x "$VITASDK/bin/arm-vita-eabi-g++" ] || fail "toolchain not found at $VITASDK (set VITASDK=...)"
[ -f Makefile.vita ] || fail "run from the repo root (Makefile.vita not found)"

step "resolving libultraship/ + imgui/ object list from Makefile.vita"
OBJS=()
while IFS= read -r line; do OBJS+=("$line"); done < <($MAKE -Bn objects 2>/dev/null | grep -oE -- '-o [^ ]+\.o ' | awk '{print $2}' | grep -E '^(libultraship|imgui)/')
[ "${#OBJS[@]}" -gt 0 ] || fail "no libultraship/imgui objects found in Makefile.vita's source list"

step "compiling ${#OBJS[@]} libultraship/imgui object file(s) (-j$JOBS)"
$MAKE "${OBJS[@]}" -j"$JOBS"

echo
step "OK: libultraship/ + imgui/ objects up to date (${#OBJS[@]} files)"
echo "Next: scripts/build-vpk.sh to link + package (after the other two project scripts, if any of them changed too)."
