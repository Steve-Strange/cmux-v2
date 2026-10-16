// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "chrome/services/cmux_terminal_renderer/public/cpp/cmux_ghostty_resources.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace {

int failures = 0;

void Check(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

}  // namespace

int main() {
  const std::string bundled =
      "/Applications/cmux test.app/Contents/Resources/ghostty";
  const std::string developer =
      "/Applications/Ghostty.app/Contents/Resources/ghostty";
  std::unordered_set<std::string> complete;
  int probes = 0;
  const auto probe = [&complete, &probes](std::string_view path) {
    ++probes;
    return complete.find(std::string(path)) != complete.end();
  };

  Check(cmux::SelectGhosttyResourcesDirectory(
            "/explicit/resources", "/Applications/cmux test.app",
            {developer}, probe) == "/explicit/resources" &&
            probes == 0,
        "a non-empty explicit resource directory wins without probing");

  complete.insert(bundled);
  Check(cmux::SelectGhosttyResourcesDirectory(
            "", "/Applications/cmux test.app", {developer}, probe) ==
            bundled,
        "the complete outer bundle is preferred");

  complete.clear();
  complete.insert(developer);
  Check(!cmux::SelectGhosttyResourcesDirectory(
             "", "/Applications/cmux test.app", {developer}, probe),
        "an incomplete product bundle never borrows developer resources");

  Check(cmux::SelectGhosttyResourcesDirectory("", "", {developer}, probe) ==
            developer,
        "an unbundled platform may use its complete colocated tree");

  complete.clear();
  Check(!cmux::SelectGhosttyResourcesDirectory(
             "", "/Applications/cmux test.app", {developer}, probe),
        "an incomplete resource tree is never selected");
  Check(!cmux::SelectGhosttyResourcesDirectory(
             "", "/Applications/cmux test.app", {developer}, {}),
        "selection fails closed without a completeness probe");

  if (failures != 0) {
    std::cerr << failures << " Ghostty resource checks failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "Ghostty resource checks passed\n";
  return EXIT_SUCCESS;
}
