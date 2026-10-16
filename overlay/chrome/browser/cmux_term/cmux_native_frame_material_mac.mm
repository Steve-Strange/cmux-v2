// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later

#import <AppKit/AppKit.h>
#import <objc/runtime.h>

#include "chrome/browser/cmux_term/cmux_native_frame_material.h"

#include "ui/gfx/native_ui_types.h"
#include "ui/views/widget/widget.h"

namespace {

constexpr CGFloat kNeutralWashAlpha = 0.20;
constexpr CGFloat kDarkThemeTintAlpha = 0.55;
constexpr CGFloat kLightThemeTintAlpha = 0.80;
char kCmuxMaterialStateKey;

NSColor* ColorFromSkColor(SkColor color, CGFloat alpha) {
  return [NSColor colorWithSRGBRed:SkColorGetR(color) / 255.0
                             green:SkColorGetG(color) / 255.0
                              blue:SkColorGetB(color) / 255.0
                             alpha:alpha];
}

NSView* MakeTintView(NSRect frame, NSColor* color) {
  NSView* view = [[NSView alloc] initWithFrame:frame];
  view.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
  view.wantsLayer = YES;
  view.layer.backgroundColor = color.CGColor;
  return view;
}

}  // namespace

// A visual-effect background must not steal clicks from Chromium's native or
// Views-hosted content layered above it.
@interface CmuxNativeMaterialView : NSVisualEffectView
@end

@implementation CmuxNativeMaterialView
- (NSView*)hitTest:(NSPoint)point {
  return nil;
}
@end

@interface CmuxNativeMaterialState : NSObject

- (instancetype)initWithWindow:(NSWindow*)window;
- (void)updateDark:(BOOL)isDark tint:(SkColor)tint;
- (void)disableAndRestore;

@end

@implementation CmuxNativeMaterialState {
  __weak NSWindow* _window;
  NSColor* _originalBackgroundColor;
  BOOL _originalOpaque;
  CmuxNativeMaterialView* _materialView;
  BOOL _isDark;
  SkColor _tint;
}

- (instancetype)initWithWindow:(NSWindow*)window {
  self = [super init];
  if (self) {
    _window = window;
    _originalBackgroundColor = window.backgroundColor;
    _originalOpaque = window.opaque;
    [[[NSWorkspace sharedWorkspace] notificationCenter]
        addObserver:self
           selector:@selector(accessibilityDisplayOptionsChanged:)
               name:NSWorkspaceAccessibilityDisplayOptionsDidChangeNotification
             object:nil];
  }
  return self;
}

- (void)dealloc {
  [[[NSWorkspace sharedWorkspace] notificationCenter] removeObserver:self];
}

- (void)updateDark:(BOOL)isDark tint:(SkColor)tint {
  _isDark = isDark;
  _tint = tint;
  [self refresh];
}

- (void)disableAndRestore {
  [_materialView removeFromSuperview];
  _materialView = nil;

  NSWindow* window = _window;
  if (window) {
    window.backgroundColor = _originalBackgroundColor;
    window.opaque = _originalOpaque;
  }
}

- (void)accessibilityDisplayOptionsChanged:(NSNotification*)notification {
  (void)notification;
  [self refresh];
}

- (void)refresh {
  NSWindow* window = _window;
  if (!window) {
    return;
  }

  [_materialView removeFromSuperview];
  _materialView = nil;

  // Honor the system accessibility choice without forgetting the requested
  // theme. If Reduce Transparency is switched off again, the notification
  // restores the material automatically.
  const BOOL reduce_transparency =
      [NSWorkspace sharedWorkspace]
          .accessibilityDisplayShouldReduceTransparency;
  if (reduce_transparency) {
    // Views surfaces are intentionally transparent while this state is
    // installed. Fall back to the requested theme tint, not the pre-cmux
    // window color, so Reduce Transparency never exposes white/undefined
    // pixels behind the toolbar and tabs.
    window.backgroundColor = ColorFromSkColor(_tint, 1.0);
    window.opaque = YES;
    return;
  }

  NSView* contentView = window.contentView;
  if (!contentView) {
    return;
  }

  window.backgroundColor = NSColor.clearColor;
  window.opaque = NO;

  CmuxNativeMaterialView* material =
      [[CmuxNativeMaterialView alloc] initWithFrame:contentView.bounds];
  material.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
  material.blendingMode = NSVisualEffectBlendingModeBehindWindow;
  material.state = NSVisualEffectStateActive;
  material.material =
      _isDark ? NSVisualEffectMaterialSidebar : NSVisualEffectMaterialMenu;
  material.appearance =
      [NSAppearance appearanceNamed:_isDark ? NSAppearanceNameVibrantDark
                                            : NSAppearanceNameVibrantLight];

  const CGFloat neutralComponent = _isDark ? 0.0 : 1.0;
  NSColor* neutralColor = [NSColor colorWithSRGBRed:neutralComponent
                                              green:neutralComponent
                                               blue:neutralComponent
                                              alpha:kNeutralWashAlpha];
  [material addSubview:MakeTintView(material.bounds, neutralColor)];

  const CGFloat tintAlpha =
      _isDark ? kDarkThemeTintAlpha : kLightThemeTintAlpha;
  [material addSubview:MakeTintView(material.bounds,
                                    ColorFromSkColor(_tint, tintAlpha))];

  [contentView addSubview:material positioned:NSWindowBelow relativeTo:nil];
  _materialView = material;
}

@end

namespace cmux {

bool NativeFrameMaterialSupported() {
  return true;
}

void UpdateNativeFrameMaterial(views::Widget* widget,
                               const NativeFrameMaterialParams& params) {
  if (!widget) {
    return;
  }
  NSWindow* window = widget->GetNativeWindow().GetNativeNSWindow();
  if (!window) {
    return;
  }

  CmuxNativeMaterialState* state =
      objc_getAssociatedObject(window, &kCmuxMaterialStateKey);
  if (!state) {
    state = [[CmuxNativeMaterialState alloc] initWithWindow:window];
    objc_setAssociatedObject(window, &kCmuxMaterialStateKey, state,
                             OBJC_ASSOCIATION_RETAIN_NONATOMIC);
  }
  [state updateDark:params.is_dark tint:params.tint];
}

void DisableNativeFrameMaterial(views::Widget* widget) {
  if (!widget) {
    return;
  }
  NSWindow* window = widget->GetNativeWindow().GetNativeNSWindow();
  if (!window) {
    return;
  }

  CmuxNativeMaterialState* state =
      objc_getAssociatedObject(window, &kCmuxMaterialStateKey);
  [state disableAndRestore];
  objc_setAssociatedObject(window, &kCmuxMaterialStateKey, nil,
                           OBJC_ASSOCIATION_RETAIN_NONATOMIC);
}

}  // namespace cmux
