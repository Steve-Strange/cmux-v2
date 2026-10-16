// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef CHROME_BROWSER_CMUX_TERM_CMUX_RAIL_CONFIG_H_
#define CHROME_BROWSER_CMUX_TERM_CMUX_RAIL_CONFIG_H_

#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>

#include "base/functional/callback_forward.h"
#include "chrome/browser/cmux_term/cmux_sidebar_metrics.h"
#include "third_party/skia/include/core/SkColor.h"

namespace cmux {

// Every visual/behavioral knob of the workspace rail (CmuxRail), in one
// serializable struct so the look can be dialed in live and persisted to disk
// (see Load/SaveRailConfig).
struct RailConfig {
  // ---- Geometry ----
  // Vertical-tab values track the pinned Helium 8030a8a3 reference's
  // patches/helium/ui/layout/vertical.patch.
  int row_height = sidebar_metrics::kRowHeight;
  int row_gap = sidebar_metrics::kRowGap;
  int top_pad = 0;  // Helium tab-strip view starts flush at the top
  int outer_horizontal_inset = sidebar_metrics::kOuterHorizontalInset;
  int left_pad = sidebar_metrics::kInnerHorizontalPadding;
  int right_pad = sidebar_metrics::kInnerHorizontalPadding;
  int indent_per_depth = 14;  // nesting indent per tree depth
  int lead_width = 18;        // leading column (chevron / icon)
  int row_corner = sidebar_metrics::kCornerRadius;
  int hover_corner = sidebar_metrics::kCornerRadius;
  int pill_inset_v = 0;    // row itself is Helium's 30 DIP detached tab
  int pill_inset_h = 0;    // strip container supplies the outer padding
  int plus_height = 38;    // deprecated footer height; parsed but unused
  int header_height = 38;  // top traffic-light/drag/plus band height
  int traffic_light_clearance = 72;  // empty top-left mac button clearance
  int header_button_size = 26;       // compact "+" button size
  int color_dot_size = 8;            // workspace color marker diameter
  int color_dot_gap = 7;             // gap after the color marker
  int scrollbar_width = 3;           // overlay thumb width

  // ---- Type ----
  int font_size = sidebar_metrics::kLabelSize;  // px; 0 = system default
  std::string font_family;  // "" = system default; e.g. "Menlo", "Georgia"
  int font_weight = 0;      // 0=normal 1=medium 2=semibold 3=bold (all rows)
  int selected_font_weight = 0;  // retained for config compatibility
  bool folder_bold = false;      // retained for config compatibility

  // ---- Colors (ARGB SkColor) ----
  SkColor bg = SkColorSetRGB(0x15, 0x17, 0x1c);
  SkColor sel_bg = SkColorSetA(SK_ColorWHITE, 0x20);
  SkColor hover_bg = SkColorSetA(SK_ColorWHITE, 0x11);
  SkColor hover_text =
      SkColorSetA(SK_ColorWHITE, 0xEE);  // label color on hover
  SkColor drag_bg = SkColorSetA(SK_ColorWHITE, 0x2C);
  SkColor text = SkColorSetA(SK_ColorWHITE, 0xA8);
  SkColor sel_text = SkColorSetA(SK_ColorWHITE, 0xF2);
  SkColor folder_text = SkColorSetA(SK_ColorWHITE, 0xC8);
  SkColor chevron = SkColorSetA(SK_ColorWHITE, 0xAA);
  SkColor badge_bg = SkColorSetA(SK_ColorWHITE, 0x12);
  SkColor badge_text = SkColorSetA(SK_ColorWHITE, 0x99);
  SkColor close_color = SkColorSetA(SK_ColorWHITE, 0xAA);
  SkColor plus_text = SkColorSetA(SK_ColorWHITE, 0xCC);
  SkColor plus_bg = SkColorSetA(SK_ColorWHITE, 0x00);
  SkColor plus_hover_bg = SkColorSetA(SK_ColorWHITE, 0x16);
  SkColor indent_guide = SkColorSetA(SK_ColorWHITE, 0x10);
  SkColor scrollbar_thumb = SkColorSetA(SK_ColorWHITE, 0x58);

  // ---- Toggles ----
  bool show_close_on_hover = true;  // X affordance on hover
  bool show_count_badge = false;    // folder child-count badge
  bool group_color_chevron = true;  // tint folder chevrons by group color
  bool show_header = true;          // deprecated; header is always present
  SkColor accent = SkColorSetRGB(0x8a, 0x8f, 0x98);  // header avatar accent
  bool animations = true;  // set by the window's layout config
  int animation_ms = 160;  // base duration set by the window's layout config

  // ---- Per-workspace custom leading icon (title -> arbitrary Unicode) ----
  // "*" is the default for unmatched titles. An empty exact/default value
  // intentionally hides the icon instead of falling back to a title initial.
  std::map<std::string, std::string> icons;
  std::set<std::string> present_keys;

  RailConfig();
  RailConfig(const RailConfig&);
  RailConfig& operator=(const RailConfig&);
  ~RailConfig();

  int RowPitch() const { return row_height + row_gap; }

  // Presets: 0=minimal (the defaults), 1=polished, 2=arc.
  static RailConfig Preset(int style);

  std::string ToJson() const;
  bool HasUserKey(std::string_view key) const;
  // Returns false if `json` couldn't be parsed (out stays unchanged).
  static bool FromJson(const std::string& json, RailConfig* out);
};

// Persist/restore the config to/from a fixed file. The default path is
// CMUX_RAIL_CONFIG, else <home>/.cmux_rail.json. Load returns nullopt if absent
// or malformed.
std::string ColorToHex(SkColor color);
SkColor HexToColor(const std::string& input, SkColor fallback);
std::string RailConfigPath();
void LoadRailConfigAsync(
    RailConfig defaults,
    base::OnceCallback<void(std::optional<RailConfig>)> callback);
bool SaveRailConfig(const RailConfig& config);

}  // namespace cmux

#endif  // CHROME_BROWSER_CMUX_TERM_CMUX_RAIL_CONFIG_H_
