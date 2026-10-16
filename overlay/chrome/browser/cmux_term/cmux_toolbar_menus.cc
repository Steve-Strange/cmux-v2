// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "chrome/browser/cmux_term/cmux_toolbar_menus.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/scoped_observation.h"
#include "base/task/single_thread_task_runner.h"
#include "build/build_config.h"
#include "chrome/browser/cmux_term/cmux_browser_finder.h"
#include "chrome/browser/download/bubble/download_bubble_ui_controller.h"
#include "chrome/browser/cmux_term/cmux_extensions_container.h"
#include "chrome/browser/cmux_term/cmux_strip_controller.h"
#include "chrome/browser/cmux_term/cmux_views.h"
#include "chrome/browser/lifetime/application_lifetime.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/signin/signin_promo_util.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/chrome_pages.h"
#include "chrome/browser/ui/download/download_bubble_contents_view_info.h"
#include "chrome/browser/ui/views/chrome_layout_provider.h"
#include "chrome/browser/ui/views/download/bubble/download_bubble_contents_view.h"
#include "chrome/browser/ui/views/download/bubble/download_bubble_navigation_handler.h"
#include "chrome/browser/ui/views/profiles/profile_menu_view.h"
#include "chrome/browser/ui/views/toolbar/toolbar_button.h"
#include "chrome/app/vector_icons/vector_icons.h"
#include "chrome/common/chrome_version.h"
#include "components/vector_icons/vector_icons.h"
#include "components/zoom/page_zoom.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/web_contents.h"
#include "content/public/common/page_zoom.h"
#include "content/public/common/referrer.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/base/models/menu_separator_types.h"
#include "ui/base/mojom/menu_source_type.mojom.h"
#include "ui/base/page_transition_types.h"
#include "ui/menus/simple_menu_model.h"
#include "ui/views/accessibility/view_accessibility.h"
#include "ui/views/bubble/bubble_dialog_delegate_view.h"
#include "ui/views/bubble/bubble_anchor.h"
#include "ui/views/controls/button/button_controller.h"
#include "ui/views/controls/button/menu_button_controller.h"
#include "ui/views/controls/button/md_text_button.h"
#include "ui/views/controls/label.h"
#include "ui/views/controls/menu/menu_runner.h"
#include "ui/views/controls/menu/menu_types.h"
#include "ui/views/interaction/element_tracker_views.h"
#include "ui/views/layout/box_layout_view.h"
#include "ui/views/vector_icons.h"
#include "ui/views/view_tracker.h"
#include "ui/views/widget/widget.h"
#include "ui/views/widget/widget_observer.h"
#include "url/gurl.h"

