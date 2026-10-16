// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef CHROME_BROWSER_CMUX_TERM_CMUX_TOOLBAR_MENUS_H_
#define CHROME_BROWSER_CMUX_TERM_CMUX_TOOLBAR_MENUS_H_

#include <memory>

#include "base/functional/callback_forward.h"

namespace content {
class WebContents;
}  // namespace content

namespace views {
class LabelButton;
class View;
}  // namespace views

namespace cmux {

std::unique_ptr<views::LabelButton> CreateCmuxExtensionsMenuButton(
    content::WebContents* web_contents);

// Native Chromium downloads and profile bubbles, anchored to cmux's toolbar.
// Their implementation adapts the upstream Views bubbles to CmuxBrowserWindow
// without requiring a BrowserView.
std::unique_ptr<views::LabelButton> CreateCmuxDownloadsButton(
    content::WebContents* web_contents,
    base::RepeatingClosure on_activate_surface);

std::unique_ptr<views::LabelButton> CreateCmuxProfileButton(
    content::WebContents* web_contents,
    base::RepeatingClosure on_activate_surface);

std::unique_ptr<views::View> CreateCmuxAppMenuButton(
    content::WebContents* web_contents,
    base::RepeatingClosure on_activate_surface);

}  // namespace cmux

#endif  // CHROME_BROWSER_CMUX_TERM_CMUX_TOOLBAR_MENUS_H_
