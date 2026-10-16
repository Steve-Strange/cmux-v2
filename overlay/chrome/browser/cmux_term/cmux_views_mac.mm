// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later

// macOS-only platform glue for the cmux niri window: the AppKit activation
// seam (PlatformActivateWindow) and the terminal-pane factory
// (PlatformCreateTerminalSurface). The cross-platform window logic lives in
// cmux_views.cc.

#import <AppKit/AppKit.h>

#include <memory>

#include "base/functional/bind.h"
#include "base/task/single_thread_task_runner.h"
#include "chrome/browser/cmux_term/cmux_compositor_test_pane.h"
#include "chrome/browser/cmux_term/cmux_surface.h"
#include "chrome/browser/cmux_term/cmux_terminal_backend.h"
#include "chrome/browser/cmux_term/cmux_terminal_pane.h"
#include "chrome/browser/cmux_term/cmux_views.h"
#include "ui/events/event.h"
#include "ui/gfx/native_ui_types.h"
#include "ui/views/view.h"
#include "ui/views/widget/widget.h"

namespace cmux {

CmuxSurface* PlatformCreateTerminalSurface(
    views::View* parent,
    scoped_refptr<CmuxTerminalBackend> backend) {
  // CMUX_COMP_TEST=1 swaps in a CPU-filled gradient IOSurface composited via
  // ui::Layer (proves the GPU-buffer compositing path before wiring Ghostty).
  if (getenv("CMUX_COMP_TEST")) {
    auto surface = std::make_unique<CmuxCompositorTestPane>();
    CmuxSurface* s = surface.get();
    parent->AddChildView(std::move(surface));
    return s;
  }
  auto surface = std::make_unique<CmuxTerminalSurface>(std::move(backend));
  CmuxSurface* s = surface.get();
  parent->AddChildView(std::move(surface));
  return s;
}

void PlatformActivateWindow(views::Widget* widget) {
  if (!widget) {
    return;
  }
  // --no-startup-window can leave the app in a non-regular activation policy,
  // so our window never becomes the key window and plain keystrokes have no
  // first responder (chords still work via the app-wide event monitor). Force
  // a regular foreground app and make the window key.
  [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
  widget->Show();
  [NSApp activateIgnoringOtherApps:YES];
  NSWindow* nsw = widget->GetNativeWindow().GetNativeNSWindow();
  [nsw makeKeyAndOrderFront:nil];
  // The app may not be active yet (startup window just closed), so re-assert
  // activation + key on the next run loop turn once the app state has settled.
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(^{
        [NSApp activateIgnoringOtherApps:YES];
        [nsw makeKeyAndOrderFront:nil];
      }));
}

void PlatformConfigureFramelessWindow(views::Widget* widget) {
  if (!widget) {
    return;
  }
  NSWindow* nsw = widget->GetNativeWindow().GetNativeNSWindow();
  if (!nsw) {
    return;
  }
  nsw.styleMask = nsw.styleMask | NSWindowStyleMaskFullSizeContentView;
  nsw.titlebarAppearsTransparent = YES;
  nsw.titleVisibility = NSWindowTitleHidden;
  [[nsw standardWindowButton:NSWindowCloseButton] setHidden:NO];
  [[nsw standardWindowButton:NSWindowMiniaturizeButton] setHidden:NO];
  [[nsw standardWindowButton:NSWindowZoomButton] setHidden:NO];

}

void PlatformBeginWindowDrag(views::Widget* widget,
                             const ui::MouseEvent& event) {
  if (!widget) {
    return;
  }
  NSWindow* nsw = widget->GetNativeWindow().GetNativeNSWindow();
  if (!nsw) {
    return;
  }
  // Empty-chrome mouse events are born from AppKit events, so the native
  // NSEvent is the reliable way to get standard macOS drag behavior, including
  // Spaces and screen-edge handling.
  NSEvent* ns_event = event.native_event().Get();
  if (!ns_event) {
    return;
  }
  [nsw performWindowDragWithEvent:ns_event];
}

}  // namespace cmux
