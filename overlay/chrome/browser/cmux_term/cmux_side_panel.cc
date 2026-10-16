// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/cmux_term/cmux_side_panel.h"

#include <algorithm>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <utility>

#include "base/callback_list.h"
#include "base/check.h"
#include "base/functional/bind.h"
#include "base/i18n/number_formatting.h"
#include "base/i18n/rtl.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/no_destructor.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/single_thread_task_runner.h"
#include "chrome/app/vector_icons/vector_icons.h"
#include "chrome/browser/cmux_term/cmux_pane_extension_pins.h"
#include "chrome/browser/cmux_term/cmux_side_panel_resize_area.h"
#include "chrome/browser/cmux_term/window_model.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/actions/chrome_actions.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/color/chrome_color_id.h"
#include "chrome/browser/ui/side_panel/side_panel_entry.h"
#include "chrome/browser/ui/side_panel/side_panel_entry_key.h"
#include "chrome/browser/ui/side_panel/side_panel_enums.h"
#include "chrome/browser/ui/side_panel/side_panel_metrics.h"
#include "chrome/browser/ui/side_panel/side_panel_registry.h"
#include "chrome/browser/ui/side_panel/side_panel_ui.h"
#include "chrome/browser/ui/tabs/public/tab_features.h"
#include "chrome/browser/ui/views/side_panel/side_panel_helper.h"
#include "chrome/browser/ui/views/chrome_layout_provider.h"
#include "chrome/browser/ui/views/toolbar/toolbar_button.h"
#include "chrome/common/chrome_version.h"
#include "chrome/common/pref_names.h"
#include "chrome/grit/generated_resources.h"
#include "components/prefs/pref_change_registrar.h"
#include "components/prefs/pref_service.h"
#include "components/prefs/scoped_user_pref_update.h"
#include "components/tabs/public/tab_interface.h"
#include "content/public/browser/web_contents.h"
#include "extensions/common/extension_id.h"
#include "ui/accessibility/mojom/ax_node_data.mojom.h"
#include "ui/actions/actions.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/base/ui_base_features.h"
#include "ui/base/unowned_user_data/scoped_unowned_user_data.h"
#include "ui/gfx/geometry/insets.h"
#include "ui/gfx/geometry/size.h"
#include "ui/views/accessibility/view_accessibility.h"
#include "ui/views/background.h"
#include "ui/views/border.h"
#include "ui/views/controls/button/image_button.h"
#include "ui/views/controls/button/image_button_factory.h"
#include "ui/views/controls/button/toggle_button.h"
#include "ui/views/controls/highlight_path_generator.h"
#include "ui/views/controls/image_view.h"
#include "ui/views/controls/label.h"
#include "ui/views/layout/box_layout.h"
#include "ui/views/layout/fill_layout.h"
#include "ui/views/vector_icons.h"
#include "ui/views/view.h"
#include "ui/views/view_class_properties.h"

