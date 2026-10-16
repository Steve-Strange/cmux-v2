#!/bin/bash
# Build and stage a revision-stamped cmux-tui binary from the dedicated cmux
# worktree. The staged manifest records both the cmux commit and the exact
# manaflow Ghostty source used by cmux-tui's libghostty-vt build.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CMUX_SRC="${CMUX_SRC:-$HOME/fun/cmuxterm-hq/worktrees/codex-cmux-tui-viewer-lease-release}"
REVISION_HEADER="$ROOT/overlay/chrome/browser/cmux_term/cmux_tui_revision.h"
EXPECTED_CMUX_REVISION="$(sed -n '/kPinnedCmuxTuiBuildCommit/{n;s/^    "\([0-9a-f]*\)";$/\1/p;}' "$REVISION_HEADER")"
EXPECTED_GHOSTTY_REVISION="$(tr -d '[:space:]' < "$ROOT/ghostty-revision.txt")"
HEADER_GHOSTTY_REVISION="$(sed -n '/kPinnedCmuxTuiGhosttyCommit/{n;s/^    "\([0-9a-f]*\)";$/\1/p;}' "$REVISION_HEADER")"

case "$EXPECTED_CMUX_REVISION" in
  *[!0-9a-f]*|'')
    echo "ERROR: cmux TUI pin is not a lowercase hexadecimal commit" >&2
    exit 1
    ;;
esac
if [ "${#EXPECTED_CMUX_REVISION}" -ne 40 ]; then
  echo "ERROR: cmux TUI pin must be a full 40-character commit" >&2
  exit 1
fi
if [ "$HEADER_GHOSTTY_REVISION" != "$EXPECTED_GHOSTTY_REVISION" ]; then
  echo "ERROR: runtime Ghostty pin differs from ghostty-revision.txt" >&2
  exit 1
fi

ACTUAL_CMUX_REVISION="$(git -C "$CMUX_SRC" rev-parse HEAD)"
if [ "$ACTUAL_CMUX_REVISION" != "$EXPECTED_CMUX_REVISION" ]; then
  echo "ERROR: cmux checkout is $ACTUAL_CMUX_REVISION; browser pin is $EXPECTED_CMUX_REVISION" >&2
  exit 1
fi
# Worktrees used for stacked development can retain a local integration repo
# as `origin`; require that the exact source commit is backed by at least one
# canonical manaflow-ai/cmux remote without assigning special meaning to the
# remote's local name.
CMUX_CANONICAL_REMOTE=""
while IFS= read -r remote; do
  case "$(git -C "$CMUX_SRC" remote get-url "$remote")" in
    https://github.com/manaflow-ai/cmux.git|git@github.com:manaflow-ai/cmux.git)
      CMUX_CANONICAL_REMOTE="$remote"
      break
      ;;
  esac
done < <(git -C "$CMUX_SRC" remote)
if [ -z "$CMUX_CANONICAL_REMOTE" ]; then
  echo "ERROR: CMUX_SRC must have a manaflow-ai/cmux remote" >&2
  exit 1
fi

# The only permitted parent dirt is the deliberate unpublished Ghostty gitlink.
# The submodule itself must be clean and at the browser's exact Ghostty pin.
UNEXPECTED_STATUS="$(git -C "$CMUX_SRC" status --porcelain --untracked-files=normal | grep -v '^ M ghostty$' || true)"
if [ -n "$UNEXPECTED_STATUS" ]; then
  echo "ERROR: CMUX_SRC has changes outside the expected Ghostty gitlink:" >&2
  echo "$UNEXPECTED_STATUS" >&2
  exit 1
fi
GHOSTTY_SRC="$CMUX_SRC/ghostty"
ACTUAL_GHOSTTY_REVISION="$(git -C "$GHOSTTY_SRC" rev-parse HEAD)"
if [ "$ACTUAL_GHOSTTY_REVISION" != "$EXPECTED_GHOSTTY_REVISION" ]; then
  echo "ERROR: cmux Ghostty is $ACTUAL_GHOSTTY_REVISION; browser pin is $EXPECTED_GHOSTTY_REVISION" >&2
  exit 1
