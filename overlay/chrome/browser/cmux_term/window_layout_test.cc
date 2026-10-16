// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Host test for the pure layout engine (no Chromium, no gtest). Build + run:
//
//   c++ -std=c++17 -I overlay \
//     overlay/chrome/browser/cmux_term/window_model.cc \
//     overlay/chrome/browser/cmux_term/window_layout.cc \
//     overlay/chrome/browser/cmux_term/window_layout_test.cc \
//     -o /tmp/layout_test && /tmp/layout_test

#include "chrome/browser/cmux_term/window_layout.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include "chrome/browser/cmux_term/window_model.h"

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

bool Near(double a, double b) {
  return std::fabs(a - b) < 1e-6;
}

bool NearRect(const LayoutRect& a, const LayoutRect& b) {
  return Near(a.x, b.x) && Near(a.y, b.y) && Near(a.width, b.width) &&
         Near(a.height, b.height);
}

LayoutRect InsetPane(const LayoutRect& pane) {
  constexpr double kPlaceholderPadding = 4.0;
  return LayoutRect{
      pane.x + kPlaceholderPadding, pane.y + kPlaceholderPadding,
      std::max(0.0, pane.width - kPlaceholderPadding * 2.0),
      std::max(0.0, pane.height - kPlaceholderPadding * 2.0)};
}

LayoutRect PaneEdgePreview(const LayoutRect& pane,
                           SplitOrientation orientation,
                           bool insert_first) {
  constexpr double kPlaceholderPadding = 4.0;
  if (orientation == SplitOrientation::kHorizontal) {
    const double half_width = pane.width * 0.5;
    return LayoutRect{
        insert_first ? pane.x + kPlaceholderPadding : pane.x + half_width,
        pane.y + kPlaceholderPadding,
        std::max(0.0, half_width - kPlaceholderPadding),
        std::max(0.0, pane.height - kPlaceholderPadding * 2.0)};
  }
  const double half_height = pane.height * 0.5;
  return LayoutRect{
      pane.x + kPlaceholderPadding,
      insert_first ? pane.y + kPlaceholderPadding : pane.y + half_height,
      std::max(0.0, pane.width - kPlaceholderPadding * 2.0),
      std::max(0.0, half_height - kPlaceholderPadding)};
}

const PaneBox* FindBox(const StripLayout& layout, PaneId pane) {
  for (const PaneBox& box : layout.panes) {
    if (box.pane == pane) {
      return &box;
    }
  }
  return nullptr;
}

const Pane* FindPaneInNode(const LayoutNode& node, PaneId pane) {
  if (node.is_pane()) {
    return node.pane->id == pane ? node.pane.get() : nullptr;
  }
  if (const Pane* p = FindPaneInNode(node.split->first, pane)) {
    return p;
  }
  return FindPaneInNode(node.split->second, pane);
}

const Pane* FindPane(const Workspace& ws, PaneId pane) {
  for (const LayoutNode& column : ws.columns) {
    if (const Pane* p = FindPaneInNode(column, pane)) {
      return p;
    }
  }
  return nullptr;
}

DragPayload TabPayload(const Workspace& ws, PaneId pane) {
  const Pane* p = FindPane(ws, pane);
  DragPayload payload;
  payload.source_pane = pane;
  payload.source_tab_count = p ? static_cast<int>(p->tabs.size()) : 0;
  payload.whole_pane = false;
  payload.tab = p && !p->tabs.empty() ? p->tabs.front().id : kInvalidId;
  return payload;
}

DragPayload PanePayload(const Workspace& ws, PaneId pane) {
  DragPayload payload = TabPayload(ws, pane);
  payload.whole_pane = true;
  payload.tab = kInvalidId;
  return payload;
}

void UseDefaultFirstColumn(WindowModel* m, WorkspaceId ws) {
  Workspace* w = m->GetWorkspace(ws);
  if (w && !w->column_widths.empty()) {
    w->column_widths[0] = 0.0;
  }
}

// Build a workspace of n single-pane columns whose in-order panes are exactly
// [p0, p1, ... p(n-1)] (the first column comes with the workspace; AddColumn
// appends the rest). Returns the pane ids.
std::vector<PaneId> MakeNColumnStrip(WindowModel* m, WorkspaceId ws, int n) {
  std::vector<PaneId> ids;
  UseDefaultFirstColumn(m, ws);
  ids.push_back(m->GetWorkspace(ws)->columns[0].pane->id);
  for (int i = 1; i < n; ++i) {
    ids.push_back(m->AddColumn(ws, SurfaceKind::kWeb));
  }
  return ids;
}

// The strip's focus scroll-into-view. The geometry derives from NiriModel but
// deliberately keeps `gap` of breathing room on BOTH edges of the focused
// column (the pane render x includes a leading gap, so a bare NiriModel clamp
// would leave the focused column resting a gap short / slightly clipped).
// Scroll is STATEFUL/path-dependent: the renderer persists the clamped
// scroll_x and feeds it back, so this test threads it the same way.
void TestStripFocusScroll() {
  const double vw = 1600, vh = 1000;
  const int n = 6;
  const LayoutMetrics m;

  WindowModel model;
  WorkspaceId ws = model.AddWorkspace(SurfaceKind::kWeb, "w");
  std::vector<PaneId> ids = MakeNColumnStrip(&model, ws, n);

  double scroll = 0;
  auto layout_at = [&](int focus) {
    StripLayout sl =
        ComputeStripLayout(*model.GetWorkspace(ws), vw, vh, scroll, ids[focus]);
    scroll = sl.scroll_x;  // persist, like the renderer does
    return sl;
  };

  // A path that exercises scroll left, right, and jumps.
  for (int focus : {5, 0, 3, 5, 2, 4, 1, 0, 5}) {
    StripLayout sl = layout_at(focus);
    Check(sl.panes.size() == static_cast<size_t>(n), "pane count matches n");
    Check(sl.dividers.empty(), "single-pane columns have no dividers");
    Check(sl.scroll_x >= 0, "scroll never negative");

    // The focused column sits fully inside the viewport with `gap` breathing
    // room on each edge.
    const LayoutRect& r = sl.panes[focus].rect;
    Check(r.x >= m.gap - 1e-6, "focused column clear of the left edge");
    Check(r.x + r.width <= vw - m.gap + 1e-6,
          "focused column clear of the right edge");

    // Every column follows the strip formula at the clamped scroll.
    bool frames_match = true;
    for (int i = 0; i < n; ++i) {
      const double x = static_cast<double>(i) * (sl.column_width + m.gap) -
                       sl.scroll_x + m.gap;
      const PaneBox& b = sl.panes[i];
      if (b.pane != ids[i] || !Near(b.rect.x, x) || !Near(b.rect.y, m.margin) ||
          !Near(b.rect.width, sl.column_width) ||
          !Near(b.rect.height, vh - 2 * m.margin)) {
        frames_match = false;
      }
    }
    Check(frames_match, "every column frame follows the strip formula");
  }

  // Path dependence: re-focusing the already-visible focused column must not
  // move the scroll.
  const double settled = layout_at(5).scroll_x;
  Check(Near(layout_at(5).scroll_x, settled),
        "re-focusing a visible column keeps the scroll");
}

void TestStripSingle() {
  WindowModel model;
  WorkspaceId ws = model.AddWorkspace(SurfaceKind::kWeb, "w");
  UseDefaultFirstColumn(&model, ws);
  PaneId p = model.GetWorkspace(ws)->columns[0].pane->id;
  StripLayout sl =
      ComputeStripLayout(*model.GetWorkspace(ws), 1200, 800, 0.0, p);
  Check(sl.panes.size() == 1, "one pane");
  Check(sl.panes[0].pane == p, "the pane id");
  // gap=10, margin=8, col_w=max(360,1200*0.62)=744, x = 0 - 0 + gap = 10.
  Check(Near(sl.panes[0].rect.x, 10), "x == gap");
  Check(Near(sl.panes[0].rect.y, 8), "y == margin");
  Check(Near(sl.panes[0].rect.width, 744), "width == 62% viewport");
  Check(Near(sl.panes[0].rect.height, 800 - 16),
        "height == viewport - 2*margin");
  Check(Near(sl.scroll_x, 0), "no scroll for single column");
}

