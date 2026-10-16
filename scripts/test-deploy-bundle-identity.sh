#!/bin/bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
GUARD="$ROOT/scripts/verify-build-bundle-identity.sh"
DEPLOY="$ROOT/scripts/deploy.sh"
PLATFORM_VERIFY="$ROOT/scripts/verify-platform-metadata.sh"
UPDATER_DOC="$ROOT/docs/auto-updates.md"
UPDATER_TEST="$ROOT/scripts/test-updater.py"
XCUITEST_RUN="$ROOT/tools/xcuitest-run.sh"
XCUITEST_SOURCE="$ROOT/tests/xcuitest/Sources/CmuxKeyboardUITests.swift"
AGENT_GUIDE="$ROOT/CLAUDE.md"
ENTITLEMENTS_DOC="$ROOT/docs/entitlements.md"
FIXTURE_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/cmux-deploy-identity-test.XXXXXX")"
trap 'rm -rf "$FIXTURE_ROOT"' EXIT

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

make_fixture() {
  local app="$1"
  local bundle_id="$2"
  local framework_binary="$app/Contents/Frameworks/cmux Framework.framework/Versions/1/cmux Framework"
  local framework_plist="$app/Contents/Frameworks/cmux Framework.framework/Versions/1/Resources/Info.plist"
  local app_executable="$app/Contents/MacOS/cmux"

  mkdir -p "$(dirname "$framework_binary")" "$(dirname "$framework_plist")" \
    "$(dirname "$app_executable")"
  plutil -create xml1 "$app/Contents/Info.plist"
  plutil -insert CFBundleIdentifier -string "$bundle_id" \
    "$app/Contents/Info.plist"
  plutil -insert CFBundleName -string cmux "$app/Contents/Info.plist"
  plutil -insert CFBundleDisplayName -string cmux "$app/Contents/Info.plist"
  plutil -insert CFBundleExecutable -string cmux "$app/Contents/Info.plist"
  plutil -create xml1 "$framework_plist"
  plutil -insert CFBundleExecutable -string "cmux Framework" "$framework_plist"
  printf '#!/bin/sh\nexit 0\n' >"$app_executable"
  chmod +x "$app_executable"
  printf '%s\n' \
    fixture \
    "7WLXT3NR37.$bundle_id.webauthn" \
    "7WLXT3NR37.$bundle_id.webauthn-uvk" \
    >"$framework_binary"
  chmod +x "$framework_binary"
}

expect_failure() {
  local expected_message="$1"
  shift
  if "$GUARD" "$@" >"$FIXTURE_ROOT/guard.out" 2>&1; then
    fail "identity guard unexpectedly accepted: $*"
  fi
  grep -Fq "$expected_message" "$FIXTURE_ROOT/guard.out" ||
    fail "identity guard did not report '$expected_message'"
}

# Deployment validates the compiled identity and never mutates the bundle ID
# after Chromium generated it from MAC_BUNDLE_ID.
grep -Fq '"$ROOT/scripts/verify-build-bundle-identity.sh"' "$DEPLOY"
grep -Fq 'scripts/resolve-chromium-runtime-image.sh "$DEST"' "$DEPLOY"
if grep -Fq \
    'Contents/Frameworks/Chromium Framework.framework/Versions/Current/Chromium Framework' \
    "$DEPLOY"; then
  fail "deploy.sh hard-codes the retired unbranded runtime framework"
fi
grep -Fq \
  'CMUX_EXPECTED_BUNDLE_ID:-${CMUX_MAC_BUNDLE_ID:-com.cmux.app}' \
  "$DEPLOY"
grep -Fq '"$DEST" "$EXPECTED_BUNDLE_ID"' "$DEPLOY"
grep -Fq 'BUILT_APP_NAME="${CMUX_BUILT_APP_NAME:-cmux.app}"' "$DEPLOY"
grep -Fq 'NAME="${CMUX_DISPLAY_NAME:-cmux ▸ $TAG}"' "$DEPLOY"
grep -Fq 'STAGING_ROOT="$(' "$DEPLOY"
grep -Fq 'STAGING_DEST="$STAGING_ROOT/$DEST_BASENAME"' "$DEPLOY"
grep -Fq 'promote_staged_app' "$DEPLOY"
grep -Fq '"$ROOT/scripts/verify-platform-metadata.sh" "$FINAL_DEST"' "$DEPLOY"
grep -Fq 'CMUX_EXPECTED_BUNDLE_ID="$BUILT_BUNDLE_ID"' "$DEPLOY"
grep -Fq 'COMPILED_MAC_TEAM_ID="7WLXT3NR37"' "$DEPLOY"
grep -Fq 'if [ "$team_id" != "$COMPILED_MAC_TEAM_ID" ]; then' "$DEPLOY"
grep -Fq 'if [ "$SIGNED_TEAM_ID" != "$COMPILED_MAC_TEAM_ID" ]; then' "$DEPLOY"
grep -Fq 'CMUX_INSTALLED_UPDATE_FEED_URL' "$DEPLOY"
grep -Fq 'UPDATE_FEED_FILE="$DEST/Contents/Resources/cmux-update-feed-url"' \
  "$DEPLOY"
