// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "chrome/services/cmux_terminal_renderer/public/cpp/cmux_terminal_host_stream.h"

#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace {

using Error = cmux::TerminalHostStreamError;
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

std::vector<uint8_t> Encode(
    Kind kind,
    uint64_t sequence,
    std::vector<uint8_t> payload = {},
    uint16_t version = cmux::kTerminalHostProtocolVersion,
    uint32_t flags = 0,
    uint64_t request_id = 0) {
  cmux::TerminalHostFrame frame;
  frame.version = version;
  frame.kind = kind;
  frame.flags = flags;
  frame.request_id = request_id;
  frame.sequence = sequence;
  frame.payload = std::move(payload);
  std::vector<uint8_t> bytes;
  Check(cmux::EncodeTerminalHostFrame(frame, &bytes) ==
            cmux::TerminalHostProtocolError::kNone,
        "test frame encodes");
  return bytes;
}

std::vector<uint8_t> SnapshotPayload(uint16_t cols = 80, uint16_t rows = 24) {
  cmux::TerminalHostSnapshot snapshot;
  snapshot.cols = cols;
  snapshot.rows = rows;
  snapshot.pid = 123;
  snapshot.replay = {'r', 'e', 'p', 'l', 'a', 'y'};
  snapshot.cwd = "/tmp";
  snapshot.command = {"sh", "-l"};
  std::vector<uint8_t> payload;
  Check(cmux::EncodeTerminalHostSnapshot(snapshot, &payload) ==
            cmux::TerminalHostProtocolError::kNone,
        "snapshot payload encodes");
  return payload;
}

cmux::TerminalHostColors SampleColors() {
  cmux::TerminalHostColors colors;
  colors.foreground = cmux::TerminalHostRgb{1, 2, 3};
  colors.background = cmux::TerminalHostRgb{4, 5, 6};
  colors.cursor = cmux::TerminalHostRgb{7, 8, 9};
  colors.cursor_visual = cmux::TerminalHostCursorVisual{
      cmux::TerminalHostCursorStyle::kBar, true};
  colors.palette = {{0, {10, 11, 12}}, {255, {13, 14, 15}}};
  return colors;
}

std::vector<uint8_t> ColorsPayload(
    const cmux::TerminalHostColors& colors = SampleColors()) {
  std::vector<uint8_t> payload;
  Check(cmux::EncodeTerminalHostColors(colors, &payload) ==
            cmux::TerminalHostProtocolError::kNone,
        "colors payload encodes");
  return payload;
}

std::vector<uint8_t> ResizePayload(uint16_t cols = 100, uint16_t rows = 30) {
  cmux::TerminalHostResize resize;
  resize.cols = cols;
  resize.rows = rows;
  resize.replay = {'n', 'e', 'w'};
  std::vector<uint8_t> payload;
  Check(cmux::EncodeTerminalHostResize(resize, &payload) ==
            cmux::TerminalHostProtocolError::kNone,
        "resize payload encodes");
  return payload;
}

std::vector<uint8_t> ResizeAckPayload(uint16_t cols = 100, uint16_t rows = 30,
                                      uint32_t result_flags = 0) {
  cmux::TerminalHostResizeAck ack;
  ack.cols = cols;
  ack.rows = rows;
  ack.result_flags = result_flags;
  std::vector<uint8_t> payload;
  Check(cmux::EncodeTerminalHostResizeAck(ack, &payload) ==
            cmux::TerminalHostProtocolError::kNone,
        "resize acknowledgement payload encodes");
  return payload;
}

void Append(std::vector<uint8_t>* output, const std::vector<uint8_t>& input) {
  output->insert(output->end(), input.begin(), input.end());
}

void Prime(cmux::TerminalHostRendererStream* stream, uint64_t boundary = 7) {
  std::vector<cmux::TerminalHostRendererEvent> events;
  std::vector<uint8_t> bytes;
  Append(&bytes, Encode(Kind::kSnapshot, boundary, SnapshotPayload()));
  Append(&bytes, Encode(Kind::kColors, boundary, ColorsPayload()));
  Check(stream->Push(Bytes(bytes), &events) == Error::kNone &&
            stream->ready() && events.size() == 1 && events[0].snapshot &&
            events[0].colors,
        "stream accepts atomic Snapshot/Colors boundary");
}

