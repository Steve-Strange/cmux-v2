#!/bin/bash
# Pull the freshly built cmux.app off the builder into dist/cmux-browser.app
# and ad-hoc codesign it so macOS will launch it. Run after a successful build
# (scripts/build.sh) and before scripts/run.sh. Set CHROMIUM_SRC to deploy from
# a non-default tree (e.g. CHROMIUM_SRC=cmux-chromium/src for the clean base),
# and DEST to deploy to a non-default bundle path.
set -euo pipefail
. "$(dirname "$0")/builder-transport.sh"
HOST="${CMUX_BUILDER:-ec2-user@aws-m4pro-1}"
SRC="${CHROMIUM_SRC:-/Volumes/recpass123/cmux-ci/cmux-browser-benchmark/src}"
cd "$(dirname "$0")/.."
ROOT="$PWD"

FINAL_DEST="${DEST:-dist/cmux-browser.app}"
BUILT_APP_NAME="${CMUX_BUILT_APP_NAME:-cmux.app}"
ENTITLEMENTS_DIR="$PWD/scripts/entitlements"
SIGN_IDENTITY="${CMUX_SIGN_IDENTITY:--}"
SIGN_TIER="Tier 1 (ad-hoc, unrestricted entitlements)"
APP_ENTITLEMENTS="$ENTITLEMENTS_DIR/cmux-app.plist"
COMPILED_MAC_TEAM_ID="7WLXT3NR37"

if [ -n "${CMUX_SIGN_IDENTITY:-}" ]; then
  SIGN_TIER="Tier 2 (Developer ID, restricted browser entitlements)"
fi

if [ "${CMUX_SIGN_DRY_RUN:-0}" = "1" ]; then
  echo "== codesign dry run =="
  echo "tier: $SIGN_TIER"
  echo "identity: $SIGN_IDENTITY"
  if [ -n "${CMUX_SIGN_IDENTITY:-}" ]; then
    echo "entitlements template: $ENTITLEMENTS_DIR/cmux-app-restricted.plist"
  else
    echo "entitlements: $APP_ENTITLEMENTS"
  fi
  exit 0
fi

DEST_PARENT="$(dirname "$FINAL_DEST")"
DEST_BASENAME="$(basename "$FINAL_DEST")"
mkdir -p "$DEST_PARENT"
STAGING_ROOT="$(
  mktemp -d "$DEST_PARENT/.${DEST_BASENAME}.staging.XXXXXX"
)"
STAGING_DEST="$STAGING_ROOT/$DEST_BASENAME"
mkdir "$STAGING_DEST"
DEST="$STAGING_DEST"
SIGN_WORK="$STAGING_ROOT/signing"
if [ -n "${CMUX_SIGN_IDENTITY:-}" ]; then
  APP_ENTITLEMENTS="$SIGN_WORK/cmux-app-restricted.generated.plist"
fi
BACKUP_ROOT=""
PROMOTED=0

cleanup_deploy() {
  local rc=$?
  set +e
  if [ "$PROMOTED" = "1" ] &&
      { [ -e "$FINAL_DEST" ] || [ -L "$FINAL_DEST" ]; }; then
    rm -rf -- "$FINAL_DEST"
  fi
  if [ -n "$BACKUP_ROOT" ] &&
      { [ -e "$BACKUP_ROOT/previous.app" ] ||
        [ -L "$BACKUP_ROOT/previous.app" ]; }; then
    if [ -e "$FINAL_DEST" ] || [ -L "$FINAL_DEST" ]; then
      rm -rf -- "$FINAL_DEST"
    fi
    mv "$BACKUP_ROOT/previous.app" "$FINAL_DEST"
  fi
  if [ -d "$STAGING_ROOT" ]; then
    rm -rf -- "$STAGING_ROOT"
  fi
  if [ -n "$BACKUP_ROOT" ] && [ -d "$BACKUP_ROOT" ]; then
    rm -rf -- "$BACKUP_ROOT"
  fi
  return "$rc"
}
trap cleanup_deploy EXIT

