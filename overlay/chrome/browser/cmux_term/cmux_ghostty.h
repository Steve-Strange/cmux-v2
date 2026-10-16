// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef CHROME_BROWSER_CMUX_TERM_CMUX_GHOSTTY_H_
#define CHROME_BROWSER_CMUX_TERM_CMUX_GHOSTTY_H_

#ifdef __OBJC__
#import <AppKit/AppKit.h>
#endif

#ifdef __cplusplus
#include <cstdint>
#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "chrome/browser/cmux_term/cmux_theme.h"
#include "mojo/public/cpp/platform/platform_handle.h"
#endif

#ifdef __OBJC__
// A real Ghostty terminal mirror as an NSView. It owns only the renderer/input
// encoder surface; cmux-tui owns the shell, PTY, and authoritative VT state.
// Each view launches one dedicated utility process containing one
// ghostty_app_t and one surface.
@interface CmuxGhosttyTerminalView : NSView <NSTextInputClient>
// Invoked when this view becomes the AppKit first responder (e.g. a click), so
// the owning pane can mark its column focused. May be nil.
@property(nonatomic, copy) void (^onActivate)(void);
// Invoked for each direct key, button press, or scroll delivered by AppKit.
// Unlike onActivate, this is never fired by programmatic first-responder
// changes and can therefore fence deferred cross-frontend focus safely.
@property(nonatomic, copy) void (^onInteraction)(void);
// Invoked when libghostty's close_surface callback fires for this surface.
// `processAlive` mirrors ghostty_runtime_close_surface_cb's second argument.
@property(nonatomic, copy) void (^onCloseSurface)(BOOL processAlive);
// Encoded keyboard/mouse/text/paste bytes produced by libghostty manual IO.
@property(nonatomic, copy) void (^onInput)(NSData* bytes);
// A renderer utility crash or queue failure. The owner requests a fresh cmux
// TUI replay; this view restarts only its one-terminal renderer process.
@property(nonatomic, copy) void (^onRendererError)(NSString* reason);
// Low-rate metadata from a direct terminal-host socket. PTY bytes remain
// inside the utility process.
@property(nonatomic, copy) void (^onTerminalHostTitle)(NSString* title);
@property(nonatomic, copy) void (^onTerminalHostPwd)(NSString* pwd);
@property(nonatomic, copy) void (^onTerminalHostBell)(void);
@property(nonatomic, copy) void (^onTerminalHostExit)(void);
@property(nonatomic, copy) void (^onTerminalHostDisconnected)(NSString* reason);
// Updated after Ghostty resolves pixel geometry through font metrics.
@property(nonatomic, copy) void (^onGridSize)(uint16_t columns, uint16_t rows);
// Match the layer exposed around a retained frame to Ghostty's resolved or
// PTY-overridden background without scaling that frame during live resize.
- (void)setTerminalBackgroundRed:(uint8_t)red
                           green:(uint8_t)green
                            blue:(uint8_t)blue;
// Restore the background reported by this renderer incarnation's Ghostty
// configuration after a sparse color state removes its OSC 11 override.
- (void)restoreConfiguredTerminalBackground;
- (void)setRendererVisible:(BOOL)visible;
- (void)restartRendererWithReason:(NSString*)reason;
- (BOOL)canColdEvict;
- (void)resumeRenderer;
// Current renderer grid after applying this view's pixel size.
- (NSSize)terminalGridSize;
// Atomically replace the mirror generation at the canonical grid resolved by
// cmux TUI. No frame at that grid is publishable until the replay has been
// applied. Returns NO when a delayed or internally inconsistent replay
// generation must be discarded as a whole.
- (BOOL)replaceTerminalState:(NSData*)replay
        authoritativeColumns:(uint16_t)columns
                        rows:(uint16_t)rows
                  generation:(uint64_t)generation;
// Feed ordered live output (or small metadata escape sequences) to the mirror.
// Returns NO only if the pending-output cap was exceeded and the owner must
// request a fresh authoritative replay.
- (BOOL)processTerminalOutput:(NSData*)output;
#ifdef __cplusplus
// Starts an ordered compatibility-to-direct input handoff. Completion is
// delivered only after every older OnInput message has reached the browser and
// all newer libghostty writes are buffered in the renderer process.
- (void)beginTerminalHostInputCutover:(uint64_t)cutoverId
                           completion:
                               (base::OnceCallback<void(bool, std::string)>)
                                   completion;
// Transfers an already-authenticated descriptor into this view's dedicated
// renderer utility. Completion fires only after its initial replay publishes,
// buffered input is queued first on that socket, and direct input is enabled.
- (void)attachTerminalHostSocket:(mojo::PlatformHandle)socket
                      terminalId:(std::vector<uint8_t>)terminalId
             terminalIncarnation:(std::vector<uint8_t>)terminalIncarnation
                          rights:(uint32_t)rights
                   protocolFlags:(uint32_t)protocolFlags
                       cutoverId:(uint64_t)cutoverId
                      completion:(base::OnceCallback<void(bool, std::string)>)
                                     completion;
- (void)cancelTerminalHostInputCutover:(uint64_t)cutoverId
                            completion:(base::OnceClosure)completion;
- (void)detachTerminalHost;
// Orders cold eviction after all older semantic input and after the direct
// host socket has flushed ReleaseViewer. `completeColdEviction` intentionally
// tears down this disposable renderer without entering crash recovery.
- (void)prepareForColdEviction:
            (uint64_t)evictionId
                     completion:
                         (base::OnceCallback<void(bool, std::string)>)completion;
- (void)completeColdEviction:(uint64_t)evictionId;
#endif
@end
#endif

#ifdef __cplusplus
namespace cmux {

// Applies resolved color defaults in place. Visible renderers update
// immediately, hidden renderers are spread across short UI-sequence batches,
// and cold renderers pick up the published selection when next resumed.
void ApplyCmuxGhosttyThemeToAllTerminalViews(const CmuxTheme& theme);

}  // namespace cmux
#endif

#endif  // CHROME_BROWSER_CMUX_TERM_CMUX_GHOSTTY_H_
