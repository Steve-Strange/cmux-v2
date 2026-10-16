// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef CHROME_BROWSER_CMUX_TERM_CMUX_UPDATE_SCRIPT_H_
#define CHROME_BROWSER_CMUX_TERM_CMUX_UPDATE_SCRIPT_H_

#include <string>

namespace cmux {

enum class UpdateInstallPlatform { kMac, kWindows, kLinux };

// The scripts take paths as process arguments rather than interpolating them.
// This keeps spaces and shell metacharacters in install paths inert.
std::string BuildUpdateInstallScript(UpdateInstallPlatform platform);

}  // namespace cmux

#endif  // CHROME_BROWSER_CMUX_TERM_CMUX_UPDATE_SCRIPT_H_
