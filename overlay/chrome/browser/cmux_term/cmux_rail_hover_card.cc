// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// The interaction and animation state machine in this file is adapted from
// Chromium 150.0.7871.128's TabHoverCardController and
// TabHoverCardBubbleView. Helium enables that Chromium implementation in the
// pinned 8030a8a3050151a141c06cc5a85f95cbbdc42a25 source snapshot. See
// THIRD_PARTY_NOTICES.md. Only the TabInterface-to-workspace data boundary is
// replaced here.

#include "chrome/browser/cmux_term/cmux_rail_hover_card.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>
#include <utility>

#include "base/check.h"
#include "base/functional/bind.h"
#include "base/memory/raw_ptr.h"
#include "base/scoped_observation.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "build/build_config.h"
#include "chrome/browser/cmux_term/cmux_rail.h"
#include "chrome/app/vector_icons/vector_icons.h"
#include "chrome/browser/ui/color/chrome_color_id.h"
#include "chrome/common/chrome_version.h"
#include "ui/accessibility/ax_enums.mojom.h"
#include "ui/base/models/image_model.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/base/mojom/dialog_button.mojom.h"
#include "ui/base/ui_base_features.h"
#include "ui/color/color_id.h"
#include "ui/color/color_provider.h"
#include "ui/compositor/layer.h"
#include "ui/events/event.h"
#include "ui/events/event_observer.h"
#include "ui/events/keycodes/keyboard_codes.h"
#include "ui/gfx/animation/animation.h"
#include "ui/gfx/animation/linear_animation.h"
#include "ui/gfx/animation/tween.h"
#include "ui/gfx/canvas.h"
#include "ui/gfx/geometry/insets.h"
#include "ui/gfx/geometry/rounded_corners_f.h"
#include "ui/gfx/image/image_skia.h"
#include "ui/gfx/paint_vector_icon.h"
#include "ui/gfx/text_constants.h"
#include "ui/views/animation/animation_delegate_views.h"
#include "ui/views/accessibility/view_accessibility.h"
#include "ui/views/animation/bubble_slide_animator.h"
#include "ui/views/animation/widget_fade_animator.h"
#include "ui/views/background.h"
#include "ui/views/bubble/bubble_dialog_delegate_view.h"
#include "ui/views/bubble/bubble_frame_view.h"
#include "ui/views/controls/image_view.h"
#include "ui/views/controls/label.h"
#include "ui/views/event_monitor.h"
#include "ui/views/focus/focus_manager.h"
#include "ui/views/layout/fill_layout.h"
#include "ui/views/layout/flex_layout.h"
#include "ui/views/layout/flex_layout_types.h"
#include "ui/views/layout/layout_provider.h"
#include "ui/views/style/typography.h"
#include "ui/views/view.h"
#include "ui/views/view_class_properties.h"
#include "ui/views/view_observer.h"
#include "ui/views/widget/widget.h"
#include "ui/views/widget/widget_observer.h"