namespace cmux {
namespace {

#if CHROME_VERSION_MAJOR >= 150
const gfx::VectorIcon& kCmuxExtensionIcon =
    vector_icons::kExtensionChromeRefreshOldIcon;
const gfx::VectorIcon& kCmuxDownloadIcon =
    vector_icons::kFileDownloadChromeRefreshOldIcon;
const gfx::VectorIcon& kCmuxProfileIcon =
    vector_icons::kAccountCircleChromeRefreshOldIcon;
#else
const gfx::VectorIcon& kCmuxExtensionIcon =
    vector_icons::kExtensionChromeRefreshIcon;
const gfx::VectorIcon& kCmuxDownloadIcon =
    vector_icons::kFileDownloadChromeRefreshIcon;
const gfx::VectorIcon& kCmuxProfileIcon =
    vector_icons::kAccountCircleChromeRefreshIcon;
#endif

constexpr int kAppCommandNewTab = 1;
constexpr int kAppCommandNewTerminalTab = 2;
constexpr int kAppCommandNewColumn = 3;
constexpr int kAppCommandHistory = 4;
constexpr int kAppCommandDownloads = 5;
constexpr int kAppCommandBookmarks = 6;
constexpr int kAppCommandManageExtensions = 7;
constexpr int kAppCommandChromeWebStore = 8;
constexpr int kAppCommandZoomIn = 9;
constexpr int kAppCommandZoomOut = 10;
constexpr int kAppCommandZoomReset = 11;
constexpr int kAppCommandAboutCmux = 12;
constexpr int kAppCommandExtensionsSubmenu = 13;
constexpr int kAppCommandNewWorkspace = 14;
constexpr int kAppCommandNewWindow = 15;
constexpr int kAppCommandNewIncognitoWindow = 16;
constexpr int kAppCommandConfigureCmux = 17;
constexpr int kAppCommandSettings = 18;
constexpr int kAppCommandPasswordManager = 19;
constexpr int kAppCommandPaymentMethods = 20;
constexpr int kAppCommandAddresses = 21;
constexpr int kAppCommandPasswordsSubmenu = 22;
constexpr int kAppCommandClearBrowsingData = 23;
constexpr int kAppCommandFullscreen = 24;
constexpr int kAppCommandPrint = 25;
constexpr int kAppCommandFind = 26;
constexpr int kAppCommandDeveloperTools = 27;
constexpr int kAppCommandTaskManager = 28;
constexpr int kAppCommandSavePage = 29;
constexpr int kAppCommandMoreToolsSubmenu = 30;
constexpr int kAppCommandHelpSubmenu = 31;
constexpr int kAppCommandHelpCenter = 32;
constexpr int kAppCommandExit = 33;
#if BUILDFLAG(IS_WIN)
constexpr int kAppCommandWindowsShellsSubmenu = 100;
constexpr int kAppCommandShellCmd = 101;
constexpr int kAppCommandShellWindowsPowerShell = 102;
constexpr int kAppCommandShellPowerShell = 103;
constexpr int kAppCommandShellWsl = 104;
constexpr int kAppCommandShellGitBash = 105;
constexpr int kAppCommandShellMsys2Ucrt64 = 106;
constexpr int kAppCommandShellMsys2Mingw64 = 107;
#endif

// These entries belong in a full browser menu, but their upstream command
// implementations require UI which CmuxBrowserWindow deliberately does not
// expose yet (BrowserView, FindBar, print preview anchoring, or a second native
// host). Keep them visible so the missing surface is explicit, and disabled so
// selecting one cannot follow a null BrowserWindow stub into a crash.
constexpr bool IsSafeAppCommandEnabled(int command_id) {
  switch (command_id) {
    case kAppCommandNewIncognitoWindow:
    case kAppCommandPrint:
    case kAppCommandFind:
    case kAppCommandTaskManager:
    case kAppCommandSavePage:
      return false;
    default:
      return true;
  }
}

static_assert(IsSafeAppCommandEnabled(kAppCommandNewWindow));
static_assert(!IsSafeAppCommandEnabled(kAppCommandNewIncognitoWindow));
static_assert(!IsSafeAppCommandEnabled(kAppCommandPrint));
static_assert(!IsSafeAppCommandEnabled(kAppCommandFind));
static_assert(IsSafeAppCommandEnabled(kAppCommandNewWorkspace));
static_assert(IsSafeAppCommandEnabled(kAppCommandExit));

void LoadURLInPane(content::WebContents* web_contents, const GURL& url) {
  if (!web_contents || !url.is_valid()) {
    return;
  }
  web_contents->GetController().LoadURL(url, content::Referrer(),
                                        ui::PAGE_TRANSITION_AUTO_TOPLEVEL,
                                        std::string());
}

base::WeakPtr<Browser> FindBrowserWeak(content::WebContents* web_contents) {
  Browser* browser = cmux::FindBrowserWithTab(web_contents);
  return browser ? browser->AsWeakPtr() : base::WeakPtr<Browser>();
}

class CmuxToolbarMenuButton : public ToolbarButton,
                              public ui::SimpleMenuModel::Delegate {
  METADATA_HEADER(CmuxToolbarMenuButton, ToolbarButton)

 public:
  explicit CmuxToolbarMenuButton(PressedCallback callback)
      : ToolbarButton(std::move(callback)) {}
  CmuxToolbarMenuButton(const CmuxToolbarMenuButton&) = delete;
  CmuxToolbarMenuButton& operator=(const CmuxToolbarMenuButton&) = delete;
  ~CmuxToolbarMenuButton() override = default;

  bool IsCommandIdEnabled(int command_id) const override { return true; }

 protected:
  ui::SimpleMenuModel* AddSubmenuModel() {
    submenu_models_.push_back(std::make_unique<ui::SimpleMenuModel>(this));
    return submenu_models_.back().get();
  }

  void RunMenu(std::unique_ptr<ui::SimpleMenuModel> model) {
    if (!GetWidget()) {
      return;
    }
    model_ = std::move(model);
    runner_ = std::make_unique<views::MenuRunner>(
        model_.get(), views::MenuRunner::HAS_MNEMONICS);
    runner_->RunMenuAt(GetWidget(), nullptr, GetBoundsInScreen(),
                       views::MenuAnchorPosition::kTopRight,
                       ui::mojom::MenuSourceType::kMouse);
  }

  void ResetMenuState() {
    runner_.reset();
    model_.reset();
    submenu_models_.clear();
  }

 private:
  std::unique_ptr<ui::SimpleMenuModel> model_;
  std::vector<std::unique_ptr<ui::SimpleMenuModel>> submenu_models_;
  std::unique_ptr<views::MenuRunner> runner_;
};

BEGIN_METADATA(CmuxToolbarMenuButton)
END_METADATA

class CmuxExtensionsMenuButton : public ToolbarButton,
                                 public views::WidgetObserver {
  METADATA_HEADER(CmuxExtensionsMenuButton, ToolbarButton)

 public:
  explicit CmuxExtensionsMenuButton(content::WebContents* web_contents)
      : container_(GetCmuxExtensionsContainerForBrowser(
            cmux::FindBrowserWithTab(web_contents))) {
    auto menu_button_controller =
        std::make_unique<views::MenuButtonController>(
            this,
            base::BindRepeating(
                &CmuxExtensionsMenuButton::ToggleExtensionsMenu,
                base::Unretained(this)),
            std::make_unique<
                views::Button::DefaultButtonControllerDelegate>(this));
    menu_button_controller_ = menu_button_controller.get();
    SetButtonController(std::move(menu_button_controller));
    button_controller()->set_notify_action(
        views::ButtonController::NotifyAction::kOnPress);

    SetVectorIcon(kCmuxExtensionIcon);
    SetTooltipText(u"Extensions");
    GetViewAccessibility().SetName(u"Extensions");
    if (container_) {
      container_->SetMenuButtonAnchor(this);
    }
  }
  CmuxExtensionsMenuButton(const CmuxExtensionsMenuButton&) = delete;
  CmuxExtensionsMenuButton& operator=(const CmuxExtensionsMenuButton&) = delete;
  ~CmuxExtensionsMenuButton() override {
    if (menu_widget_) {
      menu_widget_->CloseNow();
    }
  }

  void VisibilityChanged(views::View* starting_from, bool is_visible) override {
    if (!is_visible && container_) {
      container_->CloseExtensionsMenuIfOpen();
    }
  }

  void OnWidgetDestroying(views::Widget* widget) override {
    menu_observation_.Reset();
    menu_widget_.reset();
    pressed_lock_.reset();
  }

 private:
  void ToggleExtensionsMenu() {
    if (!container_) {
      return;
    }
    container_->SetMenuButtonAnchor(this);
    if (menu_widget_) {
      container_->ToggleExtensionsMenu();
      return;
    }

    pressed_lock_ = menu_button_controller_->TakeLock();
    container_->ToggleExtensionsMenu();
    views::Widget* widget = container_->GetMenuWidgetForTesting();
    if (!widget) {
      pressed_lock_.reset();
      return;
    }
    menu_widget_ = widget->GetWeakPtr();
    menu_observation_.Observe(widget);
  }

  raw_ptr<CmuxExtensionsContainer> container_ = nullptr;
  raw_ptr<views::MenuButtonController> menu_button_controller_ = nullptr;
  std::unique_ptr<views::MenuButtonController::PressedLock> pressed_lock_;
  base::WeakPtr<views::Widget> menu_widget_;
  base::ScopedObservation<views::Widget, views::WidgetObserver>
      menu_observation_{this};
};

BEGIN_METADATA(CmuxExtensionsMenuButton)
END_METADATA

gfx::Insets DownloadBubbleMargin() {
  return gfx::Insets::VH(ChromeLayoutProvider::Get()->GetDistanceMetric(
                             views::DISTANCE_RELATED_CONTROL_VERTICAL),
                         0);
}

// Hosts Chromium's real DownloadBubbleContentsView without requiring a
// BrowserView. cmux has a real Browser/DownloadManager, but its native window
// is a CmuxBrowserWindow, so DownloadToolbarUIController cannot be used (that
// controller is intentionally coupled to BrowserView's pinned toolbar).
class CmuxDownloadsButton : public ToolbarButton,
                            public DownloadBubbleNavigationHandler {
  METADATA_HEADER(CmuxDownloadsButton, ToolbarButton)

 public:
  CmuxDownloadsButton(content::WebContents* web_contents,
                      base::RepeatingClosure on_activate_surface)
      : ToolbarButton(base::BindRepeating(
            &CmuxDownloadsButton::ToggleBubble, base::Unretained(this))),
        browser_(FindBrowserWeak(web_contents)),
        on_activate_surface_(std::move(on_activate_surface)),
        bubble_controller_(browser_ ? std::make_unique<DownloadBubbleUIController>(
                                          browser_.get())
                                    : nullptr),
        weak_factory_(this) {
    SetVectorIcon(kCmuxDownloadIcon);
    SetTooltipText(u"Downloads");
    GetViewAccessibility().SetName(u"Downloads");
  }
  CmuxDownloadsButton(const CmuxDownloadsButton&) = delete;
  CmuxDownloadsButton& operator=(const CmuxDownloadsButton&) = delete;
  ~CmuxDownloadsButton() override { CloseBubbleNow(); }

  // DownloadBubbleNavigationHandler:
  void OpenPrimaryDialog() override {
    if (!bubble_contents_ || !bubble_delegate_) {
      return;
    }
    bubble_contents_->ShowPrimaryPage();
    bubble_delegate_->set_margins(DownloadBubbleMargin());
  }

  void OpenSecurityDialog(
      const offline_items_collection::ContentId& content_id) override {
    if (bubble_contents_ && bubble_delegate_) {
      bubble_contents_->ShowSecurityPage(content_id);
      bubble_delegate_->set_margins(DownloadBubbleMargin());
    }
  }

  void CloseDialog(views::Widget::ClosedReason reason) override {
    if (bubble_delegate_ && bubble_delegate_->GetWidget()) {
      bubble_delegate_->GetWidget()->CloseWithReason(reason);
    }
  }

  void OnDialogInteracted() override {}
  void OnSecurityDialogButtonPress(const DownloadUIModel& model,
                                   DownloadCommands::Command command) override {
  }

  std::unique_ptr<CloseOnDeactivatePin> PreventDialogCloseOnDeactivate()
      override {
    return bubble_delegate_ ? bubble_delegate_->PreventCloseOnDeactivate()
                            : nullptr;
  }

  base::WeakPtr<DownloadBubbleNavigationHandler> GetWeakPtr() override {
    return weak_factory_.GetWeakPtr();
  }

 private:
  void ToggleBubble() {
    if (on_activate_surface_) {
      on_activate_surface_.Run();
    }
#if CHROME_VERSION_MAJOR >= 151
    // The M151 widget is client-owned and may outlive the delegate's
    // WindowClosing callback. Do not replace its delegate until the matching
    // synchronous close callback has destroyed the widget.
    if (bubble_widget_) {
      if (bubble_delegate_) {
        CloseDialog(views::Widget::ClosedReason::kUnspecified);
      }
      return;
    }
#else
    if (bubble_delegate_ && bubble_delegate_->GetWidget()) {
      CloseDialog(views::Widget::ClosedReason::kUnspecified);
      return;
    }
#endif
    if (!browser_ || !bubble_controller_ || !GetWidget()) {
      return;
    }

    std::vector<DownloadUIModel::DownloadUIModelPtr> models =
        bubble_controller_->GetMainView();
    if (models.empty()) {
      ShowEmptyBubble();
      return;
    }

    auto delegate = MakeBubbleDelegate();
    auto contents = std::make_unique<DownloadBubbleContentsView>(
        browser_->AsWeakPtr(), bubble_controller_->GetWeakPtr(), GetWeakPtr(),
#if CHROME_VERSION_MAJOR >= 151
        DownloadBubbleMode::kComplete,
#else
        /*primary_view_is_partial_view=*/false,
#endif
        std::make_unique<DownloadBubbleContentsViewInfo>(std::move(models)),
        delegate.get());
    bubble_contents_ = contents.get();
    delegate->SetContentsView(std::move(contents));
    delegate->set_margins(DownloadBubbleMargin());
    delegate->SetEnableArrowKeyTraversal(true);
    ShowBubble(std::move(delegate));
  }

  std::unique_ptr<views::BubbleDialogDelegate> MakeBubbleDelegate() {
    auto delegate = std::make_unique<views::BubbleDialogDelegate>(
        this, views::BubbleBorder::TOP_RIGHT,
        views::BubbleBorder::DIALOG_SHADOW,
        /*autosize=*/true);
    delegate->SetShowTitle(false);
    delegate->SetShowCloseButton(false);
    delegate->SetButtons(static_cast<int>(ui::mojom::DialogButton::kNone));
    delegate->RegisterWindowClosingCallback(base::BindOnce(
        &CmuxDownloadsButton::OnBubbleClosing, weak_factory_.GetWeakPtr()));
    return delegate;
  }

  void ShowBubble(std::unique_ptr<views::BubbleDialogDelegate> delegate) {
#if CHROME_VERSION_MAJOR >= 151
    bubble_delegate_owner_ = std::move(delegate);
    bubble_delegate_ = bubble_delegate_owner_.get();
    bubble_widget_ = views::BubbleDialogDelegate::CreateBubble(
        bubble_delegate_,
        base::BindOnce(&CmuxDownloadsButton::OnM151BubbleClosed,
                       weak_factory_.GetWeakPtr()));
    if (!bubble_widget_) {
      bubble_contents_ = nullptr;
      bubble_delegate_ = nullptr;
      bubble_delegate_owner_.reset();
      return;
    }
    bubble_widget_->Show();
#else
    bubble_delegate_ = delegate.get();
#if CHROME_VERSION_MAJOR >= 150
    views::Widget* widget =
        views::BubbleDialogDelegate::CreateBubbleDeprecated(
            std::move(delegate),
            views::Widget::InitParams::NATIVE_WIDGET_OWNS_WIDGET);
#else
    views::Widget* widget =
        views::BubbleDialogDelegate::CreateBubble(std::move(delegate));
#endif
    if (widget) {
      widget->Show();
    }
#endif
  }

  void ShowEmptyBubble() {
    auto delegate = MakeBubbleDelegate();
    auto contents = std::make_unique<views::BoxLayoutView>();
    contents->SetOrientation(views::BoxLayout::Orientation::kVertical);
    contents->SetInsideBorderInsets(gfx::Insets::VH(16, 20));
    contents->SetBetweenChildSpacing(12);
    auto* title = contents->AddChildView(
        std::make_unique<views::Label>(u"Downloads"));
    title->SetTextStyle(views::style::STYLE_HEADLINE_4);
    title->SetHorizontalAlignment(gfx::ALIGN_LEFT);
    auto* empty = contents->AddChildView(
        std::make_unique<views::Label>(u"No recent downloads"));
    empty->SetHorizontalAlignment(gfx::ALIGN_LEFT);
    contents->AddChildView(std::make_unique<views::MdTextButton>(
        base::BindRepeating(&CmuxDownloadsButton::ShowAllDownloads,
                            base::Unretained(this)),
        u"Show all downloads"));
    delegate->SetContentsView(std::move(contents));
    ShowBubble(std::move(delegate));
  }

  void ShowAllDownloads() {
    CloseDialog(views::Widget::ClosedReason::kAcceptButtonClicked);
    if (browser_) {
      chrome::ShowDownloads(browser_.get());
    }
  }

  void OnBubbleClosing() {
    bubble_contents_ = nullptr;
    bubble_delegate_ = nullptr;
  }

#if CHROME_VERSION_MAJOR >= 151
  void OnM151BubbleClosed(views::Widget::ClosedReason) {
    bubble_contents_ = nullptr;
    bubble_delegate_ = nullptr;
    bubble_widget_.reset();
    bubble_delegate_owner_.reset();
  }
#endif

  void CloseBubbleNow() {
#if CHROME_VERSION_MAJOR >= 151
    bubble_contents_ = nullptr;
    bubble_delegate_ = nullptr;
    // The M151 CreateBubble() contract requires the delegate to outlive the
    // client-owned widget.
    bubble_widget_.reset();
    bubble_delegate_owner_.reset();
#else
    if (bubble_delegate_ && bubble_delegate_->GetWidget()) {
      bubble_delegate_->GetWidget()->CloseNow();
    }
    bubble_contents_ = nullptr;
    bubble_delegate_ = nullptr;
#endif
  }

  base::WeakPtr<Browser> browser_;
  base::RepeatingClosure on_activate_surface_;
  std::unique_ptr<DownloadBubbleUIController> bubble_controller_;
  raw_ptr<views::BubbleDialogDelegate> bubble_delegate_ = nullptr;
  raw_ptr<DownloadBubbleContentsView> bubble_contents_ = nullptr;
#if CHROME_VERSION_MAJOR >= 151
  std::unique_ptr<views::BubbleDialogDelegate> bubble_delegate_owner_;
  std::unique_ptr<views::Widget> bubble_widget_;
#endif
  base::WeakPtrFactory<CmuxDownloadsButton> weak_factory_;
};

BEGIN_METADATA(CmuxDownloadsButton)
END_METADATA

// Chromium's ProfileMenuCoordinator discovers its anchor through
// BrowserElements, whose BrowserView initialization is intentionally absent in
// a cmux window. Construct the same upstream ProfileMenuView directly from the
// tracked cmux toolbar button instead.
class CmuxProfileButton : public ToolbarButton {
  METADATA_HEADER(CmuxProfileButton, ToolbarButton)

