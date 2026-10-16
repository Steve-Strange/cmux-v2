// Copyright 2012 The Chromium Authors
// Copyright (c) 2026 Alasdair Monk
// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later AND MIT AND BSD-3-Clause
//
// Contains Bonsplit- and Chromium-derived regions; see
// docs/source-provenance.md.

#include "chrome/browser/cmux_term/cmux_tab_drag.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>

#include "base/check_op.h"
#include "base/functional/bind.h"
#include "base/functional/callback.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/single_thread_task_runner.h"
#include "base/time/time.h"
#include "cc/paint/paint_flags.h"
#include "chrome/browser/cmux_term/cmux_easing.h"
#include "chrome/browser/cmux_term/cmux_tab_strip.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/compositor/layer.h"
#include "ui/gfx/animation/animation.h"
#include "ui/gfx/animation/slide_animation.h"
#include "ui/gfx/animation/tween.h"
#include "ui/gfx/canvas.h"
#include "ui/gfx/font_list.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/rect_f.h"
#include "ui/gfx/geometry/transform.h"
#include "ui/views/animation/animation_delegate_views.h"
#include "ui/views/background.h"
#include "ui/views/view.h"
#include "ui/views/widget/widget.h"

namespace cmux {

namespace {

constexpr SkColor kDropHighlightBlue = SkColorSetRGB(0x00, 0x7a, 0xff);
// PaneContainerView.swift's dropPlaceholder spring.
constexpr double kDropHighlightSpringMs = 250.0;
constexpr double kDropHighlightBounce = 0.15;

gfx::Rect ToRect(const LayoutRect& r) {
  return gfx::Rect(static_cast<int>(r.x), static_cast<int>(r.y),
                   static_cast<int>(r.width), static_cast<int>(r.height));
}

std::string FallbackTitle(SurfaceKind kind) {
  return kind == SurfaceKind::kTerminal ? "Terminal" : "New Tab";
}

base::TimeDelta ScaledSpringRunDuration(int animation_ms,
                                        double perceptual_ms) {
  constexpr int kBaselineAnimationMs = 160;
  constexpr double kSpringRunDurationMultiplier = 1.35;
  const double scaled = std::max(0, animation_ms) * perceptual_ms *
                        kSpringRunDurationMultiplier / kBaselineAnimationMs;
  return base::Milliseconds(static_cast<int>(std::lround(scaled)));
}

base::TimeDelta ScaledDuration(int animation_ms, int base_ms) {
  constexpr int kBaselineAnimationMs = 160;
  return base::Milliseconds(std::max(0, animation_ms) * base_ms /
                            kBaselineAnimationMs);
}

class DragGhostView : public views::View {
  METADATA_HEADER(DragGhostView, views::View)

 public:
  DragGhostView(const std::string& title,
                SurfaceKind kind,
                int tab_count,
                SkColor background,
                SkColor text_color)
      : background_(background), text_color_(text_color) {
    std::string text = title.empty() ? FallbackTitle(kind) : title;
    if (tab_count > 1) {
      text += "  " + base::NumberToString(tab_count) + " tabs";
    }
    title_ = base::UTF8ToUTF16(text);
    SetPaintToLayer();
    layer()->SetFillsBoundsOpaquely(false);
    layer()->SetOpacity(0.9f);
  }

  void OnPaint(gfx::Canvas* canvas) override {
    cc::PaintFlags shadow;
    shadow.setAntiAlias(true);
    shadow.setColor(SkColorSetA(SK_ColorBLACK, 0x55));
    canvas->DrawRoundRect(gfx::RectF(2, 3, width() - 2, height() - 2), 8,
                          shadow);

    cc::PaintFlags bg;
    bg.setAntiAlias(true);
    bg.setColor(background_);
    canvas->DrawRoundRect(gfx::RectF(0, 0, width() - 3, height() - 4), 8, bg);

    canvas->DrawStringRectWithFlags(
        title_, gfx::FontList(), text_color_,
        gfx::Rect(10, 3, std::max(0, width() - 22), std::max(0, height() - 8)),
        gfx::Canvas::TEXT_ALIGN_LEFT | gfx::Canvas::NO_SUBPIXEL_RENDERING);
  }

