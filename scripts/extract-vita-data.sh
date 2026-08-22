#!/usr/bin/env bash
# extract-vita-data.sh — produce the ROM-derived data files the Vita build
# needs at runtime but Makefile.vita has no step for (unlike the desktop
# CMake build, which generates them automatically via TorchExternal /
# derive_stage_assets.py during `cmake --build`).
#
# Two categories, both required for the game to actually run:
#
#   1. BattleShip.o2r — the main asset archive. Produced by a host-native
#      Torch build (torch/ submodule) via the same invocation
#      CMakeLists.txt's TorchExternal target uses for desktop builds:
#        torch o2r <rom> -s <repo-root> -d <dest>
#      Vita has no equivalent to the desktop first-run wizard's extraction
#      path (port/first_run.cpp spawns a *subprocess* "torch"/"torch.exe"
#      via std::system()/CreateProcess — there is no fork/exec on this
#      platform) or Android's statically-linked JNI bridge
#      (port/android_torch_bridge.cpp). So this one-time extraction has to
#      happen on the dev machine and be copied onto the memory card by hand.
#
#   2. CSS stage-icon assets (final_destination/metal_cavern/battlefield
#      background+icon+nameplate PNGs) — already scripted via
#      tools/derive_stage_assets.py (same tool `make -f Makefile.vita
#      css-icons-deploy` calls); this script just also runs it so both
#      categories land in one deploy tree together.
#
# Output: dist/vita-data-deploy/ux0/data/battleship/, matching the Vita
# app-data directory (Ship::Context::GetAppDirectoryPath() -> "ux0:data/
# battleship") layout exactly, plus a matching dist/vita-data-deploy.zip:
#   BattleShip.o2r
#   assets/css_icons/*.png
# Copy/unzip that onto the memory card at ux0:data/battleship/ (see the
# vitacompanion FTP loop in CLAUDE.md's PS Vita Port section).
#
# Usage:
#   scripts/extract-vita-data.sh <path/to/baserom.us.z64>
#
# Environment overrides:
#   TORCH_BUILD_DIR   host Torch CMake build dir (default: build-torch-host/,
#                      cached across runs — only configured/rebuilt if the
#                      torch binary isn't there yet; delete it to force a
#                      clean rebuild after changing torch/ or its options)
#   PYTHON3           python3 interpreter (default: python3)
#   JOBS              build parallelism for the host Torch build
#
# Not handled here: SSB64_ASSET_RECIPE_HASH compatibility checking
# (port/first_run.cpp's sidecar-hash mechanism, which auto-detects a stale
# o2r from a different pipeline version and re-extracts). If a rebuilt VPK
# suddenly can't read an o2r produced by an older run of this script,
# re-run it — mismatched recipe versions are the first thing to suspect.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

ROM="${1:-}"
if [[ -z "$ROM" ]]; then
	echo "usage: $0 <path/to/baserom.us.z64>" >&2
	exit 2
fi
[[ -f "$ROM" ]] || { echo "extract-vita-data: ROM not found: $ROM" >&2; exit 1; }
ROM="$(cd "$(dirname "$ROM")" && pwd)/$(basename "$ROM")"

TORCH_BUILD_DIR="${TORCH_BUILD_DIR:-$ROOT/build-torch-host}"
PYTHON3="${PYTHON3:-python3}"
JOBS="${JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)}"

DIST_DIR="$ROOT/dist"
STAGE_DIR="$DIST_DIR/vita-data-deploy"
UX0_ROOT="$STAGE_DIR/ux0/data/battleship"

step() { printf '\n\033[36m=== %s ===\033[0m\n' "$1"; }
fail() { printf '\033[31mERROR: %s\033[0m\n' "$1" >&2; exit 1; }

command -v cmake >/dev/null || fail "cmake not found in PATH"
command -v zip >/dev/null || fail "zip not found in PATH"
command -v "$PYTHON3" >/dev/null || fail "$PYTHON3 not found in PATH"

# ── 1. Build (or reuse) a host-native Torch ──────────────────────────────
TORCH_EXE="$TORCH_BUILD_DIR/torch"
if [[ ! -x "$TORCH_EXE" ]]; then
	step "Configuring host Torch build ($TORCH_BUILD_DIR)"
	cmake -S torch -B "$TORCH_BUILD_DIR" \
		-DCMAKE_BUILD_TYPE=Release \
		-DUSE_STANDALONE=ON \
		-DBUILD_STORMLIB=OFF \
		-DBUILD_SM64=OFF \
		-DBUILD_MK64=OFF \
		-DBUILD_SF64=OFF \
		-DBUILD_PM64=OFF \
		-DBUILD_FZERO=OFF \
		-DBUILD_MARIO_ARTIST=OFF \
		-DBUILD_NAUDIO=ON \
		-DBUILD_SSB64=ON

	step "Building host Torch (-j$JOBS)"
	cmake --build "$TORCH_BUILD_DIR" --target torch -j"$JOBS"
else
	step "Reusing existing host Torch build: $TORCH_EXE"
fi
[[ -x "$TORCH_EXE" ]] || fail "torch executable not found after build: $TORCH_EXE"

# ── 2. Extract BattleShip.o2r from the ROM ───────────────────────────────
step "Extracting BattleShip.o2r from $(basename "$ROM")"
mkdir -p "$UX0_ROOT"
"$TORCH_EXE" o2r "$ROM" -s "$ROOT" -d "$UX0_ROOT"
[[ -f "$UX0_ROOT/BattleShip.o2r" ]] || fail "torch o2r did not produce BattleShip.o2r"

# ── 3. Extract CSS stage-icon assets ──────────────────────────────────────
step "Extracting CSS stage-icon assets (final_destination/metal_cavern/battlefield)"
"$PYTHON3" tools/derive_stage_assets.py "$ROM" "$UX0_ROOT/assets/css_icons"

# ── 4. Package for transfer ───────────────────────────────────────────────
step "Packaging"
ZIP_OUT="$DIST_DIR/vita-data-deploy.zip"
rm -f "$ZIP_OUT"
(cd "$STAGE_DIR" && zip -r -q "$ZIP_OUT" ux0)

echo
step "done"
ls -lh "$UX0_ROOT/BattleShip.o2r"
png_count=$(find "$UX0_ROOT/assets/css_icons" -name '*.png' | wc -l | tr -d ' ')
echo "$png_count CSS icon PNG(s) in $UX0_ROOT/assets/css_icons"
echo
echo "Deploy tree: $STAGE_DIR"
echo "Zip:         $ZIP_OUT"
echo
echo "Copy onto the Vita (already laid out as ux0:data/battleship/...) — e.g. over"
echo "vitacompanion FTP, from this machine with the Vita's current IP:"
echo "  curl -T \"$UX0_ROOT/BattleShip.o2r\" ftp://\$VITA_IP:1337/ux0:/data/battleship/BattleShip.o2r"
echo "  (repeat per file under assets/css_icons/, or unzip \"$ZIP_OUT\" onto the memory card)"
