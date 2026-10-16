// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "chrome/browser/cmux_term/cmux_ghostty.h"
#include "chrome/browser/cmux_term/cmux_layout_config.h"
#include "chrome/browser/cmux_term/cmux_terminal_mouse_router.h"
#include "chrome/browser/cmux_term/cmux_theme_ghostty.h"

#import <AppKit/AppKit.h>
#import <IOSurface/IOSurfaceRef.h>
#import <QuartzCore/QuartzCore.h>
#include <dispatch/dispatch.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <deque>
#include <limits>
#include <memory>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "base/apple/scoped_mach_port.h"
#include "base/check.h"
#include "base/containers/span.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/logging.h"
#include "chrome/services/cmux_terminal_renderer/public/mojom/cmux_terminal_renderer.mojom.h"
#include "content/public/browser/service_process_host.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "mojo/public/cpp/platform/platform_handle.h"
#include "ui/gfx/mac/io_surface.h"

// GHOSTTY_STATIC comes from the ghostty_public_config gn config.
#include "ghostty.h"

// libghostty is prebuilt against the system libc++; chromium statically links
// its own libc++ built with different visibility, so it omits a set of
// out-of-line libc++ symbols libghostty references (std::mutex dtor,
// basic_string out-of-line members, std::to_string, ...). The framework link
// backstops these from the system /usr/lib/libc++ dylib (same std::__1 ABI;
// libghostty's C++ objects never cross the C API boundary, so this is safe).
// See scripts/apply.sh (the `-lc++` framework ldflag).

namespace cmux {

namespace {

NSHashTable<CmuxGhosttyTerminalView*>* LiveTerminalViews() {
  static NSHashTable<CmuxGhosttyTerminalView*>* views =
      [NSHashTable weakObjectsHashTable];
  return views;
}

uint64_t& ThemeBroadcastGeneration() {
  static uint64_t generation = 0;
  return generation;
}

static_assert(GHOSTTY_SURFACE_IO_MANUAL_MIRROR == 2);
static_assert(sizeof(ghostty_surface_config_s) == 168);
constexpr NSUInteger kMaxPendingMirrorOutputBytes = 16 * 1024 * 1024;
constexpr OSType kCmuxBgraPixelFormat = 0x42475241u;
// Positioned buttons, releases, and scrolls expand to two ordered renderer
// RPCs (move + action). Keep the worst-case expanded wake burst, including the
// release reserve, well beneath the service's 4096-operation lane so a fresh
// output fence and lifecycle/control work retain ample headroom.
constexpr size_t kMaxDeferredSemanticOperations = 1024;
constexpr size_t kMaxDeferredSemanticBytes = 4 * 1024 * 1024;
constexpr size_t kMaxDeferredMouseReleaseReserve = 64;

struct PendingMouseRelease {
  uint64_t token = 0;
  float x = 0;
  float y = 0;
  int32_t button = GHOSTTY_MOUSE_UNKNOWN;
  uint32_t mods = 0;
};

struct DeferredSemanticOperation {
  enum class Kind {
    kKey,
    kText,
    kPreedit,
    kPaste,
    kCopy,
    kMouseMove,
    kMouseButton,
    kMouseRelease,
    kMouseScroll,
  };

  Kind kind = Kind::kText;
  mojom::TerminalKeyEventPtr key;
  std::string text;
  float x = 0;
  float y = 0;
  float pointer_x = 0;
  float pointer_y = 0;
  uint32_t mods = 0;
  int32_t first = 0;
  int32_t second = 0;
  uint64_t token = 0;

  size_t EstimatedBytes() const {
    return sizeof(*this) + text.size() + (key ? key->text.size() : 0);
  }
};

ghostty_input_mods_e ModsFromEvent(NSEvent* event) {
  NSEventModifierFlags flags = event.modifierFlags;
  int mods = GHOSTTY_MODS_NONE;
  if (flags & NSEventModifierFlagShift)
    mods |= GHOSTTY_MODS_SHIFT;
  if (flags & NSEventModifierFlagControl)
    mods |= GHOSTTY_MODS_CTRL;
  if (flags & NSEventModifierFlagOption)
    mods |= GHOSTTY_MODS_ALT;
  if (flags & NSEventModifierFlagCommand)
    mods |= GHOSTTY_MODS_SUPER;
  if (flags & NSEventModifierFlagCapsLock)
    mods |= GHOSTTY_MODS_CAPS;
  return static_cast<ghostty_input_mods_e>(mods);
}

ghostty_input_mouse_button_e MouseButtonFromEvent(NSEvent* event) {
  switch (event.buttonNumber) {
    case 0:
      return GHOSTTY_MOUSE_LEFT;
    case 1:
      return GHOSTTY_MOUSE_RIGHT;
    case 2:
      return GHOSTTY_MOUSE_MIDDLE;
    case 3:
      return GHOSTTY_MOUSE_EIGHT;
    case 4:
      return GHOSTTY_MOUSE_NINE;
    case 5:
      return GHOSTTY_MOUSE_SIX;
    case 6:
      return GHOSTTY_MOUSE_SEVEN;
    case 7:
      return GHOSTTY_MOUSE_FOUR;
    case 8:
      return GHOSTTY_MOUSE_FIVE;
    case 9:
      return GHOSTTY_MOUSE_TEN;
    case 10:
      return GHOSTTY_MOUSE_ELEVEN;
    default:
      return GHOSTTY_MOUSE_UNKNOWN;
  }
}

ghostty_input_scroll_mods_t ScrollModsFromEvent(NSEvent* event) {
  int momentum = GHOSTTY_MOUSE_MOMENTUM_NONE;
  switch (event.momentumPhase) {
    case NSEventPhaseBegan:
      momentum = GHOSTTY_MOUSE_MOMENTUM_BEGAN;
      break;
    case NSEventPhaseStationary:
      momentum = GHOSTTY_MOUSE_MOMENTUM_STATIONARY;
      break;
    case NSEventPhaseChanged:
      momentum = GHOSTTY_MOUSE_MOMENTUM_CHANGED;
      break;
    case NSEventPhaseEnded:
      momentum = GHOSTTY_MOUSE_MOMENTUM_ENDED;
      break;
    case NSEventPhaseCancelled:
      momentum = GHOSTTY_MOUSE_MOMENTUM_CANCELLED;
      break;
    case NSEventPhaseMayBegin:
      momentum = GHOSTTY_MOUSE_MOMENTUM_MAY_BEGIN;
      break;
    default:
      break;
  }
  return (event.hasPreciseScrollingDeltas ? 1 : 0) | (momentum << 1);
}

// Modifiers that were "consumed" to translate the key into text. Control and
// command never contribute to text translation. Shift/CapsLock do. Option/Alt
// is the subtle one: it is consumed ONLY when it actually changed the produced
// character (dead keys / alt-graph: Option-e, Option-a -> å). For keys where
// Option leaves the character unchanged (Backspace, arrows, Return, Tab), it
// must stay UNconsumed so libghostty applies the alt-as-meta ESC prefix -- that
// is exactly what turns Option-Backspace into delete-word and Option-f/b into
// word motion. (The old code marked Option consumed unconditionally, which
// suppressed that encoding, so Option-Backspace behaved like a bare Backspace.)
ghostty_input_mods_e ConsumedModsFromEvent(NSEvent* event) {
  NSEventModifierFlags flags =
      event.modifierFlags &
      ~(NSEventModifierFlagControl | NSEventModifierFlagCommand);
  int mods = GHOSTTY_MODS_NONE;
  if (flags & NSEventModifierFlagShift)
    mods |= GHOSTTY_MODS_SHIFT;
  if (flags & NSEventModifierFlagCapsLock)
    mods |= GHOSTTY_MODS_CAPS;
  if (flags & NSEventModifierFlagOption) {
    NSString* with = [event charactersByApplyingModifiers:flags];
    NSString* without = [event
        charactersByApplyingModifiers:(flags & ~NSEventModifierFlagOption)];
    if (with.length > 0 && ![with isEqualToString:without]) {
      mods |= GHOSTTY_MODS_ALT;
    }
  }
  return static_cast<ghostty_input_mods_e>(mods);
}

}  // namespace
}  // namespace cmux

static void CmuxAppendTerminalResizeTrace(int descriptor, NSString* line) {
  if (descriptor < 0 || line.length == 0) {
    return;
  }
  NSData* data = [line dataUsingEncoding:NSUTF8StringEncoding];
  // SAFETY: NSData guarantees that `bytes` addresses `length` contiguous
  // bytes for the lifetime of `data`; span keeps subsequent slicing checked.
  base::span<const uint8_t> remaining = UNSAFE_BUFFERS(
      base::span<const uint8_t>(static_cast<const uint8_t*>(data.bytes),
                                static_cast<size_t>(data.length)));
  while (!remaining.empty()) {
    const ssize_t written =
        write(descriptor, remaining.data(), remaining.size());
    if (written < 0 && errno == EINTR) {
      continue;
    }
    if (written <= 0) {
      break;
    }
    remaining = remaining.subspan(static_cast<size_t>(written));
  }
}

@interface CmuxGhosttyTerminalView ()
- (void)rendererInitialized:(uint64_t)rendererIncarnation
                    success:(BOOL)success
                      error:(NSString*)error
              backgroundRed:(uint8_t)backgroundRed
            backgroundGreen:(uint8_t)backgroundGreen
             backgroundBlue:(uint8_t)backgroundBlue;
- (void)rendererDisconnected:(NSString*)reason
         rendererIncarnation:(uint64_t)rendererIncarnation;
- (void)displayRendererSurface:(IOSurfaceRef)surface
           rendererIncarnation:(uint64_t)rendererIncarnation
              deliverySequence:(uint64_t)deliverySequence
                    frameToken:(uint64_t)frameToken
                 geometryEpoch:(uint64_t)geometryEpoch
                       widthPx:(uint32_t)widthPx
                      heightPx:(uint32_t)heightPx
                    colorSpace:(cmux::mojom::TerminalFrameColorSpace)colorSpace;
- (void)rendererInput:(NSData*)data;
- (void)rendererTerminalHostInputCutoverReady:(uint64_t)cutoverId
                                      success:(BOOL)success
                                        error:(std::string)error;
- (void)rendererTerminalHostInputCutoverCancelled:(uint64_t)cutoverId
                                           reason:(std::string)reason;
- (void)rendererColdEvictionReady:(uint64_t)evictionId
              rendererIncarnation:(uint64_t)rendererIncarnation
                           success:(BOOL)success
                             error:(std::string)error;
- (void)rendererGridColumns:(uint16_t)columns
                       rows:(uint16_t)rows
                    widthPx:(uint32_t)widthPx
                   heightPx:(uint32_t)heightPx
                cellWidthPx:(uint32_t)cellWidthPx
               cellHeightPx:(uint32_t)cellHeightPx
              geometryEpoch:(uint64_t)geometryEpoch;
- (void)rendererTerminalHostViewerSizeUnchanged:(uint16_t)columns
                                           rows:(uint16_t)rows
                                  geometryEpoch:(uint64_t)geometryEpoch;
- (void)rendererTerminalHostBackground:(BOOL)hasOverride
                                   red:(uint8_t)red
                                 green:(uint8_t)green
                                  blue:(uint8_t)blue;
- (void)prepareResolvedThemeBackgroundRed:(uint8_t)red
                                    green:(uint8_t)green
                                     blue:(uint8_t)blue;
- (BOOL)hasVisibleThemeRenderer;
- (BOOL)hasHiddenThemeRenderer;
- (void)sendResolvedThemeConfig:(const std::string&)themeConfig;
- (void)rendererClose:(BOOL)processAlive
    rendererIncarnation:(uint64_t)rendererIncarnation;
- (void)rendererTerminalHostAttached:(uint64_t)cutoverId
                 rendererIncarnation:(uint64_t)rendererIncarnation
                             success:(BOOL)success;
- (void)rendererTerminalHostDisconnected:(NSString*)reason;
- (void)sendPendingAuthoritativeReplay;
- (void)replayPendingMouseReleases;
- (void)requeueDurableMouseReleases;
- (BOOL)shouldDeferSemanticOperation;
- (BOOL)enqueueDeferredSemanticOperation:
            (cmux::DeferredSemanticOperation)operation;
- (BOOL)dispatchSemanticOperation:(cmux::DeferredSemanticOperation)operation;
- (BOOL)submitSemanticOperation:(cmux::DeferredSemanticOperation)operation;
- (void)flushDeferredSemanticOperations;
- (BOOL)sendMouseButton:(NSEvent*)event
                  state:(ghostty_input_mouse_state_e)state
                 button:(ghostty_input_mouse_button_e)button;
- (void)mouseReleaseAcknowledged:(uint64_t)token
             rendererIncarnation:(uint64_t)rendererIncarnation
                        accepted:(BOOL)accepted
                        consumed:(BOOL)consumed;
- (void)mouseButtonAcknowledged:(uint64_t)token
            rendererIncarnation:(uint64_t)rendererIncarnation
                       accepted:(BOOL)accepted
                       consumed:(BOOL)consumed;
- (void)fallbackPendingNativeRightMouseEvents;
- (void)replayNativeRightMouseEvents:(NSArray<NSEvent*>*)events;
- (void)scheduleRendererDeliveryAcknowledgment:(uint64_t)deliverySequence
                           rendererIncarnation:
                               (uint64_t)rendererIncarnation;
- (void)finishPendingRendererDeliveryAcknowledgment;
- (void)cancelPendingRendererDeliveryAcknowledgment;
- (void)rendererDisplayLinkDidFire:(id)displayLink;
- (void)acknowledgeRendererDelivery:(uint64_t)deliverySequence
                rendererIncarnation:(uint64_t)rendererIncarnation;
- (void)releaseRendererFrame:(uint64_t)frameToken
         rendererIncarnation:(uint64_t)rendererIncarnation;
- (void)copy:(id)sender;
- (void)cut:(id)sender;
- (void)paste:(id)sender;
- (void)appendResizeTraceEvent:(NSString*)event;
@end

// A CADisplayLink retains its target until invalidation. Keep the view weak so
// the view can own the display link without forming a lifetime cycle.
@interface CmuxFrameDeliveryDisplayLinkTarget : NSObject
- (instancetype)initWithView:(CmuxGhosttyTerminalView*)view;
- (void)displayLinkDidFire:(id)displayLink;
@end

@implementation CmuxFrameDeliveryDisplayLinkTarget {
  __weak CmuxGhosttyTerminalView* _view;
}

- (instancetype)initWithView:(CmuxGhosttyTerminalView*)view {
  if ((self = [super init])) {
    _view = view;
  }
  return self;
}

- (void)displayLinkDidFire:(id)displayLink {
  [_view rendererDisplayLinkDidFire:displayLink];
}

@end