namespace cmux {

namespace {

// These are verbatim Chromium 150 hover-card defaults. Helium does not change
// them; it enables preview hover cards by default.
constexpr int kCollapsedRailWidth = 56;
constexpr int kDefaultRailWidth = 200;
constexpr int kHoverCardWidth = 252;
constexpr int kHoverCardPreviewHeight = 141;
// Chromium 150 introduced BubbleAnchor-based slide animation, directional
// widget fades, and the rounded-icon feature switch. Chromium 149 retains the
// AnchorView/parameterless-fade APIs and its original icon names.
#if CHROME_VERSION_MAJOR >= 150
constexpr int kHoverCardSlideDistance = 6;
#endif
constexpr int kHoverCardCornerRadius = 12;
constexpr int kHoverCardTitleMaxLines = 2;
constexpr int kTitleDomainSpacing = 4;
constexpr auto kTextMargins = gfx::Insets::VH(12, 12);
constexpr base::TimeDelta kHoverCardSlideDuration = base::Milliseconds(200);
constexpr base::TimeDelta kShowWithoutDelayTimeBuffer =
    base::Milliseconds(300);
constexpr double kPreviewImageCrossfadeStart = 0.25;

enum class PreviewWaitState {
  kNotWaiting,
  kWaitingWithPlaceholder,
  kWaitingWithoutPlaceholder,
};

base::TimeDelta GetPreviewImageCaptureDelay(
    WorkspaceHoverCardPreviewReadiness readiness) {
  switch (readiness) {
    case WorkspaceHoverCardPreviewReadiness::kNotReady:
      return base::Milliseconds(800);
    case WorkspaceHoverCardPreviewReadiness::kReadyForInitialCapture:
    case WorkspaceHoverCardPreviewReadiness::kReadyForFinalCapture:
      return base::Milliseconds(300);
  }
  return base::TimeDelta();
}

base::TimeDelta GetShowDelay(int rail_width) {
  constexpr base::TimeDelta kMinimumTriggerDelay = base::Milliseconds(300);
  constexpr base::TimeDelta kMaximumTriggerDelay = base::Milliseconds(800);
  constexpr base::TimeDelta kMaxWidthAdditionalDelay =
      base::Milliseconds(500);

  if (rail_width <= kCollapsedRailWidth) {
    return kMinimumTriggerDelay;
  }
  const double logarithmic_fraction =
      std::log(rail_width - kCollapsedRailWidth + 1) /
      std::log(kDefaultRailWidth - kCollapsedRailWidth + 1);
  const base::TimeDelta scaling_factor =
      kMaximumTriggerDelay - kMinimumTriggerDelay;
  base::TimeDelta delay =
      logarithmic_fraction * scaling_factor + kMinimumTriggerDelay;
  if (rail_width >= kDefaultRailWidth) {
    delay += kMaxWidthAdditionalDelay;
  }
  return delay;
}

bool UseAnimations() {
  return gfx::Animation::ShouldRenderRichAnimation();
}

gfx::Size GetPreviewImageSize(gfx::Size preview_size) {
  constexpr gfx::Size kPreferredSize(kHoverCardWidth,
                                     kHoverCardPreviewHeight);
  if (preview_size.IsEmpty()) {
    return preview_size;
  }
  const float preview_aspect_ratio =
      static_cast<float>(preview_size.width()) / preview_size.height();
  const float preferred_aspect_ratio =
      static_cast<float>(kPreferredSize.width()) / kPreferredSize.height();
  const float ratio = preview_aspect_ratio / preferred_aspect_ratio;
  constexpr float kMinStretchRatio = 0.667f;
  constexpr float kMaxStretchRatio = 1.5f;
  if (ratio >= kMinStretchRatio && ratio <= kMaxStretchRatio) {
    return kPreferredSize;
  }
  return preview_size;
}

// Workspace adaptation of Chromium's TabHoverCardBubbleView::ThumbnailView.
// It deliberately preserves the two-ImageView, rewindable 200 ms crossfade
// so moving between workspace rows never flashes through an empty preview.
class HoverCardThumbnailView : public views::View,
                               public views::AnimationDelegateViews {
  METADATA_HEADER(HoverCardThumbnailView, views::View)

 public:
  HoverCardThumbnailView()
      : AnimationDelegateViews(this), image_transition_animation_(this) {
    image_transition_animation_.SetDuration(kHoverCardSlideDuration);
    target_image_ = AddChildView(CreateImageView());
    image_fading_out_ = AddChildView(CreateImageView());
    image_fading_out_->SetPaintToLayer();
    image_fading_out_->layer()->SetOpacity(0.0f);
    SetLayoutManager(std::make_unique<views::FillLayout>());
  }

  void SetAnimationEnabled(bool animation_enabled) {
    animation_enabled_ = animation_enabled;
  }

  void SetRoundedCorners(bool round_corners, float radius) {
    image_fading_out_->layer()->SetRoundedCornerRadius(
        round_corners ? gfx::RoundedCornersF(0, 0, radius, radius)
                      : gfx::RoundedCornersF());
  }

  void SetTargetWorkspaceImage(gfx::ImageSkia preview_image) {
    StartFadeOut();
    SetImage(target_image_, preview_image, ImageType::kThumbnail);
    image_type_ = ImageType::kThumbnail;
  }

  void SetPlaceholderImage() {
#if CHROME_VERSION_MAJOR >= 150
    SetImageFromIcon(ImageType::kPlaceholder,
                     features::IsRoundedIconsEnabled() ? kGlobeIcon
                                                       : kGlobeOldIcon);
#else
    SetImageFromIcon(ImageType::kPlaceholder, kGlobeIcon);
#endif
  }

  void SetCrashedImage() {
#if CHROME_VERSION_MAJOR >= 150
    SetImageFromIcon(ImageType::kCrashed,
                     features::IsRoundedIconsEnabled() ? kSadTabIcon
                                                       : kCrashedTabOldIcon);
#else
    SetImageFromIcon(ImageType::kCrashed, kCrashedTabIcon);
#endif
  }

  void ClearImage() {
    if (image_type_ == ImageType::kNone) {
      return;
    }
    StartFadeOut();
    SetImage(target_image_, gfx::ImageSkia(), ImageType::kNone);
    image_type_ = ImageType::kNone;
  }

  void SetWaitingForImage() {
    if (image_type_ == ImageType::kNone) {
      image_type_ = ImageType::kNoneButWaiting;
      InvalidateLayout();
    }
  }

 private:
  enum class ImageType {
    kNone,
    kNoneButWaiting,
    kPlaceholder,
    kCrashed,
    kThumbnail,
  };

  static std::unique_ptr<views::ImageView> CreateImageView() {
    auto image_view = std::make_unique<views::ImageView>();
    image_view->SetHorizontalAlignment(views::ImageView::Alignment::kCenter);
    return image_view;
  }

  void SetImageFromIcon(ImageType type, const gfx::VectorIcon& icon) {
    if (image_type_ == type) {
      return;
    }
    const ui::ColorProvider* const color_provider = GetColorProvider();
    if (!color_provider) {
      init_placeholder_image_ = type == ImageType::kPlaceholder;
      init_crashed_image_ = type == ImageType::kCrashed;
      return;
    }

    StartFadeOut();
    constexpr int kIconSize = 64;
    const gfx::ImageSkia image = gfx::CreateVectorIcon(
        icon, kIconSize,
        color_provider->GetColor(kColorTabHoverCardForeground));
    SetImage(target_image_, image, type);
    image_type_ = type;
  }

  void SetImage(views::ImageView* image_view,
                gfx::ImageSkia image,
                ImageType image_type) {
    image_view->SetImage(ui::ImageModel::FromImageSkia(image));
    switch (image_type) {
      case ImageType::kNone:
      case ImageType::kNoneButWaiting:
        image_view->SetBackground(
            views::CreateSolidBackground(kColorTabHoverCardBackground));
        break;
      case ImageType::kPlaceholder:
      case ImageType::kCrashed:
        image_view->SetVerticalAlignment(views::ImageView::Alignment::kCenter);
        image_view->SetImageSize(image.size());
        image_view->SetBackground(
            views::CreateSolidBackground(kColorTabHoverCardBackground));
        break;
      case ImageType::kThumbnail:
        image_view->SetVerticalAlignment(views::ImageView::Alignment::kLeading);
        image_view->SetImageSize(GetPreviewImageSize(image.size()));
        image_view->SetBackground(nullptr);
        break;
    }
  }

  gfx::Size GetMinimumSize() const override { return gfx::Size(); }

  gfx::Size CalculatePreferredSize(
      const views::SizeBounds& available_size) const override {
    return image_type_ == ImageType::kNone
               ? gfx::Size()
               : gfx::Size(kHoverCardWidth, kHoverCardPreviewHeight);
  }

  gfx::Size GetMaximumSize() const override {
    return gfx::Size(kHoverCardWidth, kHoverCardPreviewHeight);
  }

  void AddedToWidget() override {
    if (init_placeholder_image_) {
      SetPlaceholderImage();
      init_placeholder_image_ = false;
    } else if (init_crashed_image_) {
      SetCrashedImage();
      init_crashed_image_ = false;
    }
  }

  void AnimationProgressed(const gfx::Animation* animation) override {
    image_fading_out_->layer()->SetOpacity(1.0 - animation->GetCurrentValue());
  }

  void AnimationEnded(const gfx::Animation* animation) override {
    image_fading_out_->layer()->SetOpacity(0.0f);
    SetImage(image_fading_out_, gfx::ImageSkia(), ImageType::kNone);
  }

  void AnimationCanceled(const gfx::Animation* animation) override {
    AnimationEnded(animation);
  }

  void StartFadeOut() {
    if (!GetVisible() || !GetColorProvider() || !animation_enabled_) {
      return;
    }

    const gfx::ImageSkia old_image = target_image_->GetImage();
    if (image_transition_animation_.is_animating()) {
      const double current_value =
          image_transition_animation_.GetCurrentValue();
      if (current_value <= 0.5) {
        return;
      }
      image_transition_animation_.SetCurrentValue(1.0 - current_value);
      SetImage(image_fading_out_, old_image, image_type_);
      AnimationProgressed(&image_transition_animation_);
    } else {
      SetImage(image_fading_out_, old_image, image_type_);
      image_fading_out_->layer()->SetOpacity(1.0f);
      image_transition_animation_.Start();
    }
  }

  bool animation_enabled_ = true;
  bool init_placeholder_image_ = false;
  bool init_crashed_image_ = false;
  raw_ptr<views::ImageView> target_image_ = nullptr;
  raw_ptr<views::ImageView> image_fading_out_ = nullptr;
  gfx::LinearAnimation image_transition_animation_;
  ImageType image_type_ = ImageType::kNone;
};

BEGIN_METADATA(HoverCardThumbnailView)
END_METADATA

// Copy of Chromium 150's FadeLabel paint behavior: the outgoing label paints
// its own background and fades the background and foreground alpha together.
// This lets the incoming text be revealed continuously rather than leaving an
// opaque card-colored mask until the final animation frame.
class HoverCardFadeLabel : public views::Label {
  METADATA_HEADER(HoverCardFadeLabel, views::Label)