void TestPartialAndCoalescedFrames() {
  cmux::TerminalHostRendererStream stream;
  std::vector<uint8_t> bytes;
  Append(&bytes, Encode(Kind::kSnapshot, 41, SnapshotPayload(132, 44)));
  Append(&bytes, Encode(Kind::kColors, 41, ColorsPayload()));
  std::vector<cmux::TerminalHostRendererEvent> events;
  const size_t snapshot_length =
      Encode(Kind::kSnapshot, 41, SnapshotPayload(132, 44)).size();
  size_t offset = 0;
  for (uint8_t byte : bytes) {
    const char character = static_cast<char>(byte);
    Check(stream.Push(std::string_view(&character, 1), &events) == Error::kNone,
          "byte-at-a-time read succeeds");
    ++offset;
    if (offset < bytes.size()) {
      Check(events.empty() && !stream.ready(),
            offset <= snapshot_length
                ? "Snapshot is invisible until same-boundary Colors arrives"
                : "partial Colors cannot expose an unpaired Snapshot");
    }
  }
  Check(stream.ready() && stream.sequence() == 41 && events.size() == 1,
        "partial reads preserve the snapshot boundary");
  Check(events[0].snapshot && events[0].snapshot->cols == 132 &&
            events[0].snapshot->rows == 44 && events[0].colors &&
            *events[0].colors == SampleColors(),
        "partial reads decode exact grid and complete colors");

  std::vector<uint8_t> live;
  Append(&live, Encode(Kind::kOutput, 42, {'a', 'b'}));
  Append(&live, Encode(Kind::kTitle, 43, {'t', 'i', 't', 'l', 'e'}));
  events.clear();
  Check(stream.Push(Bytes(live), &events) == Error::kNone &&
            events.size() == 2 &&
            events[0].bytes == std::vector<uint8_t>({'a', 'b'}) &&
            events[1].text == "title" && stream.sequence() == 43,
        "one read can contain coalesced ordered live frames");
}

