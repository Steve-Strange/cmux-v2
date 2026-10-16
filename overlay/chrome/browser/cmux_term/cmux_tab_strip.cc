// Copyright 2012, 2015, 2022, 2023, 2025 The Chromium Authors
// Copyright 2026 Manaflow, Inc.
// Portions derived from Helium; Helium contributors retain copyright.
// SPDX-License-Identifier: GPL-3.0-or-later AND GPL-3.0-only AND BSD-3-Clause
//
// See docs/source-provenance.md for the licensed regions and source pins.

#include "chrome/browser/cmux_term/cmux_tab_strip.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <optional>
#include <set>
#include <utility>

#include "base/functional/bind.h"
#include "base/functional/callback.h"
#include "base/scoped_observation.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/single_thread_task_runner.h"
#include "base/time/time.h"
#include "cc/paint/paint_flags.h"
#include "chrome/browser/cmux_term/cmux_easing.h"
#include "chrome/browser/cmux_term/window_layout.h"
#include "chrome/common/chrome_version.h"
#include "components/vector_icons/vector_icons.h"
#include "third_party/skia/include/core/SkPath.h"
#include "third_party/skia/include/core/SkPathBuilder.h"
#include "third_party/skia/include/core/SkRRect.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/base/models/image_model.h"
#include "ui/base/mojom/menu_source_type.mojom.h"
#include "ui/compositor/layer.h"
#include "ui/events/event_utils.h"
#include "ui/gfx/animation/animation.h"
#include "ui/gfx/animation/animation_delegate.h"
#include "ui/gfx/animation/slide_animation.h"
#include "ui/gfx/animation/tween.h"
#include "ui/gfx/canvas.h"
#include "ui/gfx/color_utils.h"
#include "ui/gfx/geometry/rect_f.h"
#include "ui/gfx/geometry/skia_conversions.h"
#include "ui/gfx/geometry/size.h"
#include "ui/gfx/geometry/transform.h"
#include "ui/menus/simple_menu_model.h"
#include "ui/views/accessibility/view_accessibility.h"
#include "ui/views/animation/animation_delegate_views.h"
#include "ui/views/animation/bounds_animator.h"
#include "ui/views/animation/flood_fill_ink_drop_ripple.h"
#include "ui/views/animation/ink_drop.h"
#include "ui/views/animation/ink_drop_highlight.h"
#include "ui/views/animation/ink_drop_state.h"
#include "ui/views/background.h"
#include "ui/views/border.h"
#include "ui/views/controls/button/label_button.h"
#include "ui/views/controls/focus_ring.h"
#include "ui/views/controls/highlight_path_generator.h"
#include "ui/views/controls/image_view.h"
#include "ui/views/controls/label.h"
#include "ui/views/controls/menu/menu_runner.h"
#include "ui/views/controls/throbber.h"
#include "ui/views/masked_targeter_delegate.h"
#include "ui/views/rect_based_targeting_utils.h"
#include "ui/views/view_targeter.h"
#include "ui/views/view_observer.h"

namespace cmux {

std::optional<ui::Accelerator> TabStripDelegate::GetNewTabAccelerator(
    SurfaceKind) {
  return std::nullopt;
}

bool TabStripDelegate::IsSidebarOnRight() const {
  return false;
}

void TabStripDelegate::OnSetSidebarOnRight(bool) {}

namespace {

constexpr SkAlpha kSeparatorAlpha = 0x40;
// Keep these in lockstep with classic Helium's tab geometry. Helium names its
// layout constant kTabHeight=31, but Chromium gives each Tab a 34-DIP
// kTabStripHeight view/hit box. The detached fill begins at y=3 and is 28 DIP
// tall, leaving 3 DIP above and below. Source at Helium 3d042e6:
// patches/helium/ui/{layout-constants,tabs}.patch.
constexpr int kStripPad = 3;
constexpr int kTabSpacing = 1;
constexpr int kStripHeight = 34;
constexpr int kTabViewHeight = kStripHeight;
constexpr int kTabVisualTop = 3;
constexpr int kTabVisualHeight = 28;
constexpr int kIconSize = 16;
constexpr int kTabMinActiveWidth = 34;
constexpr int kTabMinInactiveWidth = 20;
constexpr int kTabMaxWidth = 206;
constexpr int kTabLeadingInset = 10;
constexpr int kTabTrailingInset = 6;
constexpr int kTabPreTitlePadding = 6;
constexpr int kTabAfterTitlePadding = 4;
constexpr int kTabCloseIconSize = 14;
constexpr int kTabCloseButtonSize = 26;
constexpr int kInactiveCloseButtonMinWidth = 76;
constexpr int kNewTabButtonWidth = 28;
constexpr int kNewTabButtonCornerRadius = 8;
constexpr int kNewTabButtonLeadingMargin = 4;
constexpr int kNewTabButtonTabEdgeOverlap =
    kNewTabButtonCornerRadius - kStripPad - kNewTabButtonLeadingMargin;
// Helium paints the detached tab squircle four DIPs inside the Tab view's
// logical trailing edge. cmux's detached pill paints through its View edge,
// so preserve the same *visible* pill-to-button spacing by compensating for
// that inset when positioning the control.
constexpr int kHeliumTabPaintHorizontalInset = 4;
constexpr int kNewTabButtonPaintedTabGap =
    kHeliumTabPaintHorizontalInset - kNewTabButtonTabEdgeOverlap;
constexpr int kNewTabButtonTrailingMargin = 4;
constexpr SkAlpha kNewTabButtonHoverAlpha = 0x73;  // 45% of 255.
constexpr int kPaneDragHandleMinWidth = 27;
constexpr int kInsertionGap = 10;
constexpr float kTabPillRadius = 8.0f;
constexpr base::TimeDelta kChromeTabBoundsAnimationDuration =
    base::Milliseconds(200);

enum NewTabMenuCommand {
  kCommandNewBrowserTab = 100,
  kCommandNewTerminalTab,
};

enum BrowserLayoutMenuCommand {
  kCommandBrowserLayout = 200,
  kCommandTabsOnRightSide,
};

std::u16string TabTitle(const SurfaceTab& tab) {
  if (!tab.title.empty()) {
    return base::UTF8ToUTF16(tab.title);
  }
  return tab.kind == SurfaceKind::kTerminal ? u"Terminal" : u"New Tab";
}

base::TimeDelta ScaledSpringRunDuration(int animation_ms, int perceptual_ms) {
  constexpr int kBaselineAnimationMs = 160;
  constexpr int kSpringRunNumerator = 135;
  constexpr int kSpringRunDenominator = 100;
  constexpr int kDenominator = kBaselineAnimationMs * kSpringRunDenominator;
  const long long numerator =
      static_cast<long long>(std::max(0, animation_ms)) * perceptual_ms *
      kSpringRunNumerator;
  return base::Milliseconds(
      static_cast<int>((numerator + kDenominator / 2) / kDenominator));
}

base::TimeDelta ScaledHoverRunDuration(int animation_ms) {
  if (animation_ms <= 0) {
    return base::Milliseconds(0);
  }
  constexpr int kBaselineAnimationMs = 160;
  constexpr int kTargetHoverMs = 100;
  const int scaled = animation_ms * kTargetHoverMs / kBaselineAnimationMs;
  return base::Milliseconds(std::clamp(scaled, 80, 120));
}

// Classic Helium's NewTabButton is a full-strip-height host containing a
// 28-DIP round rect inset 3 DIP above and below. Its radius is 8 DIP, not the
// circular 14-DIP Chromium default. The 16-DIP add glyph uses Chromium's
// components vector icon: kAddIcon on our Chromium 149 base is the same filled
// 1.7-DIP-arm path renamed kAddOldIcon in Helium's Chromium 150 base.
// cmux's cross-platform UI target cannot depend on the BrowserWindowInterface-
// backed NewTabButton, so this copies its paint geometry while keeping dispatch
// in cmux. Source: Helium bee47818, Chromium 150.0.7871.128.
class CmuxNewTabButton : public views::LabelButton,
                         public views::MaskedTargeterDelegate {
  METADATA_HEADER(CmuxNewTabButton, views::LabelButton)

 public:
  explicit CmuxNewTabButton(CmuxTabStrip* owner)
      : views::LabelButton(
            base::BindRepeating(
                [](CmuxTabStrip* strip) {
                  if (strip) {
                    strip->ShowNewTabMenu();
                  }
                },
                owner),
            std::u16string()),
        owner_(owner) {
    SetPreferredSize(gfx::Size(kNewTabButtonWidth, kStripHeight));
    SetImageCentered(true);
    SetHorizontalAlignment(gfx::ALIGN_CENTER);
    // Chromium makes the 28-DIP control fill the contents bounds, then adds
    // an empty 3-DIP border above and below to form Helium's 28 x 34 host.
    // Using a border (instead of merely painting at y=3) also makes image
    // layout, focus clipping, and shaped hit testing identical.
    SetBorder(views::CreateEmptyBorder(gfx::Insets::VH(kTabVisualTop, 0)));
    SetEventTargeter(std::make_unique<views::ViewTargeter>(this));
    SetFocusBehavior(FocusBehavior::ACCESSIBLE_ONLY);
    SetInstallFocusRingOnFocus(true);
    views::InstallRoundRectHighlightPathGenerator(
        this, gfx::Insets::VH(kTabVisualTop, 0),
        kNewTabButtonCornerRadius);
    views::InkDrop::Get(this)->SetMode(views::InkDropHost::InkDropMode::ON);
    views::InkDrop::Get(this)->SetLayerRegion(views::LayerRegion::kAbove);
    InstallInkDropCallbacks();
    SetTooltipText(u"New tab options");
    GetViewAccessibility().SetName(u"New tab options");
  }

  void SetThemeColors(const TabStripThemeColors& colors) {
    colors_ = colors;
#if CHROME_VERSION_MAJOR >= 151
    const gfx::VectorIcon& add_icon = vector_icons::kAddOldIcon;
#else
    const gfx::VectorIcon& add_icon = vector_icons::kAddIcon;
#endif
    const ui::ImageModel icon = ui::ImageModel::FromVectorIcon(
        add_icon, colors_.plus_text, kIconSize);
    // Helium installs one ImageModel for every interaction state. Only the
    // rounded background changes on hover/press.
    SetImageModel(views::Button::STATE_NORMAL, icon);
    SetImageModel(views::Button::STATE_HOVERED, icon);
    SetImageModel(views::Button::STATE_PRESSED, icon);
    InstallInkDropCallbacks();
    SchedulePaint();
  }

  bool GetHitTestMask(SkPath* mask) const override {
    *mask = SkPath::RRect(SkRRect::MakeRectXY(
        gfx::RectToSkRect(GetContentsBounds()), kNewTabButtonCornerRadius,
        kNewTabButtonCornerRadius));
    return true;
  }

  void NotifyClick(const ui::Event& event) override {
    views::LabelButton::NotifyClick(event);
    views::InkDrop::Get(this)->GetInkDrop()->AnimateToState(
        views::InkDropState::ACTION_TRIGGERED);
  }

 private:
  void InstallInkDropCallbacks() {
    views::InkDrop::Get(this)->SetCreateRippleCallback(base::BindRepeating(
        [](CmuxNewTabButton* host)
            -> std::unique_ptr<views::InkDropRipple> {
          const SkColor pressed = host->colors_.tab_active_bg;
          return std::make_unique<views::FloodFillInkDropRipple>(
              views::InkDrop::Get(host), host->size(),
              host->GetLocalBounds().CenterPoint(),
              SkColorSetA(pressed, SK_AlphaOPAQUE),
              SkColorGetA(pressed) / static_cast<float>(SK_AlphaOPAQUE));
        },
        this));
    views::InkDrop::Get(this)->SetCreateHighlightCallback(base::BindRepeating(
        [](CmuxNewTabButton* host) {
          // Helium's kColorTabBackgroundInactiveHoverFrameActive is the
          // active-tab background composited over the strip at 45% alpha.
          const SkColor hovered = host->colors_.tab_active_bg;
          auto highlight = std::make_unique<views::InkDropHighlight>(
              gfx::SizeF(host->size()),
              SkColorSetA(hovered, SK_AlphaOPAQUE));
          highlight->set_visible_opacity(kNewTabButtonHoverAlpha /
                                         static_cast<float>(SK_AlphaOPAQUE));
          return highlight;
        },
        this));
  }

  raw_ptr<CmuxTabStrip> owner_;
  TabStripThemeColors colors_;
};

BEGIN_METADATA(CmuxNewTabButton)
END_METADATA

class TabGlyphView : public views::View {
  METADATA_HEADER(TabGlyphView, views::View)

 public:
  enum class Kind { kGlobe, kTerminal };

  TabGlyphView(Kind kind, CmuxTabStrip* owner) : kind_(kind), owner_(owner) {
    SetPreferredSize(gfx::Size(kIconSize, kIconSize));
  }

