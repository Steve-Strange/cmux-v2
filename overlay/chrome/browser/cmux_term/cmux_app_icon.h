// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_CMUX_TERM_CMUX_APP_ICON_H_
#define CHROME_BROWSER_CMUX_TERM_CMUX_APP_ICON_H_

#include <optional>
#include <string_view>

namespace cmux {

enum class AppIconMode {
  kAutomatic,
  kLight,
  kDark,
};

std::string_view AppIconModeToString(AppIconMode mode);
std::optional<AppIconMode> AppIconModeFromString(std::string_view value);

// Reads the persisted mode. Unknown values fall back to kAutomatic.
AppIconMode GetAppIconMode();

// Persists and immediately applies the mode. Must run on the UI thread after
// NSApplication has finished launching.
void SetAppIconMode(AppIconMode mode);

// Applies the persisted mode and starts automatic appearance observation.
// Safe to call more than once after browser startup.
void StartAppIconController();

}  // namespace cmux

#endif  // CHROME_BROWSER_CMUX_TERM_CMUX_APP_ICON_H_
