#!/bin/bash
# Validate cmux's checked-in macOS identity and, when supplied, a packaged app.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

require_text() {
  local file="$1"
  local text="$2"
  grep -Fq "$text" "$file" || {
    echo "error: $file is missing: $text" >&2
    exit 1
  }
}

IDENTITY_PATCH="patches/macos_product_identity.py"
require_text "$IDENTITY_PATCH" 'DEFAULT_MAC_BUNDLE_ID = "com.cmux.app"'
require_text "$IDENTITY_PATCH" 'CMUX_MAC_BUNDLE_ID'
require_text "$IDENTITY_PATCH" '"PRODUCT_FULLNAME": "cmux"'
require_text "$IDENTITY_PATCH" '"PRODUCT_SHORTNAME": "cmux"'
require_text "$IDENTITY_PATCH" '"MAC_TEAM_ID": "7WLXT3NR37"'
require_text "$IDENTITY_PATCH" 'scheme = '\''cmux'\'''
require_text "$IDENTITY_PATCH" 'return "cmux";'

APP="${1:-}"
if [ -z "$APP" ]; then
  echo "macOS platform metadata source checks passed"
  exit 0
fi

PLIST="$APP/Contents/Info.plist"
[ -f "$PLIST" ] || {
  echo "error: missing packaged app metadata: $PLIST" >&2
  exit 1
}

EXPECTED_BUNDLE_ID="${CMUX_EXPECTED_BUNDLE_ID:-com.cmux.app}"
"$ROOT/scripts/verify-build-bundle-identity.sh" \
  "$APP" "$EXPECTED_BUNDLE_ID" >/dev/null

plist_value() {
  /usr/libexec/PlistBuddy -c "Print :$1" "$PLIST"
}

[ "$(plist_value CFBundleIdentifier)" = "$EXPECTED_BUNDLE_ID" ] || {
  echo "error: packaged bundle ID does not match $EXPECTED_BUNDLE_ID" >&2
  exit 1
}
[ "$(plist_value CFBundleExecutable)" = "cmux" ] || {
  echo "error: packaged executable is not cmux" >&2
  exit 1
}
for name_key in CFBundleName CFBundleDisplayName; do
  case "$(plist_value "$name_key")" in
    cmux|cmux\ ▸\ *) ;;
    *)
      echo "error: packaged $name_key is not branded as cmux" >&2
      exit 1
      ;;
  esac
done

# cmux retains Chromium's normal Cast support. Both the Bonjour declaration and
# a localized Local Network reason must survive branding and deployment.
BONJOUR_SERVICES="$(plist_value NSBonjourServices)"
printf '%s\n' "$BONJOUR_SERVICES" | \
    grep -Eq '^[[:space:]]*_googlecast\._tcp$' || {
  echo "error: packaged app is missing Google Cast Bonjour discovery" >&2
  exit 1
}

URL_TYPES="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleURLTypes' "$PLIST")"
for scheme in http https cmux; do
  printf '%s\n' "$URL_TYPES" | grep -Eq "^[[:space:]]*$scheme$" || {
    echo "error: packaged app is missing $scheme URL handling" >&2
    exit 1
  }
done

for capability in IsSupported EphemeralBrowserSessionIsSupported \
    CallbackURLMatchingIsSupported; do
  [ "$(plist_value \
      ASWebAuthenticationSessionWebBrowserSupportCapabilities:$capability)" = \
      "true" ] || {
    echo "error: packaged app is missing web-auth capability $capability" >&2
    exit 1
  }
done

ENGLISH_STRINGS="$APP/Contents/Resources/en.lproj/InfoPlist.strings"
[ -f "$ENGLISH_STRINGS" ] || {
  echo "error: packaged app is missing English InfoPlist.strings" >&2
  exit 1
}
LOCAL_NETWORK_REASON="$(
  plutil -extract NSLocalNetworkUsageDescription raw -o - \
    "$ENGLISH_STRINGS" 2>/dev/null || true
)"
[ -n "$LOCAL_NETWORK_REASON" ] || {
  echo "error: packaged app is missing the Local Network reason required by Cast" >&2
  exit 1
}
[ "$(
  plutil -extract "Chromium Shortcut" raw -o - \
    "$ENGLISH_STRINGS" 2>/dev/null || true
)" = "cmux Shortcut" ] || {
  echo "error: packaged app shortcut description is not branded as cmux" >&2
  exit 1
}
require_text "$ENGLISH_STRINGS" \
  'NSWebBrowserPublicKeyCredentialUsageDescription'
if ! plutil -extract CFBundleGetInfoString raw -o - "$ENGLISH_STRINGS" | \
    grep -Fq 'Chromium Authors'; then
  echo "error: packaged about string lost the Chromium attribution" >&2
  exit 1
fi

while IFS= read -r -d '' strings_file; do
  about="$(
    plutil -extract CFBundleGetInfoString raw -o - "$strings_file" \
      2>/dev/null || true
  )"
  if [ -n "$about" ] &&
      ! printf '%s\n' "$about" | grep -Eq '^cmux([[:space:]]|$)'; then
    echo "error: $strings_file:CFBundleGetInfoString is not branded as cmux" >&2
    exit 1
  fi
  for key in "Chromium Shortcut" NSAudioCaptureUsageDescription \
      NSBluetoothAlwaysUsageDescription NSBluetoothPeripheralUsageDescription \
      NSCameraUsageDescription NSLocationUsageDescription \
      NSMicrophoneUsageDescription \
      NSWebBrowserPublicKeyCredentialUsageDescription; do
    value="$(
      plutil -extract "$key" raw -o - "$strings_file" 2>/dev/null || true
    )"
    if [ -n "$value" ] && ! printf '%s\n' "$value" | grep -Fq cmux; then
      echo "error: $strings_file:$key is not branded as cmux" >&2
      exit 1
    fi
  done
done < <(
  find "$APP/Contents/Resources" -path '*/InfoPlist.strings' \
    -type f -print0
)

codesign --verify --deep --strict "$APP"
echo "macOS platform metadata packaged-app checks passed: $APP"
