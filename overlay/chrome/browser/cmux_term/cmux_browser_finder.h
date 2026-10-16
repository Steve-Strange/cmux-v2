// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef CHROME_BROWSER_CMUX_TERM_CMUX_BROWSER_FINDER_H_
#define CHROME_BROWSER_CMUX_TERM_CMUX_BROWSER_FINDER_H_

#include "chrome/common/chrome_version.h"

#if CHROME_VERSION_MAJOR >= 151
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/browser_window/public/global_browser_collection.h"
#else
#include "chrome/browser/ui/browser_finder.h"
#endif

class Browser;

namespace content {
class WebContents;
}

namespace cmux {

// Chromium 151 removed chrome::FindBrowserWithTab() in favor of the browser
// collection API. Keep the version boundary here so the cmux overlay remains
// buildable against the M149 rollback checkout.
inline Browser* FindBrowserWithTab(const content::WebContents* web_contents) {
  if (!web_contents) {
    return nullptr;
  }
#if CHROME_VERSION_MAJOR >= 151
  BrowserWindowInterface* browser_window =
      GlobalBrowserCollection::GetInstance()->FindBrowserWithTab(web_contents);
  return browser_window ? browser_window->GetBrowserForMigrationOnly()
                        : nullptr;
#else
  return chrome::FindBrowserWithTab(web_contents);
#endif
}

}  // namespace cmux

#endif  // CHROME_BROWSER_CMUX_TERM_CMUX_BROWSER_FINDER_H_
