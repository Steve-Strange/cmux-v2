// Copyright 2012, 2017 The Chromium Authors
// Copyright 2026 Manaflow, Inc.
// Portions derived from Helium; Helium contributors retain copyright.
// SPDX-License-Identifier: GPL-3.0-or-later AND GPL-3.0-only AND BSD-3-Clause
//
// Contains Chromium-derived regions; see docs/source-provenance.md.

#include "chrome/browser/cmux_term/cmux_views.h"

#include <algorithm>
#include <cstdlib>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/callback_list.h"
#include "base/command_line.h"
#include "base/feature_list.h"
#include "base/files/file_path_watcher.h"
#include "base/functional/bind.h"
#include "base/functional/callback.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/ref_counted.h"
#include "base/memory/weak_ptr.h"
#include "base/metrics/histogram_functions.h"
#include "base/no_destructor.h"
#include "base/scoped_observation.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/stringprintf.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/bind_post_task.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/single_thread_task_runner.h"
#include "base/task/task_traits.h"
#include "base/task/thread_pool.h"
#include "base/threading/sequence_bound.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "base/uuid.h"
#include "build/build_config.h"
#if BUILDFLAG(IS_MAC)
#include "base/mac/mac_util.h"
#endif
#include "cc/paint/paint_flags.h"
#include "chrome/app/vector_icons/vector_icons.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/cmux_term/cmux_browser_finder.h"
#include "chrome/browser/cmux_term/cmux_browser_pane.h"
#include "chrome/browser/cmux_term/cmux_browser_window.h"
#include "chrome/browser/cmux_term/cmux_configure_page.h"
#include "chrome/browser/cmux_term/cmux_demo_page.h"
#include "chrome/browser/cmux_term/cmux_easing.h"
#include "chrome/browser/cmux_term/cmux_extension_strip.h"
#include "chrome/browser/cmux_term/cmux_extensions_container.h"
#include "chrome/browser/cmux_term/cmux_focus.h"
#include "chrome/browser/cmux_term/cmux_ghostty.h"
#include "chrome/browser/cmux_term/cmux_input.h"
#include "chrome/browser/cmux_term/cmux_keymap.h"
#include "chrome/browser/cmux_term/cmux_layout_config.h"
#include "chrome/browser/cmux_term/cmux_native_frame_material.h"
#include "chrome/browser/cmux_term/cmux_pane.h"
#include "chrome/browser/cmux_term/cmux_pane_view.h"
#include "chrome/browser/cmux_term/cmux_rail.h"
#include "chrome/browser/cmux_term/cmux_side_panel.h"
#include "chrome/browser/cmux_term/cmux_sidebar_metrics.h"
#include "chrome/browser/cmux_term/cmux_strip_controller.h"
#include "chrome/browser/cmux_term/cmux_surface.h"
#include "chrome/browser/cmux_term/cmux_tab_drag.h"
#include "chrome/browser/cmux_term/cmux_terminal_backend.h"
#include "chrome/browser/cmux_term/cmux_terminal_placement.h"
#include "chrome/browser/cmux_term/cmux_theme.h"
#include "chrome/browser/cmux_term/cmux_tui_client.h"
#include "chrome/browser/cmux_term/cmux_update_service.h"
#include "chrome/browser/lifetime/application_lifetime_desktop.h"
#include "chrome/browser/lifetime/termination_notification.h"
#include "chrome/common/chrome_version.h"
#if CHROME_VERSION_MAJOR >= 151
#define CMUX_HAS_GLIC 1
#else
#include "chrome/common/buildflags.h"
#if BUILDFLAG(ENABLE_GLIC)
#define CMUX_HAS_GLIC 1
#else
#define CMUX_HAS_GLIC 0
#endif
#endif
#if CMUX_HAS_GLIC
#include "chrome/browser/glic/browser_ui/glic_vector_icon_manager.h"
#include "chrome/browser/glic/public/glic_enabling.h"
#include "chrome/browser/glic/public/glic_keyed_service.h"
#include "chrome/browser/glic/public/glic_keyed_service_factory.h"
#if __has_include(                                                           \
    "chrome/browser/glic/public/service/glic_instance_coordinator.h")
#define CMUX_HAS_GLIC_INSTANCE_COORDINATOR 1
#include "chrome/browser/glic/public/service/glic_instance_coordinator.h"
#else
#define CMUX_HAS_GLIC_INSTANCE_COORDINATOR 0
#endif
#include "chrome/browser/glic/resources/grit/glic_browser_resources.h"
#endif
// NOTE: terminal surfaces (Ghostty NSView / IOSurface compositor) are Obj-C++
// (AppKit) and CANNOT be included from this .cc (C++) even on mac. Each
// platform creates them via the PlatformCreateTerminalSurface() seam
// (cmux_views_mac.mm / cmux_terminal_pane_linux.cc).
#include "chrome/browser/cmux_term/window_layout.h"
#include "chrome/browser/cmux_term/window_model.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/profile_manager.h"
#include "chrome/browser/profiles/profile_observer.h"
#include "chrome/browser/resource_coordinator/tab_lifecycle_unit_external.h"
#include "chrome/browser/send_tab_to_self/send_tab_to_self_util.h"
#include "chrome/browser/themes/theme_service.h"
#include "chrome/browser/themes/theme_service_factory.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_commands.h"
#if CHROME_VERSION_MAJOR >= 150
#include "chrome/browser/ui/browser_window/public/browser_collection_observer.h"
#include "chrome/browser/ui/browser_window/public/global_browser_collection.h"
#else
#include "chrome/browser/ui/browser_list.h"
#include "chrome/browser/ui/browser_list_observer.h"
#endif
#include "chrome/browser/ui/browser_window.h"
#include "chrome/browser/ui/browser_window/public/browser_window_features.h"
#include "chrome/browser/ui/browser_tabstrip.h"
#include "chrome/browser/ui/location_bar/location_bar.h"
#include "chrome/browser/ui/side_panel/side_panel_ui.h"
#include "chrome/browser/ui/color/chrome_color_id.h"
#include "chrome/browser/ui/color/cmux_chrome_surface_colors.h"
#if __has_include(                                                           \
    "chrome/browser/ui/send_tab_to_self/send_tab_to_self_context_menu_delegate.h")
#define CMUX_HAS_SEND_TAB_TO_SELF_CONTEXT_MENU_DELEGATE 1
#include "chrome/browser/ui/send_tab_to_self/send_tab_to_self_context_menu_delegate.h"
#else
#define CMUX_HAS_SEND_TAB_TO_SELF_CONTEXT_MENU_DELEGATE 0
#endif
#include "chrome/browser/ui/tabs/features.h"
#if CMUX_HAS_GLIC
#include "chrome/browser/ui/tabs/glic_tab_sub_menu_model.h"
#endif
#include "chrome/browser/ui/tabs/tab_data.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/tabs/tab_strip_model_delegate.h"
#include "chrome/browser/ui/tabs/tab_strip_model_observer.h"
#include "chrome/browser/ui/tabs/tab_utils.h"
#include "chrome/browser/ui/thumbnails/thumbnail_image.h"
#include "chrome/browser/ui/thumbnails/thumbnail_tab_helper.h"
#include "chrome/browser/ui/ui_features.h"
#include "chrome/browser/user_education/user_education_service.h"
#include "chrome/common/pref_names.h"
#include "chrome/grit/generated_resources.h"
#include "components/keep_alive_registry/keep_alive_types.h"
#include "components/keep_alive_registry/scoped_keep_alive.h"
#include "components/prefs/pref_change_registrar.h"
#include "components/prefs/pref_service.h"
#include "components/tabs/public/tab_interface.h"
#include "components/url_formatter/url_formatter.h"
#include "components/send_tab_to_self/features.h"
#include "components/send_tab_to_self/metrics_util.h"
#include "components/vector_icons/vector_icons.h"
#include "components/web_modal/modal_dialog_host.h"
#include "components/web_modal/web_contents_modal_dialog_host.h"
#include "components/zoom/page_zoom.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/browser_url_handler.h"
#include "content/public/browser/keyboard_event_processing_result.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/navigation_entry.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_observer.h"
#include "content/public/browser/context_menu_params.h"
#include "extensions/buildflags/buildflags.h"
#include "content/public/common/page_zoom.h"
#include "content/public/common/referrer.h"
#include "content/public/common/url_constants.h"
#include "third_party/blink/public/common/input/web_input_event.h"
#include "ui/base/accelerators/accelerator.h"
#include "ui/base/accelerators/accelerator_manager.h"
#include "ui/base/clipboard/clipboard_buffer.h"
#include "ui/base/clipboard/scoped_clipboard_writer.h"
#include "ui/base/cursor/cursor.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/base/models/image_model.h"
#include "ui/base/page_transition_types.h"
#include "ui/base/ui_base_features.h"
#include "ui/color/color_provider.h"
#include "ui/color/color_provider_key.h"
#include "ui/compositor/layer.h"
#include "ui/content_accelerators/accelerator_util.h"
#include "ui/events/event.h"
#include "ui/events/event_constants.h"
#include "ui/events/event_handler.h"
#include "ui/events/keycodes/keyboard_codes.h"
#include "ui/gfx/animation/slide_animation.h"
#include "ui/gfx/animation/tween.h"
#include "ui/gfx/canvas.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/views/animation/bounds_animator.h"
#include "ui/views/animation/bounds_animator_observer.h"
#include "ui/views/background.h"
#include "ui/views/controls/webview/unhandled_keyboard_event_handler.h"
#include "ui/views/focus/focus_manager.h"
#include "ui/views/vector_icons.h"
#include "ui/views/view.h"
#include "ui/views/view_targeter.h"
#include "ui/views/widget/widget.h"
#include "ui/views/widget/widget_delegate.h"
#include "ui/views/widget/widget_observer.h"
#include "url/gurl.h"

#if BUILDFLAG(ENABLE_EXTENSIONS)
#include "chrome/browser/extensions/context_menu_matcher.h"
#include "chrome/browser/extensions/menu_manager.h"
#include "extensions/common/extension_features.h"
#endif

#if BUILDFLAG(ENABLE_EXTENSIONS) && CHROME_VERSION_MAJOR >= 151
#define CMUX_HAS_EXTENSION_TAB_CONTEXT_MENU 1
#else
#define CMUX_HAS_EXTENSION_TAB_CONTEXT_MENU 0
#endif

#if BUILDFLAG(IS_MAC) || BUILDFLAG(IS_LINUX)
#include "chrome/browser/cmux_term/cmux_theme_ghostty.h"
#include "chrome/services/cmux_terminal_renderer/public/cpp/cmux_ghostty_resources.h"
#endif

namespace cmux {

using SidebarMode = sidebar_metrics::SidebarMode;

namespace {

bool CmuxIsMenuSimplificationEnabled() {
#if CHROME_VERSION_MAJOR >= 151
  return features::IsMenuSimplificationEnabled();
#else
  return false;
#endif
}

#if CMUX_HAS_GLIC
bool CmuxIsGlicTabContextMenuEnabled() {
#if CHROME_VERSION_MAJOR >= 151
  return true;
#else
  return base::FeatureList::IsEnabled(features::kGlicMITabContextMenu);
#endif
}
#endif

bool CmuxIsSiteMuted(const TabStripModel& tabs, int index) {
  return ::IsSiteMuted(tabs, index);
}
constexpr int kPaneResizeHandleVisualWidth = 2;
constexpr int kPaneResizeHandleHitWidth = 10;
// Helium applies this exact motion to both vertical-sidebar expansion and
// collapse. See THIRD_PARTY_NOTICES.md and Helium commit 3de6ec1c.
constexpr base::TimeDelta kSidebarExpandCollapseDuration =
    base::Milliseconds(250);
constexpr gfx::Size kWorkspaceHoverCardPreviewSize(252, 141);

int PaddedWindowFrameRadius() {
#if BUILDFLAG(IS_MAC)
  // Helium follows the native window corner: 17 DIPs on macOS 26 and 12 DIPs
  // before it. The content edge is three DIPs inward, so its concentric radius
  // is 14 or 9 respectively.
  const int system_window_radius =
      base::mac::MacOSMajorVersion() >= 26 ? 17 : 12;
#else
  const int system_window_radius = 8;
#endif
  return std::max(0, system_window_radius - kRoundedFrameInset);
}

RoundedFrameGeometry MakeRoundedFrameGeometry(
    bool enabled,
    bool top_chrome_attached,
    bool left_chrome_attached,
    bool right_chrome_attached,
    bool bottom_left_window_corner,
    bool bottom_right_window_corner) {
  RoundedFrameGeometry geometry;
  geometry.enabled = enabled;
  geometry.top_inset = top_chrome_attached ? 0 : kRoundedFrameInset;
  geometry.left_inset = left_chrome_attached ? 0 : kRoundedFrameInset;
  geometry.bottom_inset = kRoundedFrameInset;
  geometry.right_inset = right_chrome_attached ? 0 : kRoundedFrameInset;
  geometry.bottom_left_radius =
      bottom_left_window_corner ? PaddedWindowFrameRadius()
                                : kRoundedFrameCornerRadius;
  geometry.bottom_right_radius =
      bottom_right_window_corner ? PaddedWindowFrameRadius()
                                 : kRoundedFrameCornerRadius;
  return geometry;
}

// Browser-owned half of the cross-platform hover-card preview boundary. The
// ThumbnailImage subscription is the cancellation token: destroying this
// request synchronously unsubscribes before releasing the backing thumbnail.
class CmuxWorkspaceHoverCardPreviewRequest final
    : public WorkspaceHoverCardPreviewRequest {
 public:
  CmuxWorkspaceHoverCardPreviewRequest(
      scoped_refptr<ThumbnailImage> thumbnail,
      base::RepeatingCallback<void(gfx::ImageSkia)> callback)
      : thumbnail_(std::move(thumbnail)) {
    CHECK(thumbnail_);
    subscription_ = thumbnail_->Subscribe();
    CHECK(subscription_);
    subscription_->SetSizeHint(kWorkspaceHoverCardPreviewSize);
    subscription_->SetUncompressedImageCallback(std::move(callback));
    thumbnail_->RequestThumbnailImage();
  }

  CmuxWorkspaceHoverCardPreviewRequest(
      const CmuxWorkspaceHoverCardPreviewRequest&) = delete;
  CmuxWorkspaceHoverCardPreviewRequest& operator=(
      const CmuxWorkspaceHoverCardPreviewRequest&) = delete;
  ~CmuxWorkspaceHoverCardPreviewRequest() override = default;

 private:
  scoped_refptr<ThumbnailImage> thumbnail_;
  std::unique_ptr<ThumbnailImage::Subscription> subscription_;
};

WorkspaceHoverCardPreviewReadiness ToWorkspacePreviewReadiness(
    ThumbnailImage::CaptureReadiness readiness) {
  switch (readiness) {
    case ThumbnailImage::CaptureReadiness::kNotReady:
      return WorkspaceHoverCardPreviewReadiness::kNotReady;
    case ThumbnailImage::CaptureReadiness::kReadyForInitialCapture:
      return WorkspaceHoverCardPreviewReadiness::kReadyForInitialCapture;
    case ThumbnailImage::CaptureReadiness::kReadyForFinalCapture:
      return WorkspaceHoverCardPreviewReadiness::kReadyForFinalCapture;
  }
  return WorkspaceHoverCardPreviewReadiness::kNotReady;
}

struct PaneEntrance {
  std::optional<SplitOrientation> split_orientation;
  bool insert_first = false;
};

enum class GhosttyThemeLoadKind {
  kStartup,
  kReload,
};

// The plain Views window does not retain a Profile. Chromium's hidden startup
// Browser and the process keep-alive bridge that lifetime until the first real
// cmux workspace Browser exists. Only then may startup suppression close the
// bootstrap Browser and release the process bridge.
void ReleaseViewsKeepAlive();
void CompleteViewsStartupAfterWorkspaceBrowserCreated() {
  FinishStartupBrowserSuppression();
  ReleaseViewsKeepAlive();
}

#if BUILDFLAG(IS_MAC) || BUILDFLAG(IS_LINUX)
scoped_refptr<base::SequencedTaskRunner> GhosttyThemeTaskRunner() {
  // libghostty owns process-global configuration state. Keep the resource
  // publication and every config read ordered across all cmux windows while
  // keeping their filesystem work off the UI sequence.
  static base::NoDestructor<scoped_refptr<base::SequencedTaskRunner>> runner(
      base::ThreadPool::CreateSequencedTaskRunner(
          {base::MayBlock(), base::TaskPriority::USER_VISIBLE,
           base::TaskShutdownBehavior::SKIP_ON_SHUTDOWN}));
  return *runner;
}
#endif

std::string NewWorkspaceRegistryKey() {
  return base::Uuid::GenerateRandomV4().AsLowercaseString();
}

std::string NewWorkspaceMutationId() {
  return base::Uuid::GenerateRandomV4().AsLowercaseString();
}

const char* CmuxBrowsingModeName(Profile* profile) {
  return profile->IsGuestSession()
             ? "guest"
             : (profile->IsOffTheRecord() ? "incognito" : "normal");
}

std::optional<TabStripModel::ContextMenuCommand> ChromeContextCommand(
    TabContextAction action) {
  switch (action) {
    case TabContextAction::kAddTabToNewGroup:
      return TabStripModel::CommandAddToNewGroup;
    case TabContextAction::kMoveTabToNewWindow:
      return TabStripModel::CommandMoveTabsToNewWindow;
    case TabContextAction::kCopyUrl:
      return TabStripModel::CommandCopyURL;
    case TabContextAction::kReload:
      return TabStripModel::CommandReload;
    case TabContextAction::kDuplicate:
      return TabStripModel::CommandDuplicate;
    case TabContextAction::kTogglePinned:
      return TabStripModel::CommandTogglePinned;
    case TabContextAction::kToggleSiteMuted:
      return TabStripModel::CommandToggleSiteMuted;
    default:
      return std::nullopt;
  }
}

std::optional<TabContextAction> WorkspaceTabContextAction(
    WorkspaceContextAction action) {
  switch (action) {
    case WorkspaceContextAction::kReload:
      return TabContextAction::kReload;
    case WorkspaceContextAction::kToggleSiteMuted:
      return TabContextAction::kToggleSiteMuted;
    default:
      return std::nullopt;
  }
}

struct DetachedSurface {
  SurfaceTabId tab = kInvalidId;
  std::unique_ptr<views::View> view;
  raw_ptr<CmuxSurface> surface = nullptr;
};

enum class CommandAction {
  kTabNewWeb,
  kTabNewTerminal,
  kTabRestore,
  kTabClose,
  kTabNext,
  kTabPrev,
  kTabJump1,
  kTabJump2,
  kTabJump3,
  kTabJump4,
  kTabJump5,
  kTabJump6,
  kTabJump7,
  kTabJump8,
  kTabJump9,
  kEditUndo,
  kEditRedo,
  kEditCut,
  kEditCopy,
  kEditPaste,
  kEditSelectAll,
  kPaneSplitRight,
  kPaneSplitDown,
  kPaneFocusLeft,
  kPaneFocusRight,
  kPaneFocusUp,
  kPaneFocusDown,
  kColumnNew,
  kColumnCycleWidth,
  kSidebarToggle,
  kOmniboxFocus,
  kOmniboxEscape,
  kPageReload,
  kPageBack,
  kPageForward,
  kZoomIn,
  kZoomOut,
  kZoomReset,
  kDevtoolsToggle,
  kDevtoolsUndock,
  kWorkspaceNext,
  kWorkspacePrev,
  kWorkspaceNew,
  kWorkspaceToggleLayout,
  kWorkspaceJump1,
  kWorkspaceJump2,
  kWorkspaceJump3,
  kWorkspaceJump4,
  kWorkspaceJump5,
  kWorkspaceJump6,
  kWorkspaceJump7,
  kWorkspaceJump8,
  kWorkspaceJump9,
  kKeymapReload,
  kThemeReload,
  kSettingsOpen,
  kWindowNew,
  kWindowClose,
  kWindowToggleFullscreen,
};

struct CommandSpec {
  std::string_view id;
  CommandAction action;
  bool allow_repeat;
};

constexpr CommandSpec kCommandSpecs[] = {
    {"tab.newWeb", CommandAction::kTabNewWeb, false},
    {"tab.newTerminal", CommandAction::kTabNewTerminal, false},
    {"tab.restore", CommandAction::kTabRestore, false},
    // Match normal browser behavior: holding the close shortcut keeps closing
    // tabs as macOS delivers auto-repeat keydown events.
    {"tab.close", CommandAction::kTabClose, true},
    {"tab.next", CommandAction::kTabNext, true},
    {"tab.prev", CommandAction::kTabPrev, true},
    {"tab.jump1", CommandAction::kTabJump1, false},
    {"tab.jump2", CommandAction::kTabJump2, false},
    {"tab.jump3", CommandAction::kTabJump3, false},
    {"tab.jump4", CommandAction::kTabJump4, false},
    {"tab.jump5", CommandAction::kTabJump5, false},
    {"tab.jump6", CommandAction::kTabJump6, false},
    {"tab.jump7", CommandAction::kTabJump7, false},
    {"tab.jump8", CommandAction::kTabJump8, false},
    {"tab.jump9", CommandAction::kTabJump9, false},
    {"edit.undo", CommandAction::kEditUndo, false},
    {"edit.redo", CommandAction::kEditRedo, false},
    {"edit.cut", CommandAction::kEditCut, false},
    {"edit.copy", CommandAction::kEditCopy, false},
    {"edit.paste", CommandAction::kEditPaste, false},
    {"edit.selectAll", CommandAction::kEditSelectAll, false},
    {"pane.splitRight", CommandAction::kPaneSplitRight, false},
    {"pane.splitDown", CommandAction::kPaneSplitDown, false},
    {"pane.focusLeft", CommandAction::kPaneFocusLeft, true},
    {"pane.focusRight", CommandAction::kPaneFocusRight, true},
    {"pane.focusUp", CommandAction::kPaneFocusUp, true},
    {"pane.focusDown", CommandAction::kPaneFocusDown, true},
    {"column.new", CommandAction::kColumnNew, false},
    {"column.cycleWidth", CommandAction::kColumnCycleWidth, false},
    {"sidebar.toggle", CommandAction::kSidebarToggle, false},
    {"omnibox.focus", CommandAction::kOmniboxFocus, false},
    {"omnibox.escape", CommandAction::kOmniboxEscape, false},
    {"page.reload", CommandAction::kPageReload, false},
    {"page.back", CommandAction::kPageBack, false},
    {"page.forward", CommandAction::kPageForward, false},
    {"zoom.in", CommandAction::kZoomIn, false},
    {"zoom.out", CommandAction::kZoomOut, false},
    {"zoom.reset", CommandAction::kZoomReset, false},
    {"devtools.toggle", CommandAction::kDevtoolsToggle, false},
    {"devtools.undock", CommandAction::kDevtoolsUndock, false},
    {"workspace.next", CommandAction::kWorkspaceNext, false},
    {"workspace.prev", CommandAction::kWorkspacePrev, false},
    {"workspace.new", CommandAction::kWorkspaceNew, false},
    {"workspace.toggleLayout", CommandAction::kWorkspaceToggleLayout, false},
    {"workspace.jump1", CommandAction::kWorkspaceJump1, false},
    {"workspace.jump2", CommandAction::kWorkspaceJump2, false},
    {"workspace.jump3", CommandAction::kWorkspaceJump3, false},
    {"workspace.jump4", CommandAction::kWorkspaceJump4, false},
    {"workspace.jump5", CommandAction::kWorkspaceJump5, false},
    {"workspace.jump6", CommandAction::kWorkspaceJump6, false},
    {"workspace.jump7", CommandAction::kWorkspaceJump7, false},
    {"workspace.jump8", CommandAction::kWorkspaceJump8, false},
    {"workspace.jump9", CommandAction::kWorkspaceJump9, false},
    {"keymap.reload", CommandAction::kKeymapReload, false},
    {"theme.reload", CommandAction::kThemeReload, false},
    {"app.settings", CommandAction::kSettingsOpen, false},
    {"window.new", CommandAction::kWindowNew, false},
    {"window.close", CommandAction::kWindowClose, false},
    {"window.toggleFullscreen", CommandAction::kWindowToggleFullscreen, false},
};

const CommandSpec* FindCommandSpec(std::string_view command_id) {
  for (const CommandSpec& spec : kCommandSpecs) {
    if (spec.id == command_id) {
      return &spec;
    }
  }
  return nullptr;
}

bool IsMacKeymap() {
#if BUILDFLAG(IS_MAC)
  return true;
#else
  return false;
#endif
}

struct KeymapFileLoad {
  std::string path;
  KeymapLoadResult result;
};

KeymapFileLoad LoadUserKeymapOnBackgroundSequence() {
  KeymapFileLoad load;
  load.path = CmuxConfigPath();
  load.result = LoadKeymapFile(load.path);
  return load;
}

Keymap BuildConfiguredKeymap(bool is_mac, const KeymapFileLoad& load) {
  Keymap keymap = KeymapFromConfig(is_mac, load.result);
  for (const std::string& warning : load.result.warnings) {
    LOG(WARNING) << "cmux-keymap: " << load.path << ": " << warning;
  }
  if (load.result.file_found) {
    LOG(WARNING) << "cmux-keymap: loaded " << load.result.rules.size()
                 << " user rules from " << load.path;
  }
  return keymap;
}

void ApplyCmuxAppLayoutDefaults(LayoutConfig* config) {
  if (!config) {
    return;
  }
  // cmux-browser's app default is full-bleed panes. The LayoutConfig struct
  // keeps historical host defaults for parser/tests; seed the app-owned config
  // before loading so absent JSON keys inherit the product default.
  config->gap = 0;
  config->margin = 0;
}

SkColor SkColorFromThemeArgb(CmuxArgb color) {
  return SkColorSetARGB(CmuxArgbA(color), CmuxArgbR(color), CmuxArgbG(color),
                        CmuxArgbB(color));
}

std::string ThemeRgbHex(CmuxArgb color) {
  return base::StringPrintf("#%02x%02x%02x", CmuxArgbR(color), CmuxArgbG(color),
                            CmuxArgbB(color));
}

CmuxArgb CmuxArgbFromSkColor(SkColor color) {
  return CmuxArgbSet(SkColorGetA(color), SkColorGetR(color), SkColorGetG(color),
                     SkColorGetB(color));
}

bool ThemeSelfTestEnabled() {
  return getenv("CMUX_THEME_SELFTEST") != nullptr;
}

std::set<views::Widget*>& ChromeSurfaceThemeWidgets() {
  static base::NoDestructor<std::set<views::Widget*>> widgets;
  return *widgets;
}

std::optional<ui::KeyboardCode> KeyboardCodeForKeyToken(std::string_view key) {
  if (key.size() == 1) {
    const char c = key[0];
    if (c >= 'a' && c <= 'z') {
      return static_cast<ui::KeyboardCode>(ui::VKEY_A + (c - 'a'));
    }
    if (c >= '0' && c <= '9') {
      return static_cast<ui::KeyboardCode>(ui::VKEY_0 + (c - '0'));
    }
    switch (c) {
      case '[':
        return ui::VKEY_OEM_4;
      case ']':
        return ui::VKEY_OEM_6;
      case '\\':
        return ui::VKEY_OEM_5;
      case ';':
        return ui::VKEY_OEM_1;
      case '\'':
        return ui::VKEY_OEM_7;
      case ',':
        return ui::VKEY_OEM_COMMA;
      case '.':
        return ui::VKEY_OEM_PERIOD;
      case '/':
        return ui::VKEY_OEM_2;
      case '-':
        return ui::VKEY_OEM_MINUS;
      case '=':
        return ui::VKEY_OEM_PLUS;
      case '`':
        return ui::VKEY_OEM_3;
      default:
        return std::nullopt;
    }
  }
  if (key == "tab") {
    return ui::VKEY_TAB;
  }
  if (key == "escape") {
    return ui::VKEY_ESCAPE;
  }
  if (key == "enter") {
    return ui::VKEY_RETURN;
  }
  if (key == "space") {
    return ui::VKEY_SPACE;
  }
  if (key == "backspace") {
    return ui::VKEY_BACK;
  }
  if (key == "delete") {
    return ui::VKEY_DELETE;
  }
  if (key == "home") {
    return ui::VKEY_HOME;
  }
  if (key == "end") {
    return ui::VKEY_END;
  }
  if (key == "pageup") {
    return ui::VKEY_PRIOR;
  }
  if (key == "pagedown") {
    return ui::VKEY_NEXT;
  }
  if (key == "left") {
    return ui::VKEY_LEFT;
  }
  if (key == "right") {
    return ui::VKEY_RIGHT;
  }
  if (key == "up") {
    return ui::VKEY_UP;
  }
  if (key == "down") {
    return ui::VKEY_DOWN;
  }
  if (key.size() >= 2 && key[0] == 'f') {
    int value = 0;
    for (size_t i = 1; i < key.size(); ++i) {
      if (key[i] < '0' || key[i] > '9') {
        return std::nullopt;
      }
      value = value * 10 + (key[i] - '0');
    }
    if (value >= 1 && value <= 24) {
      return static_cast<ui::KeyboardCode>(ui::VKEY_F1 + value - 1);
    }
  }
  return std::nullopt;
}

std::optional<std::string> KeyTokenForKeyboardCode(ui::KeyboardCode code) {
  if (code >= ui::VKEY_A && code <= ui::VKEY_Z) {
    return std::string(1, static_cast<char>('a' + (code - ui::VKEY_A)));
  }
  if (code >= ui::VKEY_0 && code <= ui::VKEY_9) {
    return std::string(1, static_cast<char>('0' + (code - ui::VKEY_0)));
  }
  switch (code) {
    case ui::VKEY_OEM_4:
      return std::string("[");
    case ui::VKEY_OEM_6:
      return std::string("]");
    case ui::VKEY_OEM_5:
      return std::string("\\");
    case ui::VKEY_OEM_1:
      return std::string(";");
    case ui::VKEY_OEM_7:
      return std::string("'");
    case ui::VKEY_OEM_COMMA:
      return std::string(",");
    case ui::VKEY_OEM_PERIOD:
      return std::string(".");
    case ui::VKEY_OEM_2:
      return std::string("/");
    case ui::VKEY_OEM_MINUS:
      return std::string("-");
    case ui::VKEY_OEM_PLUS:
      return std::string("=");
    case ui::VKEY_OEM_3:
      return std::string("`");
    case ui::VKEY_TAB:
      return std::string("tab");
    case ui::VKEY_ESCAPE:
      return std::string("escape");
    case ui::VKEY_RETURN:
      return std::string("enter");
    case ui::VKEY_SPACE:
      return std::string("space");
    case ui::VKEY_BACK:
      return std::string("backspace");
    case ui::VKEY_DELETE:
      return std::string("delete");
    case ui::VKEY_HOME:
      return std::string("home");
    case ui::VKEY_END:
      return std::string("end");
    case ui::VKEY_PRIOR:
      return std::string("pageup");
    case ui::VKEY_NEXT:
      return std::string("pagedown");
    case ui::VKEY_LEFT:
      return std::string("left");
    case ui::VKEY_RIGHT:
      return std::string("right");
    case ui::VKEY_UP:
      return std::string("up");
    case ui::VKEY_DOWN:
      return std::string("down");
    default:
      if (code >= ui::VKEY_F1 && code <= ui::VKEY_F24) {
        return std::string("f") +
               std::to_string(static_cast<int>(code - ui::VKEY_F1) + 1);
      }
      return std::nullopt;
  }
}

std::optional<ui::Accelerator> AcceleratorForChord(const KeyChord& chord,
                                                   bool is_mac) {
  const int modifiers = ResolveKeyModifiers(chord.modifiers, is_mac);
  if (!is_mac && (modifiers & kKeyModCmd) != 0) {
    return std::nullopt;
  }
  int event_flags = ui::EF_NONE;
  if ((modifiers & kKeyModCmd) != 0) {
    event_flags |= ui::EF_COMMAND_DOWN;
  }
  if ((modifiers & kKeyModCtrl) != 0) {
    event_flags |= ui::EF_CONTROL_DOWN;
  }
  if ((modifiers & kKeyModAlt) != 0) {
    event_flags |= ui::EF_ALT_DOWN;
  }
  if ((modifiers & kKeyModShift) != 0) {
    event_flags |= ui::EF_SHIFT_DOWN;
  }
  std::optional<ui::KeyboardCode> key = KeyboardCodeForKeyToken(chord.key);
  if (!key) {
    return std::nullopt;
  }
  return ui::Accelerator(*key, event_flags);
}

std::optional<KeyChord> ChordFromAccelerator(
    const ui::Accelerator& accelerator) {
  std::optional<std::string> key =
      KeyTokenForKeyboardCode(accelerator.key_code());
  if (!key) {
    return std::nullopt;
  }
  KeyChord chord;
  chord.key = *key;
  if (accelerator.IsCmdDown()) {
    chord.modifiers |= kKeyModCmd;
  }
  if (accelerator.IsCtrlDown()) {
    chord.modifiers |= kKeyModCtrl;
  }
  if (accelerator.IsAltDown()) {
    chord.modifiers |= kKeyModAlt;
  }
  if (accelerator.IsShiftDown()) {
    chord.modifiers |= kKeyModShift;
  }
  return chord;
}

bool IsReservedCommandAction(CommandAction action) {
  switch (action) {
    case CommandAction::kTabNewWeb:
    case CommandAction::kTabRestore:
    case CommandAction::kTabClose:
    case CommandAction::kTabJump1:
    case CommandAction::kTabJump2:
    case CommandAction::kTabJump3:
    case CommandAction::kTabJump4:
    case CommandAction::kTabJump5:
    case CommandAction::kTabJump6:
    case CommandAction::kTabJump7:
    case CommandAction::kTabJump8:
    case CommandAction::kTabJump9:
    case CommandAction::kEditUndo:
    case CommandAction::kEditRedo:
    case CommandAction::kEditCut:
    case CommandAction::kEditCopy:
    case CommandAction::kEditPaste:
    case CommandAction::kEditSelectAll:
    case CommandAction::kZoomIn:
    case CommandAction::kZoomOut:
    case CommandAction::kZoomReset:
    case CommandAction::kWorkspaceNext:
    case CommandAction::kWorkspacePrev:
    case CommandAction::kWorkspaceNew:
    case CommandAction::kWorkspaceJump1:
    case CommandAction::kWorkspaceJump2:
    case CommandAction::kWorkspaceJump3:
    case CommandAction::kWorkspaceJump4:
    case CommandAction::kWorkspaceJump5:
    case CommandAction::kWorkspaceJump6:
    case CommandAction::kWorkspaceJump7:
    case CommandAction::kWorkspaceJump8:
    case CommandAction::kWorkspaceJump9:
    case CommandAction::kWindowClose:
      return true;
    default:
      return false;
  }
}

bool IsReservedCommand(std::string_view command_id) {
  const CommandSpec* spec = FindCommandSpec(command_id);
  return spec && IsReservedCommandAction(spec->action);
}

// BoundsAnimator has no public SetAnimationForView in this Chromium checkout;
// the verified hook is its protected CreateAnimation() factory. Keep the
// bonsplit curve here, where ui/gfx dependencies are already allowed, while
// cmux_easing.{h,cc} remains plain host-compilable math.
class CmuxExpoSlideAnimation : public gfx::SlideAnimation {
 public:
  explicit CmuxExpoSlideAnimation(gfx::AnimationDelegate* delegate)
      : gfx::SlideAnimation(delegate) {}

  double GetCurrentValue() const override {
    return EaseOutExpo(gfx::SlideAnimation::GetCurrentValue());
  }
};

class CmuxExpoBoundsAnimator : public views::BoundsAnimator {
 public:
  explicit CmuxExpoBoundsAnimator(views::View* parent)
      : views::BoundsAnimator(parent) {}

 protected:
  std::unique_ptr<gfx::SlideAnimation> CreateAnimation() override {
    auto animation = std::make_unique<CmuxExpoSlideAnimation>(this);
    animation->SetContainer(container());
    animation->SetSlideDuration(GetAnimationDuration());
    animation->SetTweenType(gfx::Tween::LINEAR);
    return animation;
  }
};

// Workspace adaptation of Helium's vertical TabStripAnimations width motion.
// Keep this separate from CmuxExpoBoundsAnimator so pane motion retains cmux's
// own curve while the whole sidebar uses Helium's exact emphasized easing.
class CmuxSidebarBoundsAnimator : public views::BoundsAnimator {
 public:
  explicit CmuxSidebarBoundsAnimator(views::View* parent)
      : views::BoundsAnimator(parent) {}

 protected:
  std::unique_ptr<gfx::SlideAnimation> CreateAnimation() override {
    auto animation = std::make_unique<gfx::SlideAnimation>(this);
    animation->SetContainer(container());
    animation->SetSlideDuration(GetAnimationDuration());
    animation->SetTweenType(gfx::Tween::EASE_IN_OUT_EMPHASIZED);
    return animation;
  }
};

bool NodeInvariantsHold(const LayoutNode& node,
                        std::set<SurfaceTabId>* seen_tabs) {
  if (node.is_pane()) {
    if (node.pane->tabs.empty()) {
      return false;
    }
    bool selected_found = false;
    for (const SurfaceTab& tab : node.pane->tabs) {
      if (!seen_tabs->insert(tab.id).second) {
        return false;
      }
      selected_found = selected_found || tab.id == node.pane->selected;
    }
    return selected_found;
  }
  return NodeInvariantsHold(node.split->first, seen_tabs) &&
         NodeInvariantsHold(node.split->second, seen_tabs);
}

// The rail itself deliberately remains in the browser-independent
// cmux_chrome_ui target. This per-menu bridge owns Chromium's profile/tab
// dependent submenu models while appending them to the rail's top-level menu
// in TabMenuModel order.
//
// Workspace adaptation of Chromium's GlicTabSubMenuModel. A Chromium tab strip
// can ask its selection model for every selected TabInterface. cmux workspaces
// own separate Browser graphs, so the equivalent selected set must be captured
// across those graphs before the native context menu enters its nested loop.
#if CMUX_HAS_GLIC
class WorkspaceGlicTabSubMenuModel : public ui::SimpleMenuModel,
                                     public ui::SimpleMenuModel::Delegate {
 public:
  WorkspaceGlicTabSubMenuModel(
      Profile* profile,
      std::vector<base::WeakPtr<tabs::TabInterface>> targets)
      : ui::SimpleMenuModel(this),
        profile_(profile),
        targets_(std::move(targets)) {
    glic::GlicKeyedService* service =
        profile_ ? glic::GlicKeyedServiceFactory::GetGlicKeyedService(profile_)
                 : nullptr;
    if (!service) {
      return;
    }

    AddItem(TabStripModel::CommandGlicCreateNewChat,
            l10n_util::GetStringUTF16(IDS_TAB_CXMENU_GLIC_CREATE_NEW_CHAT));

    constexpr size_t kMaxRecentConversations = 10;
    recent_conversations_ =
#if CMUX_HAS_GLIC_INSTANCE_COORDINATOR
#if CHROME_VERSION_MAJOR >= 151
        service->instance_coordinator().GetRecentlyActiveInstances(
            kMaxRecentConversations, base::TimeDelta::Max());
#else
        service->instance_coordinator().GetRecentlyActiveInstances(
            kMaxRecentConversations);
#endif
#else
        service->window_controller().GetRecentlyActiveInstances(
            kMaxRecentConversations);
#endif
    if (!recent_conversations_.empty()) {
      AddSeparator(ui::NORMAL_SEPARATOR);
      for (size_t i = 0; i < recent_conversations_.size(); ++i) {
        AddItem(glic::GlicTabSubMenuModel::kMinRecentConversationCommandId +
                    static_cast<int>(i),
                base::UTF8ToUTF16(recent_conversations_[i].title));
      }
    }
  }

  WorkspaceGlicTabSubMenuModel(const WorkspaceGlicTabSubMenuModel&) = delete;
  WorkspaceGlicTabSubMenuModel& operator=(const WorkspaceGlicTabSubMenuModel&) =
      delete;
  ~WorkspaceGlicTabSubMenuModel() override = default;

  bool IsCommandIdChecked(int) const override { return false; }

  bool IsCommandIdEnabled(int command_id) const override {
    if (command_id == TabStripModel::CommandGlicCreateNewChat ||
        (command_id >=
             glic::GlicTabSubMenuModel::kMinRecentConversationCommandId &&
         command_id <=
             glic::GlicTabSubMenuModel::kMaxRecentConversationCommandId)) {
      return !LiveTabs().empty();
    }
    return false;
  }

  void ExecuteCommand(int command_id, int) override {
    std::vector<tabs::TabInterface*> tabs = LiveTabs();
    glic::GlicKeyedService* service =
        profile_ ? glic::GlicKeyedServiceFactory::GetGlicKeyedService(profile_)
                 : nullptr;
    if (!service || tabs.empty()) {
      return;
    }

    if (command_id == TabStripModel::CommandGlicCreateNewChat) {
      base::UmaHistogramCounts100(
          "Glic.TabContextMenu.PinnedTabsToNewConversation", tabs.size());
#if CMUX_HAS_GLIC_INSTANCE_COORDINATOR
      service->instance_coordinator().CreateNewConversationForTabs(tabs);
#else
      service->window_controller().CreateNewConversationForTabs(tabs);
#endif
      return;
    }

    if (command_id >=
            glic::GlicTabSubMenuModel::kMinRecentConversationCommandId &&
        command_id <=
            glic::GlicTabSubMenuModel::kMaxRecentConversationCommandId) {
      const size_t conversation_index = static_cast<size_t>(
          command_id -
          glic::GlicTabSubMenuModel::kMinRecentConversationCommandId);
      if (conversation_index >= recent_conversations_.size()) {
        return;
      }
      base::UmaHistogramCounts100(
          "Glic.TabContextMenu.PinnedTabsToExistingConversation", tabs.size());
#if CMUX_HAS_GLIC_INSTANCE_COORDINATOR
      service->instance_coordinator().ShowInstanceForTabs(
          tabs, recent_conversations_[conversation_index].instance_id);
#else
      service->window_controller().ShowInstanceForTabs(
          tabs, recent_conversations_[conversation_index].instance_id);
#endif
    }
  }

 private:
  std::vector<tabs::TabInterface*> LiveTabs() const {
    std::vector<tabs::TabInterface*> tabs;
    tabs.reserve(targets_.size());
    for (const base::WeakPtr<tabs::TabInterface>& target : targets_) {
      if (target) {
        tabs.push_back(target.get());
      }
    }
    return tabs;
  }

  raw_ptr<Profile> profile_;
  std::vector<base::WeakPtr<tabs::TabInterface>> targets_;
  std::vector<glic::ConversationInfo> recent_conversations_;
};
#endif

class WorkspaceContextMenuNativeItems {
 public:
  WorkspaceContextMenuNativeItems(
      Browser* browser,
      int context_index,
      int workspace_count,
      std::vector<base::WeakPtr<tabs::TabInterface>> glic_targets,
      ui::SimpleMenuModel* model,
      ui::SimpleMenuModel::Delegate* menu_delegate)
      : browser_(browser ? browser->AsWeakPtr() : base::WeakPtr<Browser>()),
        workspace_count_(std::max(1, workspace_count)),
        glic_targets_(std::move(glic_targets)) {
    TabStripModel* tabs = browser ? browser->tab_strip_model() : nullptr;
    if (!tabs || !tabs->ContainsIndex(context_index)) {
      return;
    }
    tab_interface_ = tabs->GetTabAtIndex(context_index)->GetWeakPtr();
    AppendItems(model, menu_delegate, tabs, context_index);
  }

  WorkspaceContextMenuNativeItems(const WorkspaceContextMenuNativeItems&) =
      delete;
  WorkspaceContextMenuNativeItems& operator=(
      const WorkspaceContextMenuNativeItems&) = delete;
  ~WorkspaceContextMenuNativeItems() = default;

  bool IsCommandEnabled(int command_id) const {
#if CMUX_HAS_EXTENSION_TAB_CONTEXT_MENU
    if (extensions::ContextMenuMatcher::IsExtensionsCustomCommandId(
            command_id)) {
      return extension_items_ &&
             extension_items_->IsCommandIdEnabled(command_id);
    }
#endif
#if CMUX_HAS_GLIC
    if (command_id == Command(TabStripModel::CommandGlicShare)) {
      return HasLiveGlicTarget();
    }
    if (command_id == Command(TabStripModel::CommandGlicUnshare)) {
      return HasPinnedGlicTarget();
    }
#endif
    const std::optional<TabStripModel::ContextMenuCommand> command =
        NativeCommand(command_id);
    TabStripModel* tabs = nullptr;
    int index = TabStripModel::kNoTab;
    return command && ResolveContext(&tabs, &index) &&
           tabs->IsContextMenuCommandEnabled(index, *command);
  }

  bool IsCommandChecked(int command_id) const {
#if CMUX_HAS_EXTENSION_TAB_CONTEXT_MENU
    return extensions::ContextMenuMatcher::IsExtensionsCustomCommandId(
               command_id) &&
           extension_items_ && extension_items_->IsCommandIdChecked(command_id);
#else
    return false;
#endif
  }

  bool IsCommandVisible(int command_id) const {
#if CMUX_HAS_EXTENSION_TAB_CONTEXT_MENU
    if (extensions::ContextMenuMatcher::IsExtensionsCustomCommandId(
            command_id)) {
      return extension_items_ &&
             extension_items_->IsCommandIdVisible(command_id);
    }
#endif
    return NativeCommand(command_id).has_value();
  }

  void ExecuteCommand(int command_id, int event_flags) {
#if CMUX_HAS_GLIC
    if (command_id == Command(TabStripModel::CommandGlicUnshare)) {
      std::vector<tabs::TabHandle> handles;
      handles.reserve(glic_targets_.size());
      Browser* target_browser = nullptr;
      for (const base::WeakPtr<tabs::TabInterface>& target : glic_targets_) {
        if (target) {
          handles.push_back(target->GetHandle());
          if (!target_browser) {
            target_browser = cmux::FindBrowserWithTab(target->GetContents());
          }
        }
      }
      if (!handles.empty() && target_browser) {
        base::UmaHistogramCounts100("Glic.TabContextMenu.UnpinnedTabs",
                                    handles.size());
        auto* delegate = target_browser->tab_strip_model()->delegate();
        delegate->GlicUnpinTabsFromAllConversations(handles);
      }
      return;
    }
#endif
    TabStripModel* tab_strip = nullptr;
    int index = TabStripModel::kNoTab;
    if (!ResolveContext(&tab_strip, &index)) {
      return;
    }
#if CMUX_HAS_EXTENSION_TAB_CONTEXT_MENU
    if (extensions::ContextMenuMatcher::IsExtensionsCustomCommandId(
            command_id)) {
      if (!extension_items_) {
        return;
      }
      content::WebContents* contents = tab_interface_->GetContents();
      if (!contents) {
        return;
      }
      content::ContextMenuParams params;
      params.page_url = contents->GetLastCommittedURL();
      extension_items_->ExecuteCommand(command_id, contents, nullptr, params);
      return;
    }
#endif
    const std::optional<TabStripModel::ContextMenuCommand> command =
        NativeCommand(command_id);
    if (command && tab_strip->IsContextMenuCommandEnabled(index, *command)) {
      tab_strip->ExecuteContextMenuCommand(index, *command);
    }
  }

 private:
  static constexpr int kNativeCommandBase = 20000;
  static constexpr int kTabMenuIconSize = 16;

  static int Command(TabStripModel::ContextMenuCommand command) {
    return kNativeCommandBase + static_cast<int>(command);
  }

  static std::optional<TabStripModel::ContextMenuCommand> NativeCommand(
      int command_id) {
    const int native = command_id - kNativeCommandBase;
    if (native <= static_cast<int>(TabStripModel::CommandFirst) ||
        native >= static_cast<int>(TabStripModel::CommandLast)) {
      return std::nullopt;
    }
    return static_cast<TabStripModel::ContextMenuCommand>(native);
  }

  bool ResolveContext(TabStripModel** tabs, int* index) const {
    Browser* browser = browser_.get();
    tabs::TabInterface* tab = tab_interface_.get();
    if (!browser || !tab) {
      return false;
    }
    TabStripModel* resolved_tabs = browser->tab_strip_model();
    const int resolved_index = resolved_tabs->GetIndexOfTab(tab);
    if (resolved_index == TabStripModel::kNoTab) {
      return false;
    }
    *tabs = resolved_tabs;
    *index = resolved_index;
    return true;
  }

#if CMUX_HAS_GLIC
  glic::GlicKeyedService* GlicServiceForTargets() const {
    for (const base::WeakPtr<tabs::TabInterface>& target : glic_targets_) {
      if (target) {
        return glic::GlicKeyedServiceFactory::GetGlicKeyedService(
            target->GetProfile());
      }
    }
    return nullptr;
  }

  bool HasLiveGlicTarget() const {
    return GlicServiceForTargets() &&
           std::ranges::any_of(glic_targets_,
                               [](const auto& candidate) {
                                 return !!candidate;
                               });
  }

  bool HasPinnedGlicTarget() const {
    auto* service = GlicServiceForTargets();
    return service &&
           std::ranges::any_of(glic_targets_, [&](const auto& candidate) {
             if (!candidate) {
               return false;
             }
#if CMUX_HAS_GLIC_INSTANCE_COORDINATOR
             return service->instance_coordinator().IsTabPinnedToAnyInstance(
                 candidate->GetHandle());
#else
             return service->IsTabPinnedToAnyInstance(candidate->GetHandle());
#endif
           });
  }
#endif

  void AppendItems(ui::SimpleMenuModel* model,
                   ui::SimpleMenuModel::Delegate* menu_delegate,
                   TabStripModel* tabs,
                   int index) {
    CHECK(model);
    CHECK(menu_delegate);

    bool glic_displayed = false;
#if CMUX_HAS_GLIC
    const bool show_glic_items =
        glic::GlicEnabling::IsReadyForProfile(tabs->profile()) &&
        CmuxIsGlicTabContextMenuEnabled() && !glic_targets_.empty();
    if (CmuxIsMenuSimplificationEnabled() && show_glic_items) {
      model->AddSeparator(ui::NORMAL_SEPARATOR);
      AppendGlicItems(model, tabs);
      model->AddSeparator(ui::NORMAL_SEPARATOR);
      glic_displayed = true;
    }
#endif

    const bool display_read_later = tabs->delegate()->SupportsReadLater();
    const std::optional<send_tab_to_self::EntryPointDisplayReason>
        send_tab_to_self_reason = send_tab_to_self::GetEntryPointDisplayReason(
            tabs->GetWebContentsAt(index));
    const bool display_send_to_self = send_tab_to_self_reason.has_value();

    if ((display_read_later || display_send_to_self) && !glic_displayed) {
      model->AddSeparator(ui::NORMAL_SEPARATOR);
    }

    if (display_read_later) {
      model->AddItemWithIcon(
          static_cast<int>(WorkspaceContextAction::kAddToReadLater),
          workspace_count_ == 1 ? u"Add Workspace to Reading List"
                                : u"Add Workspaces to Reading List",
          ui::ImageModel::FromVectorIcon(
#if CHROME_VERSION_MAJOR >= 151
              features::IsRoundedIconsEnabled()
                  ? kMenuBookIcon
                  : kMenuBookChromeRefreshOldIcon,
#else
              kMenuBookChromeRefreshIcon,
#endif
                                         ui::kColorMenuIcon, kTabMenuIconSize));
    }

#if CMUX_HAS_GLIC
    if (show_glic_items && !glic_displayed) {
      AppendGlicItems(model, tabs);
    }
#endif

    if (display_send_to_self) {
      AppendSendTabToSelfItem(model, tabs, index, *send_tab_to_self_reason);
    }

#if CMUX_HAS_EXTENSION_TAB_CONTEXT_MENU
    if (base::FeatureList::IsEnabled(
            extensions_features::kExtensionTabContextMenu)) {
      extension_items_ = std::make_unique<extensions::ContextMenuMatcher>(
          tabs->profile(), menu_delegate, model,
          base::BindRepeating([](const extensions::MenuItem* item) {
            return item->contexts().Contains(extensions::MenuItem::TAB);
          }));
      int extension_index = 0;
      for (const auto& key :
           extensions::MenuManager::Get(tabs->profile())->ExtensionIds()) {
        extension_items_->AppendExtensionItems(key, std::u16string(),
                                               &extension_index,
                                               /*is_action_menu=*/false);
      }
    }
#endif
  }

#if CMUX_HAS_GLIC
  void AppendGlicItems(ui::SimpleMenuModel* model, TabStripModel* tabs) {
    glic_tab_sub_menu_model_ = std::make_unique<WorkspaceGlicTabSubMenuModel>(
        tabs->profile(), glic_targets_);
    const std::u16string label = l10n_util::GetPluralStringFUTF16(
        IDS_TAB_CXMENU_GLIC_START_SHARE,
        static_cast<int>(glic_targets_.size()));
    if (CmuxIsMenuSimplificationEnabled()) {
      model->AddSubMenuWithIcon(Command(TabStripModel::CommandGlicShare), label,
                                glic_tab_sub_menu_model_.get(),
                                ui::ImageModel::FromVectorIcon(
                                    glic::GlicVectorIconManager::GetVectorIcon(
                                        IDR_GLIC_BUTTON_VECTOR_ICON),
                                    ui::kColorMenuIcon, kTabMenuIconSize));
    } else {
      model->AddSubMenu(Command(TabStripModel::CommandGlicShare), label,
                        glic_tab_sub_menu_model_.get());
    }

    if (HasPinnedGlicTarget()) {
      model->AddItem(Command(TabStripModel::CommandGlicUnshare),
                     l10n_util::GetStringUTF16(IDS_TAB_CXMENU_GLIC_UNSHARE));
    }
  }
#endif

  void AppendSendTabToSelfItem(
      ui::SimpleMenuModel* model,
      TabStripModel* tabs,
      int index,
      send_tab_to_self::EntryPointDisplayReason display_reason) {
#if CMUX_HAS_SEND_TAB_TO_SELF_CONTEXT_MENU_DELEGATE
    if (base::FeatureList::IsEnabled(
            send_tab_to_self::kSendTabToSelfEnhancedDesktopUI) &&
        display_reason ==
            send_tab_to_self::EntryPointDisplayReason::kOfferFeature) {
#if CHROME_VERSION_MAJOR >= 151
      send_tab_to_self_submenu_delegate_ =
          std::make_unique<send_tab_to_self::SendTabToSelfContextMenuDelegate>(
              tabs->GetWebContentsAt(index),
              send_tab_to_self::ShareEntryPoint::kTabMenu);
#else
      send_tab_to_self_submenu_delegate_ =
          std::make_unique<send_tab_to_self::SendTabToSelfContextMenuDelegate>(
              tabs->GetWebContentsAt(index));
#endif
      send_tab_to_self_submenu_ = std::make_unique<ui::SimpleMenuModel>(
          send_tab_to_self_submenu_delegate_.get());
      send_tab_to_self_submenu_delegate_->PopulateSubmenu(
          send_tab_to_self_submenu_.get());
#if BUILDFLAG(IS_MAC)
      if (CmuxIsMenuSimplificationEnabled()) {
        model->AddSubMenuWithStringIdAndIcon(
            Command(TabStripModel::CommandSendTabToSelf),
            IDS_MENU_SEND_TAB_TO_SELF, send_tab_to_self_submenu_.get(),
            ui::ImageModel::FromVectorIcon(
#if CHROME_VERSION_MAJOR >= 151
                features::IsRoundedIconsEnabled()
                    ? kDevicesIcon
                    : (features::IsRoundedIconsEnabled()
                           ? vector_icons::kDevicesIcon
                           : kDevicesOldIcon),
#else
                kDevicesIcon,
#endif
                ui::kColorMenuIcon, kTabMenuIconSize));
      } else {
        model->AddSubMenuWithStringId(
            Command(TabStripModel::CommandSendTabToSelf),
            IDS_MENU_SEND_TAB_TO_SELF, send_tab_to_self_submenu_.get());
      }
#else
      model->AddSubMenuWithStringIdAndIcon(
          Command(TabStripModel::CommandSendTabToSelf),
          IDS_MENU_SEND_TAB_TO_SELF, send_tab_to_self_submenu_.get(),
          ui::ImageModel::FromVectorIcon(
#if CHROME_VERSION_MAJOR >= 151
              features::IsRoundedIconsEnabled()
                  ? kDevicesIcon
                  : (features::IsRoundedIconsEnabled()
                         ? vector_icons::kDevicesIcon
                         : kDevicesOldIcon),
#else
              kDevicesIcon,
#endif
              ui::kColorMenuIcon, kTabMenuIconSize));
#endif
      model->SetIsNewFeatureAt(
          model->GetItemCount() - 1,
          UserEducationService::MaybeShowNewBadge(
              tabs->profile(),
              send_tab_to_self::kSendTabToSelfEnhancedDesktopUI));
      return;
    }
#endif

#if BUILDFLAG(IS_MAC)
    if (!CmuxIsMenuSimplificationEnabled()) {
      model->AddItemWithStringId(Command(TabStripModel::CommandSendTabToSelf),
                                 IDS_MENU_SEND_TAB_TO_SELF);
      return;
    }
#endif
    model->AddItemWithIcon(Command(TabStripModel::CommandSendTabToSelf),
                           l10n_util::GetStringUTF16(IDS_MENU_SEND_TAB_TO_SELF),
                           ui::ImageModel::FromVectorIcon(
#if CHROME_VERSION_MAJOR >= 151
                               features::IsRoundedIconsEnabled()
                                   ? kDevicesIcon
                                   : (features::IsRoundedIconsEnabled()
                                          ? vector_icons::kDevicesIcon
                                          : kDevicesOldIcon),
#else
                               kDevicesIcon,
#endif
                               ui::kColorMenuIcon, kTabMenuIconSize));
  }

  base::WeakPtr<Browser> browser_;
  base::WeakPtr<tabs::TabInterface> tab_interface_;
  int workspace_count_ = 1;
  std::vector<base::WeakPtr<tabs::TabInterface>> glic_targets_;
#if CMUX_HAS_GLIC
  std::unique_ptr<ui::SimpleMenuModel> glic_tab_sub_menu_model_;
#endif
#if CMUX_HAS_SEND_TAB_TO_SELF_CONTEXT_MENU_DELEGATE
  std::unique_ptr<ui::SimpleMenuModel> send_tab_to_self_submenu_;
  std::unique_ptr<send_tab_to_self::SendTabToSelfContextMenuDelegate>
      send_tab_to_self_submenu_delegate_;
#endif
#if CMUX_HAS_EXTENSION_TAB_CONTEXT_MENU
  std::unique_ptr<extensions::ContextMenuMatcher> extension_items_;
#endif
};

// The window contents: a configurable-edge workspace rail plus a niri
// scrollable strip of columns, driven by the pure, cross-platform
// WindowModel (window_model.h) and ComputeStripLayout (window_layout.h). Each
// column is a bonsplit tree of panes (split with Cmd/Ctrl-D, Cmd/Ctrl-Shift-D)
// and each pane is a CmuxPaneView: a tab strip over a stack of surfaces (web
// page or terminal), one selected.

class CmuxWindowView;

class WorkspaceResizeHandle : public views::View {
  METADATA_HEADER(WorkspaceResizeHandle, views::View)

 public:
  enum class Target { kColumn, kSplit };

  explicit WorkspaceResizeHandle(CmuxWindowView* owner, Target target)
      : owner_(owner), target_(target) {
    SetPaintToLayer();
    layer()->SetFillsBoundsOpaquely(false);
  }

  void ConfigureColumn(PaneId pane, double fraction, double resize_span) {
    target_ = Target::kColumn;
    pane_ = pane;
    value_ = fraction;
    resize_span_ = resize_span;
  }
  void ConfigureSplit(const DividerBox& divider) {
    target_ = Target::kSplit;
    split_ = divider.split;
    orientation_ = divider.orientation;
    value_ = divider.ratio;
    resize_span_ = divider.resize_span;
  }

  ui::Cursor GetCursor(const ui::MouseEvent& event) override {
    return target_ == Target::kColumn ||
                   orientation_ == SplitOrientation::kHorizontal
               ? ui::mojom::CursorType::kColumnResize
               : ui::mojom::CursorType::kRowResize;
  }

  bool OnMousePressed(const ui::MouseEvent& event) override;
  bool OnMouseDragged(const ui::MouseEvent& event) override;
  void OnMouseReleased(const ui::MouseEvent& event) override;
  void OnMouseCaptureLost() override;

  void OnPaint(gfx::Canvas* canvas) override {
    views::View::OnPaint(canvas);
    cc::PaintFlags flags;
    flags.setAntiAlias(false);
    flags.setColor(SkColorSetA(SK_ColorBLACK, 0x28));
    if (target_ == Target::kColumn ||
        orientation_ == SplitOrientation::kHorizontal) {
      const int x = (width() - kPaneResizeHandleVisualWidth) / 2;
      canvas->DrawRect(gfx::Rect(x, 0, kPaneResizeHandleVisualWidth, height()),
                       flags);
    } else {
      const int y = (height() - kPaneResizeHandleVisualWidth) / 2;
      canvas->DrawRect(gfx::Rect(0, y, width(), kPaneResizeHandleVisualWidth),
                       flags);
    }
  }

 private:
  gfx::Point ScreenPoint(const ui::MouseEvent& event) const {
    gfx::Point screen = event.location();
    ConvertPointToScreen(this, &screen);
    return screen;
  }

  raw_ptr<CmuxWindowView> owner_;
  Target target_;
  PaneId pane_ = kInvalidId;
  SplitId split_ = kInvalidId;
  SplitOrientation orientation_ = SplitOrientation::kHorizontal;
  double value_ = 0.5;
  double resize_span_ = 1.0;
  gfx::Point press_screen_;
  double start_value_ = 0.5;
  double start_resize_span_ = 1.0;
};

class RailResizeHandle : public views::View {
  METADATA_HEADER(RailResizeHandle, views::View)

 public:
  explicit RailResizeHandle(CmuxWindowView* owner) : owner_(owner) {
    SetPreferredSize(gfx::Size(sidebar_metrics::kExpandedResizeAreaWidth,
                               sidebar_metrics::kExpandedResizeAreaWidth));
    // Match Helium's plain views::ResizeArea: the edge is an invisible input
    // target, not a persistent divider. Its 5/2-DIP mode-specific bounds and
    // column-resize cursor provide the affordance without adding visual chrome.
  }

  ui::Cursor GetCursor(const ui::MouseEvent& event) override {
    return ui::mojom::CursorType::kColumnResize;
  }

  // Defined below CmuxWindowView (they need its full type).
  bool OnMousePressed(const ui::MouseEvent& event) override;
  bool OnMouseDragged(const ui::MouseEvent& event) override;
  void OnMouseReleased(const ui::MouseEvent& event) override;
  void OnMouseCaptureLost() override;

 private:
  int ScreenDeltaX(const ui::MouseEvent& event) const {
    gfx::Point screen = event.location();
    ConvertPointToScreen(this, &screen);
    return screen.x() - press_screen_.x();
  }

  raw_ptr<CmuxWindowView> owner_;
  gfx::Point press_screen_;
  int start_width_ = 0;
  bool resized_ = false;
};

// A tab close can wait on beforeunload after CloseWebContents() returns. Keep
// the pane-model selection guard alive until TabStripModel reports removal, or
// notify the owner on a later task if the user cancels the close.
class PendingFocusedWebTabCloseObserver
    : public content::WebContentsObserver {
 public:
  PendingFocusedWebTabCloseObserver(content::WebContents* contents,
                                    base::OnceClosure cancelled_callback)
      : content::WebContentsObserver(contents),
        cancelled_callback_(std::move(cancelled_callback)) {}
  PendingFocusedWebTabCloseObserver(
      const PendingFocusedWebTabCloseObserver&) = delete;
  PendingFocusedWebTabCloseObserver& operator=(
      const PendingFocusedWebTabCloseObserver&) = delete;
  ~PendingFocusedWebTabCloseObserver() override = default;

  void BeforeUnloadFired(bool proceed) override {
    if (!proceed) {
      NotifyCancelled();
    }
  }

  void BeforeUnloadDialogCancelled() override { NotifyCancelled(); }

 private:
  void NotifyCancelled() {
    if (!cancelled_callback_) {
      return;
    }
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, std::move(cancelled_callback_));
  }

  base::OnceClosure cancelled_callback_;
};

// A fixed window-space mask for the portion of pane chrome underneath macOS's
// traffic lights. Panes and their tabs remain free to translate with the niri
// strip; this sibling covers and intercepts only the unsafe native-control
// region instead of counter-translating tab contents inside each pane.
class WindowControlsOcclusionView : public views::View {
  METADATA_HEADER(WindowControlsOcclusionView, views::View)

 public:
  explicit WindowControlsOcclusionView(RailDelegate* delegate)
      : delegate_(delegate) {
    SetPaintToLayer();
    layer()->SetFillsBoundsOpaquely(false);
    SetVisible(false);
  }

  void SetColor(SkColor color) {
    if (color_ == color) {
      return;
    }
    color_ = color;
    if (SkColorGetA(color_) == 0) {
      SetBackground(nullptr);
    } else {
      SetBackground(views::CreateSolidBackground(color_));
    }
  }

  bool OnMousePressed(const ui::MouseEvent& event) override {
    if (!event.IsOnlyLeftMouseButton()) {
      return false;
    }
    if (delegate_) {
      delegate_->OnBeginWindowDrag(event);
    }
    return true;
  }

  bool OnMouseDragged(const ui::MouseEvent&) override { return true; }
  void OnMouseReleased(const ui::MouseEvent&) override {}

 private:
  raw_ptr<RailDelegate> delegate_;
  SkColor color_ = SK_ColorTRANSPARENT;
};

class CmuxWindowView : public views::View,
                       public views::ViewTargeterDelegate,
                       public views::FocusChangeListener,
                       public views::BoundsAnimatorObserver,
                       public views::WidgetObserver,
                       public CmuxStripController,
                       public RailDelegate,
                       public FocusHost,
                       public TabDragHost,
                       public CmuxPaneView::Delegate,
                       public CmuxBrowserWindowHost,
                       public CmuxTuiClient::Observer,
                       public CmuxUpdateObserver,
#if CHROME_VERSION_MAJOR >= 150
                       public BrowserCollectionObserver,
#else
                       public BrowserListObserver,
#endif
                       public TabStripModelObserver,
                       public ProfileObserver {
  METADATA_HEADER(CmuxWindowView, views::View)

  // Drag handle needs the private rail-resize plumbing.
  friend class RailResizeHandle;
  friend class WorkspaceResizeHandle;

  struct WebTabPlacement {
    WorkspaceId workspace = kInvalidId;
    PaneId pane = kInvalidId;
    SurfaceTabId surface = kInvalidId;
  };

  struct TuiTerminalFocusIntent {
    std::string workspace_key;
    std::string terminal_id;
  };

  class TuiFocusInteractionEventHandler : public ui::EventHandler {
   public:
    explicit TuiFocusInteractionEventHandler(CmuxWindowView* owner)
        : owner_(owner) {}
    TuiFocusInteractionEventHandler(const TuiFocusInteractionEventHandler&) =
        delete;
    TuiFocusInteractionEventHandler& operator=(
        const TuiFocusInteractionEventHandler&) = delete;
    ~TuiFocusInteractionEventHandler() override = default;

    void OnMouseEvent(ui::MouseEvent* event) override {
      if (owner_ && event && event->type() == ui::EventType::kMousePressed) {
        owner_->FencePendingTuiTerminalFocusForGuiInteraction();
      }
    }

    void OnKeyEvent(ui::KeyEvent* event) override {
      if (owner_ && event && event->type() == ui::EventType::kKeyPressed) {
        owner_->FencePendingTuiTerminalFocusForGuiInteraction();
      }
    }

    void OnTouchEvent(ui::TouchEvent* event) override {
      if (owner_ && event && event->type() == ui::EventType::kTouchPressed) {
        owner_->FencePendingTuiTerminalFocusForGuiInteraction();
      }
    }

   private:
    raw_ptr<CmuxWindowView> owner_;
  };

  class StripWheelEventHandler : public ui::EventHandler {
   public:
    explicit StripWheelEventHandler(CmuxWindowView* owner) : owner_(owner) {}
    StripWheelEventHandler(const StripWheelEventHandler&) = delete;
    StripWheelEventHandler& operator=(const StripWheelEventHandler&) = delete;
    ~StripWheelEventHandler() override = default;

    void OnMouseEvent(ui::MouseEvent* event) override {
      if (!event || event->type() != ui::EventType::kMousewheel || !owner_) {
        return;
      }
      if (owner_->HandleStripMouseWheel(*event->AsMouseWheelEvent())) {
        event->SetHandled();
      }
    }

   private:
    raw_ptr<CmuxWindowView> owner_;
  };

 public:
  CmuxWindowView(Profile* profile,
                 NativeWindowId native_window,
                 std::string window_group,
                 CmuxNativeWindowRegistry* native_window_registry)
      : profile_(profile),
        native_window_(native_window),
        window_group_(std::move(window_group)),
        native_window_registry_(native_window_registry),
        keymap_(DefaultKeymap(IsMacKeymap())) {
    CHECK_NE(native_window_, kInvalidNativeWindowId);
    CHECK(native_window_registry_);
    profile_observation_.Observe(profile_);
    rounded_frame_pref_change_registrar_.Init(profile_->GetPrefs());
    rounded_frame_pref_change_registrar_.Add(
        prefs::kHeliumRoundedFrame,
        base::BindRepeating(&CmuxWindowView::OnRoundedFramePreferenceChanged,
                            base::Unretained(this)));
    const CmuxFrontendIdentity frontend_identity = BuildCmuxFrontendIdentity(
        profile_->GetOriginalProfile()->GetPath().AsUTF8Unsafe(),
        CmuxBrowsingModeName(profile_), window_group_);
    tui_client_ = base::MakeRefCounted<CmuxTuiClient>(
        CmuxTuiClient::Options::FromEnvironment(frontend_identity));
    tui_client_->AddObserver(this);
    observing_tui_client_ = true;
    ApplyCmuxAppLayoutDefaults(&layout_config_);
    const uint64_t startup_theme_request = ++ghostty_theme_load_request_;
    LoadLayoutConfigAsync(
        layout_config_,
        base::BindOnce(&CmuxWindowView::OnStartupLayoutConfigLoaded,
                       weak_factory_.GetWeakPtr(), startup_theme_request));
    LoadBrowserConfigAsync(
        base::BindOnce(&CmuxWindowView::OnStartupBrowserConfigLoaded,
                       weak_factory_.GetWeakPtr()));
    SetBackground(views::CreateSolidBackground(window_bg_));
    // The niri strip lives in a content container beside the rail that CLIPS
    // its panes -- including their native web/terminal NSViews, which otherwise
    // composite above every Views layer -- to its own bounds. That keeps a
    // scrolled column from ever painting over the rail, so the rail stays on
    // top. Panes are children of content_; the animator drives content_.
    content_ = AddChildView(std::make_unique<views::View>());
    // INVARIANT for every SetPaintToLayer() view: paint your full bounds, or
    // mark the layer SetFillsBoundsOpaquely(false). content_'s layer defaults
    // to opaque, so the compositor never draws what's behind it; any region no
    // pane covers (column gutters, top/bottom margins, areas a pane animates
    // away from) would otherwise be UNDEFINED pixels -- rendered as the
    // platform's "safe" fill (white on macOS, black on Windows) and littered
    // with stale fragments (focus-border trails, dead omnibox-popup remnants)
    // that nothing ever repaints. Non-material platforms paint this layer
    // opaquely. macOS instead marks it non-opaque and lets the native frame
    // material define those pixels.
    content_->SetBackground(views::CreateSolidBackground(content_bg_));
    content_->SetPaintToLayer();
    content_->layer()->SetMasksToBounds(true);
    strip_wheel_handler_ = std::make_unique<StripWheelEventHandler>(this);
    content_->AddPreTargetHandler(strip_wheel_handler_.get());
    tui_focus_interaction_handler_ =
        std::make_unique<TuiFocusInteractionEventHandler>(this);
    AddPreTargetHandler(tui_focus_interaction_handler_.get());
    animator_ = std::make_unique<CmuxExpoBoundsAnimator>(content_);
    drag_controller_ = std::make_unique<CmuxTabDragController>(this);
    // Keep this above every pane layer but below the rail and its resize
    // handle. The native traffic-light buttons remain AppKit-owned above it.
    window_controls_occlusion_ =
        AddChildView(std::make_unique<WindowControlsOcclusionView>(this));
    window_controls_occlusion_->SetColor(content_bg_);
    // Vertical-tab tree rail, added last so it's above content_ regardless of
    // which edge the layout config places it on.
    rail_ = AddChildView(std::make_unique<CmuxRail>(this));
    AddCmuxUpdateObserver(this);
    base_rail_config_ = rail_->config();
    rail_->SetPaintToLayer();
    rail_->layer()->SetFillsBoundsOpaquely(false);
    rail_resize_handle_ =
        AddChildView(std::make_unique<RailResizeHandle>(this));
    // Native pane surfaces and the layer-backed rail meet at this exact edge.
    // Route that point explicitly so the narrow Helium resize area wins hit
    // testing without widening its 5-DIP expanded / 2-DIP icon geometry.
    SetEventTargeter(std::make_unique<views::ViewTargeter>(this));
    column_resize_handle_ =
        content_->AddChildView(std::make_unique<WorkspaceResizeHandle>(
            this, WorkspaceResizeHandle::Target::kColumn));
    column_resize_handle_->SetVisible(false);
    root_animator_ = std::make_unique<CmuxSidebarBoundsAnimator>(this);
    root_animator_->AddObserver(this);
    ApplyAnimationConfigToAnimators();
    ApplyRailLayoutConfig();
    // Do not seed a second workspace registry in the GUI. The first
    // authoritative cmux snapshot materializes its UUIDs below; an empty
    // registry causes one optimistic Home create with a fresh canonical UUID.
    RegisterCmuxBrowserWindowFactoryHost(weak_factory_.GetWeakPtr());
#if CHROME_VERSION_MAJOR >= 150
    browser_collection_observation_.Observe(
        GlobalBrowserCollection::GetInstance());
#else
    BrowserList::AddObserver(this);
#endif
    RefreshRail();
    LoadKeymapAsync();
    tui_client_->Start(base::BindOnce(
        [](base::WeakPtr<CmuxWindowView> window, bool ready,
           const std::string& error) {
          if (!window) {
            return;
          }
          if (ready) {
            VLOG(1) << "cmux-views: cmux-tui backend ready";
          } else {
            LOG(ERROR) << "cmux-views: cmux-tui backend unavailable: " << error;
          }
        },
        weak_factory_.GetWeakPtr()));
    StartKeymapWatcher();
  }
  ~CmuxWindowView() override {
    if (observing_tui_client_) {
      tui_client_->RemoveObserver(this);
      observing_tui_client_ = false;
    }
    // Window/view teardown is presentation teardown only. Terminal hosts are
    // registry-owned processes and must survive a GUI crash, reload, or normal
    // window destruction for another frontend to reattach.
    terminal_backends_.clear();
    terminal_tab_by_id_.clear();
    RemoveCmuxUpdateObserver(this);
#if CHROME_VERSION_MAJOR >= 150
    browser_collection_observation_.Reset();
#else
    BrowserList::RemoveObserver(this);
#endif
    UnregisterCmuxBrowserWindowFactoryHost(this);
    TabStripModelObserver::StopObservingAll(this);
    CancelActiveDragForHostState();
    // Browser-scoped extension containers are referenced by each pane's
    // extension strip, action view models, and puzzle-menu button. Destroy
    // those pane views while their containers are still alive.
    std::vector<PaneId> pane_ids;
    pane_ids.reserve(views_.size());
    for (const auto& pane_view : views_) {
      pane_ids.push_back(pane_view.first);
    }
    for (PaneId pane_id : pane_ids) {
      DestroyPaneView(pane_id, /*notify_focus=*/false);
    }
    // OnWindowClosing can synchronously delete the Browser-owned pages. Drop
    // our non-owning indices before that, rather than in member destruction.
    web_contents_by_surface_.clear();
    web_tab_placements_.clear();
    for (const auto& [workspace, browser] : workspace_browsers_) {
      UnregisterCmuxBrowserWindowHost(native_window_, workspace);
      if (browser) {
        browser->GetFeatures().SetSidePanelUIForCustomBrowserWindow(nullptr);
      }
      workspace_side_panel_uis_.erase(workspace);
      // ExtensionsContainer is stored as unowned data on this Browser. Remove
      // the window-local adapter before OnWindowClosing schedules Browser
      // deletion, otherwise Browser's UnownedUserDataHost destructor CHECKs.
      DestroyCmuxExtensionsContainerForBrowser(browser.get());
      if (browser &&
#if CHROME_VERSION_MAJOR >= 150
          !browser->IsDeleteScheduled()) {
#else
          !browser->is_delete_scheduled()) {
#endif
        browser->OnWindowClosing();
      }
    }
    workspace_browsers_.clear();
    if (content_ && strip_wheel_handler_) {
      content_->RemovePreTargetHandler(strip_wheel_handler_.get());
    }
    if (tui_focus_interaction_handler_) {
      RemovePreTargetHandler(tui_focus_interaction_handler_.get());
    }
    if (root_animator_) {
      root_animator_->RemoveObserver(this);
    }
  }

  // ProfileObserver ---------------------------------------------------------
  void OnProfileWillBeDestroyed(Profile* profile) override {
    CHECK_EQ(profile_, profile);
    // This notification precedes BrowserContext keyed-service shutdown. A
    // CmuxWindowView can outlive that boundary, so its ordinary object
    // WeakPtrs are not enough to make posted replies profile-safe. Fence all
    // pending window callbacks while ThemeService and the other profile
    // services are still valid; direct theme application also checks the
    // profile gate below.
    profile_services_available_ = false;
    ++ghostty_theme_load_request_;
    weak_factory_.InvalidateWeakPtrs();
    // CmuxTuiClient::Observer is a raw observer channel, not a WeakPtr-bound
    // reply. A registry snapshot can otherwise arrive after this notification
    // and materialize a Browser with the dying Profile. That reached
    // IncognitoModePrefs::CanOpenBrowser() with a freed PrefService during
    // signed-app relaunch. Stop the source and clear the pointer at the profile
    // lifetime boundary; entry-point guards below also cover an observer-list
    // dispatch that already captured this observer.
    if (observing_tui_client_) {
      tui_client_->RemoveObserver(this);
      observing_tui_client_ = false;
    }
    TabStripModelObserver::StopObservingAll(this);
    rounded_frame_pref_change_registrar_.Reset();
    profile_observation_.Reset();
    profile_ = nullptr;
  }

  // ---- CmuxTuiClient::Observer --------------------------------------------
  void OnCmuxTuiConnectionChanged(bool connected,
                                  const std::string& /*error*/) override {
    if (!profile_services_available_ || !profile_) {
      return;
    }
    if (connected) {
      LoadWorkspaceProjection();
      SyncWorkspaceRegistry();
      tui_client_->RefreshTerminalRegistry();
      RetryPendingTerminalCloses();
    } else {
      if (workspace_snapshot_) {
        workspace_snapshot_->provisional = true;
      }
      last_applied_tui_selection_.reset();
      last_applied_tui_terminal_tab_.reset();
      ClearPendingTuiTerminalFocus();
      retry_tui_terminal_focus_selection_.reset();
      suppressed_tui_terminal_focus_selection_.reset();
      terminal_close_requests_in_flight_.clear();
      terminal_closes_awaiting_absence_.clear();
    }
  }

  void OnCmuxTuiEvent(const CmuxTuiEvent& event) override {
    if (!profile_services_available_ || !profile_) {
      return;
    }
    if (event.type == CmuxTuiEvent::Type::kTreeChanged &&
        workspace_snapshot_) {
      workspace_snapshot_->provisional = true;
      // A provisional envelope is an invalidation barrier for a queued
      // responder grab. Preserve only its canonical owner-mux identity so the
      // next authoritative snapshot may re-arm it; do not reset the applied
      // selection, because an unrelated rename/add/close must not refocus an
      // already-consumed terminal selection.
      if (pending_tui_terminal_focus_) {
        retry_tui_terminal_focus_selection_ =
            std::make_pair(pending_tui_terminal_focus_->workspace_key,
                           pending_tui_terminal_focus_->terminal_id);
      }
      ClearPendingTuiTerminalFocus();
    }
    if (event.name != "frontend-projection-changed") {
      return;
    }
    const CmuxTuiClient::Options& options = tui_client_->options();
    if (event.frontend != options.frontend ||
        event.frontend_scope != options.frontend_scope ||
        event.projection_subject_key != options.projection_subject_key) {
      return;
    }
    if (!IsCurrentCmuxWorkspaceProjectionEvent(
            workspace_registry_id_, workspace_registry_generation_,
            event.registry_id, event.generation)) {
      return;
    }
    if (!event.projection_revision) {
      return;
    }
    workspace_projection_reload_barrier_.Observe(
        workspace_projection_epoch_, *event.projection_revision);
    MaybeDrainWorkspaceProjectionReload();
  }

  void OnCmuxTuiWorkspaceSnapshot(
      const CmuxTuiWorkspaceSnapshot& snapshot) override {
    if (!profile_services_available_ || !profile_) {
      return;
    }
    ReconcileWorkspaceRegistry(snapshot);
  }

  void OnCmuxTuiTerminalEvent(
      const CmuxTuiTerminalRegistryEvent& event) override {
    if (!profile_services_available_ || !profile_) {
      return;
    }
    CmuxTerminalPlacementPlan plan = terminal_placement_model_.ApplyEvent(
        event.placement, CaptureLocalTerminalPlacements(),
        CurrentGuiWorkspaceKeys(), CapturePendingTerminalCloses());
    if (plan.require_snapshot()) {
      tui_client_->RefreshTerminalRegistry();
      return;
    }
    ApplyTerminalPlacementPlan(plan);
    if (event.placement.terminal.lifecycle ==
        CmuxTerminalLifecycle::kTombstoned) {
      ConfirmTerminalAbsent(event.placement.terminal.terminal_id);
    }
  }

  void OnCmuxTuiTerminalSnapshot(
      const CmuxTerminalPlacementSnapshot& snapshot) override {
    if (!profile_services_available_ || !profile_) {
      return;
    }
    const bool had_terminal_snapshot =
        terminal_placement_model_.has_snapshot();
    const bool registry_replaced =
        had_terminal_snapshot &&
        terminal_placement_model_.registry_id() != snapshot.registry_id;
    const bool generation_changed =
        had_terminal_snapshot && !registry_replaced &&
        terminal_placement_model_.generation() != snapshot.generation;
    terminal_registry_replaced_pending_ |= registry_replaced;
    terminal_generation_change_pending_ |= generation_changed;
    if (registry_replaced) {
      // Exactly-once mutation keys are scoped to one durable registry. Never
      // replay a close intent after a socket path is reused by another DB.
      pending_terminal_closes_.clear();
      terminal_close_requests_in_flight_.clear();
      terminal_closes_awaiting_absence_.clear();
      terminal_close_retry_timer_.Stop();
      std::vector<SurfaceTabId> retired_tabs;
      retired_tabs.reserve(terminal_backends_.size());
      for (const auto& entry : terminal_backends_) {
        retired_tabs.push_back(entry.first);
      }
      for (SurfaceTabId tab : retired_tabs) {
        RemoveCanonicalTerminalProjection(tab);
      }
      RebuildTerminalBackendIndex();
    }
    CmuxTerminalPlacementPlan plan = terminal_placement_model_.ApplySnapshot(
        snapshot, CaptureLocalTerminalPlacements(), CurrentGuiWorkspaceKeys(),
        CapturePendingTerminalCloses());
    if (plan.require_snapshot()) {
      tui_client_->RefreshTerminalRegistry();
      return;
    }
    ApplyTerminalPlacementPlan(plan);
    std::map<std::string, std::string> present;
    for (const CmuxCanonicalTerminalPlacement& terminal : snapshot.terminals) {
      present.insert_or_assign(terminal.terminal_id, terminal.incarnation);
    }
    for (auto it = pending_terminal_closes_.begin();
         it != pending_terminal_closes_.end();) {
      const auto canonical = present.find(it->first);
      const bool replacement =
          canonical != present.end() && !it->second.incarnation.empty() &&
          !canonical->second.empty() &&
          canonical->second != it->second.incarnation;
      if (canonical == present.end() || replacement) {
        // Absence confirms the close. A different incarnation confirms the
        // exact close lost its CAS race; do not target the replacement.
        terminal_close_requests_in_flight_.erase(it->first);
        terminal_closes_awaiting_absence_.erase(it->first);
        it = pending_terminal_closes_.erase(it);
      } else {
        ++it;
      }
    }
    RetryPendingTerminalCloses();
  }

  base::WeakPtr<CmuxBrowserWindowHost> BrowserWindowHostWeakPtr() {
    return weak_factory_.GetWeakPtr();
  }

  // ---- cmux::RailDelegate (the vertical-tab tree) ---------------------------
  void OnSelectWorkspace(WorkspaceId id) override { SwitchWorkspace(id); }
  void OnExtendWorkspaceSelection(WorkspaceId id) override {
    model_.ExtendWorkspaceSelectionTo(id);
    ActivateModelSelectedWorkspace();
  }
  void OnAddWorkspaceSelectionFromAnchorTo(WorkspaceId id) override {
    model_.AddWorkspaceSelectionFromAnchorTo(id);
    ActivateModelSelectedWorkspace();
  }
  void OnToggleWorkspaceSelection(WorkspaceId id) override {
    model_.ToggleWorkspaceSelection(id);
    ActivateModelSelectedWorkspace();
  }
  void OnActivateWorkspaceInSelection(WorkspaceId id) override {
    model_.ActivateWorkspaceInSelection(id);
    ActivateModelSelectedWorkspace();
  }
  bool IsWorkspaceSelected(WorkspaceId id) const override {
    return model_.IsWorkspaceSelected(id);
  }
  WorkspaceSelectionState GetWorkspaceSelectionState() const override {
    return model_.GetWorkspaceSelectionState();
  }
  void OnRestoreWorkspaceSelectionState(
      const WorkspaceSelectionState& state) override {
    model_.RestoreWorkspaceSelectionState(state);
    ActivateModelSelectedWorkspace();
  }
  WorkspaceId AddPendingWorkspace(const std::string& title) {
    ResetWorkspaceMutationFailureBudget();
    const WorkspaceId workspace = model_.AddWorkspace(
        SurfaceKind::kWeb, title, NewWorkspaceRegistryKey());
    pending_workspace_mutations_.RememberCreate(
        WorkspaceRegistryKey(workspace), NewWorkspaceMutationId());
    return workspace;
  }
  void OnNewWorkspace() override {
    const WorkspaceId workspace = AddPendingWorkspace("Workspace");
    CreateBrowserForWorkspace(workspace);
    SwitchWorkspace(workspace);
    SyncNextMissingWorkspace();
  }
  std::optional<RecentWorkspaceGroup>
  GetMostRecentWorkspaceGroupForContextMenu() const override {
    WorkspaceGroupId group_id = most_recent_workspace_group_;
    if (!model_.GetWorkspaceGroup(group_id)) {
      const Workspace* selected = model_.GetWorkspace(ws_);
      group_id = selected ? selected->group : kInvalidId;
    }
    if (!model_.GetWorkspaceGroup(group_id) &&
        !model_.workspace_groups().empty()) {
      group_id = model_.workspace_groups().back().id;
    }
    const WorkspaceGroup* group = model_.GetWorkspaceGroup(group_id);
    const std::vector<WorkspaceId> members =
        group ? model_.WorkspacesInGroup(group_id)
              : std::vector<WorkspaceId>();
    if (!group || members.empty()) {
      return std::nullopt;
    }
    return RecentWorkspaceGroup{group_id, group->title,
                                static_cast<int>(members.size())};
  }
  bool IsNewWorkspaceContextActionEnabled(
      NewWorkspaceContextAction action) const override {
    switch (action) {
      case NewWorkspaceContextAction::kNewWorkspace:
      case NewWorkspaceContextAction::kNewWorkspaceGroup:
        return true;
      case NewWorkspaceContextAction::kNewWorkspaceInRecentGroup:
        return GetMostRecentWorkspaceGroupForContextMenu().has_value();
      case NewWorkspaceContextAction::kNewSplitView: {
        const Workspace* workspace = model_.GetWorkspace(ws_);
        const Pane* pane = workspace
                               ? model_.FindPane(ws_, workspace->focused)
                               : nullptr;
        return pane && pane->selected != kInvalidId;
      }
    }
    return false;
  }
  void OnNewWorkspaceContextAction(
      NewWorkspaceContextAction action,
      WorkspaceGroupId recent_group) override {
    switch (action) {
      case NewWorkspaceContextAction::kNewWorkspace:
        OnNewWorkspace();
        return;
      case NewWorkspaceContextAction::kNewWorkspaceInRecentGroup: {
        const std::vector<WorkspaceId> members =
            model_.WorkspacesInGroup(recent_group);
        if (members.empty()) {
          return;
        }
        const WorkspaceId fresh = AddPendingWorkspace("New tab");
        model_.MoveWorkspace(fresh, members.front(), -1);
        CreateBrowserForWorkspace(fresh);
        most_recent_workspace_group_ = recent_group;
        SwitchWorkspace(fresh);
        RequestWorkspaceOrder();
        return;
      }
      case NewWorkspaceContextAction::kNewWorkspaceGroup: {
        const WorkspaceId fresh = AddPendingWorkspace("New tab");
        const WorkspaceGroupId group = model_.CreateWorkspaceGroup(fresh);
        CreateBrowserForWorkspace(fresh);
        most_recent_workspace_group_ = group;
        SwitchWorkspace(fresh);
        RequestWorkspaceOrder();
        ShowWorkspaceGroupEditorBubbleDeferred(group);
        return;
      }
      case NewWorkspaceContextAction::kNewSplitView: {
        const Workspace* workspace = model_.GetWorkspace(ws_);
        const Pane* pane =
            workspace ? model_.FindPane(ws_, workspace->focused) : nullptr;
        if (pane && pane->selected != kInvalidId) {
          NewSplitWithCurrentTabDeferred(pane->id, pane->selected,
                                         SplitOrientation::kHorizontal);
        }
        return;
      }
    }
  }
  bool GetNewWorkspaceContextAccelerator(
      NewWorkspaceContextAction action,
      ui::Accelerator* accelerator) const override {
    if (!accelerator) {
      return false;
    }
    switch (action) {
      case NewWorkspaceContextAction::kNewWorkspace:
        *accelerator = ui::Accelerator(ui::VKEY_N, ui::EF_ALT_DOWN);
        return true;
      case NewWorkspaceContextAction::kNewSplitView:
        *accelerator = ui::Accelerator(ui::VKEY_D, ui::EF_PLATFORM_ACCELERATOR);
        return true;
      case NewWorkspaceContextAction::kNewWorkspaceInRecentGroup:
      case NewWorkspaceContextAction::kNewWorkspaceGroup:
        return false;
    }
    return false;
  }
  void NewWorkspace() override { OnNewWorkspace(); }
  void OnCloseWorkspace(WorkspaceId id) override {
    CloseWorkspaceAndBrowser(id);
  }
  void OnRenameWorkspace(WorkspaceId id, const std::string& name) override {
    Workspace* workspace = model_.GetWorkspace(id);
    if (!workspace || workspace->registry_key.empty() || name.empty() ||
        workspace->title == name) {
      return;
    }
    ResetWorkspaceMutationFailureBudget();
    const std::string key = workspace->registry_key;
    model_.SetWorkspaceTitle(id, name);
    RefreshRail();
    pending_workspace_mutations_.RememberRename(
        key, name, NewWorkspaceMutationId());
    SyncWorkspaceRegistry();
  }
  void OnToggleExpanded(WorkspaceId id) override {
    const Workspace* ws = model_.GetWorkspace(id);
    const WorkspaceGroup* group =
        ws ? model_.GetWorkspaceGroup(ws->group) : nullptr;
    if (group) {
      const WorkspaceGroupId group_id = group->id;
      const bool collapsing = !group->collapsed;
      const Workspace* active =
          model_.GetWorkspace(model_.selected_workspace());
      if (collapsing && active && active->group == group_id) {
        const bool has_expanded_workspace_outside_group =
            std::ranges::any_of(model_.roots(), [&](WorkspaceId candidate_id) {
              const Workspace* candidate = model_.GetWorkspace(candidate_id);
              if (!candidate || candidate->group == group_id) {
                return false;
              }
              const WorkspaceGroup* candidate_group =
                  model_.GetWorkspaceGroup(candidate->group);
              return !candidate_group || !candidate_group->collapsed;
            });
        if (!has_expanded_workspace_outside_group) {
          // Chromium creates an ungrouped active tab before collapsing a group
          // that owns the whole window. A cmux workspace owns a Browser graph,
          // so create and realize the corresponding ungrouped workspace here at
          // the controller boundary before asking the pure model to collapse.
          const WorkspaceId fallback = AddPendingWorkspace("New tab");
          CreateBrowserForWorkspace(fallback);
          SyncNextMissingWorkspace();
        }
      }
      model_.SetWorkspaceGroupCollapsed(group_id, collapsing);
      const WorkspaceId selected = model_.selected_workspace();
      if (selected != ws_) {
        SwitchWorkspace(selected);
        return;
      }
      RefreshRail();
      MarkWorkspaceProjectionDirty();
    }
  }
  void OnMoveWorkspace(WorkspaceId id,
                       WorkspaceId new_parent,
                       int index) override {
    model_.MoveWorkspace(id, new_parent, index);
    RefreshRail();
    MarkWorkspaceProjectionDirty();
    RequestWorkspaceOrder();
  }
  void OnMoveWorkspaceSelection(WorkspaceId context,
                                WorkspaceId new_parent,
                                int index) override {
    std::vector<WorkspaceId> targets =
        model_.WorkspacesForCommand(context);
    if (new_parent != kInvalidId) {
      const Workspace* parent = model_.GetWorkspace(new_parent);
      if (!parent || parent->group == kInvalidId) {
        return;
      }
      const WorkspaceGroupId destination_group = parent->group;
      // A cross-group multi-selection can include the menu's representative
      // for the destination group. Leave every existing destination member in
      // place and move only the selected outsiders; this preserves a stable
      // anchor even when the destination group was fully selected.
      const auto is_destination_member = [&](WorkspaceId target) {
        const Workspace* workspace = model_.GetWorkspace(target);
        return workspace && workspace->group == destination_group;
      };
      if (std::ranges::any_of(targets, [&](WorkspaceId target) {
            return !is_destination_member(target);
          })) {
        std::erase_if(targets, is_destination_member);
      } else if (std::ranges::contains(targets, new_parent)) {
        const std::vector<WorkspaceId> members =
            model_.WorkspacesInGroup(destination_group);
        const auto fixed =
            std::ranges::find_if(members, [&](WorkspaceId member) {
              return !std::ranges::contains(targets, member);
            });
        if (fixed == members.end()) {
          // EndRowDrag has already detached the dragged views. Even though an
          // all-members drop has no fixed anchor and cannot reorder, refresh
          // so every row is reparented into its group immediately.
          RefreshRail();
          return;
        }
        new_parent = *fixed;
      }
    }
    model_.MoveWorkspaces(targets, new_parent, index);
    RefreshRail();
    MarkWorkspaceProjectionDirty();
    RequestWorkspaceOrder();
  }
  void OnMoveWorkspaceGroup(WorkspaceGroupId id, int index) override {
    const std::vector<WorkspaceId> members = model_.WorkspacesInGroup(id);
    if (members.empty()) {
      return;
    }
    model_.MoveWorkspaces(members, kInvalidId, index);
    RefreshRail();
    MarkWorkspaceProjectionDirty();
    RequestWorkspaceOrder();
  }
  bool IsWorkspaceContextActionEnabled(
      WorkspaceId id,
      WorkspaceContextAction action) const override {
    const Workspace* workspace = model_.GetWorkspace(id);
    if (!workspace) {
      return false;
    }
    const std::vector<WorkspaceId> targets = model_.WorkspacesForCommand(id);
    const auto last_target_it =
        targets.empty()
            ? model_.roots().end()
            : std::find(model_.roots().begin(), model_.roots().end(),
                        targets.back());
    switch (action) {
      case WorkspaceContextAction::kNewWorkspaceBelow:
        return true;
      case WorkspaceContextAction::kNewSplitSideBySide:
      case WorkspaceContextAction::kNewSplitStacked: {
        const Pane* pane = model_.FindPane(id, workspace->focused);
        return targets.size() == 1 && pane &&
               pane->selected != kInvalidId;
      }
      case WorkspaceContextAction::kAddToNewGroup:
        return true;
      case WorkspaceContextAction::kRemoveFromGroup:
        return std::ranges::any_of(targets, [&](WorkspaceId target) {
          const Workspace* candidate = model_.GetWorkspace(target);
          return candidate && candidate->group != kInvalidId;
        });
      case WorkspaceContextAction::kClose:
        return true;
      case WorkspaceContextAction::kCloseOthers:
        return model_.workspace_count() > targets.size();
      case WorkspaceContextAction::kCloseBelow:
        return last_target_it != model_.roots().end() &&
               std::next(last_target_it) != model_.roots().end();
      case WorkspaceContextAction::kReload:
        return std::ranges::any_of(targets, [&](WorkspaceId target) {
          return !WebTabsInWorkspace(target).empty();
        });
      case WorkspaceContextAction::kToggleSiteMuted: {
        return std::ranges::any_of(
            WorkspaceContextWebTabTargets(id),
            [](const auto& target) {
              const auto& [tabs, browser_index] = target;
              return tabs->IsContextMenuCommandEnabled(
                  browser_index, TabStripModel::CommandToggleSiteMuted);
            });
      }
      case WorkspaceContextAction::kAddToReadLater:
        return std::ranges::any_of(targets, [&](WorkspaceId target) {
          for (SurfaceTabId surface : WebTabsInWorkspace(target)) {
            int browser_index = TabStripModel::kNoTab;
            TabStripModel* tabs =
                BrowserTabsForSurface(surface, &browser_index);
            if (tabs && browser_index != TabStripModel::kNoTab &&
                tabs->delegate()->SupportsReadLater() &&
                tabs->IsReadLaterSupportedForAny({browser_index})) {
              return true;
            }
          }
          return false;
        });
      case WorkspaceContextAction::kMoveToNewWindow:
      case WorkspaceContextAction::kTogglePinned:
        // cmux does not yet transfer/clone/pin whole workspace browser graphs.
        return false;
      case WorkspaceContextAction::kDuplicate:
        return true;
      default:
        break;
    }
    const std::optional<TabContextAction> tab_action =
        WorkspaceTabContextAction(action);
    const Pane* pane = model_.FindPane(id, workspace->focused);
    return id == ws_ && tab_action && pane && pane->selected != kInvalidId &&
           IsTabContextActionEnabled(pane->id, pane->selected, *tab_action);
  }
  bool IsWorkspaceContextActionToggled(
      WorkspaceId id,
      WorkspaceContextAction action) const override {
    const Workspace* workspace = model_.GetWorkspace(id);
    if (workspace && action == WorkspaceContextAction::kToggleSiteMuted) {
      const auto targets = WorkspaceContextWebTabTargets(id);
      return !targets.empty() &&
             std::ranges::all_of(targets, [](const auto& target) {
               const auto& [tabs, browser_index] = target;
               return CmuxIsSiteMuted(*tabs, browser_index);
             });
    }
    const std::optional<TabContextAction> tab_action =
        WorkspaceTabContextAction(action);
    const Pane* pane = workspace ? model_.FindPane(id, workspace->focused)
                                 : nullptr;
    return id == ws_ && tab_action && pane && pane->selected != kInvalidId &&
           IsTabContextActionToggled(pane->id, pane->selected, *tab_action);
  }
  int WorkspaceContextUrlCount(WorkspaceId id) const override {
    int count = 0;
    for (WorkspaceId target : model_.WorkspacesForCommand(id)) {
      count += static_cast<int>(WebTabsInWorkspace(target).size());
    }
    return count;
  }
  int WorkspaceContextTargetCount(WorkspaceId id) const override {
    return static_cast<int>(model_.WorkspacesForCommand(id).size());
  }
  bool IsWorkspaceContextMenuSimplificationEnabled() const override {
    return CmuxIsMenuSimplificationEnabled();
  }
  WorkspaceHoverCardData GetWorkspaceHoverCardData(
      WorkspaceId id) const override {
    WorkspaceHoverCardData data;
    const Workspace* workspace = model_.GetWorkspace(id);
    if (!workspace) {
      return data;
    }
    data.title = base::UTF8ToUTF16(
        workspace->title.empty() ? "Workspace" : workspace->title);

    const Pane* pane = model_.FindPane(id, workspace->focused);
    const SurfaceTab* surface = pane ? pane->SelectedTab() : nullptr;
    data.source_surface = surface ? surface->id : kInvalidId;
    content::WebContents* contents =
        surface && surface->kind == SurfaceKind::kWeb
            ? WebContentsForSurface(surface->id)
            : nullptr;
    if (!contents) {
      return data;
    }

    // Chromium never shows a preview for the active tab. A cmux workspace row
    // represents that tab boundary, so only inactive workspaces request an
    // image for their focused web surface.
    data.show_preview = id != ws_;
    tabs::TabInterface* const tab_interface =
        tabs::TabInterface::MaybeGetFromContents(contents);
    if (!tab_interface) {
      return data;
    }
    const tabs::TabData tab_data =
        tabs::TabData::FromTabInterface(tab_interface);
    if (!tab_data.title.empty()) {
      data.title = tab_data.title;
    }
    if (tab_data.should_display_url) {
      const GURL& visible_url = tab_data.visible_url;
      if (visible_url.IsAboutBlank()) {
        data.domain = u"about:blank";
      } else if (visible_url.is_valid()) {
        data.domain = url_formatter::FormatUrl(
            visible_url,
            url_formatter::kFormatUrlOmitDefaults |
                url_formatter::kFormatUrlOmitHTTPS |
                url_formatter::kFormatUrlOmitTrivialSubdomains |
                url_formatter::kFormatUrlTrimAfterHost,
            base::UnescapeRule::NORMAL, nullptr, nullptr, nullptr);
      }
    }
    data.is_crashed = tab_data.is_crashed;
    data.is_discarded = tab_data.is_tab_discarded;
    data.has_preview_source = static_cast<bool>(tab_data.thumbnail);
    if (tab_data.thumbnail) {
      data.preview_source_id =
          reinterpret_cast<uintptr_t>(tab_data.thumbnail.get());
      data.has_preview_data = tab_data.thumbnail->has_data();
      data.preview_readiness =
          ToWorkspacePreviewReadiness(
              tab_data.thumbnail->GetCaptureReadiness());
    }
    return data;
  }
  std::unique_ptr<WorkspaceHoverCardPreviewRequest>
  RequestWorkspaceHoverCardPreview(
      WorkspaceId id,
      SurfaceTabId source_surface,
      uintptr_t preview_source_id,
      base::RepeatingCallback<void(gfx::ImageSkia)> callback) override {
    const Workspace* workspace = model_.GetWorkspace(id);
    const Pane* pane =
        workspace ? model_.FindPane(id, workspace->focused) : nullptr;
    const SurfaceTab* surface = pane ? pane->SelectedTab() : nullptr;
    if (!surface || surface->id != source_surface) {
      return nullptr;
    }
    content::WebContents* contents =
        surface->kind == SurfaceKind::kWeb
            ? WebContentsForSurface(surface->id)
            : nullptr;
    ThumbnailTabHelper* const thumbnail_helper =
        contents ? ThumbnailTabHelper::FromWebContents(contents) : nullptr;
    scoped_refptr<ThumbnailImage> thumbnail =
        thumbnail_helper ? thumbnail_helper->thumbnail() : nullptr;
    if (!thumbnail ||
        reinterpret_cast<uintptr_t>(thumbnail.get()) != preview_source_id) {
      return nullptr;
    }
    return std::make_unique<CmuxWorkspaceHoverCardPreviewRequest>(
        std::move(thumbnail), std::move(callback));
  }
  void AppendOptionalWorkspaceContextMenuItems(
      WorkspaceId id,
      ui::SimpleMenuModel* menu,
      ui::SimpleMenuModel::Delegate* menu_delegate) override {
    workspace_context_menu_native_items_.reset();
    workspace_context_menu_native_workspace_ = kInvalidId;
    Browser* browser = BrowserForWorkspace(id);
    TabStripModel* context_tabs =
        browser ? browser->tab_strip_model() : nullptr;
    if (!context_tabs || context_tabs->count() == 0) {
      return;
    }

    int context_index = context_tabs->active_index();
    const Workspace* workspace = model_.GetWorkspace(id);
    const Pane* pane = workspace
                           ? model_.FindPane(id, workspace->focused)
                           : nullptr;
    const SurfaceTab* surface = pane ? pane->SelectedTab() : nullptr;
    if (surface) {
      int selected_index = TabStripModel::kNoTab;
      if (TabStripModel* selected_tabs =
              BrowserTabsForSurface(surface->id, &selected_index);
          selected_tabs == context_tabs &&
          selected_index != TabStripModel::kNoTab) {
        context_index = selected_index;
      }
    }
    if (!context_tabs->ContainsIndex(context_index)) {
      return;
    }

    std::vector<base::WeakPtr<tabs::TabInterface>> glic_targets;
    std::set<tabs::TabInterface*> seen_glic_targets;
    for (WorkspaceId target : model_.WorkspacesForCommand(id)) {
      for (SurfaceTabId target_surface : WebTabsInWorkspace(target)) {
        int target_index = TabStripModel::kNoTab;
        TabStripModel* target_tabs =
            BrowserTabsForSurface(target_surface, &target_index);
        if (!target_tabs || !target_tabs->ContainsIndex(target_index)) {
          continue;
        }
        tabs::TabInterface* target_tab =
            target_tabs->GetTabAtIndex(target_index);
        if (target_tab && seen_glic_targets.insert(target_tab).second) {
          glic_targets.push_back(target_tab->GetWeakPtr());
        }
      }
    }

    workspace_context_menu_native_workspace_ = id;
    workspace_context_menu_native_items_ =
        std::make_unique<WorkspaceContextMenuNativeItems>(
            browser, context_index, WorkspaceContextTargetCount(id),
            std::move(glic_targets), menu, menu_delegate);
  }
  bool IsOptionalWorkspaceContextMenuCommandEnabled(
      WorkspaceId id,
      int command_id) const override {
    return id == workspace_context_menu_native_workspace_ &&
           workspace_context_menu_native_items_ &&
           workspace_context_menu_native_items_->IsCommandEnabled(command_id);
  }
  bool IsOptionalWorkspaceContextMenuCommandChecked(
      WorkspaceId id,
      int command_id) const override {
    return id == workspace_context_menu_native_workspace_ &&
           workspace_context_menu_native_items_ &&
           workspace_context_menu_native_items_->IsCommandChecked(command_id);
  }
  bool IsOptionalWorkspaceContextMenuCommandVisible(
      WorkspaceId id,
      int command_id) const override {
    return id == workspace_context_menu_native_workspace_ &&
           workspace_context_menu_native_items_ &&
           workspace_context_menu_native_items_->IsCommandVisible(command_id);
  }
  void ExecuteOptionalWorkspaceContextMenuCommand(
      WorkspaceId id,
      int command_id,
      int event_flags) override {
    if (id == workspace_context_menu_native_workspace_ &&
        workspace_context_menu_native_items_) {
      workspace_context_menu_native_items_->ExecuteCommand(command_id,
                                                            event_flags);
    }
  }
  void OnSetWorkspaceGroupTitle(WorkspaceGroupId id,
                                const std::string& title) override {
    if (!model_.GetWorkspaceGroup(id)) {
      return;
    }
    model_.SetWorkspaceGroupTitle(id, title);
    RefreshRail();
    MarkWorkspaceProjectionDirty();
  }
  void OnSetWorkspaceGroupColor(WorkspaceGroupId id,
                                GroupColor color) override {
    const std::vector<WorkspaceId> members = model_.WorkspacesInGroup(id);
    if (members.empty()) {
      return;
    }
    // Group presentation and each member's future extraction preference must
    // change together. Otherwise extracting a non-first member before restart
    // resurrects its stale color, while projection restore updates all members.
    for (WorkspaceId member : members) {
      model_.SetWorkspaceColor(member, color);
    }
    RefreshRail();
    MarkWorkspaceProjectionDirty();
  }
  bool IsWorkspaceGroupContextActionEnabled(
      WorkspaceGroupId id,
      WorkspaceGroupContextAction action) const override {
    if (!model_.GetWorkspaceGroup(id) ||
        model_.WorkspacesInGroup(id).empty()) {
      return false;
    }
    switch (action) {
      case WorkspaceGroupContextAction::kNewWorkspaceInGroup:
      case WorkspaceGroupContextAction::kCloseGroup:
      case WorkspaceGroupContextAction::kUngroup:
        return true;
      case WorkspaceGroupContextAction::kMoveGroupToNewWindow:
        return false;
    }
    return false;
  }
  void OnWorkspaceGroupContextAction(
      WorkspaceGroupId id,
      WorkspaceGroupContextAction action) override {
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE,
        base::BindOnce(&CmuxWindowView::ExecuteWorkspaceGroupContextActionNow,
                       weak_factory_.GetWeakPtr(), id, action));
  }
  void OnWorkspaceContextAction(WorkspaceId id,
                                WorkspaceContextAction action) override {
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE,
        base::BindOnce(&CmuxWindowView::ExecuteWorkspaceContextActionNow,
                       weak_factory_.GetWeakPtr(), id, action));
  }
  void OnBeginWindowDrag(const ui::MouseEvent& event) override {
    PlatformBeginWindowDrag(GetWidget(), event);
  }
  void OnRailConfigLoaded() override {
    base_rail_config_ = rail_->config();
    applied_rail_header_height_ = -1;
    applied_animations_ = !layout_config_.animations;
    applied_animation_ms_ = -1;
    applied_focus_border_ = -1;
    applied_accent_color_ = SK_ColorTRANSPARENT;
    applied_drop_highlight_color_ = SK_ColorTRANSPARENT;
    applied_focus_color_ = SK_ColorTRANSPARENT;
    applied_theme_generation_ = -1;
    applied_sidebar_mode_.reset();
    ApplyRailLayoutConfig();
  }
  void OnApplyUpdate() override { ApplyReadyCmuxUpdate(); }

  // ---- CmuxUpdateObserver -------------------------------------------------
  void OnCmuxUpdateChanged(const UpdateSnapshot& snapshot) override {
    if (rail_) {
      rail_->SetUpdateReady(ShouldShowUpdateNow(snapshot.state),
                            snapshot.version);
    }
  }

  // ---- CmuxBrowserWindowHost ----------------------------------------------
  std::optional<WorkspaceId> AdoptExternalBrowser(Browser* browser) override {
    if (!browser || !browser->is_type_normal() ||
        browser->profile()->GetOriginalProfile() !=
            profile_->GetOriginalProfile()) {
      return std::nullopt;
    }
    ResetWorkspaceMutationFailureBudget();
    const WorkspaceId workspace = model_.AddWorkspace(
        SurfaceKind::kWeb, "Workspace", NewWorkspaceRegistryKey());
    pending_workspace_mutations_.RememberCreate(
        WorkspaceRegistryKey(workspace), NewWorkspaceMutationId());
    CHECK(native_window_registry_->RegisterWorkspace(
        WorkspaceLocator{native_window_, workspace}));
    RegisterCmuxBrowserWindowHost(native_window_, workspace, window_group_,
                                  WorkspaceRegistryKey(workspace),
                                  weak_factory_.GetWeakPtr());
    workspace_browsers_[workspace] = browser->AsWeakPtr();
    auto side_panel_ui = CreateCmuxSidePanelUI(browser);
    browser->GetFeatures().SetSidePanelUIForCustomBrowserWindow(
        side_panel_ui.get());
    workspace_side_panel_uis_[workspace] = std::move(side_panel_ui);
    browser->tab_strip_model()->AddObserver(this);
    CompleteViewsStartupAfterWorkspaceBrowserCreated();
    RefreshRail();
    SyncNextMissingWorkspace();
    return workspace;
  }

  views::Widget* GetWorkspaceWidget() override { return GetWidget(); }

  bool IsWorkspaceVisible(WorkspaceId workspace) const override {
    return workspace == ws_ && GetWidget() && GetWidget()->IsVisible();
  }

  bool IsWorkspaceActive(WorkspaceId workspace) const override {
    return workspace == ws_ && GetWidget() && GetWidget()->IsActive();
  }

  void ShowWorkspace(WorkspaceId workspace, bool activate) override {
    if (!model_.GetWorkspace(workspace)) {
      return;
    }
    if (views::Widget* widget = GetWidget()) {
      // A Chrome window shown inactive must not become the current window.
      // In cmux inactive workspaces share the physical widget and remain
      // logically hidden; only an activating Show selects the workspace.
      if (activate || !widget->IsVisible()) {
        SwitchWorkspace(workspace);
      }
      widget->Show();
      if (activate) {
        widget->Activate();
      }
    }
  }

  void CloseWorkspaceFromBrowser(WorkspaceId workspace) override {
    Browser* browser = BrowserForWorkspace(workspace);
    if (browser && browser->tab_strip_model()->empty() &&
        suppress_empty_browser_close_once_.erase(workspace) > 0) {
      // Chrome normally equates an empty tab strip with an empty window. A
      // cmux workspace may still contain terminal tabs, which are deliberately
      // invisible to extensions and the Browser tab model. Suppress only the
      // automatic close caused by removing the final web tab; a later explicit
      // chrome.windows.remove() must still close the workspace.
      return;
    }
    // Browser::TabStripEmpty() reaches BrowserWindow::Close() from inside a
    // TabStripModel notification. Defer the cmux layout/view teardown until
    // that notification (and any native event which initiated it) unwinds.
    ScheduleCloseWorkspace(workspace);
  }

  void FocusWorkspaceLocationBar(WorkspaceId workspace) override {
    if (workspace != ws_) {
      SwitchWorkspace(workspace);
    }
    FocusActiveOmnibar();
  }

  void FocusWorkspaceContents(WorkspaceId workspace) override {
    if (workspace != ws_) {
      SwitchWorkspace(workspace);
    }
    if (CmuxPane* pane = FocusedView()) {
      pane->FocusContent();
    }
  }

  void FocusWorkspaceAppMenu(WorkspaceId workspace) override {
    // The app-menu button is pane-local. Selecting the workspace and focusing
    // its toolbar is the stable keyboard entry point; the focused pane's menu
    // button remains reachable through normal focus traversal.
    FocusWorkspaceLocationBar(workspace);
  }

  LocationBar* GetWorkspaceLocationBar(
      WorkspaceId workspace,
      content::WebContents* web_contents) override {
    Browser* browser = BrowserForWorkspace(workspace);
    content::WebContents* target =
        web_contents
            ? web_contents
            : (browser
                   ? browser->tab_strip_model()->GetActiveWebContents()
                   : nullptr);
    auto placement = web_tab_placements_.find(target);
    if (placement != web_tab_placements_.end() &&
        placement->second.workspace == workspace) {
      if (CmuxPaneView* pane = ViewForPane(placement->second.pane)) {
        if (CmuxSurface* surface =
                pane->SurfaceFor(placement->second.surface)) {
          return surface->GetLocationBar();
        }
      }
    }
    if (web_contents) {
      return nullptr;
    }
    for (PaneId pane_id : model_.PanesOf(workspace)) {
      const Pane* pane_model = model_.FindPane(workspace, pane_id);
      CmuxPaneView* pane = ViewForPane(pane_id);
      if (!pane_model || !pane) {
        continue;
      }
      for (const SurfaceTab& tab : pane_model->tabs) {
        if (tab.kind == SurfaceKind::kWeb) {
          if (CmuxSurface* surface = pane->SurfaceFor(tab.id)) {
            return surface->GetLocationBar();
          }
        }
      }
    }
    return nullptr;
  }

  bool IsWorkspaceWebContentsVisible(
      WorkspaceId workspace,
      content::WebContents* web_contents) override {
    if (!web_contents || !IsWorkspaceVisible(workspace)) {
      return false;
    }
    const auto placement = web_tab_placements_.find(web_contents);
    if (placement == web_tab_placements_.end() ||
        placement->second.workspace != workspace) {
      return false;
    }
    const Pane* pane = model_.FindPane(workspace, placement->second.pane);
    CmuxPaneView* pane_view = ViewForPane(placement->second.pane);
    CmuxSurface* surface =
        pane_view ? pane_view->SurfaceFor(placement->second.surface) : nullptr;
    views::View* surface_view = surface ? surface->AsView() : nullptr;
    return pane && pane->selected == placement->second.surface &&
           surface_view && !surface_view->GetVisibleBounds().IsEmpty();
  }

  web_modal::WebContentsModalDialogHost*
  GetWorkspaceWebContentsModalDialogHost(
      WorkspaceId workspace,
      content::WebContents* web_contents) override {
    Browser* browser = BrowserForWorkspace(workspace);
    content::WebContents* target =
        web_contents
            ? web_contents
            : (browser
                   ? browser->tab_strip_model()->GetActiveWebContents()
                   : nullptr);
    auto placement = web_tab_placements_.find(target);
    if (placement != web_tab_placements_.end() &&
        placement->second.workspace == workspace) {
      if (CmuxPaneView* pane = ViewForPane(placement->second.pane)) {
        if (CmuxSurface* surface =
                pane->SurfaceFor(placement->second.surface)) {
          return surface->GetWebContentsModalDialogHost();
        }
      }
    }

    // Non-tab WebContents such as DevTools can still request a constrained
    // dialog through this BrowserWindow. Match BrowserView's fallback by
    // hosting it over the active web surface for this workspace.
    content::WebContents* active =
        browser ? browser->tab_strip_model()->GetActiveWebContents() : nullptr;
    placement = web_tab_placements_.find(active);
    if (placement != web_tab_placements_.end() &&
        placement->second.workspace == workspace) {
      if (CmuxPaneView* pane = ViewForPane(placement->second.pane)) {
        if (CmuxSurface* surface =
                pane->SurfaceFor(placement->second.surface)) {
          return surface->GetWebContentsModalDialogHost();
        }
      }
    }
    return nullptr;
  }

  content::KeyboardEventProcessingResult PreHandleWorkspaceKeyboardEvent(
      WorkspaceId workspace,
      const input::NativeWebKeyboardEvent& event) override {
    if (workspace != ws_ ||
        event.GetType() != blink::WebInputEvent::Type::kRawKeyDown) {
      return content::KeyboardEventProcessingResult::NOT_HANDLED;
    }
    std::optional<KeyChord> chord = KeyChordFromNativeWebKeyboardEvent(event);
    if (!chord) {
      return content::KeyboardEventProcessingResult::NOT_HANDLED;
    }
    const bool is_repeat =
        (event.GetModifiers() & blink::WebInputEvent::kIsAutoRepeat) != 0;
    return HandleReservedKeyChord(*chord, CurrentKeyContext(), is_repeat)
               ? content::KeyboardEventProcessingResult::HANDLED
               : content::KeyboardEventProcessingResult::NOT_HANDLED;
  }

  bool HandleWorkspaceKeyboardEvent(
      WorkspaceId workspace,
      const input::NativeWebKeyboardEvent& event) override {
    if (workspace != ws_) {
      return false;
    }
    views::FocusManager* focus_manager = GetFocusManager();
    return focus_manager &&
           unhandled_keyboard_event_handler_.HandleKeyboardEvent(event,
                                                                 focus_manager);
  }

  void OnWorkspaceBrowserActiveTabChanged(
      WorkspaceId workspace,
      content::WebContents* old_contents,
      content::WebContents* new_contents) override {
    if (!new_contents) {
      return;
    }
    auto it = web_tab_placements_.find(new_contents);
    if (it == web_tab_placements_.end() || it->second.workspace != workspace) {
      return;
    }
    // Closing a focused web tab lets Chromium choose a temporary active tab
    // in its flat Browser tab strip. The pane model has a stronger successor
    // rule (same pane first, otherwise the pane that absorbs the old space),
    // so do not let that temporary selection overwrite it. CloseTabNow will
    // activate and focus the model-selected successor after removal settles.
    if (focused_web_tab_close_workspace_ == workspace) {
      return;
    }
    model_.SelectTab(workspace, it->second.pane, it->second.surface);
    if (workspace == ws_) {
      focus_.FocusPane(it->second.pane, /*move_keyboard=*/true);
    } else {
      model_.FocusPane(workspace, it->second.pane);
    }
    // Inactive workspaces can change through chrome.tabs or another native
    // Browser caller. Keep their retained pane view in sync too: the rail
    // snapshots that focused pane's selected favicon even while it is hidden.
    RefreshPaneTabs(it->second.pane);
  }

  void BeginFocusedWebTabClose(WorkspaceId workspace,
                               PaneId pane,
                               SurfaceTabId surface,
                               content::WebContents* contents) {
    focused_web_tab_close_workspace_ = workspace;
    focused_web_tab_close_pane_ = pane;
    focused_web_tab_close_surface_ = surface;
    focused_web_tab_close_observer_ =
        std::make_unique<PendingFocusedWebTabCloseObserver>(
            contents,
            base::BindOnce(&CmuxWindowView::CancelFocusedWebTabClose,
                           weak_factory_.GetWeakPtr(), workspace, pane,
                           surface));
  }

  void ClearFocusedWebTabClose() {
    focused_web_tab_close_observer_.reset();
    focused_web_tab_close_workspace_ = kInvalidId;
    focused_web_tab_close_pane_ = kInvalidId;
    focused_web_tab_close_surface_ = kInvalidId;
  }

  void CancelFocusedWebTabClose(WorkspaceId workspace,
                                PaneId pane_id,
                                SurfaceTabId surface) {
    if (focused_web_tab_close_workspace_ != workspace ||
        focused_web_tab_close_pane_ != pane_id ||
        focused_web_tab_close_surface_ != surface) {
      return;
    }
    ClearFocusedWebTabClose();
    // A cancelled beforeunload means the user chose to keep this tab. Do not
    // let autorepeat intents queued behind it skip ahead to other tabs.
    pending_focused_tab_closes_ = 0;
    Pane* pane = model_.FindPane(workspace, pane_id);
    if (!pane || pane->IndexOfTab(surface) < 0) {
      return;
    }
    model_.SelectTab(workspace, pane_id, surface);
    ActivateBrowserTabForSurface(surface);
    if (workspace == ws_) {
      RefreshPaneTabs(pane_id);
      focus_.FocusPane(pane_id, /*move_keyboard=*/true);
    } else {
      model_.FocusPane(workspace, pane_id);
    }
  }

  void CompleteFocusedWebTabClose(WorkspaceId workspace,
                                  SurfaceTabId surface) {
    if (focused_web_tab_close_workspace_ != workspace ||
        focused_web_tab_close_surface_ != surface) {
      return;
    }
    ClearFocusedWebTabClose();
    if (pending_workspace_closes_.contains(workspace) ||
        closing_workspaces_.contains(workspace)) {
      pending_focused_tab_closes_ = 0;
      return;
    }
    const Workspace* workspace_state = model_.GetWorkspace(workspace);
    const PaneId successor =
        workspace_state ? workspace_state->focused : kInvalidId;
    if (!model_.FindPane(workspace, successor)) {
      pending_focused_tab_closes_ = 0;
      return;
    }
    // Keep Chromium's flat tab strip aligned with the pane model after its
    // temporary removal selection has been suppressed.
    ActivateSelectedWebTabInPane(successor);
    if (workspace == ws_) {
      focus_.FocusPane(successor, /*move_keyboard=*/true);
    }
    ScheduleNextFocusedTabClose();
  }

  // ---- TabStripModelObserver ---------------------------------------------
  void OnTabWillBeRemoved(tabs::TabInterface* tab, int index) override {
    if (!tab) {
      return;
    }
    auto placement_it = web_tab_placements_.find(tab->GetContents());
    if (placement_it == web_tab_placements_.end()) {
      return;
    }
    CmuxPaneView* pane = ViewForPane(placement_it->second.pane);
    CmuxSurface* surface =
        pane ? pane->SurfaceFor(placement_it->second.surface) : nullptr;
    views::FocusManager* focus_manager = GetFocusManager();
    views::View* focused =
        focus_manager ? focus_manager->GetFocusedView() : nullptr;
    if (surface && focused && surface->AsView()->Contains(focused)) {
      // A web surface owns its own LocationBarView. Destroying that view while
      // its omnibox is focused calls OmniboxViewViews::OnBlur(), which reaches
      // SearchTabHelper and asks the tab whether it is pinned. This is the last
      // notification before TabStripModel detaches the TabModel, so blur now,
      // while owning_model() is still valid. Blurring later from
      // RemoveBrowserTabPlacement() CHECKs in TabModel::IsPinned().
      focus_manager->ClearFocus();
    }
  }

  void OnTabChangedAt(tabs::TabInterface* tab,
                      int,
                      TabChangeType change_type) override {
    if (!tab || change_type != TabChangeType::kAll) {
      return;
    }
    const auto placement = web_tab_placements_.find(tab->GetContents());
    if (placement == web_tab_placements_.end()) {
      return;
    }
    // kAll carries the title/URL, crash, discard, and ThumbnailImage changes
    // represented by TabData. Only the focused pane's selected surface can
    // affect its workspace card.
    NotifyWorkspaceHoverCardDataChanged(
        placement->second.workspace, placement->second.pane,
        placement->second.surface);
  }

  void OnTabStripModelChanged(
      TabStripModel* tab_strip_model,
      const TabStripModelChange& change,
      const TabStripSelectionChange& selection) override {
    const WorkspaceId workspace = WorkspaceForTabStripModel(tab_strip_model);
    if (workspace == kInvalidId) {
      return;
    }
    SurfaceTabId completed_focused_close = kInvalidId;

    if (internal_browser_mutation_depth_ == 0 &&
        change.type() == TabStripModelChange::kInserted) {
      const TabStripModelChange::Insert* insert = change.GetInsert();
      if (insert) {
        for (const auto& inserted : insert->contents) {
          AdoptBrowserTabIntoWorkspace(
              workspace, inserted.contents, inserted.index,
              inserted.contents == tab_strip_model->GetActiveWebContents());
        }
      }
    } else if (internal_browser_mutation_depth_ == 0 &&
               change.type() == TabStripModelChange::kRemoved) {
      const TabStripModelChange::Remove* remove = change.GetRemove();
      if (remove) {
        for (const auto& removed : remove->contents) {
          auto placement = web_tab_placements_.find(removed.contents);
          if (placement != web_tab_placements_.end() &&
              focused_web_tab_close_workspace_ == workspace &&
              focused_web_tab_close_surface_ == placement->second.surface) {
            completed_focused_close = placement->second.surface;
          }
          RemoveBrowserTabPlacement(removed.contents);
        }
        if (tab_strip_model->empty() &&
            WorkspaceHasTerminalSurface(workspace)) {
          suppress_empty_browser_close_once_.insert(workspace);
        }
      }
    } else if (internal_browser_mutation_depth_ == 0 &&
               change.type() == TabStripModelChange::kReplaced) {
      const TabStripModelChange::Replace* replace = change.GetReplace();
      if (replace) {
        ReplaceBrowserTabContents(replace->old_contents, replace->new_contents);
      }
    } else if (internal_browser_mutation_depth_ == 0 &&
               change.type() == TabStripModelChange::kMoved) {
      const TabStripModelChange::Move* move = change.GetMove();
      if (move) {
        ApplyBrowserTabMove(workspace, tab_strip_model, move->contents,
                            move->to_index);
      }
    }

    if (selection.active_tab_changed() && selection.new_contents) {
      OnWorkspaceBrowserActiveTabChanged(workspace, selection.old_contents,
                                         selection.new_contents);
    }
    if (completed_focused_close != kInvalidId) {
      // Other TabStripModel observers can report the same temporary active
      // tab after this observer returns. Keep suppression through the complete
      // removal notification and restore the pane-model successor next task.
      base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
          FROM_HERE,
          base::BindOnce(&CmuxWindowView::CompleteFocusedWebTabClose,
                         weak_factory_.GetWeakPtr(), workspace,
                         completed_focused_close));
    }
  }

  void OnTabStripModelDestroyed(TabStripModel* tab_strip_model) override {}

#if CHROME_VERSION_MAJOR >= 150
  void OnBrowserClosed(BrowserWindowInterface* browser_window) override {
    Browser* browser = browser_window->GetBrowserForMigrationOnly();
#else
  void OnBrowserRemoved(Browser* browser) override {
#endif
    if (!profile_services_available_ || !profile_) {
      return;
    }
    WorkspaceId removed_workspace = kInvalidId;
    for (const auto& [workspace, workspace_browser] : workspace_browsers_) {
      if (workspace_browser.get() == browser) {
        removed_workspace = workspace;
        break;
      }
    }
    if (removed_workspace != kInvalidId) {
      FinalizeClosedWorkspace(removed_workspace, browser);
    }
  }

  // Watch focus so we can close any omnibox popup whose omnibox just lost
  // focus (upstream's blur-closes-popup behavior; prevents the orphaned-popup
  // crash).
  void AddedToWidget() override {
    GetWidget()->AddObserver(this);
    if (views::FocusManager* fm = GetFocusManager()) {
      fm->AddFocusChangeListener(this);
      // The mac NSEvent monitor still handles matched chords before the
      // renderer; registering accelerators there too lets unhandled renderer
      // keys re-dispatch through the standard FocusManager path. The monitor
      // can retire once PreHandle+accelerators are proven across the app.
      RegisterChords(fm);
    }
  }
  void RemovedFromWidget() override {
    GetWidget()->RemoveObserver(this);
    if (views::FocusManager* fm = GetFocusManager()) {
      fm->RemoveFocusChangeListener(this);
      fm->UnregisterAccelerators(this);
    }
  }

  void OnWidgetActivationChanged(views::Widget* widget, bool active) override {
    if (widget == GetWidget() && !active) {
      CancelActiveDragForHostState();
    }
    Browser* browser = BrowserForWorkspace(ws_);
    if (active) {
      if (browser) {
        browser->DidBecomeActive();
      }
      QueuePendingTuiTerminalFocus();
    } else if (browser) {
      browser->DidBecomeInactive();
    }
  }

  void OnWidgetVisibilityChanged(views::Widget* widget,
                                 bool visible) override {
    if (widget == GetWidget() && !visible) {
      CancelActiveDragForHostState();
    }
  }

  void OnWidgetShowStateChanged(views::Widget* widget) override {
    if (widget == GetWidget()) {
      if (widget->IsMinimized()) {
        CancelActiveDragForHostState();
      }
      RefreshRoundedFrames();
    }
  }

  void OnBoundsAnimatorProgressed(views::BoundsAnimator* animator) override {
    if (animator == root_animator_.get()) {
      UpdateWindowControlsOcclusion();
      UpdatePaneTitlebarInsets();
      CheckSidebarSelfTestFrame(/*midflight=*/true);
    }
    if (!anim_selftest_active_ || anim_selftest_mid_checked_ ||
        animator != animator_.get()) {
      return;
    }
    CmuxPaneView* view = ViewForPane(anim_selftest_pane_);
    CHECK(view);
    const int mid_x = view->bounds().x();
    // The expo curve covers ~50% by the first ~16ms tick, so the first
    // progress callback must sit strictly between the endpoints.
    CHECK_NE(mid_x, anim_selftest_start_x_)
        << "CMUX_ANIM_SELFTEST pane did not move";
    CHECK_NE(mid_x, anim_selftest_final_x_)
        << "CMUX_ANIM_SELFTEST pane snapped to final";
    anim_selftest_mid_checked_ = true;
    LOG(WARNING) << "cmux-views: CMUX_ANIM_SELFTEST PASS mid-flight motion "
                 << "start=" << anim_selftest_start_x_ << " mid=" << mid_x
                 << " final=" << anim_selftest_final_x_;
  }
  void OnBoundsAnimatorDone(views::BoundsAnimator* animator) override {
    if (animator == root_animator_.get()) {
      UpdateWindowControlsOcclusion();
      UpdatePaneTitlebarInsets();
      ApplyVisualsAndScroll(/*animated=*/false);
      FinishSidebarSelfTestTransition();
    }
    if (anim_selftest_active_ && animator == animator_.get()) {
      CHECK(anim_selftest_mid_checked_)
          << "CMUX_ANIM_SELFTEST finished without a progress tick";
      anim_selftest_active_ = false;
      animator_->RemoveObserver(this);
      CmuxPaneView* view = ViewForPane(anim_selftest_pane_);
      CHECK(view);
      LOG(WARNING) << "cmux-views: CMUX_ANIM_SELFTEST SURVIVED late="
                   << view->bounds().x() << " final=" << anim_selftest_final_x_;
    }
  }

  void RegisterChords(views::FocusManager* fm) {
    std::set<ui::Accelerator> registered;
    for (const KeyRule& rule : keymap_.rules()) {
      std::optional<ui::Accelerator> accelerator =
          AcceleratorForChord(rule.chord, IsMacKeymap());
      if (!accelerator || !registered.insert(*accelerator).second) {
        continue;
      }
      fm->RegisterAccelerator(*accelerator,
                              ui::AcceleratorManager::kNormalPriority, this);
    }
  }

  bool AcceleratorPressed(const ui::Accelerator& accelerator) override {
    std::optional<KeyChord> chord = ChordFromAccelerator(accelerator);
    if (!chord) {
      return false;
    }
    return HandleKeyChord(*chord, CurrentKeyContext(), accelerator.IsRepeat());
  }

  void OnWillChangeFocus(views::View* before, views::View* now) override {}
  void OnDidChangeFocus(views::View* before, views::View* now) override {
    for (auto& kv : views_) {
      kv.second->CloseOmniboxPopupUnlessFocused(now);
    }
    // A click inside a pane's web page / terminal / omnibox focuses some view
    // within that pane; make that pane the focused column (border + niri
    // scroll-into-view). The pane content already has the keyboard focus from
    // the click, so OnPaneActivated updates visuals only -- it never re-enters
    // the FocusManager from inside this notification.
    ActivatePaneContaining(now);
  }

  // ---- CmuxStripController: columns -----------------------------------------
  void AddChromeColumn(const GURL& url) override {
    // A freshly seeded workspace already has a model pane with no view; bind
    // to it first so the workspace's initial column is the one we fill.
    PaneId pane = FirstViewlessPane();
    bool created_column = false;
    if (pane == kInvalidId) {
      pane = model_.AddColumn(ws_, SurfaceKind::kWeb);
      created_column = true;
    }
    if (pane == kInvalidId) {
      return;
    }
    RealizePane(pane, url);
    if (created_column) {
      MarkPaneForEntrance(pane);
    }
    Pane* model_pane = model_.FindPane(ws_, pane);
    if (model_pane) {
      FocusNewWebTab(pane, model_pane->selected);
    }
  }

  void AddTerminalColumn() override {
    AddTerminalColumn(/*user_action=*/false);
  }

  void AddTerminalColumn(bool user_action) {
#if BUILDFLAG(IS_MAC) || BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_WIN)
    if (!model_.GetWorkspace(ws_)) {
      ++pending_terminal_columns_;
      return;
    }
    // Startup restoration owns an empty pane, not a placeholder web tab.
    for (PaneId empty : model_.PanesOf(ws_)) {
      if (model_.FindPane(ws_, empty)->tabs.empty()) {
        NewTabInPane(empty, SurfaceKind::kTerminal);
        return;
      }
    }
    PaneId pane = model_.AddColumn(ws_, SurfaceKind::kTerminal);
    if (pane == kInvalidId) {
      return;
    }
    RealizePane(pane, GURL());
    if (user_action) {
      MarkPaneForEntrance(pane);
    }
    focus_.OnPaneAdded(pane);
#else
    LOG(WARNING) << "cmux: terminal panes not supported on this platform yet";
#endif
  }

  void FocusColumn(int index) override {
    for (PaneId p : model_.PanesOf(ws_)) {
      if (model_.ColumnIndexOf(ws_, p) == index) {
        focus_.FocusPane(p, /*move_keyboard=*/true);
        return;
      }
    }
  }

  void ToggleFullscreen() override {
    views::Widget* widget = GetWidget();
    if (widget) {
      widget->SetFullscreen(!widget->IsFullscreen());
    }
  }

  void ToggleSidebar() override {
    CycleSidebarMode(/*persist=*/true);
  }

  void CycleSidebarMode(bool persist) {
    SetSidebarMode(sidebar_metrics::NextMode(layout_config_.sidebar_mode),
                   /*animated=*/true, persist);
  }

  void SetSidebarMode(SidebarMode mode,
                      bool animated,
                      bool persist,
                      bool cancel_active_drag = true) {
    if (layout_config_.sidebar_mode == mode) {
      return;
    }
    if (cancel_active_drag) {
      CancelActiveDragForMutation();
    }
    const PaneId focused = FocusedPane();
    layout_config_.sidebar_mode = mode;
    if (persist) {
      SaveLayoutConfig(layout_config_);
    }
    ApplyRailLayoutConfig();

    const gfx::Rect target_content =
        ContentBoundsForSidebarMode(layout_config_.sidebar_mode);
    LayoutSidebarChrome(animated);

    strip_width_override_ = target_content.width();
    if (focused != kInvalidId && model_.FindPane(ws_, focused)) {
      model_.FocusPane(ws_, focused);
    }
    ApplyVisualsAndScroll(animated);
    strip_width_override_ = -1;
  }

  void SetSidebarPosition(SidebarPosition position) {
    if (layout_config_.sidebar_position == position) {
      return;
    }
    CancelActiveDragForMutation();
    layout_config_.sidebar_position = position;
    SaveLayoutConfig(layout_config_);
    ApplyRailLayoutConfig();
    // Switching edges should be immediate: animating an expanded rail between
    // opposite sides would sweep it across the browser content.
    LayoutSidebarChrome(/*animated=*/false);
    ApplyVisualsAndScroll(/*animated=*/false);
  }

  void CycleWorkspace(int delta) override {
    std::vector<RailItem> rail = model_.BuildRail();
    if (rail.empty()) {
      return;
    }
    int current = -1;
    for (size_t i = 0; i < rail.size(); ++i) {
      if (rail[i].workspace == ws_) {
        current = static_cast<int>(i);
        break;
      }
    }
    if (current < 0) {
      current = 0;
    }
    const int count = static_cast<int>(rail.size());
    int next = (current + delta) % count;
    if (next < 0) {
      next += count;
    }
    SwitchWorkspace(rail[next].workspace);
  }

  void ToggleWorkspaceLayout() {
    CancelActiveDragForMutation();
    model_.ToggleWorkspaceLayoutMode(ws_);
    MarkWorkspaceProjectionDirty();
    ApplyVisualsAndScroll(
        /*animated=*/true, ScrollIntoViewPolicy::kEnsureVisibleWithMargin);
  }

  void JumpToWorkspace(int index) override {
    if (index < 0) {
      return;
    }
    std::vector<RailItem> rail = model_.BuildRail();
    if (index >= static_cast<int>(rail.size())) {
      return;
    }
    SwitchWorkspace(rail[index].workspace);
  }

  void MoveFocus(int delta) override {
    MoveFocusDirection(delta < 0 ? Direction::kLeft : Direction::kRight);
  }
  void MoveFocusVertical(int delta) override {
    MoveFocusDirection(delta < 0 ? Direction::kUp : Direction::kDown);
  }
  void CycleColumnWidth() override {
    const PaneId focused = FocusedPane();
    if (model_.CycleColumnWidth(ws_, focused, layout_config_.column_width_modes,
                                CurrentLayoutMetrics().column_fraction) <=
        0.0) {
      return;
    }
    ApplyVisualsAndScroll(
        /*animated=*/true, ScrollIntoViewPolicy::kEnsureVisibleWithMargin);
  }

  // ---- CmuxStripController: splits ------------------------------------------
  void SplitFocused(SplitOrientation orientation) override {
    const PaneId focused = FocusedPane();
    CmuxPaneView* view = ViewForPane(focused);
    if (!view) {
      return;
    }
    // The new pane's first tab mirrors the focused tab's kind (cmux behavior:
    // splitting a terminal makes a terminal, splitting a page makes a page).
    CmuxSurface* selected = view->selected_surface();
    const SurfaceKind kind = selected ? selected->kind() : SurfaceKind::kWeb;
    // A right split is a new niri column, not a second pane nested inside the
    // source column. This gives each side its own stable column-width slot, so
    // focusing either side and cycling its width affects only that column.
    // A down split remains a bonsplit inside the current column.
    const PaneId fresh =
        orientation == SplitOrientation::kHorizontal
            ? model_.AddColumn(ws_, kind,
                               model_.ColumnIndexOf(ws_, focused) + 1)
            : model_.SplitPane(ws_, focused, orientation, 0.5, kind);
    if (fresh == kInvalidId) {
      return;
    }
    RealizePane(fresh, GURL());
    if (orientation == SplitOrientation::kVertical) {
      MarkPaneForSplitEntrance(fresh, orientation, /*insert_first=*/false);
    } else {
      MarkPaneForEntrance(fresh);
    }
    if (kind == SurfaceKind::kWeb) {
      Pane* model_pane = model_.FindPane(ws_, fresh);
      if (model_pane) {
        FocusNewWebTab(fresh, model_pane->selected);
      }
    } else {
      focus_.OnPaneAdded(fresh);
    }
  }

  // ---- CmuxStripController: tabs --------------------------------------------
  void NewTab(SurfaceKind kind) override { NewTabInPane(FocusedPane(), kind); }

  void NewTerminalTab(std::string command, std::string title) override {
    NewTabInPane(FocusedPane(), SurfaceKind::kTerminal, std::move(command),
                 std::move(title));
  }

  void SelectAdjacentTab(int delta) override {
    const PaneId focused = FocusedPane();
    Pane* pane = model_.FindPane(ws_, focused);
    if (!pane || pane->tabs.size() < 2) {
      return;
    }
    int index = pane->IndexOfTab(pane->selected);
    index = (index + delta + static_cast<int>(pane->tabs.size())) %
            static_cast<int>(pane->tabs.size());
    model_.SelectTab(ws_, focused, pane->tabs[index].id);
    ActivateBrowserTabForSurface(pane->tabs[index].id);
    RefreshPaneTabs(focused);
    focus_.FocusPane(focused, /*move_keyboard=*/true);
  }

  // Chrome-style numbered tab selection on the focused pane. 1..8 select an
  // exact position; 9 always selects the last tab, even when there are more
  // than nine.
  void SelectTabByNumber(int number) {
    const PaneId focused = FocusedPane();
    Pane* pane = model_.FindPane(ws_, focused);
    if (!pane || pane->tabs.empty() || number < 1 || number > 9) {
      return;
    }
    const int index =
        number == 9 ? static_cast<int>(pane->tabs.size()) - 1 : number - 1;
    if (index >= static_cast<int>(pane->tabs.size())) {
      return;
    }
    const SurfaceTabId tab = pane->tabs[index].id;
    model_.SelectTab(ws_, focused, tab);
    ActivateBrowserTabForSurface(tab);
    RefreshPaneTabs(focused);
    focus_.FocusPane(focused, /*move_keyboard=*/true);
  }

  // Close the focused pane's selected tab. Deferred: Cmd-W arrives through the
  // NSEvent monitor, which runs INSIDE -[NSApplication sendEvent:]; destroying
  // the tab's WebContents and the RenderWidgetHostViewCocoa NSView that AppKit
  // still holds on the event stack right here is a use-after-free. Queue close
  // intents rather than snapshotting the selected tab in every repeat event:
  // a focused web-tab close completes asynchronously, so an autorepeat burst
  // would otherwise post several no-op closes for the same tab.
  void CloseFocused() override {
    ++pending_focused_tab_closes_;
    ScheduleNextFocusedTabClose();
  }

  void ScheduleNextFocusedTabClose() {
    if (focused_tab_close_task_pending_ || pending_focused_tab_closes_ == 0 ||
        focused_web_tab_close_workspace_ != kInvalidId) {
      return;
    }
    if (pending_workspace_closes_.contains(ws_) ||
        closing_workspaces_.contains(ws_)) {
      pending_focused_tab_closes_ = 0;
      return;
    }
    focused_tab_close_task_pending_ = true;
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE,
        base::BindOnce(&CmuxWindowView::RunNextFocusedTabClose,
                       weak_factory_.GetWeakPtr()));
  }

  void RunNextFocusedTabClose() {
    focused_tab_close_task_pending_ = false;
    if (pending_focused_tab_closes_ == 0 ||
        focused_web_tab_close_workspace_ != kInvalidId) {
      return;
    }
    if (pending_workspace_closes_.contains(ws_) ||
        closing_workspaces_.contains(ws_)) {
      pending_focused_tab_closes_ = 0;
      return;
    }
    const PaneId focused = FocusedPane();
    Pane* pane = model_.FindPane(ws_, focused);
    if (!pane || pane->IndexOfTab(pane->selected) < 0) {
      pending_focused_tab_closes_ = 0;
      return;
    }
    const SurfaceTabId selected = pane->selected;
    --pending_focused_tab_closes_;
    CloseTabNow(focused, selected);
    if (focused_web_tab_close_workspace_ == kInvalidId) {
      ScheduleNextFocusedTabClose();
    }
  }

  // Defer out of the macOS NSEvent monitor just like tab closure. Destroying
  // the NSWindow while AppKit is still dispatching Cmd+Shift+W leaves native
  // responders from that window on the event stack.
  void CloseCurrentNativeWindow() {
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(
                       [](base::WeakPtr<CmuxWindowView> window) {
                         if (!window) {
                           return;
                         }
                         if (views::Widget* widget = window->GetWidget()) {
                           // The shared Chrome bubble coordinator may still
                           // hold an anchor from this widget. Dismiss it while
                           // the anchor and active workspace Browser are both
                           // alive.
                           CloseCmuxExtensionsUi();
                           widget->Close();
                         }
                       },
                       weak_factory_.GetWeakPtr()));
  }

  // ---- CmuxStripController: selected surface --------------------------------
  void OpenDevToolsForFocused() override {
    if (CmuxPane* p = FocusedView()) {
      p->ToggleDevTools();
    }
  }
  void UndockDevToolsForFocused() override {
    if (CmuxPane* p = FocusedView()) {
      p->UndockDevTools();
    }
  }
  void GoBackFocused() override {
    if (CmuxPane* p = FocusedView()) {
      p->GoBack();
    }
  }
  void GoForwardFocused() override {
    if (CmuxPane* p = FocusedView()) {
      p->GoForward();
    }
  }
  void ReloadFocused() override {
    if (CmuxPane* p = FocusedView()) {
      p->Reload();
    }
  }
  bool HandleEscape() override {
    if (drag_controller_ && drag_controller_->active()) {
      CancelActiveDragForMutation();
      return true;
    }
    CmuxPane* p = FocusedView();
    return p && p->HandleOmniboxEscape();
  }
  KeyContext CurrentKeyContext() override {
    KeyContext context;
    context.drag_active = drag_controller_ && drag_controller_->active();
    context.sidebar_visible =
        sidebar_metrics::IsVisible(layout_config_.sidebar_mode);
    context.pane_count = static_cast<int>(model_.PanesOf(ws_).size());
    context.pane_count_gt_one = context.pane_count > 1;
    context.workspace_count = static_cast<int>(model_.workspaces().size());
#if BUILDFLAG(IS_MAC)
    context.is_mac = true;
#elif BUILDFLAG(IS_WIN)
    context.is_windows = true;
#elif BUILDFLAG(IS_LINUX)
  context.is_linux = true;
#endif

    Pane* pane = model_.FindPane(ws_, FocusedPane());
    SurfaceTab* tab = pane ? pane->FindTab(pane->selected) : nullptr;
    context.web_focused = tab && tab->kind == SurfaceKind::kWeb;
    context.terminal_focused = tab && tab->kind == SurfaceKind::kTerminal;
    context.surface_kind = context.terminal_focused ? "terminal" : "web";
    context.tab_count = pane ? static_cast<int>(pane->tabs.size()) : 0;

    views::FocusManager* fm = GetFocusManager();
    views::View* focused = fm ? fm->GetFocusedView() : nullptr;
    if (focused) {
      context.omnibox_focused =
          std::string_view(focused->GetClassName()) == "OmniboxViewViews";
      context.text_input_focused = context.omnibox_focused;
    }
    return context;
  }
  bool HandleKeyChord(const KeyChord& chord,
                      const KeyContext& context,
                      bool is_repeat) override {
    if (!pending_chord_.empty()) {
      std::vector<KeyChord> sequence = pending_chord_;
      sequence.push_back(chord);
      pending_chord_.clear();
      pending_chord_timer_.Stop();
      std::optional<ResolvedKeybinding> binding =
          keymap_.ResolveSequence(sequence, context, IsMacKeymap());
      if (!binding || binding->command.empty()) {
        return true;
      }
      return ExecuteKeyCommand(binding->command, is_repeat, binding->args_json);
    }
    if (keymap_.HasChordPrefix({chord}, context, IsMacKeymap())) {
      pending_chord_ = {chord};
      pending_chord_timer_.Start(
          FROM_HERE, base::Seconds(5),
          base::BindOnce(&CmuxWindowView::ClearPendingChord,
                         weak_factory_.GetWeakPtr()));
      return true;
    }
    std::optional<ResolvedKeybinding> binding =
        keymap_.ResolveSequence({chord}, context, IsMacKeymap());
    if (binding) {
      return binding->command.empty()
                 ? true
                 : ExecuteKeyCommand(binding->command, is_repeat,
                                     binding->args_json);
    }
    return false;
  }
  bool HandleReservedKeyChord(const KeyChord& chord,
                              const KeyContext& context,
                              bool is_repeat) override {
    std::optional<ResolvedKeybinding> binding =
        keymap_.ResolveSequence({chord}, context, IsMacKeymap());
    if (!binding || binding->command.empty() ||
        !IsReservedCommand(binding->command)) {
      return false;
    }
    return ExecuteKeyCommand(binding->command, is_repeat, binding->args_json);
  }
  void ReloadKeymap() override { LoadKeymapAsync(); }
  void ReloadTheme() override {
    if (!profile_services_available_) {
      return;
    }
    const uint64_t request_id = ++ghostty_theme_load_request_;
    LayoutConfig defaults;
    ApplyCmuxAppLayoutDefaults(&defaults);
    LoadLayoutConfigAsync(
        defaults, base::BindOnce(&CmuxWindowView::OnThemeReloadLayoutLoaded,
                                 weak_factory_.GetWeakPtr(), request_id,
                                 defaults));
  }

  void ApplyPublishedCustomization() {
    if (!profile_services_available_) {
      return;
    }
    std::optional<LayoutConfig> config = GetPublishedLayoutConfig();
    if (!config) {
      ReloadTheme();
      return;
    }
    layout_config_ = std::move(*config);
    const uint64_t request_id = ++ghostty_theme_load_request_;
    ContinueThemeReload(request_id);
  }

  void ApplyResolvedPublishedCustomization(
      const LayoutConfig& config,
      const std::optional<CmuxTheme>& theme) {
    if (!profile_services_available_) {
      return;
    }
    // Cancel any older startup/reload result before publishing the one shared
    // resolution to every window.
    ++ghostty_theme_load_request_;
    layout_config_ = config;
    ApplyLoadedGhosttyTheme(theme,
                            /*log_selftest=*/ThemeSelfTestEnabled());
    ReloadOpenNewTabPagesForTheme();
  }

  void ContinueThemeReload(uint64_t request_id) {
    if (request_id != ghostty_theme_load_request_) {
      return;
    }
    if (!GhosttyThemeEnabled()) {
      ApplyLoadedGhosttyTheme(std::nullopt,
                              /*log_selftest=*/ThemeSelfTestEnabled());
      return;
    }
    BeginGhosttyThemeLoad(request_id, GhosttyThemeLoadKind::kReload);
  }
  void FocusActiveOmnibar() override {
    if (CmuxPane* p = FocusedView()) {
      p->FocusOmnibar();
    }
  }
  std::u16string E2EFocusedOmniboxText() override {
    CmuxPane* p = FocusedView();
    return p ? p->E2EOmniboxText() : std::u16string();
  }
  void E2ENavigateFocused(const GURL& url) override {
    CmuxPane* p = FocusedView();
    content::WebContents* wc = p ? p->GetInspectableWebContents() : nullptr;
    if (wc) {
      wc->GetController().LoadURL(url, content::Referrer(),
                                  ui::PAGE_TRANSITION_TYPED, std::string());
    }
  }
  std::string E2EFocusedCommittedURL() override {
    CmuxPane* p = FocusedView();
    content::WebContents* wc = p ? p->GetInspectableWebContents() : nullptr;
    if (!wc) {
      return std::string();
    }
    // The REAL committed frame URL (post BrowserURLHandler rewrite), plus the
    // virtual (displayed) URL and title -- WebContents::GetLastCommittedURL()
    // alone reports the virtual URL, which hides whether a rewrite happened.
    return "real=" + wc->GetPrimaryMainFrame()->GetLastCommittedURL().spec() +
           " virtual=" + wc->GetLastCommittedURL().spec() +
           " title=" + base::UTF16ToUTF8(wc->GetTitle());
  }

  content::WebContents* ActiveWebContentsForExtensions() {
    CmuxPane* p = FocusedView();
    return p ? p->GetInspectableWebContents() : nullptr;
  }

  // ---- CmuxPaneView::Delegate (tab strip intents)
  // ----------------------------
  void OnSelectTabRequested(PaneId pane, SurfaceTabId tab) override {
    model_.SelectTab(ws_, pane, tab);
    ActivateBrowserTabForSurface(tab);
    RefreshPaneTabs(pane);
    focus_.FocusPane(pane, /*move_keyboard=*/true);
  }
  void OnCloseTabRequested(PaneId pane, SurfaceTabId tab) override {
    CloseTabDeferred(pane, tab);
  }
  void OnCloseOtherTabsRequested(PaneId pane, SurfaceTabId tab) override {
    CloseOtherTabsDeferred(pane, tab);
  }
  void OnNewTabRequested(PaneId pane, SurfaceKind kind) override {
    NewTabInPane(pane, kind);
  }
  std::optional<ui::Accelerator> GetNewTabAccelerator(
      SurfaceKind kind) override {
    const std::string_view command =
        kind == SurfaceKind::kTerminal ? "tab.newTerminal" : "tab.newWeb";
    const KeyContext context = CurrentKeyContext();
    std::optional<std::vector<KeyChord>> sequence =
        keymap_.PrimaryBindingForCommand(command, context, IsMacKeymap());
    if (sequence && sequence->size() == 1) {
      return AcceleratorForChord(sequence->front(), IsMacKeymap());
    }
    return std::nullopt;
  }
  bool IsSidebarOnRight() const override { return SidebarOnRight(); }
  void OnSetSidebarOnRightRequested(bool on_right) override {
    SetSidebarPosition(on_right ? SidebarPosition::kRight
                                : SidebarPosition::kLeft);
  }
  void OnNewTabRightRequested(PaneId pane, SurfaceTabId tab) override {
    NewTabRightDeferred(pane, tab);
  }
  void OnMoveTabToNewColumnRequested(PaneId pane, SurfaceTabId tab) override {
    MoveTabToNewColumnDeferred(pane, tab);
  }
  void OnSplitRightWithTabRequested(PaneId pane, SurfaceTabId tab) override {
    SplitWithTabDeferred(pane, tab, SplitOrientation::kHorizontal);
  }
  void OnSplitDownWithTabRequested(PaneId pane, SurfaceTabId tab) override {
    SplitWithTabDeferred(pane, tab, SplitOrientation::kVertical);
  }
  bool IsTabContextActionEnabled(PaneId pane_id,
                                 SurfaceTabId tab,
                                 TabContextAction action) const override {
    const Pane* pane = model_.FindPane(ws_, pane_id);
    const int pane_index = pane ? pane->IndexOfTab(tab) : -1;
    if (!pane || pane_index < 0) {
      return false;
    }
    switch (action) {
      case TabContextAction::kNewTabRight:
      case TabContextAction::kNewSplitWithCurrentTab:
        return true;
      case TabContextAction::kClose:
        // The model cannot hold an empty final pane. Closing its last terminal
        // inserts a web fallback first, while the canonical terminal close is
        // still issued immediately.
        return true;
      case TabContextAction::kCloseOtherTabs:
        return pane->tabs.size() > 1;
      case TabContextAction::kCloseTabsToLeft:
        return pane_index > 0;
      case TabContextAction::kCloseTabsToRight:
        return pane_index >= 0 &&
               pane_index + 1 < static_cast<int>(pane->tabs.size());
      case TabContextAction::kCopyUrl: {
        content::WebContents* contents = WebContentsForSurface(tab);
        return contents && contents->GetVisibleURL().is_valid();
      }
      default:
        break;
    }

    int browser_index = TabStripModel::kNoTab;
    TabStripModel* tabs = BrowserTabsForSurface(tab, &browser_index);
    if (!tabs || browser_index == TabStripModel::kNoTab) {
      return false;
    }
    if (action == TabContextAction::kHibernate) {
      resource_coordinator::TabLifecycleUnitExternal* lifecycle =
          resource_coordinator::TabLifecycleUnitExternal::FromWebContents(
              tabs->GetWebContentsAt(browser_index));
      return lifecycle && browser_index != tabs->active_index() &&
             lifecycle->IsAutoDiscardable();
    }
    const std::optional<TabStripModel::ContextMenuCommand> command =
        ChromeContextCommand(action);
    return command.has_value() &&
           tabs->IsContextMenuCommandEnabled(browser_index, *command);
  }

  bool IsTabContextActionToggled(PaneId pane,
                                 SurfaceTabId tab,
                                 TabContextAction action) const override {
    int browser_index = TabStripModel::kNoTab;
    TabStripModel* tabs = BrowserTabsForSurface(tab, &browser_index);
    if (!tabs || browser_index == TabStripModel::kNoTab) {
      return false;
    }
    switch (action) {
      case TabContextAction::kTogglePinned:
        return tabs->IsTabPinned(browser_index);
      case TabContextAction::kToggleSiteMuted:
        return !tabs->WillContextMenuMuteSites(browser_index);
      default:
        return false;
    }
  }

  void OnTabContextActionRequested(PaneId pane,
                                   SurfaceTabId tab,
                                   TabContextAction action) override {
    switch (action) {
      case TabContextAction::kNewSplitWithCurrentTab:
        NewSplitWithCurrentTabDeferred(pane, tab);
        return;
      case TabContextAction::kCloseTabsToLeft:
      case TabContextAction::kCloseTabsToRight:
        CloseTabsOnSideDeferred(pane, tab,
                                action == TabContextAction::kCloseTabsToLeft);
        return;
      case TabContextAction::kHibernate:
        HibernateTab(tab);
        return;
      case TabContextAction::kCopyUrl: {
        content::WebContents* contents = WebContentsForSurface(tab);
        if (contents && contents->GetVisibleURL().is_valid()) {
          ui::ScopedClipboardWriter(ui::ClipboardBuffer::kCopyPaste)
              .WriteText(base::UTF8ToUTF16(contents->GetVisibleURL().spec()));
        }
        return;
      }
      default:
        break;
    }

    int browser_index = TabStripModel::kNoTab;
    TabStripModel* tabs = BrowserTabsForSurface(tab, &browser_index);
    const std::optional<TabStripModel::ContextMenuCommand> command =
        ChromeContextCommand(action);
    if (tabs && browser_index != TabStripModel::kNoTab && command &&
        tabs->IsContextMenuCommandEnabled(browser_index, *command)) {
      tabs->ExecuteContextMenuCommand(browser_index, *command);
    }
  }
  void OnTabDragStarted(PaneId pane,
                        SurfaceTabId tab,
                        const gfx::Point& screen_pt,
                        const gfx::Point& grab_offset) override {
    Pane* p = model_.FindPane(ws_, pane);
    SurfaceTab* t = p ? p->FindTab(tab) : nullptr;
    if (!t || !drag_controller_) {
      return;
    }
    drag_controller_->StartTabDrag(pane, tab, t->title, t->kind, screen_pt,
                                   grab_offset);
  }
  void OnTabDragUpdated(const gfx::Point& screen_pt) override {
    if (drag_controller_) {
      drag_controller_->Update(screen_pt);
    }
  }
  void OnTabDragEnded(bool commit) override {
    if (drag_controller_) {
      drag_controller_->End(commit);
      FlushDeferredPaneTabs();
    }
  }
  bool OnLiveReorderTab(PaneId pane,
                        SurfaceTabId tab,
                        int final_index) override {
    Pane* model_pane = model_.FindPane(ws_, pane);
    if (!model_pane) {
      return false;
    }
    const int old_index = model_pane->IndexOfTab(tab);
    if (old_index < 0 || old_index == final_index ||
        model_.ReorderTab(ws_, pane, tab, final_index) == kInvalidId) {
      return false;
    }
    // Keep chrome.tabs ordering and extension-visible indices synchronized
    // with the live cmux model move. The strip owns the in-flight visuals, so
    // its normal model refresh remains deferred until the drag settles.
    SyncBrowserTabOrder(ws_);
    RefreshPaneTabs(pane);
    return true;
  }
  void OnPaneDragStarted(PaneId pane,
                         const gfx::Point& screen_pt,
                         const gfx::Point& grab_offset) override {
    if (drag_controller_) {
      drag_controller_->StartPaneDrag(pane, screen_pt, grab_offset);
    }
  }
  void OnPaneDragUpdated(const gfx::Point& screen_pt) override {
    if (drag_controller_) {
      drag_controller_->Update(screen_pt);
    }
  }
  void OnPaneDragEnded(bool commit) override {
    if (drag_controller_) {
      drag_controller_->End(commit);
      FlushDeferredPaneTabs();
    }
  }
  void OnTabTitleChanged(PaneId pane,
                         SurfaceTabId tab,
                         const std::u16string& title) override {
    const WorkspaceId workspace = WorkspaceForPane(pane);
    Pane* p = model_.FindPane(workspace, pane);
    if (!p) {
      return;
    }
    SurfaceTab* t = p->FindTab(tab);
    if (!t) {
      return;
    }
    const std::string utf8 = base::UTF16ToUTF8(title);
    if (t->title == utf8) {
      // A committed navigation can keep the same display title while changing
      // the URL/domain carried by Chromium's TabData.
      NotifyWorkspaceHoverCardDataChanged(workspace, pane, tab);
      return;
    }
    t->title = utf8;
    RefreshPaneTabs(pane);
  }
  void OnTabFaviconChanged(PaneId pane, SurfaceTabId) override {
    RefreshPaneTabs(pane);
  }
  void OnTabLoadingChanged(PaneId pane,
                           SurfaceTabId tab,
                           bool loading) override {
    const WorkspaceId workspace = WorkspaceForPane(pane);
    if (model_.SetTabLoading(workspace, pane, tab, loading)) {
      RefreshPaneTabs(pane);
    }
  }
  void OnSurfaceInteraction(PaneId pane) override {
    FencePendingTuiTerminalFocusForGuiInteraction(pane);
  }

  void Layout(PassKey) override {
    ApplyRailLayoutConfig();
    const gfx::Rect content_bounds =
        ContentBoundsForSidebarMode(layout_config_.sidebar_mode);
    const gfx::Rect rail_bounds =
        RailBoundsForSidebarMode(layout_config_.sidebar_mode);
    const gfx::Rect handle_bounds =
        RailResizeHandleBoundsForSidebarMode(layout_config_.sidebar_mode);
    const bool viewport_geometry_changed =
        !have_sidebar_layout_target_ ||
        content_bounds != last_content_layout_target_ ||
        rail_bounds != last_rail_layout_target_ ||
        handle_bounds != last_rail_resize_handle_layout_target_;
    if (viewport_geometry_changed) {
      LayoutSidebarChrome(/*animated=*/false);
      ApplyVisualsAndScroll(/*animated=*/false);
    }
    LayoutSuperclass<views::View>(this);
  }

 private:
  int ExpandedRailWidth() const {
    return sidebar_metrics::ClampWidth(layout_config_.rail_width);
  }

  int CurrentRailWidth() const {
    return rail_ ? rail_->width()
                 : RailWidthForMode(layout_config_.sidebar_mode);
  }

  int HeaderHeight() const {
    return std::max(18, layout_config_.header_height);
  }

  bool SidebarOnRight() const {
    return layout_config_.sidebar_position == SidebarPosition::kRight;
  }

  int RailWidthForMode(SidebarMode mode) const {
    const int mode_width = sidebar_metrics::WidthForMode(
        mode, layout_config_.rail_width, layout_config_.sidebar_icon_width);
    return std::min(mode_width, std::max(0, width()));
  }

  int WindowControlsClearance() const {
#if BUILDFLAG(IS_MAC)
    // Match Helium's BrowserFrameViewMac::GetBrowserLayoutParams(): fullscreen
    // has no caption-button exclusion because the traffic lights move to a
    // separate system-owned pane. Keeping the normal-window clearance here
    // would leave the first pane's tabs indented after entering fullscreen.
    if (GetWidget() && GetWidget()->IsFullscreen()) {
      return 0;
    }
    const int fallback = 72;
    const int clearance = base_rail_config_.traffic_light_clearance;
    return std::max(fallback, clearance);
#else
    return 0;
#endif
  }

  void UpdateWindowControlsOcclusion() {
    if (!window_controls_occlusion_ || !content_) {
      return;
    }
    gfx::Rect occlusion(0, 0, std::min(width(), WindowControlsClearance()),
                        std::min(height(), std::max(40, HeaderHeight())));
    occlusion.Intersect(content_->bounds());
    window_controls_occlusion_->SetBoundsRect(occlusion);
    window_controls_occlusion_->SetVisible(!occlusion.IsEmpty());
  }

  void UpdatePaneTitlebarInsets() {
    if (!content_) {
      return;
    }
    const int controls_clearance = WindowControlsClearance();
    const int controls_height = std::max(40, HeaderHeight());
    for (const auto& [pane, logical_bounds] : pane_titlebar_geometry_) {
      CmuxPaneView* view = ViewForPane(pane);
      if (!view || !view->GetVisible() || !view->tab_strip()) {
        continue;
      }
      const int pane_window_x = content_->x() + logical_bounds.x();
      int inset = 0;
      if (controls_clearance > 0 && pane_window_x < controls_clearance &&
          pane_window_x + logical_bounds.width() > 0 &&
          logical_bounds.y() < controls_height && logical_bounds.bottom() > 0) {
        inset = std::max(0, controls_clearance - pane_window_x);
      }
      view->tab_strip()->SetLeadingInset(inset);
    }
  }

  gfx::Rect ContentBoundsForSidebarMode(SidebarMode mode) const {
    const int rail_w = RailWidthForMode(mode);
    const int x = !SidebarOnRight() ? rail_w : 0;
    return gfx::Rect(x, 0, std::max(0, width() - rail_w), height());
  }

  gfx::Rect RailBoundsForSidebarMode(SidebarMode mode) const {
    const int rail_w = RailWidthForMode(mode);
    return gfx::Rect(SidebarOnRight() ? width() - rail_w : 0, 0, rail_w,
                     height());
  }

  gfx::Rect RailResizeHandleBoundsForSidebarMode(SidebarMode mode) const {
    if (mode == SidebarMode::kHidden) {
      return gfx::Rect();
    }
    const int rail_width = RailWidthForMode(mode);
    const int desired_width =
        mode == SidebarMode::kIcons
            ? sidebar_metrics::kIconsResizeAreaWidth
            : sidebar_metrics::kExpandedResizeAreaWidth;
    const int handle_width = std::min(rail_width, desired_width);
    // Helium keeps the resize area wholly inside the strip: on the trailing
    // edge for a left rail and on the leading edge for a right rail.
    const int x = SidebarOnRight() ? width() - rail_width
                                   : rail_width - handle_width;
    return gfx::Rect(x, 0, handle_width, height());
  }

  views::View* TargetForRect(views::View* root,
                             const gfx::Rect& rect) override {
    CHECK_EQ(root, this);
    if (rail_resize_handle_ && rail_resize_handle_->GetVisible() &&
        rail_resize_handle_->bounds().Intersects(rect)) {
      return rail_resize_handle_;
    }
    return views::ViewTargeterDelegate::TargetForRect(root, rect);
  }

  void LayoutSidebarChrome(bool animated) {
    if (!content_ || !rail_ || !rail_resize_handle_) {
      return;
    }
    const SidebarMode mode = layout_config_.sidebar_mode;
    rail_->SetDisplayMode(mode);
    rail_->SetCanProcessEventsWithinSubtree(sidebar_metrics::IsVisible(mode));
    const gfx::Rect content_bounds = ContentBoundsForSidebarMode(mode);
    const gfx::Rect rail_bounds = RailBoundsForSidebarMode(mode);
    const gfx::Rect handle_bounds = RailResizeHandleBoundsForSidebarMode(mode);
    have_sidebar_layout_target_ = true;
    last_content_layout_target_ = content_bounds;
    last_rail_layout_target_ = rail_bounds;
    last_rail_resize_handle_layout_target_ = handle_bounds;
    rail_resize_handle_->SetVisible(sidebar_metrics::IsVisible(mode));
    const bool can_animate =
        animated && AnimationsEnabled() && GetWidget() != nullptr;
    if (can_animate) {
      root_animator_->AnimateViewTo(content_, content_bounds);
      root_animator_->AnimateViewTo(rail_, rail_bounds);
      root_animator_->AnimateViewTo(rail_resize_handle_, handle_bounds);
    } else {
      root_animator_->StopAnimatingView(content_);
      root_animator_->StopAnimatingView(rail_);
      root_animator_->StopAnimatingView(rail_resize_handle_);
      content_->SetBoundsRect(content_bounds);
      rail_->SetBoundsRect(rail_bounds);
      rail_resize_handle_->SetBoundsRect(handle_bounds);
    }
    UpdateWindowControlsOcclusion();
    ReorderChildView(rail_resize_handle_, children().size() - 1);
  }

  int BeginRailResize() {
    rail_resize_saved_expanded_width_ = ExpandedRailWidth();
    return CurrentRailWidth();
  }

  void ResizeRailToProposedWidth(int width) {
    const sidebar_metrics::ResizeTarget target =
        sidebar_metrics::TargetForResize(width, ExpandedRailWidth());
    if (layout_config_.sidebar_mode != target.mode) {
      if (target.mode == SidebarMode::kExpanded) {
        layout_config_.rail_width = target.expanded_width;
      }
      // Helium animates when the drag crosses its collapsed/expanded state
      // boundary, but direct width changes remain attached to the pointer.
      SetSidebarMode(target.mode, /*animated=*/true, /*persist=*/false,
                     /*cancel_active_drag=*/false);
      return;
    }
    if (target.mode != SidebarMode::kExpanded ||
        layout_config_.rail_width == target.expanded_width) {
      return;
    }
    layout_config_.rail_width = target.expanded_width;
    root_animator_->StopAnimatingView(content_);
    root_animator_->StopAnimatingView(rail_);
    root_animator_->StopAnimatingView(rail_resize_handle_);
    LayoutSidebarChrome(/*animated=*/false);
    ApplyVisualsAndScroll(/*animated=*/false);
  }

  int RailWidthForScreenDelta(int start_width, int delta_x) const {
    return start_width + (SidebarOnRight() ? -delta_x : delta_x);
  }

  void FinishRailResize(int width) {
    ResizeRailToProposedWidth(width);
    if (layout_config_.sidebar_mode == SidebarMode::kIcons &&
        rail_resize_saved_expanded_width_) {
      // Helium saves the new normal width only when the pointer is released
      // while expanded. A drag released in icon mode retains the normal width
      // that was persisted before the drag began.
      layout_config_.rail_width = *rail_resize_saved_expanded_width_;
    }
    SaveLayoutConfig(layout_config_);
    rail_resize_saved_expanded_width_.reset();
  }

  void CancelRailResize() { rail_resize_saved_expanded_width_.reset(); }

  void ResizeColumnToFraction(PaneId pane, double fraction) {
    const int strip_width = StripWidthForLayout();
    const double min_fraction =
        strip_width > 0
            ? std::min(2.0, CurrentLayoutMetrics().min_column_width /
                                static_cast<double>(strip_width))
            : 0.1;
    if (model_.SetColumnWidth(ws_, pane, std::max(min_fraction, fraction)) <=
        0.0) {
      return;
    }
    ApplyVisualsAndScroll(
        /*animated=*/false, ScrollIntoViewPolicy::kEnsureVisible);
  }

  void ResizeSplitToRatio(SplitId split, double ratio) {
    model_.SetSplitRatio(ws_, split, ratio);
    ApplyVisualsAndScroll(
        /*animated=*/false, ScrollIntoViewPolicy::kEnsureVisible);
  }

  bool GhosttyThemeEnabled() const { return layout_config_.ghostty_theme; }

  SkColor EffectiveAccentColor() const {
    if (active_theme_palette_.has_value() &&
        !layout_config_.HasUserKey("accent_color")) {
      return SkColorFromThemeArgb(active_theme_palette_->layout_accent);
    }
    return LayoutAccentColor(layout_config_);
  }

  SkColor EffectiveFocusColor() const {
    return LayoutFocusColor(layout_config_);
  }

  SkColor EffectiveDropHighlightColor() const {
    return LayoutDropHighlightColor(layout_config_);
  }

  void ApplyRailThemeColor(RailConfig* config,
                           const char* key,
                           CmuxArgb color,
                           SkColor RailConfig::* field) const {
    if (!config || base_rail_config_.HasUserKey(key)) {
      return;
    }
    config->*field = SkColorFromThemeArgb(color);
  }

  RailConfig EffectiveRailConfig() const {
    RailConfig config = base_rail_config_;
    if (layout_config_.ui_font_size > 0) {
      config.font_size = layout_config_.ui_font_size;
      config.row_height = std::max(config.row_height, config.font_size + 12);
    }
    if (SidebarOnRight()) {
      // macOS window controls remain on the left edge; a right-hand rail does
      // not need their reserved header width.
      config.traffic_light_clearance = config.left_pad;
    }
    if (active_theme_palette_.has_value()) {
      const CmuxThemePalette& p = *active_theme_palette_;
      ApplyRailThemeColor(&config, "bg", p.rail_bg, &RailConfig::bg);
      ApplyRailThemeColor(&config, "sel_bg", p.rail_sel_bg,
                          &RailConfig::sel_bg);
      ApplyRailThemeColor(&config, "hover_bg", p.rail_hover_bg,
                          &RailConfig::hover_bg);
      ApplyRailThemeColor(&config, "hover_text", p.rail_hover_text,
                          &RailConfig::hover_text);
      ApplyRailThemeColor(&config, "drag_bg", p.rail_drag_bg,
                          &RailConfig::drag_bg);
      ApplyRailThemeColor(&config, "text", p.rail_text, &RailConfig::text);
      ApplyRailThemeColor(&config, "sel_text", p.rail_sel_text,
                          &RailConfig::sel_text);
      ApplyRailThemeColor(&config, "folder_text", p.rail_folder_text,
                          &RailConfig::folder_text);
      ApplyRailThemeColor(&config, "chevron", p.rail_chevron,
                          &RailConfig::chevron);
      ApplyRailThemeColor(&config, "badge_bg", p.rail_badge_bg,
                          &RailConfig::badge_bg);
      ApplyRailThemeColor(&config, "badge_text", p.rail_badge_text,
                          &RailConfig::badge_text);
      ApplyRailThemeColor(&config, "close_color", p.rail_close_color,
                          &RailConfig::close_color);
      ApplyRailThemeColor(&config, "plus_text", p.rail_plus_text,
                          &RailConfig::plus_text);
      ApplyRailThemeColor(&config, "plus_bg", p.rail_plus_bg,
                          &RailConfig::plus_bg);
      ApplyRailThemeColor(&config, "plus_hover_bg", p.rail_plus_hover_bg,
                          &RailConfig::plus_hover_bg);
      ApplyRailThemeColor(&config, "indent_guide", p.rail_indent_guide,
                          &RailConfig::indent_guide);
      ApplyRailThemeColor(&config, "scrollbar_thumb", p.rail_scrollbar_thumb,
                          &RailConfig::scrollbar_thumb);
      ApplyRailThemeColor(&config, "accent", p.rail_accent,
                          &RailConfig::accent);
    }
    config.header_height = HeaderHeight();
    config.animations = layout_config_.animations;
    config.animation_ms = std::max(0, layout_config_.animation_ms);
    return config;
  }

  void RefreshRuntimeThemeColors() {
    window_bg_ = kDefaultPaneStripBackground;
    content_bg_ = kDefaultPaneStripBackground;
    tab_theme_colors_ = TabStripThemeColors();
    pane_theme_colors_ = PaneThemeColors();
    chrome_surface_colors_.reset();

    if (active_theme_palette_.has_value()) {
      const CmuxThemePalette& p = *active_theme_palette_;
      auto configured_color = [](const std::string& value, SkColor fallback) {
        SkColor parsed = fallback;
        return !value.empty() && ParseLayoutHexColor(value, &parsed) ? parsed
                                                                     : fallback;
      };
      // The sidebar is the visual foundation of cmux's browser chrome. On
      // macOS the native frame material supplies that canvas, so the default
      // tab strip and toolbar reveal it rather than repainting opaque fills.
      // The active tab and omnibox remain lightly tinted for legibility.
      // Explicit advanced JSON colors still win and retain opaque behavior.
      const SkColor shared_chrome_background = EffectiveRailConfig().bg;
      const bool use_native_material = NativeFrameMaterialSupported();
      const bool tab_bar_uses_material =
          use_native_material && layout_config_.chrome_tab_bar_color.empty();
      CmuxChromeSurfaceColors chrome_colors;
      chrome_colors.new_tab_background = SkColorFromThemeArgb(p.window_bg);
      const SkColor resolved_tab_bar = configured_color(
          layout_config_.chrome_tab_bar_color,
          tab_bar_uses_material
              ? SkColorFromThemeArgb(p.chrome_material_bg)
              : shared_chrome_background);
      const bool toolbar_uses_material =
          tab_bar_uses_material && layout_config_.chrome_toolbar_color.empty();
      chrome_colors.toolbar = configured_color(
          layout_config_.chrome_toolbar_color, resolved_tab_bar);
      // Helium's light-mode location bar shares the active-tab surface. Its
      // dark-mode bar shares the results/popup surface, so use the same
      // Ghostty-derived active surface as the dark popup default. In light
      // mode the popup stays on the toolbar/base surface, matching Helium.
      chrome_colors.omnibox_surface = SkColorFromThemeArgb(p.tab_active_bg);
      if (!layout_config_.chrome_omnibox_color.empty()) {
        chrome_colors.omnibox = configured_color(
            layout_config_.chrome_omnibox_color, SK_ColorTRANSPARENT);
      } else if (toolbar_uses_material) {
        chrome_colors.omnibox =
            SkColorFromThemeArgb(p.chrome_material_omnibox_bg);
      }
      // Results float over page content, so keep their default surface opaque
      // even while the in-frame omnibox reveals the native material.
      const SkColor opaque_popup_base =
          toolbar_uses_material ? shared_chrome_background
                                : chrome_colors.toolbar;
      chrome_colors.omnibox_popup = configured_color(
          layout_config_.chrome_omnibox_popup_color,
          p.is_light ? opaque_popup_base : chrome_colors.omnibox_surface);
      if (!layout_config_.chrome_omnibox_popup_hover_color.empty()) {
        chrome_colors.omnibox_popup_hover =
            configured_color(layout_config_.chrome_omnibox_popup_hover_color,
                             SK_ColorTRANSPARENT);
      }
      chrome_colors.omnibox_text = SkColorFromThemeArgb(p.chrome_omnibox_text);
      chrome_colors.omnibox_focus =
          SkColorFromThemeArgb(p.chrome_omnibox_focus);
      chrome_colors.omnibox_selection_bg =
          SkColorFromThemeArgb(p.chrome_omnibox_selection_bg);
      chrome_colors.omnibox_selection_text =
          SkColorFromThemeArgb(p.chrome_omnibox_selection_text);
      chrome_surface_colors_ = chrome_colors;
      window_bg_ = SkColorFromThemeArgb(p.window_bg);
      content_bg_ = SkColorFromThemeArgb(p.content_bg);
      tab_theme_colors_.strip_background = resolved_tab_bar;
      tab_theme_colors_.tab_active_bg = SkColorFromThemeArgb(
          tab_bar_uses_material ? p.chrome_material_tab_active_bg
                                : p.tab_active_bg);
      tab_theme_colors_.tab_hover_bg = SkColorFromThemeArgb(
          tab_bar_uses_material ? p.chrome_material_tab_hover_bg
                                : p.tab_hover_bg);
      tab_theme_colors_.tab_active_text =
          SkColorFromThemeArgb(p.tab_active_text);
      tab_theme_colors_.tab_idle_text = SkColorFromThemeArgb(p.tab_idle_text);
      tab_theme_colors_.plus_text = SkColorFromThemeArgb(p.tab_plus_text);
      tab_theme_colors_.accent = SkColorFromThemeArgb(p.tab_accent);
      pane_theme_colors_.strip_background = resolved_tab_bar;
      pane_theme_colors_.focus_border =
          SkColorFromThemeArgb(p.pane_focus_border);
      pane_theme_colors_.idle_border = SkColorFromThemeArgb(p.pane_idle_border);
    }

    tab_theme_colors_.font_size = layout_config_.ui_font_size;
    tab_theme_colors_.accent = EffectiveAccentColor();
    pane_theme_colors_.focus_border = EffectiveFocusColor();
    SetCmuxChromeSurfaceColors(chrome_surface_colors_);
  }

  void ApplyWindowBackgrounds(SkColor rail_tint) {
    if (NativeFrameMaterialSupported()) {
      SetBackground(nullptr);
      if (content_) {
        content_->SetBackground(nullptr);
        content_->layer()->SetFillsBoundsOpaquely(false);
      }
      if (window_controls_occlusion_) {
        window_controls_occlusion_->SetColor(SK_ColorTRANSPARENT);
      }
      if (views::Widget* widget = GetWidget()) {
        NativeFrameMaterialParams params;
        params.is_dark = active_theme_palette_.has_value()
                             ? !active_theme_palette_->is_light
                             : widget->GetColorMode() ==
                                   ui::ColorProviderKey::ColorMode::kDark;
        params.tint = rail_tint;
        UpdateNativeFrameMaterial(widget, params);
      }
    } else {
      SetBackground(views::CreateSolidBackground(window_bg_));
      if (content_) {
        content_->SetBackground(views::CreateSolidBackground(content_bg_));
        content_->layer()->SetFillsBoundsOpaquely(true);
      }
      if (window_controls_occlusion_) {
        window_controls_occlusion_->SetColor(content_bg_);
      }
    }
  }

  void MigrateLegacyCmuxAutogeneratedTheme() {
    if (!profile_services_available_ || !profile_ ||
        !active_theme_palette_.has_value()) {
      return;
    }
    ThemeService* theme_service = ThemeServiceFactory::GetForProfile(profile_);
    if (!theme_service) {
      return;
    }
    const SkColor seed = tab_theme_colors_.strip_background;
    // Older cmux builds persisted the Ghostty chrome surface as an
    // autogenerated profile theme. Remove only that exact legacy value; a
    // user-selected autogenerated color, extension theme, or other profile
    // theme is never overwritten by cmux.
    if (theme_service->UsingAutogeneratedTheme() &&
        theme_service->GetAutogeneratedThemeColor() == seed) {
      theme_service->UseDefaultTheme();
    }
  }

  void ApplyChromeSurfaceThemeOverrides(views::Widget* widget) const {
    if (!widget) {
      return;
    }
    if (!active_theme_palette_.has_value()) {
      widget->SetColorModeOverride(std::nullopt);
      widget->SetUserColorOverride(std::nullopt);
      return;
    }
    widget->SetColorModeOverride(active_theme_palette_->is_light
                                     ? ui::ColorProviderKey::ColorMode::kLight
                                     : ui::ColorProviderKey::ColorMode::kDark);
    // The Ghostty background is a surface, not a profile accent. Color it in
    // the cmux mixer without feeding it into Material's user-color palette.
    widget->SetUserColorOverride(std::nullopt);
  }

  void ApplyChromeSurfaceThemeOverridesToWindow() {
    ApplyChromeSurfaceThemeOverrides(GetWidget());
  }

  void ApplyChromeSurfaceThemeOverridesToRegisteredWidgets() const {
    const std::vector<views::Widget*> widgets(
        ChromeSurfaceThemeWidgets().begin(), ChromeSurfaceThemeWidgets().end());
    for (views::Widget* widget : widgets) {
      ApplyChromeSurfaceThemeOverrides(widget);
    }
  }

  bool LogChromeSurfaceThemeSelfTestIfReady() {
    views::Widget* widget = GetWidget();
    if (!widget) {
      return false;
    }
    if (!active_theme_palette_.has_value()) {
      LOG(WARNING)
          << "cmux-theme-selftest: FAIL chrome surface reason=no-theme";
      return true;
    }
    const ui::ColorProvider* color_provider = widget->GetColorProvider();
    if (!color_provider) {
      LOG(WARNING)
          << "cmux-theme-selftest: FAIL chrome surface reason=no-provider";
      return true;
    }
    const CmuxArgb toolbar =
        CmuxArgbFromSkColor(color_provider->GetColor(kColorToolbar));
    const bool toolbar_is_light = RelativeLuminance(toolbar) > 0.5;
    if (toolbar_is_light != active_theme_palette_->is_light) {
      LOG(WARNING) << "cmux-theme-selftest: FAIL chrome surface bg="
                   << ThemeRgbHex(toolbar) << " expected_is_light="
                   << (active_theme_palette_->is_light ? 1 : 0);
      return true;
    }
    const CmuxArgb new_tab_background = CmuxArgbFromSkColor(
        color_provider->GetColor(kColorNewTabPageBackground));
    if (new_tab_background != active_theme_palette_->window_bg) {
      LOG(WARNING) << "cmux-theme-selftest: FAIL new tab bg="
                   << ThemeRgbHex(new_tab_background) << " expected="
                   << ThemeRgbHex(active_theme_palette_->window_bg);
      return true;
    }
    LOG(WARNING) << "cmux-theme-selftest: PASS chrome surface bg="
                 << ThemeRgbHex(toolbar)
                 << " new_tab_bg=" << ThemeRgbHex(new_tab_background);
    return true;
  }

  void ApplyLoadedGhosttyTheme(std::optional<CmuxTheme> theme,
                               bool log_selftest) {
    if (!profile_services_available_) {
      return;
    }
    const bool enabled = GhosttyThemeEnabled();
    if (enabled && theme.has_value()) {
#if BUILDFLAG(IS_LINUX)
      ApplyCmuxLinuxTerminalTheme(*theme);
#endif
      active_theme_palette_ = DeriveUiPalette(*theme);
      active_theme_name_ = layout_config_.ghostty_theme_name;
    } else {
      active_theme_palette_.reset();
      active_theme_name_.clear();
    }
    ++theme_generation_;
    RefreshRuntimeThemeColors();
    ApplyChromeSurfaceThemeOverridesToWindow();
    ApplyChromeSurfaceThemeOverridesToRegisteredWidgets();
    ApplyRailLayoutConfig();
    MigrateLegacyCmuxAutogeneratedTheme();
    SchedulePaint();

    if (!log_selftest) {
      return;
    }
    if (!enabled) {
      LOG(WARNING) << "cmux-theme-selftest: FAIL reason=disabled";
      return;
    }
    if (!theme.has_value() || !active_theme_palette_.has_value()) {
      LOG(WARNING) << "cmux-theme-selftest: FAIL reason=extraction-failed";
      return;
    }
    LOG(WARNING) << "cmux-theme-selftest: PASS source=ghostty is_light="
                 << (active_theme_palette_->is_light ? 1 : 0)
                 << " bg=" << ThemeRgbHex(theme->bg) << " accent="
                 << ThemeRgbHex(active_theme_palette_->layout_accent);
    pending_chrome_surface_selftest_ = !LogChromeSurfaceThemeSelfTestIfReady();
  }

  void ApplyStartupGhosttyTheme(uint64_t request_id) {
    if (request_id != ghostty_theme_load_request_) {
      return;
    }
    if (!GhosttyThemeEnabled()) {
      ApplyLoadedGhosttyTheme(std::nullopt,
                              /*log_selftest=*/ThemeSelfTestEnabled());
      return;
    }
    BeginGhosttyThemeLoad(request_id, GhosttyThemeLoadKind::kStartup);
  }

  void BeginGhosttyThemeLoad(uint64_t request_id,
                             GhosttyThemeLoadKind kind) {
    if (request_id != ghostty_theme_load_request_) {
      return;
    }
#if BUILDFLAG(IS_MAC) || BUILDFLAG(IS_LINUX)
    // Validate the pinned runtime tree and publish GHOSTTY_RESOURCES_DIR on a
    // blocking-capable worker. Ghostty's process-global initialization remains
    // on the UI sequence, as required by its macOS runtime, and the subsequent
    // config-file read returns to the worker below.
    if (!GhosttyThemeTaskRunner()->PostTaskAndReplyWithResult(
            FROM_HERE,
            base::BindOnce(&EnsureGhosttyResourcesDirectory),
            base::BindOnce(&CmuxWindowView::OnGhosttyResourcesPrepared,
                           weak_factory_.GetWeakPtr(), request_id, kind))) {
      ApplyGhosttyThemeLoadFailure(request_id, kind);
    }
#else
    ApplyGhosttyThemeLoadFailure(request_id, kind);
#endif
  }

  void OnGhosttyResourcesPrepared(uint64_t request_id,
                                  GhosttyThemeLoadKind kind,
                                  bool resources_ready) {
    if (request_id != ghostty_theme_load_request_) {
      return;
    }
#if BUILDFLAG(IS_MAC) || BUILDFLAG(IS_LINUX)
    if (!resources_ready || !EnsureGhosttyCoreInit()) {
      ApplyGhosttyThemeLoadFailure(request_id, kind);
      return;
    }

    base::OnceCallback<void(std::optional<CmuxTheme>)> reply;
    if (kind == GhosttyThemeLoadKind::kReload) {
      reply = base::BindOnce(&CmuxWindowView::OnThemeReloaded,
                             weak_factory_.GetWeakPtr(), request_id);
    } else {
      reply = base::BindOnce(&CmuxWindowView::OnStartupThemeLoaded,
                             weak_factory_.GetWeakPtr(), request_id);
    }
    if (!GhosttyThemeTaskRunner()->PostTaskAndReplyWithResult(
            FROM_HERE,
            base::BindOnce(&LoadGhosttyTheme,
                           layout_config_.ghostty_theme_name),
            std::move(reply))) {
      ApplyGhosttyThemeLoadFailure(request_id, kind);
    }
#else
    ApplyGhosttyThemeLoadFailure(request_id, kind);
#endif
  }

  void ApplyGhosttyThemeLoadFailure(uint64_t request_id,
                                    GhosttyThemeLoadKind kind) {
    if (request_id != ghostty_theme_load_request_) {
      return;
    }
    if (kind == GhosttyThemeLoadKind::kReload) {
      LOG(WARNING) << "cmux-theme: failed to load Ghostty theme";
    }
    ApplyLoadedGhosttyTheme(std::nullopt,
                            /*log_selftest=*/ThemeSelfTestEnabled());
  }

  void OnStartupThemeLoaded(uint64_t request_id,
                            std::optional<CmuxTheme> theme) {
    if (request_id != ghostty_theme_load_request_) {
      return;
    }
    ApplyLoadedGhosttyTheme(std::move(theme),
                            /*log_selftest=*/ThemeSelfTestEnabled());
  }

  void OnThemeReloaded(uint64_t request_id, std::optional<CmuxTheme> theme) {
    if (request_id != ghostty_theme_load_request_) {
      return;
    }
    ApplyLoadedGhosttyTheme(std::move(theme),
                            /*log_selftest=*/ThemeSelfTestEnabled());
    ReloadOpenNewTabPagesForTheme();
  }

  void OnStartupLayoutConfigLoaded(
      uint64_t request_id,
      std::optional<LayoutConfig> config) {
    if (request_id != ghostty_theme_load_request_) {
      return;
    }
    if (config) {
      layout_config_ = *config;
    }
    PublishLayoutConfig(layout_config_);
    ApplyRailLayoutConfig();
    LayoutSidebarChrome(/*animated=*/false);
    ApplyVisualsAndScroll(/*animated=*/false);
    // The layout file owns ghostty_theme. Do not begin extraction until its
    // asynchronous read has resolved, including the missing-file/default case.
    ApplyStartupGhosttyTheme(request_id);
  }

  void OnStartupBrowserConfigLoaded(std::optional<BrowserConfig> config) {
    if (!config || new_tab_page_ == config->new_tab_page) {
      return;
    }
    new_tab_page_ = config->new_tab_page;
    ApplyConfiguredPageToFreshNewTabs();
  }

  // Browser objects for every workspace are materialized before the async
  // config read completes. Update only untouched blank/New Tab pages across
  // all workspaces; a pending user navigation always wins.
  void ApplyConfiguredPageToFreshNewTabs() {
    const GURL chrome_new_tab("chrome://newtab/");
    const GURL chrome_new_tab_page("chrome://new-tab-page/");
    const GURL blank("about:blank");
    const GURL configured = ConfiguredNewTabURL();
    const auto is_fresh_new_tab_url = [&](const GURL& url) {
      return url.is_empty() || url == chrome_new_tab ||
             url == chrome_new_tab_page || url == blank;
    };
    for (const Workspace& workspace : model_.workspaces()) {
      for (PaneId pane_id : model_.PanesOf(workspace.id)) {
        const Pane* pane = model_.FindPane(workspace.id, pane_id);
        if (!pane) {
          continue;
        }
        for (const SurfaceTab& tab : pane->tabs) {
          content::WebContents* contents = WebContentsForSurface(tab.id);
          if (!contents || !is_fresh_new_tab_url(contents->GetVisibleURL())) {
            continue;
          }
          content::NavigationController& controller = contents->GetController();
          content::NavigationEntry* pending = controller.GetPendingEntry();
          if (pending && !is_fresh_new_tab_url(pending->GetURL())) {
            continue;
          }
          content::NavigationController::LoadURLParams params(configured);
          params.transition_type = ui::PAGE_TRANSITION_AUTO_TOPLEVEL;
          params.should_replace_current_entry = true;
          controller.LoadURLWithParams(params);
        }
      }
    }
  }

  void ReloadOpenNewTabPagesForTheme() {
    new_tab_theme_reload_timer_.Stop();
    pending_new_tab_theme_reloads_.clear();
    pending_new_tab_theme_generation_ = theme_generation_;
    const GURL chrome_new_tab("chrome://newtab/");
    const GURL chrome_new_tab_page("chrome://new-tab-page/");
    const auto is_new_tab_url = [&](const GURL& url) {
      return url == chrome_new_tab || url == chrome_new_tab_page;
    };
    for (const Workspace& workspace : model_.workspaces()) {
      for (PaneId pane_id : model_.PanesOf(workspace.id)) {
        const Pane* pane = model_.FindPane(workspace.id, pane_id);
        if (!pane) {
          continue;
        }
        for (const SurfaceTab& tab : pane->tabs) {
          content::WebContents* contents = WebContentsForSurface(tab.id);
          if (!contents ||
              (!is_new_tab_url(contents->GetVisibleURL()) &&
               !is_new_tab_url(contents->GetLastCommittedURL()))) {
            continue;
          }
          if (workspace.id == ws_ && tab.id == pane->selected) {
            contents->GetController().Reload(content::ReloadType::NORMAL,
                                             /*check_for_repost=*/false);
          } else {
            pending_new_tab_theme_reloads_.push_back(tab.id);
          }
        }
      }
    }
    SchedulePendingNewTabThemeReloads();
  }

  void SchedulePendingNewTabThemeReloads() {
    if (pending_new_tab_theme_reloads_.empty()) {
      return;
    }
    new_tab_theme_reload_timer_.Start(
        FROM_HERE, base::Milliseconds(16),
        base::BindOnce(&CmuxWindowView::DrainPendingNewTabThemeReloads,
                       weak_factory_.GetWeakPtr(),
                       pending_new_tab_theme_generation_));
  }

  void DrainPendingNewTabThemeReloads(int theme_generation) {
    if (theme_generation != theme_generation_) {
      pending_new_tab_theme_reloads_.clear();
      return;
    }
    constexpr size_t kReloadsPerFrame = 8;
    for (size_t i = 0;
         i < kReloadsPerFrame && !pending_new_tab_theme_reloads_.empty(); ++i) {
      const SurfaceTabId tab_id = pending_new_tab_theme_reloads_.front();
      pending_new_tab_theme_reloads_.pop_front();
      content::WebContents* contents = WebContentsForSurface(tab_id);
      if (!contents) {
        continue;
      }
      const GURL chrome_new_tab("chrome://newtab/");
      const GURL chrome_new_tab_page("chrome://new-tab-page/");
      const auto is_new_tab_url = [&](const GURL& url) {
        return url == chrome_new_tab || url == chrome_new_tab_page;
      };
      if (!is_new_tab_url(contents->GetVisibleURL()) &&
          !is_new_tab_url(contents->GetLastCommittedURL())) {
        continue;
      }
      contents->GetController().Reload(content::ReloadType::NORMAL,
                                       /*check_for_repost=*/false);
    }
    SchedulePendingNewTabThemeReloads();
  }

  void OnThemeReloadLayoutLoaded(uint64_t request_id,
                                 LayoutConfig defaults,
                                 std::optional<LayoutConfig> config) {
    if (request_id != ghostty_theme_load_request_) {
      return;
    }
    layout_config_ = config.value_or(std::move(defaults));
    PublishLayoutConfig(layout_config_);
    ContinueThemeReload(request_id);
  }

  void LoadKeymapAsync() {
    base::ThreadPool::PostTaskAndReplyWithResult(
        FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
        base::BindOnce(&LoadUserKeymapOnBackgroundSequence),
        base::BindOnce(&CmuxWindowView::OnKeymapLoaded,
                       weak_factory_.GetWeakPtr()));
  }

  void ClearPendingChord() { pending_chord_.clear(); }

  void StartKeymapWatcher() {
    const base::FilePath path =
        base::FilePath::FromUTF8Unsafe(CmuxConfigPath());
    // macOS opens kqueue descriptors while installing a watch. Chromium's UI
    // sequence disallows that blocking work, so own and initialize the watcher
    // on a MayBlock sequence and marshal notifications back to this sequence.
    keymap_file_watcher_.emplace(base::ThreadPool::CreateSequencedTaskRunner(
        {base::MayBlock(), base::TaskPriority::USER_VISIBLE}));
    keymap_file_watcher_->AsyncCall(&base::FilePathWatcher::Watch)
        .WithArgs(path, base::FilePathWatcher::Type::kNonRecursive,
                  base::BindPostTaskToCurrentDefault(
                      base::BindRepeating(&CmuxWindowView::OnKeymapFileChanged,
                                          weak_factory_.GetWeakPtr())))
        .Then(base::BindOnce(
            [](base::FilePath path, bool watching) {
              if (!watching) {
                LOG(WARNING)
                    << "cmux-keymap: could not watch " << path.AsUTF8Unsafe();
              }
            },
            path));
  }

  void OnKeymapFileChanged(const base::FilePath&, bool error) {
    if (error) {
      LOG(WARNING) << "cmux-keymap: config watcher reported an error";
      return;
    }
    keymap_reload_timer_.Start(FROM_HERE, base::Milliseconds(150),
                               base::BindOnce(&CmuxWindowView::LoadKeymapAsync,
                                              weak_factory_.GetWeakPtr()));
  }

  void OnKeymapLoaded(KeymapFileLoad load) {
    if (!load.result.valid) {
      for (const std::string& warning : load.result.warnings) {
        LOG(WARNING) << "cmux-keymap: " << load.path << ": " << warning;
      }
      LOG(WARNING) << "cmux-keymap: keeping the last valid configuration";
      return;
    }
    keymap_ = BuildConfiguredKeymap(IsMacKeymap(), load);
    pending_chord_.clear();
    pending_chord_timer_.Stop();
    if (views::FocusManager* fm = GetFocusManager()) {
      fm->UnregisterAccelerators(this);
      RegisterChords(fm);
    }
  }

  void ApplyRailLayoutConfig() {
    if (!rail_) {
      return;
    }
    RefreshRuntimeThemeColors();
    const int header_height = HeaderHeight();
    const bool animations = layout_config_.animations;
    const int animation_ms = std::max(0, layout_config_.animation_ms);
    const int focus_border = std::max(0, layout_config_.focus_border);
    const SkColor accent_color = EffectiveAccentColor();
    const SkColor drop_highlight_color = EffectiveDropHighlightColor();
    const SkColor focus_color = EffectiveFocusColor();
    if (applied_ui_font_size_ != layout_config_.ui_font_size ||
        applied_rail_header_height_ != header_height ||
        applied_sidebar_position_ != layout_config_.sidebar_position ||
        applied_animations_ != animations ||
        applied_animation_ms_ != animation_ms ||
        applied_focus_border_ != focus_border ||
        applied_accent_color_ != accent_color ||
        applied_drop_highlight_color_ != drop_highlight_color ||
        applied_focus_color_ != focus_color ||
        applied_theme_generation_ != theme_generation_) {
      RailConfig rail_config = EffectiveRailConfig();
      const SkColor rail_tint = rail_config.bg;
      if (NativeFrameMaterialSupported()) {
        rail_config.bg = SK_ColorTRANSPARENT;
      }
      rail_->SetConfig(rail_config);
      ApplyWindowBackgrounds(rail_tint);
      ApplyAnimationConfigToAnimators();
      for (auto& kv : views_) {
        ApplyAnimationConfigToPane(kv.second);
      }
      applied_ui_font_size_ = layout_config_.ui_font_size;
      applied_rail_header_height_ = header_height;
      applied_sidebar_position_ = layout_config_.sidebar_position;
      applied_animations_ = animations;
      applied_animation_ms_ = animation_ms;
      applied_focus_border_ = focus_border;
      applied_accent_color_ = accent_color;
      applied_drop_highlight_color_ = drop_highlight_color;
      applied_focus_color_ = focus_color;
      applied_theme_generation_ = theme_generation_;
    }
    if (!applied_sidebar_mode_ ||
        *applied_sidebar_mode_ != layout_config_.sidebar_mode) {
      rail_->SetDisplayMode(layout_config_.sidebar_mode);
      rail_->SetCanProcessEventsWithinSubtree(
          sidebar_metrics::IsVisible(layout_config_.sidebar_mode));
      applied_sidebar_mode_ = layout_config_.sidebar_mode;
    }
  }

  LayoutMetrics CurrentLayoutMetrics() const {
    LayoutMetrics metrics;
    metrics.gap = std::max(0, layout_config_.gap);
    metrics.margin = std::max(0, layout_config_.margin);
    metrics.min_column_width = std::max(1, layout_config_.min_column_width);
    metrics.column_fraction =
        std::clamp(layout_config_.column_fraction, 0.1, 2.0);
    return metrics;
  }

  bool AnimationsEnabled() const {
    return layout_config_.animations && layout_config_.animation_ms > 0;
  }

  base::TimeDelta AnimationDuration(int base_ms) const {
    if (!AnimationsEnabled()) {
      return base::Milliseconds(0);
    }
    return base::Milliseconds(
        std::max(0, layout_config_.animation_ms * base_ms / 160));
  }

  void ApplyAnimationConfigToAnimators() {
    if (animator_) {
      animator_->SetAnimationDuration(AnimationDuration(160));
    }
    if (root_animator_) {
      root_animator_->SetAnimationDuration(kSidebarExpandCollapseDuration);
    }
    if (drag_controller_) {
      drag_controller_->SetAnimationConfig(
          layout_config_.animations, std::max(0, layout_config_.animation_ms));
      drag_controller_->SetThemeColors(EffectiveDropHighlightColor(),
                                       tab_theme_colors_.tab_active_bg,
                                       tab_theme_colors_.tab_active_text);
    }
  }

  void ApplyAnimationConfigToPane(CmuxPaneView* pane) {
    if (pane) {
      pane->SetThemeColors(pane_theme_colors_);
      pane->SetFocusBorderThickness(layout_config_.focus_border);
      pane->SetFocusBorderColor(EffectiveFocusColor());
      pane->SetRoundedFrame(ShouldUseRoundedFrame());
      pane->SetAnimationConfig(layout_config_.animations,
                               std::max(0, layout_config_.animation_ms));
      if (pane->tab_strip()) {
        pane->tab_strip()->SetThemeColors(tab_theme_colors_);
      }
    }
  }

  bool ShouldUseRoundedFrame() const {
    if (!profile_services_available_ || !profile_ ||
        !profile_->GetPrefs()->GetBoolean(prefs::kHeliumRoundedFrame)) {
      return false;
    }
    const views::Widget* widget = GetWidget();
    return !widget || !widget->IsFullscreen();
  }

  void OnRoundedFramePreferenceChanged() {
    RefreshRoundedFrames();
  }

  void RefreshRoundedFrames() {
    const bool enabled = ShouldUseRoundedFrame();
    for (auto& entry : views_) {
      entry.second->SetRoundedFrame(enabled);
    }
  }

  RoundedFrameGeometry FrameGeometryForPane(const gfx::Rect& pane_bounds,
                                            int viewport_width) const {
    const bool has_attached_rail =
        sidebar_metrics::IsVisible(layout_config_.sidebar_mode) &&
        RailWidthForMode(layout_config_.sidebar_mode) > 0;
    const bool rail_on_left = has_attached_rail && !SidebarOnRight();
    const bool rail_on_right = has_attached_rail && SidebarOnRight();
    const bool has_area =
        pane_bounds.width() > 0 && pane_bounds.height() > 0;
    const bool touches_left = has_area && pane_bounds.x() == 0;
    const bool touches_right =
        has_area && pane_bounds.right() == viewport_width;
    const bool touches_bottom =
        has_area && pane_bounds.bottom() == height();

    // Every terminal/page content view attaches to local top chrome (the pane
    // tab strip or web toolbar). A visible workspace rail is attached side
    // chrome, so a pane meeting it drops that side's three-DIP inset. The
    // opposite side still meets the native window and keeps the inset.
    return MakeRoundedFrameGeometry(
        ShouldUseRoundedFrame(),
        /*top_chrome_attached=*/true,
        /*left_chrome_attached=*/touches_left && rail_on_left,
        /*right_chrome_attached=*/touches_right && rail_on_right,
        /*bottom_left_window_corner=*/
        touches_bottom && touches_left && !rail_on_left,
        /*bottom_right_window_corner=*/
        touches_bottom && touches_right && !rail_on_right);
  }

  int StripWidthForLayout() const {
    if (strip_width_override_ >= 0) {
      return strip_width_override_;
    }
    return content_ ? content_->width()
                    : ContentBoundsForSidebarMode(layout_config_.sidebar_mode)
                          .width();
  }

  // ---- Pane + surface realization -------------------------------------------
  // The first model pane in the active workspace that has no view yet (a
  // freshly seeded workspace's initial column), or kInvalidId.
  PaneId FirstViewlessPane() {
    for (PaneId p : model_.PanesOf(ws_)) {
      if (views_.find(p) == views_.end()) {
        return p;
      }
    }
    return kInvalidId;
  }

  // Create the CmuxPaneView for a model pane without creating any surfaces.
  // Used by drag/drop commits that will attach already-live surface views.
  CmuxPaneView* RealizePaneEmpty(PaneId pane_id) {
    Pane* pane = model_.FindPane(WorkspaceForPane(pane_id), pane_id);
    if (!pane || views_.count(pane_id)) {
      return ViewForPane(pane_id);
    }
    // Resize handles stay after every pane in the child list so their layers
    // paint above pane layers. Insert new panes immediately before that stable
    // suffix instead of repairing z-order with ReorderChildView(): reordering
    // layer-backed children while a split is being realized can make Views
    // restack a layer relative to itself.
    const size_t resize_handle_count = 1 + split_resize_handles_.size();
    CHECK_GE(content_->children().size(), resize_handle_count);
    auto* view = content_->AddChildViewAt(
        std::make_unique<CmuxPaneView>(pane_id, this),
        content_->children().size() - resize_handle_count);
    views_[pane_id] = view;
    ApplyAnimationConfigToPane(view);
    // Every pane reports native content activation through one uniform signal
    // (terminal becomeFirstResponder, browser web-contents-focused, omnibox
    // focus) so a click that's handled natively -- and never reaches the Views
    // FocusManager -- still focuses + scrolls in its column. Routed through a
    // weak-ptr forwarder because a terminal's NSView outlives the pane and can
    // fire during teardown.
    view->SetActivationCallback(
        base::BindRepeating(&CmuxWindowView::OnPaneActivatedById,
                            weak_factory_.GetWeakPtr(), pane_id));
    return view;
  }

  bool SuppressPaneEntranceForEnvironment() const {
    // CMUX_DEMO_LAYOUT builds deterministic screenshot fixtures through normal
    // user paths; keep those seeded panes instant so screenshots do not depend
    // on entrance timing.
    return getenv("CMUX_DEMO_LAYOUT") != nullptr;
  }

  void MarkPaneForEntrance(PaneId pane_id) {
    // The map is explicit instead of "new view == animate" so workspace
    // switches, startup seeding, and drag reparenting can realize views
    // without being mistaken for user-created panes.
    if (pane_id == kInvalidId || SuppressPaneEntranceForEnvironment()) {
      return;
    }
    pending_entrance_panes_[pane_id] = PaneEntrance();
  }

  void MarkPaneForSplitEntrance(PaneId pane_id,
                                SplitOrientation orientation,
                                bool insert_first) {
    if (pane_id == kInvalidId || SuppressPaneEntranceForEnvironment()) {
      return;
    }
    pending_entrance_panes_[pane_id] =
        PaneEntrance{orientation, insert_first};
  }

  gfx::Rect EntranceSeedBounds(const gfx::Rect& target,
                               double scroll_before,
                               double scroll_after,
                               const PaneEntrance& entrance) const {
    // Bonsplit creates the split with its divider at the selected edge, then
    // animates it to 50%. Seed the new pane on that same edge while the
    // existing target keeps its pre-split bounds.
    if (entrance.split_orientation.has_value()) {
      if (*entrance.split_orientation == SplitOrientation::kHorizontal) {
        const int seed_width = target.width() > 0 ? 1 : 0;
        const int x = entrance.insert_first
                          ? target.x()
                          : target.right() - seed_width;
        return gfx::Rect(x, target.y(), seed_width, target.height());
      }
      const int seed_height = target.height() > 0 ? 1 : 0;
      const int y = entrance.insert_first
                        ? target.y()
                        : target.bottom() - seed_height;
      return gfx::Rect(target.x(), y, target.width(), seed_height);
    }

    const int seed_width = target.width() > 0 ? 1 : 0;
    int x = target.x();
    // When focus reveal increases scroll_x, strip contents glide left. Pin the
    // seed to the final trailing edge so the new pane grows left with that
    // glide instead of expanding against it.
    if (scroll_after > scroll_before + 1e-6) {
      x = target.right() - seed_width;
    }
    return gfx::Rect(x, target.y(), seed_width, target.height());
  }

  // Create the CmuxPaneView for a model pane and one surface per model tab
  // (web tabs load `web_url`). No-op if the pane already has a view.
  void RealizePane(PaneId pane_id, const GURL& web_url) {
    Pane* pane = model_.FindPane(WorkspaceForPane(pane_id), pane_id);
    CmuxPaneView* view = RealizePaneEmpty(pane_id);
    if (!pane || !view) {
      return;
    }
    bool created_any = false;
    for (const SurfaceTab& tab : pane->tabs) {
      if (view->SurfaceFor(tab.id)) {
        continue;
      }
      CreateSurfaceForTab(view, tab.id, tab.kind, web_url, std::string());
      created_any = true;
    }
    if (created_any) {
      RefreshPaneTabs(pane_id);
    }
  }

  GURL ConfiguredNewTabURL() const {
    return new_tab_page_ == kBrowserNewTabPageChrome ? GURL("chrome://newtab/")
                                                     : GURL("about:blank");
  }

  // Create + bind one tab's presentation. A web tab is first inserted into
  // the workspace Browser's real TabStripModel; that model, not the pane,
  // owns its WebContents for the rest of the tab's lifetime. An empty URL
  // means the configured new-tab page.
  void CreateSurfaceForTab(CmuxPaneView* view,
                           SurfaceTabId tab,
                           SurfaceKind kind,
                           const GURL& url,
                           const std::string& terminal_command,
                           std::optional<CmuxTerminalBackendSeed>
                               terminal_seed = std::nullopt) {
    CmuxSurface* surface = nullptr;
    if (kind == SurfaceKind::kTerminal) {
#if BUILDFLAG(IS_MAC) || BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_WIN)
      scoped_refptr<CmuxTerminalBackend> backend =
          terminal_seed
              ? base::MakeRefCounted<CmuxTerminalBackend>(
                    tui_client_, terminal_command, "Terminal",
                    WorkspaceRegistryKey(WorkspaceForPane(view->pane_id())),
                    std::move(*terminal_seed))
              : base::MakeRefCounted<CmuxTerminalBackend>(
                    tui_client_, terminal_command, "Terminal",
                    WorkspaceRegistryKey(WorkspaceForPane(view->pane_id())));
      auto [backend_it, inserted] =
          terminal_backends_.try_emplace(tab, std::move(backend));
      if (inserted) {
        RegisterTerminalBackend(tab, backend_it->second);
      }
      surface = PlatformCreateTerminalSurface(view->surface_container(),
                                              backend_it->second);
      if (!surface && inserted) {
        UnregisterTerminalBackend(tab, backend_it->second);
        terminal_backends_.erase(backend_it);
      }
#endif
    } else {
      const WorkspaceId workspace = WorkspaceForPane(view->pane_id());
      Browser* browser = BrowserForWorkspace(workspace);
      if (!browser) {
        return;
      }

      content::WebContents* web_contents = WebContentsForSurface(tab);
      if (!web_contents) {
        ++internal_browser_mutation_depth_;
        web_contents = chrome::AddAndReturnTabAt(
            browser,
            url.is_valid() && !url.is_empty() ? url : ConfiguredNewTabURL(),
            /*index=*/-1, /*foreground=*/true);
        --internal_browser_mutation_depth_;
        if (!web_contents) {
          return;
        }
        RecordWebTabPlacement(web_contents, workspace, view->pane_id(), tab);
        SyncBrowserTabOrder(workspace);
      }
      surface = AddBrowserSurface(view->surface_container(), web_contents,
                                  view->pane_id());
    }
    if (surface) {
      view->BindSurface(tab, surface);
    }
  }

  void NewTabInPane(PaneId pane_id,
                    SurfaceKind kind,
                    std::string terminal_command = std::string(),
                    std::string title = std::string(),
                    GURL web_url = GURL()) {
    CmuxPaneView* view = ViewForPane(pane_id);
    if (!view) {
      return;
    }
    const SurfaceTabId tab = model_.AddTab(ws_, pane_id, kind, title);
    if (tab == kInvalidId) {
      return;
    }
    CreateSurfaceForTab(view, tab, kind, web_url, terminal_command);
    RefreshPaneTabs(pane_id);
    if (kind == SurfaceKind::kWeb) {
      FocusNewWebTab(pane_id, tab);
    } else {
      focus_.FocusPane(pane_id, /*move_keyboard=*/true);
    }
  }

  void CloseTabDeferred(PaneId pane, SurfaceTabId tab) {
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(&CmuxWindowView::CloseTabNow,
                                  weak_factory_.GetWeakPtr(), pane, tab));
  }

  void CloseOtherTabsDeferred(PaneId pane, SurfaceTabId keep) {
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(&CmuxWindowView::CloseOtherTabsNow,
                                  weak_factory_.GetWeakPtr(), pane, keep));
  }

  void CloseTabsOnSideDeferred(PaneId pane,
                               SurfaceTabId keep,
                               bool close_left) {
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE,
        base::BindOnce(&CmuxWindowView::CloseTabsOnSideNow,
                       weak_factory_.GetWeakPtr(), pane, keep, close_left));
  }

  void NewTabRightDeferred(PaneId pane, SurfaceTabId after) {
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(&CmuxWindowView::NewTabRightNow,
                                  weak_factory_.GetWeakPtr(), pane, after));
  }

  void MoveTabToNewColumnDeferred(PaneId pane, SurfaceTabId tab) {
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(&CmuxWindowView::MoveTabToNewColumnNow,
                                  weak_factory_.GetWeakPtr(), pane, tab));
  }

  void SplitWithTabDeferred(PaneId pane,
                            SurfaceTabId tab,
                            SplitOrientation orientation) {
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE,
        base::BindOnce(&CmuxWindowView::SplitWithTabNow,
                       weak_factory_.GetWeakPtr(), pane, tab, orientation));
  }

  void NewSplitWithCurrentTabDeferred(
      PaneId pane,
      SurfaceTabId tab,
      SplitOrientation orientation = SplitOrientation::kHorizontal) {
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(&CmuxWindowView::NewSplitWithCurrentTabNow,
                                  weak_factory_.GetWeakPtr(), pane, tab,
                                  orientation));
  }

  void HibernateTab(SurfaceTabId tab) {
    int browser_index = TabStripModel::kNoTab;
    TabStripModel* tabs = BrowserTabsForSurface(tab, &browser_index);
    if (!tabs || browser_index == TabStripModel::kNoTab ||
        browser_index == tabs->active_index()) {
      return;
    }
    resource_coordinator::TabLifecycleUnitExternal* lifecycle =
        resource_coordinator::TabLifecycleUnitExternal::FromWebContents(
            tabs->GetWebContentsAt(browser_index));
    if (lifecycle) {
      lifecycle->DiscardTab(mojom::LifecycleUnitDiscardReason::EXTERNAL);
    }
  }

  std::vector<SurfaceTabId> WebTabsInWorkspace(
      WorkspaceId workspace) const {
    std::vector<SurfaceTabId> tabs;
    for (PaneId pane_id : model_.PanesOf(workspace)) {
      const Pane* pane = model_.FindPane(workspace, pane_id);
      if (!pane) {
        continue;
      }
      for (const SurfaceTab& tab : pane->tabs) {
        content::WebContents* contents = WebContentsForSurface(tab.id);
        if (tab.kind == SurfaceKind::kWeb && contents &&
            contents->GetVisibleURL().is_valid()) {
          tabs.push_back(tab.id);
        }
      }
    }
    return tabs;
  }

  std::vector<std::pair<TabStripModel*, int>>
  WorkspaceContextWebTabTargets(WorkspaceId context) const {
    std::vector<std::pair<TabStripModel*, int>> targets;
    for (WorkspaceId workspace : model_.WorkspacesForCommand(context)) {
      for (SurfaceTabId surface : WebTabsInWorkspace(workspace)) {
        int browser_index = TabStripModel::kNoTab;
        TabStripModel* tabs =
            BrowserTabsForSurface(surface, &browser_index);
        if (tabs && browser_index != TabStripModel::kNoTab) {
          targets.emplace_back(tabs, browser_index);
        }
      }
    }
    return targets;
  }

  DetachedSurface DetachSurface(PaneId pane_id, SurfaceTabId tab) {
    DetachedSurface detached;
    detached.tab = tab;
    CmuxPaneView* view = ViewForPane(pane_id);
    if (!view) {
      return detached;
    }
    CmuxSurface* surface = nullptr;
    detached.view = view->DetachSurfaceView(tab, &surface);
    detached.surface = surface;
    return detached;
  }

  std::vector<DetachedSurface> DetachAllSurfaces(PaneId pane_id) {
    std::vector<DetachedSurface> out;
    Pane* pane = model_.FindPane(ws_, pane_id);
    if (!pane) {
      return out;
    }
    std::vector<SurfaceTabId> tabs;
    for (const SurfaceTab& tab : pane->tabs) {
      tabs.push_back(tab.id);
    }
    for (SurfaceTabId tab : tabs) {
      DetachedSurface detached = DetachSurface(pane_id, tab);
      if (detached.view && detached.surface) {
        out.push_back(std::move(detached));
      }
    }
    return out;
  }

  bool AttachSurfaceToPane(PaneId pane_id, DetachedSurface surface) {
    CmuxPaneView* view = RealizePaneEmpty(pane_id);
    if (!view || !surface.view || !surface.surface) {
      return false;
    }
    view->AttachSurface(surface.tab, std::move(surface.view), surface.surface);
    return true;
  }

  void DestroyPaneView(PaneId pane_id, bool notify_focus) {
    CmuxPaneView* view = ViewForPane(pane_id);
    if (!view) {
      return;
    }
    views_.erase(pane_id);
    content_->RemoveChildViewT(view);
    if (notify_focus) {
      focus_.OnPaneRemoved();
    }
  }

  void CancelActiveDragForMutation() {
    CancelActiveDrag(/*immediate=*/false);
  }

  void CancelActiveDragForHostState() {
    CancelActiveDrag(/*immediate=*/true);
  }

  void CancelActiveDrag(bool immediate) {
    if (!drag_controller_ || !drag_controller_->active()) {
      return;
    }
    if (immediate) {
      drag_controller_->CancelImmediately();
    } else {
      drag_controller_->Cancel();
    }
    for (auto& kv : views_) {
      if (kv.second && kv.second->tab_strip()) {
        kv.second->tab_strip()->CancelDragStateForExternalEnd();
      }
    }
    FlushDeferredPaneTabs();
  }

  void CloseTerminalBackend(SurfaceTabId tab) {
    auto backend = terminal_backends_.find(tab);
    if (backend == terminal_backends_.end()) {
      return;
    }
    const std::optional<TerminalHostId> terminal_id =
        backend->second->stable_terminal_id();
    if (terminal_id) {
      const std::optional<TerminalHostIncarnation> incarnation =
          backend->second->stable_terminal_incarnation();
      CmuxPendingTerminalClose pending(
          EncodeTerminalHostId(*terminal_id),
          incarnation ? EncodeTerminalHostId(*incarnation) : std::string(),
          backend->second->terminal_close_mutation_id());
      pending_terminal_closes_.insert_or_assign(
          EncodeTerminalHostId(*terminal_id), std::move(pending));
    }
    backend->second->Close();
    UnregisterTerminalBackend(tab, backend->second);
    terminal_backends_.erase(backend);
    RetryPendingTerminalCloses();
  }

  // WindowModel deliberately keeps one tab in the final pane. A web fallback
  // makes an explicit/canonical terminal close obey that invariant without
  // retaining a dead terminal or implicitly closing its workspace.
  bool EnsureWebFallbackForTerminal(PaneId pane_id, SurfaceTabId tab) {
    const WorkspaceId workspace = WorkspaceForPane(pane_id);
    Pane* pane = model_.FindPane(workspace, pane_id);
    if (!pane || pane->IndexOfTab(tab) < 0 || pane->tabs.size() > 1 ||
        model_.PanesOf(workspace).size() > 1) {
      return true;
    }
    CmuxPaneView* view = ViewForPane(pane_id);
    if (!view) {
      return false;
    }
    const SurfaceTabId fallback =
        model_.AddTab(workspace, pane_id, SurfaceKind::kWeb);
    if (fallback == kInvalidId) {
      return false;
    }
    CreateSurfaceForTab(view, fallback, SurfaceKind::kWeb, GURL(),
                        std::string());
    if (view->SurfaceFor(fallback)) {
      return true;
    }
    model_.CloseTab(workspace, pane_id, fallback);
    RefreshPaneTabs(pane_id);
    return false;
  }

  // Runs on a later task: close a tab; when it was the pane's last tab the
  // model also closes the pane (and its column when it was the column's only
  // pane), except for the workspace's last pane, which always stays.
  void CloseTabNow(PaneId pane_id, SurfaceTabId tab) {
    CancelActiveDragForMutation();
    Pane* pane = model_.FindPane(ws_, pane_id);
    CmuxPaneView* view = ViewForPane(pane_id);
    if (!pane || !view || pane->IndexOfTab(tab) < 0) {
      return;
    }
    const SurfaceTab* closing_tab = pane->FindTab(tab);
    if (closing_tab && closing_tab->kind == SurfaceKind::kWeb) {
      content::WebContents* contents = WebContentsForSurface(tab);
      Browser* browser = BrowserForWorkspace(ws_);
      if (contents && browser) {
        const bool restore_focused_pane =
            FocusedPane() == pane_id && pane->selected == tab;
        if (restore_focused_pane) {
          if (focused_web_tab_close_workspace_ != kInvalidId) {
            return;
          }
          BeginFocusedWebTabClose(ws_, pane_id, tab, contents);
        }
        chrome::CloseWebContents(browser, contents, /*add_to_history=*/true);
      }
      return;
    }
    if (!EnsureWebFallbackForTerminal(pane_id, tab)) {
      LOG(ERROR) << "cmux-views: could not create web fallback for terminal "
                    "close";
      return;
    }
    // EnsureWebFallbackForTerminal may reallocate the pane's tab vector.
    pane = model_.FindPane(ws_, pane_id);
    view = ViewForPane(pane_id);
    if (!pane || !view || pane->IndexOfTab(tab) < 0) {
      return;
    }
    CloseTerminalBackend(tab);
    model_.CloseTab(ws_, pane_id, tab);
    if (model_.FindPane(ws_, pane_id)) {
      // The pane survived: drop the tab's surface and land on the new
      // selection.
      view->RemoveSurface(tab);
      RefreshPaneTabs(pane_id);
      focus_.FocusPane(pane_id, /*move_keyboard=*/true);
      return;
    }
    // The pane (and possibly its column) is gone: destroy the whole view --
    // surfaces and their WebContents with it.
    DestroyPaneView(pane_id, /*notify_focus=*/true);
  }

  void CloseOtherTabsNow(PaneId pane_id, SurfaceTabId keep) {
    CancelActiveDragForMutation();
    Pane* pane = model_.FindPane(ws_, pane_id);
    CmuxPaneView* view = ViewForPane(pane_id);
    if (!pane || !view || pane->IndexOfTab(keep) < 0) {
      return;
    }
    std::vector<SurfaceTabId> closing;
    for (const SurfaceTab& tab : pane->tabs) {
      if (tab.id != keep) {
        closing.push_back(tab.id);
      }
    }
    if (closing.empty()) {
      model_.SelectTab(ws_, pane_id, keep);
      RefreshPaneTabs(pane_id);
      focus_.FocusPane(pane_id, /*move_keyboard=*/true);
      return;
    }
    model_.SelectTab(ws_, pane_id, keep);
    ActivateBrowserTabForSurface(keep);
    for (SurfaceTabId tab : closing) {
      Pane* current_pane = model_.FindPane(ws_, pane_id);
      const SurfaceTab* closing_tab =
          current_pane ? current_pane->FindTab(tab) : nullptr;
      if (closing_tab && closing_tab->kind == SurfaceKind::kWeb) {
        content::WebContents* contents = WebContentsForSurface(tab);
        Browser* browser = BrowserForWorkspace(ws_);
        if (contents && browser) {
          chrome::CloseWebContents(browser, contents,
                                   /*add_to_history=*/true);
        }
      } else if (closing_tab) {
        CloseTerminalBackend(tab);
        model_.CloseTab(ws_, pane_id, tab);
        view->RemoveSurface(tab);
      }
    }
    RefreshPaneTabs(pane_id);
    focus_.FocusPane(pane_id, /*move_keyboard=*/true);
  }

  void CloseTabsOnSideNow(PaneId pane_id, SurfaceTabId keep, bool close_left) {
    CancelActiveDragForMutation();
    Pane* pane = model_.FindPane(ws_, pane_id);
    if (!pane) {
      return;
    }
    const int keep_index = pane->IndexOfTab(keep);
    if (keep_index < 0) {
      return;
    }
    std::vector<SurfaceTabId> closing;
    for (int i = 0; i < static_cast<int>(pane->tabs.size()); ++i) {
      if ((close_left && i < keep_index) || (!close_left && i > keep_index)) {
        closing.push_back(pane->tabs[i].id);
      }
    }
    model_.SelectTab(ws_, pane_id, keep);
    ActivateBrowserTabForSurface(keep);
    for (SurfaceTabId tab : closing) {
      CloseTabNow(pane_id, tab);
    }
    if (model_.FindPane(ws_, pane_id)) {
      RefreshPaneTabs(pane_id);
      focus_.FocusPane(pane_id, /*move_keyboard=*/true);
    }
  }

  void NewTabRightNow(PaneId pane_id, SurfaceTabId after) {
    CancelActiveDragForMutation();
    Pane* pane = model_.FindPane(ws_, pane_id);
    CmuxPaneView* view = ViewForPane(pane_id);
    if (!pane || !view) {
      return;
    }
    const int after_index = pane->IndexOfTab(after);
    if (after_index < 0) {
      return;
    }
    const SurfaceTabId tab = model_.AddTab(ws_, pane_id, SurfaceKind::kWeb);
    if (tab == kInvalidId) {
      return;
    }
    CreateSurfaceForTab(view, tab, SurfaceKind::kWeb, GURL(), std::string());
    model_.ReorderTab(ws_, pane_id, tab, after_index + 1);
    SyncBrowserTabOrder(ws_);
    RefreshPaneTabs(pane_id);
    FocusNewWebTab(pane_id, tab);
  }

  void MoveTabToNewColumnNow(PaneId pane_id, SurfaceTabId tab) {
    CancelActiveDragForMutation();
    if (!model_.FindPane(ws_, pane_id)) {
      return;
    }
    const int column = model_.ColumnIndexOf(ws_, pane_id);
    if (column < 0) {
      return;
    }
    DragCommit commit;
    commit.source_pane = pane_id;
    commit.tab = tab;
    commit.target.kind = DropKind::kNewColumn;
    commit.target.column_index = column + 1;
    CommitTabDrop(commit);
  }

  void SplitWithTabNow(PaneId pane_id,
                       SurfaceTabId tab,
                       SplitOrientation orientation) {
    CancelActiveDragForMutation();
    if (!model_.FindPane(ws_, pane_id)) {
      return;
    }
    DragCommit commit;
    commit.source_pane = pane_id;
    commit.tab = tab;
    commit.target.kind = DropKind::kPaneEdge;
    commit.target.pane = pane_id;
    commit.target.orientation = orientation;
    commit.target.insert_first = false;
    CommitTabDrop(commit);
  }

  void NewSplitWithCurrentTabNow(PaneId pane_id,
                                 SurfaceTabId tab,
                                 SplitOrientation orientation) {
    CancelActiveDragForMutation();
    Pane* pane = model_.FindPane(ws_, pane_id);
    const SurfaceTab* source = pane ? pane->FindTab(tab) : nullptr;
    if (!source) {
      return;
    }
    GURL url;
    if (content::WebContents* contents = WebContentsForSurface(tab)) {
      url = contents->GetVisibleURL();
    }
    const PaneId fresh = model_.SplitPane(
        ws_, pane_id, orientation, 0.5, source->kind);
    if (fresh == kInvalidId) {
      return;
    }
    RealizePane(fresh, url.is_valid() ? url : GURL());
    MarkPaneForSplitEntrance(fresh, SplitOrientation::kHorizontal,
                             /*insert_first=*/false);
    focus_.OnPaneAdded(fresh);
  }

  WorkspaceId WorkspaceForPane(PaneId pane_id) const {
    for (const Workspace& workspace : model_.workspaces()) {
      if (model_.FindPane(workspace.id, pane_id)) {
        return workspace.id;
      }
    }
    return kInvalidId;
  }

  void NotifyWorkspaceHoverCardDataChanged(WorkspaceId workspace,
                                           PaneId pane,
                                           SurfaceTabId surface) {
    const Workspace* workspace_model = model_.GetWorkspace(workspace);
    const Pane* pane_model = model_.FindPane(workspace, pane);
    if (rail_ && workspace_model && pane_model &&
        workspace_model->focused == pane &&
        pane_model->selected == surface) {
      rail_->NotifyWorkspaceDataChanged(workspace);
    }
  }

  std::optional<std::pair<WorkspaceId, PaneId>> TerminalTabLocation(
      SurfaceTabId tab) const {
    for (const Workspace& workspace : model_.workspaces()) {
      for (PaneId pane_id : model_.PanesOf(workspace.id)) {
        const Pane* pane = model_.FindPane(workspace.id, pane_id);
        const int index = pane ? pane->IndexOfTab(tab) : -1;
        const SurfaceTab* surface =
            index >= 0 ? &pane->tabs[static_cast<size_t>(index)] : nullptr;
        if (surface && surface->kind == SurfaceKind::kTerminal) {
          return std::make_pair(workspace.id, pane_id);
        }
      }
    }
    return std::nullopt;
  }

  void ClearPendingTuiTerminalFocus() {
    pending_tui_terminal_focus_.reset();
    ++pending_tui_terminal_focus_generation_;
  }

  void FencePendingTuiTerminalFocusForGuiInteraction(
      PaneId interacted_pane = kInvalidId) {
    // The activation click/key must win even if a terminal's independent
    // registry stream resolves or rematerializes after this event. Remember
    // the canonical selection being cancelled, not its disposable local tab,
    // and suppress only that selection until the owner mux genuinely changes.
    std::optional<std::pair<std::string, std::string>> selection;
    if (pending_tui_terminal_focus_) {
      selection = std::make_pair(pending_tui_terminal_focus_->workspace_key,
                                 pending_tui_terminal_focus_->terminal_id);
    } else if (retry_tui_terminal_focus_selection_) {
      selection = retry_tui_terminal_focus_selection_;
    } else if (last_applied_tui_selection_ &&
               !last_applied_tui_selection_->second.empty()) {
      selection = last_applied_tui_selection_;
    }

    // Direct input on the exact canonical terminal has already satisfied the
    // handoff. Do not poison recovery for that terminal if its disposable
    // Chromium projection is later rematerialized. Input elsewhere suppresses
    // only this owner-mux selection until the owner genuinely changes it.
    bool interaction_is_on_target = false;
    if (selection && interacted_pane != kInvalidId &&
        !selection->second.empty()) {
      const Workspace* workspace =
          model_.FindWorkspaceByRegistryKey(selection->first);
      const auto terminal = terminal_tab_by_id_.find(selection->second);
      if (workspace && workspace->id == ws_ &&
          terminal != terminal_tab_by_id_.end()) {
        const std::optional<std::pair<WorkspaceId, PaneId>> location =
            TerminalTabLocation(terminal->second);
        const Pane* pane =
            location ? model_.FindPane(location->first, location->second)
                     : nullptr;
        interaction_is_on_target = location &&
                                   location->first == workspace->id &&
                                   location->second == interacted_pane &&
                                   pane && pane->selected == terminal->second &&
                                   workspace->focused == interacted_pane;
      }
    }
    if (selection) {
      if (interaction_is_on_target) {
        if (suppressed_tui_terminal_focus_selection_ == selection) {
          suppressed_tui_terminal_focus_selection_.reset();
        }
      } else {
        suppressed_tui_terminal_focus_selection_ = std::move(selection);
      }
    }
    retry_tui_terminal_focus_selection_.reset();
    ClearPendingTuiTerminalFocus();
  }

  std::optional<std::pair<PaneId, SurfaceTabId>>
  ResolveCurrentTuiTerminalFocusIntent(
      const TuiTerminalFocusIntent& intent) const {
    if (!workspace_snapshot_ || workspace_snapshot_->provisional ||
        workspace_snapshot_->active_workspace_key != intent.workspace_key ||
        workspace_snapshot_->active_terminal_id != intent.terminal_id) {
      return std::nullopt;
    }
    const Workspace* workspace = nullptr;
    for (const Workspace& candidate : model_.workspaces()) {
      if (candidate.registry_key == intent.workspace_key) {
        workspace = &candidate;
        break;
      }
    }
    if (!workspace || workspace->id != ws_) {
      return std::nullopt;
    }
    const auto terminal = terminal_tab_by_id_.find(intent.terminal_id);
    if (terminal == terminal_tab_by_id_.end()) {
      return std::nullopt;
    }
    const std::optional<std::pair<WorkspaceId, PaneId>> location =
        TerminalTabLocation(terminal->second);
    if (!location || location->first != workspace->id) {
      return std::nullopt;
    }
    const Pane* pane = model_.FindPane(location->first, location->second);
    const int tab_index = pane ? pane->IndexOfTab(terminal->second) : -1;
    const SurfaceTab* tab =
        tab_index >= 0 ? &pane->tabs[static_cast<size_t>(tab_index)] : nullptr;
    if (!pane || !tab || tab->kind != SurfaceKind::kTerminal ||
        pane->selected != terminal->second ||
        workspace->focused != location->second) {
      return std::nullopt;
    }
    return std::make_pair(location->second, terminal->second);
  }

  void ApplyPendingTuiTerminalFocus(uint64_t generation) {
    if (generation != pending_tui_terminal_focus_generation_ ||
        !pending_tui_terminal_focus_ || !GetWidget() ||
        !GetWidget()->IsActive()) {
      return;
    }
    const std::optional<std::pair<PaneId, SurfaceTabId>> target =
        ResolveCurrentTuiTerminalFocusIntent(*pending_tui_terminal_focus_);
    if (!target) {
      // Independent workspace and terminal streams may temporarily leave the
      // canonical target unresolved. Keep the intent armed; the next
      // authoritative reconciliation calls ApplyCmuxTuiSelection again.
      return;
    }
    pending_tui_terminal_focus_.reset();
    retry_tui_terminal_focus_selection_.reset();
    if (CmuxPane* pane = ViewForPane(target->first)) {
      pane->FocusContent();
    }
  }

  void QueuePendingTuiTerminalFocus() {
    if (!pending_tui_terminal_focus_) {
      return;
    }
    const uint64_t generation = pending_tui_terminal_focus_generation_;
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(&CmuxWindowView::ApplyPendingTuiTerminalFocus,
                                  weak_factory_.GetWeakPtr(), generation));
  }

  void SetTuiTerminalFocusIntent(std::string workspace_key,
                                 std::string terminal_id) {
    pending_tui_terminal_focus_ = TuiTerminalFocusIntent{
        std::move(workspace_key), std::move(terminal_id)};
    ++pending_tui_terminal_focus_generation_;
    if (GetWidget() && GetWidget()->IsActive()) {
      QueuePendingTuiTerminalFocus();
    }
  }

  void ApplyCmuxTuiSelection() {
    if (!workspace_snapshot_ || workspace_snapshot_->provisional ||
        workspace_snapshot_->active_workspace_key.empty()) {
      return;
    }
    const std::pair<std::string, std::string> selection(
        workspace_snapshot_->active_workspace_key,
        workspace_snapshot_->active_terminal_id);
    const auto terminal =
        terminal_tab_by_id_.find(workspace_snapshot_->active_terminal_id);
    const SurfaceTabId mapped_terminal =
        terminal == terminal_tab_by_id_.end() ? kInvalidId : terminal->second;
    if (suppressed_tui_terminal_focus_selection_ &&
        *suppressed_tui_terminal_focus_selection_ != selection) {
      suppressed_tui_terminal_focus_selection_.reset();
    }
    const bool retry_pending = retry_tui_terminal_focus_selection_ == selection;
    if (last_applied_tui_selection_ == selection &&
        !pending_tui_terminal_focus_ &&
        last_applied_tui_terminal_tab_ == mapped_terminal && !retry_pending) {
      return;
    }
    if (last_applied_tui_selection_ != selection) {
      // A different owner-mux selection supersedes any deferred responder
      // grab, even if its local projection has not materialized yet.
      ClearPendingTuiTerminalFocus();
      retry_tui_terminal_focus_selection_.reset();
      suppressed_tui_terminal_focus_selection_.reset();
      last_applied_tui_terminal_tab_.reset();
    }
    Workspace* workspace = model_.FindWorkspaceByRegistryKey(
        workspace_snapshot_->active_workspace_key);
    if (!workspace) {
      return;
    }
    std::optional<std::pair<WorkspaceId, PaneId>> location;
    if (terminal != terminal_tab_by_id_.end()) {
      location = TerminalTabLocation(terminal->second);
      if (!location || location->first != workspace->id) {
        location.reset();
      } else {
        const Pane* pane = model_.FindPane(location->first, location->second);
        if (!pane || pane->IndexOfTab(terminal->second) < 0) {
          location.reset();
        }
      }
    }
    const bool workspace_changed = workspace->id != ws_;
    if (workspace_changed) {
      CancelActiveDragForMutation();
      // Suppress intermediate focus only after the canonical terminal has
      // resolved inside this destination workspace. Independent workspace and
      // terminal streams otherwise use normal fallback focus, never leaving
      // AppKit pointed into the now-hidden old workspace.
      const bool terminal_focus_follows = location.has_value();
      SwitchWorkspace(workspace->id,
                      /*move_keyboard=*/!terminal_focus_follows);
      workspace = model_.GetWorkspace(ws_);
      if (!workspace) {
        return;
      }
    }

    // Empty workspaces and active non-terminal TUI surfaces still synchronize
    // workspace focus. A stable terminal id is the only safe join key for a
    // TUI tab and its disposable Chromium projection.
    if (workspace_snapshot_->active_terminal_id.empty()) {
      ClearPendingTuiTerminalFocus();
      retry_tui_terminal_focus_selection_.reset();
      suppressed_tui_terminal_focus_selection_.reset();
      last_applied_tui_selection_ = selection;
      last_applied_tui_terminal_tab_.reset();
      return;
    }
    // Keep a canonical intent armed while the independent terminal stream is
    // unresolved. Its task resolves the disposable local tab at consumption
    // time, and ApplyTerminalPlacementPlan calls back here after
    // rematerializing that tab.
    last_applied_tui_selection_ = selection;
    last_applied_tui_terminal_tab_ = mapped_terminal;
    if (suppressed_tui_terminal_focus_selection_ == selection) {
      retry_tui_terminal_focus_selection_.reset();
      ClearPendingTuiTerminalFocus();
      return;
    }
    if (!location) {
      SetTuiTerminalFocusIntent(workspace_snapshot_->active_workspace_key,
                                workspace_snapshot_->active_terminal_id);
      retry_tui_terminal_focus_selection_.reset();
      return;
    }
    Pane* pane = model_.FindPane(location->first, location->second);
    if (!pane || !pane->FindTab(terminal->second)) {
      return;
    }
    const bool tab_changed = pane->selected != terminal->second;
    const bool pane_changed = workspace->focused != location->second;
    if ((tab_changed || pane_changed) && !workspace_changed) {
      CancelActiveDragForMutation();
    }
    if (tab_changed) {
      model_.SelectTab(location->first, location->second, terminal->second);
      RefreshPaneTabs(location->second);
    }
    focus_.FocusPane(location->second, /*move_keyboard=*/false);
    SetTuiTerminalFocusIntent(workspace_snapshot_->active_workspace_key,
                              workspace_snapshot_->active_terminal_id);
    retry_tui_terminal_focus_selection_.reset();
  }

  void RegisterTerminalBackend(
      SurfaceTabId tab,
      const scoped_refptr<CmuxTerminalBackend>& backend) {
    if (!backend || !backend->stable_terminal_id()) {
      return;
    }
    const std::string terminal_id =
        EncodeTerminalHostId(*backend->stable_terminal_id());
    auto existing = terminal_tab_by_id_.find(terminal_id);
    if (existing != terminal_tab_by_id_.end() && existing->second != tab) {
      VLOG(1) << "cmux-views: duplicate local terminal projection for "
              << terminal_id;
    }
    terminal_tab_by_id_.insert_or_assign(terminal_id, tab);
  }

  void UnregisterTerminalBackend(
      SurfaceTabId tab,
      const scoped_refptr<CmuxTerminalBackend>& backend) {
    if (!backend || !backend->stable_terminal_id()) {
      return;
    }
    const std::string terminal_id =
        EncodeTerminalHostId(*backend->stable_terminal_id());
    auto existing = terminal_tab_by_id_.find(terminal_id);
    if (existing != terminal_tab_by_id_.end() && existing->second == tab) {
      terminal_tab_by_id_.erase(existing);
    }
  }

  void RebuildTerminalBackendIndex() {
    terminal_tab_by_id_.clear();
    for (const auto& [tab, backend] : terminal_backends_) {
      RegisterTerminalBackend(tab, backend);
    }
  }

  SurfaceTabId TerminalTabForLocalKey(const std::string& local_key) const {
    int64_t parsed = kInvalidId;
    if (!base::StringToInt64(local_key, &parsed) || parsed == kInvalidId ||
        !terminal_backends_.contains(parsed)) {
      return kInvalidId;
    }
    return parsed;
  }

  std::vector<CmuxLocalTerminalPlacement> CaptureLocalTerminalPlacements()
      const {
    std::vector<CmuxLocalTerminalPlacement> local;
    local.reserve(terminal_backends_.size());
    for (const auto& [tab, backend] : terminal_backends_) {
      const auto location = TerminalTabLocation(tab);
      if (!backend || !location) {
        continue;
      }
      const std::optional<TerminalHostId> terminal_id =
          backend->stable_terminal_id();
      const std::optional<TerminalHostIncarnation> incarnation =
          backend->stable_terminal_incarnation();
      const std::optional<CmuxTuiSurfaceId> surface = backend->surface_id();
      local.emplace_back(
          base::NumberToString(tab),
          terminal_id ? EncodeTerminalHostId(*terminal_id) : std::string(),
          incarnation ? EncodeTerminalHostId(*incarnation) : std::string(),
          WorkspaceRegistryKey(location->first),
          surface ? base::NumberToString(*surface) : std::string(),
          backend->terminal_create_mutation_id(),
          backend->terminal_create_pending());
    }
    return local;
  }

  std::vector<std::string> CurrentGuiWorkspaceKeys() const {
    std::vector<std::string> keys;
    keys.reserve(model_.workspace_count());
    for (const Workspace& workspace : model_.workspaces()) {
      if (!workspace.registry_key.empty()) {
        keys.push_back(workspace.registry_key);
      }
    }
    return keys;
  }

  std::vector<CmuxPendingTerminalClose> CapturePendingTerminalCloses() const {
    std::vector<CmuxPendingTerminalClose> pending;
    pending.reserve(pending_terminal_closes_.size());
    for (const auto& [terminal_id, close] : pending_terminal_closes_) {
      pending.push_back(close);
    }
    return pending;
  }

  std::optional<TerminalHostIncarnation> DecodeTerminalIncarnation(
      const std::string& incarnation) const {
    if (incarnation.empty()) {
      return std::nullopt;
    }
    TerminalHostIncarnation decoded{};
    if (!DecodeTerminalHostUuidV4(incarnation, &decoded)) {
      return std::nullopt;
    }
    return decoded;
  }

  void RemoveCanonicalTerminalProjection(SurfaceTabId tab) {
    auto backend_it = terminal_backends_.find(tab);
    if (backend_it == terminal_backends_.end()) {
      return;
    }
    const auto location = TerminalTabLocation(tab);
    if (!location) {
      UnregisterTerminalBackend(tab, backend_it->second);
      terminal_backends_.erase(backend_it);
      return;
    }
    const WorkspaceId workspace = location->first;
    const PaneId pane_id = location->second;
    CmuxPaneView* view = ViewForPane(pane_id);
    if (view) {
      view->RemoveSurface(tab);
    }
    UnregisterTerminalBackend(tab, backend_it->second);
    // Canonical Remove is projection-only. Never call backend->Close(): the
    // registry has already committed the tombstone/absence.
    terminal_backends_.erase(backend_it);
    model_.RemoveProjectedTab(workspace, pane_id, tab);
    if (model_.FindPane(workspace, pane_id)) {
      RefreshPaneTabs(pane_id);
      if (workspace == ws_) {
        ActivateSelectedWebTabInPane(pane_id);
      }
    } else if (view) {
      DestroyPaneView(pane_id, /*notify_focus=*/false);
    }
  }

  void ResolveCanonicalTerminal(
      const CmuxTerminalPlacementAction& action,
      const std::string& registry_id,
      const std::string& registry_generation) {
    if (action.resolve_reason == CmuxTerminalResolveReason::kWorkspaceMissing) {
      terminal_reconcile_waiting_for_workspace_ = true;
      tui_client_->RefreshWorkspaceRegistry();
      return;
    }
    const SurfaceTabId tab = TerminalTabForLocalKey(action.local_key);
    auto backend = terminal_backends_.find(tab);
    if (backend == terminal_backends_.end()) {
      tui_client_->RefreshTerminalRegistry();
      return;
    }
    if (action.resolve_reason == CmuxTerminalResolveReason::kPendingMutation) {
      return;
    }
    if (action.resolve_reason ==
        CmuxTerminalResolveReason::kStableIdentityMissing) {
      tui_client_->RefreshTerminalRegistry();
      return;
    }
    const std::optional<TerminalHostIncarnation> incarnation =
        DecodeTerminalIncarnation(action.incarnation);
    if (!action.incarnation.empty() && !incarnation) {
      tui_client_->RefreshTerminalRegistry();
      return;
    }
    if (!backend->second->ApplyCanonicalLifecycle(
            action.lifecycle, incarnation, registry_id,
            registry_generation, terminal_registry_replaced_pending_,
            terminal_generation_change_pending_)) {
      LOG(ERROR) << "cmux-views: rejected canonical terminal lifecycle for "
                 << action.terminal_id;
      // The fenced canonical record wins. Replace only this renderer/backend
      // projection in place; never send Close for the retired local binding.
      RemoveCanonicalTerminalProjection(tab);
      MaterializeCanonicalTerminal(action, registry_id, registry_generation);
      return;
    }
    RegisterTerminalBackend(tab, backend->second);
  }

  void MaterializeCanonicalTerminal(
      const CmuxTerminalPlacementAction& action,
      const std::string& registry_id,
      const std::string& registry_generation) {
    if (terminal_tab_by_id_.contains(action.terminal_id)) {
      return;
    }
    Workspace* workspace =
        model_.FindWorkspaceByRegistryKey(action.workspace_key);
    if (!workspace) {
      terminal_reconcile_waiting_for_workspace_ = true;
      tui_client_->RefreshWorkspaceRegistry();
      return;
    }
    const std::vector<PaneId> panes = model_.PanesOf(workspace->id);
    if (panes.empty()) {
      tui_client_->RefreshWorkspaceRegistry();
      return;
    }
    // Canonical terminal placement intentionally does not persist Browser pane
    // layout. The first in-order pane is deterministic across this local model
    // and, unlike the focused pane, cannot make a remote create steal focus.
    const PaneId pane_id = panes.front();
    CmuxPaneView* view = ViewForPane(pane_id);
    if (!view) {
      RealizePane(pane_id, GURL());
      view = ViewForPane(pane_id);
    }
    Pane* pane = model_.FindPane(workspace->id, pane_id);
    if (!pane || !view) {
      return;
    }
    TerminalHostId terminal_id{};
    if (!DecodeTerminalHostUuidV4(action.terminal_id, &terminal_id)) {
      tui_client_->RefreshTerminalRegistry();
      return;
    }
    const std::optional<TerminalHostIncarnation> incarnation =
        DecodeTerminalIncarnation(action.incarnation);
    if (!action.incarnation.empty() && !incarnation) {
      tui_client_->RefreshTerminalRegistry();
      return;
    }

    const SurfaceTabId previous_selection = pane->selected;
    const PaneId previous_focus = workspace->focused;
    const SurfaceTabId tab =
        model_.AddTab(workspace->id, pane_id, SurfaceKind::kTerminal,
                      action.lifecycle == CmuxTerminalLifecycle::kExited
                          ? "Terminal (exited)"
                          : "Terminal");
    if (tab == kInvalidId) {
      return;
    }
    CmuxTerminalBackendSeed seed;
    seed.terminal_id = terminal_id;
    seed.terminal_incarnation = incarnation;
    seed.lifecycle = action.lifecycle;
    seed.registry_id = registry_id;
    seed.registry_generation = registry_generation;
    CreateSurfaceForTab(view, tab, SurfaceKind::kTerminal, GURL(),
                        std::string(), std::move(seed));
    if (!terminal_backends_.contains(tab)) {
      model_.CloseTab(workspace->id, pane_id, tab);
      return;
    }
    if (pane->FindTab(previous_selection)) {
      model_.SelectTab(workspace->id, pane_id, previous_selection);
    }
    if (model_.FindPane(workspace->id, previous_focus)) {
      model_.FocusPane(workspace->id, previous_focus);
    }
    RefreshPaneTabs(pane_id);
  }

  void MoveCanonicalTerminal(const CmuxTerminalPlacementAction& action) {
    const SurfaceTabId tab = TerminalTabForLocalKey(action.local_key);
    const auto source_location = TerminalTabLocation(tab);
    Workspace* destination =
        model_.FindWorkspaceByRegistryKey(action.workspace_key);
    if (!source_location || !destination) {
      terminal_reconcile_waiting_for_workspace_ = true;
      tui_client_->RefreshWorkspaceRegistry();
      return;
    }
    if (source_location->first == destination->id) {
      return;
    }
    const std::vector<PaneId> destination_panes =
        model_.PanesOf(destination->id);
    if (destination_panes.empty()) {
      tui_client_->RefreshWorkspaceRegistry();
      return;
    }
    const PaneId source_pane = source_location->second;
    const PaneId destination_pane = destination_panes.front();
    CmuxPaneView* destination_view = ViewForPane(destination_pane);
    if (!destination_view) {
      RealizePane(destination_pane, GURL());
      destination_view = ViewForPane(destination_pane);
    }
    Pane* destination_model =
        model_.FindPane(destination->id, destination_pane);
    if (!destination_view || !destination_model ||
        !EnsureWebFallbackForTerminal(source_pane, tab)) {
      return;
    }
    const SurfaceTabId previous_destination_selection =
        destination_model->selected;
    const PaneId previous_destination_focus = destination->focused;
    CmuxPaneView* source_view = ViewForPane(source_pane);
    DetachedSurface detached = DetachSurface(source_pane, tab);
    model_.MoveTab(source_location->first, source_pane, tab, destination->id,
                   destination_pane, -1);
    destination_model = model_.FindPane(destination->id, destination_pane);
    if (!destination_model || !destination_model->FindTab(tab)) {
      if (detached.view && detached.surface) {
        AttachSurfaceToPane(source_pane, std::move(detached));
      }
      tui_client_->RefreshTerminalRegistry();
      return;
    }
    if (detached.view && detached.surface) {
      AttachSurfaceToPane(destination_pane, std::move(detached));
    }
    if (destination_model->FindTab(previous_destination_selection)) {
      model_.SelectTab(destination->id, destination_pane,
                       previous_destination_selection);
    }
    if (model_.FindPane(destination->id, previous_destination_focus)) {
      model_.FocusPane(destination->id, previous_destination_focus);
    }
    if (model_.FindPane(source_location->first, source_pane)) {
      RefreshPaneTabs(source_pane);
    } else if (source_view) {
      DestroyPaneView(source_pane, /*notify_focus=*/false);
    }
    RefreshPaneTabs(destination_pane);
  }

  void ApplyTerminalPlacementPlan(const CmuxTerminalPlacementPlan& plan) {
    if (plan.disposition == CmuxTerminalPlacementDisposition::kIgnored) {
      return;
    }
    if (!workspace_snapshot_ || plan.registry_id != workspace_registry_id_ ||
        plan.generation != workspace_registry_generation_) {
      // Terminal records never create workspaces. Snapshot streams are
      // independent and may arrive in either order; wait for the matching
      // workspace fence before mutating any GUI projection.
      terminal_reconcile_waiting_for_workspace_ = true;
      tui_client_->RefreshWorkspaceRegistry();
      return;
    }
    terminal_reconcile_waiting_for_workspace_ = false;
    for (const CmuxTerminalPlacementAction& action : plan.actions) {
      switch (action.kind) {
        case CmuxTerminalPlacementActionKind::kRemove:
          RemoveCanonicalTerminalProjection(
              TerminalTabForLocalKey(action.local_key));
          break;
        case CmuxTerminalPlacementActionKind::kResolve:
          ResolveCanonicalTerminal(action, plan.registry_id, plan.generation);
          break;
        case CmuxTerminalPlacementActionKind::kMove:
          MoveCanonicalTerminal(action);
          break;
        case CmuxTerminalPlacementActionKind::kMaterialize:
          MaterializeCanonicalTerminal(action, plan.registry_id,
                                       plan.generation);
          break;
      }
    }
    RebuildTerminalBackendIndex();
    terminal_registry_replaced_pending_ = false;
    terminal_generation_change_pending_ = false;
    ApplyCmuxTuiSelection();
    RefreshRail();
    ApplyVisualsAndScroll(/*animated=*/false);
  }

  void ReconcileCanonicalTerminals() {
    if (!terminal_placement_model_.has_snapshot()) {
      tui_client_->RefreshTerminalRegistry();
      return;
    }
    if (!workspace_snapshot_ ||
        terminal_placement_model_.registry_id() != workspace_registry_id_ ||
        terminal_placement_model_.generation() !=
            workspace_registry_generation_) {
      tui_client_->RefreshTerminalRegistry();
      return;
    }
    CmuxTerminalPlacementPlan plan = terminal_placement_model_.Reconcile(
        CaptureLocalTerminalPlacements(), CurrentGuiWorkspaceKeys(),
        CapturePendingTerminalCloses());
    if (plan.require_snapshot()) {
      tui_client_->RefreshTerminalRegistry();
      return;
    }
    ApplyTerminalPlacementPlan(plan);
  }

  void ConfirmTerminalAbsent(const std::string& terminal_id) {
    pending_terminal_closes_.erase(terminal_id);
    terminal_close_requests_in_flight_.erase(terminal_id);
    terminal_closes_awaiting_absence_.erase(terminal_id);
    if (pending_terminal_closes_.empty()) {
      terminal_close_retry_timer_.Stop();
    }
  }

  void RetryPendingTerminalCloses() {
    if (!tui_client_->ready() || !terminal_placement_model_.has_snapshot() ||
        terminal_placement_model_.needs_snapshot() || !workspace_snapshot_ ||
        terminal_placement_model_.registry_id() != workspace_registry_id_ ||
        terminal_placement_model_.generation() !=
            workspace_registry_generation_) {
      return;
    }
    for (const auto& [terminal_id, pending] : pending_terminal_closes_) {
      if (terminal_close_requests_in_flight_.contains(terminal_id) ||
          terminal_closes_awaiting_absence_.contains(terminal_id)) {
        continue;
      }
      TerminalHostId decoded_terminal_id{};
      if (!DecodeTerminalHostUuidV4(pending.terminal_id,
                                    &decoded_terminal_id)) {
        ConfirmTerminalAbsent(terminal_id);
        return;
      }
      std::optional<TerminalHostIncarnation> decoded_incarnation;
      if (!pending.incarnation.empty()) {
        TerminalHostIncarnation value{};
        if (!DecodeTerminalHostUuidV4(pending.incarnation, &value)) {
          ConfirmTerminalAbsent(terminal_id);
          return;
        }
        decoded_incarnation = value;
      }
      terminal_close_requests_in_flight_.insert(terminal_id);
      CmuxTuiTerminalMutation mutation;
      mutation.mutation_id = pending.mutation_id;
      mutation.expected_generation = terminal_placement_model_.generation();
      mutation.expected_terminal_revision =
          terminal_placement_model_.terminal_revision();
      tui_client_->CloseTerminal(
          decoded_terminal_id, decoded_incarnation, std::move(mutation),
          base::BindOnce(&CmuxWindowView::OnPendingTerminalCloseFinished,
                         weak_factory_.GetWeakPtr(), terminal_id,
                         terminal_placement_model_.registry_id(),
                         terminal_placement_model_.generation()));
      // Serialize close mutations so every precondition is based on the next
      // authoritative revision instead of issuing a batch against one stale
      // revision.
      return;
    }
  }

  void OnPendingTerminalCloseFinished(const std::string& terminal_id,
                                      const std::string& registry_id,
                                      const std::string& generation,
                                      CmuxTuiCommandResult result) {
    auto pending = pending_terminal_closes_.find(terminal_id);
    if (pending == pending_terminal_closes_.end()) {
      return;
    }
    terminal_close_requests_in_flight_.erase(terminal_id);
    if (registry_id != terminal_placement_model_.registry_id() ||
        generation != terminal_placement_model_.generation()) {
      terminal_closes_awaiting_absence_.erase(terminal_id);
      tui_client_->RefreshTerminalRegistry();
      return;
    }
    if (result.ok) {
      terminal_closes_awaiting_absence_.insert(terminal_id);
      tui_client_->RefreshTerminalRegistry();
      return;
    }
    terminal_closes_awaiting_absence_.erase(terminal_id);
    tui_client_->RefreshTerminalRegistry();
    if (!terminal_close_retry_timer_.IsRunning()) {
      terminal_close_retry_timer_.Start(
          FROM_HERE, base::Milliseconds(250),
          base::BindOnce(&CmuxWindowView::RetryPendingTerminalCloses,
                         weak_factory_.GetWeakPtr()));
    }
  }

  // Sync a pane view's tab strip + surface visibility to the model.
  void RefreshPaneTabs(PaneId pane_id) {
    if (drag_controller_ && drag_controller_->active()) {
      deferred_tab_refreshes_.insert(pane_id);
      return;
    }
    const WorkspaceId workspace = WorkspaceForPane(pane_id);
    Pane* pane = model_.FindPane(workspace, pane_id);
    CmuxPaneView* view = ViewForPane(pane_id);
    if (pane && view) {
      view->SetTabs(pane->tabs, pane->selected);
      NotifyWorkspaceHoverCardDataChanged(workspace, pane_id, pane->selected);
      RefreshRailIfFaviconChanged(workspace, pane_id, *view);
    }
  }

  void FlushDeferredPaneTabs() {
    std::set<PaneId> panes = std::move(deferred_tab_refreshes_);
    deferred_tab_refreshes_.clear();
    for (PaneId pane : panes) {
      RefreshPaneTabs(pane);
    }
  }

  void MoveFocusDirection(Direction direction) {
    const Workspace* ws = model_.GetWorkspace(ws_);
    const PaneId neighbor =
        ws ? SpatialNeighborPane(*ws, FocusedPane(), direction) : kInvalidId;
    if (neighbor != kInvalidId) {
      focus_.FocusPane(neighbor, /*move_keyboard=*/true);
    }
  }

  // Weak-ptr-safe forwarder for a pane's activation callback (see above).
  void OnPaneActivatedById(PaneId id) {
    // This callback also runs for programmatic FocusContent. Direct user input
    // is fenced separately by the root pre-target handler or the surface's
    // interaction callback, so a workspace handoff cannot cancel itself here.
    focus_.OnPaneActivated(id);
    ActivateSelectedWebTabInPane(id);
  }

  CmuxPaneView* ViewForPane(PaneId id) {
    auto it = views_.find(id);
    return it == views_.end() ? nullptr : it->second.get();
  }
  PaneId FocusedPane() {
    const Workspace* ws = model_.GetWorkspace(ws_);
    return ws ? ws->focused : kInvalidId;
  }
  CmuxPane* FocusedView() { return ViewForPane(FocusedPane()); }

  // A new web tab follows browser convention: select its pane and put the
  // insertion point in the omnibox. Defer until its LocationBarView has
  // completed attachment. Identity checks prevent a stale task from stealing
  // focus after a rapid tab or workspace switch.
  void FocusNewWebTab(PaneId pane, SurfaceTabId tab) {
    const WorkspaceId workspace = ws_;
    focus_.FocusPane(pane, /*move_keyboard=*/false);
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE,
        base::BindOnce(&CmuxWindowView::DeferredFocusNewWebTab,
                       weak_factory_.GetWeakPtr(), workspace, pane, tab));
  }

  void DeferredFocusNewWebTab(WorkspaceId workspace,
                              PaneId pane,
                              SurfaceTabId tab) {
    if (workspace != ws_) {
      return;
    }
    const Workspace* current_workspace = model_.GetWorkspace(workspace);
    if (!current_workspace || current_workspace->focused != pane) {
      return;
    }
    Pane* model_pane = model_.FindPane(workspace, pane);
    SurfaceTab* model_tab = model_pane ? model_pane->FindTab(tab) : nullptr;
    if (!model_tab || model_pane->selected != tab ||
        model_tab->kind != SurfaceKind::kWeb) {
      return;
    }
    if (CmuxPaneView* pane_view = ViewForPane(pane)) {
      if (CmuxSurface* surface = pane_view->SurfaceFor(tab)) {
        surface->FocusOmnibar();
      }
    }
  }

  bool ExecuteKeyCommand(std::string_view command_id,
                         bool is_repeat,
                         std::string_view args_json = std::string_view(),
                         int recursion_depth = 0) {
    if (command_id == "runCommands") {
      if (recursion_depth >= 16 || args_json.empty()) {
        return true;
      }
      std::optional<base::Value> args = base::JSONReader::Read(
          args_json, base::JSON_PARSE_CHROMIUM_EXTENSIONS |
                         base::JSON_ALLOW_TRAILING_COMMAS);
      const base::ListValue* commands =
          args && args->is_dict() ? args->GetDict().FindList("commands")
                                  : nullptr;
      if (!commands) {
        LOG(WARNING) << "cmux-keymap: runCommands requires args.commands";
        return true;
      }
      for (const base::Value& item : *commands) {
        if (item.is_string()) {
          ExecuteKeyCommand(item.GetString(), /*is_repeat=*/false,
                            std::string_view(), recursion_depth + 1);
          continue;
        }
        if (!item.is_dict()) {
          continue;
        }
        const std::string* nested_command =
            item.GetDict().FindString("command");
        if (!nested_command) {
          continue;
        }
        std::string nested_args;
        if (const base::Value* value = item.GetDict().Find("args")) {
          base::JSONWriter::Write(*value, &nested_args);
        }
        ExecuteKeyCommand(*nested_command, /*is_repeat=*/false, nested_args,
                          recursion_depth + 1);
      }
      return true;
    }
    constexpr std::string_view kExtensionPrefix = "extension.";
    constexpr std::string_view kExtensionSuffix = ".action";
    if (command_id.substr(0, kExtensionPrefix.size()) == kExtensionPrefix &&
        command_id.size() >= kExtensionSuffix.size() &&
        command_id.substr(command_id.size() - kExtensionSuffix.size()) ==
            kExtensionSuffix &&
        command_id.size() > kExtensionPrefix.size() + kExtensionSuffix.size()) {
      if (is_repeat) {
        return true;
      }
      const std::string_view extension_id = command_id.substr(
          kExtensionPrefix.size(), command_id.size() - kExtensionPrefix.size() -
                                       kExtensionSuffix.size());
      Browser* browser = BrowserForWorkspace(ws_);
      if (browser && ExecuteCmuxExtensionAction(browser, extension_id)) {
        return true;
      }
      LOG(WARNING) << "cmux-keymap: extension action unavailable "
                   << extension_id;
      return false;
    }
    const CommandSpec* spec = FindCommandSpec(command_id);
    if (!spec) {
      LOG(WARNING) << "cmux-keymap: unknown command "
                   << std::string(command_id);
      return false;
    }
    if (is_repeat && !spec->allow_repeat) {
      return true;
    }
    switch (spec->action) {
      case CommandAction::kTabNewWeb:
        NewTab(SurfaceKind::kWeb);
        return true;
      case CommandAction::kTabNewTerminal:
        if (!args_json.empty()) {
          std::optional<base::Value> args = base::JSONReader::Read(
              args_json, base::JSON_PARSE_CHROMIUM_EXTENSIONS |
                             base::JSON_ALLOW_TRAILING_COMMAS);
          if (args && args->is_dict()) {
            const std::string* command = args->GetDict().FindString("command");
            const std::string* title = args->GetDict().FindString("title");
            NewTerminalTab(command ? *command : std::string(),
                           title ? *title : std::string());
            return true;
          }
        }
        NewTab(SurfaceKind::kTerminal);
        return true;
      case CommandAction::kTabRestore:
        if (Browser* browser = BrowserForWorkspace(ws_)) {
          chrome::RestoreTab(browser);
        }
        return true;
      case CommandAction::kTabClose:
        CloseFocused();
        return true;
      case CommandAction::kTabNext:
        SelectAdjacentTab(1);
        return true;
      case CommandAction::kTabPrev:
        SelectAdjacentTab(-1);
        return true;
      case CommandAction::kTabJump1:
        SelectTabByNumber(1);
        return true;
      case CommandAction::kTabJump2:
        SelectTabByNumber(2);
        return true;
      case CommandAction::kTabJump3:
        SelectTabByNumber(3);
        return true;
      case CommandAction::kTabJump4:
        SelectTabByNumber(4);
        return true;
      case CommandAction::kTabJump5:
        SelectTabByNumber(5);
        return true;
      case CommandAction::kTabJump6:
        SelectTabByNumber(6);
        return true;
      case CommandAction::kTabJump7:
        SelectTabByNumber(7);
        return true;
      case CommandAction::kTabJump8:
        SelectTabByNumber(8);
        return true;
      case CommandAction::kTabJump9:
        SelectTabByNumber(9);
        return true;
      case CommandAction::kEditUndo:
      case CommandAction::kEditRedo:
      case CommandAction::kEditCut:
      case CommandAction::kEditCopy:
      case CommandAction::kEditPaste:
      case CommandAction::kEditSelectAll:
#if BUILDFLAG(IS_MAC)
        return ExecuteNativeEditCommand(command_id);
#else
        return false;
#endif
      case CommandAction::kPaneSplitRight:
        SplitFocused(SplitOrientation::kHorizontal);
        return true;
      case CommandAction::kPaneSplitDown:
        SplitFocused(SplitOrientation::kVertical);
        return true;
      case CommandAction::kPaneFocusLeft:
        MoveFocus(-1);
        return true;
      case CommandAction::kPaneFocusRight:
        MoveFocus(1);
        return true;
      case CommandAction::kPaneFocusUp:
        MoveFocusVertical(-1);
        return true;
      case CommandAction::kPaneFocusDown:
        MoveFocusVertical(1);
        return true;
      case CommandAction::kColumnNew:
        AddChromeColumn(GURL());
        return true;
      case CommandAction::kColumnCycleWidth:
        CycleColumnWidth();
        return true;
      case CommandAction::kSidebarToggle:
        ToggleSidebar();
        return true;
      case CommandAction::kOmniboxFocus:
        FocusActiveOmnibar();
        return true;
      case CommandAction::kOmniboxEscape:
        HandleEscape();
        return true;
      case CommandAction::kPageReload:
        ReloadFocused();
        return true;
      case CommandAction::kPageBack:
        GoBackFocused();
        return true;
      case CommandAction::kPageForward:
        GoForwardFocused();
        return true;
      case CommandAction::kZoomIn:
        if (content::WebContents* contents = ActiveWebContentsForExtensions()) {
          zoom::PageZoom::Zoom(contents, content::PAGE_ZOOM_IN);
        }
        return true;
      case CommandAction::kZoomOut:
        if (content::WebContents* contents = ActiveWebContentsForExtensions()) {
          zoom::PageZoom::Zoom(contents, content::PAGE_ZOOM_OUT);
        }
        return true;
      case CommandAction::kZoomReset:
        if (content::WebContents* contents = ActiveWebContentsForExtensions()) {
          zoom::PageZoom::Zoom(contents, content::PAGE_ZOOM_RESET);
        }
        return true;
      case CommandAction::kDevtoolsToggle:
        OpenDevToolsForFocused();
        return true;
      case CommandAction::kDevtoolsUndock:
        UndockDevToolsForFocused();
        return true;
      case CommandAction::kWorkspaceNext:
        CycleWorkspace(1);
        return true;
      case CommandAction::kWorkspacePrev:
        CycleWorkspace(-1);
        return true;
      case CommandAction::kWorkspaceNew:
        NewWorkspace();
        return true;
      case CommandAction::kWorkspaceToggleLayout:
        ToggleWorkspaceLayout();
        return true;
      case CommandAction::kWorkspaceJump1:
        JumpToWorkspace(0);
        return true;
      case CommandAction::kWorkspaceJump2:
        JumpToWorkspace(1);
        return true;
      case CommandAction::kWorkspaceJump3:
        JumpToWorkspace(2);
        return true;
      case CommandAction::kWorkspaceJump4:
        JumpToWorkspace(3);
        return true;
      case CommandAction::kWorkspaceJump5:
        JumpToWorkspace(4);
        return true;
      case CommandAction::kWorkspaceJump6:
        JumpToWorkspace(5);
        return true;
      case CommandAction::kWorkspaceJump7:
        JumpToWorkspace(6);
        return true;
      case CommandAction::kWorkspaceJump8:
        JumpToWorkspace(7);
        return true;
      case CommandAction::kWorkspaceJump9:
        JumpToWorkspace(8);
        return true;
      case CommandAction::kKeymapReload:
        ReloadKeymap();
        return true;
      case CommandAction::kSettingsOpen: {
        for (PaneId pane_id : model_.PanesOf(ws_)) {
          const Pane* pane = model_.FindPane(ws_, pane_id);
          for (const SurfaceTab& tab : pane->tabs) {
            content::WebContents* contents = WebContentsForSurface(tab.id);
            if (contents && (contents->GetVisibleURL().host() == "cmux-configure" ||
                contents->GetVisibleURL() == GURL("cmux://settings/"))) {
              model_.SelectTab(ws_, pane_id, tab.id);
              RefreshPaneTabs(pane_id);
              focus_.FocusPane(pane_id, /*move_keyboard=*/true);
              return true;
            }
          }
        }
        NewTabInPane(FocusedPane(), SurfaceKind::kWeb, std::string(),
                     std::string(), GURL("cmux://settings/"));
        return true;
      }
      case CommandAction::kThemeReload:
        ReloadTheme();
        return true;
      case CommandAction::kWindowNew:
        ShowNewViewsWebWindow();
        return true;
      case CommandAction::kWindowClose:
        CloseCurrentNativeWindow();
        return true;
      case CommandAction::kWindowToggleFullscreen:
        ToggleFullscreen();
        return true;
    }
    return false;
  }

  bool HandleStripMouseWheel(const ui::MouseWheelEvent& event) {
    if (!model_.GetWorkspace(ws_)) {
      return false;
    }
    int delta = 0;
    if (event.x_offset() != 0) {
      delta = event.x_offset();
    } else if (event.flags() & ui::EF_SHIFT_DOWN) {
      delta = event.y_offset();
    } else {
      return false;
    }
    if (delta == 0) {
      return false;
    }
    const double before = scroll_by_ws_[ws_];
    scroll_by_ws_[ws_] = before - delta;
    ApplyVisualsAndScrollInternal(
        /*animated=*/true, ScrollIntoViewPolicy::kEnsureVisible, kInvalidId);
    return scroll_by_ws_[ws_] != before;
  }

  // Map a focused Views view (from the FocusManager listener) back to its pane
  // and report it as activated. Catches omnibox focus and any Views-routed web
  // focus; the browser surface's web-contents-focused callback covers a click
  // on the page body that bypasses the FocusManager. No-op for the rail.
  void ActivatePaneContaining(views::View* focused) {
    if (!focused) {
      return;
    }
    for (auto& kv : views_) {
      if (kv.second->Contains(focused)) {
        focus_.OnPaneActivated(kv.first);
        return;
      }
    }
  }

  // ---- FocusHost: the FocusController's single window into the renderer. ----
  WorkspaceId active_workspace() override { return ws_; }
  WindowModel& model() override { return model_; }
  CmuxPane* PaneView(PaneId id) override { return ViewForPane(id); }

  // ---- TabDragHost ----------------------------------------------------------
  views::Widget* HostWidget() override { return GetWidget(); }
  views::View* strip_container() override { return content_; }
  views::View* PaneViewFor(PaneId pane) override { return ViewForPane(pane); }
  CmuxTabStrip* TabStripFor(PaneId pane) override {
    CmuxPaneView* view = ViewForPane(pane);
    return view ? view->tab_strip() : nullptr;
  }
  StripLayout CurrentLayout() override {
    const Workspace* ws = model_.GetWorkspace(ws_);
    if (!ws) {
      return StripLayout();
    }
    const int strip_w = StripWidthForLayout();
    return ComputeStripLayout(*ws, strip_w, height(), scroll_by_ws_[ws_],
                              ws->focused, CurrentLayoutMetrics());
  }
  int TabStripHeight() override {
    if (CmuxPaneView* view = ViewForPane(FocusedPane())) {
      if (view->tab_strip() && view->tab_strip()->height() > 0) {
        return view->tab_strip()->height();
      }
    }
    return 32;
  }
  void CommitDrop(const DragCommit& commit) override {
    if (commit.pane_drag) {
      CommitPaneDrop(commit);
    } else {
      CommitTabDrop(commit);
    }
  }
  void ReflowAfterNoOpDrop() override {
    ApplyVisualsAndScroll(/*animated=*/true);
  }

  int SamePaneFinalIndex(PaneId pane_id,
                         SurfaceTabId tab,
                         int insertion_index) {
    Pane* pane = model_.FindPane(ws_, pane_id);
    if (!pane) {
      return -1;
    }
    const int from = pane->IndexOfTab(tab);
    if (from < 0) {
      return -1;
    }
    int final_index = insertion_index;
    if (final_index > from) {
      --final_index;
    }
    return std::clamp(final_index, 0, static_cast<int>(pane->tabs.size()) - 1);
  }

  void CommitTabDrop(const DragCommit& commit) {
    const PaneId source = commit.source_pane;
    const SurfaceTabId tab = commit.tab;
    PaneId focus_target = kInvalidId;

    if (commit.target.kind == DropKind::kTabStrip &&
        commit.target.pane == source) {
      const int final_index = SamePaneFinalIndex(source, tab, commit.index);
      if (final_index < 0) {
        return;
      }
      if (model_.ReorderTab(ws_, source, tab, final_index) == kInvalidId) {
        return;
      }
      RefreshPaneTabs(source);
      focus_target = source;
    } else if (commit.target.kind == DropKind::kTabStrip ||
               commit.target.kind == DropKind::kPaneCenter) {
      const PaneId dest = commit.target.pane;
      CmuxPaneView* dest_view = ViewForPane(dest);
      if (!dest_view || !model_.FindPane(ws_, source) ||
          !model_.FindPane(ws_, dest)) {
        return;
      }
      DetachedSurface detached = DetachSurface(source, tab);
      if (!detached.view || !detached.surface) {
        return;
      }
      const int index =
          commit.target.kind == DropKind::kTabStrip ? commit.index : -1;
      model_.MoveTab(ws_, source, tab, ws_, dest, index);
      AttachSurfaceToPane(dest, std::move(detached));
      RefreshPaneTabs(dest);
      if (model_.FindPane(ws_, source)) {
        RefreshPaneTabs(source);
      } else {
        DestroyPaneView(source, /*notify_focus=*/false);
      }
      focus_target = dest;
    } else if (commit.target.kind == DropKind::kPaneEdge) {
      DetachedSurface detached = DetachSurface(source, tab);
      if (!detached.view || !detached.surface) {
        return;
      }
      const PaneId fresh = model_.SplitPaneWithTab(
          ws_, commit.target.pane, commit.target.orientation,
          commit.target.insert_first, source, tab);
      if (fresh == kInvalidId) {
        AttachSurfaceToPane(source, std::move(detached));
        RefreshPaneTabs(source);
        return;
      }
      AttachSurfaceToPane(fresh, std::move(detached));
      MarkPaneForSplitEntrance(fresh, commit.target.orientation,
                               commit.target.insert_first);
      RefreshPaneTabs(fresh);
      RefreshPaneTabs(commit.target.pane);
      if (model_.FindPane(ws_, source)) {
        RefreshPaneTabs(source);
      } else {
        DestroyPaneView(source, /*notify_focus=*/false);
      }
      focus_.OnPaneAdded(fresh);
      focus_target = fresh;
    } else if (commit.target.kind == DropKind::kNewColumn) {
      DetachedSurface detached = DetachSurface(source, tab);
      if (!detached.view || !detached.surface) {
        return;
      }
      const PaneId fresh = model_.MoveTabToNewColumn(
          ws_, source, tab, commit.target.column_index);
      if (fresh == kInvalidId) {
        AttachSurfaceToPane(source, std::move(detached));
        RefreshPaneTabs(source);
        return;
      }
      AttachSurfaceToPane(fresh, std::move(detached));
      MarkPaneForEntrance(fresh);
      RefreshPaneTabs(fresh);
      if (model_.FindPane(ws_, source)) {
        RefreshPaneTabs(source);
      } else {
        DestroyPaneView(source, /*notify_focus=*/false);
      }
      focus_.OnPaneAdded(fresh);
      focus_target = fresh;
    }

    if (focus_target != kInvalidId && model_.FindPane(ws_, focus_target)) {
      SyncBrowserTabOrder(ws_);
      focus_.FocusPane(focus_target, /*move_keyboard=*/true);
    } else {
      ApplyVisualsAndScroll(/*animated=*/true);
    }
  }

  void CommitPaneDrop(const DragCommit& commit) {
    const PaneId source = commit.source_pane;
    PaneId focus_target = source;
    if (!model_.FindPane(ws_, source)) {
      return;
    }
    if (commit.target.kind == DropKind::kTabStrip ||
        commit.target.kind == DropKind::kPaneCenter) {
      const PaneId dest = commit.target.pane;
      if (source == dest || !model_.FindPane(ws_, dest)) {
        return;
      }
      std::vector<DetachedSurface> surfaces = DetachAllSurfaces(source);
      const int index =
          commit.target.kind == DropKind::kTabStrip ? commit.index : -1;
      if (!model_.MergePaneInto(ws_, source, dest, index)) {
        for (DetachedSurface& surface : surfaces) {
          AttachSurfaceToPane(source, std::move(surface));
        }
        RefreshPaneTabs(source);
        return;
      }
      for (DetachedSurface& surface : surfaces) {
        AttachSurfaceToPane(dest, std::move(surface));
      }
      DestroyPaneView(source, /*notify_focus=*/false);
      RefreshPaneTabs(dest);
      focus_target = dest;
    } else if (commit.target.kind == DropKind::kPaneEdge) {
      if (model_.MovePaneToSplit(ws_, source, commit.target.pane,
                                 commit.target.orientation,
                                 commit.target.insert_first) == kInvalidId) {
        return;
      }
      RefreshPaneTabs(source);
      RefreshPaneTabs(commit.target.pane);
    } else if (commit.target.kind == DropKind::kNewColumn) {
      if (model_.MovePaneToNewColumn(ws_, source, commit.target.column_index) ==
          kInvalidId) {
        return;
      }
      RefreshPaneTabs(source);
    } else {
      return;
    }

    ApplyVisualsAndScroll(/*animated=*/true);
    SyncBrowserTabOrder(ws_);
    if (focus_target != kInvalidId && model_.FindPane(ws_, focus_target)) {
      focus_.FocusPane(focus_target, /*move_keyboard=*/true);
    }
  }

  PaneId PaneContainingTab(SurfaceTabId tab) {
    for (PaneId pane_id : model_.PanesOf(ws_)) {
      Pane* pane = model_.FindPane(ws_, pane_id);
      if (pane && pane->FindTab(tab)) {
        return pane_id;
      }
    }
    return kInvalidId;
  }

  PaneId FirstOtherPane(PaneId pane) {
    for (PaneId candidate : model_.PanesOf(ws_)) {
      if (candidate != pane && ViewForPane(candidate)) {
        return candidate;
      }
    }
    return kInvalidId;
  }

  PaneId FirstPaneInColumn(int column) const {
    for (PaneId candidate : model_.PanesOf(ws_)) {
      if (model_.ColumnIndexOf(ws_, candidate) == column) {
        return candidate;
      }
    }
    return kInvalidId;
  }

  int SurfaceChildCount(PaneId pane) {
    CmuxPaneView* view = ViewForPane(pane);
    return view ? static_cast<int>(view->surface_container()->children().size())
                : -1;
  }

  bool ModelInvariantsHold() {
    std::set<SurfaceTabId> seen_tabs;
    for (const Workspace& ws : model_.workspaces()) {
      if (ws.columns.empty() || ws.columns.size() != ws.column_widths.size()) {
        return false;
      }
      bool focused_found = false;
      for (const LayoutNode& column : ws.columns) {
        if (!NodeInvariantsHold(column, &seen_tabs)) {
          return false;
        }
      }
      for (PaneId pane : model_.PanesOf(ws.id)) {
        focused_found = focused_found || pane == ws.focused;
      }
      if (!focused_found) {
        return false;
      }
    }
    return true;
  }

  void DndSelfTestPass(const char* step) {
    CHECK(ModelInvariantsHold())
        << "CMUX_DND_SELFTEST invariant failure after " << step;
    LOG(WARNING) << "cmux-views: CMUX_DND_SELFTEST PASS " << step;
  }

 public:
  void InitializeStartupTab(const std::optional<GURL>& startup_url) {
    pending_startup_tab_ = startup_url.has_value();
    pending_startup_url_ = startup_url;
    RunPendingStartupContentIfReady();
  }

 private:
  void RunPendingStartupContentIfReady() {
    if (!model_.GetWorkspace(ws_)) {
      return;
    }
    const size_t terminal_columns = std::exchange(pending_terminal_columns_, 0);
    for (size_t index = 0; index < terminal_columns; ++index) {
      AddTerminalColumn(/*user_action=*/false);
    }
    if (!std::exchange(pending_startup_tab_, false)) {
      return;
    }
    const std::optional<GURL> startup_url = std::move(pending_startup_url_);
    pending_startup_url_.reset();
    Pane* pane = nullptr;
    SurfaceTab* tab = nullptr;
    for (PaneId pane_id : model_.PanesOf(ws_)) {
      Pane* candidate = model_.FindPane(ws_, pane_id);
      SurfaceTab* selected =
          candidate ? candidate->FindTab(candidate->selected) : nullptr;
      if (selected && selected->kind == SurfaceKind::kWeb) {
        pane = candidate;
        tab = selected;
        break;
      }
    }
    if (!pane || !tab || tab->kind != SurfaceKind::kWeb) {
      if (startup_url && startup_url->is_valid()) {
        NewTab(SurfaceKind::kWeb);
        pane = model_.FindPane(ws_, FocusedPane());
        tab = pane ? pane->FindTab(pane->selected) : nullptr;
      }
      if (!pane || !tab || tab->kind != SurfaceKind::kWeb) return;
    }
    if (startup_url && startup_url->is_valid() && !startup_url->is_empty()) {
      if (content::WebContents* contents = WebContentsForSurface(tab->id)) {
        contents->GetController().LoadURL(*startup_url, content::Referrer(),
                                          ui::PAGE_TRANSITION_TYPED,
                                          std::string());
      }
    }
    ActivateBrowserTabForSurface(tab->id);
    FocusNewWebTab(pane->id, tab->id);
  }

 public:

  std::optional<CmuxAppliedTheme> AppliedThemeForSettings() const {
    if (!active_theme_palette_) {
      return std::nullopt;
    }
    return CmuxAppliedTheme{active_theme_name_, *active_theme_palette_};
  }

  void ApplyCurrentChromeSurfaceThemeOverrides(views::Widget* widget) {
    ApplyChromeSurfaceThemeOverrides(widget);
  }

  void ApplyCurrentChromeSurfaceThemeOverridesToWindow() {
    ApplyChromeSurfaceThemeOverridesToWindow();
  }

  void FlushPendingChromeSurfaceThemeSelfTest() {
    if (pending_chrome_surface_selftest_ &&
        LogChromeSurfaceThemeSelfTestIfReady()) {
      pending_chrome_surface_selftest_ = false;
    }
  }

  void RunSidebarSelfTest() {
    LOG(WARNING) << "cmux-views: CMUX_SIDEBAR_SELFTEST starting";
    CHECK(AnimationsEnabled()) << "CMUX_SIDEBAR_SELFTEST requires animations";
    CHECK(root_animator_);
    CHECK(content_);
    CHECK(rail_);
    CHECK(!sidebar_selftest_active_);

    sidebar_selftest_active_ = true;
    sidebar_selftest_transitions_ = 0;
    sidebar_selftest_initial_mode_ = layout_config_.sidebar_mode;
    BeginSidebarSelfTestTransition();
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
        FROM_HERE,
        base::BindOnce(&CmuxWindowView::SidebarSelfTestWatchdog,
                       weak_factory_.GetWeakPtr()),
        base::Seconds(3));
  }

  void BeginSidebarSelfTestTransition() {
    CHECK(sidebar_selftest_active_);
    const SidebarMode target =
        sidebar_metrics::NextMode(layout_config_.sidebar_mode);
    sidebar_selftest_start_bounds_ = content_->bounds();
    sidebar_selftest_target_bounds_ = ContentBoundsForSidebarMode(target);
    CHECK_NE(sidebar_selftest_start_bounds_, sidebar_selftest_target_bounds_)
        << "CMUX_SIDEBAR_SELFTEST needs a window wider than the sidebar";
    sidebar_selftest_mid_checked_ = false;
    if (target == SidebarMode::kHidden) {
      // Reproduce hiding the rail while the pointer is over New workspace.
      // The button must not carry this state into the next visible mode.
      rail_->SetNewWorkspaceButtonHoveredForTesting();
      CHECK(rail_->NewWorkspaceButtonIsHoveredForTesting());
    }

    // Exercise the same three-state cycle used by sidebar.toggle / Cmd+B.
    CycleSidebarMode(/*persist=*/false);
    CHECK_EQ(layout_config_.sidebar_mode, target);
    CHECK(root_animator_->IsAnimating(content_));
    CheckSidebarSelfTestFrame(/*midflight=*/false);
  }

  void CheckSidebarSelfTestFrame(bool midflight) {
    if (!sidebar_selftest_active_) {
      return;
    }
    const gfx::Rect current = content_->bounds();
    if (midflight && current != sidebar_selftest_start_bounds_ &&
        current != sidebar_selftest_target_bounds_) {
      sidebar_selftest_mid_checked_ = true;
    }

    const PaneId first = FirstPaneInColumn(0);
    CmuxPaneView* pane = ViewForPane(first);
    const auto logical = pane_titlebar_geometry_.find(first);
    CHECK(pane);
    CHECK(pane->tab_strip());
    CHECK(logical != pane_titlebar_geometry_.end());
    const int pane_window_x = content_->x() + logical->second.x();
    const int expected_inset =
        std::max(0, WindowControlsClearance() - pane_window_x);
    CHECK_EQ(pane->tab_strip()->leading_inset_for_testing(), expected_inset)
        << "CMUX_SIDEBAR_SELFTEST first tab did not track sidebar motion";
  }

  void FinishSidebarSelfTestTransition() {
    if (!sidebar_selftest_active_) {
      return;
    }
    CHECK(sidebar_selftest_mid_checked_)
        << "CMUX_SIDEBAR_SELFTEST transition had no mid-flight frame";
    CHECK_EQ(content_->bounds(), sidebar_selftest_target_bounds_);
    CHECK_EQ(rail_->display_mode(), layout_config_.sidebar_mode);
    CHECK_EQ(rail_->width(), RailWidthForMode(layout_config_.sidebar_mode));
    if (layout_config_.sidebar_mode == SidebarMode::kHidden) {
      CHECK(!rail_resize_handle_->GetVisible());
      CHECK(!rail_->NewWorkspaceButtonIsHoveredForTesting())
          << "CMUX_SIDEBAR_SELFTEST hidden plus retained hover state";
    } else {
      CHECK(rail_resize_handle_->GetVisible());
      const gfx::Rect handle_probe(
          rail_resize_handle_->bounds().CenterPoint(), gfx::Size(1, 1));
      CHECK_EQ(TargetForRect(this, handle_probe), rail_resize_handle_)
          << "CMUX_SIDEBAR_SELFTEST resize edge did not target its handle";
    }
    if (layout_config_.sidebar_mode == SidebarMode::kIcons) {
      CHECK_EQ(rail_->width(), sidebar_metrics::kIconsWidth);
      CHECK_EQ(rail_resize_handle_->width(),
               sidebar_metrics::kIconsResizeAreaWidth);
      const gfx::Rect row_bounds = rail_->FirstRowBoundsForTesting();
      const gfx::Rect button_bounds =
          rail_->NewWorkspaceButtonBoundsForTesting();
      CHECK_EQ(row_bounds.size(),
               gfx::Size(sidebar_metrics::kRowHeight,
                         sidebar_metrics::kRowHeight))
          << "CMUX_SIDEBAR_SELFTEST workspace icon target is not square";
      CHECK_EQ(row_bounds.x(), sidebar_metrics::kOuterHorizontalInset);
      CHECK_EQ(button_bounds.size(),
               gfx::Size(sidebar_metrics::kFooterHeight,
                         sidebar_metrics::kFooterHeight))
          << "CMUX_SIDEBAR_SELFTEST new-workspace target is not square";
      CHECK_EQ(button_bounds.x(),
               sidebar_metrics::kFooterSideAndBottomInset);
    } else if (layout_config_.sidebar_mode == SidebarMode::kExpanded) {
      CHECK_EQ(rail_resize_handle_->width(),
               sidebar_metrics::kExpandedResizeAreaWidth);
    }
    CheckSidebarSelfTestFrame(/*midflight=*/false);
    ++sidebar_selftest_transitions_;
    LOG(WARNING) << "cmux-views: CMUX_SIDEBAR_SELFTEST PASS transition="
                 << sidebar_selftest_transitions_
                 << " width=" << rail_->width();

    if (sidebar_selftest_transitions_ == 3) {
      CHECK_EQ(layout_config_.sidebar_mode, sidebar_selftest_initial_mode_);
      sidebar_selftest_active_ = false;
      LOG(WARNING) << "cmux-views: CMUX_SIDEBAR_SELFTEST SURVIVED";
      return;
    }
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE,
        base::BindOnce(&CmuxWindowView::BeginSidebarSelfTestTransition,
                       weak_factory_.GetWeakPtr()));
  }

  void SidebarSelfTestWatchdog() {
    CHECK(!sidebar_selftest_active_)
        << "CMUX_SIDEBAR_SELFTEST did not finish within 3s";
  }

  void RunAnimSelfTest() {
    LOG(WARNING) << "cmux-views: CMUX_ANIM_SELFTEST starting";
    CHECK(AnimationsEnabled()) << "CMUX_ANIM_SELFTEST requires animations";
    CHECK(animator_);
    CHECK(content_);

    constexpr size_t kMinColumns = 5;
    while (model_.ColumnCountOf(ws_) < kMinColumns) {
      PaneId pane = model_.AddColumn(ws_, SurfaceKind::kWeb);
      CHECK_NE(pane, kInvalidId);
      RealizePane(pane, CmuxDemoURL());
    }

    const PaneId first = FirstPaneInColumn(0);
    const PaneId target =
        FirstPaneInColumn(static_cast<int>(model_.ColumnCountOf(ws_) - 1));
    CHECK_NE(first, kInvalidId);
    CHECK_NE(target, kInvalidId);
    CHECK_NE(first, target);

    model_.FocusPane(ws_, first);
    ApplyVisualsAndScroll(/*animated=*/false);
    CmuxPaneView* target_view = ViewForPane(target);
    CHECK(target_view);
    const int start_x = target_view->bounds().x();

    focus_.FocusPane(target, /*move_keyboard=*/true);
    const int final_x = animator_->GetTargetBounds(target_view).x();
    CHECK_NE(start_x, final_x) << "CMUX_ANIM_SELFTEST did not force a scroll";
    CHECK(animator_->IsAnimating(target_view))
        << "CMUX_ANIM_SELFTEST pane animation did not start";

    // Event-driven verification: wall-clock sampling flakes when the main
    // thread is saturated (the animator ticks on this same thread, so a
    // delayed sampler can run before the first animation tick — seen live
    // right after the DND selftest's five WebContents creations). Observe the
    // animator instead: the first Progressed tick must land strictly between
    // the endpoints, and Done must land on the target.
    anim_selftest_active_ = true;
    anim_selftest_mid_checked_ = false;
    anim_selftest_pane_ = target;
    anim_selftest_start_x_ = start_x;
    anim_selftest_final_x_ = final_x;
    animator_->AddObserver(this);
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
        FROM_HERE,
        base::BindOnce(&CmuxWindowView::AnimSelfTestWatchdog,
                       weak_factory_.GetWeakPtr()),
        base::Seconds(3));
  }

  void AnimSelfTestWatchdog() {
    CHECK(!anim_selftest_active_)
        << "CMUX_ANIM_SELFTEST no animation progress within 3s";
  }

  void RunDndSelfTest() {
    LOG(WARNING) << "cmux-views: CMUX_DND_SELFTEST starting";
    FocusColumn(0);
    PaneId p0 = FocusedPane();
    CHECK_NE(p0, kInvalidId);
    NewTabInPane(p0, SurfaceKind::kWeb);
    Pane* pane0 = model_.FindPane(ws_, p0);
    CHECK(pane0);
    SurfaceTabId reordered = pane0->selected;
    DragCommit reorder;
    reorder.source_pane = p0;
    reorder.tab = reordered;
    reorder.target.kind = DropKind::kTabStrip;
    reorder.target.pane = p0;
    reorder.index = 0;
    CommitDrop(reorder);
    CHECK_EQ(model_.FindPane(ws_, p0)->tabs.front().id, reordered);
    DndSelfTestPass("reorder tab within pane");

    SplitFocused(SplitOrientation::kHorizontal);
    PaneId p1 = FocusedPane();
    CHECK_NE(p1, kInvalidId);
    SurfaceTabId moved_to_center = model_.FindPane(ws_, p0)->tabs.back().id;
    const int p0_children_before = SurfaceChildCount(p0);
    const int p1_children_before = SurfaceChildCount(p1);
    DragCommit center;
    center.source_pane = p0;
    center.tab = moved_to_center;
    center.target.kind = DropKind::kPaneCenter;
    center.target.pane = p1;
    CommitDrop(center);
    CHECK_EQ(PaneContainingTab(moved_to_center), p1);
    CHECK_EQ(SurfaceChildCount(p0), p0_children_before - 1);
    CHECK_EQ(SurfaceChildCount(p1), p1_children_before + 1);
    DndSelfTestPass("move tab to pane center");

    SurfaceTabId edge_tab = model_.FindPane(ws_, p1)->tabs.back().id;
    const size_t panes_before_edge = model_.PanesOf(ws_).size();
    DragCommit edge;
    edge.source_pane = p1;
    edge.tab = edge_tab;
    edge.target.kind = DropKind::kPaneEdge;
    edge.target.pane = p0;
    edge.target.orientation = SplitOrientation::kHorizontal;
    edge.target.insert_first = false;
    CommitDrop(edge);
    PaneId edge_pane = PaneContainingTab(edge_tab);
    CHECK_NE(edge_pane, kInvalidId);
    CHECK_EQ(model_.PanesOf(ws_).size(), panes_before_edge + 1);
    DndSelfTestPass("edge-drop split");

    NewTabInPane(p0, SurfaceKind::kWeb);
    SurfaceTabId column_tab = model_.FindPane(ws_, p0)->selected;
    const size_t columns_before = model_.ColumnCountOf(ws_);
    DragCommit column;
    column.source_pane = p0;
    column.tab = column_tab;
    column.target.kind = DropKind::kNewColumn;
    column.target.column_index = -1;
    CommitDrop(column);
    PaneId column_pane = PaneContainingTab(column_tab);
    CHECK_NE(column_pane, kInvalidId);
    CHECK_EQ(model_.ColumnCountOf(ws_), columns_before + 1);
    DndSelfTestPass("gap-drop new column");

    DragCommit merge;
    merge.pane_drag = true;
    merge.source_pane = edge_pane;
    merge.target.kind = DropKind::kPaneCenter;
    merge.target.pane = p0;
    CommitDrop(merge);
    CHECK(!model_.FindPane(ws_, edge_pane));
    DndSelfTestPass("pane merge");

    Pane* deactivation_pane = model_.FindPane(ws_, p0);
    CmuxPaneView* deactivation_view = ViewForPane(p0);
    CHECK(deactivation_pane);
    CHECK(deactivation_view);
    const SurfaceTab* deactivation_tab = deactivation_pane->SelectedTab();
    CHECK(deactivation_tab);
    drag_controller_->StartTabDrag(
        p0, deactivation_tab->id, deactivation_tab->title,
        deactivation_tab->kind,
        deactivation_view->GetBoundsInScreen().CenterPoint(),
        gfx::Point(12, 12));
    CHECK(drag_controller_->active());
    CHECK(drag_controller_->has_overlay_for_testing());
    CancelActiveDragForHostState();
    CHECK(!drag_controller_->active());
    CHECK(!drag_controller_->has_overlay_for_testing());
    DndSelfTestPass("host deactivation destroys drag overlay");

    NewTabInPane(p0, SurfaceKind::kTerminal);
    SurfaceTabId terminal_tab = model_.FindPane(ws_, p0)->selected;
    PaneId terminal_dest = FirstOtherPane(p0);
    if (terminal_dest == kInvalidId) {
      terminal_dest = model_.AddColumn(ws_, SurfaceKind::kWeb);
      RealizePane(terminal_dest, CmuxDemoURL());
    }
    CHECK_NE(terminal_dest, kInvalidId);
    DragCommit terminal_move;
    terminal_move.source_pane = p0;
    terminal_move.tab = terminal_tab;
    terminal_move.target.kind = DropKind::kPaneCenter;
    terminal_move.target.pane = terminal_dest;
    CommitDrop(terminal_move);
    CHECK_EQ(PaneContainingTab(terminal_tab), terminal_dest);
    CmuxPaneView* dest_view = ViewForPane(terminal_dest);
    CHECK(dest_view && dest_view->SurfaceFor(terminal_tab));
    DndSelfTestPass("terminal tab reparent");

    LOG(WARNING) << "cmux-views: CMUX_DND_SELFTEST SURVIVED";
  }

  void RunTermExitSelfTest() {
    LOG(WARNING) << "cmux-views: CMUX_TERMEXIT_SELFTEST starting";
    PaneId terminal_pane = model_.AddColumn(ws_, SurfaceKind::kTerminal);
    CHECK_NE(terminal_pane, kInvalidId);
    RealizePane(terminal_pane, GURL());
    Pane* pane = model_.FindPane(ws_, terminal_pane);
    CHECK(pane);
    SurfaceTabId terminal_tab = pane->selected;
    CmuxPaneView* view = ViewForPane(terminal_pane);
    CmuxSurface* surface = view ? view->SurfaceFor(terminal_tab) : nullptr;
    CHECK(surface);
    const size_t panes_before = model_.PanesOf(ws_).size();

    // Fire the surface's close-requested callback after CmuxPaneView has bound
    // it, matching Ghostty's post-child-exit close request without killing a
    // real shell.
    surface->FireCloseRequestedForTesting();
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
        FROM_HERE,
        base::BindOnce(&CmuxWindowView::VerifyTermExitSelfTest,
                       weak_factory_.GetWeakPtr(), terminal_pane, terminal_tab,
                       panes_before),
        base::Milliseconds(250));
  }

  void VerifyTermExitSelfTest(PaneId terminal_pane,
                              SurfaceTabId terminal_tab,
                              size_t panes_before) {
    CHECK(!model_.FindPane(ws_, terminal_pane));
    CHECK(!ViewForPane(terminal_pane));
    CHECK_EQ(PaneContainingTab(terminal_tab), kInvalidId);
    CHECK_EQ(model_.PanesOf(ws_).size(), panes_before - 1);
    CHECK(ModelInvariantsHold()) << "CMUX_TERMEXIT_SELFTEST invariant failure";
    LOG(WARNING) << "cmux-views: PASS terminal exit closes tab";
  }

  void RunPageInfoSelfTest() {
    LOG(WARNING) << "cmux-views: CMUX_PAGEINFO_SELFTEST starting";
    FocusColumn(0);
    const PaneId pane_id = FocusedPane();
    Pane* pane = model_.FindPane(ws_, pane_id);
    CmuxPaneView* view = ViewForPane(pane_id);
    CmuxSurface* surface =
        pane && view ? view->SurfaceFor(pane->selected) : nullptr;
    CHECK(surface);
    CHECK_EQ(surface->kind(), SurfaceKind::kWeb);
    CHECK(surface->ShowPageInfoForTesting())
        << "CMUX_PAGEINFO_SELFTEST page-info bubble was not shown";
    LOG(WARNING) << "cmux-views: CMUX_PAGEINFO_SELFTEST PASS bubble shown";

    // Adversarial lifetime check: destroy the surface while the bubble is
    // open. PageInfo observes the WebContents and LocationBarView binds its
    // close callback through a WeakPtr, so tab teardown must close/no-op
    // without retaining the pane-local anchor.
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
        FROM_HERE,
        base::BindOnce(
            [](base::WeakPtr<CmuxWindowView> window) {
              if (window) {
                window->CloseFocused();
              }
            },
            weak_factory_.GetWeakPtr()),
        base::Seconds(1));
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
        FROM_HERE,
        base::BindOnce(
            [](base::WeakPtr<CmuxWindowView> window) {
              CHECK(window);
              LOG(WARNING) << "cmux-views: CMUX_PAGEINFO_SELFTEST PASS "
                              "teardown survived";
            },
            weak_factory_.GetWeakPtr()),
        base::Seconds(2));
  }

  void RunPermissionHostSelfTest(int attempt = 0) {
    if (attempt == 0) {
      LOG(WARNING) << "cmux-views: CMUX_PERMISSION_SELFTEST starting";
    }

    PaneId pane_id = kInvalidId;
    SurfaceTabId tab_id = kInvalidId;
    CmuxPaneView* pane_view = nullptr;
    CmuxSurface* surface = nullptr;
    for (PaneId candidate_pane : model_.PanesOf(ws_)) {
      Pane* pane = model_.FindPane(ws_, candidate_pane);
      CmuxPaneView* view = ViewForPane(candidate_pane);
      if (!pane || !view) {
        continue;
      }
      for (const SurfaceTab& tab : pane->tabs) {
        if (tab.kind != SurfaceKind::kWeb) {
          continue;
        }
        surface = view->SurfaceFor(tab.id);
        if (surface) {
          pane_id = candidate_pane;
          tab_id = tab.id;
          pane_view = view;
          break;
        }
      }
      if (surface) {
        break;
      }
    }

    // A fresh profile can start with only a terminal pane. Create a real web
    // surface for the opt-in runtime test, then wait for its pane-local
    // LocationBarView and modal host to finish attaching.
    if (!surface) {
      if (attempt == 0) {
        const PaneId focused = FocusedPane();
        CHECK_NE(focused, kInvalidId);
        NewTabInPane(focused, SurfaceKind::kWeb);
      }
      CHECK_LT(attempt, 20)
          << "CMUX_PERMISSION_SELFTEST web surface did not attach";
      base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
          FROM_HERE,
          base::BindOnce(&CmuxWindowView::RunPermissionHostSelfTest,
                         weak_factory_.GetWeakPtr(), attempt + 1),
          base::Milliseconds(100));
      return;
    }

    Browser* browser = BrowserForWorkspace(ws_);
    CHECK(browser);
    bool has_peer_web_pane = false;
    for (PaneId candidate_pane : model_.PanesOf(ws_)) {
      if (candidate_pane == pane_id) {
        continue;
      }
      const Pane* candidate_model = model_.FindPane(ws_, candidate_pane);
      CmuxPaneView* candidate_view = ViewForPane(candidate_pane);
      const SurfaceTab* selected =
          candidate_model ? candidate_model->SelectedTab() : nullptr;
      CmuxSurface* selected_surface =
          candidate_view && selected
              ? candidate_view->SurfaceFor(selected->id)
              : nullptr;
      views::View* selected_surface_view =
          selected_surface ? selected_surface->AsView() : nullptr;
      if (candidate_view && selected &&
          selected->kind == SurfaceKind::kWeb &&
          selected_surface_view &&
          !selected_surface_view->GetVisibleBounds().IsEmpty() &&
          IsWorkspaceVisible(ws_)) {
        has_peer_web_pane = true;
        break;
      }
    }
    if (!has_peer_web_pane) {
      CHECK_LT(attempt, 20)
          << "CMUX_PERMISSION_SELFTEST peer web pane did not attach";
      const PaneId peer = model_.SplitPane(
          ws_, pane_id, SplitOrientation::kVertical, 0.5, SurfaceKind::kWeb);
      CHECK_NE(peer, kInvalidId);
      RealizePane(peer, GURL());
      focus_.OnPaneAdded(peer);
      base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
          FROM_HERE,
          base::BindOnce(&CmuxWindowView::RunPermissionHostSelfTest,
                         weak_factory_.GetWeakPtr(), attempt + 1),
          base::Milliseconds(100));
      return;
    }

    model_.SelectTab(ws_, pane_id, tab_id);
    ActivateBrowserTabForSurface(tab_id);
    RefreshPaneTabs(pane_id);
    focus_.FocusPane(pane_id, /*move_keyboard=*/false);

    content::WebContents* web_contents =
        surface->GetInspectableWebContents();
    CHECK(web_contents);
    CHECK(pane_view);
    CHECK(browser->window());

    LocationBar* location_bar = browser->window()->GetLocationBar();
    CHECK(location_bar);
    CHECK_EQ(location_bar, surface->GetLocationBar());
    bool checked_visible_non_active_location_bar = false;
    for (PaneId candidate_pane : model_.PanesOf(ws_)) {
      const Pane* candidate_model = model_.FindPane(ws_, candidate_pane);
      CmuxPaneView* candidate_view = ViewForPane(candidate_pane);
      if (!candidate_model || !candidate_view) {
        continue;
      }
      for (const SurfaceTab& candidate_tab : candidate_model->tabs) {
        if (candidate_tab.kind != SurfaceKind::kWeb) {
          continue;
        }
        CmuxSurface* candidate_surface =
            candidate_view->SurfaceFor(candidate_tab.id);
        content::WebContents* candidate_contents =
            candidate_surface
                ? candidate_surface->GetInspectableWebContents()
                : nullptr;
        if (!candidate_surface || !candidate_contents) {
          continue;
        }
        CHECK_EQ(GetLocationBarForWebContents(browser, candidate_contents),
                 candidate_surface->GetLocationBar());
        views::View* candidate_surface_view = candidate_surface->AsView();
        const bool expected_visible =
            candidate_model->selected == candidate_tab.id &&
            IsWorkspaceVisible(ws_) && candidate_surface_view &&
            !candidate_surface_view->GetVisibleBounds().IsEmpty();
        CHECK_EQ(
            IsWebContentsActiveOrVisible(browser, candidate_contents),
            expected_visible);
        checked_visible_non_active_location_bar |=
            candidate_contents != web_contents && expected_visible;
      }
    }
    CHECK(checked_visible_non_active_location_bar)
        << "CMUX_PERMISSION_SELFTEST did not test visible non-active pane "
           "routing";
    location_bar->UpdateContentSettingsIcons();

    web_modal::WebContentsModalDialogHost* modal_host =
        browser->window()->GetWebContentsModalDialogHostFor(web_contents);
    CHECK(modal_host);
    CHECK(modal_host->GetHostView());
    const gfx::Size maximum_size = modal_host->GetMaximumDialogSize();
    CHECK_GT(maximum_size.width(), 0);
    CHECK_GT(maximum_size.height(), 0);
    const gfx::Point dialog_position =
        modal_host->GetDialogPosition(gfx::Size(360, 240));
    CHECK_GE(dialog_position.x(), 0);
    CHECK_GE(dialog_position.y(), 0);

    class ModalPositionObserver final
        : public web_modal::ModalDialogHostObserver {
     public:
      void OnPositionRequiresUpdate() override {
        ++position_update_count_;
      }
      void OnHostDestroying() override {}
      int position_update_count() const { return position_update_count_; }

     private:
      int position_update_count_ = 0;
    };
    ModalPositionObserver modal_observer;
    modal_host->AddObserver(&modal_observer);
    const gfx::Rect original_pane_bounds = pane_view->bounds();
    const gfx::Rect visible_content_bounds = content_->GetVisibleBounds();
    CHECK_GT(maximum_size.width(), 2);
    CHECK(!visible_content_bounds.IsEmpty());
    int translation_x = 0;
    int translation_y = 0;
    if (original_pane_bounds.right() < visible_content_bounds.right()) {
      translation_x = 1;
    } else if (original_pane_bounds.x() > visible_content_bounds.x()) {
      translation_x = -1;
    } else if (original_pane_bounds.bottom() <
               visible_content_bounds.bottom()) {
      translation_y = 1;
    } else if (original_pane_bounds.y() > visible_content_bounds.y()) {
      translation_y = -1;
    }
    CHECK(translation_x != 0 || translation_y != 0)
        << "CMUX_PERMISSION_SELFTEST needs room for visible translation";
    const int updates_before_translation =
        modal_observer.position_update_count();
    gfx::Rect translated_pane_bounds = original_pane_bounds;
    translated_pane_bounds.Offset(translation_x, translation_y);
    pane_view->SetBoundsRect(translated_pane_bounds);
    const gfx::Size translated_maximum_size =
        modal_host->GetMaximumDialogSize();
    CHECK_GT(translated_maximum_size.width(), 0);
    CHECK_GT(translated_maximum_size.height(), 0);
    CHECK_GE(translated_maximum_size.width(), maximum_size.width() - 1);
    CHECK_LE(translated_maximum_size.width(), maximum_size.width() + 1);
    CHECK_GE(translated_maximum_size.height(), maximum_size.height() - 1);
    CHECK_LE(translated_maximum_size.height(), maximum_size.height() + 1);
    CHECK_GT(modal_observer.position_update_count(),
             updates_before_translation)
        << "CMUX_PERMISSION_SELFTEST modal host ignored visible movement";
    pane_view->SetBoundsRect(original_pane_bounds);
    gfx::Rect hidden_pane_bounds = original_pane_bounds;
    hidden_pane_bounds.set_x(visible_content_bounds.x() -
                             original_pane_bounds.width() - 1);
    pane_view->SetBoundsRect(hidden_pane_bounds);
    CHECK(!IsWebContentsActiveOrVisible(browser, web_contents))
        << "CMUX_PERMISSION_SELFTEST fully clipped pane remained visible";
    CHECK(!modal_host->ShouldActivateDialog())
        << "CMUX_PERMISSION_SELFTEST fully clipped modal host remained active";
    pane_view->SetBoundsRect(original_pane_bounds);
    CHECK(IsWebContentsActiveOrVisible(browser, web_contents))
        << "CMUX_PERMISSION_SELFTEST restored pane remained hidden";
    const int clipped_width = std::max(1, maximum_size.width() / 2);
    gfx::Rect clipped_pane_bounds = original_pane_bounds;
    clipped_pane_bounds.set_x(visible_content_bounds.x() -
                              original_pane_bounds.width() + clipped_width);
    pane_view->SetBoundsRect(clipped_pane_bounds);
    const gfx::Size clipped_maximum_size =
        modal_host->GetMaximumDialogSize();
    const gfx::Point clipped_dialog_position =
        modal_host->GetDialogPosition(gfx::Size(360, 240));
    CHECK_GT(clipped_maximum_size.width(), 0);
    CHECK_LT(clipped_maximum_size.width(), maximum_size.width())
        << "CMUX_PERMISSION_SELFTEST modal host ignored ancestor clipping";
    CHECK_GE(clipped_dialog_position.x(), 0);
    pane_view->SetBoundsRect(original_pane_bounds);
    modal_host->RemoveObserver(&modal_observer);
    CHECK_GE(modal_observer.position_update_count(), 2)
        << "CMUX_PERMISSION_SELFTEST modal host ignored clipping movement";

    CHECK(browser->GetFeatures().content_setting_bubble_model_delegate());
    LOG(WARNING) << "cmux-views: CMUX_PERMISSION_SELFTEST PASS "
                    "pane-routed location bars, content settings, and modal "
                    "host ready";
  }

 private:
  void UpdateWorkspaceResizeHandles(const Workspace& workspace,
                                    const StripLayout& layout,
                                    int strip_width) {
    for (auto& entry : split_resize_handles_) {
      entry.second->SetVisible(false);
    }

    const gfx::Rect viewport(0, 0, strip_width, height());
    for (const DividerBox& divider : layout.dividers) {
      WorkspaceResizeHandle* handle = nullptr;
      auto found = split_resize_handles_.find(divider.split);
      if (found == split_resize_handles_.end()) {
        // The column handle is the last child. Add split handles directly
        // before it, preserving the pane-then-handles z-order invariant
        // without invoking layer reordering during layout.
        CHECK(!content_->children().empty());
        handle = content_->AddChildViewAt(
            std::make_unique<WorkspaceResizeHandle>(
                this, WorkspaceResizeHandle::Target::kSplit),
            content_->children().size() - 1);
        split_resize_handles_[divider.split] = handle;
      } else {
        handle = found->second;
      }
      handle->ConfigureSplit(divider);
      gfx::Rect bounds;
      if (divider.orientation == SplitOrientation::kHorizontal) {
        const int center =
            static_cast<int>(divider.rect.x + divider.rect.width * 0.5);
        bounds = gfx::Rect(center - kPaneResizeHandleHitWidth / 2,
                           static_cast<int>(divider.rect.y),
                           kPaneResizeHandleHitWidth,
                           std::max(0, static_cast<int>(divider.rect.height)));
      } else {
        const int center =
            static_cast<int>(divider.rect.y + divider.rect.height * 0.5);
        bounds = gfx::Rect(static_cast<int>(divider.rect.x),
                           center - kPaneResizeHandleHitWidth / 2,
                           std::max(0, static_cast<int>(divider.rect.width)),
                           kPaneResizeHandleHitWidth);
      }
      handle->SetBoundsRect(bounds);
      handle->SetVisible(viewport.Intersects(bounds));
    }

    const int focused_column = ColumnOf(workspace, workspace.focused);
    // A column edge is resizable only when another side-by-side column exists.
    // Painting this at the right edge of a one-column workspace looks like a
    // permanent side-panel handle even though there is nothing to resize.
    if (!column_resize_handle_ || layout.column_widths.size() < 2 ||
        focused_column < 0 || strip_width <= 0 ||
        focused_column >= static_cast<int>(layout.column_widths.size())) {
      if (column_resize_handle_) {
        column_resize_handle_->SetVisible(false);
      }
      return;
    }

    double column_right = -1.0;
    double column_top = static_cast<double>(height());
    double column_bottom = 0.0;
    for (const PaneBox& pane : layout.panes) {
      if (ColumnOf(workspace, pane.pane) != focused_column) {
        continue;
      }
      column_right = std::max(column_right, pane.rect.x + pane.rect.width);
      column_top = std::min(column_top, pane.rect.y);
      column_bottom = std::max(column_bottom, pane.rect.y + pane.rect.height);
    }
    if (column_right < 0.0 || column_bottom <= column_top) {
      column_resize_handle_->SetVisible(false);
      return;
    }
    const int half_hit = kPaneResizeHandleHitWidth / 2;
    const int center = std::clamp(static_cast<int>(column_right), half_hit,
                                  std::max(half_hit, strip_width - half_hit));
    column_resize_handle_->ConfigureColumn(
        workspace.focused, layout.column_widths[focused_column] / strip_width,
        strip_width);
    column_resize_handle_->SetBoundsRect(
        gfx::Rect(center - half_hit, static_cast<int>(column_top),
                  kPaneResizeHandleHitWidth,
                  std::max(0, static_cast<int>(column_bottom - column_top))));
    column_resize_handle_->SetVisible(true);
  }

  // Repaint focus borders + show only the active workspace's panes + run the
  // niri focus-into-view scroll (per-workspace) for the model's focused pane.
  // Never moves keyboard focus (that's FocusController's deferred job).
  void ApplyVisualsAndScroll(bool animated) {
    ApplyVisualsAndScroll(animated,
                          ScrollIntoViewPolicy::kEnsureVisibleWithMargin);
  }

  void ApplyVisualsAndScroll(bool animated,
                             ScrollIntoViewPolicy policy) override {
    ApplyVisualsAndScrollInternal(animated, policy, FocusedPane());
  }

  void ApplyVisualsAndScrollInternal(bool animated,
                                     ScrollIntoViewPolicy policy,
                                     PaneId scroll_focus) {
    const Workspace* ws = model_.GetWorkspace(ws_);
    if (!ws) {
      return;
    }
    const PaneId focused = ws->focused;
    const bool show_focus_borders = model_.PanesOf(ws_).size() > 1;
    for (auto& kv : views_) {
      const bool in_active = model_.FindPane(ws_, kv.first) != nullptr;
      // Only the active workspace's panes are visible; the others stay
      // parented but hidden, preserving their WebContents/terminal state
      // across switches.
      kv.second->SetVisible(in_active);
      kv.second->SetFocusBorderVisible(in_active && show_focus_borders);
      kv.second->SetFocusedBorder(in_active && kv.first == focused);
    }
    // Panes are children of content_, so lay them out in content_-relative
    // coordinates -- the root owns either left or right rail positioning.
    const int strip_w = StripWidthForLayout();
    double scroll = scroll_by_ws_[ws_];  // per-workspace niri scroll
    const double scroll_before = scroll;
    StripLayout sl =
        ComputeStripLayout(*ws, strip_w, height(), scroll, scroll_focus, policy,
                           CurrentLayoutMetrics());
    scroll_by_ws_[ws_] = sl.scroll_x;  // persist the clamped value for this ws
    pane_titlebar_geometry_.clear();
    const bool can_animate =
        animated && AnimationsEnabled() && GetWidget() != nullptr;
    for (const PaneBox& b : sl.panes) {
      CmuxPaneView* p = ViewForPane(b.pane);
      if (!p) {
        continue;
      }
      gfx::Rect target(static_cast<int>(b.rect.x), static_cast<int>(b.rect.y),
                       static_cast<int>(b.rect.width),
                       static_cast<int>(b.rect.height));
      p->SetRoundedFrameGeometry(FrameGeometryForPane(target, strip_w));
      // Base native-window clearance on the pane's logical, unscrolled
      // position. The current content origin is applied separately on every
      // root-animation tick, so Cmd+B moves the first tab continuously while
      // niri scrolling never counter-translates it against its own pane.
      pane_titlebar_geometry_[b.pane] =
          gfx::Rect(static_cast<int>(b.rect.x + sl.scroll_x), target.y(),
                    target.width(), target.height());
      const auto entrance_it = pending_entrance_panes_.find(b.pane);
      const bool entrance = entrance_it != pending_entrance_panes_.end();
      const PaneEntrance entrance_spec =
          entrance ? entrance_it->second : PaneEntrance();
      if (entrance) {
        pending_entrance_panes_.erase(entrance_it);
      }
      // A pane must never animate out of the default empty Views bounds. New
      // workspaces and the first app frame start at their final geometry;
      // only panes explicitly marked as entrances grow into an existing
      // layout.
      const bool has_initial_geometry = !p->bounds().IsEmpty();
      if (can_animate && (has_initial_geometry || entrance)) {
        if (entrance) {
          animator_->StopAnimatingView(p);
          p->SetBoundsRect(
              EntranceSeedBounds(target, scroll_before, sl.scroll_x,
                                 entrance_spec));
        }
        animator_->AnimateViewTo(p, target);
      } else {
        animator_->StopAnimatingView(p);
        p->SetBoundsRect(target);
      }
    }
    UpdatePaneTitlebarInsets();
    UpdateWorkspaceResizeHandles(*ws, sl, strip_w);
  }

  // ---- Workspace switching + rail sync -------------------------------------
  Browser* CreateBrowserForWorkspace(WorkspaceId workspace,
                                     bool ensure_model_tabs = true) {
    if (!profile_services_available_ || !profile_ ||
        workspace == kInvalidId || !model_.GetWorkspace(workspace)) {
      return nullptr;
    }
    if (auto it = workspace_browsers_.find(workspace);
        it != workspace_browsers_.end()) {
      return it->second.get();
    }

    const CmuxDurableWorkspaceAddress durable_workspace{
        window_group_, WorkspaceRegistryKey(workspace)};
    const std::string durable_workspace_token =
        CmuxDurableWorkspaceToken(durable_workspace);
    if (durable_workspace_token.empty()) {
      LOG(ERROR) << "cmux-views: refusing noncanonical durable workspace key";
      return nullptr;
    }

    const WorkspaceLocator locator{native_window_, workspace};
    if (!native_window_registry_->ContainsWorkspace(locator)) {
      CHECK(native_window_registry_->RegisterWorkspace(locator));
    }

    RegisterCmuxBrowserWindowHost(native_window_, workspace, window_group_,
                                  WorkspaceRegistryKey(workspace),
                                  weak_factory_.GetWeakPtr());
    Browser::CreateParams params(profile_, /*user_gesture=*/false);
    params.initial_workspace = durable_workspace_token;
    // Workspace creation is explicit cmux state restoration/interaction, not
    // a signal to run Chrome's profile-wide startup restore for every logical
    // window. The Browser still participates in normal session/window
    // services once constructed.
    params.should_trigger_session_restore = false;
    Browser* browser = Browser::Create(params);
    CHECK(browser);
    CHECK(browser->window());
    CHECK_EQ(browser->initial_workspace(), durable_workspace_token);
    workspace_browsers_[workspace] = browser->AsWeakPtr();
    auto side_panel_ui = CreateCmuxSidePanelUI(browser);
    browser->GetFeatures().SetSidePanelUIForCustomBrowserWindow(
        side_panel_ui.get());
    workspace_side_panel_uis_[workspace] = std::move(side_panel_ui);
    browser->tab_strip_model()->AddObserver(this);
    CompleteViewsStartupAfterWorkspaceBrowserCreated();
    if (ensure_model_tabs) {
      EnsureBrowserTabsForWorkspaceModel(workspace);
    }
    return browser;
  }

  Browser* BrowserForWorkspace(WorkspaceId workspace) const {
    auto it = workspace_browsers_.find(workspace);
    return it == workspace_browsers_.end() ? nullptr : it->second.get();
  }

  void EnsureBrowserTabsForWorkspaceModel(WorkspaceId workspace) {
    Browser* browser = BrowserForWorkspace(workspace);
    if (!browser) {
      return;
    }
    for (PaneId pane_id : model_.PanesOf(workspace)) {
      const Pane* pane = model_.FindPane(workspace, pane_id);
      if (!pane) {
        continue;
      }
      for (const SurfaceTab& tab : pane->tabs) {
        if (tab.kind != SurfaceKind::kWeb || WebContentsForSurface(tab.id)) {
          continue;
        }
        ++internal_browser_mutation_depth_;
        content::WebContents* contents = chrome::AddAndReturnTabAt(
            browser, ConfiguredNewTabURL(), /*index=*/-1,
            /*foreground=*/browser->tab_strip_model()->empty());
        --internal_browser_mutation_depth_;
        if (contents) {
          RecordWebTabPlacement(contents, workspace, pane_id, tab.id);
        }
      }
    }
    SyncBrowserTabOrder(workspace);
  }

  WorkspaceId WorkspaceForTabStripModel(TabStripModel* model) const {
    if (!model) {
      return kInvalidId;
    }
    for (const auto& [workspace, browser] : workspace_browsers_) {
      if (browser && browser->tab_strip_model() == model) {
        return workspace;
      }
    }
    return kInvalidId;
  }

  content::WebContents* WebContentsForSurface(SurfaceTabId surface) const {
    auto it = web_contents_by_surface_.find(surface);
    return it == web_contents_by_surface_.end() ? nullptr : it->second.get();
  }

  TabStripModel* BrowserTabsForSurface(SurfaceTabId surface, int* index) const {
    if (index) {
      *index = TabStripModel::kNoTab;
    }
    content::WebContents* contents = WebContentsForSurface(surface);
    auto placement = contents ? web_tab_placements_.find(contents)
                              : web_tab_placements_.end();
    if (!contents || placement == web_tab_placements_.end()) {
      return nullptr;
    }
    Browser* browser = BrowserForWorkspace(placement->second.workspace);
    TabStripModel* tabs = browser ? browser->tab_strip_model() : nullptr;
    if (tabs && index) {
      *index = tabs->GetIndexOfWebContents(contents);
    }
    return tabs;
  }

  void ActivateBrowserTabForSurface(SurfaceTabId surface) {
    content::WebContents* contents = WebContentsForSurface(surface);
    auto placement = contents ? web_tab_placements_.find(contents)
                              : web_tab_placements_.end();
    if (!contents || placement == web_tab_placements_.end()) {
      return;  // terminals do not participate in Browser activation
    }
    Browser* browser = BrowserForWorkspace(placement->second.workspace);
    TabStripModel* tabs = browser ? browser->tab_strip_model() : nullptr;
    const int index =
        tabs ? tabs->GetIndexOfWebContents(contents) : TabStripModel::kNoTab;
    if (tabs && index != TabStripModel::kNoTab &&
        tabs->active_index() != index) {
      tabs->ActivateTabAt(index);
    }
  }

  void ActivateSelectedWebTabInPane(PaneId pane_id) {
    const WorkspaceId workspace = WorkspaceForPane(pane_id);
    const Pane* pane = model_.FindPane(workspace, pane_id);
    if (pane) {
      ActivateBrowserTabForSurface(pane->selected);
    }
  }

  bool WorkspaceHasTerminalSurface(WorkspaceId workspace) const {
    for (PaneId pane_id : model_.PanesOf(workspace)) {
      const Pane* pane = model_.FindPane(workspace, pane_id);
      if (!pane) {
        continue;
      }
      for (const SurfaceTab& tab : pane->tabs) {
        if (tab.kind == SurfaceKind::kTerminal) {
          return true;
        }
      }
    }
    return false;
  }

  void RecordWebTabPlacement(content::WebContents* contents,
                             WorkspaceId workspace,
                             PaneId pane,
                             SurfaceTabId surface) {
    CHECK(contents);
    web_tab_placements_[contents] = {workspace, pane, surface};
    web_contents_by_surface_[surface] = contents;
  }

  void RefreshWebTabPlacementsFromModel(WorkspaceId workspace) {
    for (auto& [contents, placement] : web_tab_placements_) {
      if (placement.workspace != workspace) {
        continue;
      }
      for (PaneId pane_id : model_.PanesOf(workspace)) {
        const Pane* pane = model_.FindPane(workspace, pane_id);
        if (pane && pane->IndexOfTab(placement.surface) >= 0) {
          placement.pane = pane_id;
          break;
        }
      }
    }
  }

  std::vector<content::WebContents*> DesiredBrowserTabOrder(
      WorkspaceId workspace) const {
    std::vector<content::WebContents*> order;
    for (PaneId pane_id : model_.PanesOf(workspace)) {
      const Pane* pane = model_.FindPane(workspace, pane_id);
      if (!pane) {
        continue;
      }
      for (const SurfaceTab& tab : pane->tabs) {
        if (tab.kind != SurfaceKind::kWeb) {
          continue;
        }
        content::WebContents* contents = WebContentsForSurface(tab.id);
        if (contents) {
          order.push_back(contents);
        }
      }
    }
    return order;
  }

  void SyncBrowserTabOrder(WorkspaceId workspace) {
    Browser* browser = BrowserForWorkspace(workspace);
    TabStripModel* tabs = browser ? browser->tab_strip_model() : nullptr;
    if (!tabs) {
      return;
    }
    RefreshWebTabPlacementsFromModel(workspace);
    const std::vector<content::WebContents*> desired =
        DesiredBrowserTabOrder(workspace);
    ++internal_browser_mutation_depth_;
    for (size_t target = 0; target < desired.size(); ++target) {
      const int current = tabs->GetIndexOfWebContents(desired[target]);
      if (current != TabStripModel::kNoTab &&
          current != static_cast<int>(target)) {
        tabs->MoveWebContentsAt(current, static_cast<int>(target),
                                /*select_after_move=*/false);
      }
    }
    --internal_browser_mutation_depth_;
  }

  void AdoptBrowserTabIntoWorkspace(WorkspaceId workspace,
                                    content::WebContents* contents,
                                    int browser_index,
                                    bool foreground) {
    if (!contents || web_tab_placements_.contains(contents)) {
      return;
    }
    Workspace* workspace_model = model_.GetWorkspace(workspace);
    if (!workspace_model) {
      return;
    }
    PaneId pane_id = workspace_model->focused;
    int model_index = -1;
    Browser* browser = BrowserForWorkspace(workspace);
    TabStripModel* tabs = browser ? browser->tab_strip_model() : nullptr;
    if (tabs && browser_index >= 0) {
      // Project Chromium's linear insertion point into cmux's spatial model.
      // Prefer the following native tab (insert before it); at the end, insert
      // after the preceding tab. This also chooses the correct pane when an
      // extension supplies both windowId and index.
      for (int i = browser_index + 1; i < tabs->count(); ++i) {
        auto next = web_tab_placements_.find(tabs->GetWebContentsAt(i));
        if (next != web_tab_placements_.end() &&
            next->second.workspace == workspace) {
          pane_id = next->second.pane;
          if (const Pane* target = model_.FindPane(workspace, pane_id)) {
            model_index = target->IndexOfTab(next->second.surface);
          }
          break;
        }
      }
      if (model_index < 0) {
        for (int i = browser_index - 1; i >= 0; --i) {
          auto previous = web_tab_placements_.find(tabs->GetWebContentsAt(i));
          if (previous != web_tab_placements_.end() &&
              previous->second.workspace == workspace) {
            pane_id = previous->second.pane;
            if (const Pane* target = model_.FindPane(workspace, pane_id)) {
              model_index = target->IndexOfTab(previous->second.surface) + 1;
            }
            break;
          }
        }
      }
    }
    Pane* pane = model_.FindPane(workspace, pane_id);
    if (!pane) {
      const std::vector<PaneId> panes = model_.PanesOf(workspace);
      pane_id = panes.empty() ? kInvalidId : panes.front();
      pane = model_.FindPane(workspace, pane_id);
    }
    if (!pane) {
      return;
    }

    const SurfaceTabId previous_selection = pane->selected;
    SurfaceTabId surface = kInvalidId;
    for (const SurfaceTab& candidate : pane->tabs) {
      if (candidate.kind == SurfaceKind::kWeb &&
          !WebContentsForSurface(candidate.id)) {
        surface = candidate.id;
        break;
      }
    }
    if (surface == kInvalidId) {
      surface = model_.AddTab(workspace, pane_id, SurfaceKind::kWeb,
                              base::UTF16ToUTF8(contents->GetTitle()));
    }
    if (surface == kInvalidId) {
      return;
    }
    if (model_index >= 0) {
      model_.ReorderTab(workspace, pane_id, surface, model_index);
    }
    RecordWebTabPlacement(contents, workspace, pane_id, surface);
    if (foreground) {
      model_.SelectTab(workspace, pane_id, surface);
      model_.FocusPane(workspace, pane_id);
    }

    if (CmuxPaneView* view = ViewForPane(pane_id)) {
      CmuxSurface* presentation =
          AddBrowserSurface(view->surface_container(), contents, pane_id);
      view->BindSurface(surface, presentation);
      RefreshPaneTabs(pane_id);
    }
    if (!foreground && previous_selection != kInvalidId) {
      model_.SelectTab(workspace, pane_id, previous_selection);
      if (workspace == ws_) {
        RefreshPaneTabs(pane_id);
      }
    }
  }

  void ApplyBrowserTabMove(WorkspaceId workspace,
                           TabStripModel* tabs,
                           content::WebContents* contents,
                           int browser_index) {
    auto source_it = web_tab_placements_.find(contents);
    if (source_it == web_tab_placements_.end() || !tabs) {
      return;
    }
    const WebTabPlacement source = source_it->second;
    PaneId destination_pane = source.pane;
    int destination_index = -1;

    for (int i = browser_index + 1; i < tabs->count(); ++i) {
      content::WebContents* neighbor = tabs->GetWebContentsAt(i);
      auto neighbor_it = web_tab_placements_.find(neighbor);
      if (neighbor != contents && neighbor_it != web_tab_placements_.end() &&
          neighbor_it->second.workspace == workspace) {
        destination_pane = neighbor_it->second.pane;
        if (const Pane* pane = model_.FindPane(workspace, destination_pane)) {
          destination_index = pane->IndexOfTab(neighbor_it->second.surface);
        }
        break;
      }
    }
    if (destination_index < 0) {
      for (int i = browser_index - 1; i >= 0; --i) {
        content::WebContents* neighbor = tabs->GetWebContentsAt(i);
        auto neighbor_it = web_tab_placements_.find(neighbor);
        if (neighbor != contents && neighbor_it != web_tab_placements_.end() &&
            neighbor_it->second.workspace == workspace) {
          destination_pane = neighbor_it->second.pane;
          if (const Pane* pane = model_.FindPane(workspace, destination_pane)) {
            destination_index =
                pane->IndexOfTab(neighbor_it->second.surface) + 1;
          }
          break;
        }
      }
    }
    if (destination_index < 0) {
      return;
    }

    if (destination_pane == source.pane) {
      if (const Pane* pane = model_.FindPane(workspace, source.pane)) {
        const int source_index = pane->IndexOfTab(source.surface);
        if (source_index >= 0 && destination_index > source_index) {
          --destination_index;
        }
      }
      model_.ReorderTab(workspace, source.pane, source.surface,
                        destination_index);
      if (workspace == ws_) {
        RefreshPaneTabs(source.pane);
      }
      return;
    }

    CmuxPaneView* source_view = ViewForPane(source.pane);
    DetachedSurface detached;
    if (source_view) {
      detached = DetachSurface(source.pane, source.surface);
    }
    model_.MoveTab(workspace, source.pane, source.surface, workspace,
                   destination_pane, destination_index);
    source_it->second.pane = destination_pane;
    if (detached.view && detached.surface) {
      AttachSurfaceToPane(destination_pane, std::move(detached));
    }
    if (workspace == ws_) {
      RefreshPaneTabs(destination_pane);
      if (model_.FindPane(workspace, source.pane)) {
        RefreshPaneTabs(source.pane);
      } else if (source_view) {
        DestroyPaneView(source.pane, /*notify_focus=*/false);
      }
    }
  }

  void RemoveBrowserTabPlacement(content::WebContents* contents) {
    auto placement_it = web_tab_placements_.find(contents);
    if (placement_it == web_tab_placements_.end()) {
      return;
    }
    const WebTabPlacement placement = placement_it->second;
    web_tab_placements_.erase(placement_it);
    web_contents_by_surface_.erase(placement.surface);

    Pane* pane = model_.FindPane(placement.workspace, placement.pane);
    if (!pane || pane->IndexOfTab(placement.surface) < 0) {
      return;
    }
    CmuxPaneView* view = ViewForPane(placement.pane);
    if (view) {
      view->RemoveSurface(placement.surface);
    }

    // WindowModel deliberately refuses to empty a workspace's final pane.
    // For a native Browser whose final web tab was closed, the workspace is
    // the Chrome window and therefore closes too. Browser::TabStripEmpty may
    // request the same close; ScheduleCloseWorkspace coalesces both paths.
    if (pane->tabs.size() == 1 &&
        model_.PanesOf(placement.workspace).size() == 1) {
      ScheduleCloseWorkspace(placement.workspace);
      return;
    }

    model_.CloseTab(placement.workspace, placement.pane, placement.surface);
    if (model_.FindPane(placement.workspace, placement.pane)) {
      if (view) {
        RefreshPaneTabs(placement.pane);
      }
    } else if (view) {
      DestroyPaneView(placement.pane, /*notify_focus=*/false);
    }
  }

  void ReplaceBrowserTabContents(content::WebContents* old_contents,
                                 content::WebContents* new_contents) {
    if (!old_contents || !new_contents) {
      return;
    }
    auto it = web_tab_placements_.find(old_contents);
    if (it == web_tab_placements_.end()) {
      return;
    }
    const WebTabPlacement placement = it->second;
    web_tab_placements_.erase(it);
    RecordWebTabPlacement(new_contents, placement.workspace, placement.pane,
                          placement.surface);
    if (CmuxPaneView* view = ViewForPane(placement.pane)) {
      view->RemoveSurface(placement.surface);
      CmuxSurface* presentation =
          AddBrowserSurface(view->surface_container(), new_contents,
                            placement.pane);
      view->BindSurface(placement.surface, presentation);
      RefreshPaneTabs(placement.pane);
    }
  }

  void ScheduleCloseWorkspace(WorkspaceId workspace) {
    if (!model_.GetWorkspace(workspace) ||
        closing_workspaces_.contains(workspace) ||
        pending_workspace_closes_.contains(workspace)) {
      return;
    }
    pending_workspace_closes_.insert(workspace);
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE,
        base::BindOnce(
            [](base::WeakPtr<CmuxWindowView> self, WorkspaceId workspace) {
              if (!self) {
                return;
              }
              self->pending_workspace_closes_.erase(workspace);
              self->CloseWorkspaceAndBrowser(workspace);
            },
            weak_factory_.GetWeakPtr(), workspace));
  }

  void CloseWorkspaceAndBrowser(WorkspaceId workspace) {
    const Workspace* workspace_model = model_.GetWorkspace(workspace);
    if (!workspace_model || closing_workspaces_.contains(workspace)) {
      return;
    }
    if (!tui_originated_workspace_closes_.contains(workspace) &&
        !workspace_model->registry_key.empty()) {
      ResetWorkspaceMutationFailureBudget();
      pending_workspace_mutations_.ForgetCreate(workspace_model->registry_key);
      pending_workspace_mutations_.ForgetRename(workspace_model->registry_key);
      pending_workspace_mutations_.RememberClose(
          workspace_model->registry_key, NewWorkspaceMutationId());
      SyncWorkspaceRegistry();
    }
    closing_workspaces_.insert(workspace);

    Browser* browser = BrowserForWorkspace(workspace);
    if (browser) {
      // Browser closing can be asynchronous while beforeunload/unload and tab
      // observers run. Keep the pane LocationBar and BrowserWindow host alive
      // until BrowserList confirms the Browser has actually been removed;
      // Browser::OnTabDeactivated dereferences them during that interval.
      if (
#if CHROME_VERSION_MAJOR >= 150
          !browser->IsDeleteScheduled()) {
#else
          !browser->is_delete_scheduled()) {
#endif
        browser->OnWindowClosing();
      }
      return;
    }

    FinalizeClosedWorkspace(workspace, nullptr);
  }

  void ExecuteWorkspaceContextActionNow(WorkspaceId workspace,
                                        WorkspaceContextAction action) {
    const Workspace* model_workspace = model_.GetWorkspace(workspace);
    if (!model_workspace) {
      return;
    }
    // TabStripModel::GetIndicesForCommand semantics: a menu opened on a
    // selected row acts on the complete selection; an unselected context row
    // remains a singleton target without disturbing that selection.
    const std::vector<WorkspaceId> targets =
        model_.WorkspacesForCommand(workspace);
    switch (action) {
      case WorkspaceContextAction::kNewSplitSideBySide:
      case WorkspaceContextAction::kNewSplitStacked: {
        if (targets.size() != 1) {
          return;
        }
        if (workspace != ws_) {
          SwitchWorkspace(workspace);
        }
        const Workspace* active_workspace = model_.GetWorkspace(workspace);
        const Pane* pane =
            active_workspace
                ? model_.FindPane(workspace, active_workspace->focused)
                : nullptr;
        if (!pane || pane->selected == kInvalidId) {
          return;
        }
        NewSplitWithCurrentTabDeferred(
            pane->id, pane->selected,
            action == WorkspaceContextAction::kNewSplitSideBySide
                ? SplitOrientation::kHorizontal
                : SplitOrientation::kVertical);
        return;
      }
      case WorkspaceContextAction::kNewWorkspaceBelow: {
        const WorkspaceGroupId source_group = model_workspace->group;
        const auto it =
            std::find(model_.roots().begin(), model_.roots().end(), workspace);
        const int at = it == model_.roots().end()
                           ? static_cast<int>(model_.roots().size())
                           : static_cast<int>(it - model_.roots().begin()) + 1;
        const WorkspaceId fresh = AddPendingWorkspace("New tab");
        if (source_group != kInvalidId) {
          const std::vector<WorkspaceId> members =
              model_.WorkspacesInGroup(source_group);
          const auto member =
              std::find(members.begin(), members.end(), workspace);
          const int member_index =
              member == members.end()
                  ? static_cast<int>(members.size())
                  : static_cast<int>(member - members.begin()) + 1;
          model_.MoveWorkspace(fresh,
                               members.empty() ? workspace : members.front(),
                               member_index);
        } else {
          model_.MoveWorkspace(fresh, kInvalidId, at);
        }
        CreateBrowserForWorkspace(fresh);
        SwitchWorkspace(fresh);
        RequestWorkspaceOrder();
        return;
      }
      case WorkspaceContextAction::kAddToNewGroup: {
        const WorkspaceId first =
            targets.empty() ? kInvalidId : targets.front();
        if (first == kInvalidId) {
          return;
        }
        const WorkspaceGroupId group = model_.CreateWorkspaceGroup(first);
        if (group == kInvalidId) {
          return;
        }
        if (targets.size() > 1) {
          model_.MoveWorkspaces(
              std::vector<WorkspaceId>(targets.begin() + 1, targets.end()),
              first, -1);
        }
        RefreshRail();
        MarkWorkspaceProjectionDirty();
        RequestWorkspaceOrder();
        ShowWorkspaceGroupEditorBubbleDeferred(group);
        return;
      }
      case WorkspaceContextAction::kRemoveFromGroup:
        model_.UngroupWorkspaces(targets);
        RefreshRail();
        MarkWorkspaceProjectionDirty();
        RequestWorkspaceOrder();
        return;
      case WorkspaceContextAction::kReload:
        for (WorkspaceId target : targets) {
          for (SurfaceTabId tab : WebTabsInWorkspace(target)) {
            if (content::WebContents* contents = WebContentsForSurface(tab)) {
              contents->GetController().Reload(content::ReloadType::NORMAL,
                                               /*check_for_repost=*/true);
            }
          }
        }
        return;
      case WorkspaceContextAction::kToggleSiteMuted: {
        const auto web_tabs = WorkspaceContextWebTabTargets(workspace);
        // Match Chromium's multi-tab command: if any advertised site is
        // unmuted, mute every target; only unmute when all targets are muted.
        // Compute that decision once so mixed sites converge instead of each
        // independently toggling.
        const bool mute = std::ranges::any_of(web_tabs, [](const auto& target) {
          const auto& [tabs, browser_index] = target;
          return !CmuxIsSiteMuted(*tabs, browser_index);
        });
        for (const auto& [tabs, browser_index] : web_tabs) {
          if (!tabs->IsContextMenuCommandEnabled(
                  browser_index, TabStripModel::CommandToggleSiteMuted) ||
              CmuxIsSiteMuted(*tabs, browser_index) == mute) {
            continue;
          }
          // ExecuteContextMenuCommand owns Chromium's exact site-setting and
          // chrome:// mute behavior. Rechecking each tab before toggling also
          // prevents overlapping native selection sets from being flipped
          // more than once.
          tabs->ExecuteContextMenuCommand(
              browser_index, TabStripModel::CommandToggleSiteMuted);
        }
        return;
      }
      case WorkspaceContextAction::kAddToReadLater:
        for (WorkspaceId target : targets) {
          for (SurfaceTabId surface : WebTabsInWorkspace(target)) {
            int browser_index = TabStripModel::kNoTab;
            TabStripModel* tabs =
                BrowserTabsForSurface(surface, &browser_index);
            if (tabs && browser_index != TabStripModel::kNoTab &&
                tabs->delegate()->SupportsReadLater() &&
                tabs->IsReadLaterSupportedForAny({browser_index}) &&
                tabs->IsContextMenuCommandEnabled(
                    browser_index, TabStripModel::CommandAddToReadLater)) {
              tabs->ExecuteContextMenuCommand(
                  browser_index, TabStripModel::CommandAddToReadLater);
            }
          }
        }
        return;
      case WorkspaceContextAction::kDuplicate: {
        std::vector<WorkspaceId> duplicates;
        for (WorkspaceId target : targets) {
          if (!model_.GetWorkspace(target)) {
            continue;
          }

          // WebContents::Clone() is Chromium's own Duplicate Tab primitive:
          // unlike reloading a URL, it retains the complete
          // NavigationController history while returning independent
          // ownership. Clone before mutating the WindowModel so source surface
          // ids remain stable.
          std::map<SurfaceTabId, std::unique_ptr<content::WebContents>>
              web_clones;
          std::map<SurfaceTabId, GURL> web_urls;
          for (SurfaceTabId source_tab : WebTabsInWorkspace(target)) {
            if (content::WebContents* contents =
                    WebContentsForSurface(source_tab)) {
              web_urls.emplace(source_tab, contents->GetVisibleURL());
              web_clones.emplace(source_tab, contents->Clone());
            }
          }

          ResetWorkspaceMutationFailureBudget();
          DuplicateWorkspaceResult duplicate =
              model_.DuplicateWorkspace(target, NewWorkspaceRegistryKey());
          const WorkspaceId fresh = duplicate.workspace;
          if (fresh == kInvalidId) {
            continue;
          }
          pending_workspace_mutations_.RememberCreate(
              WorkspaceRegistryKey(fresh), NewWorkspaceMutationId());

          Browser* duplicate_browser =
              CreateBrowserForWorkspace(fresh, /*ensure_model_tabs=*/false);
          TabStripModel* duplicate_tabs =
              duplicate_browser ? duplicate_browser->tab_strip_model()
                                : nullptr;
          std::set<SurfaceTabId> cloned_tabs;
          if (duplicate_tabs) {
            for (const auto& [source_tab, duplicate_tab] : duplicate.tabs) {
              auto clone = web_clones.find(source_tab);
              if (clone == web_clones.end() || !clone->second) {
                continue;
              }
              PaneId duplicate_pane = kInvalidId;
              for (PaneId pane_id : model_.PanesOf(fresh)) {
                const Pane* pane = model_.FindPane(fresh, pane_id);
                if (pane && pane->IndexOfTab(duplicate_tab) >= 0) {
                  duplicate_pane = pane_id;
                  break;
                }
              }
              if (duplicate_pane == kInvalidId) {
                continue;
              }

              content::WebContents* cloned_contents = clone->second.get();
              ++internal_browser_mutation_depth_;
              duplicate_tabs->AppendWebContents(
                  std::move(clone->second),
                  /*foreground=*/duplicate_tabs->empty());
              --internal_browser_mutation_depth_;
              RecordWebTabPlacement(cloned_contents, fresh, duplicate_pane,
                                    duplicate_tab);
              cloned_tabs.insert(duplicate_tab);
            }
          }
          // If a live source WebContents was unavailable, realize only that
          // missing model tab as a normal fresh page. Mixed terminal/web pane
          // graphs and all tab order/selection metadata are already cloned.
          EnsureBrowserTabsForWorkspaceModel(fresh);
          for (const auto& [source_tab, duplicate_tab] : duplicate.tabs) {
            if (cloned_tabs.contains(duplicate_tab)) {
              continue;
            }
            const auto source_url = web_urls.find(source_tab);
            content::WebContents* contents =
                WebContentsForSurface(duplicate_tab);
            if (source_url != web_urls.end() && source_url->second.is_valid() &&
                contents) {
              contents->GetController().LoadURL(
                  source_url->second, content::Referrer(),
                  ui::PAGE_TRANSITION_TYPED, std::string());
            }
          }
          if (auto scroll = scroll_by_ws_.find(target);
              scroll != scroll_by_ws_.end()) {
            scroll_by_ws_[fresh] = scroll->second;
          }
          SwitchWorkspace(fresh);
          duplicates.push_back(fresh);
        }
        if (duplicates.size() > 1) {
          model_.SelectWorkspace(duplicates.front());
          for (auto it = std::next(duplicates.begin()); it != duplicates.end();
               ++it) {
            model_.ToggleWorkspaceSelection(*it);
          }
          ActivateModelSelectedWorkspace();
        }
        RequestWorkspaceOrder();
        return;
      }
      case WorkspaceContextAction::kClose:
        for (WorkspaceId target : targets) {
          CloseWorkspaceAndBrowser(target);
        }
        return;
      case WorkspaceContextAction::kCloseOthers: {
        const std::vector<WorkspaceId> roots = model_.roots();
        for (WorkspaceId id : roots) {
          if (!std::ranges::contains(targets, id)) {
            CloseWorkspaceAndBrowser(id);
          }
        }
        return;
      }
      case WorkspaceContextAction::kCloseBelow: {
        const std::vector<WorkspaceId> roots = model_.roots();
        const auto it =
            targets.empty()
                ? roots.end()
                : std::find(roots.begin(), roots.end(), targets.back());
        if (it != roots.end()) {
          for (auto closing = std::next(it); closing != roots.end(); ++closing) {
            CloseWorkspaceAndBrowser(*closing);
          }
        }
        return;
      }
      default:
        break;
    }

    const std::optional<TabContextAction> tab_action =
        WorkspaceTabContextAction(action);
    model_workspace = model_.GetWorkspace(workspace);
    Pane* pane = model_workspace
                     ? model_.FindPane(workspace, model_workspace->focused)
                     : nullptr;
    if (workspace == ws_ && tab_action && pane &&
        pane->selected != kInvalidId) {
      OnTabContextActionRequested(pane->id, pane->selected, *tab_action);
    }
  }

  void ExecuteWorkspaceGroupContextActionNow(
      WorkspaceGroupId group,
      WorkspaceGroupContextAction action) {
    const std::vector<WorkspaceId> members =
        model_.WorkspacesInGroup(group);
    if (members.empty()) {
      return;
    }
    switch (action) {
      case WorkspaceGroupContextAction::kNewWorkspaceInGroup: {
        const WorkspaceId fresh = AddPendingWorkspace("New tab");
        model_.MoveWorkspace(fresh, members.front(), -1);
        CreateBrowserForWorkspace(fresh);
        SwitchWorkspace(fresh);
        RequestWorkspaceOrder();
        return;
      }
      case WorkspaceGroupContextAction::kCloseGroup:
        for (WorkspaceId member : members) {
          CloseWorkspaceAndBrowser(member);
        }
        return;
      case WorkspaceGroupContextAction::kUngroup:
        model_.UngroupWorkspaces(members);
        RefreshRail();
        MarkWorkspaceProjectionDirty();
        RequestWorkspaceOrder();
        return;
      case WorkspaceGroupContextAction::kMoveGroupToNewWindow:
        return;
    }
  }

  void FinalizeClosedWorkspace(WorkspaceId workspace, Browser* browser) {
    const Workspace* workspace_model = model_.GetWorkspace(workspace);
    if (!workspace_model) {
      return;
    }
    closing_workspaces_.insert(workspace);
    const std::vector<PaneId> panes = model_.PanesOf(workspace);
    std::vector<SurfaceTabId> terminal_tabs;
    for (PaneId pane : panes) {
      const Pane* pane_model = model_.FindPane(workspace, pane);
      if (!pane_model) {
        continue;
      }
      for (const SurfaceTab& tab : pane_model->tabs) {
        if (tab.kind == SurfaceKind::kTerminal) {
          terminal_tabs.push_back(tab.id);
        }
      }
    }
    for (PaneId pane : panes) {
      DestroyPaneView(pane, /*notify_focus=*/false);
    }
    // close-workspace atomically tombstones every child terminal in cmux-tui.
    // Erase only the GUI projections here; individual CloseTerminal calls
    // would race that transaction and can accidentally target a replacement.
    for (SurfaceTabId tab : terminal_tabs) {
      auto backend = terminal_backends_.find(tab);
      if (backend != terminal_backends_.end()) {
        UnregisterTerminalBackend(tab, backend->second);
        terminal_backends_.erase(backend);
      }
    }

    for (auto it = web_tab_placements_.begin();
         it != web_tab_placements_.end();) {
      if (it->second.workspace == workspace) {
        web_contents_by_surface_.erase(it->second.surface);
        it = web_tab_placements_.erase(it);
      } else {
        ++it;
      }
    }

    UnregisterCmuxBrowserWindowHost(native_window_, workspace);
    native_window_registry_->UnregisterWorkspace(
        WorkspaceLocator{native_window_, workspace});
    DestroyCmuxExtensionsContainerForBrowser(browser);
    if (browser) {
      browser->GetFeatures().SetSidePanelUIForCustomBrowserWindow(nullptr);
    }
    workspace_side_panel_uis_.erase(workspace);
    workspace_browsers_.erase(workspace);
    model_.CloseWorkspace(workspace);
    closing_workspaces_.erase(workspace);
    tui_originated_workspace_closes_.erase(workspace);

    if (model_.workspace_count() == 0) {
      if (views::Widget* widget = GetWidget()) {
        widget->Close();
      }
      return;
    }
    ws_ = model_.selected_workspace();
    EnsureActivePanesRealized();
    RefreshRail();
    focus_.OnWorkspaceActivated();
  }

  std::string WorkspaceRegistryKey(WorkspaceId workspace) const {
    const Workspace* workspace_model = model_.GetWorkspace(workspace);
    return workspace_model ? workspace_model->registry_key : std::string();
  }

  std::vector<CmuxCanonicalWorkspace> CanonicalWorkspaceList() const {
    std::vector<CmuxCanonicalWorkspace> canonical;
    if (!workspace_snapshot_) {
      return canonical;
    }
    canonical.reserve(workspace_snapshot_->workspaces.size());
    for (const CmuxTuiWorkspace& workspace :
         workspace_snapshot_->workspaces) {
      canonical.push_back({workspace.key, workspace.name});
    }
    return canonical;
  }

  CmuxWorkspaceProjection CaptureWorkspaceProjection() const {
    CmuxWorkspaceProjection projection;
    const Workspace* selected = model_.GetWorkspace(ws_);
    if (selected) {
      projection.selected_key = selected->registry_key;
    }
    for (const Workspace& workspace : model_.workspaces()) {
      if (workspace.registry_key.empty()) {
        continue;
      }
      CmuxWorkspaceProjectionEntry entry;
      entry.key = workspace.registry_key;
      entry.color = static_cast<int>(workspace.preferred_group_color);
      if (workspace.group != kInvalidId) {
        const std::vector<WorkspaceId> members =
            model_.WorkspacesInGroup(workspace.group);
        if (!members.empty() && members.front() != workspace.id) {
          if (const Workspace* representative =
                  model_.GetWorkspace(members.front())) {
            entry.parent_key = representative->registry_key;
          }
        }
        if (const WorkspaceGroup* group =
                model_.GetWorkspaceGroup(workspace.group)) {
          entry.expanded = !group->collapsed;
          entry.color = static_cast<int>(group->color);
          entry.group_title = group->title;
        }
      }
      entry.tiled = workspace.layout_mode == WorkspaceLayoutMode::kTiled;
      projection.workspaces.push_back(std::move(entry));
    }
    return NormalizeCmuxWorkspaceProjection(CanonicalWorkspaceList(),
                                            projection);
  }

  base::DictValue SerializeWorkspaceProjection(
      const CmuxWorkspaceProjection& projection) const {
    base::DictValue payload;
    payload.Set("selected_key", projection.selected_key);
    base::ListValue workspaces;
    for (const CmuxWorkspaceProjectionEntry& entry : projection.workspaces) {
      base::DictValue workspace;
      workspace.Set("key", entry.key);
      workspace.Set("parent_key", entry.parent_key);
      workspace.Set("expanded", entry.expanded);
      workspace.Set("color", entry.color);
      workspace.Set("layout_mode", entry.tiled ? "tiled" : "strip");
      if (entry.group_title) {
        workspace.Set("group_title", *entry.group_title);
      }
      workspaces.Append(std::move(workspace));
    }
    payload.Set("workspaces", std::move(workspaces));
    return payload;
  }

  CmuxWorkspaceProjection ParseWorkspaceProjection(
      const base::DictValue& payload) const {
    CmuxWorkspaceProjection projection;
    if (const std::string* selected = payload.FindString("selected_key")) {
      projection.selected_key = *selected;
    }
    const base::ListValue* workspaces = payload.FindList("workspaces");
    if (!workspaces) {
      return projection;
    }
    for (const base::Value& value : *workspaces) {
      if (!value.is_dict()) {
        continue;
      }
      const base::DictValue& workspace = value.GetDict();
      const std::string* key = workspace.FindString("key");
      if (!key || key->empty()) {
        continue;
      }
      CmuxWorkspaceProjectionEntry entry;
      entry.key = *key;
      if (const std::string* parent = workspace.FindString("parent_key")) {
        entry.parent_key = *parent;
      }
      entry.expanded = workspace.FindBool("expanded").value_or(true);
      entry.color = workspace.FindInt("color").value_or(0);
      entry.tiled = workspace.FindString("layout_mode") &&
                    *workspace.FindString("layout_mode") == "tiled";
      if (const std::string* title = workspace.FindString("group_title")) {
        entry.group_title = *title;
      }
      projection.workspaces.push_back(std::move(entry));
    }
    return projection;
  }

  void ApplyLoadedWorkspaceProjection() {
    if (!workspace_projection_loaded_ || !workspace_projection_supported_ ||
        workspace_projection_dirty_ || !loaded_workspace_projection_) {
      return;
    }
    CmuxWorkspaceProjection normalized = NormalizeCmuxWorkspaceProjection(
        CanonicalWorkspaceList(), *loaded_workspace_projection_);
    const bool normalization_changed =
        normalized != *loaded_workspace_projection_;
    loaded_workspace_projection_ = normalized;
    applying_workspace_projection_ = true;

    // Re-root first so applying a valid persisted hierarchy never depends on
    // whatever hierarchy happened to be in memory before the canonical
    // snapshot arrived.
    for (const CmuxWorkspaceProjectionEntry& entry : normalized.workspaces) {
      if (const Workspace* workspace =
              model_.FindWorkspaceByRegistryKey(entry.key)) {
        model_.MoveWorkspace(workspace->id, kInvalidId, -1);
      }
    }
    for (const CmuxWorkspaceProjectionEntry& entry : normalized.workspaces) {
      Workspace* workspace = model_.FindWorkspaceByRegistryKey(entry.key);
      if (!workspace) {
        continue;
      }
      if (!entry.parent_key.empty()) {
        if (const Workspace* parent =
                model_.FindWorkspaceByRegistryKey(entry.parent_key)) {
          model_.MoveWorkspace(workspace->id, parent->id, -1);
        }
      } else if (entry.group_title) {
        // The representative of a shallow group has no parent_key. Persisted
        // group metadata is therefore also its explicit group marker, which
        // is essential when the group has only one member and no child entry
        // can otherwise recreate it. A child can precede its representative
        // in canonical order and create the group while being attached, so do
        // not split that already-restored group into a new singleton.
        if (workspace->group == kInvalidId) {
          model_.CreateWorkspaceGroup(workspace->id);
        }
      }
      model_.SetWorkspaceExpanded(workspace->id, entry.expanded);
      model_.SetWorkspaceColor(workspace->id,
                               static_cast<GroupColor>(entry.color));
      if (entry.group_title) {
        const Workspace* grouped = model_.GetWorkspace(workspace->id);
        if (grouped && grouped->group != kInvalidId) {
          model_.SetWorkspaceGroupTitle(grouped->group, *entry.group_title);
        }
      }
      model_.SetWorkspaceLayoutMode(
          workspace->id, entry.tiled ? WorkspaceLayoutMode::kTiled
                                     : WorkspaceLayoutMode::kStrip);
    }
    if (workspace_snapshot_) {
      std::vector<std::string> order;
      for (const CmuxTuiWorkspace& workspace :
           workspace_snapshot_->workspaces) {
        order.push_back(workspace.key);
      }
      model_.ReorderWorkspaceSiblingsByRegistryKeys(order);
    }
    if (const Workspace* selected =
            model_.FindWorkspaceByRegistryKey(normalized.selected_key);
        selected && selected->id != ws_) {
      SwitchWorkspace(selected->id);
    }
    applying_workspace_projection_ = false;
    if (normalization_changed) {
      MarkWorkspaceProjectionDirty();
    }
  }

  void ResetWorkspaceProjectionForRegistryChange() {
    ++workspace_projection_epoch_;
    workspace_projection_reload_barrier_.Reset(workspace_projection_epoch_);
    workspace_projection_revision_ = 0;
    workspace_projection_loaded_ = false;
    workspace_projection_get_in_flight_ = false;
    workspace_projection_put_in_flight_ = false;
    workspace_projection_supported_ = true;
    workspace_projection_dirty_ = false;
    workspace_projection_retry_count_ = 0;
    loaded_workspace_projection_.reset();
    queued_workspace_moves_.clear();
  }

  void MaybeDrainWorkspaceProjectionReload() {
    const bool blocked = workspace_projection_get_in_flight_ ||
                         workspace_projection_put_in_flight_ ||
                         !workspace_projection_supported_ ||
                         !tui_client_->ready();
    if (!workspace_projection_reload_barrier_.TakeReload(
            workspace_projection_epoch_, workspace_projection_revision_,
            blocked)) {
      return;
    }
    LoadWorkspaceProjection(/*force=*/true);
  }

  void LoadWorkspaceProjection(bool force = false) {
    if (!tui_client_->ready() || workspace_projection_get_in_flight_ ||
        workspace_projection_put_in_flight_ ||
        (!force && workspace_projection_loaded_)) {
      return;
    }
    workspace_projection_get_in_flight_ = true;
    const uint64_t epoch = workspace_projection_epoch_;
    tui_client_->GetFrontendProjection(base::BindOnce(
        &CmuxWindowView::OnWorkspaceProjectionLoaded,
        weak_factory_.GetWeakPtr(), epoch));
  }

  void OnWorkspaceProjectionLoaded(
      uint64_t epoch,
      std::optional<CmuxTuiFrontendProjection> projection,
      const std::string& error) {
    if (epoch != workspace_projection_epoch_) {
      return;
    }
    workspace_projection_get_in_flight_ = false;
    if (!projection) {
      workspace_projection_loaded_ = true;
      workspace_projection_supported_ = false;
      LOG(WARNING) << "cmux-views: frontend projection unavailable: "
                   << error;
      RunPendingStartupContentIfReady();
      MaybeDrainWorkspaceProjectionReload();
      return;
    }
    if (projection->schema_version > 1) {
      workspace_projection_loaded_ = true;
      workspace_projection_supported_ = false;
      LOG(WARNING) << "cmux-views: refusing to overwrite newer frontend "
                      "projection schema "
                   << projection->schema_version;
      RunPendingStartupContentIfReady();
      MaybeDrainWorkspaceProjectionReload();
      return;
    }

    workspace_projection_revision_ = projection->revision;
    workspace_projection_loaded_ = true;
    workspace_projection_supported_ = true;
    if (!workspace_projection_dirty_) {
      loaded_workspace_projection_ =
          projection->schema_version == 1
              ? ParseWorkspaceProjection(projection->projection)
              : CmuxWorkspaceProjection();
      ApplyLoadedWorkspaceProjection();
    }
    RunPendingStartupContentIfReady();
    MaybeDrainWorkspaceProjectionReload();
    SaveWorkspaceProjectionIfNeeded();
  }

  void MarkWorkspaceProjectionDirty() {
    if (applying_workspace_projection_) {
      return;
    }
    workspace_projection_dirty_ = true;
    ++workspace_projection_local_generation_;
    SaveWorkspaceProjectionIfNeeded();
  }

  void SaveWorkspaceProjectionIfNeeded() {
    if (!workspace_projection_dirty_ || !workspace_projection_loaded_ ||
        !workspace_projection_supported_ ||
        workspace_projection_get_in_flight_ ||
        workspace_projection_put_in_flight_ || !workspace_snapshot_ ||
        workspace_snapshot_->provisional ||
        !pending_workspace_mutations_.creates().empty() ||
        !pending_workspace_mutations_.closes().empty() ||
        !tui_client_->ready()) {
      return;
    }
    constexpr uint64_t kSchemaVersion = 1;
    workspace_projection_put_in_flight_ = true;
    const uint64_t epoch = workspace_projection_epoch_;
    const uint64_t local_generation = workspace_projection_local_generation_;
    CmuxWorkspaceProjection sent = CaptureWorkspaceProjection();
    tui_client_->PutFrontendProjection(
        kSchemaVersion, workspace_projection_revision_,
        SerializeWorkspaceProjection(sent),
        base::Uuid::GenerateRandomV4().AsLowercaseString(),
        base::BindOnce(&CmuxWindowView::OnWorkspaceProjectionSaved,
                       weak_factory_.GetWeakPtr(), epoch, local_generation,
                       std::move(sent)));
  }

  void OnWorkspaceProjectionSaved(
      uint64_t epoch,
      uint64_t local_generation,
      CmuxWorkspaceProjection sent,
      std::optional<CmuxTuiFrontendProjection> projection,
      const std::string& error) {
    if (epoch != workspace_projection_epoch_) {
      return;
    }
    workspace_projection_put_in_flight_ = false;
    if (!projection) {
      if (++workspace_projection_retry_count_ > 3) {
        workspace_projection_supported_ = false;
        LOG(ERROR) << "cmux-views: frontend projection CAS did not converge: "
                   << error;
        return;
      }
      // Fetch the winning revision, preserve the user's newer local state,
      // and retry once the CAS base is current.
      workspace_projection_loaded_ = false;
      LoadWorkspaceProjection(/*force=*/true);
      return;
    }
    workspace_projection_retry_count_ = 0;
    workspace_projection_revision_ = projection->revision;
    loaded_workspace_projection_ = std::move(sent);
    if (workspace_projection_local_generation_ == local_generation) {
      workspace_projection_dirty_ = false;
    }
    MaybeDrainWorkspaceProjectionReload();
    SaveWorkspaceProjectionIfNeeded();
  }

  void ReconcileWorkspaceRegistry(const CmuxTuiWorkspaceSnapshot& snapshot) {
    if (!snapshot.registry_id.empty() && !workspace_registry_id_.empty() &&
        snapshot.registry_id != workspace_registry_id_) {
      // The socket name was reused for a different durable registry. Drop
      // only transient operation/projection state; the incoming canonical
      // snapshot below will rebuild the GUI by UUID.
      pending_workspace_mutations_.Clear();
      workspace_snapshot_.reset();
      ResetWorkspaceProjectionForRegistryChange();
    }
    const bool generation_changed =
        !workspace_registry_generation_.empty() &&
        !snapshot.generation.empty() &&
        workspace_registry_generation_ != snapshot.generation;
    if (!snapshot.registry_id.empty()) {
      workspace_registry_id_ = snapshot.registry_id;
    }
    workspace_registry_generation_ = snapshot.generation;
    if (!snapshot.provisional && workspace_mutation_failure_count_ > 0 &&
        snapshot.revision != workspace_mutation_failed_revision_) {
      ResetWorkspaceMutationFailureBudget();
    }
    if (generation_changed) {
      ++workspace_projection_epoch_;
      workspace_projection_reload_barrier_.Reset(workspace_projection_epoch_);
      workspace_projection_get_in_flight_ = false;
      workspace_projection_put_in_flight_ = false;
      workspace_projection_loaded_ = false;
      workspace_projection_supported_ = true;
      workspace_projection_retry_count_ = 0;
    }

    std::vector<CmuxCanonicalWorkspace> canonical;
    canonical.reserve(snapshot.workspaces.size());
    for (const CmuxTuiWorkspace& remote : snapshot.workspaces) {
      canonical.push_back({remote.key, remote.name});
    }

    // There is no browser-owned startup registry. A confirmed empty canonical
    // registry gets exactly one optimistic create; otherwise every local
    // workspace below is materialized from a canonical UUID.
    if (!registry_bootstrapped_ && snapshot.workspaces.empty() &&
        model_.workspace_count() == 0) {
      const WorkspaceId home = model_.AddEmptyWorkspace(
          "Home", NewWorkspaceRegistryKey());
      CHECK_NE(home, kInvalidId);
      pending_workspace_mutations_.RememberCreate(
          WorkspaceRegistryKey(home), NewWorkspaceMutationId());
      CreateBrowserForWorkspace(home);
      ws_ = home;
      model_.SelectWorkspace(home);
      EnsureActivePanesRealized();
      // Wait for workspace registration before creating its first terminal.
      pending_terminal_columns_ = 1;
    }
    registry_bootstrapped_ = true;

    std::vector<CmuxLocalWorkspace> local_snapshot;
    local_snapshot.reserve(model_.workspace_count());
    for (const Workspace& local : model_.workspaces()) {
      local_snapshot.push_back({local.registry_key, local.title});
    }
    const CmuxWorkspaceReconcilePlan plan = BuildCmuxWorkspaceReconcilePlan(
        canonical, local_snapshot, pending_workspace_mutations_.CreateKeys(),
        pending_workspace_mutations_.CloseKeys(),
        pending_workspace_mutations_.renames());

    for (const CmuxCanonicalWorkspace& remote : plan.materialize) {
      if (!model_.FindWorkspaceByRegistryKey(remote.key)) {
        const WorkspaceId added =
            model_.AddEmptyWorkspace(remote.name, remote.key);
        CHECK(native_window_registry_->RegisterWorkspace(
            WorkspaceLocator{native_window_, added}));
        CreateBrowserForWorkspace(added);
        for (PaneId pane : model_.PanesOf(added)) {
          if (!views_.contains(pane)) {
            RealizePane(pane, GURL());
          }
        }
      }
    }
    for (const auto& [key, name] : plan.rename) {
      if (Workspace* local = model_.FindWorkspaceByRegistryKey(key)) {
        model_.SetWorkspaceTitle(local->id, name);
      }
    }
    for (const std::string& key : plan.remove) {
      if (const Workspace* local = model_.FindWorkspaceByRegistryKey(key)) {
        tui_originated_workspace_closes_.insert(local->id);
        ScheduleCloseWorkspace(local->id);
      }
    }

    const std::set<std::string> remote_keys(plan.canonical_order.begin(),
                                            plan.canonical_order.end());
    std::map<std::string, std::string> remote_names;
    for (const CmuxCanonicalWorkspace& workspace : canonical) {
      if (!workspace.key.empty()) {
        remote_names.try_emplace(workspace.key, workspace.name);
      }
    }
    for (const std::string& key : pending_workspace_mutations_.CreateKeys()) {
      if (remote_keys.contains(key)) {
        pending_workspace_mutations_.ForgetCreate(key);
      }
    }
    // A snapshot can race an older rename request. Do not treat an exact name
    // as confirmation while any registry mutation is still in flight: the
    // request can commit immediately after this snapshot and overwrite it.
    if (!snapshot.provisional && !workspace_mutation_in_flight_) {
      std::vector<std::string> completed_renames;
      for (const auto& [key, pending] :
           pending_workspace_mutations_.renames()) {
        const auto remote = remote_names.find(key);
        if ((remote != remote_names.end() && remote->second == pending.name) ||
            (remote == remote_names.end() &&
             !pending_workspace_mutations_.HasCreate(key))) {
          completed_renames.push_back(key);
        }
      }
      for (const std::string& key : completed_renames) {
        pending_workspace_mutations_.ForgetRename(key);
      }
    }
    if (!workspace_mutation_in_flight_) {
      for (const std::string& key : pending_workspace_mutations_.CloseKeys()) {
        if (!remote_keys.contains(key)) {
          pending_workspace_mutations_.ForgetClose(key);
        }
      }
    }
    if (!pending_workspace_mutations_.move() &&
        queued_workspace_moves_.empty()) {
      model_.ReorderWorkspaceSiblingsByRegistryKeys(plan.canonical_order);
    }
    workspace_snapshot_ = snapshot;
    if (!model_.GetWorkspace(ws_)) {
      ws_ = model_.selected_workspace();
    } else {
      // AddWorkspace selects the newly materialized model entry. Canonical
      // remote/empty workspace creation must not change the GUI's active
      // workspace, so restore the existing visible selection explicitly.
      model_.SelectWorkspace(ws_);
    }
    ApplyLoadedWorkspaceProjection();
    if (workspace_projection_loaded_) {
      RunPendingStartupContentIfReady();
    }
    RefreshRail();
    LoadWorkspaceProjection();
    // A terminal snapshot can legitimately arrive before its referenced empty
    // workspace snapshot. Workspace ownership is authoritative, so retry the
    // deferred terminal plan only after this registry projection exists.
    if (terminal_reconcile_waiting_for_workspace_ ||
        terminal_placement_model_.has_snapshot()) {
      ReconcileCanonicalTerminals();
    }
    ApplyCmuxTuiSelection();
    if (!snapshot.provisional) {
      StageNextQueuedWorkspaceMove();
      SyncWorkspaceRegistry();
      SaveWorkspaceProjectionIfNeeded();
    }
  }

  void SyncNextMissingWorkspace() { SyncWorkspaceRegistry(); }

  CmuxTuiWorkspaceMutation WorkspaceMutationForRetry(
      const std::string& mutation_id,
      uint64_t expected_revision) const {
    CmuxTuiWorkspaceMutation mutation;
    mutation.origin = tui_client_->options().origin.empty()
                          ? "chrome-gui"
                          : tui_client_->options().origin;
    mutation.mutation_id = mutation_id;
    if (!workspace_registry_generation_.empty()) {
      mutation.expected_generation = workspace_registry_generation_;
    }
    mutation.expected_revision = expected_revision;
    return mutation;
  }

  void SyncWorkspaceRegistry() {
    if (!workspace_snapshot_ || workspace_mutation_in_flight_ ||
        !tui_client_->ready()) {
      return;
    }
    const uint64_t revision = workspace_snapshot_->revision;
    if (workspace_mutation_failure_count_ >=
            kMaxWorkspaceMutationFailures &&
        workspace_mutation_failed_revision_ == revision) {
      return;
    }

    std::set<std::string> remote_keys;
    for (const CmuxTuiWorkspace& workspace : workspace_snapshot_->workspaces) {
      remote_keys.insert(workspace.key);
    }

    for (const auto& [key, mutation_id] :
         pending_workspace_mutations_.closes()) {
      if (!remote_keys.contains(key)) {
        continue;
      }
      workspace_mutation_in_flight_ = true;
      tui_client_->CloseWorkspace(
          key, WorkspaceMutationForRetry(mutation_id, revision),
          base::BindOnce(&CmuxWindowView::OnWorkspaceMutationFinished,
                         weak_factory_.GetWeakPtr(), revision));
      return;
    }

    for (const auto& [key, mutation_id] :
         pending_workspace_mutations_.creates()) {
      const Workspace* local = model_.FindWorkspaceByRegistryKey(key);
      if (!local || tui_originated_workspace_closes_.contains(local->id) ||
          pending_workspace_mutations_.HasClose(key) ||
          remote_keys.contains(key)) {
        continue;
      }
      workspace_mutation_in_flight_ = true;
      tui_client_->CreateWorkspace(
          local->title, local->registry_key,
          WorkspaceMutationForRetry(mutation_id, revision),
          base::BindOnce(&CmuxWindowView::OnWorkspaceMutationFinished,
                         weak_factory_.GetWeakPtr(), revision));
      return;
    }

    std::map<std::string, std::string> remote_names;
    for (const CmuxTuiWorkspace& workspace : workspace_snapshot_->workspaces) {
      remote_names.try_emplace(workspace.key, workspace.name);
    }
    for (auto it = pending_workspace_mutations_.renames().begin();
         it != pending_workspace_mutations_.renames().end(); ++it) {
      const std::string key = it->first;
      const CmuxPendingWorkspaceRename pending = it->second;
      const auto remote = remote_names.find(key);
      if (remote == remote_names.end() ||
          pending_workspace_mutations_.HasClose(key)) {
        continue;
      }
      if (remote->second == pending.name) {
        pending_workspace_mutations_.ForgetRename(key);
        SyncWorkspaceRegistry();
        return;
      }
      workspace_mutation_in_flight_ = true;
      tui_client_->RenameWorkspace(
          key, pending.name,
          WorkspaceMutationForRetry(pending.mutation_id, revision),
          base::BindOnce(&CmuxWindowView::OnWorkspaceMutationFinished,
                         weak_factory_.GetWeakPtr(), revision));
      return;
    }

    if (pending_workspace_mutations_.move()) {
      const CmuxPendingWorkspaceMove pending_move =
          *pending_workspace_mutations_.move();
      const std::string& key = pending_move.key;
      const size_t desired_index = pending_move.desired_index;
      size_t current_index = workspace_snapshot_->workspaces.size();
      for (size_t i = 0; i < workspace_snapshot_->workspaces.size(); ++i) {
        if (workspace_snapshot_->workspaces[i].key == key) {
          current_index = i;
          break;
        }
      }
      if (current_index == workspace_snapshot_->workspaces.size()) {
        // A move can be queued behind creation of the same workspace. Keep it
        // until an authoritative snapshot contains the key. If the key is no
        // longer being created, it was closed or removed remotely; skip that
        // obsolete step so the rest of the canonical order can still drain.
        if (!pending_workspace_mutations_.HasCreate(key)) {
          pending_workspace_mutations_.ForgetMove();
          CompleteQueuedWorkspaceMove(key, desired_index);
          StageNextQueuedWorkspaceMove();
          SyncWorkspaceRegistry();
        }
        return;
      }
      if (current_index == desired_index) {
        pending_workspace_mutations_.ForgetMove();
        CompleteQueuedWorkspaceMove(key, desired_index);
        StageNextQueuedWorkspaceMove();
        std::vector<std::string> keys;
        for (const CmuxTuiWorkspace& workspace :
             workspace_snapshot_->workspaces) {
          keys.push_back(workspace.key);
        }
        model_.ReorderWorkspaceSiblingsByRegistryKeys(keys);
        SyncWorkspaceRegistry();
        return;
      }
      // cmux-tui's index is an insertion position before removal. Moving an
      // item forward therefore needs one extra slot to land at desired_index.
      const size_t wire_index =
          desired_index > current_index ? desired_index + 1 : desired_index;
      workspace_mutation_in_flight_ = true;
      tui_client_->MoveWorkspace(
          key, wire_index,
          WorkspaceMutationForRetry(pending_move.mutation_id, revision),
          base::BindOnce(&CmuxWindowView::OnWorkspaceMoveFinished,
                         weak_factory_.GetWeakPtr(), key, desired_index,
                         revision));
    }
  }

  void OnWorkspaceMutationFinished(uint64_t attempted_revision,
                                   CmuxTuiCommandResult result) {
    workspace_mutation_in_flight_ = false;
    if (!WorkspaceMutationMatchesCurrentRegistry(result)) {
      VLOG(1) << "cmux-views: ignored workspace mutation response from a "
                 "superseded registry generation";
    } else if (!result.ok) {
      workspace_mutation_failed_revision_ = attempted_revision;
      ++workspace_mutation_failure_count_;
      if (workspace_mutation_failure_count_ >=
          kMaxWorkspaceMutationFailures) {
        LOG(ERROR) << "cmux-views: workspace mutation stopped after "
                   << workspace_mutation_failure_count_
                   << " failures at registry revision " << attempted_revision
                   << ": " << result.error;
      } else {
        VLOG(1) << "cmux-views: workspace mutation will reconcile after "
                << result.error;
      }
    } else {
      ResetWorkspaceMutationFailureBudget();
    }
    tui_client_->RefreshWorkspaceRegistry();
  }

  void OnWorkspaceMoveFinished(const std::string& key,
                               size_t desired_index,
                               uint64_t attempted_revision,
                               CmuxTuiCommandResult result) {
    if (result.ok && WorkspaceMutationMatchesCurrentRegistry(result) &&
        pending_workspace_mutations_.move() &&
        pending_workspace_mutations_.move()->key == key &&
        pending_workspace_mutations_.move()->desired_index == desired_index) {
      pending_workspace_mutations_.ForgetMove();
      CompleteQueuedWorkspaceMove(key, desired_index);
    }
    OnWorkspaceMutationFinished(attempted_revision, std::move(result));
  }

  void ResetWorkspaceMutationFailureBudget() {
    workspace_mutation_failure_count_ = 0;
    workspace_mutation_failed_revision_ = 0;
  }

  bool WorkspaceMutationMatchesCurrentRegistry(
      const CmuxTuiCommandResult& result) const {
    const std::string* registry_id = result.data.FindString("registry_id");
    if (registry_id && !workspace_registry_id_.empty() &&
        *registry_id != workspace_registry_id_) {
      return false;
    }
    const std::string* generation = result.data.FindString("generation");
    return !generation || workspace_registry_generation_.empty() ||
           *generation == workspace_registry_generation_;
  }

  void CompleteQueuedWorkspaceMove(const std::string& key,
                                   size_t desired_index) {
    if (!queued_workspace_moves_.empty() &&
        queued_workspace_moves_.front().key == key &&
        queued_workspace_moves_.front().desired_index == desired_index) {
      queued_workspace_moves_.erase(queued_workspace_moves_.begin());
    }
  }

  void StageNextQueuedWorkspaceMove() {
    if (pending_workspace_mutations_.move() ||
        queued_workspace_moves_.empty()) {
      return;
    }
    const CmuxPendingWorkspaceMove& next = queued_workspace_moves_.front();
    pending_workspace_mutations_.SetMove(
        next.key, next.desired_index, next.mutation_id);
  }

  void RequestWorkspaceOrder() {
    ResetWorkspaceMutationFailureBudget();
    queued_workspace_moves_.clear();
    pending_workspace_mutations_.ForgetMove();
    // Derive the complete root order, not only the dragged block: a rightward
    // block otherwise shifts an earlier moved member, and BuildRail() would
    // omit every member of a collapsed group.
    std::vector<std::string> current;
    if (workspace_snapshot_) {
      for (const CmuxTuiWorkspace& workspace :
           workspace_snapshot_->workspaces) {
        current.push_back(workspace.key);
      }
    }
    std::vector<std::string> desired;
    const std::vector<WorkspaceId>& roots = model_.roots();
    desired.reserve(roots.size());
    for (WorkspaceId root : roots) {
      const Workspace* moved = model_.GetWorkspace(root);
      if (moved && !moved->registry_key.empty()) {
        desired.push_back(moved->registry_key);
      }
    }
    for (const auto& [key, desired_index] :
         BuildCmuxWorkspaceOrderMoves(current, desired)) {
      queued_workspace_moves_.push_back(
          {key, desired_index, NewWorkspaceMutationId()});
    }
    StageNextQueuedWorkspaceMove();
    SyncWorkspaceRegistry();
  }

  void SwitchWorkspace(WorkspaceId id, bool move_keyboard = true) {
    if (!model_.GetWorkspace(id)) {
      return;
    }
    model_.SelectWorkspace(id);
    MarkWorkspaceProjectionDirty();
    ActivateWorkspace(id, move_keyboard);
  }

  // Modifier selection already updates WindowModel's active workspace. Show
  // that workspace without calling SelectWorkspace(), which would collapse the
  // multi-selection back to a singleton.
  void ActivateModelSelectedWorkspace() {
    ActivateWorkspace(model_.selected_workspace());
  }

  void ActivateWorkspace(WorkspaceId id, bool move_keyboard = true) {
    const Workspace* activating_workspace = model_.GetWorkspace(id);
    if (!activating_workspace) {
      return;
    }
    if (activating_workspace->group != kInvalidId) {
      most_recent_workspace_group_ = activating_workspace->group;
    }
    if (move_keyboard) {
      // A keyboard-moving workspace switch supersedes a not-yet-consumed TUI
      // responder handoff. An exact-terminal TUI transaction is the sole
      // caller that switches without moving the keyboard.
      ClearPendingTuiTerminalFocus();
    }
    Browser* old_browser = BrowserForWorkspace(ws_);
    const bool widget_active = GetWidget() && GetWidget()->IsActive();
    if (widget_active && old_browser) {
      old_browser->DidBecomeInactive();
    }
    ws_ = id;
    MarkWorkspaceProjectionDirty();
    pending_entrance_panes_.clear();
    const std::optional<std::pair<PaneId, SurfaceTabId>> fresh_web_tab =
        EnsureActivePanesRealized();
    RefreshRail();
    if (const Workspace* workspace = model_.GetWorkspace(ws_)) {
      ActivateSelectedWebTabInPane(workspace->focused);
    }
    if (widget_active) {
      if (Browser* browser = BrowserForWorkspace(ws_)) {
        browser->DidBecomeActive();
      }
    }
    // Re-establish the invariant for the now-active workspace. TUI selection
    // is a two-stage transaction: it first switches workspaces without
    // landing on the previous surface, then installs an exact terminal focus
    // intent after selecting its pane/tab.
    if (move_keyboard) {
      focus_.OnWorkspaceActivated();
    } else if (const Workspace* workspace = model_.GetWorkspace(ws_)) {
      focus_.FocusPane(workspace->focused, /*move_keyboard=*/false);
    }
    if (fresh_web_tab && move_keyboard) {
      FocusNewWebTab(fresh_web_tab->first, fresh_web_tab->second);
    }
  }

  // A seeded/new workspace has model panes but no views yet; give each a
  // default web column the first time the workspace is shown.
  std::optional<std::pair<PaneId, SurfaceTabId>> EnsureActivePanesRealized() {
    std::optional<std::pair<PaneId, SurfaceTabId>> focused_web_tab;
    const Workspace* workspace = model_.GetWorkspace(ws_);
    for (PaneId p : model_.PanesOf(ws_)) {
      if (!views_.count(p)) {
        RealizePane(p, GURL());
        Pane* pane = model_.FindPane(ws_, p);
        SurfaceTab* tab = pane ? pane->FindTab(pane->selected) : nullptr;
        if (workspace && workspace->focused == p && tab &&
            tab->kind == SurfaceKind::kWeb) {
          focused_web_tab = std::make_pair(p, tab->id);
        }
      }
    }
    return focused_web_tab;
  }

  void RefreshRail() {
    if (rail_) {
      std::map<WorkspaceId, gfx::ImageSkia> icons;
      for (const Workspace& workspace : model_.workspaces()) {
        if (CmuxPaneView* pane = ViewForPane(workspace.focused)) {
          gfx::ImageSkia favicon = pane->selected_favicon();
          if (!favicon.isNull()) {
            icons.emplace(workspace.id, std::move(favicon));
          }
        }
      }
      rail_favicon_snapshots_ = icons;
      rail_->Update(model_, icons);
    }
  }

  void ShowWorkspaceGroupEditorBubbleDeferred(WorkspaceGroupId group) {
    if (group == kInvalidId) {
      return;
    }
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE,
        base::BindOnce(
            [](base::WeakPtr<CmuxWindowView> self, WorkspaceGroupId group) {
              if (self && self->rail_ &&
                  self->model_.GetWorkspaceGroup(group)) {
                self->rail_->ShowGroupEditorBubble(group);
              }
            },
            weak_factory_.GetWeakPtr(), group));
  }

  void RefreshRailIfFaviconChanged(WorkspaceId workspace,
                                   PaneId pane,
                                   const CmuxPaneView& pane_view) {
    const Workspace* workspace_model = model_.GetWorkspace(workspace);
    if (!workspace_model || workspace_model->focused != pane) {
      return;
    }
    const gfx::ImageSkia favicon = pane_view.selected_favicon();
    const auto snapshot = rail_favicon_snapshots_.find(workspace);
    const bool changed =
        favicon.isNull() ? snapshot != rail_favicon_snapshots_.end()
                         : snapshot == rail_favicon_snapshots_.end() ||
                               !favicon.BackedBySameObjectAs(snapshot->second);
    if (changed) {
      RefreshRail();
    }
  }

  raw_ptr<Profile> profile_ = nullptr;
  const NativeWindowId native_window_;
  const std::string window_group_;
  raw_ptr<CmuxNativeWindowRegistry> native_window_registry_ = nullptr;
  scoped_refptr<CmuxTuiClient> tui_client_;
  bool observing_tui_client_ = false;
  std::optional<CmuxTuiWorkspaceSnapshot> workspace_snapshot_;
  // Owner-mux selection is an event-like cross-frontend intent, not durable
  // GUI presentation authority. Apply each distinct TUI selection once so an
  // unrelated registry refresh cannot snap a user back from a local web tab.
  std::optional<std::pair<std::string, std::string>>
      last_applied_tui_selection_;
  // The canonical selection is intentionally stable while its disposable
  // Chromium tab can be removed and recreated. Including the last local tab
  // in dedupe makes rematerialization re-run the handoff without letting an
  // unrelated registry refresh override a user's local GUI selection.
  std::optional<SurfaceTabId> last_applied_tui_terminal_tab_;
  // TUI selection may arrive while this native window is inactive. Preserve
  // canonical identity, resolve its disposable local tab only at consumption,
  // and let any intervening GUI interaction cancel the deferred grab.
  std::optional<TuiTerminalFocusIntent> pending_tui_terminal_focus_;
  uint64_t pending_tui_terminal_focus_generation_ = 0;
  // A tree invalidation clears local responder authority but may retry the
  // same canonical intent once the owner-mux snapshot is authoritative again.
  std::optional<std::pair<std::string, std::string>>
      retry_tui_terminal_focus_selection_;
  // A direct GUI interaction cancels both queued and future-rematerialized
  // focus for exactly this owner-mux selection. A distinct TUI selection
  // clears the fence and is eligible to focus normally.
  std::optional<std::pair<std::string, std::string>>
      suppressed_tui_terminal_focus_selection_;
  std::string workspace_registry_id_;
  std::string workspace_registry_generation_;
  std::optional<CmuxWorkspaceProjection> loaded_workspace_projection_;
  uint64_t workspace_projection_revision_ = 0;
  uint64_t workspace_projection_epoch_ = 1;
  uint64_t workspace_projection_local_generation_ = 0;
  CmuxWorkspaceProjectionReloadBarrier workspace_projection_reload_barrier_;
  bool workspace_projection_loaded_ = false;
  bool workspace_projection_supported_ = true;
  bool workspace_projection_get_in_flight_ = false;
  bool workspace_projection_put_in_flight_ = false;
  bool workspace_projection_dirty_ = false;
  bool applying_workspace_projection_ = false;
  int workspace_projection_retry_count_ = 0;
  CmuxPendingWorkspaceMutations pending_workspace_mutations_;
  // Canonical workspace moves are single-item CAS operations. A group or
  // multi-selection drag is therefore serialized as stable keyed moves in
  // final rail order, while the frontend projection preserves the complete
  // local block between authoritative snapshots.
  std::vector<CmuxPendingWorkspaceMove> queued_workspace_moves_;
  std::set<WorkspaceId> tui_originated_workspace_closes_;
  bool workspace_mutation_in_flight_ = false;
  static constexpr int kMaxWorkspaceMutationFailures = 3;
  int workspace_mutation_failure_count_ = 0;
  uint64_t workspace_mutation_failed_revision_ = 0;
  bool registry_bootstrapped_ = false;
  size_t pending_terminal_columns_ = 0;
  bool pending_startup_tab_ = false;
  std::optional<GURL> pending_startup_url_;
  CmuxCanonicalTerminalPlacementModel terminal_placement_model_;
  std::map<SurfaceTabId, scoped_refptr<CmuxTerminalBackend>> terminal_backends_;
  std::map<std::string, SurfaceTabId> terminal_tab_by_id_;
  std::map<std::string, CmuxPendingTerminalClose> pending_terminal_closes_;
  std::set<std::string> terminal_close_requests_in_flight_;
  std::set<std::string> terminal_closes_awaiting_absence_;
  base::OneShotTimer terminal_close_retry_timer_;
  bool terminal_reconcile_waiting_for_workspace_ = false;
  bool terminal_registry_replaced_pending_ = false;
  bool terminal_generation_change_pending_ = false;
  WindowModel model_;
  LayoutConfig layout_config_;
  std::string new_tab_page_ = kBrowserDefaultNewTabPage;
  Keymap keymap_;
  std::vector<KeyChord> pending_chord_;
  base::OneShotTimer pending_chord_timer_;
  std::optional<base::SequenceBound<base::FilePathWatcher>>
      keymap_file_watcher_;
  base::OneShotTimer keymap_reload_timer_;
  int applied_ui_font_size_ = -1;
  RailConfig base_rail_config_;
  std::optional<CmuxThemePalette> active_theme_palette_;
  // Theme name captured at the same moment as active_theme_palette_. The
  // published layout config changes before its asynchronous Ghostty load
  // finishes, so reporting layout_config_.ghostty_theme_name here would label
  // the previous palette as the newly selected theme.
  std::string active_theme_name_;
  std::optional<CmuxChromeSurfaceColors> chrome_surface_colors_;
  SkColor window_bg_ = kDefaultPaneStripBackground;
  SkColor content_bg_ = kDefaultPaneStripBackground;
  TabStripThemeColors tab_theme_colors_;
  PaneThemeColors pane_theme_colors_;
  WorkspaceId ws_ = kInvalidId;             // the active (visible) workspace
  WorkspaceGroupId most_recent_workspace_group_ = kInvalidId;
  raw_ptr<CmuxRail> rail_ = nullptr;        // configurable vertical-tab tree
  WorkspaceId workspace_context_menu_native_workspace_ = kInvalidId;
  std::unique_ptr<WorkspaceContextMenuNativeItems>
      workspace_context_menu_native_items_;
  raw_ptr<views::View> content_ = nullptr;  // clipped pane area beside rail
  raw_ptr<WindowControlsOcclusionView> window_controls_occlusion_ = nullptr;
  raw_ptr<RailResizeHandle> rail_resize_handle_ = nullptr;
  raw_ptr<WorkspaceResizeHandle> column_resize_handle_ = nullptr;
  std::map<SplitId, raw_ptr<WorkspaceResizeHandle>> split_resize_handles_;
  // PaneId -> pane view (all workspaces).
  std::map<PaneId, raw_ptr<CmuxPaneView>> views_;
  // Pane bounds with niri scroll removed. Combining this logical x with the
  // content view's live animated x keeps tab/traffic-light spacing smooth.
  std::map<PaneId, gfx::Rect> pane_titlebar_geometry_;
  std::map<WorkspaceId, double> scroll_by_ws_;  // per-workspace niri scroll
  // Real Chromium logical windows. BrowserManagerService owns each Browser;
  // the map is the workspace association and is cleared before asynchronous
  // Browser destruction.
  // Browser deletion is owned and scheduled by Chromium. In particular,
  // chrome.windows.remove() can destroy the Browser before our deferred cmux
  // view/model teardown task runs, so this registry must never retain a raw
  // dangling Browser pointer.
  std::map<WorkspaceId, base::WeakPtr<Browser>> workspace_browsers_;
  std::map<WorkspaceId, std::unique_ptr<SidePanelUI>>
      workspace_side_panel_uis_;
  std::map<content::WebContents*, WebTabPlacement> web_tab_placements_;
  std::map<SurfaceTabId, raw_ptr<content::WebContents>>
      web_contents_by_surface_;
  int internal_browser_mutation_depth_ = 0;
  // Non-invalid from the focused web-tab close request through TabStripModel
  // removal or beforeunload cancellation. See
  // OnWorkspaceBrowserActiveTabChanged.
  WorkspaceId focused_web_tab_close_workspace_ = kInvalidId;
  PaneId focused_web_tab_close_pane_ = kInvalidId;
  SurfaceTabId focused_web_tab_close_surface_ = kInvalidId;
  std::unique_ptr<PendingFocusedWebTabCloseObserver>
      focused_web_tab_close_observer_;
  size_t pending_focused_tab_closes_ = 0;
  bool focused_tab_close_task_pending_ = false;
  std::set<WorkspaceId> closing_workspaces_;
  std::set<WorkspaceId> pending_workspace_closes_;
  std::set<WorkspaceId> suppress_empty_browser_close_once_;
  // Avoid rebuilding every rail row for pane refreshes that only changed a
  // title/loading indicator; selected-favicon identity changes still update.
  std::map<WorkspaceId, gfx::ImageSkia> rail_favicon_snapshots_;
  views::UnhandledKeyboardEventHandler unhandled_keyboard_event_handler_;
  std::map<PaneId, PaneEntrance> pending_entrance_panes_;
  std::set<PaneId> deferred_tab_refreshes_;
  std::unique_ptr<views::BoundsAnimator> animator_;
  std::unique_ptr<views::BoundsAnimator> root_animator_;
  std::unique_ptr<CmuxTabDragController> drag_controller_;
  std::unique_ptr<StripWheelEventHandler> strip_wheel_handler_;
  std::unique_ptr<TuiFocusInteractionEventHandler>
      tui_focus_interaction_handler_;
  int strip_width_override_ = -1;
  bool have_sidebar_layout_target_ = false;
  gfx::Rect last_content_layout_target_;
  gfx::Rect last_rail_layout_target_;
  gfx::Rect last_rail_resize_handle_layout_target_;
  std::optional<int> rail_resize_saved_expanded_width_;
  int applied_rail_header_height_ = -1;
  SidebarPosition applied_sidebar_position_ = SidebarPosition::kLeft;
  bool applied_animations_ = true;
  int applied_animation_ms_ = -1;
  int applied_focus_border_ = -1;
  SkColor applied_accent_color_ = SK_ColorTRANSPARENT;
  SkColor applied_drop_highlight_color_ = SK_ColorTRANSPARENT;
  SkColor applied_focus_color_ = SK_ColorTRANSPARENT;
  std::optional<SidebarMode> applied_sidebar_mode_;
  std::deque<SurfaceTabId> pending_new_tab_theme_reloads_;
  base::OneShotTimer new_tab_theme_reload_timer_;
  int pending_new_tab_theme_generation_ = 0;
  int theme_generation_ = 0;
  int applied_theme_generation_ = -1;
  // Every startup/reload operation receives a generation before its layout
  // read begins. Replies from superseded layout, resource, and Ghostty config
  // stages are ignored so an older failure cannot clear a newer theme.
  uint64_t ghostty_theme_load_request_ = 0;
  // ProfileObserver fires before keyed services shut down. Theme replies may
  // still own UI tasks after that point even though this view remains alive.
  bool profile_services_available_ = true;
  PrefChangeRegistrar rounded_frame_pref_change_registrar_;
  base::ScopedObservation<Profile, ProfileObserver> profile_observation_{this};
  // The single focus authority; drives this view through the FocusHost
  // methods.
  FocusController focus_{this};
  // CMUX_ANIM_SELFTEST state (event-driven; see RunAnimSelfTest).
  bool anim_selftest_active_ = false;
  bool anim_selftest_mid_checked_ = false;
  PaneId anim_selftest_pane_ = kInvalidId;
  int anim_selftest_start_x_ = 0;
  int anim_selftest_final_x_ = 0;
  bool sidebar_selftest_active_ = false;
  bool sidebar_selftest_mid_checked_ = false;
  int sidebar_selftest_transitions_ = 0;
  SidebarMode sidebar_selftest_initial_mode_ = SidebarMode::kExpanded;
  gfx::Rect sidebar_selftest_start_bounds_;
  gfx::Rect sidebar_selftest_target_bounds_;
  bool pending_chrome_surface_selftest_ = false;
#if CHROME_VERSION_MAJOR >= 150
  base::ScopedObservation<GlobalBrowserCollection, BrowserCollectionObserver>
      browser_collection_observation_{this};
#endif
  base::WeakPtrFactory<CmuxWindowView> weak_factory_{this};
};

BEGIN_METADATA(CmuxWindowView)
END_METADATA

BEGIN_METADATA(WindowControlsOcclusionView)
END_METADATA

BEGIN_METADATA(WorkspaceResizeHandle)
END_METADATA

bool WorkspaceResizeHandle::OnMousePressed(const ui::MouseEvent& event) {
  if (!event.IsOnlyLeftMouseButton() || !owner_) {
    return false;
  }
  press_screen_ = ScreenPoint(event);
  start_value_ = value_;
  start_resize_span_ = std::max(1.0, resize_span_);
  return true;
}

bool WorkspaceResizeHandle::OnMouseDragged(const ui::MouseEvent& event) {
  if (!event.IsLeftMouseButton() || !owner_) {
    return false;
  }
  const gfx::Point screen = ScreenPoint(event);
  const int delta = target_ == Target::kColumn ||
                            orientation_ == SplitOrientation::kHorizontal
                        ? screen.x() - press_screen_.x()
                        : screen.y() - press_screen_.y();
  const double next = start_value_ + delta / start_resize_span_;
  if (target_ == Target::kColumn) {
    owner_->ResizeColumnToFraction(pane_, next);
  } else {
    owner_->ResizeSplitToRatio(split_, next);
  }
  return true;
}

void WorkspaceResizeHandle::OnMouseReleased(const ui::MouseEvent& event) {
  if (!owner_) {
    return;
  }
  const gfx::Point screen = ScreenPoint(event);
  const int delta = target_ == Target::kColumn ||
                            orientation_ == SplitOrientation::kHorizontal
                        ? screen.x() - press_screen_.x()
                        : screen.y() - press_screen_.y();
  const double next = start_value_ + delta / start_resize_span_;
  if (target_ == Target::kColumn) {
    owner_->ResizeColumnToFraction(pane_, next);
  } else {
    owner_->ResizeSplitToRatio(split_, next);
  }
}

void WorkspaceResizeHandle::OnMouseCaptureLost() {}

BEGIN_METADATA(RailResizeHandle)
END_METADATA

bool RailResizeHandle::OnMousePressed(const ui::MouseEvent& event) {
  if (!event.IsOnlyLeftMouseButton() || !owner_) {
    return false;
  }
  press_screen_ = event.location();
  ConvertPointToScreen(this, &press_screen_);
  start_width_ = owner_->BeginRailResize();
  resized_ = false;
  return true;
}

bool RailResizeHandle::OnMouseDragged(const ui::MouseEvent& event) {
  if (!event.IsLeftMouseButton() || !owner_) {
    return false;
  }
  resized_ = true;
  owner_->ResizeRailToProposedWidth(
      owner_->RailWidthForScreenDelta(start_width_, ScreenDeltaX(event)));
  return true;
}

void RailResizeHandle::OnMouseReleased(const ui::MouseEvent& event) {
  if (!owner_) {
    return;
  }
  if (!resized_) {
    owner_->CancelRailResize();
    return;
  }
  owner_->FinishRailResize(
      owner_->RailWidthForScreenDelta(start_width_, ScreenDeltaX(event)));
}

void RailResizeHandle::OnMouseCaptureLost() {
  if (owner_) {
    owner_->FinishRailResize(start_width_);
  }
}

// A top-level Widget that, unlike a plain views::Widget, exposes a
// ThemeProvider. Our shared widget has no BrowserFrame, so the default
// Widget::GetThemeProvider() returns null. That null makes
// ToolbarButton::SetVectorIcons() skip its UpdateIcon() repaint, so runtime
// icon swaps (the reload<->stop(X) flip) never become visible even though the
// load state and ChangeMode() fire correctly. Source the profile's provider
// the same way other non-BrowserFrame Chrome widgets do.
class CmuxWidget : public views::Widget {
 public:
  explicit CmuxWidget(Profile* profile) : profile_(profile) {}
  const ui::ThemeProvider* GetThemeProvider() const override {
    return &ThemeService::GetThemeProviderForProfile(profile_);
  }

 private:
  raw_ptr<Profile> profile_;
};

struct NativeCmuxWindow {
  NativeWindowId id = kInvalidNativeWindowId;
  raw_ptr<views::Widget> widget = nullptr;
  raw_ptr<CmuxWindowView> window = nullptr;
};

// Compatibility aliases for helpers which target the currently active cmux
// container. Ownership lives in NativeWindows().
views::Widget* g_views_widget = nullptr;
CmuxStripController* g_strip = nullptr;
CmuxWindowView* g_window = nullptr;

CmuxNativeWindowRegistry& NativeWindowRegistry() {
  static base::NoDestructor<CmuxNativeWindowRegistry> registry;
  return *registry;
}

std::map<views::Widget*, NativeCmuxWindow>& NativeWindows() {
  static base::NoDestructor<std::map<views::Widget*, NativeCmuxWindow>> windows;
  return *windows;
}

NativeCmuxWindow* ActiveNativeWindow() {
  for (auto& [widget, record] : NativeWindows()) {
    if (widget && widget->IsActive()) {
      return &record;
    }
  }
  if (NativeWindows().empty()) {
    return nullptr;
  }
  return &NativeWindows().rbegin()->second;
}
// Keeps the browser process alive while the niri window (a plain
// views::Widget, not a Browser) is up; released with the window so shutdown can
// complete (KeepAliveRegistry must drain, and mac's applicationWillTerminate
// CHECKs no BROWSER keep-alive remains).
ScopedKeepAlive* g_keep_alive = nullptr;

void ReleaseViewsKeepAlive() {
  delete g_keep_alive;
  g_keep_alive = nullptr;
}

void ClearViewsWindowStateForWidget(views::Widget* widget) {
  auto it = NativeWindows().find(widget);
  if (it == NativeWindows().end()) {
    return;
  }
  NativeWindowRegistry().UnregisterWindow(it->second.id);
  NativeWindows().erase(it);
  if (g_views_widget == widget) {
    NativeCmuxWindow* replacement = ActiveNativeWindow();
    g_views_widget = replacement ? replacement->widget.get() : nullptr;
    g_window = replacement ? replacement->window.get() : nullptr;
    g_strip = g_window;
  }
#if BUILDFLAG(IS_MAC)
  RemoveKeyEventMonitor(widget);
#endif
  if (NativeWindows().empty()) {
    CloseCmuxExtensionsUi();
    // The window may close before the first canonical snapshot creates a
    // workspace Browser. Retire the still-hidden bootstrap Browser and its
    // observer before dropping the final process bridge in that path too.
    FinishStartupBrowserSuppression();
    ReleaseViewsKeepAlive();
  }
}

class CmuxWidgetDestroyObserver : public views::WidgetObserver {
 public:
  void Observe(views::Widget* widget) {
    if (widget && observed_.insert(widget).second) {
      widget->AddObserver(this);
    }
  }

  void StopObserving(views::Widget* widget) {
    if (!observed_.erase(widget)) {
      return;
    }
    widget->RemoveObserver(this);
  }

  void OnWidgetDestroying(views::Widget* widget) override {
    if (!observed_.erase(widget)) {
      return;
    }
    widget->RemoveObserver(this);
    ClearViewsWindowStateForWidget(widget);
    // The root view has not been destroyed yet. Its extension strips, action
    // models, and menu buttons still reference their Browser's container.
    // CmuxWindowView releases each container after destroying those children.
  }

  void OnWidgetActivationChanged(views::Widget* widget, bool active) override {
    auto it = NativeWindows().find(widget);
    if (active && it != NativeWindows().end()) {
      g_views_widget = widget;
      g_window = it->second.window;
      g_strip = g_window;
      NativeWindowRegistry().ActivateWindow(it->second.id);
      RegisterCmuxBrowserWindowFactoryHost(
          it->second.window->BrowserWindowHostWeakPtr());
    }
  }

 private:
  std::set<raw_ptr<views::Widget>> observed_;
};

CmuxWidgetDestroyObserver& GetWidgetDestroyObserver() {
  static base::NoDestructor<CmuxWidgetDestroyObserver> observer;
  return *observer;
}

// Suppresses Chromium's ordinary bootstrap BrowserView during the short gap
// between Chrome startup and construction of the real cmux workspace
// Browsers. Once the workspace Browsers exist, FinishStartupSuppression()
// closes the bootstrap window and permanently unregisters this observer.
class CmuxBrowserCloser :
#if CHROME_VERSION_MAJOR >= 150
    public BrowserCollectionObserver,
#else
    public BrowserListObserver,
#endif
    public views::WidgetObserver {
 public:
#if CHROME_VERSION_MAJOR >= 150
  void OnBrowserCreated(BrowserWindowInterface* browser_window) override {
    Browser* browser = browser_window->GetBrowserForMigrationOnly();
#else
  void OnBrowserAdded(Browser* browser) override {
#endif
    ObserveBrowser(browser);
    // BrowserList notification runs before the startup window gets its first
    // compositor frame. Hide it synchronously: posting this by one UI task
    // allowed a stock Chromium frame to flash before cmux replaced it.
    HideBrowser(browser);
  }

#if CHROME_VERSION_MAJOR >= 150
  void OnBrowserClosed(BrowserWindowInterface* browser_window) override {
    Browser* browser = browser_window->GetBrowserForMigrationOnly();
#else
  void OnBrowserRemoved(Browser* browser) override {
#endif
    StopObservingBrowser(browser);
  }

  void StartObservingBrowsers() {
#if CHROME_VERSION_MAJOR >= 150
    browser_collection_observation_.Observe(
        GlobalBrowserCollection::GetInstance());
#else
    BrowserList::AddObserver(this);
#endif
  }

  void StopObservingBrowsers() {
#if CHROME_VERSION_MAJOR >= 150
    browser_collection_observation_.Reset();
#else
    BrowserList::RemoveObserver(this);
#endif
  }

  void OnWidgetVisibilityChanged(views::Widget* widget, bool visible) override {
    if (!visible) {
      return;
    }
    Browser* browser = BrowserForWidget(widget);
    if (!browser) {
      return;
    }
    LOG(WARNING) << "cmux: re-hid suppressed browser window";
    // Browser::Show can run after OnBrowserAdded. Suppress that visibility
    // transition in the observer callback itself, before compositing returns
    // to the run loop.
    HideBrowser(browser);
  }

  void OnWidgetDestroying(views::Widget* widget) override {
    for (auto it = widget_observations_.begin();
         it != widget_observations_.end();) {
      if (it->second == widget) {
        it = widget_observations_.erase(it);
      } else {
        ++it;
      }
    }
    widget->RemoveObserver(this);
  }

  void FinishStartupSuppression() {
    std::vector<base::WeakPtr<Browser>> startup_browsers;
    for (const auto& [browser, widget] : widget_observations_) {
      if (browser && browser->window() && browser->window()->AsBrowserView()) {
        startup_browsers.push_back(browser->AsWeakPtr());
      }
    }
    for (auto it = widget_observations_.begin();
         it != widget_observations_.end();) {
      it->second->RemoveObserver(this);
      it = widget_observations_.erase(it);
    }
    for (base::WeakPtr<Browser> browser : startup_browsers) {
      if (browser && browser->window()) {
        browser->window()->Close();
      }
    }
  }

  static views::Widget* GetBrowserWidget(Browser* browser) {
    if (!browser || !browser->window()) {
      return nullptr;
    }
    gfx::NativeWindow native_window = browser->window()->GetNativeWindow();
    return native_window
               ? views::Widget::GetWidgetForNativeWindow(native_window)
               : nullptr;
  }

 private:
  void ObserveBrowser(Browser* browser) {
    // GlobalBrowserCollection (M150+) also reports cmux's workspace-backed
    // Browsers. They share the cmux widget and deliberately have no
    // BrowserView; observing or hiding one here hides the real cmux window.
    // This suppressor owns only Chromium's temporary BrowserView bootstrap.
    if (!browser || !browser->window() || !browser->window()->AsBrowserView() ||
        widget_observations_.contains(browser)) {
      return;
    }
    views::Widget* widget = GetBrowserWidget(browser);
    if (!widget) {
      return;
    }
    widget->AddObserver(this);
    widget_observations_[browser] = widget;
  }

  void StopObservingBrowser(Browser* browser) {
    auto it = widget_observations_.find(browser);
    if (it == widget_observations_.end()) {
      return;
    }
    it->second->RemoveObserver(this);
    widget_observations_.erase(it);
  }

  Browser* BrowserForWidget(views::Widget* widget) const {
    for (const auto& [browser, observed_widget] : widget_observations_) {
      if (observed_widget == widget) {
        return browser;
      }
    }
    return nullptr;
  }

  static void HideBrowser(Browser* browser) {
    if (browser && browser->window() && browser->window()->AsBrowserView()) {
      browser->window()->Hide();
    }
  }

  std::map<Browser*, raw_ptr<views::Widget>> widget_observations_;
#if CHROME_VERSION_MAJOR >= 150
  base::ScopedObservation<GlobalBrowserCollection, BrowserCollectionObserver>
      browser_collection_observation_{this};
#endif
};

CmuxBrowserCloser* GetCmuxBrowserCloser() {
  static base::NoDestructor<CmuxBrowserCloser> closer;
  return closer.get();
}

// Rewrite cmux:// URLs to their chrome:// equivalent (cmux://extensions ->
// chrome://extensions), the same mechanism upstream uses to map about: to
// chrome:. Registered as a BrowserURLHandler, so the rewrite happens at
// navigation-entry creation: the WebUI loads from the chrome:// URL while the
// typed cmux:// stays as the entry's virtual (displayed) URL. Safe against
// web-page abuse for the same reason about: is -- a web renderer can never
// commit a chrome:// URL regardless of how it was produced. The omnibox half
// (classifying typed cmux:// input as a URL, not a search) is the
// IsHandledProtocol patch in apply.sh.
bool RewriteCmuxURLToChromeURL(GURL* url, content::BrowserContext*) {
  if (!url->SchemeIs("cmux")) {
    return false;
  }
  GURL::Replacements to_chrome;
  to_chrome.SetSchemeStr(content::kChromeUIScheme);
  // Only the settings root is cmux-owned; configure remains a compatibility
  // alias. Settings subroutes retain their host so cmux://settings/payments,
  // for example, becomes the native chrome://settings/payments page.
  // Other hosts retain the established cmux://foo -> chrome://foo aliases.
  const std::string_view path = url->path();
  const bool is_cmux_configure =
      url->host() == "configure" ||
      (url->host() == "settings" && (path.empty() || path == "/"));
  if (is_cmux_configure) {
    to_chrome.SetHostStr("cmux-configure");
  }
  GURL rewritten = url->ReplaceComponents(to_chrome);
  if (!rewritten.is_valid()) {
    return false;
  }
  *url = rewritten;
  return true;
}

}  // namespace

void SuppressStartupBrowsers() {
  // Close Chrome's normal Browser windows as they're added (the startup window
  // is created after this observer registers, so OnBrowserAdded catches it).
  static bool installed = false;
  if (installed) {
    return;
  }
  GetCmuxBrowserCloser()->StartObservingBrowsers();
  installed = true;
}

void FinishStartupBrowserSuppression() {
  static bool finished = false;
  if (finished) {
    return;
  }
  GetCmuxBrowserCloser()->StopObservingBrowsers();
  GetCmuxBrowserCloser()->FinishStartupSuppression();
  finished = true;
}

void HoldViewsKeepAlive() {
  // Bridges the short interval between Chrome startup and construction of the
  // real workspace Browsers. It is released as soon as those Browsers exist.
  if (!g_keep_alive) {
    g_keep_alive = new ScopedKeepAlive(KeepAliveOrigin::BROWSER,
                                       KeepAliveRestartOption::DISABLED);
  }
}

CmuxStripController* GetStripController() {
  return g_strip;
}

std::optional<KeyChord> KeyChordFromNativeWebKeyboardEvent(
    const input::NativeWebKeyboardEvent& event) {
  return ChordFromAccelerator(
      ui::GetAcceleratorFromNativeWebKeyboardEvent(event));
}

content::WebContents* GetActiveCmuxWebContents() {
  return g_window ? g_window->ActiveWebContentsForExtensions() : nullptr;
}

views::Widget* GetCmuxViewsWidget() {
  return g_views_widget;
}

bool IsAnyCmuxWindowActive() {
  for (const auto& entry : NativeWindows()) {
    views::Widget* widget = entry.first;
    if (widget && widget->IsActive()) {
      return true;
    }
  }
  return false;
}

namespace {

#if BUILDFLAG(IS_MAC) || BUILDFLAG(IS_LINUX)
struct CustomizationThemeReloadState {
  uint64_t generation = 0;
  base::OneShotTimer debounce_timer;
};

CustomizationThemeReloadState& ThemeReloadState() {
  static base::NoDestructor<CustomizationThemeReloadState> state;
  return *state;
}

void FinishCustomizationThemeReload(uint64_t generation,
                                    LayoutConfig config,
                                    std::optional<CmuxTheme> theme) {
  if (generation != ThemeReloadState().generation) {
    return;
  }
  for (const auto& [widget, record] : NativeWindows()) {
    if (widget && record.window) {
      record.window->ApplyResolvedPublishedCustomization(config, theme);
    }
  }
#if BUILDFLAG(IS_MAC)
  if (config.ghostty_theme && theme) {
    ApplyCmuxGhosttyThemeToAllTerminalViews(*theme);
  }
#endif
}

void OnCustomizationThemeResourcesPrepared(uint64_t generation,
                                           LayoutConfig config,
                                           bool resources_ready) {
  if (generation != ThemeReloadState().generation) {
    return;
  }
  if (!resources_ready || !EnsureGhosttyCoreInit()) {
    FinishCustomizationThemeReload(generation, std::move(config),
                                   std::nullopt);
    return;
  }
  if (!GhosttyThemeTaskRunner()->PostTaskAndReplyWithResult(
          FROM_HERE,
          base::BindOnce(&LoadGhosttyTheme, config.ghostty_theme_name),
          base::BindOnce(&FinishCustomizationThemeReload, generation,
                         config))) {
    FinishCustomizationThemeReload(generation, std::move(config),
                                   std::nullopt);
  }
}

void BeginCustomizationThemeReload(uint64_t generation, LayoutConfig config) {
  if (generation != ThemeReloadState().generation) {
    return;
  }
  if (!config.ghostty_theme) {
    FinishCustomizationThemeReload(generation, std::move(config),
                                   std::nullopt);
    return;
  }
  if (!GhosttyThemeTaskRunner()->PostTaskAndReplyWithResult(
          FROM_HERE, base::BindOnce(&EnsureGhosttyResourcesDirectory),
          base::BindOnce(&OnCustomizationThemeResourcesPrepared, generation,
                         config))) {
    FinishCustomizationThemeReload(generation, std::move(config),
                                   std::nullopt);
  }
}
#endif

}  // namespace

void ReloadCmuxCustomization() {
#if BUILDFLAG(IS_MAC) || BUILDFLAG(IS_LINUX)
  CustomizationThemeReloadState& state = ThemeReloadState();
  const uint64_t generation = ++state.generation;
  const std::optional<LayoutConfig> config = GetPublishedLayoutConfig();
  if (!config) {
    state.debounce_timer.Stop();
    for (const auto& [widget, record] : NativeWindows()) {
      if (widget && record.window) {
        record.window->ApplyPublishedCustomization();
      }
    }
    return;
  }
  // One frame is short enough to feel immediate and collapses rapid picker
  // changes before any Ghostty config work begins.
  state.debounce_timer.Start(
      FROM_HERE, base::Milliseconds(16),
      base::BindOnce(&BeginCustomizationThemeReload, generation, *config));
#else
  for (const auto& [widget, record] : NativeWindows()) {
    if (widget && record.window) {
      record.window->ApplyPublishedCustomization();
    }
  }
#endif
}

std::optional<CmuxAppliedTheme> GetCmuxAppliedTheme() {
  return g_window ? g_window->AppliedThemeForSettings() : std::nullopt;
}

void ApplyCmuxChromeSurfaceThemeOverrides(views::Widget* widget) {
  if (g_window) {
    g_window->ApplyCurrentChromeSurfaceThemeOverrides(widget);
    return;
  }
  if (!widget) {
    return;
  }
  widget->SetColorModeOverride(std::nullopt);
  widget->SetUserColorOverride(std::nullopt);
}

void RegisterCmuxChromeSurfaceThemeWidget(views::Widget* widget) {
  if (!widget) {
    return;
  }
  ChromeSurfaceThemeWidgets().insert(widget);
  ApplyCmuxChromeSurfaceThemeOverrides(widget);
}

void UnregisterCmuxChromeSurfaceThemeWidget(views::Widget* widget) {
  if (!widget) {
    return;
  }
  ChromeSurfaceThemeWidgets().erase(widget);
}

std::optional<GURL> StartupURLFromCommandLine() {
  for (const std::string& argument :
       base::CommandLine::ForCurrentProcess()->GetArgs()) {
    GURL candidate(argument);
    if (candidate.is_valid() && candidate.has_scheme()) {
      return candidate;
    }
  }
  return std::nullopt;
}

void ShowViewsWebWindowInternal(bool force_new) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  if (!force_new && g_views_widget) {
    g_views_widget->Show();
    g_views_widget->Activate();
    return;
  }
  LOG(WARNING) << "cmux-views: ShowViewsWebWindow creating window";
  // Normally already held from PostBrowserStart (HoldViewsKeepAlive); make
  // sure regardless.
  HoldViewsKeepAlive();
  // First (and only) window bring-up: register the cmux:// -> chrome:// URL
  // rewrite once per process. Reassert the custom WebUI registration at this
  // ownership boundary: a cmux settings URL must never be rewritten to the
  // private chrome://cmux-configure host before that host has a controller.
  static bool url_handler_registered = false;
  if (!url_handler_registered) {
    RegisterCmuxConfigureWebUI();
    content::BrowserURLHandler::GetInstance()->AddHandlerPair(
        &RewriteCmuxURLToChromeURL, content::BrowserURLHandler::null_handler());
    url_handler_registered = true;
  }
  // Tear the window down the moment shutdown begins, BEFORE profile
  // KeyedServices can die: our pane tab helpers (e.g. BookmarkTabHelper)
  // observe those services, and with DestroyProfileOnBrowserClose the profile
  // dies MID-message-loop -- closing the hidden startup Browser (see
  // SuppressStartupBrowsers) drops the last ScopedProfileKeepAlive -- so
  // waiting for PostMainMessageLoopRun is too late (~ObserverList CHECK-fails
  // on the still-registered helpers at BookmarkModel teardown).
  // OnClosingAllBrowsers(true) fires in AttemptExitInternal before any
  // Browser actually closes, on every exit path (SIGTERM/Cmd-Q/exit menu);
  // AppTerminating + PostMainMessageLoopRun are kept as backstops for paths
  // that skip it. Subscriptions leaked: process lifetime.
  static base::NoDestructor<base::CallbackListSubscription> closing_sub(
      chrome::AddClosingAllBrowsersCallback(
          base::BindRepeating([](bool closing) {
            if (closing) {
              CloseViewsWebWindow();
            }
          })));
  static base::NoDestructor<base::CallbackListSubscription> terminating_sub(
      browser_shutdown::AddAppTerminatingCallback(
          base::BindOnce(&CloseViewsWebWindow)));
  Profile* profile = g_browser_process->profile_manager()->GetLastUsedProfile();
  if (!profile) {
    LOG(ERROR) << "cmux-views: no profile";
    return;
  }
  InstallCmuxExtensionsMenuOverrides();

  BrowserProfileDomain domain{
      reinterpret_cast<uintptr_t>(profile->GetOriginalProfile()),
      profile->IsGuestSession()
          ? BrowsingMode::kGuest
          : (profile->IsOffTheRecord() ? BrowsingMode::kIncognito
                                       : BrowsingMode::kNormal)};
  BrowserRouteRequest route{domain, BrowserSurfaceType::kNormal,
                            BrowserCreationIntent::kStandardNewWindow};
  CHECK_EQ(NativeWindowRegistry().Resolve(route).action,
           BrowserRouteAction::kCreateNativeWindow);
  std::string window_group =
      force_new
          ? base::Uuid::GenerateRandomV4().AsLowercaseString()
          : BuildCmuxPrimaryWindowGroupId(
                profile->GetOriginalProfile()->GetPath().AsUTF8Unsafe(),
                CmuxBrowsingModeName(profile));
  if (const char* configured_group = getenv("CMUX_BROWSER_WINDOW_GROUP");
      configured_group && *configured_group) {
    if (IsCmuxUuid(configured_group)) {
      window_group = configured_group;
    } else {
      window_group = base::Uuid::GenerateRandomV4().AsLowercaseString();
      LOG(ERROR) << "cmux-views: invalid CMUX_BROWSER_WINDOW_GROUP; using a "
                    "fresh UUID";
    }
  }
  const NativeWindowId native_window =
      NativeWindowRegistry().RegisterWindow(domain, window_group);
  if (native_window == kInvalidNativeWindowId) {
    LOG(ERROR) << "cmux-views: duplicate live window-group UUID rejected";
    return;
  }
  auto contents_view = std::make_unique<CmuxWindowView>(
      profile, native_window, std::move(window_group),
      &NativeWindowRegistry());
  g_strip = contents_view.get();
  g_window = contents_view.get();

  auto* delegate = new views::WidgetDelegate();
  delegate->SetHasWindowSizeControls(true);
  // Window title carries an optional build tag (CMUX_TAG) so several builds
  // running side by side are distinguishable in the title bar / window list,
  // matching the bundle-name tag deploy.sh sets. e.g. CMUX_TAG=rework -> "cmux
  // ▸ rework".
  std::u16string title = u"cmux";
  if (const char* tag = getenv("CMUX_TAG"); tag && *tag) {
    title += u" ▸ " + base::UTF8ToUTF16(tag);
  }
  delegate->SetTitle(title);
  delegate->SetContentsView(std::move(contents_view));

  g_views_widget = new CmuxWidget(profile);
  views::Widget::InitParams init(
      views::Widget::InitParams::NATIVE_WIDGET_OWNS_WIDGET,
      views::Widget::InitParams::TYPE_WINDOW);
  init.delegate = delegate;
#if BUILDFLAG(IS_MAC)
  init.opacity = views::Widget::InitParams::WindowOpacity::kTranslucent;
#endif
  const int cascade_offset = 24 * static_cast<int>(NativeWindows().size() % 8);
  init.bounds = gfx::Rect(80 + cascade_offset, 80 + cascade_offset, 1500, 920);
  g_views_widget->Init(std::move(init));
  NativeWindows().emplace(
      g_views_widget,
      NativeCmuxWindow{native_window, g_views_widget, g_window});
  GetWidgetDestroyObserver().Observe(g_views_widget);
  g_window->ApplyCurrentChromeSurfaceThemeOverridesToWindow();
  g_window->FlushPendingChromeSurfaceThemeSelfTest();
  GetCmuxExtensionsContainer(profile);
  PlatformConfigureFramelessWindow(g_views_widget);

#if BUILDFLAG(IS_MAC)
  // macOS: app-wide NSEvent monitor for the niri chords + the standard Edit
  // menu (Cmd-A/C/V/X/Z routed through the responder chain). Non-mac uses the
  // FocusManager accelerators registered in CmuxWindowView instead.
  if (!getenv("CMUX_NO_MONITOR")) {
    InstallKeyMonitor(g_strip, g_views_widget);
  }
  InstallEditMenu();
#endif

  // Startup content is queued until the first canonical registry snapshot has
  // materialized its workspaces (or created Home for a confirmed empty one).
  const std::optional<GURL> startup_url = StartupURLFromCommandLine();
  g_window->InitializeStartupTab(startup_url);

  // Show + activate the window. The platform seam handles the OS specifics
  // (macOS needs NSApp activation-policy + makeKeyAndOrderFront to become the
  // key window after the startup window closes; other platforms just
  // Activate).
  PlatformActivateWindow(g_views_widget);
  LOG(WARNING) << "cmux-views: niri strip up";

#if BUILDFLAG(IS_MAC)
  // Optional automated keyboard->navigation self-test (mac-only: injects
  // NSEvents). Runs after the page and activation have settled.
  if (getenv("CMUX_E2E")) {
    RunE2ESelfTest(g_strip, g_views_widget);
  }
#endif

  // Optional showcase layout (CMUX_DEMO_LAYOUT=1): after startup settles,
  // build a tabs + splits arrangement through the same controller paths the
  // keyboard chords drive -- a second tab in the first web pane, then a
  // side-by-side and a stacked split in its column. Used for deterministic
  // cross-platform screenshots; no synthetic input involved.
  if (getenv("CMUX_DEMO_LAYOUT")) {
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
        FROM_HERE, base::BindOnce([]() {
          if (!g_strip) {
            return;
          }
          g_strip->FocusColumn(0);
          g_strip->NewTab(SurfaceKind::kWeb);
          g_strip->SplitFocused(SplitOrientation::kHorizontal);
          g_strip->SplitFocused(SplitOrientation::kVertical);
          LOG(WARNING) << "cmux-views: CMUX_DEMO_LAYOUT applied";
        }),
        base::Seconds(4));
  }

  if (getenv("CMUX_DND_SELFTEST")) {
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
        FROM_HERE, base::BindOnce([]() {
          if (g_window) {
            g_window->RunDndSelfTest();
          }
        }),
        base::Seconds(5));
  }

  if (getenv("CMUX_ANIM_SELFTEST")) {
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
        FROM_HERE, base::BindOnce([]() {
          if (g_window) {
            g_window->RunAnimSelfTest();
          }
        }),
        base::Seconds(5));
  }

  if (getenv("CMUX_SIDEBAR_SELFTEST")) {
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
        FROM_HERE, base::BindOnce([]() {
          if (g_window) {
            g_window->RunSidebarSelfTest();
          }
        }),
        base::Seconds(5));
  }

  if (getenv("CMUX_TERMEXIT_SELFTEST")) {
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
        FROM_HERE, base::BindOnce([]() {
          if (g_window) {
            g_window->RunTermExitSelfTest();
          }
        }),
        base::Seconds(5));
  }

  // Opens page info through the real LocationBarView and then closes the tab
  // while the bubble is live, covering both BrowserView-free anchoring and
  // anchor/WebContents teardown.
  if (getenv("CMUX_PAGEINFO_SELFTEST")) {
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
        FROM_HERE, base::BindOnce([]() {
          if (g_window) {
            g_window->RunPageInfoSelfTest();
          }
        }),
        base::Seconds(5));
  }

  // Verifies the custom BrowserWindow contracts used by standard permission
  // prompts, content-setting actions, device choosers, and tab-modal security
  // dialogs without depending on an external test site.
  if (getenv("CMUX_PERMISSION_SELFTEST")) {
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
        FROM_HERE, base::BindOnce([]() {
          if (g_window) {
            g_window->RunPermissionHostSelfTest();
          }
        }),
        base::Seconds(5));
  }

  // Regression for Cmd-W with focus in the closing tab's omnibox. The tab's
  // LocationBarView must blur before TabStripModel detaches its TabModel;
  // otherwise SearchTabHelper's blur callback CHECKs in TabModel::IsPinned().
  if (getenv("CMUX_CMDW_SELFTEST")) {
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
        FROM_HERE, base::BindOnce([]() {
          if (g_strip) {
            g_strip->FocusColumn(0);
            g_strip->FocusActiveOmnibar();
            LOG(WARNING) << "cmux-views: CMUX_CMDW_SELFTEST omnibox focused";
          }
        }),
        base::Seconds(4));
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
        FROM_HERE, base::BindOnce([]() {
          if (g_strip) {
            g_strip->CloseFocused();
            LOG(WARNING) << "cmux-views: CMUX_CMDW_SELFTEST close requested";
          }
        }),
        base::Seconds(5));
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
        FROM_HERE, base::BindOnce([]() {
          if (g_strip) {
            LOG(WARNING) << "cmux-views: CMUX_CMDW_SELFTEST PASS survived";
          }
        }),
        base::Seconds(7));
  }

  // Optional DevTools self-test (CMUX_DEVTOOLS_SELFTEST=1): drives the real
  // OpenDevToolsForFocused() / UndockDevToolsForFocused() paths (the same ones
  // the key monitor and the DevTools header buttons call) through a full
  // lifecycle: open docked -> undock into a window -> toggle (closes the
  // window, re-docks hidden) -> toggle (shows docked again). Column 0 (the web
  // demo pane) is focused at startup, so it inspects a real page.
  if (getenv("CMUX_DEVTOOLS_SELFTEST")) {
    auto step = [](double delay_s, const char* what,
                   void (CmuxStripController::*fn)()) {
      base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
          FROM_HERE,
          base::BindOnce(
              [](const char* what, void (CmuxStripController::*fn)()) {
                if (g_strip) {
                  g_strip->FocusColumn(0);
                  (g_strip->*fn)();
                  LOG(WARNING) << "cmux-views: CMUX_DEVTOOLS_SELFTEST " << what;
                }
              },
              what, fn),
          base::Seconds(delay_s));
    };
    step(3.5, "opened devtools (docked)",
         &CmuxStripController::OpenDevToolsForFocused);
    step(5.5, "undocked devtools to window",
         &CmuxStripController::UndockDevToolsForFocused);
    step(7, "toggled (closes the undocked window)",
         &CmuxStripController::OpenDevToolsForFocused);
    // Immediately toggle again while the async window Close() may still be in
    // flight -- must not crash regardless of which side of the race we land.
    step(7.15, "toggled during window-close race",
         &CmuxStripController::OpenDevToolsForFocused);
    step(9, "toggled (docked flip)",
         &CmuxStripController::OpenDevToolsForFocused);
    step(10.5, "undocked again",
         &CmuxStripController::UndockDevToolsForFocused);
    // The hardest teardown: close the pane's tab (== destroy the pane, its
    // page WebContents, and the DevTools contents) while the undocked DevTools
    // window is still open. ~CmuxBrowserSurface must CloseNow() the window
    // with the WebView detached, before the bindings/contents die.
    step(12.5, "closed the pane with the undocked window open",
         &CmuxStripController::CloseFocused);
    step(14.5, "SELFTEST SURVIVED (no crash through full lifecycle)",
         &CmuxStripController::FocusActiveOmnibar);
  }

  // Optional cmux:// URL-mapping self-test (CMUX_CMUXURL_SELFTEST=1): navigate
  // the focused pane to cmux://extensions and log the committed URL (should be
  // the rewritten chrome://extensions/) and the omnibox text (should display
  // the virtual cmux:// URL).
  if (getenv("CMUX_CMUXURL_SELFTEST")) {
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
        FROM_HERE, base::BindOnce([]() {
          if (g_strip) {
            g_strip->FocusColumn(0);
            g_strip->E2ENavigateFocused(GURL("cmux://extensions"));
            LOG(WARNING) << "cmux-views: CMUX_CMUXURL_SELFTEST navigating to "
                            "cmux://extensions";
          }
        }),
        base::Seconds(4));
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
        FROM_HERE, base::BindOnce([]() {
          if (g_strip) {
            LOG(WARNING) << "cmux-views: CMUX_CMUXURL_SELFTEST committed="
                         << g_strip->E2EFocusedCommittedURL() << " omnibox="
                         << base::UTF16ToUTF8(g_strip->E2EFocusedOmniboxText());
          }
        }),
        base::Seconds(9));
  }
}

void ShowViewsWebWindow() {
  ShowViewsWebWindowInternal(false);
}

void ShowNewViewsWebWindow() {
  ShowViewsWebWindowInternal(true);
}

void CloseViewsWebWindow() {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  if (NativeWindows().empty()) {
    LOG(WARNING) << "cmux-views: CloseViewsWebWindow with no window";
    DestroyCmuxExtensionsContainer();
    FinishStartupBrowserSuppression();
    ReleaseViewsKeepAlive();
    return;
  }
  // The real Chrome extensions menu bubble is owned by
  // CmuxExtensionsContainer/ExtensionsMenuCoordinator can hold references to
  // its anchor view and Browser. Close it while both are still alive.
  CloseCmuxExtensionsUi();
  // Null the globals first: nothing may route through the strip or widget
  // while (or after) they tear down.
  std::vector<views::Widget*> widgets;
  for (const auto& [widget, record] : NativeWindows()) {
    widgets.push_back(widget);
  }
  for (views::Widget* widget : widgets) {
    GetWidgetDestroyObserver().StopObserving(widget);
    ClearViewsWindowStateForWidget(widget);
    // Synchronous destruction of the whole window: the native widget owns
    // the Widget and its CmuxWindowView contents.
    widget->CloseNow();
  }
  DestroyCmuxExtensionsContainer();
  LOG(WARNING) << "cmux-views: niri window closed for shutdown";
}

#if !BUILDFLAG(IS_MAC)
// Non-mac platform activation: Views' cross-platform Show()/Activate() is
// enough (Ozone handles foreground/focus). The macOS implementation (NSApp
// activation policy + makeKeyAndOrderFront) lives in cmux_views_mac.mm.
void PlatformActivateWindow(views::Widget* widget) {
  if (!widget) {
    return;
  }
  widget->Show();
  widget->Activate();
}

void PlatformConfigureFramelessWindow(views::Widget*) {}

void PlatformBeginWindowDrag(views::Widget*, const ui::MouseEvent&) {}
#endif

}  // namespace cmux