  void OnPaint(gfx::Canvas* canvas) override {
    views::View::OnPaint(canvas);
    cc::PaintFlags flags;
    flags.setAntiAlias(true);
    flags.setStyle(cc::PaintFlags::kStroke_Style);
    flags.setStrokeWidth(1.4f);
    flags.setColor(owner_ ? owner_->theme_colors().tab_idle_text
                          : kDefaultTabIdleText);
    if (kind_ == Kind::kTerminal) {
      canvas->DrawLine(gfx::PointF(3.5f, 4.5f), gfx::PointF(8.5f, 8.0f), flags);
      canvas->DrawLine(gfx::PointF(8.5f, 8.0f), gfx::PointF(3.5f, 11.5f),
                       flags);
      canvas->DrawLine(gfx::PointF(9.5f, 11.5f), gfx::PointF(13.5f, 11.5f),
                       flags);
      return;
    }

    canvas->DrawCircle(gfx::PointF(8.0f, 8.0f), 6.0f, flags);
    canvas->DrawLine(gfx::PointF(2.5f, 8.0f), gfx::PointF(13.5f, 8.0f), flags);
    canvas->DrawLine(gfx::PointF(8.0f, 2.5f), gfx::PointF(8.0f, 13.5f), flags);
    canvas->DrawLine(gfx::PointF(4.5f, 5.0f), gfx::PointF(11.5f, 5.0f), flags);
    canvas->DrawLine(gfx::PointF(4.5f, 11.0f), gfx::PointF(11.5f, 11.0f),
                     flags);
  }

 private:
  Kind kind_;
  raw_ptr<CmuxTabStrip> owner_;
};

BEGIN_METADATA(TabGlyphView)
END_METADATA

std::unique_ptr<views::View> CreateIconSlot(const TabVisual& visual,
                                            CmuxTabStrip* owner) {
  if (visual.tab.kind == SurfaceKind::kWeb && visual.tab.loading) {
    auto throbber = std::make_unique<views::Throbber>(kIconSize);
    throbber->Start();
    return throbber;
  }
  if (visual.tab.kind == SurfaceKind::kTerminal) {
    return std::make_unique<TabGlyphView>(TabGlyphView::Kind::kTerminal, owner);
  }
  if (!visual.favicon.isNull()) {
    auto image = std::make_unique<views::ImageView>();
    image->SetImage(ui::ImageModel::FromImageSkia(visual.favicon));
    image->SetImageSize(gfx::Size(kIconSize, kIconSize));
    return image;
  }
  return std::make_unique<TabGlyphView>(TabGlyphView::Kind::kGlobe, owner);
}

// Helium's TabCloseButton keeps Chrome's larger 12-DIP layout padding but
// renders the rounded-icons variant of Chrome's close vector at 14 DIP. The
// padding deliberately belongs to the parent tab for ordinary mouse hits;
// only the 14x16 contents rect closes, which prevents near-miss tab closes.
class CmuxTabCloseButton : public views::LabelButton,
                           public views::MaskedTargeterDelegate {
  METADATA_HEADER(CmuxTabCloseButton, views::LabelButton)

 public:
  CmuxTabCloseButton(CmuxTabStrip* owner, SurfaceTabId tab)
      : views::LabelButton(
            base::BindRepeating(
                [](CmuxTabStrip* strip, SurfaceTabId id) {
                  if (strip) {
                    strip->CloseTab(id);
                  }
                },
                owner, tab),
            std::u16string()) {
    SetPreferredSize(
        gfx::Size(kTabCloseButtonSize, kTabCloseButtonSize));
    SetBorder(views::CreateEmptyBorder(gfx::Insets::VH(5, 6)));
    SetEventTargeter(std::make_unique<views::ViewTargeter>(this));
    SetFocusBehavior(FocusBehavior::ACCESSIBLE_ONLY);
    SetInstallFocusRingOnFocus(true);
    views::InstallRoundRectHighlightPathGenerator(this, gfx::Insets(6), 4);
    GetViewAccessibility().SetName(u"Close tab");
  }

  void SetColors(SkColor foreground, SkColor background) {
    foreground_ = foreground;
    background_ = background;
    SchedulePaint();
  }

  void OnPaintBackground(gfx::Canvas* canvas) override {
    if (GetState() != views::Button::STATE_HOVERED &&
        GetState() != views::Button::STATE_PRESSED) {
      return;
    }
    const SkColor contrast =
        color_utils::GetColorWithMaxContrast(background_);
    constexpr float kHoverOpacity = 0.16f;
    constexpr float kPressedOpacity = 0.14f;
    const float opacity = GetState() == views::Button::STATE_PRESSED
                              ? kPressedOpacity
                              : kHoverOpacity;
    cc::PaintFlags fill;
    fill.setAntiAlias(true);
    fill.setColor(SkColorSetA(
        contrast, static_cast<U8CPU>(std::lround(opacity * 255.0f))));
    const gfx::Rect contents = GetContentsBounds();
    const gfx::RectF hover(contents.x(), contents.y() + 1,
                           kTabCloseIconSize, kTabCloseIconSize);
    canvas->DrawRoundRect(hover, 4.0f, fill);
  }

  void PaintButtonContents(gfx::Canvas* canvas) override {
    // Exact kCloseTabChromeRefreshIcon path, scaled from its 16-DIP canvas to
    // Helium's rounded-icons 14-DIP canvas.
    constexpr float kScale = 14.0f / 16.0f;
    const gfx::Rect contents = GetContentsBounds();
    const float ox = static_cast<float>(contents.x());
    const float oy = static_cast<float>(contents.y() + 1);
    auto p = [&](float x, float y) {
      return SkPoint::Make(ox + x * kScale, oy + y * kScale);
    };
    SkPathBuilder builder;
    builder.moveTo(p(10.94f, 4.0f));
    builder.lineTo(p(4.0f, 10.94f));
    builder.lineTo(p(5.06f, 12.0f));
    builder.lineTo(p(12.0f, 5.07f));
    builder.close();
    builder.moveTo(p(5.06f, 4.0f));
    builder.lineTo(p(4.0f, 5.06f));
    builder.lineTo(p(10.94f, 12.0f));
    builder.lineTo(p(12.0f, 10.93f));
    builder.close();
    cc::PaintFlags icon;
    icon.setAntiAlias(true);
    icon.setColor(foreground_);
    canvas->DrawPath(builder.detach(), icon);
  }

  bool OnMousePressed(const ui::MouseEvent& event) override {
    if (event.IsMiddleMouseButton()) {
      return false;
    }
    return views::LabelButton::OnMousePressed(event);
  }

 private:
  views::View* TargetForRect(views::View* root,
                             const gfx::Rect& rect) override {
    CHECK_EQ(root, this);
    if (!views::UsePointBasedTargeting(rect)) {
      return views::ViewTargeterDelegate::TargetForRect(root, rect);
    }
    return GetMirroredRect(GetContentsBounds()).Intersects(rect) ? this
                                                                 : parent();
  }

  bool GetHitTestMask(SkPath* mask) const override {
    *mask = SkPath::Rect(
        gfx::RectToSkRect(GetMirroredRect(GetContentsBounds())));
    return true;
  }

  SkColor foreground_ = kDefaultTabIdleText;
  SkColor background_ = kDefaultTabActiveBg;
};

BEGIN_METADATA(CmuxTabCloseButton)
END_METADATA

// One tab pill: [ icon title  × ]. Left selects/drags, middle closes, right
// opens the context menu.
class CmuxTab : public views::View, public gfx::AnimationDelegate {
  METADATA_HEADER(CmuxTab, views::View)

 public:
  CmuxTab(const TabVisual& visual,
          bool active,
          bool animations,
          int animation_ms,
          CmuxTabStrip* owner)
      : owner_(owner),
        tab_id_(visual.tab.id),
        active_(active),
        hover_animation_(this) {
    SetNotifyEnterExitOnChild(true);
    SetAnimationConfig(animations, animation_ms);
    icon_ = AddChildView(CreateIconSlot(visual, owner_));

    const std::u16string title = TabTitle(visual.tab);
    label_ = AddChildView(std::make_unique<views::Label>(title));
    label_->SetHorizontalAlignment(gfx::ALIGN_LEFT);
    label_->SetElideBehavior(gfx::ELIDE_TAIL);
    // The insertion-gap animation paints this pill to a non-opaque layer;
    // Label DCHECKs subpixel-rendered text under such a layer (label.cc), so
    // draw grayscale-antialiased, matching the rail rows (cmux_rail.cc).
    label_->SetSubpixelRenderingEnabled(false);
    close_ = AddChildView(
        std::make_unique<CmuxTabCloseButton>(owner_, visual.tab.id));
    ApplyThemeColors();

    // Chrome tab width is a strip-level layout property, not a function of
    // title length. This also prevents a navigation title arriving during an
    // opening animation from changing its final frame.
    SetPreferredSize(gfx::Size(kTabMaxWidth, kTabViewHeight));
    kind_ = visual.tab.kind;
    loading_ = visual.tab.loading;
    favicon_ = visual.favicon;
  }

  SurfaceTabId tab_id() const { return tab_id_; }
  bool active() const { return active_; }
  bool hovered() const { return hovered_; }
  bool showing_hover_fill() const {
    return !active_ && hover_animation_.GetCurrentValue() > 0.0;
  }
  bool opening_reveal() const { return opening_visual_width_.has_value(); }

  // Cmux tabs are detached pills with positive spacing, unlike Chromium tabs
  // whose 18-DIP overlap is visually consumed by their tab path. Animating a
  // detached pill's own layout from 18 DIP makes its round rect and children
  // look scaled. Keep the already-laid-out, final-width visual fixed while the
  // outer View bounds reveal it from left to right instead.
  void BeginOpeningReveal(int final_visual_width) {
    opening_visual_width_ = std::max(0, final_visual_width);
    SchedulePaint();
  }

  void EndOpeningReveal() {
    if (!opening_visual_width_.has_value()) {
      return;
    }
    opening_visual_width_.reset();
    InvalidateLayout();
    SchedulePaint();
  }

  void UpdateVisual(const TabVisual& visual, bool active) {
    const bool icon_changed =
        kind_ != visual.tab.kind || loading_ != visual.tab.loading ||
        !favicon_.BackedBySameObjectAs(visual.favicon);
    if (icon_changed) {
      // Loading/favicon updates frequently arrive during the opening
      // animation. Preserve the fixed final-width child geometry rather than
      // allowing the replacement icon to appear at an empty/default bound.
      const gfx::Rect icon_bounds = icon_->bounds();
      // Keep the removed child alive while clearing the non-owning member.
      // Destroying it first leaves icon_ dangling until the replacement
      // assignment, which BackupRefPtr correctly traps.
      auto old_icon = RemoveChildViewT(icon_.get());
      icon_ = nullptr;
      old_icon.reset();
      icon_ = AddChildViewAt(CreateIconSlot(visual, owner_), 0);
      if (opening_visual_width_.has_value()) {
        icon_->SetBoundsRect(icon_bounds);
      }
      kind_ = visual.tab.kind;
      loading_ = visual.tab.loading;
      favicon_ = visual.favicon;
    }

    const std::u16string title = TabTitle(visual.tab);
    if (label_->GetText() != title) {
      label_->SetText(title);
    }
    if (active_ != active) {
      active_ = active;
      UpdateHoverState();
    }
    ApplyThemeColors();
    InvalidateLayout();
  }

  void ApplyThemeColors() {
    const TabStripThemeColors fallback;
    const TabStripThemeColors& colors =
        owner_ ? owner_->theme_colors() : fallback;
    if (label_) {
      label_->SetEnabledColor(active_ ? colors.tab_active_text
                                      : colors.tab_idle_text);
    }
    if (close_) {
      close_->SetColors(active_ ? colors.tab_active_text : colors.tab_idle_text,
                        active_ ? colors.tab_active_bg : colors.tab_hover_bg);
    }
    UpdateCloseState();
    SchedulePaint();
  }

  void SetAnimationConfig(bool animations, int animation_ms) {
    animations_ = animations;
    animation_ms_ = std::max(0, animation_ms);
    hover_animation_.SetTweenType(gfx::Tween::FAST_OUT_SLOW_IN);
    hover_animation_.SetSlideDuration(ScaledHoverRunDuration(animation_ms_));
    if (!animations_ || animation_ms_ <= 0) {
      hover_animation_.Stop();
      hover_animation_.Reset(hovered_ && !active_ ? 1.0 : 0.0);
      ScheduleVisualPaint();
    }
  }

  void OnMouseEntered(const ui::MouseEvent&) override {
    hovered_ = true;
    UpdateCloseState();
    UpdateHoverState();
  }

  void OnMouseExited(const ui::MouseEvent&) override {
    hovered_ = false;
    UpdateCloseState();
    UpdateHoverState();
  }

