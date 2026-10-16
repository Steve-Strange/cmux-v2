// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "chrome/browser/cmux_term/cmux_native_frame_material.h"

#include "build/build_config.h"

#if !BUILDFLAG(IS_MAC)

namespace cmux {

bool NativeFrameMaterialSupported() {
  return false;
}

void UpdateNativeFrameMaterial(views::Widget*,
                               const NativeFrameMaterialParams&) {}

void DisableNativeFrameMaterial(views::Widget*) {}

}  // namespace cmux

#endif  // !BUILDFLAG(IS_MAC)
