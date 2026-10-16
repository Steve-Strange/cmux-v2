// Copyright 2012 The Chromium Authors
// Copyright (c) 2026 Alasdair Monk
// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later AND MIT AND BSD-3-Clause
//
// Contains Bonsplit- and Chromium-derived regions; see
// docs/source-provenance.md.

#include "chrome/browser/cmux_term/window_layout.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace cmux {

// Out-of-line special members (chromium-style: structs with vector members).
StripLayout::StripLayout() = default;
StripLayout::~StripLayout() = default;
StripLayout::StripLayout(StripLayout&&) = default;
StripLayout& StripLayout::operator=(StripLayout&&) = default;

TiledLayout::TiledLayout() = default;
TiledLayout::~TiledLayout() = default;
TiledLayout::TiledLayout(TiledLayout&&) = default;
TiledLayout& TiledLayout::operator=(TiledLayout&&) = default;

namespace {

double DefaultColumnWidth(double viewport_w, const LayoutMetrics& m) {
  return std::max(m.min_column_width, viewport_w * m.column_fraction);
}

std::vector<double> ColumnWidths(const Workspace& ws,
                                 double viewport_w,
                                 const LayoutMetrics& m) {
  std::vector<double> widths;
  const double default_width = DefaultColumnWidth(viewport_w, m);
  widths.reserve(ws.columns.size());
  for (size_t i = 0; i < ws.columns.size(); ++i) {
    const double fraction =
        i < ws.column_widths.size() ? ws.column_widths[i] : 0.0;
    const double width = fraction > 0.0 ? viewport_w * fraction : default_width;
    widths.push_back(std::max(m.min_column_width, width));
  }
  return widths;
}

std::vector<double> TiledColumnWidths(const Workspace& ws,
                                      double viewport_w,
                                      const LayoutMetrics& m) {
  std::vector<double> weights;
  weights.reserve(ws.columns.size());
  double total_weight = 0.0;
  for (size_t i = 0; i < ws.columns.size(); ++i) {
    const double fraction =
        i < ws.column_widths.size() ? ws.column_widths[i] : 0.0;
    const double weight = fraction > 0.0 ? fraction : m.column_fraction;
    weights.push_back(weight);
    total_weight += weight;
  }
  if (total_weight <= 0.0) {
    total_weight = static_cast<double>(std::max<size_t>(1, weights.size()));
    for (double& weight : weights) {
      weight = 1.0;
    }
  }

  const double gap_total =
      weights.empty() ? 0.0 : m.gap * (static_cast<double>(weights.size()) + 1);
  const double available = std::max(0.0, viewport_w - gap_total);
  std::vector<double> widths;
  widths.reserve(weights.size());
  for (double weight : weights) {
    widths.push_back(available * weight / total_weight);
  }
  return widths;
}

double ContentWidth(const std::vector<double>& widths, const LayoutMetrics& m) {
  if (widths.empty()) {
    return 0.0;
  }
  double width = m.gap;
  for (double column_width : widths) {
    width += column_width + m.gap;
  }
  return width;
}

double ColumnLeft(const std::vector<double>& widths,
                  int index,
                  const LayoutMetrics& m) {
  double left = m.gap;
  for (int i = 0; i < index && i < static_cast<int>(widths.size()); ++i) {
    left += widths[i] + m.gap;
  }
  return left;
}

bool Contains(const LayoutRect& r, double x, double y) {
  return x >= r.x && y >= r.y && x <= r.x + r.width && y <= r.y + r.height;
}

double Right(const LayoutRect& r) {
  return r.x + r.width;
}

double Bottom(const LayoutRect& r) {
  return r.y + r.height;
}

enum class BonsplitDropZone {
  kCenter,
  kLeft,
  kRight,
  kTop,
  kBottom,
};

constexpr double kBonsplitDropEdgeRatio = 0.25;
constexpr double kBonsplitDropEdgeMinimum = 80.0;
constexpr double kBonsplitDropHighlightPadding = 4.0;

BonsplitDropZone ResolveBonsplitDropZone(const LayoutRect& content,
                                        double x,
                                        double y) {
  const double local_x = x - content.x;
  const double local_y = y - content.y;
  const double horizontal_edge =
      std::max(kBonsplitDropEdgeMinimum,
               content.width * kBonsplitDropEdgeRatio);
  const double vertical_edge =
      std::max(kBonsplitDropEdgeMinimum,
               content.height * kBonsplitDropEdgeRatio);

  // Match Bonsplit's strict boundaries and its left/right priority at corners.
  if (local_x < horizontal_edge) {
    return BonsplitDropZone::kLeft;
  }
  if (local_x > content.width - horizontal_edge) {
    return BonsplitDropZone::kRight;
  }
  if (local_y < vertical_edge) {
    return BonsplitDropZone::kTop;
  }
  if (local_y > content.height - vertical_edge) {
    return BonsplitDropZone::kBottom;
  }
  return BonsplitDropZone::kCenter;
}

LayoutRect BonsplitDropHighlight(const LayoutRect& content,
                                 BonsplitDropZone zone) {
  const double padding = kBonsplitDropHighlightPadding;
  const double half_width = content.width * 0.5;
  const double half_height = content.height * 0.5;
  const double full_width = std::max(0.0, content.width - padding * 2.0);
  const double full_height = std::max(0.0, content.height - padding * 2.0);
  switch (zone) {
    case BonsplitDropZone::kCenter:
      return LayoutRect{content.x + padding, content.y + padding, full_width,
                        full_height};
    case BonsplitDropZone::kLeft:
      return LayoutRect{content.x + padding, content.y + padding,
                        std::max(0.0, half_width - padding), full_height};
    case BonsplitDropZone::kRight:
      return LayoutRect{content.x + half_width, content.y + padding,
                        std::max(0.0, half_width - padding), full_height};
    case BonsplitDropZone::kTop:
      return LayoutRect{content.x + padding, content.y + padding, full_width,
                        std::max(0.0, half_height - padding)};
    case BonsplitDropZone::kBottom:
      return LayoutRect{content.x + padding, content.y + half_height,
                        full_width, std::max(0.0, half_height - padding)};
  }
  return {};
}

double Overlap(double a0, double a1, double b0, double b1) {
  return std::min(a1, b1) - std::max(a0, b0);
}

const Pane* FindPaneInNode(const LayoutNode& node, PaneId id) {
  if (node.is_pane()) {
    return node.pane->id == id ? node.pane.get() : nullptr;
  }
  if (const Pane* p = FindPaneInNode(node.split->first, id)) {
    return p;
  }
  return FindPaneInNode(node.split->second, id);
}

const Pane* FindPane(const Workspace& ws, PaneId id) {
  for (const LayoutNode& column : ws.columns) {
    if (const Pane* p = FindPaneInNode(column, id)) {
      return p;
    }
  }
  return nullptr;
}

int CountPanes(const LayoutNode& node) {
  if (node.is_pane()) {
    return 1;
  }
  return CountPanes(node.split->first) + CountPanes(node.split->second);
}

int CountPanes(const Workspace& ws) {
  int count = 0;
  for (const LayoutNode& column : ws.columns) {
    count += CountPanes(column);
  }
  return count;
}

bool IsColumnOnlyPane(const Workspace& ws, PaneId id) {
  const int column = ColumnOf(ws, id);
  return column >= 0 && column < static_cast<int>(ws.columns.size()) &&
         ws.columns[column].is_pane() && ws.columns[column].pane->id == id;
}

bool IsOwnAdjacentColumnGap(const Workspace& ws,
                            PaneId source_pane,
                            int column_index) {
  const int source_column = ColumnOf(ws, source_pane);
  return source_column >= 0 &&
         (column_index == source_column || column_index == source_column + 1);
}

bool PayloadNamesExistingSource(const Workspace& ws,
                                const DragPayload& payload,
                                int* source_tab_count) {
  const Pane* source = FindPane(ws, payload.source_pane);
  if (!source) {
    return false;
  }
  if (!payload.whole_pane && payload.tab != kInvalidId &&
      source->IndexOfTab(payload.tab) < 0) {
    return false;
  }
  if (source_tab_count) {
    *source_tab_count = payload.source_tab_count > 0
                            ? payload.source_tab_count
                            : static_cast<int>(source->tabs.size());
  }
  return true;
}

bool IsValidDropTarget(const Workspace& ws,
                       const DragPayload& payload,
                       const DropTarget& target) {
  if (target.kind == DropKind::kNone) {
    return false;
  }

  int source_tab_count = 0;
  if (!PayloadNamesExistingSource(ws, payload, &source_tab_count)) {
    return false;
  }

  // Belt-and-braces: no advertised target may remove the workspace's last pane.
  // Same-pane tab-strip reorders are the exception because they never remove
  // anything, even when they are same-index no-ops.
  const bool last_workspace_pane = CountPanes(ws) <= 1;

  if (payload.whole_pane) {
    if (last_workspace_pane) {
      return false;
    }
    if ((target.kind == DropKind::kPaneCenter ||
         target.kind == DropKind::kTabStrip ||
         target.kind == DropKind::kPaneEdge) &&
        target.pane == payload.source_pane) {
      return false;
    }
    if (target.kind == DropKind::kNewColumn &&
        IsColumnOnlyPane(ws, payload.source_pane) &&
        IsOwnAdjacentColumnGap(ws, payload.source_pane, target.column_index)) {
      return false;
    }
    return true;
  }

  if (target.kind == DropKind::kPaneCenter &&
      target.pane == payload.source_pane) {
    return false;
  }
  if (target.kind == DropKind::kPaneEdge &&
      target.pane == payload.source_pane && source_tab_count <= 1) {
    return false;
  }
  if (target.kind == DropKind::kNewColumn && source_tab_count <= 1 &&
      IsColumnOnlyPane(ws, payload.source_pane) &&
      IsOwnAdjacentColumnGap(ws, payload.source_pane, target.column_index)) {
    return false;
  }
  if (last_workspace_pane && source_tab_count <= 1 &&
      target.kind != DropKind::kTabStrip) {
    return false;
  }
  return true;
}

// Recursive tiling walk: place `node` inside `box`, emitting pane + divider
// rectangles. Mirrors a standard split-tree layout.
void TileNode(const LayoutNode& node,
              const LayoutRect& box,
              const LayoutMetrics& m,
              std::vector<PaneBox>* panes,
              std::vector<DividerBox>* dividers) {
  if (node.is_pane()) {
    panes->push_back(PaneBox{node.pane->id, box});
    return;
  }
  const Split& s = *node.split;
  const double ratio = ClampSplitRatio(s.ratio);
  if (s.orientation == SplitOrientation::kHorizontal) {
    // Side by side, divided along x with a vertical divider between them.
    const double avail = std::max(0.0, box.width - m.gap);
    const double w1 = avail * ratio;
    const double w2 = avail - w1;
    LayoutRect first{box.x, box.y, w1, box.height};
    LayoutRect second{box.x + w1 + m.gap, box.y, w2, box.height};
    if (dividers) {
      dividers->push_back(
          DividerBox{s.id, s.orientation,
                     LayoutRect{box.x + w1, box.y, m.gap, box.height}, ratio,
                     avail});
    }
    TileNode(s.first, first, m, panes, dividers);
    TileNode(s.second, second, m, panes, dividers);
  } else {
    // Stacked, divided along y with a horizontal divider between them.
    const double avail = std::max(0.0, box.height - m.gap);
    const double h1 = avail * ratio;
    const double h2 = avail - h1;
    LayoutRect first{box.x, box.y, box.width, h1};
    LayoutRect second{box.x, box.y + h1 + m.gap, box.width, h2};
    if (dividers) {
      dividers->push_back(
          DividerBox{s.id, s.orientation,
                     LayoutRect{box.x, box.y + h1, box.width, m.gap}, ratio,
                     avail});
    }
    TileNode(s.first, first, m, panes, dividers);
    TileNode(s.second, second, m, panes, dividers);
  }
}

}  // namespace

