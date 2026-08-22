#!/usr/bin/env bash
# Zips up the committed source tree (this repo + the decomp/libultraship/torch
# submodules) with no compiled/build output.
#
# Output: <repo-root>/dist/BattleShip-source-<version>-<short-sha>.zip
#
# Uses `git archive` per repo (outer + each submodule) instead of hand-rolled
# exclude patterns: it only ever emits tracked, committed files, so build/,
# *.o, the VPK/ELF/AppImage outputs, and any local ROM (baserom.*, never
# tracked - see CLAUDE.md's PS Vita Port section) are excluded for free,
# along with every .git/ directory. It also means uncommitted local changes
# are NOT included - this packages HEAD, not the working tree. Run with
# --dirty to snapshot the working tree instead (useful mid-investigation,
# e.g. to hand a WIP tree to another machine without committing).
#
# Usage:
#   scripts/package-source.sh            # archive HEAD (default)
#   scripts/package-source.sh --dirty    # archive the working tree as-is

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

DIRTY=0
if [[ "${1:-}" == "--dirty" ]]; then
	DIRTY=1
fi

DIST_DIR="$ROOT/dist"
VERSION="$(git describe --tags --abbrev=0 2>/dev/null || echo dev)"
SHA="$(git rev-parse --short HEAD)"
OUT="$DIST_DIR/BattleShip-source-${VERSION}-${SHA}.zip"
STAGING="$(mktemp -d)"

step() { printf '\n\033[36m=== %s ===\033[0m\n' "$1"; }
fail() { printf '\033[31mERROR: %s\033[0m\n' "$1" >&2; exit 1; }
cleanup() { rm -rf "$STAGING"; }
trap cleanup EXIT

command -v zip >/dev/null || fail "zip not found in PATH"

archive_into_staging() {
	# $1 = repo dir (relative to $ROOT, "." for the outer repo), $2 = prefix to
	# extract under (empty for the outer repo).
	local repo_dir="$1" prefix="$2"
	local dest="$STAGING/${prefix}"
	mkdir -p "$dest"
	if [[ "$DIRTY" -eq 1 ]]; then
		# `git archive` only knows about committed content, so for --dirty we
		# stage the tracked files as they currently sit in the working tree
		# (still tracked files only - untracked build output/ROMs stay out).
		(cd "$ROOT/$repo_dir" && git ls-files -z) \
			| rsync -a --files-from=- --from0 "$ROOT/$repo_dir/" "$dest/" \
			|| fail "failed to stage dirty working tree for $repo_dir"
	else
		git -C "$ROOT/$repo_dir" archive --format=tar --prefix="${prefix}" HEAD \
			| tar -x -C "$STAGING" \
			|| fail "git archive failed for $repo_dir"
	fi
}

mkdir -p "$DIST_DIR"

step "Archiving outer repo ($([[ $DIRTY -eq 1 ]] && echo 'working tree' || echo HEAD))"
archive_into_staging "." ""

for sub in decomp libultraship torch; do
	[[ -d "$ROOT/$sub" ]] || fail "submodule dir missing: $sub (run git submodule update --init)"
	step "Archiving submodule: $sub"
	archive_into_staging "$sub" "$sub/"
done

step "Zipping"
rm -f "$OUT"
(cd "$STAGING" && zip -r -q "$OUT" .)

echo
step "done"
ls -lh "$OUT"
echo "$(unzip -l "$OUT" | tail -1 | awk '{print $2}') files packaged"