namespace cmux {

class CmuxGhosttyRendererClient final
    : public mojom::CmuxTerminalRendererClient {
 public:
  CmuxGhosttyRendererClient(CmuxGhosttyTerminalView* view,
                            uint64_t renderer_incarnation)
      : view_(view),
        renderer_incarnation_(renderer_incarnation),
        receiver_(this) {}
  ~CmuxGhosttyRendererClient() override = default;

  mojo::PendingRemote<mojom::CmuxTerminalRendererClient> BindNewRemote() {
    return receiver_.BindNewPipeAndPassRemote();
  }

  base::WeakPtr<CmuxGhosttyRendererClient> GetWeakPtr() {
    return weak_factory_.GetWeakPtr();
  }

  void OnInitialized(bool success,
                     const std::string& error,
                     uint8_t background_red,
                     uint8_t background_green,
                     uint8_t background_blue) {
    CmuxGhosttyTerminalView* view = view_;
    if (view) {
      NSString* message = [NSString stringWithUTF8String:error.c_str()];
      const uint64_t rendererIncarnation = renderer_incarnation_;
      dispatch_async(dispatch_get_main_queue(), ^{
        [view rendererInitialized:rendererIncarnation
                          success:success
                            error:message
                    backgroundRed:background_red
                  backgroundGreen:background_green
                   backgroundBlue:background_blue];
      });
    }
  }

  void OnRendererDisconnected() {
    CmuxGhosttyTerminalView* view = view_;
    if (view) {
      const uint64_t rendererIncarnation = renderer_incarnation_;
      dispatch_async(dispatch_get_main_queue(), ^{
        [view rendererDisconnected:@"renderer utility process disconnected"
               rendererIncarnation:rendererIncarnation];
      });
    }
  }

  void OnTerminalHostAttached(
      uint64_t cutover_id,
      base::OnceCallback<void(bool, std::string)> completion,
      bool success,
      const std::string& error) {
    CmuxGhosttyTerminalView* view = view_;
    if (view) {
      [view rendererTerminalHostAttached:cutover_id
                     rendererIncarnation:renderer_incarnation_
                                 success:success];
    }
    if (completion) {
      std::move(completion).Run(success, error);
    }
  }

  void OnSelectionCopied(bool success, const std::string& text) {
    if (!success || text.empty()) {
      return;
    }
    NSString* value = [[NSString alloc] initWithBytes:text.data()
                                               length:text.size()
                                             encoding:NSUTF8StringEncoding];
    if (!value) {
      return;
    }
    dispatch_async(dispatch_get_main_queue(), ^{
      NSPasteboard* pasteboard = NSPasteboard.generalPasteboard;
      [pasteboard clearContents];
      [pasteboard writeObjects:@[ value ]];
    });
  }

  void OnMouseReleaseAcknowledged(uint64_t token,
                                  bool accepted,
                                  bool consumed) {
    CmuxGhosttyTerminalView* view = view_;
    if (view) {
      [view mouseReleaseAcknowledged:token
                 rendererIncarnation:renderer_incarnation_
                            accepted:accepted
                            consumed:consumed];
    }
  }

  void OnMouseButtonAcknowledged(uint64_t token,
                                 bool accepted,
                                 bool consumed) {
    CmuxGhosttyTerminalView* view = view_;
    if (view) {
      [view mouseButtonAcknowledged:token
                rendererIncarnation:renderer_incarnation_
                           accepted:accepted
                           consumed:consumed];
    }
  }

  // mojom::CmuxTerminalRendererClient:
  void OnFrame(mojo::PlatformHandle io_surface,
               uint64_t delivery_sequence,
               uint64_t frame_token,
               uint64_t geometry_epoch,
               uint32_t width_px,
               uint32_t height_px,
               mojom::TerminalFrameColorSpace color_space) override {
    CmuxGhosttyTerminalView* view = view_;
    if (!view) {
      return;
    }
    if (!io_surface.is_valid_mach_send()) {
      [view displayRendererSurface:nullptr
               rendererIncarnation:renderer_incarnation_
                  deliverySequence:delivery_sequence
                        frameToken:frame_token
                     geometryEpoch:geometry_epoch
                           widthPx:width_px
                          heightPx:height_px
                        colorSpace:color_space];
      return;
    }
    base::apple::ScopedMachSendRight port = io_surface.TakeMachSendRight();
    gfx::ScopedIOSurface surface(IOSurfaceLookupFromMachPort(port.get()));
    [view displayRendererSurface:surface ? surface.get() : nullptr
             rendererIncarnation:renderer_incarnation_
                deliverySequence:delivery_sequence
                      frameToken:frame_token
                   geometryEpoch:geometry_epoch
                         widthPx:width_px
                        heightPx:height_px
                      colorSpace:color_space];
  }

  void OnInput(const std::vector<uint8_t>& bytes) override {
    CmuxGhosttyTerminalView* view = view_;
    if (view && !bytes.empty()) {
      [view rendererInput:[NSData dataWithBytes:bytes.data()
                                         length:bytes.size()]];
    }
  }

  void OnTerminalHostInputCutoverReady(uint64_t cutover_id,
                                       bool success,
                                       const std::string& error) override {
    CmuxGhosttyTerminalView* view = view_;
    if (view) {
      [view rendererTerminalHostInputCutoverReady:cutover_id
                                          success:success
                                            error:error];
    }
  }

  void OnTerminalHostInputCutoverCancelled(uint64_t cutover_id,
                                           const std::string& reason) override {
    CmuxGhosttyTerminalView* view = view_;
    if (view) {
      [view rendererTerminalHostInputCutoverCancelled:cutover_id reason:reason];
    }
  }

  void OnColdEvictionReady(uint64_t eviction_id,
                           bool success,
                           const std::string& error) override {
    CmuxGhosttyTerminalView* view = view_;
    if (!view) {
      return;
    }
    const uint64_t rendererIncarnation = renderer_incarnation_;
    std::string copied_error = error;
    dispatch_async(dispatch_get_main_queue(), ^{
      [view rendererColdEvictionReady:eviction_id
                  rendererIncarnation:rendererIncarnation
                               success:success
                                 error:copied_error];
    });
  }

  void OnGridSize(uint16_t columns,
                  uint16_t rows,
                  uint32_t width_px,
                  uint32_t height_px,
                  uint32_t cell_width_px,
                  uint32_t cell_height_px,
                  uint64_t geometry_epoch) override {
    CmuxGhosttyTerminalView* view = view_;
    if (view) {
      [view rendererGridColumns:columns
                           rows:rows
                        widthPx:width_px
                       heightPx:height_px
                    cellWidthPx:cell_width_px
                   cellHeightPx:cell_height_px
                  geometryEpoch:geometry_epoch];
    }
  }

  void OnTerminalHostViewerSizeUnchanged(
      uint16_t columns,
      uint16_t rows,
      uint64_t geometry_epoch) override {
    CmuxGhosttyTerminalView* view = view_;
    if (view) {
      [view rendererTerminalHostViewerSizeUnchanged:columns
                                               rows:rows
                                      geometryEpoch:geometry_epoch];
    }
  }

  void OnTerminalHostBackground(bool has_override,
                                uint8_t red,
                                uint8_t green,
                                uint8_t blue) override {
    CmuxGhosttyTerminalView* view = view_;
    if (view) {
      [view rendererTerminalHostBackground:has_override
                                       red:red
                                     green:green
                                      blue:blue];
    }
  }

  void OnTerminalHostTitle(const std::string& title) override {
    CmuxGhosttyTerminalView* view = view_;
    if (view && view.onTerminalHostTitle) {
      view.onTerminalHostTitle([NSString stringWithUTF8String:title.c_str()]);
    }
  }

  void OnTerminalHostPwd(const std::string& pwd) override {
    CmuxGhosttyTerminalView* view = view_;
    if (view && view.onTerminalHostPwd) {
      view.onTerminalHostPwd([NSString stringWithUTF8String:pwd.c_str()]);
    }
  }

  void OnTerminalHostBell() override {
    CmuxGhosttyTerminalView* view = view_;
    if (view && view.onTerminalHostBell) {
      view.onTerminalHostBell();
    }
  }

  void OnTerminalHostExit() override {
    CmuxGhosttyTerminalView* view = view_;
    if (view && view.onTerminalHostExit) {
      view.onTerminalHostExit();
    }
  }

  void OnTerminalHostDisconnected(const std::string& reason) override {
    CmuxGhosttyTerminalView* view = view_;
    if (view) {
      [view rendererTerminalHostDisconnected:
                [NSString stringWithUTF8String:reason.c_str()]];
    }
  }

  void OnClose(bool process_alive) override {
    CmuxGhosttyTerminalView* view = view_;
    if (view) {
      const uint64_t rendererIncarnation = renderer_incarnation_;
      dispatch_async(dispatch_get_main_queue(), ^{
        [view rendererClose:process_alive
            rendererIncarnation:rendererIncarnation];
      });
    }
  }

  void OnError(const std::string& reason) override {
    CmuxGhosttyTerminalView* view = view_;
    if (view) {
      NSString* message = [NSString stringWithUTF8String:reason.c_str()];
      const uint64_t rendererIncarnation = renderer_incarnation_;
      dispatch_async(dispatch_get_main_queue(), ^{
        [view rendererDisconnected:message
               rendererIncarnation:rendererIncarnation];
      });
    }
  }

 private:
  __weak CmuxGhosttyTerminalView* view_;
  const uint64_t renderer_incarnation_;
  mojo::Receiver<mojom::CmuxTerminalRendererClient> receiver_;
  base::WeakPtrFactory<CmuxGhosttyRendererClient> weak_factory_{this};
};

}  // namespace cmux

@implementation CmuxGhosttyTerminalView {
  mojo::Remote<cmux::mojom::CmuxTerminalRenderer> _renderer;
  std::unique_ptr<cmux::CmuxGhosttyRendererClient> _rendererClient;
  NSMutableAttributedString* _markedText;
  NSMutableArray<NSString*>* _keyTextAccumulator;
  NSMutableDictionary<NSNumber*, NSEvent*>* _pendingNativeRightMouseDownEvents;
  NSMutableDictionary<NSNumber*, NSEvent*>* _pendingNativeRightMouseUpEvents;
  NSData* _pendingReplay;
  NSMutableData* _pendingOutput;
  NSString* _resizeTracePath;
  NSString* _resizeTraceViewIdentifier;
  int _resizeTraceDescriptor;
  // CADisplayLink is macOS 14+, while cmux still deploys to macOS 12. Keep the
  // stored type availability-neutral and access it only inside guards below.
  id _frameDeliveryDisplayLink;
  CmuxFrameDeliveryDisplayLinkTarget* _frameDeliveryDisplayLinkTarget;
  uint16_t _columns;
  uint16_t _rows;
  uint16_t _authoritativeColumns;
  uint16_t _authoritativeRows;
  uint64_t _authoritativeGridGeneration;
  uint64_t _lastDeliverySequence;
  uint64_t _pendingDeliveryAcknowledgmentSequence;
  uint64_t _pendingDeliveryAcknowledgmentRendererIncarnation;
  uint64_t _fallbackAcknowledgmentGeneration;
  uint64_t _displayedFrameToken;
  uint64_t _geometryEpoch;
  uint64_t _rendererIncarnation;
  uint64_t _authoritativeTransitionEpoch;
  uint64_t _terminalHostInputCutoverId;
  uint64_t _mouseCaptureRendererIncarnation;
  BOOL _rendererReady;
  BOOL _retryScheduled;
  BOOL _resizeScheduled;
  BOOL _hasAuthoritativeGrid;
  BOOL _gridIsAuthoritative;
  BOOL _authoritativeTransitionInFlight;
  BOOL _physicalResizePending;
  BOOL _hasConfiguredBackground;
  BOOL _hasTerminalHostBackgroundOverride;
  BOOL _terminalHostAttachInFlight;
  BOOL _directTerminalHostAttached;
  BOOL _rendererVisible;
  BOOL _coldSuspended;
  BOOL _coldEvictionInFlight;
  BOOL _keyCommandHandled;
  BOOL _synthesizingRightMouseRelease;
  uint8_t _configuredBackgroundRed;
  uint8_t _configuredBackgroundGreen;
  uint8_t _configuredBackgroundBlue;
  uint32_t _pendingWidthPx;
  uint32_t _pendingHeightPx;
  float _pendingScale;
  uint32_t _sentWidthPx;
  uint32_t _sentHeightPx;
  uint32_t _expectedFrameWidthPx;
  uint32_t _expectedFrameHeightPx;
  uint32_t _displayedFrameWidthPx;
  uint32_t _displayedFrameHeightPx;
  uint32_t _canonicalWidthPx;
  uint32_t _canonicalHeightPx;
  uint32_t _cellWidthPx;
  uint32_t _cellHeightPx;
  uint16_t _terminalHostRequestedColumns;
  uint16_t _terminalHostRequestedRows;
  float _sentScale;
  float _displayedFrameScale;
  float _canonicalScale;
  float _terminalHostRequestedScale;
  cmux::TerminalMouseCapture _mouseCapture;
  std::vector<cmux::PendingMouseRelease> _pendingMouseReleases;
  std::vector<cmux::PendingMouseRelease> _durableMouseReleases;
  std::unordered_set<uint64_t> _mouseReleaseTokensInFlight;
  std::deque<cmux::DeferredSemanticOperation> _deferredSemanticOperations;
  size_t _deferredSemanticBytes;
  uint64_t _nextMouseReleaseToken;
  uint64_t _nextMouseButtonToken;
  uint64_t _resizeTraceSequence;
  base::OnceCallback<void(bool, std::string)>
      _terminalHostInputCutoverReadyCompletion;
  base::OnceClosure _terminalHostInputCutoverCancelCompletion;
  base::OnceCallback<void(bool, std::string)> _coldEvictionCompletion;
  uint64_t _coldEvictionId;
}

@synthesize onActivate = _onActivate;
@synthesize onInteraction = _onInteraction;
@synthesize onCloseSurface = _onCloseSurface;
@synthesize onGridSize = _onGridSize;
@synthesize onInput = _onInput;
@synthesize onRendererError = _onRendererError;
@synthesize onTerminalHostTitle = _onTerminalHostTitle;
@synthesize onTerminalHostPwd = _onTerminalHostPwd;
@synthesize onTerminalHostBell = _onTerminalHostBell;
@synthesize onTerminalHostExit = _onTerminalHostExit;
@synthesize onTerminalHostDisconnected = _onTerminalHostDisconnected;

