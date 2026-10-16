// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef CHROME_SERVICES_CMUX_TERMINAL_RENDERER_CMUX_TERMINAL_RENDERER_SERVICE_H_
#define CHROME_SERVICES_CMUX_TERMINAL_RENDERER_CMUX_TERMINAL_RENDERER_SERVICE_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/weak_ptr.h"
#include "base/synchronization/condition_variable.h"
#include "base/synchronization/lock.h"
#include "base/task/sequenced_task_runner.h"
#include "base/threading/thread.h"
#include "base/timer/timer.h"
#include "chrome/services/cmux_terminal_renderer/cmux_terminal_ingress_drain_queue.h"
#include "chrome/services/cmux_terminal_renderer/cmux_terminal_output_drain_queue.h"
#include "chrome/services/cmux_terminal_renderer/cmux_terminal_tick_coalescer.h"
#include "chrome/services/cmux_terminal_renderer/public/cpp/cmux_terminal_host_socket.h"
#include "chrome/services/cmux_terminal_renderer/public/cpp/cmux_terminal_input_cutover.h"
#include "chrome/services/cmux_terminal_renderer/public/mojom/cmux_terminal_renderer.mojom.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "mojo/public/cpp/platform/platform_handle.h"

// GHOSTTY_STATIC comes from ghostty_public_config.
#include "ghostty.h"

namespace cmux {

// A single Ghostty app/surface in a dedicated utility process. cmux TUI owns
// the shell and authoritative VT state; this service is disposable and can be
// rebuilt from a replay after a crash.
class CmuxTerminalRendererService final : public mojom::CmuxTerminalRenderer {
 public:
  explicit CmuxTerminalRendererService(
      mojo::PendingReceiver<mojom::CmuxTerminalRenderer> receiver);
  CmuxTerminalRendererService(const CmuxTerminalRendererService&) = delete;
  CmuxTerminalRendererService& operator=(const CmuxTerminalRendererService&) =
      delete;
  ~CmuxTerminalRendererService() override;

  // mojom::CmuxTerminalRenderer:
  void Initialize(mojo::PendingRemote<mojom::CmuxTerminalRendererClient> client,
                  uint32_t width_px,
                  uint32_t height_px,
                  float scale_factor,
                  const std::string& theme_name,
                  uint64_t geometry_epoch,
                  InitializeCallback callback) override;
  void UpdateTheme(const std::string& theme_config) override;
  void SetSize(uint32_t width_px,
               uint32_t height_px,
               float scale_factor,
               uint64_t geometry_epoch) override;
  void SetFocus(bool focused) override;
  void SetVisible(bool visible) override;
  void ProcessOutput(const std::vector<uint8_t>& bytes) override;
  void ResetAndReplayAtGrid(uint16_t columns,
                            uint16_t rows,
                            const std::vector<uint8_t>& replay,
                            uint64_t geometry_epoch) override;
  void BeginTerminalHostInputCutover(uint64_t cutover_id) override;
  void AttachTerminalHost(mojo::PlatformHandle socket,
                          const std::vector<uint8_t>& terminal_id,
                          const std::vector<uint8_t>& terminal_incarnation,
                          uint32_t rights,
                          uint32_t protocol_flags,
                          uint64_t cutover_id,
                          AttachTerminalHostCallback callback) override;
  void CancelTerminalHostInputCutover(uint64_t cutover_id) override;
  void DetachTerminalHost() override;
  void PrepareForColdEviction(uint64_t eviction_id) override;
  void RequestTerminalHostViewerSize(uint16_t columns,
                                     uint16_t rows,
                                     float scale_factor,
                                     uint64_t geometry_epoch) override;
  void SendKey(mojom::TerminalKeyEventPtr event) override;
  void SendText(const std::string& text) override;
  void Paste(const std::string& text, PasteCallback callback) override;
  void CopySelection(CopySelectionCallback callback) override;
  void SetPreedit(const std::string& text) override;
  void MouseMove(float x, float y, uint32_t mods) override;
  void MouseButton(int32_t state,
                   int32_t button,
                   uint32_t mods,
                   MouseButtonCallback callback) override;
  void MouseScroll(float x, float y, int32_t scroll_mods) override;
  void AcknowledgeFrameDelivery(uint64_t delivery_sequence) override;
  void ReleaseFrame(uint64_t frame_token) override;