void TestStripScrollClampsNonNegative() {
  WindowModel model;
  WorkspaceId ws = model.AddWorkspace(SurfaceKind::kWeb, "w");
  PaneId p = model.GetWorkspace(ws)->columns[0].pane->id;
  // Request a negative scroll; engine must clamp to 0.
  StripLayout sl =
      ComputeStripLayout(*model.GetWorkspace(ws), 1200, 800, -50.0, p);
  Check(Near(sl.scroll_x, 0), "negative scroll clamps to 0");
}

void TestScrollIntoViewPolicies() {
  const double vw = 1000, vh = 700;
  const LayoutMetrics m;
  WindowModel model;
  WorkspaceId ws = model.AddWorkspace(SurfaceKind::kWeb, "w");
  std::vector<PaneId> ids = MakeNColumnStrip(&model, ws, 3);

  // Column 1 at scroll=640 rests exactly on the left edge. Click/native
  // activation should not add the historical gap margin.
  StripLayout click =
      ComputeStripLayout(*model.GetWorkspace(ws), vw, vh, 640.0, ids[1],
                         ScrollIntoViewPolicy::kEnsureVisible, m);
  Check(Near(click.scroll_x, 640.0),
        "marginless policy keeps a fully visible edge-resting column still");
  Check(Near(click.panes[1].rect.x, 0.0),
        "marginless fully-visible column remains edge-resting");

  // The keyboard/new-pane policy keeps the old gap breathing room.
  StripLayout keyboard =
      ComputeStripLayout(*model.GetWorkspace(ws), vw, vh, 640.0, ids[1],
                         ScrollIntoViewPolicy::kEnsureVisibleWithMargin, m);
  Check(Near(keyboard.scroll_x, 630.0),
        "margin policy preserves the old left-edge gap");

  // Clipped by 20px on the right -> scroll exactly 20px more.
  StripLayout clipped_right =
      ComputeStripLayout(*model.GetWorkspace(ws), vw, vh, 240.0, ids[1],
                         ScrollIntoViewPolicy::kEnsureVisible, m);
  Check(Near(clipped_right.scroll_x, 260.0),
        "marginless policy reveals exactly the right clipped pixels");

  // Clipped by 10px on the left -> scroll exactly 10px less.
  StripLayout clipped_left =
      ComputeStripLayout(*model.GetWorkspace(ws), vw, vh, 650.0, ids[1],
                         ScrollIntoViewPolicy::kEnsureVisible, m);
  Check(Near(clipped_left.scroll_x, 640.0),
        "marginless policy reveals exactly the left clipped pixels");
}

void TestZeroGapMarginStripLayout() {
  const double vw = 1000, vh = 600;
  LayoutMetrics m;
  m.gap = 0;
  m.margin = 0;
  m.min_column_width = 200;
  m.column_fraction = 0.5;

  WindowModel model;
  WorkspaceId ws = model.AddWorkspace(SurfaceKind::kWeb, "w");
  UseDefaultFirstColumn(&model, ws);
  PaneId a = model.GetWorkspace(ws)->columns[0].pane->id;
  PaneId b = model.SplitPane(ws, a, SplitOrientation::kHorizontal, 0.4);
  PaneId c = model.AddColumn(ws, SurfaceKind::kWeb);
  const Workspace& w = *model.GetWorkspace(ws);

  StripLayout sl = ComputeStripLayout(w, vw, vh, 0.0, a, m);
  const LayoutRect ar = FindBox(sl, a)->rect;
  const LayoutRect br = FindBox(sl, b)->rect;
  const LayoutRect cr = FindBox(sl, c)->rect;
  Check(Near(ar.x, 0.0) && Near(ar.y, 0.0), "zero strip starts flush");
  Check(Near(ar.width + br.width, sl.column_width),
        "zero-gap split panes exactly fill column width");
  Check(Near(br.x, ar.x + ar.width), "zero-gap split panes touch horizontally");
  Check(sl.dividers.size() == 1, "zero-gap split still emits divider box");
  Check(Near(sl.dividers[0].rect.width, 0.0),
        "zero-gap horizontal divider has zero width");
  Check(Near(cr.x, sl.column_width),
        "zero-gap second column touches first column");
  Check(Near(sl.content_width, 2 * sl.column_width),
        "zero-gap content width sums only column widths");
}

void TestZeroGapMarginTiledLayout() {
  const double vw = 1000, vh = 600;
  LayoutMetrics m;
  m.gap = 0;
  m.margin = 0;

  WindowModel model;
  WorkspaceId ws = model.AddWorkspace(SurfaceKind::kWeb, "w");
  PaneId a = model.GetWorkspace(ws)->columns[0].pane->id;
  PaneId b = model.SplitPane(ws, a, SplitOrientation::kVertical, 0.5,
                             SurfaceKind::kWeb);

  TiledLayout tl =
      ComputeTiledLayout(model.GetWorkspace(ws)->columns[0], vw, vh, m);
  const PaneBox* top = nullptr;
  const PaneBox* bottom = nullptr;
  for (const PaneBox& pb : tl.panes) {
    if (pb.pane == a) {
      top = &pb;
    }
    if (pb.pane == b) {
      bottom = &pb;
    }
  }
  Check(top && bottom, "zero tiled split panes present");
  Check(top && Near(top->rect.x, 0.0) && Near(top->rect.y, 0.0),
        "zero tiled first pane starts at origin");
  Check(top && bottom && Near(top->rect.height + bottom->rect.height, vh),
        "zero tiled heights exactly fill viewport");
  Check(top && bottom && Near(bottom->rect.y, top->rect.height),
        "zero tiled stacked panes touch vertically");
  Check(tl.dividers.size() == 1, "zero tiled split emits divider");
  Check(Near(tl.dividers[0].rect.height, 0.0),
        "zero-gap vertical divider has zero height");
}

void TestZeroGapFocusedScrollMath() {
  const double vw = 1000, vh = 600;
  LayoutMetrics m;
  m.gap = 0;
  m.margin = 0;
  m.min_column_width = 200;
  m.column_fraction = 0.62;

  WindowModel model;
  WorkspaceId ws = model.AddWorkspace(SurfaceKind::kWeb, "w");
  std::vector<PaneId> ids = MakeNColumnStrip(&model, ws, 3);
  StripLayout sl =
      ComputeStripLayout(*model.GetWorkspace(ws), vw, vh, 0.0, ids[1],
                         ScrollIntoViewPolicy::kEnsureVisibleWithMargin, m);
  const LayoutRect& focused = sl.panes[1].rect;
  Check(Near(sl.scroll_x, 240.0),
        "zero-gap margin policy scrolls only clipped pixels");
  Check(Near(focused.x + focused.width, vw),
        "zero-gap focused column lands flush to right edge");
  Check(Near(focused.y, 0.0) && Near(focused.height, vh),
        "zero-margin focused column fills viewport height");
}

