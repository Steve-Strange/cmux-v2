// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/cmux_term/cmux_pane_extension_pins.h"

#include <map>
#include <memory>
#include <tuple>
#include <utility>

#include "base/check.h"
#include "base/memory/raw_ptr.h"
#include "base/no_destructor.h"
#include "chrome/browser/cmux_term/cmux_extension_slots.h"

namespace cmux {
namespace {

struct PanePinKey {
  raw_ptr<Browser> browser = nullptr;
  PaneId pane = kInvalidId;

  friend bool operator<(const PanePinKey& lhs, const PanePinKey& rhs) {
    return std::tie(lhs.browser, lhs.pane) <
           std::tie(rhs.browser, rhs.pane);
  }
};

struct PanePinState {
  bool initialized = false;
  std::vector<std::string> pinned_ids;
  base::RepeatingClosureList changed_callbacks;
};

std::map<PanePinKey, std::unique_ptr<PanePinState>>& PanePinStates() {
  static base::NoDestructor<
      std::map<PanePinKey, std::unique_ptr<PanePinState>>>
      states;
  return *states;
}

PanePinState& StateFor(Browser* browser, PaneId pane) {
  CHECK(browser);
  CHECK_NE(pane, kInvalidId);
  auto& state = PanePinStates()[{browser, pane}];
  if (!state) {
    state = std::make_unique<PanePinState>();
  }
  return *state;
}

}  // namespace

void InitializePaneExtensionPins(
    Browser* browser,
    PaneId pane,
    const std::vector<std::string>& initial_pinned_ids) {
  PanePinState& state = StateFor(browser, pane);
  if (state.initialized) {
    return;
  }
  state.initialized = true;
  for (const std::string& id : initial_pinned_ids) {
    SetCmuxPaneExtensionPinned(&state.pinned_ids, id, true);
  }
}

std::vector<std::string> GetPaneExtensionPins(Browser* browser, PaneId pane) {
  return StateFor(browser, pane).pinned_ids;
}

bool IsExtensionPinnedInPane(Browser* browser,
                             PaneId pane,
                             const std::string& extension_id) {
  return CmuxExtensionSlotContainsId(StateFor(browser, pane).pinned_ids,
                                     extension_id);
}

void SetExtensionPinnedInPane(Browser* browser,
                              PaneId pane,
                              const std::string& extension_id,
                              bool pinned) {
  PanePinState& state = StateFor(browser, pane);
  state.initialized = true;
  if (SetCmuxPaneExtensionPinned(&state.pinned_ids, extension_id, pinned)) {
    state.changed_callbacks.Notify();
  }
}

base::CallbackListSubscription AddPaneExtensionPinsChangedCallback(
    Browser* browser,
    PaneId pane,
    base::RepeatingClosure callback) {
  return StateFor(browser, pane).changed_callbacks.Add(std::move(callback));
}

}  // namespace cmux
