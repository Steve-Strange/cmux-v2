# Alternate path: fork //chrome, add our own tab UI, embed Ghostty

Proposed 2026-06-10 after OWL-shell dogfood showed every rough edge (resize blur, mouse/keyboard, omnibar, visible host window) living in the readback/presentation seam. This doc compares the two paths and stages the fork path.

## The two architectures

**OWL (built, working):** Chromium runs as a separate host process; a native Swift/AppKit app reads web-content pixels back over Mach IPC (IOSurface) and re-hosts them, cracking input client-side. Verified 13/13 on the builder.
- Pros: native Swift UI; process isolation; tiny, rebase-friendly Chromium diff (the OWL premise); instant app startup.
- Cons: the readback/input/presentation seam is bug-dense and hard to make pixel-perfect (resize ordering, DPI, IME, hidden-window compositing, cursor). Every UX complaint so far is in this seam.

**Fork (proposed):** fork the full `//chrome` browser, modify its own Views UI for our tab strip / omnibar, and add Ghostty as a first-class terminal surface. Web contents render in-window like real Chrome.
- Pros: resize/mouse/keyboard/IME/omnibar are Chrome-quality for free (it IS Chrome); no readback seam; full Web Store extensions inherent; the terminal lives in the same window/compositor as the web tabs.
- Cons: we maintain a browser-UI fork (larger diff than OWL, heavier upstream rebases); UI work is in Chromium Views (C++/Cocoa), not SwiftUI; builds are full-chrome (minutes warm on the m1ultra, hours cold). This is exactly what OWL was designed to avoid, traded for UX quality.

Both reuse the same fork base (`~/chromium/src`, manaflow fork) and the same builder. They are not mutually exclusive: OWL can remain a thin-client/remote story; the fork is the local high-fidelity app.

## Staged plan (fork path)

- **F0 ✅ baseline** — build the full `chrome` target from our fork and run it visibly. Already have `Chromium.app`; launched directly (no `OWL_BOOTSTRAP_NAME`) it is the real Chrome browser with perfect tab/omnibar/resize/input. This is the UX bar.
- **F1 — brand + minimal UI fork** — prove the UI-fork workflow with a contained, visible change (product name → cmux, window/tab styling hook). Establishes where our tab-strip/omnibar customizations live (`chrome/browser/ui/views/frame`, `tabs`, `location_bar`).
- **F2 — Ghostty terminal surface (the novel work)** — embed libghostty as a terminal rendered into the Chromium window. Reuse the proven OSR path: Ghostty renders offscreen into an IOSurface (proven in `~/fun/ghostty-osr-spike`), hosted in a Views `NativeViewHost` wrapping an NSView whose layer shows that IOSurface; input forwarded from the Views event path into libghostty. Stage it as:
  - F2a: Ghostty in a standalone Views window/panel inside the fork (proves embed + input + resize with no tab-model surgery).
  - F2b: promote to a real tab type alongside WebContents tabs (TabStripModel integration — the hard part; a terminal tab is not a WebContents, so it needs a parallel contents path in `BrowserView`).
- **F3 — cmux integration** — agent/terminal workflows, splits, the cmux feature set, on top.

## Verification

The readback `owl-verify` harness does not apply to the fork (no IPC surface). Fork verification is: the existing Chromium test suites for unchanged behavior, plus targeted UI tests for our Views changes, plus device dogfood (the fork renders to a real window, so live behavior is the same thing the user sees). Ghostty-surface correctness gets a focused test: drive input, assert the libghostty grid state / an IOSurface pixel sample.

## Decision needed

Whether the fork **replaces** OWL as the product direction or **runs alongside** it (OWL = remote/thin-client lane, fork = local app), and acceptance of the fork-maintenance cost. Until decided, OWL stays intact on `main`; fork work goes on an experiment branch.
