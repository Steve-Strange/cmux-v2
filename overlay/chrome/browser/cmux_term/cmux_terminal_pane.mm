// Copyright 2026 Manaflow, Inc.
// Portions derived from Helium; Helium contributors retain copyright.
// SPDX-License-Identifier: GPL-3.0-or-later AND GPL-3.0-only

#include "chrome/browser/cmux_term/cmux_terminal_pane.h"

#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>
#include <dispatch/dispatch.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/check.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/strings/string_view_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/time/time.h"
#include "chrome/browser/cmux_term/cmux_tui_protocol.h"
#include "chrome/browser/ui/color/chrome_color_id.h"
#include "ui/accessibility/ax_enums.mojom.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/color/color_provider.h"
#include "ui/gfx/native_ui_types.h"
#include "ui/views/accessibility/view_accessibility.h"
#include "ui/views/background.h"
#include "ui/views/border.h"

namespace cmux {

namespace {

constexpr base::TimeDelta kColdRendererGracePeriod = base::Seconds(30);
constexpr base::TimeDelta kColdRendererEvictionTimeout = base::Seconds(10);

CGPathRef CreateRoundedFramePath(CGRect rect,
                                 CGFloat top_left_radius,
                                 CGFloat top_right_radius,
                                 CGFloat bottom_right_radius,
                                 CGFloat bottom_left_radius,
                                 bool flipped) {
  const CGFloat max_radius =
      std::max<CGFloat>(0, std::min(CGRectGetWidth(rect), CGRectGetHeight(rect)) /
                              2.0);
  const auto radius = [max_radius](CGFloat value) {
    return std::clamp(value, static_cast<CGFloat>(0), max_radius);
  };
  const CGFloat top_left = radius(top_left_radius);
  const CGFloat top_right = radius(top_right_radius);
  const CGFloat bottom_right = radius(bottom_right_radius);
  const CGFloat bottom_left = radius(bottom_left_radius);
  const CGFloat left = CGRectGetMinX(rect);
  const CGFloat right = CGRectGetMaxX(rect);
  const CGFloat top = flipped ? CGRectGetMinY(rect) : CGRectGetMaxY(rect);
  const CGFloat bottom = flipped ? CGRectGetMaxY(rect) : CGRectGetMinY(rect);
  const CGFloat toward_bottom = flipped ? 1.0 : -1.0;

  CGMutablePathRef path = CGPathCreateMutable();
  CGPathMoveToPoint(path, nullptr, left + top_left, top);
  CGPathAddLineToPoint(path, nullptr, right - top_right, top);
  CGPathAddArcToPoint(path, nullptr, right, top, right,
                      top + toward_bottom * top_right, top_right);
  CGPathAddLineToPoint(path, nullptr, right,
                       bottom - toward_bottom * bottom_right);
  CGPathAddArcToPoint(path, nullptr, right, bottom, right - bottom_right,
                      bottom, bottom_right);
  CGPathAddLineToPoint(path, nullptr, left + bottom_left, bottom);
  CGPathAddArcToPoint(path, nullptr, left, bottom, left,
                      bottom - toward_bottom * bottom_left, bottom_left);
  CGPathAddLineToPoint(path, nullptr, left,
                       top + toward_bottom * top_left);
  CGPathAddArcToPoint(path, nullptr, left, top, left + top_left, top, top_left);
  CGPathCloseSubpath(path);
  return path;
}

bool IsSafeHexColor(const std::optional<std::string>& color) {
  if (!color || color->size() != 7 || (*color)[0] != '#') {
    return false;
  }
  for (size_t i = 1; i < color->size(); ++i) {
    if (!std::isxdigit(static_cast<unsigned char>((*color)[i]))) {
      return false;
    }
  }
  return true;
}

std::optional<std::array<uint8_t, 3>> ParseHexColor(
    const std::optional<std::string>& color) {
  if (!IsSafeHexColor(color)) {
    return std::nullopt;
  }
  const auto digit = [](char c) -> uint8_t {
    if (c >= '0' && c <= '9') {
      return static_cast<uint8_t>(c - '0');
    }
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return static_cast<uint8_t>(c - 'a' + 10);
  };
  const auto channel = [&](size_t offset) -> uint8_t {
    return static_cast<uint8_t>((digit((*color)[offset]) << 4) |
                                digit((*color)[offset + 1]));
  };
  return std::array<uint8_t, 3>{channel(1), channel(3), channel(5)};
}

void AppendColorOsc(int code,
                    const std::optional<std::string>& color,
                    std::string* output) {
  if (!output || !IsSafeHexColor(color)) {
    return;
  }
  output->append("\x1b]");
  output->append(std::to_string(code));
  output->push_back(';');
  output->append(*color);
  output->append("\x1b\\");
}

void AppendPaletteOsc(const CmuxTuiColors& colors, std::string* output) {
  if (!output) {
    return;
  }
  for (const auto& [index, color] : colors.palette) {
    const std::optional<std::string> checked_color(color);
    if (!IsSafeHexColor(checked_color)) {
      continue;
    }
    output->append("\x1b]4;");
    output->append(std::to_string(index));
    output->push_back(';');
    output->append(color);
    output->append("\x1b\\");
  }
}

void AppendColorResetOsc(int code, std::string* output) {
  if (!output) {
    return;
  }
  output->append("\x1b]");
  output->append(std::to_string(code));
  output->append("\x1b\\");
}

std::string TerminalColorResetAndApplyMetadata(const CmuxTuiColors& colors) {
  std::string metadata;
  // CmuxTuiColors is a complete sparse state: an absent field means restore
  // Ghostty's configured value, never retain an override from the preceding
  // state. Reset the whole application-authored surface before applying the
  // fields that are present in this generation.
  AppendColorResetOsc(104, &metadata);
  AppendColorResetOsc(110, &metadata);
  AppendColorResetOsc(111, &metadata);
  AppendColorResetOsc(112, &metadata);
  AppendColorResetOsc(117, &metadata);
  AppendColorResetOsc(119, &metadata);
  AppendPaletteOsc(colors, &metadata);
  AppendColorOsc(10, colors.foreground, &metadata);
  AppendColorOsc(11, colors.background, &metadata);
  AppendColorOsc(12, colors.cursor, &metadata);
  AppendColorOsc(17, colors.selection_background, &metadata);
  AppendColorOsc(19, colors.selection_foreground, &metadata);
  if (colors.cursor_style && colors.cursor_blink) {
    // Cursor metadata was added in Colors v2. A legacy v1 event has no
    // opinion about cursor state, so preserve application-authored DECSCUSR
    // and mode 12 instead of resetting them. For v2, reset any prior explicit
    // style before force-applying the authoritative resolved pair.
    metadata.append("\x1b[0 q");
    int style = 0;
    if (*colors.cursor_style == "block") {
      style = *colors.cursor_blink ? 1 : 2;
    } else if (*colors.cursor_style == "underline") {
      style = *colors.cursor_blink ? 3 : 4;
    } else if (*colors.cursor_style == "bar") {
      style = *colors.cursor_blink ? 5 : 6;
    }
    if (style != 0) {
      metadata.append("\x1b[");
      metadata.append(std::to_string(style));
      metadata.append(" q");
    }
  }
  return metadata;
}

}  // namespace

CmuxTerminalSurface::CmuxTerminalSurface(
    scoped_refptr<CmuxTerminalBackend> backend)
    : backend_(std::move(backend)) {
  CHECK(backend_);
  host_ = AddChildView(std::make_unique<views::NativeViewHost>());
  // Focusable so the views FocusManager can route focus to the hosted Ghostty
  // NSView (keeps views focus and AppKit first responder in sync).
  host_->SetFocusBehavior(views::View::FocusBehavior::ALWAYS);
  host_->GetViewAccessibility().SetRole(ax::mojom::Role::kTextField);
  host_->GetViewAccessibility().SetName(u"Terminal",
                                        ax::mojom::NameFrom::kAttribute);
  SetBackground(views::CreateSolidBackground(kColorToolbar));
}

CmuxTerminalSurface::~CmuxTerminalSurface() {
  if (backend_attached_) {
    backend_->DetachFrontend(this);
    backend_attached_ = false;
  }
  if (terminal_view_) {
    terminal_view_.onActivate = nil;
    terminal_view_.onInteraction = nil;
    terminal_view_.onGridSize = nil;
    terminal_view_.onInput = nil;
    terminal_view_.onRendererError = nil;
    terminal_view_.onTerminalHostTitle = nil;
    terminal_view_.onTerminalHostPwd = nil;
    terminal_view_.onTerminalHostBell = nil;
    terminal_view_.onTerminalHostExit = nil;
    terminal_view_.onTerminalHostDisconnected = nil;
  }
  if (host_ && attached_) {
    host_->Detach();
  }

  NSView* __strong clip_view = clip_view_;
  NSView* __strong rounded_view = rounded_view_;
  CmuxGhosttyTerminalView* __strong terminal_view = terminal_view_;
  [terminal_view removeFromSuperview];
  clip_view_ = nil;
  rounded_view_ = nil;
  terminal_view_ = nil;

  if (clip_view || rounded_view || terminal_view) {
    // NativeViewHostMac detach can still be unwinding AppKit bookkeeping while
    // this pane is destroyed. Hold the views only until the next main-loop turn
    // instead of forever; when this block is destroyed the terminal view's
    // dealloc frees only its disposable Ghostty renderer surface; the durable
    // cmux terminal host and PTY remain owned by the backend until Close().
    dispatch_async(dispatch_get_main_queue(), ^{
      (void)clip_view;
      (void)rounded_view;
      (void)terminal_view;
    });
  }
}

// Create + attach the Ghostty NSView only once this surface is in a Widget, so
// NativeViewHost reparents into a real window (attaching with no host window
// tears the process down).
void CmuxTerminalSurface::AddedToWidget() {
  if (attached_) {
    return;
  }
  attached_ = true;
  terminal_view_ = [[CmuxGhosttyTerminalView alloc] initWithFrame:NSZeroRect];
  // Host a clipping container, not the terminal directly: NativeViewHostMac
  // doesn't clip plain NSViews, so a column scrolled under the rail would
  // paint over it. The container is sized to the surface's visible sub-rect
  // and clips the full-size terminal inside it -- see UpdateTerminalClip.
  clip_view_ = [[NSView alloc] initWithFrame:NSZeroRect];
  clip_view_.wantsLayer = YES;
  clip_view_.clipsToBounds = YES;
  clip_view_.layer.masksToBounds = YES;
  rounded_view_ = [[NSView alloc] initWithFrame:NSZeroRect];
  rounded_view_.wantsLayer = YES;
  rounded_view_.clipsToBounds = YES;
  rounded_view_.layer.masksToBounds = YES;
  // Suppress AppKit's native (blue) keyboard focus ring on the hosted NSViews:
  // the pane paints the cmux focus border, so the native rings would stack
  // inside it as extra nested rectangles.
  clip_view_.focusRingType = NSFocusRingTypeNone;
  rounded_view_.focusRingType = NSFocusRingTypeNone;
  terminal_view_.focusRingType = NSFocusRingTypeNone;
  [rounded_view_ addSubview:terminal_view_];
  [clip_view_ addSubview:rounded_view_];
  host_->Attach(gfx::NativeView(clip_view_));
  WireActivation();
  WireInteraction();
  WireCloseRequested();
  UpdateRoundedFrameAppearance();
  UpdateTerminalClip();
  if (!backend_attached_) {
    // Hidden panes are renderer-cold but not lifecycle-cold: register their
    // durable observer and launch/adopt the terminal host immediately.
    AttachBackend();
  }
}

void CmuxTerminalSurface::AttachBackend() {
  if (!terminal_view_) {
    return;
  }
  if (!backend_attached_) {
    scoped_refptr<CmuxTerminalBackend> backend = backend_;
    terminal_view_.onInput = ^(NSData* data) {
      if (!data || data.length == 0) {
        return;
      }
      std::vector<uint8_t> input(data.length);
      [data getBytes:input.data() length:data.length];
      backend->SendInput(base::span(input));
    };
    terminal_view_.onGridSize = ^(uint16_t columns, uint16_t rows) {
      backend->UpdateSize(columns, rows);
    };
    terminal_view_.onRendererError = ^(NSString* reason) {
      const char* text = reason.UTF8String;
      backend->RequestReplay(text ? text : "renderer utility restarted");
    };
    terminal_view_.onTerminalHostTitle = ^(NSString* title) {
      const char* text = title.UTF8String;
      backend->DirectTerminalHostTitle(text ? text : "");
    };
    terminal_view_.onTerminalHostPwd = ^(NSString* pwd) {
      const char* text = pwd.UTF8String;
      backend->DirectTerminalHostPwd(text ? text : "");
    };
    terminal_view_.onTerminalHostBell = ^{
      backend->DirectTerminalHostBell();
    };
    terminal_view_.onTerminalHostExit = ^{
      backend->DirectTerminalHostExited();
    };
    terminal_view_.onTerminalHostDisconnected = ^(NSString* reason) {
      const char* text = reason.UTF8String;
      backend->DirectTerminalHostDisconnected(
          text ? text : "terminal host disconnected");
    };
    backend_attached_ = true;
  }
  const NSSize grid = [terminal_view_ terminalGridSize];
  const uint16_t columns =
      static_cast<uint16_t>(std::clamp<CGFloat>(grid.width, 1, 65535));
  const uint16_t rows =
      static_cast<uint16_t>(std::clamp<CGFloat>(grid.height, 1, 65535));
  if (renderer_residency_ == RendererResidency::kCold) {
    backend_->AttachSuspendedFrontend(this, columns, rows);
  } else {
    backend_->AttachFrontend(this, columns, rows);
  }
}

// Run the pane's activation callback when the Ghostty NSView becomes first
// responder (a click). Captures a copy of the closure -- never `this` -- since
// pane teardown defers final NSView release by one main-loop turn.
void CmuxTerminalSurface::WireActivation() {
  if (!terminal_view_) {
    return;
  }
  base::RepeatingClosure cb = on_activated_;
  terminal_view_.onActivate = ^{
    if (cb) {
      cb.Run();
    }
  };
}

void CmuxTerminalSurface::WireInteraction() {
  if (!terminal_view_) {
    return;
  }
  base::RepeatingClosure cb = on_interaction_;
  terminal_view_.onInteraction = ^{
    if (cb) {
      cb.Run();
    }
  };
}

void CmuxTerminalSurface::WireCloseRequested() {
  if (!terminal_view_) {
    return;
  }
  base::RepeatingClosure cb = on_close_requested_;
  terminal_view_.onCloseSurface = ^(BOOL processAlive) {
    (void)processAlive;
    if (cb) {
      cb.Run();
    }
  };
}

void CmuxTerminalSurface::Layout(PassKey) {
  UpdateTerminalClip();
}

void CmuxTerminalSurface::OnBoundsChanged(const gfx::Rect& previous_bounds) {
  // The strip moves panes by position (scroll) without resizing them, which
  // doesn't trigger Layout(); recompute the clip here so it tracks the scroll.
  UpdateTerminalClip();
}

void CmuxTerminalSurface::VisibilityChanged(views::View* starting_from,
                                            bool is_visible) {
  views::View::VisibilityChanged(starting_from, is_visible);
  UpdateTerminalClip();
}

void CmuxTerminalSurface::OnThemeChanged() {
  views::View::OnThemeChanged();
  UpdateRoundedFrameAppearance();
}

void CmuxTerminalSurface::UpdateTerminalClip() {
  if (!host_ || !clip_view_ || !rounded_view_ || !terminal_view_) {
    return;
  }
  const gfx::Rect content = GetContentsBounds();  // surface-local full content
  // The portion of this surface actually on screen: GetVisibleBounds() clips
  // against every ancestor (the pane bounds and the strip's content area), in
  // local coordinates.
  gfx::Rect visible = GetVisibleBounds();
  visible.Intersect(content);

  host_->SetBoundsRect(visible);  // clip container = only the visible part
  // Full-size terminal inside the (clipping) container, shifted so its content
  // stays put -- only the origin changes on scroll, so Ghostty never reflows.
  const int dx = content.x() - visible.x();  // <= 0 when clipped on the left
  const int dy = content.y() - visible.y();  // <= 0 when clipped on the top
  [rounded_view_
      setFrame:NSMakeRect(dx, dy, content.width(), content.height())];
  [terminal_view_
      setFrame:NSMakeRect(0, 0, content.width(), content.height())];
  UpdateRoundedFrameAppearance();

  effectively_visible_ = !visible.IsEmpty() && IsDrawn();
  [terminal_view_ setRendererVisible:effectively_visible_];
  if (effectively_visible_) {
    cold_renderer_timer_.Stop();
    if (renderer_residency_ == RendererResidency::kCold) {
      WakeColdRenderer();
    }
  } else if (renderer_residency_ == RendererResidency::kHot) {
    ScheduleColdEviction();
  }
  if (backend_attached_) {
    const NSSize grid = [terminal_view_ terminalGridSize];
    backend_->UpdateSize(
        static_cast<uint16_t>(std::clamp<CGFloat>(grid.width, 1, 65535)),
        static_cast<uint16_t>(std::clamp<CGFloat>(grid.height, 1, 65535)));
  }
}

void CmuxTerminalSurface::UpdateRoundedFrameAppearance() {
  if (!rounded_view_) {
    return;
  }
  rounded_view_.layer.cornerRadius = 0;
  rounded_view_.layer.borderWidth = 0;
  rounded_view_.layer.borderColor = nil;
  if (!rounded_frame_geometry_.enabled) {
    rounded_view_.layer.mask = nil;
    rounded_outline_layer_.hidden = YES;
    return;
  }

  if (!rounded_mask_layer_) {
    rounded_mask_layer_ = [CAShapeLayer layer];
    rounded_mask_layer_.fillColor = NSColor.blackColor.CGColor;
    rounded_mask_layer_.actions = @{
      @"frame" : [NSNull null],
      @"path" : [NSNull null],
    };
  }
  if (!rounded_outline_layer_) {
    rounded_outline_layer_ = [CAShapeLayer layer];
    rounded_outline_layer_.fillColor = NSColor.clearColor.CGColor;
    rounded_outline_layer_.lineWidth = kRoundedFrameOutlineThickness;
    rounded_outline_layer_.lineJoin = kCALineJoinRound;
    rounded_outline_layer_.zPosition = 1;
    rounded_outline_layer_.actions = @{
      @"frame" : [NSNull null],
      @"hidden" : [NSNull null],
      @"path" : [NSNull null],
      @"strokeColor" : [NSNull null],
    };
    [rounded_view_.layer addSublayer:rounded_outline_layer_];
  }

  const CGRect bounds = rounded_view_.layer.bounds;
  if (CGRectIsEmpty(bounds)) {
    rounded_mask_layer_.path = nil;
    rounded_outline_layer_.path = nil;
    rounded_outline_layer_.hidden = YES;
    return;
  }
  const bool flipped = rounded_view_.layer.geometryFlipped;
  const CGFloat contents_scale = rounded_view_.layer.contentsScale;
  rounded_mask_layer_.contentsScale = contents_scale;
  rounded_outline_layer_.contentsScale = contents_scale;
  rounded_mask_layer_.frame = bounds;
  CGPathRef mask_path = CreateRoundedFramePath(
      bounds, rounded_frame_geometry_.top_left_radius,
      rounded_frame_geometry_.top_right_radius,
      rounded_frame_geometry_.bottom_right_radius,
      rounded_frame_geometry_.bottom_left_radius, flipped);
  rounded_mask_layer_.path = mask_path;
  CGPathRelease(mask_path);
  rounded_view_.layer.mask = rounded_mask_layer_;

  const CGFloat half_thickness = kRoundedFrameOutlineThickness / 2.0;
  const CGRect outline_bounds =
      CGRectInset(bounds, half_thickness, half_thickness);
  const auto outline_radius = [half_thickness](int value) {
    return std::max<CGFloat>(0, value - half_thickness);
  };
  rounded_outline_layer_.frame = bounds;
  CGPathRef outline_path = CreateRoundedFramePath(
      outline_bounds, outline_radius(rounded_frame_geometry_.top_left_radius),
      outline_radius(rounded_frame_geometry_.top_right_radius),
      outline_radius(rounded_frame_geometry_.bottom_right_radius),
      outline_radius(rounded_frame_geometry_.bottom_left_radius), flipped);
  rounded_outline_layer_.path = outline_path;
  CGPathRelease(outline_path);
  rounded_outline_layer_.hidden = NO;

  if (!GetColorProvider()) {
    rounded_outline_layer_.strokeColor = nil;
    return;
  }
  const SkColor separator =
      GetColorProvider()->GetColor(kColorToolbarContentAreaSeparator);
  rounded_outline_layer_.strokeColor =
      [NSColor colorWithSRGBRed:SkColorGetR(separator) / 255.0
                          green:SkColorGetG(separator) / 255.0
                           blue:SkColorGetB(separator) / 255.0
                          alpha:SkColorGetA(separator) / 255.0]
          .CGColor;
}

void CmuxTerminalSurface::WakeColdRenderer() {
  if (!terminal_view_) {
    return;
  }
  if (renderer_residency_ == RendererResidency::kEvicting) {
    return;
  }
  if (renderer_residency_ != RendererResidency::kCold) {
    return;
  }
  cold_renderer_timer_.Stop();
  renderer_residency_ = RendererResidency::kHot;
  [terminal_view_ setRendererVisible:effectively_visible_];
  [terminal_view_ resumeRenderer];
  AttachBackend();
}

void CmuxTerminalSurface::ScheduleColdEviction() {
  if (effectively_visible_ || renderer_residency_ != RendererResidency::kHot ||
      cold_renderer_timer_.IsRunning()) {
    return;
  }
  cold_renderer_timer_.Start(
      FROM_HERE, kColdRendererGracePeriod,
      base::BindOnce(&CmuxTerminalSurface::BeginColdEviction,
                     weak_factory_.GetWeakPtr()));
}

void CmuxTerminalSurface::BeginColdEviction() {
  if (effectively_visible_ || renderer_residency_ != RendererResidency::kHot ||
      !backend_attached_ ||
      !backend_->CanColdSuspendFrontend(this) ||
      ![terminal_view_ canColdEvict]) {
    ScheduleColdEviction();
    return;
  }
  uint64_t eviction_id = ++cold_eviction_id_;
  if (eviction_id == 0) {
    eviction_id = ++cold_eviction_id_;
  }
  renderer_residency_ = RendererResidency::kEvicting;
  cold_eviction_watchdog_.Start(
      FROM_HERE, kColdRendererEvictionTimeout,
      base::BindOnce(&CmuxTerminalSurface::OnColdEvictionTimeout,
                     weak_factory_.GetWeakPtr(), eviction_id));
  [terminal_view_
      prepareForColdEviction:eviction_id
                   completion:base::BindOnce(
                                  &CmuxTerminalSurface::OnColdEvictionReady,
                                  weak_factory_.GetWeakPtr(), eviction_id)];
}

void CmuxTerminalSurface::OnColdEvictionTimeout(uint64_t eviction_id) {
  if (eviction_id != cold_eviction_id_ ||
      renderer_residency_ != RendererResidency::kEvicting) {
    return;
  }
  ++cold_eviction_id_;  // Fence a late Ready from the wedged incarnation.
  renderer_residency_ = RendererResidency::kHot;
  [terminal_view_
      restartRendererWithReason:@"cold renderer eviction timed out"];
  if (!effectively_visible_) {
    ScheduleColdEviction();
  }
}

void CmuxTerminalSurface::OnColdEvictionReady(uint64_t eviction_id,
                                               bool success,
                                               std::string error) {
  if (eviction_id != cold_eviction_id_ ||
      renderer_residency_ != RendererResidency::kEvicting) {
    return;
  }
  cold_eviction_watchdog_.Stop();
  if (!success || !backend_attached_) {
    renderer_residency_ = RendererResidency::kHot;
    if (!success && !error.empty()) {
      VLOG(1) << "cmux-terminal: cold eviction deferred: " << error;
    }
    if (!effectively_visible_) {
      ScheduleColdEviction();
    }
    return;
  }

  // Ready is the renderer service's ordered commit marker: its direct socket
  // is already drained and released. Do not re-run the preflight predicates
  // here. Input or resize intent that arrived after Prepare remains queued in
  // the backend and is flushed after the next visible attach.
  CHECK(backend_->SuspendFrontend(this));
  [terminal_view_ completeColdEviction:eviction_id];
  renderer_residency_ = RendererResidency::kCold;
  if (effectively_visible_) {
    WakeColdRenderer();
  }
}

views::View* CmuxTerminalSurface::AsView() {
  return this;
}

SurfaceKind CmuxTerminalSurface::kind() const {
  return SurfaceKind::kTerminal;
}

void CmuxTerminalSurface::FocusContent() {
  effectively_visible_ = true;
  WakeColdRenderer();
  if (host_) {
    host_->RequestFocus();
  }
  // Explicitly make the Ghostty NSView the AppKit first responder. Routing
  // focus only through the Views FocusManager delivers TEXT (via the Views
  // input method) but not raw control keys, so Ctrl-C, Escape, arrows, etc.
  // never reach Ghostty's keyDown:. Making it the real first responder routes
  // EVERY key event straight to libghostty -- general across all keys and
  // layouts.
  if (terminal_view_ && terminal_view_.window) {
    [terminal_view_.window makeFirstResponder:terminal_view_];
  }
}

void CmuxTerminalSurface::SetActivationCallback(
    base::RepeatingClosure callback) {
  on_activated_ = std::move(callback);
  WireActivation();  // re-bind the NSView block to the new closure
}

void CmuxTerminalSurface::SetInteractionCallback(base::RepeatingClosure callback) {
  on_interaction_ = std::move(callback);
  WireInteraction();
}

void CmuxTerminalSurface::SetTitleChangedCallback(
    base::RepeatingCallback<void(const std::u16string&)> callback) {
  on_title_changed_ = std::move(callback);
}

void CmuxTerminalSurface::SetCloseRequestedCallback(
    base::RepeatingClosure callback) {
  on_close_requested_ = std::move(callback);
  WireCloseRequested();  // re-bind the NSView block to the new closure
}

void CmuxTerminalSurface::FireCloseRequestedForTesting() {
  if (on_close_requested_) {
    on_close_requested_.Run();
  }
}

void CmuxTerminalSurface::SetRoundedFrame(
    const RoundedFrameGeometry& geometry) {
  if (rounded_frame_geometry_ == geometry) {
    return;
  }
  rounded_frame_geometry_ = geometry;
  SetBorder(
      geometry.enabled
          ? views::CreateEmptyBorder(gfx::Insets::TLBR(
                geometry.top_inset, geometry.left_inset,
                geometry.bottom_inset, geometry.right_inset))
          : nullptr);
  UpdateRoundedFrameAppearance();
  InvalidateLayout();
  UpdateTerminalClip();
  SchedulePaint();
}

void CmuxTerminalSurface::OnCmuxTerminalReplay(
    uint64_t generation,
    uint16_t cols,
    uint16_t rows,
    base::span<const uint8_t> replay,
    const std::optional<CmuxTuiColors>& colors) {
  if (!terminal_view_) {
    return;
  }
  std::vector<uint8_t> filtered =
      StripCmuxTuiReplayPalette(base::as_string_view(replay));
  const CmuxTuiColors default_colors;
  const std::string metadata =
      TerminalColorResetAndApplyMetadata(colors ? *colors : default_colors);
  filtered.insert(filtered.end(), metadata.begin(), metadata.end());
  NSData* data = filtered.empty() ? [NSData data]
                                  : [NSData dataWithBytes:filtered.data()
                                                   length:filtered.size()];
  if (![terminal_view_ replaceTerminalState:data
                       authoritativeColumns:cols
                                       rows:rows
                                 generation:generation]) {
    return;
  }
  // The renderer received these colors in the same replay publication
  // boundary above. Keep the exposed CALayer background in sync without
  // enqueueing a second output operation that could trail the first frame.
  const auto background =
      colors ? ParseHexColor(colors->background) : std::nullopt;
  if (background) {
    [terminal_view_ setTerminalBackgroundRed:(*background)[0]
                                       green:(*background)[1]
                                        blue:(*background)[2]];
  } else {
    [terminal_view_ restoreConfiguredTerminalBackground];
  }
}

void CmuxTerminalSurface::OnCmuxTerminalOutput(
    base::span<const uint8_t> bytes) {
  ProcessTerminalOutputAndColors(bytes, nullptr);
}

void CmuxTerminalSurface::OnCmuxTerminalColors(const CmuxTuiColors& colors) {
  ProcessTerminalOutputAndColors(base::span<const uint8_t>(), &colors);
}

void CmuxTerminalSurface::OnCmuxTerminalTitle(const std::string& title) {
  if (on_title_changed_) {
    on_title_changed_.Run(base::UTF8ToUTF16(title));
  }
}

void CmuxTerminalSurface::OnCmuxTerminalPwd(const std::string& pwd) {
  VLOG(2) << "cmux-term: working directory changed: " << pwd;
}

void CmuxTerminalSurface::OnCmuxTerminalBell() {
  NSBeep();
}

void CmuxTerminalSurface::OnCmuxTerminalExited(const std::string& reason) {
  VLOG(1) << "cmux-term: TUI process exited: " << reason;
  if (on_title_changed_) {
    on_title_changed_.Run(u"Terminal (exited)");
  }
}

void CmuxTerminalSurface::OnCmuxTerminalClosed(const std::string& reason) {
  VLOG(1) << "cmux-term: TUI surface closed: " << reason;
  if (on_close_requested_) {
    on_close_requested_.Run();
  }
}

void CmuxTerminalSurface::BeginCmuxTerminalHostInputCutover(
    uint64_t cutover_id,
    base::OnceCallback<void(bool success, std::string error)> callback) {
  if (!terminal_view_) {
    std::move(callback).Run(false, "terminal view is unavailable");
    return;
  }
  [terminal_view_ beginTerminalHostInputCutover:cutover_id
                                     completion:std::move(callback)];
}

void CmuxTerminalSurface::AttachCmuxTerminalHost(
    uint64_t cutover_id,
    CmuxTuiRendererConnection connection,
    base::OnceCallback<void(bool success, std::string error)> callback) {
  if (!terminal_view_) {
    std::move(callback).Run(false, "terminal view is unavailable");
    return;
  }
  std::vector<uint8_t> terminal_id(connection.terminal_id.begin(),
                                   connection.terminal_id.end());
  std::vector<uint8_t> incarnation(connection.incarnation.begin(),
                                   connection.incarnation.end());
  [terminal_view_
      attachTerminalHostSocket:std::move(connection.socket)
                    terminalId:std::move(terminal_id)
           terminalIncarnation:std::move(incarnation)
                        rights:static_cast<uint32_t>(connection.rights)
                 protocolFlags:connection.protocol_flags
                     cutoverId:cutover_id
                    completion:std::move(callback)];
}

void CmuxTerminalSurface::CancelCmuxTerminalHostInputCutover(
    uint64_t cutover_id,
    base::OnceClosure callback) {
  if (!terminal_view_) {
    std::move(callback).Run();
    return;
  }
  [terminal_view_ cancelTerminalHostInputCutover:cutover_id
                                      completion:std::move(callback)];
}

void CmuxTerminalSurface::DetachCmuxTerminalHost() {
  if (terminal_view_) {
    [terminal_view_ detachTerminalHost];
  }
}

void CmuxTerminalSurface::RestartCmuxTerminalRenderer(
    const std::string& reason) {
  if (!terminal_view_) {
    return;
  }
  NSString* message = [NSString stringWithUTF8String:reason.c_str()];
  [terminal_view_
      restartRendererWithReason:message ?: @"direct renderer attach timed out"];
}

void CmuxTerminalSurface::ProcessTerminalOutputAndColors(
    base::span<const uint8_t> bytes,
    const CmuxTuiColors* colors) {
  if (!terminal_view_ || (bytes.empty() && !colors)) {
    return;
  }
  std::vector<uint8_t> combined(bytes.begin(), bytes.end());
  if (colors) {
    const std::string metadata = TerminalColorResetAndApplyMetadata(*colors);
    combined.insert(combined.end(), metadata.begin(), metadata.end());
  }
  if (colors) {
    const auto background = ParseHexColor(colors->background);
    if (background) {
      [terminal_view_ setTerminalBackgroundRed:(*background)[0]
                                         green:(*background)[1]
                                          blue:(*background)[2]];
    } else {
      [terminal_view_ restoreConfiguredTerminalBackground];
    }
  }
  if (combined.empty()) {
    return;
  }
  NSData* data = [NSData dataWithBytes:combined.data() length:combined.size()];
  if (![terminal_view_ processTerminalOutput:data]) {
    backend_->RequestReplay("macOS Ghostty pending-output cap exceeded");
  }
}

BEGIN_METADATA(CmuxTerminalSurface)
END_METADATA

}  // namespace cmux