fi
if [ -n "$(git -C "$GHOSTTY_SRC" status --porcelain --untracked-files=normal)" ]; then
  echo "ERROR: cmux Ghostty submodule must be clean" >&2
  exit 1
fi
case "$(git -C "$GHOSTTY_SRC" remote get-url origin)" in
  https://github.com/manaflow-ai/ghostty.git|git@github.com:manaflow-ai/ghostty.git) ;;
  *) echo "ERROR: cmux Ghostty origin must be manaflow-ai/ghostty" >&2; exit 1 ;;
esac

host_os="$(uname -s)"
host_arch="$(uname -m)"
case "$host_os:$host_arch" in
  Darwin:arm64) HOST_TARGET="aarch64-apple-darwin" ;;
  Darwin:x86_64) HOST_TARGET="x86_64-apple-darwin" ;;
  Linux:aarch64) HOST_TARGET="aarch64-unknown-linux-gnu" ;;
  Linux:x86_64) HOST_TARGET="x86_64-unknown-linux-gnu" ;;
  MINGW*:*|MSYS*:*|CYGWIN*:*) HOST_TARGET="x86_64-pc-windows-gnu" ;;
  *) echo "ERROR: unsupported resource-build host $host_os/$host_arch" >&2; exit 1 ;;
esac
TARGET="${CMUX_TUI_TARGET:-}"
if [ -z "$TARGET" ]; then
  TARGET="$HOST_TARGET"
elif [ "$TARGET" != "$HOST_TARGET" ]; then
  echo "ERROR: cmux-tui resource staging does not support cross-target builds ($HOST_TARGET -> $TARGET)" >&2
  exit 1
fi
RESOURCE_PROFILE="${CMUX_TUI_RESOURCE_PROFILE:-full}"
case "$RESOURCE_PROFILE" in
  full)
    EMIT_THEMES=true
    ;;
  terminfo-only)
    # The full upstream theme collection does not carry per-theme
    # redistribution metadata. Linux release artifacts deliberately stage
    # only compiled terminfo until a reviewed theme subset exists.
    EMIT_THEMES=false
    ;;
  *)
    echo "ERROR: unsupported cmux-tui resource profile $RESOURCE_PROFILE" >&2
    exit 1
    ;;
esac

case "$TARGET" in
  *windows*) EXE="cmux-tui.exe" ;;
  *) EXE="cmux-tui" ;;
esac

case "$TARGET" in
  aarch64-apple-darwin) GHOSTTY_CLI_TARGET="aarch64-macos" ;;
  x86_64-apple-darwin) GHOSTTY_CLI_TARGET="x86_64-macos" ;;
  *) GHOSTTY_CLI_TARGET="" ;;
esac

RESOURCE_TMP=""
cleanup() {
  if [ -n "$RESOURCE_TMP" ]; then
    rm -rf "$RESOURCE_TMP"
  fi
}
trap cleanup EXIT

if [ "${CMUX_TUI_VALIDATE_ONLY:-0}" = "1" ]; then
  echo "validated cmux-tui source $EXPECTED_CMUX_REVISION for $TARGET"
  echo "validated manaflow Ghostty source $EXPECTED_GHOSTTY_REVISION"
  exit 0
fi

echo "building cmux-tui $EXPECTED_CMUX_REVISION for $TARGET"
(
  cd "$CMUX_SRC/cmux-tui"
  CMUX_TUI_BUILD_COMMIT="$EXPECTED_CMUX_REVISION" \
  CMUX_TUI_GHOSTTY_COMMIT="$EXPECTED_GHOSTTY_REVISION" \
  CMUX_GHOSTTY_VT_ZIG_CPU=baseline \
    cargo build -p cmux-tui --bin cmux-tui --release --locked --target "$TARGET"
)

BINARY="$CMUX_SRC/cmux-tui/target/$TARGET/release/$EXE"
if [ ! -f "$BINARY" ]; then
  echo "ERROR: cmux-tui build did not produce $BINARY" >&2
  exit 1
fi

