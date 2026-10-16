// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef CHROME_BROWSER_CMUX_TERM_CMUX_PANE_H_
#define CHROME_BROWSER_CMUX_TERM_CMUX_PANE_H_

#include <string>

#include "base/functional/callback.h"
#include "third_party/skia/include/core/SkColor.h"

namespace views {
class View;
}
namespace content {
class WebContents;
}

namespace cmux {

// Shared strip/pane chrome colors.
inline constexpr SkColor kDefaultPaneFocusedBorder =
    SkColorSetRGB(0x8a, 0x8f, 0x98);
inline constexpr SkColor kDefaultPaneIdleBorder =
    SkColorSetRGB(0x2a, 0x2d, 0x34);
inline constexpr SkColor kDefaultPaneStripBackground =
    SkColorSetRGB(0x0e, 0x0f, 0x12);

struct PaneThemeColors {
  SkColor strip_background = kDefaultPaneStripBackground;
  SkColor focus_border = kDefaultPaneFocusedBorder;
  SkColor idle_border = kDefaultPaneIdleBorder;
};

// The strip's view of one pane (a CmuxPaneView: tab strip + surfaces). The
// per-content operations route to the pane's selected surface (CmuxSurface).
// Kept as an interface so the strip and the FocusController stay decoupled
// from the concrete view. The optional hooks have no-op defaults defined
// out-of-line in cmux_pane.cc (chromium-style: virtual methods with bodies
// must not be inline in a header).
class CmuxPane {
 public:
  virtual ~CmuxPane();

  // Required of every pane.
  virtual views::View* AsView() = 0;
  virtual void SetFocusedBorder(bool focused) = 0;
  virtual void FocusContent() = 0;

  virtual void FocusOmnibar();
  // Set a callback the pane runs when its content is directly activated by the
  // user (e.g. its terminal NSView becomes first responder on a click). Lets
  // the strip focus the column even when the click is handled natively and
  // never reaches the Views FocusManager. Default: ignored (panes whose focus
  // already routes through the FocusManager don't need it).
  virtual void SetActivationCallback(base::RepeatingClosure callback);
  // E2E hook (CMUX_E2E): read the selected surface's omnibox text.
  virtual std::u16string E2EOmniboxText();
  // Close any of this pane's omnibox autocomplete popups unless `focused` is
  // inside the owning location bar. Mirrors upstream's "blur closes the popup"
  // behavior across several pane-local LocationBars in one physical widget.
  virtual void CloseOmniboxPopupUnlessFocused(views::View* focused);
  // The WebContents DevTools should inspect for this pane, or null if the
  // selected surface isn't inspectable (terminals).
  virtual content::WebContents* GetInspectableWebContents();
  // Toggle a DevTools inspector docked inside the selected surface (no-op if
  // not inspectable). Opening again closes it.
  virtual void ToggleDevTools();
  // Move the selected surface's DevTools into its own top-level window
  // (opening it first if needed). No-op if not inspectable.
  virtual void UndockDevTools();
  // Handle Escape while this pane's omnibox is focused: revert the edit and
  // return focus to the pane content. Returns true if it handled it (i.e. the
  // omnibox was focused); false lets Escape fall through to the web content.
  virtual bool HandleOmniboxEscape();
  // Navigate the selected surface's session history. No-ops for terminals.
  virtual void GoBack();
  virtual void GoForward();
  virtual void Reload();
};

}  // namespace cmux

#endif  // CHROME_BROWSER_CMUX_TERM_CMUX_PANE_H_
