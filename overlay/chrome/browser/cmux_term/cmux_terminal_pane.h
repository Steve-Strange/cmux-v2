// Copyright 2026 Manaflow, Inc.
// Portions derived from Helium; Helium contributors retain copyright.
// SPDX-License-Identifier: GPL-3.0-or-later AND GPL-3.0-only

#ifndef CHROME_BROWSER_CMUX_TERM_CMUX_TERMINAL_PANE_H_
#define CHROME_BROWSER_CMUX_TERM_CMUX_TERMINAL_PANE_H_

#import <AppKit/AppKit.h>

#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/timer/timer.h"
#include "chrome/browser/cmux_term/cmux_ghostty.h"
#include "chrome/browser/cmux_term/cmux_surface.h"
#include "chrome/browser/cmux_term/cmux_terminal_backend.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/views/controls/native/native_view_host.h"
#include "ui/views/view.h"

@class CAShapeLayer;

namespace cmux {

// A terminal surface (one tab's content) that hosts a live Ghostty terminal
// NSView via a NativeViewHost.
class CmuxTerminalSurface : public views::View,
                            public CmuxSurface,
                            public CmuxTerminalBackend::Frontend {
  METADATA_HEADER(CmuxTerminalSurface, views::View)

 public:
  explicit CmuxTerminalSurface(scoped_refptr<CmuxTerminalBackend> backend);
  CmuxTerminalSurface(const CmuxTerminalSurface&) = delete;
  CmuxTerminalSurface& operator=(const CmuxTerminalSurface&) = delete;
  ~CmuxTerminalSurface() override;

  // views::View:
  void AddedToWidget() override;
  void Layout(PassKey) override;
  void OnBoundsChanged(const gfx::Rect& previous_bounds) override;
  void VisibilityChanged(views::View* starting_from, bool is_visible) override;
  void OnThemeChanged() override;

  // CmuxSurface:
  views::View* AsView() override;
  SurfaceKind kind() const override;
  void FocusContent() override;
  void SetActivationCallback(base::RepeatingClosure callback) override;
  void SetInteractionCallback(base::RepeatingClosure callback) override;
  void SetTitleChangedCallback(
      base::RepeatingCallback<void(const std::u16string&)> callback) override;
  void SetCloseRequestedCallback(base::RepeatingClosure callback) override;
  void FireCloseRequestedForTesting() override;
  void SetRoundedFrame(const RoundedFrameGeometry& geometry) override;

  // CmuxTerminalBackend::Frontend:
  void OnCmuxTerminalReplay(
      uint64_t generation,
      uint16_t cols,
      uint16_t rows,
      base::span<const uint8_t> replay,
      const std::optional<CmuxTuiColors>& colors) override;
  void OnCmuxTerminalOutput(base::span<const uint8_t> bytes) override;
  void OnCmuxTerminalColors(const CmuxTuiColors& colors) override;
  void OnCmuxTerminalTitle(const std::string& title) override;
  void OnCmuxTerminalPwd(const std::string& pwd) override;
  void OnCmuxTerminalBell() override;
  void OnCmuxTerminalExited(const std::string& reason) override;
  void OnCmuxTerminalClosed(const std::string& reason) override;
  void BeginCmuxTerminalHostInputCutover(
      uint64_t cutover_id,
      base::OnceCallback<void(bool success, std::string error)> callback)
      override;
  void AttachCmuxTerminalHost(
      uint64_t cutover_id,
      CmuxTuiRendererConnection connection,
      base::OnceCallback<void(bool success, std::string error)> callback)
      override;
  void CancelCmuxTerminalHostInputCutover(uint64_t cutover_id,
                                          base::OnceClosure callback) override;
  void DetachCmuxTerminalHost() override;
  void RestartCmuxTerminalRenderer(const std::string& reason) override;

 private:
  enum class RendererResidency {
    kHot,
    kEvicting,
    kCold,
  };

  void WireActivation();
  void WireInteraction();
  void WireCloseRequested();
  void AttachBackend();
  void WakeColdRenderer();
  void ScheduleColdEviction();
  void BeginColdEviction();
  void OnColdEvictionTimeout(uint64_t eviction_id);
  void OnColdEvictionReady(uint64_t eviction_id,
                           bool success,
                           std::string error);
  // Appends a complete sparse color reset/apply delta to `bytes` and submits
  // the result through exactly one renderer output operation. A null `colors`
  // pointer submits only output. The durable-host frontend uses this hook when
  // an Output frame advertises an immediately following Colors frame.
  void ProcessTerminalOutputAndColors(base::span<const uint8_t> bytes,
                                      const CmuxTuiColors* colors);
  // Clip the hosted terminal to the surface's portion that is actually visible
  // (ancestors clip it at the strip's content area and at the pane bounds), so
  // a column scrolled under the rail never paints (or intercepts clicks) over
  // it. NativeViewHostMac doesn't clip plain NSViews (InstallClip is
  // NOTIMPLEMENTED), so we do it: the NativeViewHost holds a clipsToBounds
  // container sized to the visible sub-rect, with the full-size terminal
  // offset inside it (so Ghostty never reflows on scroll).
  void UpdateTerminalClip();
  void UpdateRoundedFrameAppearance();

  raw_ptr<views::NativeViewHost> host_ = nullptr;
  NSView* __strong clip_view_ =
      nil;  // clipsToBounds container for the terminal
  NSView* __strong rounded_view_ =
      nil;  // full-size rounded frame inside the rectangular clip
  CAShapeLayer* __strong rounded_mask_layer_ = nil;
  CAShapeLayer* __strong rounded_outline_layer_ = nil;
  CmuxGhosttyTerminalView* __strong terminal_view_ = nil;
  base::RepeatingClosure on_activated_;
  base::RepeatingClosure on_interaction_;
  base::RepeatingCallback<void(const std::u16string&)> on_title_changed_;
  base::RepeatingClosure on_close_requested_;
  const scoped_refptr<CmuxTerminalBackend> backend_;
  bool attached_ = false;
  bool backend_attached_ = false;
  bool effectively_visible_ = false;
  RoundedFrameGeometry rounded_frame_geometry_;
  RendererResidency renderer_residency_ = RendererResidency::kCold;
  uint64_t cold_eviction_id_ = 0;
  base::OneShotTimer cold_renderer_timer_;
  base::OneShotTimer cold_eviction_watchdog_;
  base::WeakPtrFactory<CmuxTerminalSurface> weak_factory_{this};
};

}  // namespace cmux

#endif  // CHROME_BROWSER_CMUX_TERM_CMUX_TERMINAL_PANE_H_
