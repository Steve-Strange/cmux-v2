// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef CHROME_BROWSER_CMUX_TERM_CMUX_BROWSER_PANE_H_
#define CHROME_BROWSER_CMUX_TERM_CMUX_BROWSER_PANE_H_

#include <memory>

#include "chrome/browser/cmux_term/window_model.h"

namespace content {
class WebContents;
}
namespace views {
class View;
}

namespace cmux {

class CmuxSurface;

// Creates a web surface -- a real LocationBarView omnibox + back/forward/
// reload toolbar + docked DevTools over `web_contents` (owned by its workspace
// Browser/TabStripModel) -- adds it as a child of `parent` (which takes
// ownership of the presentation view), and returns it as the CmuxSurface
// interface. The concrete
// CmuxBrowserSurface class (and its omnibox plumbing) is private to
// cmux_browser_pane.cc.
CmuxSurface* AddBrowserSurface(views::View* parent,
                               content::WebContents* web_contents,
                               PaneId pane);

}  // namespace cmux

#endif  // CHROME_BROWSER_CMUX_TERM_CMUX_BROWSER_PANE_H_
