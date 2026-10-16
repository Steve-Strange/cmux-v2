// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "chrome/browser/cmux_term/cmux_terminal_backend.h"

#include <string_view>
#include <utility>
#include <vector>

#include "base/check.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/memory/ref_counted.h"
#include "base/rand_util.h"
#include "base/time/time.h"
#include "build/build_config.h"

namespace cmux {

namespace {

constexpr base::TimeDelta kReattachDelay = base::Milliseconds(200);
constexpr base::TimeDelta kResizeDebounce = base::Milliseconds(100);
constexpr base::TimeDelta kRendererCapabilityTtl = base::Seconds(5);
#if BUILDFLAG(IS_MAC)
constexpr base::TimeDelta kDirectAttachTimeout = base::Seconds(5);
#endif
constexpr base::TimeDelta kDirectCancelTimeout = base::Seconds(2);
constexpr base::TimeDelta kDirectExitFallbackTimeout = base::Seconds(2);

TerminalHostId GenerateTerminalId() {
  TerminalHostId id{};
  base::RandBytes(id);
  id[6] = static_cast<uint8_t>((id[6] & 0x0f) | 0x40);
  id[8] = static_cast<uint8_t>((id[8] & 0x3f) | 0x80);
  return id;
}

}  // namespace

CmuxTerminalBackendSeed::CmuxTerminalBackendSeed() = default;
CmuxTerminalBackendSeed::CmuxTerminalBackendSeed(
    const CmuxTerminalBackendSeed&) = default;
CmuxTerminalBackendSeed& CmuxTerminalBackendSeed::operator=(
    const CmuxTerminalBackendSeed&) = default;
CmuxTerminalBackendSeed::CmuxTerminalBackendSeed(CmuxTerminalBackendSeed&&) =
    default;
CmuxTerminalBackendSeed& CmuxTerminalBackendSeed::operator=(
    CmuxTerminalBackendSeed&&) = default;
CmuxTerminalBackendSeed::~CmuxTerminalBackendSeed() = default;

CmuxTerminalBackend::CmuxTerminalBackend(scoped_refptr<CmuxTuiClient> client,
                                         std::string command,
                                         std::string name,
                                         std::string workspace_key)
    : client_(std::move(client)),
      command_(std::move(command)),
      name_(std::move(name)),
      workspace_key_(std::move(workspace_key)) {
  CHECK(client_);
  const TerminalHostId terminal_id = GenerateTerminalId();
  CHECK(recovery_.ReserveTerminalId(terminal_id));
  terminal_mutation_id_ = EncodeTerminalHostId(GenerateTerminalId());
  close_mutation_id_ = EncodeTerminalHostId(GenerateTerminalId());
  DETACH_FROM_SEQUENCE(sequence_checker_);
  client_->AddObserver(this);
}

CmuxTerminalBackend::CmuxTerminalBackend(scoped_refptr<CmuxTuiClient> client,
                                         std::string command,
                                         std::string name,
                                         std::string workspace_key,
                                         CmuxTerminalBackendSeed seed)
    : client_(std::move(client)),
      command_(std::move(command)),
      name_(std::move(name)),
      workspace_key_(std::move(workspace_key)),
      create_pending_(false) {
  CHECK(client_);
  CHECK(recovery_.SeedKnownTerminal(
      seed.terminal_id, std::move(seed.terminal_incarnation), seed.surface,
      seed.server_generation, seed.lifecycle));
  terminal_mutation_id_ = seed.pending_mutation_id.empty()
                              ? EncodeTerminalHostId(GenerateTerminalId())
                              : std::move(seed.pending_mutation_id);
  close_mutation_id_ = EncodeTerminalHostId(GenerateTerminalId());
  canonical_registry_id_ = std::move(seed.registry_id);
  canonical_registry_generation_ = std::move(seed.registry_generation);
  DETACH_FROM_SEQUENCE(sequence_checker_);
  client_->AddObserver(this);
}

CmuxTerminalBackend::~CmuxTerminalBackend() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  weak_factory_.InvalidateWeakPtrs();
  retry_timer_.Stop();
  direct_retry_timer_.Stop();
  direct_attach_watchdog_timer_.Stop();
  direct_exit_timer_.Stop();
  resize_timer_.Stop();
  attachment_.reset();
  client_->RemoveObserver(this);
}

void CmuxTerminalBackend::AttachFrontend(Frontend* frontend,
                                         uint16_t cols,
                                         uint16_t rows) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  CHECK(frontend);
  CHECK(!frontend_ || frontend_ == frontend);
  frontend_ = frontend;
  frontend_suspended_ = false;
  UpdateSize(cols, rows);
  if (recovery_.exited()) {
    frontend_->OnCmuxTerminalExited("terminal process exited");
    EnsureExitedAttached();
    return;
  }
  EnsureSurface();
}

void CmuxTerminalBackend::AttachSuspendedFrontend(Frontend* frontend,
                                                  uint16_t cols,
                                                  uint16_t rows) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  CHECK(frontend);
  CHECK(!frontend_ || frontend_ == frontend);
  frontend_ = frontend;
  frontend_suspended_ = true;
  resize_queue_.SetDesired(cols, rows);
  if (recovery_.exited()) {
    frontend_->OnCmuxTerminalExited("terminal process exited");
    return;
  }
  // EnsureSurface keeps durable create/resolve work alive while suppressing
  // the high-volume attachment and direct renderer routes below.
  EnsureSurface();
}

void CmuxTerminalBackend::DetachFrontend(Frontend* frontend) {
  DetachFrontendInternal(frontend, true);
}

