// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "chrome/browser/cmux_term/cmux_pane_view.h"

#include <algorithm>
#include <memory>
#include <utility>

#include "base/functional/bind.h"
#include "base/memory/scoped_refptr.h"
#include "base/task/single_thread_task_runner.h"
#include "cc/paint/paint_flags.h"
#include "chrome/browser/cmux_term/cmux_easing.h"
#include "chrome/browser/cmux_term/cmux_surface.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/compositor/paint_recorder.h"
#include "ui/events/event.h"
#include "ui/events/event_handler.h"
#include "ui/gfx/animation/slide_animation.h"
#include "ui/gfx/animation/tween.h"
#include "ui/gfx/canvas.h"
#include "ui/gfx/geometry/insets.h"
#include "ui/gfx/geometry/rect_f.h"
#include "ui/views/animation/animation_delegate_views.h"
#include "ui/views/background.h"
#include "ui/views/border.h"
#include "ui/views/layout/box_layout.h"
#include "ui/views/layout/fill_layout.h"

namespace cmux {

namespace {

gfx::Insets FocusBorderLayoutInsets(int thickness) {
  // Helium's classic tab strip begins at the window-content top. Paint the
  // pane's top focus edge over the tab strip's three-DIP empty top band instead
  // of reserving layout space for it; the other edges continue to protect pane
  // content from the focus stroke. Custom borders thicker than that band keep
  // only their excess in layout so they never cover the tab pill.
  constexpr int kTabTopEmptyBand = 3;
  return gfx::Insets::TLBR(std::max(0, thickness - kTabTopEmptyBand), thickness,
                           thickness, thickness);
}

}  // namespace

std::optional<ui::Accelerator> CmuxPaneView::Delegate::GetNewTabAccelerator(
    SurfaceKind) {
  return std::nullopt;
}

bool CmuxPaneView::Delegate::IsSidebarOnRight() const {
  return false;
}

void CmuxPaneView::Delegate::OnSetSidebarOnRightRequested(bool) {}

class CmuxPaneView::ActivationEventHandler : public ui::EventHandler {
 public:
  explicit ActivationEventHandler(CmuxPaneView* pane) : pane_(pane) {}
  ActivationEventHandler(const ActivationEventHandler&) = delete;
  ActivationEventHandler& operator=(const ActivationEventHandler&) = delete;
  ~ActivationEventHandler() override = default;

  void OnMouseEvent(ui::MouseEvent* event) override {
    if (event->type() == ui::EventType::kMousePressed && pane_) {
      pane_->OnSurfaceActivated();
    }
  }

