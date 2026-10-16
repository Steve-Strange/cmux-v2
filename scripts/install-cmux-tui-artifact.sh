#!/bin/bash
# Install a previously staged cmux-tui artifact into a Chromium product
# directory (the executable directory on Linux/Windows, Contents/Helpers on
# macOS). Refuses manifests that differ from the browser's pinned sources.
# CMUX_TUI_MANIFEST_DIR may place the revision receipts outside the executable
# directory; macOS bundles must keep non-code files out of Contents/Helpers.
# The staged Ghostty themes and shell integration are installed alongside the
# exact helper revision instead of being borrowed from a system Ghostty.app.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEST="${1:-${CMUX_TUI_INSTALL_DIR:-}}"
if [ -z "$DEST" ]; then
  echo "usage: $0 <browser-product-helper-directory>" >&2
  exit 2
fi

TARGET="${CMUX_TUI_TARGET:-}"
if [ -z "$TARGET" ]; then
  case "$(uname -s):$(uname -m)" in
    Darwin:arm64) TARGET="aarch64-apple-darwin" ;;
    Darwin:x86_64) TARGET="x86_64-apple-darwin" ;;
    Linux:aarch64) TARGET="aarch64-unknown-linux-gnu" ;;
    Linux:x86_64) TARGET="x86_64-unknown-linux-gnu" ;;
    MINGW*:*|MSYS*:*|CYGWIN*:*) TARGET="x86_64-pc-windows-gnu" ;;
    *) echo "ERROR: set CMUX_TUI_TARGET for this host" >&2; exit 1 ;;
  esac
fi
case "$TARGET" in
  *windows*) EXE="cmux-tui.exe" ;;
  *) EXE="cmux-tui" ;;
esac

STAGE="${CMUX_TUI_STAGE:-$ROOT/dist/cmux-tui/$TARGET}"
MANIFEST_DEST="${CMUX_TUI_MANIFEST_DIR:-$DEST}"
GHOSTTY_RESOURCES_DEST="${CMUX_GHOSTTY_RESOURCES_INSTALL_DIR:-}"
if [ -z "$GHOSTTY_RESOURCES_DEST" ]; then
  case "$DEST" in
    */Contents/Helpers)
      GHOSTTY_RESOURCES_DEST="$(dirname "$DEST")/Resources/ghostty"
      ;;
    *)
      GHOSTTY_RESOURCES_DEST="$DEST/ghostty"
      ;;
  esac
fi
GHOSTTY_SHARE_DEST="$(dirname "$GHOSTTY_RESOURCES_DEST")"
GHOSTTY_TERMINFO_DEST="$GHOSTTY_SHARE_DEST/terminfo"
GHOSTTY_CLI_DEST="${CMUX_GHOSTTY_CLI_INSTALL_PATH:-}"
if [ -z "$GHOSTTY_CLI_DEST" ]; then
  case "$DEST" in
    */Contents/Helpers)
      GHOSTTY_CLI_DEST="$(dirname "$DEST")/Resources/bin/ghostty"
      ;;
    *)
      GHOSTTY_CLI_DEST="$DEST/bin/ghostty"
      ;;
  esac
fi
case "$GHOSTTY_RESOURCES_DEST" in
  ghostty|*/ghostty) ;;
  *)
    echo "ERROR: CMUX_GHOSTTY_RESOURCES_INSTALL_DIR must end in /ghostty" >&2
    exit 1
    ;;
esac
case "$GHOSTTY_CLI_DEST" in
  */bin/ghostty) ;;
  *)
    echo "ERROR: CMUX_GHOSTTY_CLI_INSTALL_PATH must end in /bin/ghostty" >&2
    exit 1
    ;;
