// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "chrome/services/cmux_terminal_renderer/public/cpp/cmux_terminal_input_cutover.h"

#include <utility>

namespace cmux {

TerminalInputCutover::TerminalInputCutover(size_t max_buffered_bytes)
    : max_buffered_bytes_(max_buffered_bytes) {}

TerminalInputCutover::~TerminalInputCutover() = default;

bool TerminalInputCutover::Begin(uint64_t cutover_id) {
  if (cutover_id == 0) {
    return false;
  }
  if (route_ == Route::kBuffered) {
    return cutover_id_ == cutover_id;
  }
  if (route_ != Route::kCompatibility) {
    return false;
  }
  cutover_id_ = cutover_id;
  buffered_input_.clear();
  route_ = Route::kBuffered;
  return true;
}

TerminalInputCutover::RouteResult TerminalInputCutover::RouteInput(
    std::string_view bytes) {
  if (route_ == Route::kCompatibility) {
    return RouteResult::kCompatibility;
  }
  if (route_ == Route::kDirect) {
    return RouteResult::kDirect;
  }
  if (buffered_input_.size() > max_buffered_bytes_ ||
      bytes.size() > max_buffered_bytes_ - buffered_input_.size()) {
    return RouteResult::kOverflow;
  }
  buffered_input_.insert(buffered_input_.end(), bytes.begin(), bytes.end());
  return RouteResult::kBuffered;
}

bool TerminalInputCutover::CanCommit(uint64_t cutover_id) const {
  return route_ == Route::kBuffered && cutover_id_ == cutover_id;
}

bool TerminalInputCutover::Commit(uint64_t cutover_id) {
  if (!CanCommit(cutover_id)) {
    return false;
  }
  buffered_input_.clear();
  route_ = Route::kDirect;
  return true;
}

std::optional<std::vector<uint8_t>> TerminalInputCutover::Cancel(
    uint64_t cutover_id) {
  if (!CanCommit(cutover_id)) {
    return std::nullopt;
  }
  std::vector<uint8_t> buffered;
  buffered.swap(buffered_input_);
  route_ = Route::kCompatibility;
  cutover_id_ = 0;
  return buffered;
}

std::vector<uint8_t> TerminalInputCutover::FallBackToCompatibility() {
  std::vector<uint8_t> buffered;
  buffered.swap(buffered_input_);
  route_ = Route::kCompatibility;
  cutover_id_ = 0;
  return buffered;
}

}  // namespace cmux
