// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef CHROME_BROWSER_CMUX_TERM_CMUX_EXTENSION_STRIP_H_
#define CHROME_BROWSER_CMUX_TERM_CMUX_EXTENSION_STRIP_H_

#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "base/callback_list.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/scoped_observation.h"
#include "base/time/time.h"
#include "chrome/browser/cmux_term/window_model.h"
#include "chrome/browser/ui/toolbar/toolbar_actions_model.h"
#include "chrome/browser/ui/views/toolbar/toolbar_action_view.h"
#include "chrome/browser/ui/views/toolbar/toolbar_icon_container_view.h"
#include "chrome/common/chrome_version.h"
#include "components/prefs/pref_change_registrar.h"
#include "content/public/browser/web_contents_observer.h"
#include "extensions/browser/extension_registry_observer.h"
#include "ui/base/metadata/metadata_header_macros.h"

class Profile;
class Browser;
class ExtensionActionViewModel;

namespace content {
class BrowserContext;
class WebContents;
}  // namespace content

namespace extensions {
class Extension;
}

namespace views {
class LabelButton;
class View;
class Widget;
}

namespace cmux {

class CmuxExtensionsContainer;

// Runs the toolbar action for an extension in `browser`. This is the command
// registry bridge used by `extension.<extension-id>.action` keybindings.
bool ExecuteCmuxExtensionAction(Browser* browser,
                                std::string_view extension_id);

// A row of Chromium-native extension action buttons for one web surface.
// ExtensionActionViewModel and ExtensionActionDelegateDesktop provide the
// normal action, popup, context-menu, and toggle behavior against the surface's
// owning Browser.
class CmuxExtensionStrip : public ToolbarIconContainerView,
                           public ToolbarActionView::Delegate,
                           public ToolbarActionsModel::Observer,
                           public extensions::ExtensionRegistryObserver,
                           public content::WebContentsObserver {
  METADATA_HEADER(CmuxExtensionStrip, ToolbarIconContainerView)

 public:
  CmuxExtensionStrip(content::WebContents* web_contents, PaneId pane);
  CmuxExtensionStrip(const CmuxExtensionStrip&) = delete;
  CmuxExtensionStrip& operator=(const CmuxExtensionStrip&) = delete;
  ~CmuxExtensionStrip() override;

  // ToolbarActionsModel::Observer:
  void OnToolbarActionAdded(const ToolbarActionsModel::ActionId& id) override;
  void OnToolbarActionRemoved(const ToolbarActionsModel::ActionId& id) override;
  void OnToolbarActionUpdated(const ToolbarActionsModel::ActionId& id) override;
  void OnToolbarModelInitialized() override;
  void OnToolbarPinnedActionsChanged() override;

  // extensions::ExtensionRegistryObserver:
  void OnExtensionLoaded(content::BrowserContext* browser_context,
                         const extensions::Extension* extension) override;
  void OnExtensionUnloaded(content::BrowserContext* browser_context,
                           const extensions::Extension* extension,
                           extensions::UnloadedExtensionReason reason) override;
  void OnExtensionInstalled(content::BrowserContext* browser_context,
                            const extensions::Extension* extension,
                            bool is_update) override;
  void OnExtensionUninstalled(content::BrowserContext* browser_context,
                              const extensions::Extension* extension,
                              extensions::UninstallReason reason) override;
  void OnShutdown(extensions::ExtensionRegistry* registry) override;

  // content::WebContentsObserver:
  void WebContentsDestroyed() override;
  void PrimaryPageChanged(content::Page& page) override;

  // ToolbarIconContainerView / views::View:
  void UpdateAllIcons() override;
  void OnBoundsChanged(const gfx::Rect& previous_bounds) override;
  void VisibilityChanged(views::View* starting_from, bool is_visible) override;

  // ToolbarActionView::Delegate. The native ToolbarActionView and
  // ExtensionActionDelegateDesktop own click/toggle/popup behavior; cmux only
  // supplies the active WebContents and compact layout geometry.
  content::WebContents* GetCurrentWebContents() override;
  views::LabelButton* GetOverflowReferenceView() const override;
  gfx::Size GetToolbarActionSize() override;
  void MovePinnedActionBy(const std::string& action_id, int move_by) override;
  void UpdateHoverCard(ToolbarActionView* action_view,
                       ToolbarActionHoverCardUpdateType update_type) override;
  // Added to ToolbarActionView::Delegate in Chromium 151. Keep the declaration
  // source-compatible with the M149 rollback while retaining override checks.
#if CHROME_VERSION_MAJOR >= 151
  bool IsFocusOnExtensionAction() const override;
#else
  bool IsFocusOnExtensionAction() const;
#endif
  void OnContextMenuShown(const std::string& action_id) override;
  void OnContextMenuClosed(const std::string& action_id) override;
  void WriteDragDataForView(views::View* sender,
                            const gfx::Point& press_pt,
                            ui::OSExchangeData* data) override;
  int GetDragOperationsForView(views::View* sender,
                               const gfx::Point& p) override;
  bool CanStartDragForView(views::View* sender,
                           const gfx::Point& press_pt,
                           const gfx::Point& p) override;

 private:
  // Drops all buttons and recreates one per action in the model.
  void Rebuild();
  void ScheduleRebuild();

  // Re-renders every button's icon + badge (badge text is per-tab state).
  void RefreshIcons();

  ToolbarActionView* ButtonForExtension(const std::string& id);
  void OnPinnedExtensionsPrefChanged();
  void LogActionButtonAddedOnce(const std::string& extension_id);

  raw_ptr<Profile> profile_;
  raw_ptr<Browser> browser_ = nullptr;
  PaneId pane_ = kInvalidId;
  raw_ptr<CmuxExtensionsContainer> extensions_container_ = nullptr;
  raw_ptr<views::LabelButton> extensions_menu_button_ = nullptr;
  raw_ptr<ToolbarActionsModel> model_ = nullptr;
  // The buttons are owned by the view hierarchy (children of this strip).
  std::vector<raw_ptr<ToolbarActionView>> buttons_;
  std::vector<std::unique_ptr<ExtensionActionViewModel>> action_view_models_;
  // Real action views are retained even while unpinned so Chrome's
  // AnimatingLayoutManager can run its native FadeIn/FadeOut path.
  PrefChangeRegistrar pref_change_registrar_;
  base::CallbackListSubscription pane_pin_subscription_;
  base::ScopedObservation<ToolbarActionsModel, ToolbarActionsModel::Observer>
      model_observation_{this};
  base::ScopedObservation<extensions::ExtensionRegistry,
                          extensions::ExtensionRegistryObserver>
      registry_observation_{this};
  base::TimeTicks creation_time_;
  std::set<std::string> logged_action_button_ids_;
  bool rebuild_pending_ = false;
  base::WeakPtrFactory<CmuxExtensionStrip> weak_factory_{this};
};

}  // namespace cmux

#endif  // CHROME_BROWSER_CMUX_TERM_CMUX_EXTENSION_STRIP_H_