  bool OnMousePressed(const ui::MouseEvent& event) override {
    pressed_left_ = false;
    pressed_middle_ = false;
    if (event.IsOnlyRightMouseButton()) {
      if (owner_) {
        gfx::Point screen = event.location();
        ConvertPointToScreen(this, &screen);
        owner_->ShowContextMenuForTab(tab_id_, screen);
      }
      return true;
    }
    if (event.IsOnlyMiddleMouseButton()) {
      pressed_middle_ = true;
      return true;
    }
    if (event.IsOnlyLeftMouseButton()) {
      press_pt_ = event.location();
      dragging_ = false;
      pressed_left_ = true;
      // Chromium activates an inactive source tab on mouse-down, before drag
      // thresholding. cmux has no multi-selection modifier state to preserve,
      // so every primary press selects the source immediately.
      if (owner_) {
        owner_->SelectTab(tab_id_);
      }
      return true;
    }
    return false;
  }

  bool OnMouseDragged(const ui::MouseEvent& event) override {
    if (!owner_ || !pressed_left_ || !event.IsLeftMouseButton()) {
      return false;
    }
    gfx::Point screen = event.location();
    ConvertPointToScreen(this, &screen);
    if (!dragging_) {
      if (!ExceededDragThreshold(event.location() - press_pt_)) {
        return true;
      }
      dragging_ = true;
      // Mouse capture means no OnMouseExited for the whole drag: pointer
      // hover must stop driving the fill or it stays latched after the drag
      // leaves. The lifted pill paints an explicit drag fill instead (it
      // must stay legible while live-reorder slides it over neighbors).
      hovered_ = false;
      hover_animation_.Stop();
      hover_animation_.Reset(0.0);
      UpdateCloseState();
      SchedulePaint();
      owner_->BeginTabDrag(tab_id_, screen, press_pt_);
    }
    owner_->UpdateDrag(screen);
    return true;
  }

  void OnMouseReleased(const ui::MouseEvent& event) override {
    if (dragging_) {
      dragging_ = false;
      pressed_left_ = false;
      SchedulePaint();  // drop the drag-lift fill
      if (owner_) {
        owner_->EndDrag(/*commit=*/true);
      }
      return;
    }
    if (pressed_middle_) {
      pressed_middle_ = false;
      if (owner_ && HitTestPoint(event.location())) {
        owner_->CloseTab(tab_id_);
      }
      return;
    }
    if (!pressed_left_) {
      return;
    }
    pressed_left_ = false;
    if (owner_) {
      owner_->SelectTab(tab_id_);
    }
  }

  void OnMouseCaptureLost() override {
    if (!dragging_) {
      pressed_left_ = false;
      pressed_middle_ = false;
      return;
    }
    dragging_ = false;
    pressed_left_ = false;
    pressed_middle_ = false;
    SchedulePaint();  // drop the drag-lift fill
    if (owner_) {
      owner_->EndDrag(/*commit=*/false);
    }
  }

  void OnPaintBackground(gfx::Canvas* canvas) override {
    const double hover_value =
        active_ ? 0.0 : (dragging_ ? 1.0 : hover_animation_.GetCurrentValue());
    if (!active_ && hover_value <= 0.0) {
      return;
    }

    cc::PaintFlags bg;
    bg.setAntiAlias(true);
    const TabStripThemeColors fallback;
    const TabStripThemeColors& colors =
        owner_ ? owner_->theme_colors() : fallback;
    const U8CPU hover_alpha = static_cast<U8CPU>(std::lround(
        SkColorGetA(colors.tab_hover_bg) *
        std::clamp(hover_value, 0.0, 1.0)));
    bg.setColor(active_ ? colors.tab_active_bg
                        : SkColorSetA(colors.tab_hover_bg, hover_alpha));
    // During opening, paint the final pill geometry and let the current View
    // bounds clip it. This is a reveal, not a resized round rect: the left
    // corners never squash into a capsule and the right corners enter only as
    // the reveal reaches them.
    canvas->Save();
    canvas->ClipRect(GetLocalBounds());
    const int visual_width = opening_visual_width_.value_or(width());
    canvas->DrawRoundRect(
        gfx::RectF(0, kTabVisualTop, visual_width, kTabVisualHeight),
        kTabPillRadius, bg);
    canvas->Restore();
  }

  void Layout(PassKey) override {
    // Match Chromium Tab::Layout's important animation invariant: leading
    // content is anchored, while the title's trailing edge and close button
    // follow the moving right edge. BoundsAnimator can therefore lay this out
    // on every tick without scaling or recentering the favicon/title.
    const bool show_close =
        active_ || (hovered_ && width() >= kInactiveCloseButtonMinWidth);
    close_->SetVisible(show_close);

    // At Helium's 34-DIP active minimum Chrome leaves only the close affordance
    // visible. Inactive minimum-width tabs retain a centered, clipped favicon.
    const bool show_icon = !active_ || width() >= 56;
    icon_->SetVisible(show_icon);
    const bool center_icon = !active_ && width() < kInactiveCloseButtonMinWidth;
    const int icon_x = center_icon ? std::max(0, (width() - kIconSize) / 2)
                                   : kTabLeadingInset;
    const int icon_y = std::max(0, (height() - kIconSize) / 2);
    icon_->SetBounds(icon_x, icon_y, kIconSize, kIconSize);

    int visible_close_left = width() - kTabTrailingInset - kTabCloseIconSize;
    visible_close_left =
        std::max(visible_close_left, (width() - kTabCloseIconSize) / 2);
    const int close_x = visible_close_left - 6;
    const int close_y = std::max(0, (height() - kTabCloseButtonSize + 1) / 2);
    close_->SetBounds(close_x, close_y, kTabCloseButtonSize,
                      kTabCloseButtonSize);

    const int title_x = show_icon
                            ? icon_x + kIconSize + kTabPreTitlePadding
                            : kTabLeadingInset;
    const int title_right = show_close
                                ? visible_close_left - kTabAfterTitlePadding
                                : width() - kTabTrailingInset;
    const int title_width = std::max(0, title_right - title_x);
    label_->SetBounds(title_x, 0, title_width, height());
    label_->SetVisible(title_width > 0 && !center_icon);
  }

  void AnimationProgressed(const gfx::Animation* animation) override {
    if (animation == &hover_animation_) {
      ScheduleVisualPaint();
    }
  }

  void AnimationEnded(const gfx::Animation* animation) override {
    if (animation == &hover_animation_) {
      ScheduleVisualPaint();
    }
  }

  void AnimationCanceled(const gfx::Animation* animation) override {
    if (animation == &hover_animation_) {
      ScheduleVisualPaint();
    }
  }

 private:
  void UpdateHoverState() {
    if (active_ || !animations_ || animation_ms_ <= 0 || !GetWidget()) {
      hover_animation_.Stop();
      hover_animation_.Reset(hovered_ && !active_ ? 1.0 : 0.0);
      ScheduleVisualPaint();
      return;
    }
    if (hovered_) {
      hover_animation_.Show();
    } else {
      hover_animation_.Hide();
    }
    ScheduleVisualPaint();
  }

  void ScheduleVisualPaint() {
    SchedulePaint();
    if (owner_) {
      owner_->SchedulePaint();
    }
  }

  void UpdateCloseState() {
    if (!close_) {
      return;
    }
    InvalidateLayout();
  }

  raw_ptr<CmuxTabStrip> owner_;
  SurfaceTabId tab_id_ = kInvalidId;
  raw_ptr<views::View> icon_ = nullptr;
  raw_ptr<views::Label> label_ = nullptr;
  raw_ptr<CmuxTabCloseButton> close_ = nullptr;
  gfx::Point press_pt_;
  SurfaceKind kind_ = SurfaceKind::kWeb;
  bool loading_ = false;
  gfx::ImageSkia favicon_;
  bool active_ = false;
  bool hovered_ = false;
  bool animations_ = true;
  int animation_ms_ = 160;
  bool dragging_ = false;
  bool pressed_left_ = false;
  bool pressed_middle_ = false;
  std::optional<int> opening_visual_width_;
  gfx::SlideAnimation hover_animation_;
};

BEGIN_METADATA(CmuxTab)
END_METADATA

// Matches Chromium's RemoveTabDelegate lifetime contract: a closing tab is
// deleted after its bounds animation ends, and cancellation has the same
// cleanup semantics. Observing the view prevents a stale pointer if a parent
// teardown removes it first.
class ClosingTabAnimationDelegate : public gfx::AnimationDelegate,
                                    public views::ViewObserver {
 public:
  ClosingTabAnimationDelegate(
      views::View* tab,
      base::OnceCallback<void(views::View*)> completion)
      : tab_(tab), completion_(std::move(completion)) {
    observation_.Observe(tab);
  }

  ClosingTabAnimationDelegate(const ClosingTabAnimationDelegate&) = delete;
  ClosingTabAnimationDelegate& operator=(
      const ClosingTabAnimationDelegate&) = delete;

  void AnimationEnded(const gfx::Animation*) override { Complete(); }
  void AnimationCanceled(const gfx::Animation*) override { Complete(); }

  void OnViewIsDeleting(views::View* observed_view) override {
    CHECK_EQ(tab_, observed_view);
    observation_.Reset();
    tab_ = nullptr;
    completion_.Reset();
  }

 private:
  void Complete() {
    if (!tab_ || !completion_) {
      return;
    }
    views::View* tab = tab_;
    observation_.Reset();
    tab_ = nullptr;
    std::move(completion_).Run(tab);
  }

  raw_ptr<views::View> tab_ = nullptr;
  base::OnceCallback<void(views::View*)> completion_;
  base::ScopedObservation<views::View, views::ViewObserver> observation_{this};
};

class CmuxWindowDragHandle : public views::View {
  METADATA_HEADER(CmuxWindowDragHandle, views::View)

 public:
  explicit CmuxWindowDragHandle(CmuxTabStrip* owner) : owner_(owner) {}

  bool OnMousePressed(const ui::MouseEvent& event) override {
    if (event.IsOnlyRightMouseButton()) {
      if (owner_) {
        gfx::Point screen = event.location();
        ConvertPointToScreen(this, &screen);
        owner_->ShowTabStripContextMenu(screen);
      }
      return true;
    }
    if (!event.IsOnlyLeftMouseButton()) {
      return false;
    }
    // This view occupies only the trailing, otherwise-empty tab-strip region.
    // Let AppKit run its native move loop so tabs and controls retain their own
    // Views drag/click behavior while the empty chrome behaves like Chrome's
    // title bar.
    if (owner_) {
      owner_->BeginWindowDrag(event);
    }
    return true;
  }

  bool OnMouseDragged(const ui::MouseEvent&) override { return true; }
  void OnMouseReleased(const ui::MouseEvent&) override {}

 private:
  raw_ptr<CmuxTabStrip> owner_;
};

BEGIN_METADATA(CmuxWindowDragHandle)
END_METADATA

class InsertionSpacer : public views::View {
  METADATA_HEADER(InsertionSpacer, views::View)

 public:
  InsertionSpacer() { SetPreferredSize(gfx::Size(0, kTabViewHeight)); }

  void SetShowing(bool showing) {
    SetPreferredSize(
        gfx::Size(showing ? kInsertionGap : 0, kTabViewHeight));
    SetVisible(showing);
  }
};

BEGIN_METADATA(InsertionSpacer)
END_METADATA

}  // namespace

class CmuxTabContextMenu : public ui::SimpleMenuModel::Delegate {
 public:
  CmuxTabContextMenu(CmuxTabStrip* owner, SurfaceTabId tab)
      : owner_(owner),
        tab_(tab),
        model_(std::make_unique<ui::SimpleMenuModel>(this)) {
    Add(TabContextAction::kNewTabRight, u"New Tab to the Right");
    Add(TabContextAction::kNewSplitWithCurrentTab,
        u"New Split View with Current Tab");
    Add(TabContextAction::kAddTabToNewGroup, u"Add Tab to New Group");
    Add(TabContextAction::kMoveTabToNewWindow, u"Move Tab to New Window");
    layout_model_ = std::make_unique<ui::SimpleMenuModel>(this);
    layout_model_->AddCheckItem(kCommandTabsOnRightSide,
                                u"Tabs on Right Side");
    model_->AddSubMenu(kCommandBrowserLayout, u"Browser Layout",
                       layout_model_.get());
    model_->AddSeparator(ui::NORMAL_SEPARATOR);
    Add(TabContextAction::kCopyUrl, u"Copy URL");
    model_->AddSeparator(ui::NORMAL_SEPARATOR);
    Add(TabContextAction::kReload, u"Reload");
    Add(TabContextAction::kDuplicate, u"Duplicate");
    Add(TabContextAction::kTogglePinned,
        owner_ && owner_->IsTabContextActionToggled(
                      tab_, TabContextAction::kTogglePinned)
            ? u"Unpin"
            : u"Pin");
    Add(TabContextAction::kToggleSiteMuted,
        owner_ && owner_->IsTabContextActionToggled(
                      tab_, TabContextAction::kToggleSiteMuted)
            ? u"Unmute Site"
            : u"Mute Site");
    model_->AddSeparator(ui::NORMAL_SEPARATOR);
    Add(TabContextAction::kHibernate, u"Hibernate");
    model_->AddSeparator(ui::NORMAL_SEPARATOR);
    Add(TabContextAction::kClose, u"Close");
    Add(TabContextAction::kCloseOtherTabs, u"Close Other Tabs");
    Add(TabContextAction::kCloseTabsToLeft, u"Close Tabs to the Left");
    Add(TabContextAction::kCloseTabsToRight, u"Close Tabs to the Right");
    runner_ = std::make_unique<views::MenuRunner>(
        model_.get(),
        views::MenuRunner::HAS_MNEMONICS | views::MenuRunner::CONTEXT_MENU);
  }

