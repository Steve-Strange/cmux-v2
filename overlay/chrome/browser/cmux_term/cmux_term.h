// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef CHROME_BROWSER_CMUX_TERM_CMUX_TERM_H_
#define CHROME_BROWSER_CMUX_TERM_CMUX_TERM_H_

#include "chrome/browser/chrome_browser_main_extra_parts.h"

class Profile;

// cmux-browser startup hook: suppresses Chrome's normal startup Browser
// window and brings up the cmux niri window (a views::Widget hosting web +
// Ghostty terminal panes) once the browser process is initialized. The macOS
// definition lives in cmux_term_mac.mm; other platforms use cmux_term.cc.
class ChromeBrowserMainExtraPartsCmuxTerm : public ChromeBrowserMainExtraParts {
 public:
  ChromeBrowserMainExtraPartsCmuxTerm() = default;
  ChromeBrowserMainExtraPartsCmuxTerm(
      const ChromeBrowserMainExtraPartsCmuxTerm&) = delete;
  ChromeBrowserMainExtraPartsCmuxTerm& operator=(
      const ChromeBrowserMainExtraPartsCmuxTerm&) = delete;
  ~ChromeBrowserMainExtraPartsCmuxTerm() override = default;

  void PreEarlyInitialization() override;
  void PreProfileInit() override;
  void PostProfileInit(Profile* profile, bool is_initial_profile) override;
  void PreBrowserStart() override;
  void PostBrowserStart() override;
  // Tears the niri window down BEFORE BrowserProcess::StartTearDown so pane
  // tab helpers unregister from profile KeyedServices while those still exist
  // (otherwise ~ObserverList CHECK-fails at exit; see cmux_views.h).
  void PostMainMessageLoopRun() override;
};

#endif  // CHROME_BROWSER_CMUX_TERM_CMUX_TERM_H_
