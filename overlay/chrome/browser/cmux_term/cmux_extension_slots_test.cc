// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Host-compilable unit test for pure cmux extension action filtering (no
// Chromium, no gtest). Build + run:
//
//   c++ -std=c++17 -I overlay \
//     overlay/chrome/browser/cmux_term/cmux_extension_slots.cc \
//     overlay/chrome/browser/cmux_term/cmux_extension_slots_test.cc \
//     -o /tmp/cmux_extension_slots_test && /tmp/cmux_extension_slots_test

#include "chrome/browser/cmux_term/cmux_extension_slots.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace cmux;

namespace {

int g_failures = 0;
int g_checks = 0;

void Check(bool ok, const char* what) {
  ++g_checks;
  if (!ok) {
    ++g_failures;
    std::printf("  FAIL: %s\n", what);
  }
}

std::vector<std::string> Build(std::vector<std::string> pinned_ids,
                               std::vector<std::string> real_action_ids) {
  return BuildVisibleCmuxExtensionActionIds(pinned_ids, real_action_ids);
}

void TestColdRegistryDoesNotCreateFakeControls() {
  const std::vector<std::string> ids = Build({"ublock", "bitwarden"}, {});
  Check(ids.empty(), "cold registry does not expose stale pinned ids");
}

void TestRealActionsKeepPinnedOrder() {
  const std::vector<std::string> ids =
      Build({"bitwarden", "ublock", "other"},
            {"other", "ublock", "bitwarden"});
  Check(ids.size() == 3, "all real pinned actions remain visible");
  Check(ids[0] == "bitwarden", "first pinned real action stays first");
  Check(ids[1] == "ublock", "second pinned real action stays second");
  Check(ids[2] == "other", "third pinned real action stays third");
}

void TestMissingDisabledAndUnpinnedActionsAreSkipped() {
  const std::vector<std::string> ids =
      Build({"missing", "disabled", "real"}, {"real", "unpinned"});
  Check(ids.size() == 1, "only a real pinned action remains visible");
  Check(ids[0] == "real", "real pinned action remains visible");
}

void TestDuplicateAndEmptyIdsAreIgnored() {
  const std::vector<std::string> ids =
      Build({"", "a", "a", "b", "b"}, {"a", "b"});
  Check(ids.size() == 2, "duplicates and empty ids are ignored");
  Check(ids[0] == "a", "first unique real action is kept");
  Check(ids[1] == "b", "second unique real action is kept");
}

void TestPaneLocalPinMutation() {
  std::vector<std::string> left = {"ublock"};
  std::vector<std::string> right = {"ublock"};

  Check(SetCmuxPaneExtensionPinned(&left, "claude", true),
        "pinning a new action reports a change");
  Check(CmuxExtensionSlotContainsId(left, "claude"),
        "new action is pinned in the target pane");
  Check(!CmuxExtensionSlotContainsId(right, "claude"),
        "pinning one pane does not affect another pane");
  Check(!SetCmuxPaneExtensionPinned(&left, "claude", true),
        "pinning an already pinned action is idempotent");
  Check(SetCmuxPaneExtensionPinned(&left, "claude", false),
        "unpinning reports a change");
  Check(!CmuxExtensionSlotContainsId(left, "claude"),
        "unpinning removes the action");
  Check(!SetCmuxPaneExtensionPinned(&left, "", true),
        "empty extension ids are ignored");
}

}  // namespace

int main() {
  std::printf("cmux_extension_slots_test\n");
  TestColdRegistryDoesNotCreateFakeControls();
  TestRealActionsKeepPinnedOrder();
  TestMissingDisabledAndUnpinnedActionsAreSkipped();
  TestDuplicateAndEmptyIdsAreIgnored();
  TestPaneLocalPinMutation();
  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
