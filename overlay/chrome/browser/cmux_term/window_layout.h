// Copyright 2012 The Chromium Authors
// Copyright (c) 2026 Alasdair Monk
// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later AND MIT AND BSD-3-Clause
//
// Contains Bonsplit- and Chromium-derived regions; see
// docs/source-provenance.md.

#ifndef CHROME_BROWSER_CMUX_TERM_WINDOW_LAYOUT_H_
#define CHROME_BROWSER_CMUX_TERM_WINDOW_LAYOUT_H_

#include <vector>

#include "chrome/browser/cmux_term/window_model.h"

// Pure geometry for a WindowModel workspace: turns its columns (each a split
// tree of Split|Pane) plus a viewport into on-screen rectangles. This is the
// half of the renderer that NiriModel used to own; pulling it out keeps
// WindowModel structural-only and lets a Views/AppKit/GTK renderer apply
// identical geometry.
//
// The two policies COMPOSE (the design's core idea):
//   * Strip  -- niri scrollable strip: every COLUMN is a fixed-width strip
//               laid left-to-right, horizontally scrolled, the focused pane's
//               column kept in view with `gap` of breathing room. INSIDE each
//               column its split tree is tiled recursively. Derived from
//               NiriModel's geometry.
//   * Tiled  -- all workspace columns fit in the viewport at once. Stored
//               column width fractions become normalized shares; scroll is 0.
//               INSIDE each column its split tree is still tiled recursively.
//
// All coordinates are logical points in the strip/viewport's own
// top-left-origin space; the renderer converts to backing pixels.

namespace cmux {

struct LayoutRect {
  double x = 0;
  double y = 0;
  double width = 0;
  double height = 0;
};

struct PaneBox {
  PaneId pane = kInvalidId;
  LayoutRect rect;
};

// A draggable divider between two children of a split (always within one
// column -- columns themselves have no dividers).
struct DividerBox {
  SplitId split = kInvalidId;
  SplitOrientation orientation = SplitOrientation::kHorizontal;
  LayoutRect rect;
  double ratio = 0.5;
  // Draggable distance excluding the divider gap. A pointer delta divided by
  // this span is the corresponding split-ratio delta.
  double resize_span = 0.0;
};

// Layout constants. Defaults mirror NiriModel::Metrics exactly so swapping the
// renderer onto this engine is behaviorally a no-op for the existing strip.
struct LayoutMetrics {
  double gap = 10;    // space between columns / split children
  double margin = 8;  // top/bottom inset inside the strip
  double min_column_width = 360;
  double column_fraction = 0.62;  // a niri column is ~62% of the viewport
};

struct StripLayout {
  std::vector<PaneBox> panes;        // flat in-order (matches PanesOf)
  std::vector<DividerBox> dividers;  // in-column split dividers only
  // Retained so drop targets use the same configured gaps as this layout.
  LayoutMetrics metrics;
  double column_width = 0;  // default width used by columns with no override
  std::vector<double> column_widths;  // actual width of each column
  double content_width = 0;  // full laid-out width, for scroll-extent reporting
  double scroll_x = 0;       // the scroll actually applied after clamping

  StripLayout();
  ~StripLayout();
  StripLayout(StripLayout&&);
  StripLayout& operator=(StripLayout&&);
};

struct TiledLayout {
  std::vector<PaneBox> panes;
  std::vector<DividerBox> dividers;

