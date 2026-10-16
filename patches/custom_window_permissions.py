#!/usr/bin/env python3
"""Make Chromium 151 permission UI work with a non-BrowserView BrowserWindow.

cmux hosts a real Browser and LocationBarView in each pane, but its shared
workspace Widget is intentionally not a BrowserView. Chromium's public
BrowserWindow boundary already exposes the required location bar; a few Views
permission call sites still bypass that boundary and assume BrowserView.

Keep the transformations explicit and fail closed when an upstream anchor
changes. Each replacement is reversible and idempotent so warm fleet
workspaces can restore the previous branch's inputs before applying new ones.
"""

from dataclasses import dataclass
from pathlib import Path
import sys


@dataclass(frozen=True)
class Replacement:
    label: str
    old: str
    new: str


MINIMUM_CHROMIUM_MAJOR = 151


REPLACEMENTS = {
    "chrome/browser/ui/views/permissions/permission_prompt_factory.cc": (
        Replacement(
            "permission factory BrowserWindow include",
            '#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"\n',
            '#include "chrome/browser/cmux_term/cmux_browser_window.h"\n'
            '#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"\n',
        ),
        Replacement(
            "permission factory fullscreen location bar",
            """  BrowserView* browser_view = BrowserView::GetBrowserViewForBrowser(browser);
  if (!browser_view) {
    return false;
  }

  LocationBar* location_bar = browser_view->GetLocationBar();

  return !location_bar || !location_bar->IsDrawn() ||
         location_bar->IsFullscreen();
""",
            """  // A custom BrowserWindow can own a production LocationBar without a
  // BrowserView. Resolve the requesting WebContents so multi-pane embedders do
  // not display one origin's permission UI over another origin's toolbar.
  LocationBar* location_bar =
      cmux::GetLocationBarForWebContents(browser, web_contents);

  return !location_bar || !location_bar->IsDrawn() ||
         location_bar->IsFullscreen();
""",
        ),
        Replacement(
            "permission factory location bar",
            """  BrowserView* browser_view = BrowserView::GetBrowserViewForBrowser(browser);
  return browser_view ? browser_view->GetLocationBar() : nullptr;
""",
            """  // Resolve the requesting WebContents, not merely the Browser's active
  // tab: cmux can keep web surfaces in multiple panes visible simultaneously.
  return cmux::GetLocationBarForWebContents(browser, web_contents);
""",
        ),
    ),
    "chrome/browser/ui/views/bubble_anchor_util_views.h": (
        Replacement(
            "bubble anchor requesting WebContents declaration",
            """AnchorConfiguration GetPageInfoAnchorConfiguration(
    BrowserWindowInterface* browser,
    Anchor = Anchor::kLocationBar);
""",
            """AnchorConfiguration GetPageInfoAnchorConfiguration(
    BrowserWindowInterface* browser,
    Anchor anchor = Anchor::kLocationBar,
    content::WebContents* web_contents = nullptr);
""",
        ),
    ),
    "chrome/browser/ui/views/bubble_anchor_util_views.cc": (
        Replacement(
            "bubble anchor cmux location include",
            '#include "chrome/browser/ui/browser_element_identifiers.h"\n',
            '#include "chrome/browser/cmux_term/cmux_browser_window.h"\n'
            '#include "chrome/browser/ui/browser_element_identifiers.h"\n',
        ),
        Replacement(
            "bubble anchor requesting WebContents definition",
            """AnchorConfiguration GetPageInfoAnchorConfiguration(
    BrowserWindowInterface* browser,
    Anchor anchor) {
""",
            """AnchorConfiguration GetPageInfoAnchorConfiguration(
    BrowserWindowInterface* browser,
    Anchor anchor,
    content::WebContents* web_contents) {
""",
        ),
        Replacement(
            "bubble anchor generic location bar",
            """  BrowserView* browser_view = BrowserView::GetBrowserViewForBrowser(browser);
  auto* location_bar_view =
      browser_view ? browser_view->GetLocationBarView() : nullptr;
""",
            """  BrowserView* browser_view = BrowserView::GetBrowserViewForBrowser(browser);
  LocationBar* location_bar =
      cmux::GetLocationBarForWebContents(browser, web_contents);
  auto* location_bar_view =
      browser_view ? browser_view->GetLocationBarView() : nullptr;
""",
        ),
        Replacement(
            "bubble anchor chip null safety",
            """  } else {
    auto chip_anchor =
        BrowserWindow::FromBrowser(browser)->GetLocationBar()->GetChipAnchor();
    if (anchor == Anchor::kLocationBar && chip_anchor) {
      return *chip_anchor;
    }
  }
""",
            """  } else if (location_bar) {
    auto chip_anchor = location_bar->GetChipAnchor();
    if (anchor == Anchor::kLocationBar && chip_anchor) {
      return *chip_anchor;
    }
  }
""",
        ),
        Replacement(
            "bubble anchor generic location bar element",
            """  if (anchor == Anchor::kLocationBar) {
    LocationBar* location_bar =
        browser_view ? browser_view->GetLocationBar() : nullptr;
    if (location_bar && location_bar->IsDrawn()) {
      if (ui::TrackedElement* element = location_bar->GetAnchorOrNull()) {
        return {views::BubbleAnchor(element), kLocationIconElementId,
                views::BubbleBorder::TOP_LEFT};
      }
    }
  }
""",
            """  if (anchor == Anchor::kLocationBar && location_bar &&
      location_bar->IsDrawn()) {
    if (ui::TrackedElement* element = location_bar->GetAnchorOrNull()) {
      return {views::BubbleAnchor(element), kLocationIconElementId,
              views::BubbleBorder::TOP_LEFT};
    }
  }
""",
        ),
        Replacement(
            "bubble anchor custom window fallback",
            """  if (anchor == Anchor::kLocationBar &&
      browser_view->GetIsPictureInPictureType()) {
""",
            """  // Location-bar anchoring above is the complete custom-window path.
  // Remaining fallbacks require concrete BrowserView toolbar/frame objects.
  if (!browser_view) {
    return {};
  }

  if (anchor == Anchor::kLocationBar &&
      browser_view->GetIsPictureInPictureType()) {
""",
        ),
        Replacement(
            "permission prompt requesting WebContents anchor",
            """  if (browser) {
    LocationBar* location_bar =
        BrowserWindow::FromBrowser(browser)->GetLocationBar();
    ChipController* chip_controller =
        location_bar ? location_bar->GetChipController() : nullptr;
    if (chip_controller && chip_controller->IsPermissionPromptChipVisible()) {
      if (auto chip_anchor = location_bar->GetChipAnchor()) {
        return *chip_anchor;
      }
    }
    return GetPageInfoAnchorConfiguration(browser);
  }
""",
            """  if (browser) {
    LocationBar* location_bar =
        cmux::GetLocationBarForWebContents(browser, web_contents);
    ChipController* chip_controller =
        location_bar ? location_bar->GetChipController() : nullptr;
    if (chip_controller && chip_controller->IsPermissionPromptChipVisible()) {
      if (auto chip_anchor = location_bar->GetChipAnchor()) {
        return *chip_anchor;
      }
    }
    return GetPageInfoAnchorConfiguration(
        browser, Anchor::kLocationBar, web_contents);
  }
""",
        ),
        Replacement(
            "bubble anchor rect custom window fallback",
            """  BrowserView* browser_view = BrowserView::GetBrowserViewForBrowser(browser);
  // Get position in view (taking RTL UI into account).
  int x_within_browser_view = browser_view->GetMirroredXInView(
""",
            """  BrowserView* browser_view = BrowserView::GetBrowserViewForBrowser(browser);
  if (!browser_view) {
    // A null anchor is valid in fullscreen and custom BrowserWindows. Give the
    // bubble a stable corner in the native window instead of dereferencing a
    // BrowserView that does not exist.
    gfx::Point browser_origin = browser->GetWindow()->GetBounds().origin();
    browser_origin.Offset(bubble_anchor_util::kNoToolbarLeftOffset, 0);
    return gfx::Rect(browser_origin, gfx::Size());
  }
  // Get position in view (taking RTL UI into account).
  int x_within_browser_view = browser_view->GetMirroredXInView(
""",
        ),
    ),
    "chrome/browser/ui/views/permissions/permission_prompt_base_view.cc": (
        Replacement(
            "permission prompt fallback cmux location include",
            '#include "chrome/browser/platform_util.h"\n',
            '#include "chrome/browser/cmux_term/cmux_browser_window.h"\n'
            '#include "chrome/browser/platform_util.h"\n',
        ),
        Replacement(
            "permission prompt requesting WebContents fallback rect",
            """  if (configuration.anchor.IsNull()) {
    SetAnchorRect(bubble_anchor_util::GetPageInfoAnchorRect(GetBrowser()));
  }
""",
            """  if (configuration.anchor.IsNull()) {
    SetAnchorRect(cmux::GetFallbackBubbleAnchorRectForWebContents(
        GetBrowser(), web_contents()));
  }
""",
        ),
    ),
    "chrome/browser/ui/views/permissions/permission_prompt_bubble_base_view.cc": (
        Replacement(
            "permission decision cmux location include",
            '#include "chrome/browser/ui/browser.h"\n',
            '#include "chrome/browser/cmux_term/cmux_browser_window.h"\n'
            '#include "chrome/browser/ui/browser.h"\n',
        ),
        Replacement(
            "permission decision generic chip controller",
            """  // `GetBrowser()` can be null for hosts that are not backed by a Browser, such
  // as a standalone Document Picture-in-Picture window. Guard against it since
  // `GetBrowserViewForBrowser()` dereferences its argument.
  auto* browser = GetBrowser();
  BrowserView* browser_view =
      browser ? BrowserView::GetBrowserViewForBrowser(browser) : nullptr;
""",
            """  // `GetBrowser()` can be null for hosts that are not backed by a Browser,
  // such as a standalone Document Picture-in-Picture window. A custom
  // BrowserWindow can own the production chip controller without BrowserView.
  auto* browser = GetBrowser();
  LocationBar* location_bar =
      browser ? cmux::GetLocationBarForWebContents(browser, web_contents())
              : nullptr;
  ChipController* chip_controller =
      location_bar ? location_bar->GetChipController() : nullptr;
""",
        ),
        Replacement(
            "permission decision generic visible chip",
            """  if (browser_view && browser_view->GetLocationBar()->GetChipController() &&
      browser_view->GetLocationBar()
          ->GetChipController()
          ->IsPermissionPromptChipVisible() &&
      browser_view->GetLocationBar()->GetChipController()->IsBubbleShowing()) {
    ChipController* chip_controller =
        browser_view->GetLocationBar()->GetChipController();
""",
            """  if (chip_controller &&
      chip_controller->IsPermissionPromptChipVisible() &&
      chip_controller->IsBubbleShowing()) {
""",
        ),
    ),
    "chrome/browser/ui/views/permissions/embedded_permission_prompt_base_view.cc": (
        Replacement(
            "permission element custom contents view",
            """  // Convert the position into screen coordinates.
  auto* content_view = GetContentsWebView(GetNativeWindow());
  views::View::ConvertRectToScreen(content_view, &element_rect_);
""",
            """  // Convert the position into screen coordinates. BrowserView registers a
  // ContentsWebView element; custom BrowserWindows such as cmux host the same
  // WebContents in their own WebView and expose its screen container bounds.
  auto* content_view = GetContentsWebView(GetNativeWindow());
  if (content_view) {
    views::View::ConvertRectToScreen(content_view, &element_rect_);
  } else {
    const gfx::Rect contents_bounds = web_contents->GetContainerBounds();
    element_rect_.Offset(contents_bounds.x(), contents_bounds.y());
  }
""",
        ),
    ),
    "chrome/browser/ui/views/permissions/chooser_bubble_ui.cc": (
        Replacement(
            "chooser cmux location include",
            '#include "chrome/browser/picture_in_picture/picture_in_picture_occlusion_tracker.h"\n',
            '#include "chrome/browser/cmux_term/cmux_browser_window.h"\n'
            '#include "chrome/browser/picture_in_picture/picture_in_picture_occlusion_tracker.h"\n',
        ),
        Replacement(
            "chooser requesting WebContents anchor helper",
            """AnchorConfiguration GetChooserAnchorConfiguration(Browser* browser) {
  return bubble_anchor_util::GetPageInfoAnchorConfiguration(browser);
}

gfx::Rect GetChooserAnchorRect(Browser* browser) {
  return bubble_anchor_util::GetPageInfoAnchorRect(browser);
}
""",
            """AnchorConfiguration GetChooserAnchorConfiguration(
    Browser* browser,
    content::WebContents* contents) {
  return bubble_anchor_util::GetPageInfoAnchorConfiguration(
      browser, bubble_anchor_util::Anchor::kLocationBar, contents);
}

gfx::Rect GetChooserAnchorRect(Browser* browser,
                               content::WebContents* contents) {
  return cmux::GetFallbackBubbleAnchorRectForWebContents(browser, contents);
}
""",
        ),
        Replacement(
            "chooser constructor requesting WebContents anchor",
            """    : LocationBarBubbleDelegateView(
          GetChooserAnchorConfiguration(browser).anchor,
          contents),
""",
            """    : LocationBarBubbleDelegateView(
          GetChooserAnchorConfiguration(browser, contents).anchor,
          contents),
""",
        ),
        Replacement(
            "chooser update requesting WebContents declaration",
            """  void UpdateAnchor(Browser* browser);
""",
            """  void UpdateAnchor(Browser* browser,
                    content::WebContents* contents);
""",
        ),
        Replacement(
            "chooser update requesting WebContents definition",
            """void ChooserBubbleUiViewDelegate::UpdateAnchor(Browser* browser) {
  AnchorConfiguration configuration = GetChooserAnchorConfiguration(browser);
""",
            """void ChooserBubbleUiViewDelegate::UpdateAnchor(
    Browser* browser,
    content::WebContents* contents) {
  AnchorConfiguration configuration =
      GetChooserAnchorConfiguration(browser, contents);
""",
        ),
        Replacement(
            "chooser requesting WebContents fallback rect",
            """    SetAnchorRect(GetChooserAnchorRect(browser));
""",
            """    SetAnchorRect(GetChooserAnchorRect(browser, contents));
""",
        ),
        Replacement(
            "chooser standard requesting WebContents update",
            """  bubble->UpdateAnchor(browser->GetBrowserForMigrationOnly());
""",
            """  bubble->UpdateAnchor(browser->GetBrowserForMigrationOnly(), contents);
""",
        ),
        Replacement(
            "extension chooser visible WebContents gate",
            """  if (browser->GetTabStripModel()->GetActiveWebContents() != contents) {
    return base::DoNothing();
  }

  // `GetExtensionsToolbarDesktop` may return `nullptr`, for instance in
""",
            """  if (!cmux::IsWebContentsActiveOrVisible(browser, contents)) {
    return base::DoNothing();
  }

  // `GetExtensionsToolbarDesktop` may return `nullptr`, for instance in
""",
        ),
        Replacement(
            "chooser visible WebContents gate",
            """  if (browser->GetTabStripModel()->GetActiveWebContents() != contents) {
    return base::DoNothing();
  }

  std::optional<base::ScopedClosureRunner> fullscreen_blocker =
""",
            """  if (!cmux::IsWebContentsActiveOrVisible(browser, contents)) {
    return base::DoNothing();
  }

  std::optional<base::ScopedClosureRunner> fullscreen_blocker =
""",
        ),
        Replacement(
            "extension device chooser custom window fallback",
            """  // `GetExtensionsToolbarDesktop` may return `nullptr`, for instance in
  // extension popup windows.
  auto* extensions_toolbar = BrowserView::GetBrowserViewForBrowser(browser)
                                 ->toolbar_button_provider()
                                 ->GetExtensionsToolbarDesktop();
""",
            """  // `GetExtensionsToolbarDesktop` may return `nullptr`, for instance in
  // extension popup windows. A custom BrowserWindow also has no BrowserView
  // toolbar, so show the same security-level chooser bubble at its production
  // location bar.
  BrowserView* browser_view = BrowserView::GetBrowserViewForBrowser(browser);
  if (!browser_view) {
    std::optional<base::ScopedClosureRunner> fullscreen_blocker =
        contents->ForSecurityDropFullscreen(display::kInvalidDisplayId);
    if (!fullscreen_blocker.has_value()) {
      return base::DoNothing();
    }

    auto bubble = std::make_unique<ChooserBubbleUiViewDelegate>(
        browser->GetBrowserForMigrationOnly(), contents, std::move(controller),
        std::move(fullscreen_blocker).value());
    bubble->UpdateAnchor(browser->GetBrowserForMigrationOnly(),
                         contents);
    base::OnceClosure close_closure = bubble->MakeCloseClosure();
    views::Widget* widget =
        views::BubbleDialogDelegateView::CreateBubble(std::move(bubble));
    widget->SetZOrderSublevel(ChromeWidgetSublevel::kSublevelSecurity);
    widget->Show();
    return close_closure;
  }

  // `GetExtensionsToolbarDesktop` may return `nullptr`, for instance in
  // extension popup windows.
  auto* extensions_toolbar = browser_view->toolbar_button_provider()
                                 ->GetExtensionsToolbarDesktop();
            """,
        ),
    ),
    "chrome/browser/ui/views/file_system_access/file_system_access_usage_bubble_view.cc": (
        Replacement(
            "file system access cmux location include",
            '#include "chrome/browser/file_system_access/chrome_file_system_access_permission_context.h"\n',
            '#include "chrome/browser/cmux_term/cmux_browser_window.h"\n'
            '#include "chrome/browser/file_system_access/chrome_file_system_access_permission_context.h"\n',
        ),
        Replacement(
            "file system access generic location bar include",
            '#include "chrome/browser/ui/file_system_access/file_system_access_ui_helpers.h"\n',
            '#include "chrome/browser/ui/file_system_access/file_system_access_ui_helpers.h"\n'
            '#include "chrome/browser/ui/location_bar/location_bar.h"\n',
        ),
        Replacement(
            "file system access generic bubble anchor",
            """  ToolbarButtonProvider* button_provider =
      BrowserView::GetBrowserViewForBrowser(browser)->toolbar_button_provider();
""",
            """  BrowserView* browser_view =
      BrowserView::GetBrowserViewForBrowser(browser);
  views::BubbleAnchor bubble_anchor;
  if (browser_view) {
    bubble_anchor =
        browser_view->toolbar_button_provider()->GetBubbleAnchor(std::nullopt);
  } else {
    LocationBar* location_bar =
        cmux::GetLocationBarForWebContents(browser, web_contents);
    if (location_bar) {
      if (ui::TrackedElement* element = location_bar->GetAnchorOrNull()) {
        bubble_anchor = views::BubbleAnchor(element);
      }
    }
  }
""",
        ),
        Replacement(
            "file system access use generic anchor",
            """  bubble_ = new FileSystemAccessUsageBubbleView(
      button_provider->GetBubbleAnchor(std::nullopt), web_contents, origin,
      std::move(usage));
""",
            """  bubble_ = new FileSystemAccessUsageBubbleView(
      bubble_anchor, web_contents, origin, std::move(usage));
""",
        ),
        Replacement(
            "file system access generic accessible title",
            """  auto* page_action_view =
      BrowserView::GetBrowserViewForBrowser(browser)
          ->toolbar_button_provider()
          ->GetPageActionViewInterface(kActionShowFileSystemAccess);
  if (!page_action_view) {
    return {};
  }
  return page_action_view->GetTooltipText();
""",
            """  BrowserView* browser_view =
      BrowserView::GetBrowserViewForBrowser(browser);
  if (browser_view) {
    auto* page_action_view =
        browser_view->toolbar_button_provider()->GetPageActionViewInterface(
            kActionShowFileSystemAccess);
    if (!page_action_view) {
      return {};
    }
    return page_action_view->GetTooltipText();
  }

  auto* action_item = actions::ActionManager::Get().FindAction(
      kActionShowFileSystemAccess, browser->GetActions()->root_action_item());
  if (!action_item) {
    return {};
  }
  return std::u16string(action_item->GetTooltipText());
            """,
        ),
    ),
    "chrome/browser/ui/views/media_router/media_remoting_dialog_view.cc": (
        Replacement(
            "media remoting generic BrowserWindow include",
            '#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"\n',
            '#include "chrome/browser/cmux_term/cmux_browser_window.h"\n'
            '#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"\n',
        ),
        Replacement(
            "media remoting generic LocationBar include",
            '#include "chrome/browser/ui/media_router/media_router_ui_service.h"\n',
            '#include "chrome/browser/ui/location_bar/location_bar.h"\n'
            '#include "chrome/browser/ui/media_router/media_router_ui_service.h"\n',
        ),
        Replacement(
            "media remoting generic bubble anchor",
            """  auto anchor = BrowserView::GetBrowserViewForBrowser(browser)
                    ->toolbar_button_provider()
                    ->GetPinnedToolbarActions()
                    ->GetBubbleAnchor(kActionRouteMedia);
""",
            """  BrowserView* browser_view =
      BrowserView::GetBrowserViewForBrowser(browser);
  views::BubbleAnchor anchor;
  if (browser_view) {
    anchor = browser_view->toolbar_button_provider()
                 ->GetPinnedToolbarActions()
                 ->GetBubbleAnchor(kActionRouteMedia);
  } else {
    LocationBar* location_bar =
        cmux::GetLocationBarForWebContents(browser, web_contents_);
    if (location_bar) {
      if (ui::TrackedElement* element = location_bar->GetAnchorOrNull()) {
        anchor = views::BubbleAnchor(element);
      }
    }
  }
""",
        ),
    ),
}