 private:
  struct PendingFrame {
    PendingFrame(mojo::PlatformHandle handle,
                 uint64_t delivery_sequence,
                 uint64_t frame_token,
                 uint64_t geometry_epoch,
                 uint32_t width_px,
                 uint32_t height_px,
                 mojom::TerminalFrameColorSpace color_space);
    PendingFrame(PendingFrame&&);
    PendingFrame& operator=(PendingFrame&&);
    ~PendingFrame();

    mojo::PlatformHandle handle;
    uint64_t delivery_sequence;
    uint64_t frame_token;
    uint64_t geometry_epoch;
    uint32_t width_px;
    uint32_t height_px;
    mojom::TerminalFrameColorSpace color_space;
  };

  enum class PendingSurfaceOperationKind {
    kService,
    // Terminal-host cancellation and teardown must be able to retire a direct
    // attempt while semantic input is waiting for an exact replacement state.
    kControl,
    kStateRestore,
    kSemanticInput,
    // A lifecycle marker that must observe every older output-worker task
    // before it is published to Browser, but does not itself require a restored
    // terminal state.
    kOutputBarrier,
  };

  struct PendingSurfaceOperation {
    PendingSurfaceOperation(PendingSurfaceOperationKind kind,
                            base::OnceClosure action,
                            size_t byte_count);
    PendingSurfaceOperation(PendingSurfaceOperation&&);
    PendingSurfaceOperation& operator=(PendingSurfaceOperation&&);
    ~PendingSurfaceOperation();

    PendingSurfaceOperationKind kind;
    base::OnceClosure action;
    size_t byte_count;
  };

  enum class PendingOutputOperationKind {
    kOutput,
    kAuthoritativeReplay,
    kSurfaceOperationFence,
  };

  // One typed FIFO is shared by ordinary output, authoritative replays, and
  // surface-operation fences. Keeping the markers in the same queue prevents
  // a batched drain from inheriting bytes across an ordering boundary.
  struct PendingOutputOperation {
    explicit PendingOutputOperation(std::vector<uint8_t> output);
    PendingOutputOperation(std::vector<uint8_t> replay,
                           uint16_t columns,
                           uint16_t rows,
                           uint32_t width_px,
                           uint32_t height_px,
                           uint64_t geometry_epoch,
                           std::optional<TerminalHostColors> colors,
                           uint64_t terminal_host_attempt,
                           uint64_t replay_generation);
    PendingOutputOperation();
    PendingOutputOperation(PendingOutputOperation&&);
    PendingOutputOperation& operator=(PendingOutputOperation&&);
    ~PendingOutputOperation();

    PendingOutputOperationKind kind;
    std::vector<uint8_t> bytes;
    uint16_t columns = 0;
    uint16_t rows = 0;
    uint32_t width_px = 0;
    uint32_t height_px = 0;
    uint64_t geometry_epoch = 0;
    std::optional<TerminalHostColors> colors;
    uint64_t terminal_host_attempt = 0;
    uint64_t replay_generation = 0;
  };

  enum class PendingTerminalHostIngressOperationKind {
    kEvent,
    kReady,
    kClosed,
  };

  struct PendingTerminalHostIngressOperation {
    explicit PendingTerminalHostIngressOperation(
        TerminalHostRendererEvent event);
    PendingTerminalHostIngressOperation();
    explicit PendingTerminalHostIngressOperation(TerminalHostSocketClose close);
    PendingTerminalHostIngressOperation(PendingTerminalHostIngressOperation&&);
    PendingTerminalHostIngressOperation& operator=(
        PendingTerminalHostIngressOperation&&);
    ~PendingTerminalHostIngressOperation();

    static size_t EventByteCount(const TerminalHostRendererEvent& event);
    size_t ByteCount() const;
    bool IsLifecycle() const;
    bool TryCoalesce(PendingTerminalHostIngressOperation* newer,
                     size_t max_combined_bytes);

    PendingTerminalHostIngressOperationKind kind;
    TerminalHostRendererEvent event;
    TerminalHostSocketClose close;
  };

  struct TerminalHostIngress {
    explicit TerminalHostIngress(uint64_t attempt);
    ~TerminalHostIngress();

