// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#import <AppKit/AppKit.h>

#include "chrome/browser/cmux_term/cmux_app_icon.h"

#include <optional>
#include <string>
#include <string_view>

#include "base/check.h"

namespace {

NSString* const kCmuxAppIconModeKey = @"appIconMode";
void* kCmuxEffectiveAppearanceContext = &kCmuxEffectiveAppearanceContext;

NSImage* IconImageForDarkMode(BOOL dark) {
  NSString* name = dark ? @"AppIconDark" : @"AppIconLight";
  return [NSImage imageNamed:name];
}

BOOL ApplicationUsesDarkAppearance() {
  NSAppearanceName match =
      [NSApp.effectiveAppearance bestMatchFromAppearancesWithNames:@[
        NSAppearanceNameDarkAqua, NSAppearanceNameAqua
      ]];
  return [match isEqualToString:NSAppearanceNameDarkAqua];
}

BOOL ApplyDarkMode(BOOL dark) {
  if (NSImage* image = IconImageForDarkMode(dark)) {
    NSApp.applicationIconImage = image;
    return YES;
  }
  return NO;
}

}  // namespace

@interface CmuxAppIconAppearanceObserver : NSObject

@property(nonatomic, assign) BOOL observing;
@property(nonatomic, assign) NSInteger lastAppliedAppearance;

+ (instancetype)sharedObserver;
- (void)startObserving;
- (void)stopObserving;
- (void)applyCurrentAppearance;

@end

@implementation CmuxAppIconAppearanceObserver

@synthesize observing = _observing;
@synthesize lastAppliedAppearance = _lastAppliedAppearance;

- (instancetype)init {
  self = [super init];
  if (self) {
    _lastAppliedAppearance = -1;
  }
  return self;
}

+ (instancetype)sharedObserver {
  static CmuxAppIconAppearanceObserver* observer;
  static dispatch_once_t once;
  dispatch_once(&once, ^{
    observer = [[CmuxAppIconAppearanceObserver alloc] init];
  });
  return observer;
}

- (void)startObserving {
  [self applyCurrentAppearance];
  if (self.observing) {
    return;
  }
  [NSApp addObserver:self
          forKeyPath:@"effectiveAppearance"
             options:0
             context:kCmuxEffectiveAppearanceContext];
  self.observing = YES;
}

- (void)stopObserving {
  if (!self.observing) {
    return;
  }
  [NSApp removeObserver:self
             forKeyPath:@"effectiveAppearance"
                context:kCmuxEffectiveAppearanceContext];
  self.observing = NO;
  self.lastAppliedAppearance = -1;
}

- (void)applyCurrentAppearance {
  const BOOL dark = ApplicationUsesDarkAppearance();
  const NSInteger appearance = dark ? 1 : 0;
  if (self.lastAppliedAppearance == appearance) {
    return;
  }
  if (ApplyDarkMode(dark)) {
    self.lastAppliedAppearance = appearance;
  }
}

- (void)observeValueForKeyPath:(NSString*)keyPath
                      ofObject:(id)object
                        change:(NSDictionary<NSKeyValueChangeKey, id>*)change
                       context:(void*)context {
  if (context == kCmuxEffectiveAppearanceContext) {
    dispatch_async(dispatch_get_main_queue(), ^{
      if (self.observing) {
        [self applyCurrentAppearance];
      }
    });
    return;
  }
  [super observeValueForKeyPath:keyPath
                       ofObject:object
                         change:change
                        context:context];
}

@end

namespace cmux {

std::string_view AppIconModeToString(AppIconMode mode) {
  switch (mode) {
    case AppIconMode::kAutomatic:
      return "automatic";
    case AppIconMode::kLight:
      return "light";
    case AppIconMode::kDark:
      return "dark";
  }
  return "automatic";
}

std::optional<AppIconMode> AppIconModeFromString(std::string_view value) {
  if (value == "automatic") {
    return AppIconMode::kAutomatic;
  }
  if (value == "light") {
    return AppIconMode::kLight;
  }
  if (value == "dark") {
    return AppIconMode::kDark;
  }
  return std::nullopt;
}

AppIconMode GetAppIconMode() {
  NSString* value =
      [[NSUserDefaults standardUserDefaults] stringForKey:kCmuxAppIconModeKey];
  if (!value) {
    return AppIconMode::kAutomatic;
  }
  std::optional<AppIconMode> mode =
      AppIconModeFromString(value.UTF8String ?: "");
  return mode.value_or(AppIconMode::kAutomatic);
}

void SetAppIconMode(AppIconMode mode) {
  DCHECK([NSThread isMainThread]);
  NSString* value = [NSString
      stringWithUTF8String:std::string(AppIconModeToString(mode)).c_str()];
  [[NSUserDefaults standardUserDefaults] setObject:value
                                            forKey:kCmuxAppIconModeKey];

  CmuxAppIconAppearanceObserver* observer =
      [CmuxAppIconAppearanceObserver sharedObserver];
  switch (mode) {
    case AppIconMode::kAutomatic:
      [observer startObserving];
      break;
    case AppIconMode::kLight:
      [observer stopObserving];
      ApplyDarkMode(NO);
      break;
    case AppIconMode::kDark:
      [observer stopObserving];
      ApplyDarkMode(YES);
      break;
  }
}

void StartAppIconController() {
  SetAppIconMode(GetAppIconMode());
}

}  // namespace cmux