bool CmuxTerminalBackend::SuspendFrontend(Frontend* frontend) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!frontend || frontend_ != frontend || frontend_suspended_) {
    return false;
  }
  frontend_suspended_ = true;
  DetachFrontendInternal(frontend, false);
  return true;
}

bool CmuxTerminalBackend::CanColdSuspendFrontend(Frontend* frontend) const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  const std::optional<CmuxTuiSurfaceId> rehydration_surface =
      recovery_.exited()
          ? recovery_.FinalSurfaceForGeneration(client_->server_generation())
          : CurrentSurface();
  return frontend && frontend_ == frontend && !frontend_suspended_ &&
         !closed_ && client_->ready() && rehydration_surface.has_value() &&
         !recovery_.terminal_error() &&
         !control_exit_pending_ && !binding_request_in_flight_ &&
         !reattach_scheduled_ && !direct_attach_in_flight_ &&
         direct_attach_phase_ == DirectAttachPhase::kIdle &&
         !direct_retry_timer_.IsRunning() &&
         !input_queue_.write_in_flight() && input_queue_.pending_bytes() == 0 &&
         !resize_queue_.write_in_flight() && !resize_timer_.IsRunning();
}

void CmuxTerminalBackend::DetachFrontendInternal(Frontend* frontend,
                                                 bool detach_renderer) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (frontend_ != frontend) {
    return;
  }
  const bool detach_direct = direct_attached_ || direct_attach_in_flight_;
  ++direct_attach_attempt_;
  direct_attach_in_flight_ = false;
  if (detach_direct && detach_renderer) {
    frontend->DetachCmuxTerminalHost();
  }
  direct_attached_ = false;
  direct_attach_phase_ = DirectAttachPhase::kIdle;
  reattach_scheduled_ = false;
  reset_surface_on_retry_ = false;
  retry_timer_.Stop();
  direct_retry_timer_.Stop();
  direct_attach_watchdog_timer_.Stop();
  resize_timer_.Stop();
  attachment_.reset();
  ++resize_request_;
  resize_queue_.CancelWrite();
  if (detach_renderer) {
    frontend_ = nullptr;
    frontend_suspended_ = false;
  }
}

void CmuxTerminalBackend::UpdateSize(uint16_t cols, uint16_t rows) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  resize_queue_.SetDesired(cols, rows);
  if (frontend_suspended_) {
    return;
  }
  if (direct_attached_) {
    return;
  }
  if (!CurrentSurface() || requested_cols_ == 0 || requested_rows_ == 0) {
    SendResizeIfNeeded();
    return;
  }
  // Pixel geometry reaches the local Ghostty surface synchronously. Wait for
  // live-window resizing to settle before asking cmux-tui for the expensive
  // authoritative grid replay, avoiding a stream of visible replacements.
  resize_timer_.Start(FROM_HERE, kResizeDebounce,
                      base::BindOnce(&CmuxTerminalBackend::SendResizeIfNeeded,
                                     weak_factory_.GetWeakPtr()));
}

void CmuxTerminalBackend::SendInput(base::span<const uint8_t> bytes) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (closed_ || recovery_.exited() || recovery_.terminal_error() ||
      bytes.empty()) {
    return;
  }
  const std::string_view input(reinterpret_cast<const char*>(bytes.data()),
                               bytes.size());
  if (!input_queue_.Push(input)) {
    LOG(WARNING) << "cmux-terminal: dropping input (pending-input cap "
                    "reached)";
    return;
  }
  if (frontend_suspended_) {
    return;
  }
  if (!CurrentSurface() || !client_->ready()) {
    EnsureSurface();
    return;
  }
  FlushPendingInput();
}

void CmuxTerminalBackend::RequestReplay(const std::string& reason) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (direct_attached_ || direct_attach_in_flight_) {
    DirectTerminalHostDisconnected(reason);
    return;
  }
  ScheduleReattach(reason);
}

void CmuxTerminalBackend::DirectTerminalHostExited() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (closed_ || (recovery_.exited() && !control_exit_pending_)) {
    return;
  }
  control_exit_pending_ = false;
  control_exit_reason_.clear();
  direct_exit_timer_.Stop();
  // Preserve the direct state until MarkExited asks the renderer to release
  // its viewer/socket. Clearing it first would defeat that cleanup condition.
  MarkExited("terminal process exited");
}

void CmuxTerminalBackend::DirectTerminalHostTitle(const std::string& title) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!closed_ && frontend_) {
    frontend_->OnCmuxTerminalTitle(title);
  }
}

void CmuxTerminalBackend::DirectTerminalHostPwd(const std::string& pwd) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!closed_ && frontend_) {
    frontend_->OnCmuxTerminalPwd(pwd);
  }
}

void CmuxTerminalBackend::DirectTerminalHostBell() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!closed_ && frontend_) {
    frontend_->OnCmuxTerminalBell();
  }
}

void CmuxTerminalBackend::DirectTerminalHostDisconnected(
    const std::string& reason) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (closed_ || (recovery_.exited() && !control_exit_pending_)) {
    return;
  }
  if (control_exit_pending_) {
    std::string exit_reason = control_exit_reason_.empty()
                                  ? "terminal process exited"
                                  : std::move(control_exit_reason_);
    control_exit_pending_ = false;
    direct_exit_timer_.Stop();
    MarkExited(exit_reason);
    return;
  }
  if (!direct_attached_ && !direct_attach_in_flight_) {
    return;
  }
  ++direct_attach_attempt_;
  direct_attach_in_flight_ = false;
  direct_attached_ = false;
  direct_attach_phase_ = DirectAttachPhase::kIdle;
  direct_attach_watchdog_timer_.Stop();
  // Restore the compatibility stream immediately, then mint a fresh one-use
  // renderer capability after a short backoff. The prior attachment may have
  // advanced while the candidate direct socket suppressed its output, so only
  // a new Snapshot is authoritative after fallback.
  attachment_.reset();
  EnsureAttached();
  // A direct QueueInput rejection is returned on the renderer client pipe
  // before its disconnect marker. It waits in this finite queue while direct
  // mode is set, then resumes through compatibility here without being lost.
  FlushPendingInput();
  ScheduleDirectAttach(reason);
}

