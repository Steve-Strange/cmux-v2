// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef CHROME_BROWSER_CMUX_TERM_CMUX_RELEASE_BUILD_H_
#define CHROME_BROWSER_CMUX_TERM_CMUX_RELEASE_BUILD_H_

namespace cmux {

// The checked-in value is deliberately false. The stable/nightly packaging
// pipeline stamps only its disposable Chromium overlay copy before compiling,
// so developer and dogfood builds remain telemetry-off unless explicitly
// opted in through CMUX_TELEMETRY_ENABLE=1.
inline constexpr bool kCmuxOfficialReleaseBuild = false;

}  // namespace cmux

#endif  // CHROME_BROWSER_CMUX_TERM_CMUX_RELEASE_BUILD_H_
