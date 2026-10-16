// Copyright 2025 The Chromium Authors
// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: BSD-3-Clause
//
// Derived from Chromium; see third_party/chromium/LICENSE.

#include "chrome/browser/cmux_term/cmux_browser_window.h"

#include <cstdint>
#include <map>
#include <utility>

#include "base/check.h"
#include "base/logging.h"
#include "base/no_destructor.h"
#include "base/strings/string_number_conversions.h"
#include "chrome/browser/cmux_term/cmux_workspace_projection.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/bubble_anchor_util.h"
#include "chrome/browser/ui/browser_window/public/browser_window_features.h"
#include "chrome/browser/ui/location_bar/location_bar.h"
#include "chrome/browser/ui/side_panel/side_panel_ui.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "components/sharing_message/sharing_dialog_data.h"
#include "content/public/browser/eye_dropper.h"
#include "content/public/browser/keyboard_event_processing_result.h"
#include "content/public/browser/web_contents.h"
#include "ui/base/mojom/window_show_state.mojom.h"
#include "ui/base/theme_provider.h"
#include "ui/color/color_provider.h"
#include "ui/native_theme/native_theme.h"
#include "ui/views/view.h"
#include "ui/views/widget/widget.h"

namespace cmux {
namespace {

constexpr char kWorkspacePrefix[] = "cmux-workspace:";

struct HostRecord {
  base::WeakPtr<CmuxBrowserWindowHost> host;
  CmuxDurableWorkspaceAddress durable;
};

using HostMap = std::map<WorkspaceLocator, HostRecord>;
using DurableHostMap =
    std::map<CmuxDurableWorkspaceAddress, WorkspaceLocator>;
using BrowserWindowMap =
    std::map<uintptr_t, raw_ptr<CmuxBrowserWindow>>;

HostMap& Hosts() {
  static base::NoDestructor<HostMap> hosts;
  return *hosts;
}

DurableHostMap& DurableHosts() {
  static base::NoDestructor<DurableHostMap> hosts;
  return *hosts;
}

BrowserWindowMap& BrowserWindows() {
  static base::NoDestructor<BrowserWindowMap> windows;
  return *windows;
}

uintptr_t BrowserIdentity(const Browser* browser) {
  return reinterpret_cast<uintptr_t>(browser);
}

base::WeakPtr<CmuxBrowserWindowHost>& FactoryHost() {
  static base::NoDestructor<base::WeakPtr<CmuxBrowserWindowHost>> host;
  return *host;
}

std::optional<WorkspaceLocator> WorkspaceFromToken(const std::string& token) {
  if (!token.starts_with(kWorkspacePrefix)) {
    return std::nullopt;
  }
  const std::string payload = token.substr(std::size(kWorkspacePrefix) - 1);
  const size_t separator = payload.find(':');
  int64_t native_window = kInvalidNativeWindowId;
  int64_t workspace = kInvalidId;
  if (separator == std::string::npos ||
      !base::StringToInt64(payload.substr(0, separator), &native_window) ||
      !base::StringToInt64(payload.substr(separator + 1), &workspace) ||
      native_window == kInvalidNativeWindowId || workspace == kInvalidId) {
    return std::nullopt;
  }
  return WorkspaceLocator{native_window, workspace};
}

}  // namespace

CmuxBrowserWindowHost::~CmuxBrowserWindowHost() = default;

CmuxBrowserWindow::CmuxBrowserWindow(
    Browser* browser,
    NativeWindowId native_window,
    WorkspaceId workspace,
    std::string window_group,
    std::string workspace_key,
    base::WeakPtr<CmuxBrowserWindowHost> host)
    : browser_(browser),
      native_window_(native_window),
      workspace_(workspace),
      window_group_(std::move(window_group)),
      workspace_key_(std::move(workspace_key)),
      host_(std::move(host)) {
  CHECK(browser_);
  CHECK_NE(native_window_, kInvalidNativeWindowId);
  CHECK_NE(workspace_, kInvalidId);
  CHECK(BrowserWindows().emplace(BrowserIdentity(browser_), this).second);
}

CmuxBrowserWindow::~CmuxBrowserWindow() {
  const auto existing = BrowserWindows().find(BrowserIdentity(browser_));
  CHECK(existing != BrowserWindows().end());
  CHECK_EQ(existing->second.get(), this);
  BrowserWindows().erase(existing);
}

LocationBar* CmuxBrowserWindow::GetLocationBarForWebContents(
    content::WebContents* web_contents) const {
  return host_
             ? host_->GetWorkspaceLocationBar(workspace_, web_contents)
             : nullptr;
}

bool CmuxBrowserWindow::IsWebContentsVisible(
    content::WebContents* web_contents) const {
  return host_ &&
         host_->IsWorkspaceWebContentsVisible(workspace_, web_contents);
}

LocationBar* GetLocationBarForWebContents(
    BrowserWindowInterface* browser,
    content::WebContents* web_contents) {
  if (!browser) {
    return nullptr;
  }
  Browser* concrete_browser = browser->GetBrowserForMigrationOnly();
  const auto cmux_window =
      BrowserWindows().find(BrowserIdentity(concrete_browser));
  if (cmux_window != BrowserWindows().end()) {
    return cmux_window->second->GetLocationBarForWebContents(web_contents);
  }
  BrowserWindow* browser_window = BrowserWindow::FromBrowser(browser);
  return browser_window ? browser_window->GetLocationBar() : nullptr;
}

bool IsWebContentsActiveOrVisible(
    BrowserWindowInterface* browser,
    content::WebContents* web_contents) {
  if (!browser || !web_contents) {
    return false;
  }
  Browser* concrete_browser = browser->GetBrowserForMigrationOnly();
  const auto cmux_window =
      BrowserWindows().find(BrowserIdentity(concrete_browser));
  if (cmux_window != BrowserWindows().end()) {
    return cmux_window->second->IsWebContentsVisible(web_contents);
  }
  return browser->GetTabStripModel()->GetActiveWebContents() == web_contents;
}

gfx::Rect GetFallbackBubbleAnchorRectForWebContents(
    BrowserWindowInterface* browser,
    content::WebContents* web_contents) {
  if (!browser) {
    return gfx::Rect();
  }
  Browser* concrete_browser = browser->GetBrowserForMigrationOnly();
  if (web_contents &&
      BrowserWindows().contains(BrowserIdentity(concrete_browser))) {
    gfx::Rect container_bounds = web_contents->GetContainerBounds();
    gfx::Point origin = container_bounds.origin();
    origin.Offset(bubble_anchor_util::kNoToolbarLeftOffset, 0);
    return gfx::Rect(origin, gfx::Size());
  }
  return bubble_anchor_util::GetPageInfoAnchorRect(browser);
}

views::Widget* CmuxBrowserWindow::widget() const {
  return host_ ? host_->GetWorkspaceWidget() : nullptr;
}

void CmuxBrowserWindow::Show() {
  if (host_) {
    host_->ShowWorkspace(workspace_, true);
  }
  browser_->DidBecomeActive();
  browser_->OnWindowDidShow();
}

void CmuxBrowserWindow::ShowInactive() {
  if (host_) {
    host_->ShowWorkspace(workspace_, false);
  }
  browser_->OnWindowDidShow();
}

void CmuxBrowserWindow::Hide() {
  // A workspace is hidden by selecting another workspace. Browser-driven Hide
  // has no meaningful target workspace, so leave selection unchanged.
}

bool CmuxBrowserWindow::IsVisible() const {
  return host_ && host_->IsWorkspaceVisible(workspace_);
}

void CmuxBrowserWindow::SetBounds(const gfx::Rect& bounds) {
  if (views::Widget* w = widget()) {
    w->SetBounds(bounds);
  }
}

void CmuxBrowserWindow::Close() {
  if (host_) {
    host_->CloseWorkspaceFromBrowser(workspace_);
  } else if (browser_) {
    browser_->OnWindowClosing();
  }
}

void CmuxBrowserWindow::Activate() {
  if (host_) {
    host_->ShowWorkspace(workspace_, true);
  }
  browser_->DidBecomeActive();
}

void CmuxBrowserWindow::Deactivate() {
  browser_->DidBecomeInactive();
  if (views::Widget* w = widget(); w && w->IsActive()) {
    w->Deactivate();
  }
}

bool CmuxBrowserWindow::IsActive() const {
  return host_ && host_->IsWorkspaceActive(workspace_);
}

void CmuxBrowserWindow::FlashFrame(bool flash) {
  if (views::Widget* w = widget()) {
    w->FlashFrame(flash);
  }
}

ui::ZOrderLevel CmuxBrowserWindow::GetZOrderLevel() const {
  return widget() ? widget()->GetZOrderLevel() : ui::ZOrderLevel::kNormal;
}

void CmuxBrowserWindow::SetZOrderLevel(ui::ZOrderLevel order) {
  if (views::Widget* w = widget()) {
    w->SetZOrderLevel(order);
  }
}

gfx::NativeWindow CmuxBrowserWindow::GetNativeWindow() const {
  return widget() ? widget()->GetNativeWindow() : gfx::NativeWindow();
}

gfx::Rect CmuxBrowserWindow::GetRestoredBounds() const {
  return widget() ? widget()->GetRestoredBounds() : gfx::Rect();
}

ui::mojom::WindowShowState CmuxBrowserWindow::GetRestoredState() const {
  if (IsMaximized()) {
    return ui::mojom::WindowShowState::kMaximized;
  }
  if (IsMinimized()) {
    return ui::mojom::WindowShowState::kMinimized;
  }
  return ui::mojom::WindowShowState::kNormal;
}

gfx::Rect CmuxBrowserWindow::GetBounds() const {
  return widget() ? widget()->GetWindowBoundsInScreen() : gfx::Rect();
}

bool CmuxBrowserWindow::IsMaximized() const {
  return widget() && widget()->IsMaximized();
}

bool CmuxBrowserWindow::IsMinimized() const {
  return widget() && widget()->IsMinimized();
}

void CmuxBrowserWindow::Maximize() {
  if (views::Widget* w = widget()) {
    w->Maximize();
  }
}

void CmuxBrowserWindow::Minimize() {
  if (views::Widget* w = widget()) {
    w->Minimize();
  }
}

void CmuxBrowserWindow::Restore() {
  if (views::Widget* w = widget()) {
    w->Restore();
  }
}

bool CmuxBrowserWindow::IsFullscreen() const {
  return widget() && widget()->IsFullscreen();
}

bool CmuxBrowserWindow::IsOnCurrentWorkspace() const {
  return IsVisible();
}

bool CmuxBrowserWindow::IsVisibleOnScreen() const {
  return IsVisible() && widget() && widget()->IsVisible();
}

void CmuxBrowserWindow::SetTopControlsShownRatio(content::WebContents*, float) {}

bool CmuxBrowserWindow::DoBrowserControlsShrinkRendererSize(
    const content::WebContents*) const {
  return false;
}

ui::NativeTheme* CmuxBrowserWindow::GetNativeTheme() {
  return ui::NativeTheme::GetInstanceForNativeUi();
}

const ui::ThemeProvider* CmuxBrowserWindow::GetThemeProvider() const {
  views::View* contents = widget() ? widget()->GetContentsView() : nullptr;
  return contents ? contents->GetThemeProvider() : nullptr;
}

const ui::ColorProvider* CmuxBrowserWindow::GetColorProvider() const {
  views::View* contents = widget() ? widget()->GetContentsView() : nullptr;
  return contents ? contents->GetColorProvider() : nullptr;
}

int CmuxBrowserWindow::GetTopControlsHeight() const { return 0; }
void CmuxBrowserWindow::SetTopControlsGestureScrollInProgress(bool) {}
std::vector<StatusBubble*> CmuxBrowserWindow::GetStatusBubbles() { return {}; }
void CmuxBrowserWindow::UpdateTitleBar() {}
#if !CMUX_BROWSER_WINDOW_HAS_DESKTOP_CAPABILITIES
void CmuxBrowserWindow::BookmarkBarStateChanged(BookmarkBar::AnimateChangeType) {}
void CmuxBrowserWindow::TemporarilyShowBookmarkBar(base::TimeDelta) {}
void CmuxBrowserWindow::UpdateDevTools(content::WebContents*) {}
bool CmuxBrowserWindow::CanDockDevTools() const { return true; }
#endif
void CmuxBrowserWindow::UpdateLoadingAnimations(bool) {}
void CmuxBrowserWindow::SetStarredState(bool) {}
bool CmuxBrowserWindow::IsTabModalPopupDeprecated() const {
  return tab_modal_popup_deprecated_;
}
void CmuxBrowserWindow::SetIsTabModalPopupDeprecated(bool deprecated) {
  tab_modal_popup_deprecated_ = deprecated;
}

void CmuxBrowserWindow::OnActiveTabChanged(
    content::WebContents* old_contents,
    content::WebContents* new_contents,
    int,
    int) {
  if (SidePanelUI* side_panel_ui = browser_->GetFeatures().side_panel_ui()) {
    side_panel_ui->OnActiveTabChanged(old_contents, new_contents,
                                      /*tab_removed_for_deletion=*/false);
  }
  if (host_) {
    host_->OnWorkspaceBrowserActiveTabChanged(workspace_, old_contents,
                                              new_contents);
  }
}

void CmuxBrowserWindow::OnTabDetached(content::WebContents*, bool) {}
#if !CMUX_BROWSER_WINDOW_HAS_DESKTOP_CAPABILITIES
void CmuxBrowserWindow::ZoomChangedForActiveTab(bool) {}
bool CmuxBrowserWindow::ShouldHideUIForFullscreen() const { return false; }
bool CmuxBrowserWindow::IsFullscreenBubbleVisible() const { return false; }
bool CmuxBrowserWindow::IsForceFullscreen() const { return force_fullscreen_; }
void CmuxBrowserWindow::SetForceFullscreen(bool force_fullscreen) {
  force_fullscreen_ = force_fullscreen;
}
#endif

gfx::Size CmuxBrowserWindow::GetContentsSize() const {
  return widget() ? widget()->GetClientAreaBoundsInScreen().size() : gfx::Size();
}

void CmuxBrowserWindow::SetContentsSize(const gfx::Size& size) {
  if (views::Widget* w = widget()) {
    w->SetSize(size);
  }
}

void CmuxBrowserWindow::UpdatePageActionIcon(PageActionIconType) {
  // LocationBarView::Update refreshes every legacy and migrated page action as
  // well as content-setting icons. A cmux workspace can display selected web
  // tabs in several panes at once, and this callback does not identify which
  // WebContents changed, so refresh every location bar users can currently
  // see instead of only the Browser's active tab.
  TabStripModel* tabs = browser_->tab_strip_model();
  for (int index = 0; index < tabs->count(); ++index) {
    content::WebContents* contents = tabs->GetWebContentsAt(index);
    if (!IsWebContentsVisible(contents)) {
      continue;
    }
    if (LocationBar* location_bar =
            GetLocationBarForWebContents(contents)) {
      location_bar->Update(contents);
    }
  }
}
autofill::AutofillBubbleHandler* CmuxBrowserWindow::GetAutofillBubbleHandler() {
  return nullptr;
}
void CmuxBrowserWindow::ExecutePageActionIconForTesting(PageActionIconType) {}
LocationBar* CmuxBrowserWindow::GetLocationBar() const {
  return GetLocationBarForWebContents(nullptr);
}
void CmuxBrowserWindow::SetFocusToLocationBar(bool) {
  if (host_) {
    host_->FocusWorkspaceLocationBar(workspace_);
  }
}
void CmuxBrowserWindow::UpdateReloadStopState(bool, bool) {}
void CmuxBrowserWindow::UpdateToolbar(content::WebContents* contents) {
  if (LocationBar* location_bar = GetLocationBarForWebContents(contents)) {
    location_bar->Update(contents);
  }
}
bool CmuxBrowserWindow::UpdateToolbarSecurityState() {
  LocationBar* location_bar = GetLocationBar();
  if (!location_bar || !location_bar->HasSecurityStateChanged()) {
    return false;
  }
  location_bar->Update(nullptr);
  return true;
}
void CmuxBrowserWindow::UpdateCustomTabBarVisibility(bool, bool) {}
#if !CMUX_BROWSER_WINDOW_HAS_DESKTOP_CAPABILITIES
void CmuxBrowserWindow::SetDevToolsScrimVisibility(bool) {}
#endif
void CmuxBrowserWindow::ResetToolbarTabState(content::WebContents* contents) {
  if (LocationBar* location_bar = GetLocationBarForWebContents(contents)) {
    location_bar->ResetTabState(contents);
  }
}
void CmuxBrowserWindow::FocusToolbar() {
  if (host_) {
    host_->FocusWorkspaceLocationBar(workspace_);
  }
}
void CmuxBrowserWindow::ToolbarSizeChanged(bool) {}
void CmuxBrowserWindow::TabDraggingStatusChanged(bool) {}
void CmuxBrowserWindow::LinkOpeningFromGesture(WindowOpenDisposition) {}
void CmuxBrowserWindow::FocusAppMenu() {
  if (host_) {
    host_->FocusWorkspaceAppMenu(workspace_);
  }
}
#if !CMUX_BROWSER_WINDOW_HAS_DESKTOP_CAPABILITIES
void CmuxBrowserWindow::FocusBookmarksToolbar() {}
void CmuxBrowserWindow::FocusInactivePopupForAccessibility() {}
void CmuxBrowserWindow::RotatePaneFocus(bool) {}
void CmuxBrowserWindow::FocusWebContentsPane() {
  if (host_) {
    host_->FocusWorkspaceContents(workspace_);
  }
}
bool CmuxBrowserWindow::IsBookmarkBarVisible() const { return false; }
bool CmuxBrowserWindow::IsBookmarkBarAnimating() const { return false; }
#endif
bool CmuxBrowserWindow::IsTabStripEditable() const { return true; }
void CmuxBrowserWindow::DisableTabStripEditingForTesting() {}
bool CmuxBrowserWindow::IsToolbarVisible() const { return true; }
bool CmuxBrowserWindow::IsToolbarShowing() const { return true; }
bool CmuxBrowserWindow::IsLocationBarVisible() const { return true; }
#if !CMUX_BROWSER_WINDOW_HAS_DESKTOP_CAPABILITIES
SharingDialog* CmuxBrowserWindow::ShowSharingDialog(content::WebContents*,
                                                     SharingDialogData) {
  return nullptr;
}
#endif
void CmuxBrowserWindow::ShowUpdateChromeDialog() {}
#if !BUILDFLAG(IS_ANDROID)
void CmuxBrowserWindow::ShowIntentPickerBubble(
    std::vector<apps::IntentPickerAppInfo>,
    bool,
    bool,
    apps::IntentPickerBubbleType,
    const std::optional<url::Origin>&,
    IntentPickerResponse) {}
#endif
void CmuxBrowserWindow::ShowBookmarkBubble(const GURL&, bool) {}
#if !CMUX_BROWSER_WINDOW_HAS_DESKTOP_CAPABILITIES
#if !BUILDFLAG(IS_ANDROID)
sharing_hub::ScreenshotCapturedBubble*
CmuxBrowserWindow::ShowScreenshotCapturedBubble(content::WebContents*,
                                                const gfx::Image&) {
  return nullptr;
}
#endif
qrcode_generator::QRCodeGeneratorBubbleView*
CmuxBrowserWindow::ShowQRCodeGeneratorBubble(content::WebContents*,
                                             const GURL&,
                                             bool) {
  return nullptr;
}
send_tab_to_self::SendTabToSelfBubbleView*
CmuxBrowserWindow::ShowSendTabToSelfDevicePickerBubble(content::WebContents*) {
  return nullptr;
}
send_tab_to_self::SendTabToSelfBubbleView*
CmuxBrowserWindow::ShowSendTabToSelfPromoBubble(content::WebContents*, bool) {
  return nullptr;
}
#endif
#if BUILDFLAG(IS_CHROMEOS)
void CmuxBrowserWindow::ToggleMultitaskMenu() {}
#elif !CMUX_BROWSER_WINDOW_HAS_DESKTOP_CAPABILITIES
sharing_hub::SharingHubBubbleView* CmuxBrowserWindow::ShowSharingHubBubble(
    share::ShareAttempt) {
  return nullptr;
}
#endif
ShowTranslateBubbleResult CmuxBrowserWindow::ShowTranslateBubble(
    content::WebContents*,
    translate::TranslateStep,
    const std::string&,
    const std::string&,
    translate::TranslateErrors,
    bool) {
  return ShowTranslateBubbleResult::kBrowserWindowNotValid;
}
#if !CMUX_BROWSER_WINDOW_HAS_DESKTOP_CAPABILITIES
void CmuxBrowserWindow::StartPartialTranslate(const std::string&,
                                              const std::string&,
                                              const std::u16string&) {}
#endif
DownloadBubbleUIController* CmuxBrowserWindow::GetDownloadBubbleUIController() {
  return nullptr;
}
void CmuxBrowserWindow::ConfirmBrowserCloseWithPendingDownloads(
    int,
    Browser::DownloadCloseType,
    base::OnceCallback<void(bool)> callback) {
  std::move(callback).Run(true);
}
void CmuxBrowserWindow::ShowAppMenu() {
  if (host_) {
    host_->FocusWorkspaceAppMenu(workspace_);
  }
}
void CmuxBrowserWindow::PreHandleDragUpdate(const content::DropData&,
                                            const gfx::PointF&) {}
void CmuxBrowserWindow::PreHandleDragExit() {}
void CmuxBrowserWindow::HandleDragEnded() {}
content::KeyboardEventProcessingResult
CmuxBrowserWindow::PreHandleKeyboardEvent(
    const input::NativeWebKeyboardEvent& event) {
  return host_ ? host_->PreHandleWorkspaceKeyboardEvent(workspace_, event)
               : content::KeyboardEventProcessingResult::NOT_HANDLED;
}
bool CmuxBrowserWindow::HandleKeyboardEvent(
    const input::NativeWebKeyboardEvent& event) {
  return host_ && host_->HandleWorkspaceKeyboardEvent(workspace_, event);
}
std::unique_ptr<FindBar> CmuxBrowserWindow::CreateFindBar() { return nullptr; }
web_modal::WebContentsModalDialogHost*
CmuxBrowserWindow::GetWebContentsModalDialogHost() {
  content::WebContents* active =
      browser_ ? browser_->tab_strip_model()->GetActiveWebContents() : nullptr;
  return host_ ? host_->GetWorkspaceWebContentsModalDialogHost(workspace_,
                                                               active)
               : nullptr;
}
web_modal::WebContentsModalDialogHost*
CmuxBrowserWindow::GetWebContentsModalDialogHostFor(
    content::WebContents* web_contents) {
  return host_ ? host_->GetWorkspaceWebContentsModalDialogHost(
                     workspace_, web_contents)
               : nullptr;
}
void CmuxBrowserWindow::ShowAvatarBubbleFromAvatarButton(bool) {}
void CmuxBrowserWindow::MaybeShowProfileSwitchIPH() {}
void CmuxBrowserWindow::MaybeShowSupervisedUserProfileSignInIPH() {}
#if BUILDFLAG(IS_CHROMEOS) || BUILDFLAG(IS_MAC) || BUILDFLAG(IS_WIN) || \
    BUILDFLAG(IS_LINUX)
void CmuxBrowserWindow::ShowHatsDialog(
    const std::string&,
    const std::optional<std::string>&,
    const std::optional<uint64_t>,
    base::OnceClosure,
    base::OnceClosure failure_callback,
    const SurveyBitsData&,
    const SurveyStringData&) {
  if (failure_callback) {
    std::move(failure_callback).Run();
  }
}
void CmuxBrowserWindow::ShowIncognitoClearBrowsingDataDialog() {}
void CmuxBrowserWindow::ShowIncognitoHistoryDisclaimerDialog() {}
#endif
ExclusiveAccessContext* CmuxBrowserWindow::GetExclusiveAccessContext() {
  return this;
}
std::string CmuxBrowserWindow::GetWorkspace() const {
  const std::string durable = CmuxDurableWorkspaceToken(
      CmuxDurableWorkspaceAddress{window_group_, workspace_key_});
  return durable.empty() ? CmuxWorkspaceToken(native_window_, workspace_)
                         : durable;
}
bool CmuxBrowserWindow::IsVisibleOnAllWorkspaces() const { return false; }
void CmuxBrowserWindow::ShowEmojiPanel() {}
std::unique_ptr<content::EyeDropper> CmuxBrowserWindow::OpenEyeDropper(
    content::RenderFrameHost*,
    content::EyeDropperListener*) {
  return nullptr;
}
void CmuxBrowserWindow::ShowCaretBrowsingDialog() {}
void CmuxBrowserWindow::CreateTabSearchBubble() {}
void CmuxBrowserWindow::CloseTabSearchBubble() {}
bool CmuxBrowserWindow::IsUnframedModeEnabled() const { return true; }
bool CmuxBrowserWindow::GetCanResize() { return true; }
ui::mojom::WindowShowState CmuxBrowserWindow::GetWindowShowState() const {
  return GetRestoredState();
}
void CmuxBrowserWindow::ShowChromeLabs() {}
BrowserView* CmuxBrowserWindow::AsBrowserView() { return nullptr; }
void CmuxBrowserWindow::DeleteBrowserWindow() { delete this; }

Profile* CmuxBrowserWindow::GetProfile() {
  return browser_ ? browser_->profile() : nullptr;
}

void CmuxBrowserWindow::EnterFullscreen(
    const url::Origin&,
    ExclusiveAccessBubbleType,
    FullscreenTabParams) {
  if (views::Widget* w = widget()) {
    w->SetFullscreen(true);
  }
}

void CmuxBrowserWindow::ExitFullscreen() {
  if (views::Widget* w = widget()) {
    w->SetFullscreen(false);
  }
}

void CmuxBrowserWindow::UpdateExclusiveAccessBubble(
    const ExclusiveAccessBubbleParams&,
    ExclusiveAccessBubbleHideCallback first_hide_callback) {
  if (first_hide_callback) {
    std::move(first_hide_callback)
        .Run(ExclusiveAccessBubbleHideReason::kNotShown);
  }
}

bool CmuxBrowserWindow::IsExclusiveAccessBubbleDisplayed() const {
  return false;
}

void CmuxBrowserWindow::OnExclusiveAccessUserInput() {}

content::WebContents*
CmuxBrowserWindow::GetWebContentsForExclusiveAccess() {
  return browser_ ? browser_->tab_strip_model()->GetActiveWebContents()
                  : nullptr;
}

bool CmuxBrowserWindow::CanUserEnterFullscreen() const {
  return true;
}

bool CmuxBrowserWindow::CanUserExitFullscreen() const {
  return true;
}

std::unique_ptr<BrowserWindow, BrowserWindowDeleter>
MaybeCreateCmuxBrowserWindow(Browser* browser) {
  if (!browser) {
    return nullptr;
  }
  std::optional<WorkspaceLocator> locator;
  if (const std::optional<CmuxDurableWorkspaceAddress> durable =
          CmuxDurableWorkspaceFromToken(browser->initial_workspace())) {
    const auto stable = DurableHosts().find(*durable);
    if (stable != DurableHosts().end()) {
      locator = stable->second;
    }
  } else {
    locator = WorkspaceFromToken(browser->initial_workspace());
  }
  if (locator && !Hosts().contains(*locator)) {
    // A v1 token from another process or a v2 token whose host has not been
    // restored must never attach by recycled runtime IDs. Treat it like an
    // unassigned Browser so the active host creates a fresh canonical UUID.
    locator.reset();
  }
  if (!locator) {
    if (!browser->is_type_normal() || !FactoryHost()) {
      LOG(WARNING) << "cmux-browser-window: native BrowserView fallback type="
                   << browser->type()
                   << " factory_host=" << static_cast<bool>(FactoryHost());
      return nullptr;
    }
    LOG(WARNING) << "cmux-browser-window: adopting Chrome-created Browser";
    std::optional<WorkspaceId> workspace =
        FactoryHost()->AdoptExternalBrowser(browser);
    if (!workspace) {
      LOG(WARNING) << "cmux-browser-window: host declined Browser adoption";
      return nullptr;
    }
    // Chrome-created Browsers are adopted by the active factory host. Locate
    // the host's newly registered composite workspace rather than guessing
    // from a process-global local WorkspaceId.
    for (const auto& [candidate, record] : Hosts()) {
      if (candidate.local_workspace == *workspace &&
          record.host.get() == FactoryHost().get()) {
        locator = candidate;
        break;
      }
    }
  }
  if (!locator) {
    return nullptr;
  }
  auto it = Hosts().find(*locator);
  if (it == Hosts().end() || !it->second.host) {
    return nullptr;
  }
  return std::unique_ptr<BrowserWindow, BrowserWindowDeleter>(
      new CmuxBrowserWindow(browser, locator->window,
                            locator->local_workspace,
                            it->second.durable.window_group,
                            it->second.durable.workspace_key,
                            it->second.host));
}

void RegisterCmuxBrowserWindowHost(
    NativeWindowId native_window,
    WorkspaceId workspace,
    std::string window_group,
    std::string workspace_key,
    base::WeakPtr<CmuxBrowserWindowHost> host) {
  CHECK_NE(native_window, kInvalidNativeWindowId);
  CHECK_NE(workspace, kInvalidId);
  CHECK(host);
  CmuxDurableWorkspaceAddress durable{std::move(window_group),
                                      std::move(workspace_key)};
  CHECK(!CmuxDurableWorkspaceToken(durable).empty());
  const WorkspaceLocator locator{native_window, workspace};
  CHECK(!Hosts().contains(locator));
  CHECK(!DurableHosts().contains(durable));
  DurableHosts().emplace(durable, locator);
  Hosts().emplace(locator, HostRecord{std::move(host), std::move(durable)});
}

void UnregisterCmuxBrowserWindowHost(NativeWindowId native_window,
                                     WorkspaceId workspace) {
  const WorkspaceLocator locator{native_window, workspace};
  const auto existing = Hosts().find(locator);
  if (existing == Hosts().end()) {
    return;
  }
  DurableHosts().erase(existing->second.durable);
  Hosts().erase(existing);
}

void RegisterCmuxBrowserWindowFactoryHost(
    base::WeakPtr<CmuxBrowserWindowHost> host) {
  CHECK(host);
  FactoryHost() = std::move(host);
  LOG(WARNING) << "cmux-browser-window: factory host registered";
}

void UnregisterCmuxBrowserWindowFactoryHost(CmuxBrowserWindowHost* host) {
  if (FactoryHost().get() == host) {
    LOG(WARNING) << "cmux-browser-window: factory host unregistered";
    FactoryHost().reset();
  }
}

std::string CmuxWorkspaceToken(NativeWindowId native_window,
                               WorkspaceId workspace) {
  return std::string(kWorkspacePrefix) + base::NumberToString(native_window) +
         ":" + base::NumberToString(workspace);
}

}  // namespace cmux