 private:
  std::u16string title_;
  SkColor background_;
  SkColor text_color_;
};

BEGIN_METADATA(DragGhostView)
END_METADATA

class DropHighlightView : public views::View {
  METADATA_HEADER(DropHighlightView, views::View)

 public:
  explicit DropHighlightView(SkColor highlight_color)
      : highlight_color_(highlight_color) {
    SetPaintToLayer();
    layer()->SetFillsBoundsOpaquely(false);
  }

  void SetHighlightColor(SkColor highlight_color) {
    if (highlight_color_ == highlight_color) {
      return;
    }
    highlight_color_ = highlight_color;
    SchedulePaint();
  }

  void set_bar(bool bar) {
    if (bar_ == bar) {
      return;
    }
    bar_ = bar;
    SchedulePaint();
  }

  bool bar() const { return bar_; }

  void OnPaint(gfx::Canvas* canvas) override {
    cc::PaintFlags fill;
    fill.setAntiAlias(true);
    fill.setColor(SkColorSetA(highlight_color_, bar_ ? 0xd0 : 0x40));
    const float radius = bar_ ? 2.0f : 8.0f;
    canvas->DrawRoundRect(gfx::RectF(GetLocalBounds()), radius, fill);
    if (bar_) {
      return;
    }
    cc::PaintFlags stroke;
    stroke.setAntiAlias(true);
    stroke.setStyle(cc::PaintFlags::kStroke_Style);
    stroke.setStrokeWidth(2.0f);
    stroke.setColor(highlight_color_);
    const gfx::RectF stroke_bounds(
        1.0f, 1.0f, std::max(0.0f, static_cast<float>(width()) - 2.0f),
        std::max(0.0f, static_cast<float>(height()) - 2.0f));
    canvas->DrawRoundRect(stroke_bounds, radius - 1.0f, stroke);
  }

