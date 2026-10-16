#!/usr/bin/env python3
"""Regression checks for native extension toolbar/menu toggle behavior."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
TOOLBAR_MENUS = (
    ROOT / "overlay/chrome/browser/cmux_term/cmux_toolbar_menus.cc"
).read_text()
CONTAINER = (
    ROOT / "overlay/chrome/browser/cmux_term/cmux_extensions_container.cc"
).read_text()
EXTENSIONS = (
    ROOT / "overlay/chrome/browser/cmux_term/cmux_extensions.cc"
).read_text()

# Match Chromium's ExtensionsToolbarButton: a menu controller firing on press
# owns a pressed lock until the menu widget is destroyed. This prevents the
# second click from closing on deactivation and reopening on release.
assert "std::make_unique<views::MenuButtonController>" in TOOLBAR_MENUS
assert "NotifyAction::kOnPress" in TOOLBAR_MENUS
assert "pressed_lock_ = menu_button_controller_->TakeLock();" in TOOLBAR_MENUS
assert "void OnWidgetDestroying(views::Widget* widget) override" in TOOLBAR_MENUS
assert "pressed_lock_.reset();" in TOOLBAR_MENUS

# Pinned extension actions must receive Chromium's popup lifecycle callbacks so
# their own pressed lock and visual state stay synchronized with the popup.
# cmux can display several copies of one action at once, so retain the exact
# view selected from SetPopupOwner instead of resolving the id against whichever
# pane happens to be active when the popup later closes.
assert "popup_action_view_ = ResolveActionView(popup_owner_);" in CONTAINER
assert "ToolbarActionView* action = popup_action_view_;" in CONTAINER
assert "popup_action_view_ = nullptr;" in CONTAINER
assert "action->OnPopupShown(by_user);" in CONTAINER
assert "action->OnPopupClosed();" in CONTAINER

# Both halves of each toggle regression must go through the real button mouse
# handlers; a direct model or coordinator call cannot prove click-to-close.
assert CONTAINER.count("container->ClickMenuButtonForSelfTest()") == 2
assert EXTENSIONS.count(
    "container->ClickActionForSelfTest(kUBlockOriginExtensionId)"
) == 2

# A cold dogfood profile can initialize extensions before its first cmux pane
# registers the Browser and native action view. Keep the runtime test bounded,
# but wait for the exact uBlock action so it tests clicks instead of startup
# timing.
assert "void RunExtensionSelfTestWhenReady(" in EXTENSIONS
assert "container->GetActionForId(kUBlockOriginExtensionId)" in EXTENSIONS
assert "GetCmuxExtensionsContainerForSelfTest(profile)" in EXTENSIONS
assert "GetCmuxExtensionsContainerForSelfTest(profile)" in CONTAINER
assert "for (auto& [browser, container] : ContainerInstances())" in CONTAINER
assert "constexpr int kMaxReadinessAttempts = 60;" in EXTENSIONS
assert "base::Milliseconds(500)" in EXTENSIONS
assert "active toolbar did not become ready" in EXTENSIONS

print("extension popup/menu two-click toggle: PASS")
