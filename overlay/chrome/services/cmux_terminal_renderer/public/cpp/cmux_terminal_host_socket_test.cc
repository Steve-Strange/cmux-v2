// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "chrome/services/cmux_terminal_renderer/public/cpp/cmux_terminal_host_socket.h"

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

using Clock = std::chrono::steady_clock;
using Kind = cmux::TerminalHostMessageKind;

int failures = 0;

void Check(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

std::string_view Bytes(const std::vector<uint8_t>& bytes) {
  return std::string_view(reinterpret_cast<const char*>(bytes.data()),
                          bytes.size());
}

cmux::TerminalHostId TestId(uint8_t seed = 1) {
  cmux::TerminalHostId id{};
  for (size_t index = 0; index < id.size(); ++index) {
    id[index] = static_cast<uint8_t>(seed + index);
  }
  id[6] = static_cast<uint8_t>((id[6] & 0x0f) | 0x40);
  id[8] = static_cast<uint8_t>((id[8] & 0x3f) | 0x80);
  return id;
}

std::vector<uint8_t> Encode(Kind kind,
                            uint64_t sequence,
                            std::vector<uint8_t> payload = {}) {
  cmux::TerminalHostFrame frame;
  frame.kind = kind;
  frame.sequence = sequence;
  frame.payload = std::move(payload);
  std::vector<uint8_t> bytes;
  Check(cmux::EncodeTerminalHostFrame(frame, &bytes) ==
            cmux::TerminalHostProtocolError::kNone,
        "socket test frame encodes");
  return bytes;
}

std::vector<uint8_t> SnapshotPayload() {
  cmux::TerminalHostSnapshot snapshot;
  snapshot.cols = 90;
  snapshot.rows = 31;
  snapshot.replay = {'s', 'n', 'a', 'p'};
  std::vector<uint8_t> payload;
  Check(cmux::EncodeTerminalHostSnapshot(snapshot, &payload) ==
            cmux::TerminalHostProtocolError::kNone,
        "socket test snapshot encodes");
  return payload;
}

std::vector<uint8_t> ColorsPayload() {
  cmux::TerminalHostColors colors;
  colors.background = cmux::TerminalHostRgb{12, 34, 56};
  colors.cursor_visual = cmux::TerminalHostCursorVisual{
      cmux::TerminalHostCursorStyle::kUnderline, false};
  colors.palette.push_back({4, {78, 90, 12}});
  std::vector<uint8_t> payload;
  Check(cmux::EncodeTerminalHostColors(colors, &payload) ==
            cmux::TerminalHostProtocolError::kNone,
        "socket test Colors encodes");
  return payload;
}

std::vector<uint8_t> InitialStream(uint64_t sequence = 3) {
  std::vector<uint8_t> bytes =
      Encode(Kind::kSnapshot, sequence, SnapshotPayload());
  const std::vector<uint8_t> colors =
      Encode(Kind::kColors, sequence, ColorsPayload());
  bytes.insert(bytes.end(), colors.begin(), colors.end());
  return bytes;
}

bool WaitDescriptor(int descriptor, short events, Clock::time_point deadline) {
  while (Clock::now() < deadline) {
    pollfd item = {descriptor, events, 0};
    const int result = poll(&item, 1, 25);
    if (result > 0 && (item.revents & events) != 0) {
      return true;
    }
    if (result < 0 && errno != EINTR) {
      return false;
    }
  }
  return false;
}

bool WriteChunks(int descriptor,
                 std::string_view bytes,
                 size_t chunk_size,
                 Clock::time_point deadline) {
  while (!bytes.empty()) {
    if (!WaitDescriptor(descriptor, POLLOUT, deadline)) {
      return false;
    }
    const size_t amount = std::min(chunk_size, bytes.size());
    const ssize_t count = send(descriptor, bytes.data(), amount, 0);
    if (count > 0) {
      bytes.remove_prefix(static_cast<size_t>(count));
    } else if (count < 0 && errno != EINTR && errno != EAGAIN &&
               errno != EWOULDBLOCK) {
      return false;
    }
  }
  return true;
}

struct Callbacks {
  std::mutex mutex;
  std::condition_variable changed;
  std::vector<cmux::TerminalHostRendererEvent> events;
  std::vector<cmux::TerminalHostSocketClose> closes;
  bool ready = false;

  cmux::TerminalHostRendererSocket::EventCallback Event() {
    return [this](cmux::TerminalHostRendererEvent event) {
      std::lock_guard<std::mutex> lock(mutex);
      events.push_back(std::move(event));
      changed.notify_all();
    };
  }

  cmux::TerminalHostRendererSocket::ReadyCallback Ready() {
    return [this]() {
      std::lock_guard<std::mutex> lock(mutex);
      ready = true;
      changed.notify_all();
    };
  }

  cmux::TerminalHostRendererSocket::ClosedCallback Closed() {
    return [this](cmux::TerminalHostSocketClose close) {
      std::lock_guard<std::mutex> lock(mutex);
      closes.push_back(std::move(close));
      changed.notify_all();
    };
  }

  bool WaitReady(
      std::chrono::milliseconds timeout = std::chrono::milliseconds(2000)) {
    std::unique_lock<std::mutex> lock(mutex);
    return changed.wait_for(lock, timeout, [this] { return ready; });
  }

  bool WaitClosed(
      std::chrono::milliseconds timeout = std::chrono::milliseconds(2000)) {
    std::unique_lock<std::mutex> lock(mutex);
    return changed.wait_for(lock, timeout, [this] { return !closes.empty(); });
  }
};

std::vector<cmux::TerminalHostFrame> ReadFrames(int descriptor,
                                                size_t wanted,
                                                size_t read_chunk = 65536) {
  cmux::TerminalHostFrameDecoder decoder;
  std::vector<cmux::TerminalHostFrame> frames;
  std::vector<char> buffer(std::max<size_t>(read_chunk, 1));
  const Clock::time_point deadline = Clock::now() + std::chrono::seconds(5);
  while (frames.size() < wanted && Clock::now() < deadline) {
    if (!WaitDescriptor(descriptor, POLLIN, deadline)) {
      break;
    }
    const ssize_t count = recv(descriptor, buffer.data(), buffer.size(), 0);
    if (count > 0) {
      Check(decoder.Push(
                std::string_view(buffer.data(), static_cast<size_t>(count)),
                &frames) == cmux::TerminalHostProtocolError::kNone,
            "outbound socket commands decode incrementally");
    } else if (count == 0) {
      break;
    } else if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
      break;
    }
  }
  return frames;
}