- (instancetype)initWithFrame:(NSRect)frame {
  if ((self = [super initWithFrame:frame])) {
    [cmux::LiveTerminalViews() addObject:self];
    _resizeTraceDescriptor = -1;
    NSString* requestedResizeTracePath =
        NSProcessInfo.processInfo.environment[@"CMUX_XCUI_TERMINAL_RESIZE_TRACE"];
    requestedResizeTracePath =
        [requestedResizeTracePath stringByStandardizingPath];
    NSString* const resizeTraceBasename = @"terminal-resize-trace.log";
    NSString* const resizeTraceSessionPrefix = @"cmux-xcui-session-";
    if ([requestedResizeTracePath.lastPathComponent
            isEqualToString:resizeTraceBasename]) {
      NSString* resizeTraceParent =
          requestedResizeTracePath.stringByDeletingLastPathComponent;
      char resolvedResizeTraceParent[PATH_MAX];
      if (realpath(resizeTraceParent.fileSystemRepresentation,
                   resolvedResizeTraceParent)) {
        resizeTraceParent =
            [NSString stringWithUTF8String:resolvedResizeTraceParent];
        NSString* const resizeTraceSessionName =
            resizeTraceParent.lastPathComponent;
        const BOOL isDirectPrivateTmpSession =
            [resizeTraceParent.stringByDeletingLastPathComponent
                isEqualToString:@"/private/tmp"] &&
            [resizeTraceSessionName hasPrefix:resizeTraceSessionPrefix] &&
            resizeTraceSessionName.length > resizeTraceSessionPrefix.length;
        if (isDirectPrivateTmpSession) {
          const int resizeTraceParentDescriptor =
              open(resizeTraceParent.fileSystemRepresentation,
                   O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
          struct stat resizeTraceParentStat = {};
          const BOOL ownsResizeTraceParent =
              resizeTraceParentDescriptor >= 0 &&
              fstat(resizeTraceParentDescriptor, &resizeTraceParentStat) == 0 &&
              S_ISDIR(resizeTraceParentStat.st_mode) &&
              resizeTraceParentStat.st_uid == geteuid();
          if (ownsResizeTraceParent) {
            const int resizeTraceDescriptor = openat(
                resizeTraceParentDescriptor,
                resizeTraceBasename.fileSystemRepresentation,
                O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW |
                    O_NONBLOCK,
                0600);
            struct stat resizeTraceStat = {};
            const BOOL ownsRegularResizeTrace =
                resizeTraceDescriptor >= 0 &&
                fstat(resizeTraceDescriptor, &resizeTraceStat) == 0 &&
                S_ISREG(resizeTraceStat.st_mode) &&
                resizeTraceStat.st_uid == geteuid();
            if (ownsRegularResizeTrace) {
              _resizeTraceDescriptor = resizeTraceDescriptor;
              _resizeTracePath = [[resizeTraceParent
                  stringByAppendingPathComponent:resizeTraceBasename] copy];
              _resizeTraceViewIdentifier = NSUUID.UUID.UUIDString;
            } else if (resizeTraceDescriptor >= 0) {
              close(resizeTraceDescriptor);
            }
          }
          if (resizeTraceParentDescriptor >= 0) {
            close(resizeTraceParentDescriptor);
          }
        }
      }
    }
    self.wantsLayer = YES;
    // During live resize the utility may be one frame behind the host view.
    // Keep that frame pixel/cell stable and expose the matching terminal
    // background around it until the exact-size frame arrives. Stretching the
    // old frame here makes the terminal visibly zoom before every reflow.
    // NSView is flipped below. Core Animation's gravity constants are
    // interpreted in the layer's native coordinate system, so a literal
    // TopLeft would pin a retained IOSurface to the visual bottom edge while
    // input hit-testing continues to measure from the visual top.
    self.layer.contentsGravity = [self.layer contentsAreFlipped]
                                     ? kCAGravityBottomLeft
                                     : kCAGravityTopLeft;
    self.layer.magnificationFilter = kCAFilterNearest;
    self.layer.backgroundColor = NSColor.blackColor.CGColor;
    _columns = 80;
    _rows = 24;
    _rendererVisible = YES;
    _coldSuspended = YES;
    _markedText = [[NSMutableAttributedString alloc] initWithString:@""];
    _pendingNativeRightMouseDownEvents = [[NSMutableDictionary alloc] init];
    _pendingNativeRightMouseUpEvents = [[NSMutableDictionary alloc] init];
    NSMenu* contextMenu = [[NSMenu alloc] initWithTitle:@"Terminal"];
    NSMenuItem* copyItem =
        [contextMenu addItemWithTitle:@"Copy"
                               action:@selector(copy:)
                        keyEquivalent:@""];
    copyItem.target = self;
    NSMenuItem* pasteItem =
        [contextMenu addItemWithTitle:@"Paste"
                               action:@selector(paste:)
                        keyEquivalent:@""];
    pasteItem.target = self;
    self.menu = contextMenu;
  }
  return self;
}

- (void)setupSurface {
  CHECK([NSThread isMainThread]);
  if (_coldSuspended || _renderer.is_bound()) {
    return;
  }

  [self cancelPendingRendererDeliveryAcknowledgment];
  const uint64_t rendererIncarnation = ++_rendererIncarnation;
  _mouseReleaseTokensInFlight.clear();
  _gridIsAuthoritative = NO;
  [self requeueDurableMouseReleases];
  _rendererClient = std::make_unique<cmux::CmuxGhosttyRendererClient>(
      self, rendererIncarnation);
  _renderer =
      content::ServiceProcessHost::Launch<cmux::mojom::CmuxTerminalRenderer>(
          content::ServiceProcessHost::Options()
              .WithDisplayName("cmux terminal renderer")
              .Pass());
  _renderer.set_disconnect_handler(
      base::BindOnce(&cmux::CmuxGhosttyRendererClient::OnRendererDisconnected,
                     _rendererClient->GetWeakPtr()));

  const CGFloat scale =
      self.window
          ? self.window.backingScaleFactor
          : (NSScreen.mainScreen ? NSScreen.mainScreen.backingScaleFactor
                                 : 2.0);
  const NSSize size = self.bounds.size;
  const uint32_t widthPx =
      std::max<uint32_t>(static_cast<uint32_t>(size.width * scale), 1);
  const uint32_t heightPx =
      std::max<uint32_t>(static_cast<uint32_t>(size.height * scale), 1);
  _sentWidthPx = widthPx;
  _sentHeightPx = heightPx;
  _expectedFrameWidthPx = widthPx;
  _expectedFrameHeightPx = heightPx;
  _sentScale = scale;
  const uint64_t geometryEpoch = ++_geometryEpoch;
  const std::string themeName =
      cmux::GetPublishedLayoutConfig()
          .value_or(cmux::LayoutConfig())
          .ghostty_theme_name;
  _renderer->Initialize(
      _rendererClient->BindNewRemote(), widthPx, heightPx, scale, themeName,
      geometryEpoch,
      base::BindOnce(&cmux::CmuxGhosttyRendererClient::OnInitialized,
                     _rendererClient->GetWeakPtr()));
  // Every renderer incarnation starts with an empty VT. On first launch expose
  // only the background; on restart keep the preceding incarnation's retained
  // frame. In both cases, never publish the utility's blank initialization
  // before the first canonical snapshot has been applied at its grid.
  _authoritativeTransitionInFlight = YES;
  _authoritativeTransitionEpoch = 0;
  _expectedFrameWidthPx = 0;
  _expectedFrameHeightPx = 0;
}

- (void)retrySetupSurface {
  _retryScheduled = NO;
  if (_coldSuspended || _renderer.is_bound()) {
    return;
  }
  [self setupSurface];
}

- (void)restartRendererWithReason:(NSString*)reason {
  CHECK([NSThread isMainThread]);
  if (_coldSuspended) {
    [self resumeRenderer];
    return;
  }
  if (_renderer.is_bound()) {
    [self rendererDisconnected:(reason.length > 0 ? reason
                                                  : @"renderer watchdog fired")
           rendererIncarnation:_rendererIncarnation];
  } else if (!_retryScheduled) {
    [self setupSurface];
  }
}

- (void)dealloc {
  [cmux::LiveTerminalViews() removeObject:self];
  [self cancelPendingRendererDeliveryAcknowledgment];
  if (@available(macOS 14.0, *)) {
    [_frameDeliveryDisplayLink invalidate];
  }
  _frameDeliveryDisplayLink = nil;
  _frameDeliveryDisplayLinkTarget = nil;
  if (_renderer.is_bound() &&
      (_terminalHostInputCutoverId != 0 || _terminalHostAttachInFlight ||
       _directTerminalHostAttached)) {
    _renderer->DetachTerminalHost();
  }
  self.onInput = nil;
  self.onGridSize = nil;
  self.onRendererError = nil;
  self.onTerminalHostTitle = nil;
  self.onTerminalHostPwd = nil;
  self.onTerminalHostBell = nil;
  self.onTerminalHostExit = nil;
  self.onTerminalHostDisconnected = nil;
  if (_displayedFrameToken != 0 && _renderer.is_bound()) {
    self.layer.contents = nil;
    [self appendResizeTraceEvent:@"contents_cleared"];
    [self releaseRendererFrame:_displayedFrameToken
           rendererIncarnation:_rendererIncarnation];
    _displayedFrameToken = 0;
  }
  if (_resizeTraceDescriptor >= 0) {
    close(_resizeTraceDescriptor);
    _resizeTraceDescriptor = -1;
  }
  _renderer.reset();
  _rendererClient.reset();
  _terminalHostInputCutoverReadyCompletion.Reset();
  _terminalHostInputCutoverCancelCompletion.Reset();
  _coldEvictionCompletion.Reset();
}

- (NSSize)terminalGridSize {
  return NSMakeSize(_columns, _rows);
}

- (BOOL)replaceTerminalState:(NSData*)replay
        authoritativeColumns:(uint16_t)columns
                        rows:(uint16_t)rows
                  generation:(uint64_t)generation {
  CHECK([NSThread isMainThread]);
  columns = std::max<uint16_t>(columns, 1);
  rows = std::max<uint16_t>(rows, 1);
  if (_hasAuthoritativeGrid && generation < _authoritativeGridGeneration) {
    return NO;
  }
  if (_hasAuthoritativeGrid && generation == _authoritativeGridGeneration &&
      (columns != _authoritativeColumns || rows != _authoritativeRows)) {
    return NO;
  }
  _hasAuthoritativeGrid = YES;
  _authoritativeColumns = columns;
  _authoritativeRows = rows;
  _authoritativeGridGeneration = generation;
  _columns = columns;
  _rows = rows;
  _pendingReplay = [replay copy];
  // Bytes buffered before an authoritative replay are already represented by
  // that replay. Bytes arriving after this call are appended and flushed after
  // the combined replay operation completes on the renderer's output queue.
  _pendingOutput = nil;

  const CGFloat scale = self.window ? self.window.backingScaleFactor : 2.0;
  const NSSize size = self.bounds.size;
  _pendingWidthPx =
      std::max<uint32_t>(static_cast<uint32_t>(size.width * scale), 1);
  _pendingHeightPx =
      std::max<uint32_t>(static_cast<uint32_t>(size.height * scale), 1);
  _pendingScale = scale;
  if (_resizeScheduled || _pendingWidthPx != _sentWidthPx ||
      _pendingHeightPx != _sentHeightPx || _pendingScale != _sentScale) {
    // This is the latest physical AppKit geometry, but cmux has not observed it
    // yet. Preserve it across the canonical transition and reissue it only
    // after that transition presents its exact-grid frame.
    _physicalResizePending = YES;
  }

  _authoritativeTransitionInFlight = YES;
  _authoritativeTransitionEpoch = 0;
  _expectedFrameWidthPx = 0;
  _expectedFrameHeightPx = 0;
  _gridIsAuthoritative = NO;
  [self sendPendingAuthoritativeReplay];
  return YES;
}

- (void)sendPendingAuthoritativeReplay {
  CHECK([NSThread isMainThread]);
  if (!_rendererReady || !_renderer.is_bound() || !_pendingReplay ||
      !_hasAuthoritativeGrid) {
    return;
  }
  NSData* replay = _pendingReplay;
  _pendingReplay = nil;
  std::vector<uint8_t> bytes(replay.length);
  if (replay.length > 0) {
    [replay getBytes:bytes.data() length:replay.length];
  }
  const uint64_t geometryEpoch = ++_geometryEpoch;
  _authoritativeTransitionEpoch = geometryEpoch;
  _authoritativeTransitionInFlight = YES;
  _expectedFrameWidthPx = 0;
  _expectedFrameHeightPx = 0;
  // The output worker reports OnGridSize only after it has parsed the entire
  // replay. Until that callback, key/mouse encoding modes and geometry still
  // belong to a blank or preceding mirror generation.
  _gridIsAuthoritative = NO;
  _renderer->ResetAndReplayAtGrid(_authoritativeColumns, _authoritativeRows,
                                  bytes, geometryEpoch);
}

- (BOOL)processTerminalOutput:(NSData*)output {
  CHECK([NSThread isMainThread]);
  if (output.length == 0) {
    return YES;
  }
  if (!_rendererReady) {
    if (_pendingOutput.length > cmux::kMaxPendingMirrorOutputBytes ||
        output.length >
            cmux::kMaxPendingMirrorOutputBytes - _pendingOutput.length) {
      _pendingReplay = nil;
      _pendingOutput = nil;
      return NO;
    }
    if (!_pendingOutput) {
      _pendingOutput = [NSMutableData data];
    }
    [_pendingOutput appendData:output];
    return YES;
  }
  std::vector<uint8_t> bytes(output.length);
  [output getBytes:bytes.data() length:output.length];
  _renderer->ProcessOutput(bytes);
  return YES;
}

- (void)beginTerminalHostInputCutover:(uint64_t)cutoverId
                           completion:
                               (base::OnceCallback<void(bool, std::string)>)
                                   completion {
  CHECK([NSThread isMainThread]);
  if (_coldSuspended || _coldEvictionInFlight || !_rendererReady ||
      !_renderer.is_bound() || cutoverId == 0 ||
      _terminalHostInputCutoverId != 0 ||
      _terminalHostInputCutoverReadyCompletion) {
    if (completion) {
      std::move(completion)
          .Run(false, "renderer cannot quiesce terminal input");
    }
    return;
  }
  _terminalHostInputCutoverId = cutoverId;
  _terminalHostInputCutoverReadyCompletion = std::move(completion);
  _renderer->BeginTerminalHostInputCutover(cutoverId);
}

- (void)attachTerminalHostSocket:(mojo::PlatformHandle)socket
                      terminalId:(std::vector<uint8_t>)terminalId
             terminalIncarnation:(std::vector<uint8_t>)terminalIncarnation
                          rights:(uint32_t)rights
                   protocolFlags:(uint32_t)protocolFlags
                       cutoverId:(uint64_t)cutoverId
                      completion:(base::OnceCallback<void(bool, std::string)>)
                                     completion {
  CHECK([NSThread isMainThread]);
  if (_coldSuspended || _coldEvictionInFlight || !_rendererReady ||
      !_renderer.is_bound() || _terminalHostAttachInFlight ||
      _terminalHostInputCutoverId != cutoverId || cutoverId == 0) {
    if (completion) {
      std::move(completion).Run(false, "renderer is not ready for direct host");
    }
    return;
  }
  _terminalHostAttachInFlight = YES;
  _authoritativeTransitionInFlight = YES;
  _authoritativeTransitionEpoch = 0;
  _gridIsAuthoritative = NO;
  [self requeueDurableMouseReleases];
  _expectedFrameWidthPx = 0;
  _expectedFrameHeightPx = 0;
  _renderer->AttachTerminalHost(
      std::move(socket), terminalId, terminalIncarnation, rights, protocolFlags,
      cutoverId,
      base::BindOnce(&cmux::CmuxGhosttyRendererClient::OnTerminalHostAttached,
                     _rendererClient->GetWeakPtr(), cutoverId,
                     std::move(completion)));
}

- (void)cancelTerminalHostInputCutover:(uint64_t)cutoverId
                            completion:(base::OnceClosure)completion {
  CHECK([NSThread isMainThread]);
  if (!_renderer.is_bound() || cutoverId == 0 ||
      _terminalHostInputCutoverId != cutoverId ||
      _terminalHostInputCutoverCancelCompletion) {
    if (completion) {
      std::move(completion).Run();
    }
    return;
  }
  _terminalHostInputCutoverCancelCompletion = std::move(completion);
  _renderer->CancelTerminalHostInputCutover(cutoverId);
}

- (void)detachTerminalHost {
  CHECK([NSThread isMainThread]);
  if (_renderer.is_bound() &&
      (_terminalHostInputCutoverId != 0 || _terminalHostAttachInFlight ||
       _directTerminalHostAttached)) {
    _renderer->DetachTerminalHost();
  }
  _terminalHostAttachInFlight = NO;
  _directTerminalHostAttached = NO;
  _terminalHostInputCutoverId = 0;
  _terminalHostInputCutoverReadyCompletion.Reset();
  if (_terminalHostInputCutoverCancelCompletion) {
    std::move(_terminalHostInputCutoverCancelCompletion).Run();
  }
  _terminalHostRequestedColumns = 0;
  _terminalHostRequestedRows = 0;
}

- (void)rendererTerminalHostAttached:(uint64_t)cutoverId
                 rendererIncarnation:(uint64_t)rendererIncarnation
                             success:(BOOL)success {
  CHECK([NSThread isMainThread]);
  // Attach replies and host lifecycle events travel on separate Mojo pipes.
  // Cancellation, disconnection, exit, or a renderer restart may therefore
  // retire this attempt before its response arrives. Always complete the
  // backend callback, but never let that stale response mutate view state.
  if (rendererIncarnation != _rendererIncarnation ||
      cutoverId != _terminalHostInputCutoverId ||
      !_terminalHostAttachInFlight ||
      _terminalHostInputCutoverCancelCompletion) {
    return;
  }
  _terminalHostAttachInFlight = NO;
  _directTerminalHostAttached = success;
  if (success) {
    _terminalHostInputCutoverId = 0;
    // The direct capability has no ViewerSize reservation yet. Force one
    // request even when the current physical bounds map to the Snapshot grid;
    // hidden services retain it and publish it only when made visible.
    _terminalHostRequestedColumns = 0;
    _terminalHostRequestedRows = 0;
    _terminalHostRequestedScale = _sentScale;
    _sentWidthPx = 0;
    _sentHeightPx = 0;
    [self pushSize];
  }
}

- (void)rendererTerminalHostInputCutoverReady:(uint64_t)cutoverId
                                      success:(BOOL)success
                                        error:(std::string)error {
  CHECK([NSThread isMainThread]);
  if (cutoverId != _terminalHostInputCutoverId ||
      !_terminalHostInputCutoverReadyCompletion) {
    return;
  }
  std::move(_terminalHostInputCutoverReadyCompletion)
      .Run(success, std::move(error));
}

- (void)rendererTerminalHostInputCutoverCancelled:(uint64_t)cutoverId
                                           reason:(std::string)reason {
  CHECK([NSThread isMainThread]);
  (void)reason;
  if (cutoverId != _terminalHostInputCutoverId) {
    return;
  }
  _terminalHostAttachInFlight = NO;
  _directTerminalHostAttached = NO;
  _terminalHostInputCutoverId = 0;
  _terminalHostInputCutoverReadyCompletion.Reset();
  // The direct socket suppressed compatibility output while it was present,
  // so its abandoned epoch cannot remain the publication gate. The backend
  // opens a fresh compatibility attachment after this ordered marker, whose
  // Snapshot establishes the next authoritative transition.
  _authoritativeTransitionInFlight = YES;
  _authoritativeTransitionEpoch = 0;
  _gridIsAuthoritative = NO;
  [self requeueDurableMouseReleases];
  _expectedFrameWidthPx = 0;
  _expectedFrameHeightPx = 0;
  _mouseReleaseTokensInFlight.clear();
  if (_physicalResizePending) {
    _physicalResizePending = NO;
    [self pushSize];
  }
  if (_terminalHostInputCutoverCancelCompletion) {
    std::move(_terminalHostInputCutoverCancelCompletion).Run();
  }
}

- (void)rendererTerminalHostDisconnected:(NSString*)reason {
  CHECK([NSThread isMainThread]);
  _terminalHostAttachInFlight = NO;
  _directTerminalHostAttached = NO;
  _terminalHostInputCutoverId = 0;
  _terminalHostInputCutoverReadyCompletion.Reset();
  _terminalHostInputCutoverCancelCompletion.Reset();
  _authoritativeTransitionInFlight = YES;
  _authoritativeTransitionEpoch = 0;
  _gridIsAuthoritative = NO;
  [self requeueDurableMouseReleases];
  _expectedFrameWidthPx = 0;
  _expectedFrameHeightPx = 0;
  _mouseReleaseTokensInFlight.clear();
  _terminalHostRequestedColumns = 0;
  _terminalHostRequestedRows = 0;
  if (self.onTerminalHostDisconnected) {
    self.onTerminalHostDisconnected(reason ?: @"terminal host disconnected");
  }
}

- (void)rendererInitialized:(uint64_t)rendererIncarnation
                    success:(BOOL)success
                      error:(NSString*)error
              backgroundRed:(uint8_t)backgroundRed
            backgroundGreen:(uint8_t)backgroundGreen
             backgroundBlue:(uint8_t)backgroundBlue {
  if (rendererIncarnation != _rendererIncarnation) {
    return;
  }
  if (!success) {
    [self rendererDisconnected:(error.length > 0
                                    ? error
                                    : @"renderer initialization failed")
           rendererIncarnation:rendererIncarnation];
    return;
  }
  _configuredBackgroundRed = backgroundRed;
  _configuredBackgroundGreen = backgroundGreen;
  _configuredBackgroundBlue = backgroundBlue;
  _hasConfiguredBackground = YES;
  [self restoreConfiguredTerminalBackground];
  _rendererReady = YES;
  _renderer->SetVisible(_rendererVisible);
  [self pushSize];
  [self sendPendingAuthoritativeReplay];
  if (_pendingOutput) {
    NSData* output = _pendingOutput;
    _pendingOutput = nil;
    [self processTerminalOutput:output];
  }
  if (self.window && self.window.firstResponder == self) {
    _renderer->SetFocus(true);
  }
  VLOG(1) << "cmux-term: external Ghostty renderer process ready";
}

- (void)setTerminalBackgroundRed:(uint8_t)red
                           green:(uint8_t)green
                            blue:(uint8_t)blue {
  NSColor* color = [NSColor colorWithSRGBRed:red / 255.0
                                       green:green / 255.0
                                        blue:blue / 255.0
                                       alpha:1.0];
  [CATransaction begin];
  [CATransaction setDisableActions:YES];
  self.layer.backgroundColor = color.CGColor;
  [CATransaction commit];
}

- (void)restoreConfiguredTerminalBackground {
  CHECK([NSThread isMainThread]);
  if (!_hasConfiguredBackground) {
    return;
  }
  [self setTerminalBackgroundRed:_configuredBackgroundRed
                           green:_configuredBackgroundGreen
                            blue:_configuredBackgroundBlue];
}

- (void)rendererTerminalHostBackground:(BOOL)hasOverride
                                   red:(uint8_t)red
                                 green:(uint8_t)green
                                  blue:(uint8_t)blue {
  CHECK([NSThread isMainThread]);
  _hasTerminalHostBackgroundOverride = hasOverride;
  if (hasOverride) {
    [self setTerminalBackgroundRed:red green:green blue:blue];
  } else {
    [self restoreConfiguredTerminalBackground];
  }
}

- (void)prepareResolvedThemeBackgroundRed:(uint8_t)red
                                    green:(uint8_t)green
                                     blue:(uint8_t)blue {
  CHECK([NSThread isMainThread]);
  _configuredBackgroundRed = red;
  _configuredBackgroundGreen = green;
  _configuredBackgroundBlue = blue;
  _hasConfiguredBackground = YES;
  if (!_hasTerminalHostBackgroundOverride) {
    [self restoreConfiguredTerminalBackground];
  }
}

- (BOOL)hasVisibleThemeRenderer {
  CHECK([NSThread isMainThread]);
  return _rendererVisible && !_coldSuspended && !_coldEvictionInFlight &&
         _renderer.is_bound();
}

- (BOOL)hasHiddenThemeRenderer {
  CHECK([NSThread isMainThread]);
  return !_rendererVisible && !_coldSuspended && !_coldEvictionInFlight &&
         _renderer.is_bound();
}

- (void)sendResolvedThemeConfig:(const std::string&)themeConfig {
  CHECK([NSThread isMainThread]);
  if (!_coldSuspended && !_coldEvictionInFlight && _renderer.is_bound()) {
    _renderer->UpdateTheme(themeConfig);
  }
}

- (void)setRendererVisible:(BOOL)visible {
  CHECK([NSThread isMainThread]);
  if (_rendererVisible == visible) {
    return;
  }
  _rendererVisible = visible;
  if (_renderer.is_bound() && !_coldEvictionInFlight && !_coldSuspended) {
    _renderer->SetVisible(visible);
  }
  if (!visible) {
    // An NSView-scoped display link intentionally stops while hidden. Open
    // the service's delivery gate now so hidden renderers cannot deadlock.
    [self finishPendingRendererDeliveryAcknowledgment];
  }
}

- (BOOL)canColdEvict {
  CHECK([NSThread isMainThread]);
  // An invisible direct renderer deliberately suppresses frames. Releasing
  // its viewer can still produce an authoritative smaller-grid replay, so do
  // not wait forever for a frame that SetVisible(false) promises not to send.
  // With no deferred semantics, the parsed hidden grid is a sufficient drain
  // boundary; wake will fetch and present a fresh canonical frame.
  const BOOL hiddenGridIsAuthoritative =
      !_rendererVisible && _gridIsAuthoritative;
  return !_coldSuspended && !_coldEvictionInFlight && _rendererReady &&
         _renderer.is_bound() && _gridIsAuthoritative &&
         (!_authoritativeTransitionInFlight || hiddenGridIsAuthoritative) &&
         _deferredSemanticOperations.empty() && !_terminalHostAttachInFlight &&
         !_terminalHostInputCutoverReadyCompletion &&
         !_terminalHostInputCutoverCancelCompletion &&
         _pendingNativeRightMouseDownEvents.count == 0 &&
         _pendingNativeRightMouseUpEvents.count == 0 &&
         !_mouseCapture.HasCapture() && ![self hasMarkedText];
}

- (void)resumeRenderer {
  CHECK([NSThread isMainThread]);
  if (!_coldSuspended) {
    return;
  }
  _coldSuspended = NO;
  [self setupSurface];
}

- (void)prepareForColdEviction:
            (uint64_t)evictionId
                     completion:
                         (base::OnceCallback<void(bool, std::string)>)completion {
  CHECK([NSThread isMainThread]);
  if (evictionId == 0 || ![self canColdEvict]) {
    std::move(completion).Run(false, "renderer is not cold-eviction ready");
    return;
  }
  _coldEvictionId = evictionId;
  _coldEvictionInFlight = YES;
  _coldEvictionCompletion = std::move(completion);
  _renderer->PrepareForColdEviction(evictionId);
}

- (void)completeColdEviction:(uint64_t)evictionId {
  CHECK([NSThread isMainThread]);
  if (evictionId == 0 || evictionId != _coldEvictionId ||
      _coldEvictionInFlight || _coldSuspended) {
    return;
  }
  _coldSuspended = YES;
  [self cancelPendingRendererDeliveryAcknowledgment];
  _rendererVisible = NO;
  _retryScheduled = NO;
  [self requeueDurableMouseReleases];
  _terminalHostAttachInFlight = NO;
  _directTerminalHostAttached = NO;
  _terminalHostInputCutoverId = 0;
  _terminalHostInputCutoverReadyCompletion.Reset();
  _terminalHostInputCutoverCancelCompletion.Reset();
  _rendererReady = NO;
  _mouseReleaseTokensInFlight.clear();
  [self fallbackPendingNativeRightMouseEvents];
  _renderer.reset();
  _rendererClient.reset();
  _pendingReplay = nil;
  _pendingOutput = nil;
  _lastDeliverySequence = 0;
  // The service releases its frame lease while exiting. Drop the layer's
  // retain as well: a cold terminal keeps only its authoritative VT in cmux,
  // not one full-size IOSurface per hidden tab. The exact terminal background
  // remains visible until a fresh renderer publishes its replayed frame.
  [CATransaction begin];
  [CATransaction setDisableActions:YES];
  self.layer.contents = nil;
  [CATransaction commit];
  [self appendResizeTraceEvent:@"contents_cleared"];
  _displayedFrameToken = 0;
  _displayedFrameWidthPx = 0;
  _displayedFrameHeightPx = 0;
  _displayedFrameScale = 0;
  _sentWidthPx = 0;
  _sentHeightPx = 0;
  _expectedFrameWidthPx = 0;
  _expectedFrameHeightPx = 0;
  _sentScale = 0;
  _gridIsAuthoritative = NO;
  _authoritativeTransitionEpoch = 0;
  _authoritativeTransitionInFlight = YES;
}

- (void)rendererColdEvictionReady:(uint64_t)evictionId
              rendererIncarnation:(uint64_t)rendererIncarnation
                           success:(BOOL)success
                             error:(std::string)error {
  CHECK([NSThread isMainThread]);
  if (rendererIncarnation != _rendererIncarnation ||
      evictionId != _coldEvictionId || !_coldEvictionInFlight) {
    return;
  }
  _coldEvictionInFlight = NO;
  base::OnceCallback<void(bool, std::string)> completion =
      std::move(_coldEvictionCompletion);
  if (!success && _renderer.is_bound()) {
    _renderer->SetVisible(_rendererVisible);
    if (self.window && self.window.firstResponder == self) {
      _renderer->SetFocus(true);
    }
    [self pushSize];
    [self flushDeferredSemanticOperations];
  }
  if (completion) {
    std::move(completion).Run(success, std::move(error));
  }
}

- (void)rendererDisconnected:(NSString*)reason
         rendererIncarnation:(uint64_t)rendererIncarnation {
  if (rendererIncarnation != _rendererIncarnation) {
    return;
  }
  if (!_renderer.is_bound() && _retryScheduled) {
    return;
  }
  if (_coldSuspended) {
    return;
  }
  if (_coldEvictionInFlight) {
    _coldEvictionInFlight = NO;
    base::OnceCallback<void(bool, std::string)> completion =
        std::move(_coldEvictionCompletion);
    if (completion) {
      const char* message = reason.UTF8String;
      std::move(completion).Run(
          false, message ? message : "renderer disconnected during eviction");
    }
  }
  LOG(ERROR) << "cmux-term: " << reason.UTF8String;
  [self cancelPendingRendererDeliveryAcknowledgment];
  [self requeueDurableMouseReleases];
  _terminalHostAttachInFlight = NO;
  _directTerminalHostAttached = NO;
  _terminalHostInputCutoverId = 0;
  _terminalHostInputCutoverReadyCompletion.Reset();
  _terminalHostInputCutoverCancelCompletion.Reset();
  _rendererReady = NO;
  _mouseReleaseTokensInFlight.clear();
  [self fallbackPendingNativeRightMouseEvents];
  _renderer.reset();
  _rendererClient.reset();
  _pendingOutput = nil;
  _lastDeliverySequence = 0;
  // The old renderer owns this token and releases it while tearing down. Keep
  // the IOSurface in the layer as a stable crash/restart placeholder.
  _displayedFrameToken = 0;
  _sentWidthPx = 0;
  _sentHeightPx = 0;
  _expectedFrameWidthPx = 0;
  _expectedFrameHeightPx = 0;
  _sentScale = 0;
  _gridIsAuthoritative = NO;
  _authoritativeTransitionEpoch = 0;
  _authoritativeTransitionInFlight = YES;
  if (self.onRendererError) {
    self.onRendererError(reason ?: @"renderer disconnected");
  }
  if (!_retryScheduled) {
    _retryScheduled = YES;
    __weak CmuxGhosttyTerminalView* weakSelf = self;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 500 * NSEC_PER_MSEC),
                   dispatch_get_main_queue(), ^{
                     [weakSelf retrySetupSurface];
                   });
  }
}

