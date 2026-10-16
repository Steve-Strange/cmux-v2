// Copyright 2022 The Chromium Authors
// Copyright 2026 Manaflow, Inc.
// Portions derived from Helium; Helium contributors retain copyright.
// SPDX-License-Identifier: GPL-3.0-or-later AND GPL-3.0-only AND BSD-3-Clause
//
// See docs/source-provenance.md for the licensed regions and source pins.

#include "chrome/browser/cmux_term/cmux_rail.h"

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>

#include "base/callback_list.h"
#include "base/functional/bind.h"
#include "base/scoped_observation.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/single_thread_task_runner.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "build/build_config.h"
#include "cc/paint/paint_flags.h"
#include "chrome/app/vector_icons/vector_icons.h"
#include "chrome/browser/cmux_term/cmux_rail_animating_layout_manager.h"
#include "chrome/browser/cmux_term/cmux_rail_group_editor_bubble.h"
#include "chrome/browser/cmux_term/cmux_rail_hover_card.h"
#include "chrome/browser/ui/color/chrome_color_id.h"
#if __has_include("chrome/browser/ui/color/tab_group_color_ids.h")
#include "chrome/browser/ui/color/tab_group_color_ids.h"
#endif
#include "chrome/common/chrome_version.h"
#include "components/vector_icons/vector_icons.h"
#include "third_party/skia/include/core/SkColor.h"
#include "third_party/skia/include/core/SkPath.h"
#include "third_party/skia/include/core/SkRRect.h"
#include "ui/accessibility/ax_enums.mojom.h"
#include "ui/base/accelerators/accelerator.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/base/models/image_model.h"
#include "ui/base/mojom/menu_source_type.mojom.h"
#include "ui/base/ui_base_features.h"
#include "ui/compositor/layer.h"
#include "ui/compositor/layer_animator.h"
#include "ui/compositor/scoped_layer_animation_settings.h"
#include "ui/color/color_id.h"
#include "ui/color/color_provider.h"
#include "ui/events/event.h"
#include "ui/events/event_constants.h"
#include "ui/events/event_observer.h"
#include "ui/events/keycodes/keyboard_codes.h"
#include "ui/gfx/animation/animation.h"
#include "ui/gfx/animation/slide_animation.h"
#include "ui/gfx/animation/tween.h"
#include "ui/gfx/canvas.h"
#include "ui/gfx/color_palette.h"
#include "ui/gfx/color_utils.h"
#include "ui/gfx/font.h"
#include "ui/gfx/font_list.h"
#include "ui/gfx/geometry/insets.h"
#include "ui/gfx/geometry/point.h"
#include "ui/gfx/geometry/point_f.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/rect_f.h"
#include "ui/gfx/geometry/rounded_corners_f.h"
#include "ui/gfx/geometry/size_f.h"
#include "ui/gfx/geometry/skia_conversions.h"
#include "ui/gfx/geometry/vector2d.h"
#include "ui/gfx/text_constants.h"
#include "ui/gfx/text_elider.h"
#include "ui/menus/simple_menu_model.h"
#include "ui/native_theme/overlay_scrollbar_constants.h"
#include "ui/views/accessibility/view_accessibility.h"
#include "ui/views/animation/flood_fill_ink_drop_ripple.h"
#include "ui/views/animation/ink_drop.h"
#include "ui/views/animation/ink_drop_host.h"
#include "ui/views/animation/ink_drop_impl.h"
#include "ui/views/background.h"
#include "ui/views/border.h"
#include "ui/views/context_menu_controller.h"
#include "ui/views/controls/button/button.h"
#include "ui/views/controls/button/button_controller.h"
#include "ui/views/controls/button/label_button.h"
#include "ui/views/controls/button/label_button_border.h"
#include "ui/views/controls/focus_ring.h"
#include "ui/views/controls/highlight_path_generator.h"
#include "ui/views/controls/image_view.h"
#include "ui/views/controls/label.h"
#include "ui/views/controls/menu/menu_runner.h"
#include "ui/views/controls/scroll_view.h"
#include "ui/views/controls/scrollbar/base_scroll_bar_thumb.h"
#include "ui/views/controls/scrollbar/scroll_bar.h"
#include "ui/views/controls/textfield/textfield.h"
#include "ui/views/controls/textfield/textfield_controller.h"
#include "ui/views/event_monitor.h"
#include "ui/views/focus/focus_manager.h"
#include "ui/views/layout/delegating_layout_manager.h"
#include "ui/views/layout/fill_layout.h"
#include "ui/views/layout/proposed_layout.h"
#include "ui/views/masked_targeter_delegate.h"
#include "ui/views/rect_based_targeting_utils.h"
#include "ui/views/view_class_properties.h"
#include "ui/views/view_observer.h"
#include "ui/views/view_targeter.h"
#include "ui/views/view_utils.h"
#include "ui/views/vector_icons.h"
#include "ui/views/widget/widget.h"

#if defined(USE_AURA)
#include "ui/aura/env.h"
#endif

namespace cmux {

WorkspaceHoverCardData::WorkspaceHoverCardData() = default;
WorkspaceHoverCardData::WorkspaceHoverCardData(
    const WorkspaceHoverCardData&) = default;
WorkspaceHoverCardData& WorkspaceHoverCardData::operator=(
    const WorkspaceHoverCardData&) = default;
WorkspaceHoverCardData::WorkspaceHoverCardData(WorkspaceHoverCardData&&) =
    default;
WorkspaceHoverCardData& WorkspaceHoverCardData::operator=(
    WorkspaceHoverCardData&&) = default;
WorkspaceHoverCardData::~WorkspaceHoverCardData() = default;

namespace {

// CMUX_RAIL_STYLE -> RailConfig::Preset index (0=minimal, 1=polished, 2=arc).
int ReadRailStyle() {
  const char* s = std::getenv("CMUX_RAIL_STYLE");
  if (s) {
    const std::string v(s);
    if (v == "polished" || v == "1") {
      return 1;
    }
    if (v == "arc" || v == "2") {
      return 2;
    }
  }
  return 0;
}

// These values are copied from Helium's patched Chromium vertical tab strip.
constexpr int kSidebarHorizontalPadding = 6;
constexpr int kWorkspaceRowHeight = 30;
constexpr int kWorkspaceIconSize = 16;
constexpr int kWorkspaceHorizontalInset = 7;
constexpr int kWorkspaceImageLabelGap = 7;
constexpr base::TimeDelta kWorkspaceGlowHoverAnimationDuration =
    base::Milliseconds(50);
// Copied from Helium's material tab-strip color mixer. This alpha belongs to
// the target hover color; the 50 ms hover animation interpolates toward it.
constexpr SkAlpha kTabInactiveHoverAlpha = 0.45 * SK_AlphaOPAQUE;
constexpr int kGroupHeaderHeight = 26;
constexpr int kGroupHeaderVerticalMargin = 3;
constexpr int kTabVerticalPadding = 2;
constexpr int kGroupLineWidth = 2;
constexpr int kGroupLineCornerRadius = 2;
constexpr int kGroupHeaderCornerRadius = 8;
constexpr int kGroupHeaderFocusRingInset = 2;
constexpr int kVerticalTabDragThreshold = 10;
constexpr int kFooterHeight = 30;

// The shipped browser installs Chrome's color mixers, but the standalone
// views_examples harness intentionally installs only the generic Views
// mixers. A non-null ColorProvider therefore does not prove that a Chrome
// ColorId is defined: unresolved IDs return gfx::kPlaceholderColor. Keep the
// exact Chrome value when present and let callers provide a neutral/config
// fallback when it is not.
std::optional<SkColor> ResolveProviderColor(const views::View* view,
                                            ui::ColorId color_id) {
  const ui::ColorProvider* provider =
      view ? view->GetColorProvider() : nullptr;
  if (!provider) {
    return std::nullopt;
  }
  const SkColor color = provider->GetColor(color_id);
  return color == gfx::kPlaceholderColor ? std::nullopt
                                         : std::optional<SkColor>(color);
}

int WorkspaceCloseIconSize() {
#if CHROME_VERSION_MAJOR >= 150
  return features::IsRoundedIconsEnabled() ? 14 : 16;
#else
  return 16;
#endif
}

gfx::Size WorkspaceCloseButtonSize() {
  // Chromium TabCloseButton keeps 12 DIP beyond the glyph for the hit target.
  const int button_size = WorkspaceCloseIconSize() + 12;
  return gfx::Size(button_size, button_size);
}

const gfx::VectorIcon& WorkspaceCloseIcon() {
#if CHROME_VERSION_MAJOR >= 150
  return features::IsRoundedIconsEnabled() ? kCloseWeight500Icon
                                            : kCloseTabChromeRefreshOldIcon;
#else
  return kCloseTabChromeRefreshIcon;
#endif
}

void SetCommandIcon(ui::SimpleMenuModel* model,
                    int command_id,
                    const ui::ImageModel& icon) {
#if CHROME_VERSION_MAJOR >= 151
  model->SetIconForCommandId(command_id, icon);
#else
  const std::optional<size_t> index = model->GetIndexOfCommandId(command_id);
  if (index) {
    model->SetIcon(*index, icon);
  }
#endif
}

const gfx::VectorIcon& GroupSubmenuIcon() {
#if CHROME_VERSION_MAJOR >= 151
  return features::IsRoundedIconsEnabled()
             ? kGridViewIcon
             : kSavedTabGroupBarEverythingOldIcon;
#else
  return kSavedTabGroupBarEverythingIcon;
#endif
}

const gfx::VectorIcon& SplitSceneIcon() {
#if CHROME_VERSION_MAJOR >= 151
  return features::IsRoundedIconsEnabled() ? kSplitSceneIcon
                                            : kSplitSceneOldIcon;
#else
  return kSplitSceneIcon;
#endif
}

const gfx::VectorIcon& StackedSplitSceneIcon() {
#if CHROME_VERSION_MAJOR >= 151
  return kSplitSceneHorizontalCustomIcon;
#else
  return kSplitSceneDownIcon;
#endif
}

const gfx::VectorIcon& TabGroupMenuIcon() {
#if CHROME_VERSION_MAJOR >= 151
  return kTabGroupOldIcon;
#else
  return kTabGroupIcon;
#endif
}

const gfx::VectorIcon& MoveToNewWindowIcon() {
#if CHROME_VERSION_MAJOR >= 151
  return features::IsRoundedIconsEnabled() ? kOpenInNewIcon
                                            : kOpenInNewOldIcon;
#else
  return kOpenInNewIcon;
#endif
}

const gfx::VectorIcon& PinIcon(bool will_pin) {
#if CHROME_VERSION_MAJOR >= 151
  if (will_pin) {
    return features::IsRoundedIconsEnabled() ? views::kKeepIcon
                                              : views::kPinOldIcon;
  }
  return features::IsRoundedIconsEnabled() ? views::kKeepFilledIcon
                                            : views::kUnpinOldIcon;
#else
  return will_pin ? views::kPinIcon : views::kUnpinIcon;
#endif
}

const gfx::VectorIcon& MuteIcon(bool will_mute) {
#if CHROME_VERSION_MAJOR >= 151
  if (will_mute) {
    return features::IsRoundedIconsEnabled()
               ? vector_icons::kVolumeOffIcon
               : vector_icons::kVolumeOffChromeRefreshOldIcon;
  }
  return features::IsRoundedIconsEnabled()
             ? vector_icons::kVolumeUpIcon
             : vector_icons::kVolumeUpChromeRefreshOldIcon;
#else
  return will_mute ? vector_icons::kVolumeOffChromeRefreshIcon
                   : vector_icons::kVolumeUpIcon;
#endif
}

const gfx::VectorIcon& AddWorkspaceIcon() {
#if CHROME_VERSION_MAJOR >= 151
  return features::IsRoundedIconsEnabled() ? vector_icons::kAddWeight500Icon
                                            : vector_icons::kAddOldIcon;
#else
  return vector_icons::kAddIcon;
#endif
}

const gfx::VectorIcon& MoreMenuIcon() {
#if CHROME_VERSION_MAJOR >= 150
  return features::IsRoundedIconsEnabled()
             ? kMoreVertIcon
             : kBrowserToolsChromeRefreshOldIcon;
#else
  return kBrowserToolsChromeRefreshIcon;
#endif
}

const gfx::VectorIcon& GroupCollapseIcon(bool collapsed) {
#if CHROME_VERSION_MAJOR >= 150
  if (collapsed) {
    return features::IsRoundedIconsEnabled()
               ? kKeyboardArrowDownIcon
               : kKeyboardArrowDownChromeRefreshOldIcon;
  }
  return features::IsRoundedIconsEnabled()
             ? kKeyboardArrowUpIcon
             : kKeyboardArrowUpChromeRefreshOldIcon;
#else
  return collapsed ? kKeyboardArrowDownChromeRefreshIcon
                   : kKeyboardArrowUpChromeRefreshIcon;
#endif
}
constexpr int kFooterIconSize = 16;
constexpr int kFooterVerticalPadding = 4;
constexpr int kFooterHorizontalPadding = 7;
constexpr int kFooterImageLabelGap = 7;
constexpr int kFooterCornerRadius = 8;

// Browser-neutral copy of the final ink-drop setup performed by Chromium
// 151.0.7922.34 (782af9cb)'s ConfigureInkDrop() and
// ConfigureToolbarInkdropForRefresh2023(). Keeping it here preserves the
// shared-button behavior and Helium 8030a8a3's patched vertical-new-tab colors
// without making the cross-platform cmux_chrome_ui target link browser toolbar
// code. The user-education promo-color override is intentionally absent: this
// workspace-only button is not registered as an in-product-help anchor.
constexpr float kFooterInkDropVisibleOpacity = 0.06f;
constexpr SkAlpha kFooterInkDropHighlightVisibleAlpha = 0x14;

using FooterColorCallback =
    base::RepeatingCallback<SkColor(ui::ColorId color_id)>;

SkColor FooterInkDropColor(const FooterColorCallback& color_callback,
                           ui::ColorId color_id) {
  return color_callback.Run(color_id);
}

void ConfigureFooterInkDrop(
    views::Button* host,
    std::unique_ptr<views::HighlightPathGenerator> highlight_generator,
    FooterColorCallback color_callback,
    ui::ColorId hover_color_id,
    ui::ColorId ripple_color_id) {
  host->SetHasInkDropActionOnClick(true);
  views::HighlightPathGenerator::Install(host,
                                         std::move(highlight_generator));

  views::InkDropHost* const ink_drop = views::InkDrop::Get(host);
  ink_drop->SetMode(views::InkDropHost::InkDropMode::ON);
  ink_drop->SetVisibleOpacity(kFooterInkDropVisibleOpacity);
  ink_drop->SetHighlightOpacity(kFooterInkDropHighlightVisibleAlpha /
                                float{SK_AlphaOPAQUE});
  ink_drop->SetBaseColorCallback(base::BindRepeating(
      &FooterInkDropColor, color_callback, kColorToolbarInkDrop));
  ink_drop->SetLayerRegion(views::LayerRegion::kAbove);
  ink_drop->SetCreateRippleCallback(base::BindRepeating(
      [](views::View* host, FooterColorCallback color_callback,
         ui::ColorId color_id)
          -> std::unique_ptr<views::InkDropRipple> {
        const SkColor pressed_color =
            FooterInkDropColor(color_callback, color_id);
        const float pressed_alpha = SkColorGetA(pressed_color);
        return std::make_unique<views::FloodFillInkDropRipple>(
            views::InkDrop::Get(host), host->size(),
            host->GetLocalBounds().CenterPoint(),
            SkColorSetA(pressed_color, SK_AlphaOPAQUE),
            pressed_alpha / SK_AlphaOPAQUE);
      },
      host, color_callback, ripple_color_id));
  ink_drop->SetCreateHighlightCallback(base::BindRepeating(
      [](views::View* host, FooterColorCallback color_callback,
         ui::ColorId color_id) {
        const SkColor hover_color =
            FooterInkDropColor(color_callback, color_id);
        const float hover_alpha = SkColorGetA(hover_color);
        auto highlight = std::make_unique<views::InkDropHighlight>(
            gfx::SizeF(host->size()),
            SkColorSetA(hover_color, SK_AlphaOPAQUE));
        highlight->set_visible_opacity(hover_alpha / SK_AlphaOPAQUE);
        return highlight;
      },
      host, color_callback, hover_color_id));
}

ui::ColorId ChromiumGroupContextMenuColorId(GroupColor color) {
  switch (color) {
    case GroupColor::kGrey:
      return kColorTabGroupContextMenuGrey;
    case GroupColor::kBlue:
      return kColorTabGroupContextMenuBlue;
    case GroupColor::kRed:
      return kColorTabGroupContextMenuRed;
    case GroupColor::kYellow:
      return kColorTabGroupContextMenuYellow;
    case GroupColor::kGreen:
      return kColorTabGroupContextMenuGreen;
    case GroupColor::kPink:
      return kColorTabGroupContextMenuPink;
    case GroupColor::kPurple:
      return kColorTabGroupContextMenuPurple;
    case GroupColor::kCyan:
      return kColorTabGroupContextMenuCyan;
    case GroupColor::kOrange:
      return kColorTabGroupContextMenuOrange;
  }
  return kColorTabGroupContextMenuGrey;
}

// Helium enables Chromium's TabGroupColorRefresh feature by default. cmux's
// browser-neutral Views target cannot depend on chrome/browser/ui/ui_features,
// so reproduce the alternate palette selected by CreateColorParams() locally.
// These named gfx constants are the exact Chromium 150 glow-up colors.
SkColor GroupSkColor(const views::View* view, GroupColor color) {
#if CHROME_VERSION_MAJOR < 151
  return ResolveProviderColor(view, ChromiumGroupContextMenuColorId(color))
      .value_or(SK_ColorGRAY);
#else
  const SkColor surface =
      ResolveProviderColor(view, kColorLocationBarBackground)
          .value_or(ResolveProviderColor(view, ui::kColorSysSurface)
                        .value_or(SK_ColorWHITE));
  const bool dark_mode = color_utils::IsDark(surface);
  switch (color) {
    case GroupColor::kGrey:
      return dark_mode ? gfx::kTabGroupGreyDarkMode
                       : gfx::kTabGroupGreyLightMode;
    case GroupColor::kBlue:
      return dark_mode ? gfx::kTabGroupBlueDarkMode
                       : gfx::kTabGroupBlueLightMode;
    case GroupColor::kRed:
      return dark_mode ? gfx::kTabGroupRedDarkMode
                       : gfx::kTabGroupRedLightMode;
    case GroupColor::kYellow:
      return dark_mode ? gfx::kTabGroupLimeDarkMode
                       : gfx::kTabGroupLimeLightMode;
    case GroupColor::kGreen:
      return dark_mode ? gfx::kTabGroupGreenDarkMode
                       : gfx::kTabGroupGreenLightMode;
    case GroupColor::kPink:
      return dark_mode ? gfx::kTabGroupMagentaDarkMode
                       : gfx::kTabGroupMagentaLightMode;
    case GroupColor::kPurple:
      return dark_mode ? gfx::kTabGroupPurpleDarkMode
                       : gfx::kTabGroupPurpleLightMode;
    case GroupColor::kCyan:
      return dark_mode ? gfx::kTabGroupCyanDarkMode
                       : gfx::kTabGroupCyanLightMode;
    case GroupColor::kOrange:
      return dark_mode ? gfx::kTabGroupOrangeDarkMode
                       : gfx::kTabGroupOrangeLightMode;
  }
  return dark_mode ? gfx::kTabGroupGreyDarkMode
                   : gfx::kTabGroupGreyLightMode;
#endif
}

gfx::Font::Weight MapWeight(int w) {
  switch (w) {
    case 1:
      return gfx::Font::Weight::MEDIUM;
    case 2:
      return gfx::Font::Weight::SEMIBOLD;
    case 3:
      return gfx::Font::Weight::BOLD;
    default:
      return gfx::Font::Weight::NORMAL;
  }
}

gfx::FontList MakeFont(const RailConfig& c, bool folder, bool selected) {
  int wi = selected ? c.selected_font_weight : c.font_weight;
  wi = std::clamp(wi + ((folder && c.folder_bold) ? 1 : 0), 0, 3);
  gfx::Font::Weight w = MapWeight(wi);
  if (!c.font_family.empty()) {
    int size = c.font_size > 0 ? c.font_size : gfx::FontList().GetFontSize();
    return gfx::FontList(gfx::Font(c.font_family, size))
        .Derive(0, gfx::Font::NORMAL, w);
  }
  gfx::FontList base;
  int delta = c.font_size > 0 ? (c.font_size - base.GetFontSize()) : 0;
  return base.Derive(delta, gfx::Font::NORMAL, w);
}

// Kept verbatim with Chromium's VerticalTabView modifier mapping: Command on
// macOS, Control everywhere else.
bool IsSelectionModifierDown(const ui::MouseEvent& event) {
#if BUILDFLAG(IS_MAC)
  return event.IsCommandDown();
#else
  return event.IsControlDown();
#endif
}

enum class GroupReorderDirection {
  kPrevious,
  kNext,
};

// Copied from Chromium 150's
// event_utils::GetReorderCommandForKeyboardEvent() vertical branch. Keeping
// the tiny mapping local preserves cmux_chrome_ui's //ui-only build boundary.
std::optional<GroupReorderDirection> GetGroupReorderCommandForKeyboardEvent(
    const ui::KeyEvent& event) {
  constexpr int kModifierFlag =
#if BUILDFLAG(IS_MAC)
      ui::EF_COMMAND_DOWN;
#else
      ui::EF_CONTROL_DOWN;
#endif

#if BUILDFLAG(IS_MAC)
  if (event.IsShiftDown()) {
    if (event.key_code() == ui::VKEY_HOME) {
      return GroupReorderDirection::kPrevious;
    }
    if (event.key_code() == ui::VKEY_END) {
      return GroupReorderDirection::kNext;
    }
  }
#endif

  if (event.type() != ui::EventType::kKeyPressed ||
      (event.flags() & kModifierFlag) == 0) {
    return std::nullopt;
  }

  switch (event.key_code()) {
    case ui::VKEY_UP:
      return GroupReorderDirection::kPrevious;
    case ui::VKEY_DOWN:
      return GroupReorderDirection::kNext;
    default:
      return std::nullopt;
  }
}

}  // namespace

// Workspace-neutral adaptation of TabDragController's EventTracker. Chromium
// listens application-wide while dragging: Escape reverts the drag, while any
// other key completes it. The native-window context keeps delivery scoped to
// this cmux window.
class CmuxRailDragEventTracker : public ui::EventObserver {
 public:
  CmuxRailDragEventTracker(base::OnceClosure end_drag_callback,
                           base::OnceClosure revert_drag_callback,
                           gfx::NativeWindow context)
      : end_drag_callback_(std::move(end_drag_callback)),
        revert_drag_callback_(std::move(revert_drag_callback)) {
    event_monitor_ = views::EventMonitor::CreateApplicationMonitor(
        this, context, {ui::EventType::kKeyPressed});
  }
  CmuxRailDragEventTracker(const CmuxRailDragEventTracker&) = delete;
  CmuxRailDragEventTracker& operator=(const CmuxRailDragEventTracker&) =
      delete;
  ~CmuxRailDragEventTracker() override = default;

  void Stop() {
    event_monitor_.reset();
    end_drag_callback_.Reset();
    revert_drag_callback_.Reset();
  }

 private:
  // ui::EventObserver:
  void OnEvent(const ui::Event& event) override {
    CHECK(event.IsKeyEvent());
    base::OnceClosure callback;
    if (event.AsKeyEvent()->key_code() == ui::VKEY_ESCAPE) {
      callback = std::move(revert_drag_callback_);
    } else {
      callback = std::move(end_drag_callback_);
    }
    // Stop monitoring before the callback ends the drag. CmuxRail retains this
    // inert tracker until the next drag so it is never destroyed from inside
    // its own EventObserver callback.
    event_monitor_.reset();
    end_drag_callback_.Reset();
    revert_drag_callback_.Reset();
    if (callback) {
      std::move(callback).Run();
    }
  }

  base::OnceClosure end_drag_callback_;
  base::OnceClosure revert_drag_callback_;
  std::unique_ptr<views::EventMonitor> event_monitor_;
};

// Workspace-neutral adaptation of Chromium 150's
// VerticalDraggedTabsContainer::InitializeDragStartAnimation. The source uses
// a rich 200ms EASE_IN_OUT slide so discontiguous selected tabs converge on the
// source tab without snapping. See THIRD_PARTY_NOTICES.md for the pinned source.
class CmuxRailDragStartAnimation : public gfx::AnimationDelegate {
 public:
  explicit CmuxRailDragStartAnimation(CmuxRail* rail)
      : rail_(rail), animation_(this) {}
  CmuxRailDragStartAnimation(const CmuxRailDragStartAnimation&) = delete;
  CmuxRailDragStartAnimation& operator=(const CmuxRailDragStartAnimation&) =
      delete;
  ~CmuxRailDragStartAnimation() override = default;

