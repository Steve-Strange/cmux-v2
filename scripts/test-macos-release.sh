#!/bin/bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TMP="$(mktemp -d "${TMPDIR:-/tmp}/cmux-macos-release-test.XXXXXX")"
MOUNT="$TMP/mount"
MOUNTED=0
cleanup() {
  if [ "$MOUNTED" = 1 ]; then
    hdiutil detach "$MOUNT" -quiet || true
  fi
  rm -rf "$TMP"
}
trap cleanup EXIT

APP="$TMP/cmux-browser.app"
CONTENTS="$APP/Contents"
FRAMEWORK="$CONTENTS/Frameworks/cmux Framework.framework/Versions/1"
mkdir -p "$CONTENTS/MacOS" "$FRAMEWORK/Resources" \
  "$CONTENTS/Resources/en.lproj" \
  "$CONTENTS/Resources/cmux-licenses/third_party/helium"

cp "$ROOT/LICENSE" "$CONTENTS/Resources/cmux-licenses/LICENSE"
cp "$ROOT/THIRD_PARTY_NOTICES.md" \
  "$CONTENTS/Resources/cmux-licenses/THIRD_PARTY_NOTICES.md"
cp "$ROOT/third_party/helium/LICENSE" \
  "$CONTENTS/Resources/cmux-licenses/third_party/helium/LICENSE"

plutil -create xml1 "$CONTENTS/Info.plist"
plutil -insert CFBundleIdentifier -string com.cmux.app.nightly \
  "$CONTENTS/Info.plist"
plutil -insert CFBundleName -string cmux "$CONTENTS/Info.plist"
plutil -insert CFBundleDisplayName -string cmux "$CONTENTS/Info.plist"
plutil -insert CFBundleExecutable -string cmux "$CONTENTS/Info.plist"
plutil -insert CFBundlePackageType -string APPL "$CONTENTS/Info.plist"
plutil -insert CFBundleShortVersionString -string 151.0.7922.99 \
  "$CONTENTS/Info.plist"
plutil -insert NSBonjourServices -json '["_googlecast._tcp"]' \
  "$CONTENTS/Info.plist"
plutil -insert CFBundleURLTypes -json \
  '[{"CFBundleURLSchemes":["http","https","cmux"]}]' "$CONTENTS/Info.plist"
plutil -insert ASWebAuthenticationSessionWebBrowserSupportCapabilities -json \
  '{"IsSupported":true,"EphemeralBrowserSessionIsSupported":true,"CallbackURLMatchingIsSupported":true}' \
  "$CONTENTS/Info.plist"

plutil -create xml1 "$FRAMEWORK/Resources/Info.plist"
plutil -insert CFBundleExecutable -string "cmux Framework" \
  "$FRAMEWORK/Resources/Info.plist"
plutil -insert CFBundleIdentifier -string com.cmux.app.nightly.framework \
  "$FRAMEWORK/Resources/Info.plist"
plutil -insert CFBundlePackageType -string FMWK \
  "$FRAMEWORK/Resources/Info.plist"
plutil -insert CFBundleVersion -string 151.0.7922.99 \
  "$FRAMEWORK/Resources/Info.plist"
ln -s 1 "$CONTENTS/Frameworks/cmux Framework.framework/Versions/Current"
ln -s "Versions/Current/cmux Framework" \
  "$CONTENTS/Frameworks/cmux Framework.framework/cmux Framework"
ln -s Versions/Current/Resources \
  "$CONTENTS/Frameworks/cmux Framework.framework/Resources"

cat > "$TMP/fixture.c" <<'C'
#include <stdio.h>
int main(void) {
  puts("7WLXT3NR37.com.cmux.app.nightly.webauthn");
  puts("7WLXT3NR37.com.cmux.app.nightly.webauthn-uvk");
  return 0;
}
C
xcrun clang -arch arm64 "$TMP/fixture.c" -o "$CONTENTS/MacOS/cmux"
xcrun clang -arch arm64 -dynamiclib "$TMP/fixture.c" \
  -o "$FRAMEWORK/cmux Framework"
printf '%s\n' \
  'https://github.com/manaflow-ai/cmux-v2/releases/download/nightly/update.json' \
  > "$CONTENTS/Resources/cmux-update-feed-url"

STRINGS="$CONTENTS/Resources/en.lproj/InfoPlist.strings"
plutil -create xml1 "$STRINGS"
plutil -insert NSLocalNetworkUsageDescription -string \
  "Allow cmux to discover Cast devices." "$STRINGS"
plutil -insert "Chromium Shortcut" -string "cmux Shortcut" "$STRINGS"
plutil -insert NSWebBrowserPublicKeyCredentialUsageDescription -string \
  "Allow cmux to use passkeys." "$STRINGS"
plutil -insert CFBundleGetInfoString -string \
  "cmux based on Chromium Authors" "$STRINGS"

codesign --force --deep --sign - "$APP" >/dev/null
CMUX_RELEASE_APP="$APP" \
CMUX_RELEASE_VERSION=151.0.7922.99 \
CMUX_RELEASE_CHANNEL=nightly \
CMUX_RELEASE_OUTPUT="$TMP/release" \
CMUX_ALLOW_UNTRUSTED_MAC_RELEASE=1 \
  "$ROOT/scripts/build-release-macos.sh"

test -f "$TMP/release/cmux-macos-arm64.zip"
test -f "$TMP/release/cmux-macos-arm64.dmg"
unzip -Z1 "$TMP/release/cmux-macos-arm64.zip" > "$TMP/zip-contents.txt"
grep -Fqx 'cmux-browser.app/Contents/Resources/cmux-update-feed-url' \
  "$TMP/zip-contents.txt"
mkdir "$TMP/extracted"
ditto -x -k "$TMP/release/cmux-macos-arm64.zip" "$TMP/extracted"
codesign --verify --deep --strict "$TMP/extracted/cmux-browser.app"
test -x "$TMP/extracted/cmux-browser.app/Contents/MacOS/cmux"
test "$(
  tr -d '\r\n' \
    < "$TMP/extracted/cmux-browser.app/Contents/Resources/cmux-update-feed-url"
)" = \
  'https://github.com/manaflow-ai/cmux-v2/releases/download/nightly/update.json'
mkdir "$MOUNT"
hdiutil attach -readonly -nobrowse -mountpoint "$MOUNT" \
  "$TMP/release/cmux-macos-arm64.dmg" >/dev/null
MOUNTED=1
test -d "$MOUNT/cmux-browser.app"
test -f "$MOUNT/cmux-browser.app/Contents/Info.plist"
test ! -d "$MOUNT/Contents"
hdiutil detach "$MOUNT" -quiet
MOUNTED=0

printf '%s\n' 'https://updates.invalid.example/nightly.json' \
  > "$CONTENTS/Resources/cmux-update-feed-url"
codesign --force --deep --sign - "$APP" >/dev/null
if CMUX_RELEASE_APP="$APP" \
    CMUX_RELEASE_VERSION=151.0.7922.99 \
    CMUX_RELEASE_CHANNEL=nightly \
    CMUX_RELEASE_OUTPUT="$TMP/rejected" \
    CMUX_ALLOW_UNTRUSTED_MAC_RELEASE=1 \
      "$ROOT/scripts/build-release-macos.sh" \
      >"$TMP/rejected.out" 2>&1; then
  echo "error: macOS packager accepted the wrong nightly feed" >&2
  exit 1
fi
grep -Fq 'does not contain the cmux-v2 nightly feed' "$TMP/rejected.out"

echo "macOS release packaging tests passed"
