#!/usr/bin/env python3
"""Static integration checks for New Tab, Ghostty, and toolbar customization."""

from pathlib import Path
import subprocess


ROOT = Path(__file__).resolve().parents[1]
CMUX = ROOT / "overlay/chrome/browser/cmux_term"
RENDERER = ROOT / "overlay/chrome/services/cmux_terminal_renderer"


def read(relative: str) -> str:
    return (ROOT / relative).read_text(encoding="utf-8")


def require(source: str, needle: str, label: str) -> None:
    assert needle in source, f"{label}: missing {needle!r}"


layout_header = read(
    "overlay/chrome/browser/cmux_term/cmux_layout_config.h"
)
layout_source = read(
    "overlay/chrome/browser/cmux_term/cmux_layout_config.cc"
)
configure = read(
    "overlay/chrome/browser/cmux_term/cmux_configure_page.cc"
)
configure_header = read(
    "overlay/chrome/browser/cmux_term/cmux_configure_page.h"
)
browser_pane = read(
    "overlay/chrome/browser/cmux_term/cmux_browser_pane.cc"
)
pane_view = read("overlay/chrome/browser/cmux_term/cmux_pane_view.cc")
theme = read(
    "overlay/chrome/browser/cmux_term/cmux_theme_ghostty.cc"
)
views = read("overlay/chrome/browser/cmux_term/cmux_views.cc")
chrome_surface_colors = read(
    "overlay/chrome/browser/ui/color/cmux_chrome_surface_colors.h"
)
chrome_color_mixers = read(
    "overlay/chrome/browser/ui/color/chrome_color_mixers.cc"
)
mac_terminal = read("overlay/chrome/browser/cmux_term/cmux_ghostty.mm")
renderer_service = read(
    "overlay/chrome/services/cmux_terminal_renderer/"
    "cmux_terminal_renderer_service.mm"
)
renderer_mojom = read(
    "overlay/chrome/services/cmux_terminal_renderer/public/mojom/"
    "cmux_terminal_renderer.mojom"
)
apply_sh = read("scripts/apply.sh")
apply_windows = read("scripts/apply_win_chrome.py")
sync_sh = read("scripts/sync.sh")
new_tab_header = read(
    "overlay/chrome/browser/cmux_term/cmux_new_tab_page.h"
)
new_tab_patch = read("patches/helium-new-tab.patch")
media_patch = read("patches/helium-media-toolbar.patch")
pinned_toolbar_patches = {
    version: read(
        f"patches/cmux-pinned-toolbar-actions-chromium-{version}.patch"
    )
    for version in ("149", "150", "151")
}

# Every checked-in mail patch must at least parse as a complete unified diff.
for patch_name in (
    "helium-new-tab.patch",
    "helium-media-toolbar.patch",
    "cmux-pinned-toolbar-actions-chromium-149.patch",
    "cmux-pinned-toolbar-actions-chromium-150.patch",
    "cmux-pinned-toolbar-actions-chromium-151.patch",
):
    subprocess.run(
        ["git", "apply", "--numstat", str(ROOT / "patches" / patch_name)],
        cwd=ROOT,
        check=True,
        capture_output=True,
        text=True,
    )

# New Tab is local and shortcut-first while retaining Chromium's native
# background and shortcut customization state.
require(
    new_tab_header,
    'kBrowserDefaultNewTabPage[] = "new_tab"',
    "default New Tab page",
)
for needle in (
    "#searchboxContainer",
    "ntp-middle-slot-promo",
    "const GURL local_url(chrome::kChromeUINewTabPageURL);",
    "if (!footer_controller) {",
    "OnFooterVisibilityUpdated(false);",
    "cmux hosts the full Customize Chrome side panel",
    "footer_controller_observation_.Observe(footer_controller);",
    "faviconUrl.searchParams.set('size', '128');",
    ".customize-text {\n+  display: none;",
    "-  ${this.showWallpaperSearchButton ? html`",
    "CustomizeChromePage.GHOSTTY_THEMES",
    "this.apiProxy_.handler.getCmuxThemePickerState();",
    "this.apiProxy_.handler.setCmuxGhosttyTheme(theme);",
    "Search Ghostty themes",
    "+  --cr-search-field-input-min-height: 20px;",
    "+  padding: 12px;",
    "Follow Ghostty config",
    "CmuxGhosttyThemePreview",
    "ghostty-theme-preview-swatches",
    "getGhosttyThemePreviewStyle_",
    "ghosttyThemeSaveGeneration_",
    "Showing ${this.getFilteredGhosttyThemes_().length} of",
    'role="radiogroup"',
    "onGhosttyThemesBackButtonClick_",
    "getGhosttyThemeSelectHandler_",
    "struct CmuxThemePickerState",
    "GetCmuxThemePickerState() => (CmuxThemePickerState state);",
    "SetCmuxGhosttyTheme(string theme) => (bool success);",
    "void CustomizeChromePageHandler::GetCmuxThemePickerState(",
    "void CustomizeChromePageHandler::SetCmuxGhosttyTheme(",
    "cmux_ghostty_themes_.contains(theme)",
    "cmux::LoadCmuxThemePickerState(",
    "cmux::ApplyCmuxGhosttyTheme(theme)",
    "private async openToolbarCustomizationPage()",
    "url.host() == cmux::kCmuxConfigureChromeHost",
    "return &NewWebUI<cmux::CmuxConfigureUI>;",
    '#include "chrome/browser/cmux_term/cmux_configure_page.h"',
    "cmux::RegisterCmuxConfigureWebUI();",
):
    require(new_tab_patch, needle, "Helium New Tab patch")
