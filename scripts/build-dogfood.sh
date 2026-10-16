#!/usr/bin/env bash
# Build and download a tagged cmux-browser from an isolated AWS M4 Pro
# workspace. Different tags use independent APFS clones, so builds can run in
# parallel without syncing or linking over each other.
set -euo pipefail

TAG="${1:-}"
MODE="${2:-}"
if [[ -z "$TAG" || ! "$TAG" =~ ^[A-Za-z0-9._-]+$ ]]; then
  echo "usage: scripts/build-dogfood.sh <tag> [--shared]" >&2
  echo "tag may contain letters, numbers, dot, underscore, and hyphen" >&2
  exit 2
fi
if [[ -n "$MODE" && "$MODE" != "--shared" ]]; then
  echo "unknown option: $MODE" >&2
  exit 2
fi

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/scripts/builder-transport.sh"

HOST="${CMUX_BUILDER:-ec2-user@aws-m4pro-1}"
BASE="${CMUX_CHROMIUM_BASE:-/Volumes/recpass123/cmux-ci/cmux-browser-benchmark}"
WORKSPACES="${CMUX_CHROMIUM_WORKSPACES:-/Volumes/recpass123/cmux-ci/cmux-browser-workspaces}"
REMOTE_ROOT="$WORKSPACES/$TAG"
REMOTE_SRC="$REMOTE_ROOT/src"
DEST="${DEST:-$HOME/Applications/cmux-browser-$TAG.app}"
BUNDLE_TAG="$(printf '%s' "$TAG" | tr '[:upper:]' '[:lower:]' | tr '._' '--')"
DOGFOOD_BUNDLE_ID="${CMUX_DOGFOOD_BUNDLE_ID:-com.cmux.app.dogfood.build-$BUNDLE_TAG}"
DOGFOOD_PRODUCT_DIR="${CMUX_DOGFOOD_PRODUCT_DIR_NAME:-cmux-browser-dogfood-$TAG}"

if [ "$MODE" = "--shared" ]; then
  REMOTE_ROOT="$BASE"
  REMOTE_SRC="$BASE/src"
  echo "== lock shared M4 Pro workspace: $REMOTE_ROOT =="
  if ! ssh "$HOST" mkdir "$REMOTE_ROOT/.cmux-build-lock"; then
    echo "shared workspace is already building: $REMOTE_ROOT" >&2
    exit 74
  fi
else
  echo "== prepare isolated M4 Pro workspace: $REMOTE_ROOT =="
prepare_script='set -euo pipefail
base="$1"
target="$2"
mkdir -p "$(dirname "$target")"
if [ ! -d "$target/src" ]; then
  prepare_lock="$target.prepare-lock"
  if ! mkdir "$prepare_lock" 2>/dev/null; then
    echo "workspace is already being prepared: $target" >&2
    exit 73
  fi
  trap '\''rmdir "$prepare_lock" 2>/dev/null || true'\'' EXIT
  tmp="$target.clone.$$"
  rm -rf "$tmp"
  # Preserve mtimes so Ninja can reuse the cloned output cache. `ditto --clone`
  # keeps metadata while using APFS copy-on-write blocks.
  ditto --clone "$base" "$tmp"
  # The canonical workspace may be actively locked while its snapshot is
  # cloned. That lock belongs to the canonical build, not this new workspace.
  rm -rf "$tmp/.cmux-build-lock"
  mv "$tmp" "$target"
fi
if ! mkdir "$target/.cmux-build-lock" 2>/dev/null; then
  echo "tag is already building: $target" >&2
  exit 74
fi'
  ssh "$HOST" /bin/bash -c "$prepare_script" -- "$BASE" "$REMOTE_ROOT"
fi

release_lock() {
  ssh "$HOST" rmdir "$REMOTE_ROOT/.cmux-build-lock" >/dev/null 2>&1 || true
}
trap release_lock EXIT

# The cached builder checkout is intentionally reused across builds, so its
# tracked Chromium files contain the previous overlay's edits. Reset those
# files before applying this branch. Build outputs and untracked overlay files
# remain in place, preserving the incremental cache; sync.sh refreshes the
# cmux-owned source directory with --delete.
echo "== reset tracked Chromium files: $REMOTE_SRC =="
ssh "$HOST" "cd '$REMOTE_SRC' && git restore --worktree -- ."

export CMUX_BUILDER="$HOST"
export CHROMIUM_SRC="$REMOTE_SRC"
# HQ keeps one durable depot_tools checkout beside the Chromium base so every
# APFS-cloned workspace can share it. Preserve an explicitly supplied path;
# legacy standalone builders still use the workspace-local checkout.
export CMUX_DEPOT_TOOLS="${CMUX_DEPOT_TOOLS:-$REMOTE_ROOT/depot_tools}"
export CMUX_MAC_BUNDLE_ID="$DOGFOOD_BUNDLE_ID"

"$ROOT/scripts/sync.sh"
"$ROOT/scripts/apply.sh"
"$ROOT/scripts/build.sh"

mkdir -p "$(dirname "$DEST")"
# The bundle ID is compiled into Chromium. CrProductDirName supplies isolated
# ProcessSingleton/profile state without wrapping or renaming the executable.
CMUX_TAG="$TAG" \
  CMUX_EXPECTED_BUNDLE_ID="$DOGFOOD_BUNDLE_ID" \
  CMUX_PRODUCT_DIR_NAME="$DOGFOOD_PRODUCT_DIR" \
  DEST="$DEST" \
  "$ROOT/scripts/deploy.sh"

echo "CMUX_DOGFOOD_OK"
echo "App path: $DEST"