  CmuxTabContextMenu(const CmuxTabContextMenu&) = delete;
  CmuxTabContextMenu& operator=(const CmuxTabContextMenu&) = delete;
  ~CmuxTabContextMenu() override = default;

  void Run(const gfx::Point& screen_pt, views::Widget* widget) {
    if (!runner_ || !widget) {
      return;
    }
    runner_->RunMenuAt(widget, nullptr, gfx::Rect(screen_pt, gfx::Size()),
                       views::MenuAnchorPosition::kTopLeft,
                       ui::mojom::MenuSourceType::kMouse);
  }

  bool IsCommandIdEnabled(int command_id) const override {
    if (command_id == kCommandBrowserLayout ||
        command_id == kCommandTabsOnRightSide) {
      return owner_ != nullptr;
    }
    return owner_ && owner_->IsTabContextActionEnabled(
                         tab_, static_cast<TabContextAction>(command_id));
  }

  bool IsCommandIdChecked(int command_id) const override {
    return command_id == kCommandTabsOnRightSide && owner_ &&
           owner_->IsSidebarOnRight();
  }

  void ExecuteCommand(int command_id, int) override {
    if (!owner_) {
      return;
    }
    if (command_id == kCommandTabsOnRightSide) {
      base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
          FROM_HERE,
          base::BindOnce(&CmuxTabStrip::SetSidebarOnRight,
                         owner_->weak_factory_.GetWeakPtr(),
                         !owner_->IsSidebarOnRight()));
      return;
    }
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE,
        base::BindOnce(&CmuxTabStrip::ExecuteContextMenuCommand,
                       owner_->weak_factory_.GetWeakPtr(), tab_, command_id));
  }

 private:
  void Add(TabContextAction action, std::u16string label) {
    model_->AddItem(static_cast<int>(action), std::move(label));
  }

  raw_ptr<CmuxTabStrip> owner_;
  SurfaceTabId tab_ = kInvalidId;
  std::unique_ptr<ui::SimpleMenuModel> model_;
  std::unique_ptr<ui::SimpleMenuModel> layout_model_;
  std::unique_ptr<views::MenuRunner> runner_;
};

class CmuxTabStripContextMenu : public ui::SimpleMenuModel::Delegate {
 public:
  explicit CmuxTabStripContextMenu(CmuxTabStrip* owner)
      : owner_(owner),
        model_(std::make_unique<ui::SimpleMenuModel>(this)),
        layout_model_(std::make_unique<ui::SimpleMenuModel>(this)) {
    model_->AddItem(kCommandNewBrowserTab, u"New Tab");
    model_->AddItem(kCommandNewTerminalTab, u"New Terminal Tab");
    model_->AddSeparator(ui::NORMAL_SEPARATOR);
    layout_model_->AddCheckItem(kCommandTabsOnRightSide,
                                u"Tabs on Right Side");
    model_->AddSubMenu(kCommandBrowserLayout, u"Browser Layout",
                       layout_model_.get());
    runner_ = std::make_unique<views::MenuRunner>(
        model_.get(),
        views::MenuRunner::HAS_MNEMONICS | views::MenuRunner::CONTEXT_MENU);
  }

  CmuxTabStripContextMenu(const CmuxTabStripContextMenu&) = delete;
  CmuxTabStripContextMenu& operator=(const CmuxTabStripContextMenu&) = delete;
  ~CmuxTabStripContextMenu() override = default;

  void Run(const gfx::Point& screen_pt, views::Widget* widget) {
    if (!runner_ || !widget) {
      return;
    }
    runner_->RunMenuAt(widget, nullptr, gfx::Rect(screen_pt, gfx::Size()),
                       views::MenuAnchorPosition::kTopLeft,
                       ui::mojom::MenuSourceType::kMouse);
  }

  bool IsCommandIdEnabled(int) const override { return owner_ != nullptr; }

  bool IsCommandIdChecked(int command_id) const override {
    return command_id == kCommandTabsOnRightSide && owner_ &&
           owner_->IsSidebarOnRight();
  }

  bool GetAcceleratorForCommandId(
      int command_id,
      ui::Accelerator* accelerator) const override {
    if (!owner_ || !accelerator ||
        (command_id != kCommandNewBrowserTab &&
         command_id != kCommandNewTerminalTab)) {
      return false;
    }
    const SurfaceKind kind = command_id == kCommandNewTerminalTab
                                 ? SurfaceKind::kTerminal
                                 : SurfaceKind::kWeb;
    std::optional<ui::Accelerator> resolved =
        owner_->GetNewTabAccelerator(kind);
    if (!resolved) {
      return false;
    }
    *accelerator = *resolved;
    return true;
  }

  void ExecuteCommand(int command_id, int) override {
    if (!owner_) {
      return;
    }
    switch (command_id) {
      case kCommandNewBrowserTab:
        owner_->NewTab(SurfaceKind::kWeb);
        break;
      case kCommandNewTerminalTab:
        owner_->NewTab(SurfaceKind::kTerminal);
        break;
      case kCommandTabsOnRightSide:
        base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
            FROM_HERE,
            base::BindOnce(&CmuxTabStrip::SetSidebarOnRight,
                           owner_->weak_factory_.GetWeakPtr(),
                           !owner_->IsSidebarOnRight()));
        break;
      default:
        break;
    }
  }

 private:
  raw_ptr<CmuxTabStrip> owner_;
  std::unique_ptr<ui::SimpleMenuModel> model_;
  std::unique_ptr<ui::SimpleMenuModel> layout_model_;
  std::unique_ptr<views::MenuRunner> runner_;
};

class CmuxNewTabMenu : public ui::SimpleMenuModel::Delegate {
 public:
  explicit CmuxNewTabMenu(CmuxTabStrip* owner)
      : owner_(owner), model_(std::make_unique<ui::SimpleMenuModel>(this)) {
    model_->AddItem(kCommandNewBrowserTab, u"New Browser Tab");
    model_->AddItem(kCommandNewTerminalTab, u"New Terminal Tab");
    runner_ = std::make_unique<views::MenuRunner>(
        model_.get(), views::MenuRunner::HAS_MNEMONICS);
  }

  CmuxNewTabMenu(const CmuxNewTabMenu&) = delete;
  CmuxNewTabMenu& operator=(const CmuxNewTabMenu&) = delete;
  ~CmuxNewTabMenu() override = default;

  void Run(views::View* anchor) {
    if (!runner_ || !anchor || !anchor->GetWidget()) {
      return;
    }
    runner_->RunMenuAt(anchor->GetWidget(), nullptr,
                       anchor->GetBoundsInScreen(),
                       views::MenuAnchorPosition::kTopLeft,
                       ui::mojom::MenuSourceType::kMouse);
  }

  bool IsCommandIdEnabled(int) const override { return true; }

  bool GetAcceleratorForCommandId(
      int command_id,
      ui::Accelerator* accelerator) const override {
    if (!owner_ || !accelerator) {
      return false;
    }
    const SurfaceKind kind = command_id == kCommandNewTerminalTab
                                 ? SurfaceKind::kTerminal
                                 : SurfaceKind::kWeb;
    std::optional<ui::Accelerator> resolved =
        owner_->GetNewTabAccelerator(kind);
    if (!resolved) {
      return false;
    }
    *accelerator = *resolved;
    return true;
  }

  void ExecuteCommand(int command_id, int) override {
    if (!owner_) {
      return;
    }
    switch (command_id) {
      case kCommandNewBrowserTab:
        owner_->NewTab(SurfaceKind::kWeb);
        break;
      case kCommandNewTerminalTab:
        owner_->NewTab(SurfaceKind::kTerminal);
        break;
      default:
        break;
    }
  }

 private:
  raw_ptr<CmuxTabStrip> owner_;
  std::unique_ptr<ui::SimpleMenuModel> model_;
  std::unique_ptr<views::MenuRunner> runner_;
};

class CmuxTabStrip::InsertionGapAnimation
    : public views::AnimationDelegateViews {
 public:
  InsertionGapAnimation(CmuxTabStrip* owner,
                        views::View* view,
                        int start_dx,
                        base::TimeDelta run_duration)
      : views::AnimationDelegateViews(view),
        owner_(owner),
        view_(view),
        start_dx_(start_dx),
        animation_(this) {
    Apply(0.0);
    animation_.Reset(0.0);
    animation_.SetTweenType(gfx::Tween::LINEAR);
    animation_.SetSlideDuration(run_duration);
    animation_.Show();
  }

  InsertionGapAnimation(const InsertionGapAnimation&) = delete;
  InsertionGapAnimation& operator=(const InsertionGapAnimation&) = delete;

  ~InsertionGapAnimation() override {
    animation_.Stop();
    ApplyIdentity();
  }

  void AnimationProgressed(const gfx::Animation* animation) override {
    if (animation == &animation_) {
      Apply(AppleSpring(animation_.GetCurrentValue(), 0.15));
      ScheduleOwnerPaint();
    }
  }

  void AnimationEnded(const gfx::Animation* animation) override {
    if (animation == &animation_) {
      ApplyIdentity();
      ScheduleOwnerPaint();
    }
  }

  void AnimationCanceled(const gfx::Animation* animation) override {
    if (animation == &animation_) {
      ApplyIdentity();
      ScheduleOwnerPaint();
    }
  }

 private:
  void Apply(double value) {
    if (!view_ || !view_->layer()) {
      return;
    }
    gfx::Transform transform;
    transform.Translate(start_dx_ * (1.0 - value), 0.0);
    view_->layer()->SetTransform(transform);
  }

  void ApplyIdentity() {
    if (view_ && view_->layer()) {
      view_->layer()->SetTransform(gfx::Transform());
    }
  }

  void ScheduleOwnerPaint() {
    if (owner_) {
      owner_->SchedulePaint();
    }
  }

  raw_ptr<CmuxTabStrip> owner_;
  raw_ptr<views::View> view_;
  int start_dx_ = 0;
  gfx::SlideAnimation animation_;
};

class CmuxTabStrip::LiveReorderAnimation
    : public views::AnimationDelegateViews {
 public:
  LiveReorderAnimation(CmuxTabStrip* owner, views::View* view)
      : views::AnimationDelegateViews(view),
        owner_(owner),
        view_(view),
        animation_(this) {}
  LiveReorderAnimation(const LiveReorderAnimation&) = delete;
  LiveReorderAnimation& operator=(const LiveReorderAnimation&) = delete;
  ~LiveReorderAnimation() override { animation_.Stop(); }

  views::View* view() const { return view_; }
  int current_dx() const { return current_dx_; }

  void SetImmediate(int dx) {
    animation_.Stop();
    current_dx_ = dx;
    start_dx_ = dx;
    target_dx_ = dx;
    if (!view_ || !view_->layer()) {
      return;
    }
    gfx::Transform transform;
    transform.Translate(dx, 0.0);
    view_->layer()->SetTransform(transform);
    ScheduleOwnerPaint();
  }

  void Retarget(int target_dx, base::TimeDelta run_duration) {
    if (!view_) {
      return;
    }
    if (target_dx_ == target_dx && animation_.is_animating()) {
      return;
    }
    if (current_dx_ == target_dx && !animation_.is_animating()) {
      return;
    }
    if (animation_.is_animating()) {
      Apply(AppleSpring(animation_.GetCurrentValue(), 0.15));
    }
    animation_.Stop();
    start_dx_ = current_dx_;
    target_dx_ = target_dx;
    if (run_duration.is_zero()) {
      Apply(1.0);
      ScheduleOwnerPaint();
      return;
    }
    animation_.Reset(0.0);
    animation_.SetTweenType(gfx::Tween::EASE_OUT);
    animation_.SetSlideDuration(run_duration);
    animation_.Show();
  }

  void Stop() { animation_.Stop(); }

  void AnimationProgressed(const gfx::Animation* animation) override {
    if (animation == &animation_) {
      Apply(animation_.GetCurrentValue());
      ScheduleOwnerPaint();
    }
  }

  void AnimationEnded(const gfx::Animation* animation) override {
    if (animation == &animation_) {
      Apply(1.0);
      ScheduleOwnerPaint();
    }
  }

  void AnimationCanceled(const gfx::Animation* animation) override {
    if (animation == &animation_) {
      ScheduleOwnerPaint();
    }
  }

 private:
  void Apply(double value) {
    if (!view_ || !view_->layer()) {
      return;
    }
    current_dx_ = gfx::Tween::IntValueBetween(value, start_dx_, target_dx_);
    gfx::Transform transform;
    transform.Translate(current_dx_, 0.0);
    view_->layer()->SetTransform(transform);
  }

  void ScheduleOwnerPaint() {
    if (owner_) {
      owner_->SchedulePaint();
    }
  }

  raw_ptr<CmuxTabStrip> owner_;
  raw_ptr<views::View> view_;
  gfx::SlideAnimation animation_;
  int start_dx_ = 0;
  int target_dx_ = 0;
  int current_dx_ = 0;
};

