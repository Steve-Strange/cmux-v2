// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef CHROME_BROWSER_CMUX_TERM_CMUX_VIEWS_H_
#define CHROME_BROWSER_CMUX_TERM_CMUX_VIEWS_H_

#include <optional>
#include <string>

#include "base/memory/scoped_refptr.h"
#include "chrome/browser/cmux_term/cmux_keymap.h"
#include "chrome/browser/cmux_term/cmux_theme.h"

// The Chromium-Views implementation of the cmux window. It builds the UI out
// of views::View so it can host the REAL Chrome omnibox, Chrome extension UI,
// the keybinding system, and terminal surfaces from one cross-platform path.
// The legacy AppKit strip was retired; CMUX_VIEWS=0 means stock Chromium.

namespace input {
struct NativeWebKeyboardEvent;
}  // namespace input

namespace views {
class View;
class Widget;
}  // namespace views

namespace content {
class WebContents;
}  // namespace content

class Profile;

namespace ui {
class MouseEvent;
}

namespace cmux {

class CmuxStripController;
class CmuxSurface;
class CmuxTerminalBackend;

struct CmuxAppliedTheme {
  std::string name;
  CmuxThemePalette palette;
};

// Create + show a top-level views::Widget containing a views::WebView bound to
// a freshly created WebContents. V1 milestone: prove Chrome Views render in
// our process before adding the real omnibox.
void ShowViewsWebWindow();
// Always creates another OS-native cmux window. Used by Cmd+Shift+N and the
// cmux app menu; unlike ShowViewsWebWindow(), it never just raises an existing
// container.
void ShowNewViewsWebWindow();

// Destroy the niri window synchronously -- panes, WebContents, and their tab
// helpers with it. MUST run at shutdown before BrowserProcess::StartTearDown
// (i.e. from PostMainMessageLoopRun): the helpers observe profile
// KeyedServices (e.g. BookmarkTabHelper -> BookmarkModel), so if the profile
// dies first, ~ObserverList CHECK-fails on the still-registered observers.
void CloseViewsWebWindow();

// Platform seam: show + activate the niri window and make it the key/foreground
// window. macOS (cmux_views_mac.mm) drives NSApp activation policy +
// makeKeyAndOrderFront (needed after the startup window closes); other
// platforms (in cmux_views.cc) use Views' cross-platform Show()/Activate().
void PlatformActivateWindow(views::Widget* widget);

// Platform seam: configure custom frame/titlebar chrome. macOS hides the native
// titlebar while keeping traffic lights and resize controls; non-mac is a no-op
// this pass.
void PlatformConfigureFramelessWindow(views::Widget* widget);

// Platform seam: begin native window dragging from a Views mouse event in
// otherwise-empty rail/tab-strip chrome. macOS uses
// -performWindowDragWithEvent:; non-mac is a no-op.
void PlatformBeginWindowDrag(views::Widget* widget,
                             const ui::MouseEvent& event);

// Platform seam: create a terminal surface (one tab's content), add it as a
// child of `parent`, and return its cross-platform CmuxSurface interface.
// The ref-counted backend owns the cmux-tui surface independently of the
// returned native frontend. macOS (cmux_views_mac.mm) hosts a Ghostty NSView
// (or the IOSurface compositor test pane when CMUX_COMP_TEST is set);
// Linux/Windows (cmux_terminal_pane_linux.cc) drive a host-owned OpenGL
// surface. Undefined on other platforms (callers guard with BUILDFLAG).
CmuxSurface* PlatformCreateTerminalSurface(
    views::View* parent,
    scoped_refptr<CmuxTerminalBackend> backend);

// Close Chrome's normal Browser windows (now + as they open) so only our niri
// window remains. Used instead of --no-startup-window, which leaves the app
// unable to make our window the key window (so keyboard input dies).
void SuppressStartupBrowsers();
void FinishStartupBrowserSuppression();

// Acquire the short-lived BROWSER keep-alive that bridges Chrome startup to
// construction of the real workspace Browsers. ShowViewsWebWindow releases it
// as soon as those Browsers exist.
void HoldViewsKeepAlive();

CmuxStripController* GetStripController();

// Convert Chromium's native web-key event form into cmux's keymap chord space.
// Kept with cmux_views.cc because it depends on Chromium ui::Accelerator
// types, while cmux_keymap.{h,cc} remains host-compilable.
std::optional<KeyChord> KeyChordFromNativeWebKeyboardEvent(
    const input::NativeWebKeyboardEvent& event);

// Narrow accessors used by per-Browser extension containers and toolbar UI.
content::WebContents* GetActiveCmuxWebContents();
views::Widget* GetCmuxViewsWidget();
bool IsAnyCmuxWindowActive();

// Apply the current Ghostty-derived Chrome surface ColorProvider overrides to a
// standalone cmux widget. Anchored child widgets inherit from the main cmux
// widget; unanchored windows such as undocked DevTools register so theme.reload
// can retarget them live.
void ApplyCmuxChromeSurfaceThemeOverrides(views::Widget* widget);
void RegisterCmuxChromeSurfaceThemeWidget(views::Widget* widget);
void UnregisterCmuxChromeSurfaceThemeWidget(views::Widget* widget);

// Reloads the shared layout/appearance configuration in every open cmux
// window. Used by cmux://settings after an atomic preference write.
void ReloadCmuxCustomization();

// Exact Ghostty-derived palette currently applied to the active cmux window.
// The theme name makes asynchronous settings saves distinguish the newly
// applied palette from the previous one.
std::optional<CmuxAppliedTheme> GetCmuxAppliedTheme();

}  // namespace cmux

#endif  // CHROME_BROWSER_CMUX_TERM_CMUX_VIEWS_H_
