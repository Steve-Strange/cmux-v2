// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef CHROME_SERVICES_CMUX_TERMINAL_RENDERER_CMUX_TERMINAL_OUTPUT_DRAIN_QUEUE_H_
#define CHROME_SERVICES_CMUX_TERMINAL_RENDERER_CMUX_TERMINAL_OUTPUT_DRAIN_QUEUE_H_

#include <cstddef>
#include <deque>
#include <utility>

namespace cmux {

// Externally synchronized FIFO and scheduling latch for the terminal output
// worker. Push() and RetireIfEmpty() must be serialized by the same lock. That
// makes a producer racing drain retirement choose exactly one outcome: either
// the current drain inherits its work, or the producer owns one successor
// task.
template <typename Work>
class CmuxTerminalOutputDrainQueue {
 public:
  CmuxTerminalOutputDrainQueue() = default;
  CmuxTerminalOutputDrainQueue(const CmuxTerminalOutputDrainQueue&) = delete;
  CmuxTerminalOutputDrainQueue& operator=(
      const CmuxTerminalOutputDrainQueue&) = delete;

  // Adds work in FIFO order and returns true only to the producer responsible
  // for posting a drain task.
  bool Push(Work work) {
    pending_.push_back(std::move(work));
    if (drain_task_scheduled_) {
      return false;
    }
    drain_task_scheduled_ = true;
    return true;
  }

  // Atomically retires the current drain when no work remains. A false result
  // means the caller must keep draining the inherited work.
  bool RetireIfEmpty() {
    if (!pending_.empty()) {
      return false;
    }
    drain_task_scheduled_ = false;
    return true;
  }

  // The caller must establish !empty() under the external lock.
  Work TakeFront() {
    Work work = std::move(pending_.front());
    pending_.pop_front();
    return work;
  }

  // The caller must establish !empty() under the external lock.
  const Work& front() const { return pending_.front(); }

  bool empty() const { return pending_.empty(); }
  size_t size() const { return pending_.size(); }
  bool drain_task_scheduled() const { return drain_task_scheduled_; }

  // Called only after the worker has stopped. Returning the abandoned items
  // lets the owner release byte reservations without running their actions.
  std::deque<Work> TakeAllForShutdown() {
    std::deque<Work> pending = std::move(pending_);
    pending_.clear();
    drain_task_scheduled_ = false;
    return pending;
  }

 private:
  std::deque<Work> pending_;
  bool drain_task_scheduled_ = false;
};

}  // namespace cmux

#endif  // CHROME_SERVICES_CMUX_TERMINAL_RENDERER_CMUX_TERMINAL_OUTPUT_DRAIN_QUEUE_H_