void TestColorsCodec() {
  const cmux::TerminalHostColors colors = SampleColors();
  const std::vector<uint8_t> payload = ColorsPayload(colors);
  Check(payload.size() == 27 && payload[0] == 2 && payload[1] == 0 &&
            payload[2] == 0x0f && payload[3] == 0 && payload[17] == 3 &&
            payload[18] == 1,
        "Colors v2 atomically carries three defaults, cursor visual, and "
        "sparse palette");
  cmux::TerminalHostColors decoded;
  Check(cmux::DecodeTerminalHostColors(Bytes(payload), &decoded) ==
                cmux::TerminalHostProtocolError::kNone &&
            decoded == colors,
        "colors wire round trips");

  cmux::TerminalHostColors duplicate = colors;
  duplicate.palette.push_back({0, {99, 98, 97}});
  std::vector<uint8_t> untouched = {0xaa};
  Check(cmux::EncodeTerminalHostColors(duplicate, &untouched) ==
                cmux::TerminalHostProtocolError::kMalformedPayload &&
            untouched == std::vector<uint8_t>({0xaa}),
        "colors encoder rejects duplicate palette indices atomically");

  std::vector<uint8_t> malformed = payload;
  malformed[6] = 1;
  Check(cmux::DecodeTerminalHostColors(Bytes(malformed), &decoded) ==
            cmux::TerminalHostProtocolError::kMalformedPayload,
        "colors decoder rejects nonzero reserved fields");
  malformed = payload;
  malformed[malformed.size() - 4] = 0;
  Check(cmux::DecodeTerminalHostColors(Bytes(malformed), &decoded) ==
            cmux::TerminalHostProtocolError::kMalformedPayload,
        "colors decoder rejects duplicate palette indices");

  cmux::TerminalHostColors legacy = colors;
  legacy.cursor_visual.reset();
  std::vector<uint8_t> legacy_payload(payload);
  legacy_payload[0] = cmux::kTerminalHostColorsVersionV1;
  legacy_payload[2] &= ~8u;
  legacy_payload.erase(legacy_payload.begin() + 17,
                       legacy_payload.begin() + 19);
  Check(cmux::DecodeTerminalHostColors(Bytes(legacy_payload), &decoded) ==
                cmux::TerminalHostProtocolError::kNone &&
            decoded == legacy && !decoded.cursor_visual,
        "Colors v1 remains decodable without a cursor visual");
  legacy_payload[2] |= 8;
  Check(cmux::DecodeTerminalHostColors(Bytes(legacy_payload), &decoded) ==
            cmux::TerminalHostProtocolError::kMalformedPayload,
        "Colors v1 rejects the v2 cursor-visual flag");

  std::vector<uint8_t> partial_visual(8, 0);
  partial_visual[0] = cmux::kTerminalHostColorsVersion;
  partial_visual[2] = 8;
  partial_visual.push_back(
      static_cast<uint8_t>(cmux::TerminalHostCursorStyle::kBar));
  Check(cmux::DecodeTerminalHostColors(Bytes(partial_visual), &decoded) ==
            cmux::TerminalHostProtocolError::kMalformedPayload,
        "Colors v2 rejects a partial cursor style/blink pair");
  partial_visual.push_back(2);
  Check(cmux::DecodeTerminalHostColors(Bytes(partial_visual), &decoded) ==
            cmux::TerminalHostProtocolError::kMalformedPayload,
        "Colors v2 rejects a non-boolean cursor blink value");
  partial_visual[8] = 0;
  partial_visual[9] = 1;
  Check(cmux::DecodeTerminalHostColors(Bytes(partial_visual), &decoded) ==
            cmux::TerminalHostProtocolError::kMalformedPayload,
        "Colors v2 rejects an unknown cursor style");

  std::vector<uint8_t> missing_visual(8, 0);
  missing_visual[0] = cmux::kTerminalHostColorsVersion;
  Check(cmux::DecodeTerminalHostColors(Bytes(missing_visual), &decoded) ==
            cmux::TerminalHostProtocolError::kMalformedPayload,
        "Colors v2 requires one authoritative cursor visual pair");

  std::vector<uint8_t> unknown_version(payload);
  unknown_version[0] = 3;
  Check(cmux::DecodeTerminalHostColors(Bytes(unknown_version), &decoded) ==
            cmux::TerminalHostProtocolError::kMalformedPayload,
        "Colors rejects unknown schema version 3");

  cmux::TerminalHostColors invalid_visual = colors;
  invalid_visual.cursor_visual->style =
      static_cast<cmux::TerminalHostCursorStyle>(4);
  untouched = {0xbb};
  Check(cmux::EncodeTerminalHostColors(invalid_visual, &untouched) ==
                cmux::TerminalHostProtocolError::kMalformedPayload &&
            untouched == std::vector<uint8_t>({0xbb}),
        "Colors encoder rejects an unknown cursor style atomically");

  cmux::TerminalHostColors missing_visual_to_encode;
  untouched = {0xcc};
  Check(cmux::EncodeTerminalHostColors(missing_visual_to_encode, &untouched) ==
                cmux::TerminalHostProtocolError::kMalformedPayload &&
            untouched == std::vector<uint8_t>({0xcc}),
        "Colors v2 encoder requires the authoritative cursor pair atomically");
}