  void Start(base::TimeTicks drag_start_time) {
    static constexpr base::TimeDelta kDragStartAnimationDuration =
        base::Milliseconds(200);
    const base::TimeDelta drag_start_animation_duration =
        gfx::Animation::RichAnimationDuration(kDragStartAnimationDuration);
    static constexpr gfx::Tween::Type kStartDragAnimationTweenType =
        gfx::Tween::Type::EASE_IN_OUT;
    const base::TimeDelta drag_time_elapsed =
        base::TimeTicks::Now() - drag_start_time;
    if (drag_time_elapsed >= drag_start_animation_duration) {
      Stop();
      return;
    }

    animation_.SetTweenType(kStartDragAnimationTweenType);
    animation_.SetSlideDuration(drag_start_animation_duration);
    // Chromium seeds this from time already spent setting up the drag instead
    // of restarting the consolidation transition after reparenting.
    animation_.Reset(gfx::Tween::CalculateValue(
        kStartDragAnimationTweenType,
        drag_time_elapsed.InMillisecondsF() /
            drag_start_animation_duration.InMilliseconds()));
    animation_.Show();
  }

  void Stop() {
    animation_.Stop();
    animation_.Reset(0.0);
  }

  bool is_animating() const { return animation_.is_animating(); }
  double GetCurrentValue() const { return animation_.GetCurrentValue(); }

 private:
  void AnimationProgressed(const gfx::Animation* animation) override {
    CHECK_EQ(animation, &animation_);
    if (rail_) {
      rail_->OnDragStartAnimationProgressed();
    }
  }

  void AnimationEnded(const gfx::Animation* animation) override {
    CHECK_EQ(animation, &animation_);
    if (rail_) {
      rail_->OnDragStartAnimationEnded();
    }
  }

  raw_ptr<CmuxRail> rail_ = nullptr;
  gfx::SlideAnimation animation_;
};

// Copied from Chromium 150's TabDragScrollHandler and adapted only to own the
// ScrollView contents-scrolled subscription used by CmuxRail. The exact 5-DIP
// increment and 20ms timer cadence are intentionally retained.
class CmuxRailDragScrollHandler {
 public:
  CmuxRailDragScrollHandler() = default;
  CmuxRailDragScrollHandler(const CmuxRailDragScrollHandler&) = delete;
  CmuxRailDragScrollHandler& operator=(const CmuxRailDragScrollHandler&) =
      delete;
  ~CmuxRailDragScrollHandler() { End(); }

  void Begin(views::ScrollView& scroll_view,
             base::RepeatingClosure on_contents_scrolled) {
    End();
    scroll_view_ = &scroll_view;
    on_scrolled_subscription_ = scroll_view.AddContentsScrolledCallback(
        std::move(on_contents_scrolled));
  }

  void OnDraggedViewPositionUpdated(
      views::ScrollView& scroll_view,
      const gfx::Rect& dragged_view_bounds_in_scroll_view) {
    views::View* host_view = scroll_view.contents();
    if (!host_view) {
      StopScrolling();
      return;
    }

    constexpr float kScrollIncrement = 5;
    const gfx::Rect visible_bounds = host_view->GetVisibleBounds();
    if (visible_bounds.bottom() < host_view->height() &&
        dragged_view_bounds_in_scroll_view.bottom() >= scroll_view.height()) {
      StartOrContinueScrolling(scroll_view, kScrollIncrement);
    } else if (visible_bounds.y() > 0 &&
               dragged_view_bounds_in_scroll_view.y() <= 0) {
      StartOrContinueScrolling(scroll_view, -kScrollIncrement);
    } else {
      StopScrolling();
    }
  }

  void StopScrolling() { scroll_timer_.Stop(); }

  void End() {
    StopScrolling();
    on_scrolled_subscription_.reset();
    scroll_view_ = nullptr;
  }

 private:
  void StartOrContinueScrolling(views::ScrollView& scroll_view,
                                float vertical_increment) {
    scroll_view_ = &scroll_view;
    vertical_scroll_increment_ = vertical_increment;
    if (scroll_timer_.IsRunning()) {
      return;
    }

    constexpr base::TimeDelta kScrollTimerDelay = base::Milliseconds(20);
    scroll_timer_.Start(
        FROM_HERE, kScrollTimerDelay,
        base::BindRepeating(&CmuxRailDragScrollHandler::UpdateScrollOffset,
                            base::Unretained(this)));
  }

  void UpdateScrollOffset() {
    if (scroll_view_) {
      scroll_view_->ScrollByOffset({0, vertical_scroll_increment_});
    }
  }

  raw_ptr<views::ScrollView> scroll_view_ = nullptr;
  base::RepeatingTimer scroll_timer_;
  float vertical_scroll_increment_ = 0;
  std::optional<base::CallbackListSubscription> on_scrolled_subscription_;
};

void RailDelegate::OnExtendWorkspaceSelection(WorkspaceId id) {
  OnSelectWorkspace(id);
}

void RailDelegate::OnAddWorkspaceSelectionFromAnchorTo(WorkspaceId id) {
  OnSelectWorkspace(id);
}

void RailDelegate::OnToggleWorkspaceSelection(WorkspaceId id) {
  OnSelectWorkspace(id);
}

void RailDelegate::OnActivateWorkspaceInSelection(WorkspaceId id) {
  OnSelectWorkspace(id);
}

bool RailDelegate::IsWorkspaceSelected(WorkspaceId) const {
  return false;
}

WorkspaceSelectionState RailDelegate::GetWorkspaceSelectionState() const {
  return {};
}

void RailDelegate::OnRestoreWorkspaceSelectionState(
    const WorkspaceSelectionState&) {}

void RailDelegate::OnMoveWorkspaceSelection(WorkspaceId context,
                                            WorkspaceId new_parent,
                                            int index) {
  OnMoveWorkspace(context, new_parent, index);
}

void RailDelegate::OnMoveWorkspaceGroup(WorkspaceGroupId, int) {}

std::optional<RecentWorkspaceGroup>
RailDelegate::GetMostRecentWorkspaceGroupForContextMenu() const {
  return std::nullopt;
}

bool RailDelegate::IsNewWorkspaceContextActionEnabled(
    NewWorkspaceContextAction action) const {
  return action != NewWorkspaceContextAction::kNewWorkspaceInRecentGroup;
}

void RailDelegate::OnNewWorkspaceContextAction(
    NewWorkspaceContextAction action,
    WorkspaceGroupId) {
  if (action == NewWorkspaceContextAction::kNewWorkspace) {
    OnNewWorkspace();
  }
}

bool RailDelegate::GetNewWorkspaceContextAccelerator(
    NewWorkspaceContextAction,
    ui::Accelerator*) const {
  return false;
}

bool RailDelegate::IsWorkspaceContextActionEnabled(
    WorkspaceId,
    WorkspaceContextAction) const {
  return false;
}

bool RailDelegate::IsWorkspaceContextActionToggled(
    WorkspaceId,
    WorkspaceContextAction) const {
  return false;
}

bool RailDelegate::IsWorkspaceContextMenuSimplificationEnabled() const {
  return false;
}

int RailDelegate::WorkspaceContextUrlCount(WorkspaceId) const {
  return 1;
}

int RailDelegate::WorkspaceContextTargetCount(WorkspaceId) const {
  return 1;
}

WorkspaceHoverCardData RailDelegate::GetWorkspaceHoverCardData(
    WorkspaceId) const {
  return {};
}

std::unique_ptr<WorkspaceHoverCardPreviewRequest>
RailDelegate::RequestWorkspaceHoverCardPreview(
    WorkspaceId,
    SurfaceTabId,
    uintptr_t,
    base::RepeatingCallback<void(gfx::ImageSkia)>) {
  return nullptr;
}

bool RailDelegate::IsOptionalWorkspaceContextMenuCommandEnabled(
    WorkspaceId,
    int) const {
  return false;
}

bool RailDelegate::IsOptionalWorkspaceContextMenuCommandChecked(
    WorkspaceId,
    int) const {
  return false;
}

bool RailDelegate::IsOptionalWorkspaceContextMenuCommandVisible(
    WorkspaceId,
    int) const {
  return false;
}

bool RailDelegate::IsWorkspaceGroupContextActionEnabled(
    WorkspaceGroupId,
    WorkspaceGroupContextAction) const {
  return false;
}

// Workspace adaptation of Chromium's TabCloseButton with Helium's
// RoundRectHighlightPathGenerator patch applied. Only the accessible name and
// pressed callback differ from the pinned tab implementation.
class CmuxRailCloseButton : public views::LabelButton,
                            public views::MaskedTargeterDelegate {
  METADATA_HEADER(CmuxRailCloseButton, views::LabelButton)

 public:
  using MouseEventCallback =
      base::RepeatingCallback<void(views::View*, const ui::MouseEvent&)>;

  CmuxRailCloseButton(PressedCallback pressed_callback,
                      MouseEventCallback mouse_event_callback)
      : views::LabelButton(std::move(pressed_callback)),
        mouse_event_callback_(std::move(mouse_event_callback)) {
    SetEventTargeter(std::make_unique<views::ViewTargeter>(this));
    GetViewAccessibility().SetName(u"Close workspace");
    SetFocusBehavior(FocusBehavior::ACCESSIBLE_ONLY);

    // A close can open a confirmation UI instead of immediately destroying
    // the row, so the click action must reset the ink-drop state.
    SetHasInkDropActionOnClick(true);
    views::InkDrop::Get(this)->SetMode(views::InkDropHost::InkDropMode::ON);
    views::InkDrop::Get(this)->SetHighlightOpacity(0.16f);
    views::InkDrop::Get(this)->SetVisibleOpacity(0.14f);

    SetImageCentered(true);

    // Chromium deliberately makes this immediate to avoid close mis-clicks.
    SetAnimationDuration(base::TimeDelta());
    views::InkDrop::Get(this)->GetInkDrop()->SetHoverHighlightFadeDuration(
        base::TimeDelta());

    image_container_view()->DestroyLayer();
    label()->SetHandlesTooltips(false);

    // These are Helium's exact patched close-highlight values.
    auto ink_drop_highlight_path =
        std::make_unique<views::RoundRectHighlightPathGenerator>(
            gfx::Insets::VH(1, 0), 4);
    ink_drop_highlight_path->set_use_contents_bounds(true);
    ink_drop_highlight_path->set_use_mirrored_rect(true);
    views::HighlightPathGenerator::Install(
        this, std::move(ink_drop_highlight_path));

    SetInstallFocusRingOnFocus(true);
    auto ring_highlight_path =
        std::make_unique<views::RoundRectHighlightPathGenerator>(
            gfx::Insets::VH(1, 0), 4);
    ring_highlight_path->set_use_contents_bounds(true);
    views::FocusRing::Get(this)->SetPathGenerator(
        std::move(ring_highlight_path));

    UpdateIcon();
  }

  void SetColors(SkColor foreground,
                 SkColor background,
                 ui::ColorId focus_ring_color) {
    if (foreground_ == foreground && background_ == background &&
        focus_ring_color_ == focus_ring_color) {
      return;
    }
    foreground_ = foreground;
    background_ = background;
    focus_ring_color_ = focus_ring_color;
    views::InkDrop::Get(this)->SetBaseColor(
        color_utils::GetColorWithMaxContrast(background_));
    views::FocusRing::Get(this)->SetColorId(focus_ring_color_);
    UpdateIcon();
  }

  bool OnMousePressed(const ui::MouseEvent& event) override {
    mouse_event_callback_.Run(this, event);
    const bool handled = views::LabelButton::OnMousePressed(event);
    // Middle clicks deliberately fall through to the workspace row.
    return !event.IsMiddleMouseButton() && handled;
  }

  void OnMouseReleased(const ui::MouseEvent& event) override {
    mouse_event_callback_.Run(this, event);
    views::Button::OnMouseReleased(event);
  }

  void OnMouseMoved(const ui::MouseEvent& event) override {
    mouse_event_callback_.Run(this, event);
    views::Button::OnMouseMoved(event);
  }

  views::View* GetTooltipHandlerForPoint(
      const gfx::Point& point) override {
    if (!HitTestPoint(point)) {
      return nullptr;
    }
    return GetEventHandlerForPoint(point);
  }

  void OnGestureEvent(ui::GestureEvent* event) override {
    // A close-button touch sequence belongs wholly to the button; allowing a
    // gesture to bubble would also select or start dragging the workspace.
    views::LabelButton::OnGestureEvent(event);
    event->SetHandled();
  }

  void AddLayerToRegion(ui::Layer* new_layer,
                        views::LayerRegion region) override {
    image_container_view()->SetPaintToLayer();
    image_container_view()->layer()->SetFillsBoundsOpaquely(false);
    ink_drop_container()->SetVisible(true);
    ink_drop_container()->AddLayerToRegion(new_layer, region);
  }

  void RemoveLayerFromRegions(ui::Layer* old_layer) override {
    ink_drop_container()->RemoveLayerFromRegions(old_layer);
    ink_drop_container()->SetVisible(false);
    image_container_view()->DestroyLayer();
  }

  gfx::Size CalculatePreferredSize(
      const views::SizeBounds& available_size) const override {
    return WorkspaceCloseButtonSize();
  }

 private:
  views::View* TargetForRect(views::View* root,
                             const gfx::Rect& rect) override {
    CHECK_EQ(root, this);
    if (!views::UsePointBasedTargeting(rect)) {
      return views::ViewTargeterDelegate::TargetForRect(root, rect);
    }
    gfx::Rect contents_bounds = GetMirroredRect(GetContentsBounds());

#if defined(USE_AURA)
    // Match TabCloseButton: touch gets the complete padded button bounds.
    if (aura::Env::GetInstance()->is_touch_down()) {
      contents_bounds = GetLocalBounds();
    }
#endif

    return contents_bounds.Intersects(rect) ? this : parent();
  }

  bool GetHitTestMask(SkPath* mask) const override {
    *mask = SkPath::Rect(
        gfx::RectToSkRect(GetMirroredRect(GetContentsBounds())));
    return true;
  }

  void UpdateIcon() {
    for (views::Button::ButtonState state :
         {views::Button::STATE_NORMAL, views::Button::STATE_HOVERED,
          views::Button::STATE_PRESSED}) {
      SetImageModel(state,
                    ui::ImageModel::FromVectorIcon(WorkspaceCloseIcon(),
                                                   foreground_,
                                                   WorkspaceCloseIconSize()));
    }
  }

  MouseEventCallback mouse_event_callback_;
  SkColor foreground_ = SK_ColorBLACK;
  SkColor background_ = SK_ColorWHITE;
  ui::ColorId focus_ring_color_ = ui::kColorSysStateFocusRing;
};

BEGIN_METADATA(CmuxRailCloseButton)
END_METADATA

// ---- One workspace row ------------------------------------------------------
class CmuxRailRenameField : public views::Textfield {
  METADATA_HEADER(CmuxRailRenameField, views::Textfield)

 public:
  explicit CmuxRailRenameField(CmuxRailRow* row) : row_(row) {}
  CmuxRailRenameField(const CmuxRailRenameField&) = delete;
  CmuxRailRenameField& operator=(const CmuxRailRenameField&) = delete;
  ~CmuxRailRenameField() override = default;

  void Detach() { row_ = nullptr; }
  void OnBlur() override;

