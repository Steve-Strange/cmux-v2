// Copyright 2026 Manaflow, Inc.
// Portions derived from Helium; Helium contributors retain copyright.
// SPDX-License-Identifier: GPL-3.0-or-later AND GPL-3.0-only

// Linux + Windows cmux terminal pane. Ghostty renders through manaflow's
// published host-owned OpenGL surface ABI. Cmux owns an offscreen EGL pbuffer
// or hidden WGL drawable, copies each presented RGBA frame, and paints it into
// a views::View through SkBitmap. cmux-tui, not this renderer, owns the PTY and
// authoritative terminal state.

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <memory>
#include <set>
#include <cmath>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/check.h"
#include "base/compiler_specific.h"
#include "base/containers/span.h"
#include "base/functional/bind.h"
#include "base/functional/callback.h"
#include "base/logging.h"
#include "base/memory/ref_counted.h"
#include "base/memory/ref_counted_delete_on_sequence.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/no_destructor.h"
#include "base/sequence_checker.h"
#include "base/strings/string_view_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/synchronization/lock.h"
#include "base/task/single_thread_task_runner.h"
#include "base/timer/timer.h"
#include "build/build_config.h"
#include "cc/paint/paint_flags.h"
#include "chrome/browser/cmux_term/cmux_ghostty_opengl_host.h"
#include "chrome/browser/cmux_term/cmux_layout_config.h"
#include "chrome/browser/cmux_term/cmux_surface.h"
#include "chrome/browser/cmux_term/cmux_terminal_backend.h"
#include "chrome/browser/cmux_term/cmux_tui_protocol.h"
#include "chrome/browser/cmux_term/cmux_views.h"
#include "chrome/browser/ui/color/chrome_color_id.h"
#include "chrome/browser/ui/simple_message_box.h"
#include "ui/base/clipboard/clipboard.h"
#include "ui/base/clipboard/scoped_clipboard_writer.h"
#include "ui/base/ime/input_method.h"
#include "ui/base/ime/text_input_client.h"
#include "ui/base/ime/text_input_flags.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "third_party/skia/include/core/SkImageInfo.h"
#include "third_party/skia/include/core/SkPath.h"
#include "third_party/skia/include/core/SkPixmap.h"
#include "third_party/skia/include/core/SkRRect.h"
#include "ui/accessibility/ax_enums.mojom.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/events/event.h"
#include "ui/events/event_constants.h"
#include "ui/events/keycodes/dom/dom_code.h"
#include "ui/events/keycodes/dom/keycode_converter.h"
#include "ui/color/color_provider.h"
#include "ui/compositor/compositor.h"
#include "ui/gfx/canvas.h"
#include "ui/gfx/geometry/rect_f.h"
#include "ui/gfx/geometry/skia_conversions.h"
#include "ui/gfx/geometry/size_conversions.h"
#include "ui/gfx/image/image_skia.h"
#include "ui/views/accessibility/view_accessibility.h"
#include "ui/views/background.h"
#include "ui/views/border.h"
#include "ui/views/view.h"
#include "ui/views/widget/widget.h"

#if BUILDFLAG(IS_LINUX)
#include "chrome/browser/cmux_term/cmux_theme_ghostty.h"
#endif

#include "third_party/cmux_ghostty/include/ghostty.h"

namespace cmux {

namespace {

static_assert(GHOSTTY_SURFACE_IO_MANUAL_MIRROR == 2);
static_assert(sizeof(ghostty_surface_config_s) == 168);

class CmuxTerminalSurfaceLinux;
void CloseSurfaceCb(void* userdata, bool process_alive);
void IoWriteCb(void* userdata, const char* bytes, uintptr_t len);

SkRRect RoundedFrameRRect(const gfx::RectF& bounds,
                          const RoundedFrameGeometry& geometry,
                          float radius_inset = 0.0f) {
  const auto adjusted_radius = [radius_inset](int radius) {
    return std::max(0.0f, radius - radius_inset);
  };
  const SkVector radii[4] = {
      {adjusted_radius(geometry.top_left_radius),
       adjusted_radius(geometry.top_left_radius)},
      {adjusted_radius(geometry.top_right_radius),
       adjusted_radius(geometry.top_right_radius)},
      {adjusted_radius(geometry.bottom_right_radius),
       adjusted_radius(geometry.bottom_right_radius)},
      {adjusted_radius(geometry.bottom_left_radius),
       adjusted_radius(geometry.bottom_left_radius)}};
  return SkRRect::MakeRectRadii(gfx::RectFToSkRect(bounds), radii);
}

bool IsSafeHexColor(const std::optional<std::string>& color) {
  if (!color || color->size() != 7 || (*color)[0] != '#') {
    return false;
  }
  for (size_t i = 1; i < color->size(); ++i) {
    if (!std::isxdigit(static_cast<unsigned char>((*color)[i]))) {
      return false;
    }
  }
  return true;
}

void AppendColorOsc(int code,
                    const std::optional<std::string>& color,
                    std::string* output) {
  if (!output || !IsSafeHexColor(color)) {
    return;
  }
  output->append("\x1b]");
  output->append(std::to_string(code));
  output->push_back(';');
  output->append(*color);
  output->append("\x1b\\");
}

void AppendPaletteOsc(const CmuxTuiColors& colors, std::string* output) {
  if (!output) {
    return;
  }
  for (const auto& [index, color] : colors.palette) {
    const std::optional<std::string> checked_color(color);
    if (!IsSafeHexColor(checked_color)) {
      continue;
    }
    output->append("\x1b]4;");
    output->append(std::to_string(index));
    output->push_back(';');
    output->append(color);
    output->append("\x1b\\");
  }
}

void AppendColorResetOsc(int code, std::string* output) {
  if (!output) {
    return;
  }
  output->append("\x1b]");
  output->append(std::to_string(code));
  output->append("\x1b\\");
}

struct WakeupTaskRunner {
  explicit WakeupTaskRunner(scoped_refptr<base::SingleThreadTaskRunner> runner)
      : ui_runner(std::move(runner)) {}

  const scoped_refptr<base::SingleThreadTaskRunner> ui_runner;
};

// Renderer-thread frames are large and can arrive faster than Views paints.
// Keep only the newest frame and at most one posted UI task so a stalled UI
// sequence cannot create an unbounded queue of pixel buffers.
// References cross the Ghostty renderer and UI sequences; destruction remains
// on UI because the receiver binds a UI-owned WeakPtr.
class OpenGLFrameMailbox
    : public base::RefCountedDeleteOnSequence<OpenGLFrameMailbox> {
 public:
  using Receiver = base::RepeatingCallback<void(CmuxGhosttyOpenGLFrame)>;

  OpenGLFrameMailbox(scoped_refptr<base::SingleThreadTaskRunner> ui_runner,
                     Receiver receiver)
      : base::RefCountedDeleteOnSequence<OpenGLFrameMailbox>(ui_runner),
        ui_runner_(std::move(ui_runner)),
        receiver_(std::move(receiver)) {}

  void Push(CmuxGhosttyOpenGLFrame frame) {
    bool post_drain = false;
    {
      base::AutoLock lock(lock_);
      if (stopped_) {
        return;
      }
      latest_frame_ = std::move(frame);
      if (!drain_posted_) {
        drain_posted_ = true;
        post_drain = true;
      }
    }
    if (post_drain) {
      ui_runner_->PostTask(
          FROM_HERE, base::BindOnce(&OpenGLFrameMailbox::Drain,
                                    scoped_refptr<OpenGLFrameMailbox>(this)));
    }
  }

  void Stop() {
    base::AutoLock lock(lock_);
    stopped_ = true;
    latest_frame_.reset();
    receiver_.Reset();
  }

 private:
  friend class base::DeleteHelper<OpenGLFrameMailbox>;
  friend class base::RefCountedDeleteOnSequence<OpenGLFrameMailbox>;
  ~OpenGLFrameMailbox() = default;

  void Drain() {
    std::optional<CmuxGhosttyOpenGLFrame> frame;
    Receiver receiver;
    {
      base::AutoLock lock(lock_);
      drain_posted_ = false;
      if (stopped_) {
        latest_frame_.reset();
        return;
      }
      frame = std::move(latest_frame_);
      latest_frame_.reset();
      receiver = receiver_;
    }
    if (frame && receiver) {
      receiver.Run(std::move(*frame));
    }
  }

  const scoped_refptr<base::SingleThreadTaskRunner> ui_runner_;
  base::Lock lock_;
  std::optional<CmuxGhosttyOpenGLFrame> latest_frame_ GUARDED_BY(lock_);
  Receiver receiver_ GUARDED_BY(lock_);
  bool drain_posted_ GUARDED_BY(lock_) = false;
  bool stopped_ GUARDED_BY(lock_) = false;
};

class SharedGhosttyTicker : public base::RefCounted<SharedGhosttyTicker> {
 public:
  void AddPane(CmuxTerminalSurfaceLinux* pane);
  void RemovePane(CmuxTerminalSurfaceLinux* pane);
  void TickNow();
  void SafetyNetTick();

