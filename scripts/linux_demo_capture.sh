#!/bin/bash
# Runs ON the Linux box AFTER views_examples is built. Launches it under a
# virtual X display (Xvfb, software GL — no GPU), selects the "Cmux Workspaces"
# example in the combobox via xdotool type-ahead, and screenshots the window so
# we get the cmux UI shell (rail + niri strip + tab strip) rendered on Linux.
set -uxo pipefail
cd "$HOME/chromium/src"

export DISPLAY=:99
pkill Xvfb || true
Xvfb :99 -screen 0 1600x1000x24 >/tmp/xvfb.log 2>&1 &
sleep 2

# SwiftShader (bundled) gives software GL so the compositor works with no GPU.
out/Release/views_examples \
  --use-gl=angle --use-angle=swiftshader \
  --enable-logging=stderr --log-level=2 >/tmp/ve.log 2>&1 &
VE=$!
sleep 10

WID="$(xdotool search --name 'Views Examples' | head -1 || true)"
echo "window=$WID"
if [ -n "$WID" ]; then
  xdotool windowactivate --sync "$WID" || true
  # The example picker is a combobox at the top; focus it and type-ahead to the
  # cmux example, which selects it and swaps the shown example view.
  xdotool key --window "$WID" Tab
  xdotool type --window "$WID" "Cmux Workspaces"
  xdotool key --window "$WID" Return
  sleep 3
fi

# Capture the whole virtual root (always works) and the window if we found it.
import -window root /tmp/cmux_linux_ui_root.png || true
if [ -n "$WID" ]; then
  import -window "$WID" /tmp/cmux_linux_ui.png || true
fi
ls -la /tmp/cmux_linux_ui*.png || true
echo "--- views_examples log tail ---"; tail -20 /tmp/ve.log || true
kill "$VE" 2>/dev/null || true
echo "CAPTURE_DONE"