void TestPartialReadsAndOrderlyCommands() {
  int pair[2] = {-1, -1};
  Check(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0,
        "socketpair creates direct renderer stream");
  Callbacks callbacks;
  cmux::TerminalHostRendererSocket socket(pair[0], TestId(1), TestId(33),
                                          callbacks.Event(), callbacks.Ready(),
                                          callbacks.Closed());
  Check(socket.QueueViewerSize(80, 24, 41) &&
            socket.QueueViewerSize(120, 40, 42) &&
            socket.queued_bytes_for_testing() ==
                cmux::kTerminalHostHeaderLength + 4,
        "consecutive unsent ViewerSize writes coalesce to the latest grid");
  Check(socket.QueueReleaseViewer() && socket.queued_bytes_for_testing() ==
                                           cmux::kTerminalHostHeaderLength,
        "occlusion replaces an unsent ViewerSize with ReleaseViewer");
  Check(socket.QueueViewerSize(120, 40, 43) &&
            socket.queued_bytes_for_testing() ==
                cmux::kTerminalHostHeaderLength + 4,
        "reactivation replaces an unsent ReleaseViewer with latest size");
  std::thread worker([&socket] { socket.Run(); });

  const std::vector<uint8_t> initial = InitialStream();
  Check(WriteChunks(pair[1], Bytes(initial), 1,
                    Clock::now() + std::chrono::seconds(3)),
        "server writes Snapshot/Colors one byte at a time");
  Check(callbacks.WaitReady(), "socket reports ready only after Colors");
  {
    std::lock_guard<std::mutex> lock(callbacks.mutex);
    Check(callbacks.events.size() == 1 && callbacks.events[0].snapshot &&
              callbacks.events[0].snapshot->cols == 90 &&
              callbacks.events[0].colors &&
              callbacks.events[0].colors->cursor_visual &&
              callbacks.events[0].colors->cursor_visual->style ==
                  cmux::TerminalHostCursorStyle::kUnderline &&
              !callbacks.events[0].colors->cursor_visual->blinking,
          "socket emits one atomic Snapshot+Colors+cursor event before "
          "ready");
  }

  const std::string input("hello\0world", 11);
  Check(socket.QueueInput(input), "Ghostty input queues on the direct socket");
  const std::string paste("line one\nline two");
  Check(socket.QueuePaste(paste), "paste queues on the direct socket");
  std::vector<cmux::TerminalHostFrame> frames = ReadFrames(pair[1], 3, 3);
  Check(
      frames.size() == 3 && frames[0].kind == Kind::kViewerSize &&
          frames[0].request_id == 43 && frames[0].sequence == 0 &&
          frames[0].payload == std::vector<uint8_t>({120, 0, 40, 0}) &&
          frames[1].kind == Kind::kInput &&
          frames[1].payload ==
              std::vector<uint8_t>(input.begin(), input.end()) &&
          frames[2].kind == Kind::kPaste &&
          frames[2].payload == std::vector<uint8_t>(paste.begin(), paste.end()),
      "partial command reads preserve viewer grid, binary input, and paste");

  const std::string final_input = "input-immediately-before-cold-eviction";
  Check(socket.QueueInput(final_input),
        "final direct input queues immediately before orderly eviction");
  socket.ReleaseAndStop();
  frames = ReadFrames(pair[1], 2, 2);
  Check(frames.size() == 2 && frames[0].kind == Kind::kInput &&
            frames[0].payload ==
                std::vector<uint8_t>(final_input.begin(), final_input.end()) &&
            frames[1].kind == Kind::kReleaseViewer &&
            frames[1].payload.empty(),
        "orderly eviction flushes prior input then ReleaseViewer before "
        "closing");
  worker.join();
  Check(callbacks.WaitClosed(), "orderly socket stop reports completion");
  {
    std::lock_guard<std::mutex> lock(callbacks.mutex);
    Check(callbacks.closes.size() == 1 &&
              callbacks.closes[0].reason ==
                  cmux::TerminalHostSocketCloseReason::kStopped,
          "orderly detach reports stopped exactly once");
  }
  close(pair[1]);
}

