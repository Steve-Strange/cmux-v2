// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "chrome/browser/cmux_term/cmux_terminal_recovery.h"

#include <utility>

namespace cmux {

namespace {

bool IsCanonicalLifecycleTransitionAllowed(CmuxTerminalLifecycle from,
                                           CmuxTerminalLifecycle to) {
  if (from == to) {
    return true;
  }
  switch (from) {
    case CmuxTerminalLifecycle::kLaunching:
      return to == CmuxTerminalLifecycle::kAdopting ||
             to == CmuxTerminalLifecycle::kRunning ||
             to == CmuxTerminalLifecycle::kExited ||
             to == CmuxTerminalLifecycle::kTombstoned;
    case CmuxTerminalLifecycle::kAdopting:
      return to == CmuxTerminalLifecycle::kRunning ||
             to == CmuxTerminalLifecycle::kExited ||
             to == CmuxTerminalLifecycle::kTombstoned;
    case CmuxTerminalLifecycle::kRunning:
      // Losing only the daemon's admin stream moves the same live host back
      // through adoption. The immutable incarnation check below prevents this
      // recovery edge from accepting a replacement process.
      return to == CmuxTerminalLifecycle::kAdopting ||
             to == CmuxTerminalLifecycle::kExited ||
             to == CmuxTerminalLifecycle::kTombstoned;
    case CmuxTerminalLifecycle::kExited:
      return to == CmuxTerminalLifecycle::kTombstoned;
    case CmuxTerminalLifecycle::kTombstoned:
      return false;
  }
  return false;
}

}  // namespace

CmuxTerminalBinding::CmuxTerminalBinding() = default;
CmuxTerminalBinding::CmuxTerminalBinding(const CmuxTerminalBinding&) = default;
CmuxTerminalBinding& CmuxTerminalBinding::operator=(
    const CmuxTerminalBinding&) = default;
CmuxTerminalBinding::CmuxTerminalBinding(CmuxTerminalBinding&&) = default;
CmuxTerminalBinding& CmuxTerminalBinding::operator=(
    CmuxTerminalBinding&&) = default;
CmuxTerminalBinding::~CmuxTerminalBinding() = default;

bool CmuxTerminalBinding::operator==(const CmuxTerminalBinding& other) const {
  return surface == other.surface && terminal_id == other.terminal_id &&
         terminal_incarnation == other.terminal_incarnation;
}

CmuxTerminalRecovery::CmuxTerminalRecovery() = default;
CmuxTerminalRecovery::~CmuxTerminalRecovery() = default;

CmuxTerminalRecoveryAction CmuxTerminalRecovery::ActionForGeneration(
    uint64_t server_generation) const {
  if (server_generation == 0 || exited_ || terminal_error_) {
    return CmuxTerminalRecoveryAction::kNone;
  }
  if (!binding_) {
    return reserved_terminal_id_ && create_request_sent_
               ? CmuxTerminalRecoveryAction::kResolve
               : CmuxTerminalRecoveryAction::kCreate;
  }
  if (binding_generation_ != server_generation) {
    return CmuxTerminalRecoveryAction::kResolve;
  }
  return CmuxTerminalRecoveryAction::kUseSurface;
}

bool CmuxTerminalRecovery::ReserveTerminalId(
    const TerminalHostId& terminal_id) {
  if (!IsValidTerminalHostUuidV4(terminal_id) || terminal_error_ || exited_ ||
      (reserved_terminal_id_ && *reserved_terminal_id_ != terminal_id)) {
    MarkTerminalError();
    return false;
  }
  reserved_terminal_id_ = terminal_id;
  return true;
}

void CmuxTerminalRecovery::MarkCreateRequestSent() {
  if (reserved_terminal_id_ && !binding_ && !terminal_error_ && !exited_) {
    create_request_sent_ = true;
  }
}

bool CmuxTerminalRecovery::SeedKnownTerminal(
    const TerminalHostId& terminal_id,
    std::optional<TerminalHostIncarnation> terminal_incarnation,
    std::optional<CmuxTuiSurfaceId> surface,
    uint64_t server_generation,
    CmuxTerminalLifecycle lifecycle) {
  const bool has_runtime_lease = surface.has_value();
  if (binding_ || reserved_terminal_id_ || create_request_sent_ || exited_ ||
      terminal_error_ || !IsValidTerminalHostUuidV4(terminal_id) ||
      (terminal_incarnation &&
       !IsValidTerminalHostUuidV4(*terminal_incarnation)) ||
      (has_runtime_lease &&
       (!terminal_incarnation || *surface == 0 || server_generation == 0)) ||
      (!has_runtime_lease && server_generation != 0) ||
      (lifecycle != CmuxTerminalLifecycle::kRunning && has_runtime_lease)) {
    MarkTerminalError();
    return false;
  }

  reserved_terminal_id_ = terminal_id;
  stable_incarnation_ = terminal_incarnation;
  lifecycle_ = lifecycle;
  create_request_sent_ = true;
  if (has_runtime_lease) {
    CmuxTerminalBinding binding;
    binding.surface = *surface;
    binding.terminal_id = terminal_id;
    binding.terminal_incarnation = *terminal_incarnation;
    binding_ = std::move(binding);
    binding_generation_ = server_generation;
  }
  if (lifecycle == CmuxTerminalLifecycle::kExited ||
      lifecycle == CmuxTerminalLifecycle::kTombstoned) {
    exited_ = true;
    if (lifecycle == CmuxTerminalLifecycle::kTombstoned) {
      binding_generation_ = 0;
    }
  }
  return true;
}

bool CmuxTerminalRecovery::ApplyCanonicalLifecycle(
    CmuxTerminalLifecycle lifecycle,
    std::optional<TerminalHostIncarnation> terminal_incarnation) {
  if (!reserved_terminal_id_ || terminal_error_ ||
      (terminal_incarnation &&
       !IsValidTerminalHostUuidV4(*terminal_incarnation))) {
    return false;
  }
  if (stable_incarnation_ && terminal_incarnation &&
      *stable_incarnation_ != *terminal_incarnation) {
    MarkTerminalError();
    return false;
  }
  if (lifecycle_ &&
      !IsCanonicalLifecycleTransitionAllowed(*lifecycle_, lifecycle)) {
    return false;
  }
  if (terminal_incarnation) {
    stable_incarnation_ = *terminal_incarnation;
  }
  lifecycle_ = lifecycle;
  create_request_sent_ = true;
  if (lifecycle == CmuxTerminalLifecycle::kExited ||
      lifecycle == CmuxTerminalLifecycle::kTombstoned) {
    exited_ = true;
    if (lifecycle == CmuxTerminalLifecycle::kTombstoned) {
      binding_generation_ = 0;
    }
  }
  return true;
}

bool CmuxTerminalRecovery::AcceptCreated(
    uint64_t server_generation,
    const CmuxTerminalBinding& binding) {
  if (server_generation == 0 || binding_ || exited_ || terminal_error_ ||
      !IsValidBinding(binding) ||
      (reserved_terminal_id_ &&
       *reserved_terminal_id_ != binding.terminal_id) ||
      stable_incarnation_) {
    MarkTerminalError();
    return false;
  }
  binding_ = binding;
  reserved_terminal_id_ = binding.terminal_id;
  stable_incarnation_ = binding.terminal_incarnation;
  lifecycle_ = CmuxTerminalLifecycle::kRunning;
  binding_generation_ = server_generation;
  return true;
}

bool CmuxTerminalRecovery::AcceptResolved(
    uint64_t server_generation,
    const CmuxTerminalBinding& binding) {
  if (server_generation == 0 || !reserved_terminal_id_ || exited_ ||
      terminal_error_ || !IsValidBinding(binding) ||
      binding.terminal_id != *reserved_terminal_id_ ||
      (stable_incarnation_ &&
       binding.terminal_incarnation != *stable_incarnation_)) {
    MarkTerminalError();
    return false;
  }
  binding_ = binding;
  reserved_terminal_id_ = binding.terminal_id;
  stable_incarnation_ = binding.terminal_incarnation;
  lifecycle_ = CmuxTerminalLifecycle::kRunning;
  binding_generation_ = server_generation;
  return true;
}

void CmuxTerminalRecovery::InvalidateLocalSurface() {
  binding_generation_ = 0;
}

void CmuxTerminalRecovery::MarkExited() {
  exited_ = true;
  if (lifecycle_ == CmuxTerminalLifecycle::kTombstoned) {
    binding_generation_ = 0;
  } else {
    lifecycle_ = CmuxTerminalLifecycle::kExited;
  }
}

void CmuxTerminalRecovery::MarkTerminalError() {
  terminal_error_ = true;
  binding_generation_ = 0;
}

std::optional<CmuxTuiSurfaceId> CmuxTerminalRecovery::SurfaceForGeneration(
    uint64_t server_generation) const {
  if (!binding_ || server_generation == 0 || exited_ || terminal_error_ ||
      binding_generation_ != server_generation) {
    return std::nullopt;
  }
  return binding_->surface;
}

std::optional<CmuxTuiSurfaceId>
CmuxTerminalRecovery::FinalSurfaceForGeneration(
    uint64_t server_generation) const {
  if (!binding_ || server_generation == 0 || !exited_ || terminal_error_ ||
      binding_generation_ != server_generation) {
    return std::nullopt;
  }
  return binding_->surface;
}

// UUIDv4 validation also rejects all-zero/default identities.
bool CmuxTerminalRecovery::IsValidBinding(
    const CmuxTerminalBinding& binding) {
  return binding.surface != 0 &&
         IsValidTerminalHostUuidV4(binding.terminal_id) &&
         IsValidTerminalHostUuidV4(binding.terminal_incarnation);
}

}  // namespace cmux