 public:
  HoverCardFadeLabel(int text_style,
                     ui::ColorId foreground_color_id,
                     bool paint_background)
      : views::Label(std::u16string(),
                     views::style::CONTEXT_DIALOG_BODY_TEXT,
                     text_style),
        foreground_color_id_(foreground_color_id),
        paint_background_(paint_background) {}

  void SetFade(double percent) {
    percent = std::min(1.0, percent);
    const ui::ColorProvider* provider = GetColorProvider();
    if (!provider) {
      return;
    }
    const SkAlpha alpha = static_cast<SkAlpha>(
        SK_AlphaOPAQUE * std::clamp(1.0 - percent, 0.0, 1.0));
    SetBackgroundColor(SkColorSetA(
        provider->GetColor(kColorTabHoverCardBackground), alpha));
    SetEnabledColor(
        SkColorSetA(provider->GetColor(foreground_color_id_), alpha));
  }

  void SetFullyVisible() {
    if (const ui::ColorProvider* provider = GetColorProvider()) {
      SetEnabledColor(provider->GetColor(foreground_color_id_));
    }
  }

  void OnPaintBackground(gfx::Canvas* canvas) override {
    if (paint_background_) {
      canvas->DrawColor(GetBackgroundColor());
    } else {
      views::Label::OnPaintBackground(canvas);
    }
  }

 private:
  const ui::ColorId foreground_color_id_;
  const bool paint_background_;
};

BEGIN_METADATA(HoverCardFadeLabel)
END_METADATA

// Minimal copy of Chromium's FadeView<FadeLabel>. The old label masks the new
// one and fades away as BubbleSlideAnimator moves between row anchors.
class CrossfadeLabel : public views::View {
  METADATA_HEADER(CrossfadeLabel, views::View)

 public:
  CrossfadeLabel(int max_lines, int text_style, ui::ColorId color_id)
      : color_id_(color_id) {
    SetUseDefaultFillLayout(true);
    current_ = AddChildView(std::make_unique<HoverCardFadeLabel>(
        text_style, color_id_, /*paint_background=*/false));
    outgoing_ = AddChildView(std::make_unique<HoverCardFadeLabel>(
        text_style, color_id_, /*paint_background=*/true));
    for (views::Label* label : {current_.get(), outgoing_.get()}) {
      label->SetHorizontalAlignment(gfx::ALIGN_LEFT);
      label->SetVerticalAlignment(gfx::ALIGN_TOP);
      label->SetElideBehavior(gfx::ELIDE_TAIL);
      if (max_lines > 1) {
        label->SetMultiLine(true);
        label->SetMaxLines(max_lines);
      }
    }
  }

