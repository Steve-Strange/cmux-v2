// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "chrome/browser/cmux_term/cmux_compositor_test_pane.h"

#import <IOSurface/IOSurfaceRef.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>

#include "base/compiler_specific.h"
#include "base/containers/span.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "chrome/browser/cmux_term/cmux_surface.h"
#include "components/viz/common/gpu/raster_context_provider.h"
#include "components/viz/common/resources/release_callback.h"
#include "components/viz/common/resources/shared_image_format.h"
#include "components/viz/common/resources/transferable_resource.h"
#include "content/public/browser/context_factory.h"
#include "gpu/command_buffer/client/client_shared_image.h"
#include "gpu/command_buffer/client/shared_image_interface.h"
#include "gpu/command_buffer/common/shared_image_usage.h"
#include "third_party/skia/include/core/SkColor.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/compositor/compositor.h"
#include "ui/compositor/layer.h"
#include "ui/gfx/color_space.h"
#include "ui/gfx/geometry/size.h"
#include "ui/gfx/gpu_memory_buffer_handle.h"
#include "ui/gfx/mac/io_surface.h"
#include "ui/views/background.h"

namespace cmux {

namespace {
constexpr int kGradW = 800;
constexpr int kGradH = 600;
constexpr size_t kMaxIOSurfaceSharedImageCacheSize = 4;
}  // namespace

CmuxCompositorTestPane::CachedIOSurfaceSharedImage::
    CachedIOSurfaceSharedImage() = default;
CmuxCompositorTestPane::CachedIOSurfaceSharedImage::CachedIOSurfaceSharedImage(
    const CachedIOSurfaceSharedImage&) = default;
CmuxCompositorTestPane::CachedIOSurfaceSharedImage&
CmuxCompositorTestPane::CachedIOSurfaceSharedImage::operator=(
    const CachedIOSurfaceSharedImage&) = default;
CmuxCompositorTestPane::CachedIOSurfaceSharedImage::
    ~CachedIOSurfaceSharedImage() = default;

CmuxCompositorTestPane::CmuxCompositorTestPane() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  SetPaintToLayer();
  SetBackground(views::CreateSolidBackground(SkColorSetRGB(0x10, 0x10, 0x10)));
  texture_layer_ = std::make_unique<ui::Layer>(ui::LAYER_TEXTURED);
  texture_layer_->SetName("CmuxCompositorTexture");
  layer()->Add(texture_layer_.get());
}

CmuxCompositorTestPane::~CmuxCompositorTestPane() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  ClearIOSurfaceCache();
}

void CmuxCompositorTestPane::AddedToWidget() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  EnsureContent();
}

void CmuxCompositorTestPane::OnBoundsChanged(const gfx::Rect& previous_bounds) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (texture_layer_) {
    texture_layer_->SetBounds(GetLocalBounds());
  }
  EnsureContent();
}

void CmuxCompositorTestPane::EnsureContent() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (created_) {
    return;
  }
  EnsureGradientTexture();
}

void CmuxCompositorTestPane::CompositeIOSurface(void* io_surface_ref) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  // UI thread. Takes ownership of the gradient path's released IOSurface ref.
  gfx::ScopedIOSurface io(static_cast<IOSurfaceRef>(io_surface_ref));
  if (!io || !texture_layer_) {
    return;
  }
  const gfx::Size size(static_cast<int>(IOSurfaceGetWidth(io.get())),
                       static_cast<int>(IOSurfaceGetHeight(io.get())));
  if (size.IsEmpty()) {
    return;
  }
  if (!iosurface_cache_size_.IsEmpty() && iosurface_cache_size_ != size) {
    ClearIOSurfaceCache();
  }
  iosurface_cache_size_ = size;

  ui::ContextFactory* cf = content::GetContextFactory();
  if (!cf) {
    return;
  }
  scoped_refptr<viz::RasterContextProvider> provider =
      cf->SharedMainThreadRasterContextProvider();
  if (!provider) {
    return;
  }
  gpu::SharedImageInterface* sii = provider->SharedImageInterface();
  if (!sii) {
    return;
  }

  const uintptr_t io_surface_key = reinterpret_cast<uintptr_t>(io.get());
  const uint32_t io_surface_id = IOSurfaceGetID(io.get());
  CachedIOSurfaceSharedImage* cached =
      FindCachedSharedImage(io_surface_key, io_surface_id);
  scoped_refptr<gpu::ClientSharedImage> si;
  gpu::SyncToken resource_sync_token;
  if (cached) {
    si = cached->shared_image;
    cached->last_used = ++iosurface_cache_use_counter_;
    ++cached->outstanding_releases;
    resource_sync_token =
        si->BackingWasExternallyUpdated(cached->release_sync_token);
  } else {
    gfx::GpuMemoryBufferHandle handle(std::move(io));
    si = sii->CreateSharedImage(
        gpu::SharedImageInfo(viz::SinglePlaneFormat::kBGRA_8888, size,
                             gfx::ColorSpace::CreateSRGB(),
                             gpu::SHARED_IMAGE_USAGE_DISPLAY_READ,
                             "CmuxComposite"),
        std::move(handle));
    if (!si) {
      LOG(ERROR) << "cmux-comp: CreateSharedImage failed";
      return;
    }
    CachedIOSurfaceSharedImage entry;
    entry.io_surface_key = io_surface_key;
    entry.io_surface_id = io_surface_id;
    entry.shared_image = si;
    entry.outstanding_releases = 1;
    entry.last_used = ++iosurface_cache_use_counter_;
    iosurface_cache_.push_back(std::move(entry));
    resource_sync_token = si->creation_sync_token();
    TrimIOSurfaceCache();
  }
  viz::TransferableResource resource = viz::TransferableResource::Make(
      si, viz::TransferableResource::ResourceSource::kUI, resource_sync_token);
  const gfx::Size dip =
      GetLocalBounds().IsEmpty() ? size : GetLocalBounds().size();
  texture_layer_->SetBounds(GetLocalBounds());
  texture_layer_->SetTransferableResource(
      resource,
      base::BindOnce(
          [](base::WeakPtr<CmuxCompositorTestPane> pane,
             scoped_refptr<gpu::ClientSharedImage> shared_image,
             const gpu::SyncToken& sync_token, bool is_lost) {
            shared_image->UpdateDestructionSyncToken(sync_token);
            if (pane) {
              pane->OnTransferableResourceReleased(shared_image.get(),
                                                   sync_token, is_lost);
            }
          },
          weak_factory_.GetWeakPtr(), std::move(si)),
      dip);
}

