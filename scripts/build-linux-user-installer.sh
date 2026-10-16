#!/usr/bin/env bash
# Build a self-extracting, per-user Linux installer. The browser lands below
# ~/.local by default, so the signed in-app updater can atomically replace its
# installation without sudo or a privileged helper.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
INPUT="${CMUX_LINUX_INSTALLER_INPUT:-$ROOT/release/cmux-browser}"
OUTPUT="${CMUX_LINUX_INSTALLER_OUTPUT:-$ROOT/release/cmux-linux-x64-installer.run}"
CHANNEL="${CMUX_RELEASE_CHANNEL:?CMUX_RELEASE_CHANNEL is required}"

case "$CHANNEL" in
  stable)
    INSTALL_SLUG="cmux-browser"
    LAUNCHER_NAME="cmux-browser"
    DESKTOP_NAME="cmux Browser"
    ;;
  nightly)
    INSTALL_SLUG="cmux-browser-nightly"
    LAUNCHER_NAME="cmux-browser-nightly"
    DESKTOP_NAME="cmux Browser Nightly"
    ;;
  *)
    echo "error: invalid release channel: $CHANNEL" >&2
    exit 2
    ;;
esac

[ -d "$INPUT" ] || {
  echo "error: Linux installer input is missing: $INPUT" >&2
  exit 2
}
[ -x "$INPUT/chrome" ] || {
  echo "error: Linux installer input has no executable chrome launcher" >&2
  exit 2
}

mkdir -p "$(dirname "$OUTPUT")"
rm -f "$OUTPUT"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/cmux-linux-installer-build.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT
{
cat <<'CMUX_INSTALLER_HEADER'
#!/bin/sh
# Self-extracting cmux Browser Linux installer.
set -eu
CMUX_INSTALLER_HEADER
printf "install_slug='%s'\n" "$INSTALL_SLUG"
printf "launcher_name='%s'\n" "$LAUNCHER_NAME"
printf "desktop_name='%s'\n" "$DESKTOP_NAME"
cat <<'CMUX_INSTALLER_BODY'

install_root="${CMUX_INSTALL_DIR:-${HOME:?HOME is required}/.local/opt/$install_slug}"
bin_root="${XDG_BIN_HOME:-$HOME/.local/bin}"
data_root="${XDG_DATA_HOME:-$HOME/.local/share}"
launcher="$bin_root/$launcher_name"
desktop_file="$data_root/applications/$launcher_name.desktop"

case "$install_root" in
  /*) ;;
  *)
    echo "error: CMUX_INSTALL_DIR must be an absolute path" >&2
    exit 2
    ;;
esac
case "$install_root" in
  /|/bin|/boot|/dev|/etc|/home|/lib|/lib64|/opt|/proc|/root|/run|/sbin|/srv|/sys|/tmp|/usr|/var)
    echo "error: refusing unsafe install path: $install_root" >&2
    exit 2
    ;;
esac
case "$install_root$bin_root$data_root" in
  *'
'*)
    echo "error: install paths must not contain newlines" >&2
    exit 2
    ;;
esac

work="$(mktemp -d "${TMPDIR:-/tmp}/cmux-linux-install.XXXXXX")"
backup="${install_root}.cmux-old.$$"
had_previous=0
promoted=0
cleanup() {
  rc=$?
  trap - EXIT INT TERM
  if [ "$rc" -ne 0 ] && [ "$promoted" -eq 1 ]; then
    rm -rf -- "$install_root"
    if [ "$had_previous" -eq 1 ] && [ -e "$backup" ]; then
      mv -- "$backup" "$install_root"
    fi
  fi
  rm -rf -- "$work"
  if [ -e "$backup" ]; then
    rm -rf -- "$backup"
  fi
  exit "$rc"
}
trap cleanup EXIT INT TERM

payload_line="$(awk '/^__CMUX_PAYLOAD_BELOW__$/ { print NR + 1; exit }' "$0")"
[ -n "$payload_line" ] || {
  echo "error: installer payload marker is missing" >&2
  exit 1
}
mkdir -p "$work/unpacked"
tail -n "+$payload_line" "$0" | gzip -dc | tar -xf - -C "$work/unpacked"
staged="$work/unpacked/cmux-browser"
[ -x "$staged/chrome" ] || {
  echo "error: installer payload is incomplete" >&2
  exit 1
}

mkdir -p "$(dirname "$install_root")" "$bin_root" "$(dirname "$desktop_file")"
if [ -e "$install_root" ] || [ -L "$install_root" ]; then
  mv -- "$install_root" "$backup"
  had_previous=1
fi
mv -- "$staged" "$install_root"
promoted=1

ln -sfn "$install_root/chrome" "$launcher"
escaped_launcher="$(printf '%s' "$launcher" | sed 's/\\/\\\\/g; s/"/\\"/g; s/`/\\`/g; s/\$/\\$/g')"
icon="$install_root/product_logo_256.png"
{
  printf '%s\n' '[Desktop Entry]' 'Type=Application'
  printf 'Name=%s\n' "$desktop_name"
  printf 'Exec="%s" %%U\n' "$escaped_launcher"
  if [ -f "$icon" ]; then
    escaped_icon="$(printf '%s' "$icon" | sed 's/\\/\\\\/g; s/"/\\"/g; s/`/\\`/g; s/\$/\\$/g')"
    printf 'Icon=%s\n' "$escaped_icon"
  fi
  printf '%s\n' \
    'Terminal=false' \
    'Categories=Network;WebBrowser;' \
    'MimeType=text/html;x-scheme-handler/http;x-scheme-handler/https;'
} > "$desktop_file"

if command -v update-desktop-database >/dev/null 2>&1; then
  update-desktop-database "$(dirname "$desktop_file")" >/dev/null 2>&1 || true
fi

if [ "$had_previous" -eq 1 ]; then
  rm -rf -- "$backup"
fi
promoted=0
echo "Installed $desktop_name in $install_root"
echo "Launcher: $launcher"

if [ "${CMUX_LINUX_INSTALLER_NO_LAUNCH:-0}" != 1 ]; then
  nohup "$launcher" >/dev/null 2>&1 &
fi
exit 0
__CMUX_PAYLOAD_BELOW__
CMUX_INSTALLER_BODY
} > "$OUTPUT"

COPYFILE_DISABLE=1 tar -C "$(dirname "$INPUT")" \
  -cf "$WORK/payload.tar" "$(basename "$INPUT")"
gzip -9 -c "$WORK/payload.tar" >> "$OUTPUT"
chmod 0755 "$OUTPUT"

echo "Linux per-user installer: $OUTPUT"
