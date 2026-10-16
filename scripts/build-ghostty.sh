#!/bin/bash
# Build our PATCHED libghostty (the full macOS embedding lib WITH the Metal
# renderer — "GhosttyKit", not libghostty-vt) from the local fork, assemble a
# GHOSTTY_KIT-shaped directory, and stage it on the builder so scripts/apply.sh
# vendors it into the Chromium tree.
#
# Why a fork: cmux-browser and cmux-tui share an exact manaflow Ghostty revision
# that adds manual-mirror IO. macOS uses Ghostty's supported native NSView/Metal
# surface; Linux and Windows use the published host-owned OpenGL callbacks.
#
# Builds LOCALLY (the macbook has zig 0.15.2 + Xcode; the builder has no zig),
# then rsyncs the artifact to the builder.
#
#   bash scripts/build-ghostty.sh
#   GHOSTTY_SRC=~/fun/ghostty-wt-cursor-visual-api bash scripts/build-ghostty.sh
#
# After this, run: GHOSTTY_KIT=<printed path> ./scripts/apply.sh
set -euo pipefail
. "$(dirname "$0")/builder-transport.sh"

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
EXPECTED_GHOSTTY_REVISION="$(tr -d '[:space:]' < "$ROOT/ghostty-revision.txt")"
GHOSTTY_SRC="${GHOSTTY_SRC:-$HOME/fun/ghostty-wt-cursor-visual-api}"
HOST="${CMUX_BUILDER:-ec2-user@aws-m4pro-1}"
SRC="${CHROMIUM_SRC:-/Volumes/recpass123/cmux-ci/cmux-browser-benchmark/src}"
# Where the staged kit lands on the builder (apply.sh reads from here).
REMOTE_KIT="${REMOTE_KIT:-cmux-ghostty-kits/$EXPECTED_GHOSTTY_REVISION-macos-arm64}"

case "$REMOTE_KIT" in
  ''|/*|*..*|*//*|*[!A-Za-z0-9._/-]*)
    echo "ERROR: REMOTE_KIT must be a safe relative path below the builder home" >&2
    exit 1
    ;;
esac

cd "$GHOSTTY_SRC"

ACTUAL_GHOSTTY_REVISION="$(git rev-parse HEAD)"
if [ "$ACTUAL_GHOSTTY_REVISION" != "$EXPECTED_GHOSTTY_REVISION" ]; then
  echo "ERROR: Ghostty checkout is $ACTUAL_GHOSTTY_REVISION; browser pin is $EXPECTED_GHOSTTY_REVISION" >&2
  exit 1
fi
if [ -n "$(git status --porcelain --untracked-files=normal)" ]; then
  echo "ERROR: GHOSTTY_SRC must be clean so the revision identifies the artifact exactly" >&2
  exit 1
fi
case "$(git remote get-url origin)" in
  https://github.com/manaflow-ai/ghostty.git|git@github.com:manaflow-ai/ghostty.git) ;;
  *) echo "ERROR: GHOSTTY_SRC origin must be manaflow-ai/ghostty" >&2; exit 1 ;;
esac

# Xcode 26 ships without the Metal shader compiler by default; Ghostty needs it.
if ! xcrun -f metal >/dev/null 2>&1; then
  echo "Metal toolchain missing; downloading (one-time, ~700MB)..."
  xcodebuild -downloadComponent MetalToolchain
fi

echo "building pinned native GhosttyKit..."
zig build \
  -Dapp-runtime=none \
  -Demit-xcframework=true \
  -Dxcframework-target=native \
  -Demit-macos-app=false \
  -Demit-docs=false \
  -Di18n=false

BUILT_KIT="macos/GhosttyKit.xcframework/macos-arm64"
FAT="$BUILT_KIT/libghostty-internal-fat.a"
HEADER="$BUILT_KIT/Headers/ghostty.h"
if [ ! -f "$FAT" ] || [ ! -f "$HEADER" ]; then
  echo "ERROR: native GhosttyKit was not produced at $BUILT_KIT" >&2
  exit 1
fi
case " $(lipo -archs "$FAT") " in
  *" arm64 "*) ;;
  *) echo "ERROR: GhosttyKit does not contain arm64" >&2; exit 1 ;;
esac
if [ "$(nm "$FAT" 2>/dev/null | grep -c "_ghostty_surface_process_output" || true)" = "0" ]; then
  echo "ERROR: GhosttyKit lacks ghostty_surface_process_output" >&2
  exit 1
fi
for symbol in _ghostty_surface_set_grid_size \
              _ghostty_surface_set_external_frame_context \
              _ghostty_surface_release_external_frame; do
  if [ "$(nm "$FAT" 2>/dev/null | grep -c "$symbol" || true)" = "0" ]; then
    echo "ERROR: GhosttyKit lacks $symbol" >&2
    exit 1
  fi
done
if ! grep -q "GHOSTTY_SURFACE_IO_MANUAL_MIRROR" "$HEADER" ||
   ! grep -q "GHOSTTY_PLATFORM_OPENGL" "$HEADER" ||
   ! grep -q "GHOSTTY_PLATFORM_METAL_EXTERNAL_LEASED" "$HEADER"; then
  echo "ERROR: GhosttyKit header lacks the pinned embedded contract" >&2
  exit 1
fi
echo "using $FAT"

# Assemble a GHOSTTY_KIT-shaped dir (matches scripts/apply.sh expectations):
#   <kit>/libghostty-internal-fat.a  and  <kit>/Headers/ghostty.h
KIT=$(mktemp -d /tmp/cmux-ghostty-kit.XXXXXX)
mkdir -p "$KIT/Headers"
cp "$FAT" "$KIT/libghostty-internal-fat.a"
cp "$HEADER" "$KIT/Headers/ghostty.h"
cp "$ROOT/ghostty-revision.txt" "$KIT/REVISION"

echo "staging kit -> $HOST:~/$REMOTE_KIT/ ..."
ssh -o BatchMode=yes "$HOST" "rm -rf ~/$REMOTE_KIT && mkdir -p ~/$REMOTE_KIT/Headers"
rsync -az -e "$CMUX_TAILSCALE_SSH" "$KIT/" "$HOST:$REMOTE_KIT/"

# Force apply.sh to re-vendor (it skips if the in-tree copy already exists).
ssh -o BatchMode=yes "$HOST" "cd '$SRC' && rm -f third_party/cmux_ghostty/lib/ghostty-internal.a third_party/cmux_ghostty/include/ghostty.h third_party/cmux_ghostty/REVISION"

rm -rf "$KIT"
REMOTE_HOME=$(ssh -o BatchMode=yes "$HOST" 'echo $HOME')
echo
echo "done. Now vendor + build with:"
echo "  GHOSTTY_KIT=$REMOTE_HOME/$REMOTE_KIT ./scripts/apply.sh"
