// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "chrome/services/cmux_terminal_renderer/public/cpp/cmux_terminal_input_cutover.h"

#include <iostream>
#include <string>

namespace {

int checks = 0;
int failures = 0;

void Check(bool condition, const char* message) {
  ++checks;
  if (!condition) {
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
  }
}

std::string Bytes(const std::vector<uint8_t>& bytes) {
  return std::string(bytes.begin(), bytes.end());
}

}  // namespace

int main() {
  using Result = cmux::TerminalInputCutover::RouteResult;

  cmux::TerminalInputCutover cutover(8);
  Check(cutover.RouteInput("A") == Result::kCompatibility,
        "input begins on the compatibility route");
  Check(!cutover.Begin(0), "zero is not a valid cutover id");
  Check(cutover.Begin(7) && cutover.buffering(),
        "a nonzero cutover enters buffered mode");
  Check(cutover.Begin(7), "the active cutover id is idempotent");
  Check(!cutover.Begin(8), "a newer id cannot replace an active cutover");

  Check(cutover.RouteInput("mouse-") == Result::kBuffered &&
            cutover.RouteInput("up") == Result::kBuffered &&
            Bytes(cutover.buffered_input()) == "mouse-up",
        "ordered writes coalesce behind the quiesced boundary");
  Check(cutover.RouteInput("!") == Result::kOverflow &&
            Bytes(cutover.buffered_input()) == "mouse-up",
        "overflow preserves the complete existing prefix");
  Check(!cutover.CanCommit(8) && !cutover.Commit(8),
        "a stale id cannot commit buffered input");

  const auto cancelled = cutover.Cancel(7);
  Check(cancelled && Bytes(*cancelled) == "mouse-up" && !cutover.buffering(),
        "cancel returns bytes before restoring compatibility");
  Check(cutover.RouteInput("B") == Result::kCompatibility,
        "cancelled input resumes on compatibility");
  Check(!cutover.Cancel(7), "cancel is exactly once");

  Check(cutover.Begin(9) && cutover.RouteInput("key") == Result::kBuffered &&
            cutover.CanCommit(9),
        "a fresh transition can buffer after cancellation");
  Check(
      cutover.Commit(9) && cutover.direct() && cutover.buffered_input().empty(),
      "commit clears the accepted prefix and enables direct input");
  Check(cutover.RouteInput("C") == Result::kDirect,
        "post-commit input stays on the direct route");
  Check(!cutover.Begin(10), "direct mode rejects a second cutover");

  Check(cutover.FallBackToCompatibility().empty() && !cutover.direct() &&
            cutover.RouteInput("D") == Result::kCompatibility,
        "socket loss restores compatibility without synthetic bytes");
  Check(cutover.Begin(11) && cutover.RouteInput("release") == Result::kBuffered,
        "a later transition retains a mouse release");
  Check(Bytes(cutover.FallBackToCompatibility()) == "release" &&
            cutover.RouteInput("E") == Result::kCompatibility,
        "buffered socket loss returns the release before fallback");
  Check(cutover.Begin(12) && cutover.cutover_id() == 12,
        "attach-time socket loss cannot strand a stale cutover id");

  if (failures == 0) {
    std::cout << "Terminal input cutover checks passed (" << checks << ")\n";
  }
  return failures == 0 ? 0 : 1;
}
