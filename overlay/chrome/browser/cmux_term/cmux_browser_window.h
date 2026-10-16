// Copyright 2025 The Chromium Authors
// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: BSD-3-Clause
//
// Derived from Chromium; see third_party/chromium/LICENSE.

#ifndef CHROME_BROWSER_CMUX_TERM_CMUX_BROWSER_WINDOW_H_
#define CHROME_BROWSER_CMUX_TERM_CMUX_BROWSER_WINDOW_H_

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/functional/callback_forward.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "build/build_config.h"
#include "chrome/browser/cmux_term/window_model.h"
#include "chrome/browser/cmux_term/cmux_native_window_registry.h"
#include "chrome/browser/ui/browser_window.h"
#include "chrome/browser/ui/browser_window_deleter.h"
#include "chrome/browser/ui/exclusive_access/exclusive_access_context.h"
#include "chrome/common/chrome_version.h"
// Chromium 150 removed these feature-specific BrowserWindow virtuals; 151
// completed their migration to DesktopBrowserWindowCapabilities. Keep the
// adapter source compatible with both sides of that boundary while the cmux
// overlay supports multiple Chromium release branches.
#if CHROME_VERSION_MAJOR >= 150
#define CMUX_BROWSER_WINDOW_HAS_DESKTOP_CAPABILITIES 1
#else
#define CMUX_BROWSER_WINDOW_HAS_DESKTOP_CAPABILITIES 0
#endif

namespace views {
class Widget;
}

namespace cmux {

// The workspace-facing half of CmuxBrowserWindow. CmuxWindowView implements
// this interface; keeping it separate lets BrowserWindow::CreateBrowserWindow
// construct a real BrowserWindow while Browser itself is still being built.
class CmuxBrowserWindowHost {
 public:
  virtual ~CmuxBrowserWindowHost();

  // Adopts a normal Browser created by Chrome itself (for example
  // chrome.windows.create) as a new cmux workspace while its BrowserWindow is
  // being constructed. Non-workspace Browser types return nullopt and keep
  // Chromium's normal BrowserView path.
  virtual std::optional<WorkspaceId> AdoptExternalBrowser(Browser* browser) = 0;