    const uint64_t attempt;
    base::Lock lock;
    CmuxTerminalIngressDrainQueue<PendingTerminalHostIngressOperation> queue
        GUARDED_BY(lock);
  };

  static void WakeupThunk(void* userdata);
  static bool ActionThunk(ghostty_app_t app,
                          ghostty_target_s target,
                          ghostty_action_s action);
  static bool ReadClipboardThunk(void* userdata,
                                 ghostty_clipboard_e location,
                                 void* state);
  static void ConfirmReadClipboardThunk(void* userdata,
                                        const char* value,
                                        void* state,
                                        ghostty_clipboard_request_e request);
  static void WriteClipboardThunk(void* userdata,
                                  ghostty_clipboard_e location,
                                  const ghostty_clipboard_content_s* content,
                                  size_t len,
                                  bool confirm);
  static void CloseSurfaceThunk(void* userdata, bool process_alive);
  static void IoWriteThunk(void* userdata, const char* bytes, uintptr_t len);
  static ghostty_metal_external_frame_disposition_e PresentLeasedThunk(
      void* userdata,
      const ghostty_metal_external_frame_s* frame);

  void Tick();
  void SetSizeNow(uint32_t width_px,
                  uint32_t height_px,
                  float scale_factor,
                  uint64_t geometry_epoch);
  void UpdateThemeNow(std::string theme_config);
  void SetVisibleNow(bool visible, uint64_t visibility_generation);
  void ScheduleTerminalHostViewerRelease();
  void ReleaseTerminalHostViewerIfHidden(uint64_t visibility_generation);
  void CompleteTerminalHostVisibilityRestore();
  void BeginTerminalHostInputCutoverNow(uint64_t cutover_id);
  void AttachTerminalHostNow(mojo::PlatformHandle socket,
                             std::vector<uint8_t> terminal_id,
                             std::vector<uint8_t> terminal_incarnation,
                             uint32_t rights,
                             uint32_t protocol_flags,
                             uint64_t cutover_id,
                             AttachTerminalHostCallback callback);
  void CancelTerminalHostInputCutoverNow(uint64_t cutover_id);
  void DetachTerminalHostNow();
  void PrepareForColdEvictionNow(uint64_t eviction_id);
  void CompleteColdEviction(uint64_t eviction_id,
                            bool success,
                            std::string error);
  void CancelColdEviction(std::string error);
  void RequestTerminalHostViewerSizeNow(uint16_t columns,
                                        uint16_t rows,
                                        float scale_factor,
                                        uint64_t geometry_epoch);
  void ProcessOutputNow(std::vector<uint8_t> bytes);
  void ResetAndReplayAtGridNow(uint16_t columns,
                               uint16_t rows,
                               std::vector<uint8_t> replay,
                               uint64_t geometry_epoch);
  bool QueueSurfaceOperation(PendingSurfaceOperationKind kind,
                             base::OnceClosure action,
                             size_t byte_count);
  bool CanQueueSurfaceOperation(size_t byte_count) const;
  bool QueueSemanticInput(base::OnceClosure action, size_t byte_count);
  void DrainSurfaceOperations();
  void CompleteSurfaceOperationFence();
  void SendKeyNow(mojom::TerminalKeyEventPtr event);
  void SendTextNow(std::string text);
  void PasteNow(std::string text, PasteCallback callback);
  void CopySelectionNow(CopySelectionCallback callback);
  void SetPreeditNow(std::string text);
  void SetFocusNow(bool focused);
  void MouseMoveNow(float x, float y, uint32_t mods);
  void MouseButtonNow(int32_t state,
                      int32_t button,
                      uint32_t mods,
                      MouseButtonCallback callback);
  void MouseScrollNow(float x, float y, int32_t scroll_mods);
  void DeliverInput(std::vector<uint8_t> bytes);
  void DeliverClose(bool process_alive);
  bool QueueFrame(const ghostty_metal_external_frame_s& frame);
  void DeliverPendingFrame();
  void NotifyGridSize(uint32_t width_px,
                      uint32_t height_px,
                      uint64_t geometry_epoch);
  void HandleTerminalHostEvent(uint64_t attempt,
                               TerminalHostRendererEvent event);
  void DrainTerminalHostIngress(
      uint64_t attempt,
      std::shared_ptr<TerminalHostIngress> ingress);
  void HandleTerminalHostEventNow(uint64_t attempt,
                                  TerminalHostRendererEvent event);
  void HandleTerminalHostReady(uint64_t attempt);
  void HandleTerminalHostReadyNow(uint64_t attempt);
  void HandleTerminalHostClosed(uint64_t attempt,
                                TerminalHostSocketClose close);
  void HandleTerminalHostClosedNow(uint64_t attempt,
                                   TerminalHostSocketClose close);
  void MaybeCompleteTerminalHostAttach(uint64_t attempt);
  void CancelTerminalHostInputCutoverInternal(uint64_t cutover_id,
                                              std::string reason,
                                              bool stop_terminal_host);
  void CancelOverflowedTerminalHostInput(std::vector<uint8_t> bytes);
  void StopTerminalHost(bool release_viewer);
  void QueueTerminalHostViewerSize(uint16_t columns,
                                   uint16_t rows,
                                   float scale_factor,
                                   uint64_t geometry_epoch);
  void SendPendingTerminalHostViewerSize();
  void ApplyTerminalHostResizeAck(const TerminalHostResizeAck& ack,
                                  uint64_t request_id);
  void ApplyTerminalHostReplay(uint16_t columns,
                               uint16_t rows,
                               std::vector<uint8_t> replay,
                               std::optional<TerminalHostColors> colors,
                               uint64_t attempt,
                               uint64_t geometry_epoch);
  void PublishTerminalHostBackground(const TerminalHostColors& colors);
  void ReleaseTrackedFrame(uint64_t frame_token);
  void ReleaseAllFrames();
  void EnqueueOutput(std::vector<uint8_t> bytes);
  bool EnqueueAuthoritativeReplay(std::vector<uint8_t> bytes,
                                  uint16_t columns,
                                  uint16_t rows,
                                  uint32_t width_px,
                                  uint32_t height_px,
                                  uint64_t geometry_epoch,
                                  std::optional<TerminalHostColors> colors,
                                  uint64_t terminal_host_attempt);
  bool ReserveOutputBytes(size_t byte_count);
  bool QueueOutputOperation(PendingOutputOperation operation);
  void EnqueueSurfaceOperationFence();
  void DrainOutputOnWorker();
  void ClearOutputQueueAfterWorkerStop();
  void ProcessOutputOnWorker(std::vector<uint8_t> bytes);
  void ProcessAuthoritativeReplayOnWorker(
      std::vector<uint8_t> bytes,
      uint16_t columns,
      uint16_t rows,
      uint32_t width_px,
      uint32_t height_px,
      uint64_t geometry_epoch,
      std::optional<TerminalHostColors> colors,
      uint64_t terminal_host_attempt,
      uint64_t replay_generation);
  void CompleteAuthoritativeReplay(uint16_t columns,
                                   uint16_t rows,
                                   uint32_t width_px,
                                   uint32_t height_px,
                                   uint64_t geometry_epoch,
                                   std::optional<TerminalHostColors> colors,
                                   uint64_t terminal_host_attempt,
                                   uint64_t replay_generation);
  void ReportError(std::string reason);

