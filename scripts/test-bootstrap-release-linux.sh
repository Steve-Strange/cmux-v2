#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SCRIPT="$ROOT/scripts/bootstrap-release-linux.sh"
OPENGL_HOST="$ROOT/overlay/chrome/browser/cmux_term/cmux_ghostty_opengl_host.cc"
RESET_TEST_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/cmux-bootstrap-reset.XXXXXX")"
trap 'rm -rf "$RESET_TEST_ROOT"' EXIT

grep -Fq 'git clone --depth 1 --single-branch --branch "$PINNED_VERSION"' \
  "$SCRIPT"
grep -Fq '"$DEPOT_TOOLS/ensure_bootstrap"' "$SCRIPT"
grep -Fq 'gclient sync -D --no-history' "$SCRIPT"
grep -Fq '"revision": "$PINNED_COMMIT"' "$SCRIPT"
grep -Fq '"name": "src"' "$SCRIPT"
grep -Fq 'cd "$GCLIENT_ROOT"' "$SCRIPT"
grep -Fq '.cmux-linux-gclient-version' "$SCRIPT"
grep -Fq '[ ! -f "$SRC/build/config/gclient_args.gni" ]' "$SCRIPT"
grep -Fq 'git -C "$SRC" restore --source=HEAD --staged --worktree -- .' \
  "$SCRIPT"
restore_line="$(
  grep -nF 'git -C "$SRC" restore --source=HEAD --staged --worktree -- .' \
    "$SCRIPT" | cut -d: -f1
)"
gclient_line="$(grep -nF 'cat > "$GCLIENT_ROOT/.gclient"' "$SCRIPT" | cut -d: -f1)"
[ "$restore_line" -lt "$gclient_line" ]
grep -Fq 'install-build-deps.sh" --no-prompt' "$SCRIPT"
grep -Fq 'is_component_build = false' "$SCRIPT"
grep -Fq 'symbol_level = 1' "$SCRIPT"
grep -Fq 'use_debug_fission = false' "$SCRIPT"
grep -Fq 'target_cpu = "x64"' "$SCRIPT"
grep -Fq 'generate_about_credits = true' "$SCRIPT"
grep -Fq '.cmux-linux-bootstrap-version' "$SCRIPT"
grep -Fq 'GHOSTTY_BUILD_KEY=' "$SCRIPT"
grep -Fq \
  'GHOSTTY_ZIG_CACHE_DIR="$BUILD_ROOT/zig-cache/ghostty-library/$GHOSTTY_REVISION"' \
  "$SCRIPT"
grep -Fq \
  'CMUX_TUI_ZIG_CACHE_DIR="$BUILD_ROOT/zig-cache/cmux-tui/$CMUX_TUI_REVISION"' \
  "$SCRIPT"
grep -Fq 'CMUX_TUI_RESOURCE_PROFILE=terminfo-only' "$SCRIPT"
grep -Fq \
  'CMUX_TUI_RESOURCE_PROFILE="$CMUX_TUI_RESOURCE_PROFILE"' \
  "$SCRIPT"
grep -Fq \
  'contains an unverified Ghostty theme or shell-integration resource' \
  "$SCRIPT"
grep -Fq 'test -e "$CMUX_TUI_STAGE/terminfo/x/xterm-ghostty"' "$SCRIPT"
grep -Fq 'ZIG_GLOBAL_CACHE_DIR="$GHOSTTY_ZIG_CACHE_DIR"' "$SCRIPT"
grep -Fq 'ZIG_GLOBAL_CACHE_DIR="$CMUX_TUI_ZIG_CACHE_DIR"' "$SCRIPT"
if grep -Fq \
  'ZIG_GLOBAL_CACHE_DIR="$BUILD_ROOT/zig-cache/$GHOSTTY_REVISION"' \
  "$SCRIPT"; then
  echo "Ghostty and cmux-tui must not share the legacy Zig cache" >&2
  exit 1
fi
grep -Fq 'prepare-ghostty-linux-pic.py' "$SCRIPT"
grep -Fq 'zig build' "$SCRIPT"
grep -Fq 'vendor-ghostty-linux-local.sh' "$SCRIPT"
grep -Fq 'collect-ghostty-licenses.py' \
  "$ROOT/scripts/vendor-ghostty-linux-local.sh"
grep -Fq '#include "base/memory/raw_ptr.h"' "$OPENGL_HOST"
grep -Fq 'reinterpret_cast<void*>(' "$OPENGL_HOST"
grep -Fq 'EGL_DEFAULT_DISPLAY)' "$OPENGL_HOST"
grep -Fq 'raw_ptr<void> lib_gl_ = nullptr;' "$OPENGL_HOST"
python3 "$ROOT/scripts/test-apply-ghostty-link.py"

RESET_REPO="$RESET_TEST_ROOT/chromium"
mkdir -p "$RESET_REPO/chrome" "$RESET_REPO/out/Release"
git -C "$RESET_REPO" init -q
git -C "$RESET_REPO" config user.email release-test@example.invalid
git -C "$RESET_REPO" config user.name "Release Test"
printf 'MAJOR=151\nMINOR=0\nBUILD=7922\nPATCH=34\n' \
  > "$RESET_REPO/chrome/VERSION"
printf 'baseline\n' > "$RESET_REPO/tracked-file"
git -C "$RESET_REPO" add chrome/VERSION tracked-file
git -C "$RESET_REPO" commit -qm baseline
printf 'MAJOR=151\nMINOR=0\nBUILD=7922\nPATCH=47\n' \
  > "$RESET_REPO/chrome/VERSION"
printf 'nightly overlay\n' > "$RESET_REPO/tracked-file"
git -C "$RESET_REPO" add chrome/VERSION
printf 'warm object\n' > "$RESET_REPO/out/Release/cache-marker"
printf 'untracked dependency\n' > "$RESET_REPO/untracked-cache"

git -C "$RESET_REPO" restore --source=HEAD --staged --worktree -- .
git -C "$RESET_REPO" restore --source=HEAD --staged --worktree -- .
git -C "$RESET_REPO" diff --quiet
git -C "$RESET_REPO" diff --cached --quiet
grep -Fxq 'PATCH=34' "$RESET_REPO/chrome/VERSION"
grep -Fxq 'baseline' "$RESET_REPO/tracked-file"
grep -Fxq 'warm object' "$RESET_REPO/out/Release/cache-marker"
grep -Fxq 'untracked dependency' "$RESET_REPO/untracked-cache"

echo "Linux release bootstrap contract tests passed"
