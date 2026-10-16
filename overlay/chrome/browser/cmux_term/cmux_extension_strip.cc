// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "chrome/browser/cmux_term/cmux_extension_strip.h"

#include <algorithm>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "base/check_op.h"
#include "base/functional/bind.h"
#include "base/functional/callback.h"
#include "base/logging.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/no_destructor.h"
#include "base/notreached.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/single_thread_task_runner.h"
#include "base/time/time.h"
#include "chrome/browser/cmux_term/cmux_browser_finder.h"
#include "chrome/browser/cmux_term/cmux_extension_slots.h"
#include "chrome/browser/cmux_term/cmux_extensions.h"
#include "chrome/browser/cmux_term/cmux_extensions_container.h"
#include "chrome/browser/cmux_term/cmux_pane_extension_pins.h"
#include "chrome/browser/cmux_term/cmux_toolbar_menus.h"
#include "chrome/browser/cmux_term/cmux_views.h"
#include "chrome/browser/extensions/api/side_panel/side_panel_service.h"
#include "chrome/browser/extensions/context_menu_matcher.h"
#include "chrome/browser/extensions/extension_action_runner.h"
#include "chrome/browser/extensions/extension_context_menu_model.h"
#include "chrome/browser/extensions/extension_tab_util.h"
#include "chrome/browser/extensions/extension_view_host.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_window.h"
#include "chrome/browser/ui/chrome_pages.h"
#include "chrome/browser/ui/extensions/extension_action_delegate.h"
#include "chrome/browser/ui/extensions/accelerator_priority.h"
#include "chrome/browser/ui/extensions/extension_action_view_model.h"
#include "chrome/browser/ui/extensions/icon_with_badge_image_source.h"
#include "chrome/browser/ui/layout_constants.h"
#include "chrome/browser/ui/views/extensions/extension_view_views.h"
#include "chrome/browser/ui/views/extensions/extension_action_delegate_desktop.h"
#include "chrome/browser/ui/views/toolbar/toolbar_action_view.h"
#include "chrome/browser/ui/views/toolbar/toolbar_ink_drop_util.h"
#include "chrome/common/chrome_version.h"
#include "chrome/common/webui_url_constants.h"
#include "components/input/native_web_keyboard_event.h"
#include "components/prefs/pref_service.h"
#include "components/sessions/content/session_tab_helper.h"
#include "components/vector_icons/vector_icons.h"
#include "content/public/browser/eye_dropper.h"
#include "content/public/browser/keyboard_event_processing_result.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/page_navigator.h"
#include "content/public/browser/web_contents.h"
#include "extensions/browser/extension_action.h"
#include "extensions/browser/extension_action_manager.h"
#include "extensions/browser/extension_prefs.h"
#include "extensions/browser/extension_registry.h"
#include "extensions/browser/extension_registry_observer.h"
#include "extensions/browser/pref_names.h"
#include "extensions/common/extension.h"
#include "extensions/common/command.h"
#include "extensions/common/manifest_handlers/options_page_info.h"
#include "extensions/common/mojom/api_permission_id.mojom.h"
#include "extensions/common/mojom/view_type.mojom.h"
#include "extensions/common/permissions/permissions_data.h"
#include "net/base/url_util.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/base/accelerators/accelerator.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/base/dragdrop/drag_drop_types.h"
#include "ui/base/models/menu_model.h"
#include "ui/base/mojom/dialog_button.mojom.h"
#include "ui/color/color_provider_manager.h"
#include "ui/gfx/geometry/insets.h"
#include "ui/gfx/geometry/rounded_corners_f.h"
#include "ui/gfx/geometry/size.h"
#include "ui/gfx/image/image.h"
#include "ui/gfx/image/image_skia.h"
#include "ui/menus/simple_menu_model.h"
#include "ui/native_theme/native_theme.h"
#include "ui/views/accessibility/view_accessibility.h"
#include "ui/views/border.h"
#include "ui/views/bubble/bubble_dialog_delegate_view.h"
#include "ui/views/bubble/bubble_frame_view.h"
#include "ui/views/context_menu_controller.h"
#include "ui/views/controls/menu/menu_runner.h"
#include "ui/views/controls/menu/menu_types.h"
#include "ui/views/controls/native/native_view_host.h"
#include "ui/views/focus/focus_manager.h"
#include "ui/views/layout/box_layout.h"
#include "ui/views/layout/fill_layout.h"
#include "ui/views/style/platform_style.h"
#include "ui/views/view_class_properties.h"
#include "ui/views/widget/widget.h"
#include "url/gurl.h"