 private:
  raw_ptr<CmuxRailRow> row_;
};

class CmuxRailRow : public views::View,
                    public gfx::AnimationDelegate,
                    public views::MaskedTargeterDelegate,
                    public views::ContextMenuController,
                    public views::TextfieldController,
                    public views::ViewObserver {
  METADATA_HEADER(CmuxRailRow, views::View)

 public:
  explicit CmuxRailRow(CmuxRail* rail)
      : rail_(rail), hover_animation_(this) {
    SetNotifyEnterExitOnChild(true);
    SetEventTargeter(std::make_unique<views::ViewTargeter>(this));
    set_context_menu_controller(this);
    SetFocusBehavior(FocusBehavior::ALWAYS);
    views::FocusRing::Install(this);
    views::InstallRoundRectHighlightPathGenerator(
        this, gfx::Insets(), kGroupHeaderCornerRadius);
    auto* focus_ring = views::FocusRing::Get(this);
    focus_ring->SetHaloInset(0.0f);
    focus_ring->SetOutsetFocusRingDisabled(true);
    focus_ring->SetColorId(ui::kColorSysStateFocusRing);
    GetViewAccessibility().SetRole(ax::mojom::Role::kTab);
    // Rows can paint once between construction and their first SetItem().
    // Mark that transient state explicitly empty; SetItem() replaces it with
    // the workspace title before the row becomes interactive.
    GetViewAccessibility().SetName(
        std::u16string(), ax::mojom::NameFrom::kAttributeExplicitlyEmpty);
    icon_ = AddChildView(std::make_unique<views::ImageView>());
    icon_->SetCanProcessEventsWithinSubtree(false);
    label_ = AddChildView(std::make_unique<views::Label>());
    label_->SetHorizontalAlignment(gfx::ALIGN_TO_HEAD);
    label_->SetElideBehavior(gfx::FADE_TAIL);
    label_->SetHandlesTooltips(false);
    label_->SetAutoColorReadabilityEnabled(false);
    label_->SetBackgroundColor(SK_ColorTRANSPARENT);
    label_->SetSkipSubpixelRenderingOpacityCheck(true);
    label_->SetCanProcessEventsWithinSubtree(false);
    close_ = AddChildView(std::make_unique<CmuxRailCloseButton>(
        base::BindRepeating([] {}),
        base::BindRepeating(
            &CmuxRailRow::OnCloseButtonMouseEvent,
            base::Unretained(this))));
    close_->SetVisible(false);
    close_button_observation_.Observe(close_);
  }
  CmuxRailRow(const CmuxRailRow&) = delete;
  CmuxRailRow& operator=(const CmuxRailRow&) = delete;
  ~CmuxRailRow() override {
    if (rename_field_) {
      rename_field_->Detach();
    }
  }

  WorkspaceId workspace_id() const { return id_; }
  WorkspaceGroupId group_id() const { return group_; }
  WorkspaceId parent_id() const { return parent_; }
  int depth() const { return depth_; }
  bool is_group() const { return false; }
  bool selected() const { return selected_; }
  bool active() const { return active_; }
  const std::string& title() const { return title_; }

  gfx::Size CalculatePreferredSize(
      const views::SizeBounds& available_size) const override {
    return gfx::Size(available_size.width().is_bounded()
                         ? available_size.width().value()
                         : width(),
                     kWorkspaceRowHeight);
  }

  void set_config(const RailConfig* cfg) {
    cfg_ = cfg;
    hover_animation_.SetSlideDuration(
        gfx::Animation::RichAnimationDuration(
            kWorkspaceGlowHoverAnimationDuration));
    if (!gfx::Animation::ShouldRenderRichAnimation()) {
      hover_animation_.Stop();
      hover_animation_.Reset(hovered_ && !active_ ? 1.0 : 0.0);
    }
    UpdateIconVisibility();
  }

  void set_display_mode(sidebar_metrics::SidebarMode mode) {
    if (display_mode_ == mode) {
      return;
    }
    display_mode_ = mode;
    const bool show_labels = sidebar_metrics::ShowsLabels(mode);
    label_->SetVisible(show_labels && !editing_);
    if (rename_field_) {
      rename_field_->SetVisible(show_labels && editing_);
    }
    if (!show_labels) {
      close_->SetVisible(false);
    }
    UpdateIconVisibility();
    InvalidateLayout();
    SchedulePaint();
  }

  void SetItem(const RailItem& item,
               WorkspaceId parent,
               const std::string& title,
               bool active,
               bool selected,
               const gfx::ImageSkia& icon) {
    CHECK(cfg_);
    id_ = item.workspace;
    close_->SetCallback(base::BindRepeating(
        &CmuxRail::CloseRow, base::Unretained(rail_), id_));
    parent_ = parent;
    depth_ = item.depth;
    group_ = item.group;
    active_ = active;
    selected_ = selected;
    GetViewAccessibility().SetIsSelected(selected_);
    UpdateHoverState();
    title_ = title;
    icon_image_ = icon;
    icon_->SetImage(icon.isNull() ? ui::ImageModel()
                                  : ui::ImageModel::FromImageSkia(icon));
    icon_->SetImageSize(gfx::Size(kWorkspaceIconSize, kWorkspaceIconSize));
    std::u16string text =
        base::UTF8ToUTF16(!title.empty() ? title : "Workspace");
    label_->SetText(text);
    SetTooltipText(text);
    GetViewAccessibility().SetName(text,
                                   ax::mojom::NameFrom::kAttribute);
    UpdateIconVisibility();
    ApplyConfigToLabel();
    InvalidateLayout();
    SchedulePaint();
    rail_->OnWorkspaceRowDataChanged(this);
  }

  // Re-apply config-derived label props (font + color). Called both on item
  // refresh and on live config changes so font size / family / weight / text
  // colors update immediately, not just on the next model Update().
  void ApplyConfigToLabel() {
    if (!cfg_ || !label_ || !close_) {
      return;
    }
    label_->SetFontList(MakeFont(*cfg_, false, false));
    const SkColor text_color = ForegroundColor();
    label_->SetEnabledColor(text_color);
    if (rename_field_) {
      rename_field_->SetFontList(MakeFont(*cfg_, false, false));
      rename_field_->SetColor(text_color);
    }
    close_->SetColors(ForegroundColor(), TargetBackgroundColor(),
                      ui::kColorSysStateFocusRing);
  }

  void Layout(PassKey) override {
    CHECK(cfg_);
    const int icon_x = IconX();
    icon_->SetBoundsRect(gfx::Rect(icon_x, (height() - kWorkspaceIconSize) / 2,
                                   kWorkspaceIconSize, kWorkspaceIconSize));
    if (!sidebar_metrics::ShowsLabels(display_mode_)) {
      label_->SetBoundsRect(gfx::Rect());
      if (rename_field_) {
        rename_field_->SetBoundsRect(gfx::Rect());
      }
      close_->SetVisible(false);
      return;
    }
    int text_x = TextX();
    int right = RightInset();
    const gfx::Rect text_bounds(
        text_x, 0, std::max(0, width() - text_x - right), height());
    label_->SetBoundsRect(text_bounds);
    if (rename_field_) {
      rename_field_->SetBoundsRect(text_bounds);
    }
    const bool show_close =
        cfg_->show_close_on_hover &&
        (active_ || hovered_ || HasFocus() || close_->HasFocus());
    close_->SetVisible(show_close);
    const gfx::Size close_size = close_->GetPreferredSize();
    close_->SetBoundsRect(
        gfx::Rect(width() - kWorkspaceHorizontalInset -
                      (kWorkspaceIconSize + close_size.width()) / 2,
                  (height() - close_size.height()) / 2, close_size.width(),
                  close_size.height()));
  }

  void OnPaintBackground(gfx::Canvas* canvas) override {
    CHECK(cfg_);
    const double hover_value =
        active_ ? 0.0 : hover_animation_.GetCurrentValue();
    if (active_ || selected_ || hover_value > 0.0) {
      gfx::RectF b(GetLocalBounds());
      cc::PaintFlags f;
      f.setAntiAlias(true);
      if (active_ || selected_) {
        f.setColor(ActiveBackgroundColor());
      } else {
        // Helium's inactive base is the location-bar RGB at alpha zero and its
        // hover target is the same RGB at 45% alpha.
        f.setColor(SkColorSetA(
            ActiveBackgroundColor(),
            static_cast<U8CPU>(std::clamp(hover_value, 0.0, 1.0) *
                               kTabInactiveHoverAlpha)));
      }
      canvas->DrawRoundRect(b, kGroupHeaderCornerRadius, f);
    }

    // Configured workspace glyphs deliberately take precedence over favicons.
    // Exact title wins over the optional "*" default, arbitrary Unicode is
    // preserved, and an explicitly empty string suppresses the icon.
    const std::string* configured_icon = ConfiguredIcon();
    if (configured_icon && !configured_icon->empty()) {
      const bool icons_only =
          !sidebar_metrics::ShowsLabels(display_mode_);
      const gfx::FontList icon_font =
          icons_only
              ? MakeFont(*cfg_, false, selected_).DeriveWithSizeDelta(2)
              : MakeFont(*cfg_, false, selected_);
      const gfx::Rect icon_bounds =
          icons_only
              ? GetLocalBounds()
              : gfx::Rect(IconX(), 0, kWorkspaceIconSize, height());
      canvas->DrawStringRectWithFlags(
          base::UTF8ToUTF16(*configured_icon), icon_font, ForegroundColor(),
          icon_bounds, gfx::Canvas::TEXT_ALIGN_CENTER);
    } else if (!configured_icon && icon_image_.isNull()) {
      // A workspace without a selected web favicon uses a small four-pane
      // mark; the 16 DIP box stays identical to Helium's favicon geometry.
      cc::PaintFlags mark;
      mark.setAntiAlias(true);
      mark.setColor(ForegroundColor());
      for (int dy : {0, 6}) {
        for (int dx : {0, 6}) {
          canvas->DrawRoundRect(gfx::RectF(IconX() + 3 + dx,
                                           height() / 2.f - 6.f + dy, 4.f, 4.f),
                                1.f, mark);
        }
      }
    }
  }

  void OnMouseMoved(const ui::MouseEvent& event) override {
    // Match Chromium's repair path for platforms where enter/exit delivery can
    // be flaky. Touch-synthesized mouse motion must not create hover state.
    if (!(event.flags() & ui::EF_FROM_TOUCH)) {
      SetHovered(true);
    }
  }
  void OnMouseEntered(const ui::MouseEvent& event) override {
    // VerticalTabView updates the card target before filtering synthesized
    // touch hover. The gesture event immediately below dismisses it again.
    rail_->OnWorkspaceRowMouseEntered(this);
    if (event.flags() & ui::EF_FROM_TOUCH) {
      return;
    }
    SetHovered(true);
  }
  void OnMouseExited(const ui::MouseEvent&) override {
    SetHovered(false);
  }

  void OnGestureEvent(ui::GestureEvent* event) override {
    CHECK(event);
    rail_->OnWorkspaceRowEvent();

    // Chromium's VerticalTabView deliberately handles TapDown without
    // initializing a drag so the surrounding ScrollView can still claim a
    // touch scroll. A long press is the point at which selection and dragging
    // begin. The follow-up gesture events are handled by Chromium's
    // VerticalTabDragHandler; cmux keeps the same state machine on the row
    // because its workspace drag adapter is intentionally local to the rail.
    switch (event->type()) {
      case ui::EventType::kGestureTapDown:
        if (touch_drag_started_) {
          rail_->EndRowDrag(/*commit=*/false);
        }
        touch_drag_initialized_ = false;
        touch_drag_started_ = false;
        event->SetHandled();
        break;

      case ui::EventType::kGestureTap:
        touch_drag_initialized_ = false;
        if (!selected_) {
          rail_->SelectRow(id_);
        } else if (!active_) {
          // Match Enter/Space and a plain mouse activation: a workspace can
          // remain selected as part of a multi-selection without being the
          // visible workspace. A touch tap must activate that member.
          rail_->ActivateRowInSelection(id_);
        }
        event->SetHandled();
        break;

      case ui::EventType::kGestureLongPress: {
        original_selection_on_press_ = rail_->GetWorkspaceSelectionState();
        if (!selected_) {
          // Match VerticalTabView: select before drag initialization, while
          // retaining the pre-press selection for a canceled drag.
          rail_->SelectRow(id_);
        }
        press_screen_pt_ = event->location();
        ConvertPointToScreen(this, &press_screen_pt_);
        touch_drag_initialized_ = true;
        touch_drag_started_ = false;
        event->SetHandled();
        break;
      }

      case ui::EventType::kGestureScrollBegin:
      case ui::EventType::kGestureScrollUpdate:
        // TabDragController applies the same 10-DIP Euclidean threshold to
        // touch after LongPress as it does to mouse presses.
        if (touch_drag_initialized_) {
          gfx::Point screen = event->location();
          ConvertPointToScreen(this, &screen);
          if (!touch_drag_started_) {
            if ((screen - press_screen_pt_).LengthSquared() <=
                kVerticalTabDragThreshold * kVerticalTabDragThreshold) {
              event->SetHandled();
              break;
            }
            rail_->StartRowDrag(this, press_screen_pt_, screen,
                                original_selection_on_press_);
            touch_drag_started_ = true;
          } else {
            rail_->UpdateRowDrag(screen);
          }
          event->SetHandled();
        }
        break;

      case ui::EventType::kGestureLongTap: {
        // MenuRunner enters a nested loop. Chromium's drag handler is canceled
        // as part of opening a tab context menu; do the equivalent explicitly
        // for cmux's local workspace drag adapter before entering that loop.
        if (touch_drag_started_) {
          rail_->EndRowDrag(/*commit=*/false);
        }
        touch_drag_initialized_ = false;
        touch_drag_started_ = false;
        gfx::Point screen = event->location();
        ConvertPointToScreen(this, &screen);
        rail_->ShowContextMenuForRow(id_, screen,
                                     ui::mojom::MenuSourceType::kTouch);
        event->SetHandled();
        break;
      }

      case ui::EventType::kGestureScrollEnd:
      case ui::EventType::kScrollFlingStart:
      case ui::EventType::kGestureEnd:
        if (touch_drag_initialized_) {
          if (touch_drag_started_) {
            rail_->EndRowDrag(/*commit=*/true);
          }
          touch_drag_initialized_ = false;
          touch_drag_started_ = false;
          event->SetHandled();
        }
        break;

      default:
        break;
    }
  }

  void OnFocus() override {
    views::View::OnFocus();
    rail_->OnWorkspaceRowFocused(this);
    ApplyConfigToLabel();
    InvalidateLayout();
  }
  void OnBlur() override {
    views::View::OnBlur();
    rail_->OnWorkspaceRowBlurred();
    ApplyConfigToLabel();
    InvalidateLayout();
  }
  void OnThemeChanged() override {
    views::View::OnThemeChanged();
    ApplyConfigToLabel();
    SchedulePaint();
  }
  void OnBoundsChanged(const gfx::Rect& previous_bounds) override {
    SetClipPath(GetPath());
    InvalidateLayout();
  }
  void UpdateParentLayer() override {
    views::View::UpdateParentLayer();
    if (layer()) {
      layer()->SetRoundedCornerRadius(
          gfx::RoundedCornersF(kGroupHeaderCornerRadius));
      layer()->SetIsFastRoundedCorner(true);
    }
  }
  void AddedToWidget() override {
    paint_as_active_subscription_ =
        GetWidget()->RegisterPaintAsActiveChangedCallback(base::BindRepeating(
            &CmuxRailRow::OnFrameActiveStateChanged, base::Unretained(this)));
    OnFrameActiveStateChanged();
    // Enter/exit events are not delivered while detached. Chromium repairs
    // hover state on reattachment so the close affordance appears immediately
    // if the pointer is already over this row.
    SetHovered(IsMouseHovered());
  }
  void RemovedFromWidget() override {
    paint_as_active_subscription_ = {};
    SetHovered(false);
  }

  bool OnMousePressed(const ui::MouseEvent& event) override {
    rail_->OnWorkspaceRowEvent();
    shift_pressed_on_mouse_down_ = event.IsShiftDown();

    const bool is_selection_press =
        event.IsOnlyLeftMouseButton() ||
        (event.IsOnlyRightMouseButton() &&
         (event.flags() & ui::EF_FROM_TOUCH));
    if (!is_selection_press) {
      no_drag_ = true;
      // ContextMenuController deliberately owns ordinary right-button timing:
      // Views opens on press or release according to the native platform and
      // also exposes the keyboard/accessibility context-menu action.
      return true;
    }
    press_screen_pt_ = event.location();
    ConvertPointToScreen(this, &press_screen_pt_);
    started_drag_ = false;
    no_drag_ = false;
    original_selection_on_press_ = rail_->GetWorkspaceSelectionState();

    // This is Chromium VerticalTabView::OnMousePressed's selection order,
    // adapted from tab indices to stable WorkspaceIds. Selection changes on
    // press so dragging an already-selected row carries the selected set.
    if (event.IsShiftDown() && IsSelectionModifierDown(event)) {
      rail_->AddSelectionFromAnchorToRow(id_);
    } else if (event.IsShiftDown()) {
      rail_->ExtendSelectionToRow(id_);
    } else if (IsSelectionModifierDown(event)) {
      rail_->ToggleRowSelection(id_);
      if (!rail_->IsRowSelected(id_)) {
        no_drag_ = true;
        return false;
      }
    } else if (!selected_) {
      rail_->SelectRow(id_);
    }
    return true;
  }
  bool OnMouseDragged(const ui::MouseEvent& event) override {
    if (no_drag_) {
      return false;
    }
    gfx::Point screen = event.location();
    ConvertPointToScreen(this, &screen);
    if (!started_drag_) {
      // TabDragController uses a 10-DIP Euclidean threshold, rather than the
      // generic Views axis-aligned 8-DIP threshold.
      if ((screen - press_screen_pt_).LengthSquared() <=
          kVerticalTabDragThreshold * kVerticalTabDragThreshold) {
        return true;
      }
      started_drag_ = true;
      rail_->StartRowDrag(this, press_screen_pt_, screen,
                          original_selection_on_press_);
    } else {
      rail_->UpdateRowDrag(screen);
    }
    return true;
  }
  void OnMouseReleased(const ui::MouseEvent& event) override {
    if (event.IsOnlyMiddleMouseButton()) {
      if (HitTestPoint(event.location())) {
        rail_->CloseRow(id_);
      }
    } else if (started_drag_) {
      rail_->EndRowDrag(/*commit=*/true);
    } else if (!no_drag_ && event.IsOnlyLeftMouseButton() &&
               !(event.IsShiftDown() || shift_pressed_on_mouse_down_) &&
               !IsSelectionModifierDown(event)) {
      // A plain click on one member of a multi-selection preserves the set on
      // press for drag, then collapses it to this row only on release.
      rail_->SelectRow(id_);
    }
    started_drag_ = false;
    no_drag_ = false;
    shift_pressed_on_mouse_down_ = false;
  }
  bool OnKeyPressed(const ui::KeyEvent& event) override {
    if (event.key_code() == ui::VKEY_RETURN && !active_) {
      if (selected_) {
        rail_->ActivateRowInSelection(id_);
      } else {
        rail_->SelectRow(id_);
      }
      return true;
    }
    const std::optional<GroupReorderDirection> reorder_direction =
        GetGroupReorderCommandForKeyboardEvent(event);
    if (!reorder_direction.has_value()) {
      return views::View::OnKeyPressed(event);
    }
    rail_->ShiftWorkspace(
        id_, *reorder_direction == GroupReorderDirection::kPrevious,
        (event.flags() & ui::EF_SHIFT_DOWN) != 0);
    return true;
  }
  bool OnKeyReleased(const ui::KeyEvent& event) override {
    if (event.key_code() == ui::VKEY_SPACE && !active_) {
      if (selected_) {
        rail_->ActivateRowInSelection(id_);
      } else {
        rail_->SelectRow(id_);
      }
      return true;
    }
    return views::View::OnKeyReleased(event);
  }

  void BeginRename() {
    if (!sidebar_metrics::ShowsLabels(display_mode_)) {
      return;
    }
    if (!rename_field_) {
      auto field = std::make_unique<CmuxRailRenameField>(this);
      field->set_controller(this);
      field->SetHorizontalAlignment(gfx::ALIGN_TO_HEAD);
      field->SetUseDefaultBorder(false);
      field->SetBorder(views::NullBorder());
      field->SetBackgroundEnabled(false);
      field->GetViewAccessibility().SetName(u"Workspace name");
      field->SetVisible(false);
      rename_field_ = AddChildView(std::move(field));
    }
    editing_ = true;
    rename_field_->SetText(base::UTF8ToUTF16(title_));
    ApplyConfigToLabel();
    label_->SetVisible(false);
    rename_field_->SetVisible(true);
    rename_field_->SelectAll(/*reversed=*/false);
    rename_field_->RequestFocus();
    InvalidateLayout();
    SchedulePaint();
  }

  void CommitRename() {
    if (!editing_ || !rename_field_) {
      return;
    }
    const std::string raw_name =
        base::UTF16ToUTF8(std::u16string(rename_field_->GetText()));
    const std::string name(
        base::TrimWhitespaceASCII(raw_name, base::TRIM_ALL));
    FinishRenameEditing();
    if (!name.empty() && name != title_) {
      rail_->RenameRow(id_, name);
    }
  }

  void CancelRename() {
    if (editing_) {
      FinishRenameEditing();
    }
  }

  bool HandleKeyEvent(views::Textfield* sender,
                      const ui::KeyEvent& event) override {
    if (!editing_ || sender != rename_field_ ||
        event.type() != ui::EventType::kKeyPressed) {
      return false;
    }
    if (event.key_code() == ui::VKEY_RETURN &&
        !rename_field_->IsIMEComposing()) {
      CommitRename();
      return true;
    }
    if (event.key_code() == ui::VKEY_ESCAPE) {
      CancelRename();
      return true;
    }
    return false;
  }

  void OnMouseCaptureLost() override {
    if (started_drag_) {
      rail_->EndRowDrag(/*commit=*/false);
      started_drag_ = false;
    }
    if (touch_drag_started_) {
      rail_->EndRowDrag(/*commit=*/false);
    }
    touch_drag_initialized_ = false;
    touch_drag_started_ = false;
    shift_pressed_on_mouse_down_ = false;
  }

  // views::ContextMenuController:
  void ShowContextMenuForViewImpl(
      views::View* source,
      const gfx::Point& point,
      ui::mojom::MenuSourceType source_type) override {
    if (source == this) {
      rail_->ShowContextMenuForRow(id_, point, source_type);
    }
  }

  // views::ViewObserver:
  void OnViewFocused(views::View* observed_view) override {
    if (observed_view == close_) {
      InvalidateLayout();
    }
  }
  void OnViewBlurred(views::View* observed_view) override {
    if (observed_view == close_) {
      InvalidateLayout();
    }
  }

  void SetDragging(bool dragging) {
    if (dragging_ == dragging) {
      return;
    }
    dragging_ = dragging;
    if (dragging_) {
      SetHovered(false);
    }
    UpdateHoverState();
    SchedulePaint();
  }

  void AnimationProgressed(const gfx::Animation* animation) override {
    if (animation == &hover_animation_) {
      SchedulePaint();
    }
  }
  void AnimationEnded(const gfx::Animation* animation) override {
    if (animation == &hover_animation_) {
      // GlowHoverController performs a synchronous layout at the end of the
      // hover animation so visibility-dependent children settle immediately.
      DeprecatedLayoutImmediately();
    }
  }
  void AnimationCanceled(const gfx::Animation* animation) override {
    if (animation == &hover_animation_) {
      SchedulePaint();
    }
  }

 private:
  void FinishRenameEditing() {
    editing_ = false;
    rename_field_->SetVisible(false);
    label_->SetVisible(sidebar_metrics::ShowsLabels(display_mode_));
    InvalidateLayout();
    SchedulePaint();
  }

  void OnCloseButtonMouseEvent(views::View*,
                               const ui::MouseEvent& event) {
    // TabCloseButton forwards press/release/move to its parent tab. Preserve
    // the parent row's event bookkeeping while leaving selection and middle
    // click handling to normal Views propagation.
    rail_->OnWorkspaceRowEvent();
    if (event.type() == ui::EventType::kMouseMoved &&
        !(event.flags() & ui::EF_FROM_TOUCH)) {
      SetHovered(true);
    }
  }

  void SetHovered(bool hovered) {
    if (hovered_ == hovered) {
      return;
    }
    hovered_ = hovered;
    ApplyConfigToLabel();
    UpdateHoverState();
    InvalidateLayout();
  }

  void UpdateHoverState() {
    // VerticalTabView's GlowHoverController is independent of the rail's
    // structural animation preference. It only snaps when rich animations are
    // disabled by the platform or the row is detached.
    if (!cfg_ || !gfx::Animation::ShouldRenderRichAnimation() ||
        !GetWidget()) {
      hover_animation_.Stop();
      hover_animation_.Reset(hovered_ ? 1.0 : 0.0);
      SchedulePaint();
      return;
    }
    if (hovered_) {
      hover_animation_.SetTweenType(gfx::Tween::EASE_OUT);
      hover_animation_.Show();
    } else {
      hover_animation_.SetTweenType(gfx::Tween::EASE_IN);
      hover_animation_.Hide();
    }
    SchedulePaint();
  }

  void OnFrameActiveStateChanged() {
    ApplyConfigToLabel();
    SchedulePaint();
  }

  const std::string* ConfiguredIcon() const {
    if (!cfg_) {
      return nullptr;
    }
    if (auto it = cfg_->icons.find(title_); it != cfg_->icons.end()) {
      return &it->second;
    }
    if (auto fallback = cfg_->icons.find("*");
        fallback != cfg_->icons.end()) {
      return &fallback->second;
    }
    return nullptr;
  }

  void UpdateIconVisibility() {
    if (icon_) {
      icon_->SetVisible(ConfiguredIcon() == nullptr);
    }
  }

  int IconX() const {
    if (sidebar_metrics::ShowsLabels(display_mode_)) {
      return kWorkspaceHorizontalInset;
    }
    return std::max(0, (width() - kWorkspaceIconSize) / 2);
  }

  int TextX() const {
    return kWorkspaceHorizontalInset + kWorkspaceIconSize +
           kWorkspaceImageLabelGap;
  }
  int RightInset() const {
    // VerticalTabView starts with a 7 DIP trailing inset. Its title keeps the
    // default 4 DIP child padding, and a visible 16 DIP close button consumes
    // another 20 DIP (16 + 4). Match those two exact flex outcomes.
    return cfg_ && cfg_->show_close_on_hover &&
                   (hovered_ || active_ || HasFocus() || close_->HasFocus())
               ? 31
               : 11;
  }

  SkPath GetPath() const {
    return SkPath::RRect(SkRRect::MakeRectXY(
        gfx::RectToSkRect(GetLocalBounds()), kGroupHeaderCornerRadius,
        kGroupHeaderCornerRadius));
  }

  bool GetHitTestMask(SkPath* mask) const override {
    *mask = GetPath();
    return true;
  }

  SkColor ActiveBackgroundColor() const {
    return ResolveProviderColor(this, kColorLocationBarBackground)
        .value_or(cfg_->sel_bg);
  }

  float GetHoverOpacity() const {
    const std::optional<SkColor> toolbar =
        ResolveProviderColor(this, kColorToolbar);
    if (!toolbar) {
      return static_cast<float>(kTabInactiveHoverAlpha) / SK_AlphaOPAQUE;
    }

    // Verbatim Helium contrast calculation: because the inactive token is
    // transparent, first composite it over the toolbar before asking for the
    // alpha needed to reach Chromium's standard/min-width contrast targets.
    const SkColor active_bg = ActiveBackgroundColor();
    const SkColor inactive_bg = SkColorSetA(active_bg, SK_AlphaTRANSPARENT);
    const SkColor inactive_contrast_bg =
        color_utils::GetResultingPaintColor(inactive_bg, *toolbar);
    const auto get_blend = [inactive_contrast_bg](SkColor target,
                                                  float contrast) {
      return color_utils::BlendForMinContrast(
          inactive_contrast_bg, inactive_contrast_bg, target, contrast);
    };
    const auto get_hover_opacity = [active_bg, &get_blend](float contrast) {
      return get_blend(active_bg, contrast).alpha /
             static_cast<float>(SK_AlphaOPAQUE);
    };
    constexpr float kStandardWidthContrast = 1.11f;
    constexpr float kMinWidthContrast = 1.19f;
    const float hover_opacity_min =
        get_hover_opacity(kStandardWidthContrast);
    const float hover_opacity_max = get_hover_opacity(kMinWidthContrast);

    // These widths and the quadratic interpolation are copied from Chromium
    // 150 VerticalTabView::GetHoverOpacity().
    constexpr float kWidthForMinHoverOpacity = 216.0f;
    constexpr float kWidthForMaxHoverOpacity = 32.0f;
    const float t = std::clamp(
        (kWidthForMinHoverOpacity - static_cast<float>(width())) /
            (kWidthForMinHoverOpacity - kWidthForMaxHoverOpacity),
        0.0f, 1.0f);
    return gfx::Tween::FloatValueBetween(
        t * t, hover_opacity_min, hover_opacity_max);
  }

  bool IsApparentlyActive() const {
    if (active_ || selected_) {
      return true;
    }
    // cmux's browser-neutral target does not link ui_features' GlassFrame
    // gate; its sidebar follows Chromium's non-glass branch.
    if (hovered_) {
      return GetHoverOpacity() > 0.5f;
    }
    return false;
  }

  SkColor ForegroundColor() const {
    const bool apparently_active = IsApparentlyActive();
    return ResolveProviderColor(
               this, apparently_active ? ui::kColorSysOnSurface
                                       : ui::kColorSysOnSurfaceSecondary)
        .value_or(apparently_active ? cfg_->sel_text : cfg_->text);
  }

  SkColor TargetBackgroundColor() const {
    if (active_ || selected_) {
      return ActiveBackgroundColor();
    }
    return SkColorSetA(ActiveBackgroundColor(),
                       hovered_ ? kTabInactiveHoverAlpha
                                : SK_AlphaTRANSPARENT);
  }

  raw_ptr<CmuxRail> rail_;
  raw_ptr<const RailConfig> cfg_ = nullptr;
  raw_ptr<views::ImageView> icon_ = nullptr;
  raw_ptr<views::Label> label_ = nullptr;
  raw_ptr<CmuxRailRenameField> rename_field_ = nullptr;
  raw_ptr<CmuxRailCloseButton> close_ = nullptr;
  WorkspaceId id_ = kInvalidId;
  WorkspaceId parent_ = kInvalidId;
  WorkspaceGroupId group_ = kInvalidId;
  int depth_ = 0;
  bool selected_ = false;
  bool active_ = false;
  bool hovered_ = false;
  bool dragging_ = false;
  gfx::ImageSkia icon_image_;
  std::string title_;

  gfx::Point press_screen_pt_;
  bool started_drag_ = false;
  bool no_drag_ = false;
  sidebar_metrics::SidebarMode display_mode_ =
      sidebar_metrics::SidebarMode::kExpanded;
  bool touch_drag_initialized_ = false;
  bool touch_drag_started_ = false;
  bool shift_pressed_on_mouse_down_ = false;
  bool editing_ = false;
  WorkspaceSelectionState original_selection_on_press_;
  gfx::SlideAnimation hover_animation_;
  base::CallbackListSubscription paint_as_active_subscription_;
  base::ScopedObservation<views::View, views::ViewObserver>
      close_button_observation_{this};
};

BEGIN_METADATA(CmuxRailRow)
END_METADATA

void CmuxRailRenameField::OnBlur() {
  views::Textfield::OnBlur();
  if (row_) {
    row_->CancelRename();
  }
}

BEGIN_METADATA(CmuxRailRenameField)
END_METADATA

// ---- Helium/Chromium-style shallow group chrome ----------------------------
namespace {

void ConfigureGroupEditorBubbleButton(views::LabelButton* button) {
  button->SetHasInkDropActionOnClick(true);

  auto highlight_path =
      std::make_unique<views::CircleHighlightPathGenerator>(gfx::Insets());
  highlight_path->set_use_contents_bounds(true);
  views::HighlightPathGenerator::Install(button, std::move(highlight_path));

  views::InkDrop::Get(button)->SetMode(views::InkDropHost::InkDropMode::ON);
  views::InkDrop::Get(button)->SetHighlightOpacity(0.2f);
  views::InkDrop::Get(button)->SetVisibleOpacity(0.08f);
  button->button_controller()->set_notify_action(
      views::ButtonController::NotifyAction::kOnPress);
  button->GetViewAccessibility().SetName(u"More Options");
  button->SetFocusBehavior(views::View::FocusBehavior::ACCESSIBLE_ONLY);
  button->SetPreferredSize(gfx::Size(kWorkspaceIconSize, kWorkspaceIconSize));
  button->SetImageCentered(true);
  button->SetVisible(false);
  views::FocusRing::Install(button);
}

}  // namespace

class CmuxRailGroupHeader : public views::View,
                            public views::ContextMenuController,
                            public views::FocusChangeListener {
  METADATA_HEADER(CmuxRailGroupHeader, views::View)

 public:
  explicit CmuxRailGroupHeader(CmuxRail* rail) : rail_(rail) {
    SetNotifyEnterExitOnChild(true);
    set_context_menu_controller(this);
    SetFocusBehavior(FocusBehavior::ALWAYS);
    GetViewAccessibility().SetRole(ax::mojom::Role::kTabList);
    GetViewAccessibility().SetIsEditable(true);
    views::FocusRing::Install(this);
    views::HighlightPathGenerator::Install(
        this, std::make_unique<views::RoundRectHighlightPathGenerator>(
                  gfx::Insets(kGroupHeaderFocusRingInset),
                  kGroupHeaderCornerRadius));
    label_ = AddChildView(std::make_unique<views::Label>());
    label_->SetHorizontalAlignment(gfx::ALIGN_TO_HEAD);
    label_->SetElideBehavior(gfx::FADE_TAIL);
    label_->SetAutoColorReadabilityEnabled(false);
    label_->SetCanProcessEventsWithinSubtree(false);
    editor_bubble_button_ =
        AddChildView(std::make_unique<views::LabelButton>(
            base::BindRepeating(&CmuxRailGroupHeader::ShowEditorBubble,
                                base::Unretained(this)),
            std::u16string()));
    ConfigureGroupEditorBubbleButton(editor_bubble_button_);
    collapse_icon_ = AddChildView(std::make_unique<views::ImageView>());
    collapse_icon_->SetCanProcessEventsWithinSubtree(false);
  }

  void SetGroup(const WorkspaceGroup& group, WorkspaceId representative) {
    group_ = group.id;
    representative_ = representative;
    color_ = group.color;
    collapsed_ = group.collapsed;
    label_->SetText(base::UTF8ToUTF16(group.title));
    UpdateColors();
    if (collapsed_) {
      GetViewAccessibility().SetIsCollapsed();
    } else {
      GetViewAccessibility().SetIsExpanded();
    }
    UpdateTooltipText();
    UpdateAccessibleName();
    SchedulePaint();
  }

  void SetWorkspaceContentString(std::u16string workspace_content) {
    workspace_content_ = std::move(workspace_content);
    UpdateTooltipText();
    UpdateAccessibleName();
  }

  void set_config(const RailConfig* config) {
    config_ = config;
    if (config_) {
      label_->SetFontList(MakeFont(*config_, false, false));
    }
    UpdateColors();
  }

  void set_display_mode(sidebar_metrics::SidebarMode mode) {
    if (display_mode_ == mode) {
      return;
    }
    display_mode_ = mode;
    const bool show_labels = sidebar_metrics::ShowsLabels(mode);
    label_->SetVisible(show_labels);
    if (!show_labels) {
      editor_bubble_button_->SetVisible(false);
    } else {
      UpdateEditorBubbleButtonVisibility();
    }
    InvalidateLayout();
    SchedulePaint();
  }

  void Layout(PassKey) override {
    constexpr int kIconWidth = 16;
    if (!sidebar_metrics::ShowsLabels(display_mode_)) {
      collapse_icon_->SetBoundsRect(
          gfx::Rect((width() - kIconWidth) / 2,
                    (height() - kIconWidth) / 2, kIconWidth, kIconWidth));
      label_->SetBoundsRect(gfx::Rect());
      editor_bubble_button_->SetBoundsRect(gfx::Rect());
      return;
    }
    int trailing_x = width() - kWorkspaceHorizontalInset;
    collapse_icon_->SetBoundsRect(
        gfx::Rect(trailing_x - kIconWidth, (height() - kIconWidth) / 2,
                  kIconWidth, kIconWidth));
    trailing_x -= kIconWidth;
    if (editor_bubble_button_->GetVisible()) {
      editor_bubble_button_->SetBoundsRect(
          gfx::Rect(trailing_x - kIconWidth, (height() - kIconWidth) / 2,
                    kIconWidth, kIconWidth));
      trailing_x -= kIconWidth;
    }
    label_->SetBoundsRect(gfx::Rect(
        kWorkspaceHorizontalInset, 0,
        std::max(0, trailing_x - kWorkspaceHorizontalInset),
        height()));
  }

  void OnThemeChanged() override {
    views::View::OnThemeChanged();
    UpdateColors();
    SchedulePaint();
  }

  void OnPaintBackground(gfx::Canvas* canvas) override {
    const SkColor background = GroupSkColor(this, color_);
    cc::PaintFlags fill;
    fill.setAntiAlias(true);
    fill.setColor(background);
    canvas->DrawRoundRect(gfx::RectF(GetLocalBounds()),
                          kGroupHeaderCornerRadius, fill);
  }

  bool OnMousePressed(const ui::MouseEvent& event) override {
    drag_armed_ = false;
    // Match TabGroupEditorBubbleTracker's posted-destruction guard: the bubble
    // deactivates before its widget is actually destroyed, so a second press
    // must not immediately reopen it.
    if (bubble_open_) {
      return false;
    }
    if (event.IsOnlyRightMouseButton()) {
      // Chromium opens the editor on mouse release. Waiting preserves native
      // context-click ordering and avoids focusing the title field mid-event.
      return true;
    }
    if (!event.IsOnlyLeftMouseButton()) {
      return false;
    }
    press_screen_pt_ = event.location();
    ConvertPointToScreen(this, &press_screen_pt_);
    started_drag_ = false;
    drag_armed_ = true;
    original_selection_on_press_ = rail_->GetWorkspaceSelectionState();
    return true;
  }

  bool OnMouseDragged(const ui::MouseEvent& event) override {
    if (!drag_armed_) {
      return false;
    }
    gfx::Point screen = event.location();
    ConvertPointToScreen(this, &screen);
    if (!started_drag_) {
      // TabDragController uses a 10-DIP Euclidean threshold before converting
      // a header press into a drag.
      if ((screen - press_screen_pt_).LengthSquared() <=
          kVerticalTabDragThreshold * kVerticalTabDragThreshold) {
        return true;
      }
      started_drag_ = true;
      rail_->StartGroupDrag(group_, press_screen_pt_, screen,
                            original_selection_on_press_);
    } else {
      rail_->UpdateRowDrag(screen);
    }
    return true;
  }

  void OnMouseReleased(const ui::MouseEvent& event) override {
    if (started_drag_) {
      rail_->EndRowDrag(/*commit=*/true);
    } else if (event.IsRightMouseButton()) {
      ShowEditorBubble();
    } else if (event.IsLeftMouseButton() && representative_ != kInvalidId) {
      rail_->ToggleRow(representative_);
    }
    started_drag_ = false;
    drag_armed_ = false;
  }

  void OnMouseCaptureLost() override {
    if (started_drag_) {
      rail_->EndRowDrag(/*commit=*/false);
      started_drag_ = false;
    }
    if (gesture_dragging_) {
      rail_->EndRowDrag(/*commit=*/false);
    }
    gesture_drag_initialized_ = false;
    gesture_dragging_ = false;
    drag_armed_ = false;
  }

  void OnGestureEvent(ui::GestureEvent* event) override {
    switch (event->type()) {
      case ui::EventType::kGestureTapDown:
        // Required so the touch system keeps this view as the target for the
        // subsequent long-press/long-tap sequence.
        if (gesture_dragging_) {
          rail_->EndRowDrag(/*commit=*/false);
        }
        gesture_drag_initialized_ = false;
        gesture_dragging_ = false;
        event->SetHandled();
        break;
      case ui::EventType::kGestureTap:
        gesture_drag_initialized_ = false;
        if (representative_ != kInvalidId) {
          rail_->ToggleRow(representative_);
        }
        event->SetHandled();
        break;
      case ui::EventType::kGestureLongPress:
        press_screen_pt_ = event->location();
        ConvertPointToScreen(this, &press_screen_pt_);
        started_drag_ = false;
        gesture_drag_initialized_ = true;
        gesture_dragging_ = false;
        original_selection_on_press_ = rail_->GetWorkspaceSelectionState();
        event->SetHandled();
        break;
      case ui::EventType::kGestureScrollBegin:
      case ui::EventType::kGestureScrollUpdate:
        // Chromium's central drag handler consumes scroll continuation only
        // after LongPress initialized a drag. Ordinary touch scrolling stays
        // unhandled and continues to the enclosing ScrollView.
        if (gesture_drag_initialized_) {
          gfx::Point screen = event->location();
          ConvertPointToScreen(this, &screen);
          if (!gesture_dragging_) {
            if ((screen - press_screen_pt_).LengthSquared() <=
                kVerticalTabDragThreshold * kVerticalTabDragThreshold) {
              event->SetHandled();
              break;
            }
            rail_->StartGroupDrag(group_, press_screen_pt_, screen,
                                  original_selection_on_press_);
            gesture_dragging_ = true;
          } else {
            rail_->UpdateRowDrag(screen);
          }
          event->SetHandled();
        }
        break;
      case ui::EventType::kGestureScrollEnd:
      case ui::EventType::kScrollFlingStart:
      case ui::EventType::kGestureEnd:
        if (gesture_drag_initialized_) {
          if (gesture_dragging_) {
            rail_->EndRowDrag(/*commit=*/true);
          }
          gesture_drag_initialized_ = false;
          gesture_dragging_ = false;
          event->SetHandled();
        }
        break;
      case ui::EventType::kGestureLongTap:
        if (gesture_dragging_) {
          rail_->EndRowDrag(/*commit=*/false);
        }
        gesture_drag_initialized_ = false;
        gesture_dragging_ = false;
        ShowEditorBubble();
        drag_armed_ = false;
        event->SetHandled();
        break;
      default:
        break;
    }
  }

  void OnMouseMoved(const ui::MouseEvent&) override {
    // Linux enter/leave delivery can be flaky, so repair missed enter events
    // from ordinary mouse movement exactly as VerticalTabGroupHeaderView does.
    UpdateEditorBubbleButtonVisibility();
  }

  void OnMouseEntered(const ui::MouseEvent&) override {
    UpdateEditorBubbleButtonVisibility();
  }

  void OnMouseExited(const ui::MouseEvent&) override {
#if BUILDFLAG(IS_LINUX)
    // Wayland/X11 can report stale IsMouseHovered() state during mouse exit.
    SetEditorBubbleButtonVisibilityOnHover(/*is_hovered=*/false);
#else
    UpdateEditorBubbleButtonVisibility();
#endif
  }

  void OnFocus() override {
    views::View::OnFocus();
    UpdateEditorBubbleButtonVisibility();
  }

  void OnBlur() override {
    views::View::OnBlur();
    UpdateEditorBubbleButtonVisibility();
  }

  void AddedToWidget() override {
    views::View::AddedToWidget();
    if (views::FocusManager* focus_manager = GetFocusManager()) {
      focus_manager->AddFocusChangeListener(this);
    }
  }

  void RemovedFromWidget() override {
    if (views::FocusManager* focus_manager = GetFocusManager()) {
      focus_manager->RemoveFocusChangeListener(this);
    }
    views::View::RemovedFromWidget();
  }

  // views::FocusChangeListener:
  void OnWillChangeFocus(views::View* focused_before,
                         views::View* focused_now) override {
    if (!focused_now || !Contains(focused_now)) {
      return;
    }
    UpdateEditorBubbleButtonVisibility();
    // During reverse traversal the initially-hidden button would otherwise be
    // skipped. Chromium posts this handoff so it occurs after focus settles on
    // the header.
    if (focused_now == this && focused_before &&
        focused_before->GetBoundsInScreen().y() > GetBoundsInScreen().y()) {
      base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
          FROM_HERE,
          base::BindOnce(
              [](base::WeakPtr<CmuxRailGroupHeader> view) {
                if (view && view->editor_bubble_button_ &&
                    view->GetFocusManager()) {
                  view->GetFocusManager()->SetFocusedViewWithReason(
                      view->editor_bubble_button_,
                      views::FocusManager::FocusChangeReason::kFocusTraversal);
                }
              },
              weak_factory_.GetWeakPtr()));
    }
  }