  TiledLayout();
  ~TiledLayout();
  TiledLayout(TiledLayout&&);
  TiledLayout& operator=(TiledLayout&&);
};

enum class DropKind {
  kNone,
  kTabStrip,
  kPaneCenter,
  kPaneEdge,
  kNewColumn,
};

struct DropTarget {
  DropKind kind = DropKind::kNone;
  PaneId pane = kInvalidId;  // kTabStrip / kPaneCenter / kPaneEdge
  SplitOrientation orientation = SplitOrientation::kHorizontal;
  bool insert_first = false;  // kPaneEdge: left/top
  int column_index = -1;      // kNewColumn: insertion index into columns
  LayoutRect highlight;
  // Bonsplit keeps the full placeholder at opacity zero when no zone is
  // active, then springs between this rect and `highlight`.
  LayoutRect hidden_highlight;
  // The pointer is over real drop geometry, but committing would leave the
  // model unchanged or violate model invariants. Render normal feedback and
  // treat release as a clean cancel.
  bool no_op = false;
};

enum class ScrollIntoViewPolicy {
  // Click/native activation: only scroll if the column is actually clipped, and
  // then only by the clipped amount.
  kEnsureVisible,
  // Keyboard focus/new pane behavior: keep the existing gap of breathing room.
  kEnsureVisibleWithMargin,
};

struct DragPayload {
  PaneId source_pane = kInvalidId;
  int source_tab_count = 0;
  bool whole_pane = false;
  SurfaceTabId tab = kInvalidId;
};

// Workspace column layout. In WorkspaceLayoutMode::kStrip, `scroll_x` is the
// requested scroll; the result's scroll_x is clamped so it never goes negative
// and (when focus_pane names a pane in `ws`) the column CONTAINING the focused
// pane is fully in view with `gap` of breathing room on each edge. In
// WorkspaceLayoutMode::kTiled, all columns fit the viewport proportionally and
// scroll_x is always 0. Each column's split tree is tiled inside its column;
// panes come out in PanesOf order.
StripLayout ComputeStripLayout(const Workspace& ws,
                               double viewport_w,
                               double viewport_h,
                               double scroll_x,
                               PaneId focus_pane,
                               const LayoutMetrics& metrics = {});

StripLayout ComputeStripLayout(const Workspace& ws,
                               double viewport_w,
                               double viewport_h,
                               double scroll_x,
                               PaneId focus_pane,
                               ScrollIntoViewPolicy policy,
                               const LayoutMetrics& metrics = {});

// Classic tiling of one split tree: panes fill the viewport, divided by each
// split's ratio.
TiledLayout ComputeTiledLayout(const LayoutNode& root,
                               double viewport_w,
                               double viewport_h,
                               const LayoutMetrics& metrics = {});

// Geometric focus relation. Uses the same strip geometry as rendering, but in
// a fixed nominal viewport so horizontal scroll cannot affect neighbor choice.
PaneId SpatialNeighborPane(const Workspace& ws,
                           PaneId from,
                           Direction direction,
                           const LayoutMetrics& metrics = {});

// Pure tab/pane drop-target resolver. Coordinates are in strip-layout space,
// matching StripLayout pane rectangles.
DropTarget ResolveDropTarget(const Workspace& ws,
                             const StripLayout& layout,
                             const DragPayload& payload,
                             double x,
                             double y,
                             double tab_strip_height);

// Pure math for live same-strip tab reordering. Mirrors Chromium's detached
// tab calculation: choose the valid insertion whose resulting ideal left edge
// is nearest the dragged pill's current left edge. The return value keeps
// cmux's pre-removal convention: if it is greater than the dragged index, the
// final model index is one less after removing the dragged tab. Destinations
// are clamped to the insertion point after `last_visible_tab_index` so an
// overflow-hidden tail cannot receive a visible dragged tab.
int ComputeLiveReorderInsertionIndex(const std::vector<int>& widths,
                                     int dragged_index,
                                     int cursor_x,
                                     int grab_offset_x,
                                     int strip_start_x,
                                     int strip_end_x,
                                     int spacing,
                                     int last_visible_tab_index);
std::vector<int> ComputeLiveReorderOffsets(const std::vector<int>& widths,
                                           int dragged_index,
                                           int insertion_index,
                                           int spacing);

}  // namespace cmux

#endif  // CHROME_BROWSER_CMUX_TERM_WINDOW_LAYOUT_H_