StripLayout ComputeStripLayout(const Workspace& ws,
                               double viewport_w,
                               double viewport_h,
                               double scroll_x,
                               PaneId focus_pane,
                               const LayoutMetrics& m) {
  return ComputeStripLayout(ws, viewport_w, viewport_h, scroll_x, focus_pane,
                            ScrollIntoViewPolicy::kEnsureVisibleWithMargin, m);
}

StripLayout ComputeStripLayout(const Workspace& ws,
                               double viewport_w,
                               double viewport_h,
                               double scroll_x,
                               PaneId focus_pane,
                               ScrollIntoViewPolicy policy,
                               const LayoutMetrics& m) {
  StripLayout out;
  out.metrics = m;
  const double default_col_w = DefaultColumnWidth(viewport_w, m);
  out.column_width = default_col_w;
  if (ws.layout_mode == WorkspaceLayoutMode::kTiled) {
    out.column_widths = TiledColumnWidths(ws, viewport_w, m);
    const size_t n = ws.columns.size();
    out.content_width = ContentWidth(out.column_widths, m);
    out.scroll_x = 0.0;

    out.panes.reserve(n);
    for (size_t i = 0; i < n; ++i) {
      const double col_w = out.column_widths[i];
      const double x = ColumnLeft(out.column_widths, static_cast<int>(i), m);
      const LayoutRect column_box{x, m.margin, col_w,
                                  viewport_h - 2 * m.margin};
      TileNode(ws.columns[i], column_box, m, &out.panes, &out.dividers);
    }
    return out;
  }

  out.column_widths = ColumnWidths(ws, viewport_w, m);
  const size_t n = ws.columns.size();
  out.content_width = ContentWidth(out.column_widths, m);

  // Keep the column CONTAINING the focused pane visible. Keyboard/new-pane
  // callers ask for the historical `gap` breathing room; click/native
  // activation asks only for actual clipped pixels to be revealed.
  int focus = ColumnOf(ws, focus_pane);
  if (focus >= 0) {
    const double left = ColumnLeft(out.column_widths, focus, m);
    const double right = left + out.column_widths[focus];
    const double edge_margin =
        policy == ScrollIntoViewPolicy::kEnsureVisibleWithMargin ? m.gap : 0.0;
    if (right - scroll_x > viewport_w - edge_margin) {
      scroll_x = right - viewport_w + edge_margin;
    }
    if (left - scroll_x < edge_margin) {
      scroll_x = left - edge_margin;
    }
  }
  if (scroll_x < 0) {
    scroll_x = 0;
  }
  const double max_scroll = std::max(0.0, out.content_width - viewport_w);
  if (scroll_x > max_scroll) {
    scroll_x = max_scroll;
  }
  out.scroll_x = scroll_x;

  // Each column is a fixed-width strip; its split tree tiles the strip
  // recursively. Panes emit column-by-column, in-order within each column, so
  // the sequence matches WindowModel::PanesOf.
  out.panes.reserve(n);
  for (size_t i = 0; i < n; ++i) {
    const double col_w = out.column_widths[i];
    const double x =
        ColumnLeft(out.column_widths, static_cast<int>(i), m) - scroll_x;
    const LayoutRect column_box{x, m.margin, col_w, viewport_h - 2 * m.margin};
    TileNode(ws.columns[i], column_box, m, &out.panes, &out.dividers);
  }
  return out;
}