bool CmuxTerminalBackend::ApplyCanonicalLifecycle(
    CmuxTerminalLifecycle lifecycle,
    std::optional<TerminalHostIncarnation> terminal_incarnation,
    const std::string& registry_id,
    const std::string& registry_generation,
    bool registry_replaced,
    bool daemon_generation_changed) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (closed_ || registry_replaced || registry_id.empty() ||
      registry_generation.empty() ||
      (!canonical_registry_id_.empty() &&
       canonical_registry_id_ != registry_id)) {
    return false;
  }
  const bool generation_changed =
      daemon_generation_changed ||
      (!canonical_registry_generation_.empty() &&
       canonical_registry_generation_ != registry_generation);
  if (generation_changed) {
    // Runtime surface leases belong to one daemon generation. The direct host
    // stream may remain healthy, but all daemon-local resolution state must be
    // reacquired before compatibility attach/resize/input can resume.
    attachment_.reset();
    recovery_.InvalidateLocalSurface();
    ResetOutbound(false);
    requested_cols_ = 0;
    requested_rows_ = 0;
  }
  if (!recovery_.ApplyCanonicalLifecycle(lifecycle, terminal_incarnation)) {
    const bool create_reply_overtook_launch_event =
        !generation_changed && !terminal_incarnation && !create_pending_ &&
        lifecycle == CmuxTerminalLifecycle::kLaunching &&
        recovery_.lifecycle() &&
        *recovery_.lifecycle() == CmuxTerminalLifecycle::kRunning;
    if (!create_reply_overtook_launch_event) {
      return false;
    }
    // The recovery state intentionally stays Running. The canonical registry
    // will advance to Running without changing the stable host incarnation.
  }
  canonical_registry_id_ = registry_id;
  canonical_registry_generation_ = registry_generation;
  // The registry has observed this client-reserved identity. From here on a
  // daemon reconnect may only resolve/adopt it; an absent later snapshot is
  // authoritative removal rather than permission to keep retrying Create.
  create_pending_ = false;
  if (lifecycle == CmuxTerminalLifecycle::kExited ||
      lifecycle == CmuxTerminalLifecycle::kTombstoned) {
    if (lifecycle == CmuxTerminalLifecycle::kTombstoned) {
      MarkExited("terminal was closed");
    } else {
      ObserveControlPlaneExit("terminal process exited");
    }
    if (lifecycle == CmuxTerminalLifecycle::kTombstoned) {
      // MarkExited preserves a tombstone, but retain this assertion at the
      // integration boundary because reviving it would permit retry races.
      CHECK(recovery_.lifecycle() == CmuxTerminalLifecycle::kTombstoned);
    }
    return true;
  }
  EnsureSurface();
  return true;
}

void CmuxTerminalBackend::Close() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (closed_) {
    return;
  }
  closed_ = true;
  reattach_scheduled_ = false;
  reset_surface_on_retry_ = false;
  retry_timer_.Stop();
  direct_retry_timer_.Stop();
  direct_attach_watchdog_timer_.Stop();
  direct_exit_timer_.Stop();
  control_exit_pending_ = false;
  control_exit_reason_.clear();
  resize_timer_.Stop();
  attachment_.reset();
  ++direct_attach_attempt_;
  direct_attach_in_flight_ = false;
  if ((direct_attached_ || direct_attach_phase_ != DirectAttachPhase::kIdle) &&
      frontend_) {
    frontend_->DetachCmuxTerminalHost();
  }
  direct_attached_ = false;
  direct_attach_phase_ = DirectAttachPhase::kIdle;
  ResetOutbound(true);
  if (recovery_.stable_terminal_id()) {
    client_->CloseTerminal(*recovery_.stable_terminal_id(),
                           recovery_.stable_incarnation(), close_mutation_id_);
  }
  recovery_.InvalidateLocalSurface();
}

std::optional<CmuxTuiSurfaceId> CmuxTerminalBackend::surface_id() const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  return CurrentSurface();
}

std::optional<TerminalHostId> CmuxTerminalBackend::stable_terminal_id() const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  return recovery_.stable_terminal_id();
}

std::optional<TerminalHostIncarnation>
CmuxTerminalBackend::stable_terminal_incarnation() const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  return recovery_.stable_incarnation();
}

const std::string& CmuxTerminalBackend::terminal_create_mutation_id() const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  return terminal_mutation_id_;
}

const std::string& CmuxTerminalBackend::terminal_close_mutation_id() const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  return close_mutation_id_;
}

bool CmuxTerminalBackend::terminal_create_pending() const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  return create_pending_;
}

bool CmuxTerminalBackend::closed() const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  return closed_;
}

void CmuxTerminalBackend::OnCmuxTuiConnectionChanged(bool connected,
                                                     const std::string& error) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (closed_) {
    return;
  }
  if (!connected) {
    ResetOutbound(false);
    binding_request_in_flight_ = false;
    reattach_scheduled_ = false;
    reset_surface_on_retry_ = false;
    retry_timer_.Stop();
    attachment_.reset();
    if (direct_attach_in_flight_ &&
        direct_attach_phase_ != DirectAttachPhase::kAttachingRenderer &&
        direct_attach_phase_ != DirectAttachPhase::kCancellingInputCutover) {
      CancelDirectAttach(
          direct_attach_attempt_,
          error.empty() ? "cmux-tui control connection lost" : error);
    }
    if (!error.empty()) {
      VLOG(1) << "cmux-terminal: control connection lost: " << error;
    }
    return;
  }
  if (recovery_.ActionForGeneration(client_->server_generation()) ==
      CmuxTerminalRecoveryAction::kResolve) {
    ResetOutbound(false);
    attachment_.reset();
    colors_.reset();
    requested_cols_ = 0;
    requested_rows_ = 0;
    reset_surface_on_retry_ = false;
  }
  if (recovery_.exited()) {
    EnsureExitedAttached();
    return;
  }
  EnsureSurface();
}