infer_team_id() {
  if [ -n "${CMUX_SIGN_TEAM_ID:-}" ]; then
    echo "$CMUX_SIGN_TEAM_ID"
    return
  fi

  # Developer ID identities are commonly named:
  #   Developer ID Application: Example, Inc. (TEAMID1234)
  if [[ "$SIGN_IDENTITY" =~ \(([A-Z0-9]{10})\)$ ]]; then
    echo "${BASH_REMATCH[1]}"
  fi
}

validate_signing_team_id() {
  if [ -z "${CMUX_SIGN_IDENTITY:-}" ]; then
    return
  fi

  local team_id
  team_id="$(infer_team_id)"
  if [ -z "$team_id" ]; then
    echo "error: Tier 2 signing needs a Team ID. Set CMUX_SIGN_TEAM_ID, or use a Developer ID identity name ending in (TEAMID)." >&2
    exit 1
  fi
  if [ "$team_id" != "$COMPILED_MAC_TEAM_ID" ]; then
    echo "error: signing Team ID $team_id does not match compiled WebAuthn Team ID $COMPILED_MAC_TEAM_ID" >&2
    exit 1
  fi
}

echo "== rsync $HOST:$SRC/out/Release/$BUILT_APP_NAME -> staging for $FINAL_DEST =="
# --delete prevents stale files from lingering. Skip compression by default:
# Chromium binaries are expensive to deflate and Tailscale is already a fast,
# encrypted transport. Set CMUX_RSYNC_COMPRESS=1 for a bandwidth-limited link.
RSYNC_ARGS=(-a --delete --partial)
if [ "${CMUX_RSYNC_COMPRESS:-0}" = "1" ]; then
  RSYNC_ARGS+=(-z)
fi
# In-place transfer avoids rsync's additional temporary copy inside the staging
# bundle. DEST itself is never touched until the staged app passes verification.
if [ "${CMUX_RSYNC_INPLACE:-0}" = "1" ]; then
  RSYNC_ARGS+=(--inplace)
fi
rsync "${RSYNC_ARGS[@]}" "$HOST:$SRC/out/Release/$BUILT_APP_NAME/" "$DEST/"

# Bundle identity is a Chromium build input: it also determines the compiled
# WebAuthn keychain groups. Validate the built app before mutating or signing
# it, and never rebrand CFBundleIdentifier during deployment.
EXPECTED_BUNDLE_ID="${CMUX_EXPECTED_BUNDLE_ID:-${CMUX_MAC_BUNDLE_ID:-com.cmux.app}}"
BUILT_BUNDLE_ID="$(
  "$ROOT/scripts/verify-build-bundle-identity.sh" \
    "$DEST" "$EXPECTED_BUNDLE_ID"
)"
echo "== compiled bundle identity: $BUILT_BUNDLE_ID =="
validate_signing_team_id

# A component Chromium build keeps its shared libraries beside the built app in
# out/Release. They are reachable on the builder through the executable's
# fallback rpath, but a copied app is not self-contained unless those dylibs
# move into Contents/Frameworks (the first application-bundle rpath). A
# non-component build has no libc++_chrome dependency and needs no extra copy.
COMPONENT_FRAMEWORKS="$DEST/Contents/Frameworks"
APP_EXECUTABLE="$(plutil -extract CFBundleExecutable raw -o - \
  "$DEST/Contents/Info.plist")"
CHROMIUM_LINK_IMAGE="$DEST/Contents/MacOS/$APP_EXECUTABLE"
if ! CHROMIUM_LINKS="$(otool -L "$CHROMIUM_LINK_IMAGE")"; then
  echo "error: could not inspect the built app's linked libraries" >&2
  exit 1
