#!/usr/bin/env bash
# Resolve the active non-component Chromium framework from its compiled
# product identity. A cmux build renames both the app executable and framework;
# deployment must not fall back to Chromium's upstream bundle spelling.
set -euo pipefail

APP="${1:-}"
if [ -z "$APP" ]; then
  echo "usage: scripts/resolve-chromium-runtime-image.sh <app>" >&2
  exit 2
fi

PLIST="$APP/Contents/Info.plist"
if [ ! -f "$PLIST" ]; then
  echo "error: missing app metadata while resolving Chromium runtime: $PLIST" >&2
  exit 1
fi

APP_EXECUTABLE="$(
  /usr/libexec/PlistBuddy -c 'Print :CFBundleExecutable' "$PLIST" 2>/dev/null
)" || {
  echo "error: app has no CFBundleExecutable while resolving Chromium runtime: $PLIST" >&2
  exit 1
}
case "$APP_EXECUTABLE" in
  ""|*/*)
    echo "error: unsafe compiled app executable while resolving Chromium runtime: $APP_EXECUTABLE" >&2
    exit 1
    ;;
esac

FRAMEWORK_EXECUTABLE="$APP_EXECUTABLE Framework"
RUNTIME_IMAGE="$APP/Contents/Frameworks/$FRAMEWORK_EXECUTABLE.framework/Versions/Current/$FRAMEWORK_EXECUTABLE"
if [ ! -f "$RUNTIME_IMAGE" ]; then
  echo "error: compiled Chromium runtime image is not a regular file: $RUNTIME_IMAGE" >&2
  exit 1
fi
printf '%s\n' "$RUNTIME_IMAGE"