  void OnDidChangeFocus(views::View* focused_before,
                        views::View* focused_now) override {
    if (focused_before && Contains(focused_before) &&
        (!focused_now || !Contains(focused_now))) {
      UpdateEditorBubbleButtonVisibility();
    }
  }

  // views::ContextMenuController:
  void ShowContextMenuForViewImpl(
      views::View*,
      const gfx::Point&,
      ui::mojom::MenuSourceType) override {
    ShowEditorBubble();
  }

  bool OnKeyPressed(const ui::KeyEvent& event) override {
    if ((event.key_code() == ui::VKEY_SPACE ||
         event.key_code() == ui::VKEY_RETURN) &&
        representative_ != kInvalidId) {
      rail_->ToggleRow(representative_);
      return true;
    }
    const std::optional<GroupReorderDirection> reorder_direction =
        GetGroupReorderCommandForKeyboardEvent(event);
    if (!reorder_direction) {
      return false;
    }
    rail_->ShiftWorkspaceGroup(
        group_, *reorder_direction == GroupReorderDirection::kPrevious);
    return true;
  }

 private:
  friend class CmuxRail;

  void UpdateColors() {
    if (!label_ || !editor_bubble_button_ || !collapse_icon_) {
      return;
    }
    const SkColor foreground =
        color_utils::GetColorWithMaxContrast(GroupSkColor(this, color_));
    label_->SetEnabledColor(foreground);
    views::InkDrop::Get(editor_bubble_button_)
        ->SetBaseColor(color_utils::GetColorWithMaxContrast(foreground));
    for (views::Button::ButtonState state :
         {views::Button::STATE_NORMAL, views::Button::STATE_HOVERED,
          views::Button::STATE_PRESSED}) {
      editor_bubble_button_->SetImageModel(
          state, ui::ImageModel::FromVectorIcon(MoreMenuIcon(), foreground,
                                                kWorkspaceIconSize));
    }
    collapse_icon_->SetImage(ui::ImageModel::FromVectorIcon(
        GroupCollapseIcon(collapsed_), foreground, kWorkspaceIconSize));
  }

  void UpdateEditorBubbleButtonVisibility() {
    views::FocusManager* focus_manager = GetFocusManager();
    if (!focus_manager) {
      return;
    }
    SetEditorBubbleButtonVisibilityOnHover(
        IsMouseHovered() || Contains(focus_manager->GetFocusedView()));
  }

  void SetEditorBubbleButtonVisibilityOnHover(bool is_hovered) {
    const bool visible = sidebar_metrics::ShowsLabels(display_mode_) &&
                         (bubble_open_ || is_hovered);
    if (editor_bubble_button_->GetVisible() == visible) {
      return;
    }
    editor_bubble_button_->SetVisible(visible);
    InvalidateLayout();
  }

  void OnBubbleOpened() {
    bubble_open_ = true;
    UpdateEditorBubbleButtonVisibility();
  }

  void OnBubbleClosed() {
    bubble_open_ = false;
    UpdateEditorBubbleButtonVisibility();
  }

  void ShowEditorBubble() {
    if (bubble_open_ || group_ == kInvalidId) {
      return;
    }
    // Expanded Chromium vertical groups anchor the editor to this button,
    // including header right-click and long-tap openings.
    rail_->ShowGroupEditorBubble(group_, editor_bubble_button_);
  }

  void UpdateTooltipText() {
    const std::u16string title(label_->GetText());
    SetTooltipText(title.empty() ? u"Unnamed group - " + workspace_content_
                                 : title + u" - " + workspace_content_);
  }

  void UpdateAccessibleName() {
    const std::u16string title(label_->GetText());
    std::u16string name =
        title.empty() ? u"Unnamed group" : u"Group " + title;
    if (!workspace_content_.empty()) {
      name += u" - " + workspace_content_;
    }
#if !BUILDFLAG(IS_WIN)
    name += collapsed_ ? u" - Collapsed" : u" - Expanded";
#endif
    GetViewAccessibility().SetName(name);
  }

  raw_ptr<CmuxRail> rail_ = nullptr;
  raw_ptr<const RailConfig> config_ = nullptr;
  raw_ptr<views::Label> label_ = nullptr;
  raw_ptr<views::LabelButton> editor_bubble_button_ = nullptr;
  raw_ptr<views::ImageView> collapse_icon_ = nullptr;
  WorkspaceGroupId group_ = kInvalidId;
  WorkspaceId representative_ = kInvalidId;
  GroupColor color_ = GroupColor::kGrey;
  bool collapsed_ = false;
  bool bubble_open_ = false;
  bool gesture_drag_initialized_ = false;
  bool gesture_dragging_ = false;
  std::u16string workspace_content_;
  gfx::Point press_screen_pt_;
  bool started_drag_ = false;
  bool drag_armed_ = false;
  WorkspaceSelectionState original_selection_on_press_;
  sidebar_metrics::SidebarMode display_mode_ =
      sidebar_metrics::SidebarMode::kExpanded;
  base::WeakPtrFactory<CmuxRailGroupHeader> weak_factory_{this};
};

BEGIN_METADATA(CmuxRailGroupHeader)
END_METADATA

class CmuxRailGroupLine : public views::View {
  METADATA_HEADER(CmuxRailGroupLine, views::View)

 public:
  void SetColor(GroupColor color) {
    color_ = color;
    SchedulePaint();
  }

  void OnThemeChanged() override {
    views::View::OnThemeChanged();
    SchedulePaint();
  }

  void OnPaint(gfx::Canvas* canvas) override {
    cc::PaintFlags fill;
    fill.setAntiAlias(true);
    fill.setColor(GroupSkColor(this, color_));
    canvas->DrawRoundRect(gfx::RectF(GetLocalBounds()), kGroupLineCornerRadius,
                          fill);
  }

 private:
  GroupColor color_ = GroupColor::kGrey;
};

BEGIN_METADATA(CmuxRailGroupLine)
END_METADATA

// ---- Chromium collection hierarchy ----------------------------------------
// Chromium does not animate vertical groups as a flat set of tab bounds. The
// unpinned collection owns each ungrouped tab or whole group as one child, and
// every group owns a second animating collection containing its tabs. These
// two adapted views keep that structure intact; only "tab" is replaced by one
// cmux workspace row at the model boundary.
class CmuxRailCollectionView
    : public views::View,
      public views::LayoutDelegate,
      public CmuxRailAnimatingLayoutManager::Delegate {
  METADATA_HEADER(CmuxRailCollectionView, views::View)

 public:
  explicit CmuxRailCollectionView(CmuxRail* rail) : rail_(rail) {
    auto manager = std::make_unique<CmuxRailAnimatingLayoutManager>(
        std::make_unique<views::DelegatingLayoutManager>(this), *this,
        CmuxRailAnimatingLayoutManager::AnimationAxis::kVertical,
        /*animate_host_size=*/true);
    layout_manager_ = manager.get();
    SetLayoutManager(std::move(manager));
  }

  void SetLogicalChildren(const std::vector<views::View*>& children) {
    logical_children_.assign(children.begin(), children.end());
    InvalidateLayout();
  }

  const std::vector<raw_ptr<views::View>>& logical_children() const {
    return logical_children_;
  }

  void SetDragGap(int index, int row_count = 1) {
    SetDragGapHeight(
        index,
        row_count * kWorkspaceRowHeight +
            std::max(0, row_count - 1) * kTabVerticalPadding);
  }

  void SetDragGapHeight(int index, int height) {
    const int next = index < 0
                         ? -1
                         : std::clamp(
                               index, 0,
                               static_cast<int>(logical_children_.size()));
    const int next_height = next < 0 ? 0 : std::max(1, height);
    if (drag_gap_index_ == next && drag_gap_height_ == next_height) {
      return;
    }
    drag_gap_index_ = next;
    drag_gap_height_ = next_height;
    InvalidateLayout();
  }

  int drag_gap_index() const { return drag_gap_index_; }

  void AddCollectionChild(std::unique_ptr<views::View> child,
                          const gfx::Rect& previous_bounds_in_screen = {}) {
    if (previous_bounds_in_screen.IsEmpty()) {
      AddChildView(std::move(child));
    } else {
      layout_manager_->AnimateAndReparentView(
          std::move(child), previous_bounds_in_screen);
    }
  }

  void AnimateAndDestroy(views::View* child) {
    layout_manager_->AnimateAndDestroyChildView(child);
  }

  const views::ProposedLayout& target_layout() const {
    return layout_manager_->target_layout();
  }

  void Layout(PassKey) override {
    LayoutSuperclass<views::View>(this);
    if (rail_ && layout_manager_->is_animating()) {
      rail_->ScheduleContentGeometrySync();
    }
  }

  views::ProposedLayout CalculateProposedLayout(
      const views::SizeBounds& size_bounds) const override {
    views::ProposedLayout layouts;
    const bool bounded_width = size_bounds.width().is_bounded();
    const int host_width = bounded_width ? size_bounds.width().value() : 0;
    const int child_width =
        bounded_width
            ? std::max(0, host_width - 2 * kSidebarHorizontalPadding)
            : 0;
    int width = 0;
    int height = 0;
    int sequence_index = 0;
    const int sequence_size = static_cast<int>(logical_children_.size()) +
                              (drag_gap_index_ >= 0 ? 1 : 0);

    for (int i = 0; i <= static_cast<int>(logical_children_.size()); ++i) {
      if (drag_gap_index_ == i) {
        height += drag_gap_height_;
        if (++sequence_index < sequence_size) {
          height += kTabVerticalPadding;
        }
      }
      if (i == static_cast<int>(logical_children_.size())) {
        break;
      }

      views::View* child = logical_children_[i];
      gfx::Size preferred = bounded_width
                                ? child->GetPreferredSize(
                                      views::SizeBounds(child_width, {}))
                                : child->GetPreferredSize();
      gfx::Rect bounds(kSidebarHorizontalPadding, height,
                       bounded_width ? child_width : preferred.width(),
                       preferred.height());
      layouts.child_layouts.emplace_back(child, child->GetVisible(), bounds);
      height += bounds.height();
      width = std::max(width, bounds.right() + kSidebarHorizontalPadding);
      if (++sequence_index < sequence_size) {
        height += kTabVerticalPadding;
      }
    }

    // VerticalUnpinnedTabContainerView includes the unclamped dragged-view
    // bottom in its preferred height so edge scrolling keeps working past the
    // previous final row. The visible views remain viewport-clamped.
    const int dragged_view_bottom =
        rail_ && rail_->dragging_ ? rail_->drag_unclamped_bottom_ : 0;
    layouts.host_size = gfx::Size(bounded_width ? host_width : width,
                                  std::max(height, dragged_view_bottom));
    return layouts;
  }

  bool IsDragging() const override { return rail_ && rail_->dragging_; }

  bool IsViewDragging(const views::View& child_view) const override {
    return rail_ && rail_->InDragGroup(&child_view);
  }

  bool ShouldSnapToTarget(const views::View& child_view) const override {
    return rail_ && rail_->ShouldSnapDragViewToTarget(child_view);
  }

  bool ShouldAnimateOpacityForAddAndRemove(
      const views::View& child_view) const override {
    return views::IsViewClass<CmuxRailRow>(&child_view);
  }

  void OnAnimationEnded() override {
    if (!rail_) {
      return;
    }
    rail_->ClearSnapDragViewsForParent(*this);
    rail_->ReflowRows(/*animated=*/false);
    rail_->UpdateScrollbar(/*flash=*/false);
  }

 private:
  raw_ptr<CmuxRail> rail_ = nullptr;
  raw_ptr<CmuxRailAnimatingLayoutManager> layout_manager_ = nullptr;
  std::vector<raw_ptr<views::View>> logical_children_;
  int drag_gap_index_ = -1;
  int drag_gap_height_ = 0;
};