CmuxTabStrip::CmuxTabStrip(TabStripDelegate* delegate)
    : delegate_(delegate),
      tab_bounds_animator_(std::make_unique<views::BoundsAnimator>(this)) {
  tab_bounds_animator_->AddObserver(this);
}

CmuxTabStrip::~CmuxTabStrip() {
  weak_factory_.InvalidateWeakPtrs();
  if (tab_bounds_animator_) {
    // BoundsAnimator is a member and therefore dies before the View base, but
    // explicitly detach first so cancellation during teardown can never call
    // back into a partially-destroyed strip.
    tab_bounds_animator_->RemoveObserver(this);
    tab_bounds_animator_->Cancel();
  }
}

void CmuxTabStrip::SetTabs(const std::vector<TabVisual>& tabs,
                           SurfaceTabId selected) {
  if (drag_origin_active_) {
    pending_tabs_ = tabs;
    pending_selected_ = selected;
    has_pending_tabs_ = true;
    return;
  }
  if (!insertion_spacer_) {
    tabs_ = tabs;
    selected_ = selected;
    Rebuild();
    return;
  }
  SyncTabs(tabs, selected);
}

int CmuxTabStrip::InsertionIndexAt(const gfx::Point& strip_local) const {
  for (size_t i = 0; i < tab_views_.size(); ++i) {
    const views::View* tab = tab_views_[i];
    if (!tab || !tab->GetVisible()) {
      continue;
    }
    if (strip_local.x() < tab->x() + tab->width() / 2) {
      return static_cast<int>(i);
    }
  }
  return static_cast<int>(tab_views_.size());
}

void CmuxTabStrip::ShowInsertionIndicator(int index) {
  if (!insertion_spacer_) {
    return;
  }
  const int clamped = std::clamp(index, 0, static_cast<int>(tab_views_.size()));
  const bool opening = insertion_indicator_index_ != clamped;
  static_cast<InsertionSpacer*>(insertion_spacer_.get())->SetShowing(true);
  ReorderChildView(insertion_spacer_, clamped);
  insertion_indicator_index_ = clamped;
  if (opening) {
    AnimateInsertionGap(clamped, /*opening=*/true);
  }
  InvalidateLayout();
  SchedulePaint();
}

void CmuxTabStrip::ClearInsertionIndicator() {
  if (!insertion_spacer_) {
    return;
  }
  if (insertion_indicator_index_ >= 0) {
    AnimateInsertionGap(insertion_indicator_index_, /*opening=*/false);
  }
  static_cast<InsertionSpacer*>(insertion_spacer_.get())->SetShowing(false);
  insertion_indicator_index_ = -1;
  InvalidateLayout();
  SchedulePaint();
}

gfx::Size CmuxTabStrip::CalculatePreferredSize(const views::SizeBounds&) const {
  return gfx::Size(kTabMaxWidth, kStripHeight);
}

void CmuxTabStrip::OnPaint(gfx::Canvas* canvas) {
  views::View::OnPaint(canvas);
  PaintTabSeparators(canvas);
}

bool CmuxTabStrip::OnMousePressed(const ui::MouseEvent& event) {
  if (event.IsOnlyRightMouseButton()) {
    gfx::Point screen = event.location();
    ConvertPointToScreen(this, &screen);
    ShowTabStripContextMenu(screen);
    return true;
  }
  if (!event.IsOnlyLeftMouseButton()) {
    return false;
  }
  // Child tabs/buttons consume their events first. Reaching the strip itself
  // therefore means the press is in padding or a gap between controls.
  BeginWindowDrag(event);
  return true;
}

void CmuxTabStrip::OnThemeChanged() {
  views::View::OnThemeChanged();
  ApplyToolbarBackground();
}

void CmuxTabStrip::OnBoundsAnimatorProgressed(
    views::BoundsAnimator* animator) {
  DCHECK_EQ(animator, tab_bounds_animator_.get());
  // Mirrors TabContainerImpl: the moving trailing edge changes the strip's
  // effective preferred geometry throughout the animation.
  PreferredSizeChanged();
}

void CmuxTabStrip::OnBoundsAnimatorDone(views::BoundsAnimator* animator) {
  DCHECK_EQ(animator, tab_bounds_animator_.get());
  ClearTabOpeningReveals();
  UpdateIdealBounds();
  SnapToIdealBounds();
  PreferredSizeChanged();
  SchedulePaint();
}

void CmuxTabStrip::Layout(PassKey) {
  UpdateIdealBounds();
  if (tab_bounds_animator_ && tab_bounds_animator_->IsAnimating()) {
    // Chrome recomputes and retargets ideal bounds when selection, available
    // width, or other layout inputs change during an animation. Same-target
    // animations are skipped in AnimateToIdealBounds(), avoiding restarts on
    // ordinary title/favicon/loading refreshes.
    AnimateToIdealBounds();
    return;
  }
  SnapToIdealBounds();
  if (live_reorder_active_) {
    ApplyLiveReorder(live_reorder_last_local_, live_reorder_grab_offset_);
  }
}

void CmuxTabStrip::UpdateIdealBounds() {
  UpdateTabWidths();

  ideal_tab_bounds_.clear();
  ideal_tab_bounds_.reserve(tab_views_.size());
  ideal_insertion_spacer_bounds_ = gfx::Rect();
  ideal_new_tab_bounds_ = gfx::Rect();
  ideal_pane_drag_handle_bounds_ = gfx::Rect();

  // Helium's Tab View consumes the full 34-DIP strip. Its detached 28-DIP fill
  // is inset in paint rather than by shrinking or vertically moving the View;
  // this keeps the full Chromium hit target and centers the contents exactly.
  const int child_y = 0;
  const int leading_x = kStripPad + leading_inset_;
  int x = leading_x;
  auto ideal_for = [&](views::View* child) {
    if (!child || !child->GetVisible()) {
      return gfx::Rect();
    }
    const gfx::Size preferred = child->GetPreferredSize();
    const int child_w = preferred.width();
    const int child_h = std::min(height(), preferred.height());
    // Chromium lays the Helium new-tab control at (x, 0), just like its tab
    // container.  Keeping every strip child top-aligned also prevents a
    // one-pixel jump when an insertion spacer becomes visible.
    const gfx::Rect bounds(x, 0, child_w, child_h);
    x += child_w + kTabSpacing;
    return bounds;
  };
  auto ideal_for_tab = [&](views::View* tab) {
    const int tab_w = tab ? tab->GetPreferredSize().width() : 0;
    const gfx::Rect bounds(x, child_y, tab_w, kTabViewHeight);
    x += tab_w + kTabSpacing;
    return bounds;
  };

  const bool spacer_visible = insertion_spacer_ &&
                              insertion_spacer_->GetVisible() &&
                              insertion_indicator_index_ >= 0;
  for (size_t i = 0; i < tab_views_.size(); ++i) {
    if (spacer_visible && insertion_indicator_index_ == static_cast<int>(i)) {
      ideal_insertion_spacer_bounds_ = ideal_for(insertion_spacer_);
    }
    // Logical tab geometry must not depend on the previous visibility pass;
    // otherwise an overflow tab alternates between consuming and not consuming
    // width on successive layouts.
    ideal_tab_bounds_.push_back(ideal_for_tab(tab_views_[i]));
  }
  if (spacer_visible &&
      insertion_indicator_index_ == static_cast<int>(tab_views_.size())) {
    ideal_insertion_spacer_bounds_ = ideal_for(insertion_spacer_);
  }
  const int controls_x_limit =
      std::max(leading_x, width() - kNewTabButtonWidth -
                              kNewTabButtonTrailingMargin -
                              kPaneDragHandleMinWidth);
  // Chromium places the control relative to the trailing tab-strip edge:
  //   right - bottom_corner_radius + strip_padding + leading_margin.
  // Helium's -8 + 3 + 4 values put the 28-DIP host one DIP inside
  // the Tab view edge. Its detached pill ends another four DIPs inside that
  // edge, leaving a three-DIP visible gap. `x` includes our one-DIP advance.
  const int desired_new_tab_x =
      std::max(leading_x,
               x - kTabSpacing + kNewTabButtonPaintedTabGap);
  const bool tabs_overflow = desired_new_tab_x > controls_x_limit;
  std::vector<bool> tab_visible(tab_views_.size(), true);
  if (tabs_overflow) {
    for (size_t i = 0; i < ideal_tab_bounds_.size(); ++i) {
      tab_visible[i] =
          ideal_tab_bounds_[i].right() + kNewTabButtonPaintedTabGap <=
              controls_x_limit;
    }

    // If minimum-width tabs overflow and the active tab is in the clipped
    // tail, retain a visible prefix plus the active tab in the final whole-tab
    // slot. Logical order/IDs remain unchanged; this is only presentation.
    auto active = std::ranges::find_if(
        tab_views_, [this](views::View* view) {
          return static_cast<CmuxTab*>(view)->tab_id() == selected_;
        });
    if (active != tab_views_.end()) {
      const size_t active_index =
          static_cast<size_t>(std::distance(tab_views_.begin(), active));
      if (!tab_visible[active_index] && !spacer_visible) {
        std::fill(tab_visible.begin(), tab_visible.end(), false);
        int visible_x = leading_x;
        const int active_width = ideal_tab_bounds_[active_index].width();
        const int active_x_limit =
            controls_x_limit - active_width - kNewTabButtonPaintedTabGap;
        for (size_t i = 0; i < active_index; ++i) {
          const int advance = ideal_tab_bounds_[i].width() + kTabSpacing;
          if (visible_x + advance > active_x_limit) {
            break;
          }
          ideal_tab_bounds_[i].set_x(visible_x);
          tab_visible[i] = true;
          visible_x += advance;
        }
        if (visible_x <= active_x_limit) {
          ideal_tab_bounds_[active_index].set_x(visible_x);
          tab_visible[active_index] = true;
        }
      }
    }
  }
  for (size_t i = 0; i < tab_views_.size(); ++i) {
    tab_views_[i]->SetVisible(tab_visible[i]);
  }

  // Controls share this view with tabs, unlike Chromium's separate tab
  // container. Pin their starting edge to the reserved region when minimum
  // tab widths overflow; invisible tabs retain their logical bounds beyond it.
  x = std::min(desired_new_tab_x, controls_x_limit);
  ideal_new_tab_bounds_ = ideal_for(new_tab_button_);
  if (pane_drag_handle_) {
    // `ideal_for()` leaves the normal one-DIP child spacing; add Helium's
    // remaining three DIPs so the empty caption/grab region starts four DIPs
    // after the button, with no displaced padding at the far edge.
    x += kNewTabButtonTrailingMargin - kTabSpacing;
    const int handle_w = std::max(0, width() - x);
    ideal_pane_drag_handle_bounds_ =
        gfx::Rect(x, child_y, handle_w, kTabViewHeight);
  }
}

void CmuxTabStrip::SnapToIdealBounds() {
  if (insertion_spacer_ && insertion_spacer_->GetVisible() &&
      !ideal_insertion_spacer_bounds_.IsEmpty()) {
    insertion_spacer_->SetBoundsRect(ideal_insertion_spacer_bounds_);
  }
  for (size_t i = 0;
       i < tab_views_.size() && i < ideal_tab_bounds_.size(); ++i) {
    tab_views_[i]->SetBoundsRect(ideal_tab_bounds_[i]);
  }
  if (new_tab_button_) {
    new_tab_button_->SetBoundsRect(ideal_new_tab_bounds_);
  }
  if (pane_drag_handle_) {
    pane_drag_handle_->SetBoundsRect(ideal_pane_drag_handle_bounds_);
  }
}

