// Copyright 2025 The Chromium Authors
// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: BSD-3-Clause
//
// Derived from Chromium; see third_party/chromium/LICENSE.

#ifndef CHROME_BROWSER_CMUX_TERM_CMUX_EXTENSIONS_CONTAINER_H_
#define CHROME_BROWSER_CMUX_TERM_CMUX_EXTENSIONS_CONTAINER_H_

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/scoped_observation.h"
#include "chrome/browser/ui/extensions/extensions_container.h"
#include "chrome/browser/ui/views/extensions/extensions_container_views.h"
#include "chrome/common/chrome_version.h"
#include "extensions/common/extension_id.h"
#include "ui/base/unowned_user_data/scoped_unowned_user_data.h"
#include "ui/views/bubble/bubble_dialog_delegate_view.h"
#include "ui/views/view_tracker.h"
#include "ui/views/widget/widget_observer.h"

class Browser;
class BrowserWindowInterface;
class ExtensionsMenuCoordinator;
class Profile;
class SkBitmap;
class ToolbarActionView;
class ToolbarActionViewModel;

namespace content {
class WebContents;
}  // namespace content

namespace extensions {
class Extension;
}  // namespace extensions

namespace views {
class FocusManager;
class View;
class Widget;
}  // namespace views

namespace cmux {

class CmuxExtensionsContainer final : public ExtensionsContainer,
                                      public ExtensionsContainerViews,
                                      public views::WidgetObserver {
 public:
  CmuxExtensionsContainer(Profile* profile, Browser* browser);
  CmuxExtensionsContainer(const CmuxExtensionsContainer&) = delete;
  CmuxExtensionsContainer& operator=(const CmuxExtensionsContainer&) = delete;
  ~CmuxExtensionsContainer() override;

  void SetMenuButtonAnchor(views::View* anchor);
  void RegisterActionAnchor(const std::string& action_id, views::View* anchor);
  void UnregisterActionAnchor(const std::string& action_id,
                              views::View* anchor);
  void RegisterActionView(const std::string& action_id,
                          content::WebContents* web_contents,
                          ToolbarActionViewModel* model,
                          ToolbarActionView* anchor);
  void UnregisterActionView(ToolbarActionViewModel* model);
  bool ActivateAction(const std::string& action_id);
  void CloseUiForWindowTeardown();
  bool ShowMenuForSelfTest();
  bool ClickMenuButtonForSelfTest();
  bool ClickActionForSelfTest(const std::string& action_id);
  bool IsActionContainerHighlightAlignedForTesting(
      const std::string& action_id) const;
  bool IsMenuShowingForTesting() const;
  views::Widget* GetMenuWidgetForTesting();
  bool ShowPostInstallDialog(
      Profile* dialog_profile,
      scoped_refptr<const extensions::Extension> extension,
      const SkBitmap& icon);
  bool IsPostInstallDialogShowingForTesting() const;
  views::Widget* GetPostInstallDialogWidgetForTesting();

  // ExtensionsContainer:
  ToolbarActionViewModel* GetActionForId(const std::string& action_id) override;
  void HideActivePopup() override;
#if CHROME_VERSION_MAJOR >= 151
  void CloseExtensionsMenuIfOpen() override;
#else
  bool CloseOverflowMenuIfOpen() override;
  void CloseExtensionsMenuIfOpen();
#endif
  bool ShowToolbarActionPopupForAPICall(const std::string& action_id,
                                        ShowPopupCallback callback) override;
  void ToggleExtensionsMenu() override;
  bool HasAnyExtensions() const override;

  // ExtensionsContainerViews:
  std::optional<extensions::ExtensionId> GetPoppedOutActionId() const override;
  bool IsActionVisibleOnToolbar(const std::string& action_id) const override;
  void UndoPopOut() override;
  void SetPopupOwner(ToolbarActionViewModel* popup_owner) override;
  void PopOutAction(const extensions::ExtensionId& action_id,
                    base::OnceClosure closure) override;
  void CollapseConfirmation() override;
  void ShowContextMenuAsFallback(
      const extensions::ExtensionId& action_id) override;
  void OnPopupShown(const extensions::ExtensionId& action_id,
                    bool by_user) override;
  void OnPopupClosed(const extensions::ExtensionId& action_id) override;
  views::FocusManager* GetFocusManagerForAccelerator() override;
  views::BubbleAnchor GetReferenceButtonForPopup(
      const extensions::ExtensionId& action_id) override;

  // views::WidgetObserver:
  void OnWidgetDestroying(views::Widget* widget) override;

 private:
  // Non-const: the builder's views::ViewTracker::view() const overload
  // returns const View*, and callers need a mutable anchor.
  views::View* ResolvePuzzleAnchor();
  views::View* ResolveMenuAnchor();
  ToolbarActionView* ResolveActionView(const std::string& action_id) const;
  ToolbarActionView* ResolveActionView(
      ToolbarActionViewModel* view_model) const;
  views::View* ResolveActionAnchor(const std::string& action_id) const;
  void ClosePostInstallDialog();

  raw_ptr<Profile> profile_;
  raw_ptr<Browser> browser_;
  std::unique_ptr<ExtensionsMenuCoordinator> extensions_menu_coordinator_;
  std::unique_ptr<ui::ScopedUnownedUserData<ExtensionsContainer>>
      scoped_extensions_container_;
  views::ViewTracker menu_anchor_;
  base::WeakPtr<views::Widget> menu_widget_;
  views::ViewTracker post_install_dialog_anchor_;
  std::map<std::string, raw_ptr<views::View>> action_anchors_;
  struct ActionViewRegistration {
    ActionViewRegistration(std::string action_id,
                           base::WeakPtr<content::WebContents> web_contents,
                           ToolbarActionViewModel* model,
                           ToolbarActionView* anchor);
    ~ActionViewRegistration();
    ActionViewRegistration(ActionViewRegistration&&);
    ActionViewRegistration& operator=(ActionViewRegistration&&);

    std::string action_id;
    base::WeakPtr<content::WebContents> web_contents;
    raw_ptr<ToolbarActionViewModel> model = nullptr;
    raw_ptr<ToolbarActionView> anchor = nullptr;
  };
  std::vector<ActionViewRegistration> action_views_;
  raw_ptr<ToolbarActionViewModel> popup_owner_ = nullptr;
  // The exact pane-local view that opened the active popup. Several visible
  // cmux panes can register an action with the same extension id, so resolving
  // by id when the popup closes can target a different pane after focus moves.
  raw_ptr<ToolbarActionView> popup_action_view_ = nullptr;
  raw_ptr<views::Widget> post_install_dialog_widget_ = nullptr;
  base::ScopedObservation<views::Widget, views::WidgetObserver>
      post_install_dialog_observation_{this};
  std::optional<extensions::ExtensionId> popped_out_action_;
};

void InstallCmuxExtensionsMenuOverrides();
CmuxExtensionsContainer* GetCmuxExtensionsContainerForBrowser(Browser* browser);
CmuxExtensionsContainer* GetCmuxExtensionsContainer(Profile* profile);
// Runtime self-tests can start before renderer input has established a
// focused cmux pane. Fall back to an already-registered container for the
// profile so the test still dispatches through the real native button views.
CmuxExtensionsContainer* GetCmuxExtensionsContainerForSelfTest(
    Profile* profile);
void DestroyCmuxExtensionsContainerForBrowser(Browser* browser);
void CloseCmuxExtensionsUi();
void DestroyCmuxExtensionsContainer();
bool ShowExtensionPostInstallDialogAtPuzzle(
    Profile* profile,
    scoped_refptr<const extensions::Extension> extension,
    const SkBitmap& icon);
bool RunCmuxExtensionsMenuSelfTest(Profile* profile);

}  // namespace cmux

#endif  // CHROME_BROWSER_CMUX_TERM_CMUX_EXTENSIONS_CONTAINER_H_