// In-column splits: columns keep today's fixed-width x positions; each
// column's tree tiles its strip; dividers appear only within columns.
void TestStripInColumnSplit() {
  const double vw = 1600, vh = 1000;
  const LayoutMetrics m;
  WindowModel model;
  WorkspaceId ws = model.AddWorkspace(SurfaceKind::kWeb, "w");
  UseDefaultFirstColumn(&model, ws);
  PaneId p0 = model.GetWorkspace(ws)->columns[0].pane->id;
  PaneId p1 = model.AddColumn(ws, SurfaceKind::kWeb);
  PaneId p2 = model.SplitPane(ws, p1, SplitOrientation::kVertical, 0.4);
  PaneId p3 = model.AddColumn(ws, SurfaceKind::kWeb);
  // Columns: [p0] [p1 over p2] [p3].

  StripLayout sl =
      ComputeStripLayout(*model.GetWorkspace(ws), vw, vh, 0.0, p0, m);
  Check(sl.panes.size() == 4, "four pane boxes");
  Check(sl.panes[0].pane == p0 && sl.panes[1].pane == p1 &&
            sl.panes[2].pane == p2 && sl.panes[3].pane == p3,
        "panes emit column-by-column, in-order within a column");

  // Pane order matches PanesOf.
  std::vector<PaneId> order = model.PanesOf(ws);
  bool same_order = order.size() == sl.panes.size();
  for (size_t i = 0; same_order && i < order.size(); ++i) {
    same_order = order[i] == sl.panes[i].pane;
  }
  Check(same_order, "strip pane order matches PanesOf");

  // Column x positions follow today's math, keyed by COLUMN index (an
  // in-column split does not shift later columns).
  const double col_w = sl.column_width;
  auto col_x = [&](int i) { return i * (col_w + m.gap) - sl.scroll_x + m.gap; };
  const LayoutRect& r0 = sl.panes[0].rect;  // p0, column 0
  const LayoutRect& r1 = sl.panes[1].rect;  // p1, column 1 top
  const LayoutRect& r2 = sl.panes[2].rect;  // p2, column 1 bottom
  const LayoutRect& r3 = sl.panes[3].rect;  // p3, column 2
  Check(Near(r0.x, col_x(0)) && Near(r1.x, col_x(1)) && Near(r2.x, col_x(1)) &&
            Near(r3.x, col_x(2)),
        "column x positions unchanged by in-column splits");
  Check(Near(r1.width, col_w) && Near(r2.width, col_w),
        "stacked panes keep the column width");

  // The stacked pair tiles the column height: ratio honored, rects + gap sum
  // to the full column height.
  const double col_h = vh - 2 * m.margin;
  Check(Near(r0.height, col_h), "lone-pane column fills the column height");
  Check(Near(r1.height + m.gap + r2.height, col_h),
        "stacked heights + gap fill the column");
  Check(Near(r1.height, (col_h - m.gap) * 0.4),
        "ratio splits the column height");
  Check(Near(r1.y, m.margin), "top pane at the strip margin");
  Check(Near(r2.y, m.margin + r1.height + m.gap),
        "bottom pane after the divider gap");

  // Exactly one divider, inside column 1, between the stacked panes.
  Check(sl.dividers.size() == 1, "one in-column divider");
  Check(Near(sl.dividers[0].rect.x, col_x(1)) &&
            Near(sl.dividers[0].rect.width, col_w),
        "divider spans its column");
  Check(Near(sl.dividers[0].rect.y, m.margin + r1.height) &&
            Near(sl.dividers[0].rect.height, m.gap),
        "divider sits between the stacked panes");

  // Content width counts columns, not panes.
  Check(Near(sl.content_width, 3 * (col_w + m.gap) + m.gap),
        "content width counts columns");

  // Focusing a DEEP pane keys the scroll by its column: column 1 must come
  // fully into view with `gap` of breathing room.
  StripLayout sf =
      ComputeStripLayout(*model.GetWorkspace(ws), vw, vh, 0.0, p2, m);
  const double left = 1 * (col_w + m.gap) + m.gap;
  const double right = left + col_w;
  Check(sf.scroll_x >= 0, "scroll non-negative");
  Check(right - sf.scroll_x <= vw - m.gap + 1e-6 &&
            left - sf.scroll_x >= m.gap - 1e-6,
        "focused pane's column scrolled into view");
}

void TestStripMixedColumnWidths() {
  const double vw = 1200, vh = 800;
  const LayoutMetrics m;
  WindowModel model;
  WorkspaceId ws = model.AddWorkspace(SurfaceKind::kWeb, "w");
  std::vector<PaneId> ids = MakeNColumnStrip(&model, ws, 3);
  Workspace* w = model.GetWorkspace(ws);
  w->column_widths = {1.0 / 3.0, 2.0 / 3.0, 0.5};

  StripLayout sl = ComputeStripLayout(*w, vw, vh, 0.0, kInvalidId, m);
  Check(Near(sl.column_width, 744.0),
        "default column width remains the homogeneous fallback");
  Check(sl.column_widths.size() == 3, "actual column widths are reported");
  Check(Near(sl.column_widths[0], 400.0) && Near(sl.column_widths[1], 800.0) &&
            Near(sl.column_widths[2], 600.0),
        "width fractions become heterogeneous column widths");
  Check(Near(sl.panes[0].rect.x, 10.0), "mixed column 0 x");
  Check(Near(sl.panes[1].rect.x, 420.0), "mixed column 1 x sums prior width");
  Check(Near(sl.panes[2].rect.x, 1230.0),
        "mixed column 2 x sums all prior widths");
  Check(Near(sl.panes[0].rect.width, 400.0) &&
            Near(sl.panes[1].rect.width, 800.0) &&
            Near(sl.panes[2].rect.width, 600.0),
        "pane widths match their columns");
  Check(Near(sl.content_width, 1840.0),
        "mixed content width sums column widths and gaps");

  StripLayout focused =
      ComputeStripLayout(*w, vw, vh, 0.0, ids[2],
                         ScrollIntoViewPolicy::kEnsureVisibleWithMargin, m);
  Check(Near(focused.scroll_x, 640.0),
        "mixed-width scroll into view uses summed column edges");
  Check(Near(focused.panes[2].rect.x, 590.0) &&
            Near(focused.panes[2].rect.x + focused.panes[2].rect.width,
                 vw - m.gap),
        "mixed-width focused column lands with the margin policy");
}

void TestTiledColumnModeProportions() {
  const double vw = 1200, vh = 800;
  const LayoutMetrics m;
  WindowModel model;
  WorkspaceId ws = model.AddWorkspace(SurfaceKind::kWeb, "w");
  std::vector<PaneId> ids = MakeNColumnStrip(&model, ws, 3);
  Workspace* w = model.GetWorkspace(ws);
  w->layout_mode = WorkspaceLayoutMode::kTiled;
  w->column_widths = {1.0 / 3.0, 2.0 / 3.0, 0.5};

  StripLayout sl = ComputeStripLayout(*w, vw, vh, 500.0, ids[2], m);
  const double available = vw - 4 * m.gap;
  const double total_weight = 1.0 / 3.0 + 2.0 / 3.0 + 0.5;
  Check(Near(sl.scroll_x, 0.0), "tiled mode clamps scroll to zero");
  Check(sl.column_widths.size() == 3, "tiled reports all column widths");
  Check(Near(sl.column_widths[0], available * (1.0 / 3.0) / total_weight) &&
            Near(sl.column_widths[1],
                 available * (2.0 / 3.0) / total_weight) &&
            Near(sl.column_widths[2], available * 0.5 / total_weight),
        "tiled column widths preserve configured ratios");
  Check(Near(sl.column_widths[0] + sl.column_widths[1] +
                 sl.column_widths[2] + 4 * m.gap,
             vw),
        "tiled columns and gaps fill the viewport width");
  Check(sl.panes[0].pane == ids[0] && sl.panes[1].pane == ids[1] &&
            sl.panes[2].pane == ids[2],
        "tiled column order is preserved");
  Check(Near(sl.panes[0].rect.x, m.gap), "tiled first column starts at gap");
  Check(Near(sl.panes[1].rect.x,
             m.gap + sl.column_widths[0] + m.gap),
        "tiled second column follows first width and gap");
  Check(Near(sl.panes[2].rect.x,
             m.gap + sl.column_widths[0] + m.gap + sl.column_widths[1] +
                 m.gap),
        "tiled third column follows summed prior widths");
  Check(Near(sl.panes[0].rect.y, m.margin) &&
            Near(sl.panes[0].rect.height, vh - 2 * m.margin),
        "tiled columns preserve vertical strip margins");
}

