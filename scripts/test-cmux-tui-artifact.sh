#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

STAGE="$TMP/stage"
HELPERS="$TMP/cmux-test.app/Contents/Helpers"
MANIFESTS="$TMP/cmux-test.app/Contents/Resources/cmux-tui"
RESOURCES="$TMP/cmux-test.app/Contents/Resources/ghostty"
TERMINFO="$TMP/cmux-test.app/Contents/Resources/terminfo"
GHOSTTY_CLI="$TMP/cmux-test.app/Contents/Resources/bin/ghostty"
mkdir -p "$STAGE/ghostty/themes" "$STAGE/ghostty/shell-integration/zsh" \
  "$STAGE/terminfo/78" "$STAGE/bin"
cp /usr/bin/true "$STAGE/cmux-tui"
cp /usr/bin/true "$STAGE/bin/ghostty"
sed -n '/kPinnedCmuxTuiBuildCommit/{n;s/^    "\([0-9a-f]*\)";$/\1/p;}' \
  "$ROOT/overlay/chrome/browser/cmux_term/cmux_tui_revision.h" \
  > "$STAGE/REVISION"
tr -d '[:space:]' < "$ROOT/ghostty-revision.txt" \
  > "$STAGE/GHOSTTY_REVISION"
cp "$STAGE/GHOSTTY_REVISION" "$STAGE/GHOSTTY_CLI_REVISION"
printf 'theme-from-pinned-fork\n' > "$STAGE/ghostty/themes/cmux-test"
printf 'spaced-theme-from-pinned-fork\n' \
  > "$STAGE/ghostty/themes/cmux test with spaces"
printf 'integration-from-pinned-fork\n' \
  > "$STAGE/ghostty/shell-integration/zsh/cmux-test"
printf 'terminfo-from-pinned-fork\n' > "$STAGE/terminfo/78/xterm-ghostty"
(
  cd "$STAGE"
  find ghostty terminfo -type f -print0 | LC_ALL=C sort -z | \
    xargs -0 shasum -a 256
) > "$STAGE/GHOSTTY_RESOURCES_MANIFEST.sha256"

mkdir -p "$RESOURCES"
printf 'stale\n' > "$RESOURCES/stale"
printf 'unrelated-resource\n' \
  > "$TMP/cmux-test.app/Contents/Resources/unrelated-resource"
CMUX_TUI_TARGET=aarch64-apple-darwin \
  CMUX_TUI_STAGE="$STAGE" \
  CMUX_TUI_MANIFEST_DIR="$MANIFESTS" \
  CMUX_TUI_SKIP_RUN_VERIFY=1 \
  "$ROOT/scripts/install-cmux-tui-artifact.sh" "$HELPERS" >/dev/null

test -x "$HELPERS/cmux-tui"
cmp "$STAGE/REVISION" "$MANIFESTS/cmux-tui.REVISION"
cmp "$STAGE/GHOSTTY_REVISION" "$MANIFESTS/cmux-tui.GHOSTTY_REVISION"
cmp "$STAGE/GHOSTTY_CLI_REVISION" \
  "$MANIFESTS/cmux-tui.GHOSTTY_CLI_REVISION"
cmp "$STAGE/GHOSTTY_RESOURCES_MANIFEST.sha256" \
  "$MANIFESTS/cmux-tui.GHOSTTY_RESOURCES_MANIFEST.sha256"
cmp "$STAGE/ghostty/themes/cmux-test" "$RESOURCES/themes/cmux-test"
cmp "$STAGE/ghostty/themes/cmux test with spaces" \
  "$RESOURCES/themes/cmux test with spaces"
cmp "$STAGE/ghostty/shell-integration/zsh/cmux-test" \
  "$RESOURCES/shell-integration/zsh/cmux-test"
cmp "$STAGE/terminfo/78/xterm-ghostty" "$TERMINFO/78/xterm-ghostty"
cmp "$STAGE/bin/ghostty" "$GHOSTTY_CLI"
test -x "$GHOSTTY_CLI"
test ! -e "$RESOURCES/stale"
grep -Fq 'unrelated-resource' \
  "$TMP/cmux-test.app/Contents/Resources/unrelated-resource"

rm "$STAGE/bin/ghostty"
if CMUX_TUI_TARGET=aarch64-apple-darwin \
     CMUX_TUI_STAGE="$STAGE" \
     CMUX_TUI_MANIFEST_DIR="$MANIFESTS" \
     CMUX_TUI_SKIP_RUN_VERIFY=1 \
     "$ROOT/scripts/install-cmux-tui-artifact.sh" "$HELPERS" \
     >"$TMP/missing-cli.out" 2>&1; then
  echo "install unexpectedly accepted a missing Ghostty CLI resolver" >&2
  exit 1