  virtual views::Widget* GetWorkspaceWidget() = 0;
  virtual bool IsWorkspaceVisible(WorkspaceId workspace) const = 0;
  virtual bool IsWorkspaceActive(WorkspaceId workspace) const = 0;
  virtual void ShowWorkspace(WorkspaceId workspace, bool activate) = 0;
  virtual void CloseWorkspaceFromBrowser(WorkspaceId workspace) = 0;
  virtual void FocusWorkspaceLocationBar(WorkspaceId workspace) = 0;
  virtual void FocusWorkspaceContents(WorkspaceId workspace) = 0;
  virtual void FocusWorkspaceAppMenu(WorkspaceId workspace) = 0;
  virtual LocationBar* GetWorkspaceLocationBar(
      WorkspaceId workspace,
      content::WebContents* web_contents) = 0;
  virtual bool IsWorkspaceWebContentsVisible(
      WorkspaceId workspace,
      content::WebContents* web_contents) = 0;
  virtual web_modal::WebContentsModalDialogHost*
  GetWorkspaceWebContentsModalDialogHost(
      WorkspaceId workspace,
      content::WebContents* web_contents) = 0;
  virtual content::KeyboardEventProcessingResult
  PreHandleWorkspaceKeyboardEvent(
      WorkspaceId workspace,
      const input::NativeWebKeyboardEvent& event) = 0;
  virtual bool HandleWorkspaceKeyboardEvent(
      WorkspaceId workspace,
      const input::NativeWebKeyboardEvent& event) = 0;
  virtual void OnWorkspaceBrowserActiveTabChanged(
      WorkspaceId workspace,
      content::WebContents* old_contents,
      content::WebContents* new_contents) = 0;
};

// Production BrowserWindow implementation for one cmux workspace. A real
// Chromium Browser owns this object and its TabStripModel/WebContents; this
// adapter maps logical window operations onto the workspace hosted inside the
// shared cmux Widget instead of constructing BrowserView/BrowserWidget.
class CmuxBrowserWindow final : public BrowserWindow,
                                public ExclusiveAccessContext {
 public:
  CmuxBrowserWindow(Browser* browser,
                    NativeWindowId native_window,
                    WorkspaceId workspace,
                    std::string window_group,
                    std::string workspace_key,
                    base::WeakPtr<CmuxBrowserWindowHost> host);
  CmuxBrowserWindow(const CmuxBrowserWindow&) = delete;
  CmuxBrowserWindow& operator=(const CmuxBrowserWindow&) = delete;
  ~CmuxBrowserWindow() override;

  Browser* browser() const { return browser_; }
  WorkspaceId workspace() const { return workspace_; }
  LocationBar* GetLocationBarForWebContents(
      content::WebContents* web_contents) const;
  bool IsWebContentsVisible(content::WebContents* web_contents) const;

  // ui::BaseWindow:
  void Show() override;
  void ShowInactive() override;
  void Hide() override;
  bool IsVisible() const override;
  void SetBounds(const gfx::Rect& bounds) override;
  void Close() override;
  void Activate() override;
  void Deactivate() override;
  bool IsActive() const override;
  void FlashFrame(bool flash) override;
  ui::ZOrderLevel GetZOrderLevel() const override;
  void SetZOrderLevel(ui::ZOrderLevel order) override;
  gfx::NativeWindow GetNativeWindow() const override;
  gfx::Rect GetRestoredBounds() const override;
  ui::mojom::WindowShowState GetRestoredState() const override;
  gfx::Rect GetBounds() const override;
  bool IsMaximized() const override;
  bool IsMinimized() const override;
  void Maximize() override;
  void Minimize() override;
  void Restore() override;
  bool IsFullscreen() const override;

  // BrowserWindow:
  bool IsOnCurrentWorkspace() const override;
  bool IsVisibleOnScreen() const override;
  void SetTopControlsShownRatio(content::WebContents* web_contents,
                                float ratio) override;
  bool DoBrowserControlsShrinkRendererSize(
      const content::WebContents* contents) const override;
  ui::NativeTheme* GetNativeTheme() override;
  const ui::ThemeProvider* GetThemeProvider() const override;
  const ui::ColorProvider* GetColorProvider() const override;
  int GetTopControlsHeight() const override;
  void SetTopControlsGestureScrollInProgress(bool in_progress) override;
  std::vector<StatusBubble*> GetStatusBubbles() override;
  void UpdateTitleBar() override;
#if !CMUX_BROWSER_WINDOW_HAS_DESKTOP_CAPABILITIES
  void BookmarkBarStateChanged(
      BookmarkBar::AnimateChangeType change_type) override;
  void TemporarilyShowBookmarkBar(base::TimeDelta duration) override;
  void UpdateDevTools(content::WebContents* inspected_web_contents) override;
  bool CanDockDevTools() const override;
#endif
  void UpdateLoadingAnimations(bool is_visible) override;
  void SetStarredState(bool is_starred) override;
  bool IsTabModalPopupDeprecated() const override;
  void SetIsTabModalPopupDeprecated(bool deprecated) override;
  void OnActiveTabChanged(content::WebContents* old_contents,
                          content::WebContents* new_contents,
                          int index,
                          int reason) override;
  void OnTabDetached(content::WebContents* contents, bool was_active) override;
#if !CMUX_BROWSER_WINDOW_HAS_DESKTOP_CAPABILITIES
  void ZoomChangedForActiveTab(bool can_show_bubble) override;
  bool ShouldHideUIForFullscreen() const override;
  bool IsFullscreenBubbleVisible() const override;
  bool IsForceFullscreen() const override;
  void SetForceFullscreen(bool force_fullscreen) override;
#endif
  gfx::Size GetContentsSize() const override;
  void SetContentsSize(const gfx::Size& size) override;
  void UpdatePageActionIcon(PageActionIconType type) override;
  autofill::AutofillBubbleHandler* GetAutofillBubbleHandler() override;
  void ExecutePageActionIconForTesting(PageActionIconType type) override;
  LocationBar* GetLocationBar() const override;
  void SetFocusToLocationBar(bool is_user_initiated) override;
  void UpdateReloadStopState(bool is_loading, bool force) override;
  void UpdateToolbar(content::WebContents* contents) override;
  bool UpdateToolbarSecurityState() override;
  void UpdateCustomTabBarVisibility(bool visible, bool animate) override;
#if !CMUX_BROWSER_WINDOW_HAS_DESKTOP_CAPABILITIES
  void SetDevToolsScrimVisibility(bool visible) override;
#endif
  void ResetToolbarTabState(content::WebContents* contents) override;
  void FocusToolbar() override;
  void ToolbarSizeChanged(bool is_animating) override;
  void TabDraggingStatusChanged(bool is_dragging) override;
  void LinkOpeningFromGesture(WindowOpenDisposition disposition) override;
  void FocusAppMenu() override;
#if !CMUX_BROWSER_WINDOW_HAS_DESKTOP_CAPABILITIES
  void FocusBookmarksToolbar() override;
  void FocusInactivePopupForAccessibility() override;
  void RotatePaneFocus(bool forwards) override;
  void FocusWebContentsPane() override;
  bool IsBookmarkBarVisible() const override;
  bool IsBookmarkBarAnimating() const override;
#endif
  bool IsTabStripEditable() const override;
  void DisableTabStripEditingForTesting() override;
  bool IsToolbarVisible() const override;
  bool IsToolbarShowing() const override;
  bool IsLocationBarVisible() const override;
#if !CMUX_BROWSER_WINDOW_HAS_DESKTOP_CAPABILITIES
  SharingDialog* ShowSharingDialog(content::WebContents* contents,
                                   SharingDialogData data) override;
#endif
  void ShowUpdateChromeDialog() override;
#if !BUILDFLAG(IS_ANDROID)
  void ShowIntentPickerBubble(
      std::vector<apps::IntentPickerAppInfo> app_info,
      bool show_stay_in_chrome,
      bool show_remember_selection,
      apps::IntentPickerBubbleType bubble_type,
      const std::optional<url::Origin>& initiating_origin,
      IntentPickerResponse callback) override;
#endif
  void ShowBookmarkBubble(const GURL& url, bool already_bookmarked) override;
#if !CMUX_BROWSER_WINDOW_HAS_DESKTOP_CAPABILITIES
#if !BUILDFLAG(IS_ANDROID)
  sharing_hub::ScreenshotCapturedBubble* ShowScreenshotCapturedBubble(
      content::WebContents* contents,
      const gfx::Image& image) override;
#endif
  qrcode_generator::QRCodeGeneratorBubbleView* ShowQRCodeGeneratorBubble(
      content::WebContents* contents,
      const GURL& url,
      bool show_back_button) override;
  send_tab_to_self::SendTabToSelfBubbleView*
  ShowSendTabToSelfDevicePickerBubble(
      content::WebContents* contents) override;
  send_tab_to_self::SendTabToSelfBubbleView* ShowSendTabToSelfPromoBubble(
      content::WebContents* contents,
      bool show_signin_button) override;
#endif
#if BUILDFLAG(IS_CHROMEOS)
  void ToggleMultitaskMenu() override;
#elif !CMUX_BROWSER_WINDOW_HAS_DESKTOP_CAPABILITIES
  sharing_hub::SharingHubBubbleView* ShowSharingHubBubble(
      share::ShareAttempt attempt) override;
#endif
  ShowTranslateBubbleResult ShowTranslateBubble(
      content::WebContents* contents,
      translate::TranslateStep step,
      const std::string& source_language,
      const std::string& target_language,
      translate::TranslateErrors error_type,
      bool is_user_gesture) override;
#if !CMUX_BROWSER_WINDOW_HAS_DESKTOP_CAPABILITIES
  void StartPartialTranslate(const std::string& source_language,
                             const std::string& target_language,
                             const std::u16string& text_selection) override;
#endif
  DownloadBubbleUIController* GetDownloadBubbleUIController() override;
  void ConfirmBrowserCloseWithPendingDownloads(
      int download_count,
      Browser::DownloadCloseType dialog_type,
      base::OnceCallback<void(bool)> callback) override;
  void ShowAppMenu() override;
  void PreHandleDragUpdate(const content::DropData& drop_data,
                           const gfx::PointF& point) override;
  void PreHandleDragExit() override;
  void HandleDragEnded() override;
  content::KeyboardEventProcessingResult PreHandleKeyboardEvent(
      const input::NativeWebKeyboardEvent& event) override;
  bool HandleKeyboardEvent(
      const input::NativeWebKeyboardEvent& event) override;
  std::unique_ptr<FindBar> CreateFindBar() override;
  web_modal::WebContentsModalDialogHost* GetWebContentsModalDialogHost()
      override;
  web_modal::WebContentsModalDialogHost* GetWebContentsModalDialogHostFor(
      content::WebContents* web_contents) override;
  void ShowAvatarBubbleFromAvatarButton(bool is_source_accelerator) override;
  void MaybeShowProfileSwitchIPH() override;
  void MaybeShowSupervisedUserProfileSignInIPH() override;
#if BUILDFLAG(IS_CHROMEOS) || BUILDFLAG(IS_MAC) || BUILDFLAG(IS_WIN) || \
    BUILDFLAG(IS_LINUX)
  void ShowHatsDialog(
      const std::string& site_id,
      const std::optional<std::string>& hats_histogram_name,
      const std::optional<uint64_t> hats_survey_ukm_id,
      base::OnceClosure success_callback,
      base::OnceClosure failure_callback,
      const SurveyBitsData& product_specific_bits_data,
      const SurveyStringData& product_specific_string_data) override;
  void ShowIncognitoClearBrowsingDataDialog() override;
  void ShowIncognitoHistoryDisclaimerDialog() override;
#endif
  ExclusiveAccessContext* GetExclusiveAccessContext() override;
  std::string GetWorkspace() const override;
  bool IsVisibleOnAllWorkspaces() const override;
  void ShowEmojiPanel() override;
  std::unique_ptr<content::EyeDropper> OpenEyeDropper(
      content::RenderFrameHost* frame,
      content::EyeDropperListener* listener) override;
  void ShowCaretBrowsingDialog() override;
  void CreateTabSearchBubble() override;
  void CloseTabSearchBubble() override;
  bool IsUnframedModeEnabled() const override;
  bool GetCanResize() override;
  ui::mojom::WindowShowState GetWindowShowState() const override;
  void ShowChromeLabs() override;
  BrowserView* AsBrowserView() override;
  void DeleteBrowserWindow() override;

  // ExclusiveAccessContext:
  Profile* GetProfile() override;
  void EnterFullscreen(const url::Origin& origin,
                       ExclusiveAccessBubbleType bubble_type,
                       FullscreenTabParams fullscreen_tab_params) override;
  void ExitFullscreen() override;
  void UpdateExclusiveAccessBubble(
      const ExclusiveAccessBubbleParams& params,
      ExclusiveAccessBubbleHideCallback first_hide_callback) override;
  bool IsExclusiveAccessBubbleDisplayed() const override;
  void OnExclusiveAccessUserInput() override;
  content::WebContents* GetWebContentsForExclusiveAccess() override;
  bool CanUserEnterFullscreen() const override;
  bool CanUserExitFullscreen() const override;

 private:
  views::Widget* widget() const;

  raw_ptr<Browser> browser_;
  const NativeWindowId native_window_;
  const WorkspaceId workspace_;
  const std::string window_group_;
  const std::string workspace_key_;
  base::WeakPtr<CmuxBrowserWindowHost> host_;
#if !CMUX_BROWSER_WINDOW_HAS_DESKTOP_CAPABILITIES
  bool force_fullscreen_ = false;
#endif
  bool tab_modal_popup_deprecated_ = false;
};

// BrowserWindow factory seam. Browser::CreateParams::initial_workspace carries
// a v2 durable window-group/workspace UUID token (with v1 runtime-token
// migration support); the normal BrowserWindow factory calls this first and
// falls back to BrowserView when it returns null.
std::unique_ptr<BrowserWindow, BrowserWindowDeleter>
MaybeCreateCmuxBrowserWindow(Browser* browser);

// Resolves the production LocationBar for the requesting WebContents. Standard
// BrowserView windows retain their active-toolbar behavior; cmux windows route
// through their pane placement so a background or separately visible tab
// cannot display another origin's permission UI over the focused pane.
LocationBar* GetLocationBarForWebContents(
    BrowserWindowInterface* browser,
    content::WebContents* web_contents);
bool IsWebContentsActiveOrVisible(
    BrowserWindowInterface* browser,
    content::WebContents* web_contents);
gfx::Rect GetFallbackBubbleAnchorRectForWebContents(
    BrowserWindowInterface* browser,
    content::WebContents* web_contents);

void RegisterCmuxBrowserWindowHost(
    NativeWindowId native_window,
    WorkspaceId workspace,
    std::string window_group,
    std::string workspace_key,
    base::WeakPtr<CmuxBrowserWindowHost> host);
void UnregisterCmuxBrowserWindowHost(NativeWindowId native_window,
                                     WorkspaceId workspace);
void RegisterCmuxBrowserWindowFactoryHost(
    base::WeakPtr<CmuxBrowserWindowHost> host);
void UnregisterCmuxBrowserWindowFactoryHost(CmuxBrowserWindowHost* host);
std::string CmuxWorkspaceToken(NativeWindowId native_window,
                               WorkspaceId workspace);

}  // namespace cmux

#endif  // CHROME_BROWSER_CMUX_TERM_CMUX_BROWSER_WINDOW_H_
