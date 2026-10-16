// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "chrome/browser/cmux_term/cmux_strip_view.h"

#include <algorithm>
#include <utility>
#include <vector>

#include "chrome/browser/cmux_term/window_layout.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/events/event.h"
#include "ui/gfx/canvas.h"
#include "ui/gfx/geometry/point.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/views/border.h"
#include "ui/views/layout/fill_layout.h"

namespace cmux {

namespace {

constexpr int kBorderThickness = 2;

// Walk one column's split tree, collecting every pane (in tree order) keyed
// by id. Mirrors WindowModel::PanesOf but works straight off a Workspace so
// the strip needs no WindowModel handle.
void CollectPanes(const LayoutNode& node, std::map<PaneId, const Pane*>* out) {
  if (node.is_pane()) {
    (*out)[node.pane->id] = node.pane.get();
    return;
  }
  if (node.is_split()) {
    CollectPanes(node.split->first, out);
    CollectPanes(node.split->second, out);
  }
}

}  // namespace

CmuxStripView::CmuxStripView(StripPaneDelegate* delegate)
    : delegate_(delegate) {}

CmuxStripView::~CmuxStripView() = default;

void CmuxStripView::SetWorkspace(const WindowModel* model,
                                 WorkspaceId workspace,
                                 PaneId focused) {
  model_ = model;
  workspace_id_ = workspace;
  focused_ = focused;

  std::map<PaneId, const Pane*> current;
  if (const Workspace* ws = this->workspace()) {
    for (const LayoutNode& column : ws->columns) {
      CollectPanes(column, &current);
    }
  }

  // Drop wrappers for panes that no longer exist.
  for (auto it = panes_.begin(); it != panes_.end();) {
    if (current.count(it->first) == 0) {
      RemoveChildViewT(it->second.get());
      it = panes_.erase(it);
    } else {
      ++it;
    }
  }

  // Create wrappers for newly-seen panes. Each wrapper is a focus-bordered
  // container we own; the delegate's content fills it.
  for (const auto& [id, pane] : current) {
    if (panes_.count(id)) {
      continue;
    }
    auto wrapper = std::make_unique<views::View>();
    wrapper->SetLayoutManager(std::make_unique<views::FillLayout>());
    if (delegate_) {
      wrapper->AddChildView(delegate_->CreateStripPane(id, *pane));
    }
    panes_[id] = AddChildView(std::move(wrapper));
  }

  UpdateFocusBorders();
  InvalidateLayout();
  SchedulePaint();
}

void CmuxStripView::SetThemeColors(const StripViewThemeColors& colors) {
  theme_colors_ = colors;
  UpdateFocusBorders();
  SchedulePaint();
}

void CmuxStripView::Layout(PassKey) {
  ApplyLayout();
}

void CmuxStripView::ApplyLayout() {
  const Workspace* ws = workspace();
  if (!ws) {
    return;
  }
  StripLayout sl =
      ComputeStripLayout(*ws, width(), height(), scroll_x_, focused_);
  scroll_x_ = sl.scroll_x;  // persist the clamped scroll
  for (const PaneBox& b : sl.panes) {
    auto it = panes_.find(b.pane);
    if (it == panes_.end()) {
      continue;
    }
    it->second->SetBoundsRect(gfx::Rect(
        static_cast<int>(b.rect.x), static_cast<int>(b.rect.y),
        static_cast<int>(b.rect.width), static_cast<int>(b.rect.height)));
  }
}

bool CmuxStripView::OnMouseWheel(const ui::MouseWheelEvent& event) {
  // niri strips scroll horizontally. Prefer the trackpad's horizontal delta;
  // fall back to the vertical wheel so a plain mouse can scroll the strip too.
  int delta = event.x_offset() != 0 ? event.x_offset() : event.y_offset();
  if (delta == 0) {
    return false;
  }
  const double before = scroll_x_;
  scroll_x_ -= delta;
  if (scroll_x_ < 0) {
    scroll_x_ = 0;
  }
  ApplyLayout();  // ComputeStripLayout clamps to content extent
  return scroll_x_ != before;
}

bool CmuxStripView::OnMousePressed(const ui::MouseEvent& event) {
  // Click a column to focus it (and scroll it fully into view).
  for (const auto& [id, wrapper] : panes_) {
    if (wrapper->bounds().Contains(event.location())) {
      if (focused_ != id) {
        focused_ = id;
        UpdateFocusBorders();
        ApplyLayout();
      }
      return true;
    }
  }
  return false;
}

void CmuxStripView::OnPaintBackground(gfx::Canvas* canvas) {
  canvas->DrawColor(theme_colors_.bg);
}

void CmuxStripView::UpdateFocusBorders() {
  for (const auto& [id, wrapper] : panes_) {
    wrapper->SetBorder(views::CreateSolidBorder(
        kBorderThickness, id == focused_ ? theme_colors_.focus_border
                                         : theme_colors_.idle_border));
  }
}

const Workspace* CmuxStripView::workspace() const {
  return model_ ? model_->GetWorkspace(workspace_id_) : nullptr;
}

BEGIN_METADATA(CmuxStripView)
END_METADATA

}  // namespace cmux
