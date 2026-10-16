// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "chrome/browser/cmux_term/cmux_theme_ghostty.h"

#include <algorithm>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/containers/span.h"
#include "base/files/file_enumerator.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/logging.h"
#include "base/no_destructor.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/strings/stringprintf.h"
#include "base/synchronization/lock.h"
#include "chrome/services/cmux_terminal_renderer/public/cpp/cmux_ghostty_resources.h"

#ifndef GHOSTTY_STATIC
#define GHOSTTY_STATIC
#endif
#include "third_party/cmux_ghostty/include/ghostty.h"

namespace cmux {

namespace {

struct GhosttyCoreInitState {
  base::Lock lock;
  bool attempted = false;
  bool ok = false;
};

GhosttyCoreInitState& CoreInitState() {
  static base::NoDestructor<GhosttyCoreInitState> state;
  return *state;
}

CmuxArgb FromGhosttyColor(const ghostty_config_color_s& color) {
  return CmuxRgb(color.r, color.g, color.b);
}

bool GetColor(ghostty_config_t config, std::string_view key, CmuxArgb* out) {
  ghostty_config_color_s color = {};
  if (!out || !ghostty_config_get(config, &color, key.data(), key.size())) {
    return false;
  }
  *out = FromGhosttyColor(color);
  return true;
}

std::optional<CmuxArgb> GetOptionalColor(ghostty_config_t config,
                                         std::string_view key) {
  CmuxArgb color = 0;
  if (!GetColor(config, key, &color)) {
    return std::nullopt;
  }
  return color;
}

void ApplyThemeOverride(ghostty_config_t config,
                        const std::string& theme_name) {
  if (theme_name.empty()) {
    return;
  }
  const std::string override = "theme = " + theme_name;
  ghostty_config_load_string(config, override.data(), override.size(),
                            "cmux://settings/appearance");
}

std::optional<base::FilePath> GhosttyThemeDirectory() {
  if (!EnsureGhosttyResourcesDirectory()) {
    return std::nullopt;
  }
  const char* resources = std::getenv("GHOSTTY_RESOURCES_DIR");
  if (!resources || !*resources) {
    return std::nullopt;
  }
  return base::FilePath::FromUTF8Unsafe(resources).AppendASCII("themes");
}

std::optional<CmuxArgb> ParseThemeColor(std::string_view text) {
  text = base::TrimWhitespaceASCII(text, base::TRIM_ALL);
  if (text.size() != 7 || text.front() != '#') {
    return std::nullopt;
  }
  uint32_t rgb = 0;
  if (!base::HexStringToUInt(text.substr(1), &rgb)) {
    return std::nullopt;
  }
  return CmuxRgb(static_cast<uint8_t>((rgb >> 16) & 0xff),
                 static_cast<uint8_t>((rgb >> 8) & 0xff),
                 static_cast<uint8_t>(rgb & 0xff));
}

void ParseThemePreview(std::string_view contents,
                       GhosttyThemePreview* preview) {
  CHECK(preview);
  bool cursor_found = false;
  for (std::string_view line :
       base::SplitStringPiece(contents, "\n", base::KEEP_WHITESPACE,
                              base::SPLIT_WANT_NONEMPTY)) {
    const size_t separator = line.find('=');
    if (separator == std::string_view::npos) {
      continue;
    }
    const std::string_view key =
        base::TrimWhitespaceASCII(line.substr(0, separator), base::TRIM_ALL);
    std::string_view value =
        base::TrimWhitespaceASCII(line.substr(separator + 1), base::TRIM_ALL);
    if (key == "background") {
      if (std::optional<CmuxArgb> color = ParseThemeColor(value)) {
        preview->background = *color;
      }
    } else if (key == "foreground") {
      if (std::optional<CmuxArgb> color = ParseThemeColor(value)) {
        preview->foreground = *color;
        if (!cursor_found) {
          preview->cursor = *color;
        }
      }
    } else if (key == "cursor-color") {
      if (std::optional<CmuxArgb> color = ParseThemeColor(value)) {
        preview->cursor = *color;
        cursor_found = true;
      }
    } else if (key == "palette") {
      const size_t palette_separator = value.find('=');
      if (palette_separator == std::string_view::npos) {
        continue;
      }
      unsigned index = 0;
      if (!base::StringToUint(
              base::TrimWhitespaceASCII(value.substr(0, palette_separator),
                                        base::TRIM_ALL),
              &index) ||
          index >= preview->palette.size()) {
        continue;
      }
      if (std::optional<CmuxArgb> color =
              ParseThemeColor(value.substr(palette_separator + 1))) {
        preview->palette[index] = *color;
      }
    }
  }
}

std::string GhosttyColorHex(CmuxArgb color) {
  return base::StringPrintf("#%02x%02x%02x", CmuxArgbR(color),
                            CmuxArgbG(color), CmuxArgbB(color));
}

}  // namespace

bool EnsureGhosttyCoreInit() {
  GhosttyCoreInitState& state = CoreInitState();
  base::AutoLock hold(state.lock);
  if (!state.attempted) {
    state.attempted = true;
    state.ok = EnsureGhosttyResourcesDirectory() &&
               ghostty_init(0, nullptr) == GHOSTTY_SUCCESS;
    if (!state.ok) {
      LOG(ERROR) << "cmux-theme: pinned Ghostty resources or init unavailable";
    }
  }
  return state.ok;
}

std::optional<CmuxTheme> LoadGhosttyTheme(std::string theme_name) {
  if (!EnsureGhosttyCoreInit()) {
    return std::nullopt;
  }

  ghostty_config_t config = ghostty_config_new();
  if (!config) {
    return std::nullopt;
  }

  ghostty_config_load_default_files(config);
  // Standalone Ghostty resolves `config-file` includes before finalizing.
  // Deliberately omit CLI loading: Chromium's switches are not Ghostty input.
  ghostty_config_load_recursive_files(config);
  ApplyThemeOverride(config, theme_name);
  ghostty_config_finalize(config);

  // Theme/config problems (e.g. a `theme = <name>` that cannot be found)
  // don't fail finalize — they land in the diagnostics list and ghostty
  // falls back to defaults. Surface them so misconfigurations are visible.
  const uint32_t diag_count = ghostty_config_diagnostics_count(config);
  for (uint32_t i = 0; i < diag_count; ++i) {
    const ghostty_diagnostic_s diag = ghostty_config_get_diagnostic(config, i);
    LOG(WARNING) << "cmux-theme: ghostty config diagnostic: "
                 << (diag.message ? diag.message : "(null)");
  }

  CmuxTheme theme;
  if (!GetColor(config, "background", &theme.bg) ||
      !GetColor(config, "foreground", &theme.fg)) {
    ghostty_config_free(config);
    return std::nullopt;
  }

  ghostty_config_palette_s palette = {};
  if (!ghostty_config_get(config, &palette, "palette",
                          std::string_view("palette").size())) {
    ghostty_config_free(config);
    return std::nullopt;
  }
  const auto palette_colors = base::span(palette.colors);
  for (size_t i = 0; i < theme.palette.size(); ++i) {
    theme.palette[i] = FromGhosttyColor(palette_colors[i]);
  }

  theme.cursor = GetOptionalColor(config, "cursor-color");
  theme.selection_bg = GetOptionalColor(config, "selection-background");
  theme.selection_fg = GetOptionalColor(config, "selection-foreground");

  ghostty_config_free(config);
  return theme;
}

std::vector<std::string> ListGhosttyThemes() {
  std::vector<std::string> themes;
  const std::optional<base::FilePath> theme_directory =
      GhosttyThemeDirectory();
  if (!theme_directory) {
    return themes;
  }
  base::FileEnumerator files(*theme_directory, /*recursive=*/false,
                             base::FileEnumerator::FILES);
  for (base::FilePath file = files.Next(); !file.empty(); file = files.Next()) {
    const std::string name = file.BaseName().AsUTF8Unsafe();
    if (!name.empty() && name != "." && name != "..") {
      themes.push_back(name);
    }
  }
  std::sort(themes.begin(), themes.end());
  themes.erase(std::unique(themes.begin(), themes.end()), themes.end());
  return themes;
}

std::vector<GhosttyThemePreview> ListGhosttyThemePreviews() {
  std::vector<GhosttyThemePreview> previews;
  const std::optional<base::FilePath> theme_directory =
      GhosttyThemeDirectory();
  if (!theme_directory) {
    return previews;
  }

  base::FileEnumerator files(*theme_directory, /*recursive=*/false,
                             base::FileEnumerator::FILES);
  for (base::FilePath file = files.Next(); !file.empty(); file = files.Next()) {
    GhosttyThemePreview preview;
    preview.name = file.BaseName().AsUTF8Unsafe();
    if (preview.name.empty() || preview.name == "." || preview.name == "..") {
      continue;
    }
    std::string contents;
    if (base::ReadFileToStringWithMaxSize(file, &contents, 64 * 1024)) {
      ParseThemePreview(contents, &preview);
    }
    previews.push_back(std::move(preview));
  }
  std::sort(previews.begin(), previews.end(),
            [](const GhosttyThemePreview& left,
               const GhosttyThemePreview& right) {
              return left.name < right.name;
            });
  previews.erase(
      std::unique(previews.begin(), previews.end(),
                  [](const GhosttyThemePreview& left,
                     const GhosttyThemePreview& right) {
                    return left.name == right.name;
                  }),
      previews.end());
  return previews;
}

std::string SerializeGhosttyThemeColors(const CmuxTheme& theme) {
  std::string config =
      base::StringPrintf("background = %s\nforeground = %s\n",
                         GhosttyColorHex(theme.bg).c_str(),
                         GhosttyColorHex(theme.fg).c_str());
  if (theme.cursor) {
    config.append(
        base::StringPrintf("cursor-color = %s\n",
                           GhosttyColorHex(*theme.cursor).c_str()));
  }
  if (theme.selection_bg) {
    config.append(base::StringPrintf(
        "selection-background = %s\n",
        GhosttyColorHex(*theme.selection_bg).c_str()));
  }
  if (theme.selection_fg) {
    config.append(base::StringPrintf(
        "selection-foreground = %s\n",
        GhosttyColorHex(*theme.selection_fg).c_str()));
  }
  for (size_t i = 0; i < theme.palette.size(); ++i) {
    config.append(base::StringPrintf("palette = %zu=%s\n", i,
                                     GhosttyColorHex(theme.palette[i]).c_str()));
  }
  return config;
}

}  // namespace cmux
