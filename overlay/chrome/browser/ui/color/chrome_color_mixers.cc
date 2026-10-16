// Copyright 2021 The Chromium Authors
// Copyright 2026 Manaflow, Inc.
// Portions derived from Helium; Helium contributors retain copyright.
// SPDX-License-Identifier: GPL-3.0-or-later AND GPL-3.0-only AND BSD-3-Clause
//
// See docs/source-provenance.md for the licensed regions and source pins.

#include "chrome/browser/ui/color/chrome_color_mixers.h"

#include <memory>
#include <optional>
#include <string_view>

#include "base/containers/fixed_flat_map.h"
#include "base/functional/bind.h"
#include "base/no_destructor.h"
#include "chrome/browser/ui/color/chrome_color_id.h"
#include "chrome/browser/ui/color/cmux_chrome_surface_colors.h"
#include "chrome/browser/ui/color/chrome_color_mixer.h"
#include "chrome/browser/ui/color/material_chrome_color_mixer.h"
#include "chrome/browser/ui/color/material_new_tab_page_color_mixer.h"
#include "chrome/browser/ui/color/material_omnibox_color_mixer.h"
#include "chrome/browser/ui/color/material_side_panel_color_mixer.h"
#include "chrome/browser/ui/color/material_tab_strip_color_mixer.h"
#include "chrome/browser/ui/color/native_chrome_color_mixer.h"
#include "chrome/browser/ui/color/new_tab_page_color_mixer.h"
#include "chrome/browser/ui/color/omnibox_color_mixer.h"
#include "chrome/browser/ui/color/product_specifications_color_mixer.h"
#include "chrome/browser/ui/color/projects_panel_color_mixer.h"
#include "chrome/browser/ui/color/tab_strip_color_mixer.h"
#include "third_party/skia/include/core/SkColor.h"
#include "ui/color/color_mixer.h"
#include "ui/color/color_provider.h"
#include "ui/color/color_provider_key.h"
#include "ui/color/color_provider_manager.h"
#include "ui/color/color_provider_utils.h"
#include "ui/color/color_recipe.h"
#include "ui/color/ref_color_mixer.h"
#include "ui/color/sys_color_mixer.h"
#include "ui/gfx/color_palette.h"