  void SetText(const std::u16string& text) {
    outgoing_->SetText(current_->GetText());
    current_->SetText(text);
  }

  void SetFade(double percent) {
    percent_ = std::clamp(percent, 0.0, 1.0);
    outgoing_->SetFade(percent_);
    current_->SetFullyVisible();
    if (percent_ == 1.0) {
      outgoing_->SetText(std::u16string());
    }
  }

  gfx::Size CalculatePreferredSize(
      const views::SizeBounds& available_size) const override {
    return current_->GetPreferredSize(available_size);
  }

  gfx::Size GetMinimumSize() const override {
    return current_->GetMinimumSize();
  }

  gfx::Size GetMaximumSize() const override {
    return gfx::Tween::SizeValueBetween(percent_,
                                        outgoing_->GetPreferredSize(),
                                        current_->GetPreferredSize());
  }

 private:
  raw_ptr<HoverCardFadeLabel> current_ = nullptr;
  raw_ptr<HoverCardFadeLabel> outgoing_ = nullptr;
  const ui::ColorId color_id_;
  double percent_ = 1.0;
};

BEGIN_METADATA(CrossfadeLabel)
END_METADATA

}  // namespace

class CmuxRailHoverCardBubble : public views::BubbleDialogDelegateView {
  METADATA_HEADER(CmuxRailHoverCardBubble,
                  views::BubbleDialogDelegateView)

 public:
  CmuxRailHoverCardBubble(views::View* anchor,
                          const WorkspaceHoverCardData& data)
      : BubbleDialogDelegateView(
            views::BubbleAnchor(anchor),
            views::BubbleBorder::Arrow::LEFT_TOP,
            views::BubbleBorder::STANDARD_SHADOW) {
    SetButtons(static_cast<int>(ui::mojom::DialogButton::kNone));
    SetAccessibleWindowRole(ax::mojom::Role::kNone);
    set_margins(gfx::Insets());
    SetCanActivate(false);
#if BUILDFLAG(IS_MAC) || BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_CHROMEOS)
    set_accept_events(false);
#endif
    set_focus_traversable_from_anchor_view(false);

    views::FlexLayout* layout =
        SetLayoutManager(std::make_unique<views::FlexLayout>());
    layout->SetOrientation(views::LayoutOrientation::kVertical)
        .SetMainAxisAlignment(views::LayoutAlignment::kStart)
        .SetCrossAxisAlignment(views::LayoutAlignment::kStretch)
        .SetCollapseMargins(true);

    title_ = AddChildView(std::make_unique<CrossfadeLabel>(
        kHoverCardTitleMaxLines, views::style::STYLE_BODY_3_EMPHASIS,
        kColorTabHoverCardForeground));
    domain_ = AddChildView(std::make_unique<CrossfadeLabel>(
        1, views::style::STYLE_BODY_4, kColorTabHoverCardSecondaryText));
    thumbnail_ =
        AddChildView(std::make_unique<HoverCardThumbnailView>());
    title_->SetProperty(
        views::kFlexBehaviorKey,
        views::FlexSpecification(views::MinimumFlexSizeRule::kScaleToMinimum,
                                 views::MaximumFlexSizeRule::kScaleToMaximum)
            .WithOrder(2));
    thumbnail_->SetProperty(
        views::kFlexBehaviorKey,
        views::FlexSpecification(views::MinimumFlexSizeRule::kScaleToMinimum,
                                 views::MaximumFlexSizeRule::kScaleToMaximum)
            .WithOrder(1));
    thumbnail_->SetAnimationEnabled(UseAnimations());
    thumbnail_->SetRoundedCorners(true, kHoverCardCornerRadius);
    UpdateContent(data);
    if (data.show_preview &&
        !(data.has_preview_source && data.has_preview_data) &&
        !(data.is_discarded && !data.has_preview_data)) {
      // Like Chromium, defer generating the icon until CreateBubble() has
      // attached a ColorProvider. Existing thumbnails intentionally keep an
      // empty image surface while their compressed data is decoded.
      thumbnail_->SetPlaceholderImage();
    }

    // M151 retains this Chromium-150-compatible ownership overload. The
    // controller observes the delegate view and clears all raw pointers when
    // the native widget closes.
    views::BubbleDialogDelegateView::CreateBubble(this);
  }

  void UpdateContent(const WorkspaceHoverCardData& data) {
    title_->SetText(data.title.empty() ? u"Workspace" : data.title);
    domain_->SetText(data.domain);
    domain_->SetVisible(!data.domain.empty());
    if (!data.show_preview ||
        (data.is_discarded && !data.has_preview_data)) {
      thumbnail_->ClearImage();
    } else {
      thumbnail_->SetWaitingForImage();
    }

    gfx::Insets title_margins = kTextMargins;
    if (!data.domain.empty()) {
      title_margins.set_bottom(0);
      gfx::Insets domain_margins = kTextMargins;
      domain_margins.set_top(kTitleDomainSpacing);
      domain_->SetProperty(views::kMarginsKey, domain_margins);
    }
    title_->SetProperty(views::kMarginsKey, title_margins);
    InvalidateLayout();
  }

  void SetTextFade(double percent) {
    title_->SetFade(percent);
    domain_->SetFade(percent);
  }

  void SetTargetWorkspaceImage(gfx::ImageSkia preview_image) {
    thumbnail_->SetTargetWorkspaceImage(std::move(preview_image));
  }

  void SetPlaceholderImage() { thumbnail_->SetPlaceholderImage(); }

  void SetCrashedImage() { thumbnail_->SetCrashedImage(); }

  bool IsSameAnchor(views::View* view) const {
    return GetAnchorView() == view;
  }