void CmuxTerminalBackend::OnCmuxTuiEvent(const CmuxTuiEvent& event) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  const std::optional<CmuxTuiSurfaceId> surface = CurrentSurface();
  if (closed_ || !surface || event.surface != *surface) {
    return;
  }
  if (event.type == CmuxTuiEvent::Type::kTitleChanged) {
    if (frontend_) {
      frontend_->OnCmuxTerminalTitle(event.title);
    }
    return;
  }
  if (event.type != CmuxTuiEvent::Type::kSurfaceExited) {
    return;
  }

  ObserveControlPlaneExit("terminal process exited");
}

void CmuxTerminalBackend::ObserveControlPlaneExit(std::string reason) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (closed_) {
    return;
  }
  if (direct_attached_ || direct_attach_in_flight_) {
    control_exit_pending_ = true;
    control_exit_reason_ = std::move(reason);
    direct_exit_timer_.Start(
        FROM_HERE, kDirectExitFallbackTimeout,
        base::BindOnce(&CmuxTerminalBackend::OnDirectExitFallback,
                       weak_factory_.GetWeakPtr()));
    return;
  }
  MarkExited(reason);
}

void CmuxTerminalBackend::OnDirectExitFallback() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (closed_ || !control_exit_pending_) {
    return;
  }
  std::string reason = control_exit_reason_.empty()
                           ? "terminal process exited (direct stream timeout)"
                           : std::move(control_exit_reason_);
  control_exit_pending_ = false;
  MarkExited(reason);
}

void CmuxTerminalBackend::EnsureSurface() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (closed_ || !frontend_ || !client_->ready()) {
    return;
  }
  const CmuxTerminalRecoveryAction action =
      recovery_.ActionForGeneration(client_->server_generation());
  if (action == CmuxTerminalRecoveryAction::kUseSurface) {
    if (frontend_suspended_) {
      return;
    }
    EnsureAttached();
    EnsureDirectAttached();
    SendResizeIfNeeded();
    FlushPendingInput();
    return;
  }
  // An ambiguous Create response must not strand a reserved identity in an
  // endless resolve-terminal loop. Replaying the exact same durable mutation
  // is safe: cmux-tui returns the original result if it committed, or performs
  // the reservation once if the request never arrived.
  if (action == CmuxTerminalRecoveryAction::kResolve && !create_pending_) {
    ResolveSurface();
    return;
  }
  if (action != CmuxTerminalRecoveryAction::kCreate &&
      action != CmuxTerminalRecoveryAction::kResolve) {
    return;
  }
  if (!create_pending_ || binding_request_in_flight_) {
    return;
  }
  binding_request_in_flight_ = true;
  recovery_.MarkCreateRequestSent();
  const CmuxTuiGridSize desired = resize_queue_.desired();
  const uint16_t initial_cols = desired.cols;
  const uint16_t initial_rows = desired.rows;
  const uint64_t request_server_generation = client_->server_generation();
  client_->CreateSurface(
      command_, name_, workspace_key_, *recovery_.stable_terminal_id(),
      terminal_mutation_id_, initial_cols, initial_rows,
      base::BindOnce(&CmuxTerminalBackend::OnSurfaceCreated,
                     base::RetainedRef(this), request_server_generation,
                     initial_cols, initial_rows));
}

void CmuxTerminalBackend::OnSurfaceCreated(
    uint64_t request_server_generation,
    uint16_t initial_cols,
    uint16_t initial_rows,
    std::optional<CmuxTerminalBinding> binding,
    const std::string& error) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  binding_request_in_flight_ = false;
  if (closed_) {
    if (binding) {
      // Replay the exact close fingerprint chosen when Close() ran. A close
      // during Launching intentionally uses the ID-only wildcard; changing it
      // to the newly returned incarnation under the same mutation ID would be
      // rejected as a non-idempotent replay.
      client_->CloseTerminal(binding->terminal_id,
                             recovery_.stable_incarnation(),
                             close_mutation_id_);
    }
    return;
  }
  if (!binding) {
    LOG(ERROR) << "cmux-terminal: failed to create cmux-tui surface: " << error;
    ScheduleReattach(error.empty() ? "cmux-tui surface creation failed"
                                   : error);
    return;
  }
  if (recovery_.exited()) {
    // A fenced canonical Exited event may overtake the create reply. The tab
    // already displays terminal state and must not be converted into a local
    // protocol-error close by the stale runtime lease.
    return;
  }
  const bool accepted =
      create_pending_
          ? recovery_.AcceptCreated(request_server_generation, *binding)
          : recovery_.AcceptResolved(request_server_generation, *binding);
  if (!accepted) {
    MarkClosed(
        "cmux-tui returned an invalid or duplicate durable terminal "
        "identity",
        true);
    return;
  }
  create_pending_ = false;
  requested_cols_ = initial_cols;
  requested_rows_ = initial_rows;
  EnsureSurface();
}