void TestOrderingAndSequenceFences() {
  {
    cmux::TerminalHostRendererStream stream;
    std::vector<cmux::TerminalHostRendererEvent> events;
    Check(stream.Push(Bytes(Encode(Kind::kOutput, 1, {'x'})), &events) ==
              Error::kUnexpectedFrame,
          "Snapshot is mandatory before Output");
  }
  {
    cmux::TerminalHostRendererStream stream;
    std::vector<cmux::TerminalHostRendererEvent> events;
    Check(stream.Push(Bytes(Encode(Kind::kSnapshot, 9, SnapshotPayload())),
                      &events) == Error::kNone,
          "Snapshot alone is buffered semantically");
    Check(stream.Push(Bytes(Encode(Kind::kColors, 10, ColorsPayload())),
                      &events) == Error::kUnexpectedFrame,
          "initial Colors must share the Snapshot boundary");
  }
  {
    cmux::TerminalHostRendererStream stream;
    Prime(&stream);
    std::vector<cmux::TerminalHostRendererEvent> events;
    Check(stream.Push(Bytes(Encode(Kind::kOutput, 9, {'x'})), &events) ==
              Error::kSequenceGap,
          "live sequence gap is terminal");
  }
  {
    cmux::TerminalHostRendererStream stream;
    Prime(&stream);
    std::vector<cmux::TerminalHostRendererEvent> events;
    Check(stream.Push(Bytes(Encode(Kind::kOutput, 7, {'x'})), &events) ==
              Error::kSequenceGap,
          "duplicate live sequence is terminal");
  }
  {
    cmux::TerminalHostRendererStream stream;
    Prime(&stream);
    std::vector<cmux::TerminalHostRendererEvent> events;
    Check(stream.Push(Bytes(Encode(Kind::kResized, 8, ResizePayload(), 1,
                                   cmux::kTerminalHostFlagColorsFollow)),
                      &events) == Error::kNone &&
              events.empty(),
          "Resized remains invisible until full-state Colors");
    events.clear();
    Check(stream.Push(Bytes(Encode(Kind::kOutput, 9, {'x'})), &events) ==
              Error::kUnexpectedFrame,
          "Resized must be followed by full-state Colors");
  }
  {
    cmux::TerminalHostRendererStream stream;
    Prime(&stream);
    std::vector<cmux::TerminalHostRendererEvent> events;
    std::vector<uint8_t> frames;
    Append(&frames, Encode(Kind::kResized, 8, ResizePayload(), 1,
                           cmux::kTerminalHostFlagColorsFollow));
    Append(&frames, Encode(Kind::kColors, 9, ColorsPayload()));
    Append(&frames, Encode(Kind::kOutput, 10, {'x'}));
    Check(stream.Push(Bytes(frames), &events) == Error::kNone &&
              events.size() == 2 && events[0].kind == Kind::kResized &&
              events[0].resize && events[0].resize->cols == 100 &&
              events[0].colors && events[1].kind == Kind::kOutput &&
              stream.ready() && stream.sequence() == 10,
          "Resized/Colors/Output coalesce without losing sequence");
  }
}

