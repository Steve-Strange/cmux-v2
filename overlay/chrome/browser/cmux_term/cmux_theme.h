// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef CHROME_BROWSER_CMUX_TERM_CMUX_THEME_H_
#define CHROME_BROWSER_CMUX_TERM_CMUX_THEME_H_

#include <array>
#include <cstdint>
#include <optional>

namespace cmux {

using CmuxArgb = uint32_t;

struct CmuxTheme {
  CmuxArgb bg = 0xff000000;
  CmuxArgb fg = 0xffffffff;
  std::array<CmuxArgb, 16> palette = {};
  std::optional<CmuxArgb> cursor;
  std::optional<CmuxArgb> selection_bg;
  std::optional<CmuxArgb> selection_fg;
};

struct CmuxThemePalette {
  // Out-of-line special members keep the chromium-style plugin happy for this
  // wide struct; definitions are defaulted in cmux_theme.cc (stdlib-only).
  CmuxThemePalette();
  CmuxThemePalette(const CmuxThemePalette&);
  CmuxThemePalette& operator=(const CmuxThemePalette&);
  ~CmuxThemePalette();

  bool is_light = false;

  CmuxArgb window_bg = 0xff0e0f12;
  CmuxArgb content_bg = 0xff0e0f12;

  CmuxArgb rail_bg = 0xff15171c;
  CmuxArgb rail_sel_bg = 0x20ffffff;
  CmuxArgb rail_hover_bg = 0x11ffffff;
  CmuxArgb rail_hover_text = 0xeeffffff;
  CmuxArgb rail_drag_bg = 0x2cffffff;
  CmuxArgb rail_text = 0xa8ffffff;
  CmuxArgb rail_sel_text = 0xf2ffffff;
  CmuxArgb rail_folder_text = 0xc8ffffff;
  CmuxArgb rail_chevron = 0xaaffffff;
  CmuxArgb rail_badge_bg = 0x12ffffff;
  CmuxArgb rail_badge_text = 0x99ffffff;
  CmuxArgb rail_close_color = 0xaaffffff;
  CmuxArgb rail_plus_text = 0xccffffff;
  CmuxArgb rail_plus_bg = 0x00ffffff;
  CmuxArgb rail_plus_hover_bg = 0x16ffffff;
  CmuxArgb rail_indent_guide = 0x10ffffff;
  CmuxArgb rail_scrollbar_thumb = 0x58ffffff;
  CmuxArgb rail_accent = 0xff8a8f98;

  CmuxArgb layout_accent = 0xff8a8f98;
  CmuxArgb layout_focus = 0xff8a8f98;

  CmuxArgb tab_strip_bg = 0xff17191f;
  CmuxArgb tab_active_bg = 0xff2b2f39;
  CmuxArgb tab_hover_bg = 0xff20232a;
  CmuxArgb tab_active_text = 0xfff2f3f6;
  CmuxArgb tab_idle_text = 0xff979dad;
  CmuxArgb tab_plus_text = 0xffb8bdcb;
  CmuxArgb tab_accent = 0xff8a8f98;

  // macOS's native frame material already supplies the shared chrome canvas.
  // These preserve the normal theme RGB while exposing that material beneath
  // the tab strip, active tab, and omnibox at intentionally different alpha.
  CmuxArgb chrome_material_bg = 0x0015171c;
  CmuxArgb chrome_material_tab_active_bg = 0xd92b2f39;
  CmuxArgb chrome_material_tab_hover_bg = 0x9920232a;
  CmuxArgb chrome_material_omnibox_bg = 0xd92b2f39;

  // Helium interaction colors adapted to the resolved Ghostty light/dark
  // mode. Text follows Ghostty; explicit Ghostty selection colors win.
  CmuxArgb chrome_omnibox_text = 0xfff2f3f6;
  CmuxArgb chrome_omnibox_focus = 0xffbac3ff;
  CmuxArgb chrome_omnibox_selection_bg = 0xffbac3ff;
  CmuxArgb chrome_omnibox_selection_text = 0xff000000;

  CmuxArgb pane_focus_border = 0xff8a8f98;
  CmuxArgb pane_idle_border = 0xff2a2d34;

  CmuxArgb strip_view_bg = 0xff101217;
  CmuxArgb strip_view_focus_border = 0xff4d9aff;
  CmuxArgb strip_view_idle_border = 0xff2a2e38;

  CmuxArgb chrome_seed_color = 0xff8a8f98;
};

constexpr CmuxArgb CmuxArgbSet(uint8_t a, uint8_t r, uint8_t g, uint8_t b) {
  return (static_cast<CmuxArgb>(a) << 24) | (static_cast<CmuxArgb>(r) << 16) |
         (static_cast<CmuxArgb>(g) << 8) | static_cast<CmuxArgb>(b);
}

constexpr CmuxArgb CmuxRgb(uint8_t r, uint8_t g, uint8_t b) {
  return CmuxArgbSet(0xff, r, g, b);
}

constexpr uint8_t CmuxArgbA(CmuxArgb color) {
  return static_cast<uint8_t>((color >> 24) & 0xff);
}

constexpr uint8_t CmuxArgbR(CmuxArgb color) {
  return static_cast<uint8_t>((color >> 16) & 0xff);
}

constexpr uint8_t CmuxArgbG(CmuxArgb color) {
  return static_cast<uint8_t>((color >> 8) & 0xff);
}

constexpr uint8_t CmuxArgbB(CmuxArgb color) {
  return static_cast<uint8_t>(color & 0xff);
}

// WCAG relative luminance for sRGB: linearize each channel, then use
// Y = 0.2126 R + 0.7152 G + 0.0722 B.
double RelativeLuminance(CmuxArgb color);
double ContrastRatio(CmuxArgb a, CmuxArgb b);

CmuxThemePalette DeriveUiPalette(const CmuxTheme& theme);

}  // namespace cmux

#endif  // CHROME_BROWSER_CMUX_TERM_CMUX_THEME_H_