# Generate the selected runtime resource profile from the same clean, pinned
# manaflow Ghostty checkout used above; relying on a separately installed
# Ghostty.app can silently mix forks or revisions. This is resource-only and
# normally reuses Zig's cache from the libghostty-vt build.
RESOURCE_TMP="$(mktemp -d)"
echo "building Ghostty $RESOURCE_PROFILE runtime resources $EXPECTED_GHOSTTY_REVISION"
(
  cd "$GHOSTTY_SRC"
  # Ghostty only attaches its resources to the install step when building an
  # executable or the library runtime. Keep the executable disabled, but select
  # the library runtime so the requested resources are installed under
  # --prefix.
  "${ZIG:-zig}" build \
    -Dapp-runtime=none \
    -Demit-exe=false \
    -Demit-test-exe=false \
    -Demit-xcframework=false \
    -Demit-macos-app=false \
    -Demit-docs=false \
    -Demit-terminfo=true \
    -Demit-termcap=true \
    "-Demit-themes=$EMIT_THEMES" \
    -Di18n=false \
    --prefix "$RESOURCE_TMP"
)
GHOSTTY_RESOURCES="$RESOURCE_TMP/share/ghostty"
required_resource_directories=("$RESOURCE_TMP/share/terminfo")
if [ "$RESOURCE_PROFILE" = full ]; then
  required_resource_directories+=(
    "$GHOSTTY_RESOURCES/themes"
    "$GHOSTTY_RESOURCES/shell-integration"
  )
fi
for directory in "${required_resource_directories[@]}"; do
  if [ ! -d "$directory" ]; then
    echo "ERROR: Ghostty resource build did not produce $directory" >&2
    exit 1
  fi
done

# macOS cmux bundles carry a small, standalone Ghostty CLI resolver alongside
# the pinned resources. cmux-tui uses it for `+show-config`, so recursive
# config-file includes and application defaults are resolved by the exact same
# manaflow Ghostty revision as libghostty-vt. This must be Ghostty's
# `cli-helper` build; copying Ghostty.app's main executable would leave an
# unusable @rpath/Sparkle dependency behind.
GHOSTTY_CLI_HELPER=""
if [ -n "$GHOSTTY_CLI_TARGET" ]; then
  GHOSTTY_CLI_HELPER="$RESOURCE_TMP/bin/ghostty"
  echo "building Ghostty CLI resolver $EXPECTED_GHOSTTY_REVISION"
  "$CMUX_SRC/scripts/build-ghostty-cli-helper.sh" \
    --target "$GHOSTTY_CLI_TARGET" \
    --output "$GHOSTTY_CLI_HELPER"
  if [ ! -x "$GHOSTTY_CLI_HELPER" ]; then
    echo "ERROR: Ghostty CLI resolver build did not produce an executable" >&2
    exit 1
  fi
  if [ "${CMUX_SKIP_ZIG_BUILD:-0}" != "1" ]; then
    GHOSTTY_CLI_VERSION_OUTPUT="$(
      GHOSTTY_RESOURCES_DIR="$GHOSTTY_RESOURCES" \
        "$GHOSTTY_CLI_HELPER" +version 2>/dev/null
    )"
    GHOSTTY_CLI_VERSION="${GHOSTTY_CLI_VERSION_OUTPUT%%$'\n'*}"
    case "$GHOSTTY_CLI_VERSION" in
      *"${EXPECTED_GHOSTTY_REVISION:0:9}"*) ;;
      *)
        echo "ERROR: Ghostty CLI resolver does not report the pinned revision" >&2
        exit 1
        ;;
    esac
    if ! GHOSTTY_RESOURCES_DIR="$GHOSTTY_RESOURCES" \
         "$GHOSTTY_CLI_HELPER" +show-config --no-pager \
         > "$RESOURCE_TMP/ghostty-show-config" 2>/dev/null || \
       ! grep -Eq '^background = #[0-9a-fA-F]{6}$' \
         "$RESOURCE_TMP/ghostty-show-config" || \
       ! grep -Eq '^foreground = #[0-9a-fA-F]{6}$' \
         "$RESOURCE_TMP/ghostty-show-config"; then
      echo "ERROR: pinned Ghostty CLI resolver could not resolve the active config" >&2
      exit 1
    fi
  fi
