#!/usr/bin/env bash
# Prepare a revision-keyed Blacksmith sticky disk for Linux release builds.
# The disk is disposable build infrastructure; Git remains the source of truth.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_ROOT="${CMUX_LINUX_BUILD_ROOT:-/opt/cmux}"
SRC="${CHROMIUM_SRC:-$BUILD_ROOT/chromium/src}"
GCLIENT_ROOT="$(dirname "$SRC")"
DEPOT_TOOLS="${CMUX_DEPOT_TOOLS:-$BUILD_ROOT/depot_tools}"
PINNED_VERSION="$(tr -d '[:space:]' < "$ROOT/.chromium-version")"
PINNED_COMMIT="$(
  sed -n 's/^- Current Browser base: `\([0-9a-f]\{40\}\)`$/\1/p' \
    "$ROOT/THIRD_PARTY_NOTICES.md"
)"
GHOSTTY_REVISION="$(tr -d '[:space:]' < "$ROOT/ghostty-revision.txt")"
GHOSTTY_SRC="${GHOSTTY_SRC:-$BUILD_ROOT/ghostty/$GHOSTTY_REVISION}"
GHOSTTY_ZIG_CACHE_DIR="$BUILD_ROOT/zig-cache/ghostty-library/$GHOSTTY_REVISION"
CMUX_TUI_REVISION="$(
  sed -n '/kPinnedCmuxTuiBuildCommit/{n;s/^    "\([0-9a-f]*\)";$/\1/p;}' \
    "$ROOT/overlay/chrome/browser/cmux_term/cmux_tui_revision.h"
)"
CMUX_TUI_SRC="${CMUX_TUI_SRC:-$BUILD_ROOT/cmux/$CMUX_TUI_REVISION}"
CMUX_TUI_STAGE="${CMUX_TUI_STAGE:-$BUILD_ROOT/cmux-tui-stage/$CMUX_TUI_REVISION}"
CMUX_TUI_ZIG_CACHE_DIR="$BUILD_ROOT/zig-cache/cmux-tui/$CMUX_TUI_REVISION"
CMUX_TUI_RESOURCE_PROFILE=terminfo-only
BOOTSTRAP_RECEIPT="$BUILD_ROOT/.cmux-linux-bootstrap-version"
BOOTSTRAP_KEY="$PINNED_VERSION:linux-release-v2"
GCLIENT_RECEIPT="$BUILD_ROOT/.cmux-linux-gclient-version"
GCLIENT_KEY="$PINNED_COMMIT:gclient-v1"
GHOSTTY_RECEIPT="$BUILD_ROOT/.cmux-linux-ghostty-version"
CMUX_TUI_RECEIPT="$BUILD_ROOT/.cmux-linux-cmux-tui-version"

case "$SRC" in
  "$BUILD_ROOT"/chromium/src) ;;
  *)
    echo "error: CHROMIUM_SRC must be inside CMUX_LINUX_BUILD_ROOT" >&2
    exit 2
    ;;
esac
case "$DEPOT_TOOLS" in
  "$BUILD_ROOT"/depot_tools) ;;
  *)
    echo "error: CMUX_DEPOT_TOOLS must be inside CMUX_LINUX_BUILD_ROOT" >&2
    exit 2
    ;;
esac
case "$GHOSTTY_SRC" in
  "$BUILD_ROOT"/ghostty/"$GHOSTTY_REVISION") ;;
  *)
    echo "error: GHOSTTY_SRC must be revision-keyed inside CMUX_LINUX_BUILD_ROOT" >&2
    exit 2
    ;;
esac
case "$CMUX_TUI_SRC" in
  "$BUILD_ROOT"/cmux/"$CMUX_TUI_REVISION") ;;
  *)
    echo "error: CMUX_TUI_SRC must be revision-keyed inside CMUX_LINUX_BUILD_ROOT" >&2
    exit 2
    ;;
esac
case "$CMUX_TUI_STAGE" in
  "$BUILD_ROOT"/cmux-tui-stage/"$CMUX_TUI_REVISION") ;;
  *)
    echo "error: CMUX_TUI_STAGE must be revision-keyed inside CMUX_LINUX_BUILD_ROOT" >&2
    exit 2
    ;;
