// Copyright 2026 Manaflow, Inc.
// Portions derived from Helium; Helium contributors retain copyright.
// SPDX-License-Identifier: GPL-3.0-or-later AND GPL-3.0-only

#ifndef CHROME_BROWSER_CMUX_TERM_CMUX_SURFACE_H_
#define CHROME_BROWSER_CMUX_TERM_CMUX_SURFACE_H_

#include <string>

#include "base/functional/callback.h"
#include "chrome/browser/cmux_term/window_model.h"

namespace gfx {
class ImageSkia;
}
namespace views {
class View;
}
namespace content {
class WebContents;
}
namespace web_modal {
class WebContentsModalDialogHost;
}
class LocationBar;

namespace cmux {

// Match Helium's ordinary-browser content frame exactly: three DIPs of
// toolbar surface around an eight-DIP corner with a one-DIP separator outline.
// Helium sources: rounded-frame-corners.patch, multi-contents-view.patch, and
// frame-radius-helper.patch (see THIRD_PARTY_NOTICES.md).
inline constexpr int kRoundedFrameInset = 3;
inline constexpr int kRoundedFrameCornerRadius = 8;
inline constexpr int kRoundedFrameOutlineThickness = 1;

// Fully resolved frame geometry for one pane. The window controller owns edge
// contact because it alone knows whether a pane edge meets the native window,
// another pane, or attached chrome such as the workspace rail. Surfaces only
// translate this geometry into their platform clip and outline.
struct RoundedFrameGeometry {
  bool enabled = false;
  int top_inset = 0;
  int left_inset = kRoundedFrameInset;
  int bottom_inset = kRoundedFrameInset;
  int right_inset = kRoundedFrameInset;
  int top_left_radius = kRoundedFrameCornerRadius;
  int top_right_radius = kRoundedFrameCornerRadius;
  int bottom_right_radius = kRoundedFrameCornerRadius;
  int bottom_left_radius = kRoundedFrameCornerRadius;

  bool operator==(const RoundedFrameGeometry& other) const {
    return enabled == other.enabled && top_inset == other.top_inset &&
           left_inset == other.left_inset &&
           bottom_inset == other.bottom_inset &&
           right_inset == other.right_inset &&
           top_left_radius == other.top_left_radius &&
           top_right_radius == other.top_right_radius &&
           bottom_right_radius == other.bottom_right_radius &&
           bottom_left_radius == other.bottom_left_radius;
  }
};

// One tab's content inside a pane: a web page (omnibox + WebView) or a
// terminal. A pane (CmuxPaneView) holds one surface per SurfaceTab and shows
// the selected one. Implementations are views::Views owned by the pane's
// surface container; this interface is how the pane and the window controller
// drive them without knowing the concrete type. The optional hooks have no-op
// defaults defined out-of-line in cmux_surface.cc (chromium-style: virtual
// methods with bodies must not be inline in a header).
class CmuxSurface {
 public:
  virtual ~CmuxSurface();

  // Required of every surface.
  virtual views::View* AsView() = 0;
  virtual SurfaceKind kind() const = 0;
  // Give the surface's content the keyboard (Views focus + native first
  // responder where applicable).
  virtual void FocusContent() = 0;
  // Make this surface's pane-local toolbar the BrowserWindow feature host for
  // ephemeral/pinned action bubbles. Default: ignored by non-web surfaces.
  virtual void ActivateToolbarHost();

  // Run when the surface's content is directly activated by the user (native
  // first-responder change, web-contents focus, omnibox focus) so the strip
  // can focus + scroll its column even when the click never reaches the Views
  // FocusManager. Default: ignored.
  virtual void SetActivationCallback(base::RepeatingClosure callback);
  // Run for direct user input delivered by a native hosted surface. This is
  // separate from activation because programmatic FocusContent also changes
  // first responder and must not be mistaken for a user interaction.
  // Default: ignored (Views-backed surfaces are covered by pre-target input).
  virtual void SetInteractionCallback(base::RepeatingClosure callback);
  // Run with the surface's display title whenever it changes (page title for
  // web surfaces). Default: ignored (terminals keep their static title).
  virtual void SetTitleChangedCallback(
      base::RepeatingCallback<void(const std::u16string&)> callback);
  // Run when a web surface has a 16 DIP favicon image update. Empty images mean
  // the strip should fall back to its placeholder. Default: ignored.
  virtual void SetFaviconChangedCallback(
      base::RepeatingCallback<void(const gfx::ImageSkia&)> callback);
  // Run when a web surface's loading state changes. Default: ignored.
  virtual void SetLoadingChangedCallback(
      base::RepeatingCallback<void(bool)> callback);
  // Run when the embedded surface asks the host to close it (Ghostty's
  // close_surface callback, or window.close() for web contents). Default:
  // ignored.
  virtual void SetCloseRequestedCallback(base::RepeatingClosure callback);
  // E2E/self-test hook: fire the currently bound close-request callback without
  // terminating a real shell or page.
  virtual void FireCloseRequestedForTesting();
  // E2E/self-test hook: open Chromium's page-info bubble through the real
  // LocationBarView delegate path. Returns false for non-web surfaces or when
  // the current navigation cannot show page info.
  virtual bool ShowPageInfoForTesting();
  // Wrap the actual terminal/page surface in Helium's rounded content frame.
  // The pane's tab strip and a web surface's toolbar remain outside it. Edge
  // insets and corner radii are resolved by the window controller so attached
  // chrome and native-window corners match Helium's distinct treatments.
  virtual void SetRoundedFrame(const RoundedFrameGeometry& geometry);

  // Web-surface operations; no-ops (or empty/false returns) elsewhere.
  virtual void FocusOmnibar();
  virtual void GoBack();
  virtual void GoForward();
  virtual void Reload();
  // Handle Escape while this surface's omnibox is focused: revert the edit and
  // return focus to the content. Returns true if consumed.
  virtual bool HandleOmniboxEscape();
  // Close this surface's omnibox autocomplete popup unless `focused` is inside
  // its location bar (upstream's blur-closes-popup behavior).
  virtual void CloseOmniboxPopupUnlessFocused(views::View* focused);
  // Toggle a DevTools inspector docked inside this surface.
  virtual void ToggleDevTools();
  // Move this surface's DevTools inspector into its own top-level window
  // (opening it first if needed). Closing that window re-docks it closed.
  virtual void UndockDevTools();
  // The WebContents DevTools should inspect, or null if not inspectable.
  virtual content::WebContents* GetInspectableWebContents();
  // The real LocationBar backing a Browser-owned web surface, or null for
  // surfaces (such as terminals) that do not have one.
  virtual LocationBar* GetLocationBar();
  // The pane-aware host for tab-modal security UI (WebAuthn, file-system
  // confirmations, collected site data, and similar constrained dialogs).
  // Non-web surfaces return null.
  virtual web_modal::WebContentsModalDialogHost*
  GetWebContentsModalDialogHost();
  // E2E hooks (CMUX_E2E): read the omnibox text.
  virtual std::u16string E2EOmniboxText();
};

}  // namespace cmux

#endif  // CHROME_BROWSER_CMUX_TERM_CMUX_SURFACE_H_