void TestFlaggedOutputAtomicity() {
  {
    cmux::TerminalHostRendererStream stream;
    Prime(&stream);
    std::vector<cmux::TerminalHostRendererEvent> events;
    Check(stream.Push(Bytes(Encode(Kind::kOutput, 8, {'o', 's', 'c'}, 1,
                                   cmux::kTerminalHostFlagColorsFollow)),
                      &events) == Error::kNone &&
              events.empty(),
          "COLORS_FOLLOW Output is invisible before Colors");
    const std::vector<uint8_t> colors =
        Encode(Kind::kColors, 9, ColorsPayload());
    Check(stream.Push(Bytes(colors).substr(0, colors.size() - 1), &events) ==
                  Error::kNone &&
              events.empty(),
          "partial Colors cannot expose flagged Output");
    Check(stream.Push(Bytes(colors).substr(colors.size() - 1), &events) ==
                  Error::kNone &&
              events.size() == 1 && events[0].kind == Kind::kOutput &&
              events[0].sequence == 9 &&
              events[0].bytes == std::vector<uint8_t>({'o', 's', 'c'}) &&
              events[0].colors && *events[0].colors == SampleColors(),
          "flagged Output and next Colors publish as one sequence boundary");
  }
  {
    cmux::TerminalHostRendererStream stream;
    Prime(&stream);
    std::vector<cmux::TerminalHostRendererEvent> events;
    Check(stream.Push(Bytes(Encode(Kind::kOutput, 8, {'x'}, 1,
                                   cmux::kTerminalHostFlagColorsFollow)),
                      &events) == Error::kNone &&
              stream.Push(Bytes(Encode(Kind::kTitle, 9, {'t'})), &events) ==
                  Error::kUnexpectedFrame,
          "flagged Output requires immediately-next Colors");
  }
  {
    cmux::TerminalHostRendererStream stream;
    Prime(&stream);
    std::vector<cmux::TerminalHostRendererEvent> events;
    Check(stream.Push(Bytes(Encode(Kind::kOutput, 8, {'x'}, 1, 2)), &events) ==
              Error::kInvalidEnvelope,
          "Output rejects unknown flag bits");
  }
  {
    cmux::TerminalHostRendererStream stream;
    Prime(&stream);
    std::vector<cmux::TerminalHostRendererEvent> events;
    Check(stream.Push(Bytes(Encode(Kind::kResized, 8, ResizePayload())),
                      &events) == Error::kInvalidEnvelope,
          "Resized requires COLORS_FOLLOW");
  }
  {
    cmux::TerminalHostRendererStream stream;
    Prime(&stream);
    std::vector<cmux::TerminalHostRendererEvent> events;
    Check(stream.Push(Bytes(Encode(Kind::kTitle, 8, {'t'}, 1,
                                   cmux::kTerminalHostFlagColorsFollow)),
                      &events) == Error::kInvalidEnvelope,
          "metadata rejects COLORS_FOLLOW");
  }
  {
    cmux::TerminalHostRendererStream stream;
    Prime(&stream);
    std::vector<cmux::TerminalHostRendererEvent> events;
    Check(stream.Push(Bytes(Encode(Kind::kColors, 8, ColorsPayload(), 1,
                                   cmux::kTerminalHostFlagColorsFollow)),
                      &events) == Error::kInvalidEnvelope,
          "Colors frames reject every header flag");
  }
}

void TestTargetedResizeAcknowledgements() {
  {
    cmux::TerminalHostRendererStream stream;
    Prime(&stream);
    std::vector<uint8_t> bytes;
    Append(&bytes, Encode(Kind::kResized, 8, ResizePayload(), 1,
                          cmux::kTerminalHostFlagColorsFollow));
    Append(&bytes, Encode(Kind::kColors, 9, ColorsPayload()));
    Append(&bytes,
           Encode(Kind::kResizeAck, 0,
                  ResizeAckPayload(
                      100, 30, cmux::kTerminalHostResizeAckCanonicalChanged),
                  1, 0, 42));
    std::vector<cmux::TerminalHostRendererEvent> events;
    Check(stream.Push(Bytes(bytes), &events) == Error::kNone &&
              events.size() == 2 && events[0].kind == Kind::kResized &&
              events[1].kind == Kind::kResizeAck &&
              events[1].request_id == 42 && events[1].resize_ack &&
              events[1].resize_ack->cols == 100 &&
              events[1].resize_ack->rows == 30 &&
              events[1].resize_ack->result_flags ==
                  cmux::kTerminalHostResizeAckCanonicalChanged &&
              stream.sequence() == 9,
          "changed resize publishes its pair before a sequence-neutral ack");

    events.clear();
    Check(stream.Push(Bytes(Encode(Kind::kResizeAck, 0,
                                   ResizeAckPayload(100, 30), 1, 0, 43)),
                      &events) == Error::kNone &&
              events.size() == 1 && events[0].request_id == 43 &&
              events[0].resize_ack && stream.sequence() == 9,
          "unchanged resize ack does not consume the live sequence");
  }
  {
    cmux::TerminalHostRendererStream stream;
    std::vector<cmux::TerminalHostRendererEvent> events;
    Check(stream.Push(
              Bytes(Encode(Kind::kResizeAck, 0, ResizeAckPayload(), 1, 0, 1)),
              &events) == Error::kUnexpectedFrame,
          "resize ack is rejected before Snapshot/Colors");
  }
  for (const auto& [sequence, flags, request_id, expected] :
       std::vector<std::tuple<uint64_t, uint32_t, uint64_t, Error>>{
           {0, 0, 0, Error::kInvalidEnvelope},
           {8, 0, 1, Error::kInvalidEnvelope},
           {0, cmux::kTerminalHostFlagColorsFollow, 1, Error::kInvalidEnvelope},
       }) {
    cmux::TerminalHostRendererStream stream;
    Prime(&stream);
    std::vector<cmux::TerminalHostRendererEvent> events;
    Check(stream.Push(Bytes(Encode(Kind::kResizeAck, sequence,
                                   ResizeAckPayload(), 1, flags, request_id)),
                      &events) == expected,
          "resize ack rejects an invalid control envelope");
  }
  {
    cmux::TerminalHostRendererStream stream;
    Prime(&stream);
    std::vector<cmux::TerminalHostRendererEvent> events;
    Check(stream.Push(Bytes(Encode(Kind::kResizeAck, 0, {1, 2, 3}, 1, 0, 1)),
                      &events) == Error::kMalformedPayload,
          "resize ack rejects malformed payloads");
  }
  {
    cmux::TerminalHostRendererStream stream;
    Prime(&stream);
    std::vector<cmux::TerminalHostRendererEvent> events;
    Check(stream.Push(Bytes(Encode(Kind::kResized, 8, ResizePayload(), 1,
                                   cmux::kTerminalHostFlagColorsFollow)),
                      &events) == Error::kNone &&
              stream.Push(Bytes(Encode(Kind::kResizeAck, 0, ResizeAckPayload(),
                                       1, 0, 1)),
                          &events) == Error::kUnexpectedFrame,
          "resize ack cannot split Resized from Colors");
  }
}