assert "window.open('cmux://settings/" not in new_tab_patch, (
    "Customize Chrome routes must use the browser-owned tab callback"
)
assert "OpenCmuxSettings" not in new_tab_patch
assert "openCmuxSettings" not in new_tab_patch
assert '@change="${() =>' not in new_tab_patch
assert "ghosttyThemeSaving_" not in new_tab_patch
assert new_tab_patch.count("if (!footer_controller) {") == 3, (
    "Helium New Tab patch must guard both New Tab footer call sites and the "
    "Customize Chrome side panel"
)
for path in (
    "overlay/chrome/browser/cmux_term/cmux_term.cc",
    "overlay/chrome/browser/cmux_term/cmux_term_mac.mm",
):
    assert "RegisterCmuxConfigureWebUI" not in read(path), (
        f"{path}: register the cmux settings WebUI from "
        "RegisterChromeWebUIConfigs before profile creation"
    )
registration_index = views.index("RegisterCmuxConfigureWebUI();")
rewrite_index = views.index(
    "content::BrowserURLHandler::GetInstance()->AddHandlerPair(",
    registration_index,
)
assert registration_index < rewrite_index, (
    "register cmux settings before installing the cmux URL rewrite"
)
for needle in (
    'resource_path.find_first_of("?#")',
    "resource_path.remove_prefix(1);",
    'resource_path.empty() || resource_path == "index.html"',
):
    require(configure, needle, "settings deep-link resource routing")

# One persisted source of truth drives all pane-local toolbar copies.
toolbar_controls = (
    ("back", "back"),
    ("forward", "forward"),
    ("reload", "reload"),
    ("home", "home"),
    ("extensions", "extensions"),
    ("downloads", "downloads"),
    ("media", "media"),
    ("profile", "profile"),
    ("menu", "menu"),
)
for field, wire_name in toolbar_controls:
    member = f"toolbar_show_{field}"
    require(layout_header, f"bool {member}", f"{field} toolbar preference")
    require(layout_source, f'd.Set("{member}", {member});',
            f"{field} toolbar serialization")
    require(layout_source, f'getb("{member}", next.{member});',
            f"{field} toolbar restoration")
    require(configure, f'data-toolbar="{wire_name}"',
            f"{field} toolbar control")
    require(configure, f'toolbar.Set("{wire_name}", config.{member});',
            f"{field} toolbar WebUI state")
    require(browser_pane, f"config.{member}",
            f"{field} live toolbar application")

# Helium's complete native pinnable-action inventory renders beside cmux's
# permanent pane controls from the same Chromium model the native page edits.
require(
    browser_pane,
    "std::make_unique<PinnedToolbarActionsContainer>(",
    "native pinned toolbar container",
)
for version, patch in pinned_toolbar_patches.items():
    for needle in (
        "PinnedToolbarActionsContainer(Browser* browser,",
        "raw_ptr<Browser> browser_;",
        "Browser* browser = browser_.get();",
        "default_button_size_",
        "browser_->browser_actions()->root_action_item()",
        "SetPinnedToolbarActionsForCustomWindow(",
    ):
        require(patch, needle, f"Chromium {version} pinned toolbar patch")
    assert "auto* browser = browser_;" not in patch, (
        f"Chromium {version} pinned toolbar patch must unwrap raw_ptr"
    )
require(
    browser_pane,
    "SetPinnedToolbarActionsForCustomWindow(\n          pinned_toolbar_actions_);",
    "active pane pinned-toolbar registration",
)
require(
    pane_view,
    "s->ActivateToolbarHost();",
    "pane activation selects its native toolbar host",
)

# Media remains Chromium's functional global-media control. The small upstream
# seam hosts it from cmux's Browser and Helium's controller gate prevents an
# active media session from overriding a user-hidden preference.
for needle in (
    "MediaToolbarButtonView(browser_view->browser(),",
    "MediaToolbarButtonView(\n+    Browser* browser,",
    "SetCanShowToolbarButton(bool can_show_toolbar_button)",
    "if (!can_show_toolbar_button_)",
):
    require(media_patch, needle, "native media toolbar patch")
require(
    browser_pane,
    "std::make_unique<MediaToolbarButtonContextualMenu>(browser_)",
    "native media contextual menu",
)

