// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "chrome/browser/cmux_term/cmux_new_tab_page.h"

#include <cstdio>
#include <string_view>

namespace {

int g_checks = 0;
int g_failures = 0;

void Check(bool condition, const char* message) {
  ++g_checks;
  if (!condition) {
    ++g_failures;
    std::printf("  FAIL: %s\n", message);
  }
}

}  // namespace

int main() {
  using namespace cmux;
  std::printf("cmux_new_tab_page_test\n");
  Check(std::string_view(kBrowserDefaultNewTabPage) ==
            kBrowserNewTabPageChrome,
        "default is the local New Tab page");
  Check(IsSupportedBrowserNewTabPage("blank"), "blank is supported");
  Check(IsSupportedBrowserNewTabPage("new_tab"), "new_tab is supported");
  Check(!IsSupportedBrowserNewTabPage(""), "empty value is rejected");
  Check(!IsSupportedBrowserNewTabPage("chrome"), "chrome is rejected");
  Check(!IsSupportedBrowserNewTabPage("Blank"), "values are case-sensitive");
  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