fi
if printf '%s\n' "$CHROMIUM_LINKS" | \
    awk '$0 ~ /@rpath\/libc\+\+_chrome\.dylib/ { found = 1 }
         END { exit !found }'; then
  if [ "${CMUX_DEPLOY_SKIP_COMPONENT_RSYNC:-0}" = "1" ]; then
    test -f "$COMPONENT_FRAMEWORKS/libchrome_dll.dylib" || {
      echo "error: CMUX_DEPLOY_SKIP_COMPONENT_RSYNC=1 requires bundled component dylibs" >&2
      exit 1
    }
    echo "== component build: reusing already-synced dylibs =="
  else
    echo "== component build: bundle out/Release dylibs =="
    find "$COMPONENT_FRAMEWORKS" -maxdepth 1 -type f -name '*.dylib' -delete
    rsync -a --partial --include='*.dylib' --exclude='*' \
      "$HOST:$SRC/out/Release/" "$COMPONENT_FRAMEWORKS/"
  fi
  echo "== bundled $(find "$COMPONENT_FRAMEWORKS" -maxdepth 1 -type f -name '*.dylib' | wc -l | tr -d ' ') component dylibs =="
  CMUX_RUNTIME_IMAGE="$COMPONENT_FRAMEWORKS/libchrome_dll.dylib"
else
  find "$COMPONENT_FRAMEWORKS" -maxdepth 1 -type f -name '*.dylib' -delete
  CMUX_RUNTIME_IMAGE="$(scripts/resolve-chromium-runtime-image.sh "$DEST")"
fi

# The helper installer validates its artifact against this checkout's header,
# but a remote incremental build can still return an older Framework. Refuse
# to assemble that split-brain bundle before installing or signing a helper.
scripts/verify-cmux-runtime-pins.sh "$CMUX_RUNTIME_IMAGE"

UBLOCK_CACHE="${CMUX_EXTENSIONS_DIR:-$HOME/cmux-extensions}/ublock"
if [ ! -f "$UBLOCK_CACHE/manifest.json" ]; then
  bash scripts/fetch-extensions.sh
fi

EXT_RES="$DEST/Contents/Resources/cmux-extensions"
mkdir -p "$EXT_RES"
rm -rf "$EXT_RES/ublock"
rsync -a --delete "$UBLOCK_CACHE/" "$EXT_RES/ublock/"
echo "== bundled uBlock Origin -> $EXT_RES/ublock =="

# cmux-browser launches this helper as its authoritative terminal backend.
# Installation verifies both the cmux source stamp and the shared manaflow
# Ghostty revision before the helper enters the signed app bundle. Revision
# receipts are resources: Contents/Helpers is a nested-code directory and
# strict codesign rejects plain text placed there.
CMUX_TUI_TARGET="${CMUX_TUI_TARGET:-aarch64-apple-darwin}" \
  CMUX_TUI_SKIP_RUN_VERIFY=0 \
  CMUX_TUI_MANIFEST_DIR="$DEST/Contents/Resources/cmux-tui" \
  CMUX_GHOSTTY_RESOURCES_INSTALL_DIR="$DEST/Contents/Resources/ghostty" \
  scripts/install-cmux-tui-artifact.sh "$DEST/Contents/Helpers"
# An incrementally built or previously staged source bundle may still carry
# legacy receipts beside the helper. Never let those cross into nested code.
rm -f "$DEST/Contents/Helpers/cmux-tui.REVISION" \
  "$DEST/Contents/Helpers/cmux-tui.GHOSTTY_REVISION"

# Ship the GPL text and pinned Helium provenance inside the signed app. Keep
# this mutation before codesigning so the deployed bundle verifies as a whole.
NOTICE_RES="$DEST/Contents/Resources/cmux-licenses"
mkdir -p "$NOTICE_RES/third_party/helium"
cp LICENSE THIRD_PARTY_NOTICES.md "$NOTICE_RES/"
cp third_party/helium/LICENSE third_party/helium/README.chromium \
  "$NOTICE_RES/third_party/helium/"
echo "== bundled third-party notices -> $NOTICE_RES =="

