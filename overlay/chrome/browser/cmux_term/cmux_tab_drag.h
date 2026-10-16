// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef CHROME_BROWSER_CMUX_TERM_CMUX_TAB_DRAG_H_
#define CHROME_BROWSER_CMUX_TERM_CMUX_TAB_DRAG_H_

#include <memory>
#include <string>

#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "chrome/browser/cmux_term/window_layout.h"
#include "chrome/browser/cmux_term/window_model.h"
#include "third_party/skia/include/core/SkColor.h"
#include "ui/gfx/geometry/point.h"
#include "ui/gfx/geometry/rect.h"

namespace views {
class View;
class Widget;
}  // namespace views

namespace cmux {

class CmuxTabStrip;

struct DragCommit {
  bool pane_drag = false;
  PaneId source_pane = kInvalidId;
  SurfaceTabId tab = kInvalidId;
  DropTarget target;
  int index = -1;  // tab-strip insertion / merge insertion index
};

class TabDragHost {
 public:
  virtual ~TabDragHost() = default;
  virtual WindowModel& model() = 0;
  virtual WorkspaceId active_workspace() = 0;
  virtual views::Widget* HostWidget() = 0;
  virtual views::View* strip_container() = 0;
  virtual views::View* PaneViewFor(PaneId pane) = 0;
  virtual CmuxTabStrip* TabStripFor(PaneId pane) = 0;
  virtual StripLayout CurrentLayout() = 0;
  virtual int TabStripHeight() = 0;
  virtual void CommitDrop(const DragCommit& commit) = 0;
  virtual void ReflowAfterNoOpDrop() = 0;
};

class CmuxTabDragController {
 public:
  explicit CmuxTabDragController(TabDragHost* host);
  CmuxTabDragController(const CmuxTabDragController&) = delete;
  CmuxTabDragController& operator=(const CmuxTabDragController&) = delete;
  ~CmuxTabDragController();

  bool active() const { return active_; }
  void SetAnimationConfig(bool animations, int animation_ms);
  // The drag overlay lives in a separate Widget, so it must receive the
  // resolved cmux colors explicitly rather than relying on view-tree theme
  // propagation.
  void SetThemeColors(SkColor drop_highlight_color,
                      SkColor ghost_background,
                      SkColor ghost_text);

  void StartTabDrag(PaneId source_pane,
                    SurfaceTabId tab,
                    const std::string& title,
                    SurfaceKind kind,
                    const gfx::Point& screen_pt,
                    const gfx::Point& grab_offset);
  void StartPaneDrag(PaneId source_pane,
                     const gfx::Point& screen_pt,
                     const gfx::Point& grab_offset);
  void Update(const gfx::Point& screen_pt);
  void End(bool commit);
  void Cancel();
  // Host-window deactivation/hide/minimize must not leave the separate native
  // overlay visible above another application while its fade-out runs.
  void CancelImmediately();

  bool has_overlay_for_testing() const { return overlay_widget_ != nullptr; }

 private:
  class HighlightAnimation;
  class OverlayFadeOutAnimation;

  void EnsureOverlays();
  gfx::Point ToOverlay(const gfx::Point& screen) const;
  void ClearFeedback(bool hide_overlay_feedback);
  void DestroyOverlays();
  void FadeOutAndDestroyOverlays();
  DragPayload CurrentPayload() const;
  bool UpdateLiveReorderIfNeeded(const gfx::Point& screen_pt);
  void EndLiveReorder(bool commit);
  void HideHighlightFeedback();

  raw_ptr<TabDragHost> host_;
  bool active_ = false;
  bool pane_drag_ = false;
  PaneId source_pane_ = kInvalidId;
  SurfaceTabId source_tab_ = kInvalidId;
  gfx::Point grab_offset_{24, 14};
  DropTarget target_;
  int target_index_ = -1;
  raw_ptr<CmuxTabStrip> indicator_strip_ = nullptr;
  raw_ptr<CmuxTabStrip> live_reorder_strip_ = nullptr;
  raw_ptr<views::View> overlay_contents_ = nullptr;
  raw_ptr<views::View> ghost_ = nullptr;
  raw_ptr<views::View> highlight_ = nullptr;
  gfx::Rect overlay_bounds_;
  gfx::Rect hidden_highlight_bounds_;
  PaneId highlighted_pane_ = kInvalidId;
  std::unique_ptr<views::Widget> overlay_widget_;
  std::unique_ptr<HighlightAnimation> highlight_animation_;
  std::unique_ptr<OverlayFadeOutAnimation> overlay_fade_animation_;
  bool animations_enabled_ = true;
  int animation_ms_ = 160;
  SkColor drop_highlight_color_ = SkColorSetRGB(0x00, 0x7a, 0xff);
  SkColor ghost_background_ = SkColorSetRGB(0x2b, 0x2f, 0x39);
  SkColor ghost_text_ = SkColorSetRGB(0xf2, 0xf3, 0xf6);
  base::WeakPtrFactory<CmuxTabDragController> weak_factory_{this};
};

}  // namespace cmux

#endif  // CHROME_BROWSER_CMUX_TERM_CMUX_TAB_DRAG_H_
