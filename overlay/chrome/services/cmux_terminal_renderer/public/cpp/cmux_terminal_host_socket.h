// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef CHROME_SERVICES_CMUX_TERMINAL_RENDERER_PUBLIC_CPP_CMUX_TERMINAL_HOST_SOCKET_H_
#define CHROME_SERVICES_CMUX_TERMINAL_RENDERER_PUBLIC_CPP_CMUX_TERMINAL_HOST_SOCKET_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "chrome/services/cmux_terminal_renderer/public/cpp/cmux_terminal_host_stream.h"

namespace cmux {

inline constexpr size_t kTerminalHostMaxRendererInput = 1024 * 1024;
inline constexpr size_t kTerminalHostMaxRendererQueuedWrites = 1024 * 1024;

enum class TerminalHostSocketCloseReason {
  kStopped,
  kExited,
  kInvalidDescriptor,
  kInvalidIdentity,
  kReadFailed,
  kWriteFailed,
  kPeerClosed,
  kProtocolFailed,
  kBackpressure,
};

struct TerminalHostSocketClose {
  TerminalHostSocketCloseReason reason =
      TerminalHostSocketCloseReason::kStopped;
  TerminalHostStreamError stream_error = TerminalHostStreamError::kNone;
  std::string message;
};

const char* TerminalHostSocketCloseReasonMessage(
    TerminalHostSocketCloseReason reason);

// Owns one already-authenticated, nonblocking terminal-host descriptor. Run()
// is a blocking poll loop intended for a dedicated worker. Queue* and Stop are
// thread-safe and wake that loop without touching the browser/service sequence.
// The callbacks run only on the Run() worker.
class TerminalHostRendererSocket {
 public:
  using EventCallback = std::function<void(TerminalHostRendererEvent)>;
  using ReadyCallback = std::function<void()>;
  using ClosedCallback = std::function<void(TerminalHostSocketClose)>;

  TerminalHostRendererSocket(int descriptor,
                             TerminalHostId terminal_id,
                             TerminalHostIncarnation incarnation,
                             EventCallback event_callback,
                             ReadyCallback ready_callback,
                             ClosedCallback closed_callback);
  TerminalHostRendererSocket(const TerminalHostRendererSocket&) = delete;
  TerminalHostRendererSocket& operator=(const TerminalHostRendererSocket&) =
      delete;
  ~TerminalHostRendererSocket();

  void Run();
  bool QueueInput(std::string_view bytes);
  bool QueuePaste(std::string_view bytes);
  bool QueueViewerSize(uint16_t cols, uint16_t rows, uint64_t request_id = 0);
  // Removes this stream from smallest-viewer arbitration without detaching;
  // output/state continue to arrive while its renderer is occluded.
  bool QueueReleaseViewer();
  void ReleaseAndStop();
  void Stop();

  const TerminalHostId& terminal_id() const { return terminal_id_; }
  const TerminalHostIncarnation& incarnation() const { return incarnation_; }
  size_t queued_bytes_for_testing() const;

 private:
  struct PendingWrite {
    PendingWrite(TerminalHostMessageKind kind,
                 std::vector<uint8_t> bytes,
                 size_t offset);
    PendingWrite(const PendingWrite&) = delete;
    PendingWrite& operator=(const PendingWrite&) = delete;
    PendingWrite(PendingWrite&&);
    PendingWrite& operator=(PendingWrite&&);
    ~PendingWrite();

    TerminalHostMessageKind kind;
    std::vector<uint8_t> bytes;
    size_t offset = 0;
  };

  bool QueueCommand(TerminalHostMessageKind kind,
                    std::string_view payload,
                    bool allow_release_reserve = false,
                    bool abort_on_backpressure = true,
                    uint64_t request_id = 0);
  void Wake();
  void Abort(TerminalHostSocketCloseReason reason);
  bool DrainReads(TerminalHostSocketClose* close);
  bool DrainWrites(TerminalHostSocketClose* close);
  bool DrainWakeup();
  void CloseDescriptors();
  void Report(TerminalHostSocketClose close);

  int descriptor_ = -1;
  int wake_read_ = -1;
  int wake_write_ = -1;
  const TerminalHostId terminal_id_;
  const TerminalHostIncarnation incarnation_;
  EventCallback event_callback_;
  ReadyCallback ready_callback_;
  ClosedCallback closed_callback_;
  TerminalHostRendererStream stream_;

  mutable std::mutex write_mutex_;
  std::deque<PendingWrite> writes_;
  size_t queued_write_bytes_ = 0;
  bool release_queued_ = false;
  bool stop_after_flush_ = false;
  std::atomic<bool> stopping_{false};
  std::atomic<TerminalHostSocketCloseReason> abort_reason_{
      TerminalHostSocketCloseReason::kStopped};
  bool ready_reported_ = false;
  bool closed_reported_ = false;
};

}  // namespace cmux

#endif  // CHROME_SERVICES_CMUX_TERMINAL_RENDERER_PUBLIC_CPP_CMUX_TERMINAL_HOST_SOCKET_H_