namespace cmux {
namespace {

constexpr int kContentOutlineWidth = 1;
constexpr int kResizeAreaMargin = 6;
constexpr int kHeaderHeight = 40;
constexpr int kMinimumPanelWidth = 160;
constexpr int kMinimumPageWidth = 320;

auto SidePanelBorderColor() {
#if CHROME_VERSION_MAJOR >= 151
  return kColorSidePanelDivider;
#else
  return kColorSidePanelBorder;
#endif
}

void ConfigureSidePanelControlButton(views::ImageButton* button) {
  button->SetImageHorizontalAlignment(views::ImageButton::ALIGN_CENTER);
  views::InstallCircleHighlightPathGenerator(button);
  const int minimum_button_size =
      ChromeLayoutProvider::Get()->GetDistanceMetric(
          ChromeDistanceMetric::DISTANCE_SIDE_PANEL_HEADER_BUTTON_MINIMUM_SIZE);
  button->SetMinimumImageSize(
      gfx::Size(minimum_button_size, minimum_button_size));
}

class CmuxSidePanelUI;

std::map<Browser*, raw_ptr<CmuxSidePanelUI>>& Coordinators() {
  static base::NoDestructor<std::map<Browser*, raw_ptr<CmuxSidePanelUI>>>
      coordinators;
  return *coordinators;
}

CmuxSidePanelUI* CoordinatorFor(Browser* browser) {
  auto it = Coordinators().find(browser);
  return it == Coordinators().end() ? nullptr : it->second.get();
}

SidePanelRegistry* RegistryFor(content::WebContents* web_contents) {
  tabs::TabInterface* tab =
      web_contents ? tabs::TabInterface::GetFromContents(web_contents)
                   : nullptr;
  return tab ? SidePanelRegistry::From(tab) : nullptr;
}

class CmuxSidePanelHostView : public views::View,
                              public CmuxSidePanelResizeDelegate {
  METADATA_HEADER(CmuxSidePanelHostView, views::View)

 public:
  CmuxSidePanelHostView(Browser* browser,
                        content::WebContents* web_contents,
                        PaneId pane)
      : browser_(browser), web_contents_(web_contents), pane_(pane) {
    CHECK(browser_);
    CHECK(web_contents_);
    CHECK_NE(pane_, kInvalidId);
    SetBackground(views::CreateSolidBackground(kColorSidePanelContentBackground));
    SetLayoutManager(std::make_unique<views::FillLayout>());

    body_ = AddChildView(std::make_unique<views::View>());
    auto body_layout = std::make_unique<views::BoxLayout>(
        views::BoxLayout::Orientation::kVertical);
    body_layout->set_cross_axis_alignment(
        views::BoxLayout::CrossAxisAlignment::kStretch);
    body_layout_ = body_->SetLayoutManager(std::move(body_layout));

    content_ = body_->AddChildView(std::make_unique<views::View>());
    content_->SetLayoutManager(std::make_unique<views::FillLayout>());
    body_layout_->SetFlexForView(content_, 1);

    // Chrome's complete SidePanelResizeArea interaction, with Helium's
    // two-DIP handle and six-DIP margin geometry. Added after the body so its
    // hover/focus affordance paints over the margin.
    resize_area_ =
        AddChildView(std::make_unique<CmuxSidePanelResizeArea>(this));

    pref_registrar_.Init(browser_->profile()->GetPrefs());
    pref_registrar_.Add(
        prefs::kSidePanelHorizontalAlignment,
        base::BindRepeating(&CmuxSidePanelHostView::UpdateAlignment,
                            base::Unretained(this)));
    pane_pin_subscription_ = AddPaneExtensionPinsChangedCallback(
        browser_, pane_,
        base::BindRepeating(&CmuxSidePanelHostView::UpdatePinButton,
                            base::Unretained(this)));
    pref_registrar_.Add(
        prefs::kSidePanelIdToWidth,
        base::BindRepeating(&CmuxSidePanelHostView::UpdateWidthFromPrefs,
                            base::Unretained(this)));
    UpdateAlignment();
    SetVisible(false);

    if (CmuxSidePanelUI* ui = CoordinatorFor(browser_)) {
      RegisterWith(ui);
    }
  }

  CmuxSidePanelHostView(const CmuxSidePanelHostView&) = delete;
  CmuxSidePanelHostView& operator=(const CmuxSidePanelHostView&) = delete;
  ~CmuxSidePanelHostView() override;

  content::WebContents* web_contents() const { return web_contents_; }
  PaneId pane() const { return pane_; }

  SidePanelEntry* current_entry() const { return current_entry_.get(); }
  bool current_is_contextual() const { return current_is_contextual_; }

  bool IsShowing(const SidePanelEntry::Key& key, bool for_tab) const {
    return current_entry_ && current_entry_->key() == key &&
           current_is_contextual_ == for_tab;
  }

  void ShowEntry(SidePanelEntry* entry,
                 bool contextual,
                 std::optional<SidePanelOpenTrigger> trigger) {
    CHECK(entry);
    if (current_entry_.get() == entry) {
      SetVisible(true);
      return;
    }
    CloseEntry(SidePanelEntryHideReason::kReplaced,
               /*reset_registry=*/false);

    current_entry_ = entry->GetWeakPtr();
    current_is_contextual_ = contextual;
    entry->set_last_open_trigger(trigger);

    // Chrome owns a persistent header outside the entry's content view. Some
    // extension entries therefore return should_show_header() == false even
    // though Chrome still renders its title, pin, and close controls.
    if (entry->should_show_header() || entry->key().extension_id()) {
      BuildHeader(entry);
    }

    SidePanelNativeView view = entry->CachedView()
                                   ? std::move(entry->CachedView())
                                   : entry->GetContent();
    if (!view) {
      current_entry_.reset();
      return;
    }
    content_->AddChildView(std::move(view));
    entry->OnEntryShown();
    UpdateWidthFromPrefs();
    SetVisible(true);
    InvalidateLayout();
  }

  void CloseEntry(SidePanelEntryHideReason reason, bool reset_registry) {
    SidePanelEntry* entry = current_entry_.get();
    if (!entry) {
      RemoveHostedViews();
      SetVisible(false);
      return;
    }

    entry->OnEntryWillHide(reason);
    if (!content_->children().empty()) {
      SidePanelNativeView view =
          content_->RemoveChildViewT(content_->children().front());
      // Match Chrome's coordinator lifecycle: preserve a view while it is
      // replaced or moved between tabs, but destroy it when the user closes
      // the panel. ExtensionSidePanelCoordinator observes that destruction to
      // dispatch chrome.sidePanel.onClosed.
      if (reason == SidePanelEntryHideReason::kReplaced ||
          reason == SidePanelEntryHideReason::kBackgrounded) {
        entry->CacheView(std::move(view));
      }
    }
    if (reset_registry) {
      SidePanelRegistry* registry =
          current_is_contextual_ ? RegistryFor(web_contents_)
                                 : SidePanelRegistry::From(browser_);
      if (registry) {
#if CHROME_VERSION_MAJOR >= 151
        registry->ResetActiveEntry();
#else
        registry->ResetActiveEntryFor(entry->type());
#endif
      }
    }
    current_entry_.reset();
    current_is_contextual_ = false;
    RemoveHeader();
    SetVisible(false);
    entry->OnEntryHidden();
    InvalidateLayout();
  }

  void OnResize(int resize_amount, bool done_resizing) override {
    if (starting_width_ < 0) {
      starting_width_ = preferred_width_;
    }
    const bool resize_area_on_left =
        (IsRightAligned() && !base::i18n::IsRTL()) ||
        (!IsRightAligned() && base::i18n::IsRTL());
    const int proposed =
        starting_width_ + (resize_area_on_left ? -resize_amount
                                               : resize_amount);
    SetPanelWidth(proposed);
    did_resize_ = true;
    if (done_resizing) {
      starting_width_ = -1;
      PersistWidth();
    }
  }

  bool IsSidePanelRightAligned() const override { return IsRightAligned(); }

  void RecordSidePanelResizeMetrics() override {
    if (!did_resize_ || !current_entry_) {
      return;
    }
    SidePanelMetrics::RecordSidePanelResizeMetrics(
        current_entry_->key().id(),
        std::max(0, width() - kResizeAreaMargin - kContentOutlineWidth),
        parent() ? parent()->width() : width());
    did_resize_ = false;
  }

  void SetSidePanelKeyboardResized(bool keyboard_resized) override {
    keyboard_resized_ = keyboard_resized;
  }

  void OnBoundsChanged(const gfx::Rect& previous_bounds) override {
    views::View::OnBoundsChanged(previous_bounds);
    if (previous_bounds.width() != width() && keyboard_resized_) {
      keyboard_resized_ = false;
      AnnounceResize();
    }
  }

  void Layout(PassKey) override {
    LayoutSuperclass<views::View>(this);

    // Helium's side panel owns a six-DIP overlay outside the one-DIP content
    // outline. cmux's body and resize area are siblings in a FillLayout, so
    // the host must apply that overlay geometry after laying out the body.
    const bool resize_area_on_left =
        (IsRightAligned() && !base::i18n::IsRTL()) ||
        (!IsRightAligned() && base::i18n::IsRTL());
    resize_area_->SetBounds(resize_area_on_left
                                ? 0
                                : std::max(0, width() - kResizeAreaMargin),
                            0, kResizeAreaMargin, height());
    resize_area_->DeprecatedLayoutImmediately();
  }

 private:
  friend class CmuxSidePanelUI;

  void RegisterWith(CmuxSidePanelUI* ui);
  void UnregisterFromCoordinator();
  void CloseFromHeader();
  void CloseFromHeaderNow();

  bool IsRightAligned() const {
    return browser_->profile()->GetPrefs()->GetBoolean(
        prefs::kSidePanelHorizontalAlignment);
  }

  void UpdateAlignment() {
    if (!parent()) {
      return;
    }
    const bool right = IsRightAligned();
    const int web_index = 0;
    parent()->ReorderChildView(this, right ? 1 : web_index);
    SetBorder(views::CreateEmptyBorder(
        right ? gfx::Insets::TLBR(0, kResizeAreaMargin + kContentOutlineWidth,
                                 0, 0)
              : gfx::Insets::TLBR(0, 0, 0,
                                 kResizeAreaMargin + kContentOutlineWidth)));
    body_->SetBorder(views::CreateSolidSidedBorder(
        right ? gfx::Insets::TLBR(0, kContentOutlineWidth, 0, 0)
              : gfx::Insets::TLBR(0, 0, 0, kContentOutlineWidth),
        SidePanelBorderColor()));
    parent()->InvalidateLayout();
    InvalidateLayout();
  }

  void BuildHeader(SidePanelEntry* entry) {
    RemoveHeader();
    header_ = body_->AddChildViewAt(std::make_unique<views::View>(), 0);
    header_->SetPreferredSize(gfx::Size(1, kHeaderHeight));
    header_->SetBackground(views::CreateSolidBackground(kColorToolbar));
    auto layout = std::make_unique<views::BoxLayout>(
        views::BoxLayout::Orientation::kHorizontal);
    layout->set_cross_axis_alignment(
        views::BoxLayout::CrossAxisAlignment::kCenter);
    layout->set_inside_border_insets(gfx::Insets::TLBR(0, 12, 0, 4));
    layout->set_between_child_spacing(6);
    auto* header_layout = header_->SetLayoutManager(std::move(layout));

    actions::ActionItem* action =
        SidePanelHelper::GetActionItem(browser_, entry->key());
    auto* icon = header_->AddChildView(std::make_unique<views::ImageView>());
    if (action && !action->GetImage().IsEmpty()) {
      icon->SetImage(action->GetImage());
    } else {
      icon->SetVisible(false);
    }
    auto* title = header_->AddChildView(std::make_unique<views::Label>(
        action ? std::u16string(action->GetText()) : u"Side panel"));
    title->SetHorizontalAlignment(gfx::ALIGN_LEFT);
    title->SetEnabledColor(kColorSidePanelEntryTitle);
    header_layout->SetFlexForView(title, 1);

    auto pin = views::CreateVectorToggleImageButton(base::BindRepeating(
        &CmuxSidePanelHostView::TogglePinFromHeader,
        base::Unretained(this)));
    pin_button_ = pin.get();
    ConfigureSidePanelControlButton(pin_button_);
    pin_button_->SetTooltipText(
        l10n_util::GetStringUTF16(IDS_SIDE_PANEL_HEADER_PIN_BUTTON_TOOLTIP));
    pin_button_->SetToggledTooltipText(
        l10n_util::GetStringUTF16(IDS_SIDE_PANEL_HEADER_UNPIN_BUTTON_TOOLTIP));
    const int dip_size = ChromeLayoutProvider::Get()->GetDistanceMetric(
        ChromeDistanceMetric::DISTANCE_SIDE_PANEL_HEADER_VECTOR_ICON_SIZE);
    const gfx::VectorIcon& pin_icon =
        features::IsRoundedIconsEnabled() ? kKeepIcon : kKeepOldIcon;
    views::SetImageFromVectorIconWithColor(
        pin_button_, pin_icon,
        {kColorSidePanelHeaderButtonIcon,
         kColorSidePanelHeaderButtonIconDisabled},
        dip_size);
    const gfx::VectorIcon& unpin_icon =
        features::IsRoundedIconsEnabled() ? kKeepOffIcon : kKeepOffOldIcon;
    views::SetToggledImageFromVectorIconWithColor(
        pin_button_, unpin_icon, dip_size,
        {kColorSidePanelHeaderButtonIcon,
         kColorSidePanelHeaderButtonIconDisabled});
    pin_button_->SetFocusBehavior(views::View::FocusBehavior::ALWAYS);
    pin_button_->GetViewAccessibility().SetDescription(
        std::u16string(),
        ax::mojom::DescriptionFrom::kAttributeExplicitlyEmpty);
    header_->AddChildView(std::move(pin));
    UpdatePinButton();

    auto* close = header_->AddChildView(std::make_unique<ToolbarButton>(
        base::BindRepeating(&CmuxSidePanelHostView::CloseFromHeader,
                            base::Unretained(this))));
#if CHROME_VERSION_MAJOR >= 151
    close->SetVectorIcon(kCloseChromeRefreshOldIcon);
#elif CHROME_VERSION_MAJOR >= 150
    close->SetVectorIcon(vector_icons::kCloseChromeRefreshOldIcon);
#else
    close->SetVectorIcon(vector_icons::kCloseChromeRefreshIcon);
#endif
    close->SetTooltipText(u"Close side panel");
    close->GetViewAccessibility().SetName(u"Close side panel");
    body_layout_->SetFlexForView(header_, 0);
  }

  void RemoveHeader() {
    if (!header_) {
      return;
    }
    body_->RemoveChildViewT(header_);
    header_ = nullptr;
    pin_button_ = nullptr;
  }

  void RemoveHostedViews() {
    if (!content_->children().empty()) {
      content_->RemoveChildViewT(content_->children().front());
    }
    RemoveHeader();
    current_entry_.reset();
    current_is_contextual_ = false;
  }

  int MaximumPanelWidth() const {
    const int available = parent() ? parent()->width() : 0;
    return std::max(kMinimumPanelWidth, available - kMinimumPageWidth);
  }

  void SetPanelWidth(int width) {
    int maximum_width = std::max(kMinimumPanelWidth, MaximumPanelWidth());
    // A newly-created tab can synchronize an already-open global extension
    // panel before its BrowserPane receives real bounds. Do not permanently
    // clamp the requested/saved width to the minimum just because the parent
    // is transiently zero-sized during construction.
    if (!parent() || parent()->width() <= kMinimumPageWidth) {
      maximum_width = std::max(maximum_width, width);
    }
    preferred_width_ =
        std::clamp(width, kMinimumPanelWidth, maximum_width);
    SetPreferredSize(gfx::Size(preferred_width_, 1));
    if (parent()) {
      parent()->InvalidateLayout();
    }
  }

  void UpdateWidthFromPrefs() {
    SidePanelEntry* entry = current_entry_.get();
    if (!entry) {
      return;
    }
    const base::DictValue& widths = browser_->profile()->GetPrefs()->GetDict(
        prefs::kSidePanelIdToWidth);
    const std::string id = SidePanelEntryIdToString(entry->key().id());
    SetPanelWidth(widths.FindInt(id).value_or(entry->GetDefaultContentWidth() +
                                              kResizeAreaMargin +
                                              kContentOutlineWidth));
  }

  void PersistWidth() {
    SidePanelEntry* entry = current_entry_.get();
    if (!entry) {
      return;
    }
    ScopedDictPrefUpdate update(browser_->profile()->GetPrefs(),
                                prefs::kSidePanelIdToWidth);
    update->Set(SidePanelEntryIdToString(entry->key().id()), preferred_width_);
  }

  void TogglePinFromHeader() {
    if (!current_entry_) {
      return;
    }
    const std::optional<extensions::ExtensionId> extension_id =
        current_entry_->key().extension_id();
    if (!extension_id) {
      return;
    }
    const bool pin =
        !IsExtensionPinnedInPane(browser_, pane_, *extension_id);
    SetExtensionPinnedInPane(browser_, pane_, *extension_id, pin);
    GetViewAccessibility().AnnounceText(l10n_util::GetStringUTF16(
        pin ? IDS_SIDE_PANEL_PINNED : IDS_SIDE_PANEL_UNPINNED));
  }

  void UpdatePinButton() {
    if (!pin_button_ || !current_entry_) {
      return;
    }
    const std::optional<extensions::ExtensionId> extension_id =
        current_entry_->key().extension_id();
    actions::ActionItem* action =
        SidePanelHelper::GetActionItem(browser_, current_entry_->key());
    const bool pinnable =
        extension_id && action &&
        action->GetProperty(actions::kActionItemPinnableKey) ==
            static_cast<int>(actions::ActionPinnableState::kPinnable) &&
        !browser_->profile()->IsIncognitoProfile() &&
        !browser_->profile()->IsGuestSession();
    pin_button_->SetVisible(pinnable);
    pin_button_->SetToggled(
        pinnable &&
        IsExtensionPinnedInPane(browser_, pane_, *extension_id));
  }

  void AnnounceResize() {
    const float total_width = parent() ? parent()->width() : width();
    if (total_width <= 0) {
      return;
    }
    const float panel_width = width();
    const float web_contents_width = std::max(0.0f, total_width - panel_width);
    int panel_percentage =
        static_cast<int>((panel_width / total_width) * 100);
    const int web_contents_percentage =
        static_cast<int>((web_contents_width / total_width) * 100);
    if (panel_percentage + web_contents_percentage > 100) {
      --panel_percentage;
    }
    const bool right = IsRightAligned();
    const std::u16string web_contents_side_text =
        l10n_util::GetStringUTF16(
            right ? IDS_SIDE_PANEL_RESIZE_LEFT_SIDE_ACCESSIBLE_ALERT
                  : IDS_SIDE_PANEL_RESIZE_RIGHT_SIDE_ACCESSIBLE_ALERT);
    const std::u16string panel_side_text =
        l10n_util::GetStringUTF16(
            right ? IDS_SIDE_PANEL_RESIZE_RIGHT_SIDE_ACCESSIBLE_ALERT
                  : IDS_SIDE_PANEL_RESIZE_LEFT_SIDE_ACCESSIBLE_ALERT);
    GetViewAccessibility().AnnounceText(l10n_util::GetStringFUTF16(
        IDS_SIDE_PANEL_RESIZE_ACCESSIBLE_ALERT, web_contents_side_text,
        base::FormatPercent(web_contents_percentage), panel_side_text,
        base::FormatPercent(panel_percentage)));
  }

  raw_ptr<Browser> browser_;
  raw_ptr<content::WebContents> web_contents_;
  PaneId pane_ = kInvalidId;
  raw_ptr<CmuxSidePanelResizeArea> resize_area_ = nullptr;
  raw_ptr<views::View> body_ = nullptr;
  raw_ptr<views::BoxLayout> body_layout_ = nullptr;
  raw_ptr<views::View> header_ = nullptr;
  raw_ptr<views::ToggleImageButton> pin_button_ = nullptr;
  raw_ptr<views::View> content_ = nullptr;
  base::WeakPtr<SidePanelEntry> current_entry_;
  bool current_is_contextual_ = false;
  int preferred_width_ = SidePanelEntry::kSidePanelDefaultContentWidth +
                         kResizeAreaMargin + kContentOutlineWidth;
  int starting_width_ = -1;
  bool did_resize_ = false;
  bool keyboard_resized_ = false;
  PrefChangeRegistrar pref_registrar_;
  base::CallbackListSubscription pane_pin_subscription_;
  base::WeakPtrFactory<CmuxSidePanelHostView> weak_factory_{this};
};

class CmuxSidePanelUI final : public SidePanelUI {
 public:
  explicit CmuxSidePanelUI(Browser* browser)
#if CHROME_VERSION_MAJOR >= 151
      : browser_(browser),
        scoped_unowned_user_data_(browser->GetUnownedUserDataHost(), *this)
#else
      : browser_(browser)
#endif
  {
    CHECK(browser_);
    CHECK(!Coordinators().contains(browser_));
    Coordinators()[browser_] = this;
    SidePanelHelper::PopulateGlobalEntries(
        browser_, SidePanelRegistry::From(browser_));
  }

