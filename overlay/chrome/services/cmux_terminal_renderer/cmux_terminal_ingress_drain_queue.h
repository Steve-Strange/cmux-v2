// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef CHROME_SERVICES_CMUX_TERMINAL_RENDERER_CMUX_TERMINAL_INGRESS_DRAIN_QUEUE_H_
#define CHROME_SERVICES_CMUX_TERMINAL_RENDERER_CMUX_TERMINAL_INGRESS_DRAIN_QUEUE_H_

#include <cstddef>
#include <deque>
#include <utility>

namespace cmux {

// Externally synchronized bounded FIFO and scheduling latch for callbacks
// entering the renderer from a terminal-host socket worker. Work must expose:
//
//   size_t ByteCount() const;
//   bool IsLifecycle() const;
//   bool TryCoalesce(Work* newer, size_t max_combined_bytes);
//
// TryCoalesce may consume `newer` only when it returns true. Lifecycle work
// can use a small reserved item slot but remains subject to the byte bound.
template <typename Work>
class CmuxTerminalIngressDrainQueue {
 public:
  enum class PushResult {
    kRejected,
    kAccepted,
    kScheduleDrain,
  };

  enum class FinishSliceResult {
    kRetired,
    kRepost,
    kOverflow,
  };

  CmuxTerminalIngressDrainQueue(size_t max_items,
                                size_t max_bytes,
                                size_t lifecycle_item_reserve)
      : max_items_(max_items),
        max_bytes_(max_bytes),
        lifecycle_item_reserve_(lifecycle_item_reserve) {}
  CmuxTerminalIngressDrainQueue(const CmuxTerminalIngressDrainQueue&) = delete;
  CmuxTerminalIngressDrainQueue& operator=(
      const CmuxTerminalIngressDrainQueue&) = delete;

  PushResult Push(Work work) {
    if (shutdown_ || overflowed_) {
      return PushResult::kRejected;
    }

    const size_t byte_count = work.ByteCount();
    if (byte_count > max_bytes_ || queued_bytes_ > max_bytes_ - byte_count) {
      return MarkOverflow();
    }

    if (!pending_.empty() &&
        pending_.back().work.TryCoalesce(&work, max_bytes_)) {
      queued_bytes_ += byte_count;
      return PushResult::kAccepted;
    }

    bool uses_lifecycle_reserve = false;
    if (pending_.size() >= max_items_) {
      if (!work.IsLifecycle() ||
          lifecycle_items_reserved_ >= lifecycle_item_reserve_) {
        return MarkOverflow();
      }
      uses_lifecycle_reserve = true;
      ++lifecycle_items_reserved_;
    }

    queued_bytes_ += byte_count;
    pending_.push_back({std::move(work), uses_lifecycle_reserve});
    if (drain_scheduled_) {
      return PushResult::kAccepted;
    }
    drain_scheduled_ = true;
    return PushResult::kScheduleDrain;
  }

  // The caller must establish !empty() under the external lock.
  Work TakeFront() {
    Entry entry = std::move(pending_.front());
    pending_.pop_front();
    queued_bytes_ -= entry.work.ByteCount();
    if (entry.uses_lifecycle_reserve) {
      --lifecycle_items_reserved_;
    }
    return std::move(entry.work);
  }

  // Called after a bounded service-sequence slice. Keeping the latch set while
  // returning kRepost makes producers join the successor instead of posting a
  // second drain. Empty retirement is atomic with respect to the next Push.
  FinishSliceResult FinishSlice() {
    if (!pending_.empty()) {
      return FinishSliceResult::kRepost;
    }
    drain_scheduled_ = false;
    if (overflowed_) {
      overflowed_ = false;
      shutdown_ = true;
      return FinishSliceResult::kOverflow;
    }
    return FinishSliceResult::kRetired;
  }

  // Stops future producers and destroys every queued item. Returns the exact
  // number abandoned so tests and owners can audit replacement teardown.
  size_t Shutdown() {
    const size_t abandoned = pending_.size();
    pending_.clear();
    queued_bytes_ = 0;
    lifecycle_items_reserved_ = 0;
    drain_scheduled_ = false;
    overflowed_ = false;
    shutdown_ = true;
    return abandoned;
  }

  bool empty() const { return pending_.empty(); }
  size_t size() const { return pending_.size(); }
  size_t queued_bytes() const { return queued_bytes_; }
  bool drain_scheduled() const { return drain_scheduled_; }
  bool overflowed() const { return overflowed_; }
  bool shutdown() const { return shutdown_; }

 private:
  struct Entry {
    Work work;
    bool uses_lifecycle_reserve = false;
  };

  PushResult MarkOverflow() {
    overflowed_ = true;
    if (drain_scheduled_) {
      return PushResult::kRejected;
    }
    drain_scheduled_ = true;
    return PushResult::kScheduleDrain;
  }

  const size_t max_items_;
  const size_t max_bytes_;
  const size_t lifecycle_item_reserve_;
  std::deque<Entry> pending_;
  size_t queued_bytes_ = 0;
  size_t lifecycle_items_reserved_ = 0;
  bool drain_scheduled_ = false;
  bool overflowed_ = false;
  bool shutdown_ = false;
};

}  // namespace cmux

#endif  // CHROME_SERVICES_CMUX_TERMINAL_RENDERER_CMUX_TERMINAL_INGRESS_DRAIN_QUEUE_H_
