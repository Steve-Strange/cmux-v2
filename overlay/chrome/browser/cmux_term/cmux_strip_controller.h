// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef CHROME_BROWSER_CMUX_TERM_CMUX_STRIP_CONTROLLER_H_
#define CHROME_BROWSER_CMUX_TERM_CMUX_STRIP_CONTROLLER_H_

#include <string>

#include "chrome/browser/cmux_term/cmux_keymap.h"
#include "chrome/browser/cmux_term/window_model.h"

class GURL;

namespace cmux {

// The strip operations the input glue (the macOS key monitor, the non-mac
// accelerators, the E2E driver) and the window entry point drive.
// CmuxWindowView implements this; exposing it as a pure-virtual interface
// lets cmux_input.mm act on the strip without a header for the concrete view
// -- which keeps the view's many inline virtual overrides in cmux_views.cc,
// where chromium-style allows them (a header would force them all
// out-of-line).
class CmuxStripController {
 public:
  virtual ~CmuxStripController() = default;

  // Columns.
  virtual void AddChromeColumn(const GURL& url) = 0;
  virtual void AddTerminalColumn() = 0;
  virtual void FocusColumn(int index) = 0;
  virtual void ToggleSidebar() = 0;
  // Toggle the window between fullscreen and normal.
  virtual void ToggleFullscreen() = 0;
  // Create and select a new logical Browser window inside the current cmux
  // native window. Native multi-window creation is a separate operation.
  virtual void NewWorkspace() = 0;
  virtual void CycleWorkspace(int delta) = 0;
  virtual void JumpToWorkspace(int index) = 0;
  // Focus the adjacent column (delta = -1 / +1).
  virtual void MoveFocus(int delta) = 0;
  // Focus the previous/next pane within the focused pane's column.
  virtual void MoveFocusVertical(int delta) = 0;
  // Cycle the focused pane's column width through configured fractions.
  virtual void CycleColumnWidth() = 0;

  // Splits: divide the focused pane in two within its column; the new pane's
  // first tab mirrors the focused tab's kind.
  virtual void SplitFocused(SplitOrientation orientation) = 0;

  // Tabs (all on the focused pane).
  virtual void NewTab(SurfaceKind kind) = 0;
  // Opens a terminal tab with an explicit shell command. An empty command
  // keeps Ghostty's configured/default shell.
  virtual void NewTerminalTab(std::string command, std::string title) = 0;
  virtual void SelectAdjacentTab(int delta) = 0;
  // Close the selected tab; closing the last tab closes the pane (and its
  // column when it was the column's only pane).
  virtual void CloseFocused() = 0;

  // Selected surface of the focused pane.
  virtual void FocusActiveOmnibar() = 0;
  virtual void ReloadFocused() = 0;
  virtual void GoBackFocused() = 0;
  virtual void GoForwardFocused() = 0;
  virtual void OpenDevToolsForFocused() = 0;
  // Move the focused pane's DevTools into its own top-level window (opening
  // it first if needed).
  virtual void UndockDevToolsForFocused() = 0;
  virtual bool HandleEscape() = 0;
  virtual KeyContext CurrentKeyContext() = 0;
  virtual bool HandleKeyChord(const KeyChord& chord,
                              const KeyContext& context,
                              bool is_repeat) = 0;
  // Like HandleKeyChord(), but only executes shortcuts that cmux must reserve
  // before the renderer sees them.
  virtual bool HandleReservedKeyChord(const KeyChord& chord,
                                      const KeyContext& context,
                                      bool is_repeat) = 0;
  virtual void ReloadKeymap() = 0;
  virtual void ReloadTheme() = 0;
  virtual std::u16string E2EFocusedOmniboxText() = 0;
  // E2E hooks (CMUX_*_SELFTEST): navigate the focused pane's selected surface
  // and read back its committed (post-rewrite) URL.
  virtual void E2ENavigateFocused(const GURL& url) = 0;
  virtual std::string E2EFocusedCommittedURL() = 0;
};

}  // namespace cmux

#endif  // CHROME_BROWSER_CMUX_TERM_CMUX_STRIP_CONTROLLER_H_