void TestTiledSingleColumnFillsAvailableWidth() {
  const double vw = 1000, vh = 600;
  const LayoutMetrics m;
  WindowModel model;
  WorkspaceId ws = model.AddWorkspace(SurfaceKind::kWeb, "w");
  PaneId p = model.GetWorkspace(ws)->columns[0].pane->id;
  model.SetWorkspaceLayoutMode(ws, WorkspaceLayoutMode::kTiled);

  StripLayout sl =
      ComputeStripLayout(*model.GetWorkspace(ws), vw, vh, 250.0, p, m);
  Check(sl.panes.size() == 1, "single tiled column has one pane");
  Check(Near(sl.scroll_x, 0.0), "single tiled column ignores requested scroll");
  Check(Near(sl.panes[0].rect.x, m.gap), "single tiled column keeps edge gap");
  Check(Near(sl.panes[0].rect.width, vw - 2 * m.gap),
        "single tiled column fills available width");
}

void TestTiledRoundTripPreservesStripWidths() {
  const double vw = 1200, vh = 800;
  const LayoutMetrics m;
  WindowModel model;
  WorkspaceId ws = model.AddWorkspace(SurfaceKind::kWeb, "w");
  MakeNColumnStrip(&model, ws, 3);
  Workspace* w = model.GetWorkspace(ws);
  w->column_widths = {1.0 / 3.0, 2.0 / 3.0, 0.5};

  StripLayout before = ComputeStripLayout(*w, vw, vh, 0.0, kInvalidId, m);
  model.ToggleWorkspaceLayoutMode(ws);
  StripLayout tiled = ComputeStripLayout(*w, vw, vh, 1000.0, kInvalidId, m);
  model.ToggleWorkspaceLayoutMode(ws);
  StripLayout after = ComputeStripLayout(*w, vw, vh, 0.0, kInvalidId, m);

  Check(Near(tiled.scroll_x, 0.0), "round-trip tiled pass clamps scroll");
  Check(w->column_widths.size() == 3 &&
            Near(w->column_widths[0], 1.0 / 3.0) &&
            Near(w->column_widths[1], 2.0 / 3.0) &&
            Near(w->column_widths[2], 0.5),
        "stored strip width fractions survive tiled round-trip");
  Check(Near(before.panes[0].rect.width, after.panes[0].rect.width) &&
            Near(before.panes[1].rect.width, after.panes[1].rect.width) &&
            Near(before.panes[2].rect.width, after.panes[2].rect.width),
        "strip rendered widths match after toggling back");
}

void TestTiledDegenerateViewport() {
  LayoutMetrics m;
  WindowModel model;
  WorkspaceId ws = model.AddWorkspace(SurfaceKind::kWeb, "w");
  MakeNColumnStrip(&model, ws, 2);
  model.SetWorkspaceLayoutMode(ws, WorkspaceLayoutMode::kTiled);

  StripLayout sl =
      ComputeStripLayout(*model.GetWorkspace(ws), 15.0, 100.0, 40.0,
                         kInvalidId, m);
  Check(Near(sl.scroll_x, 0.0), "degenerate tiled scroll remains zero");
  Check(sl.column_widths.size() == 2 && Near(sl.column_widths[0], 0.0) &&
            Near(sl.column_widths[1], 0.0),
        "degenerate tiled viewport collapses column widths to zero");
  Check(sl.panes.size() == 2 && Near(sl.panes[0].rect.x, m.gap) &&
            Near(sl.panes[1].rect.x, 2 * m.gap),
        "degenerate tiled panes still emit in order at gap slots");
}

void TestTiledDropTargetsUseTiledColumnWidths() {
  const double vw = 1200, vh = 800, tab_h = 30;
  const LayoutMetrics m;
  WindowModel model;
  WorkspaceId ws = model.AddWorkspace(SurfaceKind::kWeb, "w");
  std::vector<PaneId> ids = MakeNColumnStrip(&model, ws, 3);
  Workspace* w = model.GetWorkspace(ws);
  w->layout_mode = WorkspaceLayoutMode::kTiled;
  w->column_widths = {1.0 / 3.0, 2.0 / 3.0, 0.5};

  StripLayout layout = ComputeStripLayout(*w, vw, vh, 0.0, kInvalidId, m);
  const LayoutRect c0 = FindBox(layout, ids[0])->rect;
  const LayoutRect c1 = FindBox(layout, ids[1])->rect;
  DropTarget t =
      ResolveDropTarget(*w, layout, TabPayload(*w, ids[2]),
                        (c0.x + c0.width + c1.x) / 2.0, c0.y + c0.height / 2.0,
                        tab_h);
  Check(t.kind == DropKind::kNewColumn && t.column_index == 1,
        "tiled drop target uses actual width for column gap");
}

// Tiled policy: a single H-split divides the content width by ratio with a gap.
void TestTiledSplit() {
  WindowModel model;
  WorkspaceId ws = model.AddWorkspace(SurfaceKind::kWeb, "w");
  PaneId p0 = model.GetWorkspace(ws)->columns[0].pane->id;
  PaneId p1 = model.SplitPane(ws, p0, SplitOrientation::kHorizontal, 0.4,
                              SurfaceKind::kWeb);

  const double vw = 1000, vh = 600;  // margin=8 -> box 984 x 584, gap=10
  TiledLayout tl =
      ComputeTiledLayout(model.GetWorkspace(ws)->columns[0], vw, vh);
  Check(tl.panes.size() == 2, "two panes tiled");
  Check(tl.dividers.size() == 1, "one divider");

  // avail = 984 - 10 = 974; w1 = 974*0.4 = 389.6; w2 = 584.4.
  const PaneBox* a = nullptr;
  const PaneBox* b = nullptr;
  for (const PaneBox& pb : tl.panes) {
    if (pb.pane == p0) {
      a = &pb;
    }
    if (pb.pane == p1) {
      b = &pb;
    }
  }
  Check(a && b, "both panes present");
  Check(a && Near(a->rect.x, 8), "first pane starts at margin");
  Check(a && Near(a->rect.width, 974 * 0.4), "first pane gets ratio width");
  Check(b && Near(b->rect.x, 8 + 974 * 0.4 + 10), "second pane after gap");
  Check(b && Near(b->rect.width, 974 - 974 * 0.4),
        "second pane gets remainder");
  Check(a && b && Near(a->rect.height, b->rect.height),
        "H-split: equal heights");
  Check(Near(tl.dividers[0].rect.width, 10), "divider width == gap");
  Check(Near(tl.dividers[0].ratio, 0.4),
        "divider exposes its current drag ratio");
  Check(Near(tl.dividers[0].resize_span, 974),
        "divider exposes its horizontal drag span");
}

void TestTiledVerticalSplit() {
  WindowModel model;
  WorkspaceId ws = model.AddWorkspace(SurfaceKind::kWeb, "w");
  PaneId p0 = model.GetWorkspace(ws)->columns[0].pane->id;
  model.SplitPane(ws, p0, SplitOrientation::kVertical, 0.5, SurfaceKind::kWeb);
  TiledLayout tl =
      ComputeTiledLayout(model.GetWorkspace(ws)->columns[0], 1000, 600);
  Check(tl.panes.size() == 2, "two panes");
  // Vertical split: same x/width, stacked y.
  Check(Near(tl.panes[0].rect.x, tl.panes[1].rect.x), "V-split: same x");
  Check(Near(tl.panes[0].rect.width, tl.panes[1].rect.width),
        "V-split: same width");
  Check(tl.panes[1].rect.y > tl.panes[0].rect.y, "second pane stacked below");
  Check(tl.dividers[0].orientation == SplitOrientation::kVertical,
        "divider is vertical orientation");
  Check(Near(tl.dividers[0].resize_span, 574),
        "divider exposes its vertical drag span");
}