 public:
  CmuxProfileButton(content::WebContents* web_contents,
                    base::RepeatingClosure on_activate_surface)
      : ToolbarButton(base::BindRepeating(&CmuxProfileButton::ToggleBubble,
                                          base::Unretained(this))),
        browser_(FindBrowserWeak(web_contents)),
        on_activate_surface_(std::move(on_activate_surface)) {
    SetVectorIcon(kCmuxProfileIcon);
    SetTooltipText(u"Profile");
    GetViewAccessibility().SetName(u"Profile");
  }
  CmuxProfileButton(const CmuxProfileButton&) = delete;
  CmuxProfileButton& operator=(const CmuxProfileButton&) = delete;
  ~CmuxProfileButton() override { CloseBubbleNow(); }

 private:
  void ToggleBubble() {
    if (on_activate_surface_) {
      on_activate_surface_.Run();
    }
    if (bubble_tracker_) {
      if (bubble_tracker_.view()->GetWidget()) {
        bubble_tracker_.view()->GetWidget()->Close();
      }
      return;
    }
    if (!browser_ || !GetWidget()) {
      return;
    }
    ui::TrackedElement* anchor =
        views::ElementTrackerViews::GetInstance()->GetElementForView(
            this, /*assign_temporary_id=*/true);
    if (!anchor) {
      return;
    }
#if CHROME_VERSION_MAJOR >= 151
    std::unique_ptr<ProfileMenuViewBase> bubble =
        std::make_unique<ProfileMenuView>(
            views::BubbleAnchor(anchor), browser_.get(),
            signin::ProfileMenuAvatarButtonPromoInfo(),
            /*from_avatar_promo=*/false);
#else
    std::unique_ptr<ProfileMenuViewBase> bubble =
        std::make_unique<ProfileMenuView>(
#if CHROME_VERSION_MAJOR >= 150
            views::BubbleAnchor(anchor), browser_.get(),
#else
            anchor, browser_.get(), signin::ProfileMenuAvatarButtonPromoInfo(),
#endif
#if CHROME_VERSION_MAJOR >= 150
            signin::ProfileMenuAvatarButtonPromoInfo(),
#endif
            /*from_avatar_promo=*/false);
#endif
    ProfileMenuViewBase* bubble_ptr = bubble.get();
    bubble_tracker_.SetView(bubble_ptr);
#if CHROME_VERSION_MAJOR >= 151
    // Keep the profile bubble's historical Widget-owned lifetime explicit on
    // M151, whose ownership-transferring BubbleDialogDelegate factory was
    // renamed when the preferred caller-owned overload was introduced.
    views::Widget* widget =
        views::BubbleDialogDelegate::CreateBubbleDeprecated(
            std::move(bubble),
            views::Widget::InitParams::NATIVE_WIDGET_OWNS_WIDGET);
#else
    views::Widget* widget =
        views::BubbleDialogDelegateView::CreateBubble(std::move(bubble));
#endif
    if (widget) {
      widget->Show();
    }
  }