void CmuxCompositorTestPane::OnTransferableResourceReleased(
    gpu::ClientSharedImage* shared_image,
    const gpu::SyncToken& sync_token,
    bool /*is_lost*/) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  auto it =
      std::find_if(iosurface_cache_.begin(), iosurface_cache_.end(),
                   [shared_image](const CachedIOSurfaceSharedImage& entry) {
                     return entry.shared_image.get() == shared_image;
                   });
  if (it == iosurface_cache_.end()) {
    return;
  }
  it->release_sync_token = sync_token;
  if (it->outstanding_releases > 0) {
    --it->outstanding_releases;
  } else {
    DCHECK(false);
  }
  TrimIOSurfaceCache();
}

CmuxCompositorTestPane::CachedIOSurfaceSharedImage*
CmuxCompositorTestPane::FindCachedSharedImage(uintptr_t io_surface_key,
                                              uint32_t io_surface_id) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  auto it =
      std::find_if(iosurface_cache_.begin(), iosurface_cache_.end(),
                   [io_surface_key](const CachedIOSurfaceSharedImage& entry) {
                     return entry.io_surface_key == io_surface_key;
                   });
  if (it == iosurface_cache_.end()) {
    return nullptr;
  }
  if (it->io_surface_id != io_surface_id) {
    iosurface_cache_.erase(it);
    return nullptr;
  }
  return &*it;
}

void CmuxCompositorTestPane::ClearIOSurfaceCache() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  iosurface_cache_.clear();
  iosurface_cache_size_ = gfx::Size();
}

void CmuxCompositorTestPane::TrimIOSurfaceCache() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  while (iosurface_cache_.size() > kMaxIOSurfaceSharedImageCacheSize) {
    auto oldest =
        std::min_element(iosurface_cache_.begin(), iosurface_cache_.end(),
                         [](const CachedIOSurfaceSharedImage& a,
                            const CachedIOSurfaceSharedImage& b) {
                           return a.last_used < b.last_used;
                         });
    iosurface_cache_.erase(oldest);
  }
}

void CmuxCompositorTestPane::EnsureGradientTexture() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (created_) {
    return;
  }
  const gfx::Size size(kGradW, kGradH);
  gfx::ScopedIOSurface io =
      gfx::CreateIOSurface(size, viz::SinglePlaneFormat::kBGRA_8888,
                           /*should_clear=*/false);
  if (!io) {
    LOG(ERROR) << "cmux-comp: CreateIOSurface failed";
    return;
  }
  {
    IOSurfaceRef ref = io.get();
    IOSurfaceLock(ref, 0, nullptr);
    const size_t bpr = IOSurfaceGetBytesPerRow(ref);
    // SAFETY: the IOSurface we just created is at least bpr*kGradH bytes and is
    // locked for CPU access here, so this span covers valid memory.
    base::span<uint8_t> buf = UNSAFE_BUFFERS(
        base::span<uint8_t>(static_cast<uint8_t*>(IOSurfaceGetBaseAddress(ref)),
                            bpr * static_cast<size_t>(kGradH)));
    for (int y = 0; y < kGradH; ++y) {
      for (int x = 0; x < kGradW; ++x) {
        const size_t o =
            static_cast<size_t>(y) * bpr + static_cast<size_t>(x) * 4u;  // BGRA
        buf[o + 0] = static_cast<uint8_t>(x * 255 / kGradW);             // B
        buf[o + 1] = static_cast<uint8_t>(y * 255 / kGradH);             // G
        buf[o + 2] = 0x80;                                               // R
        buf[o + 3] = 0xff;                                               // A
      }
    }
    IOSurfaceUnlock(ref, 0, nullptr);
  }
  created_ = true;
  // Hand ownership of the IOSurface ref to CompositeIOSurface.
  CompositeIOSurface(io.release());
  VLOG(1) << "cmux-comp: gradient IOSurface composited";
}

views::View* CmuxCompositorTestPane::AsView() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  return this;
}

SurfaceKind CmuxCompositorTestPane::kind() const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  return SurfaceKind::kTerminal;
}

void CmuxCompositorTestPane::FocusContent() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
}

BEGIN_METADATA(CmuxCompositorTestPane)
END_METADATA

}  // namespace cmux
