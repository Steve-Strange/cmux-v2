// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "chrome/browser/cmux_term/cmux_terminal_mouse_router.h"
#include "chrome/services/cmux_terminal_renderer/cmux_terminal_ingress_drain_queue.h"
#include "chrome/services/cmux_terminal_renderer/cmux_terminal_output_drain_queue.h"
#include "chrome/services/cmux_terminal_renderer/cmux_terminal_tick_coalescer.h"

namespace {

int failures = 0;
int checks = 0;

void Check(bool condition, const char* message) {
  ++checks;
  if (!condition) {
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
  }
}

// Pure ordering model for the cross-process publication contract. The actual
// implementation is split between CmuxGhosttyTerminalView and
// CmuxTerminalRendererService; keeping this model dependency-free lets the
// contract run in the host suite without Chromium, AppKit, Metal, or Mojo.
struct PublicationModel {
  uint64_t browser_geometry_epoch = 1;
  uint64_t renderer_frame_context = 1;
  uint64_t transition_epoch = 0;
  bool transition_in_flight = true;
  bool physical_resize_pending = false;
  bool canonical_replay_parsed = false;
  bool canonical_colors_parsed = false;
  uint32_t expected_width = 0;
  uint32_t expected_height = 0;

  bool CanAcceptFrame(uint64_t context, uint32_t width, uint32_t height) const {
    return context == browser_geometry_epoch && width != 0 && height != 0 &&
           width == expected_width && height == expected_height;
  }

  void ObservePhysicalResize() { physical_resize_pending = true; }

  void BeginCanonicalReplay() {
    transition_epoch = ++browser_geometry_epoch;
    transition_in_flight = true;
    expected_width = 0;
    expected_height = 0;
    canonical_replay_parsed = false;
    canonical_colors_parsed = false;
    // The renderer deliberately retains its preceding context while changing
    // grid and parsing replay+colors.
  }

  void ParseCanonicalPayload(const std::string& payload) {
    canonical_replay_parsed = payload.find("REPLAY") != std::string::npos;
    canonical_colors_parsed = payload.find("OSC-COLORS") != std::string::npos;
  }

  bool PublishCanonicalGrid(uint64_t epoch, uint32_t width, uint32_t height) {
    if (epoch != transition_epoch || !canonical_replay_parsed ||
        !canonical_colors_parsed) {
      return false;
    }
    renderer_frame_context = epoch;
    expected_width = width;
    expected_height = height;
    return true;
  }

  std::optional<bool> PresentFrame(uint64_t context,
                                   uint32_t width,
                                   uint32_t height) {
    if (!CanAcceptFrame(context, width, height)) {
      return std::nullopt;
    }
    if (!transition_in_flight || context != transition_epoch) {
      return false;
    }
    transition_in_flight = false;
    const bool reissue_physical_resize = physical_resize_pending;
    physical_resize_pending = false;
    return reissue_physical_resize;
  }
};

// A direct renderer can attach after cmux-tui's host parser has already
// observed and deduplicated OSC 7. Snapshot.cwd is therefore authoritative
// client metadata and must be published, including an absent-value clear,
// before the snapshot replay can complete on the renderer worker.
struct DirectSnapshotMetadataModel {
  std::vector<std::string> client_pipe;

  void Apply(std::optional<std::string> cwd) {
    client_pipe.push_back("pwd:" + cwd.value_or(std::string()));
    client_pipe.push_back("replay");
  }
};

struct SparseColorModel {
  static constexpr uint32_t kConfiguredBackground = 0x18202a;

  uint32_t exposed_background = kConfiguredBackground;
  int renderer_output_tasks = 0;
  std::string last_payload;

  void SubmitOutputAndColors(std::string output,
                             std::optional<uint32_t> background) {
    ++renderer_output_tasks;
    last_payload = std::move(output);
    last_payload.append("RESET-PALETTE+RESET-DYNAMIC-COLORS+");
    if (background) {
      last_payload.append("OSC-11");
      exposed_background = *background;
    } else {
      exposed_background = kConfiguredBackground;
    }
  }
};

struct DirectResizeModel {
  uint16_t columns = 100;
  uint16_t rows = 40;
  uint32_t width_px = 1010;
  uint32_t height_px = 810;
  uint32_t cell_width_px = 10;
  uint32_t cell_height_px = 20;
  int speculative_reflows = 0;
  int viewer_size_requests = 0;
  int authoritative_reflows = 0;
  uint32_t sent_width_px = 0;
  uint32_t sent_height_px = 0;
  uint16_t requested_columns = 0;
  uint16_t requested_rows = 0;

  std::optional<std::pair<uint16_t, uint16_t>> RequestPhysicalSize(
      uint32_t width,
      uint32_t height) {
    const uint64_t padding_x = width_px - columns * cell_width_px;
    const uint64_t padding_y = height_px - rows * cell_height_px;
    const uint16_t desired_columns = static_cast<uint16_t>(std::max<uint64_t>(
        (width > padding_x ? width - padding_x : 0) / cell_width_px, 1));
    const uint16_t desired_rows = static_cast<uint16_t>(std::max<uint64_t>(
        (height > padding_y ? height - padding_y : 0) / cell_height_px, 1));
    sent_width_px = width;
    sent_height_px = height;
    if (desired_columns == requested_columns &&
        desired_rows == requested_rows) {
      return std::nullopt;
    }
    requested_columns = desired_columns;
    requested_rows = desired_rows;
    ++viewer_size_requests;
    return std::pair<uint16_t, uint16_t>(desired_columns, desired_rows);
  }

  void ApplyHostResize(uint16_t new_columns, uint16_t new_rows) {
    columns = new_columns;
    rows = new_rows;
    ++authoritative_reflows;
  }
};

struct UnchangedResizeAckModel {
  uint16_t columns = 100;
  uint16_t rows = 40;
  uint32_t canonical_width_px = 1010;
  uint32_t canonical_height_px = 810;
  uint32_t expected_width_px = 1010;
  uint32_t expected_height_px = 810;
  uint64_t external_context = 4;
  uint64_t transition_epoch = 0;
  bool transition_in_flight = false;
  bool physical_resize_pending = false;
  int grid_mutations = 0;
  int refreshes = 0;
  int replacement_frames = 0;
  int physical_resize_reissues = 0;

  void Begin(uint64_t epoch, bool pending_physical_resize = false) {
    transition_epoch = epoch;
    transition_in_flight = true;
    physical_resize_pending = pending_physical_resize;
    expected_width_px = 0;
    expected_height_px = 0;
  }

  bool UnchangedAck(uint16_t ack_columns,
                    uint16_t ack_rows,
                    uint64_t epoch,
                    bool scale_changed) {
    if (!transition_in_flight || epoch != transition_epoch ||
        ack_columns != columns || ack_rows != rows) {
      return false;
    }
    external_context = epoch;
    if (scale_changed) {
      ++grid_mutations;
      ++refreshes;
      ++replacement_frames;
      return true;
    }
    expected_width_px = canonical_width_px;
    expected_height_px = canonical_height_px;
    transition_in_flight = false;
    if (physical_resize_pending) {
      physical_resize_pending = false;
      ++physical_resize_reissues;
    }
    return true;
  }

  bool AcceptOrdinaryFrame(uint64_t epoch,
                           uint32_t width,
                           uint32_t height) const {
    return !transition_in_flight && epoch == external_context &&
           width == expected_width_px && height == expected_height_px;
  }
};

struct DebouncedViewerReleaseModel {
  bool visible = true;
  bool release_timer_armed = false;
  bool viewer_released = false;
  uint64_t visibility_generation = 0;
  uint64_t timer_generation = 0;
  int releases = 0;
  int requeues = 0;
  int suppressed_intermediate_frames = 0;
  int restored_frames = 0;
  bool restore_pending = false;

  void SetVisible(bool next) {
    if (visible == next) {
      return;
    }
    visible = next;
    ++visibility_generation;
    if (!visible) {
      release_timer_armed = true;
      timer_generation = visibility_generation;
      return;
    }
    release_timer_armed = false;
    if (viewer_released) {
      restore_pending = true;
      viewer_released = false;
      ++requeues;
    }
  }

  void FireTimer(uint64_t generation) {
    if (!release_timer_armed || visible || generation != timer_generation ||
        generation != visibility_generation) {
      return;
    }
    release_timer_armed = false;
    viewer_released = true;
    ++releases;
  }

  void IntermediateFrame() {
    if (restore_pending) {
      ++suppressed_intermediate_frames;
    } else {
      ++restored_frames;
    }
  }

  void CompleteRestore() {
    if (!restore_pending) {
      return;
    }
    restore_pending = false;
    ++restored_frames;
  }
};

struct MouseButtonAcknowledgementModel {
  bool durable_release_pending = true;
  bool deferred_right_press = true;
  int native_right_clicks = 0;
  int native_right_ups = 0;
  std::vector<std::pair<int, char>> pending_native_events{
      {4, 'U'}, {1, 'D'}, {3, 'D'}, {2, 'U'}};

  void RightPressAck(bool accepted, bool consumed) {
    if (!accepted || !consumed) {
      ++native_right_clicks;
    }
  }

  void ReleaseAck(bool accepted, bool consumed) {
    if (!accepted || !consumed) {
      ++native_right_ups;
    }
    if (accepted) {
      durable_release_pending = false;
    }
  }

  std::string TeardownFallback() {
    deferred_right_press = false;
    durable_release_pending = true;
    std::sort(pending_native_events.begin(), pending_native_events.end());
    std::string order;
    for (const auto& [timestamp, event] : pending_native_events) {
      (void)timestamp;
      order.push_back(event);
    }
    pending_native_events.clear();
    return order;
  }
};

struct VisibilityModel {
  bool visible = true;
  bool dirty = false;
  bool viewer_reserved = false;
  bool has_latest_viewer_size = false;
  int delivered_frames = 0;
  int refreshes = 0;
  int state_updates = 0;
  int viewer_releases = 0;
  int viewer_requeues = 0;

  void Output() {
    ++state_updates;
    dirty = true;
    if (visible) {
      ++delivered_frames;
      dirty = false;
    }
  }

  void RequestViewerSize() {
    has_latest_viewer_size = true;
    if (visible) {
      viewer_reserved = true;
    }
  }

  void SetVisible(bool next) {
    if (visible == next) {
      return;
    }
    visible = next;
    if (!visible) {
      viewer_reserved = false;
      ++viewer_releases;
    } else {
      if (has_latest_viewer_size) {
        viewer_reserved = true;
        ++viewer_requeues;
      }
      ++refreshes;
      if (dirty) {
        ++delivered_frames;
        dirty = false;
      }
    }
  }
};

struct DirectCutoverModel {
  bool socket_installed = false;
  bool direct_snapshot_applied = false;
  std::string parsed;

  void CompatibilityOutput(std::string bytes) {
    if (!socket_installed) {
      parsed.append(bytes);
    }
  }

  void CompatibilityReplay(std::string replay) {
    if (!socket_installed) {
      parsed = std::move(replay);
    }
  }

  void InstallDirectSocket() { socket_installed = true; }

  void DirectSnapshot(std::string replay) {
    parsed = std::move(replay);
    direct_snapshot_applied = true;
  }
};

struct DirectInputHandoffModel {
  enum class Route { kCompatibility, kBuffered, kDirect };

