// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef CHROME_BROWSER_CMUX_TERM_CMUX_FOCUS_H_
#define CHROME_BROWSER_CMUX_TERM_CMUX_FOCUS_H_

#include <cstdint>

#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "chrome/browser/cmux_term/window_layout.h"
#include "chrome/browser/cmux_term/window_model.h"

namespace cmux {

class CmuxPane;

// What the FocusController needs from the strip renderer (CmuxStripView). Kept
// tiny so the controller stays the single focus authority while the host owns
// the views/geometry. The host provides the active workspace, the model, the
// PaneId -> native view mapping, and a way to re-apply borders + the niri
// scroll-into-view for the model's current focused pane.
class FocusHost {
 public:
  virtual ~FocusHost() = default;
  virtual WorkspaceId active_workspace() = 0;
  virtual WindowModel& model() = 0;
  // The native pane view for `id`, or nullptr if it has no view yet (e.g. a
  // seeded workspace not shown yet, or a pane in a different workspace).
  virtual CmuxPane* PaneView(PaneId id) = 0;
  // Repaint focus borders for the model's focused pane and run the niri
  // focus-into-view scroll (per-workspace), laying panes out (animated or not).
  virtual void ApplyVisualsAndScroll(bool animated,
                                     ScrollIntoViewPolicy policy) = 0;
};

// The single focus authority. Every action that can change focus routes through
// exactly one of these entry points, so the rest-state invariant always holds:
// for the active workspace W with focused pane P --
//   model:    W.focused == P
//   visuals:  P has the focus border, others idle
//   scroll:   W's scroll keeps P fully on-screen
//   physical: P's content is the keyboard target (Views focus + AppKit FR)
//
// Mutations (FocusPane / OnWorkspaceActivated / OnPaneAdded / OnPaneRemoved)
// may move the keyboard; observations (OnPaneActivated) only reconcile visuals
// + scroll, because the content already took native focus -- this avoids
// re-entering the FocusManager from inside a focus/first-responder
// notification.
class FocusController {
 public:
  explicit FocusController(FocusHost* host);
  FocusController(const FocusController&) = delete;
  FocusController& operator=(const FocusController&) = delete;
  ~FocusController();

  // THE single focus mutation. No-op if `pane` isn't in the active workspace.
  // When `move_keyboard`, the pane's content is focused on a fresh task tagged
  // with a generation token, so a later request supersedes a stale one and the
  // grab happens after visibility/attachment settle.
  void FocusPane(PaneId pane, bool move_keyboard);

  // A pane's content took native focus (terminal becomeFirstResponder, browser
  // web-contents-focused, omnibox focus). Reconciles model + visuals + scroll
  // only. Ignored if `pane` isn't in the active workspace (stray-activation
  // guard) or is already focused.
  void OnPaneActivated(PaneId pane);

  // The active workspace changed: validate its focused pane (falling back to
  // the first pane that has a view) and land the keyboard on it.
  void OnWorkspaceActivated();

  // Structural changes within the active workspace.
  void OnPaneAdded(PaneId pane);  // focus the new column (keyboard)
  void OnPaneRemoved();           // model already moved focus to a sibling

 private:
  // Runs (deferred) the physical keyboard focus for FocusPane(move_keyboard).
  void DeferredFocus(WorkspaceId ws, PaneId pane, uint64_t generation);

  const raw_ptr<FocusHost> host_;
  uint64_t focus_generation_ = 0;
  base::WeakPtrFactory<FocusController> weak_factory_{this};
};

}  // namespace cmux

#endif  // CHROME_BROWSER_CMUX_TERM_CMUX_FOCUS_H_
