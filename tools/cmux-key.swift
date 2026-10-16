// cmux-key — send REAL key events via CGEventPost (HID-level), so they go
// through the exact same path as a physical keyboard (key-equivalent handling,
// NSEvent monitors, first-responder keyDown:). This is what makes the E2E tests
// representative; in-app [NSApp postEvent:] is NOT (it skips key-equivalent and
// menu processing, giving false positives).
//
// Requires the running process to have Accessibility (TCC) permission, else the
// events are silently dropped. Run `cmux-key --check` to verify trust.
//
// Build:  swiftc -O tools/cmux-key.swift -o tools/cmux-key
// Usage:
//   cmux-key --check                 # prints AXIsProcessTrusted
//   cmux-key type "hello world"      # type a literal string
//   cmux-key chord cmd a             # Cmd-A
//   cmux-key chord ctrl c            # Ctrl-C
//   cmux-key key escape              # a single named key (no modifiers)
//   cmux-key key return
// Keys may be chained: cmux-key key escape  ; multiple invocations are fine.

import Cocoa
import ApplicationServices

// Virtual keycodes (ANSI). Only what the tests need.
let keymap: [String: CGKeyCode] = [
  "a": 0, "s": 1, "d": 2, "f": 3, "h": 4, "g": 5, "z": 6, "x": 7, "c": 8,
  "v": 9, "b": 11, "q": 12, "w": 13, "e": 14, "r": 15, "y": 16, "t": 17,
  "1": 18, "2": 19, "3": 20, "4": 21, "6": 22, "5": 23, "9": 25, "7": 26,
  "8": 28, "0": 29, "o": 31, "u": 32, "i": 34, "p": 35, "l": 37, "j": 38,
  "k": 40, "n": 45, "m": 46,
  "return": 36, "tab": 48, "space": 49, "delete": 51, "escape": 53,
  "left": 123, "right": 124, "down": 125, "up": 126,
]

func flags(for mods: [String]) -> CGEventFlags {
  var f = CGEventFlags()
  for m in mods {
    switch m {
    case "cmd", "command": f.insert(.maskCommand)
    case "ctrl", "control": f.insert(.maskControl)
    case "opt", "option", "alt": f.insert(.maskAlternate)
    case "shift": f.insert(.maskShift)
    default: FileHandle.standardError.write("unknown modifier: \(m)\n".data(using: .utf8)!)
    }
  }
  return f
}

func tap(_ keycode: CGKeyCode, _ f: CGEventFlags) {
  let src = CGEventSource(stateID: .hidSystemState)
  let down = CGEvent(keyboardEventSource: src, virtualKey: keycode, keyDown: true)!
  let up = CGEvent(keyboardEventSource: src, virtualKey: keycode, keyDown: false)!
  down.flags = f
  up.flags = f
  down.post(tap: .cghidEventTap)
  usleep(8000)
  up.post(tap: .cghidEventTap)
  usleep(8000)
}

let args = Array(CommandLine.arguments.dropFirst())
guard let cmd = args.first else {
  FileHandle.standardError.write("usage: cmux-key <check|type|chord|key> ...\n".data(using: .utf8)!)
  exit(2)
}

switch cmd {
case "--check", "check":
  let trusted = AXIsProcessTrusted()
  print("AXIsProcessTrusted=\(trusted)")
  exit(trusted ? 0 : 1)

case "activate":
  // activate <pid> — bring the cmux app frontmost so subsequent CGEvents route
  // to it. Try NSRunningApplication by pid, else search runningApplications by
  // pid / bundle path (the lldb-launched process may not resolve by pid).
  let pidStr = args.dropFirst().first ?? ""
  let pid = pid_t(pidStr) ?? -1
  var app = NSRunningApplication(processIdentifier: pid)
  if app == nil {
    app = NSWorkspace.shared.runningApplications.first { ra in
      ra.processIdentifier == pid ||
        (ra.bundleURL?.path.contains("cmux-browser.app") ?? false) ||
        (ra.localizedName == "cmux") || (ra.localizedName == "Chromium")
    }
  }
  guard let a = app else {
    let names = NSWorkspace.shared.runningApplications.compactMap { $0.localizedName }.joined(separator: ",")
    FileHandle.standardError.write("activate: app not found (pid \(pid)). running: \(names)\n".data(using: .utf8)!)
    exit(0)  // best-effort; don't fail the harness
  }
  a.activate(options: [.activateIgnoringOtherApps])
  usleep(250000)
  print("activated \(a.localizedName ?? "?") pid \(a.processIdentifier) frontmost=\(a.isActive)")

case "click":
  // click <x> <y> — real left click at global (top-left origin) screen coords,
  // to set native first-responder focus the way a user click does.
  guard args.count >= 3, let x = Double(args[1]), let y = Double(args[2]) else {
    FileHandle.standardError.write("usage: click <x> <y>\n".data(using: .utf8)!)
    exit(2)
  }
  let pt = CGPoint(x: x, y: y)
  let src = CGEventSource(stateID: .hidSystemState)
  CGEvent(mouseEventSource: src, mouseType: .mouseMoved, mouseCursorPosition: pt, mouseButton: .left)?.post(tap: .cghidEventTap)
  usleep(20000)
  CGEvent(mouseEventSource: src, mouseType: .leftMouseDown, mouseCursorPosition: pt, mouseButton: .left)?.post(tap: .cghidEventTap)
  usleep(30000)
  CGEvent(mouseEventSource: src, mouseType: .leftMouseUp, mouseCursorPosition: pt, mouseButton: .left)?.post(tap: .cghidEventTap)
  usleep(30000)

case "type":
  // Type a literal string by synthesizing per-character unicode key events.
  let s = args.dropFirst().joined(separator: " ")
  let src = CGEventSource(stateID: .hidSystemState)
  for ch in s.utf16 {
    let down = CGEvent(keyboardEventSource: src, virtualKey: 0, keyDown: true)!
    let up = CGEvent(keyboardEventSource: src, virtualKey: 0, keyDown: false)!
    var u = [ch]
    down.keyboardSetUnicodeString(stringLength: 1, unicodeString: &u)
    up.keyboardSetUnicodeString(stringLength: 1, unicodeString: &u)
    down.post(tap: .cghidEventTap)
    usleep(6000)
    up.post(tap: .cghidEventTap)
    usleep(6000)
  }

case "chord":
  // chord <mod...> <key>   e.g.  chord cmd a   |   chord ctrl shift c
  guard args.count >= 3 else {
    FileHandle.standardError.write("usage: chord <mod...> <key>\n".data(using: .utf8)!)
    exit(2)
  }
  let key = args.last!.lowercased()
  let mods = Array(args.dropFirst().dropLast()).map { $0.lowercased() }
  guard let kc = keymap[key] else {
    FileHandle.standardError.write("unknown key: \(key)\n".data(using: .utf8)!)
    exit(2)
  }
  tap(kc, flags(for: mods))

case "key":
  guard let name = args.dropFirst().first?.lowercased(), let kc = keymap[name] else {
    FileHandle.standardError.write("unknown key\n".data(using: .utf8)!)
    exit(2)
  }
  tap(kc, CGEventFlags())

default:
  FileHandle.standardError.write("unknown command: \(cmd)\n".data(using: .utf8)!)
  exit(2)
}
