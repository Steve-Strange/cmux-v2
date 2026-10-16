// Copyright 2022 The Chromium Authors
// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later AND BSD-3-Clause
//
// Contains Chromium-derived regions; see docs/source-provenance.md.

#ifndef CHROME_BROWSER_CMUX_TERM_CMUX_TAB_STRIP_H_
#define CHROME_BROWSER_CMUX_TERM_CMUX_TAB_STRIP_H_

#include <memory>
#include <optional>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "chrome/browser/cmux_term/window_model.h"
#include "third_party/skia/include/core/SkColor.h"
#include "ui/base/accelerators/accelerator.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/gfx/geometry/point.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/image/image_skia.h"
#include "ui/views/animation/bounds_animator_observer.h"
#include "ui/views/view.h"

namespace gfx {
class Canvas;
}

namespace ui {
class MouseEvent;
}

namespace views {
class BoundsAnimator;
}

// Cross-platform per-pane tab strip. A pure views::View — it depends only on
// //ui/views + the pure cmux WindowModel, NOT on //chrome/browser, AppKit, or
// any platform surface. It renders one Pane's stack of SurfaceTabs (one
// selected) as a horizontal row of tabs plus a "new tab" affordance, and routes
// user intent (select / close / new) back through a delegate. The cmux renderer
// and the standalone cmux_ui_demo harness both embed this same class, so it
// builds and looks identical on macOS / Windows / Linux.

namespace cmux {

struct TabVisual {
  SurfaceTab tab;
  gfx::ImageSkia favicon;
};

inline constexpr SkColor kDefaultTabActiveBg = SkColorSetRGB(0x2b, 0x2f, 0x39);
inline constexpr SkColor kDefaultTabHoverBg = SkColorSetRGB(0x20, 0x23, 0x2a);
inline constexpr SkColor kDefaultTabActiveText =
    SkColorSetRGB(0xf2, 0xf3, 0xf6);
inline constexpr SkColor kDefaultTabIdleText = SkColorSetRGB(0x97, 0x9d, 0xad);
inline constexpr SkColor kDefaultTabPlusText = SkColorSetRGB(0xb8, 0xbd, 0xcb);
inline constexpr SkColor kDefaultTabAccent = SkColorSetRGB(0x8a, 0x8f, 0x98);
inline constexpr SkColor kDefaultTabStripBg = SkColorSetRGB(0x17, 0x19, 0x1f);

struct TabStripThemeColors {
  SkColor strip_background = kDefaultTabStripBg;
  SkColor tab_active_bg = kDefaultTabActiveBg;
  SkColor tab_hover_bg = kDefaultTabHoverBg;
  SkColor tab_active_text = kDefaultTabActiveText;
  SkColor tab_idle_text = kDefaultTabIdleText;
  SkColor plus_text = kDefaultTabPlusText;
  SkColor accent = kDefaultTabAccent;
};

// Semantic commands exposed by the tab context menu. The strip keeps these
// Chromium-free; CmuxWindowView maps web-tab actions to TabStripModel's real
// context-menu commands and handles mixed web/terminal actions in our model.
enum class TabContextAction {
  kNewTabRight = 1,
  kNewSplitWithCurrentTab,
  kAddTabToNewGroup,
  kMoveTabToNewWindow,
  kCopyUrl,
  kReload,
  kDuplicate,
  kTogglePinned,
  kToggleSiteMuted,
  kHibernate,
  kClose,
  kCloseOtherTabs,
  kCloseTabsToLeft,
  kCloseTabsToRight,
};

class CmuxTabContextMenu;
class CmuxTabStripContextMenu;
class CmuxNewTabMenu;

// Intent callbacks. The owner (renderer or demo) mutates the WindowModel and
// calls SetTabs() again to reflect the new state. During an originated drag the
// strip defers SetTabs() so the captured pill is not destroyed mid-drag.
class TabStripDelegate {
 public:
  virtual ~TabStripDelegate() = default;
  virtual void OnSelectTab(SurfaceTabId id) = 0;
  virtual void OnCloseTab(SurfaceTabId id) = 0;
  virtual void OnCloseOtherTabs(SurfaceTabId id) = 0;
  virtual void OnNewTab(SurfaceKind kind) = 0;
  virtual std::optional<ui::Accelerator> GetNewTabAccelerator(SurfaceKind kind);
  virtual bool IsSidebarOnRight() const;
  virtual void OnSetSidebarOnRight(bool on_right);
  virtual void OnNewTabRight(SurfaceTabId id) = 0;
  virtual void OnMoveTabToNewColumn(SurfaceTabId id) = 0;
  virtual void OnSplitRightWithTab(SurfaceTabId id) = 0;
  virtual void OnSplitDownWithTab(SurfaceTabId id) = 0;
  virtual bool IsTabContextActionEnabled(SurfaceTabId id,
                                         TabContextAction action) const = 0;
  virtual bool IsTabContextActionToggled(SurfaceTabId id,
                                         TabContextAction action) const = 0;
  virtual void OnTabContextAction(SurfaceTabId id, TabContextAction action) = 0;
  virtual void OnTabDragStarted(SurfaceTabId id,
                                const gfx::Point& screen_pt,
                                const gfx::Point& grab_offset) = 0;
  virtual void OnTabDragUpdated(const gfx::Point& screen_pt) = 0;
  virtual void OnTabDragEnded(bool commit) = 0;
  // Mirrors Chromium's attached drag session: update the backing model as
  // soon as hysteresis accepts a new same-strip destination. Returns true
  // only when the tab's model index actually changed.
  virtual bool OnLiveReorderTab(SurfaceTabId id, int final_index) = 0;
  virtual void OnPaneDragStarted(const gfx::Point& screen_pt,
                                 const gfx::Point& grab_offset) = 0;
  virtual void OnPaneDragUpdated(const gfx::Point& screen_pt) = 0;
  virtual void OnPaneDragEnded(bool commit) = 0;
  // Begin the platform's native window move loop from otherwise empty tab-strip
  // chrome. Default no-op keeps the standalone UI harness cross-platform.
  virtual void OnBeginWindowDrag(const ui::MouseEvent& event) {}
};

class CmuxTabStrip : public views::View, public views::BoundsAnimatorObserver {
  METADATA_HEADER(CmuxTabStrip, views::View)

