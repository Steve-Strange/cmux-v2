// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "chrome/services/cmux_terminal_renderer/public/cpp/cmux_terminal_host_socket.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <utility>

#if !defined(_WIN32)
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace cmux {
namespace {

#if !defined(_WIN32)

bool SetDescriptorFlags(int descriptor) {
  const int descriptor_flags = fcntl(descriptor, F_GETFD);
  const int status_flags = fcntl(descriptor, F_GETFL);
  return descriptor_flags >= 0 && status_flags >= 0 &&
         fcntl(descriptor, F_SETFD, descriptor_flags | FD_CLOEXEC) == 0 &&
         fcntl(descriptor, F_SETFL, status_flags | O_NONBLOCK) == 0;
}

#endif

std::string WithDetail(const char* message, int error) {
  std::string result(message);
  if (error != 0) {
    result.append(": ");
    result.append(std::strerror(error));
  }
  return result;
}

}  // namespace

TerminalHostRendererSocket::PendingWrite::PendingWrite(
    TerminalHostMessageKind kind,
    std::vector<uint8_t> bytes,
    size_t offset)
    : kind(kind), bytes(std::move(bytes)), offset(offset) {}
TerminalHostRendererSocket::PendingWrite::PendingWrite(PendingWrite&&) =
    default;
TerminalHostRendererSocket::PendingWrite&
TerminalHostRendererSocket::PendingWrite::operator=(PendingWrite&&) = default;
TerminalHostRendererSocket::PendingWrite::~PendingWrite() = default;

const char* TerminalHostSocketCloseReasonMessage(
    TerminalHostSocketCloseReason reason) {
  switch (reason) {
    case TerminalHostSocketCloseReason::kStopped:
      return "terminal-host renderer detached";
    case TerminalHostSocketCloseReason::kExited:
      return "terminal process exited";
    case TerminalHostSocketCloseReason::kInvalidDescriptor:
      return "terminal-host renderer received an invalid descriptor";
    case TerminalHostSocketCloseReason::kInvalidIdentity:
      return "terminal-host renderer received an invalid expected identity";
    case TerminalHostSocketCloseReason::kReadFailed:
      return "terminal-host renderer socket read failed";
    case TerminalHostSocketCloseReason::kWriteFailed:
      return "terminal-host renderer socket write failed";
    case TerminalHostSocketCloseReason::kPeerClosed:
      return "terminal-host renderer socket closed before Exit";
    case TerminalHostSocketCloseReason::kProtocolFailed:
      return "terminal-host renderer stream validation failed";
    case TerminalHostSocketCloseReason::kBackpressure:
      return "terminal-host renderer write queue overflowed";
  }
  return "unknown terminal-host renderer socket error";
}

TerminalHostRendererSocket::TerminalHostRendererSocket(
    int descriptor,
    TerminalHostId terminal_id,
    TerminalHostIncarnation incarnation,
    EventCallback event_callback,
    ReadyCallback ready_callback,
    ClosedCallback closed_callback)
    : descriptor_(descriptor),
      terminal_id_(terminal_id),
      incarnation_(incarnation),
      event_callback_(std::move(event_callback)),
      ready_callback_(std::move(ready_callback)),
      closed_callback_(std::move(closed_callback)) {
#if !defined(_WIN32)
  int wakeup[2] = {-1, -1};
  if (pipe(wakeup) == 0) {
    wake_read_ = wakeup[0];
    wake_write_ = wakeup[1];
    if (!SetDescriptorFlags(wake_read_) || !SetDescriptorFlags(wake_write_)) {
      close(wake_read_);
      close(wake_write_);
      wake_read_ = -1;
      wake_write_ = -1;
    }
  }
  if (descriptor_ >= 0) {
    if (!SetDescriptorFlags(descriptor_)) {
      close(descriptor_);
      descriptor_ = -1;
    }
#if defined(__APPLE__)
    if (descriptor_ >= 0) {
      int no_sigpipe = 1;
      setsockopt(descriptor_, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe,
                 sizeof(no_sigpipe));
    }
#endif
  }
#endif
}

TerminalHostRendererSocket::~TerminalHostRendererSocket() {
  Stop();
  CloseDescriptors();
}

void TerminalHostRendererSocket::Run() {
#if defined(_WIN32)
  Report({TerminalHostSocketCloseReason::kInvalidDescriptor,
          TerminalHostStreamError::kNone,
          "terminal-host renderer sockets are not implemented on Windows"});
  return;
#else
  if (descriptor_ < 0 || wake_read_ < 0 || wake_write_ < 0) {
    Report({TerminalHostSocketCloseReason::kInvalidDescriptor,
            TerminalHostStreamError::kNone,
            TerminalHostSocketCloseReasonMessage(
                TerminalHostSocketCloseReason::kInvalidDescriptor)});
    return;
  }
  if (!IsValidTerminalHostUuidV4(terminal_id_) ||
      !IsValidTerminalHostUuidV4(incarnation_)) {
    Report({TerminalHostSocketCloseReason::kInvalidIdentity,
            TerminalHostStreamError::kNone,
            TerminalHostSocketCloseReasonMessage(
                TerminalHostSocketCloseReason::kInvalidIdentity)});
    return;
  }

  for (;;) {
    bool wants_write = false;
    bool stop_after_flush = false;
    {
      std::lock_guard<std::mutex> lock(write_mutex_);
      wants_write = !writes_.empty();
      stop_after_flush = stop_after_flush_;
    }
    if (stopping_.load(std::memory_order_acquire) &&
        (!stop_after_flush || !wants_write)) {
      const TerminalHostSocketCloseReason reason =
          abort_reason_.load(std::memory_order_acquire);
      Report({reason, TerminalHostStreamError::kNone,
              TerminalHostSocketCloseReasonMessage(reason)});
      return;
    }

    pollfd descriptors[2] = {
        {descriptor_, static_cast<short>(POLLIN | (wants_write ? POLLOUT : 0)),
         0},
        {wake_read_, POLLIN, 0},
    };
    const int result = poll(descriptors, 2, -1);
    if (result < 0) {
      if (errno == EINTR) {
        continue;
      }
      Report({TerminalHostSocketCloseReason::kReadFailed,
              TerminalHostStreamError::kNone,
              WithDetail("terminal-host poll failed", errno)});
      return;
    }
    if ((descriptors[1].revents & POLLIN) != 0 && !DrainWakeup()) {
      Report({TerminalHostSocketCloseReason::kReadFailed,
              TerminalHostStreamError::kNone,
              WithDetail("terminal-host wakeup failed", errno)});
      return;
    }
    if (stopping_.load(std::memory_order_acquire)) {
      bool flush = false;
      bool pending = false;
      {
        std::lock_guard<std::mutex> lock(write_mutex_);
        flush = stop_after_flush_;
        pending = !writes_.empty();
      }
      if (!flush || !pending) {
        const TerminalHostSocketCloseReason reason =
            abort_reason_.load(std::memory_order_acquire);
        Report({reason, TerminalHostStreamError::kNone,
                TerminalHostSocketCloseReasonMessage(reason)});
        return;
      }
    }
    TerminalHostSocketClose close;
    if ((descriptors[0].revents & POLLIN) != 0 && !DrainReads(&close)) {
      Report(std::move(close));
      return;
    }
    if ((descriptors[0].revents & POLLOUT) != 0 && !DrainWrites(&close)) {
      Report(std::move(close));
      return;
    }
    if ((descriptors[0].revents & (POLLERR | POLLNVAL)) != 0) {
      Report({TerminalHostSocketCloseReason::kReadFailed,
              TerminalHostStreamError::kNone,
              "terminal-host descriptor reported a socket error"});
      return;
    }
    if ((descriptors[0].revents & POLLHUP) != 0 &&
        (descriptors[0].revents & POLLIN) == 0) {
      Report({TerminalHostSocketCloseReason::kPeerClosed,
              TerminalHostStreamError::kNone,
              TerminalHostSocketCloseReasonMessage(
                  TerminalHostSocketCloseReason::kPeerClosed)});
      return;
    }
  }
#endif
}

bool TerminalHostRendererSocket::QueueInput(std::string_view bytes) {
  return !bytes.empty() && bytes.size() <= kTerminalHostMaxRendererInput &&
         QueueCommand(TerminalHostMessageKind::kInput, bytes);
}

bool TerminalHostRendererSocket::QueuePaste(std::string_view bytes) {
  return !bytes.empty() && bytes.size() <= kTerminalHostMaxRendererInput &&
         QueueCommand(TerminalHostMessageKind::kPaste, bytes,
                      /*allow_release_reserve=*/false,
                      /*abort_on_backpressure=*/false);
}

bool TerminalHostRendererSocket::QueueViewerSize(uint16_t cols,
                                                 uint16_t rows,
                                                 uint64_t request_id) {
  std::vector<uint8_t> payload;
  if (EncodeTerminalHostViewerSize(cols, rows, &payload) !=
      TerminalHostProtocolError::kNone) {
    return false;
  }
  return QueueCommand(
      TerminalHostMessageKind::kViewerSize,
      std::string_view(reinterpret_cast<const char*>(payload.data()),
                       payload.size()),
      /*allow_release_reserve=*/false,
      /*abort_on_backpressure=*/true, request_id);
}

bool TerminalHostRendererSocket::QueueReleaseViewer() {
  return QueueCommand(TerminalHostMessageKind::kReleaseViewer,
                      std::string_view());
}

void TerminalHostRendererSocket::ReleaseAndStop() {
  {
    std::lock_guard<std::mutex> lock(write_mutex_);
    if (stopping_.load(std::memory_order_relaxed)) {
      return;
    }
    if (!release_queued_) {
      std::vector<uint8_t> bytes;
      if (EncodeTerminalHostRendererCommand(
              TerminalHostMessageKind::kReleaseViewer, std::string_view(),
              &bytes) == TerminalHostProtocolError::kNone) {
        queued_write_bytes_ += bytes.size();
        writes_.push_back(
            {TerminalHostMessageKind::kReleaseViewer, std::move(bytes), 0});
        release_queued_ = true;
      }
    }
    stop_after_flush_ = true;
    stopping_.store(true, std::memory_order_release);
  }
  Wake();
}

void TerminalHostRendererSocket::Stop() {
  Abort(TerminalHostSocketCloseReason::kStopped);
}

size_t TerminalHostRendererSocket::queued_bytes_for_testing() const {
  std::lock_guard<std::mutex> lock(write_mutex_);
  return queued_write_bytes_;
}

bool TerminalHostRendererSocket::QueueCommand(TerminalHostMessageKind kind,
                                              std::string_view payload,
                                              bool allow_release_reserve,
                                              bool abort_on_backpressure,
                                              uint64_t request_id) {
  if (stopping_.load(std::memory_order_acquire)) {
    return false;
  }
  std::vector<uint8_t> bytes;
  if (EncodeTerminalHostRendererCommand(kind, payload, &bytes, request_id) !=
      TerminalHostProtocolError::kNone) {
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(write_mutex_);
    if (stopping_.load(std::memory_order_relaxed) || release_queued_) {
      return false;
    }
    if (!allow_release_reserve &&
        (queued_write_bytes_ > kTerminalHostMaxRendererQueuedWrites ||
         bytes.size() >
             kTerminalHostMaxRendererQueuedWrites - queued_write_bytes_)) {
      if (abort_on_backpressure) {
        abort_reason_.store(TerminalHostSocketCloseReason::kBackpressure,
                            std::memory_order_release);
        stopping_.store(true, std::memory_order_release);
        Wake();
      }
      return false;
    }
    // Consecutive unsent sizing commands are state replacement. Retain only
    // the newest ViewerSize/ReleaseViewer so rapid resize/occlusion cannot
    // transiently reserve a stale grid or create a write backlog.
    const bool sizing_command = kind == TerminalHostMessageKind::kViewerSize ||
                                kind == TerminalHostMessageKind::kReleaseViewer;
    const bool replaces_unsent_sizing =
        sizing_command && !writes_.empty() && writes_.back().offset == 0 &&
        (writes_.back().kind == TerminalHostMessageKind::kViewerSize ||
         writes_.back().kind == TerminalHostMessageKind::kReleaseViewer);
    if (replaces_unsent_sizing) {
      queued_write_bytes_ -= writes_.back().bytes.size();
      writes_.back().kind = kind;
      writes_.back().bytes = std::move(bytes);
      queued_write_bytes_ += writes_.back().bytes.size();
    } else {
      queued_write_bytes_ += bytes.size();
      writes_.push_back({kind, std::move(bytes), 0});
    }
  }
  Wake();
  return true;
}

void TerminalHostRendererSocket::Wake() {
#if !defined(_WIN32)
  if (wake_write_ < 0) {
    return;
  }
  const uint8_t byte = 1;
  const ssize_t ignored = write(wake_write_, &byte, sizeof(byte));
  (void)ignored;
#endif
}

void TerminalHostRendererSocket::Abort(TerminalHostSocketCloseReason reason) {
  {
    std::lock_guard<std::mutex> lock(write_mutex_);
    // QueueCommand records backpressure before returning false. A caller may
    // defensively Stop() the socket afterward; preserve the first causal
    // reason instead of overwriting it with the generic kStopped value.
    if (stopping_.load(std::memory_order_relaxed)) {
      return;
    }
    abort_reason_.store(reason, std::memory_order_relaxed);
    stopping_.store(true, std::memory_order_release);
  }
#if !defined(_WIN32)
  if (descriptor_ >= 0) {
    shutdown(descriptor_, SHUT_RDWR);
  }
#endif
  Wake();
}

bool TerminalHostRendererSocket::DrainReads(TerminalHostSocketClose* close) {
#if defined(_WIN32)
  return false;
#else
  std::array<char, 64 * 1024> buffer{};
  for (;;) {
    const ssize_t count = recv(descriptor_, buffer.data(), buffer.size(), 0);
    if (count > 0) {
      std::vector<TerminalHostRendererEvent> events;
      const TerminalHostStreamError error = stream_.Push(
          std::string_view(buffer.data(), static_cast<size_t>(count)), &events);
      if (error != TerminalHostStreamError::kNone) {
        *close = {TerminalHostSocketCloseReason::kProtocolFailed, error,
                  TerminalHostStreamErrorMessage(error)};
        return false;
      }
      for (TerminalHostRendererEvent& event : events) {
        const bool exited = event.kind == TerminalHostMessageKind::kExit;
        if (event_callback_) {
          event_callback_(std::move(event));
        }
        if (exited) {
          *close = {TerminalHostSocketCloseReason::kExited,
                    TerminalHostStreamError::kNone,
                    TerminalHostSocketCloseReasonMessage(
                        TerminalHostSocketCloseReason::kExited)};
          return false;
        }
      }
      if (stream_.ready() && !ready_reported_) {
        ready_reported_ = true;
        if (ready_callback_) {
          ready_callback_();
        }
      }
      continue;
    }
    if (count == 0) {
      const TerminalHostStreamError finish = stream_.Finish();
      *close = {TerminalHostSocketCloseReason::kPeerClosed, finish,
                finish == TerminalHostStreamError::kTruncated
                    ? TerminalHostStreamErrorMessage(finish)
                    : TerminalHostSocketCloseReasonMessage(
                          TerminalHostSocketCloseReason::kPeerClosed)};
      return false;
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return true;
    }
    *close = {TerminalHostSocketCloseReason::kReadFailed,
              TerminalHostStreamError::kNone,
              WithDetail("terminal-host read failed", errno)};
    return false;
  }
#endif
}

bool TerminalHostRendererSocket::DrainWrites(TerminalHostSocketClose* close) {
#if defined(_WIN32)
  return false;
#else
  for (;;) {
    std::lock_guard<std::mutex> lock(write_mutex_);
    if (writes_.empty()) {
      return true;
    }
    PendingWrite& pending = writes_.front();
#if defined(__linux__)
    constexpr int kSendFlags = MSG_NOSIGNAL;
#else
    constexpr int kSendFlags = 0;
#endif
    const ssize_t count = send(descriptor_, pending.bytes.data(),
                               pending.bytes.size(), kSendFlags);
    if (count > 0) {
      const size_t sent = static_cast<size_t>(count);
      queued_write_bytes_ -= sent;
      if (sent == pending.bytes.size()) {
        writes_.pop_front();
      } else {
        // Keep the unsent suffix at data() so both the C++17 host tests and
        // Chromium's unsafe-buffer checker avoid raw pointer arithmetic.
        // A nonzero offset is retained only as the "write started" marker
        // that prevents sizing-command coalescing from replacing half a
        // frame.
        pending.bytes.erase(pending.bytes.begin(),
                            pending.bytes.begin() + sent);
        pending.offset = 1;
      }
      continue;
    }
    if (count == 0) {
      *close = {TerminalHostSocketCloseReason::kWriteFailed,
                TerminalHostStreamError::kNone,
                "terminal-host socket closed during write"};
      return false;
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return true;
    }
    *close = {TerminalHostSocketCloseReason::kWriteFailed,
              TerminalHostStreamError::kNone,
              WithDetail("terminal-host write failed", errno)};
    return false;
  }
#endif
}

bool TerminalHostRendererSocket::DrainWakeup() {
#if defined(_WIN32)
  return false;
#else
  std::array<uint8_t, 256> buffer{};
  for (;;) {
    const ssize_t count = read(wake_read_, buffer.data(), buffer.size());
    if (count > 0) {
      continue;
    }
    if (count == 0) {
      return false;
    }
    if (errno == EINTR) {
      continue;
    }
    return errno == EAGAIN || errno == EWOULDBLOCK;
  }
#endif
}

void TerminalHostRendererSocket::CloseDescriptors() {
#if !defined(_WIN32)
  if (descriptor_ >= 0) {
    close(descriptor_);
    descriptor_ = -1;
  }
  if (wake_read_ >= 0) {
    close(wake_read_);
    wake_read_ = -1;
  }
  if (wake_write_ >= 0) {
    close(wake_write_);
    wake_write_ = -1;
  }
#endif
}

void TerminalHostRendererSocket::Report(TerminalHostSocketClose close) {
  if (closed_reported_) {
    return;
  }
  closed_reported_ = true;
  if (closed_callback_) {
    closed_callback_(std::move(close));
  }
}

}  // namespace cmux