void TestSpatialFocusMatrix() {
  WindowModel model;
  WorkspaceId ws = model.AddWorkspace(SurfaceKind::kWeb, "w");
  UseDefaultFirstColumn(&model, ws);
  PaneId a = model.GetWorkspace(ws)->columns[0].pane->id;
  PaneId b = model.SplitPane(ws, a, SplitOrientation::kHorizontal, 0.5);
  PaneId c = model.SplitPane(ws, b, SplitOrientation::kVertical, 0.5);
  PaneId d = model.AddColumn(ws, SurfaceKind::kWeb);
  const Workspace& w = *model.GetWorkspace(ws);
  // Column0 = A | (B / C), Column1 = D.

  Check(SpatialNeighborPane(w, a, Direction::kRight) == b,
        "A right prefers in-column B, not D or C");
  Check(SpatialNeighborPane(w, b, Direction::kLeft) == a, "B left is A");
  Check(SpatialNeighborPane(w, c, Direction::kLeft) == a, "C left is A");
  Check(SpatialNeighborPane(w, b, Direction::kDown) == c, "B down is C");
  Check(SpatialNeighborPane(w, b, Direction::kUp) == kInvalidId,
        "B has no up neighbor");
  Check(SpatialNeighborPane(w, c, Direction::kUp) == b, "C up is B");
  Check(SpatialNeighborPane(w, c, Direction::kDown) == kInvalidId,
        "C has no down neighbor");
  Check(SpatialNeighborPane(w, a, Direction::kUp) == kInvalidId &&
            SpatialNeighborPane(w, a, Direction::kDown) == kInvalidId,
        "A has no vertical neighbor");
  Check(SpatialNeighborPane(w, b, Direction::kRight) == d, "B right is D");
  Check(SpatialNeighborPane(w, c, Direction::kRight) == d, "C right is D");
  Check(SpatialNeighborPane(w, d, Direction::kLeft) == b,
        "D left tie-breaks to topmost B");
}

void TestSpatialFocusSymmetricNestedShape() {
  WindowModel model;
  WorkspaceId ws = model.AddWorkspace(SurfaceKind::kWeb, "w");
  PaneId a = model.GetWorkspace(ws)->columns[0].pane->id;
  PaneId b = model.SplitPane(ws, a, SplitOrientation::kVertical, 0.5);
  PaneId c = model.SplitPane(ws, b, SplitOrientation::kHorizontal, 0.5);
  const Workspace& w = *model.GetWorkspace(ws);
  // Column0 = A / (B | C).

  Check(SpatialNeighborPane(w, a, Direction::kDown) == b,
        "stacked A down tie-breaks to leftmost B");
  Check(SpatialNeighborPane(w, b, Direction::kUp) == a, "B up is A");
  Check(SpatialNeighborPane(w, c, Direction::kUp) == a, "C up is A");
  Check(SpatialNeighborPane(w, b, Direction::kRight) == c, "B right is C");
  Check(SpatialNeighborPane(w, c, Direction::kLeft) == b, "C left is B");
  Check(SpatialNeighborPane(w, a, Direction::kRight) == kInvalidId,
        "A does not skip diagonally right");
}

void TestSpatialFocusSinglePaneColumnsRegression() {
  WindowModel model;
  WorkspaceId ws = model.AddWorkspace(SurfaceKind::kWeb, "w");
  std::vector<PaneId> panes = MakeNColumnStrip(&model, ws, 3);
  const Workspace& w = *model.GetWorkspace(ws);

  Check(SpatialNeighborPane(w, panes[0], Direction::kRight) == panes[1],
        "column 0 right is column 1");
  Check(SpatialNeighborPane(w, panes[1], Direction::kLeft) == panes[0],
        "column 1 left is column 0");
  Check(SpatialNeighborPane(w, panes[1], Direction::kRight) == panes[2],
        "column 1 right is column 2");
  Check(SpatialNeighborPane(w, panes[2], Direction::kLeft) == panes[1],
        "column 2 left is column 1");
  Check(SpatialNeighborPane(w, panes[0], Direction::kUp) == kInvalidId &&
            SpatialNeighborPane(w, panes[0], Direction::kDown) == kInvalidId,
        "single-pane columns have no vertical neighbors");
}

void TestResolveDropTargetZones() {
  const double vw = 1000, vh = 800, tab_h = 30;
  WindowModel model;
  WorkspaceId ws = model.AddWorkspace(SurfaceKind::kWeb, "w");
  UseDefaultFirstColumn(&model, ws);
  PaneId a = model.GetWorkspace(ws)->columns[0].pane->id;
  PaneId b = model.SplitPane(ws, a, SplitOrientation::kVertical, 0.5);
  PaneId c = model.AddColumn(ws, SurfaceKind::kWeb);
  const Workspace& w = *model.GetWorkspace(ws);
  StripLayout layout = ComputeStripLayout(w, vw, vh, 0.0, kInvalidId);
  const LayoutRect ar = FindBox(layout, a)->rect;
  const LayoutRect content{ar.x, ar.y + tab_h, ar.width, ar.height - tab_h};
  const LayoutRect center_preview = InsetPane(ar);
  DragPayload payload = TabPayload(w, b);

  DropTarget t = ResolveDropTarget(w, layout, payload, ar.x + ar.width / 2,
                                   ar.y + 10, tab_h);
  Check(t.kind == DropKind::kTabStrip && t.pane == a,
        "pane top band resolves to tab strip");
  Check(Near(t.highlight.y, ar.y) && Near(t.highlight.height, tab_h),
        "tab-strip highlight is the strip band");

  t = ResolveDropTarget(w, layout, payload, ar.x + ar.width / 2,
                        ar.y + ar.height / 2, tab_h);
  Check(t.kind == DropKind::kPaneCenter && t.pane == a,
        "middle of pane resolves to center");
  Check(NearRect(t.highlight, center_preview),
        "center preview is inset four points inside the whole pane");
  Check(NearRect(t.hidden_highlight, center_preview),
        "hidden placeholder rests at the full-pane preview");
  Check(t.highlight.y < content.y,
        "center preview is drawn over the target tab strip");

  t = ResolveDropTarget(w, layout, payload, ar.x + 20, ar.y + ar.height / 2,
                        tab_h);
  Check(t.kind == DropKind::kPaneEdge &&
            t.orientation == SplitOrientation::kHorizontal && t.insert_first,
        "left edge resolves to horizontal insert-first split");
  Check(NearRect(t.highlight,
                 PaneEdgePreview(ar, SplitOrientation::kHorizontal,
                                 /*insert_first=*/true)),
        "left preview is Bonsplit's current-pane half");
  Check(t.highlight.y < content.y,
        "left preview is drawn over the target tab strip");

  t = ResolveDropTarget(w, layout, payload, ar.x + ar.width - 20,
                        ar.y + ar.height / 2, tab_h);
  Check(t.kind == DropKind::kPaneEdge &&
            t.orientation == SplitOrientation::kHorizontal && !t.insert_first,
        "right edge resolves to horizontal insert-second split");
  Check(NearRect(t.highlight,
                 PaneEdgePreview(ar, SplitOrientation::kHorizontal,
                                 /*insert_first=*/false)),
        "right preview is Bonsplit's current-pane half");

  t = ResolveDropTarget(w, layout, payload, ar.x + ar.width / 2,
                        ar.y + tab_h + 10, tab_h);
  Check(t.kind == DropKind::kPaneEdge &&
            t.orientation == SplitOrientation::kVertical && t.insert_first,
        "top edge resolves to vertical insert-first split");
  Check(NearRect(t.highlight,
                 PaneEdgePreview(ar, SplitOrientation::kVertical,
                                 /*insert_first=*/true)),
        "top preview is Bonsplit's current-pane half");

  t = ResolveDropTarget(w, layout, payload, ar.x + ar.width / 2,
                        ar.y + ar.height - 10, tab_h);
  Check(t.kind == DropKind::kPaneEdge &&
            t.orientation == SplitOrientation::kVertical && !t.insert_first,
        "bottom edge resolves to vertical insert-second split");
  Check(NearRect(t.highlight,
                 PaneEdgePreview(ar, SplitOrientation::kVertical,
                                 /*insert_first=*/false)),
        "bottom preview is Bonsplit's current-pane half");

  t = ResolveDropTarget(w, layout, payload, ar.x + 5, ar.y + ar.height - 5,
                        tab_h);
  Check(t.kind == DropKind::kPaneEdge &&
            t.orientation == SplitOrientation::kHorizontal && t.insert_first,
        "left/right edge zones win at corners");

  const double horizontal_edge = std::max(80.0, content.width * 0.25);
  const double vertical_edge = std::max(80.0, content.height * 0.25);
  t = ResolveDropTarget(w, layout, payload, content.x + horizontal_edge,
                        content.y + content.height / 2, tab_h);
  Check(t.kind == DropKind::kPaneCenter,
        "left boundary itself belongs to center like Bonsplit");
  t = ResolveDropTarget(w, layout, payload,
                        content.x + content.width - horizontal_edge,
                        content.y + content.height / 2, tab_h);
  Check(t.kind == DropKind::kPaneCenter,
        "right boundary itself belongs to center like Bonsplit");
  t = ResolveDropTarget(w, layout, payload, content.x + content.width / 2,
                        content.y + vertical_edge, tab_h);
  Check(t.kind == DropKind::kPaneCenter,
        "top boundary itself belongs to center like Bonsplit");
  t = ResolveDropTarget(w, layout, payload, content.x + content.width / 2,
                        content.y + content.height - vertical_edge, tab_h);
  Check(t.kind == DropKind::kPaneCenter,
        "bottom boundary itself belongs to center like Bonsplit");
  t = ResolveDropTarget(w, layout, payload, content.x + content.width / 2,
                        content.y, tab_h);
  Check(t.kind == DropKind::kPaneEdge &&
            t.orientation == SplitOrientation::kVertical && t.insert_first,
        "first content point belongs to Bonsplit's top zone");

  const double y = ar.y + ar.height / 2;
  t = ResolveDropTarget(w, layout, payload, ar.x - 5, y, tab_h);
  Check(t.kind == DropKind::kNewColumn && t.column_index == 0,
        "left of first column inserts at index 0");
  const double col0_right = layout.panes[0].rect.x + layout.column_width;
  const double col1_left = FindBox(layout, c)->rect.x;
  t = ResolveDropTarget(w, layout, payload, (col0_right + col1_left) / 2, y,
                        tab_h);
  Check(t.kind == DropKind::kNewColumn && t.column_index == 1,
        "column gap inserts between columns");
  const LayoutRect cr = FindBox(layout, c)->rect;
  t = ResolveDropTarget(w, layout, payload, cr.x + cr.width + 40, y, tab_h);
  Check(t.kind == DropKind::kNewColumn && t.column_index == 2,
        "right of last column appends");

  t = ResolveDropTarget(w, layout, payload, ar.x + ar.width / 2, 0, tab_h);
  Check(t.kind == DropKind::kNone, "outside strip vertical extent is none");
  t = ResolveDropTarget(w, layout, payload, cr.x + cr.width + 200, cr.y - 40,
                        tab_h);
  Check(t.kind == DropKind::kNone, "outside pane and gap dead space is none");
  (void)b;
}