 public:
  explicit CmuxTabStrip(TabStripDelegate* delegate);
  CmuxTabStrip(const CmuxTabStrip&) = delete;
  CmuxTabStrip& operator=(const CmuxTabStrip&) = delete;
  ~CmuxTabStrip() override;

  // Rebuild the row from a pane's tabs + which one is selected. Cheap to call
  // on every model change; it diffs nothing and just re-lays-out a handful of
  // tabs.
  void SetTabs(const std::vector<TabVisual>& tabs, SurfaceTabId selected);
  int InsertionIndexAt(const gfx::Point& strip_local) const;
  void ShowInsertionIndicator(int index);
  void ClearInsertionIndicator();
  void SelectTab(SurfaceTabId id);
  void CloseTab(SurfaceTabId id);
  void CloseOtherTabs(SurfaceTabId id);
  void NewTab(SurfaceKind kind);
  void ShowNewTabMenu();
  std::optional<ui::Accelerator> GetNewTabAccelerator(SurfaceKind kind);
  bool IsSidebarOnRight() const;
  void SetSidebarOnRight(bool on_right);
  void ShowTabStripContextMenu(const gfx::Point& screen_pt);
  void NewTabRight(SurfaceTabId id);
  void MoveTabToNewColumn(SurfaceTabId id);
  void SplitRightWithTab(SurfaceTabId id);
  void SplitDownWithTab(SurfaceTabId id);
  bool IsTabContextActionEnabled(SurfaceTabId id,
                                 TabContextAction action) const;
  bool IsTabContextActionToggled(SurfaceTabId id,
                                 TabContextAction action) const;
  void ShowContextMenuForTab(SurfaceTabId id, const gfx::Point& screen_pt);
  void BeginTabDrag(SurfaceTabId id,
                    const gfx::Point& screen_pt,
                    const gfx::Point& grab_offset);
  void BeginPaneDrag(const gfx::Point& screen_pt,
                     const gfx::Point& grab_offset);
  void BeginWindowDrag(const ui::MouseEvent& event);
  void UpdateDrag(const gfx::Point& screen_pt);
  void EndDrag(bool commit);
  void CancelDragStateForExternalEnd();
  // Reserves otherwise-empty chrome before the first tab. The window uses this
  // when macOS traffic lights overlap a pane's top-left corner.
  void SetLeadingInset(int inset);
  int leading_inset_for_testing() const { return leading_inset_; }
  void SetAnimationConfig(bool animations, int animation_ms);
  void SetAccentColor(SkColor accent_color);
  void SetThemeColors(const TabStripThemeColors& colors);
  SkColor accent_color() const { return accent_color_; }
  const TabStripThemeColors& theme_colors() const { return theme_colors_; }
  bool UpdateLiveReorder(SurfaceTabId id,
                         const gfx::Point& strip_local,
                         const gfx::Point& grab_offset);
  void EndLiveReorder(bool commit);
  int live_reorder_index() const { return live_reorder_index_; }

