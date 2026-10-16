// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef CHROME_BROWSER_CMUX_TERM_CMUX_TERMINAL_LIFECYCLE_H_
#define CHROME_BROWSER_CMUX_TERM_CMUX_TERMINAL_LIFECYCLE_H_

namespace cmux {

// Canonical lifecycle values used by cmux-tui's terminal registry and every
// frontend projection. Keep these in one dependency-free header so placement
// reconciliation and renderer recovery cannot drift into parallel enums.
enum class CmuxTerminalLifecycle {
  kLaunching,
  kAdopting,
  kRunning,
  kExited,
  kTombstoned,
};

}  // namespace cmux

#endif  // CHROME_BROWSER_CMUX_TERM_CMUX_TERMINAL_LIFECYCLE_H_