grep -Fq 'CMUX_INSTALLED_UPDATE_FEED_URL must use https://' "$DEPLOY"
grep -Fq '"$ROOT/scripts/verify-build-bundle-identity.sh"' "$PLATFORM_VERIFY"
grep -Fq 'NSBonjourServices' "$PLATFORM_VERIFY"
grep -Fq 'NSLocalNetworkUsageDescription' "$PLATFORM_VERIFY"
grep -Fq \
  'cmux-browser.app,Contents/MacOS/cmux' "$UPDATER_DOC"
grep -Fq \
  '"mac-arm64": ("cmux-browser.app", "Contents/MacOS/cmux")' "$UPDATER_TEST"
grep -Fq \
  'plutil -extract CFBundleExecutable raw -o -' "$XCUITEST_RUN"
grep -Fq \
  'LEGACY_APP_EXECUTABLE="$APP_PATH/Contents/MacOS/$LEGACY_APP_EXECUTABLE_NAME"' \
  "$XCUITEST_RUN"
grep -Fq \
  'APP_BUNDLE_ID="$(plutil -extract CFBundleIdentifier raw -o -' \
  "$XCUITEST_RUN"
grep -Fq \
  '[ "$CMUX_XCUI_BUNDLE_ID" != "$APP_BUNDLE_ID" ]; }; then' "$XCUITEST_RUN"
grep -Fq \
  'XCUIApplication(bundleIdentifier: bundleIdentifier)' "$XCUITEST_SOURCE"
grep -Fq \
  'CMUX_XCUI_BUNDLE_ID"] ?? "com.cmux.app"' "$XCUITEST_SOURCE"
grep -Fq \
  'frontmostApplication?.processIdentifier == expectedPID' "$XCUITEST_SOURCE"
grep -Fq 'Follow [`AGENTS.md`](AGENTS.md)' "$AGENT_GUIDE"
grep -Fq 'out/Release/cmux.app' "$AGENT_GUIDE"
grep -Fq '`7WLXT3NR37`' "$ENTITLEMENTS_DOC"
grep -Fq 'CMUX_SIGN_TEAM_ID="7WLXT3NR37"' "$ENTITLEMENTS_DOC"
if grep -Fq \
    'cmux-browser.app/Contents/MacOS/Chromium' "$UPDATER_DOC" ||
    grep -Fq \
      'cmux-browser.app/Contents/MacOS/Chromium' "$XCUITEST_RUN" ||
    grep -Fq \
      'XCUIApplication(bundleIdentifier: "org.chromium.Chromium")' \
      "$XCUITEST_SOURCE"; then
  fail "a downstream macOS consumer still targets the retired executable"
fi
if grep -Fq 'out/Release/Chromium.app' "$AGENT_GUIDE" ||
    grep -Fq 'TEAMID1234' "$ENTITLEMENTS_DOC"; then
  fail "an operational guide still describes the retired packaging identity"
fi
if grep -Eq 'plutil .*CFBundleIdentifier|CFBundleIdentifier .*plutil' "$DEPLOY"; then
  fail "deploy.sh must not rewrite CFBundleIdentifier"
fi
if grep -Fq 'cmux-profile-launcher' "$DEPLOY" ||
    grep -Fq 'Chromium.real' "$DEPLOY"; then
  fail "deploy.sh must not install a forwarding executable"
fi
[ ! -e "$ROOT/scripts/cmux-profile-launcher.c" ] ||
  fail "the retired forwarding launcher source is still present"
bash "$ROOT/scripts/test-macos-localized-product-name.sh"

GOOD_ID="com.cmux.app.dogfood.build-helium-sidebar"
GOOD_APP="$FIXTURE_ROOT/good.app"
make_fixture "$GOOD_APP" "$GOOD_ID"

[ "$("$GUARD" "$GOOD_APP")" = "$GOOD_ID" ] ||
  fail "identity guard did not return the compiled identity"
[ "$("$GUARD" "$GOOD_APP" "$GOOD_ID")" = "$GOOD_ID" ] ||
  fail "identity guard rejected a matching dogfood identity"

expect_failure \
  "does not match compiled bundle ID $GOOD_ID" \
  "$GOOD_APP" "com.cmux.app.dogfood.other"

