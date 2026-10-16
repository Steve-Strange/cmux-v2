// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "chrome/browser/cmux_term/cmux_update_script.h"

namespace cmux {

std::string BuildUpdateInstallScript(UpdateInstallPlatform platform) {
  if (platform == UpdateInstallPlatform::kWindows) {
    return R"POWERSHELL(param(
  [Parameter(Mandatory=$true)][int]$ParentPid,
  [Parameter(Mandatory=$true)][string]$Current,
  [Parameter(Mandatory=$true)][string]$Staged,
  [Parameter(Mandatory=$true)][string]$Relaunch
)
$ErrorActionPreference = 'Stop'
Wait-Process -Id $ParentPid -ErrorAction SilentlyContinue
$backup = "$Current.cmux-old.$PID"
Move-Item -LiteralPath $Current -Destination $backup
try {
  Move-Item -LiteralPath $Staged -Destination $Current
  Start-Process -FilePath (Join-Path $Current $Relaunch)
  Remove-Item -LiteralPath $backup -Recurse -Force
} catch {
  if (Test-Path -LiteralPath $Current) {
    Remove-Item -LiteralPath $Current -Recurse -Force
  }
  Move-Item -LiteralPath $backup -Destination $Current
  throw
}
)POWERSHELL";
  }

  const char* relaunch = platform == UpdateInstallPlatform::kMac
                             ? "open \"$current\""
                             : "nohup \"$current/$relaunch\" >/dev/null 2>&1 &";
  return std::string(R"SHELL(#!/bin/sh
set -eu
parent_pid="$1"
current="$2"
staged="$3"
relaunch="$4"

while kill -0 "$parent_pid" 2>/dev/null; do
  sleep 1
done

backup="${current}.cmux-old.$$"
mv -- "$current" "$backup"
if mv -- "$staged" "$current"; then
  )SHELL") + relaunch + R"SHELL(
  rm -rf -- "$backup"
else
  mv -- "$backup" "$current"
  exit 1
fi
)SHELL";
}

}  // namespace cmux