class CmuxRailGroupView
    : public views::View,
      public views::LayoutDelegate,
      public CmuxRailAnimatingLayoutManager::Delegate {
  METADATA_HEADER(CmuxRailGroupView, views::View)

 public:
  CmuxRailGroupView(CmuxRail* rail, const WorkspaceGroup& group,
                    WorkspaceId representative)
      : rail_(rail),
        group_id_(group.id),
        representative_(representative),
        collapsed_(group.collapsed),
        display_mode_(rail->display_mode()) {
    header_ = AddChildView(std::make_unique<CmuxRailGroupHeader>(rail));
    line_ = AddChildView(std::make_unique<CmuxRailGroupLine>());
    header_->set_config(&rail_->config_);
    header_->set_display_mode(display_mode_);
    header_->SetGroup(group, representative);
    line_->SetColor(group.color);

    auto manager = std::make_unique<CmuxRailAnimatingLayoutManager>(
        std::make_unique<views::DelegatingLayoutManager>(this), *this);
    layout_manager_ = manager.get();
    SetLayoutManager(std::move(manager));
  }

  WorkspaceGroupId group_id() const { return group_id_; }
  WorkspaceId representative() const { return representative_; }
  bool collapsed() const { return collapsed_; }
  CmuxRailGroupHeader* header() const { return header_; }

  void SetConfig(const RailConfig* config) { header_->set_config(config); }

  void SetDisplayMode(sidebar_metrics::SidebarMode mode) {
    if (display_mode_ == mode) {
      return;
    }
    display_mode_ = mode;
    header_->set_display_mode(mode);
    UpdateChildVisibility(collapsed_);
    InvalidateLayout();
  }

  void SetRows(const std::vector<CmuxRailRow*>& rows) {
    logical_rows_.assign(rows.begin(), rows.end());
    header_->SetWorkspaceContentString(GetWorkspaceContentString());
    if (!collapsed_) {
      UpdateChildVisibility(false);
    }
    InvalidateLayout();
  }

  const std::vector<raw_ptr<CmuxRailRow>>& logical_rows() const {
    return logical_rows_;
  }

  void SetDragGap(int index, int row_count = 1) {
    const int next =
        index < 0
            ? -1
            : std::clamp(index, 0, static_cast<int>(logical_rows_.size()));
    const int next_count = next < 0 ? 0 : std::max(1, row_count);
    if (drag_gap_index_ == next && drag_gap_row_count_ == next_count) {
      return;
    }
    drag_gap_index_ = next;
    drag_gap_row_count_ = next_count;
    InvalidateLayout();
  }

  int drag_gap_index() const { return drag_gap_index_; }

  void UpdateGroup(const WorkspaceGroup& group,
                   WorkspaceId representative,
                   bool initial) {
    CHECK_EQ(group.id, group_id_);
    representative_ = representative;
    header_->SetGroup(group, representative);
    line_->SetColor(group.color);

    const bool was_collapsed = collapsed_;
    collapsed_ = group.collapsed;
    if (initial) {
      UpdateChildVisibility(collapsed_);
    } else if (was_collapsed && !collapsed_) {
      // Chromium makes children visible before expanding the group host so the
      // growing clip reveals already-laid-out rows.
      UpdateChildVisibility(false);
    }
    // On collapse children stay visible until the exact collection animation
    // finishes; OnAnimationEnded() hides them.
    InvalidateLayout();
  }

  void AddRow(std::unique_ptr<views::View> row,
              const gfx::Rect& previous_bounds_in_screen = {}) {
    if (previous_bounds_in_screen.IsEmpty()) {
      AddChildView(std::move(row));
    } else {
      layout_manager_->AnimateAndReparentView(std::move(row),
                                              previous_bounds_in_screen);
    }
  }

  void AnimateAndDestroyRow(CmuxRailRow* row) {
    layout_manager_->AnimateAndDestroyChildView(row);
  }

  const views::ProposedLayout& target_layout() const {
    return layout_manager_->target_layout();
  }

  views::ProposedLayout CalculateProposedLayout(
      const views::SizeBounds& size_bounds) const override {
    views::ProposedLayout layouts;
    const bool bounded_width = size_bounds.width().is_bounded();
    const int host_width = bounded_width ? size_bounds.width().value() : 0;
    const bool show_labels = sidebar_metrics::ShowsLabels(display_mode_);
    const int header_height =
        show_labels ? kGroupHeaderHeight : kWorkspaceRowHeight;
    int width = 0;
    int height = kGroupHeaderVerticalMargin;

    gfx::Rect header_bounds(0, height,
                            bounded_width ? host_width : header_->width(),
                            header_height);
    layouts.child_layouts.emplace_back(
        static_cast<views::View*>(header_.get()), header_->GetVisible(),
        header_bounds);
    height += header_bounds.height() + kGroupHeaderVerticalMargin +
              kTabVerticalPadding;
    width = std::max(width, header_bounds.width());

    gfx::Rect line_bounds(0, height, kGroupLineWidth, 0);
    const int row_x = show_labels ? kSidebarHorizontalPadding : 0;
    const int row_width =
        bounded_width
            ? std::max(0, host_width -
                              (show_labels ? kSidebarHorizontalPadding : 0))
            : 0;
    int sequence_index = 0;
    const int sequence_size = static_cast<int>(logical_rows_.size()) +
                              (drag_gap_index_ >= 0 ? 1 : 0);
    for (int i = 0; i <= static_cast<int>(logical_rows_.size()); ++i) {
      if (drag_gap_index_ == i) {
        height += drag_gap_row_count_ * kWorkspaceRowHeight +
                  std::max(0, drag_gap_row_count_ - 1) * kTabVerticalPadding;
        if (++sequence_index < sequence_size) {
          height += kTabVerticalPadding;
        }
      }
      if (i == static_cast<int>(logical_rows_.size())) {
        break;
      }

      CmuxRailRow* row = logical_rows_[i];
      gfx::Size preferred = bounded_width
                                ? row->GetPreferredSize(
                                      views::SizeBounds(row_width, {}))
                                : row->GetPreferredSize();
      gfx::Rect bounds(row_x, height,
                       bounded_width ? row_width : preferred.width(),
                       preferred.height());
      layouts.child_layouts.emplace_back(row, row->GetVisible(), bounds);
      height += bounds.height();
      width = std::max(width, bounds.right());
      if (++sequence_index < sequence_size) {
        height += kTabVerticalPadding;
      }
    }

    if (sequence_size > 0) {
      line_bounds.set_height(std::max(0, height - line_bounds.y()));
    } else {
      // VerticalTabGroupView unconditionally removes the final row padding.
      // Preserve its 34-DIP expanded empty-group height (32 when collapsed).
      height -= kTabVerticalPadding;
    }
    layouts.child_layouts.emplace_back(
        static_cast<views::View*>(line_.get()), line_->GetVisible(),
        line_bounds);

    if (!collapsed_) {
      height += kTabVerticalPadding;
    }
    layouts.host_size = gfx::Size(
        bounded_width ? host_width : width,
        collapsed_ ? header_height + 2 * kGroupHeaderVerticalMargin
                   : height);
    return layouts;
  }

  bool IsDragging() const override { return rail_ && rail_->dragging_; }

  bool IsViewDragging(const views::View& child_view) const override {
    return rail_ && rail_->InDragGroup(&child_view);
  }

  bool ShouldSnapToTarget(const views::View& child_view) const override {
    return rail_ && rail_->ShouldSnapDragViewToTarget(child_view);
  }

  bool ShouldAnimateOpacityForAddAndRemove(
      const views::View& child_view) const override {
    return views::IsViewClass<CmuxRailRow>(&child_view);
  }

  void OnGestureEvent(ui::GestureEvent* event) override {
    if (event->type() == ui::EventType::kGestureLongTap) {
      // Chromium forwards long taps received anywhere on the group collection
      // to the header so the same editor opens for touch context gestures.
      ui::GestureEvent converted_event(*event, static_cast<views::View*>(this),
                                       static_cast<views::View*>(header_));
      header_->OnGestureEvent(&converted_event);
      event->SetHandled();
    }
  }

  void OnAnimationEnded() override {
    if (collapsed_) {
      UpdateChildVisibility(true);
    }
    if (rail_) {
      rail_->ClearSnapDragViewsForParent(*this);
      rail_->ReflowRows(/*animated=*/false);
      rail_->UpdateScrollbar(/*flash=*/false);
    }
  }

 private:
  std::u16string GetWorkspaceContentString() const {
    if (logical_rows_.empty()) {
      return std::u16string();
    }

    constexpr size_t kMaxTitleLength = 30;
    const std::string& first_title = logical_rows_.front()->title();
    const std::u16string full_title =
        base::UTF8ToUTF16(first_title.empty() ? "Workspace" : first_title);
    std::u16string short_title;
    gfx::ElideString(full_title, kMaxTitleLength, &short_title);

    const size_t other_count = logical_rows_.size() - 1;
    if (other_count == 0) {
      return short_title;
    }
    return short_title + u" and " +
           base::UTF8ToUTF16(std::to_string(other_count)) + u" other " +
           (other_count == 1 ? u"workspace" : u"workspaces");
  }

  void UpdateChildVisibility(bool collapsed) {
    line_->SetVisible(!collapsed &&
                      sidebar_metrics::ShowsLabels(display_mode_));
    for (CmuxRailRow* row : logical_rows_) {
      row->SetVisible(!collapsed);
    }
  }

  raw_ptr<CmuxRail> rail_ = nullptr;
  WorkspaceGroupId group_id_ = kInvalidId;
  WorkspaceId representative_ = kInvalidId;
  bool collapsed_ = false;
  raw_ptr<CmuxRailGroupHeader> header_ = nullptr;
  raw_ptr<CmuxRailGroupLine> line_ = nullptr;
  raw_ptr<CmuxRailAnimatingLayoutManager> layout_manager_ = nullptr;
  std::vector<raw_ptr<CmuxRailRow>> logical_rows_;
  int drag_gap_index_ = -1;
  int drag_gap_row_count_ = 0;
  sidebar_metrics::SidebarMode display_mode_ =
      sidebar_metrics::SidebarMode::kExpanded;
};

BEGIN_METADATA(CmuxRailCollectionView)
END_METADATA

BEGIN_METADATA(CmuxRailGroupView)
END_METADATA

// Workspace adaptation of Helium/Chromium's vertical TabMenuModel ordering.
// Source provenance is recorded in THIRD_PARTY_NOTICES.md. Labels say
// "Workspace" because one cmux rail row owns a workspace rather than one tab.
class CmuxRailWorkspaceContextMenu : public ui::SimpleMenuModel::Delegate {
 public:
  CmuxRailWorkspaceContextMenu(CmuxRail* rail, WorkspaceId workspace)
      : rail_(rail),
        workspace_(workspace),
        model_(std::make_unique<ui::SimpleMenuModel>(this)),
        split_layout_model_(std::make_unique<ui::SimpleMenuModel>(this)),
        group_model_(std::make_unique<ui::SimpleMenuModel>(this)) {
    const CmuxRailRow* source = nullptr;
    for (const CmuxRailRow* row : rail_->rows_) {
      if (row->workspace_id() == workspace_) {
        source = row;
        break;
      }
    }
    const WorkspaceGroupId source_group =
        source ? source->group_id() : kInvalidId;
    const bool is_active = source && source->active();
    const int target_count =
        rail_->delegate_
            ? std::max(1, rail_->delegate_->WorkspaceContextTargetCount(
                              workspace_))
            : 1;
    const bool multiple = target_count > 1;
    const bool simplified =
        rail_->delegate_ &&
        rail_->delegate_->IsWorkspaceContextMenuSimplificationEnabled();

    model_->AddItem(kRenameCommand, u"Rename…");
    model_->AddSeparator(ui::NORMAL_SEPARATOR);
    Add(WorkspaceContextAction::kNewWorkspaceBelow, u"New Workspace Below");
    const std::u16string split_label =
        is_active ? u"Add Workspace to New Split View"
                  : u"New Split View with Current Workspace";
    split_layout_model_->AddItemWithIcon(
        Command(WorkspaceContextAction::kNewSplitSideBySide), u"Side by Side",
        ui::ImageModel::FromVectorIcon(
            SplitSceneIcon(),
            ui::kColorMenuIcon, ui::SimpleMenuModel::kDefaultIconSize));
    split_layout_model_->AddItemWithIcon(
        Command(WorkspaceContextAction::kNewSplitStacked), u"Stacked",
        ui::ImageModel::FromVectorIcon(
            StackedSplitSceneIcon(), ui::kColorMenuIcon,
            ui::SimpleMenuModel::kDefaultIconSize));
    model_->AddSubMenuWithIcon(
        kSplitLayoutSubmenuCommand, split_label, split_layout_model_.get(),
        ui::ImageModel::FromVectorIcon(
            SplitSceneIcon(),
            ui::kColorMenuIcon, 16));

    std::vector<const CmuxRail::GroupMenuEntry*> eligible_groups;
    for (const auto& entry : rail_->group_menu_entries_) {
      if (entry.id != source_group) {
        eligible_groups.push_back(&entry);
      }
    }
    if (!eligible_groups.empty()) {
      group_model_->AddItem(Command(WorkspaceContextAction::kAddToNewGroup),
                            u"New Group");
      group_model_->AddSeparator(ui::NORMAL_SEPARATOR);
      int command = kExistingGroupCommandBase;
      for (const CmuxRail::GroupMenuEntry* entry : eligible_groups) {
        group_commands_[command] = entry->representative;
        const SkColor icon_color =
            ResolveProviderColor(
                rail_, ChromiumGroupContextMenuColorId(entry->color))
                .value_or(GroupSkColor(rail_, entry->color));
        group_model_->AddItemWithIcon(
            command++, base::UTF8ToUTF16(entry->title.empty()
                                             ? "Workspace Group"
                                             : entry->title),
            ui::ImageModel::FromVectorIcon(TabGroupMenuIcon(), icon_color, 14));
        group_model_->SetMayHaveMnemonicsAt(
            group_model_->GetItemCount() - 1, false);
      }
      model_->AddSubMenu(kGroupSubmenuCommand,
                         multiple ? u"Add Workspaces to Group"
                                  : u"Add Workspace to Group",
                         group_model_.get());
      if (simplified) {
        SetCommandIcon(
            model_.get(),
            kGroupSubmenuCommand,
            ui::ImageModel::FromVectorIcon(
                GroupSubmenuIcon(), ui::kColorMenuIcon,
                ui::SimpleMenuModel::kDefaultIconSize));
      }
    } else {
      Add(WorkspaceContextAction::kAddToNewGroup,
          multiple ? u"Add Workspaces to New Group"
                   : u"Add Workspace to New Group");
      if (simplified) {
        SetCommandIcon(
            model_.get(),
            Command(WorkspaceContextAction::kAddToNewGroup),
            ui::ImageModel::FromVectorIcon(
                GroupSubmenuIcon(), ui::kColorMenuIcon,
                ui::SimpleMenuModel::kDefaultIconSize));
      }
    }
    if (rail_->delegate_ &&
        rail_->delegate_->IsWorkspaceContextActionEnabled(
            workspace_, WorkspaceContextAction::kRemoveFromGroup)) {
      Add(WorkspaceContextAction::kRemoveFromGroup, u"Remove from Group");
    }
    Add(WorkspaceContextAction::kMoveToNewWindow,
        multiple ? u"Move Workspaces to New Window"
                 : u"Move Workspace to New Window");
    if (simplified) {
      SetCommandIcon(
          model_.get(),
          Command(WorkspaceContextAction::kMoveToNewWindow),
          ui::ImageModel::FromVectorIcon(
              MoveToNewWindowIcon(), ui::kColorMenuIcon,
              ui::SimpleMenuModel::kDefaultIconSize));
    }

    model_->AddSeparator(ui::NORMAL_SEPARATOR);
    const int url_count = rail_->delegate_
                              ? rail_->delegate_->WorkspaceContextUrlCount(
                                    workspace_)
                              : 1;
    Add(WorkspaceContextAction::kReload, u"Reload");
    Add(WorkspaceContextAction::kDuplicate, u"Duplicate");
    Add(WorkspaceContextAction::kTogglePinned,
        rail_->delegate_ && rail_->delegate_->IsWorkspaceContextActionToggled(
                                workspace_,
                                WorkspaceContextAction::kTogglePinned)
            ? u"Unpin"
            : u"Pin");
    if (simplified) {
      const bool will_pin =
          !(rail_->delegate_ &&
            rail_->delegate_->IsWorkspaceContextActionToggled(
                workspace_, WorkspaceContextAction::kTogglePinned));
      SetCommandIcon(
          model_.get(),
          Command(WorkspaceContextAction::kTogglePinned),
          ui::ImageModel::FromVectorIcon(
              PinIcon(will_pin), ui::kColorMenuIcon,
              ui::SimpleMenuModel::kDefaultIconSize));
    }
    Add(WorkspaceContextAction::kToggleSiteMuted,
        rail_->delegate_ && rail_->delegate_->IsWorkspaceContextActionToggled(
                                workspace_,
                                WorkspaceContextAction::kToggleSiteMuted)
            ? (url_count <= 1 ? u"Unmute Site" : u"Unmute Sites")
            : (url_count <= 1 ? u"Mute Site" : u"Mute Sites"));
    if (simplified) {
      const bool will_mute =
          !(rail_->delegate_ &&
            rail_->delegate_->IsWorkspaceContextActionToggled(
                workspace_, WorkspaceContextAction::kToggleSiteMuted));
      SetCommandIcon(
          model_.get(),
          Command(WorkspaceContextAction::kToggleSiteMuted),
          ui::ImageModel::FromVectorIcon(
              MuteIcon(will_mute), ui::kColorMenuIcon,
              ui::SimpleMenuModel::kDefaultIconSize));
    }
    if (rail_->delegate_) {
      rail_->delegate_->AppendOptionalWorkspaceContextMenuItems(
          workspace_, model_.get(), this);
    }
    model_->AddSeparator(ui::NORMAL_SEPARATOR);
    Add(WorkspaceContextAction::kClose, u"Close");
    Add(WorkspaceContextAction::kCloseOthers, u"Close Other Workspaces");
    Add(WorkspaceContextAction::kCloseBelow, u"Close Workspaces Below");

    runner_ = std::make_unique<views::MenuRunner>(
        model_.get(),
        views::MenuRunner::HAS_MNEMONICS | views::MenuRunner::CONTEXT_MENU);
  }

  void Run(const gfx::Point& screen_pt,
           views::Widget* widget,
           ui::mojom::MenuSourceType source_type) {
    if (runner_ && widget) {
      runner_->RunMenuAt(widget, nullptr, gfx::Rect(screen_pt, gfx::Size()),
                         views::MenuAnchorPosition::kTopLeft,
                         source_type);
    }
  }

  bool IsCommandIdEnabled(int command_id) const override {
    if (!rail_ || !rail_->delegate_) {
      return false;
    }
    if (command_id == kRenameCommand) {
      return true;
    }
    if (command_id == kGroupSubmenuCommand) {
      return true;
    }
    if (command_id == kSplitLayoutSubmenuCommand) {
      return rail_->delegate_->IsWorkspaceContextActionEnabled(
                 workspace_, WorkspaceContextAction::kNewSplitSideBySide) ||
             rail_->delegate_->IsWorkspaceContextActionEnabled(
                 workspace_, WorkspaceContextAction::kNewSplitStacked);
    }
    if (auto it = group_commands_.find(command_id);
        it != group_commands_.end()) {
      return true;
    }
    if (IsWorkspaceActionCommand(command_id)) {
      return rail_->delegate_->IsWorkspaceContextActionEnabled(
          workspace_, static_cast<WorkspaceContextAction>(command_id));
    }
    return rail_->delegate_->IsOptionalWorkspaceContextMenuCommandEnabled(
        workspace_, command_id);
  }

  bool IsCommandIdChecked(int command_id) const override {
    if (!rail_ || !rail_->delegate_) {
      return false;
    }
    return rail_->delegate_->IsOptionalWorkspaceContextMenuCommandChecked(
        workspace_, command_id);
  }

  bool IsCommandIdVisible(int command_id) const override {
    if (!rail_ || !rail_->delegate_) {
      return false;
    }
    if (command_id == kRenameCommand ||
        command_id == kGroupSubmenuCommand ||
        command_id == kSplitLayoutSubmenuCommand ||
        group_commands_.contains(command_id) ||
        IsWorkspaceActionCommand(command_id)) {
      return true;
    }
    return rail_->delegate_->IsOptionalWorkspaceContextMenuCommandVisible(
        workspace_, command_id);
  }

  void ExecuteCommand(int command_id, int event_flags) override {
    if (!rail_ || !rail_->delegate_) {
      return;
    }
    if (command_id == kRenameCommand) {
      base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
          FROM_HERE,
          base::BindOnce(&CmuxRail::BeginRenameRow,
                         rail_->weak_factory_.GetWeakPtr(), workspace_));
      return;
    }
    if (auto it = group_commands_.find(command_id);
        it != group_commands_.end()) {
      rail_->delegate_->OnMoveWorkspaceSelection(workspace_, it->second, -1);
      return;
    }
    if (IsWorkspaceActionCommand(command_id)) {
      rail_->delegate_->OnWorkspaceContextAction(
          workspace_, static_cast<WorkspaceContextAction>(command_id));
      return;
    }
    rail_->delegate_->ExecuteOptionalWorkspaceContextMenuCommand(
        workspace_, command_id, event_flags);
  }

  bool GetAcceleratorForCommandId(
      int command_id,
      ui::Accelerator* accelerator) const override {
    if (!accelerator) {
      return false;
    }
    if (command_id == Command(WorkspaceContextAction::kReload)) {
      *accelerator =
          ui::Accelerator(ui::VKEY_R, ui::EF_PLATFORM_ACCELERATOR);
      return true;
    }
    if (command_id == Command(WorkspaceContextAction::kClose)) {
      *accelerator =
          ui::Accelerator(ui::VKEY_W, ui::EF_PLATFORM_ACCELERATOR);
      return true;
    }
    return false;
  }

 private:
  static constexpr int kGroupSubmenuCommand = 900;
  static constexpr int kSplitLayoutSubmenuCommand = 901;
  static constexpr int kRenameCommand = 902;
  // Keep dynamic group commands clear of the low command-id ranges used by
  // optional Chromium menu features (for example, Glic conversation items).
  static constexpr int kExistingGroupCommandBase = 100000;
  static int Command(WorkspaceContextAction action) {
    return static_cast<int>(action);
  }
  static bool IsWorkspaceActionCommand(int command_id) {
    return command_id >= Command(WorkspaceContextAction::kNewWorkspaceBelow) &&
           command_id <= Command(WorkspaceContextAction::kCloseBelow);
  }
  void Add(WorkspaceContextAction action, std::u16string label) {
    model_->AddItem(Command(action), std::move(label));
  }

  raw_ptr<CmuxRail> rail_ = nullptr;
  WorkspaceId workspace_ = kInvalidId;
  std::unique_ptr<ui::SimpleMenuModel> model_;
  std::unique_ptr<ui::SimpleMenuModel> split_layout_model_;
  std::unique_ptr<ui::SimpleMenuModel> group_model_;
  std::map<int, WorkspaceId> group_commands_;
  std::unique_ptr<views::MenuRunner> runner_;
};