namespace cmux {
namespace {

// Match normal Chrome's ToolbarView::GetToolbarButtonSize(), including touch
// density (34 px normally, 48 px in touch mode). The 28 px fallback in
// ExtensionsToolbarDesktop is only for tests without BrowserView.
int ActionButtonSize() {
  return GetLayoutConstant(LayoutConstant::kToolbarButtonHeight);
}

// Chromium 151 retained the pre-Material-refresh extension glyph under an
// "Old" suffix. Keep the placeholder visually stable across the supported
// 149 and 151 source trees.
const gfx::VectorIcon& ExtensionPlaceholderIcon() {
#if CHROME_VERSION_MAJOR >= 151
  return vector_icons::kExtensionChromeRefreshOldIcon;
#else
  return vector_icons::kExtensionChromeRefreshIcon;
#endif
}

struct PinnedIdsForSlots {
  std::vector<std::string> ids;
  bool use_default_ids_if_empty = false;
};

std::vector<std::string> DefaultPinnedExtensionIdsForSlots() {
  return {kUBlockOriginExtensionId, kBitwardenExtensionId};
}

std::vector<std::string> SelectSlotSourceIds(
    const PinnedIdsForSlots& pinned_ids,
    const std::vector<std::string>& default_ids) {
  if (pinned_ids.use_default_ids_if_empty && pinned_ids.ids.empty()) {
    return default_ids;
  }
  return pinned_ids.ids;
}

std::vector<std::string> UniqueNonEmptyIds(
    const std::vector<std::string>& ids) {
  std::vector<std::string> unique_ids;
  for (const std::string& id : ids) {
    if (!id.empty() && !CmuxExtensionActionIdsContain(unique_ids, id)) {
      unique_ids.push_back(id);
    }
  }
  return unique_ids;
}

PinnedIdsForSlots GetPinnedIdsForSlots(Profile* profile,
                                       ToolbarActionsModel* model) {
  PinnedIdsForSlots result;
  if (profile && profile->GetPrefs()) {
    if (extensions::ExtensionPrefs* extension_prefs =
            extensions::ExtensionPrefs::Get(profile)) {
      result.ids = extension_prefs->GetPinnedExtensions();
    }
    const PrefService::Preference* pref = profile->GetPrefs()->FindPreference(
        extensions::pref_names::kPinnedExtensions);
    result.use_default_ids_if_empty = pref && pref->IsDefaultValue();
    return result;
  }
  if (model) {
    result.ids.assign(model->pinned_action_ids().begin(),
                      model->pinned_action_ids().end());
  }
  return result;
}

// ---- Action button --------------------------------------------------------

// Chromium registers an extension-action shortcut once in each BrowserView's
// FocusManager. cmux deliberately embeds multiple Browser instances and
// multiple simultaneously visible panes in one physical Widget, so all of
// those action models share one FocusManager. This router preserves Chrome's
// one-handler-per-shortcut invariant and dispatches to the active workspace's
// native action model.
class CmuxExtensionAcceleratorRouter : public ui::AcceleratorTarget {
 public:
  static CmuxExtensionAcceleratorRouter* Get() {
    static base::NoDestructor<CmuxExtensionAcceleratorRouter> instance;
    return instance.get();
  }

  void RegisterAction(void* owner,
                      Browser* browser,
                      CmuxExtensionsContainer* container,
                      const std::string& action_id) {
    Unregister(owner);
    entries_.push_back(
        {owner, browser, container, nullptr, action_id, ui::Accelerator(),
         false});
  }

