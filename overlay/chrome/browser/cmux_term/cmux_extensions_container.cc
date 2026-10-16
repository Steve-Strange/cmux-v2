// Copyright 2025 The Chromium Authors
// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: BSD-3-Clause
//
// Derived from Chromium; see third_party/chromium/LICENSE.

#include "chrome/browser/cmux_term/cmux_extensions_container.h"

#include <memory>
#include <map>
#include <utility>

#include "base/check.h"
#include "base/feature_list.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/memory/raw_ptr.h"
#include "base/no_destructor.h"
#include "chrome/browser/cmux_term/cmux_browser_finder.h"
#include "chrome/browser/cmux_term/cmux_views.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/extensions/extension_action_view_model.h"
#include "chrome/browser/ui/extensions/extension_post_install_dialog.h"
#include "chrome/browser/ui/toolbar/toolbar_actions_model.h"
#include "chrome/browser/ui/views/extensions/extensions_menu_delegate_desktop.h"
#include "chrome/browser/ui/views/extensions/extensions_menu_coordinator.h"
#include "chrome/browser/ui/views/extensions/extensions_menu_entry_view.h"
#include "chrome/browser/ui/views/extensions/extensions_menu_view.h"
#include "chrome/browser/ui/views/toolbar/toolbar_action_view.h"
#include "chrome/common/chrome_version.h"
#include "content/public/browser/web_contents.h"
#include "extensions/browser/extension_action_manager.h"
#include "extensions/browser/extension_registry.h"
#include "extensions/common/extension.h"
#include "extensions/common/extension_features.h"
#include "ui/base/base_window.h"
#include "ui/base/models/dialog_model.h"
#include "ui/compositor/layer.h"
#include "ui/events/event.h"
#include "ui/views/bubble/bubble_dialog_delegate_view.h"
#include "ui/views/bubble/bubble_dialog_model_host.h"
#include "ui/views/focus/focus_manager.h"
#include "ui/views/view.h"
#include "ui/views/widget/widget.h"