- (void)displayRendererSurface:(IOSurfaceRef)surface
           rendererIncarnation:(uint64_t)rendererIncarnation
              deliverySequence:(uint64_t)deliverySequence
                    frameToken:(uint64_t)frameToken
                 geometryEpoch:(uint64_t)geometryEpoch
                       widthPx:(uint32_t)widthPx
                      heightPx:(uint32_t)heightPx
                    colorSpace:
                        (cmux::mojom::TerminalFrameColorSpace)colorSpace {
  const auto release = [&] {
    [self releaseRendererFrame:frameToken
           rendererIncarnation:rendererIncarnation];
  };
  if (rendererIncarnation != _rendererIncarnation) {
    return;
  }
  // The service admits one frame at a time. Hold its gate until the next
  // display tick so all intervening Ghostty completions coalesce off-process
  // instead of flooding Browser's main thread and Core Animation.
  [self scheduleRendererDeliveryAcknowledgment:deliverySequence
                           rendererIncarnation:rendererIncarnation];
  if (!_gridIsAuthoritative || !surface || frameToken == 0 ||
      deliverySequence <= _lastDeliverySequence || !self.layer ||
      geometryEpoch != _geometryEpoch || widthPx == 0 || heightPx == 0 ||
      widthPx != _expectedFrameWidthPx || heightPx != _expectedFrameHeightPx ||
      colorSpace != cmux::mojom::TerminalFrameColorSpace::kDisplayP3) {
    release();
    return;
  }
  if (IOSurfaceGetWidth(surface) != widthPx ||
      IOSurfaceGetHeight(surface) != heightPx ||
      IOSurfaceGetPixelFormat(surface) != cmux::kCmuxBgraPixelFormat) {
    release();
    return;
  }
  _lastDeliverySequence = deliverySequence;
  const uint64_t replacedFrameToken = _displayedFrameToken;
  _displayedFrameToken = frameToken;
  const uint64_t displayedRendererIncarnation = _rendererIncarnation;
  // Present at the scale paired with the accepted canonical geometry. The
  // window may already have moved to a different display while this frame was
  // in flight; using its newer scale would stretch the retained IOSurface.
  const CGFloat scale = std::max<CGFloat>(_canonicalScale, 1.0);
  _displayedFrameWidthPx = widthPx;
  _displayedFrameHeightPx = heightPx;
  _displayedFrameScale = scale;
  __weak CmuxGhosttyTerminalView* weakSelf = self;
  [CATransaction begin];
  [CATransaction setDisableActions:YES];
  if (replacedFrameToken != 0) {
    [CATransaction setCompletionBlock:^{
      [weakSelf releaseRendererFrame:replacedFrameToken
                 rendererIncarnation:displayedRendererIncarnation];
    }];
  }
  self.layer.contentsScale = scale;
  self.layer.contents = (__bridge id)surface;
  [CATransaction commit];
  [self appendResizeTraceEvent:@"frame_presented"];
  if (_authoritativeTransitionInFlight &&
      geometryEpoch == _authoritativeTransitionEpoch) {
    _authoritativeTransitionInFlight = NO;
    _authoritativeTransitionEpoch = 0;
    if (_physicalResizePending) {
      _physicalResizePending = NO;
      // Do not start the next resize merely because the renderer announced
      // its grid. The matching IOSurface is the publication boundary: waiting
      // until it is installed prevents a slow frame from being fenced by the
      // next geometry epoch before it can ever become visible.
      [self pushSize];
    } else {
      [self appendResizeTraceEvent:@"resize_settled"];
    }
  }
  [self replayPendingMouseReleases];
  [self flushDeferredSemanticOperations];
  [self replayPendingMouseReleases];
}