  mojo::Receiver<mojom::CmuxTerminalRenderer> receiver_;
  mojo::Remote<mojom::CmuxTerminalRendererClient> client_;
  scoped_refptr<base::SequencedTaskRunner> service_task_runner_;
  base::Thread output_thread_{"cmux-ghostty-output"};

  ghostty_app_t app_ = nullptr;
  ghostty_surface_t surface_ = nullptr;
  std::atomic<size_t> queued_output_bytes_{0};
  base::Lock output_queue_lock_;
  CmuxTerminalOutputDrainQueue<PendingOutputOperation> output_queue_
      GUARDED_BY(output_queue_lock_);
  std::atomic<bool> shutting_down_{false};
  CmuxTerminalTickCoalescer tick_coalescer_;
  uint64_t latest_geometry_epoch_ = 0;
  uint64_t authoritative_replay_generation_ = 0;
  bool authoritative_replay_in_flight_ = false;
  bool authoritative_state_ready_ = false;
  bool surface_output_pending_ = false;
  bool surface_operation_fence_in_flight_ = false;
  bool draining_surface_operations_ = false;
  float surface_scale_factor_ = 1.0f;
  float published_surface_scale_factor_ = 1.0f;
  std::deque<PendingSurfaceOperation> pending_surface_operations_;
  std::optional<PendingSurfaceOperation> fenced_surface_operation_;
  size_t pending_surface_operation_bytes_ = 0;