void CmuxTerminalBackend::ResolveSurface() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (closed_ || !frontend_ || !client_->ready() ||
      binding_request_in_flight_ || !recovery_.stable_terminal_id()) {
    return;
  }
  binding_request_in_flight_ = true;
  const uint64_t request_server_generation = client_->server_generation();
  client_->ResolveTerminal(
      *recovery_.stable_terminal_id(),
      base::BindOnce(&CmuxTerminalBackend::OnSurfaceResolved,
                     base::RetainedRef(this), request_server_generation));
}

void CmuxTerminalBackend::OnSurfaceResolved(
    uint64_t request_server_generation,
    std::optional<CmuxTerminalBinding> binding,
    const std::string& error) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  binding_request_in_flight_ = false;
  if (closed_) {
    return;
  }
  if (!binding) {
    // terminal_not_found is expected while a restarted daemon adopts utility
    // processes. Transport failures are equally retryable. Identity and
    // protocol-integrity failures are not: silently creating a replacement
    // here would orphan the surviving shell.
    if (error == "invalid_terminal_id" || error == "duplicate_terminal_id" ||
        error == "terminal_incarnation_mismatch" ||
        error == "invalid_terminal_incarnation" ||
        error == "cmux-tui returned an invalid terminal resolve binding") {
      recovery_.MarkTerminalError();
      MarkClosed(error, true);
      return;
    }
    ScheduleReattach(error.empty() ? "cmux-tui terminal resolve failed"
                                   : error);
    return;
  }
  if (!recovery_.AcceptResolved(request_server_generation, *binding)) {
    MarkClosed("cmux-tui resolved a different terminal incarnation", true);
    return;
  }
  create_pending_ = false;
  requested_cols_ = 0;
  requested_rows_ = 0;
  EnsureSurface();
}

std::optional<CmuxTuiSurfaceId> CmuxTerminalBackend::CurrentSurface() const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  return recovery_.SurfaceForGeneration(client_->server_generation());
}

std::optional<CmuxTuiSurfaceId>
CmuxTerminalBackend::CurrentRenderableSurface() const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (const std::optional<CmuxTuiSurfaceId> live = CurrentSurface()) {
    return live;
  }
  return recovery_.FinalSurfaceForGeneration(client_->server_generation());
}

void CmuxTerminalBackend::EnsureDirectAttached() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  // Only the macOS frontend implements the direct terminal renderer. A failed
  // cutover resets the compatibility attachment and requests another replay,
  // so attempting it on Linux continually rebuilds the visible terminal.
#if BUILDFLAG(IS_MAC)
  const std::optional<CmuxTuiSurfaceId> surface = CurrentSurface();
  if (closed_ || !frontend_ || frontend_suspended_ || !surface ||
      !client_->ready() ||
      direct_attached_ || direct_attach_in_flight_ ||
      direct_retry_timer_.IsRunning() || !recovery_.binding()) {
    return;
  }
  direct_attach_in_flight_ = true;
  const uint64_t attempt = ++direct_attach_attempt_;
  direct_attach_phase_ = DirectAttachPhase::kQuiescingInput;
  direct_attach_watchdog_timer_.Start(
      FROM_HERE, kDirectAttachTimeout,
      base::BindOnce(&CmuxTerminalBackend::OnDirectAttachWatchdog,
                     weak_factory_.GetWeakPtr(), attempt));
  frontend_->BeginCmuxTerminalHostInputCutover(
      attempt, base::BindOnce(&CmuxTerminalBackend::OnDirectInputCutoverReady,
                              base::RetainedRef(this), attempt));
#endif
}

void CmuxTerminalBackend::OnDirectInputCutoverReady(uint64_t attempt,
                                                    bool success,
                                                    std::string error) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (attempt != direct_attach_attempt_ || closed_ ||
      direct_attach_phase_ != DirectAttachPhase::kQuiescingInput) {
    return;
  }
  if (!success) {
    CancelDirectAttach(attempt,
                       error.empty() ? "renderer input quiesce failed" : error);
    return;
  }
  direct_attach_phase_ = DirectAttachPhase::kDrainingCompatibilityInput;
  if (input_queue_.write_in_flight() || input_queue_.pending_bytes() != 0) {
    FlushPendingInput();
    return;
  }
  MintDirectRendererAfterInputDrain(attempt);
}

void CmuxTerminalBackend::MintDirectRendererAfterInputDrain(uint64_t attempt) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (attempt != direct_attach_attempt_ || closed_ ||
      direct_attach_phase_ != DirectAttachPhase::kDrainingCompatibilityInput) {
    return;
  }
  if (input_queue_.write_in_flight() || input_queue_.pending_bytes() != 0) {
    return;
  }
  const std::optional<CmuxTuiSurfaceId> surface = CurrentSurface();
  if (!surface || !client_->ready() || !frontend_ || frontend_suspended_) {
    CancelDirectAttach(attempt, !frontend_ ? "terminal frontend disappeared"
                                : frontend_suspended_
                                    ? "terminal renderer is cold-suspended"
                                : !client_->ready()
                                    ? "cmux-tui control connection disappeared"
                                    : "cmux-tui runtime surface disappeared");
    return;
  }
  direct_attach_phase_ = DirectAttachPhase::kMintingCapability;
  // MintCapability is written after every drained Input frame on the same
  // terminal-host admin stream. Its response therefore proves the host has
  // written that complete compatibility prefix to the PTY before the renderer
  // capability can be attached on a second socket.
  client_->MintTerminalRenderer(
      *surface, kRendererCapabilityTtl,
      base::BindOnce(&CmuxTerminalBackend::OnRendererConnectionMinted,
                     base::RetainedRef(this), attempt));
}