  void Register(void* owner,
                Browser* browser,
                CmuxExtensionsContainer* container,
                views::FocusManager* focus_manager,
                const std::string& action_id,
                const ui::Accelerator& accelerator) {
    Entry* existing = EntryForOwner(owner);
    if (!existing) {
      entries_.push_back({owner, browser, container, focus_manager, action_id,
                          accelerator, true});
    } else {
      existing->browser = browser;
      existing->container = container;
      existing->focus_manager = focus_manager;
      existing->action_id = action_id;
      existing->accelerator = accelerator;
      existing->has_accelerator = true;
    }
    RefreshFocusManagerRegistrations();
  }

  void ClearAccelerator(void* owner) {
    if (Entry* entry = EntryForOwner(owner)) {
      entry->focus_manager = nullptr;
      entry->has_accelerator = false;
      RefreshFocusManagerRegistrations();
    }
  }

  void Unregister(void* owner) {
    const auto old_size = entries_.size();
    std::erase_if(entries_,
                  [owner](const Entry& entry) { return entry.owner == owner; });
    if (entries_.size() != old_size) {
      RefreshFocusManagerRegistrations();
    }
  }

  bool AcceleratorPressed(const ui::Accelerator& accelerator) override {
    for (const Entry& entry : entries_) {
      if (!entry.has_accelerator || entry.accelerator != accelerator ||
          !entry.browser ||
          !entry.browser->window() || !entry.browser->window()->IsActive() ||
          !entry.container) {
        continue;
      }
      ToolbarActionViewModel* action =
          entry.container->GetActionForId(entry.action_id);
      if (action && action->CanHandleAccelerators()) {
        return action->TryHandleAcceleratorPress();
      }
    }
    return false;
  }

  bool Execute(Browser* browser, std::string_view action_id) {
    for (const Entry& entry : entries_) {
      if (entry.browser != browser || entry.action_id != action_id ||
          !entry.container) {
        continue;
      }
      ToolbarActionViewModel* action =
          entry.container->GetActionForId(entry.action_id);
      return action && action->CanHandleAccelerators() &&
             action->TryHandleAcceleratorPress();
    }
    return false;
  }

  bool CanHandleAccelerators() const override {
    for (const Entry& entry : entries_) {
      if (entry.browser && entry.browser->window() &&
          entry.browser->window()->IsActive()) {
        return true;
      }
    }
    return false;
  }

 private:
  friend class base::NoDestructor<CmuxExtensionAcceleratorRouter>;

  struct Entry {
    raw_ptr<void> owner;
    raw_ptr<Browser> browser;
    raw_ptr<CmuxExtensionsContainer> container;
    raw_ptr<views::FocusManager> focus_manager;
    std::string action_id;
    ui::Accelerator accelerator;
    bool has_accelerator;
  };

  struct FocusRegistration {
    raw_ptr<views::FocusManager> focus_manager;
    ui::Accelerator accelerator;
  };

  CmuxExtensionAcceleratorRouter() = default;

  Entry* EntryForOwner(void* owner) {
    auto it = std::ranges::find_if(
        entries_, [owner](const Entry& entry) { return entry.owner == owner; });
    return it == entries_.end() ? nullptr : &*it;
  }

  void RefreshFocusManagerRegistrations() {
    for (const FocusRegistration& registration : focus_registrations_) {
      registration.focus_manager->UnregisterAccelerator(
          registration.accelerator, this);
    }
    focus_registrations_.clear();
    for (const Entry& entry : entries_) {
      if (!entry.has_accelerator || !entry.focus_manager ||
          std::ranges::any_of(
              focus_registrations_,
              [&entry](const FocusRegistration& registration) {
                return registration.focus_manager == entry.focus_manager &&
                       registration.accelerator == entry.accelerator;
              })) {
        continue;
      }
      entry.focus_manager->RegisterAccelerator(
          entry.accelerator, kExtensionAcceleratorPriority, this);
      focus_registrations_.push_back(
          {entry.focus_manager, entry.accelerator});
    }
  }

