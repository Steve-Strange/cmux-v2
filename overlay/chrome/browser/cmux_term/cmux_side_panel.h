// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_CMUX_TERM_CMUX_SIDE_PANEL_H_
#define CHROME_BROWSER_CMUX_TERM_CMUX_SIDE_PANEL_H_

#include <memory>

#include "chrome/browser/cmux_term/window_model.h"
#include "extensions/common/extension_id.h"

class Browser;
class BrowserWindowInterface;
class SidePanelUI;

namespace content {
class WebContents;
}

namespace views {
class View;
}

namespace cmux {

// Creates the window-scoped SidePanelUI for one workspace Browser. Chrome's
// extension side-panel service continues to own options and per-tab entries;
// this object only replaces BrowserView's single presentation coordinator
// with a coordinator capable of hosting one contextual panel per visible cmux
// tab surface.
std::unique_ptr<SidePanelUI> CreateCmuxSidePanelUI(Browser* browser);

// Adds the in-surface [resize handle | header + content] host for one web tab.
// The host registers itself with the workspace SidePanelUI and consumes width
// inside `parent`; it never changes the surrounding pane's bounds.
views::View* AddCmuxSidePanelHost(views::View* parent,
                                  Browser* browser,
                                  content::WebContents* web_contents,
                                  PaneId pane);

// Keeps an already-realized surface in sync with a contextual entry that may
// have been activated while the surface was hidden.
void SyncCmuxSidePanelForWebContents(Browser* browser,
                                     content::WebContents* web_contents);

// Extension API bridges for tab-specific open/close calls. They return false
// for normal BrowserView windows so upstream behavior remains unchanged.
bool OpenCmuxContextualSidePanel(BrowserWindowInterface& browser_window,
                                 content::WebContents& web_contents,
                                 const extensions::ExtensionId& extension_id);
bool CloseCmuxContextualSidePanel(BrowserWindowInterface* browser_window,
                                  content::WebContents* web_contents,
                                  const extensions::ExtensionId& extension_id);

}  // namespace cmux

#endif  // CHROME_BROWSER_CMUX_TERM_CMUX_SIDE_PANEL_H_