esac
[ -n "$PINNED_COMMIT" ] || {
  echo "error: could not resolve the pinned Chromium commit" >&2
  exit 2
}
[[ "$CMUX_TUI_REVISION" =~ ^[0-9a-f]{40}$ ]] || {
  echo "error: could not resolve the pinned cmux-tui commit" >&2
  exit 2
}
command -v zig >/dev/null || {
  echo "error: Zig 0.15.2 is required for the pinned Ghostty library" >&2
  exit 2
}
[ "$(zig version)" = "0.15.2" ] || {
  echo "error: expected Zig 0.15.2; found $(zig version)" >&2
  exit 2
}
command -v rustc >/dev/null || {
  echo "error: Rust 1.96.0 is required for cmux-tui" >&2
  exit 2
}
RUST_COMPILER="$(rustc --version)"
RUST_VERSION="$(awk '{print $2}' <<<"$RUST_COMPILER")"
[ "$RUST_VERSION" = "1.96.0" ] || {
  echo "error: expected Rust 1.96.0; found $RUST_COMPILER" >&2
  exit 2
}

mkdir -p "$BUILD_ROOT"
if [ ! -d "$DEPOT_TOOLS/.git" ]; then
  git clone --depth 1 \
    https://chromium.googlesource.com/chromium/tools/depot_tools.git \
    "$DEPOT_TOOLS"
fi
export PATH="$DEPOT_TOOLS:$PATH"
export DEPOT_TOOLS_UPDATE=0
"$DEPOT_TOOLS/ensure_bootstrap"

if [ ! -d "$SRC/.git" ]; then
  mkdir -p "$(dirname "$SRC")"
  git clone --depth 1 --single-branch --branch "$PINNED_VERSION" \
    https://chromium.googlesource.com/chromium/src.git "$SRC"
fi

# Successful nightlies leave tracked release stamps and overlay changes on the
# sticky disk. Reset Git-owned state to the pinned commit while preserving the
# ignored/untracked build outputs that make the disk useful as a warm cache.
git -C "$SRC" restore --source=HEAD --staged --worktree -- .

cat > "$GCLIENT_ROOT/.gclient" <<GCLIENT
solutions = [
  {
    "name": "src",
    "url": "https://chromium.googlesource.com/chromium/src.git",
    "revision": "$PINNED_COMMIT",
    "custom_deps": {},
    "custom_vars": {},
  },
]
GCLIENT
if [ "$(cat "$GCLIENT_RECEIPT" 2>/dev/null || true)" != "$GCLIENT_KEY" ] ||
    [ ! -f "$SRC/build/config/gclient_args.gni" ]; then
  (
    cd "$GCLIENT_ROOT"
    gclient sync -D --no-history
  )
  printf '%s\n' "$GCLIENT_KEY" > "$GCLIENT_RECEIPT"
fi

ACTUAL_VERSION="$(
  awk -F= '
    $1 == "MAJOR" { major=$2 }
    $1 == "MINOR" { minor=$2 }
    $1 == "BUILD" { build=$2 }
    $1 == "PATCH" { patch=$2 }
    END { print major "." minor "." build "." patch }
  ' "$SRC/chrome/VERSION"
)"
if [ "$ACTUAL_VERSION" != "$PINNED_VERSION" ]; then
  echo "error: sticky Chromium tree is $ACTUAL_VERSION; expected $PINNED_VERSION" >&2
  exit 1
fi
ACTUAL_COMMIT="$(git -C "$SRC" rev-parse HEAD)"
if [ "$ACTUAL_COMMIT" != "$PINNED_COMMIT" ]; then
  echo "error: sticky Chromium commit is $ACTUAL_COMMIT; expected $PINNED_COMMIT" >&2
  exit 1
fi

if [ "$(cat "$BOOTSTRAP_RECEIPT" 2>/dev/null || true)" != "$BOOTSTRAP_KEY" ]; then
  sudo "$SRC/build/install-build-deps.sh" --no-prompt
  mkdir -p "$SRC/out/Release"
  cat > "$SRC/out/Release/args.gn" <<'GN'
