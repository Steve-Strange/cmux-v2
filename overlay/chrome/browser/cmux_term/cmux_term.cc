// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later

// Non-mac (Linux/Ozone) definition of the cmux startup extra-parts. The macOS
// build uses cmux_term_mac.mm for mac startup glue; this file provides the
// cross-platform Chromium-Views startup:
// suppress Chrome's startup browser, hold a keep-alive, and bring up the
// cmux::ShowViewsWebWindow() niri window (which hosts native-omnibox panes).

#include "chrome/browser/cmux_term/cmux_term.h"

#include "build/build_config.h"

#if !BUILDFLAG(IS_MAC)

#include <cstdlib>
#include <string_view>

#include "base/command_line.h"
#include "base/environment.h"
#include "base/functional/bind.h"
#include "base/task/single_thread_task_runner.h"
#include "chrome/browser/cmux_term/cmux_extensions.h"
#include "chrome/browser/cmux_term/cmux_telemetry.h"
#include "chrome/browser/cmux_term/cmux_update_service.h"
#include "chrome/browser/cmux_term/cmux_views.h"
#include "ui/gfx/switches.h"

namespace {

bool CmuxViewsEnabled() {
  // Chromium's headless controller owns startup and teardown in this mode.
  // Starting cmux's native Widget/TUI graph alongside Ozone Headless makes
  // their incompatible lifetimes overlap.
  if (base::CommandLine::ForCurrentProcess()->HasSwitch(switches::kHeadless)) {
    return false;
  }
  const char* value = std::getenv("CMUX_VIEWS");
  return !value || std::string_view(value) != "0";
}

}  // namespace

void ChromeBrowserMainExtraPartsCmuxTerm::PreEarlyInitialization() {
  // cmux is frequently launched from an IDE or agent process. Do not leak
  // NO_COLOR from that parent into every terminal child: its mere presence
  // makes Claude, npm, ripgrep, and many other CLIs suppress ANSI colors.
  base::Environment::Create()->UnSetVar("NO_COLOR");

  // Mirror the mac path: force the mock keychain so os_crypt never blocks on an
  // OS keyring prompt during startup (harmless where unused).
  base::CommandLine* cmd = base::CommandLine::ForCurrentProcess();
  if (!cmd->HasSwitch("use-mock-keychain")) {
    cmd->AppendSwitch("use-mock-keychain");
  }
}

void ChromeBrowserMainExtraPartsCmuxTerm::PreBrowserStart() {
  // Close Chrome's initial Browser window as it's added, leaving only our niri
  // window. Registered before the startup window is created.
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
  cmux::StopCmuxTelemetry();
  cmux::CloseViewsWebWindow();
  cmux::StopCmuxUpdateService();
}

void ChromeBrowserMainExtraPartsCmuxTerm::PostBrowserStart() {
  if (!CmuxViewsEnabled()) {
    return;
  }
  // Bridge startup until ShowViewsWebWindow constructs real workspace
  // Browsers; it releases this keep-alive immediately afterward.
  cmux::HoldViewsKeepAlive();
  cmux::StartCmuxUpdateService();
  cmux::StartCmuxTelemetry();
  // Defer window creation to after browser init (synchronous creation here can
  // deadlock startup).
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(&cmux::ShowViewsWebWindow));
}

#endif  // !BUILDFLAG(IS_MAC)