def replacement_state(replacement: Replacement, source: str) -> str:
    """Return whether one replacement is pristine or patched, or fail closed."""
    new_count = source.count(replacement.new)
    old_count = source.count(replacement.old)
    embedded_old_count = replacement.new.count(replacement.old) * new_count
    standalone_old_count = old_count - embedded_old_count

    if new_count == 1 and standalone_old_count == 0:
        return "patched"
    if new_count == 0 and old_count == 1:
        return "pristine"
    raise AssertionError(
        f"{replacement.label}: expected exactly one patched or upstream "
        f"anchor, got patched={new_count}, upstream={standalone_old_count}"
    )


def patch_source(relative_path: str, source: str) -> tuple[str, list[str]]:
    """Apply all transformations for one source and return changed labels."""
    changed = []
    for replacement in REPLACEMENTS[relative_path]:
        if replacement_state(replacement, source) == "patched":
            continue
        source = source.replace(replacement.old, replacement.new, 1)
        changed.append(replacement.label)
    return source, changed


def restore_source(relative_path: str, source: str) -> tuple[str, list[str]]:
    """Reverse all transformations for one source and return changed labels."""
    changed = []
    for replacement in reversed(REPLACEMENTS[relative_path]):
        if replacement_state(replacement, source) == "pristine":
            continue
        source = source.replace(replacement.new, replacement.old, 1)
        changed.append(replacement.label)
    return source, changed