fi
grep -Fq "missing staged Ghostty CLI resolver artifact" \
  "$TMP/missing-cli.out"
test -x "$GHOSTTY_CLI"
cp /usr/bin/true "$STAGE/bin/ghostty"

printf 'not-in-manifest\n' > "$STAGE/ghostty/themes/unlisted"
if CMUX_TUI_TARGET=aarch64-apple-darwin \
     CMUX_TUI_STAGE="$STAGE" \
     CMUX_TUI_MANIFEST_DIR="$MANIFESTS" \
     CMUX_TUI_SKIP_RUN_VERIFY=1 \
     "$ROOT/scripts/install-cmux-tui-artifact.sh" "$HELPERS" \
     >"$TMP/unlisted.out" 2>&1; then
  echo "install unexpectedly accepted an unlisted Ghostty resource" >&2
  exit 1
fi
grep -Fq "files not described by its manifest" "$TMP/unlisted.out"
rm "$STAGE/ghostty/themes/unlisted"

ln -s cmux-test "$STAGE/ghostty/themes/symlink"
if CMUX_TUI_TARGET=aarch64-apple-darwin \
     CMUX_TUI_STAGE="$STAGE" \
     CMUX_TUI_MANIFEST_DIR="$MANIFESTS" \
     CMUX_TUI_SKIP_RUN_VERIFY=1 \
     "$ROOT/scripts/install-cmux-tui-artifact.sh" "$HELPERS" \
     >"$TMP/symlink.out" 2>&1; then
  echo "install unexpectedly accepted a symlinked Ghostty resource" >&2
  exit 1
fi
grep -Fq "non-regular entry" "$TMP/symlink.out"
rm "$STAGE/ghostty/themes/symlink"

if CMUX_TUI_TARGET=aarch64-apple-darwin \
     CMUX_TUI_STAGE="$STAGE" \
     CMUX_GHOSTTY_RESOURCES_INSTALL_DIR="$TMP/wrong-name" \
     CMUX_TUI_SKIP_RUN_VERIFY=1 \
     "$ROOT/scripts/install-cmux-tui-artifact.sh" "$HELPERS" \
     >"$TMP/wrong-name.out" 2>&1; then
  echo "install unexpectedly accepted a non-ghostty resource destination" >&2
  exit 1
fi
grep -Fq "must end in /ghostty" "$TMP/wrong-name.out"
test ! -e "$TMP/wrong-name"

rm -rf "$STAGE/ghostty/themes"
if CMUX_TUI_TARGET=aarch64-apple-darwin \
     CMUX_TUI_STAGE="$STAGE" \
     CMUX_TUI_MANIFEST_DIR="$MANIFESTS" \
     CMUX_TUI_SKIP_RUN_VERIFY=1 \
     "$ROOT/scripts/install-cmux-tui-artifact.sh" "$HELPERS" \
     >"$TMP/missing.out" 2>&1; then
  echo "install unexpectedly accepted missing pinned Ghostty themes" >&2
  exit 1
fi
grep -Fq "missing staged Ghostty resources" "$TMP/missing.out"
cmp "$STAGE/ghostty/shell-integration/zsh/cmux-test" \
  "$RESOURCES/shell-integration/zsh/cmux-test"

mkdir -p "$STAGE/ghostty/themes"
cp "$RESOURCES/themes/cmux-test" "$STAGE/ghostty/themes/cmux-test"
cp "$RESOURCES/themes/cmux test with spaces" \
  "$STAGE/ghostty/themes/cmux test with spaces"
printf 'tampered\n' > "$STAGE/ghostty/shell-integration/zsh/cmux-test"
if CMUX_TUI_TARGET=aarch64-apple-darwin \
     CMUX_TUI_STAGE="$STAGE" \
     CMUX_TUI_MANIFEST_DIR="$MANIFESTS" \
     CMUX_TUI_SKIP_RUN_VERIFY=1 \
     "$ROOT/scripts/install-cmux-tui-artifact.sh" "$HELPERS" \
     >"$TMP/tampered.out" 2>&1; then
  echo "install unexpectedly accepted tampered Ghostty resources" >&2
  exit 1
fi
grep -Fq "staged Ghostty resource manifest verification failed" \
  "$TMP/tampered.out"
grep -Fq 'integration-from-pinned-fork' \
  "$RESOURCES/shell-integration/zsh/cmux-test"

echo "cmux-tui artifact tests passed"