  std::vector<Entry> entries_;
  std::vector<FocusRegistration> focus_registrations_;
};

class CmuxExtensionActionDelegateDesktop
    : public ExtensionActionDelegateDesktop {
 public:
  CmuxExtensionActionDelegateDesktop(
      const std::string& action_id,
      Browser* browser,
      CmuxExtensionsContainer* extensions_container)
      : ExtensionActionDelegateDesktop(browser,
                                       extensions_container,
                                       extensions_container),
        action_id_(action_id),
        browser_(browser),
        extensions_container_(extensions_container) {
    CmuxExtensionAcceleratorRouter::Get()->RegisterAction(
        this, browser_, extensions_container_, action_id_);
  }

  ~CmuxExtensionActionDelegateDesktop() override {
    CmuxExtensionAcceleratorRouter::Get()->Unregister(this);
  }

 private:
  void RegisterCommand() override {
    ToolbarActionViewModel* action =
        extensions_container_->GetActionForId(action_id_);
    auto* extension_action = static_cast<ExtensionActionViewModel*>(action);
    extensions::Command command;
    views::FocusManager* focus_manager =
        extensions_container_->GetFocusManagerForAccelerator();
    if (extension_action && focus_manager &&
        extension_action->GetExtensionCommand(&command)) {
      CmuxExtensionAcceleratorRouter::Get()->Register(
          this, browser_, extensions_container_, focus_manager, action_id_,
          command.accelerator());
    }
  }

  void UnregisterCommand() override {
    CmuxExtensionAcceleratorRouter::Get()->ClearAccelerator(this);
  }

  std::string action_id_;
  raw_ptr<Browser> browser_;
  raw_ptr<CmuxExtensionsContainer> extensions_container_;
};


class CmuxExtensionPlaceholderButton : public views::ImageButton {
  METADATA_HEADER(CmuxExtensionPlaceholderButton, views::ImageButton)

 public:
  explicit CmuxExtensionPlaceholderButton(std::string extension_id)
      : extension_id_(std::move(extension_id)) {
    SetImageHorizontalAlignment(views::ImageButton::ALIGN_CENTER);
    SetImageVerticalAlignment(views::ImageButton::ALIGN_MIDDLE);
    SetPreferredSize(gfx::Size(ActionButtonSize(), ActionButtonSize()));
    SetFocusBehavior(views::View::FocusBehavior::NEVER);
    SetEnabled(false);
    const ui::ImageModel icon = ui::ImageModel::FromVectorIcon(
        ExtensionPlaceholderIcon(), kColorToolbarButtonIconDisabled,
        GetLayoutConstant(LayoutConstant::kToolbarButtonIconSize));
    SetImageModel(views::Button::STATE_NORMAL, icon);
    SetImageModel(views::Button::STATE_DISABLED, icon);
    GetViewAccessibility().SetName(u"Extension loading");
  }

  CmuxExtensionPlaceholderButton(const CmuxExtensionPlaceholderButton&) =
      delete;
  CmuxExtensionPlaceholderButton& operator=(
      const CmuxExtensionPlaceholderButton&) = delete;
  ~CmuxExtensionPlaceholderButton() override = default;

  const std::string& extension_id() const { return extension_id_; }

