// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef CHROME_BROWSER_CMUX_TERM_CMUX_LAYOUT_CONFIG_H_
#define CHROME_BROWSER_CMUX_TERM_CMUX_LAYOUT_CONFIG_H_

#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "base/callback_list.h"
#include "base/functional/callback_forward.h"
#include "chrome/browser/cmux_term/cmux_new_tab_page.h"
#include "chrome/browser/cmux_term/cmux_sidebar_metrics.h"
#include "third_party/skia/include/core/SkColor.h"

namespace cmux {

inline constexpr char kLayoutDefaultAccentColor[] = "#8a8f98";
inline constexpr char kLayoutDefaultDropHighlightColor[] = "#007aff";
inline constexpr char kLayoutDefaultFocusColor[] = "#007aff";
inline constexpr char kLayoutDefaultChromeSurfaceColor[] = "";

enum class SidebarPosition {
  kLeft,
  kRight,
};

// Persisted geometry for the cmux window chrome and strip. animation_ms is the
// global animation scale: 160ms == 1.0x bonsplit pane timing.
struct LayoutConfig {
  int rail_width = 200;
  int sidebar_icon_width = sidebar_metrics::kIconsWidth;
  int gap = 10;
  int margin = 8;
  int min_column_width = 360;
  double column_fraction = 0.62;
  // Cmd/Ctrl-R cycles through these fractions in order. Keeping the modes in
  // an array makes both their values and their count user-configurable.
  std::vector<double> column_width_modes = {1.0, 2.0 / 3.0, 0.5, 1.0 / 3.0};
  int header_height = 38;
  sidebar_metrics::SidebarMode sidebar_mode =
      sidebar_metrics::SidebarMode::kExpanded;
  SidebarPosition sidebar_position = SidebarPosition::kLeft;
  int focus_border = 1;
  bool animations = true;
  int animation_ms = 160;
  std::string accent_color = kLayoutDefaultAccentColor;
  // Kept independent from accent_color so Ghostty's ANSI palette cannot
  // recolor Bonsplit-style drag feedback.
  std::string drop_highlight_color = kLayoutDefaultDropHighlightColor;
  // Independently configurable, but blue by default on every Ghostty theme.
  std::string focus_color = kLayoutDefaultFocusColor;
  // Empty means derive from the active Ghostty theme. These independent
  // overrides intentionally live in the advanced JSON config.
  std::string chrome_tab_bar_color = kLayoutDefaultChromeSurfaceColor;
  std::string chrome_toolbar_color = kLayoutDefaultChromeSurfaceColor;
  std::string chrome_omnibox_color = kLayoutDefaultChromeSurfaceColor;
  std::string chrome_omnibox_popup_color = kLayoutDefaultChromeSurfaceColor;
  std::string chrome_omnibox_popup_hover_color =
      kLayoutDefaultChromeSurfaceColor;
  bool ghostty_theme = true;
  // Empty follows the theme selected by the user's Ghostty configuration.
  // Otherwise this is the basename of one bundled Ghostty theme.
  std::string ghostty_theme_name;

  // Visibility of cmux's pane-local toolbar controls. These mirror Helium's
  // Customize Toolbar surface, with Downloads added because cmux renders it as
  // a first-class control. Home follows Chromium's hidden-by-default behavior.
  bool toolbar_show_back = true;
  bool toolbar_show_forward = true;
  bool toolbar_show_reload = true;
  bool toolbar_show_home = false;
  bool toolbar_show_extensions = true;
  bool toolbar_show_downloads = true;
  bool toolbar_show_media = true;
  bool toolbar_show_profile = true;
  bool toolbar_show_menu = true;
  std::set<std::string> present_keys;

  LayoutConfig();
  LayoutConfig(const LayoutConfig&);
  LayoutConfig& operator=(const LayoutConfig&);
  ~LayoutConfig();

  std::string ToJson() const;
  bool HasUserKey(std::string_view key) const;
  // Returns false if `json` could not be parsed; `out` stays unchanged.
  static bool FromJson(const std::string& json, LayoutConfig* out);
};

bool ParseLayoutHexColor(std::string_view text, SkColor* out);
SkColor LayoutAccentColor(const LayoutConfig& config);
SkColor LayoutDropHighlightColor(const LayoutConfig& config);
SkColor LayoutFocusColor(const LayoutConfig& config);

// Persist/restore the config to/from a fixed file. The default path is
// CMUX_LAYOUT_CONFIG, else <home>/.cmux_layout.json. Load returns nullopt if
// absent or malformed.
std::string LayoutConfigPath();
void LoadLayoutConfigAsync(
    LayoutConfig defaults,
    base::OnceCallback<void(std::optional<LayoutConfig>)> callback);
bool SaveLayoutConfig(const LayoutConfig& config);

// The last configuration published by the main cmux window or settings page.
// Consumers use this to update pane-local UI and disposable terminal renderers
// without maintaining a second preference store.
std::optional<LayoutConfig> GetPublishedLayoutConfig();
using LayoutConfigChangedCallback =
    base::RepeatingCallback<void(const LayoutConfig&)>;
base::CallbackListSubscription AddLayoutConfigChangedCallback(
    LayoutConfigChangedCallback callback);
void PublishLayoutConfig(const LayoutConfig& config);

// Application-level browser settings read from the shared cmux JSONC config.
// This browser fork never rewrites that document, preserving its comments and
// settings owned by the main cmux app.
struct BrowserConfig {
  // "blank" for about:blank, or "new_tab" for Chrome's New Tab page.
  std::string new_tab_page = kBrowserDefaultNewTabPage;
  // Shared with the native cmux app. When false, neither anonymous PostHog
  // activity nor Sentry crash reports leave the machine.
  bool send_anonymous_telemetry = true;

  // Returns false if `json` could not be parsed; `out` stays unchanged.
  static bool FromJson(const std::string& json, BrowserConfig* out);
};

enum class BrowserConfigLoadStatus {
  kAbsent,
  kLoaded,
  kError,
};

struct BrowserConfigLoadResult {
  BrowserConfigLoadStatus status = BrowserConfigLoadStatus::kError;
  std::optional<BrowserConfig> config;
};

// An absolute CMUX_CONFIG, else $XDG_CONFIG_HOME/cmux/cmux.json, else
// ~/.config/cmux/cmux.json. Privacy-sensitive loaders reject a relative
// CMUX_CONFIG so working-directory changes cannot reinterpret an opt-out.
std::string CmuxConfigPath();
void LoadBrowserConfigAsync(
    base::OnceCallback<void(std::optional<BrowserConfig>)> callback);
// Unlike the UI convenience loader above, this preserves the distinction
// between a genuinely absent config and an unreadable or malformed one so
// privacy-sensitive callers can fail closed.
void LoadBrowserConfigWithStatusAsync(
    base::OnceCallback<void(BrowserConfigLoadResult)> callback);

}  // namespace cmux

#endif  // CHROME_BROWSER_CMUX_TERM_CMUX_LAYOUT_CONFIG_H_
