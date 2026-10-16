#!/bin/bash
# Rsync our overlay sources into the Chromium checkout on the builder.
# Overlay paths mirror the real chromium tree under overlay/.
#
# Deletion: chrome/browser/cmux_term/ is wholly cmux-owned, so it syncs with
# --delete — retiring/renaming a file in the overlay removes the old name on
# the builder (stale leftovers have shadowed renames before; e.g. retired
# niri_model.* had to be rm'd by hand). Other overlay dirs (ui/views/examples)
# mix our files in among upstream ones, so they must NEVER get --delete.
set -euo pipefail
. "$(dirname "$0")/builder-transport.sh"
HOST="${CMUX_BUILDER:-ec2-user@aws-m4pro-1}"
SRC="${CHROMIUM_SRC:-/Volumes/recpass123/cmux-ci/cmux-browser-benchmark/src}"
cd "$(dirname "$0")/.."
# Always transfer cmux-owned inputs, even when their bytes match the previous
# workspace. APFS-cloned Ninja outputs can otherwise be newer than an
# identical source file left behind by a prior branch, so a header's current
# defaults and a stale provider object disagree at runtime. `--no-times`
# gives every transferred input a fresh builder mtime; `--ignore-times` makes
# that happen for byte-identical inputs too.
#
# Restore every previously mirrored source transformation before replacing
# patches/. Otherwise a changed or removed patch definition cannot reverse its
# old output. The stable M151 helium-settings path also carries the permission
# hunks so older branches—which already reverse that path—can roll back a warm
# checkout even though they predate restore-custom-window-permissions.py. Use
# the previous custom-permission definition, not the incoming one, and fail
# closed when the warm tree matches neither known state.
CHROMIUM_MAJOR="$(ssh -o BatchMode=yes "$HOST" \
  "awk -F= '\$1 == \"MAJOR\" { print \$2 }' '$SRC/chrome/VERSION'")"
case "$CHROMIUM_MAJOR" in
  149)
    PREVIOUS_HELIUM_SETTINGS_PATCH=".cmux-patches/helium-settings-chromium-149.patch"
    PREVIOUS_PINNED_TOOLBAR_PATCH=".cmux-patches/cmux-pinned-toolbar-actions-chromium-149.patch"
    ;;
  150)
    PREVIOUS_HELIUM_SETTINGS_PATCH=".cmux-patches/helium-settings-chromium-149.patch"
    PREVIOUS_PINNED_TOOLBAR_PATCH=".cmux-patches/cmux-pinned-toolbar-actions-chromium-150.patch"
    ;;
  151)
    PREVIOUS_HELIUM_SETTINGS_PATCH=".cmux-patches/helium-settings-chromium-151.patch"
    PREVIOUS_PINNED_TOOLBAR_PATCH=".cmux-patches/cmux-pinned-toolbar-actions-chromium-151.patch"
    ;;
  *)
    echo "helium-settings-refresh: unsupported Chromium major $CHROMIUM_MAJOR" >&2
    exit 1
    ;;
esac
ssh -o BatchMode=yes "$HOST" "cd '$SRC' && set -e
  if [ -f '$PREVIOUS_HELIUM_SETTINGS_PATCH' ]; then
    if git apply --reverse --check '$PREVIOUS_HELIUM_SETTINGS_PATCH' >/dev/null 2>&1; then
      git apply --reverse '$PREVIOUS_HELIUM_SETTINGS_PATCH'
      echo 'helium-settings-refresh: reversed previous patch'
    elif git apply --check '$PREVIOUS_HELIUM_SETTINGS_PATCH' >/dev/null 2>&1; then
      echo 'helium-settings-refresh: previous patch was not applied'
    else
      echo 'helium-settings-refresh: warm tree matches neither applied nor pristine state' >&2
      exit 1
    fi
  fi
  for previous_patch in \
      '.cmux-patches/helium-new-tab.patch' \
      '.cmux-patches/helium-media-toolbar.patch' \
      '$PREVIOUS_PINNED_TOOLBAR_PATCH'; do
    if [ ! -f \"\$previous_patch\" ]; then
      continue
    fi
    if git apply --reverse --check \"\$previous_patch\" >/dev/null 2>&1; then
      git apply --reverse \"\$previous_patch\"
      echo \"helium-ui-refresh: reversed \$previous_patch\"
    elif git apply --check \"\$previous_patch\" >/dev/null 2>&1; then
      echo \"helium-ui-refresh: \$previous_patch was not applied\"
    else
      echo \"helium-ui-refresh: \$previous_patch matches neither applied nor pristine state\" >&2
      exit 1
    fi
  done"
ssh -o BatchMode=yes "$HOST" "cd '$SRC' && python3 -B - ." \
  < "$CMUX_SCRIPTS_DIR/restore-custom-window-permissions.py"

rsync -az --ignore-times --no-times --delete overlay/chrome/browser/cmux_term/ "$HOST:$SRC/chrome/browser/cmux_term/"
rsync -az --ignore-times --no-times overlay/ "$HOST:$SRC/"
rsync -az --ignore-times --no-times --delete patches/ "$HOST:$SRC/.cmux-patches/"
rsync -az --ignore-times --no-times third_party/helium/ "$HOST:$SRC/third_party/helium/"
echo "synced overlay/ -> $HOST:$SRC/ (cmux_term with --delete)"
