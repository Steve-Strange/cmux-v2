// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef CHROME_BROWSER_UI_COLOR_CMUX_CHROME_SURFACE_COLORS_H_
#define CHROME_BROWSER_UI_COLOR_CMUX_CHROME_SURFACE_COLORS_H_

#include <optional>

#include "third_party/skia/include/core/SkColor.h"

struct CmuxChromeSurfaceColors {
  SkColor toolbar = SK_ColorTRANSPARENT;
  // The New Tab canvas follows Ghostty's resolved background exactly rather
  // than a Material neutral derived only from its light/dark classification.
  SkColor new_tab_background = SK_ColorTRANSPARENT;
  // Ghostty-derived counterpart to Helium's light-mode Surface5 location-bar
  // fill. In dark mode Helium aliases the location bar to omnibox_popup.
  SkColor omnibox_surface = SK_ColorTRANSPARENT;
  // Empty keeps Chromium's custom-theme luminance recipe. A value can be an
  // explicit chrome_omnibox_color override or the native-material emphasis
  // surface used by cmux on macOS.
  std::optional<SkColor> omnibox;
  SkColor omnibox_popup = SK_ColorTRANSPARENT;
  // Empty keeps Chromium's state-layer recipe over omnibox_popup.
  std::optional<SkColor> omnibox_popup_hover;
  SkColor omnibox_text = SK_ColorWHITE;
  SkColor omnibox_focus = SkColorSetRGB(0xba, 0xc3, 0xff);
  SkColor omnibox_selection_bg = SkColorSetRGB(0xba, 0xc3, 0xff);
  SkColor omnibox_selection_text = SK_ColorBLACK;

  bool operator==(const CmuxChromeSurfaceColors& other) const {
    // Do not use a defaulted comparison here. Chromium's hardened libc++ has
    // diagnosed an optional::operator* on a disengaged value while comparing
    // the two optional override fields during the repeated startup theme
    // refresh. Compare presence before value explicitly so an omitted
    // override remains a safe and normal state.
    const auto optional_color_equal = [](const std::optional<SkColor>& left,
                                         const std::optional<SkColor>& right) {
      if (left.has_value() != right.has_value()) {
        return false;
      }
      return !left.has_value() || left.value() == right.value();
    };
    return toolbar == other.toolbar &&
           new_tab_background == other.new_tab_background &&
           omnibox_surface == other.omnibox_surface &&
           optional_color_equal(omnibox, other.omnibox) &&
           omnibox_popup == other.omnibox_popup &&
           optional_color_equal(omnibox_popup_hover,
                                other.omnibox_popup_hover) &&
           omnibox_text == other.omnibox_text &&
           omnibox_focus == other.omnibox_focus &&
           omnibox_selection_bg == other.omnibox_selection_bg &&
           omnibox_selection_text == other.omnibox_selection_text;
  }
};

// Installs process-wide cmux surface recipes while a Ghostty palette is active.
// Passing nullopt restores the unmodified Chromium color graph.
void SetCmuxChromeSurfaceColors(
    std::optional<CmuxChromeSurfaceColors> colors);

#endif  // CHROME_BROWSER_UI_COLOR_CMUX_CHROME_SURFACE_COLORS_H_