# A channel build carries its signed-manifest URL in the bundle's sealed
# Resources directory. Plain data in Contents/MacOS makes codesign depend on
# extended attributes that ordinary updater ZIP extraction does not preserve.
UPDATE_FEED_FILE="$DEST/Contents/Resources/cmux-update-feed-url"
if [ -n "${CMUX_INSTALLED_UPDATE_FEED_URL:-}" ]; then
  case "$CMUX_INSTALLED_UPDATE_FEED_URL" in
    https://*) ;;
    *)
      echo "error: CMUX_INSTALLED_UPDATE_FEED_URL must use https://" >&2
      exit 2
      ;;
  esac
  case "$CMUX_INSTALLED_UPDATE_FEED_URL" in
    *$'\n'*|*$'\r'*)
      echo "error: CMUX_INSTALLED_UPDATE_FEED_URL must be one line" >&2
      exit 2
      ;;
  esac
  printf '%s\n' "$CMUX_INSTALLED_UPDATE_FEED_URL" > "$UPDATE_FEED_FILE"
  echo "== installed update feed: $CMUX_INSTALLED_UPDATE_FEED_URL =="
else
  rm -f "$UPDATE_FEED_FILE"
fi

# Tag the bundle so multiple builds are distinguishable in the dock / Cmd-Tab /
# menu bar / window list. CMUX_TAG names this build (e.g. "rework", "basic");
# defaults to the bundle's own name. Must run before codesign (it mutates the
# bundle).
TAG="${CMUX_TAG:-$(basename "$FINAL_DEST" .app | sed 's/^cmux-//')}"
NAME="${CMUX_DISPLAY_NAME:-cmux ▸ $TAG}"
for k in CFBundleName CFBundleDisplayName; do
  plutil -replace "$k" -string "$NAME" "$DEST/Contents/Info.plist"
done
if [ -n "${CMUX_PRODUCT_DIR_NAME:-}" ]; then
  if ! plutil -replace CrProductDirName -string "$CMUX_PRODUCT_DIR_NAME" \
      "$DEST/Contents/Info.plist" 2>/dev/null; then
    plutil -insert CrProductDirName -string "$CMUX_PRODUCT_DIR_NAME" \
      "$DEST/Contents/Info.plist"
  fi
  echo "== profile directory: ~/Library/Application Support/$CMUX_PRODUCT_DIR_NAME =="
fi

# Chromium's translated InfoPlist.strings files can retain localized product
# spellings. Rewrite only product-facing values, leaving Cast's localized Local
# Network reason and Chromium's authorship attribution intact.
bash "$ROOT/scripts/rewrite-macos-localized-product-name.sh" \
  "$DEST/Contents/Resources"
echo "== tagged as: $NAME =="

codesign_part() {
  local path="$1"
  local entitlements="${2:-}"

  if [ ! -e "$path" ]; then
    return
  fi

  local cmd=(codesign --force --sign "$SIGN_IDENTITY")
  # Real-identity (Tier 2) parts get the hardened runtime + a secure timestamp,
  # matching upstream's HARDENED_RUNTIME signing options; Developer ID
  # distribution/notarization requires both. Ad-hoc (Tier 1) skips them: the
  # hardened runtime without a real identity only risks launch flakiness.
  if [ "$SIGN_IDENTITY" != "-" ]; then
    cmd+=(--options runtime --timestamp)
  fi
  if [ -n "$entitlements" ]; then
    cmd+=(--entitlements "$entitlements")
  fi
  cmd+=("$path")

  if [ -n "$entitlements" ]; then
    echo "codesign: $path (entitlements: ${entitlements#$PWD/})"
  else
    echo "codesign: $path"
  fi
  "${cmd[@]}"
}