 private:
  raw_ptr<CmuxPaneView> pane_;
};

class CmuxPaneView::FocusBorderAnimation
    : public views::AnimationDelegateViews {
 public:
  explicit FocusBorderAnimation(CmuxPaneView* pane)
      : views::AnimationDelegateViews(pane), pane_(pane), animation_(this) {}
  FocusBorderAnimation(const FocusBorderAnimation&) = delete;
  FocusBorderAnimation& operator=(const FocusBorderAnimation&) = delete;
  ~FocusBorderAnimation() override { animation_.Stop(); }

  void AnimateTo(SkColor target, base::TimeDelta duration) {
    if (!pane_) {
      return;
    }
    if (animation_.is_animating()) {
      Apply(animation_.GetCurrentValue());
    }
    animation_.Stop();
    start_ = pane_->focus_border_color_;
    target_ = target;
    if (duration.is_zero() || !pane_->GetWidget()) {
      pane_->ApplyFocusBorderColor(target_);
      return;
    }
    animation_.Reset(0.0);
    animation_.SetTweenType(gfx::Tween::LINEAR);
    animation_.SetSlideDuration(duration);
    animation_.Show();
  }

  void Stop() { animation_.Stop(); }

  void AnimationProgressed(const gfx::Animation* animation) override {
    if (animation == &animation_) {
      Apply(animation_.GetCurrentValue());
    }
  }

  void AnimationEnded(const gfx::Animation* animation) override {
    if (animation == &animation_ && pane_) {
      pane_->ApplyFocusBorderColor(target_);
    }
  }

 private:
  void Apply(double value) {
    if (!pane_) {
      return;
    }
    pane_->ApplyFocusBorderColor(
        gfx::Tween::ColorValueBetween(EaseOutExpo(value), start_, target_));
  }

  raw_ptr<CmuxPaneView> pane_;
  gfx::SlideAnimation animation_;
  SkColor start_ = kDefaultPaneIdleBorder;
  SkColor target_ = kDefaultPaneIdleBorder;
};

CmuxPaneView::CmuxPaneView(PaneId pane_id, Delegate* delegate)
    : pane_id_(pane_id), delegate_(delegate) {
  SetBorder(views::CreateEmptyBorder(0));
  ApplyFocusBorderColor(SK_ColorTRANSPARENT);
  auto* box = SetLayoutManager(std::make_unique<views::BoxLayout>(
      views::BoxLayout::Orientation::kVertical));
  box->set_cross_axis_alignment(views::BoxLayout::CrossAxisAlignment::kStretch);

  tab_strip_ = AddChildView(std::make_unique<CmuxTabStrip>(this));
  box->SetFlexForView(tab_strip_, 0);

  surface_container_ = AddChildView(std::make_unique<views::View>());
  surface_container_->SetLayoutManager(std::make_unique<views::FillLayout>());
  box->SetFlexForView(surface_container_, 1);

  // Parent OnMousePressed only runs after children decline the event. The page
  // WebView, omnibox, tab strip, and terminal usually consume their presses, so
  // this pane-local pre-target observer sees mouse-downs anywhere in the pane's
  // Views subtree first and only reports activation. It does not mark the event
  // handled and it does not move keyboard focus;
  // FocusController::OnPaneActivated applies visuals/scroll only, avoiding
  // FocusManager re-entry.
  activation_event_handler_ = std::make_unique<ActivationEventHandler>(this);
  AddPreTargetHandler(activation_event_handler_.get());
}

CmuxPaneView::~CmuxPaneView() {
  if (activation_event_handler_) {
    RemovePreTargetHandler(activation_event_handler_.get());
  }
}

void CmuxPaneView::BindSurface(SurfaceTabId tab, CmuxSurface* surface) {
  if (!surface) {
    return;
  }
  surfaces_[tab] = surface;
  // Any surface activation (a click on its content, native first responder,
  // omnibox focus) counts as activating this pane; forward through the
  // pane-level callback the strip installed.
  surface->SetActivationCallback(base::BindRepeating(
      &CmuxPaneView::OnSurfaceActivated, weak_factory_.GetWeakPtr()));
  surface->SetInteractionCallback(base::BindRepeating(
      &CmuxPaneView::OnSurfaceInteracted, weak_factory_.GetWeakPtr()));
  surface->SetTitleChangedCallback(base::BindRepeating(
      [](base::WeakPtr<CmuxPaneView> self, SurfaceTabId tab,
         const std::u16string& title) {
        if (self && self->delegate_) {
          self->delegate_->OnTabTitleChanged(self->pane_id_, tab, title);
        }
      },
      weak_factory_.GetWeakPtr(), tab));
  surface->SetFaviconChangedCallback(base::BindRepeating(
      [](base::WeakPtr<CmuxPaneView> self, SurfaceTabId tab,
         const gfx::ImageSkia& favicon) {
        if (!self) {
          return;
        }
        if (favicon.isNull()) {
          self->favicons_.erase(tab);
        } else {
          self->favicons_[tab] = favicon;
        }
        if (self->delegate_) {
          self->delegate_->OnTabFaviconChanged(self->pane_id_, tab);
        }
      },
      weak_factory_.GetWeakPtr(), tab));
  surface->SetLoadingChangedCallback(base::BindRepeating(
      [](base::WeakPtr<CmuxPaneView> self, SurfaceTabId tab, bool loading) {
        if (self && self->delegate_) {
          self->delegate_->OnTabLoadingChanged(self->pane_id_, tab, loading);
        }
      },
      weak_factory_.GetWeakPtr(), tab));
  scoped_refptr<base::SingleThreadTaskRunner> ui_runner =
      base::SingleThreadTaskRunner::GetCurrentDefault();
  surface->SetCloseRequestedCallback(base::BindRepeating(
      [](base::WeakPtr<CmuxPaneView> self,
         scoped_refptr<base::SingleThreadTaskRunner> ui_runner,
         SurfaceTabId tab) {
        ui_runner->PostTask(
            FROM_HERE,
            base::BindOnce(
                [](base::WeakPtr<CmuxPaneView> self, SurfaceTabId tab) {
                  if (self && self->delegate_) {
                    self->delegate_->OnCloseTabRequested(self->pane_id_, tab);
                  }
                },
                self, tab));
      },
      weak_factory_.GetWeakPtr(), std::move(ui_runner), tab));
  surface->SetRoundedFrame(rounded_frame_geometry_);
  // Hidden until SetTabs selects it.
  surface->AsView()->SetVisible(false);
}

void CmuxPaneView::RemoveSurface(SurfaceTabId tab) {
  auto it = surfaces_.find(tab);
  if (it == surfaces_.end()) {
    return;
  }
  views::View* view = it->second->AsView();
  it->second->SetActivationCallback(base::RepeatingClosure());
  it->second->SetInteractionCallback(base::RepeatingClosure());
  it->second->SetTitleChangedCallback(
      base::RepeatingCallback<void(const std::u16string&)>());
  it->second->SetFaviconChangedCallback(
      base::RepeatingCallback<void(const gfx::ImageSkia&)>());
  it->second->SetLoadingChangedCallback(base::RepeatingCallback<void(bool)>());
  it->second->SetCloseRequestedCallback(base::RepeatingClosure());
  surfaces_.erase(it);
  favicons_.erase(tab);
  if (selected_ == tab) {
    selected_ = kInvalidId;
  }
  surface_container_->RemoveChildViewT(view);
}

std::unique_ptr<views::View> CmuxPaneView::DetachSurfaceView(
    SurfaceTabId tab,
    CmuxSurface** out) {
  if (out) {
    *out = nullptr;
  }
  auto it = surfaces_.find(tab);
  if (it == surfaces_.end()) {
    return nullptr;
  }
  CmuxSurface* surface = it->second;
  views::View* view = surface->AsView();
  surface->SetActivationCallback(base::RepeatingClosure());
  surface->SetInteractionCallback(base::RepeatingClosure());
  surface->SetTitleChangedCallback(
      base::RepeatingCallback<void(const std::u16string&)>());
  surface->SetFaviconChangedCallback(
      base::RepeatingCallback<void(const gfx::ImageSkia&)>());
  surface->SetLoadingChangedCallback(base::RepeatingCallback<void(bool)>());
  surface->SetCloseRequestedCallback(base::RepeatingClosure());
  surfaces_.erase(it);
  favicons_.erase(tab);
  if (selected_ == tab) {
    selected_ = kInvalidId;
  }
  if (out) {
    *out = surface;
  }
  return surface_container_->RemoveChildViewT(view);
}

void CmuxPaneView::AttachSurface(SurfaceTabId tab,
                                 std::unique_ptr<views::View> view,
                                 CmuxSurface* surface) {
  if (!view || !surface) {
    return;
  }
  // Native terminal surfaces are reparented within the same widget; the
  // NativeViewHost keeps the platform view attached while ownership of the
  // Views wrapper moves between pane containers.
  surface_container_->AddChildView(std::move(view));
  BindSurface(tab, surface);
}

CmuxSurface* CmuxPaneView::SurfaceFor(SurfaceTabId tab) {
  auto it = surfaces_.find(tab);
  return it == surfaces_.end() ? nullptr : it->second;
}

gfx::ImageSkia CmuxPaneView::selected_favicon() const {
  auto it = favicons_.find(selected_);
  return it == favicons_.end() ? gfx::ImageSkia() : it->second;
}

void CmuxPaneView::SetTabs(const std::vector<SurfaceTab>& tabs,
                           SurfaceTabId selected) {
  selected_ = selected;
  if (tab_strip_) {
    std::vector<TabVisual> visuals;
    visuals.reserve(tabs.size());
    for (const SurfaceTab& tab : tabs) {
      TabVisual visual;
      visual.tab = tab;
      auto it = favicons_.find(tab.id);
      if (it != favicons_.end()) {
        visual.favicon = it->second;
      }
      visuals.push_back(std::move(visual));
    }
    tab_strip_->SetTabs(visuals, selected);
  }
  for (auto& [tab, surface] : surfaces_) {
    surface->AsView()->SetVisible(tab == selected);
  }
  InvalidateLayout();
}

void CmuxPaneView::SetAnimationConfig(bool animations, int animation_ms) {
  animations_ = animations;
  animation_ms_ = std::max(0, animation_ms);
  if (!FocusAnimationsEnabled()) {
    if (focus_border_animation_) {
      focus_border_animation_->Stop();
    }
    ApplyFocusBorderColor(FocusBorderTargetColor(focused_));
  }
  if (tab_strip_) {
    tab_strip_->SetAnimationConfig(animations, animation_ms);
  }
}

void CmuxPaneView::SetRoundedFrame(bool enabled) {
  RoundedFrameGeometry geometry = rounded_frame_geometry_;
  geometry.enabled = enabled;
  SetRoundedFrameGeometry(geometry);
}

void CmuxPaneView::SetRoundedFrameGeometry(
    const RoundedFrameGeometry& geometry) {
  if (rounded_frame_geometry_ == geometry) {
    return;
  }
  rounded_frame_geometry_ = geometry;
  for (auto& entry : surfaces_) {
    entry.second->SetRoundedFrame(geometry);
  }
}

void CmuxPaneView::SetFocusBorderThickness(int focus_border) {
  const int thickness = std::max(0, focus_border);
  if (focus_border_thickness_ == thickness) {
    return;
  }
  focus_border_thickness_ = thickness;
  SetBorder(views::CreateEmptyBorder(
      focus_border_visible_ ? FocusBorderLayoutInsets(focus_border_thickness_)
                            : gfx::Insets()));
  if (focus_border_animation_) {
    focus_border_animation_->Stop();
  }
  ApplyFocusBorderColor(FocusBorderTargetColor(focused_));
}

void CmuxPaneView::SetFocusBorderColor(SkColor color) {
  if (focus_border_target_color_ == color) {
    return;
  }
  focus_border_target_color_ = color;
  if (focus_border_animation_) {
    focus_border_animation_->Stop();
  }
  ApplyFocusBorderColor(FocusBorderTargetColor(focused_));
}

void CmuxPaneView::SetFocusBorderVisible(bool visible) {
  if (focus_border_visible_ == visible) {
    return;
  }
  focus_border_visible_ = visible;
  SetBorder(views::CreateEmptyBorder(
      focus_border_visible_ ? FocusBorderLayoutInsets(focus_border_thickness_)
                            : gfx::Insets()));
  if (focus_border_animation_) {
    focus_border_animation_->Stop();
  }
  ApplyFocusBorderColor(FocusBorderTargetColor(focused_));
}

void CmuxPaneView::SetThemeColors(const PaneThemeColors& colors) {
  const bool focus_changed = theme_colors_.focus_border != colors.focus_border;
  const bool idle_changed = theme_colors_.idle_border != colors.idle_border;
  theme_colors_ = colors;
  ApplyToolbarBackground();
  focus_border_target_color_ = colors.focus_border;
  if (focus_changed || idle_changed) {
    if (focus_border_animation_) {
      focus_border_animation_->Stop();
    }
    ApplyFocusBorderColor(FocusBorderTargetColor(focused_));
  }
}

views::View* CmuxPaneView::AsView() {
  return this;
}

void CmuxPaneView::SetFocusedBorder(bool focused) {
  if (focused_ == focused) {
    return;
  }
  focused_ = focused;
  const SkColor target = FocusBorderTargetColor(focused_);
  if (!FocusAnimationsEnabled()) {
    if (focus_border_animation_) {
      focus_border_animation_->Stop();
    }
    ApplyFocusBorderColor(target);
    return;
  }
  if (!focus_border_animation_) {
    focus_border_animation_ = std::make_unique<FocusBorderAnimation>(this);
  }
  focus_border_animation_->AnimateTo(target, FocusBorderDuration());
}

void CmuxPaneView::FocusContent() {
  if (CmuxSurface* s = selected_surface()) {
    s->ActivateToolbarHost();
    s->FocusContent();
  }
}

void CmuxPaneView::FocusOmnibar() {
  if (CmuxSurface* s = selected_surface()) {
    s->FocusOmnibar();
  }
}

void CmuxPaneView::SetActivationCallback(base::RepeatingClosure callback) {
  on_activated_ = std::move(callback);
}

std::u16string CmuxPaneView::E2EOmniboxText() {
  CmuxSurface* s = selected_surface();
  return s ? s->E2EOmniboxText() : std::u16string();
}

void CmuxPaneView::CloseOmniboxPopupUnlessFocused(views::View* focused) {
  // Every surface, not just the selected one: a popup could linger on a tab
  // that was switched away from.
  for (auto& [tab, surface] : surfaces_) {
    surface->CloseOmniboxPopupUnlessFocused(focused);
  }
}

content::WebContents* CmuxPaneView::GetInspectableWebContents() {
  CmuxSurface* s = selected_surface();
  return s ? s->GetInspectableWebContents() : nullptr;
}

void CmuxPaneView::ToggleDevTools() {
  if (CmuxSurface* s = selected_surface()) {
    s->ToggleDevTools();
  }
}

void CmuxPaneView::UndockDevTools() {
  if (CmuxSurface* s = selected_surface()) {
    s->UndockDevTools();
  }
}

bool CmuxPaneView::HandleOmniboxEscape() {
  CmuxSurface* s = selected_surface();
  return s && s->HandleOmniboxEscape();
}

void CmuxPaneView::GoBack() {
  if (CmuxSurface* s = selected_surface()) {
    s->GoBack();
  }
}

void CmuxPaneView::GoForward() {
  if (CmuxSurface* s = selected_surface()) {
    s->GoForward();
  }
}

void CmuxPaneView::Reload() {
  if (CmuxSurface* s = selected_surface()) {
    s->Reload();
  }
}

void CmuxPaneView::OnSelectTab(SurfaceTabId id) {
  if (delegate_) {
    delegate_->OnSelectTabRequested(pane_id_, id);
  }
}

void CmuxPaneView::OnCloseTab(SurfaceTabId id) {
  if (delegate_) {
    delegate_->OnCloseTabRequested(pane_id_, id);
  }
}

void CmuxPaneView::OnCloseOtherTabs(SurfaceTabId id) {
  if (delegate_) {
    delegate_->OnCloseOtherTabsRequested(pane_id_, id);
  }
}

void CmuxPaneView::OnNewTab(SurfaceKind kind) {
  if (delegate_) {
    delegate_->OnNewTabRequested(pane_id_, kind);
  }
}

std::optional<ui::Accelerator> CmuxPaneView::GetNewTabAccelerator(
    SurfaceKind kind) {
  return delegate_ ? delegate_->GetNewTabAccelerator(kind) : std::nullopt;
}

bool CmuxPaneView::IsSidebarOnRight() const {
  return delegate_ && delegate_->IsSidebarOnRight();
}

void CmuxPaneView::OnSetSidebarOnRight(bool on_right) {
  if (delegate_) {
    delegate_->OnSetSidebarOnRightRequested(on_right);
  }
}

void CmuxPaneView::OnNewTabRight(SurfaceTabId id) {
  if (delegate_) {
    delegate_->OnNewTabRightRequested(pane_id_, id);
  }
}

void CmuxPaneView::OnMoveTabToNewColumn(SurfaceTabId id) {
  if (delegate_) {
    delegate_->OnMoveTabToNewColumnRequested(pane_id_, id);
  }
}

void CmuxPaneView::OnSplitRightWithTab(SurfaceTabId id) {
  if (delegate_) {
    delegate_->OnSplitRightWithTabRequested(pane_id_, id);
  }
}

void CmuxPaneView::OnSplitDownWithTab(SurfaceTabId id) {
  if (delegate_) {
    delegate_->OnSplitDownWithTabRequested(pane_id_, id);
  }
}

bool CmuxPaneView::IsTabContextActionEnabled(SurfaceTabId id,
                                             TabContextAction action) const {
  return delegate_ &&
         delegate_->IsTabContextActionEnabled(pane_id_, id, action);
}

bool CmuxPaneView::IsTabContextActionToggled(SurfaceTabId id,
                                             TabContextAction action) const {
  return delegate_ &&
         delegate_->IsTabContextActionToggled(pane_id_, id, action);
}

void CmuxPaneView::OnTabContextAction(SurfaceTabId id,
                                      TabContextAction action) {
  if (delegate_) {
    delegate_->OnTabContextActionRequested(pane_id_, id, action);
  }
}

void CmuxPaneView::OnTabDragStarted(SurfaceTabId id,
                                    const gfx::Point& screen_pt,
                                    const gfx::Point& grab_offset) {
  if (delegate_) {
    delegate_->OnTabDragStarted(pane_id_, id, screen_pt, grab_offset);
  }
}

void CmuxPaneView::OnTabDragUpdated(const gfx::Point& screen_pt) {
  if (delegate_) {
    delegate_->OnTabDragUpdated(screen_pt);
  }
}

void CmuxPaneView::OnTabDragEnded(bool commit) {
  if (delegate_) {
    delegate_->OnTabDragEnded(commit);
  }
}

bool CmuxPaneView::OnLiveReorderTab(SurfaceTabId id, int final_index) {
  return delegate_ && delegate_->OnLiveReorderTab(pane_id_, id, final_index);
}

void CmuxPaneView::OnPaneDragStarted(const gfx::Point& screen_pt,
                                     const gfx::Point& grab_offset) {
  if (delegate_) {
    delegate_->OnPaneDragStarted(pane_id_, screen_pt, grab_offset);
  }
}

void CmuxPaneView::OnPaneDragUpdated(const gfx::Point& screen_pt) {
  if (delegate_) {
    delegate_->OnPaneDragUpdated(screen_pt);
  }
}

void CmuxPaneView::OnPaneDragEnded(bool commit) {
  if (delegate_) {
    delegate_->OnPaneDragEnded(commit);
  }
}

void CmuxPaneView::OnBeginWindowDrag(const ui::MouseEvent& event) {
  if (delegate_) {
    delegate_->OnBeginWindowDrag(event);
  }
}

bool CmuxPaneView::OnMousePressed(const ui::MouseEvent&) {
  OnSurfaceActivated();
  return false;
}

void CmuxPaneView::OnThemeChanged() {
  views::View::OnThemeChanged();
  ApplyToolbarBackground();
}

void CmuxPaneView::OnPaint(gfx::Canvas* canvas) {
  // The focus border is intentionally painted after children so the full-bleed
  // tab-strip background cannot cover its top edge.
  OnPaintBackground(canvas);
}

void CmuxPaneView::PaintChildren(const views::PaintInfo& paint_info) {
  views::View::PaintChildren(paint_info);

  ui::PaintCache paint_cache;
  ui::PaintRecorder recorder(
      paint_info.context(), paint_info.paint_recording_size(),
      paint_info.paint_recording_scale_x(),
      paint_info.paint_recording_scale_y(), &paint_cache);
  OnPaintBorder(recorder.canvas());
}

void CmuxPaneView::ApplyToolbarBackground() {
  if (SkColorGetA(theme_colors_.strip_background) == 0) {
    SetBackground(nullptr);
  } else {
    SetBackground(
        views::CreateSolidBackground(theme_colors_.strip_background));
  }
}

void CmuxPaneView::OnPaintBorder(gfx::Canvas* canvas) {
  views::View::OnPaintBorder(canvas);
  if (SkColorGetA(focus_border_color_) == 0) {
    return;
  }
  // Focus fades are paint-only. The empty Border installed on the view reserves
  // any configured insets; the current color is drawn here so animation ticks
  // do not replace the Border object and invalidate layout.
  const float stroke_width =
      focus_border_thickness_ > 0 ? focus_border_thickness_ : 2.0f;
  cc::PaintFlags stroke;
  stroke.setAntiAlias(false);
  stroke.setStyle(cc::PaintFlags::kStroke_Style);
  stroke.setStrokeWidth(stroke_width);
  stroke.setColor(focus_border_color_);
  gfx::RectF rect(GetLocalBounds());
  rect.Inset(stroke_width * 0.5f);
  canvas->DrawRect(rect, stroke);
}

void CmuxPaneView::OnSurfaceActivated() {
  if (CmuxSurface* s = selected_surface()) {
    s->ActivateToolbarHost();
  }
  if (on_activated_) {
    on_activated_.Run();
  }
}

void CmuxPaneView::OnSurfaceInteracted() {
  if (delegate_) {
    delegate_->OnSurfaceInteraction(pane_id_);
  }
}

void CmuxPaneView::ApplyFocusBorderColor(SkColor color) {
  focus_border_color_ = color;
  SchedulePaint();
}

SkColor CmuxPaneView::FocusBorderTargetColor(bool focused) const {
  if (!focus_border_visible_) {
    return SK_ColorTRANSPARENT;
  }
  if (focus_border_thickness_ <= 0) {
    return focused ? focus_border_target_color_ : SK_ColorTRANSPARENT;
  }
  return focused ? focus_border_target_color_ : theme_colors_.idle_border;
}

base::TimeDelta CmuxPaneView::FocusBorderDuration() const {
  if (!FocusAnimationsEnabled()) {
    return base::Milliseconds(0);
  }
  return base::Milliseconds(std::max(0, animation_ms_ * 140 / 160));
}

bool CmuxPaneView::FocusAnimationsEnabled() const {
  return animations_ && animation_ms_ > 0;
}

BEGIN_METADATA(CmuxPaneView)
END_METADATA

}  // namespace cmux