 private:
  friend class base::RefCounted<SharedGhosttyTicker>;

  ~SharedGhosttyTicker() = default;

  bool HasPane(CmuxTerminalSurfaceLinux* pane) const;

  base::RepeatingTimer safety_timer_;
  std::vector<CmuxTerminalSurfaceLinux*> panes_;
  SEQUENCE_CHECKER(sequence_checker_);
};

scoped_refptr<SharedGhosttyTicker> GetSharedGhosttyTicker() {
  static base::NoDestructor<scoped_refptr<SharedGhosttyTicker>> ticker(
      base::MakeRefCounted<SharedGhosttyTicker>());
  return *ticker;
}

void TickSharedGhosttyApp() {
  GetSharedGhosttyTicker()->TickNow();
}

// ---- process-global Ghostty app singleton (renderer/input host) ----
// UI-sequence only; Ghostty background threads reach it by posting through
// runtime.userdata's UI runner cell. cmux-tui, not this app, owns every shell.
ghostty_app_t g_app = nullptr;
std::string& CurrentThemeColors() {
  static base::NoDestructor<std::string> colors;
  return *colors;
}
base::RepeatingCallbackList<void()>& ThemeCallbacks() {
  static base::NoDestructor<base::RepeatingCallbackList<void()>> callbacks;
  return *callbacks;
}

void WakeupCb(void* userdata) {
  // Called from a Ghostty background thread. Hop to the UI thread to tick.
  auto* runner = static_cast<WakeupTaskRunner*>(userdata);
  if (runner && runner->ui_runner) {
    runner->ui_runner->PostTask(FROM_HERE,
                                base::BindOnce(&TickSharedGhosttyApp));
  }
}
bool ReadClipboardCb(void*, ghostty_clipboard_e, void*);
void ConfirmReadClipboardCb(void*, const char*, void*, ghostty_clipboard_request_e);
void WriteClipboardCb(void*, ghostty_clipboard_e,
                      const ghostty_clipboard_content_s*, size_t, bool);
bool ActionCb(ghostty_app_t, ghostty_target_s, ghostty_action_s);

base::SequenceChecker& AppSequenceChecker() {
  // Intentionally leaked: verifies the UI-only Ghostty app singleton without an
  // exit-time destructor.
  static base::SequenceChecker* checker = new base::SequenceChecker();
  return *checker;
}

ghostty_app_t EnsureApp() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(AppSequenceChecker());
  if (g_app) {
    return g_app;
  }
#if BUILDFLAG(IS_LINUX)
  if (!EnsureGhosttyCoreInit()) {
    LOG(ERROR) << "cmux-term-linux: ghostty core init failed";
    return nullptr;
  }
#else
  if (ghostty_init(0, nullptr) != GHOSTTY_SUCCESS) {
    LOG(ERROR) << "cmux-term: ghostty_init failed";
    return nullptr;
  }
#endif
  ghostty_config_t cfg = ghostty_config_new();
  if (!cfg) {
    LOG(ERROR) << "cmux-term: ghostty_config_new failed";
    return nullptr;
  }
  ghostty_config_load_default_files(cfg);
  ghostty_config_load_recursive_files(cfg);
  const std::string theme_name =
      GetPublishedLayoutConfig().value_or(LayoutConfig()).ghostty_theme_name;
  if (!theme_name.empty()) {
    const std::string override = "theme = " + theme_name;
    ghostty_config_load_string(cfg, override.data(), override.size(),
                              "cmux://settings/appearance");
  }
  ghostty_config_finalize(cfg);

  // Intentionally leaked: Ghostty owns only the raw userdata pointer and may
  // call wakeup_cb from its own threads for the process lifetime. Storing the
  // runner before ghostty_app_new publishes it to those threads; that call is
  // the happens-before edge for their later wakeup_cb reads.
  auto* wakeup_runner =
      new WakeupTaskRunner(base::SingleThreadTaskRunner::GetCurrentDefault());

  ghostty_runtime_config_s rt = {};
  rt.userdata = wakeup_runner;
  rt.supports_selection_clipboard = ui::Clipboard::IsSupportedClipboardBuffer(
      ui::ClipboardBuffer::kSelection);
  rt.wakeup_cb = WakeupCb;
  rt.action_cb = ActionCb;
  rt.read_clipboard_cb = ReadClipboardCb;
  rt.confirm_read_clipboard_cb = ConfirmReadClipboardCb;
  rt.write_clipboard_cb = WriteClipboardCb;
  rt.close_surface_cb = CloseSurfaceCb;