void TestMalformedAndReconnectConditions() {
  {
    cmux::TerminalHostRendererStream stream;
    std::vector<cmux::TerminalHostRendererEvent> events;
    Check(stream.Push(Bytes(Encode(Kind::kSnapshot, 0, SnapshotPayload(), 2)),
                      &events) == Error::kInvalidVersion,
          "stream requires the negotiated protocol version");
  }
  {
    cmux::TerminalHostRendererStream stream;
    std::vector<cmux::TerminalHostRendererEvent> events;
    Check(
        stream.Push(Bytes(Encode(Kind::kSnapshot, 0, SnapshotPayload(), 1, 1)),
                    &events) == Error::kInvalidEnvelope,
        "stream rejects nonzero flags");
  }
  {
    cmux::TerminalHostRendererStream stream;
    std::vector<cmux::TerminalHostRendererEvent> events;
    Check(stream.Push(
              Bytes(Encode(Kind::kSnapshot, 0, SnapshotPayload(), 1, 0, 17)),
              &events) == Error::kInvalidEnvelope,
          "stream rejects control request IDs on live data");
  }
  {
    cmux::TerminalHostRendererStream stream;
    Prime(&stream);
    std::vector<cmux::TerminalHostRendererEvent> events;
    Check(stream.Push(Bytes(Encode(Kind::kTitle, 8, {0xc0, 0x80})), &events) ==
              Error::kMalformedPayload,
          "metadata must be canonical UTF-8");
  }
  {
    cmux::TerminalHostRendererStream stream;
    Prime(&stream);
    std::vector<cmux::TerminalHostRendererEvent> events;
    Check(stream.Push(Bytes(Encode(Kind::kBell, 8, {'x'})), &events) ==
              Error::kMalformedPayload,
          "Bell payload must be empty");
  }
  {
    cmux::TerminalHostRendererStream stream;
    Prime(&stream);
    std::vector<cmux::TerminalHostRendererEvent> events;
    Check(stream.Push(Bytes(Encode(Kind::kResyncRequired, 8)), &events) ==
              Error::kResyncRequired,
          "ResyncRequired forces a newly minted snapshot");
  }
  {
    cmux::TerminalHostRendererStream stream;
    Prime(&stream);
    Check(stream.Finish() == Error::kUnexpectedFrame,
          "clean socket EOF before Exit reconnects");
  }
  {
    cmux::TerminalHostRendererStream stream;
    std::vector<cmux::TerminalHostRendererEvent> events;
    const std::vector<uint8_t> snapshot =
        Encode(Kind::kSnapshot, 0, SnapshotPayload());
    Check(stream.Push(Bytes(snapshot).substr(0, 17), &events) == Error::kNone &&
              stream.Finish() == Error::kTruncated,
          "partial frame at disconnect is distinguished from clean EOF");
  }
  {
    cmux::TerminalHostRendererStream stream;
    Prime(&stream);
    std::vector<cmux::TerminalHostRendererEvent> events;
    Check(stream.Push(Bytes(Encode(Kind::kExit, 8)), &events) == Error::kNone &&
              stream.exited() && stream.Finish() == Error::kNone,
          "ordered Exit permits clean shutdown");
  }
}