 private:
  std::string extension_id_;
};

BEGIN_METADATA(CmuxExtensionPlaceholderButton)
END_METADATA

}  // namespace

bool ExecuteCmuxExtensionAction(Browser* browser,
                                std::string_view extension_id) {
  return CmuxExtensionAcceleratorRouter::Get()->Execute(browser, extension_id);
}

// ---- Strip ------------------------------------------------------------------

CmuxExtensionStrip::CmuxExtensionStrip(content::WebContents* web_contents,
                                       PaneId pane)
    : ToolbarIconContainerView(/*uses_highlight=*/true),
      content::WebContentsObserver(web_contents),
      profile_(Profile::FromBrowserContext(web_contents->GetBrowserContext())),
      browser_(cmux::FindBrowserWithTab(web_contents)),
      pane_(pane),
      extensions_container_(GetCmuxExtensionsContainerForBrowser(browser_)),
      creation_time_(base::TimeTicks::Now()) {
  CHECK(browser_);
  CHECK_NE(pane_, kInvalidId);
  EnsureDefaultPinnedExtensions(profile_);

  // Match ExtensionsToolbarDesktop: the puzzle button is the trailing main
  // item and the whole action group is one hover/focus-highlighted container.
  // ToolbarIconContainerView supplies Chrome's animated layout, spacing, and
  // one-pixel maximum-radius outline.
  SetNotifyEnterExitOnChild(true);
  std::unique_ptr<views::LabelButton> menu_button =
      CreateCmuxExtensionsMenuButton(web_contents);
  extensions_menu_button_ = menu_button.get();
  extensions_menu_button_->SetPreferredSize(
      gfx::Size(ActionButtonSize(), ActionButtonSize()));
  AddMainItem(menu_button.release());

  // ToolbarIconContainerView's generic default is 4 px of horizontal margin
  // around each child. Chrome's ExtensionsToolbarDesktop deliberately
  // overrides that to 2 px: collapsed adjacent margins then produce Chrome's
  // compact extension-action cadence without shrinking the 34 px button/ink
  // drop target. Keep real ToolbarActionViews and the trailing puzzle button on
  // that same geometry.
  GetTargetLayoutManager()->SetDefault(views::kMarginsKey,
                                       gfx::Insets::VH(0, 2));

  if (profile_ && profile_->GetPrefs()) {
    pref_change_registrar_.Init(profile_->GetPrefs());
    pref_change_registrar_.Add(
        extensions::pref_names::kPinnedExtensions,
        base::BindRepeating(&CmuxExtensionStrip::OnPinnedExtensionsPrefChanged,
                            base::Unretained(this)));
  }

  model_ = ToolbarActionsModel::Get(profile_);
  const PinnedIdsForSlots profile_pins =
      GetPinnedIdsForSlots(profile_, model_);
  InitializePaneExtensionPins(
      browser_, pane_,
      UniqueNonEmptyIds(
          SelectSlotSourceIds(profile_pins,
                              DefaultPinnedExtensionIdsForSlots())));
  pane_pin_subscription_ = AddPaneExtensionPinsChangedCallback(
      browser_, pane_,
      base::BindRepeating(&CmuxExtensionStrip::ScheduleRebuild,
                          base::Unretained(this)));
  if (model_) {
    model_observation_.Observe(model_);
  }
  if (extensions::ExtensionRegistry* registry =
          extensions::ExtensionRegistry::Get(profile_)) {
    registry_observation_.Observe(registry);
  }
  // Match upstream's observe-then-catch-up ordering so no action transition can
  // land between the initial snapshot and observer attachment.
  Rebuild();
}

CmuxExtensionStrip::~CmuxExtensionStrip() {
  for (const auto& view_model : action_view_models_) {
    if (extensions_container_ && view_model) {
      extensions_container_->UnregisterActionView(view_model.get());
    }
  }
  buttons_.clear();
  extensions_menu_button_ = nullptr;
  // ToolbarActionView keeps a raw pointer to its view model and calls it from
  // its destructor, so destroy child buttons before their models.
  RemoveAllChildViews();
  action_view_models_.clear();
}

void CmuxExtensionStrip::OnToolbarActionAdded(
    const ToolbarActionsModel::ActionId& id) {
  ScheduleRebuild();
}

void CmuxExtensionStrip::OnToolbarActionRemoved(
    const ToolbarActionsModel::ActionId& id) {
  ScheduleRebuild();
}

void CmuxExtensionStrip::OnToolbarActionUpdated(
    const ToolbarActionsModel::ActionId& id) {
  // Badge text/color or icon changed (e.g. uBOL's per-tab blocked count).
  if (ToolbarActionView* button = ButtonForExtension(id)) {
    button->UpdateState();
  }
}

void CmuxExtensionStrip::OnToolbarModelInitialized() {
  ScheduleRebuild();
}

void CmuxExtensionStrip::OnToolbarPinnedActionsChanged() {
  ScheduleRebuild();
}

void CmuxExtensionStrip::OnExtensionLoaded(content::BrowserContext*,
                                           const extensions::Extension*) {
  ScheduleRebuild();
}

void CmuxExtensionStrip::OnExtensionUnloaded(
    content::BrowserContext*,
    const extensions::Extension* extension,
    extensions::UnloadedExtensionReason) {
  ScheduleRebuild();
}

void CmuxExtensionStrip::OnExtensionInstalled(
    content::BrowserContext*,
    const extensions::Extension* extension,
    bool is_update) {
  ScheduleRebuild();
}

void CmuxExtensionStrip::OnExtensionUninstalled(
    content::BrowserContext*,
    const extensions::Extension* extension,
    extensions::UninstallReason) {
  ScheduleRebuild();
}

void CmuxExtensionStrip::OnShutdown(extensions::ExtensionRegistry* registry) {
  registry_observation_.Reset();
}

void CmuxExtensionStrip::WebContentsDestroyed() {
  if (extensions_container_) {
    extensions_container_->HideActivePopup();
  }
}

void CmuxExtensionStrip::PrimaryPageChanged(content::Page& page) {
  // Per-tab badge state (e.g. DNR action counts) resets across navigations.
  RefreshIcons();
}

void CmuxExtensionStrip::VisibilityChanged(views::View* starting_from,
                                           bool is_visible) {
  // Our surface got hidden (tab switch / pane close): don't leave the popup
  // floating over whatever replaced us.
  if (!is_visible) {
    for (const auto& view_model : action_view_models_) {
      if (view_model) {
        view_model->HidePopup();
      }
    }
  }
}

void CmuxExtensionStrip::UpdateAllIcons() {
  RefreshIcons();
}

void CmuxExtensionStrip::OnBoundsChanged(const gfx::Rect& previous_bounds) {
  ToolbarIconContainerView::OnBoundsChanged(previous_bounds);

  // Chromium's ExtensionsToolbarDesktop is a direct child of the top-level
  // browser toolbar layer. ToolbarIconContainerView therefore positions its
  // separate highlight-border layer in widget coordinates. A cmux strip is
  // nested below the compositor layer for its pane; widget coordinates include
  // that pane's origin even though the border layer's parent already applies
  // it, translating the border twice. The border is the sole layer registered
  // below this view, and as a sibling of this view's layer it must use the same
  // parent-relative bounds.
  const std::vector<ui::Layer*> associated_layers =
      GetLayersInOrder(views::ViewLayer::kExclude);
  DCHECK_EQ(associated_layers.size(), 1u);
  if (associated_layers.empty() || !layer()) {
    return;
  }
  associated_layers.front()->SetBounds(layer()->bounds());
  associated_layers.front()->SchedulePaint(GetLocalBounds());
}

void CmuxExtensionStrip::Rebuild() {
  rebuild_pending_ = false;
  if (!model_ || !web_contents()) {
    SetVisible(false);
    return;
  }
  extensions::ExtensionRegistry* registry =
      extensions::ExtensionRegistry::Get(profile_);
  extensions::ExtensionActionManager* action_manager =
      extensions::ExtensionActionManager::Get(profile_);
  if (!registry || !action_manager) {
    SetVisible(false);
    return;
  }

  PinnedIdsForSlots pinned_ids;
  pinned_ids.ids = GetPaneExtensionPins(browser_, pane_);
  const std::vector<std::string> default_ids =
      DefaultPinnedExtensionIdsForSlots();
  const std::vector<std::string> slot_source_ids =
      UniqueNonEmptyIds(SelectSlotSourceIds(pinned_ids, default_ids));
  std::vector<std::string> real_action_ids;
  for (const std::string& id : model_->action_ids()) {
    const extensions::Extension* extension = registry->GetExtensionById(
        id, extensions::ExtensionRegistry::EVERYTHING);
    if (!extension) {
      continue;
    }
    extension = registry->enabled_extensions().GetByID(id);
    if (!extension) {
      continue;
    }
    extensions::ExtensionAction* action =
        action_manager->GetExtensionAction(*extension);
    if (!action) {
      continue;
    }
    real_action_ids.push_back(id);
  }
  const std::vector<std::string> visible_action_ids =
      BuildVisibleCmuxExtensionActionIds(slot_source_ids, real_action_ids);

  // Chromium's ExtensionsToolbarDesktop creates one persistent action view per
  // enabled extension, sets its initial visibility before insertion, and then
  // uses AnimatingLayoutManager::FadeIn/FadeOut for pin changes. Helium keeps
  // that implementation unchanged. Retaining the views here gives cmux the
  // same 250 ms EASE_IN_OUT trailing-edge slide and avoids tearing down an
  // action model while an install/open callback is still using it.
  std::vector<std::string> stale_action_ids;
  for (ToolbarActionView* button : buttons_) {
    if (!CmuxExtensionActionIdsContain(real_action_ids,
                                       button->view_model()->GetId())) {
      stale_action_ids.push_back(button->view_model()->GetId());
    }
  }
  for (const std::string& id : stale_action_ids) {
    ToolbarActionView* button = ButtonForExtension(id);
    auto model_it = std::find_if(
        action_view_models_.begin(), action_view_models_.end(),
        [&id](const std::unique_ptr<ExtensionActionViewModel>& view_model) {
          return view_model && view_model->GetId() == id;
        });
    if (model_it != action_view_models_.end() && extensions_container_) {
      extensions_container_->UnregisterActionView(model_it->get());
    }
    std::erase(buttons_, button);
    if (button) {
      RemoveChildViewT(button);
    }
    if (model_it != action_view_models_.end()) {
      action_view_models_.erase(model_it);
    }
  }

  const auto is_pinned = [&visible_action_ids](const std::string& id) {
    return CmuxExtensionActionIdsContain(visible_action_ids, id);
  };
  for (const std::string& id : real_action_ids) {
    if (ButtonForExtension(id) || !browser_ || !extensions_container_) {
      continue;
    }
    auto view_model = ExtensionActionViewModel::Create(
        id, browser_,
        std::make_unique<CmuxExtensionActionDelegateDesktop>(
            id, browser_, extensions_container_));
    if (!view_model) {
      continue;
    }
    ExtensionActionViewModel* view_model_ptr = view_model.get();
    auto button_owner =
        std::make_unique<ToolbarActionView>(view_model_ptr, this);
    ToolbarActionView* button = button_owner.get();
    // Chrome sets visibility before adding the child. This prevents a newly
    // discovered but unpinned action from briefly animating into the toolbar.
    button->SetVisible(is_pinned(id));
    extensions_container_->RegisterActionView(id, web_contents(),
                                              view_model_ptr, button);
    ObserveButton(button);
    AddChildViewAt(std::move(button_owner), children().size() - 1);
    action_view_models_.push_back(std::move(view_model));
    buttons_.push_back(button);
    LogActionButtonAddedOnce(id);
  }

  // Match Chrome's ordering: pinned actions first in pin order, followed by
  // hidden action views, with the extensions puzzle button always trailing.
  int child_index = 0;
  std::set<std::string> ordered_action_ids;
  for (const std::string& id : visible_action_ids) {
    if (ToolbarActionView* button = ButtonForExtension(id)) {
      ReorderChildView(button, child_index++);
      ordered_action_ids.insert(id);
    }
  }
  for (const std::string& id : real_action_ids) {
    if (ordered_action_ids.contains(id)) {
      continue;
    }
    if (ToolbarActionView* button = ButtonForExtension(id)) {
      ReorderChildView(button, child_index++);
    }
  }

  for (ToolbarActionView* button : buttons_) {
    if (is_pinned(button->view_model()->GetId())) {
      GetAnimatingLayoutManager()->FadeIn(button);
    } else {
      GetAnimatingLayoutManager()->FadeOut(button);
    }
  }
  // Match Chromium: missing/stale pinned ids do not create controls, and the
  // extensions container is visible only while a real action exists.
  SetVisible(!model_->action_ids().empty());
  InvalidateLayout();
}

void CmuxExtensionStrip::ScheduleRebuild() {
  if (rebuild_pending_) {
    return;
  }
  rebuild_pending_ = true;
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE,
      base::BindOnce(&CmuxExtensionStrip::Rebuild, weak_factory_.GetWeakPtr()));
}

