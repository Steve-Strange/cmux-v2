// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef CHROME_BROWSER_CMUX_TERM_CMUX_GHOSTTY_OPENGL_HOST_H_
#define CHROME_BROWSER_CMUX_TERM_CMUX_GHOSTTY_OPENGL_HOST_H_

#include <cstdint>
#include <memory>
#include <vector>

#include "base/functional/callback.h"
#include "build/build_config.h"
#include "third_party/cmux_ghostty/include/ghostty.h"

namespace cmux {

// An immutable RGBA8 frame copied from Ghostty's host-owned OpenGL drawable.
// Rows retain OpenGL's bottom-up orientation; the Views frontend flips and
// converts them while installing the frame in its SkBitmap.
struct CmuxGhosttyOpenGLFrame {
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t stride = 0;
  std::vector<uint8_t> pixels;
};

// Owns the native OpenGL context and offscreen drawable required by manaflow
// Ghostty's published GHOSTTY_PLATFORM_OPENGL embedding ABI. Ghostty owns the
// renderer thread; swap_buffers copies a frame before posting it to Chromium.
// The Ghostty surface must be freed before this host is destroyed.
class CmuxGhosttyOpenGLHost {
 public:
  using FrameCallback =
      base::RepeatingCallback<void(CmuxGhosttyOpenGLFrame)>;

  static std::unique_ptr<CmuxGhosttyOpenGLHost> Create(
      uint32_t width,
      uint32_t height,
      FrameCallback frame_callback);

  CmuxGhosttyOpenGLHost(const CmuxGhosttyOpenGLHost&) = delete;
  CmuxGhosttyOpenGLHost& operator=(const CmuxGhosttyOpenGLHost&) = delete;
  virtual ~CmuxGhosttyOpenGLHost();

  void PopulateSurfaceConfig(ghostty_surface_config_s* config);
  virtual void SetSize(uint32_t width, uint32_t height) = 0;

 protected:
  explicit CmuxGhosttyOpenGLHost(FrameCallback frame_callback);

  void EmitFrame(uint32_t width,
                 uint32_t height,
                 void* pixel_store_i,
                 void* read_pixels);

 private:
  static bool MakeCurrentThunk(void* userdata);
  static void ClearCurrentThunk(void* userdata);
  static void* GetProcAddressThunk(void* userdata, const char* name);
  static void SwapBuffersThunk(void* userdata);

  virtual bool MakeCurrent() = 0;
  virtual void ClearCurrent() = 0;
  virtual void* GetProcAddress(const char* name) = 0;
  virtual void SwapBuffers() = 0;

  FrameCallback frame_callback_;
};

}  // namespace cmux

#endif  // CHROME_BROWSER_CMUX_TERM_CMUX_GHOSTTY_OPENGL_HOST_H_