// ---- Bottom new-workspace action -------------------------------------------
// Workspace adaptation of Helium's VerticalNewTabButton and its patched
// Chromium TabStripFlatEdgeButton/NewTabButton base classes. Keep the shared
// LabelButton layout, shaped target, ink drop, frame-active paint updates, and
// width-responsive label exact; only dispatch changes from tabs to workspaces.
class CmuxRailNewWorkspaceButton : public views::LabelButton,
                                   public views::MaskedTargeterDelegate,
                                   public views::ContextMenuController,
                                   public ui::SimpleMenuModel::Delegate {
  METADATA_HEADER(CmuxRailNewWorkspaceButton, views::LabelButton)

 public:
  explicit CmuxRailNewWorkspaceButton(RailDelegate* delegate,
                                      bool is_update = false)
      : views::LabelButton(
            base::BindRepeating(
                [](RailDelegate* d, bool update) {
                  if (d) {
                    if (update) {
                      d->OnApplyUpdate();
                    } else {
                      d->OnNewWorkspace();
                    }
                  }
                },
                delegate,
                is_update),
            std::u16string()),
        delegate_(delegate),
        is_update_(is_update) {
    SetHorizontalAlignment(gfx::ALIGN_LEFT);
    SetPreferredSize(gfx::Size(-1, kFooterHeight));
    if (!is_update_) {
      set_context_menu_controller(this);
      ConfigureFooterInkDrop(
          this,
          std::make_unique<views::RoundRectHighlightPathGenerator>(
              gfx::Insets(), gfx::RoundedCornersF(kFooterCornerRadius)),
          base::BindRepeating(
              &CmuxRailNewWorkspaceButton::ResolveFooterColor,
              base::Unretained(this)),
          kColorTabBackgroundInactiveHoverFrameActive,
          kColorTabBackgroundActiveFrameActive);
      SetPaintToLayer();
      layer()->SetFillsBoundsOpaquely(false);
      SetImageLabelSpacing(kFooterImageLabelGap);
      SetInsets(gfx::Insets::VH(kFooterVerticalPadding,
                                kFooterHorizontalPadding));
      SetShouldShowLabel(true);
      SetLabelText(u"New workspace");
      UpdateNewWorkspaceIcon(ui::ImageModel::FromVectorIcon(
          AddWorkspaceIcon(), ui::kColorIcon));
    } else {
      SetFocusBehavior(FocusBehavior::ALWAYS);
      SetInsets(gfx::Insets::TLBR(
          kFooterVerticalPadding,
          kFooterHorizontalPadding + kFooterIconSize + kFooterImageLabelGap,
          kFooterVerticalPadding, kFooterHorizontalPadding));
      SetText(u"Update now");
    }
    SetTooltipText(is_update_ ? u"Update now" : u"New workspace");
    GetViewAccessibility().SetName(is_update_ ? u"Update now"
                                              : u"New workspace");
  }

  void SetUpdateVersion(const std::string& version) {
    if (!is_update_) {
      return;
    }
    SetTooltipText(base::UTF8ToUTF16("Update to " + version));
  }

  void set_config(const RailConfig* cfg) {
    cfg_ = cfg;
    if (cfg_ && is_update_) {
      label()->SetFontList(MakeFont(*cfg_, false, false));
    }
    if (!is_update_) {
      const std::optional<ui::ImageModel>& model =
          GetImageModel(views::Button::STATE_NORMAL);
      if (model && model->IsVectorIcon()) {
        UpdateNewWorkspaceIcon(*model);
      }
    }
    UpdateLabelColor();
    SchedulePaint();
  }

  void set_display_mode(sidebar_metrics::SidebarMode mode) {
    display_mode_ = mode;
    if (mode == sidebar_metrics::SidebarMode::kHidden) {
      // A button hidden while hovered does not receive the mouse-exit event
      // that would normally clear Button::state_. Do not carry that visual
      // state into the next visible mode.
      SetState(views::Button::STATE_NORMAL);
    }
    const bool show_labels = sidebar_metrics::ShowsLabels(mode);
    if (is_update_) {
      label()->SetVisible(show_labels);
    } else {
      UpdateLabel(show_labels &&
                  width() > CalculatePreferredSize({}).width());
    }
    InvalidateLayout();
    SchedulePaint();
  }

  gfx::Size CalculatePreferredSize(
      const views::SizeBounds& available_size) const override {
    return gfx::Size(kFooterHeight, kFooterHeight);
  }

  void OnBoundsChanged(const gfx::Rect& previous_bounds) override {
    if (!is_update_) {
      UpdateLabel(sidebar_metrics::ShowsLabels(display_mode_) &&
                  width() > CalculatePreferredSize({}).width());
    }
  }

  void OnPaintBackground(gfx::Canvas* canvas) override {
    if (!is_update_) {
      // The new-workspace action is visually part of the rail at rest. Its
      // configured ink drop supplies hover/press feedback without an idle
      // tile behind the plus icon.
      return;
    }
    if (!cfg_) {
      return;
    }
    SkColor bg = cfg_->plus_bg;
    if (GetState() == views::Button::STATE_HOVERED ||
        GetState() == views::Button::STATE_PRESSED) {
      bg = cfg_->plus_hover_bg;
    }
    if (SkColorGetA(bg) == 0) {
      return;
    }
    gfx::RectF r(GetLocalBounds());
    cc::PaintFlags f;
    f.setAntiAlias(true);
    f.setColor(bg);
    canvas->DrawRoundRect(r, cfg_->row_corner, f);
  }

  void PaintButtonContents(gfx::Canvas* canvas) override {
    views::LabelButton::PaintButtonContents(canvas);
    if (!cfg_) {
      return;
    }
    if (!is_update_) {
      return;
    }
    cc::PaintFlags plus;
    plus.setAntiAlias(true);
    SkColor foreground = cfg_->plus_text;
    if (GetState() == views::Button::STATE_HOVERED ||
        GetState() == views::Button::STATE_PRESSED) {
      foreground = cfg_->sel_text;
    }
    plus.setColor(foreground);
    plus.setStrokeWidth(1.5f);
    const int cx = kFooterHorizontalPadding + kFooterIconSize / 2;
    const int cy = height() / 2;
    canvas->DrawLine(gfx::PointF(cx, cy - 6), gfx::PointF(cx, cy + 3), plus);
    canvas->DrawLine(gfx::PointF(cx - 4, cy), gfx::PointF(cx, cy + 4), plus);
    canvas->DrawLine(gfx::PointF(cx, cy + 4), gfx::PointF(cx + 4, cy), plus);
  }

  void OnMouseEvent(ui::MouseEvent* event) override {
    // shared::NewTabButton consumes middle click on every platform (and uses
    // the selection clipboard on Linux). cmux has no platform selection
    // clipboard on macOS/Windows, but must still avoid treating it as a normal
    // new-workspace click.
    if (!is_update_ && event->IsOnlyMiddleMouseButton()) {
      event->SetHandled();
      return;
    }
    views::LabelButton::OnMouseEvent(event);
  }

  // views::ContextMenuController:
  void ShowContextMenuForViewImpl(
      views::View* source,
      const gfx::Point& point,
      ui::mojom::MenuSourceType source_type) override {
    if (is_update_ || !delegate_ || !source || !source->GetWidget()) {
      return;
    }

    recent_group_ = delegate_->GetMostRecentWorkspaceGroupForContextMenu();
    context_menu_model_ = std::make_unique<ui::SimpleMenuModel>(this);
    context_menu_model_->AddItem(
        Command(NewWorkspaceContextAction::kNewWorkspace), u"New Workspace");

    std::u16string recent_group_label = u"New Workspace in Group";
    if (recent_group_) {
      if (!recent_group_->title.empty()) {
        recent_group_label =
            u"New Workspace in " +
            base::UTF8ToUTF16(recent_group_->title);
      } else {
        recent_group_label =
            u"New Workspace in " +
            base::NumberToString16(
                std::max(1, recent_group_->member_count)) +
            (recent_group_->member_count == 1 ? u" Workspace"
                                               : u" Workspaces");
      }
    }
    context_menu_model_->AddItem(
        Command(NewWorkspaceContextAction::kNewWorkspaceInRecentGroup),
        recent_group_label);

    context_menu_model_->AddSeparator(ui::NORMAL_SEPARATOR);
    context_menu_model_->AddItem(
        Command(NewWorkspaceContextAction::kNewWorkspaceGroup),
        u"New Workspace Group");

    context_menu_model_->AddSeparator(ui::NORMAL_SEPARATOR);
    context_menu_model_->AddItem(
        Command(NewWorkspaceContextAction::kNewSplitView),
        u"New Split View with Current Workspace");

    context_menu_runner_ = std::make_unique<views::MenuRunner>(
        context_menu_model_.get(),
        views::MenuRunner::HAS_MNEMONICS | views::MenuRunner::CONTEXT_MENU);
    context_menu_runner_->RunMenuAt(
        source->GetWidget(), nullptr, gfx::Rect(point, gfx::Size()),
        views::MenuAnchorPosition::kTopLeft, source_type);
  }

  // ui::SimpleMenuModel::Delegate:
  bool IsCommandIdEnabled(int command_id) const override {
    if (!delegate_) {
      return false;
    }
    const NewWorkspaceContextAction action =
        static_cast<NewWorkspaceContextAction>(command_id);
    if (action == NewWorkspaceContextAction::kNewWorkspaceInRecentGroup &&
        !recent_group_) {
      return false;
    }
    return delegate_->IsNewWorkspaceContextActionEnabled(action);
  }

  void ExecuteCommand(int command_id, int) override {
    if (!delegate_) {
      return;
    }
    delegate_->OnNewWorkspaceContextAction(
        static_cast<NewWorkspaceContextAction>(command_id),
        recent_group_ ? recent_group_->id : kInvalidId);
  }

  bool GetAcceleratorForCommandId(
      int command_id,
      ui::Accelerator* accelerator) const override {
    return delegate_ && accelerator &&
           delegate_->GetNewWorkspaceContextAccelerator(
               static_cast<NewWorkspaceContextAction>(command_id),
               accelerator);
  }

  void OnThemeChanged() override {
    views::LabelButton::OnThemeChanged();
    if (!is_update_) {
      const std::optional<ui::ImageModel>& model =
          GetImageModel(views::Button::STATE_NORMAL);
      if (model && model->IsVectorIcon()) {
        UpdateNewWorkspaceIcon(*model);
      }
    }
    UpdateLabelColor();
    SchedulePaint();
  }

  bool GetHitTestMask(SkPath* mask) const override {
    *mask = SkPath::RRect(GetButtonShape());
    return true;
  }

 private:
  static int Command(NewWorkspaceContextAction action) {
    return static_cast<int>(action);
  }

  void AddedToWidget() override {
    if (is_update_) {
      views::LabelButton::AddedToWidget();
      return;
    }
    paint_as_active_subscription_ =
        GetWidget()->RegisterPaintAsActiveChangedCallback(base::BindRepeating(
            &CmuxRailNewWorkspaceButton::OnThemeChanged,
            base::Unretained(this)));
  }

  void RemovedFromWidget() override {
    if (is_update_) {
      views::LabelButton::RemovedFromWidget();
      return;
    }
    paint_as_active_subscription_ = {};
  }

  void SetInsets(const gfx::Insets& insets) {
    std::unique_ptr<views::LabelButtonBorder> border = CreateDefaultBorder();
    border->set_insets(insets);
    SetBorder(std::move(border));
  }

  void SetShouldShowLabel(bool show_label) {
    should_show_label_ = show_label;
    if (should_show_label_) {
      label()->SetPaintToLayer();
      label()->SetSkipSubpixelRenderingOpacityCheck(true);
      label()->layer()->SetFillsBoundsOpaquely(false);
      label()->SetSubpixelRenderingEnabled(false);
    }
  }

  void SetLabelText(const std::u16string& text) {
    if (label_text_ == text) {
      return;
    }
    label_text_ = text;
    UpdateLabel(width() > CalculatePreferredSize({}).width());
  }

  void UpdateLabel(bool should_show) {
    if (!should_show_label_) {
      return;
    }
    if (should_show == GetText().empty()) {
      SetHorizontalAlignment(should_show ? gfx::ALIGN_LEFT : gfx::ALIGN_CENTER);
      SetInsets(gfx::Insets::VH(kFooterVerticalPadding,
                                kFooterHorizontalPadding));
    }
    const std::u16string label =
        should_show ? label_text_ : std::u16string();
    if (label != GetText()) {
      SetText(label);
    }
    if (should_show) {
      UpdateLabelColor();
    }
  }

  void UpdateNewWorkspaceIcon(const ui::ImageModel& icon_image) {
    CHECK(icon_image.IsVectorIcon());
    const ui::ImageModel image_model = ui::ImageModel::FromVectorIcon(
        *icon_image.GetVectorIcon().vector_icon(),
        ResolveFooterColor(kColorToolbarButtonIconDisabled), kFooterIconSize);
    SetImageModel(views::Button::STATE_NORMAL, image_model);
    SetImageModel(views::Button::STATE_HOVERED, image_model);
    SetImageModel(views::Button::STATE_PRESSED, image_model);
    SetImageModel(views::Button::STATE_DISABLED, image_model);
  }

  void UpdateLabelColor() {
    if (!is_update_) {
      const SkColor foreground =
          ResolveFooterColor(kColorToolbarButtonIconDisabled);
      SetTextColor(views::Button::STATE_NORMAL, foreground);
      SetTextColor(views::Button::STATE_HOVERED, foreground);
      SetTextColor(views::Button::STATE_PRESSED, foreground);
      SetTextColor(views::Button::STATE_DISABLED, foreground);
      return;
    }
    if (!cfg_) {
      return;
    }
    SetTextColor(views::Button::STATE_NORMAL, cfg_->plus_text);
    SetTextColor(views::Button::STATE_HOVERED, cfg_->sel_text);
    SetTextColor(views::Button::STATE_PRESSED, cfg_->sel_text);
    SetTextColor(views::Button::STATE_DISABLED, cfg_->plus_text);
  }

  SkColor ResolveFooterColor(ui::ColorId color_id) const {
    if (const std::optional<SkColor> color =
            ResolveProviderColor(this, color_id)) {
      return *color;
    }
    if (cfg_) {
      if (color_id == kColorTabBackgroundInactiveHoverFrameActive) {
        return cfg_->plus_hover_bg;
      }
      if (color_id == kColorTabBackgroundActiveFrameActive) {
        return cfg_->sel_bg;
      }
      return cfg_->plus_text;
    }
    return ResolveProviderColor(this, ui::kColorIcon)
        .value_or(SK_ColorGRAY);
  }

  SkRRect GetButtonShape() const {
    const gfx::RoundedCornersF corners(kFooterCornerRadius);
    const SkRect rect = gfx::RectToSkRect(GetLocalBounds());
    SkVector radii[4];
    radii[0] = {corners.upper_left(), corners.upper_left()};
    radii[1] = {corners.upper_right(), corners.upper_right()};
    radii[2] = {corners.lower_right(), corners.lower_right()};
    radii[3] = {corners.lower_left(), corners.lower_left()};
    SkRRect rrect;
    rrect.setRectRadii(rect, radii);
    return rrect;
  }

  raw_ptr<RailDelegate> delegate_ = nullptr;
  raw_ptr<const RailConfig> cfg_ = nullptr;
  bool should_show_label_ = false;
  std::u16string label_text_;
  base::CallbackListSubscription paint_as_active_subscription_;
  std::optional<RecentWorkspaceGroup> recent_group_;
  std::unique_ptr<ui::SimpleMenuModel> context_menu_model_;
  std::unique_ptr<views::MenuRunner> context_menu_runner_;
  sidebar_metrics::SidebarMode display_mode_ =
      sidebar_metrics::SidebarMode::kExpanded;
  const bool is_update_;
};

BEGIN_METADATA(CmuxRailNewWorkspaceButton)
END_METADATA

// ---- Header: traffic-light clearance + window drag -------------------------
class CmuxRailHeader : public views::View {
  METADATA_HEADER(CmuxRailHeader, views::View)

 public:
  explicit CmuxRailHeader(RailDelegate* delegate) : delegate_(delegate) {}
  void set_config(const RailConfig* cfg) {
    cfg_ = cfg;
    InvalidateLayout();
    SchedulePaint();
  }
  void OnPaint(gfx::Canvas* canvas) override {
    // Helium removes Chromium's top-container separator. This empty drag band
    // only reserves the native caption-button area.
  }

  bool OnMousePressed(const ui::MouseEvent& event) override {
    if (!event.IsOnlyLeftMouseButton()) {
      return false;
    }
    if (delegate_) {
      delegate_->OnBeginWindowDrag(event);
    }
    return true;
  }

  bool OnMouseDragged(const ui::MouseEvent&) override { return true; }
  void OnMouseReleased(const ui::MouseEvent&) override {}

 private:
  raw_ptr<RailDelegate> delegate_;
  raw_ptr<const RailConfig> cfg_ = nullptr;
};

BEGIN_METADATA(CmuxRailHeader)
END_METADATA

// Workspace-neutral copy of Chromium 150's tabs::RoundedScrollBar. Helium's
// vertical strip installs this real, interactive overlay scrollbar in its
// layer-backed ScrollView; keeping that architecture is important for wheel,
// gesture, thumb-drag, track-click, hover, and fade behavior.
class CmuxRailScrollBar : public views::ScrollBar {
  METADATA_HEADER(CmuxRailScrollBar, views::ScrollBar)

 public:
  CmuxRailScrollBar()
      : views::ScrollBar(views::ScrollBar::Orientation::kVertical) {
    SetNotifyEnterExitOnChild(true);
    SetLayoutManager(std::make_unique<views::FillLayout>());
    auto* thumb = new Thumb(this);
    SetThumb(thumb);
    thumb->Init();
  }
  CmuxRailScrollBar(const CmuxRailScrollBar&) = delete;
  CmuxRailScrollBar& operator=(const CmuxRailScrollBar&) = delete;
  ~CmuxRailScrollBar() override = default;

  void SetAnimationConfig(bool, base::TimeDelta) {}

  void OnMouseEntered(const ui::MouseEvent&) override {
    views::AsViewClass<Thumb>(GetThumb())->Show();
  }

  void OnMouseExited(const ui::MouseEvent&) override {
    views::AsViewClass<Thumb>(GetThumb())->StartHideCountdown();
  }

  bool OverlapsContent() const override { return true; }

  gfx::Rect GetTrackBounds() const override { return GetContentsBounds(); }

  int GetThickness() const override {
    return kThumbThickness + kThumbTrailingPadding;
  }

 protected:
  static constexpr int kThumbThickness = 5;
  static constexpr int kThumbTrailingPadding = 4;

  class Thumb : public views::BaseScrollBarThumb {
    METADATA_HEADER(Thumb, views::BaseScrollBarThumb)

   public:
    explicit Thumb(CmuxRailScrollBar* scroll_bar)
        : views::BaseScrollBarThumb(scroll_bar) {}
    Thumb(const Thumb&) = delete;
    Thumb& operator=(const Thumb&) = delete;
    ~Thumb() override = default;

    void Init() {
      SetFlipCanvasOnPaintForRTLUI(true);
      SetPaintToLayer();
      layer()->SetFillsBoundsOpaquely(false);
      StartHideCountdown();
      layer()->SetAnimator(ui::LayerAnimator::CreateImplicitAnimator());
    }

    void Show() {
      if (layer()->GetTargetOpacity() != 1.0f) {
        layer()->SetOpacity(1.0f);
      }
      hide_timer_.Stop();
    }

    void Hide() {
      ui::ScopedLayerAnimationSettings settings(layer()->GetAnimator());
      settings.SetTransitionDuration(ui::GetOverlayScrollbarFadeDuration());
      if (layer()->GetTargetOpacity() != 0.0f) {
        layer()->SetOpacity(0.0f);
      }
    }

    void StartHideCountdown() {
      hide_timer_.Start(
          FROM_HERE, ui::GetOverlayScrollbarFadeDelay(),
          base::BindOnce(&Thumb::Hide, base::Unretained(this)));
    }

   protected:
    gfx::Size CalculatePreferredSize(
        const views::SizeBounds&) const override {
      return gfx::Size(kThumbThickness + kThumbTrailingPadding,
                       kThumbThickness + kThumbTrailingPadding);
    }

    void OnPaint(gfx::Canvas* canvas) override {
      cc::PaintFlags fill;
      fill.setStyle(cc::PaintFlags::kFill_Style);
      fill.setColor(
          GetColorProvider()->GetColor(ui::kColorSysStateDisabled));
      gfx::RectF bounds(GetLocalBounds());
      bounds.Inset(gfx::InsetsF::TLBR(0, 0, 0, kThumbTrailingPadding));
      canvas->DrawRoundRect(bounds, bounds.width() / 2.0f, fill);
    }

    void OnBoundsChanged(const gfx::Rect&) override {
      Show();
      if (GetState() == views::Button::STATE_NORMAL) {
        StartHideCountdown();
      }
    }

   private:
    base::OneShotTimer hide_timer_;
  };
};

BEGIN_METADATA(CmuxRailScrollBar, Thumb)
END_METADATA

BEGIN_METADATA(CmuxRailScrollBar)
END_METADATA

// ---- The rail ---------------------------------------------------------------
CmuxRail::CmuxRail(RailDelegate* delegate, int forced_style)
    : delegate_(delegate) {
  SetNotifyEnterExitOnChild(true);
  const int style = forced_style >= 0 ? forced_style : ReadRailStyle();
  config_ = RailConfig::Preset(style);
  drag_start_animation_ =
      std::make_unique<CmuxRailDragStartAnimation>(this);
  drag_scroll_handler_ = std::make_unique<CmuxRailDragScrollHandler>();
  LoadRailConfigAsync(config_, base::BindOnce(
                                   [](base::WeakPtr<CmuxRail> rail,
                                      std::optional<RailConfig> config) {
                                     if (rail && config) {
                                       rail->SetConfig(*config);
                                       if (rail->delegate_) {
                                         rail->delegate_->OnRailConfigLoaded();
                                       }
                                     }
                                   },
                                   weak_factory_.GetWeakPtr()));

  scroll_view_ = AddChildView(std::make_unique<views::ScrollView>(
      views::ScrollView::ScrollWithLayers::kEnabled));
  scroll_view_->SetUseContentsPreferredSize(true);
  scroll_view_->SetBackgroundColor(std::nullopt);
  scroll_view_->SetHorizontalScrollBarMode(
      views::ScrollView::ScrollBarMode::kDisabled);
  scroll_view_->SetOverflowGradientMask(
      views::ScrollView::GradientDirection::kVertical);
  auto scroll_bar = std::make_unique<CmuxRailScrollBar>();
  scrollbar_ = scroll_bar.get();
  scroll_view_->SetVerticalScrollBar(std::move(scroll_bar));

  content_ = scroll_view_->SetContents(std::make_unique<views::View>());
  collection_ =
      content_->AddChildView(std::make_unique<CmuxRailCollectionView>(this));
  hover_card_controller_ =
      std::make_unique<CmuxRailHoverCardController>(this, delegate_);
  hover_card_scroll_subscription_ =
      scroll_view_->AddContentsScrolledCallback(base::BindRepeating(
          [](base::WeakPtr<CmuxRail> rail) {
            if (rail && rail->hover_card_controller_) {
              rail->hover_card_controller_->Update(
                  nullptr, kInvalidId,
                  CmuxRailHoverCardController::UpdateType::kAnimating);
            }
          },
          weak_factory_.GetWeakPtr()));
  ApplyAnimationConfig();
  ApplyChrome();
}

CmuxRail::~CmuxRail() = default;

void CmuxRail::AddedToWidget() {
  if (hover_card_controller_) {
    hover_card_controller_->AddedToWidget();
  }
}

void CmuxRail::RemovedFromWidget() {
  if (hover_card_controller_) {
    hover_card_controller_->RemovedFromWidget();
  }
}

void CmuxRail::OnMouseExited(const ui::MouseEvent&) {
  if (hover_card_controller_) {
    hover_card_controller_->Update(
        nullptr, kInvalidId,
        CmuxRailHoverCardController::UpdateType::kHover);
  }
}

void CmuxRail::OnWorkspaceRowMouseEntered(CmuxRailRow* row) {
  if (hover_card_controller_ && row) {
    hover_card_controller_->Update(
        row, row->workspace_id(),
        CmuxRailHoverCardController::UpdateType::kHover);
  }
}

void CmuxRail::OnWorkspaceRowFocused(CmuxRailRow* row) {
  if (hover_card_controller_ && row) {
    hover_card_controller_->Update(
        row, row->workspace_id(),
        CmuxRailHoverCardController::UpdateType::kFocus);
  }
}

void CmuxRail::OnWorkspaceRowBlurred() {
  views::FocusManager* focus_manager = GetFocusManager();
  views::View* focused =
      focus_manager ? focus_manager->GetFocusedView() : nullptr;
  if (hover_card_controller_ && (!focused || !Contains(focused))) {
    hover_card_controller_->Update(
        nullptr, kInvalidId,
        CmuxRailHoverCardController::UpdateType::kFocus);
  }
}

void CmuxRail::OnWorkspaceRowEvent() {
  if (hover_card_controller_) {
    hover_card_controller_->Update(
        nullptr, kInvalidId,
        CmuxRailHoverCardController::UpdateType::kEvent);
  }
}

void CmuxRail::NotifyWorkspaceDataChanged(WorkspaceId id) {
  const auto row = std::ranges::find_if(rows_, [id](CmuxRailRow* candidate) {
    return candidate && candidate->workspace_id() == id;
  });
  if (row != rows_.end()) {
    OnWorkspaceRowDataChanged(*row);
  }
}

void CmuxRail::OnWorkspaceRowDataChanged(CmuxRailRow* row) {
  if (hover_card_controller_ && row) {
    hover_card_controller_->Update(
        row, row->workspace_id(),
        CmuxRailHoverCardController::UpdateType::kDataChanged);
  }
}

void CmuxRail::ApplyChrome() {
  SetBackground(views::CreateSolidBackground(config_.bg));
  header_h_ = std::max(0, config_.header_height);

  if (!header_) {
    header_ = AddChildView(std::make_unique<CmuxRailHeader>(delegate_));
  }
  header_->set_config(&config_);

  if (!footer_) {
    footer_ =
        AddChildView(std::make_unique<CmuxRailNewWorkspaceButton>(delegate_));
  }
  footer_->set_config(&config_);
  footer_->set_display_mode(display_mode_);
  footer_->SetVisible(true);

  if (!update_footer_) {
    update_footer_ = AddChildView(
        std::make_unique<CmuxRailNewWorkspaceButton>(delegate_, true));
  }
  update_footer_->set_config(&config_);
  update_footer_->set_display_mode(display_mode_);
  update_footer_->SetUpdateVersion(update_version_);
  update_footer_->SetVisible(update_ready_);

  InvalidateLayout();
  SchedulePaint();
}

void CmuxRail::SetDisplayMode(sidebar_metrics::SidebarMode mode) {
  if (display_mode_ == mode) {
    return;
  }
  display_mode_ = mode;
  for (CmuxRailRow* row : rows_) {
    row->set_display_mode(mode);
  }
  for (auto [group_id, group_view] : group_views_) {
    group_view->SetDisplayMode(mode);
  }
  ApplyChrome();
  ReflowRows(/*animated=*/false);
}

void CmuxRail::SetUpdateReady(bool ready, const std::string& version) {
  update_ready_ = ready;
  update_version_ = version;
  if (!update_footer_) {
    return;
  }
  update_footer_->SetUpdateVersion(version);
  update_footer_->SetVisible(ready);
  InvalidateLayout();
}

gfx::Rect CmuxRail::FirstRowBoundsForTesting() const {
  return rows_.empty() ? gfx::Rect() : rows_.front()->bounds();
}

gfx::Rect CmuxRail::NewWorkspaceButtonBoundsForTesting() const {
  return footer_ ? footer_->bounds() : gfx::Rect();
}

void CmuxRail::SetNewWorkspaceButtonHoveredForTesting() {
  if (footer_) {
    footer_->SetState(views::Button::STATE_HOVERED);
  }
}

bool CmuxRail::NewWorkspaceButtonIsHoveredForTesting() const {
  return footer_ && footer_->GetState() == views::Button::STATE_HOVERED;
}

void CmuxRail::SetConfig(const RailConfig& config) {
  config_ = config;
  ApplyAnimationConfig();
  ApplyChrome();
  for (CmuxRailRow* row : rows_) {
    row->set_config(&config_);
    row->ApplyConfigToLabel();
    row->InvalidateLayout();
    row->SchedulePaint();
  }
  for (auto [group, group_view] : group_views_) {
    group_view->SetConfig(&config_);
  }
  ClampScroll();
  ReflowRows(/*animated=*/false);
  UpdateScrollbar(/*flash=*/false);
}

bool CmuxRail::AnimationsEnabled() const {
  return config_.animations && config_.animation_ms > 0;
}

base::TimeDelta CmuxRail::AnimationDuration(int base_ms) const {
  if (!AnimationsEnabled()) {
    return base::Milliseconds(0);
  }
  return base::Milliseconds(std::max(0, config_.animation_ms * base_ms / 160));
}

void CmuxRail::ApplyAnimationConfig() {
  if (scrollbar_) {
    scrollbar_->SetAnimationConfig(AnimationsEnabled(), AnimationDuration(250));
  }
}

bool CmuxRail::ActiveDragStructureMatches(const WindowModel& model) const {
  if (!drag_views_detached_ || !dragging_ || !content_ ||
      model.roots().size() != rows_.size()) {
    return false;
  }

  // Keep the drag controller alive across non-structural model notifications
  // (selection, title, and icon changes), just as Chromium keeps detached tab
  // views in the drag controller's coordinate space. Any ordering, membership,
  // collapse, or ownership change invalidates the captured gap geometry and
  // must cancel before ordinary reconciliation reparents a view.
  for (size_t i = 0; i < rows_.size(); ++i) {
    CmuxRailRow* row = rows_[i];
    const WorkspaceId id = model.roots()[i];
    const Workspace* workspace = model.GetWorkspace(id);
    if (!row || !workspace || row->workspace_id() != id ||
        row->group_id() != workspace->group) {
      return false;
    }
  }

  std::set<WorkspaceGroupId> desired_groups;
  for (const WorkspaceGroup& group : model.workspace_groups()) {
    const std::vector<WorkspaceId> members = model.WorkspacesInGroup(group.id);
    if (members.empty()) {
      continue;
    }
    desired_groups.insert(group.id);
    const auto current = group_views_.find(group.id);
    if (current == group_views_.end() ||
        current->second->collapsed() != group.collapsed) {
      return false;
    }
  }
  if (desired_groups.size() != group_views_.size() ||
      std::ranges::any_of(group_views_, [&desired_groups](const auto& entry) {
        return !desired_groups.contains(entry.first);
      })) {
    return false;
  }

  return std::ranges::all_of(drag_views_, [this](const views::View* view) {
    return view && view->parent() == content_;
  });
}

