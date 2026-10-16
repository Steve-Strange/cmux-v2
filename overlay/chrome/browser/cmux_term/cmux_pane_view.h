// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef CHROME_BROWSER_CMUX_TERM_CMUX_PANE_VIEW_H_
#define CHROME_BROWSER_CMUX_TERM_CMUX_PANE_VIEW_H_

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/time/time.h"
#include "chrome/browser/cmux_term/cmux_pane.h"
#include "chrome/browser/cmux_term/cmux_surface.h"
#include "chrome/browser/cmux_term/cmux_tab_strip.h"
#include "chrome/browser/cmux_term/window_model.h"
#include "ui/base/accelerators/accelerator.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/gfx/geometry/point.h"
#include "ui/gfx/image/image_skia.h"
#include "ui/views/view.h"

namespace cmux {

// One pane in the strip: a CmuxTabStrip on top of a stack of surfaces (one
// per SurfaceTab, the selected one visible). Implements CmuxPane so the strip
// treats it like any column content, routing the per-content operations to
// the selected surface. Cross-platform: depends only on //ui/views, the pure
// model, and the CmuxSurface interface -- surface creation happens outside
// (the window controller calls a factory with surface_container() as the
// parent, then hands the interface pointer to BindSurface()).
class CmuxPaneView : public views::View,
                     public CmuxPane,
                     public TabStripDelegate {
  METADATA_HEADER(CmuxPaneView, views::View)

 public:
  // Tab intents + model-affecting notifications, routed to the window
  // controller (which mutates the WindowModel and refreshes this view).
  class Delegate {
   public:
    virtual ~Delegate() = default;
    virtual void OnSelectTabRequested(PaneId pane, SurfaceTabId tab) = 0;
    virtual void OnCloseTabRequested(PaneId pane, SurfaceTabId tab) = 0;
    virtual void OnCloseOtherTabsRequested(PaneId pane, SurfaceTabId tab) = 0;
    virtual void OnNewTabRequested(PaneId pane, SurfaceKind kind) = 0;
    virtual std::optional<ui::Accelerator> GetNewTabAccelerator(
        SurfaceKind kind);
    virtual bool IsSidebarOnRight() const;
    virtual void OnSetSidebarOnRightRequested(bool on_right);
    virtual void OnNewTabRightRequested(PaneId pane, SurfaceTabId tab) = 0;
    virtual void OnMoveTabToNewColumnRequested(PaneId pane,
                                               SurfaceTabId tab) = 0;
    virtual void OnSplitRightWithTabRequested(PaneId pane,
                                              SurfaceTabId tab) = 0;
    virtual void OnSplitDownWithTabRequested(PaneId pane, SurfaceTabId tab) = 0;
    virtual bool IsTabContextActionEnabled(PaneId pane,
                                           SurfaceTabId tab,
                                           TabContextAction action) const = 0;
    virtual bool IsTabContextActionToggled(PaneId pane,
                                           SurfaceTabId tab,
                                           TabContextAction action) const = 0;
    virtual void OnTabContextActionRequested(PaneId pane,
                                             SurfaceTabId tab,
                                             TabContextAction action) = 0;
    virtual void OnTabDragStarted(PaneId pane,
                                  SurfaceTabId tab,
                                  const gfx::Point& screen_pt,
                                  const gfx::Point& grab_offset) = 0;
    virtual void OnTabDragUpdated(const gfx::Point& screen_pt) = 0;
    virtual void OnTabDragEnded(bool commit) = 0;
    virtual bool OnLiveReorderTab(PaneId pane,
                                  SurfaceTabId tab,
                                  int final_index) = 0;
    virtual void OnPaneDragStarted(PaneId pane,
                                   const gfx::Point& screen_pt,
                                   const gfx::Point& grab_offset) = 0;
    virtual void OnPaneDragUpdated(const gfx::Point& screen_pt) = 0;
    virtual void OnPaneDragEnded(bool commit) = 0;
    virtual void OnBeginWindowDrag(const ui::MouseEvent& event) {}
    virtual void OnTabTitleChanged(PaneId pane,
                                   SurfaceTabId tab,
                                   const std::u16string& title) = 0;
    virtual void OnTabFaviconChanged(PaneId pane, SurfaceTabId tab) = 0;
    virtual void OnTabLoadingChanged(PaneId pane,
                                     SurfaceTabId tab,
                                     bool loading) = 0;
    virtual void OnSurfaceInteraction(PaneId pane) = 0;
  };

  CmuxPaneView(PaneId pane_id, Delegate* delegate);
  CmuxPaneView(const CmuxPaneView&) = delete;
  CmuxPaneView& operator=(const CmuxPaneView&) = delete;
  ~CmuxPaneView() override;

  PaneId pane_id() const { return pane_id_; }
  CmuxTabStrip* tab_strip() { return tab_strip_; }

  // The parent view surface factories add surface views under.
  views::View* surface_container() { return surface_container_; }

  // Associate `surface` (whose view is already a child of
  // surface_container()) with `tab`. Wires the surface's activation + title
  // callbacks through this pane.
  void BindSurface(SurfaceTabId tab, CmuxSurface* surface);
  // Destroys the surface's view (and the surface with it).
  void RemoveSurface(SurfaceTabId tab);
  // Detaches a surface view without destroying it so another pane can adopt
  // the same WebContents/terminal/native view.
  std::unique_ptr<views::View> DetachSurfaceView(SurfaceTabId tab,
                                                 CmuxSurface** out);
  void AttachSurface(SurfaceTabId tab,
                     std::unique_ptr<views::View> view,
                     CmuxSurface* surface);

  CmuxSurface* SurfaceFor(SurfaceTabId tab);
  CmuxSurface* selected_surface() { return SurfaceFor(selected_); }
  gfx::ImageSkia selected_favicon() const;

  // Sync the tab strip + surface visibility to the model's tabs for this pane.
  void SetTabs(const std::vector<SurfaceTab>& tabs, SurfaceTabId selected);
  void SetAnimationConfig(bool animations, int animation_ms);
  void SetFocusBorderThickness(int focus_border);
  void SetFocusBorderColor(SkColor color);
  void SetFocusBorderVisible(bool visible);
  void SetThemeColors(const PaneThemeColors& colors);
  // Preference/fullscreen changes toggle the current geometry; layout changes
  // replace it with edge contacts resolved by the window controller.
  void SetRoundedFrame(bool enabled);
  void SetRoundedFrameGeometry(const RoundedFrameGeometry& geometry);

  // CmuxPane:
  views::View* AsView() override;
  void SetFocusedBorder(bool focused) override;
  void FocusContent() override;
  void FocusOmnibar() override;
  void SetActivationCallback(base::RepeatingClosure callback) override;
  std::u16string E2EOmniboxText() override;
  void CloseOmniboxPopupUnlessFocused(views::View* focused) override;
  content::WebContents* GetInspectableWebContents() override;
  void ToggleDevTools() override;
  void UndockDevTools() override;
  bool HandleOmniboxEscape() override;
  void GoBack() override;
  void GoForward() override;
  void Reload() override;

  // TabStripDelegate:
  void OnSelectTab(SurfaceTabId id) override;
  void OnCloseTab(SurfaceTabId id) override;
  void OnCloseOtherTabs(SurfaceTabId id) override;
  void OnNewTab(SurfaceKind kind) override;
  std::optional<ui::Accelerator> GetNewTabAccelerator(
      SurfaceKind kind) override;
  bool IsSidebarOnRight() const override;
  void OnSetSidebarOnRight(bool on_right) override;
  void OnNewTabRight(SurfaceTabId id) override;
  void OnMoveTabToNewColumn(SurfaceTabId id) override;
  void OnSplitRightWithTab(SurfaceTabId id) override;
  void OnSplitDownWithTab(SurfaceTabId id) override;
  bool IsTabContextActionEnabled(SurfaceTabId id,
                                 TabContextAction action) const override;
  bool IsTabContextActionToggled(SurfaceTabId id,
                                 TabContextAction action) const override;
  void OnTabContextAction(SurfaceTabId id, TabContextAction action) override;
  void OnTabDragStarted(SurfaceTabId id,
                        const gfx::Point& screen_pt,
                        const gfx::Point& grab_offset) override;
  void OnTabDragUpdated(const gfx::Point& screen_pt) override;
  void OnTabDragEnded(bool commit) override;
  bool OnLiveReorderTab(SurfaceTabId id, int final_index) override;
  void OnPaneDragStarted(const gfx::Point& screen_pt,
                         const gfx::Point& grab_offset) override;
  void OnPaneDragUpdated(const gfx::Point& screen_pt) override;
  void OnPaneDragEnded(bool commit) override;
  void OnBeginWindowDrag(const ui::MouseEvent& event) override;

  bool OnMousePressed(const ui::MouseEvent& event) override;
  void OnThemeChanged() override;
  void OnPaint(gfx::Canvas* canvas) override;
  void PaintChildren(const views::PaintInfo& paint_info) override;
  void OnPaintBorder(gfx::Canvas* canvas) override;

 private:
  class ActivationEventHandler;
  class FocusBorderAnimation;

  void OnSurfaceActivated();
  void OnSurfaceInteracted();
  void ApplyFocusBorderColor(SkColor color);
  SkColor FocusBorderTargetColor(bool focused) const;
  base::TimeDelta FocusBorderDuration() const;
  bool FocusAnimationsEnabled() const;
  void ApplyToolbarBackground();

  const PaneId pane_id_;
  const raw_ptr<Delegate> delegate_;
  raw_ptr<CmuxTabStrip> tab_strip_ = nullptr;
  raw_ptr<views::View> surface_container_ = nullptr;
  std::map<SurfaceTabId, raw_ptr<CmuxSurface>> surfaces_;
  std::map<SurfaceTabId, gfx::ImageSkia> favicons_;
  std::unique_ptr<ActivationEventHandler> activation_event_handler_;
  SurfaceTabId selected_ = kInvalidId;
  base::RepeatingClosure on_activated_;
  std::unique_ptr<FocusBorderAnimation> focus_border_animation_;
  PaneThemeColors theme_colors_;
  SkColor focus_border_target_color_ = kDefaultPaneFocusedBorder;
  SkColor focus_border_color_ = SK_ColorTRANSPARENT;
  int focus_border_thickness_ = 1;
  bool focus_border_visible_ = false;
  bool focused_ = false;
  RoundedFrameGeometry rounded_frame_geometry_;
  bool animations_ = true;
  int animation_ms_ = 160;
  base::WeakPtrFactory<CmuxPaneView> weak_factory_{this};
};

}  // namespace cmux

#endif  // CHROME_BROWSER_CMUX_TERM_CMUX_PANE_VIEW_H_