namespace {

std::optional<CmuxChromeSurfaceColors> g_cmux_surface_colors;

class ChromeColorProviderUtilsCallbacks
    : public ui::ColorProviderUtilsCallbacks {
 public:
  bool ColorIdName(ui::ColorId color_id, std::string_view* color_name) override;
};

#include "ui/color/color_id_map_macros.inc"

bool ChromeColorProviderUtilsCallbacks::ColorIdName(
    ui::ColorId color_id,
    std::string_view* color_name) {
  static constexpr const auto chrome_color_id_map =
      base::MakeFixedFlatMap<ui::ColorId, const char*>({CHROME_COLOR_IDS});
  auto i = chrome_color_id_map.find(color_id);
  if (i != chrome_color_id_map.cend()) {
    *color_name = i->second;
    return true;
  }
  return false;
}

// Note that this second include is not redundant. The second inclusion of the
// .inc file serves to undefine the macros the first inclusion defined.
#include "ui/color/color_id_map_macros.inc"

// While a Ghostty palette is active, keep the outer toolbar tied to the
// tab-strip surface and translate Helium's omnibox state graph onto those
  // surfaces. Geometry is carried by patches/helium-omnibar-chromium-149.patch;
  // this mixer owns only the theme-dependent part.
void AddCmuxGhosttyChromeSurfaceMixer(ui::ColorProvider* provider,
                                      const ui::ColorProviderKey& key) {
  if (!g_cmux_surface_colors.has_value()) {
    return;
  }

  const CmuxChromeSurfaceColors& colors = *g_cmux_surface_colors;

  // Ghostty's background is a surface, not a Material accent seed. Reinstall
  // Chromium's stock baseline ref and sys layers before applying the handful
  // of surfaces that are intentionally Ghostty-themed. This also makes the
  // result deterministic when a plain or descendant Widget inherits a native
  // platform accent. Color recipes resolve lazily, so the already-installed
  // Material and Chrome recipes follow the replacement system tokens without
  // per-control overrides.
  ui::ColorProviderKey baseline_key = key;
  baseline_key.user_color.reset();
  baseline_key.user_color_source =
      ui::ColorProviderKey::UserColorSource::kBaseline;
  baseline_key.scheme_variant.reset();
  ui::AddRefColorMixer(provider, baseline_key);
  ui::AddSysColorMixer(provider, baseline_key);

  ui::ColorMixer& mixer = provider->AddMixer();
  mixer[kColorToolbar] = {colors.toolbar};
  mixer[kColorNewTabPageBackground] = {colors.new_tab_background};
  mixer[kColorNewTabPageBackgroundOverride] = {colors.new_tab_background};

  // Preserve Helium's actual location-bar state graph. Its light bar uses the
  // Surface5/active-tab fill, while its dark bar aliases the omnibox results
  // surface. A cmux-provided omnibox color (an explicit override or the
  // macOS material emphasis layer) wins in either mode. Keep the dark
  // subtle-emphasis token on the toolbar surface, as Helium does with its
  // darker omnibox-container token; it is not the visible dark bar fill.
  mixer[kColorOmniboxResultsBackground] = {colors.omnibox_popup};
  mixer[kColorOmniboxResultsBackgroundIph] = {colors.omnibox_popup};
  if (colors.omnibox.has_value()) {
    mixer[kColorToolbarBackgroundSubtleEmphasis] = {*colors.omnibox};
    mixer[kColorLocationBarBackground] = {*colors.omnibox};
  } else if (key.color_mode == ui::ColorProviderKey::ColorMode::kDark) {
    mixer[kColorToolbarBackgroundSubtleEmphasis] = {colors.toolbar};
    mixer[kColorLocationBarBackground] = {kColorOmniboxResultsBackground};
  } else {
    mixer[kColorToolbarBackgroundSubtleEmphasis] = {colors.omnibox_surface};
    mixer[kColorLocationBarBackground] = {colors.omnibox_surface};
  }
  mixer[kColorToolbarBackgroundSubtleEmphasisHovered] =
      ui::GetResultingPaintColor(ui::kColorSysStateHoverBrightBlendProtection,
                                 kColorToolbarBackgroundSubtleEmphasis);
  mixer[kColorLocationBarBackgroundHovered] =
      ui::GetResultingPaintColor(ui::kColorSysStateHoverBrightBlendProtection,
                                 kColorLocationBarBackground);
  if (colors.omnibox_popup_hover.has_value()) {
    mixer[kColorOmniboxResultsBackgroundHovered] = {
        *colors.omnibox_popup_hover};
  } else {
    mixer[kColorOmniboxResultsBackgroundHovered] =
        ui::GetResultingPaintColor(ui::kColorSysStateHoverOnSubtle,
                                   kColorOmniboxResultsBackground);
  }
  mixer[kColorOmniboxResultsBackgroundSelected] = {
      kColorOmniboxResultsBackgroundHovered};

  // Helium primary-40/primary-80 blue is selected by the Ghostty-derived
  // light/dark mode. Text and optional selection colors come from Ghostty.
  mixer[ui::kColorSysStateFocusRing] = {colors.omnibox_focus};
  mixer[ui::kColorFocusableBorderFocused] = {colors.omnibox_focus};
  mixer[kColorOmniboxResultsFocusIndicator] = {colors.omnibox_focus};
  mixer[kColorOmniboxText] = {colors.omnibox_text};
  mixer[kColorOmniboxResultsTextSelected] = {colors.omnibox_text};
  mixer[kColorOmniboxSelectionBackground] = {
      colors.omnibox_selection_bg};
  mixer[kColorOmniboxSelectionForeground] = {
      colors.omnibox_selection_text};
}

}  // namespace

void SetCmuxChromeSurfaceColors(
    std::optional<CmuxChromeSurfaceColors> colors) {
  if (g_cmux_surface_colors == colors) {
    return;
  }
  g_cmux_surface_colors = std::move(colors);
  ui::ColorProviderManager::Get().ResetColorProviderCache();
}

void AddChromeColorMixers(ui::ColorProvider* provider,
                          const ui::ColorProviderKey& key) {
  static base::NoDestructor<ChromeColorProviderUtilsCallbacks>
      chrome_color_provider_utils_callbacks;
  ui::SetColorProviderUtilsCallbacks(
      chrome_color_provider_utils_callbacks.get());
  AddChromeColorMixer(provider, key);
  AddNewTabPageColorMixer(provider, key);
  AddOmniboxColorMixer(provider, key);
  AddProductSpecificationsColorMixer(provider, key);
  AddProjectsPanelColorMixer(provider, key);
  AddTabStripColorMixer(provider, key);

  AddMaterialChromeColorMixer(provider, key);
  AddMaterialNewTabPageColorMixer(provider, key);
  AddMaterialOmniboxColorMixer(provider, key);
  AddMaterialSidePanelColorMixer(provider, key);
  AddMaterialTabStripColorMixer(provider, key);

  // Must be the last one in order to override other mixer colors.
  AddNativeChromeColorMixer(provider, key);

  if (key.custom_theme) {
    key.custom_theme->AddColorMixers(provider, key);
  }

  if (key.app_controller) {
    key.app_controller->AddColorMixers(provider, key);
  }

  // Keep this after every upstream supplier so the neutral cmux surfaces
  // cannot be re-tinted by the Material or autogenerated-theme mixers.
  AddCmuxGhosttyChromeSurfaceMixer(provider, key);
}