  void CloseBubbleNow() {
    if (bubble_tracker_ && bubble_tracker_.view()->GetWidget()) {
      bubble_tracker_.view()->GetWidget()->CloseNow();
    }
    bubble_tracker_.SetView(nullptr);
  }

  base::WeakPtr<Browser> browser_;
  base::RepeatingClosure on_activate_surface_;
  views::ViewTracker bubble_tracker_;
};

BEGIN_METADATA(CmuxProfileButton)
END_METADATA

class CmuxAppMenuButton : public CmuxToolbarMenuButton {
  METADATA_HEADER(CmuxAppMenuButton, CmuxToolbarMenuButton)

 public:
  CmuxAppMenuButton(content::WebContents* web_contents,
                    base::RepeatingClosure on_activate_surface)
      : CmuxToolbarMenuButton(base::BindRepeating(&CmuxAppMenuButton::ShowMenu,
                                                  base::Unretained(this))),
        web_contents_(web_contents->GetWeakPtr()),
        on_activate_surface_(std::move(on_activate_surface)),
        weak_factory_(this) {
#if CHROME_VERSION_MAJOR >= 150
    SetVectorIcon(kMoreVertIcon);
#else
    SetVectorIcon(views::kMenuIcon);
#endif
    SetTooltipText(u"Customize and control cmux");
    GetViewAccessibility().SetName(u"Customize and control cmux");
  }
  CmuxAppMenuButton(const CmuxAppMenuButton&) = delete;
  CmuxAppMenuButton& operator=(const CmuxAppMenuButton&) = delete;
  ~CmuxAppMenuButton() override = default;