  g_app = ghostty_app_new(&rt, cfg);
  ghostty_config_free(cfg);
  if (g_app) {
    ghostty_app_set_focus(g_app, true);
  } else {
    LOG(ERROR) << "cmux-term-linux: ghostty_app_new failed";
  }
  return g_app;
}

ghostty_input_mods_e ModsFromFlags(int flags) {
  int m = GHOSTTY_MODS_NONE;
  if (flags & ui::EF_SHIFT_DOWN)
    m |= GHOSTTY_MODS_SHIFT;
  if (flags & ui::EF_CONTROL_DOWN)
    m |= GHOSTTY_MODS_CTRL;
  if (flags & ui::EF_ALT_DOWN)
    m |= GHOSTTY_MODS_ALT;
  if (flags & ui::EF_COMMAND_DOWN)
    m |= GHOSTTY_MODS_SUPER;
  if (flags & ui::EF_CAPS_LOCK_ON)
    m |= GHOSTTY_MODS_CAPS;
  return static_cast<ghostty_input_mods_e>(m);
}

ghostty_input_mouse_button_e MouseButton(const ui::MouseEvent& e) {
  if (e.IsOnlyRightMouseButton() || (e.flags() & ui::EF_RIGHT_MOUSE_BUTTON))
    return GHOSTTY_MOUSE_RIGHT;
  if (e.flags() & ui::EF_MIDDLE_MOUSE_BUTTON)
    return GHOSTTY_MOUSE_MIDDLE;
  return GHOSTTY_MOUSE_LEFT;
}

class CmuxTerminalSurfaceLinux : public views::View,
                                 public CmuxSurface,
                                 public CmuxTerminalBackend::Frontend,
                                 public ui::TextInputClient {
  METADATA_HEADER(CmuxTerminalSurfaceLinux, views::View)

 public:
  explicit CmuxTerminalSurfaceLinux(scoped_refptr<CmuxTerminalBackend> backend)
      : backend_(std::move(backend)) {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    CHECK(backend_);
    ui_runner_ = base::SingleThreadTaskRunner::GetCurrentDefault();
    SetFocusBehavior(views::View::FocusBehavior::ALWAYS);
    // Focusable views must expose a valid a11y role or the accessibility paint
    // check fatally DCHECKs under dcheck_always_on. A terminal is a text I/O
    // surface, so present it as a text field.
    GetViewAccessibility().SetRole(ax::mojom::Role::kTextField);
    GetViewAccessibility().SetName(u"Terminal",
                                   ax::mojom::NameFrom::kAttribute);
    SetBackground(
        views::CreateSolidBackground(SkColorSetRGB(0x10, 0x12, 0x17)));
    ticker_ = GetSharedGhosttyTicker();
    ReplaceMirror(base::span<const uint8_t>());
    uint16_t cols = 80;
    uint16_t rows = 24;
    if (surface_) {
      const ghostty_surface_size_s size = ghostty_surface_size(surface_);
      cols = std::max<uint16_t>(size.columns, 1);
      rows = std::max<uint16_t>(size.rows, 1);
    }
    backend_attached_ = true;
    backend_->AttachFrontend(this, cols, rows);
    layout_subscription_ = AddLayoutConfigChangedCallback(base::BindRepeating(
        &CmuxTerminalSurfaceLinux::ApplyFontSize, weak_factory_.GetWeakPtr()));
    theme_subscription_ = ThemeCallbacks().Add(base::BindRepeating(
        &CmuxTerminalSurfaceLinux::ApplyThemeColors, weak_factory_.GetWeakPtr()));
    ApplyFontSize(GetPublishedLayoutConfig().value_or(LayoutConfig()));
  }

  CmuxTerminalSurfaceLinux(const CmuxTerminalSurfaceLinux&) = delete;
  CmuxTerminalSurfaceLinux& operator=(const CmuxTerminalSurfaceLinux&) = delete;

  ~CmuxTerminalSurfaceLinux() override {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    if (backend_attached_) {
      backend_->DetachFrontend(this);
      backend_attached_ = false;
    }
    if (GetInputMethod()) {
      GetInputMethod()->DetachTextInputClient(this);
    }
    CancelClipboardRequests();
    mirror_retry_timer_.Stop();
    UnregisterFromTicker();
    StopFrameMailbox();
    if (surface_) {
      CancelClipboardRequests();
      ghostty_surface_free(surface_);
      surface_ = nullptr;
    }
    opengl_host_.reset();
  }

  // ---- CmuxSurface ----
  views::View* AsView() override {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    return this;
  }
  SurfaceKind kind() const override {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    return SurfaceKind::kTerminal;
  }
  void FocusContent() override {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    RequestFocus();
  }
  void SetActivationCallback(base::RepeatingClosure callback) override {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    on_activated_ = std::move(callback);
  }
  void SetTitleChangedCallback(
      base::RepeatingCallback<void(const std::u16string&)> callback) override {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    on_title_changed_ = std::move(callback);
  }
  void SetCloseRequestedCallback(base::RepeatingClosure callback) override {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    on_close_requested_ = std::move(callback);
  }
  void FireCloseRequestedForTesting() override {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    OnGhosttyCloseSurface(false);
  }
  void SetRoundedFrame(const RoundedFrameGeometry& geometry) override {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    if (rounded_frame_geometry_ == geometry) {
      return;
    }
    rounded_frame_geometry_ = geometry;
    SetBorder(
        geometry.enabled
            ? views::CreateEmptyBorder(gfx::Insets::TLBR(
                  geometry.top_inset, geometry.left_inset,
                  geometry.bottom_inset, geometry.right_inset))
            : nullptr);
    SetBackground(
        geometry.enabled
            ? views::CreateSolidBackground(kColorToolbar)
            : views::CreateSolidBackground(SkColorSetRGB(0x10, 0x12, 0x17)));
    InvalidateLayout();
    OnBoundsChanged(bounds());
    SchedulePaint();
  }

  // ---- CmuxTerminalBackend::Frontend ----
  void OnCmuxTerminalReplay(
      uint64_t /*generation*/,
      uint16_t /*cols*/,
      uint16_t /*rows*/,
      base::span<const uint8_t> replay,
      const std::optional<CmuxTuiColors>& colors) override {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    const std::vector<uint8_t> filtered =
        StripCmuxTuiReplayPalette(base::as_string_view(replay));
    ReplayMirror(filtered);
    if (colors) {
      ApplyTerminalColors(*colors);
    }
  }
  void OnCmuxTerminalOutput(base::span<const uint8_t> bytes) override {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    ProcessMirrorOutput(base::as_string_view(bytes));
  }
  void OnCmuxTerminalColors(const CmuxTuiColors& colors) override {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    ApplyTerminalColors(colors);
  }
  void OnCmuxTerminalTitle(const std::string& title) override {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    if (on_title_changed_) {
      on_title_changed_.Run(base::UTF8ToUTF16(title));
    }
  }
  void OnCmuxTerminalPwd(const std::string& pwd) override {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    VLOG(2) << "cmux-term: working directory changed: " << pwd;
  }
  void OnCmuxTerminalBell() override {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  }
  void OnCmuxTerminalExited(const std::string& reason) override {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    VLOG(1) << "cmux-term: TUI process exited: " << reason;
    if (on_title_changed_) {
      on_title_changed_.Run(u"Terminal (exited)");
    }
  }
  void OnCmuxTerminalClosed(const std::string& reason) override {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    VLOG(1) << "cmux-term: TUI surface closed: " << reason;
    if (on_close_requested_) {
      on_close_requested_.Run();
    }
  }
  void BeginCmuxTerminalHostInputCutover(
      uint64_t /*cutover_id*/,
      base::OnceCallback<void(bool success, std::string error)> callback)
      override {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    std::move(callback).Run(false,
                            "direct terminal hosts require the macOS renderer");
  }
  void AttachCmuxTerminalHost(
      uint64_t /*cutover_id*/,
      CmuxTuiRendererConnection /*connection*/,
      base::OnceCallback<void(bool success, std::string error)> callback)
      override {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    std::move(callback).Run(false,
                            "direct terminal hosts require the macOS renderer");
  }
  void CancelCmuxTerminalHostInputCutover(uint64_t /*cutover_id*/,
                                          base::OnceClosure callback) override {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    std::move(callback).Run();
  }
  void DetachCmuxTerminalHost() override {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  }
  void RestartCmuxTerminalRenderer(const std::string& /*reason*/) override {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  }

  // ---- views::View ----
  void AddedToWidget() override {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    UpdateTickerState();
    OnBoundsChanged(bounds());
  }

  void RemovedFromWidget() override {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    UnregisterFromTicker();
  }

  void VisibilityChanged(views::View* starting_from, bool is_visible) override {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    views::View::VisibilityChanged(starting_from, is_visible);
    UpdateTickerState();
  }

  void OnPaint(gfx::Canvas* canvas) override {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    views::View::OnPaint(canvas);
    if (frame_.drawsNothing()) {
      return;
    }
    const gfx::Rect b = GetContentsBounds();
    if (rounded_frame_geometry_.enabled) {
      canvas->Save();
      canvas->ClipPath(
          SkPath::RRect(
              RoundedFrameRRect(gfx::RectF(b), rounded_frame_geometry_)),
          /*do_anti_alias=*/true);
    }
    canvas->DrawImageInt(gfx::ImageSkia::CreateFrom1xBitmap(frame_), 0, 0,
                         frame_.width(), frame_.height(), b.x(), b.y(),
                         b.width(), b.height(), /*filter=*/false);
    if (!rounded_frame_geometry_.enabled) {
      return;
    }
    canvas->Restore();
    cc::PaintFlags outline;
    outline.setAntiAlias(true);
    outline.setStyle(cc::PaintFlags::kStroke_Style);
    outline.setStrokeWidth(kRoundedFrameOutlineThickness);
    outline.setColor(
        GetColorProvider()->GetColor(kColorToolbarContentAreaSeparator));
    gfx::RectF outline_bounds(b);
    const float half_thickness = kRoundedFrameOutlineThickness / 2.0f;
    outline_bounds.Inset(half_thickness);
    canvas->DrawPath(
        SkPath::RRect(RoundedFrameRRect(
            outline_bounds, rounded_frame_geometry_, half_thickness)),
        outline);
  }

  void OnBoundsChanged(const gfx::Rect& previous) override {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    if (!surface_) {
      return;
    }
    const gfx::Rect b = GetContentsBounds();
    if (b.width() <= 0 || b.height() <= 0) {
      return;
    }
    const float scale = ContentScale();
    const gfx::Size pixels = gfx::ScaleToCeiledSize(b.size(), scale);
    ghostty_surface_set_content_scale(surface_, scale, scale);
    opengl_host_->SetSize(static_cast<uint32_t>(pixels.width()),
                          static_cast<uint32_t>(pixels.height()));
    ghostty_surface_set_size(surface_, static_cast<uint32_t>(pixels.width()),
                             static_cast<uint32_t>(pixels.height()));
    const ghostty_surface_size_s size = ghostty_surface_size(surface_);
    VLOG(1) << "cmux-term-geometry: scale=" << scale
            << " logical=" << b.size().ToString()
            << " pixels=" << pixels.ToString()
            << " cells=" << size.columns << "x" << size.rows;
    if (backend_attached_) {
      backend_->UpdateSize(std::max<uint16_t>(size.columns, 1),
                           std::max<uint16_t>(size.rows, 1));
    }
    DrawSurfaceForEvent();
  }

  void OnDeviceScaleFactorChanged(float old_scale, float new_scale) override {
    views::View::OnDeviceScaleFactorChanged(old_scale, new_scale);
    OnBoundsChanged(bounds());
  }

  bool OnMousePressed(const ui::MouseEvent& e) override {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    RequestFocus();
    if (on_activated_) {
      on_activated_.Run();
    }
    if (surface_) {
      const gfx::Rect content = GetContentsBounds();
      if (!content.Contains(e.location())) {
        return true;
      }
      ghostty_surface_mouse_pos(surface_, e.x() - content.x(),
                                e.y() - content.y(),
                                ModsFromFlags(e.flags()));
      ghostty_surface_mouse_button(surface_, GHOSTTY_MOUSE_PRESS,
                                   MouseButton(e), ModsFromFlags(e.flags()));
    }
    return true;
  }
  void OnMouseReleased(const ui::MouseEvent& e) override {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    if (surface_) {
      ghostty_surface_mouse_button(surface_, GHOSTTY_MOUSE_RELEASE,
                                   MouseButton(e), ModsFromFlags(e.flags()));
    }
  }
  void OnMouseMoved(const ui::MouseEvent& e) override {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    if (surface_) {
      const gfx::Rect content = GetContentsBounds();
      if (!content.Contains(e.location())) {
        return;
      }
      ghostty_surface_mouse_pos(surface_, e.x() - content.x(),
                                e.y() - content.y(),
                                ModsFromFlags(e.flags()));
    }
  }
  bool OnMouseDragged(const ui::MouseEvent& e) override {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    if (surface_) {
      const gfx::Rect content = GetContentsBounds();
      if (!content.Contains(e.location())) {
        return true;
      }
      ghostty_surface_mouse_pos(surface_, e.x() - content.x(),
                                e.y() - content.y(),
                                ModsFromFlags(e.flags()));
    }
    return true;
  }
  bool OnMouseWheel(const ui::MouseWheelEvent& e) override {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    if (surface_) {
      ghostty_surface_mouse_scroll(surface_, e.x_offset() / 40.0,
                                   e.y_offset() / 40.0, 0);
    }
    return true;
  }

  void OnKeyEvent(ui::KeyEvent* e) override {
    if (e->key_code() == ui::VKEY_INSERT) {
      VLOG(1) << "cmux-term: insert event code=" << static_cast<int>(e->code())
              << " character=" << static_cast<int>(e->GetCharacter())
              << " shift=" << e->IsShiftDown();
    }
    // ProcessKey is the IME's composition notification, not terminal input.
    // Printable/control characters are delivered once through InsertChar.
    if (e->key_code() == ui::VKEY_PROCESSKEY ||
        (e->type() == ui::EventType::kKeyPressed && e->GetCharacter())) {
      return;
    }
    SendKey(*e);
    e->StopPropagation();
  }

  void SendKey(const ui::KeyEvent& event) {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    if (!surface_) {
      return;
    }
    const ui::KeyEvent* e = &event;
    ghostty_input_key_s k = {};
    k.action =
        e->type() == ui::EventType::kKeyPressed
            ? (e->is_repeat() ? GHOSTTY_ACTION_REPEAT : GHOSTTY_ACTION_PRESS)
            : GHOSTTY_ACTION_RELEASE;
    k.mods = ModsFromFlags(e->flags());
    // ghostty's Linux build matches its keycode table on the XKB keycode;
    // ui::KeyboardCode (VKEY_*) is the wrong space. Convert the W3C DomCode to
    // the native (XKB) keycode, which is exactly what ghostty expects.
    k.keycode = static_cast<uint32_t>(
        ui::KeycodeConverter::DomCodeToNativeKeycode(e->code()));
    std::string text;
    uint32_t unshifted = 0;
    if (k.action != GHOSTTY_ACTION_RELEASE) {
      const char16_t ch = e->GetCharacter();
      if (ch >= 0x20 && ch != 0x7f) {
        text = base::UTF16ToUTF8(std::u16string(1, ch));
      }
      // GetCharacter() folds Ctrl+V to U+0016. Ghostty bindings need the
      // printable key identity, otherwise Ctrl+Shift+C/V never match.
      unshifted = e->GetDomKey().IsCharacter()
                      ? e->GetDomKey().ToCharacter() : ch;
      if (unshifted >= 'A' && unshifted <= 'Z') unshifted += 'a' - 'A';
    }
#if BUILDFLAG(IS_WIN)
    // On Windows the DomCode->native keycode does not line up with ghostty's
    // Windows keycode table, so control keys carrying no printable text
    // (Enter/Tab/Backspace/Escape) never reach the pty. Encode them directly as
    // their control bytes so the terminal is actually usable.
    if (text.empty() && k.action != GHOSTTY_ACTION_RELEASE) {
      switch (e->code()) {
        case ui::DomCode::ENTER:
        case ui::DomCode::NUMPAD_ENTER:
          text = "\r";
          break;
        case ui::DomCode::TAB:
          text = "\t";
          break;
        case ui::DomCode::BACKSPACE:
          text = "\x7f";
          break;
        case ui::DomCode::ESCAPE:
          text = "\x1b";
          break;
        default:
          break;
      }
    }
#endif
    // Shift/CapsLock are "consumed" when they produced the text character, so
    // ghostty inserts the text instead of re-encoding a shifted-key escape
    // sequence. Ctrl/Alt/Super stay active (real terminal modifiers, e.g. ^C).
    k.consumed_mods =
        text.empty() ? GHOSTTY_MODS_NONE
                     : static_cast<ghostty_input_mods_e>(
                           k.mods & (GHOSTTY_MODS_SHIFT | GHOSTTY_MODS_CAPS));
    k.unshifted_codepoint = unshifted;
    k.text = text.c_str();
    k.composing = false;
    ghostty_surface_key(surface_, k);
  }

  void OnFocus() override {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    views::View::OnFocus();
    if (GetInputMethod()) {
      GetInputMethod()->SetFocusedTextInputClient(this);
      NotifyCaretBoundsChanged();
    }
    if (surface_) {
      ghostty_surface_set_focus(surface_, true);
      DrawSurfaceForEvent();
    }
    if (on_activated_) {
      on_activated_.Run();
    }
  }
  void OnBlur() override {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    if (GetInputMethod()) {
      GetInputMethod()->CancelComposition(this);
      GetInputMethod()->DetachTextInputClient(this);
    }
    ClearCompositionText();
    views::View::OnBlur();
    if (surface_) {
      ghostty_surface_set_focus(surface_, false);
      DrawSurfaceForEvent();
    }
  }

  // A terminal has no editable surrounding document: the PTY owns editing.
  // Only preedit and committed text belong to this input-method client.
  base::WeakPtr<ui::TextInputClient> AsWeakPtr() override {
    return weak_factory_.GetWeakPtr();
  }
  void SetCompositionText(const ui::CompositionText& composition) override {
    composition_ = composition.text;
    if (surface_) {
      const std::string text = base::UTF16ToUTF8(composition_);
      ghostty_surface_preedit(surface_, text.data(), text.size());
      DrawSurfaceForEvent();
    }
    NotifyCaretBoundsChanged();
  }
  size_t ConfirmCompositionText(bool) override {
    const std::u16string text = composition_;
    InsertText(text, InsertTextCursorBehavior::kMoveCursorAfterText);
    return text.size();
  }
  void ClearCompositionText() override {
    composition_.clear();
    if (surface_) {
      ghostty_surface_preedit(surface_, nullptr, 0);
      DrawSurfaceForEvent();
    }
  }
  void InsertText(const std::u16string& text, InsertTextCursorBehavior) override {
    ClearCompositionText();
    if (surface_ && !text.empty()) {
      const std::string utf8 = base::UTF16ToUTF8(text);
      // Committed IME text is typing, not bracketed clipboard paste.
      ghostty_surface_text_input(surface_, utf8.data(), utf8.size());
    }
  }
  void InsertChar(const ui::KeyEvent& event) override { SendKey(event); }
  ui::TextInputType GetTextInputType() const override {
    return ui::TEXT_INPUT_TYPE_TEXT;
  }
  ui::TextInputMode GetTextInputMode() const override {
    return ui::TEXT_INPUT_MODE_DEFAULT;
  }
  base::i18n::TextDirection GetTextDirection() const override {
    return base::i18n::LEFT_TO_RIGHT;
  }
  int GetTextInputFlags() const override { return ui::TEXT_INPUT_FLAG_NONE; }
  bool CanComposeInline() const override { return true; }
  gfx::Rect GetCaretBounds() const override {
    double x = 0, y = 0, width = 1, height = 18;
    if (surface_) {
      // Ghostty returns logical coordinates (already divided by content scale).
      ghostty_surface_ime_point(surface_, &x, &y, &width, &height);
    }
    gfx::Rect caret(static_cast<int>(std::floor(x)),
                    static_cast<int>(std::floor(y - height)),
                    std::max(1, static_cast<int>(std::ceil(width))),
                    std::max(1, static_cast<int>(std::ceil(height))));
    caret.Offset(GetContentsBounds().OffsetFromOrigin());
    views::View::ConvertRectToScreen(this, &caret);
    return caret;
  }
  gfx::Rect GetSelectionBoundingBox() const override { return {}; }
  bool GetCompositionCharacterBounds(size_t index, gfx::Rect* rect) const override {
    if (index >= composition_.size()) return false;
    *rect = GetCaretBounds();
    return true;
  }
  bool HasCompositionText() const override { return !composition_.empty(); }
  FocusReason GetFocusReason() const override {
    return HasFocus() ? FOCUS_REASON_OTHER : FOCUS_REASON_NONE;
  }
  bool GetTextRange(gfx::Range*) const override { return false; }
  bool GetCompositionTextRange(gfx::Range*) const override { return false; }
  bool GetEditableSelectionRange(gfx::Range*) const override { return false; }
  bool SetEditableSelectionRange(const gfx::Range&) override { return false; }
  bool GetTextFromRange(const gfx::Range&, std::u16string*) const override {
    return false;
  }
  void OnInputMethodChanged() override { NotifyCaretBoundsChanged(); }
  bool ChangeTextDirectionAndLayoutAlignment(base::i18n::TextDirection) override {
    return false;
  }
  void ExtendSelectionAndDelete(size_t, size_t) override {}
  void EnsureCaretNotInRect(const gfx::Rect&) override {}
  bool IsTextEditCommandEnabled(ui::TextEditCommand) const override { return false; }
  void SetTextEditCommandForNextKeyEvent(ui::TextEditCommand) override {}
  ukm::SourceId GetClientSourceForMetrics() const override {
    return ukm::kInvalidSourceId;
  }
  bool ShouldDoLearning() override { return false; }
  bool SetCompositionFromExistingText(
      const gfx::Range&, const std::vector<ui::ImeTextSpan>&) override {
    return false;
  }
  void GetActiveTextInputControlLayoutBounds(
      std::optional<gfx::Rect>* control, std::optional<gfx::Rect>* selection) override {
    *control = GetBoundsInScreen();
    *selection = GetCaretBounds();
  }
#if BUILDFLAG(IS_WIN)
  std::optional<gfx::Rect> GetProximateCharacterBounds(const gfx::Range&) const override {
    return std::nullopt;
  }
  std::optional<size_t> GetProximateCharacterIndexFromPoint(
      const gfx::Point&, ui::IndexFromPointFlags) const override {
    return std::nullopt;
  }
  void SetActiveCompositionForAccessibility(
      const gfx::Range&, const std::u16string&, bool) override {}
#endif

  bool ReadClipboard(ghostty_clipboard_e clipboard, void* request) {
    if (!surface_) return false;
    const auto buffer = clipboard == GHOSTTY_CLIPBOARD_STANDARD
                            ? ui::ClipboardBuffer::kCopyPaste
                            : ui::ClipboardBuffer::kSelection;
    if (!ui::Clipboard::IsSupportedClipboardBuffer(buffer)) return false;
    VLOG(1) << "cmux-term: clipboard read buffer=" << static_cast<int>(buffer);
    clipboard_requests_.insert(request);
    ui::Clipboard::GetForCurrentThread()->ReadText(
        buffer, std::nullopt,
        base::BindOnce([](base::WeakPtr<CmuxTerminalSurfaceLinux> pane,
                          void* request, std::u16string text) {
          if (pane) pane->CompleteClipboard(request, base::UTF16ToUTF8(text), false);
        }, weak_factory_.GetWeakPtr(), request));
    return true;
  }
  void ConfirmClipboard(const char* text, void* request,
                        ghostty_clipboard_request_e kind) {
    clipboard_requests_.insert(request);
    chrome::ShowQuestionMessageBoxAsync(
        GetWidget() ? GetWidget()->GetNativeWindow() : gfx::NativeWindow(),
        u"Terminal clipboard",
        kind == GHOSTTY_CLIPBOARD_REQUEST_PASTE
            ? u"This paste contains control characters or multiple lines that may execute commands. Paste anyway?"
            : u"Allow the terminal application to access the system clipboard?",
        base::BindOnce([](base::WeakPtr<CmuxTerminalSurfaceLinux> pane,
                          void* request, std::string text, chrome::MessageBoxResult result) {
          if (pane) pane->CompleteClipboard(request,
              result == chrome::MESSAGE_BOX_RESULT_YES ? text : std::string(), true);
        }, weak_factory_.GetWeakPtr(), request, std::string(text ? text : "")));
  }
  void WriteClipboard(ghostty_clipboard_e clipboard, std::string text, bool confirm) {
    const auto buffer = clipboard == GHOSTTY_CLIPBOARD_STANDARD
                            ? ui::ClipboardBuffer::kCopyPaste
                            : ui::ClipboardBuffer::kSelection;
    if (!ui::Clipboard::IsSupportedClipboardBuffer(buffer)) return;
    if (confirm) {
      chrome::ShowQuestionMessageBoxAsync(
          GetWidget() ? GetWidget()->GetNativeWindow() : gfx::NativeWindow(),
          u"Terminal clipboard", u"Allow the terminal application to replace the clipboard?",
          base::BindOnce([](base::WeakPtr<CmuxTerminalSurfaceLinux> pane,
                            ghostty_clipboard_e clipboard, std::string text,
                            chrome::MessageBoxResult result) {
            if (pane && result == chrome::MESSAGE_BOX_RESULT_YES)
              pane->WriteClipboard(clipboard, std::move(text), false);
          }, weak_factory_.GetWeakPtr(), clipboard, std::move(text)));
      return;
    }
    ui::ScopedClipboardWriter writer(buffer);
    writer.WriteText(base::UTF8ToUTF16(text));
  }

 private:
  static constexpr uint32_t kInitW = 800;
  static constexpr uint32_t kInitH = 600;
  static constexpr size_t kMaxPendingMirrorOutputBytes = 16 * 1024 * 1024;

  void ApplyFontSize(const LayoutConfig& config) {
    if (!surface_ || applied_font_size_ == config.terminal_font_size) return;
    applied_font_size_ = config.terminal_font_size;
    const std::string action = config.terminal_font_size == 0
        ? "reset_font_size" : "set_font_size:" + std::to_string(config.terminal_font_size);
    ghostty_surface_binding_action(surface_, action.data(), action.size());
    OnBoundsChanged(bounds());
  }
  void ApplyThemeColors() {
    if (!surface_ || CurrentThemeColors().empty()) return;
    ghostty_config_t config = ghostty_config_new();
    if (!config) return;
    ghostty_config_load_string(config, CurrentThemeColors().data(),
                              CurrentThemeColors().size(), "cmux://settings/appearance");
    ghostty_config_finalize(config);
    ghostty_surface_update_theme_config(surface_, config);
    ghostty_config_free(config);
    DrawSurfaceForEvent();
  }

  void NotifyCaretBoundsChanged() {
    if (HasFocus() && GetInputMethod()) {
      GetInputMethod()->OnCaretBoundsChanged(this);
    }
  }
  void CompleteClipboard(void* request, const std::string& text, bool confirmed) {
    if (surface_ && clipboard_requests_.erase(request)) {
      ghostty_surface_complete_clipboard_request(surface_, text.c_str(), request, confirmed);
    }
  }
  void CancelClipboardRequests() {
    const auto requests = clipboard_requests_;
    for (void* request : requests) CompleteClipboard(request, std::string(), true);
  }

  float ContentScale() const {
    const views::Widget* widget = GetWidget();
    return widget && widget->GetCompositor()
               ? widget->GetCompositor()->device_scale_factor()
               : 1.0f;
  }

  void ReplayMirror(base::span<const uint8_t> replay) {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    if (!surface_) {
      ReplaceMirror(replay);
      return;
    }
    // A snapshot replaces terminal state, not the renderer's lifetime. Reset
    // the existing VT state before replaying it so resize/recovery does not
    // destroy a live Ghostty surface or leave stale screen/scrollback data.
    pending_replay_.clear();
    pending_output_.clear();
    awaiting_replay_ = false;
    static constexpr char kReset[] = "\x1b" "c" "\x1b[3J";
    ghostty_surface_process_output(surface_, kReset, sizeof(kReset) - 1);
    if (!replay.empty()) {
      ghostty_surface_process_output(
          surface_, reinterpret_cast<const char*>(replay.data()), replay.size());
    }
    DrawSurfaceForEvent();
  }

  void ReplaceMirror(base::span<const uint8_t> replay) {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    pending_replay_.assign(replay.begin(), replay.end());
    pending_output_.clear();
    awaiting_replay_ = false;
    CreateMirror();
  }

  void CreateMirror() {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    mirror_retry_timer_.Stop();
    UnregisterFromTicker();
    StopFrameMailbox();
    if (surface_) {
      CancelClipboardRequests();
      ghostty_surface_free(surface_);
      surface_ = nullptr;
    }
    opengl_host_.reset();
    frame_.reset();
    SchedulePaint();

    ghostty_app_t app = EnsureApp();
    if (!app) {
      return;
    }
    const gfx::Rect bounds = GetContentsBounds();
    const float scale = ContentScale();
    const gfx::Size pixels = gfx::ScaleToCeiledSize(
        gfx::Size(bounds.width() > 0 ? bounds.width() : kInitW,
                  bounds.height() > 0 ? bounds.height() : kInitH), scale);
    const uint32_t width = static_cast<uint32_t>(pixels.width());
    const uint32_t height = static_cast<uint32_t>(pixels.height());
    frame_mailbox_ = base::MakeRefCounted<OpenGLFrameMailbox>(
        ui_runner_, base::BindRepeating(&CmuxTerminalSurfaceLinux::OnFrame,
                                        weak_factory_.GetWeakPtr()));
    opengl_host_ = CmuxGhosttyOpenGLHost::Create(
        width, height,
        base::BindRepeating(&OpenGLFrameMailbox::Push, frame_mailbox_));
    if (!opengl_host_) {
      StopFrameMailbox();
      LOG(ERROR) << "cmux-term: unable to initialize the OpenGL host";
      if (!awaiting_replay_) {
        mirror_retry_timer_.Start(
            FROM_HERE, base::Seconds(2),
            base::BindOnce(&CmuxTerminalSurfaceLinux::CreateMirror,
                           weak_factory_.GetWeakPtr()));
      }
      return;
    }
    ghostty_surface_config_s cfg = ghostty_surface_config_new();
    cfg.userdata = this;
    opengl_host_->PopulateSurfaceConfig(&cfg);
    cfg.scale_factor = scale;
    cfg.font_size = 0;
    cfg.context = GHOSTTY_SURFACE_CONTEXT_WINDOW;
    cfg.io_mode = GHOSTTY_SURFACE_IO_MANUAL_MIRROR;
    cfg.io_write_cb = IoWriteCb;
    cfg.io_write_userdata = this;
    surface_ = ghostty_surface_new(app, &cfg);
    if (!surface_) {
      LOG(ERROR) << "cmux-term: manual-mirror ghostty_surface_new failed";
      StopFrameMailbox();
      opengl_host_.reset();
      if (!awaiting_replay_) {
        mirror_retry_timer_.Start(
            FROM_HERE, base::Seconds(2),
            base::BindOnce(&CmuxTerminalSurfaceLinux::CreateMirror,
                           weak_factory_.GetWeakPtr()));
      }
      return;
    }
    ghostty_surface_set_content_scale(surface_, scale, scale);
    ghostty_surface_set_size(surface_, width, height);
    ghostty_surface_set_focus(surface_, HasFocus());
    applied_font_size_ = -1;
    ApplyFontSize(GetPublishedLayoutConfig().value_or(LayoutConfig()));
    ApplyThemeColors();
    if (!pending_replay_.empty()) {
      ghostty_surface_process_output(
          surface_, reinterpret_cast<const char*>(pending_replay_.data()),
          pending_replay_.size());
    }
    if (!pending_output_.empty()) {
      ghostty_surface_process_output(
          surface_, reinterpret_cast<const char*>(pending_output_.data()),
          pending_output_.size());
    }
    pending_replay_.clear();
    pending_output_.clear();
    if (backend_attached_) {
      const ghostty_surface_size_s size = ghostty_surface_size(surface_);
      backend_->UpdateSize(std::max<uint16_t>(size.columns, 1),
                           std::max<uint16_t>(size.rows, 1));
    }
    UpdateTickerState();
    DrawSurfaceForEvent();
  }

  void ProcessMirrorOutput(std::string_view bytes) {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    if (bytes.empty() || awaiting_replay_) {
      return;
    }
    if (surface_) {
      ghostty_surface_process_output(surface_, bytes.data(), bytes.size());
      return;
    }
    if (bytes.size() > kMaxPendingMirrorOutputBytes - pending_output_.size()) {
      pending_replay_.clear();
      pending_output_.clear();
      awaiting_replay_ = true;
      mirror_retry_timer_.Stop();
      backend_->RequestReplay("OpenGL Ghostty pending-output cap exceeded");
      return;
    }
    const auto byte_span = base::as_byte_span(bytes);
    pending_output_.insert(pending_output_.end(), byte_span.begin(),
                           byte_span.end());
  }

  void ApplyTerminalColors(const CmuxTuiColors& colors) {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    std::string metadata;
    // ColorsChanged is a complete sparse state. Clear values omitted by this
    // generation before applying the values that remain.
    AppendColorResetOsc(104, &metadata);
    AppendColorResetOsc(110, &metadata);
    AppendColorResetOsc(111, &metadata);
    AppendColorResetOsc(112, &metadata);
    AppendColorResetOsc(117, &metadata);
    AppendColorResetOsc(119, &metadata);
    AppendPaletteOsc(colors, &metadata);
    AppendColorOsc(10, colors.foreground, &metadata);
    AppendColorOsc(11, colors.background, &metadata);
    AppendColorOsc(12, colors.cursor, &metadata);
    AppendColorOsc(17, colors.selection_background, &metadata);
    AppendColorOsc(19, colors.selection_foreground, &metadata);
    if (colors.cursor_style && colors.cursor_blink) {
      // v1 has no cursor metadata and therefore preserves the raw VT state.
      // v2 resolves both fields and force-replaces the active-screen cursor.
      metadata.append("\x1b[0 q");
      int style = 0;
      if (*colors.cursor_style == "block") {
        style = *colors.cursor_blink ? 1 : 2;
      } else if (*colors.cursor_style == "underline") {
        style = *colors.cursor_blink ? 3 : 4;
      } else if (*colors.cursor_style == "bar") {
        style = *colors.cursor_blink ? 5 : 6;
      }
      if (style != 0) {
        metadata.append("\x1b[");
        metadata.append(std::to_string(style));
        metadata.append(" q");
      }
    }
    ProcessMirrorOutput(metadata);
  }

  void SendInputOnUi(std::vector<uint8_t> bytes) {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    backend_->SendInput(base::span(bytes));
  }

  void OnFrame(CmuxGhosttyOpenGLFrame frame) {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    if (frame.width == 0 || frame.height == 0 ||
        frame.stride < static_cast<size_t>(frame.width) * 4u ||
        frame.pixels.size() <
            static_cast<size_t>(frame.stride) * frame.height) {
      return;
    }
    if (frame_.width() != static_cast<int>(frame.width) ||
        frame_.height() != static_cast<int>(frame.height)) {
      VLOG(1) << "cmux-term-frame: pixels=" << frame.width << "x" << frame.height;
      // N32 (BGRA on little-endian) is required by gfx::ImageSkia.
      frame_.allocPixels(SkImageInfo::Make(
          frame.width, frame.height, kN32_SkColorType, kPremul_SkAlphaType));
    }
    // Flip GL bottom-up rows to top-down and let Skia swizzle RGBA -> N32.
    // SAFETY: the renderer guarantees `data` spans >= stride*height; the
    // SkBitmap row is at least width*4 bytes (allocated above). Span subspans
    // are bounds-checked, and the destination points to a live SkBitmap row.
    const size_t row_bytes = static_cast<size_t>(frame.width) * 4u;
    base::span<const uint8_t> src_all(frame.pixels);
    const SkImageInfo src_row_info = SkImageInfo::Make(
        frame.width, 1, kRGBA_8888_SkColorType, kPremul_SkAlphaType);
    const SkImageInfo dst_row_info = SkImageInfo::Make(
        frame.width, 1, kN32_SkColorType, kPremul_SkAlphaType);
    for (uint32_t y = 0; y < frame.height; ++y) {
      base::span<const uint8_t> src = src_all.subspan(
          static_cast<size_t>(frame.height - 1 - y) * frame.stride, row_bytes);
      SkPixmap src_row(src_row_info, src.data(), frame.stride);
      SkPixmap dst_row(dst_row_info, frame_.getAddr32(0, y), frame_.rowBytes());
      if (!src_row.readPixels(dst_row)) {
        LOG(ERROR) << "cmux-term: failed to convert OpenGL frame";
        return;
      }
    }
    NotifyCaretBoundsChanged();
    SchedulePaint();
  }

  void DrawSurfaceForEvent() {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    if (surface_ && ticker_registered_) {
      ghostty_surface_draw(surface_);
    }
  }

  void StopFrameMailbox() {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    if (frame_mailbox_) {
      frame_mailbox_->Stop();
      frame_mailbox_.reset();
    }
  }

  void UpdateTickerState() {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    if (!ticker_ || !GetWidget() || !IsDrawn()) {
      UnregisterFromTicker();
      return;
    }
    if (ticker_registered_) {
      return;
    }
    ticker_->AddPane(this);
    ticker_registered_ = true;
    ticker_->TickNow();
    DrawSurfaceForEvent();
  }

  void UnregisterFromTicker() {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    if (!ticker_ || !ticker_registered_) {
      return;
    }
    ticker_->RemovePane(this);
    ticker_registered_ = false;
  }

  friend class SharedGhosttyTicker;
  friend bool ActionCb(ghostty_app_t, ghostty_target_s, ghostty_action_s);

  int applied_font_size_ = -1;
  base::CallbackListSubscription layout_subscription_;
  base::CallbackListSubscription theme_subscription_;
  std::u16string composition_;
  std::set<void*> clipboard_requests_;
  ghostty_surface_t surface_ = nullptr;
  std::unique_ptr<CmuxGhosttyOpenGLHost> opengl_host_;
  scoped_refptr<OpenGLFrameMailbox> frame_mailbox_;
  SkBitmap frame_;
  std::vector<uint8_t> pending_replay_;
  std::vector<uint8_t> pending_output_;
  scoped_refptr<SharedGhosttyTicker> ticker_;
  scoped_refptr<base::SingleThreadTaskRunner> ui_runner_;
  base::RepeatingClosure on_activated_;
  base::RepeatingCallback<void(const std::u16string&)> on_title_changed_;
  base::RepeatingClosure on_close_requested_;
  bool ticker_registered_ = false;
  bool backend_attached_ = false;
  bool awaiting_replay_ = false;
  RoundedFrameGeometry rounded_frame_geometry_;
  base::OneShotTimer mirror_retry_timer_;
  SEQUENCE_CHECKER(sequence_checker_);
  const scoped_refptr<CmuxTerminalBackend> backend_;
  base::WeakPtrFactory<CmuxTerminalSurfaceLinux> weak_factory_{this};

  void OnGhosttyCloseSurface(bool process_alive) {
    // close_surface_cb is app-thread-only (see CloseSurfaceCb): enforce the
    // verified contract so a future libghostty rebase that moves it off the
    // app thread fails loudly here instead of racing teardown.
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    if (!ui_runner_) {
      return;
    }
    ui_runner_->PostTask(
        FROM_HERE,
        base::BindOnce(&CmuxTerminalSurfaceLinux::RunCloseRequestedOnUi,
                       weak_factory_.GetWeakPtr(), process_alive));
  }

  void RunCloseRequestedOnUi(bool /*process_alive*/) {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    if (on_close_requested_) {
      on_close_requested_.Run();
    }
  }

  friend void CloseSurfaceCb(void* userdata, bool process_alive);
  friend void IoWriteCb(void* userdata, const char* bytes, uintptr_t len);
};

bool ReadClipboardCb(void* userdata, ghostty_clipboard_e clipboard, void* request) {
  auto* pane = static_cast<CmuxTerminalSurfaceLinux*>(userdata);
  return pane && pane->ReadClipboard(clipboard, request);
}
void ConfirmReadClipboardCb(void* userdata, const char* text, void* request,
                            ghostty_clipboard_request_e kind) {
  if (auto* pane = static_cast<CmuxTerminalSurfaceLinux*>(userdata))
    pane->ConfirmClipboard(text, request, kind);
}
void WriteClipboardCb(void* userdata, ghostty_clipboard_e clipboard,
                      const ghostty_clipboard_content_s* contents, size_t count,
                      bool confirm) {
  auto* pane = static_cast<CmuxTerminalSurfaceLinux*>(userdata);
  if (!pane || !contents) return;
  const auto items = UNSAFE_BUFFERS(base::span(contents, count));
  for (const auto& item : items) {
    if (item.mime && item.data && std::string_view(item.mime).starts_with("text/plain")) {
      pane->WriteClipboard(clipboard, item.data, confirm);
      return;
    }
  }
}

bool ActionCb(ghostty_app_t, ghostty_target_s target, ghostty_action_s action) {
  if (action.tag == GHOSTTY_ACTION_RENDER &&
      target.tag == GHOSTTY_TARGET_SURFACE) {
    auto* pane = static_cast<CmuxTerminalSurfaceLinux*>(
        ghostty_surface_userdata(target.target.surface));
    if (!pane) {
      return false;
    }
    pane->DrawSurfaceForEvent();
    return true;
  }
  // No window/tab/split orchestration is wired to Ghostty actions here.
  // Returning false tells Ghostty the embedder did not handle the action.
  return false;
}

bool SharedGhosttyTicker::HasPane(CmuxTerminalSurfaceLinux* pane) const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  return std::find(panes_.begin(), panes_.end(), pane) != panes_.end();
}

