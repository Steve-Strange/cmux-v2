// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "chrome/browser/cmux_term/cmux_input.h"

#import <AppKit/AppKit.h>

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/memory/raw_ptr.h"
#include "base/no_destructor.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/single_thread_task_runner.h"
#include "chrome/browser/cmux_term/cmux_demo_page.h"
#include "chrome/browser/cmux_term/cmux_keymap.h"
#include "chrome/browser/cmux_term/cmux_strip_controller.h"
#include "chrome/browser/cmux_term/cmux_views.h"
#include "ui/base/ime/input_method.h"
#include "ui/base/ime/text_input_client.h"
#include "ui/gfx/native_ui_types.h"
#include "ui/views/controls/textfield/textfield.h"
#include "ui/views/focus/focus_manager.h"
#include "ui/views/view.h"
#include "ui/views/widget/widget.h"

namespace cmux {

namespace {
// The installed NSEvent monitor token and target state.
id g_key_monitor = nil;
struct KeyMonitorTarget {
  raw_ptr<CmuxStripController> strip = nullptr;
  NSWindow* window = nil;
};

std::vector<KeyMonitorTarget>& KeyMonitorTargets() {
  static base::NoDestructor<std::vector<KeyMonitorTarget>> targets;
  return *targets;
}

constexpr unsigned short kVKReturn = 36;
constexpr unsigned short kVKTab = 48;
constexpr unsigned short kVKDelete = 51;
constexpr unsigned short kVKEscape = 53;
constexpr unsigned short kVKKeypadEnter = 76;
constexpr unsigned short kVKHome = 115;
constexpr unsigned short kVKPageUp = 116;
constexpr unsigned short kVKForwardDelete = 117;
constexpr unsigned short kVKEnd = 119;
constexpr unsigned short kVKPageDown = 121;
constexpr unsigned short kVKF1 = 122;
constexpr unsigned short kVKF2 = 120;
constexpr unsigned short kVKF3 = 99;
constexpr unsigned short kVKF4 = 118;
constexpr unsigned short kVKF5 = 96;
constexpr unsigned short kVKF6 = 97;
constexpr unsigned short kVKF7 = 98;
constexpr unsigned short kVKF8 = 100;
constexpr unsigned short kVKF9 = 101;
constexpr unsigned short kVKF10 = 109;
constexpr unsigned short kVKF11 = 103;
constexpr unsigned short kVKF12 = 111;
constexpr unsigned short kVKF13 = 105;
constexpr unsigned short kVKF14 = 107;
constexpr unsigned short kVKF15 = 113;
constexpr unsigned short kVKF16 = 106;
constexpr unsigned short kVKF17 = 64;
constexpr unsigned short kVKF18 = 79;
constexpr unsigned short kVKF19 = 80;
constexpr unsigned short kVKF20 = 90;
constexpr unsigned short kVKLeftArrow = 123;
constexpr unsigned short kVKRightArrow = 124;
constexpr unsigned short kVKDownArrow = 125;
constexpr unsigned short kVKUpArrow = 126;
constexpr unsigned short kVKL = 37;
constexpr unsigned short kVKN = 45;

std::optional<std::string> KeyTokenForEvent(NSEvent* e) {
  switch (e.keyCode) {
    case kVKReturn:
    case kVKKeypadEnter:
      return std::string("enter");
    case kVKTab:
      return std::string("tab");
    case kVKDelete:
      return std::string("backspace");
    case kVKEscape:
      return std::string("escape");
    case kVKHome:
      return std::string("home");
    case kVKPageUp:
      return std::string("pageup");
    case kVKForwardDelete:
      return std::string("delete");
    case kVKEnd:
      return std::string("end");
    case kVKPageDown:
      return std::string("pagedown");
    case kVKLeftArrow:
      return std::string("left");
    case kVKRightArrow:
      return std::string("right");
    case kVKDownArrow:
      return std::string("down");
    case kVKUpArrow:
      return std::string("up");
    case kVKF1:
      return std::string("f1");
    case kVKF2:
      return std::string("f2");
    case kVKF3:
      return std::string("f3");
    case kVKF4:
      return std::string("f4");
    case kVKF5:
      return std::string("f5");
    case kVKF6:
      return std::string("f6");
    case kVKF7:
      return std::string("f7");
    case kVKF8:
      return std::string("f8");
    case kVKF9:
      return std::string("f9");
    case kVKF10:
      return std::string("f10");
    case kVKF11:
      return std::string("f11");
    case kVKF12:
      return std::string("f12");
    case kVKF13:
      return std::string("f13");
    case kVKF14:
      return std::string("f14");
    case kVKF15:
      return std::string("f15");
    case kVKF16:
      return std::string("f16");
    case kVKF17:
      return std::string("f17");
    case kVKF18:
      return std::string("f18");
    case kVKF19:
      return std::string("f19");
    case kVKF20:
      return std::string("f20");
    default:
      break;
  }

  // Use the unmodified codepoint and carry Shift as a modifier separately.
  // charactersIgnoringModifiers can report control-modified text for chords
  // like Ctrl-[; charactersByApplyingModifiers:0 stays in the key-token space.
  NSString* chars = [e charactersByApplyingModifiers:0].lowercaseString;
  if (!chars || chars.length == 0) {
    return std::nullopt;
  }
  const char* utf8 = chars.UTF8String;
  if (!utf8) {
    return std::nullopt;
  }
  std::string error;
  return NormalizeKeyToken(std::string(utf8), &error);
}

std::optional<KeyChord> ChordFromEvent(NSEvent* e) {
  std::optional<std::string> key = KeyTokenForEvent(e);
  if (!key) {
    return std::nullopt;
  }
  KeyChord chord;
  chord.key = *key;
  const NSEventModifierFlags flags = e.modifierFlags;
  if ((flags & NSEventModifierFlagCommand) != 0) {
    chord.modifiers |= kKeyModCmd;
  }
  if ((flags & NSEventModifierFlagControl) != 0) {
    chord.modifiers |= kKeyModCtrl;
  }
  if ((flags & NSEventModifierFlagOption) != 0) {
    chord.modifiers |= kKeyModAlt;
  }
  if ((flags & NSEventModifierFlagShift) != 0) {
    chord.modifiers |= kKeyModShift;
  }
  return chord;
}
}  // namespace

void InstallKeyMonitor(CmuxStripController* controller,
                       views::Widget* widget) {
  NSWindow* target_window = widget->GetNativeWindow().GetNativeNSWindow();
  for (const KeyMonitorTarget& target : KeyMonitorTargets()) {
    if (target.window == target_window) {
      return;
    }
  }
  KeyMonitorTargets().push_back({controller, target_window});
  if (g_key_monitor) {
    return;
  }

  g_key_monitor = [NSEvent
      addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown
                                   handler:^NSEvent*(NSEvent* e) {
                                     CmuxStripController* strip = nullptr;
                                     for (const KeyMonitorTarget& target :
                                          KeyMonitorTargets()) {
                                       if (target.window == e.window) {
                                         strip = target.strip;
                                         break;
                                       }
                                     }
                                     std::optional<KeyChord> chord =
                                         ChordFromEvent(e);
                                     if (!strip) {
                                       // Once the last cmux container closes,
                                       // there is no window-scoped controller
                                       // left to consume Chrome's New Window
                                       // shortcuts. Keep these two native
                                       // chords inside cmux instead of letting
                                       // Chromium create a stock BrowserView.
                                       // Post out of AppKit key dispatch: native
                                       // Widget construction can re-enter the
                                       // menu/event loop.
                                       const int new_window_modifiers =
                                           chord ? chord->modifiers &
                                                       ~kKeyModShift
                                                 : 0;
                                       if (chord && chord->key == "n" &&
                                           new_window_modifiers == kKeyModCmd &&
                                           (chord->modifiers &
                                            ~(kKeyModCmd | kKeyModShift)) == 0) {
                                         base::SingleThreadTaskRunner::
                                             GetCurrentDefault()
                                                 ->PostTask(
                                                     FROM_HERE,
                                                     base::BindOnce(
                                                         &ShowNewViewsWebWindow));
                                         return nil;
                                       }
                                       return e;
                                     }
                                     if (chord &&
                                         strip->HandleKeyChord(
                                             *chord, strip->CurrentKeyContext(),
                                             e.isARepeat)) {
                                       return nil;
                                     }
                                     return e;
                                   }];
}

void RemoveKeyEventMonitor(views::Widget* widget) {
  if (!widget) {
    return;
  }
  NSWindow* window = widget->GetNativeWindow().GetNativeNSWindow();
  std::erase_if(KeyMonitorTargets(),
                [window](const KeyMonitorTarget& target) {
                  return target.window == window;
                });
  // Keep the process-wide monitor installed after the last cmux window closes
  // so Cmd-N/Cmd-Shift-N can recreate a native cmux container. The monitor is
  // removed only by RemoveKeyEventMonitors() during process teardown.
}

void RemoveKeyEventMonitors() {
  if (g_key_monitor) {
    [NSEvent removeMonitor:g_key_monitor];
    g_key_monitor = nil;
  }
  KeyMonitorTargets().clear();
}

// Install a standard Edit menu whose items dispatch through the responder chain
// (target=nil) using the conventional selectors. This is the GENERAL way macOS
// delivers editing shortcuts: Cmd-A/C/V/X/Z reach whatever is first responder
// (the web pane's RenderWidgetHostViewCocoa, the Ghostty terminal, or the real
// LocationBarView omnibox), and key-equivalent matching is keyboard-layout
// aware, so it works for every language. The shared cmux widget is not a
// BrowserView command target even though each workspace owns a Browser, so we
// install this responder-chain menu for the physical window.
void InstallEditMenu() {
  NSMenu* main = NSApp.mainMenu;
  if (!main) {
    return;
  }
  for (NSInteger i = main.numberOfItems - 1; i >= 0; i--) {
    NSMenu* sub = [main itemAtIndex:i].submenu;
    if (sub && [sub indexOfItemWithTarget:nil
                                andAction:@selector(selectAll:)] >= 0) {
      [main removeItemAtIndex:i];
    }
  }
  NSMenuItem* editItem = [[NSMenuItem alloc] init];
  NSMenu* edit = [[NSMenu alloc] initWithTitle:@"Edit"];
  editItem.submenu = edit;
  auto add = [&](NSString* title, SEL sel, NSString* key,
                 NSEventModifierFlags mods) {
    NSMenuItem* mi = [[NSMenuItem alloc] initWithTitle:title
                                                action:sel
                                         keyEquivalent:key];
    mi.keyEquivalentModifierMask = mods;
    mi.target = nil;  // dispatch through the responder chain
    [edit addItem:mi];
  };
  add(@"Undo", @selector(undo:), @"z", NSEventModifierFlagCommand);
  add(@"Redo", @selector(redo:), @"z",
      NSEventModifierFlagCommand | NSEventModifierFlagShift);
  [edit addItem:[NSMenuItem separatorItem]];
  add(@"Cut", @selector(cut:), @"x", NSEventModifierFlagCommand);
  add(@"Copy", @selector(copy:), @"c", NSEventModifierFlagCommand);
  add(@"Paste", @selector(paste:), @"v", NSEventModifierFlagCommand);
  add(@"Select All", @selector(selectAll:), @"a", NSEventModifierFlagCommand);
  [main insertItem:editItem atIndex:MIN((NSInteger)1, main.numberOfItems)];
}

bool ExecuteNativeEditCommand(std::string_view command_id) {
  SEL selector = nil;
  if (command_id == "edit.undo") {
    selector = @selector(undo:);
  } else if (command_id == "edit.redo") {
    selector = @selector(redo:);
  } else if (command_id == "edit.cut") {
    selector = @selector(cut:);
  } else if (command_id == "edit.copy") {
    selector = @selector(copy:);
  } else if (command_id == "edit.paste") {
    selector = @selector(paste:);
  } else if (command_id == "edit.selectAll") {
    selector = @selector(selectAll:);
  }
  return selector && [NSApp sendAction:selector to:nil from:nil];
}

// ---- E2E self-test (CMUX_E2E=1) ----
// Drives the REAL key path with no OS-level synthetic input (which macOS TCC
// drops for untrusted processes): NSEvents posted via [NSApp postEvent:]
// dispatch through [NSApp sendEvent:] -> keyWindow -> firstResponder -- the
// exact routing a hardware key uses. If the niri window is not the key window
// (the historical isKey=0 bug), these keyDowns are dropped and nothing
// navigates; if it is key, Cmd-L focuses the omnibox, the typed URL lands in
// it, and Return navigates the pane's WebContents. CDP
// (--remote-debugging-port) observes the navigation as the oracle.

namespace {
void PostKeyEvent(NSWindow* w,
                  NSString* chars,
                  unsigned short keyCode,
                  NSEventModifierFlags mods) {
  NSInteger wn = w.windowNumber;
  NSTimeInterval ts = [[NSProcessInfo processInfo] systemUptime];
  for (NSEventType type : {NSEventTypeKeyDown, NSEventTypeKeyUp}) {
    NSEvent* e = [NSEvent keyEventWithType:type
                                  location:NSZeroPoint
                             modifierFlags:mods
                                 timestamp:ts
                              windowNumber:wn
                                   context:nil
                                characters:chars
               charactersIgnoringModifiers:chars
                                 isARepeat:NO
                                   keyCode:keyCode];
    [NSApp postEvent:e atStart:NO];
  }
}

// Focus the active pane's omnibox (Cmd-L), insert `url` the way the IME hands
// text to the focused control, then press Return through the natural accept
// path. Invokes `done` after Return. `label` tags the logs per pane.
// `label`/`url` are taken BY VALUE: the dispatch blocks below run after this
// call returns, so a reference to a temporary argument would dangle.
void E2EFocusTypeEnter(views::Widget* widget,
                       CmuxStripController* strip,
                       NSWindow* nsw,
                       std::string label,
                       std::string url,
                       dispatch_block_t done) {
  PostKeyEvent(nsw, @"l", kVKL, NSEventModifierFlagCommand);  // Cmd-L
  dispatch_after(
      dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.5 * NSEC_PER_SEC)),
      dispatch_get_main_queue(), ^{
        views::FocusManager* fm = widget->GetFocusManager();
        views::View* fv = fm ? fm->GetFocusedView() : nullptr;
        std::string fvname =
            fv ? std::string(fv->GetClassName()) : std::string("null");
        ui::InputMethod* im = widget->GetInputMethod();
        ui::TextInputClient* tic = im ? im->GetTextInputClient() : nullptr;
        views::Textfield* tf = (fvname == "OmniboxViewViews")
                                   ? static_cast<views::Textfield*>(fv)
                                   : nullptr;
        LOG(WARNING) << "cmux-e2e[" << label
                     << "]: isKey=" << (int)[nsw isKeyWindow]
                     << " viewsFocused=" << fvname << " textInputType="
                     << (tic ? static_cast<int>(tic->GetTextInputType()) : -1)
                     << " omniboxReadOnly="
                     << (tf ? (int)tf->GetReadOnly() : -1);
        if (tic) {
          tic->InsertText(base::UTF8ToUTF16(url),
                          ui::TextInputClient::InsertTextCursorBehavior::
                              kMoveCursorAfterText);
        }
        // After autocomplete settles, press Return (a key event, not text, so
        // synthetic postEvent reaches it) -- natural timing avoids the
        // OpenMatch metrics DCHECK a same-tick programmatic accept trips.
        dispatch_after(
            dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.7 * NSEC_PER_SEC)),
            dispatch_get_main_queue(), ^{
              std::u16string typed = strip->E2EFocusedOmniboxText();
              LOG(WARNING) << "cmux-e2e[" << label << "]: omnibox='"
                           << base::UTF16ToUTF8(typed)
                           << "' -> Return (navigating to " << url << ")";
              PostKeyEvent(nsw, @"\r", kVKReturn, 0);
              if (done) {
                done();
              }
            });
      });
}
}  // namespace