  Route route = Route::kCompatibility;
  std::string browser_queue;
  std::string renderer_buffer;
  std::string pty;
  std::string client_pipe;
  bool host_barrier_observed = false;

  void Input(std::string bytes) {
    switch (route) {
      case Route::kCompatibility:
        browser_queue.append(bytes);
        client_pipe.append("input;");
        break;
      case Route::kBuffered:
        renderer_buffer.append(bytes);
        break;
      case Route::kDirect:
        pty.append(bytes);
        break;
    }
  }

  void Begin() {
    route = Route::kBuffered;
    // Ready shares the client pipe with compatibility input.
    client_pipe.append("ready;");
  }

  void DrainAndMint() {
    pty.append(browser_queue);
    browser_queue.clear();
    // MintCapability follows drained Input on one host admin stream; its
    // response is the cross-socket host-side barrier.
    host_barrier_observed = true;
  }

  bool Attach() {
    if (!host_barrier_observed || route != Route::kBuffered) {
      return false;
    }
    pty.append(renderer_buffer);
    renderer_buffer.clear();
    route = Route::kDirect;
    return true;
  }

  void Cancel() {
    browser_queue.append(renderer_buffer);
    renderer_buffer.clear();
    route = Route::kCompatibility;
    client_pipe.append("input;cancelled;");
  }
};

struct AttachReplyFenceModel {
  uint64_t renderer_incarnation = 3;
  uint64_t cutover_id = 0;
  bool attach_in_flight = false;
  bool cancel_pending = false;
  bool direct = false;
  bool transition_in_flight = false;
  bool grid_authoritative = true;
  int compatibility_snapshot_requests = 0;

  void Begin(uint64_t next_cutover_id) {
    cutover_id = next_cutover_id;
    attach_in_flight = true;
    cancel_pending = false;
    transition_in_flight = true;
  }

  void RequestCancel() { cancel_pending = true; }

  bool AttachReply(uint64_t reply_renderer,
                   uint64_t reply_cutover,
                   bool success) {
    if (reply_renderer != renderer_incarnation || reply_cutover != cutover_id ||
        !attach_in_flight || cancel_pending) {
      return false;
    }
    attach_in_flight = false;
    direct = success;
    if (success) {
      cutover_id = 0;
    }
    return true;
  }

  void Cancelled(uint64_t cancelled_cutover) {
    if (cancelled_cutover != cutover_id) {
      return;
    }
    attach_in_flight = false;
    cancel_pending = false;
    direct = false;
    cutover_id = 0;
    transition_in_flight = true;
    grid_authoritative = false;
    ++compatibility_snapshot_requests;
  }

  void HostDisconnected() {
    attach_in_flight = false;
    cancel_pending = false;
    direct = false;
    cutover_id = 0;
    transition_in_flight = true;
    grid_authoritative = false;
    ++compatibility_snapshot_requests;
  }
};

struct DirectAttachPrerequisiteModel {
  enum class Phase { kDraining, kMinting, kCancelling };

  Phase phase = Phase::kDraining;
  bool in_flight = true;

  bool Mint(bool has_surface, bool client_ready, bool has_frontend) {
    if (phase != Phase::kDraining) {
      return false;
    }
    if (!has_surface || !client_ready || !has_frontend) {
      phase = Phase::kCancelling;
      return false;
    }
    phase = Phase::kMinting;
    return true;
  }
};

struct TerminalHostCloseFenceModel {
  uint64_t active_attempt = 4;

  bool AcceptReplay(uint64_t replay_attempt) const {
    return replay_attempt == active_attempt;
  }

  bool HandleClose(uint64_t close_attempt, bool exited) {
    if (close_attempt != active_attempt) {
      return false;
    }
    ++active_attempt;
    return !exited;
  }

  uint64_t IntentionalDetach() { return active_attempt++; }
};

// Dependency-free model of the renderer service's cold-eviction contract.
// Prepare is ordered behind older semantics; a direct renderer becomes ready
// only after the exact socket attempt reports that its write queue drained.
struct ColdEvictionDrainModel {
  enum class CloseReason { kStopped, kExited, kFailed };

  uint64_t active_host_attempt = 9;
  uint64_t eviction_id = 0;
  uint64_t eviction_host_attempt = 0;
  std::deque<std::string> semantic_input;
  std::string direct_writes;
  std::string client_pipe;
  bool release_and_stop = false;
  bool disconnect_reported = false;

  void Semantic(std::string input) {
    semantic_input.push_back(std::move(input));
  }

  void Prepare(uint64_t id, bool direct_socket) {
    while (!semantic_input.empty()) {
      direct_writes.append(semantic_input.front()).append(";");
      semantic_input.pop_front();
    }
    eviction_id = id;
    eviction_host_attempt = active_host_attempt;
    if (direct_socket) {
      release_and_stop = true;
      direct_writes.append("release-viewer;");
    } else {
      client_pipe.append("ready-ok:").append(std::to_string(id)).append(";");
      eviction_id = 0;
      eviction_host_attempt = 0;
    }
  }

  void CompatibilityBuffered(std::string input) {
    client_pipe.append("input:").append(input).append(";");
  }

  bool Close(uint64_t attempt, CloseReason reason) {
    if (attempt != active_host_attempt ||
        attempt != eviction_host_attempt || eviction_id == 0) {
      return false;
    }
    ++active_host_attempt;
    const uint64_t completed_id = eviction_id;
    eviction_id = 0;
    eviction_host_attempt = 0;
    const bool success =
        (release_and_stop && reason == CloseReason::kStopped) ||
        reason == CloseReason::kExited;
    if (!success) {
      disconnect_reported = true;
      client_pipe.append("disconnected;");
    }
    client_pipe.append(success ? "ready-ok:" : "ready-failed:")
        .append(std::to_string(completed_id))
        .append(";");
    return true;
  }
};

// Browser-side residency is a two-phase protocol. Eligibility is checked
// before Prepare, but a successful same-pipe Ready is an irrevocable commit:
// the service has already released its socket, so post-marker intent must be
// retained for wake instead of causing Browser to abandon the suspension.
struct BrowserColdResidencyModel {
  enum class Residency { kHot, kEvicting, kCold };

  Residency residency = Residency::kHot;
  bool visible = false;
  bool ui_observer_attached = true;
  bool renderer_stream_attached = true;
  bool direct_attached = true;
  bool exited = false;
  bool exited_title = false;
  bool current_surface = true;
  bool final_surface = false;
  bool durable_surface_requested = false;
  bool direct_retry_timer = false;
  std::string pending_input;
  std::string flushed_input;
  uint16_t desired_columns = 80;
  uint16_t applied_columns = 80;

  void RegisterBornCold() {
    residency = Residency::kCold;
    ui_observer_attached = true;
    renderer_stream_attached = false;
    direct_attached = false;
    durable_surface_requested = true;
  }

  bool Begin() {
    const bool rehydratable = exited ? final_surface : current_surface;
    if (visible || residency != Residency::kHot ||
        !renderer_stream_attached ||
        !rehydratable || direct_retry_timer) {
      return false;
    }
    residency = Residency::kEvicting;
    return true;
  }

  void PostPrepareInput(std::string input, uint16_t columns) {
    pending_input.append(input);
    desired_columns = columns;
  }

  void SetVisible(bool value) {
    visible = value;
    if (visible && residency == Residency::kCold) {
      Wake();
    }
  }

  bool Ready(bool success) {
    if (residency != Residency::kEvicting) {
      return false;
    }
    if (!success) {
      residency = Residency::kHot;
      return false;
    }
    renderer_stream_attached = false;
    direct_attached = false;
    residency = Residency::kCold;
    if (visible) {
      Wake();
    }
    return true;
  }

  void ObserveExit() {
    exited = true;
    current_surface = false;
    final_surface = true;
    exited_title = ui_observer_attached;
  }

  void Wake() {
    if (!visible || residency != Residency::kCold) {
      return;
    }
    residency = Residency::kHot;
    renderer_stream_attached = true;
    direct_attached = true;
    flushed_input.append(pending_input);
    pending_input.clear();
    applied_columns = desired_columns;
  }
};

struct DirectInputRejectionModel {
  bool attach_response_pending = true;
  bool compatibility_established = false;
  std::string browser_queue;
  std::string pty;

  void RejectCurrentBatch(std::string bytes) { browser_queue.append(bytes); }

  void FlushIfCompatible() {
    if (!attach_response_pending && compatibility_established) {
      pty.append(browser_queue);
      browser_queue.clear();
    }
  }

  void ObserveDisconnectAndFallback() {
    attach_response_pending = false;
    compatibility_established = true;
    FlushIfCompatible();
  }
};

struct EffectiveVisibilityModel {
  bool self_visible = true;
  bool ancestor_visible = true;
  bool has_visible_bounds = true;

  bool RendererVisible() const {
    return self_visible && ancestor_visible && has_visible_bounds;
  }
};

struct InputMethodModel {
  std::string preedit;
  std::string committed_text;
  int physical_keys = 0;

  void NormalKey(std::string interpreted, std::string event_text) {
    if (interpreted == event_text) {
      ++physical_keys;
    } else {
      committed_text.append(interpreted);
    }
  }

  void SetPreedit(std::string text) { preedit = std::move(text); }

  void CommitComposition(std::string text) {
    preedit.clear();
    committed_text.append(text);
  }
};

struct FramePresentationModel {
  float current_window_scale = 2;
  float sent_geometry_scale = 2;
  float canonical_frame_scale = 0;
  float layer_contents_scale = 0;

  void ChangeBackingScale(float next) { current_window_scale = next; }

  void ObserveGridForSentGeometry() {
    canonical_frame_scale = sent_geometry_scale;
  }

  void AcceptFrame() {
    // Presentation is paired with the geometry that produced the IOSurface,
    // not whichever display scale AppKit reports when the callback arrives.
    layer_contents_scale = canonical_frame_scale;
  }
};

struct OrderedSurfaceLaneModel {
  enum class Kind { kOutput, kSemantic, kControl };
  struct Operation {
    Kind kind;
    std::string value;
  };

  std::deque<Operation> operations;
  bool bracketed_paste = false;
  bool authoritative = true;
  std::string trace;

  void Output(std::string value) {
    operations.push_back({Kind::kOutput, std::move(value)});
  }
  void Semantic(std::string value) {
    operations.push_back({Kind::kSemantic, std::move(value)});
  }
  void Control(std::string value) {
    operations.push_back({Kind::kControl, std::move(value)});
  }

  void Drain() {
    while (authoritative && !operations.empty()) {
      Operation operation = std::move(operations.front());
      operations.pop_front();
      switch (operation.kind) {
        case Kind::kOutput:
          bracketed_paste = operation.value == "enable-bracketed";
          trace.append(operation.value).append(";");
          break;
        case Kind::kSemantic:
          trace
              .append(bracketed_paste ? "encoded-bracketed:" : "encoded-plain:")
              .append(operation.value)
              .append(";");
          break;
        case Kind::kControl:
          trace.append(operation.value).append(";");
          break;
      }
    }
  }
};

// Deterministic model of the renderer's externally locked output FIFO. The
// production queue carries ordinary bytes, replay metadata, and fences through
// the same scheduling latch; this model records their worker-visible order and
// exact byte reservations without needing Chromium's task environment.
struct BatchedOutputWorkerModel {
  static constexpr size_t kMaxQueuedBytes = 32 * 1024 * 1024;