void CmuxRail::RefreshActiveDragModelFields(
    const WindowModel& model,
    const std::map<WorkspaceId, gfx::ImageSkia>& icons) {
  CHECK(ActiveDragStructureMatches(model));

  for (size_t i = 0; i < rows_.size(); ++i) {
    CmuxRailRow* row = rows_[i];
    const WorkspaceId id = model.roots()[i];
    const Workspace* workspace = model.GetWorkspace(id);
    CHECK(workspace);
    const WorkspaceGroup* group =
        model.GetWorkspaceGroup(workspace->group);

    WorkspaceId representative = kInvalidId;
    if (group) {
      const std::vector<WorkspaceId> members =
          model.WorkspacesInGroup(group->id);
      if (!members.empty() && members.front() != id) {
        representative = members.front();
      }
    }

    RailItem item;
    item.workspace = id;
    item.group = group ? group->id : kInvalidId;
    item.depth = group ? 1 : 0;
    const auto icon = icons.find(id);
    row->SetItem(item, representative, workspace->title,
                 id == model.selected_workspace(),
                 model.IsWorkspaceSelected(id),
                 icon == icons.end() ? gfx::ImageSkia() : icon->second);
  }

  for (auto [group_id, group_view] : group_views_) {
    const WorkspaceGroup* group = model.GetWorkspaceGroup(group_id);
    CHECK(group);
    const std::vector<WorkspaceId> members =
        model.WorkspacesInGroup(group_id);
    CHECK(!members.empty());
    // ActiveDragStructureMatches() already rejected collapse changes, which
    // would invalidate the captured drag-block height. Title and color updates
    // are geometry-neutral and remain live while the drag is in progress.
    group_view->UpdateGroup(*group, members.front(), /*initial=*/false);
  }

  // Recompute the native insertion gap from the last pointer location without
  // rebuilding either logical collection. In particular, no content_-owned
  // drag view is offered to AddRow()/AddCollectionChild().
  UpdateRowDrag(drag_last_point_in_screen_);
  UpdateScrollbar(/*flash=*/false);
}

void CmuxRail::Update(const WindowModel& model) {
  Update(model, {});
}

void CmuxRail::Update(const WindowModel& model,
                      const std::map<WorkspaceId, gfx::ImageSkia>& icons) {
  group_menu_entries_.clear();
  for (const WorkspaceGroup& group : model.workspace_groups()) {
    const std::vector<WorkspaceId> members = model.WorkspacesInGroup(group.id);
    if (!members.empty()) {
      group_menu_entries_.push_back(
          {group.id, members.front(), group.title, group.color});
    }
  }

  // Detached drag visuals are deliberately absent from their logical
  // collections. Preserve that ownership invariant for geometry-neutral
  // refreshes; otherwise cancel the native-style drag while its raw pointers
  // and old hierarchy are still complete, then let ordinary reconciliation
  // consume the structural model change.
  if (drag_views_detached_) {
    if (ActiveDragStructureMatches(model)) {
      RefreshActiveDragModelFields(model, icons);
      return;
    }
    EndRowDrag(/*commit=*/false);
  }

  std::map<WorkspaceId, raw_ptr<CmuxRailRow>> rows_by_id;
  for (CmuxRailRow* row : rows_) {
    rows_by_id[row->workspace_id()] = row;
  }

  std::set<WorkspaceGroupId> desired_groups;
  std::set<WorkspaceGroupId> new_groups;
  for (const WorkspaceGroup& group : model.workspace_groups()) {
    const std::vector<WorkspaceId> members = model.WorkspacesInGroup(group.id);
    if (members.empty()) {
      continue;
    }
    desired_groups.insert(group.id);
    if (!group_views_.contains(group.id)) {
      auto group_view = std::make_unique<CmuxRailGroupView>(
          this, group, members.front());
      CmuxRailGroupView* group_ptr = group_view.get();
      collection_->AddCollectionChild(std::move(group_view));
      group_views_[group.id] = group_ptr;
      new_groups.insert(group.id);
    }
  }

  std::map<WorkspaceGroupId, std::vector<CmuxRailRow*>> group_rows;
  std::vector<views::View*> top_level;
  std::set<WorkspaceGroupId> emitted_groups;
  std::vector<raw_ptr<CmuxRailRow>> next_rows;
  next_rows.reserve(model.roots().size());

  for (WorkspaceId id : model.roots()) {
    const Workspace* workspace = model.GetWorkspace(id);
    if (!workspace) {
      continue;
    }
    const WorkspaceGroup* group =
        model.GetWorkspaceGroup(workspace->group);
    CmuxRailGroupView* target_group =
        group ? group_views_.at(group->id).get() : nullptr;

    CmuxRailRow* row = nullptr;
    auto existing = rows_by_id.find(id);
    if (existing != rows_by_id.end()) {
      row = existing->second;
    } else {
      auto created = std::make_unique<CmuxRailRow>(this);
      row = created.get();
      row->set_config(&config_);
      row->set_display_mode(display_mode_);
      if (target_group) {
        target_group->AddRow(std::move(created));
      } else {
        collection_->AddCollectionChild(std::move(created));
      }
    }

    views::View* target_parent =
        target_group ? static_cast<views::View*>(target_group)
                     : static_cast<views::View*>(collection_);
    if (row->parent() != target_parent) {
      const gfx::Rect previous_bounds =
          row->GetWidget() ? row->GetBoundsInScreen() : gfx::Rect();
      std::unique_ptr<views::View> owned =
          row->parent()->RemoveChildViewT(row);
      if (target_group) {
        target_group->AddRow(std::move(owned), previous_bounds);
      } else {
        collection_->AddCollectionChild(std::move(owned), previous_bounds);
      }
    }

    RailItem item;
    item.workspace = id;
    item.group = group ? group->id : kInvalidId;
    item.depth = group ? 1 : 0;
    WorkspaceId representative = kInvalidId;
    if (group) {
      const std::vector<WorkspaceId> members =
          model.WorkspacesInGroup(group->id);
      if (!members.empty() && members.front() != id) {
        representative = members.front();
      }
    }
    auto icon = icons.find(id);
    row->SetItem(item, representative, workspace->title,
                 id == model.selected_workspace(),
                 model.IsWorkspaceSelected(id),
                 icon == icons.end() ? gfx::ImageSkia() : icon->second);
    next_rows.push_back(row);

    if (group) {
      group_rows[group->id].push_back(row);
      if (emitted_groups.insert(group->id).second) {
        top_level.push_back(target_group);
      }
    } else {
      top_level.push_back(row);
    }
  }

  // Target layouts come from the model-owned logical order. Pending-delete
  // children deliberately remain in the View hierarchy but are absent here,
  // exactly like Chromium's TabCollectionNode::GetDirectChildren().
  for (auto [group_id, group_view] : group_views_) {
    if (!desired_groups.contains(group_id)) {
      continue;
    }
    const WorkspaceGroup* group = model.GetWorkspaceGroup(group_id);
    CHECK(group);
    std::vector<CmuxRailRow*> members = group_rows[group_id];
    group_view->SetRows(members);
    group_view->UpdateGroup(
        *group, members.empty() ? kInvalidId : members.front()->workspace_id(),
        new_groups.contains(group_id));
  }
  collection_->SetLogicalChildren(top_level);

  // True workspace removals use the same deferred zero-height/opacity path as
  // Chromium. Group collapse never reaches this path: every member row above
  // remains parented inside its group.
  for (CmuxRailRow* old_row : rows_) {
    if (std::ranges::contains(next_rows, old_row)) {
      continue;
    }
    CmuxRailGroupView* parent_group = nullptr;
    for (auto [group_id, group_view] : group_views_) {
      if (old_row->parent() == group_view) {
        parent_group = group_view;
        break;
      }
    }
    if (parent_group &&
        desired_groups.contains(parent_group->group_id())) {
      parent_group->AnimateAndDestroyRow(old_row);
    } else if (old_row->parent() == collection_) {
      collection_->AnimateAndDestroy(old_row);
    } else if (old_row->parent() == content_) {
      content_->RemoveChildViewT(old_row);
    }
  }
  rows_ = std::move(next_rows);

  // Empty groups animate out as one outer collection child. Their inner
  // manager and any pending-delete descendants remain owned until that outer
  // transition completes.
  for (auto it = group_views_.begin(); it != group_views_.end();) {
    if (desired_groups.contains(it->first)) {
      ++it;
      continue;
    }
    CmuxRailGroupView* stale = it->second;
    if (stale->parent() == collection_) {
      collection_->AnimateAndDestroy(stale);
    }
    it = group_views_.erase(it);
  }

  ClampScroll();
  ReflowRows(/*animated=*/true);
  UpdateScrollbar(/*flash=*/false);
}

void CmuxRail::Layout(PassKey) {
  const int w = width();
  const bool update_ready = update_footer_ && update_footer_->GetVisible();
  const int footer_reserved =
      sidebar_metrics::FooterReservedHeight(update_ready);
  const int content_h = std::max(0, height() - header_h_ - footer_reserved);
  if (header_ && header_h_ > 0) {
    header_->SetBoundsRect(gfx::Rect(0, 0, w, std::min(header_h_, height())));
  }
  if (scroll_view_) {
    scroll_view_->SetBoundsRect(gfx::Rect(0, header_h_, w, content_h));
  }
  if (footer_) {
    const int footer_x = sidebar_metrics::ControlXForMode(
        display_mode_, w, sidebar_metrics::kFooterSideAndBottomInset);
    const int footer_width = sidebar_metrics::ControlWidthForMode(
        display_mode_, w, sidebar_metrics::kFooterSideAndBottomInset);
    footer_->SetBoundsRect(
        gfx::Rect(footer_x,
                  height() - sidebar_metrics::kFooterSideAndBottomInset -
                      sidebar_metrics::kFooterHeight,
                  footer_width, sidebar_metrics::kFooterHeight));
  }
  if (update_footer_ && update_ready) {
    const int footer_x = sidebar_metrics::ControlXForMode(
        display_mode_, w, sidebar_metrics::kFooterSideAndBottomInset);
    const int footer_width = sidebar_metrics::ControlWidthForMode(
        display_mode_, w, sidebar_metrics::kFooterSideAndBottomInset);
    update_footer_->SetBoundsRect(gfx::Rect(
        footer_x,
        height() - sidebar_metrics::kFooterSideAndBottomInset -
            2 * sidebar_metrics::kFooterHeight - sidebar_metrics::kFooterGap,
        footer_width, sidebar_metrics::kFooterHeight));
  }
  ClampScroll();
  ReflowRows(/*animated=*/false);
  UpdateScrollbar(/*flash=*/false);
}

int CmuxRail::ContentHeight() const {
  if (!collection_) {
    return 0;
  }
  const int available_width =
      scroll_view_ ? scroll_view_->GetContentsBounds().width() : width();
  return collection_
      ->GetPreferredSize(views::SizeBounds(available_width, {}))
      .height();
}

int CmuxRail::MaxScroll() const {
  return std::max(0, ContentHeight() -
                         (scroll_view_ ? scroll_view_->height() : 0));
}

void CmuxRail::ClampScroll() {}

void CmuxRail::UpdateScrollbar(bool) {}

bool CmuxRail::OnMouseWheel(const ui::MouseWheelEvent&) { return false; }

void CmuxRail::ReflowRows(bool) {
  if (!scroll_view_ || !content_ || !collection_) {
    return;
  }
  const int content_width = scroll_view_->GetContentsBounds().width();
  const int collection_height =
      collection_
          ->GetPreferredSize(views::SizeBounds(content_width, {}))
          .height();
  content_->SetPreferredSize(gfx::Size(content_width, collection_height));
  collection_->SetBoundsRect(
      gfx::Rect(0, 0, content_width, collection_height));
  scroll_view_->InvalidateLayout();
}

void CmuxRail::ScheduleContentGeometrySync() {
  if (content_geometry_sync_pending_) {
    return;
  }
  content_geometry_sync_pending_ = true;
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE,
      base::BindOnce(&CmuxRail::RunScheduledContentGeometrySync,
                     weak_factory_.GetWeakPtr()));
}

void CmuxRail::RunScheduledContentGeometrySync() {
  content_geometry_sync_pending_ = false;
  ReflowRows(/*animated=*/false);
}

// ---- Drag and drop ----------------------------------------------------------
void CmuxRail::StartRowDrag(CmuxRailRow* row,
                            const gfx::Point& press_screen_pt,
                            const gfx::Point& current_screen_pt,
                            const WorkspaceSelectionState& original_selection) {
  StartDrag(row, press_screen_pt, current_screen_pt, kInvalidId,
            original_selection);
}

void CmuxRail::StartGroupDrag(
    WorkspaceGroupId id,
    const gfx::Point& press_screen_pt,
    const gfx::Point& current_screen_pt,
    const WorkspaceSelectionState& original_selection) {
  auto found = group_views_.find(id);
  if (found == group_views_.end() || found->second->logical_rows().empty()) {
    return;
  }
  StartDrag(found->second->logical_rows().front(), press_screen_pt,
            current_screen_pt, id, original_selection);
}

void CmuxRail::StartDrag(CmuxRailRow* row,
                        const gfx::Point& press_screen_pt,
                        const gfx::Point& current_screen_pt,
                        WorkspaceGroupId header_group,
                        const WorkspaceSelectionState& original_selection) {
  if (!content_ || !collection_ || !row || dragging_) {
    return;
  }
  // TabDragController records this immediately after crossing the drag
  // threshold. VerticalDraggedTabsContainer counts all setup time against its
  // 200 ms consolidation animation.
  const base::TimeTicks drag_start_time = base::TimeTicks::Now();

  CmuxRailGroupView* header_group_view = nullptr;
  if (header_group != kInvalidId) {
    auto found = group_views_.find(header_group);
    if (found == group_views_.end() || found->second->logical_rows().empty()) {
      return;
    }
    header_group_view = found->second;
  } else if (delegate_) {
    // Chromium makes the source active/anchor when the drag actually crosses
    // threshold, while retaining the complete selected set.
    delegate_->OnActivateWorkspaceInSelection(row->workspace_id());
  }

  dragging_ = row;
  if (GetWidget()) {
    drag_event_tracker_ = std::make_unique<CmuxRailDragEventTracker>(
        base::BindOnce(&CmuxRail::EndRowDrag, base::Unretained(this),
                       /*commit=*/true),
        base::BindOnce(&CmuxRail::EndRowDrag, base::Unretained(this),
                       /*commit=*/false),
        GetWidget()->GetNativeWindow());
  }
  dragging_group_header_ = header_group;
  drag_original_selection_ = original_selection;
  dragging_group_active_workspace_ = kInvalidId;
  drag_group_.clear();
  if (header_group_view) {
    // A header represents the collection node itself. TabDragController resets
    // selection to the group's tabs when the drag crosses threshold, choosing
    // the group's previously active member or its first member as active.
    drag_group_.assign(header_group_view->logical_rows().begin(),
                       header_group_view->logical_rows().end());
    const bool original_active_is_member = std::ranges::any_of(
        drag_group_, [&](const CmuxRailRow* candidate) {
          return candidate->workspace_id() == original_selection.active;
        });
    dragging_group_active_workspace_ =
        original_active_is_member ? original_selection.active
                                  : drag_group_.front()->workspace_id();
    if (delegate_) {
      WorkspaceSelectionState dragging_selection;
      dragging_selection.active = dragging_group_active_workspace_;
      dragging_selection.anchor = dragging_group_active_workspace_;
      for (const CmuxRailRow* candidate : drag_group_) {
        dragging_selection.selected.push_back(candidate->workspace_id());
      }
      delegate_->OnRestoreWorkspaceSelectionState(dragging_selection);
    }
  } else {
    for (CmuxRailRow* candidate : rows_) {
      if (IsRowSelected(candidate->workspace_id())) {
        drag_group_.push_back(candidate);
      }
    }
    if (!std::ranges::contains(drag_group_, row)) {
      drag_group_ = {row};
    }
  }
  drag_origin_group_ = row->group_id();
  drop_group_ = drag_origin_group_;
  snap_drag_views_to_target_.clear();
  drag_unclamped_bottom_ = 0;

  const gfx::Rect previous_bounds =
      header_group_view ? header_group_view->GetBoundsInScreen()
                        : row->GetBoundsInScreen();
  drag_grab_dy_ = press_screen_pt.y() - previous_bounds.y();

  // A completely selected group is one drag collection in Chromium. Build a
  // rail-ordered list of visual drag items where such a group contributes its
  // real outer view exactly once; selected rows from partial groups remain
  // independent items.
  std::map<WorkspaceGroupId, int> group_member_counts;
  std::map<WorkspaceGroupId, int> selected_group_member_counts;
  for (CmuxRailRow* candidate : rows_) {
    if (candidate->group_id() == kInvalidId) {
      continue;
    }
    ++group_member_counts[candidate->group_id()];
    if (InDragGroup(candidate)) {
      ++selected_group_member_counts[candidate->group_id()];
    }
  }
  drag_full_groups_.clear();
  for (const auto& [group_id, member_count] : group_member_counts) {
    if (member_count > 0 && group_views_.contains(group_id) &&
        selected_group_member_counts[group_id] == member_count) {
      drag_full_groups_.insert(group_id);
    }
  }

  drag_views_.clear();
  std::set<WorkspaceGroupId> emitted_drag_groups;
  for (CmuxRailRow* candidate : rows_) {
    if (!InDragGroup(candidate)) {
      continue;
    }
    if (drag_full_groups_.contains(candidate->group_id())) {
      if (emitted_drag_groups.insert(candidate->group_id()).second) {
        drag_views_.push_back(group_views_.at(candidate->group_id()));
      }
    } else {
      drag_views_.push_back(candidate);
    }
  }

  // Capture every visual's pre-drag position before changing either nested
  // collection. Chromium uses these offsets to animate discontiguous selected
  // views into one contiguous stack around the source view.
  std::vector<gfx::Rect> drag_original_bounds_in_screen;
  drag_original_bounds_in_screen.reserve(drag_views_.size());
  for (views::View* drag_view : drag_views_) {
    drag_original_bounds_in_screen.push_back(drag_view->GetBoundsInScreen());
  }

  drag_view_heights_.clear();
  drag_primary_offset_y_ = 0;
  drag_visual_height_ = 0;
  CmuxRailGroupView* primary_group =
      drag_full_groups_.contains(row->group_id())
          ? group_views_.at(row->group_id()).get()
          : nullptr;
  std::optional<views::ProposedLayout> fallback_outer_target_layout;
  for (size_t i = 0; i < drag_views_.size(); ++i) {
    views::View* drag_view = drag_views_[i];
    int item_height = kWorkspaceRowHeight;
    if (!IsDraggedFullGroup(drag_view)) {
      item_height = kWorkspaceRowHeight;
    } else {
      // BuildDragLayout sizes whole groups from the collection's target
      // layout, never from an in-flight animated height.
      const views::ChildLayout* target_child_layout =
          collection_->target_layout().GetLayoutFor(drag_view);
      if (!target_child_layout) {
        if (!fallback_outer_target_layout.has_value()) {
          fallback_outer_target_layout.emplace(
              collection_->CalculateProposedLayout(
                  views::SizeBounds(collection_->width(), {})));
        }
        target_child_layout =
            fallback_outer_target_layout->GetLayoutFor(drag_view);
      }
      if (target_child_layout && target_child_layout->bounds.height() > 0) {
        item_height = target_child_layout->bounds.height();
      } else {
        item_height = drag_view
                          ->GetPreferredSize(views::SizeBounds(
                              std::max(0, content_->width() -
                                              2 * kSidebarHorizontalPadding),
                              {}))
                          .height();
      }
    }
    item_height = std::max(1, item_height);
    drag_view_heights_.push_back(item_height);

    if (drag_view == row) {
      drag_primary_offset_y_ = drag_visual_height_;
    } else if (drag_view == primary_group) {
      if (header_group_view) {
        drag_primary_offset_y_ = drag_visual_height_;
      } else {
        drag_primary_offset_y_ = drag_visual_height_ + previous_bounds.y() -
                                 primary_group->GetBoundsInScreen().y();
      }
    }
    drag_visual_height_ += item_height;
    if (i + 1 < drag_views_.size()) {
      drag_visual_height_ += kTabVerticalPadding;
    }
  }

  drag_original_visibility_.clear();
  for (CmuxRailRow* candidate : drag_group_) {
    drag_original_visibility_[candidate->workspace_id()] =
        candidate->GetVisible();
  }

  ClearDragGaps();

  // Remove every selected row from the nested logical collections as one
  // operation. Partially selected groups retain their unselected members. A
  // fully selected group leaves the outer collection as one view, preserving
  // its header and color line in the drag block.
  std::map<WorkspaceGroupId, std::vector<CmuxRailRow*>> grouped;
  std::vector<views::View*> top_level;
  std::set<WorkspaceGroupId> emitted;
  for (CmuxRailRow* candidate : rows_) {
    if (InDragGroup(candidate)) {
      continue;
    }
    if (candidate->group_id() != kInvalidId &&
        group_views_.contains(candidate->group_id())) {
      grouped[candidate->group_id()].push_back(candidate);
      if (emitted.insert(candidate->group_id()).second) {
        top_level.push_back(group_views_.at(candidate->group_id()));
      }
    } else {
      top_level.push_back(candidate);
    }
  }
  for (auto [group_id, group] : group_views_) {
    if (!drag_full_groups_.contains(group_id)) {
      group->SetRows(grouped[group_id]);
    }
  }
  collection_->SetLogicalChildren(top_level);

  const gfx::Rect bounds_in_content =
      views::View::ConvertRectFromScreen(content_, previous_bounds);
  int drag_item_y = bounds_in_content.y() - drag_primary_offset_y_;
  drag_last_point_in_screen_ = current_screen_pt;
  drag_last_visual_block_top_ = drag_item_y;
  drag_last_nested_in_group_ = false;
  drag_start_offset_ys_.clear();
  drag_start_offset_ys_.reserve(drag_views_.size());
  bool has_drag_start_offset = false;
  for (size_t i = 0; i < drag_views_.size(); ++i) {
    views::View* drag_view = drag_views_[i];
    std::unique_ptr<views::View> owned =
        drag_view->parent()->RemoveChildViewT(drag_view);
    content_->AddChildView(std::move(owned));
    drag_view->SetVisible(true);
    const gfx::Rect original_bounds_in_content = views::View::ConvertRectFromScreen(
        content_, drag_original_bounds_in_screen[i]);
    const int start_offset_y = original_bounds_in_content.y() - drag_item_y;
    drag_start_offset_ys_.push_back(start_offset_y);
    has_drag_start_offset |= start_offset_y != 0;
    drag_view->SetBoundsRect(gfx::Rect(
        kSidebarHorizontalPadding, drag_item_y,
        std::max(0, content_->width() - 2 * kSidebarHorizontalPadding),
        drag_view_heights_[i]));
    drag_item_y += drag_view_heights_[i] + kTabVerticalPadding;
    content_->ReorderChildView(drag_view, content_->children().size() - 1);
  }
  drag_views_detached_ = true;
  for (CmuxRailRow* candidate : drag_group_) {
    if (!drag_full_groups_.contains(candidate->group_id())) {
      candidate->SetVisible(true);
    }
    candidate->SetDragging(true);
  }

  if (has_drag_start_offset) {
    drag_start_animation_->Start(drag_start_time);
  } else {
    drag_start_animation_->Stop();
  }
  drag_scroll_handler_->Begin(
      *scroll_view_,
      base::BindRepeating(&CmuxRail::OnDragContentsScrolled,
                          weak_factory_.GetWeakPtr()));
  UpdateRowDrag(current_screen_pt);
}

bool CmuxRail::InDragGroup(const views::View* view) const {
  return IsDraggedFullGroup(view) ||
         std::ranges::any_of(drag_group_, [view](const CmuxRailRow* candidate) {
           return candidate == view;
         });
}

bool CmuxRail::IsDraggedFullGroup(const views::View* view) const {
  return std::ranges::any_of(
      drag_full_groups_, [this, view](WorkspaceGroupId group_id) {
        auto found = group_views_.find(group_id);
        return found != group_views_.end() && found->second == view;
      });
}

bool CmuxRail::ShouldSnapDragViewToTarget(const views::View& view) const {
  return snap_drag_views_to_target_.contains(&view);
}

void CmuxRail::ClearSnapDragViewsForParent(const views::View& parent) {
  for (const views::View* child : parent.children()) {
    snap_drag_views_to_target_.erase(child);
  }
}