void CmuxTabStrip::AnimateToIdealBounds() {
  UpdateIdealBounds();
  if (!tab_bounds_animator_) {
    SnapToIdealBounds();
    return;
  }

  tab_bounds_animator_->SetAnimationDuration(
      gfx::Animation::RichAnimationDuration(
          kChromeTabBoundsAnimationDuration));
  tab_bounds_animator_->set_tween_type(gfx::Tween::EASE_OUT);

  auto animate_to = [&](views::View* view, const gfx::Rect& target) {
    if (!view || tab_bounds_animator_->GetTargetBounds(view) == target) {
      return;
    }
    tab_bounds_animator_->AnimateViewTo(view, target);
  };

  for (size_t i = 0;
       i < tab_views_.size() && i < ideal_tab_bounds_.size(); ++i) {
    auto* tab = static_cast<CmuxTab*>(tab_views_[i].get());
    if (tab->opening_reveal()) {
      // A rapid resize/structural retarget can make an opening tab wider than
      // its new ideal. Continuing to paint the old final silhouette would
      // expose a stale right edge; from this point it is a normal shrink.
      if (tab->width() > ideal_tab_bounds_[i].width()) {
        tab->EndOpeningReveal();
      } else {
        tab->BeginOpeningReveal(ideal_tab_bounds_[i].width());
      }
    }
    animate_to(tab, ideal_tab_bounds_[i]);
  }
  animate_to(new_tab_button_, ideal_new_tab_bounds_);
  animate_to(pane_drag_handle_, ideal_pane_drag_handle_bounds_);
  PreferredSizeChanged();
}

void CmuxTabStrip::Rebuild() {
  StopInsertionGapAnimations();
  StopLiveReorderAnimations();
  StopTabMutationAnimations();
  live_reorder_active_ = false;
  drag_origin_tab_index_ = -1;
  live_reorder_dragged_index_ = -1;
  live_reorder_index_ = -1;
  RemoveAllChildViews();
  tab_views_.clear();
  closing_tab_views_.clear();
  ideal_tab_bounds_.clear();
  insertion_spacer_ = AddChildView(std::make_unique<InsertionSpacer>());
  static_cast<InsertionSpacer*>(insertion_spacer_.get())->SetShowing(false);
  for (const TabVisual& tab : tabs_) {
    auto* pill = AddChildView(std::make_unique<CmuxTab>(
        tab, tab.tab.id == selected_, animations_, animation_ms_, this));
    tab_views_.push_back(pill);
  }

  // Chrome-compatible trailing new-tab control. Browser and terminal tab
  // creation live in its menu instead of competing for strip width.
  auto* plus = AddChildView(std::make_unique<CmuxNewTabButton>(this));
  plus->SetThemeColors(theme_colors_);
  new_tab_button_ = plus;

  pane_drag_handle_ =
      AddChildView(std::make_unique<CmuxWindowDragHandle>(this));
  pane_drag_handle_->SetPreferredSize(
      gfx::Size(kPaneDragHandleMinWidth, kTabViewHeight));

  InvalidateLayout();
}

void CmuxTabStrip::SyncTabs(const std::vector<TabVisual>& tabs,
                            SurfaceTabId selected) {
  std::vector<SurfaceTabId> new_order;
  new_order.reserve(tabs.size());
  for (const TabVisual& visual : tabs) {
    new_order.push_back(visual.tab.id);
  }

  std::vector<SurfaceTabId> old_order;
  old_order.reserve(tab_views_.size());
  for (views::View* view : tab_views_) {
    old_order.push_back(static_cast<CmuxTab*>(view)->tab_id());
  }
  const bool should_animate = animations_ && animation_ms_ > 0 && GetWidget();

  StopInsertionGapAnimations();
  StopLiveReorderAnimations();
  if (!should_animate) {
    StopTabMutationAnimations();
  }

  std::map<SurfaceTabId, gfx::Rect> old_bounds;
  std::map<SurfaceTabId, CmuxTab*> old_views;
  for (views::View* view : tab_views_) {
    auto* tab = static_cast<CmuxTab*>(view);
    old_bounds[tab->tab_id()] = tab->bounds();
    old_views[tab->tab_id()] = tab;
  }
  std::set<SurfaceTabId> new_ids;
  std::set<SurfaceTabId> inserted_ids;
  std::vector<raw_ptr<views::View>> new_views;
  new_views.reserve(tabs.size());
  for (const TabVisual& visual : tabs) {
    new_ids.insert(visual.tab.id);
    CmuxTab* pill = nullptr;
    auto old = old_views.find(visual.tab.id);
    if (old != old_views.end()) {
      pill = old->second;
      pill->UpdateVisual(visual, visual.tab.id == selected);
    } else {
      pill = AddChildView(std::make_unique<CmuxTab>(
          visual, visual.tab.id == selected, animations_, animation_ms_, this));
      inserted_ids.insert(visual.tab.id);
    }
    new_views.push_back(pill);
  }

  struct RemovedTab {
    raw_ptr<views::View> view;
    size_t former_index;
  };
  std::vector<RemovedTab> removed_tabs;
  for (size_t i = 0; i < old_order.size(); ++i) {
    if (!new_ids.contains(old_order[i])) {
      views::View* view = old_views.at(old_order[i]);
      closing_tab_views_.push_back(view);
      removed_tabs.push_back({view, i});
    }
  }

  tabs_ = tabs;
  selected_ = selected;
  tab_views_ = std::move(new_views);
  insertion_indicator_index_ = -1;
  static_cast<InsertionSpacer*>(insertion_spacer_.get())->SetShowing(false);

  OrderChildrenForTabs();
  UpdateIdealBounds();

  // Chrome does not animate the first tab in a strip or mutations before the
  // containing widget is available.
  if (!should_animate || old_bounds.empty()) {
    for (RemovedTab& removed : removed_tabs) {
      views::View* removed_view = removed.view;
      // The completion callback deletes the child. Release this temporary
      // tracking raw_ptr first, while the child is still alive.
      removed.view = nullptr;
      OnCloseTabAnimationCompleted(removed_view);
    }
    UpdateIdealBounds();
    SnapToIdealBounds();
    ClearTabOpeningReveals();
    SchedulePaint();
    return;
  }

  // Lay out a newly inserted pill at its final width once, then shrink only
  // its outer clipping bounds. This preserves the left-to-right reveal without
  // scaling its round rect or children.
  for (size_t i = 0; i < tab_views_.size(); ++i) {
    auto* tab = static_cast<CmuxTab*>(tab_views_[i].get());
    if (!inserted_ids.contains(tab->tab_id())) {
      continue;
    }
    const gfx::Rect target_bounds = ideal_tab_bounds_[i];
    tab->SetBoundsRect(target_bounds);
    tab->DeprecatedLayoutImmediately();
    tab->BeginOpeningReveal(target_bounds.width());

    int opening_x = target_bounds.x();
    for (size_t previous = i; previous > 0; --previous) {
      const auto* candidate =
          static_cast<CmuxTab*>(tab_views_[previous - 1].get());
      const auto previous_bounds = old_bounds.find(candidate->tab_id());
      if (previous_bounds != old_bounds.end()) {
        opening_x = previous_bounds->second.right() + kTabSpacing;
        break;
      }
    }
    if (i == 0) {
      for (size_t next = 1; next < tab_views_.size(); ++next) {
        const auto* candidate = static_cast<CmuxTab*>(tab_views_[next].get());
        const auto next_bounds = old_bounds.find(candidate->tab_id());
        if (next_bounds != old_bounds.end()) {
          opening_x = next_bounds->second.x();
          break;
        }
      }
    }
    tab->SetBounds(opening_x, target_bounds.y(), 0, target_bounds.height());
  }

  AnimateToIdealBounds();

  for (const RemovedTab& removed : removed_tabs) {
    int target_x = kStripPad + leading_inset_;
    for (size_t previous = removed.former_index; previous > 0; --previous) {
      const SurfaceTabId previous_id = old_order[previous - 1];
      auto live = std::ranges::find(new_order, previous_id);
      if (live != new_order.end()) {
        const size_t live_index =
            static_cast<size_t>(std::distance(new_order.begin(), live));
        target_x = ideal_tab_bounds_[live_index].right() + kTabSpacing;
        break;
      }
    }
    StartRemoveTabAnimation(removed.view, target_x);
  }
  SchedulePaint();
}

void CmuxTabStrip::OrderChildrenForTabs() {
  if (!insertion_spacer_) {
    return;
  }
  size_t index = 0;
  ReorderChildView(insertion_spacer_, index++);
  for (views::View* tab : tab_views_) {
    if (tab && tab->parent() == this) {
      ReorderChildView(tab, index++);
    }
  }
  // Closing tabs paint above their converging live neighbors, but below the
  // trailing controls. They are not part of logical ordering or layout.
  for (views::View* tab : closing_tab_views_) {
    if (tab && tab->parent() == this) {
      ReorderChildView(tab, index++);
    }
  }
  if (new_tab_button_) {
    ReorderChildView(new_tab_button_, index++);
  }
  if (pane_drag_handle_) {
    ReorderChildView(pane_drag_handle_, index);
  }
}

void CmuxTabStrip::StartRemoveTabAnimation(views::View* view, int target_x) {
  if (!view || !tab_bounds_animator_) {
    OnCloseTabAnimationCompleted(view);
    return;
  }

  auto* tab = static_cast<CmuxTab*>(view);
  view->SetCanProcessEventsWithinSubtree(false);
  const int target_visual_width = std::max(
      {view->width(), view->GetPreferredSize().width(),
       tab_bounds_animator_->GetTargetBounds(view).width()});
  tab->EndOpeningReveal();
  tab->BeginOpeningReveal(target_visual_width);

  tab_bounds_animator_->SetAnimationDuration(
      gfx::Animation::RichAnimationDuration(
          kChromeTabBoundsAnimationDuration));
  tab_bounds_animator_->set_tween_type(gfx::Tween::EASE_OUT);
  const gfx::Rect target(target_x, view->y(), 0, view->height());
  tab_bounds_animator_->AnimateViewTo(
      view, target,
      std::make_unique<ClosingTabAnimationDelegate>(
          view, base::BindOnce(&CmuxTabStrip::OnCloseTabAnimationCompleted,
                               weak_factory_.GetWeakPtr())));
}

void CmuxTabStrip::OnCloseTabAnimationCompleted(views::View* view) {
  if (!view) {
    return;
  }
  auto it = std::ranges::find(closing_tab_views_, view);
  if (it != closing_tab_views_.end()) {
    closing_tab_views_.erase(it);
  }
  if (view->parent() == this) {
    RemoveChildViewT(view);
  }
  SchedulePaint();
}

void CmuxTabStrip::SelectTab(SurfaceTabId id) {
  PostSelectTab(id);
}

void CmuxTabStrip::PostSelectTab(SurfaceTabId id) {
  // Defer select because the delegate can synchronously Rebuild(), deleting
  // the CmuxTab whose mouse handler is still on the stack.
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE,
      base::BindOnce(
          [](base::WeakPtr<CmuxTabStrip> strip, SurfaceTabId id) {
            TabStripDelegate* delegate =
                strip ? strip->delegate_.get() : nullptr;
            if (delegate) {
              delegate->OnSelectTab(id);
            }
          },
          weak_factory_.GetWeakPtr(), id));
}

void CmuxTabStrip::CloseTab(SurfaceTabId id) {
  if (delegate_) {
    delegate_->OnCloseTab(id);
  }
}

void CmuxTabStrip::CloseOtherTabs(SurfaceTabId id) {
  if (delegate_) {
    delegate_->OnCloseOtherTabs(id);
  }
}

void CmuxTabStrip::NewTab(SurfaceKind kind) {
  PostNewTab(kind);
}

void CmuxTabStrip::ShowNewTabMenu() {
  if (!new_tab_button_ || !new_tab_button_->GetWidget()) {
    return;
  }
  new_tab_menu_ = std::make_unique<CmuxNewTabMenu>(this);
  new_tab_menu_->Run(new_tab_button_);
}

std::optional<ui::Accelerator> CmuxTabStrip::GetNewTabAccelerator(
    SurfaceKind kind) {
  return delegate_ ? delegate_->GetNewTabAccelerator(kind) : std::nullopt;
}

bool CmuxTabStrip::IsSidebarOnRight() const {
  return delegate_ && delegate_->IsSidebarOnRight();
}

void CmuxTabStrip::SetSidebarOnRight(bool on_right) {
  if (delegate_) {
    delegate_->OnSetSidebarOnRight(on_right);
  }
}

void CmuxTabStrip::ShowTabStripContextMenu(
    const gfx::Point& screen_pt) {
  if (!delegate_ || !GetWidget()) {
    return;
  }
  tab_strip_context_menu_ =
      std::make_unique<CmuxTabStripContextMenu>(this);
  tab_strip_context_menu_->Run(screen_pt, GetWidget());
}

void CmuxTabStrip::PostNewTab(SurfaceKind kind) {
  // Defer new-tab because the delegate can synchronously Rebuild(), deleting
  // the button whose pressed callback is still on the stack.
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE,
      base::BindOnce(
          [](base::WeakPtr<CmuxTabStrip> strip, SurfaceKind kind) {
            TabStripDelegate* delegate =
                strip ? strip->delegate_.get() : nullptr;
            if (delegate) {
              delegate->OnNewTab(kind);
            }
          },
          weak_factory_.GetWeakPtr(), kind));
}