  enum class Kind { kOutput, kReplay, kFence };
  struct Work {
    Kind kind;
    std::string value;
    size_t byte_count;
  };

  cmux::CmuxTerminalOutputDrainQueue<Work> queue;
  size_t reserved_bytes = 0;
  size_t abandoned_bytes = 0;
  int drain_tasks_posted = 0;
  int ordinary_parser_calls = 0;
  int replay_parser_calls = 0;
  int replay_completions_posted = 0;
  int retirements = 0;
  std::string trace;

  bool Reserve(size_t byte_count) {
    if (byte_count > kMaxQueuedBytes - reserved_bytes) {
      return false;
    }
    reserved_bytes += byte_count;
    return true;
  }

  bool Output(std::string value, size_t byte_count = 0) {
    if (byte_count == 0) {
      byte_count = value.size();
    }
    if (!Reserve(byte_count)) {
      return false;
    }
    Push({Kind::kOutput, std::move(value), byte_count});
    return true;
  }

  bool Replay(std::string value, size_t byte_count = 0) {
    if (byte_count == 0) {
      byte_count = value.size();
    }
    if (!Reserve(byte_count)) {
      return false;
    }
    Push({Kind::kReplay, std::move(value), byte_count});
    return true;
  }

  void Fence(std::string value) {
    Push({Kind::kFence, std::move(value), 0});
  }

  void Push(Work work) {
    if (queue.Push(std::move(work))) {
      ++drain_tasks_posted;
    }
  }

  // Processes one contiguous output batch or one ordering marker. Calling
  // this once more after the last item performs atomic drain retirement.
  bool ProcessNext() {
    if (queue.RetireIfEmpty()) {
      ++retirements;
      return false;
    }

    Work work = queue.TakeFront();
    if (work.kind == Kind::kOutput) {
      std::string batch = std::move(work.value);
      size_t batch_bytes = work.byte_count;
      while (!queue.empty() && queue.front().kind == Kind::kOutput) {
        Work next = queue.TakeFront();
        batch.append(next.value);
        batch_bytes += next.byte_count;
      }
      reserved_bytes -= batch_bytes;
      ++ordinary_parser_calls;
      trace.append("output:").append(batch).append(";");
      return true;
    }

    if (work.kind == Kind::kReplay) {
      reserved_bytes -= work.byte_count;
      ++replay_parser_calls;
      ++replay_completions_posted;
      trace.append("replay:").append(work.value).append(";");
      return true;
    }

    trace.append("fence:").append(work.value).append(";");
    return true;
  }

  void Drain() {
    while (ProcessNext()) {
    }
  }

  size_t Shutdown() {
    std::deque<Work> abandoned = queue.TakeAllForShutdown();
    size_t released = 0;
    for (const auto& work : abandoned) {
      released += work.byte_count;
    }
    abandoned_bytes += released;
    reserved_bytes -= released;
    return abandoned.size();
  }
};

// Models the socket-thread to service-sequence ingress queue. Only adjacent
// Output messages may combine; every state and lifecycle message remains an
// observable FIFO boundary.
struct TerminalHostIngressWork {
  enum class Kind {
    kOutput,
    kSnapshot,
    kReady,
    kColors,
    kPwd,
    kResized,
    kExit,
    kError,
    kClose,
  };

  Kind kind;
  std::string value;

  size_t ByteCount() const { return value.size(); }

  bool IsLifecycle() const {
    return kind == Kind::kExit || kind == Kind::kError ||
           kind == Kind::kClose;
  }

  bool TryCoalesce(TerminalHostIngressWork* newer,
                   size_t max_combined_bytes) {
    if (!newer || kind != Kind::kOutput || newer->kind != Kind::kOutput ||
        value.empty() || newer->value.empty() ||
        newer->value.size() > max_combined_bytes ||
        value.size() > max_combined_bytes - newer->value.size()) {
      return false;
    }
    value.append(newer->value);
    return true;
  }
};

using TerminalHostIngressQueue =
    cmux::CmuxTerminalIngressDrainQueue<TerminalHostIngressWork>;

const char* TerminalHostIngressKindName(TerminalHostIngressWork::Kind kind) {
  using Kind = TerminalHostIngressWork::Kind;
  switch (kind) {
    case Kind::kOutput:
      return "output";
    case Kind::kSnapshot:
      return "snapshot";
    case Kind::kReady:
      return "ready";
    case Kind::kColors:
      return "colors";
    case Kind::kPwd:
      return "pwd";
    case Kind::kResized:
      return "resized";
    case Kind::kExit:
      return "exit";
    case Kind::kError:
      return "error";
    case Kind::kClose:
      return "close";
  }
  return "unknown";
}

struct TerminalHostIngressAttemptModel {
  uint64_t current_attempt = 1;
  TerminalHostIngressQueue* current_queue = nullptr;
  size_t handled = 0;
  size_t abandoned = 0;
  TerminalHostIngressQueue::FinishSliceResult last_finish =
      TerminalHostIngressQueue::FinishSliceResult::kRetired;

  void Install(uint64_t attempt, TerminalHostIngressQueue* queue) {
    current_attempt = attempt;
    current_queue = queue;
  }

  void Drain(uint64_t attempt, TerminalHostIngressQueue* queue) {
    if (attempt != current_attempt || queue != current_queue) {
      abandoned += queue->Shutdown();
      return;
    }
    while (!queue->empty()) {
      queue->TakeFront();
      ++handled;
    }
    last_finish = queue->FinishSlice();
  }
};

struct ReplayGenerationModel {
  uint64_t generation = 0;
  bool authoritative = false;

  uint64_t BeginReplay() {
    authoritative = false;
    return ++generation;
  }
  void ObserveClose() {
    ++generation;
    authoritative = false;
  }
  bool Complete(uint64_t candidate) {
    if (candidate != generation) {
      return false;
    }
    authoritative = true;
    return true;
  }
};

struct DirectAttachWatchdogModel {
  enum class Phase { kQuiescing, kCancelling, kIdle };
  Phase phase = Phase::kQuiescing;
  uint64_t attempt = 1;
  int renderer_restarts = 0;

  void Timeout(uint64_t candidate) {
    if (candidate != attempt || phase == Phase::kIdle) {
      return;
    }
    if (phase != Phase::kCancelling) {
      phase = Phase::kCancelling;
      return;
    }
    ++attempt;
    phase = Phase::kIdle;
    ++renderer_restarts;
  }
};

struct MatchingDirectCancelModel {
  uint64_t cutover_id = 7;
  bool direct = true;
  bool socket_installed = true;

  void Cancel(uint64_t candidate) {
    if (!direct || candidate != cutover_id) {
      return;
    }
    direct = false;
    socket_installed = false;
  }
};

struct DirectExitOrderingModel {
  bool direct = true;
  bool control_exit_pending = false;
  bool exited = false;
  bool output_drained = false;
  bool final_state = false;
  bool authoritative_state = false;

  void ControlExit() {
    if (direct) {
      control_exit_pending = true;
    } else {
      exited = true;
    }
  }
  void OrderedDirectExit() {
    if (control_exit_pending) {
      output_drained = true;
      final_state = true;
      authoritative_state = true;
      control_exit_pending = false;
      exited = true;
    }
  }

  void DetachRendererHost() {
    direct = false;
    authoritative_state = final_state;
  }
};

struct DurableReleaseMergeModel {
  std::optional<uint64_t> durable;
  std::optional<uint64_t> pending;
  bool captured = false;

  std::optional<uint64_t> Requeue() {
    if (captured) {
      return std::nullopt;
    }
    // A pending release is chronologically newer than an acknowledged
    // tombstone for the same button.
    return pending ? pending : durable;
  }
};

struct ExactStateControlModel {
  enum class Kind { kSemantic, kControl, kRestore, kLifecycleBarrier };

  std::deque<Kind> operations;
  bool authoritative = false;
  bool cancelled = false;
  bool final_state = false;
  int encoded_semantics = 0;

  void Drain() {
    while (!operations.empty()) {
      auto next = operations.begin();
      if (*next == Kind::kSemantic && !authoritative) {
        next = std::find_if(operations.begin() + 1, operations.end(),
                            [](Kind candidate) {
                              return candidate == Kind::kControl ||
                                     candidate == Kind::kRestore ||
                                     candidate == Kind::kLifecycleBarrier;
                            });
        if (next == operations.end()) {
          return;
        }
      }
      const Kind operation = *next;
      operations.erase(next);
      switch (operation) {
        case Kind::kSemantic:
          ++encoded_semantics;
          break;
        case Kind::kControl:
          cancelled = true;
          break;
        case Kind::kRestore:
          authoritative = true;
          break;
        case Kind::kLifecycleBarrier:
          final_state = true;
          authoritative = true;
          break;
      }
    }
  }
};

struct MouseReleasePipeModel {
  std::set<uint64_t> in_flight;
  std::string renderer_pipe;

  bool Release(int button, uint64_t token) {
    if (in_flight.find(token) != in_flight.end()) {
      return false;
    }
    in_flight.insert(token);
    renderer_pipe.append("release-")
        .append(std::to_string(button))
        .append(":")
        .append(std::to_string(token))
        .append(";");
    return true;
  }

  void Press(int button) {
    renderer_pipe.append("press-").append(std::to_string(button)).append(";");
  }

  void Acknowledge(uint64_t token) { in_flight.erase(token); }
};

struct DeferredMouseQueueModel {
  static constexpr size_t kNormalLimit = 2;
  static constexpr size_t kReleaseReserve = 1;

  std::deque<std::string> operations;
  bool captured = false;

  bool Push(std::string operation, bool reserved_release = false) {
    const size_t limit =
        kNormalLimit + (reserved_release ? kReleaseReserve : 0);
    if (operations.size() >= limit) {
      return false;
    }
    operations.push_back(std::move(operation));
    return true;
  }

  bool Press(float x, float y) {
    if (!Push("press@" + std::to_string(x) + "," + std::to_string(y))) {
      return false;
    }
    captured = true;
    return true;
  }

  bool Release(float x, float y) {
    if (!captured ||
        !Push("release@" + std::to_string(x) + "," + std::to_string(y),
              true)) {
      return false;
    }
    captured = false;
    return true;
  }
};

struct ColdMouseViewportModel {
  uint32_t displayed_width_px = 240;
  uint32_t displayed_height_px = 100;
  float displayed_scale = 2;
  uint32_t canonical_width_px = 240;
  uint32_t canonical_height_px = 100;
  float canonical_scale = 2;
  float bounds_width = 180;
  float bounds_height = 80;

  void RetireFrame() {
    displayed_width_px = 0;
    displayed_height_px = 0;
    displayed_scale = 0;
  }

  bool Contains(float x, float y) const {
    if (x < 0 || y < 0 || x >= bounds_width || y >= bounds_height) {
      return false;
    }
    if (displayed_width_px == 0 || displayed_height_px == 0 ||
        displayed_scale <= 0) {
      if (canonical_width_px == 0 || canonical_height_px == 0 ||
          canonical_scale <= 0) {
        return true;
      }
      return cmux::TerminalMouseBounds{canonical_width_px,
                                       canonical_height_px, canonical_scale}
          .Contains(x, y);
    }
    return cmux::TerminalMouseBounds{displayed_width_px,
                                     displayed_height_px, displayed_scale}
        .Contains(x, y);
  }
};

struct HiddenAuthoritativeGridModel {
  bool visible = false;
  bool grid_authoritative = true;
  bool transition_in_flight = true;
  bool deferred_semantics = false;
  bool pending_native_mouse_ack = false;