void CmuxExtensionStrip::RefreshIcons() {
  for (ToolbarActionView* button : buttons_) {
    button->UpdateState();
  }
}

ToolbarActionView* CmuxExtensionStrip::ButtonForExtension(
    const std::string& id) {
  for (ToolbarActionView* button : buttons_) {
    if (button->view_model()->GetId() == id) {
      return button;
    }
  }
  return nullptr;
}

content::WebContents* CmuxExtensionStrip::GetCurrentWebContents() {
  return web_contents();
}

views::LabelButton* CmuxExtensionStrip::GetOverflowReferenceView() const {
  return extensions_menu_button_;
}

gfx::Size CmuxExtensionStrip::GetToolbarActionSize() {
  return gfx::Size(ActionButtonSize(), ActionButtonSize());
}

void CmuxExtensionStrip::MovePinnedActionBy(const std::string& action_id,
                                            int move_by) {
  if (!model_ || move_by == 0) {
    return;
  }
  const auto& ids = model_->pinned_action_ids();
  auto it = std::find(ids.begin(), ids.end(), action_id);
  if (it == ids.end()) {
    return;
  }
  const int current = static_cast<int>(it - ids.begin());
  const int target =
      std::clamp(current + move_by, 0, static_cast<int>(ids.size()) - 1);
  if (target != current) {
    model_->MovePinnedAction(action_id, static_cast<size_t>(target));
  }
}

