#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TMP="$(mktemp -d "${TMPDIR:-/tmp}/cmux-linux-installer-test.XXXXXX")"
cmux_test_backend=
cleanup() {
  if [ -n "$cmux_test_backend" ]; then
    kill "$cmux_test_backend" 2>/dev/null || true
    wait "$cmux_test_backend" 2>/dev/null || true
  fi
  rm -rf "$TMP"
}
trap cleanup EXIT

make_fixture() {
  local version="$1"
  rm -rf "$TMP/cmux-browser"
  mkdir -p "$TMP/cmux-browser"
  cat > "$TMP/cmux-browser/chrome" <<EOF
#!/bin/sh
printf '%s\n' '$version'
EOF
  chmod 0755 "$TMP/cmux-browser/chrome"
  printf 'png fixture\n' > "$TMP/cmux-browser/product_logo_256.png"
  printf 'notice fixture\n' > "$TMP/cmux-browser/THIRD_PARTY_NOTICES.md"
}

run_installer() {
  HOME="$TMP/home" \
  XDG_BIN_HOME="$TMP/home/bin" \
  XDG_DATA_HOME="$TMP/home/share" \
  CMUX_INSTALL_DIR="$TMP/home/opt/cmux-browser-nightly" \
  CMUX_LINUX_INSTALLER_NO_LAUNCH=1 \
    sh "$TMP/cmux-linux-x64-installer.run"
}

make_fixture v1
CMUX_RELEASE_CHANNEL=nightly \
CMUX_LINUX_INSTALLER_INPUT="$TMP/cmux-browser" \
CMUX_LINUX_INSTALLER_OUTPUT="$TMP/cmux-linux-x64-installer.run" \
  "$ROOT/scripts/build-linux-user-installer.sh"

run_installer
test "$("$TMP/home/opt/cmux-browser-nightly/chrome")" = v1
test "$(readlink "$TMP/home/bin/cmux-browser-nightly")" = \
  "$TMP/home/opt/cmux-browser-nightly/chrome"
grep -Fq 'Name=cmux Browser Nightly' \
  "$TMP/home/share/applications/cmux-browser-nightly.desktop"
grep -Fq "Exec=\"$TMP/home/bin/cmux-browser-nightly\" %U" \
  "$TMP/home/share/applications/cmux-browser-nightly.desktop"

make_fixture v2
CMUX_RELEASE_CHANNEL=nightly \
CMUX_LINUX_INSTALLER_INPUT="$TMP/cmux-browser" \
CMUX_LINUX_INSTALLER_OUTPUT="$TMP/cmux-linux-x64-installer.run" \
  "$ROOT/scripts/build-linux-user-installer.sh"
run_installer
test "$("$TMP/home/opt/cmux-browser-nightly/chrome")" = v2
test -z "$(find "$TMP/home/opt" -maxdepth 1 -name '*.cmux-old.*' -print)"

# A headless PTY backend can outlive its browser. Reinstalling must leave its
# current_exe() path executable so creating another terminal still works.
cp /bin/sleep "$TMP/home/opt/cmux-browser-nightly/cmux-tui"
"$TMP/home/opt/cmux-browser-nightly/cmux-tui" 30 &
cmux_test_backend=$!
for attempt in {1..100}; do
  if [ "$(readlink "/proc/$cmux_test_backend/exe")" = \
       "$TMP/home/opt/cmux-browser-nightly/cmux-tui" ]; then
    break
  fi
  sleep 0.01
done
run_installer
retained_exe="$(readlink "/proc/$cmux_test_backend/exe")"
test -x "$retained_exe"
"$retained_exe" 0
kill "$cmux_test_backend"
wait "$cmux_test_backend" 2>/dev/null || true
cmux_test_backend=

if HOME="$TMP/home" \
    CMUX_INSTALL_DIR=/ \
    CMUX_LINUX_INSTALLER_NO_LAUNCH=1 \
      sh "$TMP/cmux-linux-x64-installer.run" \
      >"$TMP/unsafe.out" 2>&1; then
  echo "error: Linux installer accepted an unsafe install root" >&2
  exit 1
fi
grep -Fq 'refusing unsafe install path' "$TMP/unsafe.out"

echo "Linux per-user installer tests passed"