# Ghostty's bundled theme directory feeds the searchable picker, selected
# names are persisted, and both the browser palette and terminal renderer get
# the exact same override.
for needle in (
    "std::vector<std::string> ListGhosttyThemes()",
    "std::vector<GhosttyThemePreview> ListGhosttyThemePreviews()",
    "base::ReadFileToStringWithMaxSize(",
    "std::string SerializeGhosttyThemeColors(const CmuxTheme& theme)",
    'AppendASCII("themes")',
    "base::FileEnumerator::FILES",
    'const std::string override = "theme = " + theme_name;',
):
    require(theme, needle, "Ghostty theme catalog")
for needle in (
    'list="ghostty-theme-options"',
    'sendWithPromise(\'getCustomization\')',
    'sendWithPromise(\'getAppliedCustomization\')',
    'sendWithPromise(\'setCustomization\'',
    "palette.Set(\"background\", css_color(applied->palette.window_bg));",
    "renderPalette(state.palette);",
    "base::BindOnce(&ListGhosttyThemes)",
    "!ghostty_themes_.contains(*theme)",
):
    require(configure, needle, "appearance customization WebUI")
for needle in (
    "using CmuxThemePickerStateCallback",
    "std::vector<GhosttyThemePreview>",
    "void LoadCmuxThemePickerState(CmuxThemePickerStateCallback callback);",
    "bool ApplyCmuxGhosttyTheme(const std::string& theme);",
):
    require(configure_header, needle, "native Ghostty theme picker API")
for needle in (
    "void LoadCmuxThemePickerState(CmuxThemePickerStateCallback callback)",
    "base::BindOnce(&ListGhosttyThemePreviews)",
    "bool ApplyCmuxGhosttyTheme(const std::string& theme)",
    "config.ghostty_theme_name = theme;",
    "ReloadCustomizationConsumers(previous_theme !=",
):
    require(configure, needle, "native Ghostty theme picker implementation")
require(
    views,
    "layout_config_.ghostty_theme_name",
    "browser Ghostty palette selection",
)
for needle in (
    "active_theme_name_ = layout_config_.ghostty_theme_name;",
    "return CmuxAppliedTheme{active_theme_name_, *active_theme_palette_};",
):
    require(views, needle, "applied Ghostty theme identity")
require(
    views,
    "chrome_colors.new_tab_background =",
    "Ghostty New Tab background publication",
)
require(
    views,
    "new_tab_background != active_theme_palette_->window_bg",
    "Ghostty New Tab background runtime self-test",
)
require(
    views,
    "ReloadOpenNewTabPagesForTheme();",
    "live New Tab theme refresh",
)
for needle in (
    "struct CustomizationThemeReloadState",
    "base::Milliseconds(16)",
    "ApplyResolvedPublishedCustomization(config, theme)",
    "ApplyCmuxGhosttyThemeToAllTerminalViews(*theme)",
    "constexpr size_t kReloadsPerFrame = 8",
    "pending_new_tab_theme_reloads_.push_back(tab.id)",
):
    require(views, needle, "coalesced shared theme resolution")
require(
    chrome_surface_colors,
    "SkColor new_tab_background",
    "Ghostty New Tab color contract",
)
for needle in (
    "mixer[kColorNewTabPageBackground] = {colors.new_tab_background};",
    "mixer[kColorNewTabPageBackgroundOverride] = "
    "{colors.new_tab_background};",
):
    require(chrome_color_mixers, needle, "Ghostty New Tab color mixer")
for needle in (
    "ApplyCmuxGhosttyThemeToAllTerminalViews(const CmuxTheme& theme)",
    "SerializeGhosttyThemeColors(theme)",
    "[view sendResolvedThemeConfig:theme_config]",
    "constexpr NSUInteger kBatchSize = 8",
):
    require(mac_terminal, needle, "live macOS terminal theme broadcast")
assert "ReloadAllCmuxGhosttyTerminalViews" not in mac_terminal
for needle in (
    "string theme_name",
    "UpdateTheme(string theme_config)",
):
    require(renderer_mojom, needle, "renderer theme IPC")
for needle in (
    'const std::string override = "theme = " + theme_name;',
    "CmuxTerminalRendererService::UpdateTheme(",
    "PendingSurfaceOperationKind::kOutputBarrier",
    "ghostty_surface_update_theme_config(surface_, config);",
):
    require(renderer_service, needle, "in-place renderer theme update")

# Every desktop patch path receives the New Tab and media seams. Windows also
# compiles the theme catalog beside its vendored Ghostty library.
for patch_name in (
    "helium-new-tab.patch",
    "helium-media-toolbar.patch",
    "cmux-pinned-toolbar-actions-chromium-149.patch",
    "cmux-pinned-toolbar-actions-chromium-150.patch",
    "cmux-pinned-toolbar-actions-chromium-151.patch",
):
    require(apply_sh, patch_name, "POSIX patch application")
    require(apply_windows, patch_name, "Windows patch application")
    require(sync_sh, patch_name, "warm-tree patch refresh")
for source_name in ("cmux_theme_ghostty.h", "cmux_theme_ghostty.cc"):
    require(apply_windows, source_name, "Windows Ghostty theme sources")

print("New Tab, Ghostty appearance, and toolbar customization: PASS")
