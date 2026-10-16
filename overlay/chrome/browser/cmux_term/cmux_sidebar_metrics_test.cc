// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/cmux_term/cmux_sidebar_metrics.h"

#include <cstdlib>
#include <iostream>

namespace {

void Expect(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}

}  // namespace

int main() {
  namespace metrics = cmux::sidebar_metrics;
  Expect(metrics::InsetWidth(200, 6) == 188, "six DIP side insets");
  Expect(metrics::InsetWidth(8, 6) == 0, "inset width clamps at zero");
  Expect(metrics::FooterReservedHeight() == 38, "footer vertical footprint");
  Expect(metrics::ClampWidth(100) == 160, "width clamps low");
  Expect(metrics::ClampWidth(500) == 400, "width clamps high");
  Expect(metrics::kIconsWidth == 42, "icons rail matches Helium collapsed width");
  Expect(metrics::kIconsWidth ==
             metrics::kRowHeight + 2 * metrics::kOuterHorizontalInset,
         "icons rail is one control plus Helium side insets");
  Expect(metrics::kCollapseSnapWidth == 101,
         "collapse boundary matches Helium midpoint");
  Expect(metrics::kExpandedResizeAreaWidth == 5,
         "normal resize area matches Helium");
  Expect(metrics::kIconsResizeAreaWidth == 2,
         "icons resize area matches Helium");
  Expect(metrics::ClampIconsWidth(12) == 42, "icons width clamps low");
  Expect(metrics::ClampIconsWidth(500) == 42, "icons width clamps high");
  Expect(metrics::WidthForMode(metrics::SidebarMode::kExpanded, 220, 52) == 220,
         "normal mode uses expanded width");
  Expect(metrics::WidthForMode(metrics::SidebarMode::kIcons, 220, 52) == 42,
         "icons mode uses fixed Helium width");
  Expect(metrics::WidthForMode(metrics::SidebarMode::kHidden, 220, 52) == 0,
         "hidden mode has zero width");
  Expect(metrics::ControlXForMode(metrics::SidebarMode::kIcons, 42, 99) == 6,
         "icon target has Helium left inset");
  Expect(metrics::ControlWidthForMode(metrics::SidebarMode::kIcons, 42, 99) ==
             30,
         "icon target is 30 DIPs wide");
  Expect(metrics::RowHeightForMode(metrics::SidebarMode::kIcons, 47) == 30,
         "icon target is 30 DIPs tall");
  Expect(metrics::ControlWidthForMode(metrics::SidebarMode::kIcons, 42, 99) ==
             metrics::RowHeightForMode(metrics::SidebarMode::kIcons, 47),
         "icon target is square");
  Expect(metrics::RowGapForMode(metrics::SidebarMode::kIcons, 9) == 2,
         "icons use Helium row gap");
  Expect(metrics::TopPaddingForMode(metrics::SidebarMode::kIcons, 11) == 6,
         "icons use Helium top padding");
  Expect(metrics::ControlXForMode(metrics::SidebarMode::kExpanded, 200, 8) ==
             8,
         "normal target honors configured inset");
  Expect(metrics::ControlWidthForMode(metrics::SidebarMode::kExpanded, 200,
                                      8) == 184,
         "normal target fills configured inset");
  Expect(metrics::ControlWidthForMode(metrics::SidebarMode::kIcons, 200, 99) ==
             188,
         "normal-to-icons target shrinks continuously");
  Expect(metrics::ControlXForMode(metrics::SidebarMode::kHidden, 0, 99) == -15,
         "icons remain centered while clipping to hidden");
  Expect(metrics::NextMode(metrics::SidebarMode::kExpanded) ==
             metrics::SidebarMode::kHidden,
         "normal cycles to hidden");
  Expect(metrics::NextMode(metrics::SidebarMode::kIcons) ==
             metrics::SidebarMode::kExpanded,
         "icons cycles to normal");
  Expect(metrics::NextMode(metrics::SidebarMode::kHidden) ==
             metrics::SidebarMode::kIcons,
         "hidden cycles to icons");
  Expect(metrics::ShowsLabels(metrics::SidebarMode::kExpanded),
         "normal mode shows labels");
  Expect(!metrics::ShowsLabels(metrics::SidebarMode::kIcons),
         "icons mode hides labels");
  Expect(!metrics::ShowsLabels(metrics::SidebarMode::kHidden),
         "hidden mode hides labels");
  Expect(metrics::IsVisible(metrics::SidebarMode::kExpanded),
         "normal mode is visible");
  Expect(metrics::IsVisible(metrics::SidebarMode::kIcons),
         "icons mode is visible");
  Expect(!metrics::IsVisible(metrics::SidebarMode::kHidden),
         "hidden mode is not visible");
  Expect(metrics::SnapExpandedWidth(186) == 200,
         "inherited Helium snap lower boundary");
  Expect(metrics::SnapExpandedWidth(214) == 200,
         "inherited Helium snap upper boundary");
  Expect(metrics::SnapExpandedWidth(185) == 185,
         "below Helium snap remains unsnapped");
  Expect(metrics::SnapExpandedWidth(215) == 215,
         "above Helium snap remains unsnapped");

  const metrics::ResizeTarget below_collapse =
      metrics::TargetForResize(100, 220);
  Expect(below_collapse.mode == metrics::SidebarMode::kIcons,
         "drag below collapse boundary enters icons mode");
  Expect(below_collapse.expanded_width == 220,
         "collapsing preserves the stored normal width");
  const metrics::ResizeTarget at_collapse =
      metrics::TargetForResize(101, 220);
  Expect(at_collapse.mode == metrics::SidebarMode::kIcons,
         "collapse boundary is inclusive");
  const metrics::ResizeTarget above_collapse =
      metrics::TargetForResize(102, 220);
  Expect(above_collapse.mode == metrics::SidebarMode::kExpanded,
         "drag above collapse boundary enters normal mode");
  Expect(above_collapse.expanded_width == 160,
         "normal mode jumps to Helium minimum width");
  const metrics::ResizeTarget default_snap =
      metrics::TargetForResize(194, 220);
  Expect(default_snap.mode == metrics::SidebarMode::kExpanded &&
             default_snap.expanded_width == 200,
         "normal resize snaps to Helium default width");
  const metrics::ResizeTarget maximum =
      metrics::TargetForResize(500, 220);
  Expect(maximum.mode == metrics::SidebarMode::kExpanded &&
             maximum.expanded_width == 400,
         "normal resize clamps to Helium maximum width");

  Expect(!metrics::ShouldShowCloseButton(true, false, false, false, false, 80),
         "inactive idle rows do not show close");
  Expect(metrics::ShouldShowCloseButton(true, true, false, false, false, 56),
         "active row shows close at threshold");
  Expect(metrics::ShouldShowCloseButton(true, false, true, false, false, 80),
         "hovered row shows close");
  Expect(metrics::ShouldShowCloseButton(true, false, false, true, false, 80),
         "focused row shows close");
  Expect(!metrics::ShouldShowCloseButton(true, true, true, true, true, 80),
         "dragging suppresses close");
  Expect(!metrics::ShouldShowCloseButton(true, true, false, false, false, 55),
         "narrow rows suppress close");
  Expect(!metrics::ShouldShowCloseButton(false, true, true, true, false, 80),
         "config can disable close");

  std::cout << "cmux_sidebar_metrics_test: PASS\n";
  return 0;
}
