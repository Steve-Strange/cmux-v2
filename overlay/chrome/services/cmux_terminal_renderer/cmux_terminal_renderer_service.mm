// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "chrome/services/cmux_terminal_renderer/cmux_terminal_renderer_service.h"

#import <CoreGraphics/CoreGraphics.h>
#import <IOSurface/IOSurfaceRef.h>
#include <dispatch/dispatch.h>

#include <algorithm>
#include <utility>

#include "base/apple/scoped_mach_port.h"
#include "base/auto_reset.h"
#include "base/containers/span.h"
#include "base/files/scoped_file.h"
#include "base/functional/bind.h"
#include "base/location.h"
#include "base/logging.h"
#include "base/task/thread_pool.h"
#include "base/time/time.h"
#include "chrome/services/cmux_terminal_renderer/public/cpp/cmux_ghostty_resources.h"

namespace cmux {

namespace {

constexpr size_t kMaxQueuedOutputBytes = 32 * 1024 * 1024;
constexpr size_t kMaxPendingSurfaceOperations = 4096;
constexpr size_t kMaxPendingSurfaceOperationBytes = 32 * 1024 * 1024;
constexpr size_t kMaxThemeConfigBytes = 64 * 1024;
constexpr size_t kMaxTerminalHostIngressOperations = 4096;
constexpr size_t kMaxTerminalHostIngressBytes = kMaxQueuedOutputBytes;
constexpr size_t kTerminalHostIngressLifecycleReserve = 2;
constexpr size_t kTerminalHostIngressSliceOperations = 64;
constexpr size_t kTerminalHostIngressSliceBytes = 1024 * 1024;
constexpr base::TimeDelta kTerminalHostIngressSliceTime =
    base::Milliseconds(2);
constexpr base::TimeDelta kTerminalHostViewerReleaseDebounce =
    base::Milliseconds(100);
static_assert(GHOSTTY_PLATFORM_METAL_EXTERNAL_LEASED == 5);
static_assert(GHOSTTY_SURFACE_IO_MANUAL_MIRROR == 2);
static_assert(sizeof(ghostty_surface_config_s) == 168);

CFPropertyListRef DisplayP3ColorSpacePropertyList() {
  static dispatch_once_t once;
  static CFPropertyListRef property_list = nullptr;
  dispatch_once(&once, ^{
    CGColorSpaceRef color_space =
        CGColorSpaceCreateWithName(kCGColorSpaceDisplayP3);
    if (!color_space) {
      return;
    }
    // Process-lifetime metadata shared by the renderer's small IOSurface swap
    // chain. IOSurface retains the value each time it is attached.
    property_list = CGColorSpaceCopyPropertyList(color_space);
    CGColorSpaceRelease(color_space);
  });
  return property_list;
}

}  // namespace

CmuxTerminalRendererService::PendingFrame::PendingFrame(
    mojo::PlatformHandle handle,
    uint64_t delivery_sequence,
    uint64_t frame_token,
    uint64_t geometry_epoch,
    uint32_t width_px,
    uint32_t height_px,
    mojom::TerminalFrameColorSpace color_space)
    : handle(std::move(handle)),
      delivery_sequence(delivery_sequence),
      frame_token(frame_token),
      geometry_epoch(geometry_epoch),
      width_px(width_px),
      height_px(height_px),
      color_space(color_space) {}
CmuxTerminalRendererService::PendingFrame::PendingFrame(PendingFrame&&) =
    default;
CmuxTerminalRendererService::PendingFrame&
CmuxTerminalRendererService::PendingFrame::operator=(PendingFrame&&) = default;
CmuxTerminalRendererService::PendingFrame::~PendingFrame() = default;

CmuxTerminalRendererService::PendingSurfaceOperation::PendingSurfaceOperation(
    PendingSurfaceOperationKind kind,
    base::OnceClosure action,
    size_t byte_count)
    : kind(kind), action(std::move(action)), byte_count(byte_count) {}
CmuxTerminalRendererService::PendingSurfaceOperation::PendingSurfaceOperation(
    PendingSurfaceOperation&&) = default;
CmuxTerminalRendererService::PendingSurfaceOperation&
CmuxTerminalRendererService::PendingSurfaceOperation::operator=(
    PendingSurfaceOperation&&) = default;
CmuxTerminalRendererService::PendingSurfaceOperation::
    ~PendingSurfaceOperation() = default;

CmuxTerminalRendererService::PendingOutputOperation::PendingOutputOperation(
    std::vector<uint8_t> output)
    : kind(PendingOutputOperationKind::kOutput), bytes(std::move(output)) {}

CmuxTerminalRendererService::PendingOutputOperation::PendingOutputOperation(
    std::vector<uint8_t> replay,
    uint16_t columns,
    uint16_t rows,
    uint32_t width_px,
    uint32_t height_px,
    uint64_t geometry_epoch,
    std::optional<TerminalHostColors> colors,
    uint64_t terminal_host_attempt,
    uint64_t replay_generation)
    : kind(PendingOutputOperationKind::kAuthoritativeReplay),
      bytes(std::move(replay)),
      columns(columns),
      rows(rows),
      width_px(width_px),
      height_px(height_px),
      geometry_epoch(geometry_epoch),
      colors(std::move(colors)),
      terminal_host_attempt(terminal_host_attempt),
      replay_generation(replay_generation) {}

CmuxTerminalRendererService::PendingOutputOperation::PendingOutputOperation()
    : kind(PendingOutputOperationKind::kSurfaceOperationFence) {}
CmuxTerminalRendererService::PendingOutputOperation::PendingOutputOperation(
    PendingOutputOperation&&) = default;
CmuxTerminalRendererService::PendingOutputOperation&
CmuxTerminalRendererService::PendingOutputOperation::operator=(
    PendingOutputOperation&&) = default;
CmuxTerminalRendererService::PendingOutputOperation::~PendingOutputOperation() =
    default;

CmuxTerminalRendererService::PendingTerminalHostIngressOperation::
    PendingTerminalHostIngressOperation(TerminalHostRendererEvent event)
    : kind(PendingTerminalHostIngressOperationKind::kEvent),
      event(std::move(event)) {}

CmuxTerminalRendererService::PendingTerminalHostIngressOperation::
    PendingTerminalHostIngressOperation()
    : kind(PendingTerminalHostIngressOperationKind::kReady) {}

CmuxTerminalRendererService::PendingTerminalHostIngressOperation::
    PendingTerminalHostIngressOperation(TerminalHostSocketClose close)
    : kind(PendingTerminalHostIngressOperationKind::kClosed),
      close(std::move(close)) {}

CmuxTerminalRendererService::PendingTerminalHostIngressOperation::
    PendingTerminalHostIngressOperation(PendingTerminalHostIngressOperation&&) =
        default;
CmuxTerminalRendererService::PendingTerminalHostIngressOperation&
CmuxTerminalRendererService::PendingTerminalHostIngressOperation::operator=(
    PendingTerminalHostIngressOperation&&) = default;
CmuxTerminalRendererService::PendingTerminalHostIngressOperation::
    ~PendingTerminalHostIngressOperation() = default;

size_t CmuxTerminalRendererService::PendingTerminalHostIngressOperation::
    EventByteCount(const TerminalHostRendererEvent& event) {
  size_t size = event.bytes.size() + event.text.size();
  if (event.snapshot) {
    size += event.snapshot->replay.size();
    if (event.snapshot->cwd) {
      size += event.snapshot->cwd->size();
    }
    for (const std::string& argument : event.snapshot->command) {
      size += argument.size();
    }
  }
  if (event.resize) {
    size += event.resize->replay.size();
  }
  if (event.resize_ack) {
    size += sizeof(TerminalHostResizeAck);
  }
  if (event.colors) {
    size += event.colors->palette.size() * sizeof(TerminalHostPaletteEntry) +
            sizeof(TerminalHostColors);
  }
  return size;
}

size_t CmuxTerminalRendererService::PendingTerminalHostIngressOperation::
    ByteCount() const {
  if (kind != PendingTerminalHostIngressOperationKind::kEvent) {
    // Ready and Closed are fixed-size lifecycle markers produced by this
    // process, not peer-controlled payloads. Their reserved item slots keep
    // them deliverable even when ordinary ingress has reached its byte bound.
    return 0;
  }
  return EventByteCount(event);
}

bool CmuxTerminalRendererService::PendingTerminalHostIngressOperation::
    IsLifecycle() const {
  return kind == PendingTerminalHostIngressOperationKind::kClosed ||
         (kind == PendingTerminalHostIngressOperationKind::kEvent &&
          event.kind == TerminalHostMessageKind::kExit);
}

bool CmuxTerminalRendererService::PendingTerminalHostIngressOperation::
    TryCoalesce(PendingTerminalHostIngressOperation* newer,
                size_t max_combined_bytes) {
  if (!newer || kind != PendingTerminalHostIngressOperationKind::kEvent ||
      newer->kind != PendingTerminalHostIngressOperationKind::kEvent ||
      event.kind != TerminalHostMessageKind::kOutput ||
      newer->event.kind != TerminalHostMessageKind::kOutput ||
      !event.text.empty() || !newer->event.text.empty() || event.snapshot ||
      newer->event.snapshot || event.resize || newer->event.resize ||
      event.resize_ack || newer->event.resize_ack || event.colors ||
      newer->event.colors || event.bytes.empty() ||
      newer->event.bytes.empty() ||
      newer->event.bytes.size() > max_combined_bytes ||
      event.bytes.size() > max_combined_bytes - newer->event.bytes.size()) {
    return false;
  }
  event.bytes.insert(event.bytes.end(), newer->event.bytes.begin(),
                     newer->event.bytes.end());
  // The stream validated every original sequence before this ingress stage.
  // Retain the newest boundary for diagnostics after combining its bytes.
  event.sequence = newer->event.sequence;
  return true;
}

CmuxTerminalRendererService::TerminalHostIngress::TerminalHostIngress(
    uint64_t attempt)
    : attempt(attempt),
      queue(kMaxTerminalHostIngressOperations,
            kMaxTerminalHostIngressBytes,
            kTerminalHostIngressLifecycleReserve) {}
CmuxTerminalRendererService::TerminalHostIngress::~TerminalHostIngress() =
    default;

CmuxTerminalRendererService::CmuxTerminalRendererService(
    mojo::PendingReceiver<mojom::CmuxTerminalRenderer> receiver)
    : receiver_(this, std::move(receiver)),
      service_task_runner_(base::SequencedTaskRunner::GetCurrentDefault()) {
  weak_this_ = weak_factory_.GetWeakPtr();
}

CmuxTerminalRendererService::~CmuxTerminalRendererService() {
  shutting_down_.store(true, std::memory_order_release);
  StopTerminalHost(false);
  weak_factory_.InvalidateWeakPtrs();
  {
    base::AutoLock hold(frame_lock_);
    while (active_frame_callbacks_ != 0) {
      frame_callbacks_drained_.Wait();
    }
  }
  if (output_thread_.IsRunning()) {
    output_thread_.Stop();
  }
  ClearOutputQueueAfterWorkerStop();
  ReleaseAllFrames();
  if (surface_) {
    ghostty_surface_free(surface_);
    surface_ = nullptr;
  }
  if (app_) {
    ghostty_app_free(app_);
    app_ = nullptr;
  }
}

void CmuxTerminalRendererService::Initialize(
    mojo::PendingRemote<mojom::CmuxTerminalRendererClient> client,
    uint32_t width_px,
    uint32_t height_px,
    float scale_factor,
    const std::string& theme_name,
    uint64_t geometry_epoch,
    InitializeCallback callback) {
  if (surface_) {
    std::move(callback).Run(false, "renderer already initialized", 0, 0, 0);
    return;
  }
  client_.Bind(std::move(client));
  if (!EnsureGhosttyResourcesDirectory()) {
    std::move(callback).Run(false, "pinned Ghostty resources are unavailable",
                            0, 0, 0);
    return;
  }
  if (ghostty_init(0, nullptr) != GHOSTTY_SUCCESS) {
    std::move(callback).Run(false, "ghostty_init failed", 0, 0, 0);
    return;
  }
  if (!output_thread_.Start()) {
    std::move(callback).Run(false, "output worker failed to start", 0, 0, 0);
    return;
  }

  ghostty_config_t config = ghostty_config_new();
  if (!config) {
    std::move(callback).Run(false, "ghostty_config_new failed", 0, 0, 0);
    return;
  }
  ghostty_config_load_default_files(config);
  // Match standalone Ghostty's file semantics without asking Ghostty to parse
  // Chromium's unrelated command-line switches.
  ghostty_config_load_recursive_files(config);
  if (theme_name.find_first_of("\r\n") != std::string::npos) {
    ghostty_config_free(config);
    std::move(callback).Run(false, "invalid Ghostty theme name", 0, 0, 0);
    return;
  }
  if (!theme_name.empty()) {
    const std::string override = "theme = " + theme_name;
    ghostty_config_load_string(config, override.data(), override.size(),
                              "cmux://settings/appearance");
  }
  ghostty_config_finalize(config);
  ghostty_config_color_s background = {};
  const bool has_background = ghostty_config_get(
      config, &background, "background", sizeof("background") - 1);

  ghostty_runtime_config_s runtime = {};
  runtime.userdata = this;
  runtime.supports_selection_clipboard = false;
  runtime.wakeup_cb = &CmuxTerminalRendererService::WakeupThunk;
  runtime.action_cb = &CmuxTerminalRendererService::ActionThunk;
  runtime.read_clipboard_cb = &CmuxTerminalRendererService::ReadClipboardThunk;
  runtime.confirm_read_clipboard_cb =
      &CmuxTerminalRendererService::ConfirmReadClipboardThunk;
  runtime.write_clipboard_cb =
      &CmuxTerminalRendererService::WriteClipboardThunk;
  runtime.close_surface_cb = &CmuxTerminalRendererService::CloseSurfaceThunk;
  app_ = ghostty_app_new(&runtime, config);
  ghostty_config_free(config);
  if (!app_) {
    std::move(callback).Run(false, "ghostty_app_new failed", 0, 0, 0);
    return;
  }

  ghostty_surface_config_s surface_config = ghostty_surface_config_new();
  surface_config.userdata = this;
  surface_config.platform_tag = GHOSTTY_PLATFORM_METAL_EXTERNAL_LEASED;
  surface_config.platform.metal_external_leased.userdata = this;
  surface_config.platform.metal_external_leased.present =
      &CmuxTerminalRendererService::PresentLeasedThunk;
  surface_config.scale_factor = std::max(scale_factor, 1.0f);
  surface_config.context = GHOSTTY_SURFACE_CONTEXT_WINDOW;
  surface_config.io_mode = GHOSTTY_SURFACE_IO_MANUAL_MIRROR;
  surface_config.io_write_cb = &CmuxTerminalRendererService::IoWriteThunk;
  surface_config.io_write_userdata = this;
  surface_ = ghostty_surface_new(app_, &surface_config);
  if (!surface_) {
    std::move(callback).Run(false, "ghostty_surface_new failed", 0, 0, 0);
    return;
  }
  surface_scale_factor_ = surface_config.scale_factor;
  published_surface_scale_factor_ = surface_config.scale_factor;

  ghostty_app_set_focus(app_, true);
  SetSize(width_px, height_px, scale_factor, geometry_epoch);
  std::move(callback).Run(
      true, std::string(), has_background ? background.r : 0,
      has_background ? background.g : 0, has_background ? background.b : 0);
}

void CmuxTerminalRendererService::UpdateTheme(
    const std::string& theme_config) {
  if (theme_config.empty() || theme_config.size() > kMaxThemeConfigBytes) {
    ReportError("renderer received an invalid theme update");
    return;
  }
  // Theme defaults and PTY output both mutate the surface's color state.
  // Reuse the finite output barrier so a change never races the output worker.
  if (!QueueSurfaceOperation(
          PendingSurfaceOperationKind::kOutputBarrier,
          base::BindOnce(&CmuxTerminalRendererService::UpdateThemeNow,
                         weak_this_, theme_config),
          theme_config.size())) {
    ReportError("renderer surface-operation queue exceeded its limit");
  }
}

void CmuxTerminalRendererService::UpdateThemeNow(std::string theme_config) {
  if (!surface_) {
    return;
  }
  ghostty_config_t config = ghostty_config_new();
  if (!config) {
    ReportError("ghostty_config_new failed during theme update");
    return;
  }
  ghostty_config_load_string(config, theme_config.data(), theme_config.size(),
                            "cmux://settings/appearance");
  ghostty_config_finalize(config);
  if (ghostty_config_diagnostics_count(config) != 0) {
    ghostty_config_free(config);
    ReportError("Ghostty rejected the resolved theme update");
    return;
  }
  ghostty_surface_update_theme_config(surface_, config);
  ghostty_config_free(config);
  ghostty_surface_refresh(surface_);
}

void CmuxTerminalRendererService::SetSize(uint32_t width_px,
                                          uint32_t height_px,
                                          float scale_factor,
                                          uint64_t geometry_epoch) {
  if (!QueueSurfaceOperation(
          PendingSurfaceOperationKind::kService,
          base::BindOnce(&CmuxTerminalRendererService::SetSizeNow, weak_this_,
                         width_px, height_px, scale_factor, geometry_epoch),
          sizeof(width_px) + sizeof(height_px) + sizeof(scale_factor) +
              sizeof(geometry_epoch))) {
    ReportError("renderer surface-operation queue exceeded its limit");
  }
}

void CmuxTerminalRendererService::SetSizeNow(uint32_t width_px,
                                             uint32_t height_px,
                                             float scale_factor,
                                             uint64_t geometry_epoch) {
  if (!surface_) {
    return;
  }
  if (terminal_host_socket_ && terminal_host_stream_ready_) {
    // A direct host arbitrates the logical grid. Speculatively calling
    // ghostty_surface_set_size here would publish a reflow before cmux-tui's
    // canonical Resized+Colors replay and cause the visible double-resize.
    return;
  }
  width_px = std::max(width_px, 1u);
  height_px = std::max(height_px, 1u);
  latest_geometry_epoch_ = geometry_epoch;
  const double scale = std::max<double>(scale_factor, 1.0);
  ghostty_surface_set_content_scale(surface_, scale, scale);
  surface_scale_factor_ = static_cast<float>(scale);
  ghostty_surface_set_size(surface_, width_px, height_px);
  // Publish the epoch only after both geometry mutations have been queued.
  // A draw already in progress retains the preceding epoch and is rejected by
  // the browser. Refresh guarantees a frame that captures the new epoch.
  ghostty_surface_set_external_frame_context(surface_, geometry_epoch);
  ghostty_surface_refresh(surface_);
  published_surface_scale_factor_ = surface_scale_factor_;
  NotifyGridSize(width_px, height_px, geometry_epoch);
}

void CmuxTerminalRendererService::ResetAndReplayAtGrid(
    uint16_t columns,
    uint16_t rows,
    const std::vector<uint8_t>& replay,
    uint64_t geometry_epoch) {
  if (!QueueSurfaceOperation(
          PendingSurfaceOperationKind::kStateRestore,
          base::BindOnce(&CmuxTerminalRendererService::ResetAndReplayAtGridNow,
                         weak_this_, columns, rows, replay, geometry_epoch),
          replay.size())) {
    ReportError("renderer surface-operation queue exceeded its limit");
  }
}

void CmuxTerminalRendererService::ResetAndReplayAtGridNow(
    uint16_t columns,
    uint16_t rows,
    std::vector<uint8_t> replay,
    uint64_t geometry_epoch) {
  if (!surface_ || terminal_host_socket_) {
    // AttachTerminalHost establishes the cutover boundary before its worker
    // starts. Any compatibility replay racing after that point may contain
    // bytes already represented by the direct Snapshot and must be ignored.
    return;
  }
  terminal_host_final_state_ = false;
  columns = std::max<uint16_t>(columns, 1);
  rows = std::max<uint16_t>(rows, 1);
  latest_geometry_epoch_ = geometry_epoch;
  ghostty_surface_size_s resolved = {};
  if (!ghostty_surface_set_grid_size(surface_, columns, rows, &resolved)) {
    ReportError("Ghostty rejected the authoritative terminal grid");
    return;
  }

  // Keep the preceding external-frame context while the grid mutation and VT
  // reset are in flight. Any opportunistic draw is therefore rejected by the
  // browser's new geometry epoch. The output worker advances publication only
  // after it has parsed the complete replay.
  std::vector<uint8_t> replacement = {0x1b, 'c'};
  replacement.insert(replacement.end(), replay.begin(), replay.end());
  if (!EnqueueAuthoritativeReplay(
          std::move(replacement), std::max<uint16_t>(resolved.columns, 1),
          std::max<uint16_t>(resolved.rows, 1), resolved.width_px,
          resolved.height_px, geometry_epoch, std::nullopt, 0)) {
    ReportError("authoritative terminal replay could not be queued");
  }
}

void CmuxTerminalRendererService::BeginTerminalHostInputCutover(
    uint64_t cutover_id) {
  if (!QueueSurfaceOperation(
          PendingSurfaceOperationKind::kService,
          base::BindOnce(
              &CmuxTerminalRendererService::BeginTerminalHostInputCutoverNow,
              weak_this_, cutover_id),
          sizeof(cutover_id))) {
    if (client_.is_connected()) {
      client_->OnTerminalHostInputCutoverReady(
          cutover_id, false,
          "renderer surface-operation queue exceeded its limit");
    }
    ReportError("renderer surface-operation queue exceeded its limit");
  }
}

void CmuxTerminalRendererService::BeginTerminalHostInputCutoverNow(
    uint64_t cutover_id) {
  std::string error;
  if (terminal_host_socket_) {
    error = "terminal-host socket is already installed";
  } else if (!terminal_host_input_cutover_.Begin(cutover_id)) {
    error = "terminal-host input cutover is already active";
  }
  if (client_.is_connected()) {
    // OnInput and this marker share one ordered client pipe. Once the browser
    // observes Ready, every older compatibility byte is already in its finite
    // queue and every newer libghostty write is buffered in this process.
    client_->OnTerminalHostInputCutoverReady(cutover_id, error.empty(), error);
  }
}

void CmuxTerminalRendererService::AttachTerminalHost(
    mojo::PlatformHandle socket,
    const std::vector<uint8_t>& terminal_id,
    const std::vector<uint8_t>& terminal_incarnation,
    uint32_t rights,
    uint32_t protocol_flags,
    uint64_t cutover_id,
    AttachTerminalHostCallback callback) {
  const size_t byte_count = terminal_id.size() + terminal_incarnation.size();
  if (!CanQueueSurfaceOperation(byte_count)) {
    std::move(callback).Run(
        false, "renderer surface-operation queue exceeded its limit");
    ReportError("renderer surface-operation queue exceeded its limit");
    return;
  }
  QueueSurfaceOperation(
      PendingSurfaceOperationKind::kService,
      base::BindOnce(&CmuxTerminalRendererService::AttachTerminalHostNow,
                     weak_this_, std::move(socket), terminal_id,
                     terminal_incarnation, rights, protocol_flags, cutover_id,
                     std::move(callback)),
      byte_count);
}

void CmuxTerminalRendererService::AttachTerminalHostNow(
    mojo::PlatformHandle socket,
    std::vector<uint8_t> terminal_id,
    std::vector<uint8_t> terminal_incarnation,
    uint32_t rights,
    uint32_t protocol_flags,
    uint64_t cutover_id,
    AttachTerminalHostCallback callback) {
  if (!surface_) {
    std::move(callback).Run(false, "renderer is not initialized");
    return;
  }
  if (!socket.is_fd() || !socket.is_valid_fd()) {
    std::move(callback).Run(false,
                            "terminal-host handle is not a valid descriptor");
    return;
  }
  TerminalHostId decoded_id{};
  TerminalHostIncarnation decoded_incarnation{};
  if (terminal_id.size() != decoded_id.size() ||
      terminal_incarnation.size() != decoded_incarnation.size()) {
    std::move(callback).Run(false,
                            "terminal-host identity has an invalid length");
    return;
  }
  std::copy(terminal_id.begin(), terminal_id.end(), decoded_id.begin());
  std::copy(terminal_incarnation.begin(), terminal_incarnation.end(),
            decoded_incarnation.begin());
  const auto decoded_rights = static_cast<TerminalHostCapabilityRights>(rights);
  if (!IsValidTerminalHostUuidV4(decoded_id) ||
      !IsValidTerminalHostUuidV4(decoded_incarnation) ||
      decoded_rights != TerminalHostCapabilityRights::kRenderer ||
      (protocol_flags & ~kTerminalHostFlagViewerSizeAcks) != 0) {
    std::move(callback).Run(false,
                            "terminal-host identity or rights are invalid");
    return;
  }
  if (terminal_host_socket_) {
    if (terminal_host_stream_ready_ && terminal_host_id_ == decoded_id &&
        terminal_host_incarnation_ == decoded_incarnation &&
        terminal_host_input_cutover_.direct() &&
        terminal_host_input_cutover_.cutover_id() == cutover_id) {
      // A reconnect raced an already-live exact stream. Keep the hot socket;
      // destruction of the unused PlatformHandle closes the redundant grant.
      std::move(callback).Run(true, std::string());
      return;
    }
    std::move(callback).Run(false,
                            "terminal-host attachment is already in progress");
    return;
  }
  if (!terminal_host_input_cutover_.CanCommit(cutover_id)) {
    std::move(callback).Run(false,
                            "terminal-host input is not quiesced for attach");
    return;
  }

  base::ScopedFD descriptor = socket.TakeFD();
  if (!descriptor.is_valid()) {
    std::move(callback).Run(false, "terminal-host descriptor transfer failed");
    return;
  }
  if (++authoritative_replay_generation_ == 0) {
    ++authoritative_replay_generation_;
  }
  authoritative_replay_in_flight_ = false;
  authoritative_state_ready_ = false;
  const uint64_t attempt = ++terminal_host_attempt_;
  terminal_host_id_ = decoded_id;
  terminal_host_incarnation_ = decoded_incarnation;
  terminal_host_stream_ready_ = false;
  terminal_host_initial_replay_complete_ = false;
  terminal_host_exit_reported_ = false;
  terminal_host_final_state_ = false;
  terminal_host_replay_in_flight_ = false;
  terminal_host_close_pending_attempt_ = 0;
  terminal_host_close_pending_exited_ = false;
  terminal_host_geometry_epoch_floor_ = latest_geometry_epoch_;
  terminal_host_viewer_size_acks_ =
      (protocol_flags & kTerminalHostFlagViewerSizeAcks) != 0;
  terminal_host_next_resize_request_id_ = 1;
  terminal_host_resize_in_flight_ = false;
  terminal_host_resize_request_id_ = 0;
  terminal_host_resize_epoch_ = 0;
  terminal_host_resize_replay_observed_ = false;
  terminal_host_resize_pending_ = false;
  terminal_host_has_latest_viewer_size_ = false;
  terminal_host_viewer_released_ = false;
  terminal_host_viewer_size_dirty_ = false;
  terminal_host_visibility_restore_pending_ = false;
  terminal_host_viewer_release_timer_.Stop();
  terminal_host_attach_callback_ = std::move(callback);

  const scoped_refptr<base::SequencedTaskRunner> runner = service_task_runner_;
  const base::WeakPtr<CmuxTerminalRendererService> service = weak_this_;
  auto ingress = std::make_shared<TerminalHostIngress>(attempt);
  terminal_host_ingress_ = ingress;
  auto enqueue_ingress = [runner, service, ingress](
                             PendingTerminalHostIngressOperation operation) {
    bool should_post = false;
    {
      base::AutoLock hold(ingress->lock);
      should_post = ingress->queue.Push(std::move(operation)) ==
                    CmuxTerminalIngressDrainQueue<
                        PendingTerminalHostIngressOperation>::PushResult::
                        kScheduleDrain;
    }
    if (should_post) {
      runner->PostTask(
          FROM_HERE,
          base::BindOnce(
              &CmuxTerminalRendererService::DrainTerminalHostIngress, service,
              ingress->attempt, ingress));
    }
  };
  terminal_host_socket_ = std::make_shared<TerminalHostRendererSocket>(
      descriptor.release(), decoded_id, decoded_incarnation,
      [enqueue_ingress](TerminalHostRendererEvent event) mutable {
        enqueue_ingress(
            PendingTerminalHostIngressOperation(std::move(event)));
      },
      [enqueue_ingress]() mutable {
        enqueue_ingress(PendingTerminalHostIngressOperation());
      },
      [enqueue_ingress](TerminalHostSocketClose close) mutable {
        enqueue_ingress(
            PendingTerminalHostIngressOperation(std::move(close)));
      });
  std::shared_ptr<TerminalHostRendererSocket> host = terminal_host_socket_;
  base::ThreadPool::PostTask(
      FROM_HERE,
      {base::MayBlock(), base::TaskPriority::USER_VISIBLE,
       base::TaskShutdownBehavior::CONTINUE_ON_SHUTDOWN},
      base::BindOnce(
          [](std::shared_ptr<TerminalHostRendererSocket> host) { host->Run(); },
          std::move(host)));
}

void CmuxTerminalRendererService::CancelTerminalHostInputCutover(
    uint64_t cutover_id) {
  if (!QueueSurfaceOperation(
          PendingSurfaceOperationKind::kControl,
          base::BindOnce(
              &CmuxTerminalRendererService::CancelTerminalHostInputCutoverNow,
              weak_this_, cutover_id),
          sizeof(cutover_id))) {
    ReportError("renderer surface-operation queue exceeded its limit");
  }
}

void CmuxTerminalRendererService::CancelTerminalHostInputCutoverNow(
    uint64_t cutover_id) {
  CancelTerminalHostInputCutoverInternal(
      cutover_id, "terminal-host input cutover cancelled",
      /*stop_terminal_host=*/true);
}

void CmuxTerminalRendererService::DetachTerminalHost() {
  if (!QueueSurfaceOperation(
          PendingSurfaceOperationKind::kControl,
          base::BindOnce(&CmuxTerminalRendererService::DetachTerminalHostNow,
                         weak_this_),
          0)) {
    ReportError("renderer surface-operation queue exceeded its limit");
  }
}

void CmuxTerminalRendererService::DetachTerminalHostNow() {
  if (cold_eviction_id_ != 0) {
    // ReleaseAndStop already owns the exact direct socket. A lifecycle event
    // can make Browser send its ordinary Detach while that orderly close is in
    // flight; treating the redundant request as a second teardown would fence
    // the matching close and strand the cold-eviction completion.
    return;
  }
  if (terminal_host_input_cutover_.buffering()) {
    CancelTerminalHostInputCutoverInternal(
        terminal_host_input_cutover_.cutover_id(),
        "terminal-host renderer detached", /*stop_terminal_host=*/true);
    return;
  }
  terminal_host_input_cutover_.FallBackToCompatibility();
  StopTerminalHost(true);
}

void CmuxTerminalRendererService::PrepareForColdEviction(
    uint64_t eviction_id) {
  if (eviction_id == 0) {
    if (client_.is_connected()) {
      client_->OnColdEvictionReady(eviction_id, false,
                                   "cold eviction id must be non-zero");
    }
    return;
  }
  if (cold_eviction_id_ != 0) {
    if (cold_eviction_id_ != eviction_id && client_.is_connected()) {
      client_->OnColdEvictionReady(
          eviction_id, false, "another cold eviction is already in progress");
    }
    return;
  }
  // Reserve the exact attempt before enqueueing the ordered operation. A
  // later ordinary Detach caused by a lifecycle event is then recognized as
  // redundant even while older semantic input is still draining.
  cold_eviction_id_ = eviction_id;
  cold_eviction_terminal_host_attempt_ = terminal_host_attempt_;
  cold_eviction_release_started_ = false;
  // kService deliberately cannot bypass older semantic input waiting for an
  // authoritative replay. Once this operation runs, every older Ghostty input
  // encoder has either published OnInput on the ordered client pipe or queued
  // its bytes on the direct socket that ReleaseAndStop flushes below.
  if (!QueueSurfaceOperation(
          PendingSurfaceOperationKind::kService,
          base::BindOnce(
              &CmuxTerminalRendererService::PrepareForColdEvictionNow,
              weak_this_, eviction_id),
          sizeof(eviction_id))) {
    CompleteColdEviction(
        eviction_id, false,
        "renderer surface-operation queue exceeded its limit");
    ReportError("renderer surface-operation queue exceeded its limit");
  }
}

void CmuxTerminalRendererService::PrepareForColdEvictionNow(
    uint64_t eviction_id) {
  if (eviction_id != cold_eviction_id_ ||
      cold_eviction_terminal_host_attempt_ != terminal_host_attempt_) {
    return;
  }
  if (terminal_host_socket_) {
    // ReleaseAndStop retains the socket pump until every older queued write
    // plus ReleaseViewer reaches the peer. Its matching close callback is the
    // only successful completion path for a direct renderer.
    terminal_host_viewer_release_timer_.Stop();
    cold_eviction_release_started_ = true;
    terminal_host_socket_->ReleaseAndStop();
    return;
  }

  // A compatibility-to-direct cutover can be quiesced without having received
  // its renderer socket yet. Return that finite prefix on the same client pipe
  // before Ready, exactly like an ordered cutover cancellation.
  if (terminal_host_input_cutover_.buffering()) {
    const uint64_t cutover_id = terminal_host_input_cutover_.cutover_id();
    std::optional<std::vector<uint8_t>> buffered =
        terminal_host_input_cutover_.Cancel(cutover_id);
    if (client_.is_connected()) {
      if (buffered && !buffered->empty()) {
        client_->OnInput(*buffered);
      }
      client_->OnTerminalHostInputCutoverCancelled(
          cutover_id, "terminal-host renderer prepared for cold eviction");
    }
  } else if (terminal_host_input_cutover_.direct()) {
    // A direct route without its socket cannot prove whether its last accepted
    // input reached the host. Restore compatibility, but fail the eviction so
    // Browser runs its normal snapshot recovery instead of retiring this
    // renderer under a false drain guarantee.
    terminal_host_input_cutover_.FallBackToCompatibility();
    const std::string error =
        "direct terminal-host route disappeared before cold eviction";
    if (client_.is_connected()) {
      client_->OnTerminalHostDisconnected(error);
    }
    CompleteColdEviction(eviction_id, false, error);
    return;
  }
  CompleteColdEviction(eviction_id, true, std::string());
}

void CmuxTerminalRendererService::CompleteColdEviction(
    uint64_t eviction_id,
    bool success,
    std::string error) {
  if (eviction_id == 0 || eviction_id != cold_eviction_id_) {
    return;
  }
  cold_eviction_id_ = 0;
  cold_eviction_terminal_host_attempt_ = 0;
  cold_eviction_release_started_ = false;
  if (client_.is_connected()) {
    client_->OnColdEvictionReady(eviction_id, success, std::move(error));
  }
}

void CmuxTerminalRendererService::CancelColdEviction(std::string error) {
  if (cold_eviction_id_ == 0) {
    return;
  }
  if (shutting_down_.load(std::memory_order_acquire)) {
    cold_eviction_id_ = 0;
    cold_eviction_terminal_host_attempt_ = 0;
    cold_eviction_release_started_ = false;
    return;
  }
  const uint64_t eviction_id = cold_eviction_id_;
  CompleteColdEviction(eviction_id, false, std::move(error));
}

void CmuxTerminalRendererService::RequestTerminalHostViewerSize(
    uint16_t columns,
    uint16_t rows,
    float scale_factor,
    uint64_t geometry_epoch) {
  if (!QueueSurfaceOperation(
          PendingSurfaceOperationKind::kService,
          base::BindOnce(
              &CmuxTerminalRendererService::RequestTerminalHostViewerSizeNow,
              weak_this_, columns, rows, scale_factor, geometry_epoch),
          sizeof(columns) + sizeof(rows) + sizeof(scale_factor) +
              sizeof(geometry_epoch))) {
    ReportError("renderer surface-operation queue exceeded its limit");
  }
}

void CmuxTerminalRendererService::RequestTerminalHostViewerSizeNow(
    uint16_t columns,
    uint16_t rows,
    float scale_factor,
    uint64_t geometry_epoch) {
  if (!terminal_host_socket_ || !terminal_host_stream_ready_ ||
      geometry_epoch <= latest_geometry_epoch_) {
    return;
  }
  columns = std::max<uint16_t>(columns, 1);
  rows = std::max<uint16_t>(rows, 1);
  terminal_host_has_latest_viewer_size_ = true;
  terminal_host_latest_viewer_columns_ = columns;
  terminal_host_latest_viewer_rows_ = rows;
  terminal_host_latest_viewer_scale_factor_ = std::max(scale_factor, 1.0f);
  terminal_host_latest_viewer_geometry_epoch_ = geometry_epoch;
  terminal_host_geometry_epoch_floor_ =
      std::max(terminal_host_geometry_epoch_floor_, geometry_epoch);
  if (!visible_.load(std::memory_order_acquire)) {
    terminal_host_viewer_size_dirty_ = true;
    return;
  }
  if (terminal_host_resize_in_flight_) {
    terminal_host_resize_pending_ = true;
    terminal_host_pending_columns_ = columns;
    terminal_host_pending_rows_ = rows;
    terminal_host_pending_scale_factor_ = std::max(scale_factor, 1.0f);
    terminal_host_pending_geometry_epoch_ = geometry_epoch;
    return;
  }
  QueueTerminalHostViewerSize(columns, rows, scale_factor, geometry_epoch);
}

void CmuxTerminalRendererService::SetFocus(bool focused) {
  if (!surface_) {
    return;
  }
  if (!QueueSemanticInput(
          base::BindOnce(&CmuxTerminalRendererService::SetFocusNow, weak_this_,
                         focused),
          sizeof(focused))) {
    ReportError("renderer semantic-input queue exceeded its limit");
  }
}

void CmuxTerminalRendererService::SetFocusNow(bool focused) {
  if (surface_) {
    ghostty_surface_set_focus(surface_, focused);
  }
}

void CmuxTerminalRendererService::SetVisible(bool visible) {
  if (terminal_host_requested_visible_ == visible) {
    return;
  }
  terminal_host_requested_visible_ = visible;
  uint64_t visibility_generation = ++terminal_host_visibility_generation_;
  if (visibility_generation == 0) {
    visibility_generation = ++terminal_host_visibility_generation_;
  }
  if (!QueueSurfaceOperation(
          PendingSurfaceOperationKind::kService,
          base::BindOnce(&CmuxTerminalRendererService::SetVisibleNow,
                         weak_this_, visible, visibility_generation),
          sizeof(visible))) {
    ReportError("renderer surface-operation queue exceeded its limit");
  }
}

void CmuxTerminalRendererService::SetVisibleNow(
    bool visible,
    uint64_t visibility_generation) {
  if (visibility_generation != terminal_host_visibility_generation_ ||
      visible != terminal_host_requested_visible_) {
    return;
  }
  if (visible_.load(std::memory_order_acquire) == visible) {
    return;
  }
  visible_.store(visible, std::memory_order_release);
  if (terminal_host_socket_ && terminal_host_stream_ready_) {
    if (!visible) {
      terminal_host_visibility_restore_pending_ = false;
      terminal_host_viewer_size_dirty_ =
          terminal_host_viewer_size_dirty_ ||
          terminal_host_resize_in_flight_ || terminal_host_resize_pending_;
      terminal_host_resize_in_flight_ = false;
      terminal_host_resize_request_id_ = 0;
      terminal_host_resize_epoch_ = 0;
      terminal_host_resize_replay_observed_ = false;
      terminal_host_resize_pending_ = false;
      ScheduleTerminalHostViewerRelease();
    } else {
      terminal_host_viewer_release_timer_.Stop();
      terminal_host_visibility_restore_pending_ =
          terminal_host_visibility_restore_pending_ ||
          (terminal_host_viewer_released_ &&
           terminal_host_viewer_size_acks_);
    }
    if (visible && terminal_host_has_latest_viewer_size_ &&
        (terminal_host_viewer_released_ ||
         terminal_host_viewer_size_dirty_)) {
      if (terminal_host_replay_in_flight_) {
        terminal_host_resize_pending_ = true;
        terminal_host_pending_columns_ = terminal_host_latest_viewer_columns_;
        terminal_host_pending_rows_ = terminal_host_latest_viewer_rows_;
        terminal_host_pending_scale_factor_ =
            terminal_host_latest_viewer_scale_factor_;
        terminal_host_pending_geometry_epoch_ =
            terminal_host_latest_viewer_geometry_epoch_;
      } else {
        QueueTerminalHostViewerSize(
            terminal_host_latest_viewer_columns_,
            terminal_host_latest_viewer_rows_,
            terminal_host_latest_viewer_scale_factor_,
            terminal_host_latest_viewer_geometry_epoch_);
      }
    }
  }
  if (surface_) {
    const bool publish_visible =
        visible && !terminal_host_visibility_restore_pending_;
    ghostty_surface_set_occlusion(surface_, publish_visible);
    if (publish_visible) {
      ghostty_surface_refresh(surface_);
    }
  }
}

void CmuxTerminalRendererService::ScheduleTerminalHostViewerRelease() {
  if (visible_.load(std::memory_order_acquire) || !terminal_host_socket_ ||
      !terminal_host_stream_ready_ || terminal_host_viewer_released_) {
    return;
  }
  const uint64_t visibility_generation =
      terminal_host_visibility_generation_;
  terminal_host_viewer_release_timer_.Start(
      FROM_HERE, kTerminalHostViewerReleaseDebounce,
      base::BindOnce(
          &CmuxTerminalRendererService::ReleaseTerminalHostViewerIfHidden,
          weak_this_, visibility_generation));
}

void CmuxTerminalRendererService::ReleaseTerminalHostViewerIfHidden(
    uint64_t visibility_generation) {
  if (visibility_generation != terminal_host_visibility_generation_ ||
      terminal_host_requested_visible_ ||
      visible_.load(std::memory_order_acquire) || !terminal_host_socket_ ||
      !terminal_host_stream_ready_ || terminal_host_viewer_released_) {
    return;
  }
  if (!terminal_host_socket_->QueueReleaseViewer()) {
    terminal_host_socket_->Stop();
    return;
  }
  terminal_host_viewer_released_ = true;
}

void CmuxTerminalRendererService::CompleteTerminalHostVisibilityRestore() {
  if (!terminal_host_visibility_restore_pending_) {
    return;
  }
  terminal_host_visibility_restore_pending_ = false;
  if (surface_ && visible_.load(std::memory_order_acquire)) {
    ghostty_surface_set_occlusion(surface_, true);
    ghostty_surface_refresh(surface_);
  }
}

void CmuxTerminalRendererService::ProcessOutput(
    const std::vector<uint8_t>& bytes) {
  if (bytes.empty()) {
    return;
  }
  if (!QueueSurfaceOperation(
          PendingSurfaceOperationKind::kService,
          base::BindOnce(&CmuxTerminalRendererService::ProcessOutputNow,
                         weak_this_, bytes),
          bytes.size())) {
    ReportError("renderer surface-operation queue exceeded its limit");
  }
}

void CmuxTerminalRendererService::ProcessOutputNow(std::vector<uint8_t> bytes) {
  // Presence of the socket, rather than stream readiness, is the cutover
  // boundary. The direct Snapshot supersedes every compatibility byte that
  // arrives while authentication/replay is still in flight.
  if (terminal_host_socket_) {
    return;
  }
  EnqueueOutput(std::move(bytes));
}

bool CmuxTerminalRendererService::CanQueueSurfaceOperation(
    size_t byte_count) const {
  const size_t operation_count = pending_surface_operations_.size() +
                                 (fenced_surface_operation_ ? 1u : 0u);
  return operation_count < kMaxPendingSurfaceOperations &&
         byte_count <= kMaxPendingSurfaceOperationBytes &&
         pending_surface_operation_bytes_ <=
             kMaxPendingSurfaceOperationBytes - byte_count;
}

bool CmuxTerminalRendererService::QueueSurfaceOperation(
    PendingSurfaceOperationKind kind,
    base::OnceClosure action,
    size_t byte_count) {
  if (!action || !CanQueueSurfaceOperation(byte_count)) {
    return false;
  }
  pending_surface_operation_bytes_ += byte_count;
  pending_surface_operations_.emplace_back(kind, std::move(action), byte_count);
  DrainSurfaceOperations();
  return true;
}

bool CmuxTerminalRendererService::QueueSemanticInput(base::OnceClosure action,
                                                     size_t byte_count) {
  return QueueSurfaceOperation(PendingSurfaceOperationKind::kSemanticInput,
                               std::move(action), byte_count);
}

void CmuxTerminalRendererService::DrainSurfaceOperations() {
  if (draining_surface_operations_) {
    return;
  }
  base::AutoReset<bool> draining(&draining_surface_operations_, true);
  while (!authoritative_replay_in_flight_ &&
         !surface_operation_fence_in_flight_ &&
         !pending_surface_operations_.empty()) {
    auto next = pending_surface_operations_.begin();
    if (pending_surface_operations_.front().kind ==
            PendingSurfaceOperationKind::kSemanticInput &&
        !authoritative_state_ready_) {
      // During initial attach or crash fallback, the replacement Snapshot can
      // arrive behind user input that must be encoded from that Snapshot.
      // State restore, cancellation/teardown, and final lifecycle barriers may
      // bypass blocked semantic actions. Begin/Attach remain ordinary service
      // operations: letting either overtake older semantic input would violate
      // the compatibility-to-direct cutover prefix.
      next = std::find_if(
          pending_surface_operations_.begin() + 1,
          pending_surface_operations_.end(),
          [](const PendingSurfaceOperation& candidate) {
            return candidate.kind ==
                       PendingSurfaceOperationKind::kStateRestore ||
                   candidate.kind == PendingSurfaceOperationKind::kControl ||
                   candidate.kind ==
                       PendingSurfaceOperationKind::kOutputBarrier;
          });
      if (next == pending_surface_operations_.end()) {
        return;
      }
    }

    PendingSurfaceOperation operation = std::move(*next);
    pending_surface_operations_.erase(next);
    pending_surface_operation_bytes_ -= operation.byte_count;
    const bool requires_output_barrier =
        operation.kind == PendingSurfaceOperationKind::kSemanticInput ||
        operation.kind == PendingSurfaceOperationKind::kOutputBarrier;
    if (!requires_output_barrier) {
      std::move(operation.action).Run();
      continue;
    }
    if (!surface_output_pending_) {
      std::move(operation.action).Run();
      continue;
    }

    // Only the output worker mutates Ghostty's VT parser. Hold every later
    // service operation in the bounded lane while this finite fence waits for
    // all output observed before the input. The reply runs on this sequence,
    // so semantic encoders retain Ghostty's app-thread affinity.
    surface_operation_fence_in_flight_ = true;
    pending_surface_operation_bytes_ += operation.byte_count;
    fenced_surface_operation_.emplace(std::move(operation));
    // This marker must share the batched output FIFO. Posting a standalone
    // task would let a drain that already owns later bytes run across the
    // semantic/lifecycle boundary.
    EnqueueSurfaceOperationFence();
  }
}

void CmuxTerminalRendererService::CompleteSurfaceOperationFence() {
  if (!surface_operation_fence_in_flight_ || !fenced_surface_operation_) {
    return;
  }
  surface_operation_fence_in_flight_ = false;
  surface_output_pending_ = false;
  PendingSurfaceOperation operation = std::move(*fenced_surface_operation_);
  fenced_surface_operation_.reset();
  pending_surface_operation_bytes_ -= operation.byte_count;
  if (operation.kind != PendingSurfaceOperationKind::kSemanticInput ||
      authoritative_state_ready_) {
    std::move(operation.action).Run();
  } else {
    pending_surface_operation_bytes_ += operation.byte_count;
    pending_surface_operations_.push_front(std::move(operation));
  }
  if (!shutting_down_.load(std::memory_order_acquire)) {
    DrainSurfaceOperations();
  }
}

void CmuxTerminalRendererService::SendKey(mojom::TerminalKeyEventPtr event) {
  if (!surface_ || !event) {
    return;
  }
  const size_t byte_count = sizeof(*event) + event->text.size();
  if (!QueueSemanticInput(
          base::BindOnce(&CmuxTerminalRendererService::SendKeyNow, weak_this_,
                         std::move(event)),
          byte_count)) {
    ReportError("renderer semantic-input queue exceeded its limit");
  }
}

void CmuxTerminalRendererService::SendKeyNow(mojom::TerminalKeyEventPtr event) {
  if (!surface_ || !event) {
    return;
  }
  ghostty_input_key_s key = {};
  key.action = static_cast<ghostty_input_action_e>(event->action);
  key.mods = static_cast<ghostty_input_mods_e>(event->mods);
  key.consumed_mods = static_cast<ghostty_input_mods_e>(event->consumed_mods);
  key.keycode = event->keycode;
  key.text = event->text.c_str();
  key.unshifted_codepoint = event->unshifted_codepoint;
  key.composing = event->composing;
  ghostty_surface_key(surface_, key);
}

void CmuxTerminalRendererService::SendText(const std::string& text) {
  if (!surface_ || text.empty()) {
    return;
  }
  if (!QueueSemanticInput(
          base::BindOnce(&CmuxTerminalRendererService::SendTextNow, weak_this_,
                         text),
          text.size())) {
    ReportError("renderer semantic-input queue exceeded its limit");
  }
}

void CmuxTerminalRendererService::SendTextNow(std::string text) {
  if (surface_ && !text.empty()) {
    ghostty_surface_text_input(surface_, text.data(), text.size());
  }
}

void CmuxTerminalRendererService::Paste(const std::string& text,
                                        PasteCallback callback) {
  if (!surface_) {
    std::move(callback).Run(false, "renderer is not initialized");
    return;
  }
  if (text.empty()) {
    std::move(callback).Run(true, std::string());
    return;
  }
  if (!CanQueueSurfaceOperation(text.size())) {
    std::move(callback).Run(false,
                            "renderer semantic-input queue exceeded its limit");
    ReportError("renderer semantic-input queue exceeded its limit");
    return;
  }
  QueueSemanticInput(base::BindOnce(&CmuxTerminalRendererService::PasteNow,
                                    weak_this_, text, std::move(callback)),
                     text.size());
}

void CmuxTerminalRendererService::PasteNow(std::string text,
                                           PasteCallback callback) {
  if (!surface_) {
    std::move(callback).Run(false, "renderer is not initialized");
    return;
  }
  // Use Ghostty's encoder on every route. Its IO callback joins the same
  // service-sequence FIFO as keys, text, and mouse events; queueing a semantic
  // Paste frame here would let it overtake an earlier posted IoWrite task.
  // The direct mirror is kept authoritative by Snapshot/replay, so bracketed
  // paste mode is resolved from the same terminal state.
  pending_paste_text_ = text;
  constexpr char kPasteAction[] = "paste_from_clipboard";
  const bool accepted = ghostty_surface_binding_action(
      surface_, kPasteAction, sizeof(kPasteAction) - 1);
  pending_paste_text_.reset();
  std::move(callback).Run(
      accepted, accepted ? std::string() : "Ghostty rejected paste input");
}

void CmuxTerminalRendererService::CopySelection(
    CopySelectionCallback callback) {
  if (!surface_) {
    std::move(callback).Run(false, std::string());
    return;
  }
  if (!CanQueueSurfaceOperation(0)) {
    std::move(callback).Run(false, std::string());
    ReportError("renderer semantic-input queue exceeded its limit");
    return;
  }
  QueueSemanticInput(
      base::BindOnce(&CmuxTerminalRendererService::CopySelectionNow, weak_this_,
                     std::move(callback)),
      0);
}

void CmuxTerminalRendererService::CopySelectionNow(
    CopySelectionCallback callback) {
  if (!surface_) {
    std::move(callback).Run(false, std::string());
    return;
  }
  ghostty_text_s selection = {};
  if (!ghostty_surface_read_selection(surface_, &selection) ||
      !selection.text) {
    std::move(callback).Run(false, std::string());
    return;
  }
  std::string text(selection.text, selection.text_len);
  ghostty_surface_free_text(surface_, &selection);
  std::move(callback).Run(true, std::move(text));
}

void CmuxTerminalRendererService::SetPreedit(const std::string& text) {
  if (!surface_) {
    return;
  }
  if (!QueueSemanticInput(
          base::BindOnce(&CmuxTerminalRendererService::SetPreeditNow,
                         weak_this_, text),
          text.size())) {
    ReportError("renderer semantic-input queue exceeded its limit");
  }
}

void CmuxTerminalRendererService::SetPreeditNow(std::string text) {
  if (surface_) {
    ghostty_surface_preedit(surface_, text.empty() ? nullptr : text.data(),
                            text.size());
  }
}

void CmuxTerminalRendererService::MouseMove(float x, float y, uint32_t mods) {
  if (!surface_) {
    return;
  }
  if (!QueueSemanticInput(
          base::BindOnce(&CmuxTerminalRendererService::MouseMoveNow, weak_this_,
                         x, y, mods),
          sizeof(x) + sizeof(y) + sizeof(mods))) {
    ReportError("renderer semantic-input queue exceeded its limit");
  }
}

void CmuxTerminalRendererService::MouseMoveNow(float x,
                                               float y,
                                               uint32_t mods) {
  if (surface_) {
    ghostty_surface_mouse_pos(surface_, x, y,
                              static_cast<ghostty_input_mods_e>(mods));
  }
}

void CmuxTerminalRendererService::MouseButton(int32_t state,
                                              int32_t button,
                                              uint32_t mods,
                                              MouseButtonCallback callback) {
  if (!surface_) {
    std::move(callback).Run(false, false);
    return;
  }
  const size_t byte_count = sizeof(state) + sizeof(button) + sizeof(mods);
  if (!CanQueueSurfaceOperation(byte_count)) {
    std::move(callback).Run(false, false);
    ReportError("renderer semantic-input queue exceeded its limit");
    return;
  }
  QueueSemanticInput(
      base::BindOnce(&CmuxTerminalRendererService::MouseButtonNow, weak_this_,
                     state, button, mods, std::move(callback)),
      byte_count);
}

void CmuxTerminalRendererService::MouseButtonNow(int32_t state,
                                                 int32_t button,
                                                 uint32_t mods,
                                                 MouseButtonCallback callback) {
  bool consumed = false;
  if (surface_) {
    consumed = ghostty_surface_mouse_button(
        surface_, static_cast<ghostty_input_mouse_state_e>(state),
        static_cast<ghostty_input_mouse_button_e>(button),
        static_cast<ghostty_input_mods_e>(mods));
  }
  std::move(callback).Run(surface_ != nullptr, consumed);
}

void CmuxTerminalRendererService::MouseScroll(float x,
                                              float y,
                                              int32_t scroll_mods) {
  if (!surface_) {
    return;
  }
  if (!QueueSemanticInput(
          base::BindOnce(&CmuxTerminalRendererService::MouseScrollNow,
                         weak_this_, x, y, scroll_mods),
          sizeof(x) + sizeof(y) + sizeof(scroll_mods))) {
    ReportError("renderer semantic-input queue exceeded its limit");
  }
}

void CmuxTerminalRendererService::MouseScrollNow(float x,
                                                 float y,
                                                 int32_t scroll_mods) {
  if (surface_) {
    ghostty_surface_mouse_scroll(surface_, x, y, scroll_mods);
  }
}

void CmuxTerminalRendererService::AcknowledgeFrameDelivery(
    uint64_t delivery_sequence) {
  bool post_task = false;
  {
    base::AutoLock hold(frame_lock_);
    if (!frame_in_flight_ || delivery_sequence != frame_in_flight_sequence_) {
      return;
    }
    frame_in_flight_ = false;
    frame_in_flight_sequence_ = 0;
    if (pending_frame_ && !frame_task_posted_) {
      frame_task_posted_ = true;
      post_task = true;
    }
  }
  if (post_task) {
    service_task_runner_->PostTask(
        FROM_HERE,
        base::BindOnce(&CmuxTerminalRendererService::DeliverPendingFrame,
                       weak_this_));
  }
}

void CmuxTerminalRendererService::ReleaseFrame(uint64_t frame_token) {
  ReleaseTrackedFrame(frame_token);
}

void CmuxTerminalRendererService::WakeupThunk(void* userdata) {
  auto* service = static_cast<CmuxTerminalRendererService*>(userdata);
  if (!service->tick_coalescer_.RequestTickTask()) {
    return;
  }
  service->service_task_runner_->PostTask(
      FROM_HERE,
      base::BindOnce(&CmuxTerminalRendererService::Tick, service->weak_this_));
}

bool CmuxTerminalRendererService::ActionThunk(ghostty_app_t,
                                              ghostty_target_s,
                                              ghostty_action_s) {
  return false;
}

bool CmuxTerminalRendererService::ReadClipboardThunk(
    void* userdata,
    ghostty_clipboard_e location,
    void* state) {
  auto* service = static_cast<CmuxTerminalRendererService*>(userdata);
  if (!service->surface_ || !state || location != GHOSTTY_CLIPBOARD_STANDARD ||
      !service->pending_paste_text_) {
    return false;
  }
  ghostty_surface_complete_clipboard_request(
      service->surface_, service->pending_paste_text_->c_str(), state,
      /*confirmed=*/true);
  return true;
}

void CmuxTerminalRendererService::ConfirmReadClipboardThunk(
    void*,
    const char*,
    void*,
    ghostty_clipboard_request_e) {}

void CmuxTerminalRendererService::WriteClipboardThunk(
    void*,
    ghostty_clipboard_e,
    const ghostty_clipboard_content_s*,
    size_t,
    bool) {}

void CmuxTerminalRendererService::CloseSurfaceThunk(void* userdata,
                                                    bool process_alive) {
  auto* service = static_cast<CmuxTerminalRendererService*>(userdata);
  service->service_task_runner_->PostTask(
      FROM_HERE, base::BindOnce(&CmuxTerminalRendererService::DeliverClose,
                                service->weak_this_, process_alive));
}

void CmuxTerminalRendererService::IoWriteThunk(void* userdata,
                                               const char* bytes,
                                               uintptr_t len) {
  if (!bytes || len == 0) {
    return;
  }
  auto* service = static_cast<CmuxTerminalRendererService*>(userdata);
  const auto* first = reinterpret_cast<const uint8_t*>(bytes);
  const base::span<const uint8_t> input =
      UNSAFE_BUFFERS(base::span(first, len));
  std::vector<uint8_t> copy(input.begin(), input.end());
  if (service->service_task_runner_->RunsTasksInCurrentSequence()) {
    // Ghostty semantic APIs synchronously invoke this callback. Classify
    // those bytes before a later cutover cancel/commit marker on this same
    // sequence. Device replies generated by the output worker still post.
    service->DeliverInput(std::move(copy));
    return;
  }
  service->service_task_runner_->PostTask(
      FROM_HERE, base::BindOnce(&CmuxTerminalRendererService::DeliverInput,
                                service->weak_this_, std::move(copy)));
}

ghostty_metal_external_frame_disposition_e
CmuxTerminalRendererService::PresentLeasedThunk(
    void* userdata,
    const ghostty_metal_external_frame_s* frame) {
  if (!frame || !frame->iosurface || frame->frame_token == 0) {
    return GHOSTTY_METAL_EXTERNAL_FRAME_DROP;
  }
  auto* service = static_cast<CmuxTerminalRendererService*>(userdata);
  {
    base::AutoLock hold(service->frame_lock_);
    if (service->shutting_down_.load(std::memory_order_relaxed)) {
      return GHOSTTY_METAL_EXTERNAL_FRAME_DROP;
    }
    ++service->active_frame_callbacks_;
  }
  const bool acquired = service->QueueFrame(*frame);
  {
    base::AutoLock hold(service->frame_lock_);
    --service->active_frame_callbacks_;
    if (service->active_frame_callbacks_ == 0) {
      service->frame_callbacks_drained_.Signal();
    }
  }
  return acquired ? GHOSTTY_METAL_EXTERNAL_FRAME_ACQUIRE
                  : GHOSTTY_METAL_EXTERNAL_FRAME_DROP;
}

void CmuxTerminalRendererService::Tick() {
  tick_coalescer_.BeginTickTask();
  if (app_) {
    ghostty_app_tick(app_);
  }
  if (tick_coalescer_.EndTickTask()) {
    service_task_runner_->PostTask(
        FROM_HERE,
        base::BindOnce(&CmuxTerminalRendererService::Tick, weak_this_));
  }
}

void CmuxTerminalRendererService::DeliverInput(std::vector<uint8_t> bytes) {
  if (bytes.empty()) {
    return;
  }
  const std::string_view input(reinterpret_cast<const char*>(bytes.data()),
                               bytes.size());
  switch (terminal_host_input_cutover_.RouteInput(input)) {
    case TerminalInputCutover::RouteResult::kCompatibility:
      if (client_.is_connected()) {
        client_->OnInput(bytes);
      }
      return;
    case TerminalInputCutover::RouteResult::kBuffered:
      return;
    case TerminalInputCutover::RouteResult::kDirect:
      if (!terminal_host_socket_ || !terminal_host_stream_ready_ ||
          !terminal_host_socket_->QueueInput(input)) {
        // QueueInput false proves this batch was not accepted. Return it on
        // the ordered client pipe before the socket's disconnect marker; the
        // Browser holds it until fallback clears direct mode. This is distinct
        // from the acknowledged limitation for an older accepted-but-unflushed
        // batch, whose bytes are no longer available here.
        if (client_.is_connected()) {
          client_->OnInput(bytes);
        }
        if (terminal_host_socket_) {
          terminal_host_socket_->Stop();
        } else {
          terminal_host_input_cutover_.FallBackToCompatibility();
          if (client_.is_connected()) {
            client_->OnTerminalHostDisconnected(
                "terminal-host socket disappeared before direct input");
          }
        }
      }
      return;
    case TerminalInputCutover::RouteResult::kOverflow:
      CancelOverflowedTerminalHostInput(std::move(bytes));
      return;
  }
}

void CmuxTerminalRendererService::DeliverClose(bool process_alive) {
  if (client_.is_connected()) {
    client_->OnClose(process_alive);
  }
}

bool CmuxTerminalRendererService::QueueFrame(
    const ghostty_metal_external_frame_s& frame) {
  if (shutting_down_.load(std::memory_order_acquire) ||
      !visible_.load(std::memory_order_acquire)) {
    return false;
  }
  IOSurfaceRef io_surface = static_cast<IOSurfaceRef>(frame.iosurface);
  if (frame.host_context == 0 || frame.width_px == 0 || frame.height_px == 0 ||
      IOSurfaceGetWidth(io_surface) != frame.width_px ||
      IOSurfaceGetHeight(io_surface) != frame.height_px ||
      frame.color_space != GHOSTTY_METAL_EXTERNAL_COLOR_SPACE_DISPLAY_P3) {
    return false;
  }
  if (CFPropertyListRef color_space = DisplayP3ColorSpacePropertyList()) {
    IOSurfaceSetValue(io_surface, kIOSurfaceColorSpace, color_space);
  }
  mach_port_t port = IOSurfaceCreateMachPort(io_surface);
  if (port == MACH_PORT_NULL) {
    return false;
  }
  mojo::PlatformHandle handle{base::apple::ScopedMachSendRight(port)};

  bool post_task = false;
  uint64_t dropped_token = 0;
  {
    base::AutoLock hold(frame_lock_);
    if (shutting_down_.load(std::memory_order_relaxed)) {
      return false;
    }
    if (!outstanding_frame_tokens_.insert(frame.frame_token).second) {
      LOG(ERROR) << "cmux terminal renderer received duplicate frame token "
                 << frame.frame_token;
      return false;
    }
    if (pending_frame_) {
      dropped_token = pending_frame_->frame_token;
    }
    pending_frame_.emplace(std::move(handle), ++next_frame_sequence_,
                           frame.frame_token, frame.host_context,
                           frame.width_px, frame.height_px,
                           mojom::TerminalFrameColorSpace::kDisplayP3);
    if (!frame_task_posted_ && !frame_in_flight_) {
      frame_task_posted_ = true;
      post_task = true;
    }
  }
  if (dropped_token != 0) {
    ReleaseTrackedFrame(dropped_token);
  }
  if (post_task) {
    service_task_runner_->PostTask(
        FROM_HERE,
        base::BindOnce(&CmuxTerminalRendererService::DeliverPendingFrame,
                       weak_this_));
  }
  return true;
}

void CmuxTerminalRendererService::DeliverPendingFrame() {
  std::optional<PendingFrame> frame;
  const bool client_connected =
      client_.is_connected() && visible_.load(std::memory_order_acquire) &&
      !terminal_host_visibility_restore_pending_;
  {
    base::AutoLock hold(frame_lock_);
    frame = std::move(pending_frame_);
    pending_frame_.reset();
    frame_task_posted_ = false;
    if (frame && client_connected) {
      frame_in_flight_ = true;
      frame_in_flight_sequence_ = frame->delivery_sequence;
    }
  }
  if (frame && client_connected) {
    client_->OnFrame(std::move(frame->handle), frame->delivery_sequence,
                     frame->frame_token, frame->geometry_epoch, frame->width_px,
                     frame->height_px, frame->color_space);
  } else if (frame) {
    ReleaseTrackedFrame(frame->frame_token);
  }
}

void CmuxTerminalRendererService::NotifyGridSize(uint32_t width_px,
                                                 uint32_t height_px,
                                                 uint64_t geometry_epoch) {
  if (!surface_ || !client_.is_connected()) {
    return;
  }
  const ghostty_surface_size_s size = ghostty_surface_size(surface_);
  client_->OnGridSize(std::max<uint16_t>(size.columns, 1),
                      std::max<uint16_t>(size.rows, 1), width_px, height_px,
                      size.cell_width_px, size.cell_height_px, geometry_epoch);
}

void CmuxTerminalRendererService::DrainTerminalHostIngress(
    uint64_t attempt,
    std::shared_ptr<TerminalHostIngress> ingress) {
  if (!ingress) {
    return;
  }
  if (shutting_down_.load(std::memory_order_acquire) ||
      attempt != ingress->attempt ||
      attempt != terminal_host_attempt_ ||
      terminal_host_ingress_ != ingress) {
    base::AutoLock hold(ingress->lock);
    ingress->queue.Shutdown();
    return;
  }

  const base::TimeTicks deadline =
      base::TimeTicks::Now() + kTerminalHostIngressSliceTime;
  size_t operation_count = 0;
  size_t byte_count = 0;
  while (operation_count < kTerminalHostIngressSliceOperations &&
         (operation_count == 0 || byte_count < kTerminalHostIngressSliceBytes) &&
         base::TimeTicks::Now() < deadline) {
    std::optional<PendingTerminalHostIngressOperation> operation;
    {
      base::AutoLock hold(ingress->lock);
      if (ingress->queue.empty()) {
        break;
      }
      operation.emplace(ingress->queue.TakeFront());
    }

    byte_count += operation->ByteCount();
    ++operation_count;
    switch (operation->kind) {
      case PendingTerminalHostIngressOperationKind::kEvent:
        HandleTerminalHostEvent(attempt, std::move(operation->event));
        break;
      case PendingTerminalHostIngressOperationKind::kReady:
        HandleTerminalHostReady(attempt);
        break;
      case PendingTerminalHostIngressOperationKind::kClosed:
        HandleTerminalHostClosed(attempt, std::move(operation->close));
        break;
    }

    if (attempt != terminal_host_attempt_ ||
        terminal_host_ingress_ != ingress) {
      base::AutoLock hold(ingress->lock);
      ingress->queue.Shutdown();
      return;
    }
  }

  using FinishSliceResult =
      CmuxTerminalIngressDrainQueue<
          PendingTerminalHostIngressOperation>::FinishSliceResult;
  FinishSliceResult result;
  {
    base::AutoLock hold(ingress->lock);
    result = ingress->queue.FinishSlice();
  }
  if (result == FinishSliceResult::kRepost) {
    service_task_runner_->PostTask(
        FROM_HERE,
        base::BindOnce(
            &CmuxTerminalRendererService::DrainTerminalHostIngress, weak_this_,
            attempt, std::move(ingress)));
    return;
  }
  if (result != FinishSliceResult::kOverflow ||
      attempt != terminal_host_attempt_ ||
      terminal_host_ingress_ != ingress) {
    return;
  }

  TerminalHostSocketClose close;
  close.reason = TerminalHostSocketCloseReason::kBackpressure;
  close.message = "terminal-host renderer ingress exceeded its bounded queue";
  if (terminal_host_socket_) {
    terminal_host_socket_->Stop();
  }
  HandleTerminalHostClosed(attempt, std::move(close));
}

void CmuxTerminalRendererService::HandleTerminalHostEvent(
    uint64_t attempt,
    TerminalHostRendererEvent event) {
  if (attempt != terminal_host_attempt_ || !terminal_host_socket_) {
    return;
  }
  const size_t byte_count =
      PendingTerminalHostIngressOperation::EventByteCount(event);
  const PendingSurfaceOperationKind kind =
      event.kind == TerminalHostMessageKind::kExit
          ? PendingSurfaceOperationKind::kOutputBarrier
          : PendingSurfaceOperationKind::kStateRestore;
  if (!QueueSurfaceOperation(
          kind,
          base::BindOnce(
              &CmuxTerminalRendererService::HandleTerminalHostEventNow,
              weak_this_, attempt, std::move(event)),
          byte_count)) {
    terminal_host_socket_->Stop();
  }
}

void CmuxTerminalRendererService::HandleTerminalHostEventNow(
    uint64_t attempt,
    TerminalHostRendererEvent event) {
  if (attempt != terminal_host_attempt_ || !terminal_host_socket_) {
    return;
  }
  switch (event.kind) {
    case TerminalHostMessageKind::kSnapshot: {
      if (!event.snapshot || !event.colors) {
        terminal_host_socket_->Stop();
        return;
      }
      // PWD is snapshot state, not merely a future live delta. cmux-tui
      // suppresses duplicate OSC 7 publications after the host parser has
      // observed them, so a renderer attaching later must publish the
      // snapshot value now. An absent value is an explicit clear; otherwise a
      // Browser could retain the PWD from the renderer's preceding host.
      if (client_.is_connected()) {
        client_->OnTerminalHostPwd(event.snapshot->cwd.value_or(std::string()));
      }
      const uint64_t geometry_epoch = std::max(
          latest_geometry_epoch_ + 1, terminal_host_geometry_epoch_floor_);
      latest_geometry_epoch_ = geometry_epoch;
      ApplyTerminalHostReplay(event.snapshot->cols, event.snapshot->rows,
                              std::move(event.snapshot->replay),
                              std::move(event.colors), attempt, geometry_epoch);
      return;
    }
    case TerminalHostMessageKind::kOutput:
      if (event.colors) {
        AppendTerminalHostColorMetadata(*event.colors, &event.bytes);
      }
      if (!event.bytes.empty()) {
        // Output and its advertised Colors delta enter Ghostty through one
        // worker task and one process-output call.
        EnqueueOutput(std::move(event.bytes));
      }
      if (event.colors) {
        PublishTerminalHostBackground(*event.colors);
      }
      return;
    case TerminalHostMessageKind::kResized: {
      if (!event.resize || !event.colors) {
        terminal_host_socket_->Stop();
        return;
      }
      if (terminal_host_viewer_size_acks_ && terminal_host_resize_in_flight_) {
        terminal_host_resize_replay_observed_ = true;
      }
      const uint64_t geometry_epoch = std::max(
          latest_geometry_epoch_ + 1, terminal_host_geometry_epoch_floor_);
      latest_geometry_epoch_ = geometry_epoch;
      ApplyTerminalHostReplay(event.resize->cols, event.resize->rows,
                              std::move(event.resize->replay),
                              std::move(event.colors), attempt, geometry_epoch);
      return;
    }
    case TerminalHostMessageKind::kResizeAck:
      if (terminal_host_close_pending_attempt_ == attempt) {
        return;
      }
      if (!event.resize_ack || !terminal_host_viewer_size_acks_) {
        terminal_host_socket_->Stop();
        return;
      }
      ApplyTerminalHostResizeAck(*event.resize_ack, event.request_id);
      return;
    case TerminalHostMessageKind::kColors: {
      if (!event.colors) {
        terminal_host_socket_->Stop();
        return;
      }
      std::vector<uint8_t> bytes;
      AppendTerminalHostColorMetadata(*event.colors, &bytes);
      EnqueueOutput(std::move(bytes));
      PublishTerminalHostBackground(*event.colors);
      return;
    }
    case TerminalHostMessageKind::kTitle:
      if (client_.is_connected()) {
        client_->OnTerminalHostTitle(event.text);
      }
      return;
    case TerminalHostMessageKind::kPwd:
      if (client_.is_connected()) {
        client_->OnTerminalHostPwd(event.text);
      }
      return;
    case TerminalHostMessageKind::kBell:
      if (client_.is_connected()) {
        client_->OnTerminalHostBell();
      }
      return;
    case TerminalHostMessageKind::kExit:
      terminal_host_exit_reported_ = true;
      // Exit is queued as an output barrier, so every older Output task has
      // already reached Ghostty. Preserve that final inspectable state before
      // Browser synchronously asks this renderer to detach the host socket.
      terminal_host_final_state_ = true;
      authoritative_state_ready_ = true;
      if (client_.is_connected()) {
        client_->OnTerminalHostExit();
      }
      return;
    default:
      terminal_host_socket_->Stop();
      return;
  }
}

void CmuxTerminalRendererService::HandleTerminalHostReady(uint64_t attempt) {
  if (attempt != terminal_host_attempt_ || !terminal_host_socket_) {
    return;
  }
  if (!QueueSurfaceOperation(
          PendingSurfaceOperationKind::kService,
          base::BindOnce(
              &CmuxTerminalRendererService::HandleTerminalHostReadyNow,
              weak_this_, attempt),
          0)) {
    terminal_host_socket_->Stop();
  }
}

void CmuxTerminalRendererService::HandleTerminalHostReadyNow(uint64_t attempt) {
  if (attempt != terminal_host_attempt_ || !terminal_host_socket_) {
    return;
  }
  if (terminal_host_close_pending_attempt_ == attempt) {
    return;
  }
  terminal_host_stream_ready_ = true;
  if (!visible_.load(std::memory_order_acquire)) {
    ScheduleTerminalHostViewerRelease();
  }
  MaybeCompleteTerminalHostAttach(attempt);
}

void CmuxTerminalRendererService::HandleTerminalHostClosed(
    uint64_t attempt,
    TerminalHostSocketClose close) {
  if (attempt != terminal_host_attempt_) {
    return;
  }
  const bool exited = close.reason == TerminalHostSocketCloseReason::kExited;
  // A failure invalidates publication immediately, but still parses causally
  // prior bytes before the semantic fence. Exit is different: its preceding
  // replay is the final inspectable frame and may finish publishing, while
  // close_pending prevents it from committing a doomed direct Attach.
  if (!exited) {
    if (++authoritative_replay_generation_ == 0) {
      ++authoritative_replay_generation_;
    }
    authoritative_replay_in_flight_ = false;
    terminal_host_replay_in_flight_ = false;
    // Do not open the semantic gate against stale pre-direct modes. The close
    // control operation bypasses that gate and the replacement compatibility
    // Snapshot will open it after fallback.
    authoritative_state_ready_ = false;
  }
  terminal_host_close_pending_attempt_ = attempt;
  terminal_host_close_pending_exited_ = exited;
  const size_t byte_count = close.message.size() + sizeof(close.reason);
  if (!CanQueueSurfaceOperation(byte_count)) {
    ReportError("renderer surface-operation queue exceeded its limit");
    HandleTerminalHostClosedNow(attempt, std::move(close));
    return;
  }
  QueueSurfaceOperation(
      PendingSurfaceOperationKind::kControl,
      base::BindOnce(&CmuxTerminalRendererService::HandleTerminalHostClosedNow,
                     weak_this_, attempt, std::move(close)),
      byte_count);
}

void CmuxTerminalRendererService::HandleTerminalHostClosedNow(
    uint64_t attempt,
    TerminalHostSocketClose close) {
  if (attempt != terminal_host_attempt_) {
    return;
  }
  const uint64_t cold_eviction_id =
      cold_eviction_terminal_host_attempt_ == attempt ? cold_eviction_id_ : 0;
  const bool cold_eviction_succeeded =
      cold_eviction_id != 0 &&
      ((cold_eviction_release_started_ &&
        close.reason == TerminalHostSocketCloseReason::kStopped) ||
       close.reason == TerminalHostSocketCloseReason::kExited);
  terminal_host_close_pending_attempt_ = 0;
  terminal_host_close_pending_exited_ = false;
  std::shared_ptr<TerminalHostIngress> ingress =
      std::move(terminal_host_ingress_);
  if (ingress) {
    base::AutoLock hold(ingress->lock);
    ingress->queue.Shutdown();
  }
  // Fence every replay/grid/background task already posted by this socket.
  // Its close and a replacement compatibility replay can otherwise share a
  // numeric geometry epoch and let stale direct state open the view gate.
  ++terminal_host_attempt_;
  if (++authoritative_replay_generation_ == 0) {
    ++authoritative_replay_generation_;
  }
  authoritative_replay_in_flight_ = false;
  const bool exited = terminal_host_exit_reported_ ||
                      close.reason == TerminalHostSocketCloseReason::kExited;
  terminal_host_final_state_ = exited;
  // The final mirror remains useful for selection/copy after process exit.
  // Outbound bytes fall back through Browser, whose exited backend drops
  // them. Non-exit failure instead waits for a fresh compatibility Snapshot.
  authoritative_state_ready_ = exited;
  const std::string reason =
      close.message.empty() ? TerminalHostSocketCloseReasonMessage(close.reason)
                            : close.message;
  uint64_t cancelled_cutover_id = 0;
  std::optional<std::vector<uint8_t>> cancelled_input;
  if (terminal_host_input_cutover_.buffering()) {
    cancelled_cutover_id = terminal_host_input_cutover_.cutover_id();
    cancelled_input = terminal_host_input_cutover_.Cancel(cancelled_cutover_id);
  }
  if (terminal_host_input_cutover_.direct()) {
    terminal_host_input_cutover_.FallBackToCompatibility();
  }
  terminal_host_socket_.reset();
  terminal_host_stream_ready_ = false;
  terminal_host_initial_replay_complete_ = false;
  terminal_host_replay_in_flight_ = false;
  terminal_host_close_pending_attempt_ = 0;
  terminal_host_viewer_size_acks_ = false;
  terminal_host_resize_in_flight_ = false;
  terminal_host_resize_request_id_ = 0;
  terminal_host_resize_epoch_ = 0;
  terminal_host_resize_replay_observed_ = false;
  terminal_host_resize_pending_ = false;
  terminal_host_has_latest_viewer_size_ = false;
  terminal_host_viewer_released_ = false;
  terminal_host_viewer_size_dirty_ = false;
  CompleteTerminalHostVisibilityRestore();
  terminal_host_viewer_release_timer_.Stop();
  if (client_.is_connected() && cancelled_input) {
    // The socket can disappear before Attach's response crosses its separate
    // Mojo pipe. Return the complete buffered prefix and retire the active id
    // on the ordered client pipe first, so neither response ordering can leave
    // a stale cutover blocking every future attempt.
    if (!cancelled_input->empty()) {
      client_->OnInput(*cancelled_input);
    }
    client_->OnTerminalHostInputCutoverCancelled(cancelled_cutover_id, reason);
  }
  if (terminal_host_attach_callback_) {
    std::move(terminal_host_attach_callback_).Run(false, reason);
  }
  if (exited && !terminal_host_exit_reported_ && client_.is_connected()) {
    // Exit can be observed by the socket immediately after its event was
    // posted but before a replay-blocked event reaches the ordered lane.
    // Close is then the authoritative lifecycle marker.
    terminal_host_exit_reported_ = true;
    client_->OnTerminalHostExit();
  }
  // Intentional detach increments terminal_host_attempt_ before stopping the
  // socket, so its close callback is rejected at the top of this method. Cold
  // eviction is the sole intentional close that keeps the attempt live: an
  // orderly kStopped close proves queued input plus ReleaseViewer were flushed
  // and must not look like a direct-renderer failure to Browser.
  if (!exited && !cold_eviction_succeeded && client_.is_connected()) {
    client_->OnTerminalHostDisconnected(reason);
  }
  if (cold_eviction_id != 0) {
    CompleteColdEviction(
        cold_eviction_id, cold_eviction_succeeded,
        cold_eviction_succeeded ? std::string() : reason);
  }
  if (!shutting_down_.load(std::memory_order_acquire)) {
    DrainSurfaceOperations();
  }
}

void CmuxTerminalRendererService::MaybeCompleteTerminalHostAttach(
    uint64_t attempt) {
  if (attempt != terminal_host_attempt_ || !terminal_host_socket_ ||
      terminal_host_close_pending_attempt_ == attempt ||
      !terminal_host_stream_ready_ || !terminal_host_initial_replay_complete_ ||
      !terminal_host_attach_callback_) {
    return;
  }
  const uint64_t cutover_id = terminal_host_input_cutover_.cutover_id();
  if (!terminal_host_input_cutover_.CanCommit(cutover_id)) {
    AttachTerminalHostCallback callback =
        std::move(terminal_host_attach_callback_);
    StopTerminalHost(true);
    std::move(callback).Run(false, "terminal-host input cutover was cancelled");
    return;
  }
  const std::vector<uint8_t>& buffered =
      terminal_host_input_cutover_.buffered_input();
  if (!buffered.empty()) {
    const std::string_view input(reinterpret_cast<const char*>(buffered.data()),
                                 buffered.size());
    if (!terminal_host_socket_->QueueInput(input)) {
      AttachTerminalHostCallback callback =
          std::move(terminal_host_attach_callback_);
      terminal_host_socket_->Stop();
      std::move(callback).Run(false,
                              "terminal-host rejected buffered cutover input");
      return;
    }
  }
  if (!terminal_host_input_cutover_.Commit(cutover_id)) {
    AttachTerminalHostCallback callback =
        std::move(terminal_host_attach_callback_);
    terminal_host_socket_->Stop();
    std::move(callback).Run(false, "terminal-host input cutover commit failed");
    return;
  }
  std::move(terminal_host_attach_callback_).Run(true, std::string());
}

void CmuxTerminalRendererService::CancelTerminalHostInputCutoverInternal(
    uint64_t cutover_id,
    std::string reason,
    bool stop_terminal_host) {
  const bool matching_direct =
      terminal_host_input_cutover_.direct() &&
      terminal_host_input_cutover_.cutover_id() == cutover_id;
  std::optional<std::vector<uint8_t>> buffered =
      terminal_host_input_cutover_.Cancel(cutover_id);
  if (matching_direct) {
    terminal_host_input_cutover_.FallBackToCompatibility();
  }
  if (stop_terminal_host && (buffered || matching_direct)) {
    StopTerminalHost(true);
  }
  if (client_.is_connected()) {
    if (buffered && !buffered->empty()) {
      client_->OnInput(*buffered);
    }
    // Even a stale request receives an ordered marker. The browser matches the
    // id, so it can retire an old attempt without perturbing a newer one.
    client_->OnTerminalHostInputCutoverCancelled(cutover_id, reason);
  }
}

void CmuxTerminalRendererService::CancelOverflowedTerminalHostInput(
    std::vector<uint8_t> bytes) {
  const uint64_t cutover_id = terminal_host_input_cutover_.cutover_id();
  std::optional<std::vector<uint8_t>> buffered =
      terminal_host_input_cutover_.Cancel(cutover_id);
  StopTerminalHost(true);
  if (!client_.is_connected()) {
    return;
  }
  if (buffered && !buffered->empty()) {
    client_->OnInput(*buffered);
  }
  if (!bytes.empty()) {
    client_->OnInput(bytes);
  }
  client_->OnTerminalHostInputCutoverCancelled(
      cutover_id, "terminal-host input cutover buffer exceeded its limit");
}

void CmuxTerminalRendererService::StopTerminalHost(bool release_viewer) {
  CancelColdEviction("cold eviction superseded by terminal-host teardown");
  terminal_host_viewer_release_timer_.Stop();
  std::shared_ptr<TerminalHostIngress> ingress =
      std::move(terminal_host_ingress_);
  if (ingress) {
    base::AutoLock hold(ingress->lock);
    ingress->queue.Shutdown();
  }
  ++terminal_host_attempt_;
  if (++authoritative_replay_generation_ == 0) {
    ++authoritative_replay_generation_;
  }
  authoritative_replay_in_flight_ = false;
  authoritative_state_ready_ = terminal_host_final_state_;
  if (terminal_host_socket_) {
    if (release_viewer) {
      terminal_host_socket_->ReleaseAndStop();
    } else {
      terminal_host_socket_->Stop();
    }
    terminal_host_socket_.reset();
  }
  terminal_host_stream_ready_ = false;
  terminal_host_initial_replay_complete_ = false;
  terminal_host_replay_in_flight_ = false;
  terminal_host_close_pending_attempt_ = 0;
  terminal_host_close_pending_exited_ = false;
  terminal_host_viewer_size_acks_ = false;
  terminal_host_resize_in_flight_ = false;
  terminal_host_resize_request_id_ = 0;
  terminal_host_resize_epoch_ = 0;
  terminal_host_resize_replay_observed_ = false;
  terminal_host_resize_pending_ = false;
  terminal_host_has_latest_viewer_size_ = false;
  terminal_host_viewer_released_ = false;
  terminal_host_viewer_size_dirty_ = false;
  CompleteTerminalHostVisibilityRestore();
  if (terminal_host_attach_callback_) {
    std::move(terminal_host_attach_callback_)
        .Run(false, "terminal-host renderer detached");
  }
  if (!shutting_down_.load(std::memory_order_acquire)) {
    DrainSurfaceOperations();
  }
}

void CmuxTerminalRendererService::QueueTerminalHostViewerSize(
    uint16_t columns,
    uint16_t rows,
    float scale_factor,
    uint64_t geometry_epoch) {
  if (!terminal_host_socket_ || terminal_host_resize_in_flight_ ||
      !visible_.load(std::memory_order_acquire)) {
    return;
  }
  terminal_host_resize_in_flight_ = true;
  terminal_host_resize_epoch_ = geometry_epoch;
  terminal_host_resize_replay_observed_ = false;
  uint64_t request_id = 0;
  if (terminal_host_viewer_size_acks_) {
    request_id = terminal_host_next_resize_request_id_++;
    if (request_id == 0) {
      request_id = terminal_host_next_resize_request_id_++;
    }
    if (terminal_host_next_resize_request_id_ == 0) {
      terminal_host_next_resize_request_id_ = 1;
    }
  }
  terminal_host_resize_request_id_ = request_id;
  // Any opportunistic frame caused by changing backing scale retains the old
  // external context. The browser has already gated on `geometry_epoch`, so
  // only the subsequent canonical Resized replay or ResizeAck completion can
  // publish.
  const float scale = std::max(scale_factor, 1.0f);
  if (surface_scale_factor_ != scale) {
    ghostty_surface_set_content_scale(surface_, scale, scale);
    surface_scale_factor_ = scale;
  }
  terminal_host_viewer_released_ = false;
  terminal_host_viewer_size_dirty_ = false;
  if (!terminal_host_socket_->QueueViewerSize(columns, rows, request_id)) {
    terminal_host_socket_->Stop();
  }
}

void CmuxTerminalRendererService::SendPendingTerminalHostViewerSize() {
  if (!terminal_host_resize_pending_ || terminal_host_resize_in_flight_ ||
      !visible_.load(std::memory_order_acquire)) {
    return;
  }
  terminal_host_resize_pending_ = false;
  QueueTerminalHostViewerSize(terminal_host_pending_columns_,
                              terminal_host_pending_rows_,
                              terminal_host_pending_scale_factor_,
                              terminal_host_pending_geometry_epoch_);
}

void CmuxTerminalRendererService::ApplyTerminalHostResizeAck(
    const TerminalHostResizeAck& ack,
    uint64_t request_id) {
  if (!terminal_host_socket_ || request_id == 0) {
    return;
  }
  if (!terminal_host_resize_in_flight_) {
    // A ViewerSize can be released while its targeted response is already
    // queued. It no longer owns a browser geometry epoch and is safe to drop.
    return;
  }
  if (request_id != terminal_host_resize_request_id_) {
    if (request_id < terminal_host_resize_request_id_) {
      return;
    }
    terminal_host_socket_->Stop();
    return;
  }

  const bool canonical_changed =
      (ack.result_flags & kTerminalHostResizeAckCanonicalChanged) != 0;
  if (canonical_changed && !terminal_host_resize_replay_observed_) {
    terminal_host_socket_->Stop();
    return;
  }
  if (!canonical_changed &&
      latest_geometry_epoch_ < terminal_host_resize_epoch_) {
    if (!surface_) {
      terminal_host_socket_->Stop();
      return;
    }
    const ghostty_surface_size_s current = ghostty_surface_size(surface_);
    const bool exact_grid_retained =
        std::max<uint16_t>(current.columns, 1) == ack.cols &&
        std::max<uint16_t>(current.rows, 1) == ack.rows;
    const uint64_t geometry_epoch = terminal_host_resize_epoch_;
    latest_geometry_epoch_ = geometry_epoch;
    if (exact_grid_retained &&
        surface_scale_factor_ == published_surface_scale_factor_) {
      // The canonical VT and retained IOSurface are already exact. Advance the
      // frame context so later ordinary output remains publishable, but avoid
      // manufacturing a grid mutation and replacement frame for every live-
      // resize cell boundary while another viewer pins the canonical grid.
      ghostty_surface_set_external_frame_context(surface_, geometry_epoch);
      if (client_.is_connected()) {
        client_->OnTerminalHostViewerSizeUnchanged(
            std::max<uint16_t>(current.columns, 1),
            std::max<uint16_t>(current.rows, 1), geometry_epoch);
      }
    } else {
      // A backing-scale change still needs a newly rasterized IOSurface. A
      // grid mismatch is a defensive slow path: reconcile to the host's ACK
      // and preserve the normal exact-frame publication boundary.
      ghostty_surface_size_s resolved = {};
      if (!ghostty_surface_set_grid_size(surface_, ack.cols, ack.rows,
                                         &resolved)) {
        terminal_host_socket_->Stop();
        return;
      }
      ghostty_surface_set_external_frame_context(surface_, geometry_epoch);
      if (client_.is_connected()) {
        const ghostty_surface_size_s size = ghostty_surface_size(surface_);
        client_->OnGridSize(std::max<uint16_t>(resolved.columns, 1),
                            std::max<uint16_t>(resolved.rows, 1),
                            resolved.width_px, resolved.height_px,
                            size.cell_width_px, size.cell_height_px,
                            geometry_epoch);
      }
      ghostty_surface_refresh(surface_);
      published_surface_scale_factor_ = surface_scale_factor_;
    }
  }

  terminal_host_resize_in_flight_ = false;
  terminal_host_resize_request_id_ = 0;
  terminal_host_resize_epoch_ = 0;
  terminal_host_resize_replay_observed_ = false;
  CompleteTerminalHostVisibilityRestore();
  SendPendingTerminalHostViewerSize();
}

void CmuxTerminalRendererService::ApplyTerminalHostReplay(
    uint16_t columns,
    uint16_t rows,
    std::vector<uint8_t> replay,
    std::optional<TerminalHostColors> colors,
    uint64_t attempt,
    uint64_t geometry_epoch) {
  if (!surface_ || !colors || attempt != terminal_host_attempt_) {
    return;
  }
  terminal_host_replay_in_flight_ = true;
  columns = std::max<uint16_t>(columns, 1);
  rows = std::max<uint16_t>(rows, 1);
  ghostty_surface_size_s resolved = {};
  if (!ghostty_surface_set_grid_size(surface_, columns, rows, &resolved)) {
    terminal_host_socket_->Stop();
    return;
  }
  std::vector<uint8_t> replacement = {0x1b, 'c'};
  replacement.insert(replacement.end(), replay.begin(), replay.end());
  AppendTerminalHostColorMetadata(*colors, &replacement);
  if (!EnqueueAuthoritativeReplay(
          std::move(replacement), std::max<uint16_t>(resolved.columns, 1),
          std::max<uint16_t>(resolved.rows, 1), resolved.width_px,
          resolved.height_px, geometry_epoch, std::move(colors), attempt)) {
    terminal_host_replay_in_flight_ = false;
    if (terminal_host_socket_) {
      terminal_host_socket_->Stop();
    }
  } else if (terminal_host_close_pending_attempt_ == attempt &&
             !terminal_host_close_pending_exited_) {
    // Parse this causally earlier replay before the semantic fence, but Close
    // receipt has already forbidden it from publishing or completing Attach.
    if (++authoritative_replay_generation_ == 0) {
      ++authoritative_replay_generation_;
    }
    authoritative_replay_in_flight_ = false;
    terminal_host_replay_in_flight_ = false;
    authoritative_state_ready_ = true;
  }
}

void CmuxTerminalRendererService::PublishTerminalHostBackground(
    const TerminalHostColors& colors) {
  if (!client_.is_connected()) {
    return;
  }
  if (colors.background) {
    client_->OnTerminalHostBackground(true, colors.background->red,
                                      colors.background->green,
                                      colors.background->blue);
  } else {
    client_->OnTerminalHostBackground(false, 0, 0, 0);
  }
}

void CmuxTerminalRendererService::ReleaseTrackedFrame(uint64_t frame_token) {
  if (frame_token == 0 || !surface_) {
    return;
  }
  {
    base::AutoLock hold(frame_lock_);
    if (outstanding_frame_tokens_.erase(frame_token) == 0) {
      return;
    }
  }
  if (!ghostty_surface_release_external_frame(surface_, frame_token)) {
    LOG(WARNING) << "cmux terminal renderer rejected frame lease "
                 << frame_token;
  }
}

void CmuxTerminalRendererService::ReleaseAllFrames() {
  if (!surface_) {
    return;
  }
  std::vector<uint64_t> tokens;
  {
    base::AutoLock hold(frame_lock_);
    tokens.assign(outstanding_frame_tokens_.begin(),
                  outstanding_frame_tokens_.end());
    outstanding_frame_tokens_.clear();
    pending_frame_.reset();
    frame_task_posted_ = false;
    frame_in_flight_ = false;
    frame_in_flight_sequence_ = 0;
  }
  for (uint64_t token : tokens) {
    if (!ghostty_surface_release_external_frame(surface_, token)) {
      LOG(WARNING) << "cmux terminal renderer failed to release frame lease "
                   << token << " during teardown";
    }
  }
}

void CmuxTerminalRendererService::EnqueueOutput(std::vector<uint8_t> bytes) {
  if (!surface_ || bytes.empty() ||
      shutting_down_.load(std::memory_order_acquire)) {
    return;
  }
  if (!ReserveOutputBytes(bytes.size())) {
    return;
  }
  const size_t byte_count = bytes.size();
  if (!QueueOutputOperation(PendingOutputOperation(std::move(bytes)))) {
    queued_output_bytes_.fetch_sub(byte_count, std::memory_order_acq_rel);
    return;
  }
  surface_output_pending_ = true;
}

bool CmuxTerminalRendererService::EnqueueAuthoritativeReplay(
    std::vector<uint8_t> bytes,
    uint16_t columns,
    uint16_t rows,
    uint32_t width_px,
    uint32_t height_px,
    uint64_t geometry_epoch,
    std::optional<TerminalHostColors> colors,
    uint64_t terminal_host_attempt) {
  if (!surface_ || bytes.empty() || !ReserveOutputBytes(bytes.size())) {
    return false;
  }
  if (++authoritative_replay_generation_ == 0) {
    ++authoritative_replay_generation_;
  }
  const uint64_t replay_generation = authoritative_replay_generation_;
  authoritative_replay_in_flight_ = true;
  authoritative_state_ready_ = false;
  const size_t byte_count = bytes.size();
  if (!QueueOutputOperation(PendingOutputOperation(
          std::move(bytes), columns, rows, width_px, height_px, geometry_epoch,
          std::move(colors), terminal_host_attempt, replay_generation))) {
    queued_output_bytes_.fetch_sub(byte_count, std::memory_order_acq_rel);
    authoritative_replay_in_flight_ = false;
    return false;
  }
  surface_output_pending_ = true;
  return true;
}

bool CmuxTerminalRendererService::ReserveOutputBytes(size_t byte_count) {
  size_t queued = queued_output_bytes_.load(std::memory_order_relaxed);
  do {
    if (queued > kMaxQueuedOutputBytes ||
        byte_count > kMaxQueuedOutputBytes - queued) {
      ReportError("renderer output queue exceeded 32 MiB");
      return false;
    }
  } while (!queued_output_bytes_.compare_exchange_weak(
      queued, queued + byte_count, std::memory_order_acq_rel,
      std::memory_order_relaxed));
  return true;
}

bool CmuxTerminalRendererService::QueueOutputOperation(
    PendingOutputOperation operation) {
  if (shutting_down_.load(std::memory_order_acquire)) {
    return false;
  }

  bool should_post = false;
  {
    base::AutoLock hold(output_queue_lock_);
    if (shutting_down_.load(std::memory_order_acquire)) {
      return false;
    }
    should_post = output_queue_.Push(std::move(operation));
  }
  if (!should_post) {
    return true;
  }

  // The scheduling latch is set before releasing output_queue_lock_. A worker
  // can therefore retire before PostTask() returns, but every racing producer
  // still observes either that task or owns exactly one successor.
  CHECK(output_thread_.IsRunning());
  CHECK(output_thread_.task_runner()->PostTask(
      FROM_HERE,
      base::BindOnce(&CmuxTerminalRendererService::DrainOutputOnWorker,
                     base::Unretained(this))));
  return true;
}

void CmuxTerminalRendererService::EnqueueSurfaceOperationFence() {
  if (!QueueOutputOperation(PendingOutputOperation()) &&
      !shutting_down_.load(std::memory_order_acquire)) {
    ReportError("renderer output worker rejected an ordering fence");
  }
}

void CmuxTerminalRendererService::DrainOutputOnWorker() {
  for (;;) {
    std::optional<PendingOutputOperation> operation;
    std::vector<std::vector<uint8_t>> output_chunks;
    {
      base::AutoLock hold(output_queue_lock_);
      if (output_queue_.RetireIfEmpty()) {
        return;
      }
      operation.emplace(output_queue_.TakeFront());
      if (operation->kind == PendingOutputOperationKind::kOutput) {
        output_chunks.push_back(std::move(operation->bytes));
        while (!output_queue_.empty() &&
               output_queue_.front().kind ==
                   PendingOutputOperationKind::kOutput) {
          PendingOutputOperation next = output_queue_.TakeFront();
          output_chunks.push_back(std::move(next.bytes));
        }
      }
    }

    if (operation->kind == PendingOutputOperationKind::kOutput) {
      CHECK(!output_chunks.empty());
      if (output_chunks.size() == 1) {
        ProcessOutputOnWorker(std::move(output_chunks.front()));
        continue;
      }
      size_t byte_count = 0;
      for (const auto& chunk : output_chunks) {
        byte_count += chunk.size();
      }
      std::vector<uint8_t> batch;
      batch.reserve(byte_count);
      for (const auto& chunk : output_chunks) {
        batch.insert(batch.end(), chunk.begin(), chunk.end());
      }
      ProcessOutputOnWorker(std::move(batch));
      continue;
    }

    if (operation->kind ==
        PendingOutputOperationKind::kAuthoritativeReplay) {
      ProcessAuthoritativeReplayOnWorker(
          std::move(operation->bytes), operation->columns, operation->rows,
          operation->width_px, operation->height_px,
          operation->geometry_epoch, std::move(operation->colors),
          operation->terminal_host_attempt, operation->replay_generation);
      continue;
    }
    DCHECK(operation->kind ==
           PendingOutputOperationKind::kSurfaceOperationFence);
    service_task_runner_->PostTask(
        FROM_HERE,
        base::BindOnce(
            &CmuxTerminalRendererService::CompleteSurfaceOperationFence,
            weak_this_));
  }
}

void CmuxTerminalRendererService::ClearOutputQueueAfterWorkerStop() {
  std::deque<PendingOutputOperation> abandoned;
  {
    base::AutoLock hold(output_queue_lock_);
    abandoned = output_queue_.TakeAllForShutdown();
  }
  size_t abandoned_bytes = 0;
  for (const auto& operation : abandoned) {
    abandoned_bytes += operation.bytes.size();
  }
  const size_t reserved_bytes =
      queued_output_bytes_.exchange(0, std::memory_order_acq_rel);
  DCHECK_EQ(reserved_bytes, abandoned_bytes);
}

void CmuxTerminalRendererService::ProcessOutputOnWorker(
    std::vector<uint8_t> bytes) {
  if (surface_) {
    ghostty_surface_process_output(
        surface_, reinterpret_cast<const char*>(bytes.data()), bytes.size());
  }
  queued_output_bytes_.fetch_sub(bytes.size(), std::memory_order_acq_rel);
}

void CmuxTerminalRendererService::ProcessAuthoritativeReplayOnWorker(
    std::vector<uint8_t> bytes,
    uint16_t columns,
    uint16_t rows,
    uint32_t width_px,
    uint32_t height_px,
    uint64_t geometry_epoch,
    std::optional<TerminalHostColors> colors,
    uint64_t terminal_host_attempt,
    uint64_t replay_generation) {
  if (surface_) {
    ghostty_surface_process_output(
        surface_, reinterpret_cast<const char*>(bytes.data()), bytes.size());
  }
  queued_output_bytes_.fetch_sub(bytes.size(), std::memory_order_acq_rel);
  service_task_runner_->PostTask(
      FROM_HERE,
      base::BindOnce(&CmuxTerminalRendererService::CompleteAuthoritativeReplay,
                     weak_this_, columns, rows, width_px, height_px,
                     geometry_epoch, std::move(colors), terminal_host_attempt,
                     replay_generation));
}

void CmuxTerminalRendererService::CompleteAuthoritativeReplay(
    uint16_t columns,
    uint16_t rows,
    uint32_t width_px,
    uint32_t height_px,
    uint64_t geometry_epoch,
    std::optional<TerminalHostColors> colors,
    uint64_t terminal_host_attempt,
    uint64_t replay_generation) {
  if (!surface_ || replay_generation != authoritative_replay_generation_ ||
      geometry_epoch != latest_geometry_epoch_) {
    return;
  }
  if (terminal_host_attempt != 0 &&
      terminal_host_attempt != terminal_host_attempt_) {
    return;
  }
  if (colors) {
    PublishTerminalHostBackground(*colors);
  }
  ghostty_surface_set_external_frame_context(surface_, geometry_epoch);
  published_surface_scale_factor_ = surface_scale_factor_;
  if (client_.is_connected()) {
    const ghostty_surface_size_s size = ghostty_surface_size(surface_);
    client_->OnGridSize(columns, rows, width_px, height_px, size.cell_width_px,
                        size.cell_height_px, geometry_epoch);
  }
  // OnGridSize is enqueued on the same client pipe before any frame delivery
  // task produced by this refresh, so the browser installs exact dimensions
  // before the first publishable frame arrives.
  ghostty_surface_refresh(surface_);
  authoritative_replay_in_flight_ = false;
  authoritative_state_ready_ = true;
  if (terminal_host_attempt != 0) {
    terminal_host_initial_replay_complete_ = true;
    MaybeCompleteTerminalHostAttach(terminal_host_attempt);
    if (terminal_host_resize_in_flight_ && !terminal_host_viewer_size_acks_) {
      terminal_host_resize_in_flight_ = false;
      terminal_host_resize_request_id_ = 0;
      terminal_host_resize_epoch_ = 0;
      terminal_host_resize_replay_observed_ = false;
      CompleteTerminalHostVisibilityRestore();
    }
    terminal_host_replay_in_flight_ = false;
    if (!terminal_host_replay_in_flight_) {
      SendPendingTerminalHostViewerSize();
    }
  }
  DrainSurfaceOperations();
}

void CmuxTerminalRendererService::ReportError(std::string reason) {
  if (client_.is_connected()) {
    client_->OnError(reason);
  }
}

}  // namespace cmux
