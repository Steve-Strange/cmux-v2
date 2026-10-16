// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_CMUX_TERM_CMUX_PANE_EXTENSION_PINS_H_
#define CHROME_BROWSER_CMUX_TERM_CMUX_PANE_EXTENSION_PINS_H_

#include <string>
#include <vector>

#include "base/callback_list.h"
#include "chrome/browser/cmux_term/window_model.h"

class Browser;

namespace cmux {

// Seeds a pane's pin list once from Chrome's profile-level toolbar model.
// Subsequent mutations are pane-local: tabs in the same pane share the list,
// while sibling panes in the workspace remain independent.
void InitializePaneExtensionPins(
    Browser* browser,
    PaneId pane,
    const std::vector<std::string>& initial_pinned_ids);

std::vector<std::string> GetPaneExtensionPins(Browser* browser, PaneId pane);

bool IsExtensionPinnedInPane(Browser* browser,
                             PaneId pane,
                             const std::string& extension_id);

void SetExtensionPinnedInPane(Browser* browser,
                              PaneId pane,
                              const std::string& extension_id,
                              bool pinned);

base::CallbackListSubscription AddPaneExtensionPinsChangedCallback(
    Browser* browser,
    PaneId pane,
    base::RepeatingClosure callback);

}  // namespace cmux

#endif  // CHROME_BROWSER_CMUX_TERM_CMUX_PANE_EXTENSION_PINS_H_