  bool IsCommandIdEnabled(int command_id) const override {
    return IsSafeAppCommandEnabled(command_id);
  }

  void ExecuteCommand(int command_id, int event_flags) override {
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(&CmuxAppMenuButton::ExecuteMenuCommand,
                                  weak_factory_.GetWeakPtr(), command_id));
  }

 private:
  void ShowMenu() {
    ResetMenuState();
    auto menu = std::make_unique<ui::SimpleMenuModel>(this);
    menu->AddItem(kAppCommandNewTab, u"New Tab");
    menu->AddItem(kAppCommandNewWindow, u"New Window");
    menu->AddItem(kAppCommandNewIncognitoWindow,
                  u"New Incognito Window (coming soon)");
    menu->AddItem(kAppCommandNewWorkspace, u"New Workspace");
    menu->AddSeparator(ui::NORMAL_SEPARATOR);
    menu->AddItem(kAppCommandNewTerminalTab, u"New Terminal Tab");
#if BUILDFLAG(IS_WIN)
    ui::SimpleMenuModel* shells_submenu = AddSubmenuModel();
    shells_submenu->AddItem(kAppCommandShellCmd, u"Command Prompt");
    shells_submenu->AddItem(kAppCommandShellWindowsPowerShell,
                            u"Windows PowerShell");
    shells_submenu->AddItem(kAppCommandShellPowerShell, u"PowerShell 7");
    shells_submenu->AddItem(kAppCommandShellWsl, u"WSL (default distro)");
    shells_submenu->AddItem(kAppCommandShellGitBash, u"Git Bash");
    shells_submenu->AddItem(kAppCommandShellMsys2Ucrt64, u"MSYS2 UCRT64");
    shells_submenu->AddItem(kAppCommandShellMsys2Mingw64, u"MSYS2 MINGW64");
    menu->AddSubMenu(kAppCommandWindowsShellsSubmenu, u"New terminal with",
                     shells_submenu);
#endif
    menu->AddItem(kAppCommandNewColumn, u"New Column");
    menu->AddSeparator(ui::NORMAL_SEPARATOR);
    menu->AddItem(kAppCommandHistory, u"History");
    menu->AddItem(kAppCommandDownloads, u"Downloads");
    menu->AddItem(kAppCommandBookmarks, u"Bookmarks");

    ui::SimpleMenuModel* passwords_submenu = AddSubmenuModel();
    passwords_submenu->AddItem(kAppCommandPasswordManager,
                               u"Google Password Manager");
    passwords_submenu->AddItem(kAppCommandPaymentMethods, u"Payment methods");
    passwords_submenu->AddItem(kAppCommandAddresses,
                               u"Addresses and more");
    menu->AddSubMenu(kAppCommandPasswordsSubmenu, u"Passwords and autofill",
                     passwords_submenu);

    ui::SimpleMenuModel* extensions_submenu = AddSubmenuModel();
    extensions_submenu->AddItem(kAppCommandManageExtensions,
                                u"Manage extensions");
    extensions_submenu->AddItem(kAppCommandChromeWebStore,
                                u"Visit Chrome Web Store");
    menu->AddSubMenu(kAppCommandExtensionsSubmenu, u"Extensions",
                     extensions_submenu);
    menu->AddItem(kAppCommandClearBrowsingData, u"Delete browsing data");

    menu->AddSeparator(ui::NORMAL_SEPARATOR);
    menu->AddItem(kAppCommandZoomIn, u"Zoom In");
    menu->AddItem(kAppCommandZoomOut, u"Zoom Out");
    menu->AddItem(kAppCommandZoomReset, u"Reset Zoom");
    menu->AddItem(kAppCommandFullscreen, u"Fullscreen");
    menu->AddSeparator(ui::NORMAL_SEPARATOR);
    menu->AddItem(kAppCommandPrint, u"Print (coming soon)");
    menu->AddItem(kAppCommandFind, u"Find and edit (coming soon)");

    ui::SimpleMenuModel* more_tools_submenu = AddSubmenuModel();
    more_tools_submenu->AddItem(kAppCommandSavePage,
                                u"Save page as (coming soon)");
    more_tools_submenu->AddItem(kAppCommandTaskManager,
                                u"Task manager (coming soon)");
    more_tools_submenu->AddItem(kAppCommandDeveloperTools,
                                u"Developer tools");
    menu->AddSubMenu(kAppCommandMoreToolsSubmenu, u"More tools",
                     more_tools_submenu);

    menu->AddSeparator(ui::NORMAL_SEPARATOR);
    menu->AddItem(kAppCommandConfigureCmux, u"Configure cmux");
    menu->AddItem(kAppCommandSettings, u"Settings");

    ui::SimpleMenuModel* help_submenu = AddSubmenuModel();
    help_submenu->AddItem(kAppCommandHelpCenter, u"Help Center");
    help_submenu->AddItem(kAppCommandAboutCmux, u"About cmux");
    menu->AddSubMenu(kAppCommandHelpSubmenu, u"Help", help_submenu);

    menu->AddSeparator(ui::NORMAL_SEPARATOR);
    menu->AddItem(kAppCommandExit, u"Exit cmux");
    RunMenu(std::move(menu));
  }