fi

STAGE="${CMUX_TUI_STAGE:-$ROOT/dist/cmux-tui/$TARGET}"
mkdir -p "$STAGE"
cp "$BINARY" "$STAGE/$EXE"
rm -rf "$STAGE/bin"
rm -f "$STAGE/GHOSTTY_CLI_REVISION"
if [ -n "$GHOSTTY_CLI_HELPER" ]; then
  mkdir -p "$STAGE/bin"
  cp "$GHOSTTY_CLI_HELPER" "$STAGE/bin/ghostty"
  chmod +x "$STAGE/bin/ghostty"
  printf '%s\n' "$EXPECTED_GHOSTTY_REVISION" > "$STAGE/GHOSTTY_CLI_REVISION"
fi
rm -rf "$STAGE/ghostty"
mkdir -p "$STAGE/ghostty/themes" "$STAGE/ghostty/shell-integration"
if [ "$RESOURCE_PROFILE" = full ]; then
  rsync -a --delete "$GHOSTTY_RESOURCES/" "$STAGE/ghostty/"
fi
rm -rf "$STAGE/terminfo"
mkdir -p "$STAGE/terminfo"
# tic represents secondary TERM names as symlinks. Materialize those aliases
# so every packaged resource remains a self-contained regular file.
rsync -a --copy-links --delete \
  "$RESOURCE_TMP/share/terminfo/" "$STAGE/terminfo/"
if [ "$RESOURCE_PROFILE" = terminfo-only ]; then
  UNEXPECTED_UNLICENSED_RESOURCE="$(
    find "$STAGE/ghostty/themes" "$STAGE/ghostty/shell-integration" \
      -mindepth 1 -print -quit
  )"
  if [ -n "$UNEXPECTED_UNLICENSED_RESOURCE" ]; then
    echo "ERROR: terminfo-only profile staged an unverified Ghostty theme or shell-integration resource: $UNEXPECTED_UNLICENSED_RESOURCE" >&2
    exit 1
  fi
fi
UNEXPECTED_RESOURCE_ENTRY="$(
  cd "$STAGE"
  find ghostty terminfo ! -type d ! -type f -print -quit
)"
if [ -n "$UNEXPECTED_RESOURCE_ENTRY" ]; then
  echo "ERROR: Ghostty resources contain a non-regular entry: $UNEXPECTED_RESOURCE_ENTRY" >&2
  exit 1
fi
(
  cd "$STAGE"
  find ghostty terminfo -type f -print0 | LC_ALL=C sort -z | \
    xargs -0 shasum -a 256
) > "$STAGE/GHOSTTY_RESOURCES_MANIFEST.sha256"
printf '%s\n' "$EXPECTED_CMUX_REVISION" > "$STAGE/REVISION"
printf '%s\n' "$EXPECTED_GHOSTTY_REVISION" > "$STAGE/GHOSTTY_REVISION"

if [ "${CMUX_TUI_SKIP_RUN_VERIFY:-0}" != "1" ]; then
  VERSION="$($STAGE/$EXE --version)"
  case "$VERSION" in
    *"$EXPECTED_CMUX_REVISION"*"ghostty $EXPECTED_GHOSTTY_REVISION"*) ;;
    *) echo "ERROR: staged cmux-tui does not report both pinned commits" >&2; exit 1 ;;
  esac
fi

echo "staged cmux-tui -> $STAGE"
echo "staged Ghostty $RESOURCE_PROFILE resources -> $STAGE/ghostty"
echo "staged Ghostty terminfo -> $STAGE/terminfo"
if [ -n "$GHOSTTY_CLI_HELPER" ]; then
  echo "staged pinned Ghostty CLI resolver -> $STAGE/bin/ghostty"
fi
echo "staged Ghostty resource manifest -> $STAGE/GHOSTTY_RESOURCES_MANIFEST.sha256"
echo "cmux revision: $EXPECTED_CMUX_REVISION"
echo "Ghostty revision: $EXPECTED_GHOSTTY_REVISION"