  ~CmuxSidePanelUI() override {
    for (auto& [_, host] : hosts_) {
      host->RemoveHostedViews();
    }
    Coordinators().erase(browser_);
  }

  void RegisterHost(CmuxSidePanelHostView* host) {
    CHECK(host);
    hosts_[host->web_contents()] = host;
    Sync(host->web_contents());
  }

  void UnregisterHost(CmuxSidePanelHostView* host) {
    if (!host) {
      return;
    }
    const PaneId pane = host->pane();
    auto it = hosts_.find(host->web_contents());
    if (it != hosts_.end() && it->second == host) {
      hosts_.erase(it);
    }
    if (pane == global_owner_pane_ &&
        std::ranges::none_of(hosts_, [pane](const auto& item) {
          return item.second->pane() == pane;
        })) {
      global_owner_pane_ = kInvalidId;
#if CHROME_VERSION_MAJOR >= 151
      SidePanelRegistry::From(browser_)->ResetActiveEntry();
#else
      for (SidePanelEntry::PanelType type : SidePanelEntry::PanelTypes::All()) {
        SidePanelRegistry::From(browser_)->ResetActiveEntryFor(type);
      }
#endif
    }
  }

  bool OpenContextual(content::WebContents* web_contents,
                      const SidePanelEntry::Key& key,
                      SidePanelOpenTrigger trigger) {
    SidePanelRegistry* registry = RegistryFor(web_contents);
    SidePanelEntry* entry = registry ? registry->GetEntryForKey(key) : nullptr;
    CmuxSidePanelHostView* host = HostFor(web_contents);
    if (!entry || !host) {
      return false;
    }
    registry->SetActiveEntry(entry);
    host->ShowEntry(entry, /*contextual=*/true, trigger);
    shown_callbacks_.Notify();
    return true;
  }

