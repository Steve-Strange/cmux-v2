// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "chrome/browser/cmux_term/cmux_focus.h"

#include "base/functional/bind.h"
#include "base/location.h"
#include "base/task/single_thread_task_runner.h"
#include "chrome/browser/cmux_term/cmux_pane.h"

namespace cmux {

FocusController::FocusController(FocusHost* host) : host_(host) {}
FocusController::~FocusController() = default;

void FocusController::FocusPane(PaneId pane, bool move_keyboard) {
  const WorkspaceId ws = host_->active_workspace();
  WindowModel& model = host_->model();
  if (!model.FindPane(ws, pane)) {
    return;
  }
  model.FocusPane(ws, pane);
  host_->ApplyVisualsAndScroll(
      /*animated=*/true, ScrollIntoViewPolicy::kEnsureVisibleWithMargin);
  if (!move_keyboard) {
    return;
  }
  // Defer the physical focus grab so pane visibility/attachment from this same
  // turn are committed first, and so only the latest request wins (a rapid
  // workspace switch bumps the generation and drops the stale grab).
  const uint64_t generation = ++focus_generation_;
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE,
      base::BindOnce(&FocusController::DeferredFocus,
                     weak_factory_.GetWeakPtr(), ws, pane, generation));
}

void FocusController::DeferredFocus(WorkspaceId ws,
                                    PaneId pane,
                                    uint64_t generation) {
  if (generation != focus_generation_) {
    return;  // superseded by a later FocusPane()
  }
  if (host_->active_workspace() != ws) {
    return;  // workspace changed out from under us
  }
  const Workspace* w = host_->model().GetWorkspace(ws);
  if (!w || w->focused != pane) {
    return;  // focus moved on since we were posted
  }
  if (CmuxPane* view = host_->PaneView(pane)) {
    view->FocusContent();
  }
}

void FocusController::OnPaneActivated(PaneId pane) {
  const WorkspaceId ws = host_->active_workspace();
  WindowModel& model = host_->model();
  // Stray-activation guard: a hidden / other-workspace terminal can fire
  // becomeFirstResponder; ignore anything not in the active workspace.
  if (!model.FindPane(ws, pane)) {
    return;
  }
  const Workspace* w = model.GetWorkspace(ws);
  if (w && w->focused == pane) {
    return;  // already the focused column; idempotent
  }
  model.FocusPane(ws, pane);
  // Visuals + scroll only -- the content already holds the keyboard, and we may
  // be inside a focus/first-responder notification, so we must NOT re-enter the
  // FocusManager by moving focus here.
  host_->ApplyVisualsAndScroll(/*animated=*/true,
                               ScrollIntoViewPolicy::kEnsureVisible);
}

void FocusController::OnWorkspaceActivated() {
  const WorkspaceId ws = host_->active_workspace();
  WindowModel& model = host_->model();
  const Workspace* w = model.GetWorkspace(ws);
  if (!w) {
    return;
  }
  PaneId focused = w->focused;
  // Fall back to the first pane that actually has a native view if the recorded
  // focus is stale or view-less.
  if (!model.FindPane(ws, focused) || !host_->PaneView(focused)) {
    focused = kInvalidId;
    for (PaneId p : model.PanesOf(ws)) {
      if (host_->PaneView(p)) {
        focused = p;
        break;
      }
    }
  }
  if (focused == kInvalidId) {
    host_->ApplyVisualsAndScroll(
        /*animated=*/false,
        ScrollIntoViewPolicy::kEnsureVisibleWithMargin);  // nothing to focus
                                                          // yet
    return;
  }
  FocusPane(focused, /*move_keyboard=*/true);
}

void FocusController::OnPaneAdded(PaneId pane) {
  FocusPane(pane, /*move_keyboard=*/true);
}

void FocusController::OnPaneRemoved() {
  // WindowModel::ClosePane already moved focus to a surviving sibling.
  const Workspace* w = host_->model().GetWorkspace(host_->active_workspace());
  if (w && w->focused != kInvalidId) {
    FocusPane(w->focused, /*move_keyboard=*/true);
  } else {
    host_->ApplyVisualsAndScroll(
        /*animated=*/true, ScrollIntoViewPolicy::kEnsureVisibleWithMargin);
  }
}

}  // namespace cmux