  void SetSliding(bool sliding) { sliding_ = sliding; }

 protected:
  void AddedToWidget() override {
    set_adjust_if_offscreen(true);
    GetBubbleFrameView()->SetPreferredArrowAdjustment(
        views::BubbleFrameView::PreferredArrowAdjustment::kOffset);
    GetBubbleFrameView()->set_hit_test_transparent(true);
    GetBubbleFrameView()->SetRoundedCorners(
        gfx::RoundedCornersF(kHoverCardCornerRadius));
    SetTextFade(1.0);
  }

 private:
  gfx::Size CalculatePreferredSize(
      const views::SizeBounds& available_size) const override {
    return gfx::Size(
        kHoverCardWidth,
        GetLayoutManager()->GetPreferredHeightForWidth(this, kHoverCardWidth));
  }

  void OnAnchorBoundsChanged() override {
    if (!sliding_) {
      views::BubbleDialogDelegateView::OnAnchorBoundsChanged();
    }
  }

  raw_ptr<CrossfadeLabel> title_ = nullptr;
  raw_ptr<CrossfadeLabel> domain_ = nullptr;
  raw_ptr<HoverCardThumbnailView> thumbnail_ = nullptr;
  bool sliding_ = false;
};

BEGIN_METADATA(CmuxRailHoverCardBubble)
END_METADATA

class CmuxRailHoverCardController::Impl : public views::ViewObserver,
                                                public views::WidgetObserver {
 public:
  Impl(views::View* rail, RailDelegate* delegate)
      : rail_(rail), delegate_(delegate) {
    CHECK(rail_);
    CHECK(delegate_);
  }

  ~Impl() override {
    target_observation_.Reset();
    rail_widget_observation_.Reset();
    CloseCardImmediately();
  }

  void Update(views::View* target,
              WorkspaceId workspace,
              UpdateType update_type) {
    // Model refresh calls this for every retained row. Data changes update an
    // existing target; they must never make an unrelated row the target.
    if (update_type == UpdateType::kDataChanged &&
        (target_ != target || target_workspace_ != workspace)) {
      return;
    }
    if (target && (!target->GetVisible() || !target->GetWidget())) {
      target = nullptr;
      workspace = kInvalidId;
    }

    if (target_ != target || target_workspace_ != workspace) {
      delayed_show_timer_.Stop();
      StopPreviewRequest();
      target_observation_.Reset();
      if (target) {
        start_hover_card_fade_ = target_ != nullptr;
        target_observation_.Observe(target);
      }
      target_ = target;
      target_workspace_ = workspace;
    }

    if (!hover_card_ &&
        (!target_ || !rail_->GetWidget() ||
         !rail_->GetWidget()->IsVisibleOnScreen())) {
      return;
    }

    if (update_type == UpdateType::kHover && !target_) {
      last_mouse_exit_timestamp_ = base::TimeTicks::Now();
    }

    if (target_) {
      UpdateOrShowCard(update_type);
    } else {
      HideHoverCard();
    }
  }

  void AddedToWidget() {
    if (views::Widget* widget = rail_->GetWidget()) {
      rail_widget_observation_.Observe(widget);
    }
  }

  void RemovedFromWidget() {
    rail_widget_observation_.Reset();
    Update(nullptr, kInvalidId, UpdateType::kEvent);
  }

 private:
  class EventSniffer : public ui::EventObserver {
   public:
    explicit EventSniffer(Impl* controller) : controller_(controller) {
      views::Widget* widget = controller_->rail_->GetWidget();
      event_monitor_ = views::EventMonitor::CreateWindowMonitor(
          this,
          widget ? widget->GetTopLevelWidget()->GetNativeWindow()
                 : gfx::NativeWindow(),
          {ui::EventType::kKeyPressed, ui::EventType::kKeyReleased,
           ui::EventType::kMousePressed, ui::EventType::kMouseReleased,
           ui::EventType::kGestureBegin, ui::EventType::kGestureEnd});
    }

   private:
    void OnEvent(const ui::Event& event) override {
      bool close_hover_card = true;
      if (event.IsKeyEvent()) {
        views::FocusManager* focus_manager =
            controller_->rail_->GetFocusManager();
        views::View* focused =
            focus_manager ? focus_manager->GetFocusedView() : nullptr;
        const bool rail_focused = focused &&
                                  controller_->rail_->Contains(focused);
        const ui::KeyboardCode key = event.AsKeyEvent()->key_code();
        close_hover_card = key == ui::VKEY_RETURN || key == ui::VKEY_ESCAPE ||
                           !rail_focused;
      }
      if (close_hover_card) {
        controller_->Update(nullptr, kInvalidId, UpdateType::kEvent);
      }
    }

    raw_ptr<Impl> controller_;
    std::unique_ptr<views::EventMonitor> event_monitor_;
  };

  void UpdateOrShowCard(UpdateType update_type) {
    if (hover_card_ && GetCardWidget()->IsClosed()) {
      OnCardClosing();
    }
    if (!TargetIsValid()) {
      Update(nullptr, kInvalidId, UpdateType::kEvent);
      return;
    }

    WorkspaceHoverCardData next_data =
        delegate_->GetWorkspaceHoverCardData(target_workspace_);
    if (update_type == UpdateType::kDataChanged) {
      if (!hover_card_ || !fade_animator_ || fade_animator_->IsFadingOut()) {
        return;
      }
      const bool content_changed =
          data_.title != next_data.title || data_.domain != next_data.domain;
      const bool preview_changed =
          data_.source_surface != next_data.source_surface ||
          data_.preview_source_id != next_data.preview_source_id ||
          data_.show_preview != next_data.show_preview ||
          data_.has_preview_source != next_data.has_preview_source ||
          data_.has_preview_data != next_data.has_preview_data ||
          data_.is_discarded != next_data.is_discarded ||
          data_.is_crashed != next_data.is_crashed ||
          data_.preview_readiness != next_data.preview_readiness;
      if (!content_changed && !preview_changed) {
        return;
      }
      data_ = std::move(next_data);
      if (preview_changed) {
        // Chromium re-observes after discard because the ThumbnailImage can
        // move. cmux also does this when the workspace selects another surface.
        StopPreviewRequest();
      }
      UpdateCardContent();
      MaybeStartPreviewRequest(/*is_initial_show=*/false);
#if CHROME_VERSION_MAJOR >= 150
      slide_animator_->UpdateTargetBounds(views::BubbleAnchor(target_.get()));
#else
      slide_animator_->UpdateTargetBounds();
#endif
      return;
    }
    data_ = std::move(next_data);

    if (hover_card_) {
      if (fade_animator_->IsFadingOut()) {
        fade_animator_->CancelFadeOut();
      }
#if CHROME_VERSION_MAJOR >= 150
      fade_animator_->CancelSlide(false);
#endif
      UpdateCardContent();
      MaybeStartPreviewRequest(/*is_initial_show=*/false);
      if (!UseAnimations() ||
          (hover_card_->IsSameAnchor(target_) &&
           !slide_animator_->is_animating())) {
#if CHROME_VERSION_MAJOR >= 150
        slide_animator_->SnapToAnchor(views::BubbleAnchor(target_.get()));
#else
        slide_animator_->SnapToAnchorView(target_.get());
#endif
      } else {
#if CHROME_VERSION_MAJOR >= 150
        slide_animator_->AnimateToAnchor(views::BubbleAnchor(target_.get()));
#else
        slide_animator_->AnimateToAnchorView(target_.get());
#endif
      }
      return;
    }

    const bool is_initial = !ShouldShowImmediately();
    if (is_initial) {
      delayed_show_timer_.Start(
          FROM_HERE, GetShowDelay(rail_->width()),
          base::BindOnce(&Impl::ShowHoverCard,
                         weak_ptr_factory_.GetWeakPtr(), true, target_,
                         target_workspace_));
    } else {
      delayed_show_timer_.Stop();
      ShowHoverCard(false, target_, target_workspace_);
    }
  }

  void ShowHoverCard(bool is_initial,
                     views::View* intended_target,
                     WorkspaceId intended_workspace) {
    if (hover_card_ || target_ != intended_target ||
        target_workspace_ != intended_workspace || !TargetIsValid()) {
      return;
    }

    data_ = delegate_->GetWorkspaceHoverCardData(target_workspace_);
    hover_card_ = new CmuxRailHoverCardBubble(target_, data_);
    hover_card_observation_.Observe(hover_card_.get());
    if (!TargetIsValid() || !hover_card_->GetWidget()) {
      HideHoverCard();
      return;
    }

    event_sniffer_ = std::make_unique<EventSniffer>(this);
    slide_animator_ =
        std::make_unique<views::BubbleSlideAnimator>(hover_card_.get());
    slide_animator_->SetSlideDuration(kHoverCardSlideDuration);
    slide_progressed_subscription_ =
        slide_animator_->AddSlideProgressedCallback(base::BindRepeating(
            &Impl::OnSlideAnimationProgressed,
            weak_ptr_factory_.GetWeakPtr()));
    slide_complete_subscription_ =
        slide_animator_->AddSlideCompleteCallback(base::BindRepeating(
            &Impl::OnSlideAnimationComplete,
            weak_ptr_factory_.GetWeakPtr()));
    fade_animator_ =
        std::make_unique<views::WidgetFadeAnimator>(GetCardWidget());
    fade_complete_subscription_ =
        fade_animator_->AddFadeCompleteCallback(base::BindRepeating(
            &Impl::OnFadeAnimationEnded, weak_ptr_factory_.GetWeakPtr()));

    UpdateCardContent();
#if CHROME_VERSION_MAJOR >= 150
    slide_animator_->UpdateTargetBounds(views::BubbleAnchor(intended_target));
#else
    slide_animator_->UpdateTargetBounds();
#endif
    // Chromium establishes the bubble geometry before subscribing to the
    // thumbnail. BubbleSlideAnimator may synchronously invoke its completion
    // callback while snapping; observing first would make that callback flash
    // a placeholder over an already-cached preview awaiting decode.
    MaybeStartPreviewRequest(is_initial);
    if (!is_initial || !UseAnimations()) {
      GetCardWidget()->Show();
    } else {
#if CHROME_VERSION_MAJOR >= 150
      fade_animator_->FadeIn(
          kHoverCardSlideDistance,
          views::WidgetFadeAnimator::SlideDirection::kTrailing);
#else
      fade_animator_->FadeIn();
#endif
    }
  }

  void UpdateCardContent() {
    if (!hover_card_) {
      return;
    }
    if (start_hover_card_fade_) {
      hover_card_->SetTextFade(0.0);
      start_hover_card_fade_ = false;
    }
    hover_card_->UpdateContent(data_);
  }

  void MaybeStartPreviewRequest(bool is_initial_show) {
    if (!hover_card_) {
      return;
    }
    if (!data_.show_preview) {
      StopPreviewRequest();
      return;
    }
    if (data_.is_discarded && !data_.has_preview_data) {
      StopPreviewRequest();
      return;
    }
    if (data_.is_crashed) {
      hover_card_->SetCrashedImage();
      StopPreviewRequest();
      return;
    }
    if (!data_.has_preview_source) {
      hover_card_->SetPlaceholderImage();
      StopPreviewRequest();
      return;
    }
    if (preview_request_ &&
        preview_request_workspace_ == target_workspace_ &&
        preview_request_source_surface_ == data_.source_surface &&
        preview_request_source_id_ == data_.preview_source_id) {
      return;
    }

    preview_wait_state_ = PreviewWaitState::kWaitingWithoutPlaceholder;
    const base::TimeDelta capture_delay =
        is_initial_show || data_.has_preview_data
            ? base::TimeDelta()
            : GetPreviewImageCaptureDelay(data_.preview_readiness);
    if (capture_delay.is_zero()) {
      StartPreviewRequest(target_workspace_, data_.source_surface,
                          data_.preview_source_id);
      return;
    }
    if (delayed_show_timer_.IsRunning()) {
      return;
    }

    // Chromium stops showing the old image immediately when capture itself is
    // delayed. The slide-progress crossfade below is reserved for zero-delay
    // requests whose asynchronous decompression has not completed yet.
    if (preview_wait_state_ == PreviewWaitState::kWaitingWithoutPlaceholder) {
      hover_card_->SetPlaceholderImage();
      preview_wait_state_ = PreviewWaitState::kWaitingWithPlaceholder;
    }
    delayed_show_timer_.Start(
        FROM_HERE, capture_delay,
        base::BindOnce(&Impl::StartPreviewRequest,
                       weak_ptr_factory_.GetWeakPtr(), target_workspace_,
                       data_.source_surface, data_.preview_source_id));
  }

  void StartPreviewRequest(WorkspaceId intended_workspace,
                           SurfaceTabId intended_source_surface,
                           uintptr_t intended_source_id) {
    if (!hover_card_ || target_workspace_ != intended_workspace ||
        data_.source_surface != intended_source_surface ||
        data_.preview_source_id != intended_source_id ||
        !TargetIsValid() ||
        preview_wait_state_ == PreviewWaitState::kNotWaiting) {
      return;
    }

    preview_request_workspace_ = intended_workspace;
    preview_request_source_surface_ = intended_source_surface;
    preview_request_source_id_ = intended_source_id;
    preview_request_ = delegate_->RequestWorkspaceHoverCardPreview(
        intended_workspace, intended_source_surface, intended_source_id,
        base::BindRepeating(&Impl::OnPreviewImageAvailable,
                            weak_ptr_factory_.GetWeakPtr(),
                            intended_workspace, intended_source_surface,
                            intended_source_id));
    if (!preview_request_) {
      preview_request_workspace_ = kInvalidId;
      preview_request_source_surface_ = kInvalidId;
      preview_request_source_id_ = 0;
      preview_wait_state_ = PreviewWaitState::kNotWaiting;
      hover_card_->SetPlaceholderImage();
    }
  }

  void StopPreviewRequest() {
    delayed_show_timer_.Stop();
    preview_request_.reset();
    preview_request_workspace_ = kInvalidId;
    preview_request_source_surface_ = kInvalidId;
    preview_request_source_id_ = 0;
    preview_wait_state_ = PreviewWaitState::kNotWaiting;
  }

  void OnPreviewImageAvailable(WorkspaceId intended_workspace,
                               SurfaceTabId intended_source_surface,
                               uintptr_t intended_source_id,
                               gfx::ImageSkia preview_image) {
    if (!hover_card_ || target_workspace_ != intended_workspace ||
        preview_request_workspace_ != intended_workspace ||
        data_.source_surface != intended_source_surface ||
        data_.preview_source_id != intended_source_id ||
        preview_request_source_surface_ != intended_source_surface ||
        preview_request_source_id_ != intended_source_id ||
        preview_image.isNull()) {
      return;
    }
    preview_wait_state_ = PreviewWaitState::kNotWaiting;
    hover_card_->SetTargetWorkspaceImage(std::move(preview_image));
  }

  void HideHoverCard() {
    StopPreviewRequest();
    if (!hover_card_ || !GetCardWidget() || GetCardWidget()->IsClosed()) {
      return;
    }
#if CHROME_VERSION_MAJOR >= 150
    bool slide_on_fade_out =
        slide_animator_ && !slide_animator_->is_animating();
#endif
    if (fade_animator_ && fade_animator_->IsFadingIn()) {
#if CHROME_VERSION_MAJOR >= 150
      slide_on_fade_out = false;
#endif
      fade_animator_->CancelFadeIn();
    }
    if (slide_animator_) {
      slide_animator_->StopAnimation();
    }
    if (!UseAnimations() || !fade_animator_) {
      GetCardWidget()->Close();
      return;
    }
    if (fade_animator_->IsFadingOut()) {
      return;
    }
#if CHROME_VERSION_MAJOR >= 150
    fade_animator_->FadeOut(
        kHoverCardSlideDistance,
        slide_on_fade_out
            ? views::WidgetFadeAnimator::SlideDirection::kTrailing
            : views::WidgetFadeAnimator::SlideDirection::kNone);
#else
    fade_animator_->FadeOut();
#endif
  }

  bool ShouldShowImmediately() const {
    const base::TimeDelta elapsed =
        base::TimeTicks::Now() - last_mouse_exit_timestamp_;
    const bool within_buffer = !last_mouse_exit_timestamp_.is_null() &&
                               elapsed <= kShowWithoutDelayTimeBuffer;
    views::FocusManager* focus_manager = target_->GetFocusManager();
    return within_buffer || target_->HasFocus() ||
           (focus_manager &&
            target_->Contains(focus_manager->GetFocusedView()));
  }

  bool TargetIsValid() const {
    return target_ && target_workspace_ != kInvalidId &&
           target_->GetVisible() && target_->GetWidget() &&
           target_->IsDrawn();
  }

  views::Widget* GetCardWidget() const {
    return hover_card_ ? hover_card_->GetWidget() : nullptr;
  }

  void CloseCardImmediately() {
    delayed_show_timer_.Stop();
    StopPreviewRequest();
    event_sniffer_.reset();
    slide_progressed_subscription_ = {};
    slide_complete_subscription_ = {};
    fade_complete_subscription_ = {};
    slide_animator_.reset();
    fade_animator_.reset();
    hover_card_observation_.Reset();
    if (hover_card_ && hover_card_->GetWidget()) {
      hover_card_->GetWidget()->CloseNow();
    }
    hover_card_ = nullptr;
  }

  void OnCardClosing() {
    delayed_show_timer_.Stop();
    StopPreviewRequest();
    hover_card_observation_.Reset();
    event_sniffer_.reset();
    slide_progressed_subscription_ = {};
    slide_complete_subscription_ = {};
    fade_complete_subscription_ = {};
    slide_animator_.reset();
    fade_animator_.reset();
    hover_card_ = nullptr;
  }

  void OnFadeAnimationEnded(
      views::WidgetFadeAnimator*,
      views::WidgetFadeAnimator::FadeType fade_type) {
    if (fade_type == views::WidgetFadeAnimator::FadeType::kFadeOut &&
        GetCardWidget()) {
      GetCardWidget()->Close();
    }
  }

  void OnSlideAnimationProgressed(views::BubbleSlideAnimator*, double value) {
    if (hover_card_) {
      hover_card_->SetTextFade(value);
      hover_card_->SetSliding(value > 0.0);
    }
    if (hover_card_ &&
        preview_wait_state_ ==
            PreviewWaitState::kWaitingWithoutPlaceholder &&
        value >= kPreviewImageCrossfadeStart) {
      hover_card_->SetPlaceholderImage();
      preview_wait_state_ = PreviewWaitState::kWaitingWithPlaceholder;
    }
  }

  void OnSlideAnimationComplete(views::BubbleSlideAnimator*) {
    if (hover_card_) {
      hover_card_->SetTextFade(1.0);
      hover_card_->SetSliding(false);
    }
    if (hover_card_ &&
        preview_wait_state_ ==
            PreviewWaitState::kWaitingWithoutPlaceholder) {
      hover_card_->SetPlaceholderImage();
      preview_wait_state_ = PreviewWaitState::kWaitingWithPlaceholder;
    }
  }

  void OnViewIsDeleting(views::View* observed_view) override {
    if (hover_card_ == observed_view) {
      OnCardClosing();
    } else if (target_ == observed_view) {
      Update(nullptr, kInvalidId, UpdateType::kEvent);
    }
  }

  void OnViewVisibilityChanged(views::View* observed_view,
                               views::View*,
                               bool visible) override {
    if (target_ == observed_view && !visible) {
      Update(nullptr, kInvalidId, UpdateType::kEvent);
    }
  }

  void OnWidgetActivationChanged(views::Widget*, bool) override {
    Update(nullptr, kInvalidId, UpdateType::kEvent);
  }

  void OnWidgetDestroying(views::Widget*) override {
    rail_widget_observation_.Reset();
    CloseCardImmediately();
  }

  raw_ptr<views::View> rail_;
  raw_ptr<RailDelegate> delegate_;
  raw_ptr<views::View> target_ = nullptr;
  WorkspaceId target_workspace_ = kInvalidId;
  WorkspaceHoverCardData data_;
  raw_ptr<CmuxRailHoverCardBubble> hover_card_ = nullptr;
  bool start_hover_card_fade_ = false;
  base::TimeTicks last_mouse_exit_timestamp_;
  base::OneShotTimer delayed_show_timer_;
  std::unique_ptr<WorkspaceHoverCardPreviewRequest> preview_request_;
  WorkspaceId preview_request_workspace_ = kInvalidId;
  SurfaceTabId preview_request_source_surface_ = kInvalidId;
  uintptr_t preview_request_source_id_ = 0;
  PreviewWaitState preview_wait_state_ = PreviewWaitState::kNotWaiting;
  std::unique_ptr<EventSniffer> event_sniffer_;
  std::unique_ptr<views::BubbleSlideAnimator> slide_animator_;
  std::unique_ptr<views::WidgetFadeAnimator> fade_animator_;
  base::CallbackListSubscription slide_progressed_subscription_;
  base::CallbackListSubscription slide_complete_subscription_;
  base::CallbackListSubscription fade_complete_subscription_;
  base::ScopedObservation<views::View, views::ViewObserver>
      target_observation_{this};
  base::ScopedObservation<views::View, views::ViewObserver>
      hover_card_observation_{this};
  base::ScopedObservation<views::Widget, views::WidgetObserver>
      rail_widget_observation_{this};
  base::WeakPtrFactory<Impl> weak_ptr_factory_{this};
};

CmuxRailHoverCardController::CmuxRailHoverCardController(
    views::View* rail,
    RailDelegate* delegate)
    : impl_(std::make_unique<Impl>(rail, delegate)) {}

CmuxRailHoverCardController::~CmuxRailHoverCardController() = default;

void CmuxRailHoverCardController::Update(views::View* target,
                                         WorkspaceId workspace,
                                         UpdateType update_type) {
  impl_->Update(target, workspace, update_type);
}

void CmuxRailHoverCardController::AddedToWidget() {
  impl_->AddedToWidget();
}

void CmuxRailHoverCardController::RemovedFromWidget() {
  impl_->RemovedFromWidget();
}

}  // namespace cmux