void TestDropPreviewUsesCurrentTargetBounds() {
  const double vw = 1000, vh = 800, tab_h = 30;

  {
    // Bonsplit does not predict the tree after source collapse. It shows a
    // stable half of the pane currently under the pointer.
    LayoutMetrics metrics;
    metrics.gap = 24;
    metrics.margin = 13;
    WindowModel model;
    WorkspaceId ws = model.AddWorkspace(SurfaceKind::kWeb, "w");
    UseDefaultFirstColumn(&model, ws);
    PaneId target = model.GetWorkspace(ws)->columns[0].pane->id;
    PaneId source =
        model.SplitPane(ws, target, SplitOrientation::kVertical, 0.5);
    const Workspace& before = *model.GetWorkspace(ws);
    StripLayout before_layout =
        ComputeStripLayout(before, vw, vh, 0.0, kInvalidId, metrics);
    const LayoutRect target_rect = FindBox(before_layout, target)->rect;
    const DragPayload payload = TabPayload(before, source);
    DropTarget drop = ResolveDropTarget(
        before, before_layout, payload, target_rect.x + target_rect.width - 10,
        target_rect.y + target_rect.height / 2, tab_h);

    Check(drop.kind == DropKind::kPaneEdge &&
              NearRect(drop.highlight,
                       PaneEdgePreview(target_rect,
                                       SplitOrientation::kHorizontal,
                                       /*insert_first=*/false)),
          "source collapse does not change the current target preview");
  }

  {
    // The same rule applies to cmux's whole-pane drag extension.
    WindowModel model;
    WorkspaceId ws = model.AddWorkspace(SurfaceKind::kWeb, "w");
    PaneId source = model.GetWorkspace(ws)->columns[0].pane->id;
    PaneId target = model.AddColumn(ws, SurfaceKind::kWeb);
    model.SetColumnWidth(ws, source, 0.4);
    model.SetColumnWidth(ws, target, 0.4);
    const Workspace& before = *model.GetWorkspace(ws);
    StripLayout before_layout =
        ComputeStripLayout(before, vw, vh, 0.0, kInvalidId);
    const LayoutRect target_rect = FindBox(before_layout, target)->rect;
    const DragPayload payload = PanePayload(before, source);
    DropTarget drop = ResolveDropTarget(
        before, before_layout, payload, target_rect.x + target_rect.width / 2,
        target_rect.y + target_rect.height - 10, tab_h);

    Check(drop.kind == DropKind::kPaneEdge &&
              NearRect(drop.highlight,
                       PaneEdgePreview(target_rect,
                                       SplitOrientation::kVertical,
                                       /*insert_first=*/false)),
          "column removal does not change the current target preview");
  }
}

void TestResolveDropTargetBandMinimum() {
  WindowModel model;
  WorkspaceId ws = model.AddWorkspace(SurfaceKind::kWeb, "w");
  PaneId a = model.GetWorkspace(ws)->columns[0].pane->id;
  PaneId b = model.SplitPane(ws, a, SplitOrientation::kHorizontal, 0.5);
  const Workspace& w = *model.GetWorkspace(ws);
  StripLayout layout = ComputeStripLayout(w, 200, 500, 0.0, kInvalidId);
  const LayoutRect ar = FindBox(layout, a)->rect;
  DropTarget t = ResolveDropTarget(w, layout, TabPayload(w, b), ar.x + 70,
                                   ar.y + ar.height / 2, 30);
  Check(t.kind == DropKind::kPaneEdge &&
            t.orientation == SplitOrientation::kHorizontal && t.insert_first,
        "edge band uses the 80px minimum on narrow panes");
}

