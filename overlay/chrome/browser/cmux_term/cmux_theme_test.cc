// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Host-compilable unit test for the pure cmux theme model (no Chromium,
// no gtest). Build + run:
//
//   c++ -std=c++17 -I overlay \
//     overlay/chrome/browser/cmux_term/cmux_theme.cc \
//     overlay/chrome/browser/cmux_term/cmux_theme_test.cc \
//     -o /tmp/cmux_theme_test && /tmp/cmux_theme_test

#include "chrome/browser/cmux_term/cmux_theme.h"

#include <array>
#include <cstdio>
#include <string>

using namespace cmux;

namespace {

int g_failures = 0;
int g_checks = 0;

void Check(bool ok, const char* what) {
  ++g_checks;
  if (!ok) {
    ++g_failures;
    std::printf("  FAIL: %s\n", what);
  }
}

void CheckEq(CmuxArgb actual, CmuxArgb expected, const char* what) {
  ++g_checks;
  if (actual != expected) {
    ++g_failures;
    std::printf("  FAIL: %s: got #%08x expected #%08x\n", what, actual,
                expected);
  }
}

void CheckContrast(CmuxArgb fg, CmuxArgb bg, const char* what) {
  ++g_checks;
  const double contrast = ContrastRatio(fg, bg);
  if (contrast < 4.5) {
    ++g_failures;
    std::printf("  FAIL: %s contrast %.3f < 4.5\n", what, contrast);
  }
}

CmuxTheme AizenLight() {
  CmuxTheme theme;
  theme.bg = CmuxRgb(0xf0, 0xf2, 0xf6);
  theme.fg = CmuxRgb(0x4a, 0x4d, 0x66);
  theme.palette[4] = CmuxRgb(0x4d, 0x68, 0xb0);
  return theme;
}

CmuxTheme MonokaiClassic() {
  CmuxTheme theme;
  theme.bg = CmuxRgb(0x27, 0x28, 0x22);
  theme.fg = CmuxRgb(0xfd, 0xff, 0xf1);
  // The shipped Monokai Classic ANSI blue is intentionally warm. Keep cursor
  // unset so this fixture exercises the palette fallback that exposed broad
  // orange Chrome surfaces.
  theme.palette[4] = CmuxRgb(0xfd, 0x97, 0x1f);
  return theme;
}

void CheckCommonContrast(const CmuxTheme& theme,
                         const CmuxThemePalette& palette,
                         const char* name) {
  const CmuxArgb fg = theme.fg;
  CheckContrast(fg, palette.window_bg, (std::string(name) + " window").c_str());
  CheckContrast(fg, palette.content_bg,
                (std::string(name) + " content").c_str());
  CheckContrast(fg, palette.rail_bg, (std::string(name) + " rail").c_str());
  CheckContrast(fg, palette.tab_strip_bg,
                (std::string(name) + " tab strip").c_str());
  CheckContrast(fg, palette.tab_active_bg,
                (std::string(name) + " active tab").c_str());
}

void TestAizenLight() {
  CmuxTheme theme = AizenLight();
  CmuxThemePalette p = DeriveUiPalette(theme);
  Check(p.is_light, "Aizen Light detected as light");
  Check(p.window_bg != p.rail_bg && p.rail_bg != p.tab_strip_bg,
        "Aizen elevation tiers are distinct");
  Check(RelativeLuminance(p.window_bg) > RelativeLuminance(p.rail_bg),
        "Aizen rail darkens from window");
  Check(RelativeLuminance(p.rail_bg) > RelativeLuminance(p.tab_strip_bg),
        "Aizen strip darkens from rail");
  CheckCommonContrast(theme, p, "Aizen");

  CheckEq(p.window_bg, 0xfff0f2f6, "Aizen window bg");
  CheckEq(p.content_bg, 0xffeceef2, "Aizen content bg");
  CheckEq(p.rail_bg, 0xffe5e7eb, "Aizen rail bg");
  CheckEq(p.rail_hover_bg, 0xffd9dbe0, "Aizen rail hover bg");
  CheckEq(p.rail_hover_text, 0xff4a4d66, "Aizen rail hover text");
  CheckEq(p.tab_strip_bg, 0xffe0e2e6, "Aizen tab strip bg");
  CheckEq(p.tab_active_bg, 0xffd9dbdf, "Aizen tab active bg");
  CheckEq(p.tab_hover_bg, 0xffe5e7eb, "Aizen tab hover bg");
  CheckEq(p.tab_active_text, 0xff4a4d66, "Aizen tab active text");
  CheckEq(p.tab_idle_text, 0xff777a8c, "Aizen tab idle text");
  CheckEq(p.tab_plus_text, 0xff62657a, "Aizen tab plus text");
  CheckEq(p.tab_accent, 0xff4d68b0, "Aizen tab accent");
  CheckEq(p.chrome_material_bg, 0x00e5e7eb,
          "Aizen material chrome canvas");
  CheckEq(p.chrome_material_tab_active_bg, 0xd9d9dbdf,
          "Aizen material active tab");
  CheckEq(p.chrome_material_tab_hover_bg, 0x99e5e7eb,
          "Aizen material hovered tab");
  CheckEq(p.chrome_material_omnibox_bg, 0xd9d9dbdf,
          "Aizen material omnibox");
  CheckEq(p.chrome_omnibox_text, 0xff4a4d66, "Aizen omnibox text");
  CheckEq(p.chrome_omnibox_focus, 0xff3551d2,
          "Aizen Helium omnibox focus");
  CheckEq(p.chrome_omnibox_selection_bg, 0xff3551d2,
          "Aizen Helium omnibox selection");
  CheckEq(p.chrome_omnibox_selection_text, 0xffffffff,
          "Aizen omnibox selected text");
  CheckEq(p.rail_sel_bg, 0xffcfd1d8, "Aizen rail selection bg");
  CheckEq(p.rail_drag_bg, 0xffc6c8d0, "Aizen rail drag bg");
  CheckEq(p.rail_text, 0xff797b8e, "Aizen rail text");
  CheckEq(p.rail_sel_text, 0xff4a4d66, "Aizen rail selection text");
  CheckEq(p.rail_folder_text, 0xff63667b, "Aizen rail folder text");
  CheckEq(p.rail_chevron, 0xff797b8e, "Aizen rail chevron");
  CheckEq(p.rail_badge_bg, 0xffd6d8de, "Aizen rail badge bg");
  CheckEq(p.rail_badge_text, 0xff9fa2af, "Aizen rail badge text");
  CheckEq(p.rail_close_color, 0xff797b8e, "Aizen rail close color");
  CheckEq(p.rail_plus_text, 0xff63667b, "Aizen rail plus text");
  CheckEq(p.rail_plus_bg, 0xffe5e7eb, "Aizen rail plus bg");
  CheckEq(p.rail_plus_hover_bg, 0xffd6d8de, "Aizen rail plus hover bg");
  CheckEq(p.rail_indent_guide, 0xffd6d8de, "Aizen rail indent guide");
  CheckEq(p.rail_scrollbar_thumb, 0xffb0b3be, "Aizen rail scrollbar thumb");
  CheckEq(p.rail_accent, 0xff4d68b0, "Aizen rail accent");
  CheckEq(p.layout_accent, 0xff4d68b0, "Aizen accent");
  CheckEq(p.layout_focus, 0xff4d68b0, "Aizen focus");
  CheckEq(p.pane_focus_border, 0xff4d68b0, "Aizen pane focus border");
  CheckEq(p.pane_idle_border, 0xffcacbcf, "Aizen pane idle border");
  CheckEq(p.strip_view_bg, 0xffe0e2e6, "Aizen strip view bg");
  CheckEq(p.strip_view_focus_border, 0xff4d68b0,
          "Aizen strip view focus border");
  CheckEq(p.strip_view_idle_border, 0xffcacbcf, "Aizen strip view idle border");
  CheckEq(p.chrome_seed_color, p.tab_strip_bg, "Aizen Chrome/tab-strip surface");
}

void TestMonokaiClassic() {
  CmuxTheme theme = MonokaiClassic();
  CmuxThemePalette p = DeriveUiPalette(theme);
  Check(!p.is_light, "Monokai Classic detected as dark");
  Check(p.window_bg != p.rail_bg && p.rail_bg != p.tab_strip_bg,
        "Monokai elevation tiers are distinct");
  Check(RelativeLuminance(p.window_bg) < RelativeLuminance(p.rail_bg),
        "Monokai rail lightens from window");
  Check(RelativeLuminance(p.rail_bg) < RelativeLuminance(p.tab_strip_bg),
        "Monokai strip lightens from rail");
  CheckCommonContrast(theme, p, "Monokai");

  CheckEq(p.window_bg, 0xff272822, "Monokai window bg");
  CheckEq(p.content_bg, 0xff2c2d28, "Monokai content bg");
  CheckEq(p.rail_bg, 0xff33342e, "Monokai rail bg");
  CheckEq(p.rail_hover_bg, 0xff43443e, "Monokai rail hover bg");
  CheckEq(p.rail_hover_text, 0xfffdfff1, "Monokai rail hover text");
  CheckEq(p.tab_strip_bg, 0xff373833, "Monokai tab strip bg");
  CheckEq(p.tab_active_bg, 0xff4a4a45, "Monokai tab active bg");
  CheckEq(p.tab_hover_bg, 0xff3d3e38, "Monokai tab hover bg");
  CheckEq(p.tab_active_text, 0xfffdfff1, "Monokai tab active text");
  CheckEq(p.tab_idle_text, 0xffc2c3b8, "Monokai tab idle text");
  CheckEq(p.tab_plus_text, 0xffdddfd3, "Monokai tab plus text");
  CheckEq(p.tab_accent, 0xfffd971f, "Monokai tab accent");
  CheckEq(p.chrome_material_bg, 0x0033342e,
          "Monokai material chrome canvas");
  CheckEq(p.chrome_material_tab_active_bg, 0xd94a4a45,
          "Monokai material active tab");
  CheckEq(p.chrome_material_tab_hover_bg, 0x993d3e38,
          "Monokai material hovered tab");
  CheckEq(p.chrome_material_omnibox_bg, 0xd94a4a45,
          "Monokai material omnibox");
  CheckEq(p.chrome_omnibox_text, 0xfffdfff1, "Monokai omnibox text");
  CheckEq(p.chrome_omnibox_focus, 0xffbac3ff,
          "Monokai Helium omnibox focus");
  CheckEq(p.chrome_omnibox_selection_bg, 0xffbac3ff,
          "Monokai Helium omnibox selection");
  CheckEq(p.chrome_omnibox_selection_text, 0xff000000,
          "Monokai omnibox selected text");
  CheckEq(p.rail_sel_bg, 0xff4f5049, "Monokai rail selection bg");
  CheckEq(p.rail_drag_bg, 0xff5b5d55, "Monokai rail drag bg");
  CheckEq(p.rail_text, 0xffc0c2b7, "Monokai rail text");
  CheckEq(p.rail_sel_text, 0xfffdfff1, "Monokai rail selection text");
  CheckEq(p.rail_folder_text, 0xffdddfd2, "Monokai rail folder text");
  CheckEq(p.rail_chevron, 0xffc0c2b7, "Monokai rail chevron");
  CheckEq(p.rail_badge_bg, 0xff474842, "Monokai rail badge bg");
  CheckEq(p.rail_badge_text, 0xff8e8f86, "Monokai rail badge text");
  CheckEq(p.rail_close_color, 0xffc0c2b7, "Monokai rail close color");
  CheckEq(p.rail_plus_text, 0xffdddfd2, "Monokai rail plus text");
  CheckEq(p.rail_plus_bg, 0xff33342e, "Monokai rail plus bg");
  CheckEq(p.rail_plus_hover_bg, 0xff474842, "Monokai rail plus hover bg");
  CheckEq(p.rail_indent_guide, 0xff474842, "Monokai rail indent guide");
  CheckEq(p.rail_scrollbar_thumb, 0xff787970, "Monokai rail scrollbar thumb");
  CheckEq(p.rail_accent, 0xfffd971f, "Monokai rail accent");
  CheckEq(p.layout_accent, 0xfffd971f, "Monokai accent");
  CheckEq(p.layout_focus, 0xfffd971f, "Monokai focus");
  CheckEq(p.pane_focus_border, 0xfffd971f, "Monokai pane focus border");
  CheckEq(p.pane_idle_border, 0xff4e4f4a, "Monokai pane idle border");
  CheckEq(p.strip_view_bg, 0xff373833, "Monokai strip view bg");
  CheckEq(p.strip_view_focus_border, 0xfffd971f,
          "Monokai strip view focus border");
  CheckEq(p.strip_view_idle_border, 0xff4e4f4a,
          "Monokai strip view idle border");
  CheckEq(p.chrome_seed_color, p.tab_strip_bg,
          "Monokai Chrome/tab-strip surface");
}

void TestOptionals() {
  CmuxTheme theme = AizenLight();
  theme.cursor = CmuxRgb(0xff, 0x33, 0x66);
  theme.selection_bg = CmuxRgb(0x12, 0x34, 0x56);
  theme.selection_fg = CmuxRgb(0xee, 0xdd, 0xcc);
  CmuxThemePalette p = DeriveUiPalette(theme);
  CheckEq(p.layout_accent, 0xffff3366, "cursor overrides accent");
  CheckEq(p.chrome_seed_color, p.tab_strip_bg,
          "cursor accent leaves shared Chrome surface unchanged");
  CheckEq(p.rail_drag_bg, 0xff123456, "selection background drives drag");
  CheckEq(p.rail_sel_text, 0xffeeddcc,
          "selection foreground drives selected text");
  CheckEq(p.chrome_omnibox_selection_bg, 0xff123456,
          "selection background drives omnibox highlight");
  CheckEq(p.chrome_omnibox_selection_text, 0xffeeddcc,
          "selection foreground drives omnibox selected text");
}

void TestAccentFallbackToForeground() {
  CmuxTheme theme = AizenLight();
  theme.palette[4] = 0;
  CmuxThemePalette p = DeriveUiPalette(theme);
  CheckEq(p.layout_accent, theme.fg, "unset cursor and ANSI blue fall back fg");
  CheckEq(p.chrome_seed_color, p.tab_strip_bg,
          "Chrome surface is independent from accent fallback");
}

}  // namespace

int main() {
  std::printf("cmux_theme_test\n");
  TestAizenLight();
  TestMonokaiClassic();
  TestOptionals();
  TestAccentFallbackToForeground();
  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
