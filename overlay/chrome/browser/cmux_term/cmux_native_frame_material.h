// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef CHROME_BROWSER_CMUX_TERM_CMUX_NATIVE_FRAME_MATERIAL_H_
#define CHROME_BROWSER_CMUX_TERM_CMUX_NATIVE_FRAME_MATERIAL_H_

#include "third_party/skia/include/core/SkColor.h"

namespace views {
class Widget;
}  // namespace views

namespace cmux {

struct NativeFrameMaterialParams {
  bool is_dark = false;
  SkColor tint = SK_ColorTRANSPARENT;
};

// Whether the current platform can place a native translucent material behind
// a cmux window. This is deliberately false outside macOS.
bool NativeFrameMaterialSupported();

// Installs or refreshes the native material behind `widget`'s content. The
// native view never participates in hit testing. Calling this repeatedly is
// supported, including when the theme changes.
void UpdateNativeFrameMaterial(views::Widget* widget,
                               const NativeFrameMaterialParams& params);

// Removes the native material and restores the window properties captured
// before the first UpdateNativeFrameMaterial() call.
void DisableNativeFrameMaterial(views::Widget* widget);

}  // namespace cmux

#endif  // CHROME_BROWSER_CMUX_TERM_CMUX_NATIVE_FRAME_MATERIAL_H_
