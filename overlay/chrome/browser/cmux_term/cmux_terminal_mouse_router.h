// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef CHROME_BROWSER_CMUX_TERM_CMUX_TERMINAL_MOUSE_ROUTER_H_
#define CHROME_BROWSER_CMUX_TERM_CMUX_TERMINAL_MOUSE_ROUTER_H_

#include <cstdint>

namespace cmux {

// The renderer IOSurface is pixel-sized while AppKit reports pointer positions
// in points. This is deliberately based on the frame actually displayed, not
// the view's current (possibly newer) bounds during a live resize.
struct TerminalMouseBounds {
  uint32_t width_px = 0;
  uint32_t height_px = 0;
  float scale = 0;

  bool Contains(double x_points, double y_points) const {
    return width_px != 0 && height_px != 0 && scale > 0 && x_points >= 0 &&
           y_points >= 0 && x_points * scale < width_px &&
           y_points * scale < height_px;
  }
};

// Button ownership is established only by a press inside the displayed frame.
// Once established it survives motion outside that frame so the matching
// release can always reach the terminal application.
class TerminalMouseCapture {
 public:
  bool Begin(uint32_t button, bool inside_displayed_frame) {
    const uint64_t mask = ButtonMask(button);
    if (!inside_displayed_frame || mask == 0) {
      return false;
    }
    captured_buttons_ |= mask;
    return true;
  }

  bool End(uint32_t button) {
    const uint64_t mask = ButtonMask(button);
    if (mask == 0 || (captured_buttons_ & mask) == 0) {
      return false;
    }
    captured_buttons_ &= ~mask;
    return true;
  }

  bool HasCapture() const { return captured_buttons_ != 0; }

  bool HasCapture(uint32_t button) const {
    const uint64_t mask = ButtonMask(button);
    return mask != 0 && (captured_buttons_ & mask) != 0;
  }

  bool ShouldForwardDrag() const { return HasCapture(); }

  bool PreserveActualMotion(bool inside_displayed_frame) const {
    return inside_displayed_frame || HasCapture();
  }

  void Reset() { captured_buttons_ = 0; }

 private:
  static uint64_t ButtonMask(uint32_t button) {
    return button < 64 ? uint64_t{1} << button : 0;
  }

  uint64_t captured_buttons_ = 0;
};

}  // namespace cmux

#endif  // CHROME_BROWSER_CMUX_TERM_CMUX_TERMINAL_MOUSE_ROUTER_H_