  std::shared_ptr<TerminalHostRendererSocket> terminal_host_socket_;
  std::shared_ptr<TerminalHostIngress> terminal_host_ingress_;
  TerminalHostId terminal_host_id_{};
  TerminalHostIncarnation terminal_host_incarnation_{};
  AttachTerminalHostCallback terminal_host_attach_callback_;
  uint64_t terminal_host_attempt_ = 0;
  bool terminal_host_stream_ready_ = false;
  bool terminal_host_initial_replay_complete_ = false;
  bool terminal_host_exit_reported_ = false;
  bool terminal_host_final_state_ = false;
  bool terminal_host_replay_in_flight_ = false;
  uint64_t terminal_host_close_pending_attempt_ = 0;
  bool terminal_host_close_pending_exited_ = false;
  // Non-zero only while one intentional cold-eviction request owns the exact
  // terminal-host attempt whose orderly socket close will complete it.
  uint64_t cold_eviction_id_ = 0;
  uint64_t cold_eviction_terminal_host_attempt_ = 0;
  bool cold_eviction_release_started_ = false;
  uint64_t terminal_host_geometry_epoch_floor_ = 0;
  bool terminal_host_viewer_size_acks_ = false;
  uint64_t terminal_host_next_resize_request_id_ = 1;
  bool terminal_host_resize_in_flight_ = false;
  uint64_t terminal_host_resize_request_id_ = 0;
  uint64_t terminal_host_resize_epoch_ = 0;
  bool terminal_host_resize_replay_observed_ = false;
  bool terminal_host_resize_pending_ = false;
  uint16_t terminal_host_pending_columns_ = 0;
  uint16_t terminal_host_pending_rows_ = 0;
  float terminal_host_pending_scale_factor_ = 1.0f;
  uint64_t terminal_host_pending_geometry_epoch_ = 0;
  bool terminal_host_has_latest_viewer_size_ = false;
  uint16_t terminal_host_latest_viewer_columns_ = 0;
  uint16_t terminal_host_latest_viewer_rows_ = 0;
  float terminal_host_latest_viewer_scale_factor_ = 1.0f;
  uint64_t terminal_host_latest_viewer_geometry_epoch_ = 0;
  bool terminal_host_viewer_released_ = false;
  bool terminal_host_viewer_size_dirty_ = false;
  bool terminal_host_visibility_restore_pending_ = false;
  uint64_t terminal_host_visibility_generation_ = 0;
  base::OneShotTimer terminal_host_viewer_release_timer_;
  TerminalInputCutover terminal_host_input_cutover_{
      kTerminalHostMaxRendererQueuedWrites / 2};
  bool terminal_host_requested_visible_ = true;
  std::atomic<bool> visible_{true};
  std::optional<std::string> pending_paste_text_;

  base::Lock frame_lock_;
  base::ConditionVariable frame_callbacks_drained_{&frame_lock_};
  size_t active_frame_callbacks_ GUARDED_BY(frame_lock_) = 0;
  std::optional<PendingFrame> pending_frame_ GUARDED_BY(frame_lock_);
  bool frame_task_posted_ GUARDED_BY(frame_lock_) = false;
  bool frame_in_flight_ GUARDED_BY(frame_lock_) = false;
  uint64_t frame_in_flight_sequence_ GUARDED_BY(frame_lock_) = 0;
  uint64_t next_frame_sequence_ GUARDED_BY(frame_lock_) = 0;
  std::unordered_set<uint64_t> outstanding_frame_tokens_
      GUARDED_BY(frame_lock_);

  base::WeakPtr<CmuxTerminalRendererService> weak_this_;
  base::WeakPtrFactory<CmuxTerminalRendererService> weak_factory_{this};
};

}  // namespace cmux

#endif  // CHROME_SERVICES_CMUX_TERMINAL_RENDERER_CMUX_TERMINAL_RENDERER_SERVICE_H_