void TestPartialWritesAndFloodBound() {
  {
    int pair[2] = {-1, -1};
    Check(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0,
          "partial-write socketpair creates");
    int small_buffer = 1024;
    setsockopt(pair[0], SOL_SOCKET, SO_SNDBUF, &small_buffer,
               sizeof(small_buffer));
    Callbacks callbacks;
    cmux::TerminalHostRendererSocket socket(
        pair[0], TestId(2), TestId(34), callbacks.Event(), callbacks.Ready(),
        callbacks.Closed());
    std::thread worker([&socket] { socket.Run(); });
    const std::vector<uint8_t> initial = InitialStream();
    Check(WriteChunks(pair[1], Bytes(initial), initial.size(),
                      Clock::now() + std::chrono::seconds(2)) &&
              callbacks.WaitReady(),
          "partial-write test reaches ready");
    std::string input(512 * 1024, 'x');
    input[1024] = '\0';
    Check(socket.QueueInput(input), "large bounded input queues");
    const std::vector<cmux::TerminalHostFrame> frames =
        ReadFrames(pair[1], 1, 17);
    Check(frames.size() == 1 && frames[0].kind == Kind::kInput &&
              frames[0].payload.size() == input.size() &&
              frames[0].payload[1024] == 0,
          "nonblocking partial writes deliver one exact binary Input frame");
    socket.Stop();
    worker.join();
    close(pair[1]);
  }

  {
    int pair[2] = {-1, -1};
    Check(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0,
          "backpressure socketpair creates");
    Callbacks callbacks;
    cmux::TerminalHostRendererSocket socket(
        pair[0], TestId(3), TestId(35), callbacks.Event(), callbacks.Ready(),
        callbacks.Closed());
    const std::string chunk(64 * 1024, 'f');
    size_t accepted = 0;
    while (socket.QueueInput(chunk)) {
      ++accepted;
    }
    Check(accepted > 0 && socket.queued_bytes_for_testing() <=
                              cmux::kTerminalHostMaxRendererQueuedWrites,
          "input flood is rejected at the bounded write budget");
    socket.Stop();
    std::thread worker([&socket] { socket.Run(); });
    worker.join();
    Check(callbacks.WaitClosed(), "write overflow terminates the socket pump");
    {
      std::lock_guard<std::mutex> lock(callbacks.mutex);
      Check(callbacks.closes[0].reason ==
                cmux::TerminalHostSocketCloseReason::kBackpressure,
            "write overflow has a deterministic backpressure reason");
    }
    close(pair[1]);
  }

  {
    int pair[2] = {-1, -1};
    Check(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0,
          "nonfatal-paste socketpair creates");
    Callbacks callbacks;
    cmux::TerminalHostRendererSocket socket(
        pair[0], TestId(4), TestId(36), callbacks.Event(), callbacks.Ready(),
        callbacks.Closed());
    const std::string framed_over_budget(cmux::kTerminalHostMaxRendererInput,
                                         'p');
    Check(!socket.QueuePaste(framed_over_budget),
          "paste whose framed form exceeds the write budget is rejected");
    Check(socket.QueuePaste("renderer-still-live"),
          "rejected paste does not poison the renderer socket");
    socket.Stop();
    close(pair[1]);
  }
}

