// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef CHROME_BROWSER_CMUX_TERM_CMUX_COMPOSITOR_TEST_PANE_H_
#define CHROME_BROWSER_CMUX_TERM_CMUX_COMPOSITOR_TEST_PANE_H_

#include <cstdint>
#include <memory>
#include <vector>

#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/sequence_checker.h"
#include "chrome/browser/cmux_term/cmux_surface.h"
#include "gpu/command_buffer/common/sync_token.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/gfx/geometry/size.h"
#include "ui/views/view.h"

namespace gpu {
class ClientSharedImage;
}
namespace ui {
class Layer;
}

namespace cmux {

// Composites a CPU-filled macOS IOSurface into a ui::Layer inside a
// views::View. This isolates the Chromium SharedImage compositing path from the
// production terminal, which uses Ghostty's supported native NSView/Metal
// surface. The previous terminal mode depended on an unpublished offscreen ABI.
class CmuxCompositorTestPane : public views::View, public CmuxSurface {
  METADATA_HEADER(CmuxCompositorTestPane, views::View)

 public:
  CmuxCompositorTestPane();
  CmuxCompositorTestPane(const CmuxCompositorTestPane&) = delete;
  CmuxCompositorTestPane& operator=(const CmuxCompositorTestPane&) = delete;
  ~CmuxCompositorTestPane() override;

  // views::View:
  void AddedToWidget() override;
  void OnBoundsChanged(const gfx::Rect& previous_bounds) override;

  // CmuxSurface:
  views::View* AsView() override;
  SurfaceKind kind() const override;
  void FocusContent() override;

 private:
  // Sets up content once a compositor is available. Safe to call repeatedly.
  void EnsureContent();
  void EnsureGradientTexture();

  // UI thread: wrap an owned IOSurface reference into a SharedImage and display
  // it in texture_layer_.
  void CompositeIOSurface(void* io_surface_ref);
  void OnTransferableResourceReleased(gpu::ClientSharedImage* shared_image,
                                      const gpu::SyncToken& sync_token,
                                      bool is_lost);

  struct CachedIOSurfaceSharedImage {
    CachedIOSurfaceSharedImage();
    CachedIOSurfaceSharedImage(const CachedIOSurfaceSharedImage&);
    CachedIOSurfaceSharedImage& operator=(const CachedIOSurfaceSharedImage&);
    ~CachedIOSurfaceSharedImage();

    // Identity key only (never dereferenced): the IOSurfaceRef address as an
    // integer. uintptr_t (not a pointer) keeps the rawptr plugin satisfied and
    // makes the may-dangle semantics explicit; io_surface_id guards reuse of a
    // freed-then-reallocated address.
    uintptr_t io_surface_key = 0;
    uint32_t io_surface_id = 0;
    scoped_refptr<gpu::ClientSharedImage> shared_image;
    gpu::SyncToken release_sync_token;
    int outstanding_releases = 0;
    uint64_t last_used = 0;
  };

  CachedIOSurfaceSharedImage* FindCachedSharedImage(uintptr_t io_surface_key,
                                                    uint32_t io_surface_id);
  void ClearIOSurfaceCache();
  void TrimIOSurfaceCache();

  std::unique_ptr<ui::Layer> texture_layer_;
  std::vector<CachedIOSurfaceSharedImage> iosurface_cache_;
  gfx::Size iosurface_cache_size_;
  uint64_t iosurface_cache_use_counter_ = 0;
  bool created_ = false;

  SEQUENCE_CHECKER(sequence_checker_);
  base::WeakPtrFactory<CmuxCompositorTestPane> weak_factory_{this};
};

}  // namespace cmux

#endif  // CHROME_BROWSER_CMUX_TERM_CMUX_COMPOSITOR_TEST_PANE_H_