void SharedGhosttyTicker::AddPane(CmuxTerminalSurfaceLinux* pane) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  DCHECK(!HasPane(pane));
  panes_.push_back(pane);
  if (panes_.size() == 1) {
    VLOG(1) << "cmux-term-linux: shared ghostty safety ticker started";
    safety_timer_.Start(FROM_HERE, base::Milliseconds(250), this,
                        &SharedGhosttyTicker::SafetyNetTick);
  }
}

void SharedGhosttyTicker::RemovePane(CmuxTerminalSurfaceLinux* pane) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  auto it = std::find(panes_.begin(), panes_.end(), pane);
  if (it == panes_.end()) {
    DCHECK(false);
    return;
  }
  panes_.erase(it);
  if (panes_.empty()) {
    safety_timer_.Stop();
    VLOG(1) << "cmux-term-linux: shared ghostty safety ticker stopped";
  }
}

void SharedGhosttyTicker::TickNow() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  // The repeating timer only runs while panes_ is non-empty, but Ghostty
  // wakeups can arrive after the last pane is removed. Still tick the shared
  // app so app-mailbox and teardown work drains; the render-action handler's
  // visibility gate prevents drawing then.
  if (g_app) {
    ghostty_app_tick(g_app);
  }
}

void SharedGhosttyTicker::SafetyNetTick() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  TickNow();
  // Linux redraw requests are posted to the app mailbox with `.instant`;
  // Ghostty drops that push if the mailbox is full. The slow safety draw
  // recovers a missed redraw without restoring the old 16 ms cadence.
  std::vector<CmuxTerminalSurfaceLinux*> panes = panes_;
  for (CmuxTerminalSurfaceLinux* pane : panes) {
    if (HasPane(pane)) {
      pane->DrawSurfaceForEvent();
    }
  }
}

