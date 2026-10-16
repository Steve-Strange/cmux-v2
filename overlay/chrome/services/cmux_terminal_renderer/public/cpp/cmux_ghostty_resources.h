// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef CHROME_SERVICES_CMUX_TERMINAL_RENDERER_PUBLIC_CPP_CMUX_GHOSTTY_RESOURCES_H_
#define CHROME_SERVICES_CMUX_TERMINAL_RENDERER_PUBLIC_CPP_CMUX_GHOSTTY_RESOURCES_H_

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cmux {

using GhosttyResourcesProbe = std::function<bool(std::string_view)>;

// Pure selection seam used by both the browser and renderer utility. A
// non-empty explicit value always wins. Otherwise the outer cmux bundle is
// authoritative when present: an incomplete product bundle fails closed
// instead of borrowing resources from a different installed Ghostty build.
// Fallbacks are considered only by unbundled platforms.
std::optional<std::string> SelectGhosttyResourcesDirectory(
    std::string_view explicit_directory,
    std::string_view outer_bundle_directory,
    const std::vector<std::string>& fallback_directories,
    const GhosttyResourcesProbe& is_complete);

// Sets GHOSTTY_RESOURCES_DIR before libghostty initialization. On macOS this
// resolves the outer Chromium bundle even from a nested renderer helper.
// Returns false when no complete pinned runtime tree can be selected.
bool EnsureGhosttyResourcesDirectory();

}  // namespace cmux

#endif  // CHROME_SERVICES_CMUX_TERMINAL_RENDERER_PUBLIC_CPP_CMUX_GHOSTTY_RESOURCES_H_
