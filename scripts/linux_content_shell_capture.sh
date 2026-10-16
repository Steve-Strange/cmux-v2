#!/bin/bash
# Runs ON the Linux box after content_shell is built. Loads a real web page in
# content_shell under Xvfb (software GL, no GPU) and screenshots it.
set -uxo pipefail
cd "$HOME/chromium/src"
export DISPLAY=:99
pkill -f content_shell || true; pkill Xvfb || true; sleep 1
Xvfb :99 -screen 0 1400x1000x24 >/tmp/xvfb.log 2>&1 &
sleep 2
URL="${1:-https://en.wikipedia.org/wiki/Google_Chrome}"
out/Release/content_shell --no-sandbox --use-gl=angle --use-angle=swiftshader \
  --force-device-scale-factor=1 "$URL" >/tmp/cs.log 2>&1 &
CS=$!
sleep 15
WID="$(xdotool search --name 'Content Shell' | head -1 || true)"
echo "wid=$WID"
import -window root /tmp/cs_linux_root.png || true
[ -n "$WID" ] && import -window "$WID" /tmp/cs_linux.png || true
ls -la /tmp/cs_linux*.png || true
echo '--- cs.log tail ---'; tail -12 /tmp/cs.log || true
kill "$CS" 2>/dev/null || true
echo CS_CAPTURE_DONE