BEGIN_METADATA(CmuxTerminalSurfaceLinux)
END_METADATA

void CloseSurfaceCb(void* userdata, bool process_alive) {
  auto* surface = static_cast<CmuxTerminalSurfaceLinux*>(userdata);
  if (!surface) {
    return;
  }
  // Ghostty emits close_surface for both explicit close_surface actions and the
  // post-child-exit key path (Surface.zig closes after the first encoded key
  // following "Process exited...", with process_alive=false). VERIFIED in the
  // fork (2026-07-09): unlike wakeup_cb, this callback only fires on the app
  // thread — every core Surface.close() caller runs under ghostty_app_tick's
  // mailbox drain or ghostty_surface_key, i.e. our UI thread — so touching the
  // surface here is sequence-safe (asserted in OnGhosttyCloseSurface). The
  // post below is a same-thread deferral, not a cross-thread hop: it lets the
  // in-progress ghostty key/tick call unwind before CloseTabDeferred tears the
  // pane down.
  surface->OnGhosttyCloseSurface(process_alive);
}

void IoWriteCb(void* userdata, const char* bytes, uintptr_t len) {
  auto* surface = static_cast<CmuxTerminalSurfaceLinux*>(userdata);
  if (!surface || !bytes || len == 0) {
    return;
  }
  base::span<const char> encoded =
      UNSAFE_BUFFERS(base::span(bytes, static_cast<size_t>(len)));
  surface->SendInputOnUi(std::vector<uint8_t>(encoded.begin(), encoded.end()));
}

}  // namespace

#if BUILDFLAG(IS_LINUX)
void ApplyCmuxLinuxTerminalTheme(const CmuxTheme& theme) {
  CurrentThemeColors() = SerializeGhosttyThemeColors(theme);
  ThemeCallbacks().Notify();
}
#endif

// Linux/Windows definition of the seam declared in cmux_views.h (mac's lives
// in cmux_views_mac.mm). Creates the OpenGL terminal frontend, parents it, and
// retains its independently owned cmux-tui backend.
CmuxSurface* PlatformCreateTerminalSurface(
    views::View* parent,
    scoped_refptr<CmuxTerminalBackend> backend) {
  auto surface = std::make_unique<CmuxTerminalSurfaceLinux>(std::move(backend));
  CmuxSurface* s = surface.get();
  parent->AddChildView(std::move(surface));
  return s;
}

}  // namespace cmux
