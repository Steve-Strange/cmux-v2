// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef CHROME_SERVICES_CMUX_TERMINAL_RENDERER_PUBLIC_CPP_CMUX_TERMINAL_HOST_STREAM_H_
#define CHROME_SERVICES_CMUX_TERMINAL_RENDERER_PUBLIC_CPP_CMUX_TERMINAL_HOST_STREAM_H_

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "chrome/services/cmux_terminal_renderer/public/cpp/cmux_terminal_host_protocol.h"

namespace cmux {

enum class TerminalHostStreamError {
  kNone,
  kDecoderFailed,
  kInvalidVersion,
  kInvalidEnvelope,
  kUnexpectedFrame,
  kSequenceGap,
  kMalformedPayload,
  kResyncRequired,
  kTruncated,
};

const char* TerminalHostStreamErrorMessage(TerminalHostStreamError error);

// One semantically validated host-to-renderer event. Snapshot, Resized, and
// Output marked COLORS_FOLLOW are emitted only after their required Colors
// frame has also validated, and carry bytes/replay + complete sparse color
// state as one publication boundary. A standalone live Colors event carries
// only `colors`; no callback can observe half an atomic color transition.
struct TerminalHostRendererEvent {
  TerminalHostRendererEvent();
  TerminalHostRendererEvent(const TerminalHostRendererEvent&);
  TerminalHostRendererEvent& operator=(const TerminalHostRendererEvent&);
  TerminalHostRendererEvent(TerminalHostRendererEvent&&);
  TerminalHostRendererEvent& operator=(TerminalHostRendererEvent&&);
  ~TerminalHostRendererEvent();

  TerminalHostMessageKind kind = TerminalHostMessageKind::kOutput;
  uint64_t request_id = 0;
  uint64_t sequence = 0;
  std::optional<TerminalHostSnapshot> snapshot;
  std::optional<TerminalHostResize> resize;
  std::optional<TerminalHostResizeAck> resize_ack;
  std::optional<TerminalHostColors> colors;
  std::vector<uint8_t> bytes;
  std::string text;
};

// Incrementally validates the already-authenticated renderer stream beginning
// immediately before Snapshot. It enforces the atomic Snapshot/Colors
// boundary, full-state Colors after every Resized frame, and a contiguous
// global live sequence. Any error is terminal; callers must close and mint a
// fresh capability rather than continuing a potentially corrupt mirror.
class TerminalHostRendererStream {
 public:
  TerminalHostRendererStream();
  TerminalHostRendererStream(const TerminalHostRendererStream&) = delete;
  TerminalHostRendererStream& operator=(const TerminalHostRendererStream&) =
      delete;
  ~TerminalHostRendererStream();

  TerminalHostStreamError Push(std::string_view bytes,
                               std::vector<TerminalHostRendererEvent>* events);
  TerminalHostStreamError Finish();

  bool ready() const { return phase_ == Phase::kLive; }
  bool exited() const { return phase_ == Phase::kExited; }
  bool failed() const { return failed_; }
  uint64_t sequence() const { return sequence_; }

 private:
  enum class Phase {
    kSnapshot,
    kSnapshotColors,
    kLive,
    kOutputColors,
    kResizeColors,
    kExited,
  };

  TerminalHostStreamError Consume(
      TerminalHostFrame frame,
      std::vector<TerminalHostRendererEvent>* events);
  TerminalHostStreamError Fail(TerminalHostStreamError error);

  TerminalHostFrameDecoder decoder_;
  Phase phase_ = Phase::kSnapshot;
  std::optional<TerminalHostSnapshot> pending_snapshot_;
  std::optional<std::vector<uint8_t>> pending_output_;
  std::optional<TerminalHostResize> pending_resize_;
  uint64_t sequence_ = 0;
  bool failed_ = false;
};

// Serializes one renderer-to-host command. Commands are outside the live host
// sequence and always carry sequence=0. A negotiated ViewerSize may carry a
// nonzero request_id for its targeted ResizeAck; other commands may not.
TerminalHostProtocolError EncodeTerminalHostRendererCommand(
    TerminalHostMessageKind kind, std::string_view payload,
    std::vector<uint8_t>* bytes, uint64_t request_id = 0);

}  // namespace cmux

#endif  // CHROME_SERVICES_CMUX_TERMINAL_RENDERER_PUBLIC_CPP_CMUX_TERMINAL_HOST_STREAM_H_