UNBRANDED_APP="$FIXTURE_ROOT/unbranded.app"
make_fixture "$UNBRANDED_APP" "$GOOD_ID"
mv "$UNBRANDED_APP/Contents/MacOS/cmux" \
  "$UNBRANDED_APP/Contents/MacOS/Chromium"
plutil -replace CFBundleExecutable -string Chromium \
  "$UNBRANDED_APP/Contents/Info.plist"
expect_failure \
  "built app executable is Chromium; expected cmux" \
  "$UNBRANDED_APP" "$GOOD_ID"

MISSING_EXECUTABLE_APP="$FIXTURE_ROOT/missing-executable.app"
make_fixture "$MISSING_EXECUTABLE_APP" "$GOOD_ID"
rm "$MISSING_EXECUTABLE_APP/Contents/MacOS/cmux"
expect_failure \
  "built app executable is missing or not executable" \
  "$MISSING_EXECUTABLE_APP" "$GOOD_ID"

FORWARDER_APP="$FIXTURE_ROOT/forwarder.app"
make_fixture "$FORWARDER_APP" "$GOOD_ID"
printf '#!/bin/sh\nexit 0\n' \
  >"$FORWARDER_APP/Contents/MacOS/Chromium.real"
chmod +x "$FORWARDER_APP/Contents/MacOS/Chromium.real"
expect_failure \
  "built app contains unsupported forwarding executable" \
  "$FORWARDER_APP" "$GOOD_ID"

UNBRANDED_FRAMEWORK_APP="$FIXTURE_ROOT/unbranded-framework.app"
make_fixture "$UNBRANDED_FRAMEWORK_APP" "$GOOD_ID"
FRAMEWORK_VERSION="$UNBRANDED_FRAMEWORK_APP/Contents/Frameworks/cmux Framework.framework/Versions/1"
mv "$FRAMEWORK_VERSION/cmux Framework" "$FRAMEWORK_VERSION/Chromium Framework"
plutil -replace CFBundleExecutable -string "Chromium Framework" \
  "$FRAMEWORK_VERSION/Resources/Info.plist"
expect_failure \
  "built framework executable is Chromium Framework; expected cmux Framework" \
  "$UNBRANDED_FRAMEWORK_APP" "$GOOD_ID"

UNBRANDED_HELPER_APP="$FIXTURE_ROOT/unbranded-helper.app"
make_fixture "$UNBRANDED_HELPER_APP" "$GOOD_ID"
HELPER_CONTENTS="$UNBRANDED_HELPER_APP/Contents/Frameworks/cmux Framework.framework/Versions/1/Helpers/cmux Helper.app/Contents"
mkdir -p "$HELPER_CONTENTS/MacOS"
plutil -create xml1 "$HELPER_CONTENTS/Info.plist"
plutil -insert CFBundleExecutable -string "Chromium Helper" \
  "$HELPER_CONTENTS/Info.plist"
printf '#!/bin/sh\nexit 0\n' >"$HELPER_CONTENTS/MacOS/Chromium Helper"
chmod +x "$HELPER_CONTENTS/MacOS/Chromium Helper"
expect_failure \
  "built helper executable is Chromium Helper; expected a cmux Helper identity" \
  "$UNBRANDED_HELPER_APP" "$GOOD_ID"

MISSING_GROUP_APP="$FIXTURE_ROOT/missing-group.app"
make_fixture "$MISSING_GROUP_APP" "$GOOD_ID"
printf '%s\n' fixture "7WLXT3NR37.$GOOD_ID.webauthn" \
  >"$MISSING_GROUP_APP/Contents/Frameworks/cmux Framework.framework/Versions/1/cmux Framework"
expect_failure \
  "compiled framework is missing WebAuthn identity" \
  "$MISSING_GROUP_APP" "$GOOD_ID"

BAD_ID_APP="$FIXTURE_ROOT/bad-id.app"
make_fixture "$BAD_ID_APP" "com.example.cmux"
expect_failure "unsupported compiled cmux bundle ID" "$BAD_ID_APP"

NO_FRAMEWORK_APP="$FIXTURE_ROOT/no-framework.app"
mkdir -p "$NO_FRAMEWORK_APP/Contents/MacOS"
plutil -create xml1 "$NO_FRAMEWORK_APP/Contents/Info.plist"
plutil -insert CFBundleIdentifier -string com.cmux.app \
  "$NO_FRAMEWORK_APP/Contents/Info.plist"
plutil -insert CFBundleExecutable -string cmux \
  "$NO_FRAMEWORK_APP/Contents/Info.plist"
printf '#!/bin/sh\nexit 0\n' >"$NO_FRAMEWORK_APP/Contents/MacOS/cmux"
chmod +x "$NO_FRAMEWORK_APP/Contents/MacOS/cmux"
expect_failure \
  "could not locate the built cmux Framework metadata" \
  "$NO_FRAMEWORK_APP"