void RunE2ESelfTest(CmuxStripController* strip, views::Widget* widget) {
  if (!widget) {
    return;
  }
  NSWindow* nsw = widget->GetNativeWindow().GetNativeNSWindow();
  LOG(WARNING) << "cmux-e2e: BEFORE focus -- isKey=" << (int)[nsw isKeyWindow]
               << " isMain=" << (int)[nsw isMainWindow]
               << " isVisible=" << (int)[nsw isVisible]
               << " appActive=" << (int)[NSApp isActive]
               << " policy=" << (long)[NSApp activationPolicy];

  // Pane 1: type a URL into the startup Chrome pane's omnibox and navigate.
  E2EFocusTypeEnter(widget, strip, nsw, "pane1", "example.com", ^{
    // Multi-page: open a new Chrome column (Cmd-N), then drive ITS omnibox.
    dispatch_after(
        dispatch_time(DISPATCH_TIME_NOW, (int64_t)(1.6 * NSEC_PER_SEC)),
        dispatch_get_main_queue(), ^{
          PostKeyEvent(nsw, @"n", kVKN, NSEventModifierFlagCommand);  // Cmd-N
          dispatch_after(
              dispatch_time(DISPATCH_TIME_NOW, (int64_t)(1.6 * NSEC_PER_SEC)),
              dispatch_get_main_queue(), ^{
                E2EFocusTypeEnter(widget, strip, nsw, "pane2", "example.org", ^{
                  LOG(WARNING)
                      << "cmux-e2e: DONE multi-page (pane1 example.com "
                         "+ pane2 example.org)";
                });
              });
        });
  });
}

}  // namespace cmux