void CmuxRail::PositionDraggedViews(int visual_block_top,
                                    bool nested_in_group) {
  if (!content_ || drag_views_.size() != drag_view_heights_.size()) {
    return;
  }

  const bool is_consolidating =
      drag_start_animation_ && drag_start_animation_->is_animating();
  const double animation_value =
      drag_start_animation_ ? drag_start_animation_->GetCurrentValue() : 1.0;
  const gfx::Rect visible_bounds = content_->GetVisibleBounds();
  const int nested_drag_x =
      kSidebarHorizontalPadding +
      (nested_in_group && sidebar_metrics::ShowsLabels(display_mode_)
           ? kSidebarHorizontalPadding
           : 0);

  int drag_item_y = visual_block_top;
  for (size_t i = 0; i < drag_views_.size(); ++i) {
    views::View* drag_view = drag_views_[i];
    int animated_y = drag_item_y;
    const bool has_start_offset =
        i < drag_start_offset_ys_.size() && drag_start_offset_ys_[i] != 0;
    if (is_consolidating && has_start_offset) {
      animated_y += gfx::Tween::IntValueBetween(
          animation_value, drag_start_offset_ys_[i], 0);
    }

    // Whole groups always remain outer collection visuals. Individual rows use
    // Helium's second 6-DIP inset while hovering over an existing group.
    const int drag_x = IsDraggedFullGroup(drag_view)
                           ? kSidebarHorizontalPadding
                           : nested_drag_x;
    const int drag_width = std::max(
        0, content_->width() - drag_x - kSidebarHorizontalPadding);
    gfx::Rect bounds(drag_x, animated_y, drag_width, drag_view_heights_[i]);

    // Chromium clamps each still-consolidating view to the viewport after
    // applying its original-position offset. This keeps an offscreen member of
    // a large selection from flying through clipped space.
    if (is_consolidating && has_start_offset && !visible_bounds.IsEmpty()) {
      bounds.AdjustToFit(visible_bounds);
    }
    drag_view->SetBoundsRect(bounds);
    drag_item_y += drag_view_heights_[i] + kTabVerticalPadding;
  }
}

void CmuxRail::OnDragStartAnimationProgressed() {
  if (dragging_) {
    PositionDraggedViews(drag_last_visual_block_top_,
                         drag_last_nested_in_group_);
  }
}

void CmuxRail::OnDragStartAnimationEnded() {
  if (!dragging_) {
    return;
  }
  drag_start_offset_ys_.clear();
  PositionDraggedViews(drag_last_visual_block_top_,
                       drag_last_nested_in_group_);
  if (content_) {
    content_->InvalidateLayout();
  }
}

void CmuxRail::OnDragContentsScrolled() {
  if (dragging_) {
    UpdateRowDrag(drag_last_point_in_screen_);
  }
}

void CmuxRail::ReparentDraggedFullGroupsToCollection() {
  if (!content_ || !collection_) {
    return;
  }
  for (WorkspaceGroupId group_id : drag_full_groups_) {
    auto found = group_views_.find(group_id);
    if (found == group_views_.end() || found->second->parent() != content_) {
      continue;
    }
    CmuxRailGroupView* group = found->second;
    const gfx::Rect previous_bounds = group->GetBoundsInScreen();
    std::unique_ptr<views::View> owned = content_->RemoveChildViewT(group);
    collection_->AddCollectionChild(std::move(owned), previous_bounds);
  }
}

void CmuxRail::ClearDragGaps() {
  if (collection_) {
    collection_->SetDragGap(-1);
  }
  for (auto [group_id, group] : group_views_) {
    group->SetDragGap(-1);
  }
}

void CmuxRail::OpenGapAt(int gap_slot) {
  drag_slot_ = gap_slot;
  ClearDragGaps();
  if (collection_) {
    collection_->SetDragGap(gap_slot);
  }
  ReflowRows(/*animated=*/true);
}

void CmuxRail::RestoreDraggedRow() {
  if (!dragging_) {
    return;
  }

  ReparentDraggedFullGroupsToCollection();
  for (CmuxRailRow* row : drag_group_) {
    if (row->parent() == content_) {
      const gfx::Rect previous_bounds = row->GetBoundsInScreen();
      std::unique_ptr<views::View> owned = content_->RemoveChildViewT(row);
      if (row->group_id() != kInvalidId &&
          group_views_.contains(row->group_id())) {
        group_views_.at(row->group_id())
            ->AddRow(std::move(owned), previous_bounds);
      } else {
        collection_->AddCollectionChild(std::move(owned), previous_bounds);
      }
    }
    row->SetDragging(false);
  }

  std::map<WorkspaceGroupId, std::vector<CmuxRailRow*>> grouped;
  std::vector<views::View*> top_level;
  std::set<WorkspaceGroupId> emitted;
  for (CmuxRailRow* candidate : rows_) {
    if (candidate->group_id() != kInvalidId &&
        group_views_.contains(candidate->group_id())) {
      grouped[candidate->group_id()].push_back(candidate);
      if (emitted.insert(candidate->group_id()).second) {
        top_level.push_back(group_views_.at(candidate->group_id()));
      }
    } else {
      top_level.push_back(candidate);
    }
  }
  for (auto [group_id, group] : group_views_) {
    group->SetRows(grouped[group_id]);
  }
  collection_->SetLogicalChildren(top_level);
  for (CmuxRailRow* row : drag_group_) {
    auto visible = drag_original_visibility_.find(row->workspace_id());
    if (visible != drag_original_visibility_.end()) {
      row->SetVisible(visible->second);
    }
  }
}

void CmuxRail::UpdateRowDrag(const gfx::Point& screen_pt) {
  if (!dragging_ || !scroll_view_ || !content_ || !collection_) {
    return;
  }

  drag_last_point_in_screen_ = screen_pt;
  gfx::Point point_in_content = screen_pt;
  views::View::ConvertPointFromScreen(content_, &point_in_content);
  const int drag_count = std::max(1, static_cast<int>(drag_group_.size()));
  const int block_height = std::max(1, drag_visual_height_);
  // AddViewToVerticalDragLayout appends 2 DIP after every dragged visual,
  // including the final one. Keep that logical tail for overlap, clamping,
  // and edge scrolling without adding it to the visible stack or slot gap.
  const int drag_hit_test_height = block_height + kTabVerticalPadding;
  const int logical_block_top = point_in_content.y() - drag_grab_dy_ -
                                drag_primary_offset_y_;
  gfx::Rect logical_bounds(kSidebarHorizontalPadding, logical_block_top,
                           std::max(0, content_->width() -
                                           2 * kSidebarHorizontalPadding),
                           drag_hit_test_height);
  drag_unclamped_bottom_ = std::max(0, logical_bounds.bottom());

  // Match Chromium's split between logical drag bounds and visual bounds: the
  // unclamped box controls insertion and edge scrolling, while the rendered
  // stack remains adjusted to the visible viewport.
  const gfx::Rect logical_bounds_in_scroll = views::View::ConvertRectToTarget(
      content_, scroll_view_, logical_bounds);
  drag_scroll_handler_->OnDraggedViewPositionUpdated(
      *scroll_view_, logical_bounds_in_scroll);

  gfx::Rect visual_bounds = logical_bounds;
  const gfx::Rect visible_bounds = content_->GetVisibleBounds();
  if (!visible_bounds.IsEmpty()) {
    visual_bounds.AdjustToFit(visible_bounds);
  }
  const int visual_block_top = visual_bounds.y();

  gfx::Rect proposed_screen = logical_bounds;
  views::View::ConvertRectToScreen(content_, &proposed_screen);

  CmuxRailGroupView* target_group = nullptr;
  auto vertical_overlap = [](const gfx::Rect& a, const gfx::Rect& b) {
    return std::max(0, std::min(a.bottom(), b.bottom()) -
                           std::max(a.y(), b.y()));
  };
  const views::ProposedLayout outer_target_layout =
      collection_->CalculateProposedLayout(
          views::SizeBounds(collection_->width(), {}));
  auto target_group_bounds_in_screen =
      [&](const CmuxRailGroupView* group) -> std::optional<gfx::Rect> {
    const views::ChildLayout* group_layout =
        outer_target_layout.GetLayoutFor(group);
    if (!group_layout || !group_layout->visible) {
      return std::nullopt;
    }
    gfx::Rect bounds = group_layout->bounds;
    views::View::ConvertRectToScreen(collection_, &bounds);
    return bounds;
  };

  // Chromium's VerticalUnpinnedTabContainerView keeps handling a drag at the
  // outer container whenever VerticalTabDragHandler::IsDraggingGroups() is
  // true. A completely selected workspace group is likewise an atomic outer
  // child here, so neither it nor any other selected rows may enter a group.
  if (drag_full_groups_.empty()) {
    // Chromium uses asymmetric group-entry hysteresis: 40% of the 26 DIP
    // header to enter, and only exits once all but 10% of that header has
    // peeled away.
    if (drop_group_ != kInvalidId && group_views_.contains(drop_group_)) {
      CmuxRailGroupView* current = group_views_.at(drop_group_);
      if (!current->collapsed() && !current->logical_rows().empty() &&
          std::ranges::contains(collection_->logical_children(), current)) {
        const std::optional<gfx::Rect> current_target_bounds =
            target_group_bounds_in_screen(current);
        if (current_target_bounds.has_value()) {
          const int overlap =
              vertical_overlap(proposed_screen, *current_target_bounds);
          const float required =
              proposed_screen.height() - current->header()->height() * 0.1f;
          if (overlap >= required) {
            target_group = current;
          }
        }
      }
    }
    if (!target_group) {
      for (views::View* child : collection_->logical_children()) {
        CmuxRailGroupView* group = nullptr;
        for (auto [group_id, candidate] : group_views_) {
          if (child == candidate) {
            group = candidate;
            break;
          }
        }
        if (!group || group->collapsed()) {
          continue;
        }
        const std::optional<gfx::Rect> group_target_bounds =
            target_group_bounds_in_screen(group);
        if (!group_target_bounds.has_value()) {
          continue;
        }
        const int overlap =
            vertical_overlap(proposed_screen, *group_target_bounds);
        const float required = group->header()->height() * 0.4f;
        if (overlap >= required) {
          target_group = group;
          break;
        }
      }
    }
  }

  ClearDragGaps();
  if (target_group) {
    int index = 0;
    const views::ProposedLayout target_layout =
        target_group->CalculateProposedLayout(
            views::SizeBounds(target_group->width(), {}));
    for (CmuxRailRow* candidate : target_group->logical_rows()) {
      // Dragged views have already been removed from the logical collection,
      // so no post-source height discount is needed here. Chromium's remaining
      // comparison is the dragged block top against each candidate center.
      const views::ChildLayout* candidate_layout =
          target_layout.GetLayoutFor(candidate);
      if (!candidate_layout) {
        continue;
      }
      gfx::Point candidate_center = candidate_layout->bounds.CenterPoint();
      views::View::ConvertPointToScreen(target_group, &candidate_center);
      if (proposed_screen.y() >= candidate_center.y()) {
        ++index;
      }
    }
    target_group->SetDragGap(index, drag_count);
    drop_group_ = target_group->group_id();
    drop_parent_ = target_group->logical_rows().empty()
                       ? kInvalidId
                       : target_group->logical_rows().front()->workspace_id();
    drop_index_ = index;
    drag_slot_ = index;
  } else {
    int top_index = 0;
    const views::ProposedLayout target_layout =
        collection_->CalculateProposedLayout(
            views::SizeBounds(collection_->width(), {}));
    for (views::View* child : collection_->logical_children()) {
      const views::ChildLayout* child_layout = target_layout.GetLayoutFor(child);
      if (!child_layout) {
        continue;
      }
      gfx::Point child_center = child_layout->bounds.CenterPoint();
      views::View::ConvertPointToScreen(collection_, &child_center);
      if (proposed_screen.y() >= child_center.y()) {
        ++top_index;
      }
    }
    collection_->SetDragGapHeight(top_index, block_height);
    drag_slot_ = top_index;
    drop_group_ = kInvalidId;
    drop_parent_ = kInvalidId;

    // The outer collection treats a group as one atomic child. Convert its
    // insertion slot back to WindowModel's flat root index only at the model
    // boundary, so a drop can never split an existing group's run.
    int root_index = 0;
    for (int i = 0; i < top_index; ++i) {
      views::View* child = collection_->logical_children()[i];
      bool was_group = false;
      for (auto [group_id, group] : group_views_) {
        if (child == group) {
          root_index += static_cast<int>(group->logical_rows().size());
          was_group = true;
          break;
        }
      }
      if (!was_group) {
        ++root_index;
      }
    }
    drop_index_ = root_index;
  }

  drag_last_visual_block_top_ = visual_block_top;
  drag_last_nested_in_group_ = target_group != nullptr;
  PositionDraggedViews(visual_block_top, drag_last_nested_in_group_);
  ReflowRows(/*animated=*/true);
}

void CmuxRail::EndRowDrag(bool commit) {
  if (!dragging_) {
    return;
  }

  if (drag_event_tracker_) {
    drag_event_tracker_->Stop();
  }
  drag_scroll_handler_->End();
  drag_start_animation_->Stop();

  const WorkspaceId moved = dragging_->workspace_id();
  const WorkspaceGroupId moved_group = dragging_group_header_;
  const WorkspaceId moved_group_active = dragging_group_active_workspace_;
  const std::optional<WorkspaceSelectionState> original_selection =
      drag_original_selection_;
  const WorkspaceId parent = drop_parent_;
  const int index = drop_index_;

  ClearDragGaps();
  if (!commit || !delegate_) {
    snap_drag_views_to_target_.clear();
    RestoreDraggedRow();
  } else {
    snap_drag_views_to_target_.insert(drag_views_.begin(), drag_views_.end());
    snap_drag_views_to_target_.insert(drag_group_.begin(), drag_group_.end());
    // The model refresh below may retain this group at a new top-level slot or
    // dissolve it into an existing destination group. Return ownership to the
    // outer collection first so either synchronous update path starts from a
    // valid Chromium collection hierarchy.
    ReparentDraggedFullGroupsToCollection();
    for (CmuxRailRow* row : drag_group_) {
      row->SetDragging(false);
    }
  }

  drag_group_.clear();
  drag_full_groups_.clear();
  drag_views_.clear();
  drag_views_detached_ = false;
  drag_view_heights_.clear();
  drag_start_offset_ys_.clear();
  drag_original_visibility_.clear();
  drag_primary_offset_y_ = 0;
  drag_visual_height_ = 0;
  drag_unclamped_bottom_ = 0;
  drag_last_visual_block_top_ = 0;
  drag_last_nested_in_group_ = false;
  dragging_ = nullptr;
  drag_slot_ = -1;
  drag_origin_group_ = kInvalidId;
  dragging_group_header_ = kInvalidId;
  dragging_group_active_workspace_ = kInvalidId;
  drag_original_selection_.reset();
  drag_origin_index_ = -1;
  drop_group_ = kInvalidId;
  drop_parent_ = kInvalidId;
  drop_index_ = -1;

  if (commit && delegate_ && moved != kInvalidId) {
    if (moved_group != kInvalidId) {
      delegate_->OnMoveWorkspaceGroup(moved_group, index);
      if (moved_group_active != kInvalidId) {
        // CompleteDrag resets a successful Chromium group-header drag to the
        // group's active tab. Use ordinary selection for the workspace
        // adaptation so a collapsed group is also revealed; restoring the
        // drag-only selection would leave the active workspace hidden.
        delegate_->OnSelectWorkspace(moved_group_active);
      }
    } else {
      delegate_->OnMoveWorkspaceSelection(moved, parent, index);
    }
    ReflowRows(/*animated=*/true);
  } else {
    ReflowRows(/*animated=*/true);
    if (delegate_ && original_selection.has_value()) {
      delegate_->OnRestoreWorkspaceSelectionState(*original_selection);
    }
  }
}

void CmuxRail::SelectRow(WorkspaceId id) {
  if (delegate_) {
    delegate_->OnSelectWorkspace(id);
  }
}

void CmuxRail::ActivateRowInSelection(WorkspaceId id) {
  if (delegate_) {
    delegate_->OnActivateWorkspaceInSelection(id);
  }
}

void CmuxRail::ExtendSelectionToRow(WorkspaceId id) {
  if (delegate_) {
    delegate_->OnExtendWorkspaceSelection(id);
  }
}

void CmuxRail::AddSelectionFromAnchorToRow(WorkspaceId id) {
  if (delegate_) {
    delegate_->OnAddWorkspaceSelectionFromAnchorTo(id);
  }
}

void CmuxRail::ToggleRowSelection(WorkspaceId id) {
  if (delegate_) {
    delegate_->OnToggleWorkspaceSelection(id);
  }
}

bool CmuxRail::IsRowSelected(WorkspaceId id) const {
  return delegate_ && delegate_->IsWorkspaceSelected(id);
}

WorkspaceSelectionState CmuxRail::GetWorkspaceSelectionState() const {
  return delegate_ ? delegate_->GetWorkspaceSelectionState()
                   : WorkspaceSelectionState();
}

void CmuxRail::ToggleRow(WorkspaceId id) {
  if (delegate_) {
    delegate_->OnToggleExpanded(id);
  }
}

void CmuxRail::RenameRow(WorkspaceId id, const std::string& name) {
  if (delegate_ && !name.empty()) {
    delegate_->OnRenameWorkspace(id, name);
  }
}

void CmuxRail::BeginRenameRow(WorkspaceId id) {
  for (CmuxRailRow* row : rows_) {
    if (row->workspace_id() == id) {
      row->BeginRename();
    } else {
      row->CancelRename();
    }
  }
}

void CmuxRail::CloseRow(WorkspaceId id) {
  // Defer so we never delete the row inside its own mouse-event handler.
  if (!delegate_) {
    return;
  }
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE,
      base::BindOnce(&CmuxRail::CloseRowNow, weak_factory_.GetWeakPtr(), id));
}

void CmuxRail::ShowContextMenuForRow(WorkspaceId id,
                                     const gfx::Point& screen_pt,
                                     ui::mojom::MenuSourceType source_type) {
  if (!delegate_ || !GetWidget()) {
    return;
  }
  context_menu_ = std::make_unique<CmuxRailWorkspaceContextMenu>(this, id);
  context_menu_->Run(screen_pt, GetWidget(), source_type);
}

void CmuxRail::ShiftWorkspace(WorkspaceId id,
                              bool toward_start,
                              bool move_to_end) {
  if (!delegate_) {
    return;
  }

  const auto current = std::ranges::find_if(rows_, [id](CmuxRailRow* row) {
    return row->workspace_id() == id;
  });
  if (current == rows_.end()) {
    return;
  }
  const int current_index = static_cast<int>(current - rows_.begin());
  CmuxRailRow* moving = *current;

  // Chromium's Shift+reorder commands move a tab to the first/last unpinned
  // slot and remove it from any group. Workspaces have no pinned partition, so
  // the equivalent boundaries are the two ends of rail order.
  if (move_to_end) {
    delegate_->OnMoveWorkspace(id, kInvalidId, toward_start ? 0 : -1);
    return;
  }

  const int offset = toward_start ? -1 : 1;
  const int target_index = current_index + offset;
  const WorkspaceGroupId old_group = moving->group_id();
  if (target_index < 0 || target_index >= static_cast<int>(rows_.size())) {
    // Reordering outward at a strip boundary still removes a grouped tab from
    // its group in Chromium.
    if (old_group != kInvalidId) {
      delegate_->OnMoveWorkspace(id, kInvalidId, current_index);
    }
    return;
  }

  CmuxRailRow* target = rows_[target_index];
  const WorkspaceGroupId target_group = target->group_id();
  if (old_group != target_group) {
    if (old_group != kInvalidId) {
      // Crossing out of a group is a distinct first keystroke: ungroup in
      // place, then a subsequent command can move farther.
      delegate_->OnMoveWorkspace(id, kInvalidId, current_index);
      return;
    }

    if (target_group != kInvalidId) {
      const auto group = group_views_.find(target_group);
      if (group == group_views_.end()) {
        return;
      }
      if (group->second->collapsed()) {
        // A collapsed group moves as one visual slot. Skip over all of its
        // members without adding the workspace to it.
        int group_edge = target_index;
        while (group_edge + offset >= 0 &&
               group_edge + offset < static_cast<int>(rows_.size()) &&
               rows_[group_edge + offset]->group_id() == target_group) {
          group_edge += offset;
        }
        delegate_->OnMoveWorkspace(id, kInvalidId, group_edge);
      } else {
        // At an expanded group boundary Chromium changes membership without
        // moving past the adjacent tab.
        const int member_index =
            toward_start
                ? static_cast<int>(group->second->logical_rows().size())
                : 0;
        delegate_->OnMoveWorkspace(id, target->workspace_id(), member_index);
      }
      return;
    }

    delegate_->OnMoveWorkspace(id, kInvalidId, target_index);
    return;
  }

  if (old_group == kInvalidId) {
    delegate_->OnMoveWorkspace(id, kInvalidId, target_index);
    return;
  }

  const auto group = group_views_.find(old_group);
  if (group == group_views_.end()) {
    return;
  }
  const auto& members = group->second->logical_rows();
  const auto member = std::ranges::find(members, moving);
  if (member == members.end()) {
    return;
  }
  // `index` is evaluated after WindowModel removes the moving workspace.
  // Moving either direction by one therefore inserts at the moving member's
  // old index plus the direction; deriving it from the already-adjacent target
  // would apply the offset twice (for example A,B,C + move C up => C,A,B).
  const int member_index =
      static_cast<int>(member - members.begin()) + offset;
  delegate_->OnMoveWorkspace(id, target->workspace_id(), member_index);
}

void CmuxRail::ShiftWorkspaceGroup(WorkspaceGroupId id, bool toward_start) {
  if (!delegate_ || !collection_ || !group_views_.contains(id)) {
    return;
  }

  const std::vector<raw_ptr<views::View>>& children =
      collection_->logical_children();
  CmuxRailGroupView* moving_group = group_views_.at(id);
  const auto current = std::ranges::find(children, moving_group);
  if (current == children.end()) {
    return;
  }
  const int current_index = static_cast<int>(current - children.begin());
  const int target_index = current_index + (toward_start ? -1 : 1);
  if (target_index < 0 || target_index >= static_cast<int>(children.size())) {
    return;
  }

  auto workspace_count = [&](views::View* child) {
    for (const auto& group_entry : group_views_) {
      if (child == group_entry.second) {
        return static_cast<int>(
            group_entry.second->logical_rows().size());
      }
    }
    return views::IsViewClass<CmuxRailRow>(child) ? 1 : 0;
  };

  // OnMoveWorkspaceGroup's index is in WindowModel's flat root list after the
  // moving group's members have been removed. Convert the adjacent top-level
  // collection swap to that boundary exactly once here.
  int insertion_index = 0;
  for (int i = 0; i < static_cast<int>(children.size()); ++i) {
    if (children[i] == moving_group) {
      continue;
    }
    if (toward_start && i == target_index) {
      break;
    }
    insertion_index += workspace_count(children[i]);
    if (!toward_start && i == target_index) {
      break;
    }
  }
  delegate_->OnMoveWorkspaceGroup(id, insertion_index);
}

void CmuxRail::ShowGroupEditorBubble(WorkspaceGroupId id) {
  auto group = group_views_.find(id);
  if (group == group_views_.end()) {
    return;
  }
  ShowGroupEditorBubble(id, group->second->header());
}

void CmuxRail::ShowGroupEditorBubble(WorkspaceGroupId id,
                                     views::View* anchor_view) {
  if (!delegate_ || !GetWidget() || id == kInvalidId || !anchor_view) {
    return;
  }
  // TabGroupEditorBubbleTracker ignores a second context click until the open
  // editor has finished closing. The weak view pointer gives cmux the same
  // posted-destruction behavior without importing browser tab-group state.
  if (group_editor_bubble_) {
    return;
  }
  auto entry = std::ranges::find(group_menu_entries_, id,
                                 &GroupMenuEntry::id);
  if (entry == group_menu_entries_.end()) {
    return;
  }
  group_editor_bubble_ = CmuxRailGroupEditorBubble::Show(
      delegate_, id, entry->title, entry->color, anchor_view,
      base::BindOnce(&CmuxRail::OnGroupEditorBubbleClosed,
                     weak_factory_.GetWeakPtr(), id));
  if (group_editor_bubble_) {
    auto group = group_views_.find(id);
    if (group != group_views_.end()) {
      group->second->header()->OnBubbleOpened();
    }
  }
}

void CmuxRail::OnGroupEditorBubbleClosed(WorkspaceGroupId id) {
  group_editor_bubble_ = {};
  auto group = group_views_.find(id);
  if (group != group_views_.end()) {
    group->second->header()->OnBubbleClosed();
  }
}

void CmuxRail::CloseRowNow(WorkspaceId id) {
  if (delegate_) {
    delegate_->OnCloseWorkspace(id);
  }
}

BEGIN_METADATA(CmuxRail)
END_METADATA

}  // namespace cmux