esac
EXPECTED_CMUX_REVISION="$(sed -n '/kPinnedCmuxTuiBuildCommit/{n;s/^    "\([0-9a-f]*\)";$/\1/p;}' "$ROOT/overlay/chrome/browser/cmux_term/cmux_tui_revision.h")"
EXPECTED_GHOSTTY_REVISION="$(tr -d '[:space:]' < "$ROOT/ghostty-revision.txt")"
RUNTIME_GHOSTTY_REVISION="$(sed -n '/kPinnedCmuxTuiGhosttyCommit/{n;s/^    "\([0-9a-f]*\)";$/\1/p;}' "$ROOT/overlay/chrome/browser/cmux_term/cmux_tui_revision.h")"

VERIFY_TMP="$(mktemp -d)"
trap 'rm -rf "$VERIFY_TMP"' EXIT
verify_resource_tree() {
  local base="$1"
  local manifest="$2"
  local label="$3"
  local unexpected
  unexpected="$(
    cd "$base"
    find ghostty terminfo ! -type d ! -type f -print -quit
  )"
  if [ -n "$unexpected" ]; then
    echo "ERROR: $label contains a non-regular entry: $unexpected" >&2
    return 1
  fi
  if grep -Ev '^[0-9a-f]{64}  (ghostty|terminfo)/' "$manifest" \
       > "$VERIFY_TMP/bad-manifest"; then
    echo "ERROR: $label manifest contains an invalid path" >&2
    return 1
  fi
  (
    cd "$base"
    find ghostty terminfo -type f -print | LC_ALL=C sort
  ) > "$VERIFY_TMP/actual-paths"
  sed -E 's/^[0-9a-f]{64}  //' "$manifest" | LC_ALL=C sort \
    > "$VERIFY_TMP/manifest-paths"
  if ! diff -u "$VERIFY_TMP/manifest-paths" "$VERIFY_TMP/actual-paths" \
       > "$VERIFY_TMP/tree-diff"; then
    echo "ERROR: $label has files not described by its manifest" >&2
    return 1
  fi
  if ! (cd "$base" && shasum -a 256 -c "$manifest" >/dev/null); then
    echo "ERROR: $label manifest verification failed" >&2
    return 1
  fi
}

if [ "$RUNTIME_GHOSTTY_REVISION" != "$EXPECTED_GHOSTTY_REVISION" ]; then
  echo "ERROR: runtime Ghostty pin differs from ghostty-revision.txt" >&2
  exit 1
fi

for path in "$STAGE/$EXE" "$STAGE/REVISION" "$STAGE/GHOSTTY_REVISION" \
            "$STAGE/GHOSTTY_RESOURCES_MANIFEST.sha256"; do
  if [ ! -f "$path" ]; then
    echo "ERROR: missing staged cmux-tui artifact $path" >&2
    exit 1
  fi
done
case "$TARGET" in
  *apple-darwin)
    for path in "$STAGE/bin/ghostty" "$STAGE/GHOSTTY_CLI_REVISION"; do
      if [ ! -f "$path" ]; then
        echo "ERROR: missing staged Ghostty CLI resolver artifact $path" >&2
        exit 1
      fi
    done
    if [ ! -x "$STAGE/bin/ghostty" ]; then
      echo "ERROR: staged Ghostty CLI resolver is not executable" >&2
      exit 1
    fi
    if [ "$(tr -d '[:space:]' < "$STAGE/GHOSTTY_CLI_REVISION")" != \
         "$EXPECTED_GHOSTTY_REVISION" ]; then
      echo "ERROR: staged Ghostty CLI resolver revision differs from browser pin" >&2
      exit 1
    fi
    ;;
esac
for directory in "$STAGE/ghostty/themes" \
                 "$STAGE/ghostty/shell-integration" \
                 "$STAGE/terminfo"; do
  if [ ! -d "$directory" ]; then
    echo "ERROR: missing staged Ghostty resources $directory" >&2
    exit 1
  fi
done
if ! verify_resource_tree "$STAGE" \
       "$STAGE/GHOSTTY_RESOURCES_MANIFEST.sha256" \
       "staged Ghostty resource tree"; then
  echo "ERROR: staged Ghostty resource manifest verification failed" >&2
  exit 1
fi
if [ "$(tr -d '[:space:]' < "$STAGE/REVISION")" != "$EXPECTED_CMUX_REVISION" ]; then
  echo "ERROR: staged cmux-tui revision differs from browser pin" >&2
  exit 1
