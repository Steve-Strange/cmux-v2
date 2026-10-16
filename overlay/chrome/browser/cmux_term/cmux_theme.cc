// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "chrome/browser/cmux_term/cmux_theme.h"

#include <algorithm>
#include <cmath>

namespace cmux {

namespace {

// All derivation factors live here so the palette can be tuned without hunting
// through the mapping code below.
constexpr double kLightThreshold = 0.5;
constexpr double kLightContentDarken = 0.015;
constexpr double kLightRailDarken = 0.045;
constexpr double kLightStripDarken = 0.065;
constexpr double kLightTabActiveDarken = 0.095;
constexpr double kLightTabHoverDarken = 0.045;
constexpr double kLightIdleBorderDarken = 0.16;
constexpr double kDarkContentLighten = 0.025;
constexpr double kDarkRailLighten = 0.055;
constexpr double kDarkStripLighten = 0.075;
constexpr double kDarkTabActiveLighten = 0.16;
constexpr double kDarkTabHoverLighten = 0.10;
constexpr double kDarkIdleBorderLighten = 0.18;
constexpr double kTextDimAlpha = 0.70;
constexpr double kTextFolderAlpha = 0.84;
constexpr double kTextFaintAlpha = 0.45;
constexpr double kHoverBgAlpha = 0.08;
constexpr double kSelectionBgAlpha = 0.14;
constexpr double kDragBgAlpha = 0.20;
constexpr double kBadgeBgAlpha = 0.10;
constexpr double kPlusHoverBgAlpha = 0.10;
constexpr double kIndentGuideAlpha = 0.10;
constexpr double kScrollbarThumbAlpha = 0.34;
constexpr uint8_t kChromeMaterialCanvasAlpha = 0x00;
constexpr uint8_t kChromeMaterialEmphasisAlpha = 0xd9;
constexpr uint8_t kChromeMaterialHoverAlpha = 0x99;

// Helium's baseline primary-40/primary-80 pair. The UI mode follows the
// Ghostty background, so each theme gets the same focused-blue treatment at
// the contrast level Helium uses for that mode.
constexpr CmuxArgb kHeliumFocusLight = 0xff3551d2;
constexpr CmuxArgb kHeliumFocusDark = 0xffbac3ff;

uint8_t RoundChannel(double value) {
  return static_cast<uint8_t>(
      std::clamp(static_cast<int>(std::lround(value)), 0, 255));
}

CmuxArgb Opaque(CmuxArgb color) {
  return CmuxArgbSet(0xff, CmuxArgbR(color), CmuxArgbG(color),
                     CmuxArgbB(color));
}

CmuxArgb WithAlpha(CmuxArgb color, uint8_t alpha) {
  return CmuxArgbSet(alpha, CmuxArgbR(color), CmuxArgbG(color),
                     CmuxArgbB(color));
}

CmuxArgb Blend(CmuxArgb fg, CmuxArgb bg, double alpha) {
  alpha = std::clamp(alpha, 0.0, 1.0);
  const double inv = 1.0 - alpha;
  return CmuxArgbSet(0xff,
                     RoundChannel(CmuxArgbR(fg) * alpha + CmuxArgbR(bg) * inv),
                     RoundChannel(CmuxArgbG(fg) * alpha + CmuxArgbG(bg) * inv),
                     RoundChannel(CmuxArgbB(fg) * alpha + CmuxArgbB(bg) * inv));
}

CmuxArgb Elevate(CmuxArgb bg,
                 bool is_light,
                 double light_darken,
                 double dark_lighten) {
  return is_light ? Blend(CmuxRgb(0, 0, 0), bg, light_darken)
                  : Blend(CmuxRgb(255, 255, 255), bg, dark_lighten);
}

CmuxArgb TextOn(CmuxArgb fg, CmuxArgb bg, double alpha) {
  return Blend(fg, bg, alpha);
}

CmuxArgb AccentColor(const CmuxTheme& theme) {
  if (theme.cursor.has_value()) {
    return Opaque(*theme.cursor);
  }
  if (CmuxArgbA(theme.palette[4]) != 0) {
    return Opaque(theme.palette[4]);
  }
  return Opaque(theme.fg);
}

double LinearChannel(uint8_t channel) {
  const double s = channel / 255.0;
  if (s <= 0.04045) {
    return s / 12.92;
  }
  return std::pow((s + 0.055) / 1.055, 2.4);
}

}  // namespace

double RelativeLuminance(CmuxArgb color) {
  return 0.2126 * LinearChannel(CmuxArgbR(color)) +
         0.7152 * LinearChannel(CmuxArgbG(color)) +
         0.0722 * LinearChannel(CmuxArgbB(color));
}

double ContrastRatio(CmuxArgb a, CmuxArgb b) {
  const double la = RelativeLuminance(a);
  const double lb = RelativeLuminance(b);
  const double lighter = std::max(la, lb);
  const double darker = std::min(la, lb);
  return (lighter + 0.05) / (darker + 0.05);
}

CmuxThemePalette::CmuxThemePalette() = default;
CmuxThemePalette::CmuxThemePalette(const CmuxThemePalette&) = default;
CmuxThemePalette& CmuxThemePalette::operator=(const CmuxThemePalette&) =
    default;
CmuxThemePalette::~CmuxThemePalette() = default;

CmuxThemePalette DeriveUiPalette(const CmuxTheme& theme) {
  const CmuxArgb bg = Opaque(theme.bg);
  const CmuxArgb fg = Opaque(theme.fg);
  const bool is_light = RelativeLuminance(bg) > kLightThreshold;
  const CmuxArgb accent = AccentColor(theme);

  CmuxThemePalette out;
  out.is_light = is_light;
  out.window_bg = bg;
  out.content_bg =
      Elevate(bg, is_light, kLightContentDarken, kDarkContentLighten);
  out.rail_bg = Elevate(bg, is_light, kLightRailDarken, kDarkRailLighten);
  out.tab_strip_bg =
      Elevate(bg, is_light, kLightStripDarken, kDarkStripLighten);
  out.strip_view_bg = out.tab_strip_bg;

  const CmuxArgb text = TextOn(fg, out.rail_bg, kTextDimAlpha);
  const CmuxArgb folder = TextOn(fg, out.rail_bg, kTextFolderAlpha);
  const CmuxArgb faint = TextOn(fg, out.rail_bg, kTextFaintAlpha);
  out.rail_sel_bg = Blend(fg, out.rail_bg, kSelectionBgAlpha);
  out.rail_hover_bg = Blend(fg, out.rail_bg, kHoverBgAlpha);
  out.rail_hover_text = fg;
  out.rail_drag_bg = theme.selection_bg.has_value()
                         ? Opaque(*theme.selection_bg)
                         : Blend(fg, out.rail_bg, kDragBgAlpha);
  out.rail_text = text;
  out.rail_sel_text =
      theme.selection_fg.has_value() ? Opaque(*theme.selection_fg) : fg;
  out.rail_folder_text = folder;
  out.rail_chevron = text;
  out.rail_badge_bg = Blend(fg, out.rail_bg, kBadgeBgAlpha);
  out.rail_badge_text = faint;
  out.rail_close_color = text;
  out.rail_plus_text = folder;
  out.rail_plus_bg = out.rail_bg;
  out.rail_plus_hover_bg = Blend(fg, out.rail_bg, kPlusHoverBgAlpha);
  out.rail_indent_guide = Blend(fg, out.rail_bg, kIndentGuideAlpha);
  out.rail_scrollbar_thumb = Blend(fg, out.rail_bg, kScrollbarThumbAlpha);
  out.rail_accent = accent;

  out.layout_accent = accent;
  out.layout_focus = accent;

  out.tab_active_bg =
      Elevate(bg, is_light, kLightTabActiveDarken, kDarkTabActiveLighten);
  out.tab_hover_bg = is_light
                         ? Elevate(bg, is_light, kLightTabHoverDarken, 0.0)
                         : Elevate(bg, is_light, 0.0, kDarkTabHoverLighten);
  out.tab_active_text = fg;
  out.tab_idle_text = TextOn(fg, out.tab_strip_bg, kTextDimAlpha);
  out.tab_plus_text = TextOn(fg, out.tab_strip_bg, kTextFolderAlpha);
  out.tab_accent = accent;

  out.chrome_material_bg =
      WithAlpha(out.rail_bg, kChromeMaterialCanvasAlpha);
  out.chrome_material_tab_active_bg =
      WithAlpha(out.tab_active_bg, kChromeMaterialEmphasisAlpha);
  out.chrome_material_tab_hover_bg =
      WithAlpha(out.tab_hover_bg, kChromeMaterialHoverAlpha);
  out.chrome_material_omnibox_bg =
      WithAlpha(out.tab_active_bg, kChromeMaterialEmphasisAlpha);

  out.chrome_omnibox_text = fg;
  out.chrome_omnibox_focus =
      is_light ? kHeliumFocusLight : kHeliumFocusDark;
  out.chrome_omnibox_selection_bg =
      theme.selection_bg.has_value() ? Opaque(*theme.selection_bg)
                                     : out.chrome_omnibox_focus;
  out.chrome_omnibox_selection_text =
      theme.selection_fg.has_value()
          ? Opaque(*theme.selection_fg)
          : (is_light ? CmuxRgb(255, 255, 255) : CmuxRgb(0, 0, 0));

  out.pane_focus_border = accent;
  out.pane_idle_border =
      Elevate(bg, is_light, kLightIdleBorderDarken, kDarkIdleBorderLighten);

  out.strip_view_focus_border = accent;
  out.strip_view_idle_border = out.pane_idle_border;

  // Chrome's toolbar starts from the Ghostty-derived tab-strip surface. The
  // final Chrome mixer derives Helium's location-bar state surfaces from it.
  out.chrome_seed_color = out.tab_strip_bg;
  return out;
}

}  // namespace cmux