void CmuxExtensionStrip::UpdateHoverCard(
    ToolbarActionView*,
    ToolbarActionHoverCardUpdateType) {}

bool CmuxExtensionStrip::IsFocusOnExtensionAction() const {
  const views::FocusManager* focus_manager = GetFocusManager();
  const views::View* focused_view =
      focus_manager ? focus_manager->GetFocusedView() : nullptr;
  for (ToolbarActionView* button : buttons_) {
    if (button == focused_view) {
      return true;
    }
  }
  return false;
}

void CmuxExtensionStrip::OnContextMenuShown(const std::string&) {}
void CmuxExtensionStrip::OnContextMenuClosed(const std::string&) {}

void CmuxExtensionStrip::WriteDragDataForView(views::View*,
                                              const gfx::Point&,
                                              ui::OSExchangeData*) {}

int CmuxExtensionStrip::GetDragOperationsForView(views::View*,
                                                 const gfx::Point&) {
  return ui::DragDropTypes::DRAG_NONE;
}

bool CmuxExtensionStrip::CanStartDragForView(views::View*,
                                             const gfx::Point&,
                                             const gfx::Point&) {
  return false;
}

void CmuxExtensionStrip::OnPinnedExtensionsPrefChanged() {
  ScheduleRebuild();
}

void CmuxExtensionStrip::LogActionButtonAddedOnce(
    const std::string& extension_id) {
  if (!logged_action_button_ids_.insert(extension_id).second) {
    return;
  }
  LOG(WARNING) << "cmux-ext: action button added id=" << extension_id << " t="
               << (base::TimeTicks::Now() - creation_time_).InMilliseconds()
               << "ms since strip creation";
}

BEGIN_METADATA(CmuxExtensionStrip)
END_METADATA

}  // namespace cmux
