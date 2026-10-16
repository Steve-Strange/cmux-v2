// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef CHROME_SERVICES_CMUX_TERMINAL_RENDERER_CMUX_TERMINAL_TICK_COALESCER_H_
#define CHROME_SERVICES_CMUX_TERMINAL_RENDERER_CMUX_TERMINAL_TICK_COALESCER_H_

#include <atomic>
#include <cstdint>

namespace cmux {

// Coalesces arbitrary-thread Ghostty wakeups into at most one scheduled Tick
// task. A wakeup concurrent with the end of Tick is owned by exactly one of
// RequestTickTask() or EndTickTask(), so it cannot be lost or double-posted.
class CmuxTerminalTickCoalescer {
 public:
  CmuxTerminalTickCoalescer() = default;
  CmuxTerminalTickCoalescer(const CmuxTerminalTickCoalescer&) = delete;
  CmuxTerminalTickCoalescer& operator=(const CmuxTerminalTickCoalescer&) =
      delete;

  // Records a wakeup and returns true only to the caller responsible for
  // posting the next Tick task.
  bool RequestTickTask() {
    const uint8_t previous = state_.fetch_or(
        kTaskScheduled | kTickRequested, std::memory_order_acq_rel);
    return (previous & kTaskScheduled) == 0;
  }

  // Consumes every request that happened before this scheduled task began.
  // Wakeups after this point remain set for EndTickTask().
  void BeginTickTask() {
    state_.fetch_and(static_cast<uint8_t>(~kTickRequested),
                     std::memory_order_acq_rel);
  }

  // Returns true when a wakeup arrived during Tick and the current task must
  // post one successor. Otherwise, atomically retires the scheduling latch.
  // A racing RequestTickTask() either makes this CAS retry and is inherited by
  // the current task, or observes the retired latch and posts the successor.
  bool EndTickTask() {
    uint8_t state = state_.load(std::memory_order_acquire);
    for (;;) {
      if ((state & kTickRequested) != 0) {
        return true;
      }
      if (state_.compare_exchange_weak(state, 0, std::memory_order_acq_rel,
                                       std::memory_order_acquire)) {
        return false;
      }
    }
  }

 private:
  static constexpr uint8_t kTaskScheduled = 1 << 0;
  static constexpr uint8_t kTickRequested = 1 << 1;

  std::atomic<uint8_t> state_{0};
};

}  // namespace cmux

#endif  // CHROME_SERVICES_CMUX_TERMINAL_RENDERER_CMUX_TERMINAL_TICK_COALESCER_H_