prepare_app_entitlements() {
  if [ -z "${CMUX_SIGN_IDENTITY:-}" ]; then
    return
  fi

  local bundle_id
  bundle_id="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' \
    "$DEST/Contents/Info.plist")"

  local team_id
  team_id="$(infer_team_id)"
  if [ -z "$team_id" ]; then
    echo "error: Tier 2 signing needs a Team ID. Set CMUX_SIGN_TEAM_ID, or use a Developer ID identity name ending in (TEAMID)." >&2
    exit 1
  fi
  if [ "$team_id" != "$COMPILED_MAC_TEAM_ID" ]; then
    echo "error: signing Team ID $team_id does not match compiled WebAuthn Team ID $COMPILED_MAC_TEAM_ID" >&2
    exit 1
  fi

  mkdir -p "$SIGN_WORK"
  sed \
    -e "s/\${CMUX_TEAM_ID}/$team_id/g" \
    -e "s/\${CMUX_BUNDLE_ID}/$bundle_id/g" \
    "$ENTITLEMENTS_DIR/cmux-app-restricted.plist" > "$APP_ENTITLEMENTS"
  plutil -lint "$APP_ENTITLEMENTS" >/dev/null

  local decoded_profile="$SIGN_WORK/embedded-profile.generated.plist"
  security cms -D -i "$CMUX_SIGN_PROVISION_PROFILE" > "$decoded_profile"

  local profile_app_id
  profile_app_id="$(/usr/libexec/PlistBuddy -c \
    'Print :Entitlements:com.apple.application-identifier' \
    "$decoded_profile")"
  if [ "$profile_app_id" != "$team_id.$bundle_id" ]; then
    echo "error: provisioning profile app identifier is $profile_app_id; expected $team_id.$bundle_id" >&2
    exit 1
  fi
  if [ "$(/usr/libexec/PlistBuddy -c \
      'Print :Entitlements:com.apple.developer.web-browser.public-key-credential' \
      "$decoded_profile" 2>/dev/null || true)" != "true" ]; then
    echo "error: provisioning profile does not grant com.apple.developer.web-browser.public-key-credential" >&2
    exit 1
  fi

  cp "$CMUX_SIGN_PROVISION_PROFILE" \
    "$DEST/Contents/embedded.provisionprofile"
  echo "== embedded passkey provisioning profile: $CMUX_SIGN_PROVISION_PROFILE =="
}

helper_entitlements_for_app() {
  case "$(basename "$1")" in
    *" Helper (Renderer).app")
      echo "$ENTITLEMENTS_DIR/helper-renderer-entitlements.plist"
      ;;
    *" Helper (GPU).app")
      echo "$ENTITLEMENTS_DIR/helper-gpu-entitlements.plist"
      ;;
  esac
}

sign_nested_code() {
  local frameworks_dir="$DEST/Contents/Frameworks"

  echo "== nested helper apps present =="
  find "$DEST" -name "*.app" -print | sort

  if [ ! -d "$frameworks_dir" ]; then
    codesign_part "$DEST/Contents/Helpers/cmux-tui"
    codesign_part "$DEST/Contents/Resources/bin/ghostty"
    return
  fi

  codesign_part "$DEST/Contents/Helpers/cmux-tui"
  codesign_part "$DEST/Contents/Resources/bin/ghostty"

  local path
  while IFS= read -r path; do
    codesign_part "$path"
  done < <(find "$frameworks_dir" -maxdepth 1 -name "*.dylib" -type f | sort)

  while IFS= read -r path; do
    codesign_part "$path"
  done < <(find "$frameworks_dir" -path "*/Libraries/*.dylib" -type f | sort)

  while IFS= read -r path; do
    codesign_part "$path"
  done < <(find "$frameworks_dir" -path "*/Helpers/*" -type f \
    \( -name "chrome_crashpad_handler" \
       -o -name "app_mode_loader" \
       -o -name "web_app_shortcut_copier" \) | sort)

  while IFS= read -r path; do
    codesign_part "$path" "$(helper_entitlements_for_app "$path")"
  done < <(find "$frameworks_dir" -path "*/Helpers/*.app" -type d | sort)

  while IFS= read -r path; do
    codesign_part "$path"
  done < <(find "$frameworks_dir" -maxdepth 1 -name "*.framework" -type d | sort)

  if [ -d "$DEST/Contents/Library/LaunchServices" ]; then
    while IFS= read -r path; do
      codesign_part "$path"
    done < <(find "$DEST/Contents/Library/LaunchServices" -type f | sort)
  fi
}

