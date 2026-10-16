#!/bin/bash
# Fast cross-platform UI iteration loop for the cmux chrome Views (tab strip /
# vertical-tab rail). Builds the pure-Views widgets in a standalone
# views_examples harness — NO chrome, NO pages, NO Ghostty — so the edit->see
# cycle is a ~20-30s incremental relink instead of a ~5min full-chrome build.
#
#   1. edit overlay/chrome/browser/cmux_term/cmux_tab_strip.cc (or the rail)
#   2. ./scripts/uidemo.sh
#   3. look at the relaunched window -> "Cmux Tabs" in the left list
#
# The SAME view classes ship in cmux; this harness just renders them with mock
# WindowModel data. Being a plain Views app, it also proves cross-platform-ness.
set -euo pipefail
. "$(dirname "$0")/builder-transport.sh"
HOST="${CMUX_BUILDER:-ec2-user@aws-m4pro-1}"
SRC="${CHROMIUM_SRC:-/Volumes/recpass123/cmux-ci/cmux-browser-benchmark/src}"
cd "$(dirname "$0")/.."

echo "== sync cmux UI sources =="
RSYNC_ARGS=(-az --ignore-times --no-times)
rsync "${RSYNC_ARGS[@]}" \
  overlay/chrome/browser/cmux_term/BUILD.gn \
  overlay/chrome/browser/cmux_term/cmux_rail.h \
  overlay/chrome/browser/cmux_term/cmux_rail.cc \
  overlay/chrome/browser/cmux_term/cmux_rail_animating_layout_manager.h \
  overlay/chrome/browser/cmux_term/cmux_rail_animating_layout_manager.cc \
  overlay/chrome/browser/cmux_term/cmux_rail_group_editor_bubble.h \
  overlay/chrome/browser/cmux_term/cmux_rail_group_editor_bubble.cc \
  overlay/chrome/browser/cmux_term/cmux_rail_hover_card.h \
  overlay/chrome/browser/cmux_term/cmux_rail_hover_card.cc \
  overlay/chrome/browser/cmux_term/cmux_tab_strip.h \
  overlay/chrome/browser/cmux_term/cmux_tab_strip.cc \
  overlay/chrome/browser/cmux_term/window_model.h \
  overlay/chrome/browser/cmux_term/window_model.cc \
  overlay/chrome/browser/cmux_term/window_layout.h \
  overlay/chrome/browser/cmux_term/window_layout.cc \
  "$HOST:$SRC/chrome/browser/cmux_term/"
rsync "${RSYNC_ARGS[@]}" \
  overlay/ui/views/examples/cmux_demo_example.h \
  overlay/ui/views/examples/cmux_demo_example.cc \
  "$HOST:$SRC/ui/views/examples/"

# views_examples mode applies only the shared product metadata, the hover-card
# constructor prerequisite, and the standalone demo target wiring. Keep its
# authoritative patch input current without paying for a full overlay sync.
# Reverse the complete previously staged settings patch before --delete
# replaces its definition. Future revisions may add migration hunks to this
# stable path; reversing by the old definition lets this baseline safely open
# a warm checkout produced by those revisions. Chromium 151's stable patch
# now mirrors the permission hunks, so reverse it before the granular legacy
# restorer handles earlier standalone definitions.
CHROMIUM_MAJOR="$(ssh -o BatchMode=yes "$HOST" \
  "awk -F= '\$1 == \"MAJOR\" { print \$2 }' '$SRC/chrome/VERSION'")"
case "$CHROMIUM_MAJOR" in
  149|150)
    PREVIOUS_HELIUM_SETTINGS_PATCH=".cmux-patches/helium-settings-chromium-149.patch"
    ;;
  151)
    PREVIOUS_HELIUM_SETTINGS_PATCH=".cmux-patches/helium-settings-chromium-151.patch"
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
  fi"
ssh -o BatchMode=yes "$HOST" "cd '$SRC' && python3 -B - ." \
  < "$CMUX_SCRIPTS_DIR/restore-custom-window-permissions.py"
rsync "${RSYNC_ARGS[@]}" --delete patches/ "$HOST:$SRC/.cmux-patches/"
CMUX_TARGET=views_examples "$CMUX_SCRIPTS_DIR/apply.sh"

echo "== build harness (incremental) =="
remote_build_rc=0
ssh -o BatchMode=yes "$HOST" "
  export PATH=\"\$HOME/depot_tools:\$PATH\"
  cd '$SRC' || exit 90
  log='out/Release/cmux_ui_demo_build.log'
  autoninja -C out/Release views_examples > \"\$log\" 2>&1
  build_rc=\$?
  grep -E 'error:|FAILED|chromium-style|Build Succeeded|ninja: build' \"\$log\" | tail -8 || true
  if [ \"\$build_rc\" -ne 0 ]; then
    echo '--- last 20 build log lines ---'
    tail -20 \"\$log\"
    exit \"\$build_rc\"
  fi
  rm -rf out/Release/cmux_ui_demo.app &&
    cp -R 'out/Release/Views Examples.app' out/Release/cmux_ui_demo.app
" || remote_build_rc=$?
if [ "$remote_build_rc" -ne 0 ]; then
  echo "build failed — not relaunching"
  exit "$remote_build_rc"
fi

echo "== deploy local + relaunch =="
mkdir -p dist-uidemo
rsync -az --delete "$HOST:$SRC/out/Release/cmux_ui_demo.app/" \
  dist-uidemo/cmux_ui_demo.app/
for k in CFBundleName CFBundleDisplayName; do
  plutil -replace "$k" -string "cmux ▸ ui-demo" \
    dist-uidemo/cmux_ui_demo.app/Contents/Info.plist
done
codesign --force --deep --sign - dist-uidemo/cmux_ui_demo.app >/dev/null
demo_app="$(pwd)/dist-uidemo/cmux_ui_demo.app"
demo_executable="$demo_app/Contents/MacOS/Views Examples"
demo_pids() {
  ps -axo pid=,command= | while read -r pid command; do
    if [ "$command" = "$demo_executable" ]; then
      printf '%s\n' "$pid"
    fi
  done
}
while read -r old_demo_pid; do
  if [ -n "$old_demo_pid" ]; then
    kill "$old_demo_pid" 2>/dev/null || true
  fi
done < <(demo_pids)
sleep 1
/usr/bin/open -na "$demo_app"
for _ in $(seq 1 20); do
  demo_pid="$(demo_pids | tail -1)"
  if [ -n "$demo_pid" ]; then
    break
  fi
  sleep 0.25
done
if [ -z "${demo_pid:-}" ]; then
  echo "ui demo did not start through Launch Services" >&2
  exit 1
fi
# Chromium 151 initializes the viz GPU service asynchronously. Waiting through
# that path ensures a delayed startup CHECK cannot masquerade as a live demo.
sleep 8
if ! demo_pids | grep -Fxq "$demo_pid"; then
  echo "ui demo exited during startup" >&2
  exit 1
fi
echo "relaunched (pid $demo_pid) — open 'Cmux Workspaces' in the left example list"
