// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef CHROME_BROWSER_CMUX_TERM_CMUX_TERMINAL_RECOVERY_H_
#define CHROME_BROWSER_CMUX_TERM_CMUX_TERMINAL_RECOVERY_H_

#include <cstdint>
#include <optional>

#include "chrome/browser/cmux_term/cmux_terminal_lifecycle.h"
#include "chrome/services/cmux_terminal_renderer/public/cpp/cmux_terminal_host_protocol.h"

namespace cmux {

using CmuxTuiSurfaceId = uint64_t;

// The durable identity returned by terminal creation. `surface` is only a
// daemon-local routing number; terminal_id + incarnation identify the same
// utility process across cmux-tui daemon restarts.
struct CmuxTerminalBinding {
  CmuxTerminalBinding();
  CmuxTerminalBinding(const CmuxTerminalBinding&);
  CmuxTerminalBinding& operator=(const CmuxTerminalBinding&);
  CmuxTerminalBinding(CmuxTerminalBinding&&);
  CmuxTerminalBinding& operator=(CmuxTerminalBinding&&);
  ~CmuxTerminalBinding();

  CmuxTuiSurfaceId surface = 0;
  TerminalHostId terminal_id{};
  TerminalHostIncarnation terminal_incarnation{};

  bool operator==(const CmuxTerminalBinding& other) const;
};

enum class CmuxTerminalRecoveryAction {
  // The terminal is exited/invalid, or no control request is needed.
  kNone,
  // Creation is legal only before a durable identity has ever been learned.
  kCreate,
  // Resolve the durable identity into this daemon generation's surface id.
  kResolve,
  // The daemon-local surface is already valid for this generation.
  kUseSurface,
};

// Pure state machine that prevents daemon reconnects from launching a second
// shell. It deliberately retains durable identity when the daemon-local
// surface becomes stale.
class CmuxTerminalRecovery {
 public:
  CmuxTerminalRecovery();
  CmuxTerminalRecovery(const CmuxTerminalRecovery&) = delete;
  CmuxTerminalRecovery& operator=(const CmuxTerminalRecovery&) = delete;
  ~CmuxTerminalRecovery();

  CmuxTerminalRecoveryAction ActionForGeneration(
      uint64_t server_generation) const;

  // Reserves the canonical id before the create RPC. Once a request using it
  // may have reached cmux-tui, recovery resolves/replays that id and can never
  // issue an unrelated create after an ambiguous response.
  bool ReserveTerminalId(const TerminalHostId& terminal_id);
  void MarkCreateRequestSent();

  // Seeds a browser projection from canonical registry state. A seeded id is
  // never eligible for an unrelated create: without a current runtime surface
  // it is resolved until cmux-tui supplies the matching incarnation/lease.
  bool SeedKnownTerminal(
      const TerminalHostId& terminal_id,
      std::optional<TerminalHostIncarnation> terminal_incarnation,
      std::optional<CmuxTuiSurfaceId> surface,
      uint64_t server_generation,
      CmuxTerminalLifecycle lifecycle);

  // Applies a newer canonical registry lifecycle without changing identity or
  // making Create legal. Incarnation is learned once and then immutable.
  bool ApplyCanonicalLifecycle(
      CmuxTerminalLifecycle lifecycle,
      std::optional<TerminalHostIncarnation> terminal_incarnation);

  // Accepts the first creation result. Calling this after identity has been
  // assigned is a terminal protocol error, even if all fields happen to match.
  bool AcceptCreated(uint64_t server_generation,
                     const CmuxTerminalBinding& binding);

  // Rebinds only when the daemon echoes the exact durable identity. A changed
  // daemon-local surface is expected; a changed incarnation is not.
  bool AcceptResolved(uint64_t server_generation,
                      const CmuxTerminalBinding& binding);

  // Drops only the daemon-local routing lease. Stable identity survives.
  void InvalidateLocalSurface();
  void MarkExited();
  void MarkTerminalError();

  std::optional<CmuxTuiSurfaceId> SurfaceForGeneration(
      uint64_t server_generation) const;
  // An exited terminal may still have a final, read-only VT snapshot in the
  // daemon generation that observed its exit. This lease is never eligible
  // for input, resize, or a direct renderer capability; it exists solely so a
  // rematerialized frontend can inspect the final frame.
  std::optional<CmuxTuiSurfaceId> FinalSurfaceForGeneration(
      uint64_t server_generation) const;
  const std::optional<CmuxTerminalBinding>& binding() const {
    return binding_;
  }
  const std::optional<TerminalHostId>& stable_terminal_id() const {
    return reserved_terminal_id_;
  }
  const std::optional<TerminalHostIncarnation>& stable_incarnation() const {
    return stable_incarnation_;
  }
  bool has_stable_identity() const {
    return reserved_terminal_id_.has_value();
  }
  bool exited() const { return exited_; }
  bool terminal_error() const { return terminal_error_; }
  const std::optional<CmuxTerminalLifecycle>& lifecycle() const {
    return lifecycle_;
  }

 private:
  static bool IsValidBinding(const CmuxTerminalBinding& binding);

  std::optional<CmuxTerminalBinding> binding_;
  std::optional<TerminalHostId> reserved_terminal_id_;
  std::optional<TerminalHostIncarnation> stable_incarnation_;
  std::optional<CmuxTerminalLifecycle> lifecycle_;
  uint64_t binding_generation_ = 0;
  bool create_request_sent_ = false;
  bool exited_ = false;
  bool terminal_error_ = false;
};

}  // namespace cmux

#endif  // CHROME_BROWSER_CMUX_TERM_CMUX_TERMINAL_RECOVERY_H_