void TestResolveDropTargetValidity() {
  const double vw = 1000, vh = 800, tab_h = 30;

  {
    WindowModel model;
    WorkspaceId ws = model.AddWorkspace(SurfaceKind::kWeb, "w");
    PaneId a = model.GetWorkspace(ws)->columns[0].pane->id;
    const Workspace& w = *model.GetWorkspace(ws);
    StripLayout layout = ComputeStripLayout(w, vw, vh, 0.0, kInvalidId);
    const LayoutRect ar = FindBox(layout, a)->rect;
    DragPayload payload = TabPayload(w, a);

    DropTarget t = ResolveDropTarget(w, layout, payload, ar.x + ar.width / 2,
                                     ar.y + ar.height / 2, tab_h);
    Check(t.kind == DropKind::kPaneCenter && t.pane == a && t.no_op,
          "tab center-drop on its own pane is a no-op center target");

    t = ResolveDropTarget(w, layout, payload, ar.x + ar.width / 2, ar.y + 10,
                          tab_h);
    Check(t.kind == DropKind::kTabStrip && t.pane == a && !t.no_op,
          "tab drop on its own strip stays valid for reorder");

    t = ResolveDropTarget(w, layout, payload, ar.x + 20, ar.y + ar.height / 2,
                          tab_h);
    Check(t.kind == DropKind::kPaneEdge && t.pane == a && t.no_op,
          "only tab edge-split of its own pane is a no-op edge target");

    t = ResolveDropTarget(w, layout, payload, ar.x - 5, ar.y + ar.height / 2,
                          tab_h);
    Check(t.kind == DropKind::kNewColumn && t.column_index == 0 && t.no_op,
          "only tab in the last workspace pane left gap is a no-op target");
  }

  {
    WindowModel model;
    WorkspaceId ws = model.AddWorkspace(SurfaceKind::kWeb, "w");
    PaneId a = model.GetWorkspace(ws)->columns[0].pane->id;
    model.AddTab(ws, a, SurfaceKind::kWeb);
    const Workspace& w = *model.GetWorkspace(ws);
    StripLayout layout = ComputeStripLayout(w, vw, vh, 0.0, kInvalidId);
    const LayoutRect ar = FindBox(layout, a)->rect;
    DropTarget t = ResolveDropTarget(w, layout, TabPayload(w, a), ar.x + 20,
                                     ar.y + ar.height / 2, tab_h);
    Check(t.kind == DropKind::kPaneEdge && t.pane == a && !t.no_op,
          "multi-tab edge-split of its own pane is valid");
  }

  {
    WindowModel model;
    WorkspaceId ws = model.AddWorkspace(SurfaceKind::kWeb, "w");
    PaneId left = model.GetWorkspace(ws)->columns[0].pane->id;
    PaneId source = model.AddColumn(ws, SurfaceKind::kWeb);
    const Workspace& w = *model.GetWorkspace(ws);
    StripLayout layout = ComputeStripLayout(w, vw, vh, 0.0, kInvalidId);
    const LayoutRect lr = FindBox(layout, left)->rect;
    const LayoutRect sr = FindBox(layout, source)->rect;
    const double y = sr.y + sr.height / 2;
    DragPayload payload = TabPayload(w, source);

    DropTarget t = ResolveDropTarget(w, layout, payload,
                                     (lr.x + lr.width + sr.x) / 2, y, tab_h);
    Check(t.kind == DropKind::kNewColumn && t.column_index == 1 && t.no_op,
          "only tab in only pane of a column own left gap is no-op");
    t = ResolveDropTarget(w, layout, payload, sr.x + sr.width + 40, y, tab_h);
    Check(t.kind == DropKind::kNewColumn && t.column_index == 2 && t.no_op,
          "only tab in only pane of a column own right gap is no-op");

    model.AddTab(ws, source, SurfaceKind::kWeb);
    const Workspace& w2 = *model.GetWorkspace(ws);
    StripLayout layout2 = ComputeStripLayout(w2, vw, vh, 0.0, kInvalidId);
    const LayoutRect lr2 = FindBox(layout2, left)->rect;
    const LayoutRect sr2 = FindBox(layout2, source)->rect;
    t = ResolveDropTarget(w2, layout2, TabPayload(w2, source),
                          (lr2.x + lr2.width + sr2.x) / 2,
                          sr2.y + sr2.height / 2, tab_h);
    Check(t.kind == DropKind::kNewColumn && !t.no_op,
          "multi-tab source can drop in its adjacent column gap");
  }

  {
    // Positive guard for the lone-column case: a single-tab lone-column pane
    // may still drop into a gap that is NOT adjacent to its own column.
    WindowModel model;
    WorkspaceId ws = model.AddWorkspace(SurfaceKind::kWeb, "w");
    PaneId c0 = model.GetWorkspace(ws)->columns[0].pane->id;
    PaneId c1 = model.AddColumn(ws, SurfaceKind::kWeb);
    PaneId source = model.AddColumn(ws, SurfaceKind::kWeb);
    (void)c1;
    const Workspace& w = *model.GetWorkspace(ws);
    StripLayout layout = ComputeStripLayout(w, vw, vh, 0.0, kInvalidId);
    const LayoutRect r0 = FindBox(layout, c0)->rect;
    DropTarget t = ResolveDropTarget(w, layout, TabPayload(w, source), r0.x - 5,
                                     r0.y + r0.height / 2, tab_h);
    Check(t.kind == DropKind::kNewColumn && t.column_index == 0 && !t.no_op,
          "single-tab lone-column pane can drop in a non-adjacent gap");
  }

  {
    WindowModel model;
    WorkspaceId ws = model.AddWorkspace(SurfaceKind::kWeb, "w");
    PaneId a = model.GetWorkspace(ws)->columns[0].pane->id;
    model.SplitPane(ws, a, SplitOrientation::kHorizontal, 0.5);
    const Workspace& w = *model.GetWorkspace(ws);
    StripLayout layout = ComputeStripLayout(w, vw, vh, 0.0, kInvalidId);
    const LayoutRect ar = FindBox(layout, a)->rect;
    DragPayload payload = PanePayload(w, a);

    DropTarget t = ResolveDropTarget(w, layout, payload, ar.x + ar.width / 2,
                                     ar.y + ar.height / 2, tab_h);
    Check(t.kind == DropKind::kPaneCenter && t.pane == a && t.no_op,
          "whole-pane center-drop onto itself is no-op");
    t = ResolveDropTarget(w, layout, payload, ar.x + ar.width / 2, ar.y + 10,
                          tab_h);
    Check(t.kind == DropKind::kTabStrip && t.pane == a && t.no_op,
          "whole-pane drop onto its own strip is no-op");
    t = ResolveDropTarget(w, layout, payload, ar.x + 20, ar.y + ar.height / 2,
                          tab_h);
    Check(t.kind == DropKind::kPaneEdge && t.pane == a && t.no_op,
          "whole-pane edge-split onto itself is no-op");
  }

  {
    WindowModel model;
    WorkspaceId ws = model.AddWorkspace(SurfaceKind::kWeb, "w");
    PaneId left = model.GetWorkspace(ws)->columns[0].pane->id;
    PaneId source = model.AddColumn(ws, SurfaceKind::kWeb);
    const Workspace& w = *model.GetWorkspace(ws);
    StripLayout layout = ComputeStripLayout(w, vw, vh, 0.0, kInvalidId);
    const LayoutRect lr = FindBox(layout, left)->rect;
    const LayoutRect sr = FindBox(layout, source)->rect;
    const double y = sr.y + sr.height / 2;
    DragPayload payload = PanePayload(w, source);

    DropTarget t = ResolveDropTarget(w, layout, payload,
                                     (lr.x + lr.width + sr.x) / 2, y, tab_h);
    Check(t.kind == DropKind::kNewColumn && t.column_index == 1 && t.no_op,
          "whole single-pane column own left gap is no-op");
    t = ResolveDropTarget(w, layout, payload, sr.x + sr.width + 40, y, tab_h);
    Check(t.kind == DropKind::kNewColumn && t.column_index == 2 && t.no_op,
          "whole single-pane column own right gap is no-op");
  }

  {
    WindowModel model;
    WorkspaceId ws = model.AddWorkspace(SurfaceKind::kWeb, "w");
    PaneId a = model.GetWorkspace(ws)->columns[0].pane->id;
    const Workspace& w = *model.GetWorkspace(ws);
    StripLayout layout = ComputeStripLayout(w, vw, vh, 0.0, kInvalidId);
    const LayoutRect ar = FindBox(layout, a)->rect;
    DragPayload payload = PanePayload(w, a);

    DropTarget t = ResolveDropTarget(w, layout, payload, ar.x + 20,
                                     ar.y + ar.height / 2, tab_h);
    Check(t.kind == DropKind::kPaneEdge && t.pane == a && t.no_op,
          "whole drag of workspace's last pane edge is no-op");
    t = ResolveDropTarget(w, layout, payload, ar.x - 5, ar.y + ar.height / 2,
                          tab_h);
    Check(t.kind == DropKind::kNewColumn && t.column_index == 0 && t.no_op,
          "whole drag of workspace's last pane gap is no-op");
  }
}