  bool CloseContextual(content::WebContents* web_contents,
                       const SidePanelEntry::Key& key) {
    CmuxSidePanelHostView* host = HostFor(web_contents);
    if (!host || !host->IsShowing(key, /*for_tab=*/true)) {
      return false;
    }
    host->CloseEntry(SidePanelEntryHideReason::kSidePanelClosed,
                     /*reset_registry=*/true);
    return true;
  }

  bool CloseGlobalFromHost(CmuxSidePanelHostView* host) {
    if (!host || !host->current_entry() || host->current_is_contextual()) {
      return false;
    }
    CloseHost(host, SidePanelEntryHideReason::kSidePanelClosed,
              /*reset_registry=*/true);
    return true;
  }

  void Sync(content::WebContents* web_contents) {
    CmuxSidePanelHostView* host = HostFor(web_contents);
    SidePanelRegistry* contextual = RegistryFor(web_contents);
    if (!host || !contextual) {
      return;
    }
#if CHROME_VERSION_MAJOR >= 151
    if (auto active = contextual->GetActiveEntry()) {
      host->ShowEntry(*active, /*contextual=*/true,
                      SidePanelOpenTrigger::kTabChanged);
      return;
    }
    if (browser_->GetActiveTabInterface() &&
        browser_->GetActiveTabInterface()->GetContents() == web_contents) {
      if (auto global = SidePanelRegistry::From(browser_)->GetActiveEntry()) {
        if (global_owner_pane_ == kInvalidId) {
          global_owner_pane_ = host->pane();
        }
        if (host->pane() == global_owner_pane_) {
          MoveGlobalEntryTo(host, *global,
                            SidePanelOpenTrigger::kTabChanged);
        }
      }
    }
#else
    for (SidePanelEntry::PanelType type : SidePanelEntry::PanelTypes::All()) {
      if (auto active = contextual->GetActiveEntryFor(type)) {
        host->ShowEntry(*active, /*contextual=*/true,
                        SidePanelOpenTrigger::kTabChanged);
        continue;
      }
      if (browser_->GetActiveTabInterface() &&
          browser_->GetActiveTabInterface()->GetContents() == web_contents) {
        if (auto global =
                SidePanelRegistry::From(browser_)->GetActiveEntryFor(type)) {
          if (global_owner_pane_ == kInvalidId) {
            global_owner_pane_ = host->pane();
          }
          if (host->pane() == global_owner_pane_) {
            MoveGlobalEntryTo(host, *global,
                              SidePanelOpenTrigger::kTabChanged);
          }
        }
      }
    }
#endif
  }