void TestRendererCommands() {
  std::vector<uint8_t> bytes;
  Check(cmux::EncodeTerminalHostRendererCommand(Kind::kInput, "abc", &bytes) ==
            cmux::TerminalHostProtocolError::kNone,
        "Input command encodes");
  cmux::TerminalHostFrameDecoder decoder;
  std::vector<cmux::TerminalHostFrame> frames;
  Check(decoder.Push(Bytes(bytes), &frames) ==
                cmux::TerminalHostProtocolError::kNone &&
            frames.size() == 1 && frames[0].kind == Kind::kInput &&
            frames[0].request_id == 0 && frames[0].sequence == 0 &&
            frames[0].payload == std::vector<uint8_t>({'a', 'b', 'c'}),
        "Input stays outside the live sequence");

  std::vector<uint8_t> viewer_payload;
  Check(cmux::EncodeTerminalHostViewerSize(120, 40, &viewer_payload) ==
                cmux::TerminalHostProtocolError::kNone &&
            cmux::EncodeTerminalHostRendererCommand(
                Kind::kViewerSize, Bytes(viewer_payload), &bytes, 42) ==
                cmux::TerminalHostProtocolError::kNone,
        "ViewerSize command uses the canonical four-byte payload");
  frames.clear();
  cmux::TerminalHostFrameDecoder viewer_decoder;
  Check(viewer_decoder.Push(Bytes(bytes), &frames) ==
                cmux::TerminalHostProtocolError::kNone &&
            frames.size() == 1 && frames[0].request_id == 42 &&
            frames[0].sequence == 0,
        "acknowledged ViewerSize carries its nonzero request identity");
  Check(cmux::EncodeTerminalHostRendererCommand(Kind::kViewerSize, "bad",
                                                &bytes) ==
            cmux::TerminalHostProtocolError::kMalformedPayload,
        "ViewerSize rejects a malformed size");
  Check(cmux::EncodeTerminalHostRendererCommand(Kind::kReleaseViewer, "",
                                                &bytes) ==
                cmux::TerminalHostProtocolError::kNone &&
            cmux::EncodeTerminalHostRendererCommand(Kind::kReleaseViewer, "x",
                                                    &bytes) ==
                cmux::TerminalHostProtocolError::kMalformedPayload,
        "ReleaseViewer is an empty orderly-detach command");
  Check(cmux::EncodeTerminalHostRendererCommand(Kind::kTerminate, "", &bytes) ==
            cmux::TerminalHostProtocolError::kMalformedPayload,
        "renderer command encoder cannot escalate to Terminate");
  Check(
      cmux::EncodeTerminalHostRendererCommand(Kind::kInput, "x", &bytes, 42) ==
          cmux::TerminalHostProtocolError::kMalformedPayload,
      "only ViewerSize may carry a renderer request identity");
}

}  // namespace

int main() {
  TestPartialAndCoalescedFrames();
  TestColorsCodec();
  TestOrderingAndSequenceFences();
  TestFlaggedOutputAtomicity();
  TestTargetedResizeAcknowledgements();
  TestMalformedAndReconnectConditions();
  TestRendererCommands();
  if (failures != 0) {
    std::cerr << failures << " failure(s)\n";
    return 1;
  }
  std::cout << "cmux terminal-host renderer stream tests passed\n";
  return 0;
}