namespace cmux {
namespace {

using ContainerMap =
    std::map<raw_ptr<Browser>, std::unique_ptr<CmuxExtensionsContainer>>;

ContainerMap& ContainerInstances() {
  static base::NoDestructor<ContainerMap> instances;
  return *instances;
}

}  // namespace

CmuxExtensionsContainer::ActionViewRegistration::ActionViewRegistration(
    std::string action_id,
    base::WeakPtr<content::WebContents> web_contents,
    ToolbarActionViewModel* model,
    ToolbarActionView* anchor)
    : action_id(std::move(action_id)),
      web_contents(std::move(web_contents)),
      model(model),
      anchor(anchor) {}

CmuxExtensionsContainer::ActionViewRegistration::~ActionViewRegistration() =
    default;
CmuxExtensionsContainer::ActionViewRegistration::ActionViewRegistration(
    ActionViewRegistration&&) = default;
CmuxExtensionsContainer::ActionViewRegistration&
CmuxExtensionsContainer::ActionViewRegistration::operator=(
    ActionViewRegistration&&) = default;

CmuxExtensionsContainer::CmuxExtensionsContainer(Profile* profile,
                                                 Browser* browser)
    : profile_(profile), browser_(browser) {
  CHECK(profile_);
  CHECK(browser_);
  scoped_extensions_container_ =
      std::make_unique<ui::ScopedUnownedUserData<ExtensionsContainer>>(
          browser_->GetUnownedUserDataHost(), *this);
  extensions_menu_coordinator_ =
      std::make_unique<ExtensionsMenuCoordinator>(browser_, this);
}

CmuxExtensionsContainer::~CmuxExtensionsContainer() {
  CloseUiForWindowTeardown();
}

void CmuxExtensionsContainer::SetMenuButtonAnchor(views::View* anchor) {
  menu_anchor_.SetView(anchor);
}

void CmuxExtensionsContainer::RegisterActionAnchor(const std::string& action_id,
                                                   views::View* anchor) {
  if (anchor) {
    action_anchors_[action_id] = anchor;
  }
}

void CmuxExtensionsContainer::UnregisterActionAnchor(
    const std::string& action_id,
    views::View* anchor) {
  auto it = action_anchors_.find(action_id);
  if (it != action_anchors_.end() && it->second == anchor) {
    action_anchors_.erase(it);
  }
}

void CmuxExtensionsContainer::RegisterActionView(
    const std::string& action_id,
    content::WebContents* web_contents,
    ToolbarActionViewModel* model,
    ToolbarActionView* anchor) {
  if (!web_contents || !model || !anchor) {
    return;
  }
  action_views_.push_back({action_id, web_contents->GetWeakPtr(), model,
                           anchor});
}

void CmuxExtensionsContainer::UnregisterActionView(
    ToolbarActionViewModel* model) {
  for (const ActionViewRegistration& registration : action_views_) {
    if (registration.model == model &&
        registration.anchor == popup_action_view_) {
      popup_action_view_ = nullptr;
      break;
    }
  }
  std::erase_if(action_views_,
                [model](const ActionViewRegistration& registration) {
                  return registration.model == model;
                });
}

bool CmuxExtensionsContainer::ActivateAction(const std::string& action_id) {
  ToolbarActionViewModel* action = GetActionForId(action_id);
  if (!action) {
    return false;
  }
  action->ExecuteUserAction(ToolbarActionViewModel::InvocationSource::kMenuEntry);
  return true;
}

void CmuxExtensionsContainer::CloseUiForWindowTeardown() {
  HideActivePopup();
  ClosePostInstallDialog();
  CloseExtensionsMenuIfOpen();
  popup_owner_ = nullptr;
  popup_action_view_ = nullptr;
  popped_out_action_.reset();
  action_anchors_.clear();
  action_views_.clear();
  post_install_dialog_anchor_.SetView(nullptr);
  menu_anchor_.SetView(nullptr);
}

bool CmuxExtensionsContainer::ShowMenuForSelfTest() {
  const bool access_control = base::FeatureList::IsEnabled(
      extensions_features::kExtensionsMenuAccessControl);
  if ((access_control && extensions_menu_coordinator_->IsShowing()) ||
      (!access_control && ExtensionsMenuView::IsShowing())) {
    return false;
  }
  views::View* anchor = ResolveMenuAnchor();
  if (!anchor) {
    return false;
  }
  views::Widget* widget = nullptr;
  if (access_control) {
    extensions_menu_coordinator_->Show(views::BubbleAnchor(anchor), this);
    widget = extensions_menu_coordinator_->GetExtensionsMenuWidget();
  } else {
    widget = ExtensionsMenuView::ShowBubble(anchor, browser_, this, this);
  }
  menu_widget_ = widget ? widget->GetWeakPtr() : nullptr;
  return widget && (access_control
                        ? extensions_menu_coordinator_->IsShowing()
                        : ExtensionsMenuView::IsShowing());
}

bool CmuxExtensionsContainer::ClickMenuButtonForSelfTest() {
  views::View* button = ResolveMenuAnchor();
  if (!button) {
    return false;
  }
  ui::MouseEvent press(ui::EventType::kMousePressed, gfx::Point(),
                       gfx::Point(), base::TimeTicks::Now(),
                       ui::EF_LEFT_MOUSE_BUTTON, 0);
  ui::MouseEvent release(ui::EventType::kMouseReleased, gfx::Point(),
                         gfx::Point(), base::TimeTicks::Now(),
                         ui::EF_LEFT_MOUSE_BUTTON, 0);
  button->OnMousePressed(press);
  button->OnMouseReleased(release);
  return true;
}

bool CmuxExtensionsContainer::ClickActionForSelfTest(
    const std::string& action_id) {
  views::View* action = ResolveActionAnchor(action_id);
  if (!action) {
    return false;
  }
  ui::MouseEvent press(ui::EventType::kMousePressed, gfx::Point(),
                       gfx::Point(), base::TimeTicks::Now(),
                       ui::EF_LEFT_MOUSE_BUTTON, 0);
  ui::MouseEvent release(ui::EventType::kMouseReleased, gfx::Point(),
                         gfx::Point(), base::TimeTicks::Now(),
                         ui::EF_LEFT_MOUSE_BUTTON, 0);
  // Dispatch through View's public virtual interface. The concrete anchor is
  // Chromium's ToolbarActionView, so this exercises its popup-hide and
  // suppress-next-release path exactly as a second left click does.
  action->OnMousePressed(press);
  action->OnMouseReleased(release);
  return true;
}

bool CmuxExtensionsContainer::IsActionContainerHighlightAlignedForTesting(
    const std::string& action_id) const {
  ToolbarActionView* action = ResolveActionView(action_id);
  views::View* container = action ? action->parent() : nullptr;
  if (!container || !container->layer()) {
    return false;
  }
  const std::vector<ui::Layer*> associated_layers =
      container->GetLayersInOrder(views::ViewLayer::kExclude);
  return associated_layers.size() == 1u &&
         associated_layers.front()->bounds() == container->layer()->bounds();
}

bool CmuxExtensionsContainer::IsMenuShowingForTesting() const {
  if (base::FeatureList::IsEnabled(
          extensions_features::kExtensionsMenuAccessControl)) {
    return extensions_menu_coordinator_->IsShowing();
  }
  return menu_widget_ && ExtensionsMenuView::IsShowing();
}

views::Widget* CmuxExtensionsContainer::GetMenuWidgetForTesting() {
  if (base::FeatureList::IsEnabled(
          extensions_features::kExtensionsMenuAccessControl)) {
    return extensions_menu_coordinator_->GetExtensionsMenuWidget();
  }
  return menu_widget_.get();
}

bool CmuxExtensionsContainer::ShowPostInstallDialog(
    Profile* dialog_profile,
    scoped_refptr<const extensions::Extension> extension,
    const SkBitmap& icon) {
  if (!dialog_profile || !extension) {
    return false;
  }
  content::WebContents* web_contents = GetActiveCmuxWebContents();
  if (!web_contents) {
    return false;
  }
  views::View* anchor = ResolvePuzzleAnchor();
  if (!anchor) {
    return false;
  }

  std::unique_ptr<ui::DialogModel> dialog_model =
      extensions::BuildExtensionPostInstallDialogModel(
          dialog_profile, web_contents, std::move(extension), icon);
  if (!dialog_model) {
    return false;
  }

  ClosePostInstallDialog();
  auto bubble = std::make_unique<views::BubbleDialogModelHost>(
      std::move(dialog_model), views::BubbleAnchor(anchor),
      views::BubbleBorder::TOP_RIGHT);
#if CHROME_VERSION_MAJOR >= 151
  // M151's preferred CreateBubble() overload requires caller-owned delegate
  // and Widget lifetimes. This container intentionally keeps the historical
  // native-widget ownership used by M149, so use the explicit compatibility
  // API and transfer the model host to the Widget.
  views::Widget* widget =
      views::BubbleDialogDelegate::CreateBubbleDeprecated(
          std::move(bubble),
          views::Widget::InitParams::NATIVE_WIDGET_OWNS_WIDGET);
#else
  views::Widget* widget =
#if CHROME_VERSION_MAJOR >= 150
      views::BubbleDialogDelegate::CreateBubbleDeprecated(
          std::move(bubble),
          views::Widget::InitParams::NATIVE_WIDGET_OWNS_WIDGET);
#else
      views::BubbleDialogDelegate::CreateBubble(std::move(bubble));
#endif
#endif
  post_install_dialog_widget_ = widget;
  post_install_dialog_anchor_.SetView(anchor);
  post_install_dialog_observation_.Observe(widget);
  widget->Show();
  return true;
}

bool CmuxExtensionsContainer::IsPostInstallDialogShowingForTesting() const {
  return post_install_dialog_widget_ && post_install_dialog_anchor_.view() &&
         !post_install_dialog_widget_->IsClosed();
}

views::Widget* CmuxExtensionsContainer::GetPostInstallDialogWidgetForTesting() {
  return post_install_dialog_widget_;
}

ToolbarActionViewModel* CmuxExtensionsContainer::GetActionForId(
    const std::string& action_id) {
  content::WebContents* active = GetActiveCmuxWebContents();
  for (const ActionViewRegistration& registration : action_views_) {
    if (registration.action_id == action_id &&
        registration.web_contents.get() == active) {
      return registration.model;
    }
  }
  for (const ActionViewRegistration& registration : action_views_) {
    if (registration.action_id == action_id && registration.web_contents) {
      return registration.model;
    }
  }
  return nullptr;
}

void CmuxExtensionsContainer::HideActivePopup() {
  if (popup_owner_) {
    popup_owner_->HidePopup();
  }
  DCHECK(!popup_owner_);
}

void CmuxExtensionsContainer::CloseExtensionsMenuIfOpen() {
  if (base::FeatureList::IsEnabled(
          extensions_features::kExtensionsMenuAccessControl)) {
    if (extensions_menu_coordinator_->IsShowing()) {
      extensions_menu_coordinator_->Hide();
    }
  } else if (menu_widget_ && ExtensionsMenuView::IsShowing()) {
    ExtensionsMenuView::Hide();
  }
  menu_widget_.reset();
}

#if CHROME_VERSION_MAJOR < 151
bool CmuxExtensionsContainer::CloseOverflowMenuIfOpen() {
  const bool was_showing =
      base::FeatureList::IsEnabled(
          extensions_features::kExtensionsMenuAccessControl)
          ? extensions_menu_coordinator_->IsShowing()
          : menu_widget_ && ExtensionsMenuView::IsShowing();
  CloseExtensionsMenuIfOpen();
  return was_showing;
}
#endif

bool CmuxExtensionsContainer::ShowToolbarActionPopupForAPICall(
    const std::string& action_id,
    ShowPopupCallback callback) {
  ToolbarActionViewModel* action = GetActionForId(action_id);
  if (!action || !browser_->GetWindow()->IsActive()) {
    if (callback) {
      std::move(callback).Run(nullptr);
    }
    return false;
  }
  action->TriggerPopupForAPI(std::move(callback));
  return true;
}

void CmuxExtensionsContainer::ToggleExtensionsMenu() {
  HideActivePopup();
  const bool access_control = base::FeatureList::IsEnabled(
      extensions_features::kExtensionsMenuAccessControl);
  if ((access_control && extensions_menu_coordinator_->IsShowing()) ||
      (!access_control && ExtensionsMenuView::IsShowing())) {
    if (access_control) {
      extensions_menu_coordinator_->Hide();
    } else {
      ExtensionsMenuView::Hide();
    }
    menu_widget_.reset();
    return;
  }
  views::View* anchor = ResolveMenuAnchor();
  if (!anchor) {
    return;
  }
  views::Widget* widget = nullptr;
  if (access_control) {
    extensions_menu_coordinator_->Show(views::BubbleAnchor(anchor), this);
    widget = extensions_menu_coordinator_->GetExtensionsMenuWidget();
  } else {
    widget = ExtensionsMenuView::ShowBubble(anchor, browser_, this, this);
  }
  menu_widget_ = widget ? widget->GetWeakPtr() : nullptr;
}

bool CmuxExtensionsContainer::HasAnyExtensions() const {
  extensions::ExtensionRegistry* registry =
      extensions::ExtensionRegistry::Get(profile_);
  extensions::ExtensionActionManager* action_manager =
      extensions::ExtensionActionManager::Get(profile_);
  if (!registry || !action_manager) {
    return false;
  }
  for (const auto& extension : registry->enabled_extensions()) {
    if (action_manager->GetExtensionAction(*extension)) {
      return true;
    }
  }
  return false;
}

std::optional<extensions::ExtensionId>
CmuxExtensionsContainer::GetPoppedOutActionId() const {
  return popped_out_action_;
}

bool CmuxExtensionsContainer::IsActionVisibleOnToolbar(
    const std::string& action_id) const {
  return ResolveActionAnchor(action_id) != nullptr;
}

void CmuxExtensionsContainer::UndoPopOut() {
  popped_out_action_.reset();
}

void CmuxExtensionsContainer::SetPopupOwner(
    ToolbarActionViewModel* popup_owner) {
  DCHECK((popup_owner_ != nullptr) ^ (popup_owner != nullptr));
  popup_owner_ = popup_owner;
  if (popup_owner_) {
    popup_action_view_ = ResolveActionView(popup_owner_);
  }
}

void CmuxExtensionsContainer::PopOutAction(
    const extensions::ExtensionId& action_id,
    base::OnceClosure closure) {
  popped_out_action_ = action_id;
  if (closure) {
    std::move(closure).Run();
  }
}

void CmuxExtensionsContainer::CollapseConfirmation() {}

void CmuxExtensionsContainer::ShowContextMenuAsFallback(
    const extensions::ExtensionId& action_id) {}

void CmuxExtensionsContainer::OnPopupShown(
    const extensions::ExtensionId& action_id,
    bool by_user) {
  ToolbarActionView* action = popup_action_view_;
  if (!action && popup_owner_) {
    action = ResolveActionView(popup_owner_);
  }
  if (!action) {
    action = ResolveActionView(action_id);
  }
  if (action) {
    popup_action_view_ = action;
    action->OnPopupShown(by_user);
  }
}

void CmuxExtensionsContainer::OnPopupClosed(
    const extensions::ExtensionId& action_id) {
  ToolbarActionView* action = popup_action_view_;
  if (!action) {
    action = ResolveActionView(action_id);
  }
  if (action) {
    action->OnPopupClosed();
  }
  popup_action_view_ = nullptr;
  if (popped_out_action_ == action_id) {
    popped_out_action_.reset();
  }
}

views::FocusManager* CmuxExtensionsContainer::GetFocusManagerForAccelerator() {
  views::Widget* widget = GetCmuxViewsWidget();
  return widget ? widget->GetFocusManager() : nullptr;
}

views::BubbleAnchor CmuxExtensionsContainer::GetReferenceButtonForPopup(
    const extensions::ExtensionId& action_id) {
  if (views::View* anchor = ResolveActionAnchor(action_id)) {
    return views::BubbleAnchor(anchor);
  }
  return views::BubbleAnchor(ResolveMenuAnchor());
}

void CmuxExtensionsContainer::OnWidgetDestroying(views::Widget* widget) {
  if (widget == post_install_dialog_widget_) {
    post_install_dialog_observation_.Reset();
    post_install_dialog_widget_ = nullptr;
    post_install_dialog_anchor_.SetView(nullptr);
  }
}

views::View* CmuxExtensionsContainer::ResolvePuzzleAnchor() {
  views::View* anchor = menu_anchor_.view();
  if (!anchor || !anchor->GetWidget() || !anchor->GetVisible()) {
    return nullptr;
  }
  return anchor;
}

views::View* CmuxExtensionsContainer::ResolveMenuAnchor() {
  if (views::View* anchor = menu_anchor_.view();
      anchor && anchor->GetWidget() && anchor->GetVisible()) {
    return anchor;
  }
  views::Widget* widget = GetCmuxViewsWidget();
  return widget ? widget->GetContentsView() : nullptr;
}

views::View* CmuxExtensionsContainer::ResolveActionAnchor(
    const std::string& action_id) const {
  if (ToolbarActionView* action = ResolveActionView(action_id)) {
    return action;
  }
  auto it = action_anchors_.find(action_id);
  if (it == action_anchors_.end()) {
    return nullptr;
  }
  views::View* anchor = it->second;
  if (!anchor || !anchor->GetWidget() || !anchor->GetVisible()) {
    return nullptr;
  }
  return anchor;
}

ToolbarActionView* CmuxExtensionsContainer::ResolveActionView(
    const std::string& action_id) const {
  content::WebContents* active = GetActiveCmuxWebContents();
  for (const ActionViewRegistration& registration : action_views_) {
    ToolbarActionView* anchor = registration.anchor;
    if (registration.action_id == action_id &&
        registration.web_contents.get() == active && anchor &&
        anchor->GetWidget() && anchor->GetVisible()) {
      return anchor;
    }
  }
  for (const ActionViewRegistration& registration : action_views_) {
    ToolbarActionView* anchor = registration.anchor;
    if (registration.action_id == action_id && registration.web_contents &&
        anchor && anchor->GetWidget() && anchor->GetVisible()) {
      return anchor;
    }
  }
  return nullptr;
}

ToolbarActionView* CmuxExtensionsContainer::ResolveActionView(
    ToolbarActionViewModel* view_model) const {
  if (!view_model) {
    return nullptr;
  }
  for (const ActionViewRegistration& registration : action_views_) {
    ToolbarActionView* anchor = registration.anchor;
    if (registration.model == view_model && anchor && anchor->GetWidget() &&
        anchor->GetVisible()) {
      return anchor;
    }
  }
  return nullptr;
}

void CmuxExtensionsContainer::ClosePostInstallDialog() {
  views::Widget* widget = post_install_dialog_widget_;
  post_install_dialog_widget_ = nullptr;
  post_install_dialog_anchor_.SetView(nullptr);
  post_install_dialog_observation_.Reset();
  if (widget && !widget->IsClosed()) {
    widget->Close();
  }
}

void InstallCmuxExtensionsMenuOverrides() {
  // No embedder activation override is needed now that every workspace has a
  // real Browser. ExtensionsMenuDelegateDesktop uses its normal action model,
  // popup, permissions, and context-menu paths.
}

CmuxExtensionsContainer* GetCmuxExtensionsContainerForBrowser(
    Browser* browser) {
  if (!browser || !browser->profile() || browser->profile()->IsOffTheRecord()) {
    return nullptr;
  }
  ContainerMap& instances = ContainerInstances();
  auto [it, inserted] = instances.try_emplace(browser);
  if (inserted) {
    it->second =
        std::make_unique<CmuxExtensionsContainer>(browser->profile(), browser);
  }
  return it->second.get();
}

CmuxExtensionsContainer* GetCmuxExtensionsContainer(Profile* profile) {
  if (!profile) {
    return nullptr;
  }
  content::WebContents* contents = GetActiveCmuxWebContents();
  Browser* browser = contents ? cmux::FindBrowserWithTab(contents) : nullptr;
  if (!browser || browser->profile()->GetOriginalProfile() !=
                      profile->GetOriginalProfile()) {
    return nullptr;
  }
  return GetCmuxExtensionsContainerForBrowser(browser);
}

CmuxExtensionsContainer* GetCmuxExtensionsContainerForSelfTest(
    Profile* profile) {
  if (CmuxExtensionsContainer* active =
          GetCmuxExtensionsContainer(profile)) {
    return active;
  }
  if (!profile) {
    return nullptr;
  }
  Profile* original_profile = profile->GetOriginalProfile();
  for (auto& [browser, container] : ContainerInstances()) {
    if (browser && container &&
        browser->profile()->GetOriginalProfile() == original_profile) {
      return container.get();
    }
  }
  return nullptr;
}

void DestroyCmuxExtensionsContainerForBrowser(Browser* browser) {
  if (browser) {
    ContainerInstances().erase(browser);
  }
}

void CloseCmuxExtensionsUi() {
  for (auto& [browser, container] : ContainerInstances()) {
    container->CloseUiForWindowTeardown();
  }
}

void DestroyCmuxExtensionsContainer() {
  ContainerInstances().clear();
}

bool ShowExtensionPostInstallDialogAtPuzzle(
    Profile* profile,
    scoped_refptr<const extensions::Extension> extension,
    const SkBitmap& icon) {
  if (!profile) {
    return false;
  }
  CmuxExtensionsContainer* container =
      GetCmuxExtensionsContainer(profile->GetOriginalProfile());
  return container &&
         container->ShowPostInstallDialog(profile, std::move(extension), icon);
}

bool RunCmuxExtensionsMenuSelfTest(Profile* profile) {
  CmuxExtensionsContainer* container =
      GetCmuxExtensionsContainerForSelfTest(profile);
  if (!container) {
    LOG(ERROR) << "cmux-ext-selftest: FAIL chrome extensions menu no container";
    return false;
  }
  if (!container->ClickMenuButtonForSelfTest()) {
    LOG(ERROR) << "cmux-ext-selftest: FAIL chrome extensions menu no button";
    return false;
  }
  views::Widget* widget = container->GetMenuWidgetForTesting();
  if (!container->IsMenuShowingForTesting() || !widget || widget->IsClosed()) {
    LOG(ERROR) << "cmux-ext-selftest: FAIL chrome extensions menu first click "
                  "did not open";
    container->CloseExtensionsMenuIfOpen();
    return false;
  }
  LOG(WARNING)
      << "cmux-ext-selftest: PASS chrome extensions menu first click opened";
  if (!container->ClickMenuButtonForSelfTest()) {
    LOG(ERROR)
        << "cmux-ext-selftest: FAIL chrome extensions menu second click no "
           "button";
    container->CloseExtensionsMenuIfOpen();
    return false;
  }
  if (container->IsMenuShowingForTesting()) {
    LOG(ERROR) << "cmux-ext-selftest: FAIL chrome extensions menu second click "
                  "stayed open";
    container->CloseExtensionsMenuIfOpen();
    return false;
  }
  LOG(WARNING)
      << "cmux-ext-selftest: PASS chrome extensions menu second click closed";
  return true;
}

}  // namespace cmux