  using SidePanelUI::Close;
  using SidePanelUI::Show;

  void Show(SidePanelEntry::Id entry_id,
            std::optional<SidePanelOpenTrigger> trigger,
            bool suppress_animations) override {
    Show(SidePanelEntry::Key(entry_id), trigger, suppress_animations);
  }

  void Show(SidePanelEntry::Key key,
            std::optional<SidePanelOpenTrigger> trigger,
            bool suppress_animations) override {
    content::WebContents* active = ActiveWebContents();
    if (!active) {
      return;
    }
    SidePanelRegistry* contextual = RegistryFor(active);
    if (SidePanelEntry* contextual_entry =
            contextual ? contextual->GetEntryForKey(key) : nullptr) {
      contextual->SetActiveEntry(contextual_entry);
      HostFor(active)->ShowEntry(contextual_entry, /*contextual=*/true,
                                 trigger);
    } else if (SidePanelEntry* global_entry =
                   SidePanelRegistry::From(browser_)->GetEntryForKey(key)) {
      CmuxSidePanelHostView* active_host = HostFor(active);
      if (!active_host) {
        return;
      }
      global_owner_pane_ = active_host->pane();
      SidePanelRegistry::From(browser_)->SetActiveEntry(global_entry);
      MoveGlobalEntryTo(active_host, global_entry, trigger);
    } else {
      return;
    }
    shown_callbacks_.Notify();
  }

