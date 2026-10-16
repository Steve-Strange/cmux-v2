#!/bin/bash
# Package an already-deployed, signed, and notarized macOS app for first install
# and in-app updates. This script never mutates the app after codesigning.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
APP="${CMUX_RELEASE_APP:?CMUX_RELEASE_APP is required}"
VERSION="${CMUX_RELEASE_VERSION:?CMUX_RELEASE_VERSION is required}"
CHANNEL="${CMUX_RELEASE_CHANNEL:?CMUX_RELEASE_CHANNEL is required}"
OUT="${CMUX_RELEASE_OUTPUT:-$ROOT/release}"
ARCH="${CMUX_MAC_RELEASE_ARCH:-arm64}"
NIGHTLY_FEED_URL="https://github.com/manaflow-ai/cmux-v2/releases/download/nightly/update.json"

case "$CHANNEL" in
  stable)
    EXPECTED_BUNDLE_ID="${CMUX_EXPECTED_BUNDLE_ID:-com.cmux.app}"
    VOLUME_CHANNEL="Stable"
    ;;
  nightly)
    EXPECTED_BUNDLE_ID="${CMUX_EXPECTED_BUNDLE_ID:-com.cmux.app.nightly}"
    VOLUME_CHANNEL="Nightly"
    ;;
  *)
    echo "error: invalid release channel: $CHANNEL" >&2
    exit 2
    ;;
esac
case "$ARCH" in
  arm64|x64) ;;
  *)
    echo "error: CMUX_MAC_RELEASE_ARCH must be arm64 or x64" >&2
    exit 2
    ;;
esac

[ -d "$APP" ] || { echo "error: release app does not exist: $APP" >&2; exit 2; }
[ "$(basename "$APP")" = "cmux-browser.app" ] || {
  echo "error: release app must be named cmux-browser.app" >&2
  exit 2
}

CMUX_EXPECTED_BUNDLE_ID="$EXPECTED_BUNDLE_ID" \
  "$ROOT/scripts/verify-platform-metadata.sh" "$APP"

PLIST="$APP/Contents/Info.plist"
EXECUTABLE_NAME="$(plutil -extract CFBundleExecutable raw -o - "$PLIST")"
EXECUTABLE="$APP/Contents/MacOS/$EXECUTABLE_NAME"
SHORT_VERSION="$(plutil -extract CFBundleShortVersionString raw -o - \
  "$PLIST" 2>/dev/null || true)"
BUNDLE_VERSION="$(plutil -extract CFBundleVersion raw -o - \
  "$PLIST" 2>/dev/null || true)"
if [ "$SHORT_VERSION" != "$VERSION" ] && [ "$BUNDLE_VERSION" != "$VERSION" ]; then
  echo "error: app version is short=$SHORT_VERSION bundle=$BUNDLE_VERSION; expected $VERSION" >&2
  exit 1
fi

case " $(lipo -archs "$EXECUTABLE") " in
  *" $ARCH "*) ;;
  *)
    echo "error: $EXECUTABLE does not contain architecture $ARCH" >&2
    exit 1
    ;;
esac

FEED_FILE="$APP/Contents/Resources/cmux-update-feed-url"
if [ "$CHANNEL" = nightly ]; then
  [ "$(tr -d '\r\n' < "$FEED_FILE" 2>/dev/null || true)" = \
      "$NIGHTLY_FEED_URL" ] || {
    echo "error: nightly app does not contain the cmux-v2 nightly feed" >&2
    exit 1
  }
elif [ -e "$FEED_FILE" ]; then
  echo "error: stable app unexpectedly contains a channel feed selector" >&2
  exit 1
fi

for required_notice in \
    "$APP/Contents/Resources/cmux-licenses/LICENSE" \
    "$APP/Contents/Resources/cmux-licenses/THIRD_PARTY_NOTICES.md" \
    "$APP/Contents/Resources/cmux-licenses/third_party/helium/LICENSE"; do
  [ -f "$required_notice" ] || {
    echo "error: release app is missing notice: $required_notice" >&2
    exit 1
  }
done

codesign --verify --deep --strict "$APP"
if [ "${CMUX_ALLOW_UNTRUSTED_MAC_RELEASE:-0}" != "1" ]; then
  SIGNATURE_DETAILS="$(codesign -dvvv "$APP" 2>&1)"
  printf '%s\n' "$SIGNATURE_DETAILS" | \
    grep -Fq "Authority=Developer ID Application:" || {
      echo "error: release app is not signed with Developer ID Application" >&2
      exit 1
    }
  printf '%s\n' "$SIGNATURE_DETAILS" | \
    grep -Fq "TeamIdentifier=7WLXT3NR37" || {
      echo "error: release app signature does not use Manaflow team 7WLXT3NR37" >&2
      exit 1
    }
  spctl --assess --type execute --verbose=4 "$APP"
  xcrun stapler validate "$APP"
fi

mkdir -p "$OUT"
case "$ARCH" in
  arm64) PLATFORM="mac-arm64"; ASSET_ARCH="arm64" ;;
  x64) PLATFORM="mac-x64"; ASSET_ARCH="x64" ;;
esac
ZIP="$OUT/cmux-macos-$ASSET_ARCH.zip"
DMG="$OUT/cmux-macos-$ASSET_ARCH.dmg"
rm -f "$ZIP" "$DMG"

python3 "$ROOT/scripts/build-update-archive.py" \
  --input "$APP" --output "$ZIP"
DMG_STAGE="$(mktemp -d "${TMPDIR:-/tmp}/cmux-release-dmg.XXXXXX")"
trap 'rm -rf "$DMG_STAGE"' EXIT
ditto "$APP" "$DMG_STAGE/$(basename "$APP")"
hdiutil create -quiet -ov -format UDZO \
  -volname "cmux Browser $VOLUME_CHANNEL" -srcfolder "$DMG_STAGE" "$DMG"
hdiutil verify "$DMG" >/dev/null

python3 - "$ZIP" "$APP" "$EXECUTABLE_NAME" <<'PY'
from pathlib import Path
import sys
import zipfile

archive = Path(sys.argv[1])
app = Path(sys.argv[2])
executable = sys.argv[3]
expected_root = app.name
with zipfile.ZipFile(archive) as zipped:
    names = set(zipped.namelist())
    required = {
        f"{expected_root}/Contents/Info.plist",
        f"{expected_root}/Contents/MacOS/{executable}",
        f"{expected_root}/Contents/Resources/cmux-licenses/LICENSE",
        f"{expected_root}/Contents/Resources/cmux-licenses/THIRD_PARTY_NOTICES.md",
    }
    missing = sorted(required - names)
    if missing:
        raise SystemExit(f"update archive is incomplete: {missing}")
PY

echo "macOS release artifacts:"
echo "  first install: $DMG"
echo "  in-app update: $ZIP"
echo "  manifest: --artifact $PLATFORM=$ZIP,cmux-browser.app,Contents/MacOS/$EXECUTABLE_NAME"
