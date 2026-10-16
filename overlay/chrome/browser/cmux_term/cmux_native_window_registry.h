// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef CHROME_BROWSER_CMUX_TERM_CMUX_NATIVE_WINDOW_REGISTRY_H_
#define CHROME_BROWSER_CMUX_TERM_CMUX_NATIVE_WINDOW_REGISTRY_H_

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>

namespace cmux {

using NativeWindowId = int64_t;
constexpr NativeWindowId kInvalidNativeWindowId = 0;

// Runtime identity for a Profile partition. `profile` represents the original
// Profile; browsing mode remains separate so normal and off-the-record Browser
// instances can never share one native cmux container.
enum class BrowsingMode { kNormal, kIncognito, kGuest };

struct BrowserProfileDomain {
  uint64_t profile = 0;
  BrowsingMode mode = BrowsingMode::kNormal;

  friend bool operator==(const BrowserProfileDomain& lhs,
                         const BrowserProfileDomain& rhs) {
    return lhs.profile == rhs.profile && lhs.mode == rhs.mode;
  }
  friend bool operator!=(const BrowserProfileDomain& lhs,
                         const BrowserProfileDomain& rhs) {
    return !(lhs == rhs);
  }
};

// WindowModel IDs are local to a CmuxWindowView and restart at 1. Any
// process-wide lookup must therefore use this composite locator, not a bare
// WorkspaceId.
struct WorkspaceLocator {
  NativeWindowId window = kInvalidNativeWindowId;
  int64_t local_workspace = 0;

  friend bool operator==(const WorkspaceLocator& lhs,
                         const WorkspaceLocator& rhs) {
    return lhs.window == rhs.window &&
           lhs.local_workspace == rhs.local_workspace;
  }
  friend bool operator<(const WorkspaceLocator& lhs,
                        const WorkspaceLocator& rhs) {
    return lhs.window < rhs.window ||
           (lhs.window == rhs.window &&
            lhs.local_workspace < rhs.local_workspace);
  }
};

enum class BrowserSurfaceType {
  kNormal,
  kPopup,
  kApp,
  kDevTools,
};

enum class BrowserCreationIntent {
  // Chrome's ordinary New Window command: create a new native cmux container.
  kStandardNewWindow,
  // cmux's explicit logical-window command inside an existing container.
  kNewWorkspace,
  // chrome.windows.create() or an equivalent extension request.
  kExtensionWindowCreate,
  kSessionRestore,
  kUnknown,
};

struct BrowserRouteRequest {
  BrowserProfileDomain domain;
  BrowserSurfaceType surface = BrowserSurfaceType::kNormal;
  BrowserCreationIntent intent = BrowserCreationIntent::kUnknown;
  // Must be propagated from the Browser/WebContents which initiated creation.
  std::optional<NativeWindowId> invoking_window;
  // Set only for a Browser created for an already-registered workspace.
  std::optional<WorkspaceLocator> assigned_workspace;
};

enum class BrowserRouteAction {
  // Attach a Browser to the exact pre-existing workspace in `workspace`.
  kAttachToExistingWorkspace,
  // Ask `window` to create a new logical workspace and adopt the Browser.
  kAdoptAsNewWorkspace,
  // Construct a new native cmux container for this Browser/profile domain.
  kCreateNativeWindow,
  // Keep Chromium's BrowserView path (popups/apps/DevTools or invalid token).
  kUseChromiumWindow,
};

struct BrowserRouteDecision {
  BrowserRouteAction action = BrowserRouteAction::kUseChromiumWindow;
  std::optional<NativeWindowId> window;
  std::optional<WorkspaceLocator> workspace;
};

// Pure registry and routing policy. It owns IDs and metadata only; production
// integration must keep native containers/Widgets in a separate owner and
// register weak/non-owning host handles against these IDs.
class CmuxNativeWindowRegistry {
 public:
  CmuxNativeWindowRegistry();
  ~CmuxNativeWindowRegistry();

  NativeWindowId RegisterWindow(BrowserProfileDomain domain,
                                std::string group_id = std::string());
  bool UnregisterWindow(NativeWindowId window);
  bool ActivateWindow(NativeWindowId window);
  bool ContainsWindow(NativeWindowId window) const;
  std::optional<BrowserProfileDomain> DomainForWindow(
      NativeWindowId window) const;
  std::optional<std::string> GroupForWindow(NativeWindowId window) const;
  std::optional<NativeWindowId> MostRecentlyActiveWindow(
      BrowserProfileDomain domain) const;

  bool RegisterWorkspace(WorkspaceLocator workspace);
  bool UnregisterWorkspace(WorkspaceLocator workspace);
  bool ContainsWorkspace(WorkspaceLocator workspace) const;

  BrowserRouteDecision Resolve(const BrowserRouteRequest& request) const;

  size_t window_count() const { return windows_.size(); }
  size_t workspace_count() const { return workspaces_.size(); }

 private:
  struct WindowRecord {
    BrowserProfileDomain domain;
    std::string group_id;
    uint64_t activation_serial = 0;
  };

  bool IsCompatible(NativeWindowId window,
                    BrowserProfileDomain domain) const;

  NativeWindowId next_window_id_ = 1;
  uint64_t next_activation_serial_ = 1;
  std::map<NativeWindowId, WindowRecord> windows_;
  std::map<WorkspaceLocator, BrowserProfileDomain> workspaces_;
};

}  // namespace cmux

#endif  // CHROME_BROWSER_CMUX_TERM_CMUX_NATIVE_WINDOW_REGISTRY_H_
