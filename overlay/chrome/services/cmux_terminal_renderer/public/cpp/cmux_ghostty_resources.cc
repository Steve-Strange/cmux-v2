// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "chrome/services/cmux_terminal_renderer/public/cpp/cmux_ghostty_resources.h"

#include <utility>

namespace cmux {
namespace {

std::string AppendPath(std::string_view base, std::string_view suffix) {
  std::string path(base);
  if (!path.empty() && path.back() != '/') {
    path.push_back('/');
  }
  path.append(suffix);
  return path;
}

}  // namespace

std::optional<std::string> SelectGhosttyResourcesDirectory(
    std::string_view explicit_directory,
    std::string_view outer_bundle_directory,
    const std::vector<std::string>& fallback_directories,
    const GhosttyResourcesProbe& is_complete) {
  if (!explicit_directory.empty()) {
    return std::string(explicit_directory);
  }
  if (!is_complete) {
    return std::nullopt;
  }
  if (!outer_bundle_directory.empty()) {
    std::string bundled =
        AppendPath(outer_bundle_directory, "Contents/Resources/ghostty");
    if (is_complete(bundled)) {
      return bundled;
    }
    return std::nullopt;
  }
  for (const std::string& fallback : fallback_directories) {
    if (!fallback.empty() && is_complete(fallback)) {
      return fallback;
    }
  }
  return std::nullopt;
}

}  // namespace cmux
