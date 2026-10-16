# cmux AX native-window lab

This standalone AppKit harness tests whether Sky/Computer Use can discover and
operate controls hosted by native windows that are not visible to the user.
It intentionally does not depend on the Chromium build.

Build and launch:

```sh
./tools/ax-window-lab/build.sh
open -n "tools/ax-window-lab/build/cmux AX Window Lab.app"
```

Successful actions are shown in the visible shell and appended to
`/tmp/cmux-ax-window-lab.log`.

## Results (macOS, 2026-07-16)

| Presentation | Fresh tree in Sky | Background AX action | Notes |
| --- | --- | --- | --- |
| Opaque, fully occluded | Only while focused | Yes, with a retained element reference | Sky changed the text field and pressed the button after the shell re-occluded the window. |
| `alphaValue = 0` | Yes, while key | Yes | Sky changed the text field and pressed the button while the window had no visible pixels. |
| Ordered offscreen | Yes, while key | Yes | Sky changed the text field and pressed the button while the window was beyond every display. |
| Miniaturized | Not while left minimized | Not established | Cycling to it deminiaturizes/focuses it. |
| `orderOut` | No | Not through a fresh Sky tree | It is absent from normal window discovery/cycling. |

Observed proof strings:

```text
OPERATED occluded input=sky-background-occluded
OPERATED transparent input=sky-fresh-transparent
OPERATED offscreen input=sky-fresh-offscreen
```

## Interpretation for cmux

The AppKit/Accessibility layer is not the blocker. An ordered native window can
remain actionable even when it contributes no visible pixels. The current Sky
API is the constraint: `get_app_state(app:)` returns the focused native
window's tree, rather than every window under the application's `AXWindows`.

A cmux prototype therefore needs both:

1. one ordered `NSWindow` per workspace (transparent or parked offscreen when
   inactive; do not use `orderOut`), and
2. a way for Sky to address a particular window, either by adding a window
   selector to Computer Use or through a cmux accessibility broker that selects
   the target without changing visible pixels.

Transparent/offscreen windows still participate in AppKit window management.
A production experiment must suppress unwanted Cmd-` cycling, Mission Control,
Window-menu, Spaces, and activation side effects before treating the technique
as user-invisible.
