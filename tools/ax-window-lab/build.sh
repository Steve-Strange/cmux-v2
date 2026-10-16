#!/bin/zsh
set -euo pipefail

script_dir=${0:A:h}
app_dir="$script_dir/build/cmux AX Window Lab.app"
contents_dir="$app_dir/Contents"

mkdir -p "$contents_dir/MacOS"
xcrun swiftc \
  -parse-as-library \
  "$script_dir/main.swift" \
  -o "$contents_dir/MacOS/cmux-ax-window-lab"
cp "$script_dir/Info.plist" "$contents_dir/Info.plist"
codesign --force --deep --sign - "$app_dir"
print -r -- "$app_dir"
