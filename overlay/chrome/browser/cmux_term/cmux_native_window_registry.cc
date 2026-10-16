// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "chrome/browser/cmux_term/cmux_native_window_registry.h"

#include <utility>

namespace cmux {

CmuxNativeWindowRegistry::CmuxNativeWindowRegistry() = default;
CmuxNativeWindowRegistry::~CmuxNativeWindowRegistry() = default;

NativeWindowId CmuxNativeWindowRegistry::RegisterWindow(
    BrowserProfileDomain domain,
    std::string group_id) {
  if (!group_id.empty()) {
    for (const auto& [id, record] : windows_) {
      if (record.domain == domain && record.group_id == group_id) {
        return kInvalidNativeWindowId;
      }
    }
  }
  const NativeWindowId id = next_window_id_++;
  windows_.emplace(
      id, WindowRecord{domain, std::move(group_id), next_activation_serial_++});
  return id;
}

bool CmuxNativeWindowRegistry::UnregisterWindow(NativeWindowId window) {
  if (windows_.erase(window) == 0) {
    return false;
  }
  for (auto it = workspaces_.begin(); it != workspaces_.end();) {
    if (it->first.window == window) {
      it = workspaces_.erase(it);
    } else {
      ++it;
    }
  }
  return true;
}

bool CmuxNativeWindowRegistry::ActivateWindow(NativeWindowId window) {
  auto it = windows_.find(window);
  if (it == windows_.end()) {
    return false;
  }
  it->second.activation_serial = next_activation_serial_++;
  return true;
}

bool CmuxNativeWindowRegistry::ContainsWindow(NativeWindowId window) const {
  return windows_.find(window) != windows_.end();
}

std::optional<BrowserProfileDomain>
CmuxNativeWindowRegistry::DomainForWindow(NativeWindowId window) const {
  auto it = windows_.find(window);
  return it == windows_.end() ? std::nullopt
                             : std::optional(it->second.domain);
}

std::optional<std::string> CmuxNativeWindowRegistry::GroupForWindow(
    NativeWindowId window) const {
  auto it = windows_.find(window);
  return it == windows_.end() ? std::nullopt
                             : std::optional(it->second.group_id);
}

std::optional<NativeWindowId>
CmuxNativeWindowRegistry::MostRecentlyActiveWindow(
    BrowserProfileDomain domain) const {
  std::optional<NativeWindowId> result;
  uint64_t newest = 0;
  for (const auto& [id, record] : windows_) {
    if (record.domain == domain && record.activation_serial > newest) {
      newest = record.activation_serial;
      result = id;
    }
  }
  return result;
}

bool CmuxNativeWindowRegistry::RegisterWorkspace(WorkspaceLocator workspace) {
  auto window = windows_.find(workspace.window);
  if (window == windows_.end() || workspace.local_workspace <= 0) {
    return false;
  }
  return workspaces_.emplace(workspace, window->second.domain).second;
}

bool CmuxNativeWindowRegistry::UnregisterWorkspace(
    WorkspaceLocator workspace) {
  return workspaces_.erase(workspace) != 0;
}

bool CmuxNativeWindowRegistry::ContainsWorkspace(
    WorkspaceLocator workspace) const {
  return workspaces_.find(workspace) != workspaces_.end();
}

BrowserRouteDecision CmuxNativeWindowRegistry::Resolve(
    const BrowserRouteRequest& request) const {
  // BrowserView-only window types retain Chromium's own ownership and bounds
  // semantics until cmux grows dedicated popup/app containers.
  if (request.surface != BrowserSurfaceType::kNormal) {
    return {};
  }

  if (request.assigned_workspace) {
    auto existing = workspaces_.find(*request.assigned_workspace);
    if (existing == workspaces_.end() || existing->second != request.domain) {
      // A stale/forged workspace token must never silently attach elsewhere.
      return {};
    }
    return {BrowserRouteAction::kAttachToExistingWorkspace,
            request.assigned_workspace->window, request.assigned_workspace};
  }

  // Standard New Window always means a new OS-native container, even when an
  // invoking cmux window is known.
  if (request.intent == BrowserCreationIntent::kStandardNewWindow ||
      request.intent == BrowserCreationIntent::kSessionRestore ||
      request.intent == BrowserCreationIntent::kUnknown) {
    return {BrowserRouteAction::kCreateNativeWindow, std::nullopt,
            std::nullopt};
  }

  // New Workspace and extension-created normal windows may be adopted only
  // when the initiating container is explicit, live, and in the exact same
  // profile/privacy domain. Missing context falls back to a new native window,
  // never whichever cmux window happened to activate most recently.
  if (request.invoking_window &&
      IsCompatible(*request.invoking_window, request.domain)) {
    return {BrowserRouteAction::kAdoptAsNewWorkspace,
            request.invoking_window, std::nullopt};
  }
  return {BrowserRouteAction::kCreateNativeWindow, std::nullopt, std::nullopt};
}

bool CmuxNativeWindowRegistry::IsCompatible(
    NativeWindowId window,
    BrowserProfileDomain domain) const {
  auto it = windows_.find(window);
  return it != windows_.end() && it->second.domain == domain;
}

}  // namespace cmux