  bool CanColdEvict() const {
    const bool hidden_grid_boundary = !visible && grid_authoritative;
    return grid_authoritative && !deferred_semantics &&
           !pending_native_mouse_ack &&
           (!transition_in_flight || hidden_grid_boundary);
  }
};

// Models the frame ownership split across the utility renderer, Browser, and
// Core Animation. The utility permits one delivered frame at a time and keeps
// only its newest pending completion. Browser acknowledges that delivery on a
// display tick; the acknowledgement is flow control, not IOSurface ownership.
struct DisplayCadenceAdmissionModel {
  struct Frame {
    uint64_t token;
    bool browser_accepts;
  };

  uint64_t renderer_incarnation = 1;
  bool visible = true;
  bool inside_display_tick = false;
  int deliveries_in_current_tick = 0;
  int acknowledgements = 0;
  int hidden_ack_flushes = 0;
  std::optional<Frame> utility_in_flight;
  std::optional<Frame> utility_pending;
  std::optional<uint64_t> pending_ack_incarnation;
  std::optional<uint64_t> displayed_token;
  std::deque<uint64_t> ca_completion_releases;
  std::set<uint64_t> released_tokens;
  std::vector<uint64_t> delivered_tokens;
  std::vector<int> deliveries_per_tick;

  bool IsReleased(uint64_t token) const {
    return released_tokens.find(token) != released_tokens.end();
  }

  void Release(uint64_t token) { released_tokens.insert(token); }

  void ReceiveInBrowser(Frame frame) {
    delivered_tokens.push_back(frame.token);
    if (inside_display_tick) {
      ++deliveries_in_current_tick;
    }
    pending_ack_incarnation = renderer_incarnation;
    if (!frame.browser_accepts) {
      // Invalid geometry or metadata retires the lease immediately, while the
      // delivery ACK remains cadence-gated like an accepted frame.
      Release(frame.token);
      return;
    }
    if (displayed_token) {
      ca_completion_releases.push_back(*displayed_token);
    }
    displayed_token = frame.token;
  }

  void Deliver(Frame frame) {
    utility_in_flight = frame;
    ReceiveInBrowser(frame);
  }

  void CompleteFrame(uint64_t token, bool browser_accepts = true) {
    Frame frame{token, browser_accepts};
    if (!visible) {
      Release(token);
      return;
    }
    if (!utility_in_flight) {
      Deliver(frame);
      return;
    }
    if (utility_pending) {
      Release(utility_pending->token);
    }
    utility_pending = frame;
  }

  void AcknowledgeCurrentDelivery() {
    if (!utility_in_flight) {
      return;
    }
    ++acknowledgements;
    utility_in_flight.reset();
    if (!visible || !utility_pending) {
      return;
    }
    Frame next = *utility_pending;
    utility_pending.reset();
    Deliver(next);
  }

  void DisplayTick(uint64_t callback_incarnation) {
    inside_display_tick = true;
    deliveries_in_current_tick = 0;
    if (pending_ack_incarnation &&
        *pending_ack_incarnation == callback_incarnation &&
        callback_incarnation == renderer_incarnation) {
      pending_ack_incarnation.reset();
      AcknowledgeCurrentDelivery();
    }
    deliveries_per_tick.push_back(deliveries_in_current_tick);
    inside_display_tick = false;
  }

  void SetVisible(bool next) {
    if (visible == next) {
      return;
    }
    visible = next;
    if (visible) {
      return;
    }
    if (utility_pending) {
      Release(utility_pending->token);
      utility_pending.reset();
    }
    if (pending_ack_incarnation) {
      pending_ack_incarnation.reset();
      ++hidden_ack_flushes;
      AcknowledgeCurrentDelivery();
    }
  }

  void ResetIncarnation(uint64_t next_incarnation) {
    renderer_incarnation = next_incarnation;
    utility_in_flight.reset();
    utility_pending.reset();
    pending_ack_incarnation.reset();
  }

  void CompleteCoreAnimationCommit() {
    if (ca_completion_releases.empty()) {
      return;
    }
    Release(ca_completion_releases.front());
    ca_completion_releases.pop_front();
  }
};

}  // namespace