void TestMalformedDisconnectExitAndIdentity() {
  {
    int pair[2] = {-1, -1};
    Check(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0,
          "malformed socketpair creates");
    Callbacks callbacks;
    cmux::TerminalHostRendererSocket socket(
        pair[0], TestId(4), TestId(36), callbacks.Event(), callbacks.Ready(),
        callbacks.Closed());
    std::thread worker([&socket] { socket.Run(); });
    std::vector<uint8_t> invalid =
        Encode(Kind::kSnapshot, 5, SnapshotPayload());
    const std::vector<uint8_t> output = Encode(Kind::kOutput, 6, {'x'});
    invalid.insert(invalid.end(), output.begin(), output.end());
    Check(WriteChunks(pair[1], Bytes(invalid), invalid.size(),
                      Clock::now() + std::chrono::seconds(2)) &&
              callbacks.WaitClosed(),
          "unexpected frame order closes the socket");
    worker.join();
    {
      std::lock_guard<std::mutex> lock(callbacks.mutex);
      Check(callbacks.closes[0].reason ==
                    cmux::TerminalHostSocketCloseReason::kProtocolFailed &&
                callbacks.closes[0].stream_error ==
                    cmux::TerminalHostStreamError::kUnexpectedFrame,
            "malformed ordering is surfaced as a protocol reconnect");
    }
    close(pair[1]);
  }

  {
    int pair[2] = {-1, -1};
    Check(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0,
          "sequence-gap socketpair creates");
    Callbacks callbacks;
    cmux::TerminalHostRendererSocket socket(
        pair[0], TestId(41), TestId(73), callbacks.Event(), callbacks.Ready(),
        callbacks.Closed());
    std::thread worker([&socket] { socket.Run(); });
    std::vector<uint8_t> bytes = InitialStream(20);
    const std::vector<uint8_t> gap = Encode(Kind::kOutput, 22, {'g'});
    bytes.insert(bytes.end(), gap.begin(), gap.end());
    Check(WriteChunks(pair[1], Bytes(bytes), bytes.size(),
                      Clock::now() + std::chrono::seconds(2)) &&
              callbacks.WaitClosed(),
          "live sequence gap closes the socket");
    worker.join();
    {
      std::lock_guard<std::mutex> lock(callbacks.mutex);
      Check(callbacks.closes[0].reason ==
                    cmux::TerminalHostSocketCloseReason::kProtocolFailed &&
                callbacks.closes[0].stream_error ==
                    cmux::TerminalHostStreamError::kSequenceGap,
            "socket surfaces a live gap as a fresh-Snapshot condition");
    }
    close(pair[1]);
  }

  {
    int pair[2] = {-1, -1};
    Check(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0,
          "oversized-frame socketpair creates");
    Callbacks callbacks;
    cmux::TerminalHostRendererSocket socket(
        pair[0], TestId(42), TestId(74), callbacks.Event(), callbacks.Ready(),
        callbacks.Closed());
    std::thread worker([&socket] { socket.Run(); });
    std::vector<uint8_t> oversized = Encode(Kind::kSnapshot, 0);
    const uint32_t length =
        static_cast<uint32_t>(cmux::kTerminalHostMaxFramePayload + 1);
    oversized[12] = static_cast<uint8_t>(length);
    oversized[13] = static_cast<uint8_t>(length >> 8);
    oversized[14] = static_cast<uint8_t>(length >> 16);
    oversized[15] = static_cast<uint8_t>(length >> 24);
    Check(WriteChunks(pair[1], Bytes(oversized), oversized.size(),
                      Clock::now() + std::chrono::seconds(2)) &&
              callbacks.WaitClosed(),
          "oversized advertised frame closes before payload allocation");
    worker.join();
    {
      std::lock_guard<std::mutex> lock(callbacks.mutex);
      Check(callbacks.closes[0].reason ==
                cmux::TerminalHostSocketCloseReason::kProtocolFailed,
            "oversized frame is a protocol reconnect condition");
    }
    close(pair[1]);
  }

  {
    int pair[2] = {-1, -1};
    Check(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0,
          "disconnect socketpair creates");
    Callbacks callbacks;
    cmux::TerminalHostRendererSocket socket(
        pair[0], TestId(5), TestId(37), callbacks.Event(), callbacks.Ready(),
        callbacks.Closed());
    std::thread worker([&socket] { socket.Run(); });
    close(pair[1]);
    Check(callbacks.WaitClosed(), "peer disconnect wakes the socket worker");
    worker.join();
    std::lock_guard<std::mutex> lock(callbacks.mutex);
    Check(callbacks.closes[0].reason ==
              cmux::TerminalHostSocketCloseReason::kPeerClosed,
          "EOF before Exit requests a fresh renderer attachment");
  }

  {
    int pair[2] = {-1, -1};
    Check(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0,
          "Exit socketpair creates");
    Callbacks callbacks;
    cmux::TerminalHostRendererSocket socket(
        pair[0], TestId(6), TestId(38), callbacks.Event(), callbacks.Ready(),
        callbacks.Closed());
    std::thread worker([&socket] { socket.Run(); });
    std::vector<uint8_t> bytes = InitialStream(8);
    const std::vector<uint8_t> exit = Encode(Kind::kExit, 9);
    bytes.insert(bytes.end(), exit.begin(), exit.end());
    Check(WriteChunks(pair[1], Bytes(bytes), bytes.size(),
                      Clock::now() + std::chrono::seconds(2)) &&
              callbacks.WaitClosed(),
          "ordered Exit ends the socket pump");
    worker.join();
    std::lock_guard<std::mutex> lock(callbacks.mutex);
    Check(callbacks.closes[0].reason ==
                  cmux::TerminalHostSocketCloseReason::kExited &&
              callbacks.events.size() == 2 &&
              callbacks.events.back().kind == Kind::kExit,
          "Exit event reaches the browser before the exited close reason");
    close(pair[1]);
  }

  {
    int pair[2] = {-1, -1};
    Check(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0,
          "identity socketpair creates");
    Callbacks callbacks;
    cmux::TerminalHostId invalid{};
    cmux::TerminalHostRendererSocket socket(
        pair[0], invalid, TestId(39), callbacks.Event(), callbacks.Ready(),
        callbacks.Closed());
    socket.Run();
    Check(callbacks.WaitClosed(), "invalid expected identity fails before I/O");
    std::lock_guard<std::mutex> lock(callbacks.mutex);
    Check(callbacks.closes[0].reason ==
              cmux::TerminalHostSocketCloseReason::kInvalidIdentity,
          "utility validates browser-provided terminal/incarnation UUIDs");
    close(pair[1]);
  }
}

}  // namespace

int main() {
  TestPartialReadsAndOrderlyCommands();
  TestPartialWritesAndFloodBound();
  TestMalformedDisconnectExitAndIdentity();
  if (failures != 0) {
    std::cerr << failures << " failure(s)\n";
    return 1;
  }
  std::cout << "cmux terminal-host socket pump tests passed\n";
  return 0;
}