void CmuxTerminalBackend::OnRendererConnectionMinted(
    uint64_t attempt,
    std::optional<CmuxTuiRendererConnection> connection,
    const std::string& error) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (attempt != direct_attach_attempt_ || closed_) {
    return;
  }
  if (direct_attach_phase_ != DirectAttachPhase::kMintingCapability) {
    return;
  }
  if (!connection || !frontend_ || frontend_suspended_ ||
      !recovery_.binding()) {
    CancelDirectAttach(
        attempt, error.empty() ? "terminal renderer grant failed" : error);
    return;
  }
  if (connection->terminal_id != recovery_.binding()->terminal_id ||
      connection->incarnation != recovery_.binding()->terminal_incarnation ||
      connection->rights != TerminalHostCapabilityRights::kRenderer) {
    CancelDirectAttach(attempt, "terminal renderer grant identity mismatch");
    return;
  }
  direct_attach_phase_ = DirectAttachPhase::kAttachingRenderer;
  frontend_->AttachCmuxTerminalHost(
      attempt, std::move(*connection),
      base::BindOnce(&CmuxTerminalBackend::OnDirectAttachComplete,
                     base::RetainedRef(this), attempt));
}

void CmuxTerminalBackend::OnDirectAttachComplete(uint64_t attempt,
                                                 bool success,
                                                 std::string error) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (attempt != direct_attach_attempt_ || closed_) {
    return;
  }
  if (direct_attach_phase_ != DirectAttachPhase::kAttachingRenderer) {
    return;
  }
  if (!success) {
    CancelDirectAttach(attempt,
                       error.empty() ? "direct terminal attach failed" : error);
    return;
  }
  direct_attach_in_flight_ = false;
  direct_attach_phase_ = DirectAttachPhase::kIdle;
  direct_attached_ = true;
  direct_attach_watchdog_timer_.Stop();
  attachment_.reset();
}

void CmuxTerminalBackend::CancelDirectAttach(uint64_t attempt,
                                             std::string reason) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (attempt != direct_attach_attempt_ || closed_ ||
      direct_attach_phase_ == DirectAttachPhase::kIdle ||
      direct_attach_phase_ == DirectAttachPhase::kCancellingInputCutover) {
    return;
  }
  direct_attach_phase_ = DirectAttachPhase::kCancellingInputCutover;
  direct_attach_watchdog_timer_.Start(
      FROM_HERE, kDirectCancelTimeout,
      base::BindOnce(&CmuxTerminalBackend::OnDirectAttachWatchdog,
                     weak_factory_.GetWeakPtr(), attempt));
  if (!frontend_) {
    direct_attach_in_flight_ = false;
    direct_attach_phase_ = DirectAttachPhase::kIdle;
    direct_attach_watchdog_timer_.Stop();
    ScheduleDirectAttach(reason);
    return;
  }
  frontend_->CancelCmuxTerminalHostInputCutover(
      attempt,
      base::BindOnce(&CmuxTerminalBackend::OnDirectInputCutoverCancelled,
                     base::RetainedRef(this), attempt, std::move(reason)));
}

void CmuxTerminalBackend::OnDirectInputCutoverCancelled(uint64_t attempt,
                                                        std::string reason) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (attempt != direct_attach_attempt_ || closed_ ||
      direct_attach_phase_ != DirectAttachPhase::kCancellingInputCutover) {
    return;
  }
  direct_attach_in_flight_ = false;
  direct_attach_phase_ = DirectAttachPhase::kIdle;
  direct_attached_ = false;
  direct_attach_watchdog_timer_.Stop();
  // Compatibility events were intentionally ignored by the renderer while a
  // candidate direct socket existed. Replace that potentially stale stream
  // with a fresh Snapshot before accepting it as the mirror again.
  attachment_.reset();
  EnsureAttached();
  FlushPendingInput();
  ScheduleDirectAttach(reason);
}

void CmuxTerminalBackend::OnDirectAttachWatchdog(uint64_t attempt) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (attempt != direct_attach_attempt_ || closed_ ||
      direct_attach_phase_ == DirectAttachPhase::kIdle) {
    return;
  }
  if (direct_attach_phase_ != DirectAttachPhase::kCancellingInputCutover) {
    CancelDirectAttach(attempt, "direct renderer attach timed out");
    return;
  }

  // A second timeout proves the utility did not even finish the ordered
  // Cancel RPC. Fence every late callback, replace only this terminal's
  // disposable renderer process, and resume from a fresh compatibility
  // Snapshot. The cmux-tui terminal host and child process remain alive.
  ++direct_attach_attempt_;
  direct_attach_in_flight_ = false;
  direct_attached_ = false;
  direct_attach_phase_ = DirectAttachPhase::kIdle;
  direct_attach_watchdog_timer_.Stop();
  attachment_.reset();
  if (frontend_) {
    frontend_->RestartCmuxTerminalRenderer(
        "direct renderer cancel timed out; restarting utility");
  }
  EnsureAttached();
  FlushPendingInput();
  ScheduleDirectAttach("direct renderer watchdog recovery");
}

void CmuxTerminalBackend::ScheduleDirectAttach(const std::string& reason) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (closed_ || frontend_suspended_ || recovery_.exited() || direct_attached_ ||
      direct_retry_timer_.IsRunning()) {
    return;
  }
  VLOG(1) << "cmux-terminal: retrying direct renderer: " << reason;
  direct_retry_timer_.Start(
      FROM_HERE, kReattachDelay,
      base::BindOnce(&CmuxTerminalBackend::EnsureDirectAttached,
                     weak_factory_.GetWeakPtr()));
}