  gfx::Size CalculatePreferredSize(const views::SizeBounds&) const override;
  void Layout(PassKey) override;
  void OnThemeChanged() override;
  void OnPaint(gfx::Canvas* canvas) override;
  bool OnMousePressed(const ui::MouseEvent& event) override;

  // views::BoundsAnimatorObserver:
  void OnBoundsAnimatorProgressed(views::BoundsAnimator* animator) override;
  void OnBoundsAnimatorDone(views::BoundsAnimator* animator) override;

 private:
  class InsertionGapAnimation;
  class LiveReorderAnimation;

  void Rebuild();
  void SyncTabs(const std::vector<TabVisual>& tabs, SurfaceTabId selected);
  void ApplyPendingTabs();
  void AnimateInsertionGap(int index, bool opening);
  void StopInsertionGapAnimations();
  void StopLiveReorderAnimations();
  void StopTabMutationAnimations();
  void ClearTabOpeningReveals();
  void UpdateIdealBounds();
  void SnapToIdealBounds();
  void AnimateToIdealBounds();
  void StartRemoveTabAnimation(views::View* tab, int target_x);
  void OnCloseTabAnimationCompleted(views::View* tab);
  void OrderChildrenForTabs();
  int LiveReorderInsertionIndexAt(const gfx::Point& strip_local,
                                  const gfx::Point& grab_offset) const;
  void ApplyLiveReorder(const gfx::Point& strip_local,
                        const gfx::Point& grab_offset);
  void ResetLiveReorderTransforms(bool animate);
  void SetTabTranslate(views::View* tab, int dx, bool animate);
  LiveReorderAnimation* LiveAnimationFor(views::View* tab);
  void ExecuteContextMenuCommand(SurfaceTabId id, int command_id);
  void UpdateTabWidths();
  void PaintTabSeparators(gfx::Canvas* canvas) const;
  bool ShouldPaintSeparator(int left_index) const;
  void ApplyToolbarBackground();

  friend class CmuxTabContextMenu;
  friend class CmuxTabStripContextMenu;

  void PostSelectTab(SurfaceTabId id);
  void PostNewTab(SurfaceKind kind);
  void PostNewTabRight(SurfaceTabId id);

  raw_ptr<TabStripDelegate> delegate_;
  std::vector<TabVisual> tabs_;
  std::vector<TabVisual> pending_tabs_;
  std::vector<raw_ptr<views::View>> tab_views_;
  // Closing tabs leave the logical model immediately, as in Chromium, but
  // remain children until their bounds animation completes.
  std::vector<raw_ptr<views::View>> closing_tab_views_;
  std::vector<gfx::Rect> ideal_tab_bounds_;
  gfx::Rect ideal_insertion_spacer_bounds_;
  gfx::Rect ideal_new_tab_bounds_;
  gfx::Rect ideal_pane_drag_handle_bounds_;
  SurfaceTabId selected_ = kInvalidId;
  SurfaceTabId pending_selected_ = kInvalidId;
  bool has_pending_tabs_ = false;
  bool drag_origin_active_ = false;
  bool pane_drag_active_ = false;
  bool live_reorder_active_ = false;
  int leading_inset_ = 0;
  bool animations_ = true;
  int animation_ms_ = 160;
  TabStripThemeColors theme_colors_;
  SkColor accent_color_ = kDefaultTabAccent;
  int insertion_indicator_index_ = -1;
  int drag_origin_tab_index_ = -1;
  int live_reorder_dragged_index_ = -1;
  int live_reorder_index_ = -1;
  std::optional<int> live_reorder_last_move_x_;
  gfx::Point live_reorder_last_local_;
  gfx::Point live_reorder_grab_offset_;
  raw_ptr<views::View> insertion_spacer_ = nullptr;
  raw_ptr<views::View> new_tab_button_ = nullptr;
  raw_ptr<views::View> pane_drag_handle_ = nullptr;
  std::unique_ptr<CmuxTabContextMenu> context_menu_;
  std::unique_ptr<CmuxTabStripContextMenu> tab_strip_context_menu_;
  std::unique_ptr<CmuxNewTabMenu> new_tab_menu_;
  std::vector<std::unique_ptr<InsertionGapAnimation>> gap_animations_;
  std::vector<std::unique_ptr<LiveReorderAnimation>> live_reorder_animations_;
  std::unique_ptr<views::BoundsAnimator> tab_bounds_animator_;
  base::WeakPtrFactory<CmuxTabStrip> weak_factory_{this};
};

}  // namespace cmux

#endif  // CHROME_BROWSER_CMUX_TERM_CMUX_TAB_STRIP_H_
