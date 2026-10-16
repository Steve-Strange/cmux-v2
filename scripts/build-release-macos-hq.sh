#!/bin/bash
# Build, Developer-ID sign, notarize, and package macOS through an HQ fleet
# lease. Intended for the registered controller-side GitHub runner.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VERSION="${CMUX_RELEASE_VERSION:?CMUX_RELEASE_VERSION is required}"
CHANNEL="${CMUX_RELEASE_CHANNEL:?CMUX_RELEASE_CHANNEL is required}"
HQ="${CMUX_HQ_BIN:-/Users/cmux-lawrence/fun/cmux-browser-hq/bin/hq}"
OUT="${CMUX_RELEASE_OUTPUT:-$ROOT/release}"
APP="${CMUX_RELEASE_APP:-$OUT/cmux-browser.app}"
AGENT="${CMUX_AGENT_ID:-cmux-nightly-macos}"
JOB="${CMUX_HQ_JOB:-cmux-${CHANNEL}-macos-${VERSION}}"

[ -x "$HQ" ] || { echo "error: HQ controller not found: $HQ" >&2; exit 2; }
[ -n "${CMUX_SIGN_IDENTITY:-}" ] || {
  echo "error: CMUX_SIGN_IDENTITY is required for a public macOS release" >&2
  exit 2
}
[ -f "${CMUX_SIGN_PROVISION_PROFILE:-}" ] || {
  echo "error: CMUX_SIGN_PROVISION_PROFILE is missing" >&2
  exit 2
}
[ -n "${CMUX_NOTARYTOOL_KEYCHAIN_PROFILE:-}" ] || {
  echo "error: CMUX_NOTARYTOOL_KEYCHAIN_PROFILE is required" >&2
  exit 2
}

case "$CHANNEL" in
  stable)
    BUNDLE_ID="com.cmux.app"
    DISPLAY_NAME="cmux Browser"
    PRODUCT_DIR_NAME="cmux Browser"
    INSTALLED_FEED_URL=""
    ;;
  nightly)
    BUNDLE_ID="com.cmux.app.nightly"
    DISPLAY_NAME="cmux Browser Nightly"
    PRODUCT_DIR_NAME="cmux Browser Nightly"
    INSTALLED_FEED_URL="https://github.com/manaflow-ai/cmux-v2/releases/download/nightly/update.json"
    ;;
  *)
    echo "error: invalid release channel: $CHANNEL" >&2
    exit 2
    ;;
esac

mkdir -p "$OUT"
export CMUX_RELEASE_VERSION="$VERSION"
export CMUX_RELEASE_CHANNEL="$CHANNEL"
export CMUX_RELEASE_OUTPUT="$OUT"
export CMUX_RELEASE_APP="$APP"
export CMUX_MAC_BUNDLE_ID="$BUNDLE_ID"
export CMUX_EXPECTED_BUNDLE_ID="$BUNDLE_ID"
export CMUX_DISPLAY_NAME="$DISPLAY_NAME"
export CMUX_PRODUCT_DIR_NAME="$PRODUCT_DIR_NAME"
export CMUX_INSTALLED_UPDATE_FEED_URL="$INSTALLED_FEED_URL"
export CMUX_SIGN_TEAM_ID="7WLXT3NR37"
export CMUX_AGENT_ID="$AGENT"

"$HQ" monitor-build --label "$JOB" -- \
  "$HQ" run \
    --repo "$ROOT" \
    --sync \
    --job "$JOB" \
    --agent "$AGENT" \
    -- \
    bash -c '
      set -euo pipefail
      ./scripts/stamp-release-macos.sh
      ./scripts/build.sh
      DEST="$CMUX_RELEASE_APP" ./scripts/deploy.sh
    '

"$ROOT/scripts/notarize-release-macos.sh"
"$ROOT/scripts/build-release-macos.sh"