int LiveIndexForLeft(const std::vector<int>& widths,
                     int dragged_index,
                     int dragged_left,
                     int grab_offset_x,
                     int strip_start_x,
                     int strip_end_x,
                     int spacing,
                     int last_visible_tab_index) {
  return ComputeLiveReorderInsertionIndex(
      widths, dragged_index, dragged_left + grab_offset_x, grab_offset_x,
      strip_start_x, strip_end_x, spacing, last_visible_tab_index);
}

void TestLiveReorderOffsets() {
  const std::vector<int> widths = {100, 120, 80, 90};
  std::vector<int> offsets =
      ComputeLiveReorderOffsets(widths, /*dragged_index=*/1,
                                /*insertion_index=*/0, /*spacing=*/4);
  Check(offsets == std::vector<int>({124, 0, 0, 0}),
        "drag left shifts intervening tabs right");

  offsets = ComputeLiveReorderOffsets(widths, /*dragged_index=*/1,
                                      /*insertion_index=*/4, /*spacing=*/4);
  Check(offsets == std::vector<int>({0, 0, -124, -124}),
        "drag right shifts intervening tabs left");

  offsets = ComputeLiveReorderOffsets(widths, /*dragged_index=*/1,
                                      /*insertion_index=*/2, /*spacing=*/4);
  Check(offsets == std::vector<int>({0, 0, 0, 0}),
        "same-slot reorder leaves siblings still");

  offsets = ComputeLiveReorderOffsets(widths, /*dragged_index=*/99,
                                      /*insertion_index=*/0, /*spacing=*/4);
  Check(offsets == std::vector<int>({0, 0, 0, 0}),
        "invalid dragged index leaves siblings still");
}

void TestLiveReorderInsertionIndex() {
  constexpr int kStart = 2;
  constexpr int kEnd = 600;
  constexpr int kSpacing = 4;

  {
    const std::vector<int> widths = {100, 100, 100};
    // Ideal left edges are 2, 106, and 210. Chromium chooses the closest
    // resulting ideal left edge, so a drag from slot 1 crosses at each
    // half-slot midpoint rather than waiting for tab centers to cross.
    for (int grab : {0, 50, 100}) {
      Check(LiveIndexForLeft(widths, /*dragged_index=*/1,
                             /*dragged_left=*/55, grab, kStart, kEnd, kSpacing,
                             /*last_visible_tab_index=*/2) == 1,
            "equal tabs wait just right of the left midpoint");
      Check(LiveIndexForLeft(widths, /*dragged_index=*/1,
                             /*dragged_left=*/54, grab, kStart, kEnd, kSpacing,
                             /*last_visible_tab_index=*/2) == 0,
            "equal tabs choose the earlier slot at left midpoint tie");
      Check(LiveIndexForLeft(widths, /*dragged_index=*/1,
                             /*dragged_left=*/158, grab, kStart, kEnd,
                             kSpacing,
                             /*last_visible_tab_index=*/2) == 1,
            "equal tabs retain current slot at right midpoint tie");
      Check(LiveIndexForLeft(widths, /*dragged_index=*/1,
                             /*dragged_left=*/159, grab, kStart, kEnd,
                             kSpacing,
                             /*last_visible_tab_index=*/2) == 3,
            "equal tabs move right just after the midpoint");
    }
  }

  {
    const std::vector<int> widths = {40, 160, 80, 90};
    // Resulting valid left edges for dragging slot 1 are 2, 46, 130, 224.
    Check(LiveIndexForLeft(widths, /*dragged_index=*/1,
                           /*dragged_left=*/88, /*grab_offset_x=*/80, kStart,
                           kEnd, kSpacing,
                           /*last_visible_tab_index=*/3) == 1,
          "unequal tabs retain the earlier candidate at midpoint tie");
    Check(LiveIndexForLeft(widths, /*dragged_index=*/1,
                           /*dragged_left=*/89, /*grab_offset_x=*/80, kStart,
                           kEnd, kSpacing,
                           /*last_visible_tab_index=*/3) == 3,
          "unequal tabs cross into the next resulting ideal position");
    Check(LiveIndexForLeft(widths, /*dragged_index=*/1,
                           /*dragged_left=*/178, /*grab_offset_x=*/80, kStart,
                           kEnd, kSpacing,
                           /*last_visible_tab_index=*/3) == 4,
          "unequal tabs can cross multiple resulting ideal positions");
  }

  {
    const std::vector<int> widths = {100, 100, 100, 100, 100};
    Check(LiveIndexForLeft(widths, /*dragged_index=*/0,
                           /*dragged_left=*/10000, /*grab_offset_x=*/0, kStart,
                           kEnd, kSpacing,
                           /*last_visible_tab_index=*/2) == 3,
          "overflow clamps insertion after the last visible tab");
    Check(LiveIndexForLeft(widths, /*dragged_index=*/0,
                           /*dragged_left=*/10000, /*grab_offset_x=*/0, kStart,
                           kEnd, kSpacing,
                           /*last_visible_tab_index=*/1) == 2,
          "a shorter visible prefix further clamps insertion");
    Check(LiveIndexForLeft(widths, /*dragged_index=*/0,
                           /*dragged_left=*/10000, /*grab_offset_x=*/0, kStart,
                           kEnd, kSpacing,
                           /*last_visible_tab_index=*/-1) == 0,
          "no visible tabs clamp to the first insertion point");
  }

  {
    const std::vector<int> widths = {40, 160, 80};
    Check(ComputeLiveReorderInsertionIndex(
              widths, /*dragged_index=*/1, /*cursor_x=*/-10000,
              /*grab_offset_x=*/widths[1], kStart, kEnd, kSpacing,
              /*last_visible_tab_index=*/2) == 0,
          "live reorder clamps far-left edge to strip start");
    Check(ComputeLiveReorderInsertionIndex(
              widths, /*dragged_index=*/1, /*cursor_x=*/10000,
              /*grab_offset_x=*/0, kStart, kEnd, kSpacing,
              /*last_visible_tab_index=*/2) == 3,
          "live reorder clamps far-right edge to strip end");
    Check(ComputeLiveReorderInsertionIndex(
              widths, /*dragged_index=*/99, /*cursor_x=*/100,
              /*grab_offset_x=*/0, kStart, kEnd, kSpacing,
              /*last_visible_tab_index=*/2) == 0,
          "invalid dragged index returns the first insertion point");
  }
}

}  // namespace

int main() {
  std::printf("window_layout_test\n");
  TestStripFocusScroll();
  TestStripSingle();
  TestStripScrollClampsNonNegative();
  TestScrollIntoViewPolicies();
  TestZeroGapMarginStripLayout();
  TestZeroGapMarginTiledLayout();
  TestZeroGapFocusedScrollMath();
  TestStripInColumnSplit();
  TestStripMixedColumnWidths();
  TestTiledColumnModeProportions();
  TestTiledSingleColumnFillsAvailableWidth();
  TestTiledRoundTripPreservesStripWidths();
  TestTiledDegenerateViewport();
  TestTiledDropTargetsUseTiledColumnWidths();
  TestTiledSplit();
  TestTiledVerticalSplit();
  TestSpatialFocusMatrix();
  TestSpatialFocusSymmetricNestedShape();
  TestSpatialFocusSinglePaneColumnsRegression();
  TestResolveDropTargetZones();
  TestDropPreviewUsesCurrentTargetBounds();
  TestResolveDropTargetBandMinimum();
  TestResolveDropTargetValidity();
  TestLiveReorderOffsets();
  TestLiveReorderInsertionIndex();
  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
