// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef CHROME_BROWSER_CMUX_TERM_CMUX_TERMINAL_HOST_CONNECTION_H_
#define CHROME_BROWSER_CMUX_TERM_CMUX_TERMINAL_HOST_CONNECTION_H_

#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <string>

#include "chrome/services/cmux_terminal_renderer/public/cpp/cmux_terminal_host_protocol.h"

namespace cmux {

// Thread-safe cancellation shared between the browser UI sequence and the
// blocking worker. Polls are sliced so cancellation closes a pending local
// connect or handshake promptly rather than waiting for the full timeout.
class TerminalHostConnectCancellation {
 public:
  TerminalHostConnectCancellation();
  TerminalHostConnectCancellation(const TerminalHostConnectCancellation&) =
      delete;
  TerminalHostConnectCancellation& operator=(
      const TerminalHostConnectCancellation&) = delete;
  ~TerminalHostConnectCancellation();

  void Cancel();
  bool cancelled() const;

 private:
  std::atomic_bool cancelled_{false};
};

enum class TerminalHostConnectError {
  kNone,
  kUnsupportedPlatform,
  kCancelled,
  kTimedOut,
  kInvalidEndpointPath,
  kInvalidEndpointType,
  kInvalidEndpointOwner,
  kInvalidEndpointPermissions,
  kSocketFailed,
  kConnectFailed,
  kPeerCredentialMismatch,
  kWriteFailed,
  kReadFailed,
  kInvalidHostHello,
  kIdentityMismatch,
  kRightsMismatch,
};

struct TerminalHostConnectResult;

// Owns an authenticated terminal-host stream positioned exactly before the
// initial Snapshot frame. The descriptor remains CLOEXEC and nonblocking for
// transfer into the renderer utility process.
class TerminalHostAuthenticatedSocket {
 public:
  TerminalHostAuthenticatedSocket();
  TerminalHostAuthenticatedSocket(const TerminalHostAuthenticatedSocket&) =
      delete;
  TerminalHostAuthenticatedSocket& operator=(
      const TerminalHostAuthenticatedSocket&) = delete;
  TerminalHostAuthenticatedSocket(TerminalHostAuthenticatedSocket&& other);
  TerminalHostAuthenticatedSocket& operator=(
      TerminalHostAuthenticatedSocket&& other);
  ~TerminalHostAuthenticatedSocket();

  bool valid() const { return descriptor_ >= 0; }
  int descriptor() const { return descriptor_; }
  int TakeDescriptor();
  const TerminalHostId& terminal_id() const { return terminal_id_; }
  const TerminalHostIncarnation& incarnation() const { return incarnation_; }
  TerminalHostCapabilityRights rights() const { return rights_; }
  uint32_t protocol_flags() const { return protocol_flags_; }

 private:
  friend struct TerminalHostConnectResult;
  friend TerminalHostConnectResult ConnectAndAuthenticateTerminalHost(
      const TerminalHostRendererGrant&,
      std::chrono::milliseconds,
      const std::shared_ptr<TerminalHostConnectCancellation>&);

  TerminalHostAuthenticatedSocket(int descriptor,
                                  TerminalHostId terminal_id,
                                  TerminalHostIncarnation incarnation,
                                  TerminalHostCapabilityRights rights,
                                  uint32_t protocol_flags);
  void Reset();

  int descriptor_ = -1;
  TerminalHostId terminal_id_{};
  TerminalHostIncarnation incarnation_{};
  TerminalHostCapabilityRights rights_ = TerminalHostCapabilityRights::kNone;
  uint32_t protocol_flags_ = 0;
};

struct TerminalHostConnectResult {
  TerminalHostConnectResult();
  TerminalHostConnectResult(const TerminalHostConnectResult&) = delete;
  TerminalHostConnectResult& operator=(const TerminalHostConnectResult&) =
      delete;
  TerminalHostConnectResult(TerminalHostConnectResult&&);
  TerminalHostConnectResult& operator=(TerminalHostConnectResult&&);
  ~TerminalHostConnectResult();

  std::optional<TerminalHostAuthenticatedSocket> socket;
  TerminalHostConnectError error = TerminalHostConnectError::kNone;
  std::string message;
};

TerminalHostConnectResult ConnectAndAuthenticateTerminalHost(
    const TerminalHostRendererGrant& grant,
    std::chrono::milliseconds timeout,
    const std::shared_ptr<TerminalHostConnectCancellation>& cancellation);

const char* TerminalHostConnectErrorMessage(TerminalHostConnectError error);

}  // namespace cmux

#endif  // CHROME_BROWSER_CMUX_TERM_CMUX_TERMINAL_HOST_CONNECTION_H_
