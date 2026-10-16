#!/bin/bash
# Verify that a built macOS app's packaged and compiled identities agree.
set -euo pipefail

APP="${1:-}"
EXPECTED_BUNDLE_ID="${2:-}"

if [ -z "$APP" ]; then
  echo "usage: scripts/verify-build-bundle-identity.sh <app> [expected-bundle-id]" >&2
  exit 2
fi

PLIST="$APP/Contents/Info.plist"
if [ ! -f "$PLIST" ]; then
  echo "error: missing built app metadata: $PLIST" >&2
  exit 1
fi

BUILT_BUNDLE_ID="$(
  /usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' "$PLIST" 2>/dev/null
)" || {
  echo "error: built app has no CFBundleIdentifier: $PLIST" >&2
  exit 1
}

if [[ ! "$BUILT_BUNDLE_ID" =~ ^com\.cmux\.app(\.[A-Za-z0-9-]+)*$ ]]; then
  echo "error: unsupported compiled cmux bundle ID: $BUILT_BUNDLE_ID" >&2
  exit 1
fi

if [ -n "$EXPECTED_BUNDLE_ID" ] &&
    [ "$EXPECTED_BUNDLE_ID" != "$BUILT_BUNDLE_ID" ]; then
  echo "error: requested deploy bundle ID $EXPECTED_BUNDLE_ID does not match compiled bundle ID $BUILT_BUNDLE_ID" >&2
  echo "error: rebuild with CMUX_MAC_BUNDLE_ID=$EXPECTED_BUNDLE_ID; deploy.sh will not rewrite a compiled app identity" >&2
  exit 1
fi

BUILT_EXECUTABLE="$(
  /usr/libexec/PlistBuddy -c 'Print :CFBundleExecutable' "$PLIST" 2>/dev/null
)" || {
  echo "error: built app has no CFBundleExecutable: $PLIST" >&2
  exit 1
}
if [ "$BUILT_EXECUTABLE" != "cmux" ]; then
  echo "error: built app executable is $BUILT_EXECUTABLE; expected cmux" >&2
  echo "error: rebuild after applying cmux product branding; deploy.sh will not wrap or rename an unbranded executable" >&2
  exit 1
fi
if [ ! -x "$APP/Contents/MacOS/$BUILT_EXECUTABLE" ]; then
  echo "error: built app executable is missing or not executable: $APP/Contents/MacOS/$BUILT_EXECUTABLE" >&2
  exit 1
fi

# The retired dogfood workflow renamed Chromium to Chromium.real and exec'd it
# through a profile launcher. macOS then attributed Local Network access to the
# metadata-less forwarding image. Profile isolation now happens inside Chromium
# via CrProductDirName; no forwarding image may ship anywhere in the bundle.
FORWARDING_EXECUTABLE="$(
  find "$APP/Contents" \
    \( -type f -o -type l \) -name '*.real' -print -quit 2>/dev/null || true
)"
if [ -n "$FORWARDING_EXECUTABLE" ]; then
  echo "error: built app contains unsupported forwarding executable: $FORWARDING_EXECUTABLE" >&2
  exit 1
fi

UNBRANDED_NESTED_BUNDLE="$(
  find "$APP/Contents/Frameworks" \
    \( -name 'Chromium Framework.framework' \
       -o -name 'Chromium Helper*.app' \) \
    -print -quit 2>/dev/null || true
)"
if [ -n "$UNBRANDED_NESTED_BUNDLE" ]; then
  echo "error: built app contains an unbranded nested bundle: $UNBRANDED_NESTED_BUNDLE" >&2
  exit 1
fi

FRAMEWORK_DIR="$APP/Contents/Frameworks/cmux Framework.framework"
FRAMEWORK_PLIST="$(
  find "$FRAMEWORK_DIR/Versions" -path '*/Resources/Info.plist' \
    -type f -print -quit 2>/dev/null || true
)"
if [ -z "$FRAMEWORK_PLIST" ]; then
  echo "error: could not locate the built cmux Framework metadata in $APP" >&2
  exit 1
fi
FRAMEWORK_EXECUTABLE="$(
  /usr/libexec/PlistBuddy -c 'Print :CFBundleExecutable' \
    "$FRAMEWORK_PLIST" 2>/dev/null
)" || {
  echo "error: built framework has no CFBundleExecutable: $FRAMEWORK_PLIST" >&2
  exit 1
}
if [ "$FRAMEWORK_EXECUTABLE" != "cmux Framework" ]; then
  echo "error: built framework executable is $FRAMEWORK_EXECUTABLE; expected cmux Framework" >&2
  exit 1
fi
FRAMEWORK_VERSION_DIR="$(dirname "$(dirname "$FRAMEWORK_PLIST")")"
FRAMEWORK_BINARY="$FRAMEWORK_VERSION_DIR/$FRAMEWORK_EXECUTABLE"
if [ ! -x "$FRAMEWORK_BINARY" ]; then
  echo "error: built framework executable is missing or not executable: $FRAMEWORK_BINARY" >&2
  exit 1
fi

while IFS= read -r HELPER_PLIST; do
  HELPER_EXECUTABLE="$(
    /usr/libexec/PlistBuddy -c 'Print :CFBundleExecutable' \
      "$HELPER_PLIST" 2>/dev/null
  )" || {
    echo "error: built helper has no CFBundleExecutable: $HELPER_PLIST" >&2
    exit 1
  }
  case "$HELPER_EXECUTABLE" in
    "cmux Helper"|"cmux Helper ("*")") ;;
    *)
      echo "error: built helper executable is $HELPER_EXECUTABLE; expected a cmux Helper identity" >&2
      exit 1
      ;;
  esac
  HELPER_CONTENTS="$(dirname "$HELPER_PLIST")"
  HELPER_APP="$(dirname "$HELPER_CONTENTS")"
  if [ "$(basename "$HELPER_APP")" != "$HELPER_EXECUTABLE.app" ]; then
    echo "error: built helper bundle name does not match its executable: $HELPER_APP -> $HELPER_EXECUTABLE" >&2
    exit 1
  fi
  if [ ! -x "$HELPER_CONTENTS/MacOS/$HELPER_EXECUTABLE" ]; then
    echo "error: built helper executable is missing or not executable: $HELPER_CONTENTS/MacOS/$HELPER_EXECUTABLE" >&2
    exit 1
  fi
done < <(
  find "$FRAMEWORK_DIR" -path '*/Helpers/*.app/Contents/Info.plist' \
    -type f -print 2>/dev/null
)

for ACCESS_GROUP in \
    "7WLXT3NR37.$BUILT_BUNDLE_ID.webauthn" \
    "7WLXT3NR37.$BUILT_BUNDLE_ID.webauthn-uvk"; do
  if ! grep -Fxq "$ACCESS_GROUP" < <(strings "$FRAMEWORK_BINARY"); then
    echo "error: compiled framework is missing WebAuthn identity $ACCESS_GROUP" >&2
    exit 1
  fi
done

printf '%s\n' "$BUILT_BUNDLE_ID"