 private:
  SkColor highlight_color_ = kDropHighlightBlue;
  bool bar_ = false;
};

BEGIN_METADATA(DropHighlightView)
END_METADATA

}  // namespace

class CmuxTabDragController::HighlightAnimation
    : public views::AnimationDelegateViews {
 public:
  explicit HighlightAnimation(views::View* view)
      : views::AnimationDelegateViews(view), view_(view), animation_(this) {}

  HighlightAnimation(const HighlightAnimation&) = delete;
  HighlightAnimation& operator=(const HighlightAnimation&) = delete;

  ~HighlightAnimation() override { animation_.Stop(); }

  void SetImmediate(const gfx::Rect& target, float opacity = 1.0f) {
    animation_.Stop();
    has_target_ = true;
    target_bounds_ = target;
    target_opacity_ = opacity;
    current_opacity_ = opacity;
    if (!view_) {
      return;
    }
    view_->SetTransform(gfx::Transform());
    view_->SetBoundsRect(target);
    if (view_->layer()) {
      view_->layer()->SetOpacity(opacity);
    }
    view_->SetVisible(opacity > 0.0f);
  }

  void AnimateFrom(const gfx::Rect& start,
                   float start_opacity,
                   const gfx::Rect& target,
                   float target_opacity,
                   bool animations_enabled,
                   base::TimeDelta run_duration) {
    if (!animations_enabled || run_duration.is_zero() || !view_) {
      SetImmediate(target, target_opacity);
      return;
    }
    animation_.Stop();
    start_bounds_ = start;
    spring_start_opacity_ = start_opacity;
    has_target_ = true;
    target_bounds_ = target;
    target_opacity_ = target_opacity;
    current_opacity_ = start_opacity;
    view_->SetTransform(gfx::Transform());
    view_->SetBoundsRect(start);
    view_->SetVisible(true);
    if (view_->layer()) {
      view_->layer()->SetOpacity(start_opacity);
    }
    animation_.Reset(0.0);
    animation_.SetTweenType(gfx::Tween::LINEAR);
    animation_.SetSlideDuration(run_duration);
    animation_.Show();
    ApplySpring(0.0);
  }

  void Retarget(const gfx::Rect& target,
                float target_opacity,
                bool animations_enabled,
                base::TimeDelta run_duration) {
    if (!animations_enabled || run_duration.is_zero() || !view_) {
      SetImmediate(target, target_opacity);
      return;
    }
    // Pointer updates arrive much faster than animation frames. Restarting for
    // an identical target keeps normalized progress near zero, making the
    // highlight move only after the pointer stops. Let the current run finish.
    if (has_target_ && target_bounds_ == target &&
        target_opacity_ == target_opacity) {
      return;
    }
    if (animation_.is_animating()) {
      ApplyCurrentFrame();
    }
    animation_.Stop();
    start_bounds_ = view_->bounds();
    target_bounds_ = target;
    has_target_ = true;
    spring_start_opacity_ = current_opacity_;
    target_opacity_ = target_opacity;
    view_->SetTransform(gfx::Transform());
    view_->SetVisible(true);
    if (start_bounds_ == target_bounds_ &&
        std::fabs(current_opacity_ - target_opacity_) < 0.001f) {
      SetImmediate(target, target_opacity);
      return;
    }
    animation_.Reset(0.0);
    animation_.SetTweenType(gfx::Tween::LINEAR);
    animation_.SetSlideDuration(run_duration);
    animation_.Show();
  }

  void Stop() { animation_.Stop(); }

  void ResetForHide() {
    animation_.Stop();
    has_target_ = false;
    start_bounds_ = gfx::Rect();
    target_bounds_ = gfx::Rect();
    current_opacity_ = 0.0f;
    target_opacity_ = 0.0f;
    if (!view_) {
      return;
    }
    view_->SetTransform(gfx::Transform());
    if (view_->layer()) {
      view_->layer()->SetOpacity(0.0f);
    }
    view_->SetVisible(false);
  }

  void AnimationProgressed(const gfx::Animation* animation) override {
    if (animation != &animation_) {
      return;
    }
    ApplySpring(animation_.GetCurrentValue());
  }

  void AnimationEnded(const gfx::Animation* animation) override {
    if (animation == &animation_) {
      SetImmediate(target_bounds_, target_opacity_);
    }
  }

  void AnimationCanceled(const gfx::Animation*) override {}

 private:
  void ApplyCurrentFrame() {
    ApplySpring(animation_.GetCurrentValue());
  }

  void ApplySpring(double t) {
    if (!view_) {
      return;
    }
    const double value = AppleSpring(t, kDropHighlightBounce);
    view_->SetBoundsRect(
        gfx::Tween::RectValueBetween(value, start_bounds_, target_bounds_));
    current_opacity_ = static_cast<float>(gfx::Tween::DoubleValueBetween(
        std::clamp(value, 0.0, 1.0), spring_start_opacity_, target_opacity_));
    if (view_->layer()) {
      view_->layer()->SetOpacity(current_opacity_);
    }
  }

  raw_ptr<views::View> view_;
  gfx::SlideAnimation animation_;
  bool has_target_ = false;
  gfx::Rect start_bounds_;
  gfx::Rect target_bounds_;
  float current_opacity_ = 0.0f;
  float spring_start_opacity_ = 1.0f;
  float target_opacity_ = 1.0f;
};

class CmuxTabDragController::OverlayFadeOutAnimation
    : public views::AnimationDelegateViews {
 public:
  OverlayFadeOutAnimation(views::View* contents, base::OnceClosure done)
      : views::AnimationDelegateViews(contents),
        contents_(contents),
        done_(std::move(done)),
        animation_(this) {}
  OverlayFadeOutAnimation(const OverlayFadeOutAnimation&) = delete;
  OverlayFadeOutAnimation& operator=(const OverlayFadeOutAnimation&) = delete;
  ~OverlayFadeOutAnimation() override { animation_.Stop(); }

  void Start(base::TimeDelta duration) {
    if (!contents_ || !contents_->layer() || duration.is_zero()) {
      RunDone();
      return;
    }
    contents_->layer()->SetOpacity(1.0f);
    animation_.Reset(0.0);
    animation_.SetTweenType(gfx::Tween::LINEAR);
    animation_.SetSlideDuration(duration);
    animation_.Show();
  }

  void Stop() { animation_.Stop(); }

  void AnimationProgressed(const gfx::Animation* animation) override {
    if (animation == &animation_ && contents_ && contents_->layer()) {
      contents_->layer()->SetOpacity(
          static_cast<float>(gfx::Tween::DoubleValueBetween(
              animation_.GetCurrentValue(), 1.0, 0.0)));
    }
  }

  void AnimationEnded(const gfx::Animation* animation) override {
    if (animation == &animation_) {
      RunDone();
    }
  }

 private:
  void RunDone() {
    if (done_) {
      std::move(done_).Run();
    }
  }

  raw_ptr<views::View> contents_;
  base::OnceClosure done_;
  gfx::SlideAnimation animation_;
};

CmuxTabDragController::CmuxTabDragController(TabDragHost* host) : host_(host) {}

CmuxTabDragController::~CmuxTabDragController() {
  DestroyOverlays();
}

void CmuxTabDragController::SetAnimationConfig(bool animations,
                                               int animation_ms) {
  animations_enabled_ = animations;
  animation_ms_ = std::max(0, animation_ms);
}

void CmuxTabDragController::SetThemeColors(SkColor drop_highlight_color,
                                           SkColor ghost_background,
                                           SkColor ghost_text) {
  drop_highlight_color_ = drop_highlight_color;
  ghost_background_ = ghost_background;
  ghost_text_ = ghost_text;
  if (highlight_) {
    static_cast<DropHighlightView*>(highlight_.get())
        ->SetHighlightColor(drop_highlight_color_);
  }
}

void CmuxTabDragController::StartTabDrag(PaneId source_pane,
                                         SurfaceTabId tab,
                                         const std::string& title,
                                         SurfaceKind kind,
                                         const gfx::Point& screen_pt,
                                         const gfx::Point& grab_offset) {
  DestroyOverlays();
  active_ = true;
  pane_drag_ = false;
  source_pane_ = source_pane;
  source_tab_ = tab;
  grab_offset_ = grab_offset;
  target_ = DropTarget();
  target_index_ = -1;
  hidden_highlight_bounds_ = gfx::Rect();
  highlighted_pane_ = kInvalidId;
  EnsureOverlays();
  if (overlay_contents_) {
    if (ghost_) {
      overlay_contents_->RemoveChildViewT(ghost_.get());
    }
    ghost_ = overlay_contents_->AddChildView(std::make_unique<DragGhostView>(
        title, kind, /*tab_count=*/1, ghost_background_, ghost_text_));
    ghost_->SetBounds(0, 0, 190, 30);
    overlay_contents_->ReorderChildView(
        ghost_, overlay_contents_->children().size() - 1);
  }
  Update(screen_pt);
}

void CmuxTabDragController::StartPaneDrag(PaneId source_pane,
                                          const gfx::Point& screen_pt,
                                          const gfx::Point& grab_offset) {
  const Pane* pane =
      host_->model().FindPane(host_->active_workspace(), source_pane);
  if (!pane) {
    return;
  }
  DestroyOverlays();
  active_ = true;
  pane_drag_ = true;
  source_pane_ = source_pane;
  source_tab_ = kInvalidId;
  grab_offset_ = grab_offset;
  target_ = DropTarget();
  target_index_ = -1;
  hidden_highlight_bounds_ = gfx::Rect();
  highlighted_pane_ = kInvalidId;

  std::string title;
  SurfaceKind kind = SurfaceKind::kWeb;
  if (const SurfaceTab* selected = pane->SelectedTab()) {
    title = selected->title;
    kind = selected->kind;
  }
  EnsureOverlays();
  if (overlay_contents_) {
    if (ghost_) {
      overlay_contents_->RemoveChildViewT(ghost_.get());
    }
    ghost_ = overlay_contents_->AddChildView(std::make_unique<DragGhostView>(
        title, kind, static_cast<int>(pane->tabs.size()), ghost_background_,
        ghost_text_));
    ghost_->SetBounds(0, 0, 220, 30);
    overlay_contents_->ReorderChildView(
        ghost_, overlay_contents_->children().size() - 1);
  }
  Update(screen_pt);
}

void CmuxTabDragController::EnsureOverlays() {
  views::View* parent = host_->strip_container();
  views::Widget* host_widget = host_->HostWidget();
  if (!parent || !host_widget) {
    return;
  }
  if (!overlay_widget_) {
    overlay_bounds_ = parent->GetLocalBounds();
    views::View::ConvertRectToScreen(parent, &overlay_bounds_);
    if (overlay_bounds_.IsEmpty()) {
      return;
    }

    overlay_widget_ = std::make_unique<views::Widget>();
    // Match Chromium's caller-owned drag-image widgets (button_drag_utils):
    // the controller owns the Widget and closes it deterministically on
    // End/Cancel/destruction instead of letting the native child window own us.
    views::Widget::InitParams params(
        views::Widget::InitParams::CLIENT_OWNS_WIDGET,
        views::Widget::InitParams::TYPE_POPUP);
    params.child = true;
    params.parent = host_widget->GetNativeView();
    params.bounds = overlay_bounds_;
    params.opacity = views::Widget::InitParams::WindowOpacity::kTranslucent;
    params.activatable = views::Widget::InitParams::Activatable::kNo;
    params.accept_events = false;
    params.shadow_type = views::Widget::InitParams::ShadowType::kNone;
    // A non-normal z-order is an application-independent always-on-top level.
    // The native parent already keeps this child popup above the host window
    // on macOS, Windows, and Linux, including above embedded native surfaces.
    // Keep the normal z-order so another application can cover both windows.
    params.name = "cmux-tab-drag-overlay";
    overlay_widget_->Init(std::move(params));
    overlay_widget_->SetVisibilityChangedAnimationsEnabled(false);

    auto contents = std::make_unique<views::View>();
    contents->SetPaintToLayer();
    contents->layer()->SetFillsBoundsOpaquely(false);
    contents->layer()->SetOpacity(1.0f);
    overlay_contents_ = overlay_widget_->SetContentsView(std::move(contents));
    overlay_widget_->ShowInactive();
  }
  if (!overlay_contents_) {
    return;
  }
  if (!highlight_) {
    highlight_ = overlay_contents_->AddChildView(
        std::make_unique<DropHighlightView>(drop_highlight_color_));
    highlight_->SetVisible(false);
  }
  if (!highlight_animation_ && highlight_) {
    highlight_animation_ = std::make_unique<HighlightAnimation>(highlight_);
  }
}

gfx::Point CmuxTabDragController::ToOverlay(const gfx::Point& screen) const {
  gfx::Point overlay = screen;
  overlay.Offset(-overlay_bounds_.x(), -overlay_bounds_.y());
  if (host_) {
    if (views::View* parent = host_->strip_container()) {
      gfx::Point content_local = screen;
      views::View::ConvertPointFromScreen(parent, &content_local);
      DCHECK_EQ(content_local.x(), overlay.x());
      DCHECK_EQ(content_local.y(), overlay.y());
    }
  }
  return overlay;
}

bool CmuxTabDragController::UpdateLiveReorderIfNeeded(
    const gfx::Point& screen_pt) {
  if (pane_drag_ || target_.kind != DropKind::kTabStrip ||
      target_.pane != source_pane_) {
    EndLiveReorder(/*commit=*/false);
    return false;
  }
  CmuxTabStrip* strip = host_->TabStripFor(source_pane_);
  if (!strip) {
    EndLiveReorder(/*commit=*/false);
    return false;
  }
  if (indicator_strip_) {
    indicator_strip_->ClearInsertionIndicator();
    indicator_strip_ = nullptr;
  }
  HideHighlightFeedback();
  if (ghost_) {
    ghost_->SetVisible(false);
  }
  gfx::Point strip_local = screen_pt;
  views::View::ConvertPointFromScreen(strip, &strip_local);
  if (!strip->UpdateLiveReorder(source_tab_, strip_local, grab_offset_)) {
    EndLiveReorder(/*commit=*/false);
    return false;
  }
  live_reorder_strip_ = strip;
  target_index_ = strip->live_reorder_index();
  return true;
}

void CmuxTabDragController::EndLiveReorder(bool commit) {
  if (!live_reorder_strip_) {
    return;
  }
  live_reorder_strip_->EndLiveReorder(commit);
  live_reorder_strip_ = nullptr;
}

void CmuxTabDragController::HideHighlightFeedback() {
  if (!highlight_ || !highlight_->GetVisible()) {
    return;
  }
  if (!highlight_animation_) {
    highlight_->SetVisible(false);
    return;
  }
  const auto* highlight_view =
      static_cast<const DropHighlightView*>(highlight_.get());
  const gfx::Rect hidden_bounds =
      highlight_view->bar() || hidden_highlight_bounds_.IsEmpty()
          ? highlight_->bounds()
          : hidden_highlight_bounds_;
  highlight_animation_->Retarget(
      hidden_bounds, 0.0f, animations_enabled_ && animation_ms_ > 0,
      ScaledSpringRunDuration(animation_ms_, kDropHighlightSpringMs));
}

void CmuxTabDragController::Update(const gfx::Point& screen_pt) {
  if (!active_) {
    return;
  }
  views::View* parent = host_->strip_container();
  if (!parent || !overlay_contents_) {
    return;
  }
  gfx::Point local = ToOverlay(screen_pt);
  if (ghost_) {
    ghost_->SetBounds(local.x() - grab_offset_.x(),
                      local.y() - grab_offset_.y(), ghost_->width(),
                      ghost_->height());
  }

  StripLayout layout = host_->CurrentLayout();
  const Workspace* workspace =
      host_->model().GetWorkspace(host_->active_workspace());
  if (!workspace) {
    return;
  }
  target_ = ResolveDropTarget(*workspace, layout, CurrentPayload(), local.x(),
                              local.y(), host_->TabStripHeight());

  if (UpdateLiveReorderIfNeeded(screen_pt)) {
    return;
  }
  if (ghost_) {
    ghost_->SetVisible(true);
    if (ghost_->layer()) {
      ghost_->layer()->SetOpacity(0.9f);
    }
  }

  CmuxTabStrip* previous_indicator_strip = indicator_strip_;
  const int previous_target_index = target_index_;
  target_index_ = -1;

  if (target_.kind == DropKind::kTabStrip) {
    HideHighlightFeedback();
    CmuxTabStrip* strip = host_->TabStripFor(target_.pane);
    if (indicator_strip_ && indicator_strip_ != strip) {
      indicator_strip_->ClearInsertionIndicator();
    }
    indicator_strip_ = strip;
    if (strip) {
      gfx::Point strip_local = screen_pt;
      views::View::ConvertPointFromScreen(strip, &strip_local);
      target_index_ = strip->InsertionIndexAt(strip_local);
      if (strip != previous_indicator_strip ||
          target_index_ != previous_target_index) {
        strip->ShowInsertionIndicator(target_index_);
      }
    }
    return;
  }

  if (indicator_strip_) {
    indicator_strip_->ClearInsertionIndicator();
    indicator_strip_ = nullptr;
  }
  if (!highlight_) {
    return;
  }
  if (target_.kind == DropKind::kPaneCenter ||
      target_.kind == DropKind::kPaneEdge ||
      target_.kind == DropKind::kNewColumn) {
    auto* highlight_view =
        static_cast<DropHighlightView*>(highlight_.get());
    const bool show_as_bar = target_.kind == DropKind::kNewColumn;
    const bool was_visible = highlight_->GetVisible();
    const bool style_changed =
        was_visible && highlight_view->bar() != show_as_bar;
    highlight_view->set_bar(show_as_bar);
    auto to_overlay_rect = [&](const LayoutRect& rect) {
      gfx::Rect bounds = ToRect(rect);
      gfx::Point origin = bounds.origin();
      views::View::ConvertPointToScreen(parent, &origin);
      bounds.set_origin(ToOverlay(origin));
      return bounds;
    };
    const gfx::Rect target_bounds = to_overlay_rect(target_.highlight);
    const gfx::Rect hidden_bounds =
        show_as_bar ? target_bounds : to_overlay_rect(target_.hidden_highlight);
    const bool pane_changed =
        !show_as_bar && highlighted_pane_ != target_.pane;
    const bool animations_enabled =
        animations_enabled_ && animation_ms_ > 0;
    const base::TimeDelta spring_duration =
        ScaledSpringRunDuration(animation_ms_, kDropHighlightSpringMs);
    if (!highlight_animation_ || !overlay_contents_->GetWidget()) {
      highlight_->SetVisible(true);
      highlight_->SetBoundsRect(target_bounds);
      if (highlight_->layer()) {
        highlight_->layer()->SetOpacity(1.0f);
      }
    } else if (!was_visible || style_changed || pane_changed) {
      highlight_animation_->AnimateFrom(
          hidden_bounds, 0.0f, target_bounds, 1.0f, animations_enabled,
          spring_duration);
    } else {
      highlight_animation_->Retarget(target_bounds, 1.0f, animations_enabled,
                                     spring_duration);
    }
    hidden_highlight_bounds_ = hidden_bounds;
    highlighted_pane_ = show_as_bar ? kInvalidId : target_.pane;
  } else {
    HideHighlightFeedback();
  }
}

void CmuxTabDragController::End(bool commit) {
  if (!active_) {
    return;
  }
  // A successful attached drag is already reflected in the model. Keep its
  // transforms alive until CmuxTabStrip::EndDrag applies the deferred model
  // order and settles from the pointer position, matching Chromium's
  // StoppedDragging flow. Cancellation still restores immediately.
  if (commit) {
    live_reorder_strip_ = nullptr;
  } else {
    EndLiveReorder(/*commit=*/false);
  }
  DragCommit drag;
  drag.pane_drag = pane_drag_;
  drag.source_pane = source_pane_;
  drag.tab = source_tab_;
  drag.target = target_;
  drag.index = target_index_;
  ClearFeedback(/*hide_overlay_feedback=*/false);
  if (commit && target_.kind != DropKind::kNone && target_.no_op) {
    host_->ReflowAfterNoOpDrop();
  } else if (commit && target_.kind != DropKind::kNone) {
    host_->CommitDrop(drag);
  }
  active_ = false;
  pane_drag_ = false;
  source_pane_ = kInvalidId;
  source_tab_ = kInvalidId;
  target_ = DropTarget();
  target_index_ = -1;
  FadeOutAndDestroyOverlays();
}

void CmuxTabDragController::Cancel() {
  if (!active_) {
    DestroyOverlays();
    return;
  }
  EndLiveReorder(/*commit=*/false);
  ClearFeedback(/*hide_overlay_feedback=*/false);
  active_ = false;
  pane_drag_ = false;
  source_pane_ = kInvalidId;
  source_tab_ = kInvalidId;
  target_ = DropTarget();
  target_index_ = -1;
  FadeOutAndDestroyOverlays();
}

void CmuxTabDragController::CancelImmediately() {
  Cancel();
  // Cancel() normally preserves Bonsplit's short exit transition. Once the
  // host is unavailable, even that brief separate-window animation is wrong.
  DestroyOverlays();
}

void CmuxTabDragController::ClearFeedback(bool hide_overlay_feedback) {
  if (indicator_strip_) {
    indicator_strip_->ClearInsertionIndicator();
    indicator_strip_ = nullptr;
  }
  if (highlight_ && hide_overlay_feedback && highlight_->GetVisible()) {
    if (highlight_animation_) {
      highlight_animation_->ResetForHide();
    }
    highlight_->SetVisible(false);
  }
}

void CmuxTabDragController::DestroyOverlays() {
  // A queued fade-done task must never tear down a NEWER drag's overlays.
  weak_factory_.InvalidateWeakPtrs();
  if (overlay_fade_animation_) {
    overlay_fade_animation_->Stop();
  }
  overlay_fade_animation_.reset();
  if (highlight_animation_) {
    highlight_animation_->Stop();
  }
  highlight_animation_.reset();
  if (overlay_widget_) {
    overlay_widget_->CloseNow();
    overlay_widget_.reset();
  }
  overlay_contents_ = nullptr;
  ghost_ = nullptr;
  highlight_ = nullptr;
  overlay_bounds_ = gfx::Rect();
  hidden_highlight_bounds_ = gfx::Rect();
  highlighted_pane_ = kInvalidId;
}

void CmuxTabDragController::FadeOutAndDestroyOverlays() {
  if (!overlay_widget_ || !overlay_contents_) {
    DestroyOverlays();
    return;
  }
  if (!animations_enabled_ || animation_ms_ <= 0) {
    DestroyOverlays();
    return;
  }
  if (highlight_ && highlight_->GetVisible()) {
    // Bonsplit clears activeDropZone on exit/drop, which springs the current
    // placeholder back to the full hidden frame at 0 opacity. Keep the popup
    // alive for that same transition; the drag preview itself is finished.
    if (ghost_) {
      ghost_->SetVisible(false);
    }
    const base::TimeDelta duration =
        ScaledSpringRunDuration(animation_ms_, kDropHighlightSpringMs);
    HideHighlightFeedback();
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
        FROM_HERE,
        base::BindOnce(&CmuxTabDragController::DestroyOverlays,
                       weak_factory_.GetWeakPtr()),
        duration);
    return;
  }
  const base::TimeDelta duration = ScaledDuration(animation_ms_, 90);
  if (duration.is_zero()) {
    DestroyOverlays();
    return;
  }
  if (highlight_animation_) {
    highlight_animation_->Stop();
  }
  overlay_fade_animation_ = std::make_unique<OverlayFadeOutAnimation>(
      overlay_contents_,
      base::BindOnce(
          [](base::WeakPtr<CmuxTabDragController> self) {
            base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
                FROM_HERE,
                base::BindOnce(&CmuxTabDragController::DestroyOverlays, self));
          },
          weak_factory_.GetWeakPtr()));
  overlay_fade_animation_->Start(duration);
}

DragPayload CmuxTabDragController::CurrentPayload() const {
  DragPayload payload;
  payload.source_pane = source_pane_;
  payload.whole_pane = pane_drag_;
  payload.tab = source_tab_;
  const Pane* source =
      host_->model().FindPane(host_->active_workspace(), source_pane_);
  payload.source_tab_count = source ? static_cast<int>(source->tabs.size()) : 0;
  return payload;
}

}  // namespace cmux