void CmuxTerminalBackend::EnsureAttached() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  const std::optional<CmuxTuiSurfaceId> surface = CurrentSurface();
  if (closed_ || !frontend_ || frontend_suspended_ || !surface ||
      !client_->ready() || attachment_ || direct_attached_ ||
      reattach_scheduled_) {
    return;
  }
  const CmuxTuiGridSize desired = resize_queue_.desired();
  requested_cols_ = desired.cols;
  requested_rows_ = desired.rows;
  attachment_ = client_->AttachSurface(
      *surface, desired.cols, desired.rows,
      base::BindRepeating(&CmuxTerminalBackend::OnAttachmentEvent,
                          weak_factory_.GetWeakPtr()));
}

void CmuxTerminalBackend::EnsureExitedAttached() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  const std::optional<CmuxTuiSurfaceId> surface =
      recovery_.FinalSurfaceForGeneration(client_->server_generation());
  if (closed_ || !frontend_ || frontend_suspended_ || !surface ||
      !client_->ready() || attachment_) {
    return;
  }
  const CmuxTuiGridSize desired = resize_queue_.desired();
  attachment_ = client_->AttachSurface(
      *surface, desired.cols, desired.rows,
      base::BindRepeating(&CmuxTerminalBackend::OnAttachmentEvent,
                          weak_factory_.GetWeakPtr()));
}

void CmuxTerminalBackend::OnAttachmentEvent(CmuxTuiEvent event) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  const std::optional<CmuxTuiSurfaceId> surface = CurrentRenderableSurface();
  if (closed_ || frontend_suspended_ || reattach_scheduled_ || !surface ||
      event.surface != *surface) {
    return;
  }
  if (!event.error.empty() && (event.type == CmuxTuiEvent::Type::kVtState ||
                               event.type == CmuxTuiEvent::Type::kOutput ||
                               event.type == CmuxTuiEvent::Type::kResized)) {
    ScheduleReattach(event.error);
    return;
  }

  switch (event.type) {
    case CmuxTuiEvent::Type::kVtState:
    case CmuxTuiEvent::Type::kResized:
      if (event.cols == 0 || event.rows == 0) {
        ScheduleReattach("cmux-tui replay has an invalid zero-sized grid");
        return;
      }
      if (event.colors) {
        colors_ = event.colors;
      }
      ++replay_generation_;
      if (frontend_ && !frontend_suspended_) {
        frontend_->OnCmuxTerminalReplay(replay_generation_, event.cols,
                                        event.rows, base::span(event.bytes),
                                        colors_);
      }
      break;
    case CmuxTuiEvent::Type::kOutput:
      if (frontend_ && !frontend_suspended_) {
        frontend_->OnCmuxTerminalOutput(base::span(event.bytes));
      }
      break;
    case CmuxTuiEvent::Type::kColorsChanged:
      if (event.colors) {
        colors_ = event.colors;
      }
      if (frontend_ && !frontend_suspended_ && colors_) {
        frontend_->OnCmuxTerminalColors(*colors_);
      }
      break;
    case CmuxTuiEvent::Type::kDetached:
      ScheduleReattach("cmux-tui detached the surface stream");
      break;
    case CmuxTuiEvent::Type::kOverflow:
      ScheduleReattach(event.error.empty() ? "cmux-tui surface stream overflow"
                                           : event.error);
      break;
    case CmuxTuiEvent::Type::kDisconnected:
      ScheduleReattach(event.error.empty() ? "cmux-tui attach socket closed"
                                           : event.error);
      break;
    case CmuxTuiEvent::Type::kSurfaceMissing:
      if (recovery_.ActionForGeneration(client_->server_generation()) ==
          CmuxTerminalRecoveryAction::kUseSurface) {
        // The same server no longer has this surface: the process exited or
        // another frontend explicitly closed it. Recreating it here would
        // launch an unexpected replacement shell.
        ObserveControlPlaneExit(
            event.error.empty() ? "cmux-tui surface is missing" : event.error);
      } else {
        ScheduleReattach(
            event.error.empty() ? "cmux-tui surface is missing" : event.error,
            true);
      }
      break;
    default:
      break;
  }
}

void CmuxTerminalBackend::MarkExited(const std::string& reason) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  retry_timer_.Stop();
  direct_retry_timer_.Stop();
  direct_attach_watchdog_timer_.Stop();
  direct_exit_timer_.Stop();
  control_exit_pending_ = false;
  control_exit_reason_.clear();
  resize_timer_.Stop();
  reattach_scheduled_ = false;
  reset_surface_on_retry_ = false;
  attachment_.reset();
  const bool detach_direct = direct_attached_ || direct_attach_in_flight_;
  ++direct_attach_attempt_;
  direct_attach_in_flight_ = false;
  direct_attached_ = false;
  direct_attach_phase_ = DirectAttachPhase::kIdle;
  // Invalidate callbacks before Detach: the view may synchronously retire a
  // pending cutover completion while sending the asynchronous renderer RPC.
  if (frontend_ && detach_direct) {
    frontend_->DetachCmuxTerminalHost();
  }
  recovery_.MarkExited();
  ResetOutbound(true);
  if (frontend_) {
    frontend_->OnCmuxTerminalExited(reason);
  }
}

void CmuxTerminalBackend::MarkClosed(const std::string& reason,
                                     bool reset_attachment) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  retry_timer_.Stop();
  direct_retry_timer_.Stop();
  direct_attach_watchdog_timer_.Stop();
  direct_exit_timer_.Stop();
  control_exit_pending_ = false;
  control_exit_reason_.clear();
  reattach_scheduled_ = false;
  reset_surface_on_retry_ = false;
  if (reset_attachment) {
    attachment_.reset();
  }
  const bool detach_direct = direct_attached_ || direct_attach_in_flight_;
  ++direct_attach_attempt_;
  direct_attach_in_flight_ = false;
  direct_attached_ = false;
  direct_attach_phase_ = DirectAttachPhase::kIdle;
  if (frontend_ && detach_direct) {
    frontend_->DetachCmuxTerminalHost();
  }
  recovery_.InvalidateLocalSurface();
  ResetOutbound(true);
  closed_ = true;
  Frontend* frontend = frontend_;
  if (frontend) {
    // The callback normally schedules its owning browser tab for closure and
    // may release the final backend reference. Do not touch members after it.
    frontend->OnCmuxTerminalClosed(reason);
  }
}