- (void)scheduleRendererDeliveryAcknowledgment:(uint64_t)deliverySequence
                           rendererIncarnation:
                               (uint64_t)rendererIncarnation {
  CHECK([NSThread isMainThread]);
  if (deliverySequence == 0 || rendererIncarnation != _rendererIncarnation ||
      !_renderer.is_bound()) {
    return;
  }
  // The renderer service cannot issue another delivery before this one is
  // acknowledged, so two different pending sequences indicate a protocol
  // violation. Recover by opening the older gate before tracking the newer.
  if (_pendingDeliveryAcknowledgmentSequence != 0) {
    if (_pendingDeliveryAcknowledgmentSequence == deliverySequence &&
        _pendingDeliveryAcknowledgmentRendererIncarnation ==
            rendererIncarnation) {
      return;
    }
    LOG(ERROR) << "cmux-term: renderer delivered a frame before its prior "
                  "display-cadence acknowledgment";
    [self finishPendingRendererDeliveryAcknowledgment];
  }

  _pendingDeliveryAcknowledgmentSequence = deliverySequence;
  _pendingDeliveryAcknowledgmentRendererIncarnation = rendererIncarnation;
  if (!_rendererVisible || !self.window) {
    // View-scoped display links intentionally do not tick while hidden or
    // detached, but the renderer service must never remain delivery-gated.
    [self finishPendingRendererDeliveryAcknowledgment];
    return;
  }

  if (@available(macOS 14.0, *)) {
    if (!_frameDeliveryDisplayLink) {
      _frameDeliveryDisplayLinkTarget =
          [[CmuxFrameDeliveryDisplayLinkTarget alloc] initWithView:self];
      _frameDeliveryDisplayLink = [self
          displayLinkWithTarget:_frameDeliveryDisplayLinkTarget
                       selector:@selector(displayLinkDidFire:)];
      [_frameDeliveryDisplayLink setPaused:YES];
      [_frameDeliveryDisplayLink addToRunLoop:NSRunLoop.mainRunLoop
                                      forMode:NSRunLoopCommonModes];
    }
    [_frameDeliveryDisplayLink setPaused:NO];
    return;
  }

  uint64_t generation = ++_fallbackAcknowledgmentGeneration;
  if (generation == 0) {
    generation = ++_fallbackAcknowledgmentGeneration;
  }
  NSInteger framesPerSecond = self.window.screen.maximumFramesPerSecond;
  if (framesPerSecond <= 0) {
    framesPerSecond = 60;
  }
  const int64_t delay = NSEC_PER_SEC / framesPerSecond;
  __weak CmuxGhosttyTerminalView* weakSelf = self;
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, delay),
                 dispatch_get_main_queue(), ^{
                   CmuxGhosttyTerminalView* strongSelf = weakSelf;
                   if (strongSelf &&
                       strongSelf->_fallbackAcknowledgmentGeneration ==
                           generation) {
                     [strongSelf
                         finishPendingRendererDeliveryAcknowledgment];
                   }
                 });
}

- (void)finishPendingRendererDeliveryAcknowledgment {
  CHECK([NSThread isMainThread]);
  const uint64_t deliverySequence =
      _pendingDeliveryAcknowledgmentSequence;
  const uint64_t rendererIncarnation =
      _pendingDeliveryAcknowledgmentRendererIncarnation;
  _pendingDeliveryAcknowledgmentSequence = 0;
  _pendingDeliveryAcknowledgmentRendererIncarnation = 0;
  if (@available(macOS 14.0, *)) {
    [_frameDeliveryDisplayLink setPaused:YES];
  }
  ++_fallbackAcknowledgmentGeneration;
  [self acknowledgeRendererDelivery:deliverySequence
                 rendererIncarnation:rendererIncarnation];
}

- (void)cancelPendingRendererDeliveryAcknowledgment {
  CHECK([NSThread isMainThread]);
  _pendingDeliveryAcknowledgmentSequence = 0;
  _pendingDeliveryAcknowledgmentRendererIncarnation = 0;
  if (@available(macOS 14.0, *)) {
    [_frameDeliveryDisplayLink setPaused:YES];
  }
  ++_fallbackAcknowledgmentGeneration;
}

- (void)rendererDisplayLinkDidFire:(id)displayLink {
  CHECK([NSThread isMainThread]);
  if (displayLink == _frameDeliveryDisplayLink) {
    [self finishPendingRendererDeliveryAcknowledgment];
  }
}

- (void)acknowledgeRendererDelivery:(uint64_t)deliverySequence
                rendererIncarnation:(uint64_t)rendererIncarnation {
  if (rendererIncarnation == _rendererIncarnation && _renderer.is_bound() &&
      deliverySequence != 0) {
    _renderer->AcknowledgeFrameDelivery(deliverySequence);
  }
}

- (void)releaseRendererFrame:(uint64_t)frameToken
         rendererIncarnation:(uint64_t)rendererIncarnation {
  if (rendererIncarnation == _rendererIncarnation && _renderer.is_bound() &&
      frameToken != 0) {
    _renderer->ReleaseFrame(frameToken);
  }
}

- (void)rendererInput:(NSData*)data {
  if (data.length > 0 && self.onInput) {
    self.onInput(data);
  }
}

- (void)rendererGridColumns:(uint16_t)columns
                       rows:(uint16_t)rows
                    widthPx:(uint32_t)widthPx
                   heightPx:(uint32_t)heightPx
                cellWidthPx:(uint32_t)cellWidthPx
               cellHeightPx:(uint32_t)cellHeightPx
              geometryEpoch:(uint64_t)geometryEpoch {
  const BOOL directHost =
      _terminalHostAttachInFlight || _directTerminalHostAttached;
  const BOOL directInitialGrid = directHost && geometryEpoch > _geometryEpoch;
  const BOOL directRequestedGrid =
      directHost && geometryEpoch == _geometryEpoch &&
      _authoritativeTransitionInFlight &&
      geometryEpoch == _authoritativeTransitionEpoch;
  if (widthPx == 0 || heightPx == 0 || cellWidthPx == 0 || cellHeightPx == 0 ||
      (!directInitialGrid && !directRequestedGrid &&
       (geometryEpoch != _geometryEpoch ||
        (_authoritativeTransitionInFlight &&
         geometryEpoch != _authoritativeTransitionEpoch)))) {
    return;
  }
  if (directInitialGrid) {
    _geometryEpoch = geometryEpoch;
    // An unsolicited direct-host resize (including the initial Snapshot) has
    // no browser-authored transition epoch. Adopt its epoch here, but keep the
    // transition closed until displayRendererSurface installs its exact frame.
    _authoritativeTransitionInFlight = YES;
    _authoritativeTransitionEpoch = geometryEpoch;
  }
  _expectedFrameWidthPx = widthPx;
  _expectedFrameHeightPx = heightPx;
  _columns = std::max<uint16_t>(columns, 1);
  _rows = std::max<uint16_t>(rows, 1);
  _canonicalWidthPx = widthPx;
  _canonicalHeightPx = heightPx;
  _cellWidthPx = cellWidthPx;
  _cellHeightPx = cellHeightPx;
  if (directRequestedGrid && _terminalHostRequestedScale > 0) {
    _canonicalScale = _terminalHostRequestedScale;
  } else {
    // This callback describes the geometry request already sent to the
    // renderer. A window can move between displays while that request is in
    // flight, so pair the frame with the request's scale, not AppKit's newer
    // backing scale.
    _canonicalScale = std::max(_sentScale, 1.0f);
  }
  _gridIsAuthoritative = YES;
  if (self.onGridSize) {
    self.onGridSize(_columns, _rows);
  }
}

- (void)rendererTerminalHostViewerSizeUnchanged:(uint16_t)columns
                                           rows:(uint16_t)rows
                                  geometryEpoch:(uint64_t)geometryEpoch {
  const BOOL currentTransition =
      _directTerminalHostAttached && _gridIsAuthoritative &&
      _authoritativeTransitionInFlight &&
      geometryEpoch == _geometryEpoch &&
      geometryEpoch == _authoritativeTransitionEpoch;
  if (!currentTransition) {
    return;
  }
  if (columns != _columns || rows != _rows || _canonicalWidthPx == 0 ||
      _canonicalHeightPx == 0 || _cellWidthPx == 0 || _cellHeightPx == 0) {
    [self restartRendererWithReason:
              @"terminal-host unchanged resize disagreed with retained grid"];
    return;
  }

  // The service advanced Ghostty's external frame context on the same client
  // pipe but intentionally produced no frame. Keep the retained exact
  // IOSurface and make subsequent ordinary output at this epoch publishable.
  _expectedFrameWidthPx = _canonicalWidthPx;
  _expectedFrameHeightPx = _canonicalHeightPx;
  _authoritativeTransitionInFlight = NO;
  _authoritativeTransitionEpoch = 0;
  if (_physicalResizePending) {
    _physicalResizePending = NO;
    [self pushSize];
  } else {
    [self appendResizeTraceEvent:@"resize_settled"];
  }
  [self replayPendingMouseReleases];
  [self flushDeferredSemanticOperations];
  [self replayPendingMouseReleases];
}

- (void)requeueDurableMouseReleases {
  std::vector<cmux::PendingMouseRelease> stillCaptured;
  for (const cmux::PendingMouseRelease& release : _durableMouseReleases) {
    if (_mouseCapture.HasCapture(static_cast<uint32_t>(release.button))) {
      stillCaptured.push_back(release);
      continue;
    }
    const bool newerReleasePending =
        std::any_of(_pendingMouseReleases.begin(), _pendingMouseReleases.end(),
                    [&release](const cmux::PendingMouseRelease& pending) {
                      return pending.button == release.button;
                    });
    if (!newerReleasePending) {
      _pendingMouseReleases.push_back(release);
    }
  }
  _durableMouseReleases.swap(stillCaptured);
}

- (BOOL)shouldDeferSemanticOperation {
  return _coldSuspended || _coldEvictionInFlight || !_rendererReady ||
         !_renderer.is_bound() || !_gridIsAuthoritative ||
         _authoritativeTransitionInFlight;
}

- (BOOL)enqueueDeferredSemanticOperation:
            (cmux::DeferredSemanticOperation)operation {
  const auto replaceable = [](cmux::DeferredSemanticOperation::Kind kind) {
    return kind == cmux::DeferredSemanticOperation::Kind::kMouseMove ||
           kind == cmux::DeferredSemanticOperation::Kind::kPreedit;
  };
  const bool reservedMouseRelease =
      operation.kind ==
      cmux::DeferredSemanticOperation::Kind::kMouseRelease;
  const size_t operationLimit =
      cmux::kMaxDeferredSemanticOperations +
      (reservedMouseRelease ? cmux::kMaxDeferredMouseReleaseReserve : 0);
  const size_t byteLimit =
      cmux::kMaxDeferredSemanticBytes +
      (reservedMouseRelease
           ? cmux::kMaxDeferredMouseReleaseReserve * sizeof(operation)
           : 0);
  const size_t operationBytes = operation.EstimatedBytes();
  if (!_deferredSemanticOperations.empty() &&
      replaceable(operation.kind) &&
      _deferredSemanticOperations.back().kind == operation.kind) {
    _deferredSemanticBytes -=
        _deferredSemanticOperations.back().EstimatedBytes();
    _deferredSemanticOperations.pop_back();
  }
  while ((_deferredSemanticOperations.size() >= operationLimit ||
          _deferredSemanticBytes + operationBytes > byteLimit) &&
         !_deferredSemanticOperations.empty()) {
    const auto droppable = std::find_if(
        _deferredSemanticOperations.begin(),
        _deferredSemanticOperations.end(),
        [](const cmux::DeferredSemanticOperation& candidate) {
          return candidate.kind ==
                     cmux::DeferredSemanticOperation::Kind::kMouseMove ||
                 candidate.kind ==
                     cmux::DeferredSemanticOperation::Kind::kMouseScroll ||
                 candidate.kind ==
                     cmux::DeferredSemanticOperation::Kind::kPreedit ||
                 candidate.kind ==
                     cmux::DeferredSemanticOperation::Kind::kCopy;
        });
    if (droppable == _deferredSemanticOperations.end()) {
      break;
    }
    _deferredSemanticBytes -= droppable->EstimatedBytes();
    _deferredSemanticOperations.erase(droppable);
  }
  if (_deferredSemanticOperations.size() >= operationLimit ||
      _deferredSemanticBytes + operationBytes > byteLimit) {
    LOG(ERROR) << "cmux-terminal: deferred semantic-input queue exceeded its "
                  "limit";
    return NO;
  }
  _deferredSemanticBytes += operationBytes;
  _deferredSemanticOperations.push_back(std::move(operation));
  return YES;
}