  void ShowFrom(SidePanelEntry::Key key, gfx::Rect) override { Show(key); }

#if CHROME_VERSION_MAJOR >= 151
  void Close(SidePanelEntryHideReason reason,
             bool suppress_animations) override {
#else
  void Close(SidePanelEntry::PanelType type,
             SidePanelEntryHideReason reason,
             bool suppress_animations) override {
#endif
    if (pending_query_key_) {
      for (auto& [_, host] : hosts_) {
        if (host->IsShowing(*pending_query_key_, pending_query_for_tab_)) {
          CloseHost(host, reason, /*reset_registry=*/true);
        }
      }
      pending_query_key_.reset();
      return;
    }
    CmuxSidePanelHostView* host = CurrentHostForQuery();
    if (host && host->current_entry()
#if CHROME_VERSION_MAJOR < 151
        && host->current_entry()->type() == type
#endif
    ) {
      CloseHost(host, reason, /*reset_registry=*/true);
    }
  }

  void Toggle(SidePanelEntry::Key key,
              SidePanelOpenTrigger trigger) override {
    CmuxSidePanelHostView* host = HostFor(ActiveWebContents());
    if (host && host->current_entry() && host->current_entry()->key() == key) {
      CloseHost(host, SidePanelEntryHideReason::kSidePanelClosed,
                /*reset_registry=*/true);
    } else {
      Show(key, trigger, /*suppress_animations=*/false);
    }
  }

#if CHROME_VERSION_MAJOR >= 151
  std::optional<SidePanelEntry::Id> GetCurrentEntryId() const override {
#else
  std::optional<SidePanelEntry::Id> GetCurrentEntryId(
      SidePanelEntry::PanelType type) const override {
#endif
    CmuxSidePanelHostView* host = CurrentHostForQuery();
    if (!host || !host->current_entry()
#if CHROME_VERSION_MAJOR < 151
        || host->current_entry()->type() != type
#endif
    ) {
      return std::nullopt;
    }
    return host->current_entry()->key().id();
  }

#if CHROME_VERSION_MAJOR >= 151
  int GetCurrentEntryDefaultContentWidth() const override {
#else
  int GetCurrentEntryDefaultContentWidth(
      SidePanelEntry::PanelType type) const override {
#endif
    CmuxSidePanelHostView* host = CurrentHostForQuery();
    return host && host->current_entry() &&
#if CHROME_VERSION_MAJOR < 151
                   host->current_entry()->type() == type
#else
                   true
#endif
               ? host->current_entry()->GetDefaultContentWidth()
               : SidePanelEntry::kSidePanelDefaultContentWidth;
  }

#if CHROME_VERSION_MAJOR >= 151
  bool IsSidePanelShowing() const override {
    return std::ranges::any_of(hosts_, [](const auto& item) {
      return item.second->current_entry();
    });
  }
#else
  bool IsSidePanelShowing(SidePanelEntry::PanelType type) const override {
    return std::ranges::any_of(hosts_, [type](const auto& item) {
      return item.second->current_entry() &&
             item.second->current_entry()->type() == type;
    });
  }
#endif

  bool IsSidePanelEntryShowing(
      const SidePanelEntry::Key& key) const override {
    for (const auto& [_, host] : hosts_) {
      if (host->IsShowing(key, /*for_tab=*/false)) {
        pending_query_key_ = key;
        pending_query_for_tab_ = false;
        return true;
      }
    }
    return std::ranges::any_of(hosts_, [&key](const auto& item) {
      return item.second->current_entry() &&
             item.second->current_entry()->key() == key;
    });
  }

  bool IsSidePanelEntryShowing(const SidePanelEntry::Key& key,
                               bool for_tab) const override {
    const bool showing =
        std::ranges::any_of(hosts_, [&key, for_tab](const auto& item) {
          return item.second->IsShowing(key, for_tab);
        });
    if (showing) {
      pending_query_key_ = key;
      pending_query_for_tab_ = for_tab;
    }
    return showing;
  }

  base::CallbackListSubscription RegisterSidePanelShown(
#if CHROME_VERSION_MAJOR < 151
      SidePanelEntry::PanelType,
#endif
      ShownCallback callback) override {
    return shown_callbacks_.Add(std::move(callback));
  }

  void OnActiveTabChanged(content::WebContents* old_contents,
                          content::WebContents* new_contents,
                          bool tab_removed_for_deletion) override {
    CmuxSidePanelHostView* old_host = HostFor(old_contents);
    CmuxSidePanelHostView* new_host = HostFor(new_contents);
    if (old_host && old_host->current_entry() &&
        !old_host->current_is_contextual()) {
      // A cmux workspace maps to a Chrome window, but its side panel belongs
      // to the pane where it was opened. Only rehost the entry when changing
      // tabs inside that pane; merely focusing another pane must not move or
      // resize the panel.
      if (new_host && old_host->pane() == new_host->pane()) {
        CloseHost(old_host, SidePanelEntryHideReason::kBackgrounded,
                  /*reset_registry=*/false);
      }
    }
    Sync(new_contents);
  }

  content::WebContents* GetWebContentsForTest(
      SidePanelEntry::Id) override {
    return nullptr;
  }

  void DisableAnimationsForTesting() override {}
  void SetNoDelaysForTesting(bool) override {}

 private:
  content::WebContents* ActiveWebContents() const {
    tabs::TabInterface* tab = browser_->GetActiveTabInterface();
    return tab ? tab->GetContents() : nullptr;
  }

  CmuxSidePanelHostView* HostFor(content::WebContents* contents) const {
    auto it = contents ? hosts_.find(contents) : hosts_.end();
    return it == hosts_.end() ? nullptr : it->second.get();
  }

  CmuxSidePanelHostView* GlobalOwnerHost() const {
    if (global_owner_pane_ == kInvalidId) {
      return nullptr;
    }
    for (const auto& [_, host] : hosts_) {
      if (host->pane() == global_owner_pane_ && host->current_entry() &&
          !host->current_is_contextual()) {
        return host;
      }
    }
    return nullptr;
  }

  CmuxSidePanelHostView* CurrentHostForQuery() const {
    CmuxSidePanelHostView* active = HostFor(ActiveWebContents());
    return active && active->current_entry() ? active : GlobalOwnerHost();
  }

  void CloseHost(CmuxSidePanelHostView* host,
                 SidePanelEntryHideReason reason,
                 bool reset_registry) {
    if (!host) {
      return;
    }
    const bool closing_global =
        host->current_entry() && !host->current_is_contextual();
    host->CloseEntry(reason, reset_registry);
    if (closing_global && reset_registry) {
      global_owner_pane_ = kInvalidId;
    }
  }

  void MoveGlobalEntryTo(CmuxSidePanelHostView* destination,
                         SidePanelEntry* entry,
                         std::optional<SidePanelOpenTrigger> trigger) {
    if (!destination) {
      return;
    }
    for (auto& [_, host] : hosts_) {
      if (host != destination && host->current_entry() == entry) {
        host->CloseEntry(SidePanelEntryHideReason::kBackgrounded,
                         /*reset_registry=*/false);
      }
    }
    destination->ShowEntry(entry, /*contextual=*/false, trigger);
  }

  raw_ptr<Browser> browser_;
  std::map<content::WebContents*, raw_ptr<CmuxSidePanelHostView>> hosts_;
  PaneId global_owner_pane_ = kInvalidId;
  base::RepeatingCallbackList<void()> shown_callbacks_;
  mutable std::optional<SidePanelEntry::Key> pending_query_key_;
  mutable bool pending_query_for_tab_ = false;
#if CHROME_VERSION_MAJOR >= 151
  ui::ScopedUnownedUserData<SidePanelUI> scoped_unowned_user_data_;
#endif
};

CmuxSidePanelHostView::~CmuxSidePanelHostView() {
  CloseEntry(SidePanelEntryHideReason::kSidePanelClosed,
             /*reset_registry=*/false);
  UnregisterFromCoordinator();
}

void CmuxSidePanelHostView::RegisterWith(CmuxSidePanelUI* ui) {
  ui->RegisterHost(this);
}

void CmuxSidePanelHostView::UnregisterFromCoordinator() {
  if (CmuxSidePanelUI* ui = CoordinatorFor(browser_)) {
    ui->UnregisterHost(this);
  }
}

void CmuxSidePanelHostView::CloseFromHeader() {
  // ToolbarButton invokes its callback from OnMouseReleased(). Closing the
  // entry tears down this button's header, so defer destruction until the
  // current mouse event has returned to the Views dispatcher.
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE,
      base::BindOnce(&CmuxSidePanelHostView::CloseFromHeaderNow,
                     weak_factory_.GetWeakPtr()));
}

void CmuxSidePanelHostView::CloseFromHeaderNow() {
  if (CmuxSidePanelUI* ui = CoordinatorFor(browser_)) {
    SidePanelEntry* entry = current_entry_.get();
    if (entry && current_is_contextual_) {
      ui->CloseContextual(web_contents_, entry->key());
      return;
    }
    if (entry && ui->CloseGlobalFromHost(this)) {
      return;
    }
  }
  CloseEntry(SidePanelEntryHideReason::kSidePanelClosed,
             /*reset_registry=*/true);
}

BEGIN_METADATA(CmuxSidePanelHostView)
END_METADATA

}  // namespace

std::unique_ptr<SidePanelUI> CreateCmuxSidePanelUI(Browser* browser) {
  return std::make_unique<CmuxSidePanelUI>(browser);
}

views::View* AddCmuxSidePanelHost(views::View* parent,
                                  Browser* browser,
                                  content::WebContents* web_contents,
                                  PaneId pane) {
  CHECK(parent);
  return parent->AddChildView(
      std::make_unique<CmuxSidePanelHostView>(browser, web_contents, pane));
}

void SyncCmuxSidePanelForWebContents(Browser* browser,
                                     content::WebContents* web_contents) {
  if (CmuxSidePanelUI* ui = CoordinatorFor(browser)) {
    ui->Sync(web_contents);
  }
}

bool OpenCmuxContextualSidePanel(
    BrowserWindowInterface& browser_window,
    content::WebContents& web_contents,
    const extensions::ExtensionId& extension_id) {
  Browser* browser = browser_window.GetBrowserForMigrationOnly();
  CmuxSidePanelUI* ui = CoordinatorFor(browser);
  return ui && ui->OpenContextual(
                   &web_contents,
                   SidePanelEntry::Key(SidePanelEntry::Id::kExtension,
                                       extension_id),
                   SidePanelOpenTrigger::kExtension);
}

bool CloseCmuxContextualSidePanel(
    BrowserWindowInterface* browser_window,
    content::WebContents* web_contents,
    const extensions::ExtensionId& extension_id) {
  Browser* browser =
      browser_window ? browser_window->GetBrowserForMigrationOnly() : nullptr;
  CmuxSidePanelUI* ui = CoordinatorFor(browser);
  return ui && web_contents &&
         ui->CloseContextual(
             web_contents,
             SidePanelEntry::Key(SidePanelEntry::Id::kExtension,
                                 extension_id));
}

}  // namespace cmux