void CmuxTerminalBackend::ScheduleReattach(const std::string& reason,
                                           bool reset_surface) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (closed_) {
    return;
  }
  reset_surface_on_retry_ |= reset_surface;
  if (reattach_scheduled_) {
    return;
  }
  reattach_scheduled_ = true;
  VLOG(1) << "cmux-terminal: reattaching mirror: " << reason;
  retry_timer_.Start(
      FROM_HERE, kReattachDelay,
      base::BindOnce(&CmuxTerminalBackend::ResetAttachmentAndRetry,
                     weak_factory_.GetWeakPtr()));
}

void CmuxTerminalBackend::ResetAttachmentAndRetry() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  reattach_scheduled_ = false;
  attachment_.reset();
  if (reset_surface_on_retry_) {
    reset_surface_on_retry_ = false;
    ResetOutbound(false);
    recovery_.InvalidateLocalSurface();
    colors_.reset();
    requested_cols_ = 0;
    requested_rows_ = 0;
  }
  if (recovery_.exited()) {
    EnsureExitedAttached();
    return;
  }
  EnsureSurface();
}

void CmuxTerminalBackend::SendResizeIfNeeded() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (closed_ || direct_attached_ || !CurrentSurface() || !attachment_ ||
      !client_->ready()) {
    return;
  }
  const std::optional<CmuxTuiGridSize> resize =
      resize_queue_.BeginWrite({requested_cols_, requested_rows_});
  if (!resize) {
    return;
  }
  requested_cols_ = resize->cols;
  requested_rows_ = resize->rows;
  const uint64_t request = ++resize_request_;
  attachment_->ResizeSurface(
      resize->cols, resize->rows,
      base::BindOnce(&CmuxTerminalBackend::OnResizeResult,
                     base::RetainedRef(this), request, resize->cols,
                     resize->rows));
}

void CmuxTerminalBackend::OnResizeResult(uint64_t request,
                                         uint16_t cols,
                                         uint16_t rows,
                                         CmuxTuiCommandResult result) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (request != resize_request_) {
    return;
  }
  resize_queue_.FinishWrite();
  if (closed_) {
    return;
  }
  if (!result.ok) {
    if (requested_cols_ == cols && requested_rows_ == rows) {
      requested_cols_ = 0;
      requested_rows_ = 0;
    }
    LOG(WARNING) << "cmux-terminal: resize failed: " << result.error;
    return;
  }
  if (resize_queue_.desired() !=
      CmuxTuiGridSize{requested_cols_, requested_rows_}) {
    resize_timer_.Start(FROM_HERE, kResizeDebounce,
                        base::BindOnce(&CmuxTerminalBackend::SendResizeIfNeeded,
                                       weak_factory_.GetWeakPtr()));
  }
}

void CmuxTerminalBackend::FlushPendingInput() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  const std::optional<CmuxTuiSurfaceId> surface = CurrentSurface();
  // The service can commit direct mode and reject a later batch before its
  // separate Attach response reaches Browser. Hold that returned batch until
  // Cancelled/Disconnected establishes compatibility again; sending it now
  // could reorder it ahead of an older accepted direct write on another
  // socket. The kDraining phase remains writable for the finite pre-cutover
  // compatibility prefix.
  if (closed_ || direct_attached_ ||
      direct_attach_phase_ == DirectAttachPhase::kAttachingRenderer ||
      direct_attach_phase_ == DirectAttachPhase::kCancellingInputCutover ||
      !surface || !client_->ready()) {
    return;
  }
  std::vector<uint8_t> input = input_queue_.BeginWrite();
  if (input.empty()) {
    return;
  }
  const uint64_t request = ++input_request_;
  client_->SendBytes(*surface, std::move(input),
                     base::BindOnce(&CmuxTerminalBackend::OnInputResult,
                                    base::RetainedRef(this), request));
}

void CmuxTerminalBackend::OnInputResult(uint64_t request,
                                        CmuxTuiCommandResult result) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (request != input_request_) {
    return;
  }
  input_queue_.FinishWrite();
  if (closed_) {
    return;
  }
  if (!result.ok) {
    LOG(WARNING) << "cmux-terminal: input send failed: " << result.error;
    if (direct_attach_phase_ ==
        DirectAttachPhase::kDrainingCompatibilityInput) {
      CancelDirectAttach(direct_attach_attempt_,
                         result.error.empty()
                             ? "compatibility input drain failed"
                             : result.error);
      return;
    }
  }
  if (direct_attach_phase_ == DirectAttachPhase::kDrainingCompatibilityInput &&
      input_queue_.pending_bytes() == 0) {
    MintDirectRendererAfterInputDrain(direct_attach_attempt_);
    return;
  }
  FlushPendingInput();
}

void CmuxTerminalBackend::ResetOutbound(bool clear_pending_input) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  ++input_request_;
  if (clear_pending_input) {
    input_queue_.Clear();
  } else {
    input_queue_.CancelWrite();
  }
  ++resize_request_;
  resize_timer_.Stop();
  if (resize_queue_.write_in_flight()) {
    // A disconnected resize may have committed without its response. Resend
    // the latest desired grid after reconnect; duplicate resize is harmless.
    requested_cols_ = 0;
    requested_rows_ = 0;
  }
  resize_queue_.CancelWrite();
}

}  // namespace cmux
