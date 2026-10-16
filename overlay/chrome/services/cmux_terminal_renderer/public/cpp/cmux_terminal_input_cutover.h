// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef CHROME_SERVICES_CMUX_TERMINAL_RENDERER_PUBLIC_CPP_CMUX_TERMINAL_INPUT_CUTOVER_H_
#define CHROME_SERVICES_CMUX_TERMINAL_RENDERER_PUBLIC_CPP_CMUX_TERMINAL_INPUT_CUTOVER_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace cmux {

// Pure state for the compatibility-Mojo -> terminal-host input handoff. The
// renderer emits its quiesced marker only after Begin() succeeds. Every later
// libghostty write is retained here until the browser has drained its finite
// compatibility queue and minted a renderer capability on the same admin
// stream (the mint response is the host-side ordering barrier).
class TerminalInputCutover {
 public:
  enum class RouteResult {
    kCompatibility,
    kBuffered,
    kDirect,
    kOverflow,
  };

  explicit TerminalInputCutover(size_t max_buffered_bytes);
  TerminalInputCutover(const TerminalInputCutover&) = delete;
  TerminalInputCutover& operator=(const TerminalInputCutover&) = delete;
  ~TerminalInputCutover();

  // Starts one transition. Repeating the active id is idempotent so a Mojo
  // retry can request another same-pipe quiesced marker without losing bytes.
  bool Begin(uint64_t cutover_id);

  // Classifies one ordered libghostty write and retains it only while the
  // transition is quiesced. kOverflow never mutates the existing buffer.
  RouteResult RouteInput(std::string_view bytes);

  bool CanCommit(uint64_t cutover_id) const;
  const std::vector<uint8_t>& buffered_input() const { return buffered_input_; }

  // Commit must be called only after buffered_input() has been accepted by the
  // direct socket. It then makes all subsequent writes direct.
  bool Commit(uint64_t cutover_id);

  // Cancellation returns every buffered byte and restores compatibility.
  // A stale id cannot cancel a newer transition.
  std::optional<std::vector<uint8_t>> Cancel(uint64_t cutover_id);

  // Used when the socket disappears independently of a browser request.
  // Direct mode has no retained bytes; buffered mode returns its complete
  // prefix so the service can publish it before a cancellation marker.
  std::vector<uint8_t> FallBackToCompatibility();

  bool buffering() const { return route_ == Route::kBuffered; }
  bool direct() const { return route_ == Route::kDirect; }
  uint64_t cutover_id() const { return cutover_id_; }

 private:
  enum class Route {
    kCompatibility,
    kBuffered,
    kDirect,
  };

  const size_t max_buffered_bytes_;
  Route route_ = Route::kCompatibility;
  uint64_t cutover_id_ = 0;
  std::vector<uint8_t> buffered_input_;
};

}  // namespace cmux

#endif  // CHROME_SERVICES_CMUX_TERMINAL_RENDERER_PUBLIC_CPP_CMUX_TERMINAL_INPUT_CUTOVER_H_
