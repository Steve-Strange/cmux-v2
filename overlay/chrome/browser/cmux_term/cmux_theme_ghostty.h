// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef CHROME_BROWSER_CMUX_TERM_CMUX_THEME_GHOSTTY_H_
#define CHROME_BROWSER_CMUX_TERM_CMUX_THEME_GHOSTTY_H_

#include <array>
#include <optional>
#include <string>
#include <vector>

#include "chrome/browser/cmux_term/cmux_theme.h"

namespace cmux {

// Lightweight colors read directly from one bundled Ghostty theme file. The
// picker uses these to render every catalog entry without constructing 500+
// libghostty configurations.
struct GhosttyThemePreview {
  std::string name;
  CmuxArgb background = CmuxRgb(0x1e, 0x1e, 0x1e);
  CmuxArgb foreground = CmuxRgb(0xf5, 0xf5, 0xf5);
  CmuxArgb cursor = CmuxRgb(0xf5, 0xf5, 0xf5);
  std::array<CmuxArgb, 8> palette = {
      CmuxRgb(0x1e, 0x1e, 0x1e), CmuxRgb(0xe0, 0x6c, 0x75),
      CmuxRgb(0x98, 0xc3, 0x79), CmuxRgb(0xe5, 0xc0, 0x7b),
      CmuxRgb(0x61, 0xaf, 0xef), CmuxRgb(0xc6, 0x78, 0xdd),
      CmuxRgb(0x56, 0xb6, 0xc2), CmuxRgb(0xf5, 0xf5, 0xf5),
  };
};

// Initializes Ghostty's process-global core state once. Must be called before
// any ghostty_config_* API. The UI thread calls this before posting background
// theme loads, so reload cannot race a terminal-pane initialization.
bool EnsureGhosttyCoreInit();

// Loads the user's resolved Ghostty theme from the default Ghostty config
// files. A non-empty `theme_name` overrides only Ghostty's theme selection,
// preserving the rest of the user's recursive configuration. Returns nullopt
// if libghostty core init/config creation fails or a required color is absent.
std::optional<CmuxTheme> LoadGhosttyTheme(std::string theme_name = {});

// Returns every theme shipped in the selected pinned Ghostty resource tree.
// The empty "follow Ghostty config" choice is represented by the caller.
std::vector<std::string> ListGhosttyThemes();

// Returns the complete bundled catalog with compact terminal-preview colors.
// File IO and parsing are intentionally caller-scheduled off the UI sequence.
std::vector<GhosttyThemePreview> ListGhosttyThemePreviews();

// Serializes only the resolved color defaults needed by Ghostty's lightweight
// surface theme-update API. This lets one browser-side theme resolution update
// every terminal without repeating config IO or restarting renderer processes.
std::string SerializeGhosttyThemeColors(const CmuxTheme& theme);

}  // namespace cmux

#endif  // CHROME_BROWSER_CMUX_TERM_CMUX_THEME_GHOSTTY_H_
