#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VERIFY="$ROOT/scripts/verify-cmux-runtime-pins.sh"
RESOLVE="$ROOT/scripts/resolve-chromium-runtime-image.sh"
HEADER="$ROOT/overlay/chrome/browser/cmux_term/cmux_tui_revision.h"
CMUX_REVISION="$(sed -n '/kPinnedCmuxTuiBuildCommit/{n;s/^    "\([0-9a-f]*\)";$/\1/p;}' "$HEADER")"
GHOSTTY_REVISION="$(sed -n '/kPinnedCmuxTuiGhosttyCommit/{n;s/^    "\([0-9a-f]*\)";$/\1/p;}' "$HEADER")"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

mkdir -p "$TMP/good/Contents/Frameworks/cmux Framework.framework/Versions/Current"
printf 'unrelated\0%s\0%s\0' "$CMUX_REVISION" "$GHOSTTY_REVISION" > \
  "$TMP/good/Contents/Frameworks/cmux Framework.framework/Versions/Current/cmux Framework"
GOOD_IMAGE="$TMP/good/Contents/Frameworks/cmux Framework.framework/Versions/Current/cmux Framework"
if [ "$(uname -s)" = Darwin ]; then
  plutil -create xml1 "$TMP/good/Contents/Info.plist"
  plutil -insert CFBundleExecutable -string cmux "$TMP/good/Contents/Info.plist"
  [ "$("$RESOLVE" "$TMP/good")" = "$GOOD_IMAGE" ]
fi
"$VERIFY" "$GOOD_IMAGE" >/dev/null

if [ "$(uname -s)" = Darwin ]; then
  mkdir -p \
    "$TMP/legacy/Contents/Frameworks/Chromium Framework.framework/Versions/Current"
  plutil -create xml1 "$TMP/legacy/Contents/Info.plist"
  plutil -insert CFBundleExecutable -string Chromium \
    "$TMP/legacy/Contents/Info.plist"
  printf legacy > \
    "$TMP/legacy/Contents/Frameworks/Chromium Framework.framework/Versions/Current/Chromium Framework"
  [ "$("$RESOLVE" "$TMP/legacy")" = \
    "$TMP/legacy/Contents/Frameworks/Chromium Framework.framework/Versions/Current/Chromium Framework" ]
fi

mkdir -p "$TMP/stale/Contents/Frameworks"
printf 'unrelated\0%s\0%s\0' \
  0000000000000000000000000000000000000000 \
  1111111111111111111111111111111111111111 > \
  "$TMP/stale/Contents/Frameworks/libchrome_dll.dylib"
STALE_IMAGE="$TMP/stale/Contents/Frameworks/libchrome_dll.dylib"
if "$VERIFY" "$STALE_IMAGE" >"$TMP/stale.out" 2>"$TMP/stale.err"; then
  echo "stale compiled runtime unexpectedly passed" >&2
  exit 1
fi
grep -Fq "does not contain this checkout's cmux-tui/Ghostty identity pins" \
  "$TMP/stale.err"

mkdir -p \
  "$TMP/mixed/Contents/Frameworks/cmux Framework.framework/Versions/Current"
printf 'unrelated\0%s\0%s\0' "$CMUX_REVISION" "$GHOSTTY_REVISION" > \
  "$TMP/mixed/Contents/Frameworks/cmux Framework.framework/Versions/Current/cmux Framework"
printf 'unrelated\0%s\0%s\0' \
  0000000000000000000000000000000000000000 \
  1111111111111111111111111111111111111111 > \
  "$TMP/mixed/Contents/Frameworks/libchrome_dll.dylib"
if "$VERIFY" "$TMP/mixed/Contents/Frameworks/libchrome_dll.dylib" \
    >"$TMP/mixed.out" 2>"$TMP/mixed.err"; then
  echo "matching inactive Framework masked a stale active component runtime" >&2
  exit 1
fi

echo "cmux runtime pin tests passed"