void CmuxTabStrip::NewTabRight(SurfaceTabId id) {
  PostNewTabRight(id);
}

void CmuxTabStrip::PostNewTabRight(SurfaceTabId id) {
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE,
      base::BindOnce(
          [](base::WeakPtr<CmuxTabStrip> strip, SurfaceTabId id) {
            TabStripDelegate* delegate =
                strip ? strip->delegate_.get() : nullptr;
            if (delegate) {
              delegate->OnNewTabRight(id);
            }
          },
          weak_factory_.GetWeakPtr(), id));
}

void CmuxTabStrip::MoveTabToNewColumn(SurfaceTabId id) {
  if (delegate_) {
    delegate_->OnMoveTabToNewColumn(id);
  }
}

void CmuxTabStrip::SplitRightWithTab(SurfaceTabId id) {
  if (delegate_) {
    delegate_->OnSplitRightWithTab(id);
  }
}

void CmuxTabStrip::SplitDownWithTab(SurfaceTabId id) {
  if (delegate_) {
    delegate_->OnSplitDownWithTab(id);
  }
}

bool CmuxTabStrip::IsTabContextActionEnabled(
    SurfaceTabId id,
    TabContextAction action) const {
  return delegate_ && delegate_->IsTabContextActionEnabled(id, action);
}

bool CmuxTabStrip::IsTabContextActionToggled(
    SurfaceTabId id,
    TabContextAction action) const {
  return delegate_ && delegate_->IsTabContextActionToggled(id, action);
}

void CmuxTabStrip::ShowContextMenuForTab(SurfaceTabId id,
                                         const gfx::Point& screen_pt) {
  if (!delegate_ || !GetWidget()) {
    return;
  }
  context_menu_ = std::make_unique<CmuxTabContextMenu>(this, id);
  context_menu_->Run(screen_pt, GetWidget());
}

void CmuxTabStrip::BeginTabDrag(SurfaceTabId id,
                                const gfx::Point& screen_pt,
                                const gfx::Point& grab_offset) {
  drag_origin_active_ = true;
  pane_drag_active_ = false;
  live_reorder_active_ = false;
  auto it = std::find_if(
      tabs_.begin(), tabs_.end(),
      [&](const TabVisual& visual) { return visual.tab.id == id; });
  drag_origin_tab_index_ =
      it == tabs_.end() ? -1 : static_cast<int>(it - tabs_.begin());
  SchedulePaint();
  if (delegate_) {
    delegate_->OnTabDragStarted(id, screen_pt, grab_offset);
  }
}

void CmuxTabStrip::BeginPaneDrag(const gfx::Point& screen_pt,
                                 const gfx::Point& grab_offset) {
  drag_origin_active_ = true;
  pane_drag_active_ = true;
  drag_origin_tab_index_ = -1;
  if (delegate_) {
    delegate_->OnPaneDragStarted(screen_pt, grab_offset);
  }
}

void CmuxTabStrip::BeginWindowDrag(const ui::MouseEvent& event) {
  if (delegate_) {
    delegate_->OnBeginWindowDrag(event);
  }
}

void CmuxTabStrip::UpdateDrag(const gfx::Point& screen_pt) {
  if (!delegate_) {
    return;
  }
  if (pane_drag_active_) {
    delegate_->OnPaneDragUpdated(screen_pt);
  } else {
    delegate_->OnTabDragUpdated(screen_pt);
  }
}

void CmuxTabStrip::EndDrag(bool commit) {
  base::WeakPtr<CmuxTabStrip> weak = weak_factory_.GetWeakPtr();
  if (delegate_ && pane_drag_active_) {
    delegate_->OnPaneDragEnded(commit);
  } else if (delegate_) {
    delegate_->OnTabDragEnded(commit);
  }
  if (!weak) {
    return;
  }

  // Preserve each pill's composited screen position while swapping the
  // deferred backing-model order into the strip. Chrome has already mutated
  // its model at this point; release only settles the dragged/non-dragged
  // views to those final ideals with BoundsAnimator's 200ms EASE_OUT timing.
  std::map<SurfaceTabId, int> visual_x;
  if (commit && live_reorder_active_) {
    for (views::View* view : tab_views_) {
      if (!view) {
        continue;
      }
      int dx = 0;
      for (const auto& animation : live_reorder_animations_) {
        if (animation && animation->view() == view) {
          dx = animation->current_dx();
          break;
        }
      }
      visual_x[static_cast<CmuxTab*>(view)->tab_id()] = view->x() + dx;
    }
  }
  drag_origin_active_ = false;
  pane_drag_active_ = false;
  drag_origin_tab_index_ = -1;
  ApplyPendingTabs();
  if (!visual_x.empty()) {
    StopTabMutationAnimations();
    UpdateIdealBounds();
    SnapToIdealBounds();
    StopLiveReorderAnimations();
    for (views::View* view : tab_views_) {
      if (!view) {
        continue;
      }
      const auto position =
          visual_x.find(static_cast<CmuxTab*>(view)->tab_id());
      if (position != visual_x.end()) {
        SetTabTranslate(view, position->second - view->x(), /*animate=*/false);
      }
    }
    live_reorder_active_ = false;
    live_reorder_dragged_index_ = -1;
    live_reorder_index_ = -1;
    live_reorder_last_move_x_.reset();
    ResetLiveReorderTransforms(/*animate=*/true);
    OrderChildrenForTabs();
  } else {
    EndLiveReorder(commit);
  }
  SchedulePaint();
}

void CmuxTabStrip::CancelDragStateForExternalEnd() {
  if (!drag_origin_active_) {
    return;
  }
  drag_origin_active_ = false;
  pane_drag_active_ = false;
  drag_origin_tab_index_ = -1;
  EndLiveReorder(/*commit=*/false);
  ClearInsertionIndicator();
  ApplyPendingTabs();
}

void CmuxTabStrip::SetLeadingInset(int inset) {
  const int clamped = std::max(0, inset);
  if (leading_inset_ == clamped) {
    return;
  }
  leading_inset_ = clamped;
  InvalidateLayout();
}

void CmuxTabStrip::SetAnimationConfig(bool animations, int animation_ms) {
  animations_ = animations;
  animation_ms_ = std::max(0, animation_ms);
  for (views::View* tab : tab_views_) {
    if (tab) {
      static_cast<CmuxTab*>(tab)->SetAnimationConfig(animations_,
                                                     animation_ms_);
    }
  }
  if (!animations_ || animation_ms_ <= 0) {
    StopInsertionGapAnimations();
    StopTabMutationAnimations();
    ResetLiveReorderTransforms(/*animate=*/false);
  }
}

void CmuxTabStrip::SetAccentColor(SkColor accent_color) {
  if (accent_color_ == accent_color) {
    return;
  }
  accent_color_ = accent_color;
  theme_colors_.accent = accent_color;
  for (views::View* tab : tab_views_) {
    if (tab) {
      tab->SchedulePaint();
    }
  }
  if (insertion_spacer_) {
    insertion_spacer_->SchedulePaint();
  }
  SchedulePaint();
}

void CmuxTabStrip::SetThemeColors(const TabStripThemeColors& colors) {
  theme_colors_ = colors;
  accent_color_ = colors.accent;
  ApplyToolbarBackground();
  for (views::View* tab : tab_views_) {
    if (tab) {
      static_cast<CmuxTab*>(tab)->ApplyThemeColors();
      tab->SchedulePaint();
    }
  }
  if (new_tab_button_) {
    static_cast<CmuxNewTabButton*>(new_tab_button_.get())
        ->SetThemeColors(theme_colors_);
  }
  if (insertion_spacer_) {
    insertion_spacer_->SchedulePaint();
  }
  SchedulePaint();
}

void CmuxTabStrip::ApplyToolbarBackground() {
  if (SkColorGetA(theme_colors_.strip_background) == 0) {
    SetBackground(nullptr);
  } else {
    SetBackground(
        views::CreateSolidBackground(theme_colors_.strip_background));
  }
}

bool CmuxTabStrip::UpdateLiveReorder(SurfaceTabId id,
                                     const gfx::Point& strip_local,
                                     const gfx::Point& grab_offset) {
  auto it = std::find_if(
      tabs_.begin(), tabs_.end(),
      [&](const TabVisual& visual) { return visual.tab.id == id; });
  if (it == tabs_.end()) {
    EndLiveReorder(/*commit=*/false);
    return false;
  }
  const int dragged_index = static_cast<int>(it - tabs_.begin());
  if (dragged_index < 0 ||
      dragged_index >= static_cast<int>(tab_views_.size())) {
    EndLiveReorder(/*commit=*/false);
    return false;
  }

  if (!live_reorder_active_) {
    ClearInsertionIndicator();
    StopInsertionGapAnimations();
    live_reorder_active_ = true;
    live_reorder_dragged_index_ = dragged_index;
    live_reorder_index_ = dragged_index;
    live_reorder_last_move_x_.reset();
  }
  live_reorder_last_local_ = strip_local;
  live_reorder_grab_offset_ = grab_offset;

  const int candidate = LiveReorderInsertionIndexAt(strip_local, grab_offset);
  const int candidate_final =
      candidate > live_reorder_dragged_index_ ? candidate - 1 : candidate;
  const int accepted_final =
      live_reorder_index_ > live_reorder_dragged_index_
          ? live_reorder_index_ - 1
          : live_reorder_index_;
  if (candidate_final != accepted_final) {
    const int target_width =
        tab_views_[std::clamp(candidate_final, 0,
                              static_cast<int>(tab_views_.size()) - 1)]
            ->width();
    // Chromium scales its 16-DIP attached-drag hysteresis by the destination
    // width relative to a standard tab. Measure from the last successful live
    // model move rather than frame-to-frame, and allow the first attached move
    // immediately.
    const int threshold = std::max(
        1, static_cast<int>(std::lround(
               16.0 * std::max(0, target_width) / kTabMaxWidth)));
    const bool beyond_hysteresis =
        !live_reorder_last_move_x_.has_value() ||
        std::abs(strip_local.x() - *live_reorder_last_move_x_) > threshold;
    if (beyond_hysteresis && delegate_ &&
        delegate_->OnLiveReorderTab(
            static_cast<CmuxTab*>(tab_views_[live_reorder_dragged_index_].get())
                ->tab_id(),
            candidate_final)) {
      live_reorder_index_ = candidate;
      live_reorder_last_move_x_ = strip_local.x();
    }
  }
  ApplyLiveReorder(strip_local, grab_offset);
  return true;
}

void CmuxTabStrip::EndLiveReorder(bool commit) {
  if (!live_reorder_active_ && live_reorder_animations_.empty()) {
    return;
  }
  if (!commit && live_reorder_active_ && delegate_ &&
      drag_origin_tab_index_ >= 0 && live_reorder_dragged_index_ >= 0 &&
      live_reorder_dragged_index_ < static_cast<int>(tab_views_.size())) {
    delegate_->OnLiveReorderTab(
        static_cast<CmuxTab*>(tab_views_[live_reorder_dragged_index_].get())
            ->tab_id(),
        drag_origin_tab_index_);
  }
  const bool animate_to_rest = !commit && animations_ && animation_ms_ > 0;
  ResetLiveReorderTransforms(animate_to_rest);
  live_reorder_active_ = false;
  live_reorder_dragged_index_ = -1;
  live_reorder_index_ = -1;
  live_reorder_last_move_x_.reset();
  OrderChildrenForTabs();
  SchedulePaint();
}

void CmuxTabStrip::ApplyPendingTabs() {
  if (!has_pending_tabs_) {
    return;
  }
  has_pending_tabs_ = false;
  std::vector<TabVisual> pending = std::move(pending_tabs_);
  const SurfaceTabId selected = pending_selected_;
  pending_selected_ = kInvalidId;
  if (!insertion_spacer_) {
    tabs_ = std::move(pending);
    selected_ = selected;
    Rebuild();
    return;
  }
  SyncTabs(pending, selected);
}

void CmuxTabStrip::ExecuteContextMenuCommand(SurfaceTabId id, int command_id) {
  const TabContextAction action = static_cast<TabContextAction>(command_id);
  switch (action) {
    case TabContextAction::kClose:
      CloseTab(id);
      break;
    case TabContextAction::kCloseOtherTabs:
      CloseOtherTabs(id);
      break;
    case TabContextAction::kNewTabRight:
      NewTabRight(id);
      break;
    default:
      if (delegate_) {
        delegate_->OnTabContextAction(id, action);
      }
      break;
  }
}

