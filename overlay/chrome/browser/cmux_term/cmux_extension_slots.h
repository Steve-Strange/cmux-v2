// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef CHROME_BROWSER_CMUX_TERM_CMUX_EXTENSION_SLOTS_H_
#define CHROME_BROWSER_CMUX_TERM_CMUX_EXTENSION_SLOTS_H_

#include <algorithm>
#include <string>
#include <vector>

namespace cmux {

inline bool CmuxExtensionActionIdsContain(
    const std::vector<std::string>& ids,
    const std::string& id) {
  return std::find(ids.begin(), ids.end(), id) != ids.end();
}

inline bool CmuxExtensionSlotContainsId(
    const std::vector<std::string>& ids,
    const std::string& id) {
  return CmuxExtensionActionIdsContain(ids, id);
}

// Applies Chrome's pin/unpin semantics to one pane-local ordered pin list.
// Pinning appends a previously absent action; unpinning removes every stale
// duplicate so a pane can never materialize the same toolbar action twice.
inline bool SetCmuxPaneExtensionPinned(std::vector<std::string>* pinned_ids,
                                      const std::string& id,
                                      bool pinned) {
  if (!pinned_ids || id.empty()) {
    return false;
  }
  const bool was_pinned = CmuxExtensionSlotContainsId(*pinned_ids, id);
  if (pinned == was_pinned) {
    return false;
  }
  if (pinned) {
    pinned_ids->push_back(id);
  } else {
    pinned_ids->erase(
        std::remove(pinned_ids->begin(), pinned_ids->end(), id),
        pinned_ids->end());
  }
  return true;
}

// Returns the loaded action ids that should have toolbar buttons, preserving
// pinned order. Chromium only materializes ToolbarActionViews for real action
// models: a stale or not-yet-installed pinned id must not become a disabled,
// generic-looking toolbar control.
inline std::vector<std::string> BuildVisibleCmuxExtensionActionIds(
    const std::vector<std::string>& pinned_ids,
    const std::vector<std::string>& real_action_ids) {
  std::vector<std::string> visible_ids;
  std::vector<std::string> seen_ids;
  for (const std::string& id : pinned_ids) {
    if (id.empty() || CmuxExtensionActionIdsContain(seen_ids, id)) {
      continue;
    }
    seen_ids.push_back(id);

    if (CmuxExtensionActionIdsContain(real_action_ids, id)) {
      visible_ids.push_back(id);
    }
  }
  return visible_ids;
}

}  // namespace cmux

#endif  // CHROME_BROWSER_CMUX_TERM_CMUX_EXTENSION_SLOTS_H_