- (BOOL)dispatchSemanticOperation:
            (cmux::DeferredSemanticOperation)operation {
  if (!_rendererReady || !_renderer.is_bound()) {
    return [self enqueueDeferredSemanticOperation:std::move(operation)];
  }
  switch (operation.kind) {
    case cmux::DeferredSemanticOperation::Kind::kKey:
      _renderer->SendKey(std::move(operation.key));
      return YES;
    case cmux::DeferredSemanticOperation::Kind::kText:
      _renderer->SendText(std::move(operation.text));
      return YES;
    case cmux::DeferredSemanticOperation::Kind::kPreedit:
      _renderer->SetPreedit(std::move(operation.text));
      return YES;
    case cmux::DeferredSemanticOperation::Kind::kPaste:
      _renderer->Paste(
          std::move(operation.text),
          base::BindOnce([](bool success, const std::string& error) {
            if (!success) {
              LOG(WARNING) << "cmux terminal paste rejected: " << error;
            }
          }));
      return YES;
    case cmux::DeferredSemanticOperation::Kind::kCopy:
      if (_rendererClient) {
        _renderer->CopySelection(
            base::BindOnce(
                &cmux::CmuxGhosttyRendererClient::OnSelectionCopied,
                _rendererClient->GetWeakPtr()));
      }
      return YES;
    case cmux::DeferredSemanticOperation::Kind::kMouseMove:
      _renderer->MouseMove(operation.x, operation.y, operation.mods);
      return YES;
    case cmux::DeferredSemanticOperation::Kind::kMouseButton:
      _renderer->MouseMove(operation.x, operation.y, operation.mods);
      if (operation.token != 0 && _rendererClient) {
        _renderer->MouseButton(
            operation.first, operation.second, operation.mods,
            base::BindOnce(
                &cmux::CmuxGhosttyRendererClient::OnMouseButtonAcknowledged,
                _rendererClient->GetWeakPtr(), operation.token));
      } else {
        _renderer->MouseButton(operation.first, operation.second,
                               operation.mods, base::DoNothing());
      }
      return YES;
    case cmux::DeferredSemanticOperation::Kind::kMouseRelease:
      if (operation.token == 0 ||
          _mouseReleaseTokensInFlight.find(operation.token) !=
              _mouseReleaseTokensInFlight.end()) {
        return YES;
      }
      _mouseReleaseTokensInFlight.insert(operation.token);
      _renderer->MouseMove(operation.x, operation.y, operation.mods);
      _renderer->MouseButton(
          GHOSTTY_MOUSE_RELEASE, operation.first, operation.mods,
          base::BindOnce(
              &cmux::CmuxGhosttyRendererClient::OnMouseReleaseAcknowledged,
              _rendererClient->GetWeakPtr(), operation.token));
      return YES;
    case cmux::DeferredSemanticOperation::Kind::kMouseScroll:
      _renderer->MouseMove(operation.pointer_x, operation.pointer_y,
                           operation.mods);
      _renderer->MouseScroll(operation.x, operation.y, operation.first);
      return YES;
  }
  return NO;
}

- (BOOL)submitSemanticOperation:(cmux::DeferredSemanticOperation)operation {
  if ([self shouldDeferSemanticOperation]) {
    return [self enqueueDeferredSemanticOperation:std::move(operation)];
  }
  return [self dispatchSemanticOperation:std::move(operation)];
}

- (void)flushDeferredSemanticOperations {
  if ([self shouldDeferSemanticOperation]) {
    return;
  }
  while (!_deferredSemanticOperations.empty()) {
    cmux::DeferredSemanticOperation operation =
        std::move(_deferredSemanticOperations.front());
    _deferredSemanticBytes -= operation.EstimatedBytes();
    _deferredSemanticOperations.pop_front();
    [self dispatchSemanticOperation:std::move(operation)];
    if ([self shouldDeferSemanticOperation]) {
      return;
    }
  }
}

- (void)replayPendingMouseReleases {
  if ([self shouldDeferSemanticOperation] ||
      _pendingMouseReleases.empty()) {
    return;
  }
  // Put every physical release on this renderer's ordered Mojo pipe
  // immediately. Acknowledgments prove only acceptance; they must not gate the
  // next release, because doing so could let a later press overtake it.
  for (const cmux::PendingMouseRelease& release : _pendingMouseReleases) {
    const bool deferred = std::any_of(
        _deferredSemanticOperations.begin(),
        _deferredSemanticOperations.end(),
        [&release](const cmux::DeferredSemanticOperation& operation) {
          return operation.kind ==
                     cmux::DeferredSemanticOperation::Kind::kMouseRelease &&
                 operation.token == release.token;
        });
    if (_mouseCapture.HasCapture(static_cast<uint32_t>(release.button)) ||
        deferred ||
        _mouseReleaseTokensInFlight.find(release.token) !=
            _mouseReleaseTokensInFlight.end()) {
      continue;
    }
    _mouseReleaseTokensInFlight.insert(release.token);
    _renderer->MouseMove(release.x, release.y, release.mods);
    _renderer->MouseButton(
        GHOSTTY_MOUSE_RELEASE, release.button, release.mods,
        base::BindOnce(
            &cmux::CmuxGhosttyRendererClient::OnMouseReleaseAcknowledged,
            _rendererClient->GetWeakPtr(), release.token));
  }
}

- (void)mouseReleaseAcknowledged:(uint64_t)token
             rendererIncarnation:(uint64_t)rendererIncarnation
                        accepted:(BOOL)accepted
                        consumed:(BOOL)consumed {
  if (rendererIncarnation != _rendererIncarnation) {
    return;
  }
  const auto acknowledged =
      std::find_if(_pendingMouseReleases.begin(), _pendingMouseReleases.end(),
                   [token](const cmux::PendingMouseRelease& release) {
                     return release.token == token;
                   });
  if (acknowledged == _pendingMouseReleases.end()) {
    return;
  }
  if (_mouseReleaseTokensInFlight.erase(token) == 0) {
    return;
  }
  NSNumber* nativeKey = @(token);
  NSEvent* nativeRightMouseUp = _pendingNativeRightMouseUpEvents[nativeKey];
  [_pendingNativeRightMouseUpEvents removeObjectForKey:nativeKey];
  if (accepted && _gridIsAuthoritative) {
    const cmux::PendingMouseRelease release = *acknowledged;
    _pendingMouseReleases.erase(acknowledged);
    // A release is idempotent. Retain the last acknowledged release per
    // button as a tombstone; if this renderer/direct socket disappears before
    // downstream delivery is provable, the replacement safely replays it.
    // Responses are token-matched and may arrive independently, so an older
    // acknowledgment must never replace a newer same-button tombstone.
    const auto durable =
        std::find_if(_durableMouseReleases.begin(), _durableMouseReleases.end(),
                     [&release](const cmux::PendingMouseRelease& candidate) {
                       return candidate.button == release.button;
                     });
    if (durable == _durableMouseReleases.end()) {
      _durableMouseReleases.push_back(release);
    } else if (durable->token < release.token) {
      *durable = release;
    }
  }
  if (nativeRightMouseUp && (!accepted || !consumed)) {
    [super rightMouseUp:nativeRightMouseUp];
  }
  if (!accepted) {
    return;
  }
  [self replayPendingMouseReleases];
}

- (void)fallbackPendingNativeRightMouseEvents {
  std::unordered_set<uint64_t> downTokens;
  for (NSNumber* token in _pendingNativeRightMouseDownEvents) {
    downTokens.insert(token.unsignedLongLongValue);
  }
  std::unordered_set<uint64_t> upTokens;
  for (NSNumber* token in _pendingNativeRightMouseUpEvents) {
    upTokens.insert(token.unsignedLongLongValue);
  }
  for (auto operation = _deferredSemanticOperations.begin();
       operation != _deferredSemanticOperations.end();) {
    const bool nativeDown =
        operation->kind ==
            cmux::DeferredSemanticOperation::Kind::kMouseButton &&
        downTokens.contains(operation->token);
    const bool nativeUp =
        operation->kind ==
            cmux::DeferredSemanticOperation::Kind::kMouseRelease &&
        upTokens.contains(operation->token);
    if (!nativeDown && !nativeUp) {
      ++operation;
      continue;
    }
    _deferredSemanticBytes -= operation->EstimatedBytes();
    operation = _deferredSemanticOperations.erase(operation);
  }
  _pendingMouseReleases.erase(
      std::remove_if(
          _pendingMouseReleases.begin(), _pendingMouseReleases.end(),
          [&upTokens](const cmux::PendingMouseRelease& release) {
            return upTokens.contains(release.token);
          }),
      _pendingMouseReleases.end());
  for (uint64_t token : upTokens) {
    _mouseReleaseTokensInFlight.erase(token);
  }

  NSMutableArray<NSEvent*>* events = [[NSMutableArray alloc] init];
  [events addObjectsFromArray:_pendingNativeRightMouseDownEvents.allValues];
  [events addObjectsFromArray:_pendingNativeRightMouseUpEvents.allValues];
  [events sortUsingComparator:^NSComparisonResult(NSEvent* lhs, NSEvent* rhs) {
    if (lhs.timestamp < rhs.timestamp) {
      return NSOrderedAscending;
    }
    if (lhs.timestamp > rhs.timestamp) {
      return NSOrderedDescending;
    }
    if (lhs.eventNumber < rhs.eventNumber) {
      return NSOrderedAscending;
    }
    if (lhs.eventNumber > rhs.eventNumber) {
      return NSOrderedDescending;
    }
    return NSOrderedSame;
  }];
  [_pendingNativeRightMouseDownEvents removeAllObjects];
  [_pendingNativeRightMouseUpEvents removeAllObjects];
  if (_mouseCapture.HasCapture(GHOSTTY_MOUSE_RIGHT)) {
    CHECK(_mouseCapture.End(GHOSTTY_MOUSE_RIGHT));
    if (!_mouseCapture.HasCapture()) {
      _mouseCaptureRendererIncarnation = 0;
    }
  }
  if (events.count != 0) {
    const bool hasPendingRightRelease = std::any_of(
        _pendingMouseReleases.begin(), _pendingMouseReleases.end(),
        [](const cmux::PendingMouseRelease& release) {
          return release.button == GHOSTTY_MOUSE_RIGHT;
        });
    if (!hasPendingRightRelease) {
      uint64_t token = ++_nextMouseReleaseToken;
      if (token == 0) {
        token = ++_nextMouseReleaseToken;
      }
      NSEvent* lastEvent = events.lastObject;
      const NSPoint point = [self surfacePoint:lastEvent];
      _pendingMouseReleases.push_back(
          {token, static_cast<float>(point.x), static_cast<float>(point.y),
           GHOSTTY_MOUSE_RIGHT, cmux::ModsFromEvent(lastEvent)});
    }
  }
  if (events.count == 0) {
    return;
  }
  __weak CmuxGhosttyTerminalView* weakSelf = self;
  dispatch_async(dispatch_get_main_queue(), ^{
    [weakSelf replayNativeRightMouseEvents:events];
  });
}

- (void)replayNativeRightMouseEvents:(NSArray<NSEvent*>*)events {
  for (NSEvent* event in events) {
    if (event.type == NSEventTypeRightMouseDown) {
      [super rightMouseDown:event];
    } else if (event.type == NSEventTypeRightMouseUp) {
      [super rightMouseUp:event];
    }
  }
}

- (void)mouseButtonAcknowledged:(uint64_t)token
            rendererIncarnation:(uint64_t)rendererIncarnation
                       accepted:(BOOL)accepted
                       consumed:(BOOL)consumed {
  if (rendererIncarnation != _rendererIncarnation || token == 0) {
    return;
  }
  NSNumber* key = @(token);
  NSEvent* event = _pendingNativeRightMouseDownEvents[key];
  if (!event) {
    return;
  }
  [_pendingNativeRightMouseDownEvents removeObjectForKey:key];
  if (accepted && consumed) {
    return;
  }

  // Ghostty records button state even when the application does not consume
  // a right-click. Balance that state before entering AppKit's native menu
  // tracking, which may consume the physical mouse-up and would otherwise
  // strand Browser's capture journal.
  _synthesizingRightMouseRelease = YES;
  [self sendMouseButton:event
                  state:GHOSTTY_MOUSE_RELEASE
                 button:GHOSTTY_MOUSE_RIGHT];
  _synthesizingRightMouseRelease = NO;
  [super rightMouseDown:event];
}

- (void)rendererClose:(BOOL)processAlive
    rendererIncarnation:(uint64_t)rendererIncarnation {
  if (rendererIncarnation != _rendererIncarnation) {
    return;
  }
  if (self.onCloseSurface) {
    self.onCloseSurface(processAlive);
  }
}

// MARK: geometry

- (BOOL)isFlipped {
  return YES;
}

- (void)appendResizeTraceEvent:(NSString*)event {
  if (_resizeTracePath.length == 0 || _resizeTraceViewIdentifier.length == 0) {
    return;
  }
  CALayer* layer = self.layer;
  const CGFloat scale = self.window ? self.window.backingScaleFactor : 2.0;
  const NSSize size = self.bounds.size;
  const uint32_t boundsWidth =
      std::max<uint32_t>(static_cast<uint32_t>(size.width * scale), 1);
  const uint32_t boundsHeight =
      std::max<uint32_t>(static_cast<uint32_t>(size.height * scale), 1);
  NSString* expectedGravity = [layer contentsAreFlipped]
                                  ? kCAGravityBottomLeft
                                  : kCAGravityTopLeft;
  const BOOL anchored =
      layer && [layer.contentsGravity isEqualToString:expectedGravity];
  const BOOL contentsPresent =
      layer && layer.contents != nil && _displayedFrameToken != 0;
  const BOOL scaleStable =
      layer && _displayedFrameScale > 0 &&
      std::abs(layer.contentsScale - _displayedFrameScale) < 0.001;
  const CATransform3D transform =
      layer ? layer.transform : CATransform3DIdentity;
  const BOOL transformIdentity = layer && CATransform3DIsIdentity(transform);
  NSString* line = [NSString
      stringWithFormat:
          @"view=%@ sequence=%llu geometry_epoch=%llu event=%@ live=%d "
          @"bounds_width=%u "
          @"bounds_height=%u displayed_width=%u displayed_height=%u "
          @"layer_width=%.3f layer_height=%.3f contents_scale=%.6f "
          @"displayed_scale=%.6f transform_m11=%.6f transform_m12=%.6f "
          @"transform_m21=%.6f transform_m22=%.6f transform_m41=%.6f "
          @"transform_m42=%.6f transform_identity=%d columns=%u rows=%u "
          @"contents=%d anchored=%d scale_stable=%d\n",
          _resizeTraceViewIdentifier,
          static_cast<unsigned long long>(++_resizeTraceSequence),
          static_cast<unsigned long long>(_geometryEpoch), event,
          self.inLiveResize ? 1 : 0, boundsWidth, boundsHeight,
          _displayedFrameWidthPx, _displayedFrameHeightPx,
          static_cast<double>(layer.bounds.size.width),
          static_cast<double>(layer.bounds.size.height),
          static_cast<double>(layer.contentsScale),
          static_cast<double>(_displayedFrameScale),
          static_cast<double>(transform.m11),
          static_cast<double>(transform.m12),
          static_cast<double>(transform.m21),
          static_cast<double>(transform.m22),
          static_cast<double>(transform.m41),
          static_cast<double>(transform.m42), transformIdentity ? 1 : 0,
          _columns, _rows,
          contentsPresent ? 1 : 0, anchored ? 1 : 0,
          scaleStable ? 1 : 0];
  CmuxAppendTerminalResizeTrace(_resizeTraceDescriptor, line);
}