int main() {
  cmux::CmuxTerminalTickCoalescer wakeup_burst;
  int burst_tasks = wakeup_burst.RequestTickTask() ? 1 : 0;
  for (int i = 0; i < 10000; ++i) {
    burst_tasks += wakeup_burst.RequestTickTask() ? 1 : 0;
  }
  Check(burst_tasks == 1,
        "ten thousand wakeups coalesce behind one scheduled Tick task");
  wakeup_burst.BeginTickTask();
  Check(!wakeup_burst.EndTickTask(),
        "the scheduled Tick consumes every wakeup that preceded it");

  cmux::CmuxTerminalTickCoalescer wakeup_during_tick;
  Check(wakeup_during_tick.RequestTickTask(),
        "the first wakeup schedules a Tick task");
  wakeup_during_tick.BeginTickTask();
  int posts_during_tick = 0;
  for (int i = 0; i < 10000; ++i) {
    posts_during_tick += wakeup_during_tick.RequestTickTask() ? 1 : 0;
  }
  Check(posts_during_tick == 0 && wakeup_during_tick.EndTickTask(),
        "wakeups during Tick schedule exactly one successor at handoff");
  wakeup_during_tick.BeginTickTask();
  Check(!wakeup_during_tick.EndTickTask(),
        "the successor consumes the coalesced in-Tick wakeups once");

  cmux::CmuxTerminalTickCoalescer wakeup_after_tick;
  Check(wakeup_after_tick.RequestTickTask(),
        "an idle coalescer initially grants posting ownership");
  wakeup_after_tick.BeginTickTask();
  Check(!wakeup_after_tick.EndTickTask() &&
            wakeup_after_tick.RequestTickTask(),
        "a wakeup after Tick retirement schedules the successor itself");
  wakeup_after_tick.BeginTickTask();
  Check(!wakeup_after_tick.EndTickTask(),
        "the post-retirement successor leaves no stale scheduling latch");

  BatchedOutputWorkerModel output_burst;
  bool output_burst_admitted = true;
  for (int i = 0; i < 10000; ++i) {
    output_burst_admitted =
        output_burst.Output("x") && output_burst_admitted;
  }
  Check(output_burst_admitted && output_burst.drain_tasks_posted == 1 &&
            output_burst.queue.size() == 10000 &&
            output_burst.reserved_bytes == 10000,
        "ten thousand output chunks schedule one worker drain task");
  output_burst.Drain();
  Check(output_burst.ordinary_parser_calls == 1 &&
            output_burst.trace.size() == 10008 &&
            output_burst.reserved_bytes == 0 &&
            output_burst.retirements == 1 &&
            !output_burst.queue.drain_task_scheduled(),
        "one drain batches ten thousand adjacent chunks and retires exactly");

  BatchedOutputWorkerModel output_during_drain;
  output_during_drain.Output("A");
  Check(output_during_drain.ProcessNext(),
        "active drain processes its first output batch");
  output_during_drain.Output("B");
  Check(output_during_drain.drain_tasks_posted == 1 &&
            output_during_drain.ProcessNext() &&
            !output_during_drain.ProcessNext() &&
            output_during_drain.trace == "output:A;output:B;",
        "enqueue before retirement is inherited by the active drain");

  BatchedOutputWorkerModel output_after_retirement;
  output_after_retirement.Output("A");
  Check(output_after_retirement.ProcessNext() &&
            !output_after_retirement.ProcessNext(),
        "empty drain atomically retires its scheduling latch");
  output_after_retirement.Output("B");
  Check(output_after_retirement.drain_tasks_posted == 2 &&
            output_after_retirement.queue.drain_task_scheduled(),
        "enqueue after retirement owns exactly one successor task");
  output_after_retirement.Drain();
  Check(output_after_retirement.trace == "output:A;output:B;" &&
            output_after_retirement.retirements == 2,
        "successor drains post-retirement output exactly once");

  BatchedOutputWorkerModel output_barriers;
  output_barriers.Output("A");
  output_barriers.Output("B");
  output_barriers.Fence("semantic");
  output_barriers.Output("C");
  output_barriers.Output("D");
  output_barriers.Replay("R");
  output_barriers.Output("E");
  output_barriers.Fence("exit");
  output_barriers.Drain();
  Check(output_barriers.trace ==
            "output:AB;fence:semantic;output:CD;replay:R;output:E;fence:exit;",
        "typed FIFO never batches output across semantic, replay, or exit "
        "barriers");
  Check(output_barriers.drain_tasks_posted == 1 &&
            output_barriers.ordinary_parser_calls == 3 &&
            output_barriers.replay_parser_calls == 1 &&
            output_barriers.replay_completions_posted == 1 &&
            output_barriers.reserved_bytes == 0,
        "barriers preserve parser accounting inside one scheduled drain");

  BatchedOutputWorkerModel output_shutdown;
  output_shutdown.Output("queued", 17);
  output_shutdown.Fence("semantic");
  output_shutdown.Replay("replacement", 23);
  output_shutdown.Output("tail", 5);
  Check(output_shutdown.reserved_bytes == 45 &&
            output_shutdown.queue.drain_task_scheduled(),
        "queued output and replay bytes remain reserved until execution");
  Check(output_shutdown.Shutdown() == 4 &&
            output_shutdown.abandoned_bytes == 45 &&
            output_shutdown.reserved_bytes == 0 &&
            output_shutdown.trace.empty() &&
            !output_shutdown.queue.drain_task_scheduled(),
        "shutdown clears typed work without callbacks and releases exact "
        "reservations");

  BatchedOutputWorkerModel output_backpressure;
  Check(output_backpressure.Output(
            "full", BatchedOutputWorkerModel::kMaxQueuedBytes) &&
            !output_backpressure.Output("overflow", 1) &&
            output_backpressure.reserved_bytes ==
                BatchedOutputWorkerModel::kMaxQueuedBytes &&
            output_backpressure.queue.size() == 1,
        "batched scheduling retains the exact 32 MiB admission limit");
  output_backpressure.Shutdown();
  Check(output_backpressure.reserved_bytes == 0,
        "backpressure reservation is fully retired during shutdown");

  using IngressKind = TerminalHostIngressWork::Kind;
  using IngressPushResult = TerminalHostIngressQueue::PushResult;
  using IngressFinishResult = TerminalHostIngressQueue::FinishSliceResult;

  TerminalHostIngressQueue ingress_burst(4096, 32 * 1024 * 1024, 2);
  int ingress_burst_posts = 0;
  bool ingress_burst_admitted = true;
  for (int i = 0; i < 10000; ++i) {
    const IngressPushResult result =
        ingress_burst.Push({IngressKind::kOutput, "x"});
    ingress_burst_posts +=
        result == IngressPushResult::kScheduleDrain ? 1 : 0;
    ingress_burst_admitted =
        result != IngressPushResult::kRejected && ingress_burst_admitted;
  }
  Check(ingress_burst_admitted && ingress_burst_posts == 1 &&
            ingress_burst.size() == 1 &&
            ingress_burst.queued_bytes() == 10000 &&
            ingress_burst.drain_scheduled(),
        "ten thousand socket Output events coalesce behind one service wake");
  TerminalHostIngressWork burst_work = ingress_burst.TakeFront();
  Check(burst_work.kind == IngressKind::kOutput &&
            burst_work.value.size() == 10000 &&
            ingress_burst.queued_bytes() == 0 &&
            ingress_burst.FinishSlice() == IngressFinishResult::kRetired &&
            !ingress_burst.drain_scheduled(),
        "sustained ingress preserves every byte and atomically retires its "
        "wake latch");
  Check(ingress_burst.Push({IngressKind::kOutput, "successor"}) ==
            IngressPushResult::kScheduleDrain,
        "post-retirement ingress owns exactly one successor wake");
  ingress_burst.Shutdown();

  TerminalHostIngressQueue ingress_barriers(32, 1024, 2);
  int ingress_barrier_posts = 0;
  auto PushIngressBarrier = [&](IngressKind kind, std::string value) {
    const IngressPushResult result =
        ingress_barriers.Push({kind, std::move(value)});
    ingress_barrier_posts +=
        result == IngressPushResult::kScheduleDrain ? 1 : 0;
    return result != IngressPushResult::kRejected;
  };
  const bool ingress_barriers_admitted =
      PushIngressBarrier(IngressKind::kOutput, "A") &&
      PushIngressBarrier(IngressKind::kSnapshot, "S") &&
      PushIngressBarrier(IngressKind::kOutput, "B") &&
      PushIngressBarrier(IngressKind::kOutput, "C") &&
      PushIngressBarrier(IngressKind::kReady, "R") &&
      PushIngressBarrier(IngressKind::kColors, "C") &&
      PushIngressBarrier(IngressKind::kOutput, "D") &&
      PushIngressBarrier(IngressKind::kPwd, "P") &&
      PushIngressBarrier(IngressKind::kResized, "Z") &&
      PushIngressBarrier(IngressKind::kOutput, "E") &&
      PushIngressBarrier(IngressKind::kExit, "X") &&
      PushIngressBarrier(IngressKind::kError, "!") &&
      PushIngressBarrier(IngressKind::kClose, "Q");
  std::string ingress_barrier_trace;
  while (!ingress_barriers.empty()) {
    TerminalHostIngressWork work = ingress_barriers.TakeFront();
    ingress_barrier_trace.append(TerminalHostIngressKindName(work.kind))
        .append(":")
        .append(work.value)
        .append(";");
  }
  Check(ingress_barriers_admitted && ingress_barrier_posts == 1 &&
            ingress_barriers.FinishSlice() ==
                IngressFinishResult::kRetired,
        "state and lifecycle ingress joins the active service wake");
  Check(ingress_barrier_trace ==
            "output:A;snapshot:S;output:BC;ready:R;colors:C;output:D;"
            "pwd:P;resized:Z;output:E;exit:X;error:!;close:Q;",
        "Output combines only across adjacent payloads and never crosses "
        "Snapshot, Ready, Colors, Pwd, Resized, Exit, Error, or Close");

  TerminalHostIngressQueue ingress_yielding(64, 1024, 2);
  int ingress_yield_initial_posts = 0;
  for (int i = 0; i < 10; ++i) {
    const IngressPushResult result = ingress_yielding.Push(
        {IngressKind::kPwd, std::to_string(i)});
    ingress_yield_initial_posts +=
        result == IngressPushResult::kScheduleDrain ? 1 : 0;
  }
  size_t ingress_slice_count = 0;
  size_t ingress_reposts = 0;
  size_t ingress_max_slice = 0;
  std::vector<std::string> ingress_yield_trace;
  IngressPushResult joined_active_slice = IngressPushResult::kRejected;
  while (true) {
    size_t processed = 0;
    while (processed < 3 && !ingress_yielding.empty()) {
      ingress_yield_trace.push_back(ingress_yielding.TakeFront().value);
      ++processed;
    }
    ++ingress_slice_count;
    ingress_max_slice = std::max(ingress_max_slice, processed);
    const IngressFinishResult finish = ingress_yielding.FinishSlice();
    if (ingress_slice_count == 1) {
      joined_active_slice =
          ingress_yielding.Push({IngressKind::kPwd, "tail"});
    }
    if (finish != IngressFinishResult::kRepost) {
      break;
    }
    ++ingress_reposts;
  }
  Check(ingress_yield_initial_posts == 1 &&
            joined_active_slice == IngressPushResult::kAccepted &&
            ingress_slice_count == 4 && ingress_reposts == 3 &&
            ingress_max_slice == 3 && !ingress_yielding.drain_scheduled(),
        "bounded service slices repost while producers inherit the same wake "
        "latch");
  Check(ingress_yield_trace ==
            std::vector<std::string>({"0", "1", "2", "3", "4", "5",
                                      "6", "7", "8", "9", "tail"}),
        "yielding retains exact FIFO order for a producer active between "
        "service slices");

  TerminalHostIngressQueue ingress_byte_bound(4, 8, 2);
  Check(ingress_byte_bound.Push({IngressKind::kOutput, "1234"}) ==
            IngressPushResult::kScheduleDrain &&
            ingress_byte_bound.Push({IngressKind::kOutput, "5678"}) ==
                IngressPushResult::kAccepted &&
            ingress_byte_bound.Push({IngressKind::kOutput, "9"}) ==
                IngressPushResult::kRejected &&
            ingress_byte_bound.size() == 1 &&
            ingress_byte_bound.queued_bytes() == 8 &&
            ingress_byte_bound.overflowed(),
        "adjacent Output coalescing remains bounded by the exact byte cap");
  Check(ingress_byte_bound.TakeFront().value == "12345678" &&
            ingress_byte_bound.FinishSlice() ==
                IngressFinishResult::kOverflow &&
            ingress_byte_bound.shutdown() &&
            ingress_byte_bound.Push({IngressKind::kClose, std::string()}) ==
                IngressPushResult::kRejected,
        "overflow drains its admitted prefix then permanently rejects the "
        "socket attempt");

  TerminalHostIngressQueue ingress_lifecycle_reserve(1, 64, 2);
  Check(ingress_lifecycle_reserve.Push({IngressKind::kPwd, "ordinary"}) ==
            IngressPushResult::kScheduleDrain &&
            ingress_lifecycle_reserve.Push(
                {IngressKind::kExit, std::string()}) ==
                IngressPushResult::kAccepted &&
            ingress_lifecycle_reserve.Push(
                {IngressKind::kClose, std::string()}) ==
                IngressPushResult::kAccepted &&
            ingress_lifecycle_reserve.Push(
                {IngressKind::kError, std::string()}) ==
                IngressPushResult::kRejected &&
            ingress_lifecycle_reserve.size() == 3,
        "Exit and Close retain two bounded lifecycle slots behind a full "
        "ordinary queue");
  std::vector<IngressKind> ingress_lifecycle_order;
  while (!ingress_lifecycle_reserve.empty()) {
    ingress_lifecycle_order.push_back(
        ingress_lifecycle_reserve.TakeFront().kind);
  }
  Check(ingress_lifecycle_order ==
            std::vector<IngressKind>({IngressKind::kPwd, IngressKind::kExit,
                                      IngressKind::kClose}) &&
            ingress_lifecycle_reserve.FinishSlice() ==
                IngressFinishResult::kOverflow,
        "reserved lifecycle markers remain FIFO before bounded overflow "
        "teardown");

  TerminalHostIngressQueue ingress_shutdown(8, 128, 2);
  ingress_shutdown.Push({IngressKind::kSnapshot, "snapshot"});
  ingress_shutdown.Push({IngressKind::kPwd, "pwd"});
  Check(ingress_shutdown.Shutdown() == 2 && ingress_shutdown.shutdown() &&
            ingress_shutdown.empty() &&
            ingress_shutdown.queued_bytes() == 0 &&
            !ingress_shutdown.drain_scheduled() &&
            ingress_shutdown.Push({IngressKind::kClose, std::string()}) ==
                IngressPushResult::kRejected,
        "shutdown abandons exact queued work and fences every later producer");

  TerminalHostIngressQueue old_ingress_attempt(8, 128, 2);
  TerminalHostIngressQueue replacement_ingress_attempt(8, 128, 2);
  old_ingress_attempt.Push({IngressKind::kSnapshot, "old-snapshot"});
  old_ingress_attempt.Push({IngressKind::kPwd, "old-pwd"});
  replacement_ingress_attempt.Push({IngressKind::kSnapshot, "new-snapshot"});
  TerminalHostIngressAttemptModel ingress_attempt;
  ingress_attempt.Install(1, &old_ingress_attempt);
  ingress_attempt.Install(2, &replacement_ingress_attempt);
  ingress_attempt.Drain(1, &old_ingress_attempt);
  ingress_attempt.Drain(2, &replacement_ingress_attempt);
  Check(ingress_attempt.abandoned == 2 && ingress_attempt.handled == 1 &&
            old_ingress_attempt.shutdown() &&
            ingress_attempt.last_finish == IngressFinishResult::kRetired,
        "attempt replacement abandons stale ingress without leaking it into "
        "the current socket");

  TerminalHostIngressQueue same_attempt_stale_ingress(8, 128, 2);
  TerminalHostIngressQueue same_attempt_current_ingress(8, 128, 2);
  same_attempt_stale_ingress.Push({IngressKind::kPwd, "stale"});
  same_attempt_current_ingress.Push({IngressKind::kPwd, "current"});
  ingress_attempt.Install(3, &same_attempt_current_ingress);
  ingress_attempt.Drain(3, &same_attempt_stale_ingress);
  ingress_attempt.Drain(3, &same_attempt_current_ingress);
  Check(ingress_attempt.abandoned == 3 && ingress_attempt.handled == 2 &&
            same_attempt_stale_ingress.shutdown(),
        "shared-queue identity fences stale work even if an attempt value is "
        "reused");

  PublicationModel model;

  Check(!model.CanAcceptFrame(1, 1600, 1000),
        "initial blank renderer frame is gated before snapshot");

  // AppKit observes its final bounds immediately before a canonical resize
  // response arrives. That resize was never sent, so receipt of the replay
  // must preserve it rather than treating it as causally older.
  model.ObservePhysicalResize();
  model.BeginCanonicalReplay();
  const uint64_t canonical_epoch = model.transition_epoch;
  Check(model.physical_resize_pending,
        "unsent physical resize survives canonical transition start");

  // Grid mutation and replay parsing may each provoke a draw. Both retain the
  // preceding frame context and must remain unpublishable.
  Check(!model.CanAcceptFrame(model.renderer_frame_context, 1500, 900),
        "grid-mutation frame cannot publish old VT at new size");
  model.ParseCanonicalPayload("REPLAY");
  Check(!model.PublishCanonicalGrid(canonical_epoch, 1500, 900),
        "publication waits for colors paired with replay");
  Check(model.transition_in_flight,
        "failed publication leaves the transition gate closed");

  model.ParseCanonicalPayload("REPLAY+OSC-COLORS");
  Check(model.PublishCanonicalGrid(canonical_epoch, 1500, 900),
        "complete replay publishes canonical grid dimensions");
  Check(model.transition_in_flight && model.physical_resize_pending,
        "slow canonical frame keeps the next physical resize gated");
  Check(model.CanAcceptFrame(canonical_epoch, 1500, 900),
        "announced exact-grid frame is eligible for presentation");
  Check(!model.PresentFrame(canonical_epoch - 1, 1500, 900).has_value() &&
            model.transition_in_flight && model.physical_resize_pending,
        "stale frame cannot open the gate or consume the pending resize");
  Check(model.PresentFrame(canonical_epoch, 1500, 900) ==
            std::optional<bool>(true),
        "accepted canonical frame releases the latest pending resize");
  Check(!model.transition_in_flight && !model.physical_resize_pending,
        "frame presentation, not grid announcement, opens the transition");

  // A delayed completion from an older replay cannot reopen publication or
  // replace the dimensions selected by a newer generation.
  model.BeginCanonicalReplay();
  const uint64_t newer_epoch = model.transition_epoch;
  model.ParseCanonicalPayload("REPLAY+OSC-COLORS");
  Check(!model.PublishCanonicalGrid(canonical_epoch, 1400, 800),
        "stale replay completion cannot publish");
  Check(!model.CanAcceptFrame(canonical_epoch, 1500, 900),
        "old generation remains fenced during newer transition");
  Check(model.PublishCanonicalGrid(newer_epoch, 1400, 800),
        "newest complete replay announces its canonical grid");
  Check(model.transition_in_flight,
        "newest transition remains closed until its frame is accepted");
  Check(
      model.PresentFrame(newer_epoch, 1400, 800) == std::optional<bool>(false),
      "newest exact frame publishes without inventing a pending resize");

  DirectSnapshotMetadataModel snapshot_metadata;
  snapshot_metadata.Apply("/worktree");
  Check(snapshot_metadata.client_pipe ==
            std::vector<std::string>({"pwd:/worktree", "replay"}),
        "late direct attach publishes snapshot PWD before replay completion");
  DirectSnapshotMetadataModel empty_snapshot_metadata;
  empty_snapshot_metadata.Apply(std::nullopt);
  Check(empty_snapshot_metadata.client_pipe ==
            std::vector<std::string>({"pwd:", "replay"}),
        "snapshot without a PWD explicitly clears preceding renderer state");

  SparseColorModel colors;
  const int tasks_before_combined = colors.renderer_output_tasks;
  colors.SubmitOutputAndColors("OUTPUT+", 0x345678);
  Check(colors.renderer_output_tasks == tasks_before_combined + 1,
        "output and following colors use one renderer task boundary");
  Check(colors.last_payload.find("OUTPUT+RESET-PALETTE") == 0,
        "color reset/apply delta follows output in the same payload");
  Check(colors.exposed_background == 0x345678,
        "present sparse background overrides configured Ghostty color");
  colors.SubmitOutputAndColors("OUTPUT+", std::nullopt);
  Check(colors.exposed_background == SparseColorModel::kConfiguredBackground,
        "absent sparse background restores configured Ghostty color");

  DirectResizeModel resize;
  const auto desired = resize.RequestPhysicalSize(1510, 1010);
  Check(desired == std::optional<std::pair<uint16_t, uint16_t>>({150, 50}),
        "physical pixels invert canonical cell metrics and fixed padding");
  Check(resize.speculative_reflows == 0 && resize.viewer_size_requests == 1,
        "direct physical resize emits ViewerSize without speculative reflow");
  Check(!resize.RequestPhysicalSize(1515, 1015),
        "same-grid physical resize does not publish a redundant ViewerSize");
  Check(resize.sent_width_px == 1515 && resize.sent_height_px == 1015 &&
            resize.viewer_size_requests == 1 && resize.speculative_reflows == 0,
        "same-grid resize records physical bounds without speculative SetSize");
  resize.ApplyHostResize(desired->first, desired->second);
  Check(resize.authoritative_reflows == 1 && resize.columns == 150 &&
            resize.rows == 50,
        "only host Resized applies the authoritative reflow");

  UnchangedResizeAckModel unchanged_resize;
  unchanged_resize.Begin(5, true);
  Check(unchanged_resize.UnchangedAck(100, 40, 5, false) &&
            !unchanged_resize.transition_in_flight &&
            !unchanged_resize.physical_resize_pending &&
            unchanged_resize.physical_resize_reissues == 1 &&
            unchanged_resize.expected_width_px == 1010 &&
            unchanged_resize.expected_height_px == 810,
        "same-scale unchanged ACK retains exact pixels and opens transition");
  Check(unchanged_resize.grid_mutations == 0 &&
            unchanged_resize.refreshes == 0 &&
            unchanged_resize.replacement_frames == 0,
        "unchanged ACK avoids grid mutation, refresh, and replacement frame");
  Check(unchanged_resize.AcceptOrdinaryFrame(5, 1010, 810),
        "unchanged ACK advances context for the next ordinary output frame");
  unchanged_resize.Begin(6);
  Check(!unchanged_resize.UnchangedAck(99, 40, 6, false) &&
            unchanged_resize.transition_in_flight,
        "mismatched unchanged ACK cannot open the browser transition");
  Check(unchanged_resize.UnchangedAck(100, 40, 6, true) &&
            unchanged_resize.transition_in_flight &&
            unchanged_resize.grid_mutations == 1 &&
            unchanged_resize.refreshes == 1 &&
            unchanged_resize.replacement_frames == 1,
        "scale-changing ACK keeps the exact replacement-frame boundary");

  DebouncedViewerReleaseModel viewer_debounce;
  viewer_debounce.SetVisible(false);
  const uint64_t cancelled_generation = viewer_debounce.timer_generation;
  viewer_debounce.SetVisible(true);
  viewer_debounce.FireTimer(cancelled_generation);
  Check(viewer_debounce.releases == 0 && viewer_debounce.requeues == 0,
        "rapid hide-show cancels ReleaseViewer before it reaches the host");
  viewer_debounce.SetVisible(false);
  viewer_debounce.FireTimer(viewer_debounce.timer_generation);
  Check(viewer_debounce.releases == 1 && viewer_debounce.viewer_released,
        "sustained occlusion releases exactly one viewer lease");
  viewer_debounce.SetVisible(true);
  Check(viewer_debounce.requeues == 1 && !viewer_debounce.viewer_released &&
            viewer_debounce.restore_pending,
        "show after a committed release fences exactly one ViewerSize restore");
  viewer_debounce.IntermediateFrame();
  Check(viewer_debounce.suppressed_intermediate_frames == 1 &&
            viewer_debounce.restored_frames == 0,
        "release-induced intermediate frame stays hidden during re-add");
  viewer_debounce.CompleteRestore();
  Check(!viewer_debounce.restore_pending &&
            viewer_debounce.restored_frames == 1,
        "matching re-add completion publishes one final restored frame");

  MouseButtonAcknowledgementModel mouse_ack;
  mouse_ack.RightPressAck(true, true);
  Check(mouse_ack.native_right_clicks == 0,
        "consumed right press remains owned by Ghostty");
  mouse_ack.RightPressAck(true, false);
  Check(mouse_ack.native_right_clicks == 1,
        "accepted but unconsumed right press falls through to AppKit");
  mouse_ack.RightPressAck(false, false);
  Check(mouse_ack.native_right_clicks == 2,
        "rejected right press also falls through to AppKit");
  mouse_ack.ReleaseAck(true, false);
  Check(!mouse_ack.durable_release_pending &&
            mouse_ack.native_right_ups == 1,
        "unconsumed release preserves durability and AppKit mouse-up");
  const std::string native_fallback_order = mouse_ack.TeardownFallback();
  Check(!mouse_ack.deferred_right_press && mouse_ack.durable_release_pending,
        "teardown fallback removes stale terminal press and journals release");
  Check(native_fallback_order == "DUDU",
        "teardown fallback preserves chronological native click order");

  VisibilityModel visibility;
  visibility.SetVisible(false);
  visibility.RequestViewerSize();
  visibility.Output();
  visibility.Output();
  Check(visibility.delivered_frames == 0 && visibility.state_updates == 2 &&
            !visibility.viewer_reserved && visibility.viewer_releases == 1,
        "hidden renderer parses state, suppresses frames, and releases sizing");
  visibility.SetVisible(true);
  Check(visibility.refreshes == 1 && visibility.delivered_frames == 1 &&
            visibility.viewer_reserved && visibility.viewer_requeues == 1,
        "showing a dirty renderer restores latest ViewerSize and one frame");
  visibility.SetVisible(true);
  Check(visibility.refreshes == 1,
        "duplicate visibility notification does not refresh again");

  EffectiveVisibilityModel effective_visibility;
  Check(effective_visibility.RendererVisible(),
        "drawn terminal with visible bounds renders");
  effective_visibility.ancestor_visible = false;
  Check(!effective_visibility.RendererVisible(),
        "hidden workspace ancestor occludes an otherwise visible terminal");
  effective_visibility.ancestor_visible = true;
  Check(effective_visibility.RendererVisible(),
        "showing the workspace ancestor reactivates its terminal");

  InputMethodModel input_method;
  input_method.NormalKey("a", "a");
  Check(input_method.physical_keys == 1 && input_method.committed_text.empty(),
        "ordinary interpreted text preserves Ghostty's physical-key encoder");
  input_method.SetPreedit("k");
  input_method.CommitComposition("\xE3\x81\x8B");
  Check(
      input_method.preedit.empty() &&
          input_method.committed_text == "\xE3\x81\x8B" &&
          input_method.physical_keys == 1,
      "IME commit clears preedit and uses text input without a duplicate key");

  DirectCutoverModel cutover;
  cutover.CompatibilityOutput("before-");
  cutover.InstallDirectSocket();
  cutover.CompatibilityOutput("duplicate-output-");
  cutover.CompatibilityReplay("duplicate-replay");
  cutover.DirectSnapshot("canonical-snapshot");
  cutover.CompatibilityOutput("late-duplicate");
  Check(
      cutover.direct_snapshot_applied && cutover.parsed == "canonical-snapshot",
      "socket installation fences legacy output/replay before direct Snapshot");

  DirectInputHandoffModel input_handoff;
  input_handoff.Input("A");
  input_handoff.Begin();
  Check(input_handoff.client_pipe == "input;ready;",
        "same-pipe Ready fences every older compatibility input callback");
  input_handoff.Input("B");  // Mouse press during the transition.
  input_handoff.Input("C");  // Matching mouse release stays behind it.
  Check(!input_handoff.Attach(),
        "renderer input cannot attach before the host-side barrier");
  input_handoff.DrainAndMint();
  Check(input_handoff.Attach(),
        "drain plus capability response permits direct attach");
  input_handoff.Input("D");
  Check(
      input_handoff.pty == "ABCD",
      "compatibility, buffered press/release, and live direct input are FIFO");

  DirectInputHandoffModel cancelled_handoff;
  cancelled_handoff.Begin();
  cancelled_handoff.Input("release");
  cancelled_handoff.Cancel();
  Check(cancelled_handoff.browser_queue == "release" &&
            cancelled_handoff.client_pipe == "ready;input;cancelled;",
        "cancel republishes buffered mouse release before its marker");

  AttachReplyFenceModel attach_fence;
  attach_fence.Begin(7);
  attach_fence.RequestCancel();
  Check(!attach_fence.AttachReply(3, 7, true) && !attach_fence.direct,
        "attach reply cannot resurrect direct state after cancellation starts");
  attach_fence.Cancelled(7);
  Check(attach_fence.transition_in_flight && !attach_fence.grid_authoritative &&
            attach_fence.compatibility_snapshot_requests == 1,
        "cancelled attach retires its epoch and requests a fresh compatibility "
        "snapshot");
  attach_fence.Begin(8);
  Check(!attach_fence.AttachReply(3, 7, true) && attach_fence.cutover_id == 8 &&
            attach_fence.attach_in_flight,
        "stale attach reply cannot clear a newer cutover");
  Check(!attach_fence.AttachReply(2, 8, true) && attach_fence.cutover_id == 8,
        "old renderer incarnation cannot complete the current cutover");
  Check(attach_fence.AttachReply(3, 8, true) && attach_fence.direct,
        "matching renderer and cutover may enter direct mode");
  attach_fence.HostDisconnected();
  Check(!attach_fence.direct && attach_fence.transition_in_flight &&
            !attach_fence.grid_authoritative &&
            attach_fence.compatibility_snapshot_requests == 2,
        "direct disconnect gates new mouse input and requests a fresh "
        "compatibility snapshot");

  DirectAttachPrerequisiteModel attach_prerequisite;
  Check(!attach_prerequisite.Mint(false, true, true) &&
            attach_prerequisite.in_flight &&
            attach_prerequisite.phase ==
                DirectAttachPrerequisiteModel::Phase::kCancelling,
        "vanished runtime surface cancels input buffering instead of stalling "
        "the attach");

  TerminalHostCloseFenceModel host_close;
  Check(host_close.AcceptReplay(4) && host_close.HandleClose(4, false) &&
            !host_close.AcceptReplay(4),
        "active stopped socket notifies Browser and fences its posted replay");
  const uint64_t detached_attempt = host_close.IntentionalDetach();
  Check(!host_close.HandleClose(detached_attempt, false),
        "intentional detach invalidates its asynchronous close callback");
  Check(!host_close.HandleClose(host_close.active_attempt, true),
        "terminal exit remains a lifecycle event rather than a reconnect");

  ColdEvictionDrainModel cold_eviction;
  cold_eviction.Semantic("press");
  cold_eviction.Semantic("release");
  cold_eviction.Prepare(41, true);
  Check(cold_eviction.semantic_input.empty() &&
            cold_eviction.release_and_stop &&
            cold_eviction.direct_writes ==
                "press;release;release-viewer;" &&
            cold_eviction.client_pipe.empty(),
        "cold eviction orders behind semantic input and waits for socket "
        "drain before Ready");
  Check(!cold_eviction.Close(8,
                             ColdEvictionDrainModel::CloseReason::kStopped) &&
            cold_eviction.client_pipe.empty(),
        "stale socket close cannot complete a newer cold eviction");
  Check(cold_eviction.Close(9,
                            ColdEvictionDrainModel::CloseReason::kStopped) &&
            !cold_eviction.disconnect_reported &&
            cold_eviction.client_pipe == "ready-ok:41;",
        "matching orderly close completes eviction without a disconnect "
        "error");

  ColdEvictionDrainModel compatibility_eviction;
  compatibility_eviction.CompatibilityBuffered("quiesced");
  compatibility_eviction.Prepare(42, false);
  Check(compatibility_eviction.client_pipe ==
            "input:quiesced;ready-ok:42;",
        "compatibility input precedes cold-eviction Ready on one client pipe");

  ColdEvictionDrainModel failed_eviction;
  failed_eviction.Prepare(43, true);
  Check(failed_eviction.Close(
            9, ColdEvictionDrainModel::CloseReason::kFailed) &&
            failed_eviction.disconnect_reported &&
            failed_eviction.client_pipe ==
                "disconnected;ready-failed:43;",
        "failed direct drain preserves normal recovery before failed Ready");

  ColdEvictionDrainModel unprepared_stop;
  unprepared_stop.eviction_id = 44;
  unprepared_stop.eviction_host_attempt = 9;
  Check(unprepared_stop.Close(
            9, ColdEvictionDrainModel::CloseReason::kStopped) &&
            unprepared_stop.disconnect_reported &&
            unprepared_stop.client_pipe ==
                "disconnected;ready-failed:44;",
        "ordinary socket stop cannot impersonate a completed eviction drain");

  BrowserColdResidencyModel born_cold;
  born_cold.RegisterBornCold();
  born_cold.ObserveExit();
  Check(born_cold.durable_surface_requested &&
            born_cold.ui_observer_attached &&
            !born_cold.renderer_stream_attached && born_cold.exited_title,
        "never-visible terminal launches durably and observes lifecycle without "
        "allocating a renderer stream");

  HiddenAuthoritativeGridModel hidden_grid;
  Check(hidden_grid.CanColdEvict(),
        "invisible authoritative grid can evict without an intentionally "
        "suppressed frame");
  hidden_grid.pending_native_mouse_ack = true;
  Check(!hidden_grid.CanColdEvict(),
        "pending cross-pipe mouse acknowledgement blocks cold eviction");

  BrowserColdResidencyModel browser_cold;
  Check(browser_cold.Begin(),
        "hidden renderer with a current surface may begin cold eviction");

  BrowserColdResidencyModel retrying_direct;
  retrying_direct.direct_retry_timer = true;
  Check(!retrying_direct.Begin(),
        "pending direct-attach retry blocks the Prepare-to-Ready race window");
  browser_cold.PostPrepareInput("release", 91);
  Check(browser_cold.Ready(true) &&
            browser_cold.residency ==
                BrowserColdResidencyModel::Residency::kCold &&
            browser_cold.ui_observer_attached &&
            !browser_cold.renderer_stream_attached &&
            !browser_cold.direct_attached &&
            browser_cold.pending_input == "release" &&
            browser_cold.desired_columns == 91,
        "successful Ready commits suspension while preserving post-Prepare "
        "input and resize intent");
  browser_cold.SetVisible(true);
  Check(browser_cold.residency ==
                BrowserColdResidencyModel::Residency::kHot &&
            browser_cold.ui_observer_attached &&
            browser_cold.renderer_stream_attached &&
            browser_cold.direct_attached &&
            browser_cold.pending_input.empty() &&
            browser_cold.flushed_input == "release" &&
            browser_cold.applied_columns == 91,
        "visible wake reattaches and flushes intent retained across eviction");

  BrowserColdResidencyModel transient_visibility;
  Check(transient_visibility.Begin(),
        "second hidden renderer begins cold eviction");
  transient_visibility.SetVisible(true);
  transient_visibility.SetVisible(false);
  Check(transient_visibility.Ready(true) &&
            transient_visibility.residency ==
                BrowserColdResidencyModel::Residency::kCold,
        "visible-then-hidden during eviction does not leave a sticky wake");
  transient_visibility.ObserveExit();
  Check(transient_visibility.exited_title &&
            transient_visibility.ui_observer_attached &&
            !transient_visibility.renderer_stream_attached,
        "cold renderer keeps its durable UI observer for immediate exit state");

  BrowserColdResidencyModel exited_cold;
  exited_cold.exited = true;
  exited_cold.current_surface = false;
  exited_cold.final_surface = true;
  Check(exited_cold.Begin() && exited_cold.Ready(true) &&
            exited_cold.residency ==
                BrowserColdResidencyModel::Residency::kCold,
        "same-generation final surface makes an exited renderer evictable");
  exited_cold.SetVisible(true);
  Check(exited_cold.residency ==
            BrowserColdResidencyModel::Residency::kHot,
        "cold exited terminal reattaches its final surface when shown");

  BrowserColdResidencyModel unleased_exit;
  unleased_exit.exited = true;
  unleased_exit.current_surface = false;
  Check(!unleased_exit.Begin(),
        "exited renderer without a same-generation final lease stays hot");

  DirectInputRejectionModel rejected_input;
  rejected_input.RejectCurrentBatch("mouse-release");
  rejected_input.FlushIfCompatible();
  Check(rejected_input.attach_response_pending && rejected_input.pty.empty() &&
            rejected_input.browser_queue == "mouse-release",
        "rejected direct batch waits while the separate attach response is "
        "pending");
  rejected_input.ObserveDisconnectAndFallback();
  Check(rejected_input.compatibility_established &&
            rejected_input.browser_queue.empty() &&
            rejected_input.pty == "mouse-release",
        "fallback flushes the rejected mouse release through compatibility");

  OrderedSurfaceLaneModel ordered_lane;
  ordered_lane.Output("enable-bracketed");
  ordered_lane.Semantic("paste");
  ordered_lane.Control("cancelled");
  ordered_lane.Output("disable-bracketed");
  ordered_lane.Drain();
  Check(ordered_lane.trace ==
            "enable-bracketed;encoded-bracketed:paste;cancelled;disable-"
            "bracketed;",
        "output-worker fence preserves output, semantic input, cutover marker, "
        "and later output order");
  OrderedSurfaceLaneModel replay_blocked_lane;
  replay_blocked_lane.authoritative = false;
  replay_blocked_lane.Semantic("release");
  replay_blocked_lane.Drain();
  Check(replay_blocked_lane.trace.empty(),
        "semantic release waits while the authoritative mirror is invalid");
  replay_blocked_lane.authoritative = true;
  replay_blocked_lane.Drain();
  Check(replay_blocked_lane.trace == "encoded-plain:release;",
        "matching authoritative replay releases held semantics exactly once");

  ReplayGenerationModel replay_generation;
  const uint64_t stale_replay = replay_generation.BeginReplay();
  replay_generation.ObserveClose();
  Check(!replay_generation.Complete(stale_replay) &&
            !replay_generation.authoritative,
        "close generation rejects a stale compatibility or direct replay");
  const uint64_t fresh_replay = replay_generation.BeginReplay();
  Check(replay_generation.Complete(fresh_replay) &&
            replay_generation.authoritative,
        "only the fresh fallback replay reopens authoritative publication");

  MatchingDirectCancelModel matching_cancel;
  matching_cancel.Cancel(6);
  Check(matching_cancel.direct && matching_cancel.socket_installed,
        "stale cancel cannot detach a committed direct socket");
  matching_cancel.Cancel(7);
  Check(!matching_cancel.direct && !matching_cancel.socket_installed,
        "matching cancel after commit falls back instead of leaving "
        "split-brain direct mode");

  DirectAttachWatchdogModel watchdog;
  watchdog.Timeout(1);
  Check(watchdog.phase == DirectAttachWatchdogModel::Phase::kCancelling &&
            watchdog.renderer_restarts == 0,
        "first direct-attach watchdog timeout requests ordered cancellation");
  watchdog.Timeout(1);
  Check(
      watchdog.phase == DirectAttachWatchdogModel::Phase::kIdle &&
          watchdog.renderer_restarts == 1,
      "second timeout fences callbacks and restarts only the wedged renderer");
  watchdog.Timeout(1);
  Check(watchdog.renderer_restarts == 1,
        "late watchdog callbacks cannot restart a replacement renderer");

  DirectExitOrderingModel exit_ordering;
  exit_ordering.ControlExit();
  Check(exit_ordering.control_exit_pending && !exit_ordering.exited,
        "control-plane exit waits for the direct stream's ordered final frame");
  exit_ordering.OrderedDirectExit();
  Check(exit_ordering.exited && exit_ordering.output_drained &&
            exit_ordering.final_state && !exit_ordering.control_exit_pending,
        "ordered direct Exit completes lifecycle after final renderer events");
  exit_ordering.DetachRendererHost();
  Check(
      exit_ordering.authoritative_state,
      "Browser detach preserves the final renderer state established by Exit");

  ExactStateControlModel exact_state_control;
  exact_state_control.operations = {ExactStateControlModel::Kind::kSemantic,
                                    ExactStateControlModel::Kind::kControl};
  exact_state_control.Drain();
  Check(
      exact_state_control.cancelled &&
          exact_state_control.encoded_semantics == 0 &&
          !exact_state_control.authoritative,
      "direct cancel bypasses a blocked semantic without opening stale state");
  exact_state_control.operations.push_back(
      ExactStateControlModel::Kind::kRestore);
  exact_state_control.Drain();
  Check(exact_state_control.authoritative &&
            exact_state_control.encoded_semantics == 1,
        "fresh compatibility state releases the held semantic exactly once");

  DurableReleaseMergeModel release_merge{1, 2, false};
  Check(
      release_merge.Requeue() == 2,
      "newer pending release supersedes an older durable tombstone coordinate");
  release_merge.captured = true;
  Check(!release_merge.Requeue(),
        "renderer recovery never replays a stale release while its button is "
        "physically captured");

  MouseReleasePipeModel release_pipe;
  Check(release_pipe.Release(1, 11) && release_pipe.Release(0, 12),
        "unrelated releases enter the renderer pipe without waiting for acks");
  release_pipe.Press(0);
  Check(release_pipe.Release(0, 13),
        "next same-button release enters the pipe while its predecessor is in "
        "flight");
  release_pipe.Press(0);
  Check(release_pipe.renderer_pipe ==
            "release-1:11;release-0:12;press-0;release-0:13;press-0;",
        "every same-button release precedes its subsequent press regardless of "
        "acknowledgment order");
  Check(!release_pipe.Release(0, 13),
        "an already-sent release token is never duplicated before its ack");
  release_pipe.Acknowledge(12);
  Check(release_pipe.in_flight.find(13) != release_pipe.in_flight.end(),
        "acknowledgment retires only its exact release token");

  DeferredMouseQueueModel deferred_mouse;
  Check(deferred_mouse.Push("older-key") && deferred_mouse.Press(12, 34) &&
            deferred_mouse.Release(15, 37),
        "atomic click position and its reserved release survive a full normal "
        "semantic queue");
  Check(deferred_mouse.operations[0] == "older-key" &&
            deferred_mouse.operations[1] == "press@12.000000,34.000000" &&
            deferred_mouse.operations[2] == "release@15.000000,37.000000",
        "reserved mouse release remains FIFO behind all older semantics");
  Check(!deferred_mouse.Press(20, 40) && !deferred_mouse.captured,
        "rejected over-capacity press cannot begin local mouse capture");

  cmux::TerminalMouseBounds displayed_mouse_bounds{240, 100, 2};
  Check(displayed_mouse_bounds.Contains(119.5f, 49.5f),
        "mouse points inside the displayed IOSurface route to Ghostty");
  Check(!displayed_mouse_bounds.Contains(120, 20) &&
            !displayed_mouse_bounds.Contains(20, 50) &&
            !displayed_mouse_bounds.Contains(-1, 20),
        "right, bottom, and negative resize margins stay outside Ghostty");

  // The renderer may acknowledge a larger canonical grid before its frame is
  // swapped into the layer. Hit testing deliberately retains these old bounds.
  cmux::TerminalMouseBounds pending_mouse_bounds{400, 200, 2};
  Check(pending_mouse_bounds.Contains(150, 20) &&
            !displayed_mouse_bounds.Contains(150, 20),
        "pending grid geometry cannot make an undisplayed margin clickable");

  ColdMouseViewportModel cold_mouse_viewport;
  Check(!cold_mouse_viewport.Contains(150, 60),
        "crash placeholder keeps native displayed hit bounds even after its "
        "renderer frame token is retired");
  cold_mouse_viewport.RetireFrame();
  Check(cold_mouse_viewport.Contains(100, 40) &&
            !cold_mouse_viewport.Contains(150, 60),
        "cold wake admits canonical cells but rejects smallest-viewer margins");
  Check(!cold_mouse_viewport.Contains(180, 20) &&
            !cold_mouse_viewport.Contains(20, 80),
        "cold wake still rejects input outside the current AppKit viewport");
  ColdMouseViewportModel never_rendered_mouse_viewport;
  never_rendered_mouse_viewport.canonical_width_px = 0;
  never_rendered_mouse_viewport.canonical_height_px = 0;
  never_rendered_mouse_viewport.canonical_scale = 0;
  never_rendered_mouse_viewport.RetireFrame();
  Check(never_rendered_mouse_viewport.Contains(150, 60),
        "never-rendered cold terminal provisionally uses current bounds");

  cmux::TerminalMouseCapture mouse_capture;
  Check(!mouse_capture.Begin(0, false) && !mouse_capture.HasCapture() &&
            !mouse_capture.ShouldForwardDrag(),
        "outside press followed by an inside drag stays suppressed");
  Check(mouse_capture.Begin(0, true) && mouse_capture.HasCapture(0) &&
            !mouse_capture.HasCapture(1) &&
            mouse_capture.PreserveActualMotion(false),
        "inside press preserves actual drag motion beyond the displayed frame");
  Check(mouse_capture.End(0) && !mouse_capture.HasCapture() &&
            !mouse_capture.End(0),
        "inside-started drag releases exactly once even when outside");

  cmux::TerminalMouseCapture crash_capture;
  const uint64_t press_renderer = 4;
  uint64_t current_renderer = press_renderer;
  bool pending_release = false;
  Check(crash_capture.Begin(0, true),
        "renderer-crash model begins with an owned press");
  current_renderer = 5;
  if (crash_capture.End(0) && current_renderer != press_renderer) {
    pending_release = true;
  }
  Check(pending_release && !crash_capture.HasCapture(),
        "release after renderer replacement is retained instead of dropped");
  bool authoritative_grid = false;
  Check(pending_release && !authoritative_grid,
        "retained release waits until the replacement mirror is authoritative");
  Check(!authoritative_grid,
        "new press and wheel input stay gated before authoritative replay");
  authoritative_grid = true;
  if (authoritative_grid) {
    pending_release = false;
  }
  Check(!pending_release,
        "replacement authoritative grid replays the retained release once");

  FramePresentationModel presentation;
  presentation.ChangeBackingScale(1);
  presentation.ObserveGridForSentGeometry();
  presentation.AcceptFrame();
  Check(presentation.layer_contents_scale == 2 &&
            presentation.layer_contents_scale !=
                presentation.current_window_scale,
        "in-flight and retained IOSurfaces keep their sent geometry scale "
        "across display moves");

  DisplayCadenceAdmissionModel cadence;
  cadence.CompleteFrame(1);
  Check(cadence.delivered_tokens == std::vector<uint64_t>{1} &&
            cadence.displayed_token == 1 && cadence.acknowledgements == 0,
        "first renderer frame presents immediately without waiting for a "
        "display tick");

  for (uint64_t token = 2; token <= 10000; ++token) {
    cadence.CompleteFrame(token);
  }
  Check(cadence.delivered_tokens == std::vector<uint64_t>{1} &&
            cadence.utility_pending && cadence.utility_pending->token == 10000,
        "ten thousand completions retain one in-flight delivery and only the "
        "latest pending frame");
  Check(cadence.released_tokens.size() == 9998 &&
            cadence.IsReleased(2) && cadence.IsReleased(9999) &&
            !cadence.IsReleased(1) && !cadence.IsReleased(10000),
        "utility coalescing immediately retires every superseded pending "
        "frame");

  cadence.DisplayTick(1);
  Check(cadence.delivered_tokens == std::vector<uint64_t>({1, 10000}) &&
            cadence.displayed_token == 10000 &&
            cadence.deliveries_per_tick.back() == 1,
        "one display tick admits exactly the latest utility frame");
  Check(cadence.acknowledgements == 1 && !cadence.IsReleased(1) &&
            cadence.ca_completion_releases == std::deque<uint64_t>{1},
        "delivery ACK does not release the replaced IOSurface before Core "
        "Animation completes");
  cadence.CompleteCoreAnimationCommit();
  Check(cadence.IsReleased(1) && !cadence.IsReleased(10000),
        "Core Animation completion retires the replaced token but retains the "
        "displayed token");
  cadence.DisplayTick(1);
  Check(cadence.acknowledgements == 2 && !cadence.IsReleased(10000) &&
            cadence.deliveries_per_tick.back() == 0,
        "ACK of a displayed frame changes flow control without changing its "
        "lease ownership");
  Check(std::all_of(cadence.deliveries_per_tick.begin(),
                    cadence.deliveries_per_tick.end(),
                    [](int deliveries) { return deliveries <= 1; }),
        "no display tick delivers more than one frame");

  cadence.CompleteFrame(10001, false);
  cadence.CompleteFrame(10002);
  Check(cadence.IsReleased(10001) && cadence.acknowledgements == 2 &&
            cadence.pending_ack_incarnation == 1 &&
            cadence.delivered_tokens.back() == 10001 &&
            cadence.utility_pending && cadence.utility_pending->token == 10002,
        "rejected frame releases immediately but keeps its ACK tick-gated");
  cadence.DisplayTick(1);
  Check(cadence.acknowledgements == 3 &&
            cadence.delivered_tokens.back() == 10002 &&
            cadence.deliveries_per_tick.back() == 1,
        "rejected delivery opens one latest-pending admission on the next "
        "display tick");

  cadence.CompleteFrame(10003);
  const int acknowledgements_before_hide = cadence.acknowledgements;
  cadence.SetVisible(false);
  Check(cadence.hidden_ack_flushes == 1 &&
            cadence.acknowledgements == acknowledgements_before_hide + 1 &&
            !cadence.pending_ack_incarnation && !cadence.utility_in_flight &&
            !cadence.utility_pending && cadence.IsReleased(10003),
        "hiding flushes the tick-gated ACK and retires an undelivered pending "
        "frame");

  cadence.SetVisible(true);
  cadence.CompleteFrame(10004);
  const uint64_t stale_ack_incarnation =
      *cadence.pending_ack_incarnation;
  cadence.ResetIncarnation(2);
  cadence.CompleteFrame(20001);
  const int acknowledgements_before_stale_tick = cadence.acknowledgements;
  cadence.DisplayTick(stale_ack_incarnation);
  Check(cadence.acknowledgements == acknowledgements_before_stale_tick &&
            cadence.utility_in_flight &&
            cadence.utility_in_flight->token == 20001 &&
            cadence.pending_ack_incarnation == 2,
        "stale display callback cannot ACK a replacement renderer "
        "incarnation");
  cadence.DisplayTick(2);
  Check(cadence.acknowledgements == acknowledgements_before_stale_tick + 1 &&
            !cadence.utility_in_flight && !cadence.pending_ack_incarnation,
        "matching display callback ACKs the replacement incarnation once");
  Check(std::all_of(cadence.deliveries_per_tick.begin(),
                    cadence.deliveries_per_tick.end(),
                    [](int deliveries) { return deliveries <= 1; }),
        "rejection, visibility, and incarnation changes preserve one delivery "
        "per tick");

  std::cout << "cmux-renderer-ordering: " << checks << " checks, " << failures
            << " failures\n";
  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
