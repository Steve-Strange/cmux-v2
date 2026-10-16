// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "chrome/browser/cmux_term/cmux_layout_config.h"

#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "base/base_paths.h"
#include "base/environment.h"
#include "base/files/file.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/important_file_writer.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/no_destructor.h"
#include "base/path_service.h"
#include "base/task/thread_pool.h"
#include "base/values.h"

namespace cmux {

namespace {

constexpr SkColor kDefaultAccentSkColor = SkColorSetRGB(0x8a, 0x8f, 0x98);
constexpr SkColor kDefaultDropHighlightSkColor =
    SkColorSetRGB(0x00, 0x7a, 0xff);
constexpr SkColor kDefaultFocusSkColor = SkColorSetRGB(0x00, 0x7a, 0xff);

const char* SidebarModeName(sidebar_metrics::SidebarMode mode) {
  switch (mode) {
    case sidebar_metrics::SidebarMode::kExpanded:
      return "normal";
    case sidebar_metrics::SidebarMode::kIcons:
      return "icons";
    case sidebar_metrics::SidebarMode::kHidden:
      return "hidden";
  }
  return "normal";
}

std::optional<sidebar_metrics::SidebarMode> ParseSidebarMode(
    std::string_view value) {
  if (value == "normal" || value == "expanded") {
    return sidebar_metrics::SidebarMode::kExpanded;
  }
  // The short-lived three-width dogfood called icons-only "compact". Accept
  // that spelling so its persisted config migrates without surprising users.
  if (value == "compact") {
    return sidebar_metrics::SidebarMode::kIcons;
  }
  if (value == "icons") {
    return sidebar_metrics::SidebarMode::kIcons;
  }
  if (value == "hidden") {
    return sidebar_metrics::SidebarMode::kHidden;
  }
  return std::nullopt;
}

int HexDigitValue(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

bool HexByte(std::string_view text, size_t offset, int* out) {
  const int hi = HexDigitValue(text[offset]);
  const int lo = HexDigitValue(text[offset + 1]);
  if (hi < 0 || lo < 0) {
    return false;
  }
  *out = hi * 16 + lo;
  return true;
}

std::string SanitizedColorString(const char* key,
                                 const std::string& value,
                                 const char* default_value,
                                 bool allow_empty) {
  if (allow_empty && value.empty()) {
    return std::string();
  }
  SkColor color;
  if (ParseLayoutHexColor(value, &color)) {
    return value;
  }
  LOG(WARNING) << "cmux-layout: invalid " << key << " \"" << value
               << "\"; using default "
               << (allow_empty ? "accent_color" : default_value);
  return std::string(default_value);
}

std::string SanitizedNewTabPage(const std::string& value) {
  if (IsSupportedBrowserNewTabPage(value)) {
    return value;
  }
  LOG(WARNING) << "cmux-config: invalid browser.newTabPage \"" << value
               << "\"; using default " << kBrowserDefaultNewTabPage;
  return kBrowserDefaultNewTabPage;
}

std::optional<base::FilePath> ResolveCmuxConfigPath(
    bool require_absolute_config) {
  std::unique_ptr<base::Environment> environment =
      base::Environment::Create();
  if (std::optional<std::string> configured =
          environment->GetVar("CMUX_CONFIG");
      configured && !configured->empty()) {
    const base::FilePath configured_path =
        base::FilePath::FromUTF8Unsafe(*configured);
    if (require_absolute_config && !configured_path.IsAbsolute()) {
      return std::nullopt;
    }
    return configured_path;
  }
  if (std::optional<std::string> xdg =
          environment->GetVar("XDG_CONFIG_HOME");
      xdg && !xdg->empty()) {
    const base::FilePath xdg_path = base::FilePath::FromUTF8Unsafe(*xdg);
    if (xdg_path.IsAbsolute()) {
      return xdg_path.AppendASCII("cmux").AppendASCII("cmux.json");
    }
  }
  base::FilePath home;
  if (base::PathService::Get(base::DIR_HOME, &home)) {
    return home.AppendASCII(".config")
        .AppendASCII("cmux")
        .AppendASCII("cmux.json");
  }
  return base::FilePath::FromUTF8Unsafe("/tmp/cmux/cmux.json");
}

std::optional<LayoutConfig> LoadLayoutConfigOnBackgroundSequence(
    LayoutConfig defaults) {
  std::string json;
  if (!base::ReadFileToString(
          base::FilePath::FromUTF8Unsafe(LayoutConfigPath()), &json)) {
    return std::nullopt;
  }
  LayoutConfig config = std::move(defaults);
  if (!LayoutConfig::FromJson(json, &config)) {
    return std::nullopt;
  }
  return config;
}

std::optional<BrowserConfig> LoadBrowserConfigOnBackgroundSequence() {
  const std::optional<base::FilePath> path =
      ResolveCmuxConfigPath(/*require_absolute_config=*/false);
  std::string json;
  if (!path || !base::ReadFileToString(*path, &json)) {
    return std::nullopt;
  }
  BrowserConfig config;
  if (!BrowserConfig::FromJson(json, &config)) {
    return std::nullopt;
  }
  return config;
}

BrowserConfigLoadResult
LoadBrowserConfigWithStatusOnBackgroundSequence() {
  const std::optional<base::FilePath> path =
      ResolveCmuxConfigPath(/*require_absolute_config=*/true);
  if (!path) {
    return {.status = BrowserConfigLoadStatus::kError};
  }
  base::File readable_file(*path,
                           base::File::FLAG_OPEN | base::File::FLAG_READ);
  if (!readable_file.IsValid()) {
    const BrowserConfigLoadStatus status =
        readable_file.error_details() == base::File::FILE_ERROR_NOT_FOUND
            ? BrowserConfigLoadStatus::kAbsent
            : BrowserConfigLoadStatus::kError;
    return {.status = status};
  }
  readable_file.Close();

  std::string json;
  if (!base::ReadFileToString(*path, &json)) {
    return {.status = BrowserConfigLoadStatus::kError};
  }
  BrowserConfig config;
  if (!BrowserConfig::FromJson(json, &config)) {
    return {.status = BrowserConfigLoadStatus::kError};
  }
  return {
      .status = BrowserConfigLoadStatus::kLoaded,
      .config = std::move(config),
  };
}

base::ImportantFileWriter& LayoutConfigWriter() {
  // Non-const ImportantFileWriter calls stay on the UI sequence; file I/O is
  // delegated to this BLOCK_SHUTDOWN background sequence.
  static base::NoDestructor<base::ImportantFileWriter> writer(
      base::FilePath::FromUTF8Unsafe(LayoutConfigPath()),
      base::ThreadPool::CreateSequencedTaskRunner(
          {base::MayBlock(), base::TaskShutdownBehavior::BLOCK_SHUTDOWN}));
  return *writer;
}

struct PublishedLayoutConfig {
  std::optional<LayoutConfig> value;
  base::RepeatingCallbackList<void(const LayoutConfig&)> callbacks;
};

PublishedLayoutConfig& PublishedConfig() {
  static base::NoDestructor<PublishedLayoutConfig> published;
  return *published;
}

std::string SanitizedThemeName(const std::string& value) {
  if (value.find_first_of("\r\n") != std::string::npos) {
    LOG(WARNING) << "cmux-layout: ghostty_theme_name contains a newline; "
                    "following Ghostty config instead";
    return std::string();
  }
  return value;
}

}  // namespace

LayoutConfig::LayoutConfig() = default;
LayoutConfig::LayoutConfig(const LayoutConfig&) = default;
LayoutConfig& LayoutConfig::operator=(const LayoutConfig&) = default;
LayoutConfig::~LayoutConfig() = default;

std::string LayoutConfig::ToJson() const {
  base::DictValue d;
  d.Set("rail_width", rail_width);
  d.Set("sidebar_icon_width",
        sidebar_metrics::ClampIconsWidth(sidebar_icon_width));
  d.Set("gap", gap);
  d.Set("margin", margin);
  d.Set("min_column_width", min_column_width);
  d.Set("column_fraction", column_fraction);
  base::ListValue column_width_modes_value;
  for (double mode : column_width_modes) {
    column_width_modes_value.Append(mode);
  }
  d.Set("column_width_modes", std::move(column_width_modes_value));
  d.Set("header_height", header_height);
  d.Set("sidebar_mode", SidebarModeName(sidebar_mode));
  // Keep the historical key so an older cmux build still interprets the
  // zero-width endpoint as collapsed. `sidebar_mode` is authoritative here.
  d.Set("sidebar_collapsed",
        sidebar_mode == sidebar_metrics::SidebarMode::kHidden);
  d.Set("sidebar_position",
        sidebar_position == SidebarPosition::kRight ? "right" : "left");
  d.Set("focus_border", focus_border);
  d.Set("animations", animations);
  d.Set("animation_ms", animation_ms);
  if (HasUserKey("accent_color")) {
    d.Set("accent_color", SanitizedColorString("accent_color", accent_color,
                                               kLayoutDefaultAccentColor,
                                               /*allow_empty=*/false));
  }
  if (HasUserKey("drop_highlight_color")) {
    d.Set("drop_highlight_color",
          SanitizedColorString("drop_highlight_color", drop_highlight_color,
                               kLayoutDefaultDropHighlightColor,
                               /*allow_empty=*/false));
  }
  if (HasUserKey("focus_color")) {
    d.Set("focus_color", SanitizedColorString("focus_color", focus_color,
                                              kLayoutDefaultFocusColor,
                                              /*allow_empty=*/false));
  }
  auto set_optional_color = [&](const char* key, const std::string& value) {
    if (HasUserKey(key)) {
      d.Set(key,
            SanitizedColorString(key, value, kLayoutDefaultChromeSurfaceColor,
                                 /*allow_empty=*/true));
    }
  };
  set_optional_color("chrome_tab_bar_color", chrome_tab_bar_color);
  set_optional_color("chrome_toolbar_color", chrome_toolbar_color);
  set_optional_color("chrome_omnibox_color", chrome_omnibox_color);
  set_optional_color("chrome_omnibox_popup_color", chrome_omnibox_popup_color);
  set_optional_color("chrome_omnibox_popup_hover_color",
                     chrome_omnibox_popup_hover_color);
  d.Set("ghostty_theme", ghostty_theme);
  d.Set("ghostty_theme_name", SanitizedThemeName(ghostty_theme_name));
  d.Set("toolbar_show_back", toolbar_show_back);
  d.Set("toolbar_show_forward", toolbar_show_forward);
  d.Set("toolbar_show_reload", toolbar_show_reload);
  d.Set("toolbar_show_home", toolbar_show_home);
  d.Set("toolbar_show_extensions", toolbar_show_extensions);
  d.Set("toolbar_show_downloads", toolbar_show_downloads);
  d.Set("toolbar_show_media", toolbar_show_media);
  d.Set("toolbar_show_profile", toolbar_show_profile);
  d.Set("toolbar_show_menu", toolbar_show_menu);
  std::string out;
  base::JSONWriter::WriteWithOptions(d, base::JSONWriter::OPTIONS_PRETTY_PRINT,
                                     &out);
  return out;
}

bool LayoutConfig::HasUserKey(std::string_view key) const {
  return present_keys.find(std::string(key)) != present_keys.end();
}

bool LayoutConfig::FromJson(const std::string& json, LayoutConfig* out) {
  if (!out) {
    return false;
  }
  std::optional<base::Value> v =
      base::JSONReader::Read(json, base::JSON_PARSE_RFC);
  if (!v || !v->is_dict()) {
    return false;
  }

  LayoutConfig next = *out;
  const base::DictValue& d = v->GetDict();
  next.present_keys.clear();
  for (auto entry : d) {
    next.present_keys.insert(std::string(entry.first));
  }
  auto geti = [&](const char* k, int& dst) {
    if (auto x = d.FindInt(k)) {
      dst = *x;
    }
  };
  auto getb = [&](const char* k, bool& dst) {
    if (auto x = d.FindBool(k)) {
      dst = *x;
    }
  };
  auto getd = [&](const char* k, double& dst) {
    if (auto x = d.FindDouble(k)) {
      dst = *x;
    } else if (auto i = d.FindInt(k)) {
      dst = *i;
    }
  };
  auto gets = [&](const char* k, std::string& dst, const char* default_value,
                  bool allow_empty) {
    if (const std::string* x = d.FindString(k)) {
      dst = SanitizedColorString(k, *x, default_value, allow_empty);
    }
  };

  geti("rail_width", next.rail_width);
  geti("sidebar_icon_width", next.sidebar_icon_width);
  next.sidebar_icon_width =
      sidebar_metrics::ClampIconsWidth(next.sidebar_icon_width);
  geti("gap", next.gap);
  geti("margin", next.margin);
  geti("min_column_width", next.min_column_width);
  getd("column_fraction", next.column_fraction);
  if (const base::ListValue* modes = d.FindList("column_width_modes")) {
    std::vector<double> parsed_modes;
    parsed_modes.reserve(modes->size());
    for (const base::Value& mode : *modes) {
      double value = 0.0;
      if (mode.is_double()) {
        value = mode.GetDouble();
      } else if (mode.is_int()) {
        value = mode.GetInt();
      } else {
        continue;
      }
      if (value >= 0.1 && value <= 2.0) {
        parsed_modes.push_back(value);
      }
    }
    // An empty/invalid array must not turn the shortcut into a no-op.
    if (!parsed_modes.empty()) {
      next.column_width_modes = std::move(parsed_modes);
    }
  }
  geti("header_height", next.header_height);
  if (auto collapsed = d.FindBool("sidebar_collapsed")) {
    next.sidebar_mode = *collapsed ? sidebar_metrics::SidebarMode::kHidden
                                   : sidebar_metrics::SidebarMode::kExpanded;
  }
  if (const std::string* mode = d.FindString("sidebar_mode")) {
    if (auto parsed = ParseSidebarMode(*mode)) {
      next.sidebar_mode = *parsed;
    }
  }
  if (const std::string* position = d.FindString("sidebar_position")) {
    if (*position == "left") {
      next.sidebar_position = SidebarPosition::kLeft;
    } else if (*position == "right") {
      next.sidebar_position = SidebarPosition::kRight;
    }
  }
  geti("focus_border", next.focus_border);
  getb("animations", next.animations);
  geti("animation_ms", next.animation_ms);
  gets("accent_color", next.accent_color, kLayoutDefaultAccentColor,
       /*allow_empty=*/false);
  gets("drop_highlight_color", next.drop_highlight_color,
       kLayoutDefaultDropHighlightColor, /*allow_empty=*/false);
  gets("focus_color", next.focus_color, kLayoutDefaultFocusColor,
       /*allow_empty=*/false);
  gets("chrome_tab_bar_color", next.chrome_tab_bar_color,
       kLayoutDefaultChromeSurfaceColor, /*allow_empty=*/true);
  gets("chrome_toolbar_color", next.chrome_toolbar_color,
       kLayoutDefaultChromeSurfaceColor, /*allow_empty=*/true);
  gets("chrome_omnibox_color", next.chrome_omnibox_color,
       kLayoutDefaultChromeSurfaceColor, /*allow_empty=*/true);
  gets("chrome_omnibox_popup_color", next.chrome_omnibox_popup_color,
       kLayoutDefaultChromeSurfaceColor, /*allow_empty=*/true);
  gets("chrome_omnibox_popup_hover_color",
       next.chrome_omnibox_popup_hover_color, kLayoutDefaultChromeSurfaceColor,
       /*allow_empty=*/true);
  getb("ghostty_theme", next.ghostty_theme);
  if (const std::string* theme_name = d.FindString("ghostty_theme_name")) {
    next.ghostty_theme_name = SanitizedThemeName(*theme_name);
  }
  getb("toolbar_show_back", next.toolbar_show_back);
  getb("toolbar_show_forward", next.toolbar_show_forward);
  getb("toolbar_show_reload", next.toolbar_show_reload);
  getb("toolbar_show_home", next.toolbar_show_home);
  getb("toolbar_show_extensions", next.toolbar_show_extensions);
  getb("toolbar_show_downloads", next.toolbar_show_downloads);
  getb("toolbar_show_media", next.toolbar_show_media);
  getb("toolbar_show_profile", next.toolbar_show_profile);
  getb("toolbar_show_menu", next.toolbar_show_menu);
  next.accent_color =
      SanitizedColorString("accent_color", next.accent_color,
                           kLayoutDefaultAccentColor, /*allow_empty=*/false);
  next.drop_highlight_color = SanitizedColorString(
      "drop_highlight_color", next.drop_highlight_color,
      kLayoutDefaultDropHighlightColor, /*allow_empty=*/false);
  next.focus_color =
      SanitizedColorString("focus_color", next.focus_color,
                           kLayoutDefaultFocusColor, /*allow_empty=*/false);
  auto sanitize_optional_color = [&](const char* key, std::string* value) {
    *value = SanitizedColorString(key, *value, kLayoutDefaultChromeSurfaceColor,
                                  /*allow_empty=*/true);
  };
  sanitize_optional_color("chrome_tab_bar_color", &next.chrome_tab_bar_color);
  sanitize_optional_color("chrome_toolbar_color", &next.chrome_toolbar_color);
  sanitize_optional_color("chrome_omnibox_color", &next.chrome_omnibox_color);
  sanitize_optional_color("chrome_omnibox_popup_color",
                          &next.chrome_omnibox_popup_color);
  sanitize_optional_color("chrome_omnibox_popup_hover_color",
                          &next.chrome_omnibox_popup_hover_color);
  *out = next;
  return true;
}

bool BrowserConfig::FromJson(const std::string& json, BrowserConfig* out) {
  if (!out) {
    return false;
  }
  std::optional<base::Value> value =
      base::JSONReader::Read(json, base::JSON_PARSE_CHROMIUM_EXTENSIONS |
                                       base::JSON_ALLOW_TRAILING_COMMAS);
  if (!value || !value->is_dict()) {
    return false;
  }

  BrowserConfig next = *out;
  const base::DictValue* browser = value->GetDict().FindDict("browser");
  if (browser) {
    if (const std::string* page = browser->FindString("newTabPage")) {
      next.new_tab_page = SanitizedNewTabPage(*page);
    }
  }
  if (const base::Value* app_value = value->GetDict().Find("app")) {
    if (!app_value->is_dict()) {
      return false;
    }
    if (const base::Value* telemetry =
            app_value->GetDict().Find("sendAnonymousTelemetry")) {
      if (!telemetry->is_bool()) {
        return false;
      }
      next.send_anonymous_telemetry = telemetry->GetBool();
    }
  }
  next.new_tab_page = SanitizedNewTabPage(next.new_tab_page);
  *out = std::move(next);
  return true;
}

bool ParseLayoutHexColor(std::string_view text, SkColor* out) {
  if (!out || text.size() != 7 || text[0] != '#') {
    return false;
  }
  int r = 0;
  int g = 0;
  int b = 0;
  if (!HexByte(text, 1, &r) || !HexByte(text, 3, &g) || !HexByte(text, 5, &b)) {
    return false;
  }
  *out = SkColorSetRGB(r, g, b);
  return true;
}

SkColor LayoutAccentColor(const LayoutConfig& config) {
  SkColor color;
  if (ParseLayoutHexColor(config.accent_color, &color)) {
    return color;
  }
  LOG(WARNING) << "cmux-layout: invalid accent_color \"" << config.accent_color
               << "\"; using default " << kLayoutDefaultAccentColor;
  return kDefaultAccentSkColor;
}

SkColor LayoutDropHighlightColor(const LayoutConfig& config) {
  SkColor color;
  if (ParseLayoutHexColor(config.drop_highlight_color, &color)) {
    return color;
  }
  LOG(WARNING) << "cmux-layout: invalid drop_highlight_color \""
               << config.drop_highlight_color << "\"; using default "
               << kLayoutDefaultDropHighlightColor;
  return kDefaultDropHighlightSkColor;
}

SkColor LayoutFocusColor(const LayoutConfig& config) {
  SkColor color;
  if (ParseLayoutHexColor(config.focus_color, &color)) {
    return color;
  }
  LOG(WARNING) << "cmux-layout: invalid focus_color \"" << config.focus_color
               << "\"; using default " << kLayoutDefaultFocusColor;
  return kDefaultFocusSkColor;
}

std::string LayoutConfigPath() {
  if (const char* p = std::getenv("CMUX_LAYOUT_CONFIG")) {
    if (*p) {
      return std::string(p);
    }
  }
  base::FilePath home;
  if (base::PathService::Get(base::DIR_HOME, &home)) {
    return home.AppendASCII(".cmux_layout.json").AsUTF8Unsafe();
  }
  return "/tmp/.cmux_layout.json";
}

std::string CmuxConfigPath() {
  const std::optional<base::FilePath> path =
      ResolveCmuxConfigPath(/*require_absolute_config=*/false);
  return path ? path->AsUTF8Unsafe() : std::string();
}

void LoadLayoutConfigAsync(
    LayoutConfig defaults,
    base::OnceCallback<void(std::optional<LayoutConfig>)> callback) {
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(&LoadLayoutConfigOnBackgroundSequence,
                     std::move(defaults)),
      std::move(callback));
}

void LoadBrowserConfigAsync(
    base::OnceCallback<void(std::optional<BrowserConfig>)> callback) {
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(&LoadBrowserConfigOnBackgroundSequence),
      std::move(callback));
}

void LoadBrowserConfigWithStatusAsync(
    base::OnceCallback<void(BrowserConfigLoadResult)> callback) {
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(&LoadBrowserConfigWithStatusOnBackgroundSequence),
      std::move(callback));
}

bool SaveLayoutConfig(const LayoutConfig& config) {
  LayoutConfigWriter().WriteNow(config.ToJson());
  PublishLayoutConfig(config);
  return true;
}

std::optional<LayoutConfig> GetPublishedLayoutConfig() {
  return PublishedConfig().value;
}

base::CallbackListSubscription AddLayoutConfigChangedCallback(
    LayoutConfigChangedCallback callback) {
  return PublishedConfig().callbacks.Add(std::move(callback));
}

void PublishLayoutConfig(const LayoutConfig& config) {
  PublishedConfig().value = config;
  PublishedConfig().callbacks.Notify(config);
}

}  // namespace cmux