TiledLayout ComputeTiledLayout(const LayoutNode& root,
                               double viewport_w,
                               double viewport_h,
                               const LayoutMetrics& m) {
  TiledLayout out;
  LayoutRect box{m.margin, m.margin, viewport_w - 2 * m.margin,
                 viewport_h - 2 * m.margin};
  TileNode(root, box, m, &out.panes, &out.dividers);
  return out;
}

PaneId SpatialNeighborPane(const Workspace& ws,
                           PaneId from,
                           Direction direction,
                           const LayoutMetrics& m) {
  constexpr double kNominalViewport = 1000.0;
  StripLayout layout = ComputeStripLayout(ws, kNominalViewport,
                                          kNominalViewport, 0.0, kInvalidId, m);

  const PaneBox* focused = nullptr;
  for (const PaneBox& pane : layout.panes) {
    if (pane.pane == from) {
      focused = &pane;
      break;
    }
  }
  if (!focused) {
    return kInvalidId;
  }

  const LayoutRect& f = focused->rect;
  const double epsilon = std::max(1.0, m.gap + 1e-6);
  PaneId best = kInvalidId;
  double best_distance = std::numeric_limits<double>::infinity();
  double best_overlap = -1.0;
  double best_top = std::numeric_limits<double>::infinity();
  double best_left = std::numeric_limits<double>::infinity();

  for (const PaneBox& candidate : layout.panes) {
    if (candidate.pane == from) {
      continue;
    }
    const LayoutRect& r = candidate.rect;
    double distance = 0.0;
    double overlap = 0.0;
    bool qualifies = false;
    switch (direction) {
      case Direction::kRight:
        qualifies = r.x >= Right(f) - epsilon;
        distance = std::max(0.0, r.x - Right(f));
        overlap = Overlap(f.y, Bottom(f), r.y, Bottom(r));
        break;
      case Direction::kLeft:
        qualifies = Right(r) <= f.x + epsilon;
        distance = std::max(0.0, f.x - Right(r));
        overlap = Overlap(f.y, Bottom(f), r.y, Bottom(r));
        break;
      case Direction::kDown:
        qualifies = r.y >= Bottom(f) - epsilon;
        distance = std::max(0.0, r.y - Bottom(f));
        overlap = Overlap(f.x, Right(f), r.x, Right(r));
        break;
      case Direction::kUp:
        qualifies = Bottom(r) <= f.y + epsilon;
        distance = std::max(0.0, f.y - Bottom(r));
        overlap = Overlap(f.x, Right(f), r.x, Right(r));
        break;
    }
    if (!qualifies || overlap <= 0.0) {
      continue;
    }
    const bool better =
        distance < best_distance - 1e-6 ||
        (std::fabs(distance - best_distance) <= 1e-6 &&
         (overlap > best_overlap + 1e-6 ||
          (std::fabs(overlap - best_overlap) <= 1e-6 &&
           (r.y < best_top - 1e-6 ||
            (std::fabs(r.y - best_top) <= 1e-6 && r.x < best_left - 1e-6)))));
    if (better) {
      best = candidate.pane;
      best_distance = distance;
      best_overlap = overlap;
      best_top = r.y;
      best_left = r.x;
    }
  }
  return best;
}

