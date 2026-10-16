// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "chrome/browser/cmux_term/cmux_term.h"

#include <stdlib.h>

#include <string_view>

#include "base/command_line.h"
#include "base/environment.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/task/single_thread_task_runner.h"
#include "chrome/browser/cmux_term/cmux_app_icon.h"
#include "chrome/browser/cmux_term/cmux_extensions.h"
#include "chrome/browser/cmux_term/cmux_update_service.h"
#include "chrome/browser/cmux_term/cmux_views.h"
#include "ui/gfx/switches.h"

namespace {

bool CmuxViewsEnabled() {
  // Chromium's headless controller owns startup and teardown in this mode.
  // Starting cmux's native Widget/TUI graph alongside it gives the two UI
  // lifetimes incompatible shutdown ordering.
  if (base::CommandLine::ForCurrentProcess()->HasSwitch(switches::kHeadless)) {
    return false;
  }
  const char* value = getenv("CMUX_VIEWS");
  return !value || std::string_view(value) != "0";
}

}  // namespace

void ChromeBrowserMainExtraPartsCmuxTerm::PreEarlyInitialization() {
  // cmux is frequently launched from an IDE or agent process. Do not leak
  // NO_COLOR from that parent into every terminal child: its mere presence
  // makes Claude, npm, ripgrep, and many other CLIs suppress ANSI colors.
  base::Environment::Create()->UnSetVar("NO_COLOR");

  // This dev build is ad-hoc signed, so the macOS Keychain ACL doesn't
  // recognize it and os_crypt blocks on a Keychain prompt during startup,
  // intermittently hanging BrowserProcessImpl::PreMainMessageLoopRun. Force
  // the mock keychain (no real OS encryption) so the build always boots. This
  // runs before os_crypt initializes.
  base::CommandLine* cmd = base::CommandLine::ForCurrentProcess();
  if (!cmd->HasSwitch("use-mock-keychain")) {
    cmd->AppendSwitch("use-mock-keychain");
  }
  // NOTE: we deliberately do NOT add --no-startup-window. It
  // leaves the app unable to make our window the key window (keyboard input
  // dies). Instead we let Chrome's startup window open and close it via
  // SuppressStartupBrowsers() in PostBrowserStart, which keeps the app properly
  // activated so our niri window can take key.
}

void ChromeBrowserMainExtraPartsCmuxTerm::PreBrowserStart() {
  // Register the startup-browser closer BEFORE Chrome creates its initial
  // window, so OnBrowserAdded catches and closes it (leaving only our niri
  // window). Registering in PostBrowserStart is too late.
  if (!CmuxViewsEnabled()) {
    return;
  }
  cmux::SuppressStartupBrowsers();
}

void ChromeBrowserMainExtraPartsCmuxTerm::PreProfileInit() {
  cmux::RegisterUserExternalExtensions();
}

void ChromeBrowserMainExtraPartsCmuxTerm::PostProfileInit(Profile* profile,
                                                          bool) {
  if (base::CommandLine::ForCurrentProcess()->HasSwitch(switches::kHeadless)) {
    return;
  }
  cmux::RegisterProfileExtensions(profile);
}

void ChromeBrowserMainExtraPartsCmuxTerm::PostMainMessageLoopRun() {
  // Destroy the niri window (and every pane's WebContents + tab
  // helpers) before BrowserProcess::StartTearDown destroys the profile
  // KeyedServices those helpers observe.
  VLOG(1) << "cmux: PostMainMessageLoopRun (shutdown) reached";
  cmux::CloseViewsWebWindow();
  cmux::StopCmuxUpdateService();
}

void ChromeBrowserMainExtraPartsCmuxTerm::PostBrowserStart() {
  if (!CmuxViewsEnabled()) {
    return;
  }
  cmux::StartAppIconController();
  // Bridge startup until ShowViewsWebWindow constructs real workspace
  // Browsers; it releases this keep-alive immediately afterward.
  cmux::HoldViewsKeepAlive();
  cmux::StartCmuxUpdateService();
  // Defer window creation to after browser init (synchronous creation here can
  // deadlock startup).
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(&cmux::ShowViewsWebWindow));
}
