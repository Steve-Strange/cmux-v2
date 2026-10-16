#!/bin/bash
# Runs ON the provisioned Linux box. Stands up a Chromium checkout at the cmux
# base revision and configures a Linux/Ozone GN out dir for the views_examples
# demo (the cross-platform cmux UI shell: rail + niri strip + tab strip). The
# overlay is rsynced + applied SEPARATELY from the Mac (scripts/sync.sh +
# CMUX_TARGET=views_examples scripts/apply.sh) once this finishes.
#
# Heavy step = `fetch chromium` (~100GB, the dominant cost). No GPU needed;
# views_examples runs under Xvfb with software GL (llvmpipe).
set -euxo pipefail

CMUX_BASE="${CMUX_BASE:-daf5ae401e}"  # tag cmux-base-149.0.7780.0

# 1. depot_tools
cd "$HOME"
if [ ! -d depot_tools ]; then
  git clone --depth 1 https://chromium.googlesource.com/chromium/tools/depot_tools.git
fi
export PATH="$HOME/depot_tools:$PATH"

# 2. Chromium checkout at the cmux base revision.
mkdir -p "$HOME/chromium"
cd "$HOME/chromium"
if [ ! -d src ]; then
  fetch --nohooks chromium
fi
cd src
git fetch --tags origin "$CMUX_BASE" || git fetch origin
git checkout "$CMUX_BASE"
gclient sync -D

# 3. Linux build deps (no prompt). Needs sudo.
sudo ./build/install-build-deps.sh --no-prompt || ./build/install-build-deps.sh --no-prompt

# 4. Tools for the headless screenshot.
sudo apt-get update -y
sudo apt-get install -y xvfb x11-apps imagemagick xdotool

# 5. GN: Linux, component build (fast link), no remote exec, software GL is fine.
gn gen out/Release --args='is_debug=false symbol_level=0 is_component_build=true use_remoteexec=false dcheck_always_on=false'

echo "LINUX_SETUP_DONE"