# A plain stable deployment must reject a stale channel build without mutating
# the existing stable destination. An explicit channel build supplies its
# expected compiled identity.
STALE_CHANNEL_SOURCE="$FIXTURE_ROOT/stale-channel-source.app"
STABLE_DEST="$FIXTURE_ROOT/existing-stable.app"
make_fixture "$STALE_CHANNEL_SOURCE" "$GOOD_ID"
make_fixture "$STABLE_DEST" "com.cmux.app"
printf 'keep this stable app\n' >"$STABLE_DEST/stable-marker"
STABLE_HASH_BEFORE="$(
  find "$STABLE_DEST" -type f -exec shasum -a 256 {} + |
    sort | shasum -a 256 | awk '{print $1}'
)"
mkdir -p "$FIXTURE_ROOT/extensions/ublock" "$FIXTURE_ROOT/bin"
printf '{}\n' >"$FIXTURE_ROOT/extensions/ublock/manifest.json"
printf '%s\n' \
  '#!/bin/bash' \
  'set -e' \
  'if [ ! -e "$CMUX_TEST_RSYNC_MARKER" ]; then' \
  '  : >"$CMUX_TEST_RSYNC_MARKER"' \
  '  destination="${!#}"' \
  '  cp -R "$CMUX_TEST_RSYNC_SOURCE/." "$destination/"' \
  'fi' \
  >"$FIXTURE_ROOT/bin/rsync"
chmod +x "$FIXTURE_ROOT/bin/rsync"
if PATH="$FIXTURE_ROOT/bin:$PATH" \
    DEST="$STABLE_DEST" \
    CMUX_TEST_RSYNC_SOURCE="$STALE_CHANNEL_SOURCE" \
    CMUX_TEST_RSYNC_MARKER="$FIXTURE_ROOT/stale-rsync.marker" \
    CMUX_EXTENSIONS_DIR="$FIXTURE_ROOT/extensions" \
    "$DEPLOY" >"$FIXTURE_ROOT/stale-deploy.out" 2>&1; then
  fail "default deploy accepted a stale channel-identity build"
fi
grep -Fq \
  "requested deploy bundle ID com.cmux.app does not match compiled bundle ID $GOOD_ID" \
  "$FIXTURE_ROOT/stale-deploy.out" ||
  fail "default deploy did not enforce the stable bundle identity"
STABLE_HASH_AFTER="$(
  find "$STABLE_DEST" -type f -exec shasum -a 256 {} + |
    sort | shasum -a 256 | awk '{print $1}'
)"
[ "$STABLE_HASH_BEFORE" = "$STABLE_HASH_AFTER" ] ||
  fail "rejected channel build mutated the existing stable destination"
[ -f "$STABLE_DEST/stable-marker" ] ||
  fail "rejected channel build removed the existing stable destination"

# Tier-2 deployment must reject a signing team that cannot access the
# framework's compiled WebAuthn groups. The fake rsync keeps this fixture local;
# the mismatch is rejected before any signing command runs.
WRONG_TEAM_SOURCE="$FIXTURE_ROOT/wrong-team-source.app"
WRONG_TEAM_DEST="$FIXTURE_ROOT/wrong-team-dest.app"
make_fixture "$WRONG_TEAM_SOURCE" "$GOOD_ID"
mkdir -p "$WRONG_TEAM_SOURCE/Contents/Resources"
if PATH="$FIXTURE_ROOT/bin:$PATH" \
    DEST="$WRONG_TEAM_DEST" \
    CMUX_TEST_RSYNC_SOURCE="$WRONG_TEAM_SOURCE" \
    CMUX_TEST_RSYNC_MARKER="$FIXTURE_ROOT/wrong-team-rsync.marker" \
    CMUX_EXPECTED_BUNDLE_ID="$GOOD_ID" \
    CMUX_EXTENSIONS_DIR="$FIXTURE_ROOT/extensions" \
    CMUX_SIGN_IDENTITY='Developer ID Application: Other, Inc. (AAAAAAAAAA)' \
    CMUX_SIGN_TEAM_ID='AAAAAAAAAA' \
    "$DEPLOY" >"$FIXTURE_ROOT/deploy.out" 2>&1; then
  fail "deploy.sh accepted a signing team that differs from compiled WebAuthn"
fi
if ! grep -Fq \
    'signing Team ID AAAAAAAAAA does not match compiled WebAuthn Team ID 7WLXT3NR37' \
    "$FIXTURE_ROOT/deploy.out"; then
  sed -n '1,160p' "$FIXTURE_ROOT/deploy.out" >&2
  fail "deploy.sh did not report the compiled/signing team mismatch"
fi
[ ! -e "$WRONG_TEAM_DEST" ] ||
  fail "rejected signing-team build was promoted into place"

echo "deploy bundle identity tests passed"
