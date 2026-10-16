// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef CHROME_BROWSER_CMUX_TERM_CMUX_NEW_TAB_PAGE_H_
#define CHROME_BROWSER_CMUX_TERM_CMUX_NEW_TAB_PAGE_H_

#include <string_view>

namespace cmux {

inline constexpr char kBrowserNewTabPageChrome[] = "new_tab";
inline constexpr char kBrowserNewTabPageBlank[] = "blank";
inline constexpr char kBrowserDefaultNewTabPage[] = "new_tab";

inline bool IsSupportedBrowserNewTabPage(std::string_view value) {
  return value == kBrowserNewTabPageChrome ||
         value == kBrowserNewTabPageBlank;
}

}  // namespace cmux

#endif  // CHROME_BROWSER_CMUX_TERM_CMUX_NEW_TAB_PAGE_H_