  void ExecuteMenuCommand(int command_id) {
    switch (command_id) {
      case kAppCommandNewTab:
        ActivateSurface();
        if (CmuxStripController* controller = GetStripController()) {
          controller->NewTab(SurfaceKind::kWeb);
        }
        break;
      case kAppCommandNewWorkspace:
        if (CmuxStripController* controller = GetStripController()) {
          controller->NewWorkspace();
        }
        break;
      case kAppCommandNewWindow:
        ShowNewViewsWebWindow();
        break;
      case kAppCommandNewIncognitoWindow:
        // Incognito needs a separately partitioned native window/profile.
        // Keep disabled until that profile-selection path is wired.
        break;
      case kAppCommandNewTerminalTab:
        ActivateSurface();
        if (CmuxStripController* controller = GetStripController()) {
          controller->NewTab(SurfaceKind::kTerminal);
        }
        break;
#if BUILDFLAG(IS_WIN)
      case kAppCommandShellCmd:
        OpenWindowsShell("cmd.exe", "Command Prompt");
        break;
      case kAppCommandShellWindowsPowerShell:
        OpenWindowsShell("powershell.exe -NoLogo", "Windows PowerShell");
        break;
      case kAppCommandShellPowerShell:
        OpenWindowsShell("pwsh.exe -NoLogo", "PowerShell 7");
        break;
      case kAppCommandShellWsl:
        OpenWindowsShell("wsl.exe", "WSL");
        break;
      case kAppCommandShellGitBash:
        // Ghostty's Windows embedded command parser currently tokenizes on
        // whitespace without honoring quotes. The conventional 8.3 alias
        // keeps the standard Git for Windows install path a single token.
        OpenWindowsShell("C:\\Progra~1\\Git\\bin\\bash.exe --login -i",
                         "Git Bash");
        break;
      case kAppCommandShellMsys2Ucrt64:
        OpenWindowsShell(
            "C:\\msys64\\usr\\bin\\env.exe MSYSTEM=UCRT64 "
            "C:\\msys64\\usr\\bin\\bash.exe --login -i",
            "MSYS2 UCRT64");
        break;
      case kAppCommandShellMsys2Mingw64:
        OpenWindowsShell(
            "C:\\msys64\\usr\\bin\\env.exe MSYSTEM=MINGW64 "
            "C:\\msys64\\usr\\bin\\bash.exe --login -i",
            "MSYS2 MINGW64");
        break;
#endif
      case kAppCommandNewColumn:
        ActivateSurface();
        if (CmuxStripController* controller = GetStripController()) {
          controller->AddChromeColumn(GURL());
        }
        break;
      case kAppCommandHistory:
        NavigateTo(GURL("chrome://history"));
        break;
      case kAppCommandDownloads:
        NavigateTo(GURL("chrome://downloads"));
        break;
      case kAppCommandBookmarks:
        NavigateTo(GURL("chrome://bookmarks"));
        break;
      case kAppCommandPasswordManager:
        NavigateTo(GURL("chrome://password-manager/passwords"));
        break;
      case kAppCommandPaymentMethods:
        NavigateTo(GURL("cmux://settings/payments"));
        break;
      case kAppCommandAddresses:
        NavigateTo(GURL("cmux://settings/addresses"));
        break;
      case kAppCommandManageExtensions:
        NavigateTo(GURL("cmux://extensions"));
        break;
      case kAppCommandChromeWebStore:
        NavigateTo(GURL("https://chromewebstore.google.com"));
        break;
      case kAppCommandClearBrowsingData:
        NavigateTo(GURL("cmux://settings/clearBrowserData"));
        break;
      case kAppCommandZoomIn:
        if (web_contents_) {
          zoom::PageZoom::Zoom(web_contents_.get(), content::PAGE_ZOOM_IN);
        }
        break;
      case kAppCommandZoomOut:
        if (web_contents_) {
          zoom::PageZoom::Zoom(web_contents_.get(), content::PAGE_ZOOM_OUT);
        }
        break;
      case kAppCommandZoomReset:
        if (web_contents_) {
          zoom::PageZoom::Zoom(web_contents_.get(), content::PAGE_ZOOM_RESET);
        }
        break;
      case kAppCommandFullscreen:
        if (CmuxStripController* controller = GetStripController()) {
          controller->ToggleFullscreen();
        }
        break;
      case kAppCommandDeveloperTools:
        ActivateSurface();
        if (CmuxStripController* controller = GetStripController()) {
          controller->OpenDevToolsForFocused();
        }
        break;
      case kAppCommandPrint:
      case kAppCommandFind:
      case kAppCommandTaskManager:
      case kAppCommandSavePage:
        // Disabled: these upstream commands depend on BrowserView services
        // which CmuxBrowserWindow does not implement yet.
        break;
      case kAppCommandConfigureCmux:
        NavigateTo(GURL("cmux://settings"));
        break;
      case kAppCommandSettings:
        NavigateTo(GURL("cmux://settings"));
        break;
      case kAppCommandHelpCenter:
        NavigateTo(GURL("https://support.google.com/chrome"));
        break;
      case kAppCommandAboutCmux:
        NavigateTo(GURL("cmux://version"));
        break;
      case kAppCommandExit:
        chrome::AttemptUserExit();
        break;
      default:
        break;
    }
  }

#if BUILDFLAG(IS_WIN)
  void OpenWindowsShell(std::string command, std::string title) {
    ActivateSurface();
    if (CmuxStripController* controller = GetStripController()) {
      controller->NewTerminalTab(std::move(command), std::move(title));
    }
  }
#endif

