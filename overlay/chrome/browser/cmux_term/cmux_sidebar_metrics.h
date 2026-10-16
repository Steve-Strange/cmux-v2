// Copyright 2012 The Chromium Authors
// Copyright 2026 Manaflow, Inc.
// Portions derived from Helium; Helium contributors retain copyright.
// SPDX-License-Identifier: GPL-3.0-or-later AND GPL-3.0-only AND BSD-3-Clause
//
// See docs/source-provenance.md for the licensed regions and source pins.

#ifndef CHROME_BROWSER_CMUX_TERM_CMUX_SIDEBAR_METRICS_H_
#define CHROME_BROWSER_CMUX_TERM_CMUX_SIDEBAR_METRICS_H_

#include <algorithm>

namespace cmux::sidebar_metrics {

enum class SidebarMode {
  kExpanded,
  kIcons,
  kHidden,
};

// Clean-room equivalents of the dimensions used by Helium's vertical strip.
inline constexpr int kRowHeight = 30;
inline constexpr int kRowGap = 2;
inline constexpr int kOuterHorizontalInset = 6;
inline constexpr int kInnerHorizontalPadding = 7;
inline constexpr int kCornerRadius = 8;
inline constexpr int kLabelSize = 12;
inline constexpr int kCloseButtonSize = 16;
inline constexpr int kMinimumContentsWidthForCloseButton = 56;
inline constexpr int kHoverDelayMs = 50;
inline constexpr int kExpandedWidth = 200;
inline constexpr int kMinimumWidth = 160;
inline constexpr int kMaximumWidth = 400;
// Helium inherits Chromium's 15 DIP default-width snap and adds an inclusive
// 6 DIP snap around the same value. The inherited range is wider, so it is the
// effective boundary in Helium's patched build.
inline constexpr int kResizeSnapDistance = 15;
inline constexpr int kDefaultWidthSnapDistance = 6;
// Helium's collapsed strip is exactly one 30 DIP control plus 6 DIP of
// horizontal padding on each side. Keep icon mode fixed to that geometry:
// allowing a wider persisted value would make its targets rectangular.
inline constexpr int kIconsWidth =
    kRowHeight + 2 * kOuterHorizontalInset;  // 42
inline constexpr int kMinimumIconsWidth = kIconsWidth;
inline constexpr int kMaximumIconsWidth = kIconsWidth;
inline constexpr int kCollapseSnapWidth =
    (kMinimumWidth + kIconsWidth) / 2;  // 101
inline constexpr int kExpandedResizeAreaWidth = 5;
inline constexpr int kIconsResizeAreaWidth = 2;

inline constexpr int kFooterHeight = 30;
inline constexpr int kFooterGap = 2;
inline constexpr int kFooterSideAndBottomInset = 6;
inline constexpr int kFooterIconSize = 16;
inline constexpr int kFooterVerticalPadding = 4;
inline constexpr int kFooterHorizontalPadding = 7;
inline constexpr int kFooterImageLabelGap = 7;

constexpr int InsetWidth(int available_width, int horizontal_inset) {
  return std::max(0, available_width - 2 * horizontal_inset);
}

// During the normal -> icons animation the rail can still be wider than its
// 42 DIP endpoint. In that case keep the control attached to the same 6 DIP
// edges so it shrinks continuously. At and below the endpoint, retain a
// centered 30x30 control and let the rail clip it as it animates to hidden.
constexpr int ControlWidthForMode(SidebarMode mode,
                                  int available_width,
                                  int expanded_horizontal_inset) {
  if (mode == SidebarMode::kExpanded) {
    return InsetWidth(available_width, expanded_horizontal_inset);
  }
  return std::max(kRowHeight,
                  InsetWidth(available_width, kOuterHorizontalInset));
}

constexpr int ControlXForMode(SidebarMode mode,
                              int available_width,
                              int expanded_horizontal_inset) {
  if (mode == SidebarMode::kExpanded) {
    return expanded_horizontal_inset;
  }
  return (available_width -
          ControlWidthForMode(mode, available_width,
                              expanded_horizontal_inset)) /
         2;
}

constexpr int RowHeightForMode(SidebarMode mode, int expanded_height) {
  return mode == SidebarMode::kExpanded ? expanded_height : kRowHeight;
}

constexpr int RowGapForMode(SidebarMode mode, int expanded_gap) {
  return mode == SidebarMode::kExpanded ? expanded_gap : kRowGap;
}

constexpr int TopPaddingForMode(SidebarMode mode, int expanded_top_padding) {
  return mode == SidebarMode::kExpanded ? expanded_top_padding
                                        : kOuterHorizontalInset;
}

constexpr bool ShouldShowCloseButton(bool enabled,
                                     bool active,
                                     bool hovered,
                                     bool focused,
                                     bool dragging,
                                     int available_contents_width) {
  return enabled && !dragging &&
         available_contents_width >= kMinimumContentsWidthForCloseButton &&
         (active || hovered || focused);
}

constexpr int FooterReservedHeight(bool update_ready = false) {
  return kFooterGap + kFooterHeight + kFooterSideAndBottomInset +
         (update_ready ? kFooterGap + kFooterHeight : 0);
}

constexpr int ClampWidth(int width) {
  return std::clamp(width, kMinimumWidth, kMaximumWidth);
}

constexpr int ClampIconsWidth(int width) {
  return std::clamp(width, kMinimumIconsWidth, kMaximumIconsWidth);
}

constexpr int WidthForMode(SidebarMode mode,
                           int expanded_width,
                           int icons_width) {
  switch (mode) {
    case SidebarMode::kExpanded:
      return ClampWidth(expanded_width);
    case SidebarMode::kIcons:
      return ClampIconsWidth(icons_width);
    case SidebarMode::kHidden:
      return 0;
  }
  return ClampWidth(expanded_width);
}

constexpr bool ShowsLabels(SidebarMode mode) {
  return mode == SidebarMode::kExpanded;
}

constexpr bool IsVisible(SidebarMode mode) {
  return mode != SidebarMode::kHidden;
}

constexpr SidebarMode NextMode(SidebarMode mode) {
  switch (mode) {
    case SidebarMode::kExpanded:
      return SidebarMode::kHidden;
    case SidebarMode::kIcons:
      return SidebarMode::kExpanded;
    case SidebarMode::kHidden:
      return SidebarMode::kIcons;
  }
  return SidebarMode::kExpanded;
}

constexpr int SnapExpandedWidth(int width) {
  const int clamped = ClampWidth(width);
  const bool within_chromium_snap =
      clamped > kExpandedWidth - kResizeSnapDistance &&
      clamped < kExpandedWidth + kResizeSnapDistance;
  const bool within_helium_snap =
      clamped >= kExpandedWidth - kDefaultWidthSnapDistance &&
      clamped <= kExpandedWidth + kDefaultWidthSnapDistance;
  return within_chromium_snap || within_helium_snap
             ? kExpandedWidth
             : clamped;
}

struct ResizeTarget {
  SidebarMode mode = SidebarMode::kExpanded;
  int expanded_width = kExpandedWidth;
};

// Matches Helium's VerticalTabStripRegionView::OnResize state calculation.
// Resizing never hides the rail: proposed widths through the collapse boundary
// snap to the fixed icon strip, while larger widths jump to the 160 DIP
// expanded minimum and then track the pointer up to 400 DIPs.
constexpr ResizeTarget TargetForResize(int proposed_width,
                                       int current_expanded_width) {
  if (proposed_width <= kCollapseSnapWidth) {
    return {SidebarMode::kIcons, ClampWidth(current_expanded_width)};
  }
  return {SidebarMode::kExpanded, SnapExpandedWidth(proposed_width)};
}

}  // namespace cmux::sidebar_metrics

#endif  // CHROME_BROWSER_CMUX_TERM_CMUX_SIDEBAR_METRICS_H_
