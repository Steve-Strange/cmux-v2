// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef CHROME_BROWSER_CMUX_TERM_CMUX_TERMINAL_BACKEND_H_
#define CHROME_BROWSER_CMUX_TERM_CMUX_TERMINAL_BACKEND_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "base/containers/span.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/ref_counted.h"
#include "base/memory/weak_ptr.h"
#include "base/sequence_checker.h"
#include "base/timer/timer.h"
#include "chrome/browser/cmux_term/cmux_tui_client.h"
#include "chrome/browser/cmux_term/cmux_tui_protocol.h"

namespace cmux {

// A canonical cmux-tui placement projected into a Browser terminal backend.
// `surface` is a daemon-local lease and is valid only with its generation;
// terminal_id/incarnation remain stable when that lease disappears.
struct CmuxTerminalBackendSeed {
  CmuxTerminalBackendSeed();
  CmuxTerminalBackendSeed(const CmuxTerminalBackendSeed&);
  CmuxTerminalBackendSeed& operator=(const CmuxTerminalBackendSeed&);
  CmuxTerminalBackendSeed(CmuxTerminalBackendSeed&&);
  CmuxTerminalBackendSeed& operator=(CmuxTerminalBackendSeed&&);
  ~CmuxTerminalBackendSeed();

  TerminalHostId terminal_id{};
  std::optional<TerminalHostIncarnation> terminal_incarnation;
  std::optional<CmuxTuiSurfaceId> surface;
  uint64_t server_generation = 0;
  CmuxTerminalLifecycle lifecycle = CmuxTerminalLifecycle::kAdopting;
  std::string pending_mutation_id;
  std::string registry_id;
  std::string registry_generation;
};

// The lifetime boundary between a cmux-browser terminal tab and its native
// Ghostty mirror. The backend owns the cmux-tui surface/PTY identity and can
// outlive a particular Views/AppKit frontend. A frontend only renders replay
// and output bytes and turns local input back into protocol `send` requests.
class CmuxTerminalBackend
    : public base::RefCountedThreadSafe<CmuxTerminalBackend>,
      public CmuxTuiClient::Observer {
 public:
  class Frontend {
   public:
    virtual ~Frontend() = default;

    // `generation` increases whenever mirror state must be reset from a
    // complete replay (initial attach, server resize, or overflow recovery).
    virtual void OnCmuxTerminalReplay(
        uint64_t generation,
        uint16_t cols,
        uint16_t rows,
        base::span<const uint8_t> replay,
        const std::optional<CmuxTuiColors>& colors) = 0;
    virtual void OnCmuxTerminalOutput(base::span<const uint8_t> bytes) = 0;
    virtual void OnCmuxTerminalColors(const CmuxTuiColors& colors) = 0;
    virtual void OnCmuxTerminalTitle(const std::string& title) = 0;
    virtual void OnCmuxTerminalPwd(const std::string& pwd) = 0;
    virtual void OnCmuxTerminalBell() = 0;
    // Process exit is terminal state, not a request to close the GUI tab or
    // mutate the canonical workspace. The user may inspect the final frame
    // and explicitly close the terminal later.
    virtual void OnCmuxTerminalExited(const std::string& reason) = 0;
    virtual void OnCmuxTerminalClosed(const std::string& reason) = 0;
    virtual void BeginCmuxTerminalHostInputCutover(
        uint64_t cutover_id,
        base::OnceCallback<void(bool success, std::string error)> callback) = 0;
    virtual void AttachCmuxTerminalHost(
        uint64_t cutover_id,
        CmuxTuiRendererConnection connection,
        base::OnceCallback<void(bool success, std::string error)> callback) = 0;
    virtual void CancelCmuxTerminalHostInputCutover(
        uint64_t cutover_id,
        base::OnceClosure callback) = 0;
    virtual void DetachCmuxTerminalHost() = 0;
    virtual void RestartCmuxTerminalRenderer(const std::string& reason) = 0;
  };

  CmuxTerminalBackend(scoped_refptr<CmuxTuiClient> client,
                      std::string command,
                      std::string name,
                      std::string workspace_key);
  // Adopts canonical registry state in place. This overload never launches an
  // unrelated replacement process, including when the runtime surface is not
  // present yet or the original create response was ambiguous.
  CmuxTerminalBackend(scoped_refptr<CmuxTuiClient> client,
                      std::string command,
                      std::string name,
                      std::string workspace_key,
                      CmuxTerminalBackendSeed seed);
  CmuxTerminalBackend(const CmuxTerminalBackend&) = delete;
  CmuxTerminalBackend& operator=(const CmuxTerminalBackend&) = delete;

  // Only one native frontend is attached at a time. Detaching destroys its
  // dedicated stream, not the authoritative TUI surface or child process.
  void AttachFrontend(Frontend* frontend, uint16_t cols, uint16_t rows);
  // Register the durable GUI observer and ensure/create the canonical terminal
  // host without attaching a replay/direct renderer stream. Used by panes that
  // are born hidden so their title and lifecycle still update immediately.
  void AttachSuspendedFrontend(Frontend* frontend,
                               uint16_t cols,
                               uint16_t rows);
  void DetachFrontend(Frontend* frontend);
  // A renderer that completed the ordered cold-eviction handshake has
  // already flushed input and released its direct viewer. Retire only the
  // Browser-side routes; never close the durable terminal host.
  bool SuspendFrontend(Frontend* frontend);
  bool CanColdSuspendFrontend(Frontend* frontend) const;

  void UpdateSize(uint16_t cols, uint16_t rows);
  void SendInput(base::span<const uint8_t> bytes);
  void RequestReplay(const std::string& reason);
  void DirectTerminalHostExited();
  void DirectTerminalHostTitle(const std::string& title);
  void DirectTerminalHostPwd(const std::string& pwd);
  void DirectTerminalHostBell();
  void DirectTerminalHostDisconnected(const std::string& reason);

  // Reconciles a newer canonical placement into this existing pane/renderer.
  // Identity changes fail closed; lifecycle progress never enables Create.
  bool ApplyCanonicalLifecycle(
      CmuxTerminalLifecycle lifecycle,
      std::optional<TerminalHostIncarnation> terminal_incarnation,
      const std::string& registry_id,
      const std::string& registry_generation,
      bool registry_replaced,
      bool daemon_generation_changed);

  // Explicit tab close. View destruction alone must never call this.
  void Close();

  std::optional<CmuxTuiSurfaceId> surface_id() const;
  std::optional<TerminalHostId> stable_terminal_id() const;
  std::optional<TerminalHostIncarnation> stable_terminal_incarnation() const;
  const std::string& terminal_create_mutation_id() const;
  const std::string& terminal_close_mutation_id() const;
  bool terminal_create_pending() const;
  bool closed() const;
  const std::string& command() const { return command_; }

  // CmuxTuiClient::Observer:
  void OnCmuxTuiConnectionChanged(bool connected,
                                  const std::string& error) override;
  void OnCmuxTuiEvent(const CmuxTuiEvent& event) override;

 private:
  friend class base::RefCountedThreadSafe<CmuxTerminalBackend>;
  ~CmuxTerminalBackend() override;

  enum class DirectAttachPhase {
    kIdle,
    kQuiescingInput,
    kDrainingCompatibilityInput,
    kMintingCapability,
    kAttachingRenderer,
    kCancellingInputCutover,
  };

  void EnsureSurface();
  void OnSurfaceCreated(uint64_t request_server_generation,
                        uint16_t initial_cols,
                        uint16_t initial_rows,
                        std::optional<CmuxTerminalBinding> binding,
                        const std::string& error);
  void ResolveSurface();
  void OnSurfaceResolved(uint64_t request_server_generation,
                         std::optional<CmuxTerminalBinding> binding,
                         const std::string& error);
  std::optional<CmuxTuiSurfaceId> CurrentSurface() const;
  std::optional<CmuxTuiSurfaceId> CurrentRenderableSurface() const;
  void DetachFrontendInternal(Frontend* frontend, bool detach_renderer);
  void EnsureDirectAttached();
  void OnDirectInputCutoverReady(uint64_t attempt,
                                 bool success,
                                 std::string error);
  void MintDirectRendererAfterInputDrain(uint64_t attempt);
  void OnRendererConnectionMinted(
      uint64_t attempt,
      std::optional<CmuxTuiRendererConnection> connection,
      const std::string& error);
  void OnDirectAttachComplete(uint64_t attempt,
                              bool success,
                              std::string error);
  void CancelDirectAttach(uint64_t attempt, std::string reason);
  void OnDirectInputCutoverCancelled(uint64_t attempt, std::string reason);
  void OnDirectAttachWatchdog(uint64_t attempt);
  void OnDirectExitFallback();
  void ObserveControlPlaneExit(std::string reason);
  void ScheduleDirectAttach(const std::string& reason);
  void EnsureAttached();
  void EnsureExitedAttached();
  void OnAttachmentEvent(CmuxTuiEvent event);
  void MarkExited(const std::string& reason);
  void MarkClosed(const std::string& reason, bool reset_attachment);
  void ScheduleReattach(const std::string& reason, bool reset_surface = false);
  void ResetAttachmentAndRetry();
  void SendResizeIfNeeded();
  void OnResizeResult(uint64_t request,
                      uint16_t cols,
                      uint16_t rows,
                      CmuxTuiCommandResult result);
  void FlushPendingInput();
  void OnInputResult(uint64_t request, CmuxTuiCommandResult result);
  void ResetOutbound(bool clear_pending_input);

  const scoped_refptr<CmuxTuiClient> client_;
  const std::string command_;
  const std::string name_;
  const std::string workspace_key_;
  std::string terminal_mutation_id_;
  std::string close_mutation_id_;
  std::string canonical_registry_id_;
  std::string canonical_registry_generation_;
  raw_ptr<Frontend> frontend_ = nullptr;
  // Cold renderer eviction suspends only the high-volume replay/direct stream.
  // Keep the durable frontend observer alive so title/lifecycle changes still
  // mutate the GUI immediately while no renderer process exists.
  bool frontend_suspended_ = false;
  CmuxTerminalRecovery recovery_;
  std::optional<CmuxTuiColors> colors_;
  std::unique_ptr<CmuxTuiAttachment> attachment_;
  CmuxTuiInputQueue input_queue_;
  CmuxTuiResizeCoalescer resize_queue_;
  uint16_t requested_cols_ = 0;
  uint16_t requested_rows_ = 0;
  uint64_t input_request_ = 0;
  uint64_t resize_request_ = 0;
  uint64_t replay_generation_ = 0;
  bool binding_request_in_flight_ = false;
  uint64_t direct_attach_attempt_ = 0;
  bool direct_attach_in_flight_ = false;
  bool direct_attached_ = false;
  bool control_exit_pending_ = false;
  std::string control_exit_reason_;
  DirectAttachPhase direct_attach_phase_ = DirectAttachPhase::kIdle;
  bool reattach_scheduled_ = false;
  bool reset_surface_on_retry_ = false;
  // True only for a GUI-reserved identity whose idempotent create has not yet
  // been confirmed by either the create response or canonical registry state.
  // Retrying while this is true always uses the same terminal/mutation IDs.
  bool create_pending_ = true;
  bool closed_ = false;
  base::OneShotTimer retry_timer_;
  base::OneShotTimer direct_retry_timer_;
  base::OneShotTimer direct_attach_watchdog_timer_;
  base::OneShotTimer direct_exit_timer_;
  base::OneShotTimer resize_timer_;
  SEQUENCE_CHECKER(sequence_checker_);
  base::WeakPtrFactory<CmuxTerminalBackend> weak_factory_{this};
};

}  // namespace cmux

#endif  // CHROME_BROWSER_CMUX_TERM_CMUX_TERMINAL_BACKEND_H_