  void ActivateSurface() {
    if (on_activate_surface_) {
      on_activate_surface_.Run();
    }
  }

  void NavigateTo(const GURL& url) {
    ActivateSurface();
    LoadURLInPane(web_contents_.get(), url);
  }

  base::WeakPtr<content::WebContents> web_contents_;
  base::RepeatingClosure on_activate_surface_;
  base::WeakPtrFactory<CmuxAppMenuButton> weak_factory_;
};

BEGIN_METADATA(CmuxAppMenuButton)
END_METADATA

}  // namespace

std::unique_ptr<views::LabelButton> CreateCmuxExtensionsMenuButton(
    content::WebContents* web_contents) {
  return std::make_unique<CmuxExtensionsMenuButton>(web_contents);
}

std::unique_ptr<views::LabelButton> CreateCmuxDownloadsButton(
    content::WebContents* web_contents,
    base::RepeatingClosure on_activate_surface) {
  return std::make_unique<CmuxDownloadsButton>(
      web_contents, std::move(on_activate_surface));
}

std::unique_ptr<views::LabelButton> CreateCmuxProfileButton(
    content::WebContents* web_contents,
    base::RepeatingClosure on_activate_surface) {
  return std::make_unique<CmuxProfileButton>(
      web_contents, std::move(on_activate_surface));
}

std::unique_ptr<views::View> CreateCmuxAppMenuButton(
    content::WebContents* web_contents,
    base::RepeatingClosure on_activate_surface) {
  return std::make_unique<CmuxAppMenuButton>(web_contents,
                                             std::move(on_activate_surface));
}

}  // namespace cmux