- (void)pushSize {
  if (!_renderer.is_bound()) {
    return;
  }
  const CGFloat scale = self.window ? self.window.backingScaleFactor : 2.0;
  const NSSize size = self.bounds.size;
  _pendingWidthPx =
      std::max<uint32_t>(static_cast<uint32_t>(size.width * scale), 1);
  _pendingHeightPx =
      std::max<uint32_t>(static_cast<uint32_t>(size.height * scale), 1);
  _pendingScale = scale;
  if (_authoritativeTransitionInFlight) {
    _physicalResizePending = YES;
    return;
  }
  if (_resizeScheduled) {
    return;
  }
  _resizeScheduled = YES;
  __weak CmuxGhosttyTerminalView* weakSelf = self;
  dispatch_after(
      dispatch_time(DISPATCH_TIME_NOW, 16 * NSEC_PER_MSEC),
      dispatch_get_main_queue(), ^{
        CmuxGhosttyTerminalView* strongSelf = weakSelf;
        if (!strongSelf) {
          return;
        }
        strongSelf->_resizeScheduled = NO;
        if (strongSelf->_coldEvictionInFlight || strongSelf->_coldSuspended) {
          strongSelf->_physicalResizePending = YES;
          return;
        }
        if (strongSelf->_authoritativeTransitionInFlight) {
          strongSelf->_physicalResizePending = YES;
          return;
        }
        if (!strongSelf->_renderer.is_bound() || !strongSelf->_rendererReady) {
          return;
        }
        if (strongSelf->_pendingWidthPx == strongSelf->_sentWidthPx &&
            strongSelf->_pendingHeightPx == strongSelf->_sentHeightPx &&
            strongSelf->_pendingScale == strongSelf->_sentScale) {
          if (strongSelf->_directTerminalHostAttached &&
              strongSelf->_gridIsAuthoritative) {
            [strongSelf appendResizeTraceEvent:@"resize_settled"];
          }
          return;
        }
        if (strongSelf->_directTerminalHostAttached) {
          if (strongSelf->_cellWidthPx == 0 || strongSelf->_cellHeightPx == 0 ||
              strongSelf->_canonicalWidthPx == 0 ||
              strongSelf->_canonicalHeightPx == 0) {
            strongSelf->_physicalResizePending = YES;
            return;
          }
          const double canonicalScale =
              std::max<double>(strongSelf->_canonicalScale, 1.0);
          const double scaleRatio =
              std::max<double>(strongSelf->_pendingScale, 1.0) / canonicalScale;
          const uint64_t canonicalGridWidth =
              static_cast<uint64_t>(strongSelf->_columns) *
              strongSelf->_cellWidthPx;
          const uint64_t canonicalGridHeight =
              static_cast<uint64_t>(strongSelf->_rows) *
              strongSelf->_cellHeightPx;
          const uint64_t paddingWidth =
              strongSelf->_canonicalWidthPx > canonicalGridWidth
                  ? strongSelf->_canonicalWidthPx - canonicalGridWidth
                  : 0;
          const uint64_t paddingHeight =
              strongSelf->_canonicalHeightPx > canonicalGridHeight
                  ? strongSelf->_canonicalHeightPx - canonicalGridHeight
                  : 0;
          const uint64_t scaledCellWidth = std::max<uint64_t>(
              static_cast<uint64_t>(
                  std::llround(strongSelf->_cellWidthPx * scaleRatio)),
              1);
          const uint64_t scaledCellHeight = std::max<uint64_t>(
              static_cast<uint64_t>(
                  std::llround(strongSelf->_cellHeightPx * scaleRatio)),
              1);
          const uint64_t scaledPaddingWidth =
              static_cast<uint64_t>(std::llround(paddingWidth * scaleRatio));
          const uint64_t scaledPaddingHeight =
              static_cast<uint64_t>(std::llround(paddingHeight * scaleRatio));
          const uint64_t availableWidth =
              strongSelf->_pendingWidthPx > scaledPaddingWidth
                  ? strongSelf->_pendingWidthPx - scaledPaddingWidth
                  : 0;
          const uint64_t availableHeight =
              strongSelf->_pendingHeightPx > scaledPaddingHeight
                  ? strongSelf->_pendingHeightPx - scaledPaddingHeight
                  : 0;
          const uint16_t columns = static_cast<uint16_t>(
              std::clamp<uint64_t>(availableWidth / scaledCellWidth, 1,
                                   std::numeric_limits<uint16_t>::max()));
          const uint16_t rows = static_cast<uint16_t>(
              std::clamp<uint64_t>(availableHeight / scaledCellHeight, 1,
                                   std::numeric_limits<uint16_t>::max()));
          // These are the physical bounds we just evaluated, even
          // when the fixed cell grid did not change. Recording
          // them before the early return prevents every AppKit
          // bounds notification from scheduling the same work and
          // gives the first direct replay a current scale fallback.
          strongSelf->_sentWidthPx = strongSelf->_pendingWidthPx;
          strongSelf->_sentHeightPx = strongSelf->_pendingHeightPx;
          strongSelf->_sentScale = strongSelf->_pendingScale;
          if (columns == strongSelf->_terminalHostRequestedColumns &&
              rows == strongSelf->_terminalHostRequestedRows &&
              strongSelf->_pendingScale ==
                  strongSelf->_terminalHostRequestedScale) {
            [strongSelf appendResizeTraceEvent:@"resize_settled"];
            return;
          }
          strongSelf->_terminalHostRequestedColumns = columns;
          strongSelf->_terminalHostRequestedRows = rows;
          strongSelf->_terminalHostRequestedScale = strongSelf->_pendingScale;
          strongSelf->_physicalResizePending = NO;
          strongSelf->_authoritativeTransitionInFlight = YES;
          const uint64_t geometryEpoch = ++strongSelf->_geometryEpoch;
          strongSelf->_authoritativeTransitionEpoch = geometryEpoch;
          strongSelf->_expectedFrameWidthPx = 0;
          strongSelf->_expectedFrameHeightPx = 0;
          strongSelf->_renderer->RequestTerminalHostViewerSize(
              columns, rows, strongSelf->_pendingScale, geometryEpoch);
          return;
        }
        strongSelf->_sentWidthPx = strongSelf->_pendingWidthPx;
        strongSelf->_sentHeightPx = strongSelf->_pendingHeightPx;
        strongSelf->_sentScale = strongSelf->_pendingScale;
        strongSelf->_expectedFrameWidthPx = strongSelf->_sentWidthPx;
        strongSelf->_expectedFrameHeightPx = strongSelf->_sentHeightPx;
        strongSelf->_gridIsAuthoritative = NO;
        const uint64_t geometryEpoch = ++strongSelf->_geometryEpoch;
        strongSelf->_renderer->SetSize(strongSelf->_sentWidthPx,
                                       strongSelf->_sentHeightPx,
                                       strongSelf->_sentScale, geometryEpoch);
      });
}

- (void)setFrameSize:(NSSize)newSize {
  [super setFrameSize:newSize];
  [self appendResizeTraceEvent:@"frame_size"];
  [self pushSize];
}

- (void)viewDidMoveToWindow {
  [super viewDidMoveToWindow];
  if (self.window) {
    [self pushSize];
    // Do NOT grab first responder here: any terminal re-attaching to the window
    // (e.g. on a workspace switch) would steal focus / fire onActivate for the
    // wrong column. The FocusController is the sole authority -- it drives
    // FocusContent (-> makeFirstResponder) for the pane that should be focused.
  } else {
    // The display link does not fire while detached. Flush (rather than
    // cancel) because the renderer service remains alive and delivery-gated.
    [self finishPendingRendererDeliveryAcknowledgment];
  }
}

- (void)viewDidChangeBackingProperties {
  [super viewDidChangeBackingProperties];
  if (_displayedFrameScale > 0) {
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    // Keep the retained IOSurface pixel/cell stable until a frame rendered for
    // the new backing scale is accepted. Updating this to the window's scale
    // would visibly stretch the old frame during every display transition.
    self.layer.contentsScale = _displayedFrameScale;
    [CATransaction commit];
  }
  [self pushSize];
}

// MARK: focus + input

- (BOOL)acceptsFirstResponder {
  return YES;
}

- (BOOL)becomeFirstResponder {
  if (_renderer.is_bound() && !_coldEvictionInFlight && !_coldSuspended)
    _renderer->SetFocus(true);
  if (self.onActivate)
    self.onActivate();
  return [super becomeFirstResponder];
}

- (BOOL)resignFirstResponder {
  if (_renderer.is_bound() && !_coldEvictionInFlight && !_coldSuspended)
    _renderer->SetFocus(false);
  return [super resignFirstResponder];
}

- (void)keyDown:(NSEvent*)event {
  // Native hosted views bypass the root Views pre-target handler. Report
  // every direct key interaction, not only the first becomeFirstResponder,
  // so a deferred cross-frontend focus handoff cannot overwrite the user.
  if (self.onInteraction) self.onInteraction();
  const BOOL markedBefore = [self hasMarkedText];
  _keyCommandHandled = NO;
  _keyTextAccumulator = [NSMutableArray array];
  [self interpretKeyEvents:@[ event ]];
  const BOOL commandHandled = _keyCommandHandled;
  _keyCommandHandled = NO;
  NSArray<NSString*>* committed = [_keyTextAccumulator copy];
  _keyTextAccumulator = nil;
  [self syncTerminalPreeditClearingIfEmpty:markedBefore];

  if (commandHandled) {
    return;
  }

  if (markedBefore && committed.count > 0) {
    for (NSString* text in committed) {
      [self sendCommittedText:text suppressComposingControls:YES];
    }
    return;
  }

  // Most layout translation also arrives through insertText. Preserve the
  // physical key path (Kitty keyboard protocol, modifiers, keybindings) when
  // AppKit committed exactly the event's ordinary characters. A distinct or
  // multi-stage IME commit uses Ghostty's text-input API instead.
  if (committed.count > 0) {
    NSString* interpreted = [committed componentsJoinedByString:@""];
    NSString* eventText = event.characters ?: @"";
    if (![interpreted isEqualToString:eventText]) {
      [self sendCommittedText:interpreted suppressComposingControls:NO];
      return;
    }
  }
  [self sendKey:event
         action:GHOSTTY_ACTION_PRESS
      composing:[self hasMarkedText] || markedBefore];
}

- (void)keyUp:(NSEvent*)event {
  [self sendKey:event action:GHOSTTY_ACTION_RELEASE composing:NO];
}

- (void)sendKey:(NSEvent*)event
         action:(ghostty_input_action_e)action
      composing:(BOOL)composing {
  // Compute the text exactly like Ghostty's NSEvent.ghosttyCharacters:
  // libghostty does its own control- and modifier-encoding, so we must hand it
  // the BASE character, not the OS-translated control byte.
  NSString* textStr = nil;
  if (action == GHOSTTY_ACTION_PRESS) {
    NSString* chars = event.characters;
    if (chars.length == 1) {
      unichar ch = [chars characterAtIndex:0];
      if (ch < 0x20) {
        // Single control char (e.g. Ctrl-C -> 0x03): pass the char WITHOUT
        // control so libghostty's encoder maps it to the raw control byte. If
        // we pass the control byte itself, with the Kitty keyboard protocol
        // active Ghostty re-encodes it as a CSI-u escape (^[[3;5u) that never
        // triggers SIGINT.
        textStr =
            [event charactersByApplyingModifiers:(event.modifierFlags &
                                                  ~NSEventModifierFlagControl)];
      } else if (ch >= 0xF700 && ch <= 0xF8FF) {
        textStr = nil;  // PUA function key (arrows/F-keys): send no text
      } else {
        textStr = chars;
      }
    } else if (chars.length > 0) {
      textStr = chars;  // multi-codepoint (IME-committed text, emoji, etc.)
    }
  }
  auto key = cmux::mojom::TerminalKeyEvent::New();
  key->action = (action == GHOSTTY_ACTION_PRESS && event.isARepeat)
                    ? GHOSTTY_ACTION_REPEAT
                    : action;
  key->mods = cmux::ModsFromEvent(event);
  key->consumed_mods = cmux::ConsumedModsFromEvent(event);
  key->keycode = event.keyCode;
  key->text = textStr.length ? textStr.UTF8String : "";
  // Unshifted codepoint: the char with NO modifiers. Use byApplyingModifiers:0
  // (not charactersIgnoringModifiers, which misbehaves when control is held).
  NSString* unshifted = [event charactersByApplyingModifiers:0];
  key->unshifted_codepoint =
      unshifted.length > 0 ? [unshifted characterAtIndex:0] : 0;
  key->composing = composing;
  cmux::DeferredSemanticOperation operation;
  operation.kind = cmux::DeferredSemanticOperation::Kind::kKey;
  operation.key = std::move(key);
  [self submitSemanticOperation:std::move(operation)];
}

- (void)sendCommittedText:(NSString*)text
    suppressComposingControls:(BOOL)suppressControls {
  if (text.length == 0) {
    return;
  }
  if (suppressControls && text.length == 1 &&
      [text characterAtIndex:0] < 0x20) {
    return;
  }
  cmux::DeferredSemanticOperation operation;
  operation.kind = cmux::DeferredSemanticOperation::Kind::kText;
  operation.text = text.UTF8String ?: "";
  [self submitSemanticOperation:std::move(operation)];
}

- (void)syncTerminalPreeditClearingIfEmpty:(BOOL)clearIfEmpty {
  cmux::DeferredSemanticOperation operation;
  operation.kind = cmux::DeferredSemanticOperation::Kind::kPreedit;
  if (_markedText.length > 0) {
    operation.text = _markedText.string.UTF8String ?: "";
  } else if (clearIfEmpty) {
    operation.text.clear();
  } else {
    return;
  }
  [self submitSemanticOperation:std::move(operation)];
}

// MARK: NSTextInputClient

- (BOOL)hasMarkedText {
  return _markedText.length > 0;
}

- (NSRange)markedRange {
  return _markedText.length > 0 ? NSMakeRange(0, _markedText.length)
                                : NSMakeRange(NSNotFound, 0);
}

- (NSRange)selectedRange {
  return NSMakeRange(0, 0);
}

- (void)setMarkedText:(id)string
        selectedRange:(NSRange)selectedRange
     replacementRange:(NSRange)replacementRange {
  (void)selectedRange;
  (void)replacementRange;
  if ([string isKindOfClass:[NSAttributedString class]]) {
    _markedText = [[NSMutableAttributedString alloc]
        initWithAttributedString:(NSAttributedString*)string];
  } else if ([string isKindOfClass:[NSString class]]) {
    _markedText =
        [[NSMutableAttributedString alloc] initWithString:(NSString*)string];
  } else {
    return;
  }
  if (!_keyTextAccumulator) {
    [self syncTerminalPreeditClearingIfEmpty:YES];
  }
}

- (void)unmarkText {
  const BOOL hadMarkedText = [self hasMarkedText];
  [_markedText.mutableString setString:@""];
  if (!_keyTextAccumulator) {
    [self syncTerminalPreeditClearingIfEmpty:hadMarkedText];
  }
}