void CmuxTabStrip::UpdateTabWidths() {
  const int tab_count = static_cast<int>(tab_views_.size());
  if (tab_count <= 0) {
    return;
  }
  const int spacer_advance =
      insertion_spacer_ && insertion_spacer_->GetVisible()
          ? insertion_spacer_->GetPreferredSize().width() + kTabSpacing
          : 0;
  // Each tab contributes width + the detached 1-DIP advance. The trailing
  // advance is replaced by Helium's three-DIP visible pill gap and four-DIP
  // gap to the frame grab handle.
  const int reserved_controls =
      kStripPad + leading_inset_ - kTabSpacing +
      kNewTabButtonPaintedTabGap +
      kNewTabButtonWidth + kNewTabButtonTrailingMargin +
      kPaneDragHandleMinWidth;
  const int available_advance =
      std::max(0, width() - reserved_controls - spacer_advance);

  std::vector<int> minimum_advances;
  minimum_advances.reserve(tab_views_.size());
  int minimum_sum = 0;
  int crossover_sum = 0;
  int preferred_sum = 0;
  for (views::View* view : tab_views_) {
    const auto* tab = static_cast<const CmuxTab*>(view);
    const int minimum_width =
        tab->tab_id() == selected_ ? kTabMinActiveWidth
                                  : kTabMinInactiveWidth;
    const int minimum_advance = minimum_width + kTabSpacing;
    minimum_advances.push_back(minimum_advance);
    minimum_sum += minimum_advance;
    crossover_sum += kTabMinActiveWidth + kTabSpacing;
    preferred_sum += kTabMaxWidth + kTabSpacing;
  }

  enum class WidthDomain { kBelowActive, kEqualActive };
  const WidthDomain domain = available_advance < crossover_sum
                                 ? WidthDomain::kBelowActive
                                 : WidthDomain::kEqualActive;
  const int domain_start =
      domain == WidthDomain::kBelowActive ? minimum_sum : crossover_sum;
  const int domain_end =
      domain == WidthDomain::kBelowActive ? crossover_sum : preferred_sum;
  const float fraction =
      domain_start == domain_end
          ? 1.0f
          : std::clamp(static_cast<float>(available_advance - domain_start) /
                           static_cast<float>(domain_end - domain_start),
                       0.0f, 1.0f);

  std::vector<int> widths;
  widths.reserve(tab_views_.size());
  int used_advance = 0;
  for (size_t i = 0; i < tab_views_.size(); ++i) {
    const int start = domain == WidthDomain::kBelowActive
                          ? minimum_advances[i]
                          : kTabMinActiveWidth + kTabSpacing;
    const int end = domain == WidthDomain::kBelowActive
                        ? kTabMinActiveWidth + kTabSpacing
                        : kTabMaxWidth + kTabSpacing;
    const int advance = static_cast<int>(
        std::floor(start + fraction * static_cast<float>(end - start)));
    widths.push_back(advance - kTabSpacing);
    used_advance += advance;
  }

  // Chromium floors interpolated widths, then gives every unused pixel back
  // from left to right. Below the minimum domain we deliberately keep minimum
  // widths and allow whole trailing tabs to be hidden instead of crushing.
  int leftover = std::max(0, available_advance - used_advance);
  for (size_t i = 0; i < widths.size() && leftover > 0; ++i) {
    const int limit = domain == WidthDomain::kBelowActive
                          ? kTabMinActiveWidth
                          : kTabMaxWidth;
    if (widths[i] < limit) {
      ++widths[i];
      --leftover;
    }
  }

  for (size_t i = 0; i < tab_views_.size(); ++i) {
    tab_views_[i]->SetPreferredSize(gfx::Size(widths[i], kTabViewHeight));
  }
}

void CmuxTabStrip::PaintTabSeparators(gfx::Canvas* canvas) const {
  if (tab_views_.size() < 2) {
    return;
  }

  cc::PaintFlags flags;
  flags.setAntiAlias(false);
  flags.setColor(SkColorSetA(theme_colors_.tab_idle_text, kSeparatorAlpha));
  flags.setStrokeWidth(1.0f);

  for (int i = 0; i + 1 < static_cast<int>(tab_views_.size()); ++i) {
    if (!ShouldPaintSeparator(i)) {
      continue;
    }
    const views::View* left = tab_views_[i];
    if (!left) {
      continue;
    }
    const float x = static_cast<float>(left->bounds().right());
    constexpr int kHeliumSeparatorHeight = 16;
    const float top = left->y() +
                      (left->height() - kHeliumSeparatorHeight) / 2.0f;
    const float bottom = top + kHeliumSeparatorHeight;
    canvas->DrawLine(gfx::PointF(x, top), gfx::PointF(x, bottom), flags);
  }
}

bool CmuxTabStrip::ShouldPaintSeparator(int left_index) const {
  if (left_index < 0 || left_index + 1 >= static_cast<int>(tab_views_.size())) {
    return false;
  }

  const auto* left = static_cast<const CmuxTab*>(tab_views_[left_index].get());
  const auto* right =
      static_cast<const CmuxTab*>(tab_views_[left_index + 1].get());
  if (!left || !right) {
    return false;
  }
  if (!left->GetVisible() || !right->GetVisible()) {
    return false;
  }
  if (left->active() || right->active() || left->hovered() ||
      right->hovered() || left->showing_hover_fill() ||
      right->showing_hover_fill()) {
    return false;
  }
  auto has_transform = [](const views::View* tab) {
    return tab && tab->layer() && !tab->layer()->transform().IsIdentity();
  };
  if (has_transform(left) || has_transform(right)) {
    return false;
  }
  if (drag_origin_active_ && !pane_drag_active_ &&
      drag_origin_tab_index_ >= 0 &&
      (left_index == drag_origin_tab_index_ ||
       left_index == drag_origin_tab_index_ - 1)) {
    return false;
  }
  if (insertion_indicator_index_ > 0 &&
      insertion_indicator_index_ < static_cast<int>(tab_views_.size()) &&
      left_index == insertion_indicator_index_ - 1) {
    return false;
  }
  if (!live_reorder_active_ || live_reorder_dragged_index_ < 0 ||
      live_reorder_index_ < 0) {
    return true;
  }

  const int dragged = live_reorder_dragged_index_;
  const int target =
      std::clamp(live_reorder_index_, 0, static_cast<int>(tab_views_.size()));
  if (target > dragged) {
    return !(left_index >= dragged && left_index < target);
  }
  if (target < dragged) {
    return !(left_index >= target - 1 && left_index <= dragged);
  }
  return left_index != dragged && left_index != dragged - 1;
}

void CmuxTabStrip::StopInsertionGapAnimations() {
  gap_animations_.clear();
}

void CmuxTabStrip::StopLiveReorderAnimations() {
  for (const auto& animation : live_reorder_animations_) {
    if (animation) {
      animation->Stop();
    }
  }
  live_reorder_animations_.clear();
}

void CmuxTabStrip::StopTabMutationAnimations() {
  if (tab_bounds_animator_) {
    // The previous transform-based implementation restored identity here,
    // which exposed the already-computed target bounds. Complete rather than
    // cancel so disabling animations or starting a second mutation preserves
    // that same snap-to-ideal behavior.
    tab_bounds_animator_->Complete();
  }
  ClearTabOpeningReveals();
}

void CmuxTabStrip::ClearTabOpeningReveals() {
  for (views::View* view : tab_views_) {
    if (view) {
      static_cast<CmuxTab*>(view)->EndOpeningReveal();
    }
  }
}

int CmuxTabStrip::LiveReorderInsertionIndexAt(
    const gfx::Point& strip_local,
    const gfx::Point& grab_offset) const {
  std::vector<int> widths;
  widths.reserve(tab_views_.size());
  for (views::View* tab : tab_views_) {
    widths.push_back(tab ? tab->width() : 0);
  }
  int last_visible_tab_index = live_reorder_dragged_index_;
  for (size_t i = 0; i < tab_views_.size(); ++i) {
    if (tab_views_[i] && tab_views_[i]->GetVisible()) {
      last_visible_tab_index = static_cast<int>(i);
    }
  }
  const int drag_area_end = ideal_new_tab_bounds_.IsEmpty()
                                ? width() - kStripPad
                                : ideal_new_tab_bounds_.x() - kTabSpacing;
  return ComputeLiveReorderInsertionIndex(
      widths, live_reorder_dragged_index_, strip_local.x(), grab_offset.x(),
      kStripPad + leading_inset_, drag_area_end, kTabSpacing,
      last_visible_tab_index);
}

void CmuxTabStrip::ApplyLiveReorder(const gfx::Point& strip_local,
                                    const gfx::Point& grab_offset) {
  if (!live_reorder_active_ || live_reorder_dragged_index_ < 0 ||
      live_reorder_dragged_index_ >= static_cast<int>(tab_views_.size())) {
    return;
  }

  std::vector<int> widths;
  widths.reserve(tab_views_.size());
  for (views::View* tab : tab_views_) {
    widths.push_back(tab ? tab->width() : 0);
  }
  const std::vector<int> offsets = ComputeLiveReorderOffsets(
      widths, live_reorder_dragged_index_, live_reorder_index_, kTabSpacing);
  for (size_t i = 0; i < tab_views_.size(); ++i) {
    views::View* tab = tab_views_[i];
    if (!tab || static_cast<int>(i) == live_reorder_dragged_index_) {
      continue;
    }
    SetTabTranslate(tab, offsets[i], /*animate=*/true);
  }

  views::View* dragged = tab_views_[live_reorder_dragged_index_];
  if (!dragged) {
    return;
  }
  if (!dragged->layer()) {
    dragged->SetPaintToLayer();
    dragged->layer()->SetFillsBoundsOpaquely(false);
  }
  const int min_x = kStripPad + leading_inset_;
  const int drag_area_end = ideal_new_tab_bounds_.IsEmpty()
                                ? width() - kStripPad
                                : ideal_new_tab_bounds_.x() - kTabSpacing;
  const int max_x = std::max(
      min_x, drag_area_end - std::max(0, dragged->width()));
  const int dragged_x =
      std::clamp(strip_local.x() - grab_offset.x(), min_x, max_x);
  if (LiveReorderAnimation* animation = LiveAnimationFor(dragged)) {
    animation->SetImmediate(dragged_x - dragged->x());
  }
  ReorderChildView(dragged, children().size() - 1);
  SchedulePaint();
}

void CmuxTabStrip::ResetLiveReorderTransforms(bool animate) {
  for (views::View* tab : tab_views_) {
    SetTabTranslate(tab, 0, animate);
  }
  if (!animate) {
    StopLiveReorderAnimations();
  }
}

void CmuxTabStrip::SetTabTranslate(views::View* tab, int dx, bool animate) {
  if (!tab) {
    return;
  }
  if (!tab->layer()) {
    tab->SetPaintToLayer();
    tab->layer()->SetFillsBoundsOpaquely(false);
  }
  if (animate && animations_ && animation_ms_ > 0 && GetWidget()) {
    LiveReorderAnimation* animation = LiveAnimationFor(tab);
    if (animation) {
      animation->Retarget(dx, gfx::Animation::RichAnimationDuration(
                                  kChromeTabBoundsAnimationDuration));
    }
    return;
  }
  if (LiveReorderAnimation* animation = LiveAnimationFor(tab)) {
    animation->SetImmediate(dx);
    return;
  }
  gfx::Transform transform;
  transform.Translate(dx, 0.0);
  tab->layer()->SetTransform(transform);
  SchedulePaint();
}

CmuxTabStrip::LiveReorderAnimation* CmuxTabStrip::LiveAnimationFor(
    views::View* tab) {
  if (!tab) {
    return nullptr;
  }
  for (const auto& animation : live_reorder_animations_) {
    if (animation && animation->view() == tab) {
      return animation.get();
    }
  }
  live_reorder_animations_.push_back(
      std::make_unique<LiveReorderAnimation>(this, tab));
  return live_reorder_animations_.back().get();
}

void CmuxTabStrip::AnimateInsertionGap(int index, bool opening) {
  StopInsertionGapAnimations();
  if (!animations_ || animation_ms_ <= 0 || !GetWidget()) {
    return;
  }
  int spacer_child_index = -1;
  for (size_t i = 0; i < children().size(); ++i) {
    if (children()[i] == insertion_spacer_) {
      spacer_child_index = static_cast<int>(i);
      break;
    }
  }
  if (spacer_child_index < 0) {
    return;
  }
  const int start = std::max(spacer_child_index + 1, index);
  const int dx = opening ? -kInsertionGap : kInsertionGap;
  const base::TimeDelta run_duration =
      ScaledSpringRunDuration(animation_ms_, 300);
  for (int i = start; i < static_cast<int>(children().size()); ++i) {
    views::View* child = children()[i];
    if (!child || child == insertion_spacer_) {
      continue;
    }
    if (!child->layer()) {
      child->SetPaintToLayer();
      child->layer()->SetFillsBoundsOpaquely(false);
    }
    gap_animations_.push_back(
        std::make_unique<InsertionGapAnimation>(this, child, dx, run_duration));
  }
}

BEGIN_METADATA(CmuxTabStrip)
END_METADATA

}  // namespace cmux