echo "== codesign: $SIGN_TIER =="
prepare_app_entitlements
sign_nested_code
codesign_part "$DEST" "$APP_ENTITLEMENTS"

if [ -n "${CMUX_SIGN_IDENTITY:-}" ]; then
  SIGNED_TEAM_ID="$(
    codesign -dvv "$DEST" 2>&1 |
      sed -n 's/^TeamIdentifier=//p' |
      head -1
  )"
  if [ "$SIGNED_TEAM_ID" != "$COMPILED_MAC_TEAM_ID" ]; then
    echo "error: signed app TeamIdentifier is ${SIGNED_TEAM_ID:-missing}; expected compiled WebAuthn Team ID $COMPILED_MAC_TEAM_ID" >&2
    exit 1
  fi
fi

echo "== codesign verify =="
codesign --verify --deep --strict "$DEST"

if [ -n "${CMUX_SIGN_IDENTITY:-}" ]; then
  SIGNED_TEAM_ID="$(
    codesign -dvvv "$DEST" 2>&1 |
      sed -n 's/^TeamIdentifier=//p' |
      head -1
  )"
  if [ "$SIGNED_TEAM_ID" != "$COMPILED_MAC_TEAM_ID" ]; then
    echo "error: signed app Team ID ${SIGNED_TEAM_ID:-<missing>} does not match compiled WebAuthn Team ID $COMPILED_MAC_TEAM_ID" >&2
    exit 1
  fi
fi

CMUX_EXPECTED_BUNDLE_ID="$BUILT_BUNDLE_ID" \
  "$ROOT/scripts/verify-platform-metadata.sh" "$DEST"

echo "== outer app entitlements =="
codesign -d --entitlements - "$DEST"

promote_staged_app() {
  local obsolete_backup
  if [ -e "$FINAL_DEST" ] || [ -L "$FINAL_DEST" ]; then
    BACKUP_ROOT="$(
      mktemp -d "$DEST_PARENT/.${DEST_BASENAME}.backup.XXXXXX"
    )"
    mv "$FINAL_DEST" "$BACKUP_ROOT/previous.app"
  fi

  rm -rf -- "$SIGN_WORK"
  mv "$STAGING_DEST" "$FINAL_DEST"
  PROMOTED=1
  rmdir "$STAGING_ROOT"

  # Verify the exact final path before discarding the rollback copy. A failure
  # leaves PROMOTED set, so the EXIT trap restores the previous app.
  CMUX_EXPECTED_BUNDLE_ID="$BUILT_BUNDLE_ID" \
    "$ROOT/scripts/verify-platform-metadata.sh" "$FINAL_DEST"

  obsolete_backup="$BACKUP_ROOT"
  BACKUP_ROOT=""
  PROMOTED=0
  if [ -n "$obsolete_backup" ] &&
      ! rm -rf -- "$obsolete_backup"; then
    echo "warning: could not remove prior deployment backup: $obsolete_backup" >&2
  fi
}

promote_staged_app

# Keep controller retention ordered by successful local deployment time.
# rsync -a preserves the builder's bundle mtime, so it is not a reliable local
# recency signal. The adjacent marker does not modify the signed bundle.
LAST_USED_MARKER="$FINAL_DEST.cmux-last-used"
LAST_USED_TEMP="$(dirname "$LAST_USED_MARKER")/.$(basename "$LAST_USED_MARKER").$$"
trap 'rm -f "$LAST_USED_TEMP"' EXIT
(umask 077; : > "$LAST_USED_TEMP")
mv -f "$LAST_USED_TEMP" "$LAST_USED_MARKER"
trap - EXIT

echo "deployed: $FINAL_DEST  ($NAME)"