- (void)insertText:(id)string replacementRange:(NSRange)replacementRange {
  (void)replacementRange;
  NSString* text = nil;
  if ([string isKindOfClass:[NSAttributedString class]]) {
    text = ((NSAttributedString*)string).string;
  } else if ([string isKindOfClass:[NSString class]]) {
    text = (NSString*)string;
  }
  if (!text) {
    return;
  }
  // AppKit may hand insertText: a mutable view into the active marked-text
  // storage. Clearing _markedText below can therefore also empty |text|. Take
  // a value copy first, matching Swift String's value semantics in Ghostty's
  // native AppKit frontend, so an ordinary key press cannot turn into an
  // empty commit and suppress its physical key-down event.
  NSString* committedText = [text copy];
  const BOOL hadMarkedText = [self hasMarkedText];
  [_markedText.mutableString setString:@""];
  if (_keyTextAccumulator) {
    [_keyTextAccumulator addObject:committedText];
    return;
  }
  [self syncTerminalPreeditClearingIfEmpty:hadMarkedText];
  [self sendCommittedText:committedText
      suppressComposingControls:hadMarkedText];
}

- (NSArray<NSAttributedStringKey>*)validAttributesForMarkedText {
  return @[];
}

- (NSAttributedString*)attributedSubstringForProposedRange:(NSRange)range
                                               actualRange:
                                                   (NSRangePointer)actualRange {
  if (actualRange) {
    *actualRange = NSMakeRange(NSNotFound, 0);
  }
  return nil;
}

- (NSUInteger)characterIndexForPoint:(NSPoint)point {
  (void)point;
  return NSNotFound;
}

- (NSRect)firstRectForCharacterRange:(NSRange)range
                         actualRange:(NSRangePointer)actualRange {
  if (actualRange) {
    *actualRange = range;
  }
  const NSRect local = NSMakeRect(0, NSMaxY(self.bounds), 0, 0);
  const NSRect inWindow = [self convertRect:local toView:nil];
  return self.window ? [self.window convertRectToScreen:inWindow] : inWindow;
}

- (void)doCommandBySelector:(SEL)selector {
  if (selector == @selector(copy:)) {
    _keyCommandHandled = YES;
    [self copy:nil];
    return;
  }
  if (selector == @selector(cut:)) {
    _keyCommandHandled = YES;
    [self cut:nil];
    return;
  }
  if (selector == @selector(paste:)) {
    _keyCommandHandled = YES;
    [self paste:nil];
    return;
  }
  // keyDown sends the original event after interpretKeyEvents returns. Swallow
  // other AppKit commands here so terminal navigation keys do not beep.
}

- (void)copy:(id)sender {
  (void)sender;
  cmux::DeferredSemanticOperation operation;
  operation.kind = cmux::DeferredSemanticOperation::Kind::kCopy;
  [self submitSemanticOperation:std::move(operation)];
}

- (void)cut:(id)sender {
  // A terminal selection is immutable history; Cut therefore has Copy
  // semantics and never sends a delete sequence to the PTY.
  [self copy:sender];
}

- (void)paste:(id)sender {
  (void)sender;
  NSString* value =
      [NSPasteboard.generalPasteboard stringForType:NSPasteboardTypeString];
  if (value.length > 0) {
    cmux::DeferredSemanticOperation operation;
    operation.kind = cmux::DeferredSemanticOperation::Kind::kPaste;
    operation.text = value.UTF8String ?: "";
    [self submitSemanticOperation:std::move(operation)];
  }
}

- (NSPoint)surfacePoint:(NSEvent*)event {
  NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
  return p;  // isFlipped: origin top-left, matches libghostty
}

- (BOOL)pointIsInsideDisplayedTerminal:(NSPoint)point {
  // A cold renderer retains no IOSurface. Admit its first click against the
  // intersection of the current AppKit viewport and its last authoritative
  // canonical viewport, so smallest-viewer margins cannot become clickable.
  // A never-rendered terminal has no canonical pixel metrics yet and uses its
  // current bounds provisionally. Normal resize transitions retain a real old
  // frame and therefore continue hit-testing only what is visible.
  if (_displayedFrameWidthPx == 0 || _displayedFrameHeightPx == 0 ||
      _displayedFrameScale <= 0) {
    if (!NSPointInRect(point, self.bounds)) {
      return NO;
    }
    if (_canonicalWidthPx == 0 || _canonicalHeightPx == 0 ||
        _canonicalScale <= 0) {
      return YES;
    }
    return cmux::TerminalMouseBounds{
        _canonicalWidthPx, _canonicalHeightPx, _canonicalScale}
        .Contains(point.x, point.y);
  }
  return cmux::TerminalMouseBounds{
      _displayedFrameWidthPx, _displayedFrameHeightPx, _displayedFrameScale}
      .Contains(point.x, point.y);
}

- (void)mouseMoved:(NSEvent*)event {
  NSPoint p = [self surfacePoint:event];
  const BOOL inside = [self pointIsInsideDisplayedTerminal:p];
  cmux::DeferredSemanticOperation operation;
  operation.kind = cmux::DeferredSemanticOperation::Kind::kMouseMove;
  operation.mods = cmux::ModsFromEvent(event);
  if (_mouseCapture.PreserveActualMotion(inside)) {
    operation.x = p.x;
    operation.y = p.y;
  } else {
    // Hover in a resize margin is outside the terminal, not its last cell.
    operation.x = -1;
    operation.y = -1;
  }
  [self submitSemanticOperation:std::move(operation)];
}

- (void)mouseDragged:(NSEvent*)event {
  if (!_mouseCapture.ShouldForwardDrag()) {
    return;
  }
  [self mouseMoved:event];
}

- (void)rightMouseDragged:(NSEvent*)event {
  if (!_mouseCapture.ShouldForwardDrag()) {
    return;
  }
  [self mouseMoved:event];
}

- (void)otherMouseDragged:(NSEvent*)event {
  if (!_mouseCapture.ShouldForwardDrag()) {
    return;
  }
  [self mouseMoved:event];
}

- (BOOL)sendMouseButton:(NSEvent*)event
                  state:(ghostty_input_mouse_state_e)state
                 button:(ghostty_input_mouse_button_e)button {
  NSPoint p = [self surfacePoint:event];
  const uint32_t mods = cmux::ModsFromEvent(event);
  const uint32_t buttonToken = static_cast<uint32_t>(button);
  if (state == GHOSTTY_MOUSE_PRESS) {
    if ([self shouldDeferSemanticOperation]) {
      for (const cmux::PendingMouseRelease& pending :
           _pendingMouseReleases) {
        if (pending.button != static_cast<int32_t>(button)) {
          continue;
        }
        const bool alreadyDeferred = std::any_of(
            _deferredSemanticOperations.begin(),
            _deferredSemanticOperations.end(),
            [&pending](const cmux::DeferredSemanticOperation& operation) {
              return operation.kind == cmux::DeferredSemanticOperation::Kind::
                                           kMouseRelease &&
                     operation.token == pending.token;
            });
        if (alreadyDeferred) {
          continue;
        }
        cmux::DeferredSemanticOperation release;
        release.kind = cmux::DeferredSemanticOperation::Kind::kMouseRelease;
        release.x = pending.x;
        release.y = pending.y;
        release.mods = pending.mods;
        release.first = pending.button;
        release.token = pending.token;
        if (![self submitSemanticOperation:std::move(release)]) {
          // This release was emitted before the deferred suffix and will be
          // replayed ahead of it after recovery. Reject the new press rather
          // than accepting a press whose prerequisite cannot be journaled.
          return NO;
        }
      }
    }
    [self.window makeFirstResponder:self];
    const BOOL inside = [self pointIsInsideDisplayedTerminal:p];
    const BOOL hadCapture = _mouseCapture.HasCapture();
    if (!_mouseCapture.Begin(buttonToken, inside)) {
      return NO;
    }
    cmux::DeferredSemanticOperation press;
    press.kind = cmux::DeferredSemanticOperation::Kind::kMouseButton;
    press.x = p.x;
    press.y = p.y;
    press.first = state;
    press.second = button;
    press.mods = mods;
    if (button == GHOSTTY_MOUSE_RIGHT) {
      uint64_t token = ++_nextMouseButtonToken;
      if (token == 0) {
        token = ++_nextMouseButtonToken;
      }
      press.token = token;
      _pendingNativeRightMouseDownEvents[@(token)] = [event copy];
    }
    if (![self submitSemanticOperation:std::move(press)]) {
      CHECK(_mouseCapture.End(buttonToken));
      if (button == GHOSTTY_MOUSE_RIGHT) {
        [_pendingNativeRightMouseDownEvents
            removeObjectForKey:@(_nextMouseButtonToken)];
      }
      return NO;
    }
    if (!hadCapture) {
      _mouseCaptureRendererIncarnation = _rendererIncarnation;
    }
    return YES;
  }

  if (!_mouseCapture.End(buttonToken)) {
    return NO;
  }
  if (!_mouseCapture.HasCapture()) {
    _mouseCaptureRendererIncarnation = 0;
  }
  if (state == GHOSTTY_MOUSE_RELEASE) {
    uint64_t token = ++_nextMouseReleaseToken;
    if (token == 0) {
      token = ++_nextMouseReleaseToken;
    }
    if (button == GHOSTTY_MOUSE_RIGHT &&
        !_synthesizingRightMouseRelease) {
      _pendingNativeRightMouseUpEvents[@(token)] = [event copy];
    }
    _pendingMouseReleases.push_back({token, static_cast<float>(p.x),
                                     static_cast<float>(p.y),
                                     static_cast<int32_t>(button), mods});
    if ([self shouldDeferSemanticOperation]) {
      cmux::DeferredSemanticOperation release;
      release.kind = cmux::DeferredSemanticOperation::Kind::kMouseRelease;
      release.x = p.x;
      release.y = p.y;
      release.mods = mods;
      release.first = button;
      release.token = token;
      // Every admitted press reserves room for its physical release. This is
      // the state-correctness event that must survive queue pressure.
      CHECK([self submitSemanticOperation:std::move(release)]);
    } else {
      [self replayPendingMouseReleases];
    }
    return YES;
  }
  return NO;
}

- (void)mouseDown:(NSEvent*)event {
  // Repeated clicks do not call becomeFirstResponder again; still report the
  // interaction so it fences any deferred cross-frontend responder change.
  if (self.onInteraction) self.onInteraction();
  [self sendMouseButton:event
                  state:GHOSTTY_MOUSE_PRESS
                 button:GHOSTTY_MOUSE_LEFT];
}

- (void)mouseUp:(NSEvent*)event {
  [self sendMouseButton:event
                  state:GHOSTTY_MOUSE_RELEASE
                 button:GHOSTTY_MOUSE_LEFT];
}

- (void)rightMouseDown:(NSEvent*)event {
  if (self.onInteraction) self.onInteraction();
  if (![self sendMouseButton:event
                       state:GHOSTTY_MOUSE_PRESS
                      button:GHOSTTY_MOUSE_RIGHT]) {
    [super rightMouseDown:event];
  }
}

- (void)rightMouseUp:(NSEvent*)event {
  if (![self sendMouseButton:event
                       state:GHOSTTY_MOUSE_RELEASE
                      button:GHOSTTY_MOUSE_RIGHT]) {
    [super rightMouseUp:event];
  }
}

- (void)otherMouseDown:(NSEvent*)event {
  if (self.onInteraction) self.onInteraction();
  [self sendMouseButton:event
                  state:GHOSTTY_MOUSE_PRESS
                 button:cmux::MouseButtonFromEvent(event)];
}

- (void)otherMouseUp:(NSEvent*)event {
  [self sendMouseButton:event
                  state:GHOSTTY_MOUSE_RELEASE
                 button:cmux::MouseButtonFromEvent(event)];
}

- (void)scrollWheel:(NSEvent*)event {
  if (self.onInteraction) self.onInteraction();
  // Scroll encoding depends on terminal modes restored by the authoritative
  // replay. Do not interpret a wheel event against a replacement renderer's
  // transient empty VT after a crash.
  NSPoint p = [self surfacePoint:event];
  if (![self pointIsInsideDisplayedTerminal:p]) {
    return;
  }
  double x = event.scrollingDeltaX;
  double y = event.scrollingDeltaY;
  if (event.hasPreciseScrollingDeltas) {
    x *= 2;
    y *= 2;
  }
  cmux::DeferredSemanticOperation scroll;
  scroll.kind = cmux::DeferredSemanticOperation::Kind::kMouseScroll;
  scroll.pointer_x = p.x;
  scroll.pointer_y = p.y;
  scroll.mods = cmux::ModsFromEvent(event);
  scroll.x = x;
  scroll.y = y;
  scroll.first = cmux::ScrollModsFromEvent(event);
  [self submitSemanticOperation:std::move(scroll)];
}

- (void)mouseEntered:(NSEvent*)event {
  [self mouseMoved:event];
}

- (void)mouseExited:(NSEvent*)event {
  [self mouseMoved:event];
}

- (void)updateTrackingAreas {
  [super updateTrackingAreas];
  for (NSTrackingArea* area in self.trackingAreas) {
    [self removeTrackingArea:area];
  }
  NSTrackingArea* area = [[NSTrackingArea alloc]
      initWithRect:self.bounds
           options:(NSTrackingActiveAlways | NSTrackingMouseMoved |
                    NSTrackingMouseEnteredAndExited | NSTrackingInVisibleRect)
             owner:self
          userInfo:nil];
  [self addTrackingArea:area];
}

@end

namespace cmux {

void ApplyCmuxGhosttyThemeToAllTerminalViews(const CmuxTheme& theme) {
  CHECK([NSThread isMainThread]);
  const uint64_t generation = ++ThemeBroadcastGeneration();
  const std::string theme_config = SerializeGhosttyThemeColors(theme);
  NSMutableArray<CmuxGhosttyTerminalView*>* hidden_views =
      [[NSMutableArray alloc] init];
  for (CmuxGhosttyTerminalView* view in LiveTerminalViews().allObjects) {
    [view prepareResolvedThemeBackgroundRed:CmuxArgbR(theme.bg)
                                      green:CmuxArgbG(theme.bg)
                                       blue:CmuxArgbB(theme.bg)];
    if ([view hasVisibleThemeRenderer]) {
      [view sendResolvedThemeConfig:theme_config];
    } else if ([view hasHiddenThemeRenderer]) {
      [hidden_views addObject:view];
    }
  }

  // One terminal IPC is cheap, but hundreds in one UI task can still create a
  // visible input hitch. Keep visible terminals synchronous and spread hidden
  // live renderers across small batches. A newer selection invalidates every
  // older pending batch; cold renderers need no message because Initialize
  // reads the latest published selection when they resume.
  constexpr NSUInteger kBatchSize = 8;
  constexpr int64_t kBatchSpacingNanoseconds = 8 * NSEC_PER_MSEC;
  for (NSUInteger start = 0; start < hidden_views.count;
       start += kBatchSize) {
    const NSRange range =
        NSMakeRange(start, std::min(kBatchSize, hidden_views.count - start));
    NSArray<CmuxGhosttyTerminalView*>* batch =
        [hidden_views subarrayWithRange:range];
    const int64_t delay =
        static_cast<int64_t>(start / kBatchSize + 1) *
        kBatchSpacingNanoseconds;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, delay),
                   dispatch_get_main_queue(), ^{
                     if (generation != ThemeBroadcastGeneration()) {
                       return;
                     }
                     for (CmuxGhosttyTerminalView* view in batch) {
                       [view sendResolvedThemeConfig:theme_config];
                     }
                   });
  }
}

}  // namespace cmux