DropTarget ResolveDropTarget(const Workspace& ws,
                             const StripLayout& layout,
                             const DragPayload& payload,
                             double x,
                             double y,
                             double tab_strip_height) {
  const LayoutMetrics& m = layout.metrics;
  DropTarget out;
  auto validated = [&]() {
    if (IsValidDropTarget(ws, payload, out)) {
      return out;
    }
    out.no_op = true;
    return out;
  };
  for (const PaneBox& pane : layout.panes) {
    const LayoutRect& r = pane.rect;
    if (!Contains(r, x, y)) {
      continue;
    }
    const double strip_h = std::max(0.0, std::min(tab_strip_height, r.height));
    const LayoutRect content{r.x, r.y + strip_h, r.width,
                             std::max(0.0, r.height - strip_h)};
    if (y < content.y || content.height <= 0.0) {
      out.kind = DropKind::kTabStrip;
      out.pane = pane.pane;
      out.highlight = LayoutRect{r.x, r.y, r.width, strip_h};
      return validated();
    }

    out.pane = pane.pane;
    const BonsplitDropZone zone = ResolveBonsplitDropZone(content, x, y);
    // Bonsplit computes the active zone in content-only coordinates. cmux
    // deliberately applies its exact placeholder formula to the current whole
    // pane so the blue feedback also covers the target's tab strip.
    out.highlight = BonsplitDropHighlight(r, zone);
    out.hidden_highlight =
        BonsplitDropHighlight(r, BonsplitDropZone::kCenter);
    switch (zone) {
      case BonsplitDropZone::kCenter:
        out.kind = DropKind::kPaneCenter;
        break;
      case BonsplitDropZone::kLeft:
        out.kind = DropKind::kPaneEdge;
        out.orientation = SplitOrientation::kHorizontal;
        out.insert_first = true;
        break;
      case BonsplitDropZone::kRight:
        out.kind = DropKind::kPaneEdge;
        out.orientation = SplitOrientation::kHorizontal;
        out.insert_first = false;
        break;
      case BonsplitDropZone::kTop:
        out.kind = DropKind::kPaneEdge;
        out.orientation = SplitOrientation::kVertical;
        out.insert_first = true;
        break;
      case BonsplitDropZone::kBottom:
        out.kind = DropKind::kPaneEdge;
        out.orientation = SplitOrientation::kVertical;
        out.insert_first = false;
        break;
    }
    return validated();
  }

  if (ws.columns.empty() || layout.column_widths.empty()) {
    return out;
  }
  double strip_top = std::numeric_limits<double>::infinity();
  double strip_bottom = -std::numeric_limits<double>::infinity();
  for (const PaneBox& pane : layout.panes) {
    strip_top = std::min(strip_top, pane.rect.y);
    strip_bottom = std::max(strip_bottom, Bottom(pane.rect));
  }
  if (!std::isfinite(strip_top) || y < strip_top || y > strip_bottom) {
    return out;
  }

  const int n = static_cast<int>(ws.columns.size());
  auto column_width = [&](int i) {
    if (i >= 0 && i < static_cast<int>(layout.column_widths.size())) {
      return layout.column_widths[i];
    }
    return layout.column_width;
  };
  auto column_left = [&](int i) {
    double left = m.gap - layout.scroll_x;
    for (int j = 0; j < i; ++j) {
      left += column_width(j) + m.gap;
    }
    return left;
  };
  for (int i = 0; i < n; ++i) {
    const double left = column_left(i);
    const double right = left + column_width(i);
    if (x >= left && x <= right) {
      return out;
    }
  }

  int index = 0;
  double bar_x = column_left(0) - m.gap * 0.5;
  if (x < column_left(0)) {
    index = 0;
  } else {
    index = n;
    bar_x = column_left(n - 1) + column_width(n - 1) + m.gap * 0.5;
    for (int i = 0; i < n - 1; ++i) {
      const double right = column_left(i) + column_width(i);
      const double next_left = column_left(i + 1);
      if (x > right && x < next_left) {
        index = i + 1;
        bar_x = (right + next_left) * 0.5;
        break;
      }
    }
  }
  out.kind = DropKind::kNewColumn;
  out.column_index = index;
  out.highlight =
      LayoutRect{bar_x - 1.5, strip_top, 3.0, strip_bottom - strip_top};
  return validated();
}