def patch_tree(root: Path) -> None:
    for relative_path in REPLACEMENTS:
        path = root / relative_path
        source = path.read_text()
        patched, changed = patch_source(relative_path, source)
        if changed:
            with path.open("w", newline="\n") as output:
                output.write(patched)
            for label in changed:
                print(f"custom-window-permissions: {label}: patched")
        else:
            print(f"custom-window-permissions: {relative_path}: already patched")


def restore_tree(root: Path) -> None:
    for relative_path in REPLACEMENTS:
        path = root / relative_path
        source = path.read_text()
        restored, changed = restore_source(relative_path, source)
        if changed:
            with path.open("w", newline="\n") as output:
                output.write(restored)
            for label in changed:
                print(f"custom-window-permissions: {label}: restored")
        else:
            print(
                f"custom-window-permissions: {relative_path}: already pristine"
            )


def main(argv: list[str]) -> int:
    arguments = argv[1:]
    restore = bool(arguments and arguments[0] == "--restore")
    if restore:
        arguments.pop(0)
    if len(arguments) > 1:
        print(
            f"usage: {argv[0]} [--restore] [chromium-src]",
            file=sys.stderr,
        )
        return 2
    root = Path(arguments[0]) if arguments else Path.cwd()
    if restore:
        restore_tree(root)
    else:
        patch_tree(root)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
