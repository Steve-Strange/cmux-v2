// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef CHROME_BROWSER_CMUX_TERM_CMUX_DEMO_PAGE_H_
#define CHROME_BROWSER_CMUX_TERM_CMUX_DEMO_PAGE_H_

#include "url/gurl.h"

namespace cmux {

// A rich self-contained dogfood page (text/select/range/color inputs,
// contenteditable, drag-and-drop, a long scroll region) written to a temp file
// and returned as a file:// URL so every web API behaves same-origin.
GURL CmuxDemoURL();

}  // namespace cmux

#endif  // CHROME_BROWSER_CMUX_TERM_CMUX_DEMO_PAGE_H_