is_component_build = false
is_debug = false
symbol_level = 1
blink_symbol_level = 0
v8_symbol_level = 0
target_cpu = "x64"
use_debug_fission = false
use_remoteexec = false
use_thin_lto = false
generate_about_credits = true
GN
  (
    cd "$SRC"
    gn gen out/Release
  )
  printf '%s\n' "$BOOTSTRAP_KEY" > "$BOOTSTRAP_RECEIPT"
fi

if [ ! -d "$GHOSTTY_SRC/.git" ]; then
  mkdir -p "$(dirname "$GHOSTTY_SRC")"
  git init "$GHOSTTY_SRC"
  git -C "$GHOSTTY_SRC" remote add origin \
    https://github.com/manaflow-ai/ghostty.git
  git -C "$GHOSTTY_SRC" fetch --depth 1 origin "$GHOSTTY_REVISION"
  git -C "$GHOSTTY_SRC" checkout --detach FETCH_HEAD
fi
case "$(git -C "$GHOSTTY_SRC" remote get-url origin)" in
  https://github.com/manaflow-ai/ghostty.git) ;;
  *) echo "error: sticky Ghostty origin is not manaflow-ai/ghostty" >&2; exit 1 ;;
esac
ACTUAL_GHOSTTY_REVISION="$(git -C "$GHOSTTY_SRC" rev-parse HEAD)"
if [ "$ACTUAL_GHOSTTY_REVISION" != "$GHOSTTY_REVISION" ]; then
  echo "error: sticky Ghostty tree is $ACTUAL_GHOSTTY_REVISION; expected $GHOSTTY_REVISION" >&2
  exit 1
fi
python3 "$ROOT/scripts/prepare-ghostty-linux-pic.py" \
  --source "$GHOSTTY_SRC" \
  --revision "$GHOSTTY_REVISION"

GHOSTTY_BUILD_KEY="$GHOSTTY_REVISION:zig-$(zig version):linux-x64-release-v3"
if [ "$(cat "$GHOSTTY_RECEIPT" 2>/dev/null || true)" != "$GHOSTTY_BUILD_KEY" ] ||
    [ ! -s "$GHOSTTY_SRC/zig-out/lib/ghostty-internal.a" ] ||
    [ ! -d "$GHOSTTY_ZIG_CACHE_DIR/p" ]; then
  (
    cd "$GHOSTTY_SRC"
    export ZIG_GLOBAL_CACHE_DIR="$GHOSTTY_ZIG_CACHE_DIR"
    zig build \
      -Dtarget=x86_64-linux \
      -Dapp-runtime=none \
      -Doptimize=ReleaseFast \
      -Di18n=false \
      -Demit-docs=false \
      -Demit-terminfo=false \
      -Demit-termcap=false \
      -Demit-themes=false
  )
  printf '%s\n' "$GHOSTTY_BUILD_KEY" > "$GHOSTTY_RECEIPT"
fi

# License collection intentionally walks the complete package cache. Keep this
# cache exclusive to the pinned libghostty build: cmux-tui's later Cargo and
# resource builds resolve additional Zig packages that are not libghostty
# dependencies and must not contaminate its exact compliance inventory.
GHOSTTY_SRC="$GHOSTTY_SRC" \
ZIG_GLOBAL_CACHE_DIR="$GHOSTTY_ZIG_CACHE_DIR" \
CHROMIUM_SRC="$SRC" \
  "$ROOT/scripts/vendor-ghostty-linux-local.sh"

if [ ! -d "$CMUX_TUI_SRC/.git" ]; then
  mkdir -p "$(dirname "$CMUX_TUI_SRC")"
  git init "$CMUX_TUI_SRC"
  git -C "$CMUX_TUI_SRC" remote add origin \
    https://github.com/manaflow-ai/cmux.git
  git -C "$CMUX_TUI_SRC" fetch --depth 1 origin "$CMUX_TUI_REVISION"
  git -C "$CMUX_TUI_SRC" checkout --detach FETCH_HEAD
fi
case "$(git -C "$CMUX_TUI_SRC" remote get-url origin)" in
  https://github.com/manaflow-ai/cmux.git) ;;
  *) echo "error: sticky cmux origin is not manaflow-ai/cmux" >&2; exit 1 ;;