int ComputeLiveReorderInsertionIndex(const std::vector<int>& widths,
                                     int dragged_index,
                                     int cursor_x,
                                     int grab_offset_x,
                                     int strip_start_x,
                                     int strip_end_x,
                                     int spacing,
                                     int last_visible_tab_index) {
  const int count = static_cast<int>(widths.size());
  if (count == 0 || dragged_index < 0 || dragged_index >= count) {
    return 0;
  }

  const int dragged_width = std::max(0, widths[dragged_index]);
  const int clamped_last_visible =
      std::clamp(last_visible_tab_index, -1, count - 1);
  const int last_insertion = clamped_last_visible + 1;
  if (last_insertion <= 0) {
    return 0;
  }

  const int max_left =
      std::max(strip_start_x, strip_end_x - dragged_width);
  const int dragged_left = std::clamp(cursor_x - grab_offset_x,
                                      strip_start_x, max_left);

  std::vector<int> ideal_lefts;
  ideal_lefts.reserve(widths.size());
  int x = strip_start_x;
  for (int i = 0; i < count; ++i) {
    ideal_lefts.push_back(x);
    x += std::max(0, widths[i]) + spacing;
  }

  int best_insertion = 0;
  long long best_distance = std::numeric_limits<long long>::max();
  for (int candidate = 0; candidate <= last_insertion; ++candidate) {
    // Inserting immediately after the dragged tab is the same logical slot as
    // leaving it in place and is not a valid Chromium candidate.
    if (candidate == dragged_index + 1) {
      continue;
    }

    int candidate_left = strip_start_x;
    if (candidate > 0) {
      const int previous = candidate - 1;
      candidate_left = ideal_lefts[previous] +
                       std::max(0, widths[previous]) + spacing;
      if (previous > dragged_index) {
        candidate_left -= dragged_width + spacing;
      }
    }
    const long long distance = std::abs(
        static_cast<long long>(dragged_left) - candidate_left);
    // Chromium walks candidates from left to right and retains the earlier
    // candidate at an exact midpoint tie.
    if (distance < best_distance) {
      best_distance = distance;
      best_insertion = candidate;
    }
  }
  return best_insertion;
}

std::vector<int> ComputeLiveReorderOffsets(const std::vector<int>& widths,
                                           int dragged_index,
                                           int insertion_index,
                                           int spacing) {
  std::vector<int> offsets(widths.size(), 0);
  const int count = static_cast<int>(widths.size());
  if (dragged_index < 0 || dragged_index >= count) {
    return offsets;
  }
  const int clamped_insertion = std::clamp(insertion_index, 0, count);
  const int shift = std::max(0, widths[dragged_index] + spacing);
  if (clamped_insertion <= dragged_index) {
    for (int i = clamped_insertion; i < dragged_index; ++i) {
      offsets[i] = shift;
    }
    return offsets;
  }
  for (int i = dragged_index + 1; i < clamped_insertion; ++i) {
    offsets[i] = -shift;
  }
  return offsets;
}

}  // namespace cmux
