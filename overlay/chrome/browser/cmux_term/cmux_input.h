// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef CHROME_BROWSER_CMUX_TERM_CMUX_INPUT_H_
#define CHROME_BROWSER_CMUX_TERM_CMUX_INPUT_H_

#include <string_view>

namespace views {
class Widget;
}

namespace cmux {

class CmuxStripController;

// macOS input glue for the niri window. Installs the app-wide NSEvent key
// monitor (niri chords -> strip operations on `strip`, scoped to `widget`'s
// window) and the standard Edit menu (Cmd-A/C/V/X/Z/undo/redo routed through
// the responder chain, so they reach whatever is first responder in any
// layout).
void InstallKeyMonitor(CmuxStripController* strip, views::Widget* widget);
void RemoveKeyEventMonitor(views::Widget* widget);
// Removes every NSEvent monitor this file installed; safe to call twice /
// without prior install.
void RemoveKeyEventMonitors();
void InstallEditMenu();
// Executes a responder-chain edit command for a user-configured shortcut.
// Returns false when `command_id` is not a supported native edit command.
bool ExecuteNativeEditCommand(std::string_view command_id);

// Optional self-test (CMUX_E2E=1): drives the real key path via posted NSEvents
// and navigates `strip`'s panes; CDP observes the result. No-op otherwise.
void RunE2ESelfTest(CmuxStripController* strip, views::Widget* widget);

}  // namespace cmux

#endif  // CHROME_BROWSER_CMUX_TERM_CMUX_INPUT_H_
