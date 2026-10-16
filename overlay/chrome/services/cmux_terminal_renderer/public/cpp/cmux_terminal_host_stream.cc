// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "chrome/services/cmux_terminal_renderer/public/cpp/cmux_terminal_host_stream.h"

#include <iterator>
#include <limits>
#include <utility>

namespace cmux {
namespace {

bool IsRendererCommand(TerminalHostMessageKind kind) {
  return kind == TerminalHostMessageKind::kInput ||
         kind == TerminalHostMessageKind::kPaste ||
         kind == TerminalHostMessageKind::kViewerSize ||
         kind == TerminalHostMessageKind::kReleaseViewer;
}

}  // namespace

TerminalHostRendererEvent::TerminalHostRendererEvent() = default;
TerminalHostRendererEvent::TerminalHostRendererEvent(
    const TerminalHostRendererEvent&) = default;
TerminalHostRendererEvent& TerminalHostRendererEvent::operator=(
    const TerminalHostRendererEvent&) = default;
TerminalHostRendererEvent::TerminalHostRendererEvent(
    TerminalHostRendererEvent&&) = default;
TerminalHostRendererEvent& TerminalHostRendererEvent::operator=(
    TerminalHostRendererEvent&&) = default;
TerminalHostRendererEvent::~TerminalHostRendererEvent() = default;

const char* TerminalHostStreamErrorMessage(TerminalHostStreamError error) {
  switch (error) {
    case TerminalHostStreamError::kNone:
      return "";
    case TerminalHostStreamError::kDecoderFailed:
      return "terminal-host frame decoding failed";
    case TerminalHostStreamError::kInvalidVersion:
      return "terminal-host stream version mismatch";
    case TerminalHostStreamError::kInvalidEnvelope:
      return "terminal-host stream has invalid flags or request identity";
    case TerminalHostStreamError::kUnexpectedFrame:
      return "terminal-host stream frame arrived out of order";
    case TerminalHostStreamError::kSequenceGap:
      return "terminal-host live sequence has a gap or duplicate";
    case TerminalHostStreamError::kMalformedPayload:
      return "terminal-host stream payload is malformed";
    case TerminalHostStreamError::kResyncRequired:
      return "terminal-host requested a fresh snapshot";
    case TerminalHostStreamError::kTruncated:
      return "terminal-host stream ended in a partial frame";
  }
  return "unknown terminal-host stream error";
}

TerminalHostRendererStream::TerminalHostRendererStream() = default;
TerminalHostRendererStream::~TerminalHostRendererStream() = default;

TerminalHostStreamError TerminalHostRendererStream::Push(
    std::string_view bytes,
    std::vector<TerminalHostRendererEvent>* events) {
  if (!events || failed_) {
    return Fail(TerminalHostStreamError::kDecoderFailed);
  }
  std::vector<TerminalHostFrame> frames;
  const TerminalHostProtocolError decoded = decoder_.Push(bytes, &frames);
  if (decoded != TerminalHostProtocolError::kNone) {
    return Fail(TerminalHostStreamError::kDecoderFailed);
  }
  std::vector<TerminalHostRendererEvent> validated;
  validated.reserve(frames.size());
  for (TerminalHostFrame& frame : frames) {
    const TerminalHostStreamError result =
        Consume(std::move(frame), &validated);
    if (result != TerminalHostStreamError::kNone) {
      return Fail(result);
    }
  }
  events->insert(events->end(), std::make_move_iterator(validated.begin()),
                 std::make_move_iterator(validated.end()));
  return TerminalHostStreamError::kNone;
}

TerminalHostStreamError TerminalHostRendererStream::Finish() {
  if (failed_) {
    return TerminalHostStreamError::kDecoderFailed;
  }
  if (decoder_.Finish() != TerminalHostProtocolError::kNone) {
    return Fail(TerminalHostStreamError::kTruncated);
  }
  // A clean EOF is only valid after the host has published Exit. Before that
  // it is a reconnect condition even if it ended between complete frames.
  if (phase_ != Phase::kExited) {
    return Fail(TerminalHostStreamError::kUnexpectedFrame);
  }
  return TerminalHostStreamError::kNone;
}

TerminalHostStreamError TerminalHostRendererStream::Consume(
    TerminalHostFrame frame,
    std::vector<TerminalHostRendererEvent>* events) {
  if (frame.version != kTerminalHostProtocolVersion) {
    return TerminalHostStreamError::kInvalidVersion;
  }
  if (phase_ == Phase::kExited) {
    return TerminalHostStreamError::kUnexpectedFrame;
  }

  TerminalHostRendererEvent event;
  event.kind = frame.kind;
  event.request_id = frame.request_id;
  event.sequence = frame.sequence;
  if (frame.kind == TerminalHostMessageKind::kResizeAck) {
    if (phase_ != Phase::kLive) {
      return TerminalHostStreamError::kUnexpectedFrame;
    }
    if (frame.flags != 0 || frame.request_id == 0 || frame.sequence != 0) {
      return TerminalHostStreamError::kInvalidEnvelope;
    }
    TerminalHostResizeAck ack;
    if (DecodeTerminalHostResizeAck(
            std::string_view(
                reinterpret_cast<const char*>(frame.payload.data()),
                frame.payload.size()),
            &ack) != TerminalHostProtocolError::kNone) {
      return TerminalHostStreamError::kMalformedPayload;
    }
    event.resize_ack = ack;
    events->push_back(std::move(event));
    return TerminalHostStreamError::kNone;
  }
  if (frame.request_id != 0) {
    return TerminalHostStreamError::kInvalidEnvelope;
  }
  if (phase_ == Phase::kSnapshot) {
    if (frame.kind != TerminalHostMessageKind::kSnapshot) {
      return TerminalHostStreamError::kUnexpectedFrame;
    }
    if (frame.flags != 0) {
      return TerminalHostStreamError::kInvalidEnvelope;
    }
    TerminalHostSnapshot snapshot;
    if (DecodeTerminalHostSnapshot(
            std::string_view(
                reinterpret_cast<const char*>(frame.payload.data()),
                frame.payload.size()),
            &snapshot) != TerminalHostProtocolError::kNone) {
      return TerminalHostStreamError::kMalformedPayload;
    }
    sequence_ = frame.sequence;
    pending_snapshot_ = std::move(snapshot);
    phase_ = Phase::kSnapshotColors;
    return TerminalHostStreamError::kNone;
  }

  const Phase phase_before = phase_;
  if (phase_ == Phase::kSnapshotColors) {
    if (frame.kind != TerminalHostMessageKind::kColors ||
        frame.sequence != sequence_) {
      return TerminalHostStreamError::kUnexpectedFrame;
    }
    if (frame.flags != 0) {
      return TerminalHostStreamError::kInvalidEnvelope;
    }
  } else {
    if (sequence_ == std::numeric_limits<uint64_t>::max() ||
        frame.sequence != sequence_ + 1) {
      return TerminalHostStreamError::kSequenceGap;
    }
    sequence_ = frame.sequence;
    if ((phase_ == Phase::kOutputColors || phase_ == Phase::kResizeColors) &&
        frame.kind != TerminalHostMessageKind::kColors) {
      return TerminalHostStreamError::kUnexpectedFrame;
    }
    if (phase_ != Phase::kOutputColors && phase_ != Phase::kResizeColors &&
        frame.kind != TerminalHostMessageKind::kOutput &&
        frame.kind != TerminalHostMessageKind::kResized && frame.flags != 0) {
      return TerminalHostStreamError::kInvalidEnvelope;
    }
  }

  switch (frame.kind) {
    case TerminalHostMessageKind::kOutput:
      if (phase_ != Phase::kLive) {
        return TerminalHostStreamError::kUnexpectedFrame;
      }
      if ((frame.flags & ~kTerminalHostFlagColorsFollow) != 0) {
        return TerminalHostStreamError::kInvalidEnvelope;
      }
      if ((frame.flags & kTerminalHostFlagColorsFollow) != 0) {
        pending_output_ = std::move(frame.payload);
        phase_ = Phase::kOutputColors;
        return TerminalHostStreamError::kNone;
      }
      event.bytes = std::move(frame.payload);
      break;
    case TerminalHostMessageKind::kResized: {
      if (phase_ != Phase::kLive ||
          frame.flags != kTerminalHostFlagColorsFollow) {
        if (phase_ == Phase::kLive) {
          return TerminalHostStreamError::kInvalidEnvelope;
        }
        return TerminalHostStreamError::kUnexpectedFrame;
      }
      TerminalHostResize resize;
      if (DecodeTerminalHostResize(
              std::string_view(
                  reinterpret_cast<const char*>(frame.payload.data()),
                  frame.payload.size()),
              &resize) != TerminalHostProtocolError::kNone) {
        return TerminalHostStreamError::kMalformedPayload;
      }
      pending_resize_ = std::move(resize);
      phase_ = Phase::kResizeColors;
      return TerminalHostStreamError::kNone;
    }
    case TerminalHostMessageKind::kColors: {
      if (frame.flags != 0) {
        return TerminalHostStreamError::kInvalidEnvelope;
      }
      TerminalHostColors colors;
      if (DecodeTerminalHostColors(
              std::string_view(
                  reinterpret_cast<const char*>(frame.payload.data()),
                  frame.payload.size()),
              &colors) != TerminalHostProtocolError::kNone) {
        return TerminalHostStreamError::kMalformedPayload;
      }
      event.colors = std::move(colors);
      if (phase_before == Phase::kSnapshotColors) {
        if (!pending_snapshot_) {
          return TerminalHostStreamError::kUnexpectedFrame;
        }
        event.kind = TerminalHostMessageKind::kSnapshot;
        event.snapshot = std::move(pending_snapshot_);
        pending_snapshot_.reset();
      } else if (phase_before == Phase::kOutputColors) {
        if (!pending_output_) {
          return TerminalHostStreamError::kUnexpectedFrame;
        }
        event.kind = TerminalHostMessageKind::kOutput;
        event.bytes = std::move(*pending_output_);
        pending_output_.reset();
      } else if (phase_before == Phase::kResizeColors) {
        if (!pending_resize_) {
          return TerminalHostStreamError::kUnexpectedFrame;
        }
        event.kind = TerminalHostMessageKind::kResized;
        event.resize = std::move(pending_resize_);
        pending_resize_.reset();
      }
      phase_ = Phase::kLive;
      break;
    }
    case TerminalHostMessageKind::kTitle:
    case TerminalHostMessageKind::kPwd:
      if (phase_ != Phase::kLive ||
          DecodeTerminalHostUtf8(
              std::string_view(
                  reinterpret_cast<const char*>(frame.payload.data()),
                  frame.payload.size()),
              &event.text) != TerminalHostProtocolError::kNone) {
        return phase_ == Phase::kLive
                   ? TerminalHostStreamError::kMalformedPayload
                   : TerminalHostStreamError::kUnexpectedFrame;
      }
      break;
    case TerminalHostMessageKind::kBell:
      if (phase_ != Phase::kLive || !frame.payload.empty()) {
        return phase_ == Phase::kLive
                   ? TerminalHostStreamError::kMalformedPayload
                   : TerminalHostStreamError::kUnexpectedFrame;
      }
      break;
    case TerminalHostMessageKind::kExit:
      if (phase_ != Phase::kLive || !frame.payload.empty()) {
        return phase_ == Phase::kLive
                   ? TerminalHostStreamError::kMalformedPayload
                   : TerminalHostStreamError::kUnexpectedFrame;
      }
      phase_ = Phase::kExited;
      break;
    case TerminalHostMessageKind::kResyncRequired:
      if (phase_ != Phase::kLive || !frame.payload.empty()) {
        return phase_ == Phase::kLive
                   ? TerminalHostStreamError::kMalformedPayload
                   : TerminalHostStreamError::kUnexpectedFrame;
      }
      return TerminalHostStreamError::kResyncRequired;
    default:
      return TerminalHostStreamError::kUnexpectedFrame;
  }
  events->push_back(std::move(event));
  return TerminalHostStreamError::kNone;
}

TerminalHostStreamError TerminalHostRendererStream::Fail(
    TerminalHostStreamError error) {
  failed_ = true;
  return error;
}

TerminalHostProtocolError EncodeTerminalHostRendererCommand(
    TerminalHostMessageKind kind, std::string_view payload,
    std::vector<uint8_t>* bytes, uint64_t request_id) {
  if (!bytes) {
    return TerminalHostProtocolError::kInvalidArgument;
  }
  if (!IsRendererCommand(kind) ||
      (kind == TerminalHostMessageKind::kViewerSize && payload.size() != 4) ||
      (kind == TerminalHostMessageKind::kReleaseViewer && !payload.empty()) ||
      (request_id != 0 && kind != TerminalHostMessageKind::kViewerSize)) {
    return TerminalHostProtocolError::kMalformedPayload;
  }
  TerminalHostFrame frame;
  frame.kind = kind;
  frame.request_id = request_id;
  frame.payload.assign(payload.begin(), payload.end());
  return EncodeTerminalHostFrame(frame, bytes);
}

}  // namespace cmux
