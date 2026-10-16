#!/bin/bash
# Submit a Developer ID-signed app to Apple's notarization service and staple
# the accepted ticket before public packaging.
set -euo pipefail

APP="${CMUX_RELEASE_APP:?CMUX_RELEASE_APP is required}"
PROFILE="${CMUX_NOTARYTOOL_KEYCHAIN_PROFILE:?CMUX_NOTARYTOOL_KEYCHAIN_PROFILE is required}"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/cmux-notarize.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT
SUBMISSION="$WORK/cmux-browser.zip"

[ -d "$APP" ] || { echo "error: app does not exist: $APP" >&2; exit 2; }
codesign --verify --deep --strict "$APP"
codesign -dvvv "$APP" 2>&1 | \
  grep -Fq "Authority=Developer ID Application:" || {
    echo "error: refusing to notarize a non-Developer-ID app" >&2
    exit 1
  }

ditto -c -k --keepParent "$APP" "$SUBMISSION"
xcrun notarytool submit "$SUBMISSION" \
  --keychain-profile "$PROFILE" --wait
xcrun stapler staple "$APP"
xcrun stapler validate "$APP"
spctl --assess --type execute --verbose=4 "$APP"

echo "notarized and stapled: $APP"