fi
if [ "$(tr -d '[:space:]' < "$STAGE/GHOSTTY_REVISION")" != "$EXPECTED_GHOSTTY_REVISION" ]; then
  echo "ERROR: staged cmux-tui Ghostty revision differs from browser pin" >&2
  exit 1
fi

mkdir -p "$DEST" "$MANIFEST_DEST" "$GHOSTTY_RESOURCES_DEST" \
  "$GHOSTTY_TERMINFO_DEST"
cp "$STAGE/$EXE" "$DEST/$EXE"
rsync -a --delete "$STAGE/ghostty/" "$GHOSTTY_RESOURCES_DEST/"
rsync -a --delete "$STAGE/terminfo/" "$GHOSTTY_TERMINFO_DEST/"
case "$TARGET" in
  *apple-darwin)
    mkdir -p "$(dirname "$GHOSTTY_CLI_DEST")"
    cp "$STAGE/bin/ghostty" "$GHOSTTY_CLI_DEST"
    chmod +x "$GHOSTTY_CLI_DEST"
    ;;
esac
if ! verify_resource_tree "$GHOSTTY_SHARE_DEST" \
       "$STAGE/GHOSTTY_RESOURCES_MANIFEST.sha256" \
       "installed Ghostty resource tree"; then
  echo "ERROR: installed Ghostty resource manifest verification failed" >&2
  exit 1
fi
cp "$STAGE/REVISION" "$MANIFEST_DEST/cmux-tui.REVISION"
cp "$STAGE/GHOSTTY_REVISION" "$MANIFEST_DEST/cmux-tui.GHOSTTY_REVISION"
cp "$STAGE/GHOSTTY_RESOURCES_MANIFEST.sha256" \
  "$MANIFEST_DEST/cmux-tui.GHOSTTY_RESOURCES_MANIFEST.sha256"
chmod +x "$DEST/$EXE"
case "$TARGET" in
  *apple-darwin)
    cp "$STAGE/GHOSTTY_CLI_REVISION" \
      "$MANIFEST_DEST/cmux-tui.GHOSTTY_CLI_REVISION"
    ;;
  *)
    rm -f "$MANIFEST_DEST/cmux-tui.GHOSTTY_CLI_REVISION"
    ;;
esac

if [ "${CMUX_TUI_SKIP_RUN_VERIFY:-0}" != "1" ]; then
  VERSION="$($DEST/$EXE --version)"
  case "$VERSION" in
    *"$EXPECTED_CMUX_REVISION"*"ghostty $EXPECTED_GHOSTTY_REVISION"*) ;;
    *) echo "ERROR: installed cmux-tui does not report both pinned commits" >&2; exit 1 ;;
  esac
  case "$TARGET" in
    *apple-darwin)
      GHOSTTY_CLI_VERSION_OUTPUT="$(
        GHOSTTY_RESOURCES_DIR="$GHOSTTY_RESOURCES_DEST" \
          "$GHOSTTY_CLI_DEST" +version 2>/dev/null
      )"
      GHOSTTY_CLI_VERSION="${GHOSTTY_CLI_VERSION_OUTPUT%%$'\n'*}"
      case "$GHOSTTY_CLI_VERSION" in
        *"${EXPECTED_GHOSTTY_REVISION:0:9}"*) ;;
        *)
          echo "ERROR: installed Ghostty CLI resolver does not report the pinned revision" >&2
          exit 1
          ;;
      esac
      ;;
  esac
fi

echo "installed pinned cmux-tui -> $DEST/$EXE"
echo "installed pinned Ghostty resources -> $GHOSTTY_RESOURCES_DEST"
echo "installed pinned Ghostty terminfo -> $GHOSTTY_TERMINFO_DEST"
case "$TARGET" in
  *apple-darwin)
    echo "installed pinned Ghostty CLI resolver -> $GHOSTTY_CLI_DEST"
    ;;
esac
echo "installed revision receipts -> $MANIFEST_DEST"