esac
ACTUAL_CMUX_TUI_REVISION="$(git -C "$CMUX_TUI_SRC" rev-parse HEAD)"
if [ "$ACTUAL_CMUX_TUI_REVISION" != "$CMUX_TUI_REVISION" ]; then
  echo "error: sticky cmux tree is $ACTUAL_CMUX_TUI_REVISION; expected $CMUX_TUI_REVISION" >&2
  exit 1
fi
git -C "$CMUX_TUI_SRC" submodule update --init --depth 1 ghostty
ACTUAL_CMUX_TUI_GHOSTTY_REVISION="$(
  git -C "$CMUX_TUI_SRC/ghostty" rev-parse HEAD
)"
if [ "$ACTUAL_CMUX_TUI_GHOSTTY_REVISION" != "$GHOSTTY_REVISION" ]; then
  echo "error: cmux-tui Ghostty is $ACTUAL_CMUX_TUI_GHOSTTY_REVISION; expected $GHOSTTY_REVISION" >&2
  exit 1
fi

CMUX_TUI_SCRIPT_DIGEST="$(
  sha256sum "$ROOT/scripts/build-cmux-tui.sh" \
    "$ROOT/scripts/install-cmux-tui-artifact.sh" |
    sha256sum |
    awk '{print $1}'
)"
CMUX_TUI_BUILD_KEY="$CMUX_TUI_REVISION:$GHOSTTY_REVISION:$RUST_COMPILER:zig-$(zig version):$CMUX_TUI_RESOURCE_PROFILE:$CMUX_TUI_SCRIPT_DIGEST"
if [ "$(cat "$CMUX_TUI_RECEIPT" 2>/dev/null || true)" != \
     "$CMUX_TUI_BUILD_KEY" ] ||
   [ ! -x "$CMUX_TUI_STAGE/cmux-tui" ] ||
   [ ! -s "$CMUX_TUI_STAGE/GHOSTTY_RESOURCES_MANIFEST.sha256" ]; then
  ZIG_GLOBAL_CACHE_DIR="$CMUX_TUI_ZIG_CACHE_DIR" \
  CMUX_SRC="$CMUX_TUI_SRC" \
  CMUX_TUI_STAGE="$CMUX_TUI_STAGE" \
  CMUX_TUI_TARGET=x86_64-unknown-linux-gnu \
  CMUX_TUI_RESOURCE_PROFILE="$CMUX_TUI_RESOURCE_PROFILE" \
    "$ROOT/scripts/build-cmux-tui.sh"
  printf '%s\n' "$CMUX_TUI_BUILD_KEY" > "$CMUX_TUI_RECEIPT"
fi

for directory in "$CMUX_TUI_STAGE/ghostty/themes" \
                 "$CMUX_TUI_STAGE/ghostty/shell-integration"; do
  test -d "$directory"
done
UNEXPECTED_UNLICENSED_RESOURCE="$(
  find "$CMUX_TUI_STAGE/ghostty/themes" \
       "$CMUX_TUI_STAGE/ghostty/shell-integration" \
    -mindepth 1 -print -quit
)"
if [ -n "$UNEXPECTED_UNLICENSED_RESOURCE" ]; then
  echo "error: Linux cmux-tui stage contains an unverified Ghostty theme or shell-integration resource: $UNEXPECTED_UNLICENSED_RESOURCE" >&2
  exit 1
fi
test -x "$DEPOT_TOOLS/autoninja"
test -f "$SRC/out/Release/build.ninja"
test -s "$SRC/third_party/cmux_ghostty/lib/ghostty-internal.a"
test -s "$SRC/third_party/cmux_ghostty/licenses/SOURCE-MANIFEST.json"
test -x "$CMUX_TUI_STAGE/cmux-tui"
test -s "$CMUX_TUI_STAGE/GHOSTTY_RESOURCES_MANIFEST.sha256"
test -e "$CMUX_TUI_STAGE/terminfo/x/xterm-ghostty"
echo "Linux release checkout ready: Chromium $PINNED_VERSION at $SRC"
