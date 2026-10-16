// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef CHROME_BROWSER_CMUX_TERM_CMUX_UPDATE_INSTALLER_H_
#define CHROME_BROWSER_CMUX_TERM_CMUX_UPDATE_INSTALLER_H_

#include <string>

#include "base/files/file_path.h"

namespace cmux {

struct ReadyUpdate {
  std::string version;
  base::FilePath staged_root;
  std::string relaunch_path;
};

// Blocking filesystem preflight; call on a MayBlock sequence.
bool CurrentInstallCanBeReplaced();

// Writes a detached, platform-native swap helper and starts it. The helper
// waits for this browser process to exit, atomically replaces the installation,
// relaunches, and deletes the rollback copy. Returns only whether handoff was
// successfully launched; the service exits Chromium after true.
bool LaunchReadyUpdateInstaller(const ReadyUpdate& update);

}  // namespace cmux

#endif  // CHROME_BROWSER_CMUX_TERM_CMUX_UPDATE_INSTALLER_H_
