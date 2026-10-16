// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "chrome/browser/cmux_term/cmux_rail_config.h"

#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "base/base_paths.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/important_file_writer.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/no_destructor.h"
#include "base/path_service.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/stringprintf.h"
#include "base/task/thread_pool.h"
#include "base/values.h"

namespace cmux {

std::string ColorToHex(SkColor c) {
  return base::StringPrintf("#%02x%02x%02x%02x", SkColorGetR(c), SkColorGetG(c),
                            SkColorGetB(c), SkColorGetA(c));
}

SkColor HexToColor(const std::string& in, SkColor fallback) {
  std::string h = in;
  if (!h.empty() && h[0] == '#') {
    h = h.substr(1);
  }
  if (h.size() == 6) {
    h += "ff";  // assume opaque
  }
  uint32_t v = 0;
  if (h.size() != 8 || !base::HexStringToUInt(h, &v)) {
    return fallback;
  }
  return SkColorSetARGB(v & 0xff, (v >> 24) & 0xff, (v >> 16) & 0xff,
                        (v >> 8) & 0xff);
}

namespace {

std::optional<RailConfig> LoadRailConfigOnBackgroundSequence(
    RailConfig defaults) {
  std::string json;
  if (!base::ReadFileToString(base::FilePath::FromUTF8Unsafe(RailConfigPath()),
                              &json)) {
    return std::nullopt;
  }
  RailConfig config = std::move(defaults);
  if (!RailConfig::FromJson(json, &config)) {
    return std::nullopt;
  }
  return config;
}

base::ImportantFileWriter& RailConfigWriter() {
  // Non-const ImportantFileWriter calls stay on the UI sequence; file I/O is
  // delegated to this BLOCK_SHUTDOWN background sequence.
  static base::NoDestructor<base::ImportantFileWriter> writer(
      base::FilePath::FromUTF8Unsafe(RailConfigPath()),
      base::ThreadPool::CreateSequencedTaskRunner(
          {base::MayBlock(), base::TaskShutdownBehavior::BLOCK_SHUTDOWN}));
  return *writer;
}

}  // namespace

RailConfig::RailConfig() = default;
RailConfig::RailConfig(const RailConfig&) = default;
RailConfig& RailConfig::operator=(const RailConfig&) = default;
RailConfig::~RailConfig() = default;

RailConfig RailConfig::Preset(int style) {
  RailConfig c;      // defaults == polished cmux rail (style 0)
  if (style == 1) {  // polished
    c.show_count_badge = true;
    c.sel_bg = SkColorSetA(SK_ColorWHITE, 0x22);
  } else if (style == 2) {  // arc
    c.show_count_badge = true;
    c.bg = SkColorSetRGB(0x17, 0x19, 0x1f);
    c.sel_bg = SkColorSetA(SK_ColorWHITE, 0x22);
  }
  return c;
}

std::string RailConfig::ToJson() const {
  base::DictValue d;
  d.Set("row_height", row_height);
  d.Set("row_gap", row_gap);
  d.Set("top_pad", top_pad);
  d.Set("outer_horizontal_inset", outer_horizontal_inset);
  d.Set("left_pad", left_pad);
  d.Set("right_pad", right_pad);
  d.Set("indent_per_depth", indent_per_depth);
  d.Set("lead_width", lead_width);
  d.Set("row_corner", row_corner);
  d.Set("hover_corner", hover_corner);
  d.Set("pill_inset_v", pill_inset_v);
  d.Set("pill_inset_h", pill_inset_h);
  d.Set("plus_height", plus_height);
  d.Set("header_height", header_height);
  d.Set("traffic_light_clearance", traffic_light_clearance);
  d.Set("header_button_size", header_button_size);
  d.Set("color_dot_size", color_dot_size);
  d.Set("color_dot_gap", color_dot_gap);
  d.Set("scrollbar_width", scrollbar_width);
  d.Set("font_size", font_size);
  d.Set("font_family", font_family);
  d.Set("font_weight", font_weight);
  d.Set("selected_font_weight", selected_font_weight);
  d.Set("folder_bold", folder_bold);
  d.Set("bg", ColorToHex(bg));
  d.Set("sel_bg", ColorToHex(sel_bg));
  d.Set("hover_bg", ColorToHex(hover_bg));
  d.Set("hover_text", ColorToHex(hover_text));
  d.Set("drag_bg", ColorToHex(drag_bg));
  d.Set("text", ColorToHex(text));
  d.Set("sel_text", ColorToHex(sel_text));
  d.Set("folder_text", ColorToHex(folder_text));
  d.Set("chevron", ColorToHex(chevron));
  d.Set("badge_bg", ColorToHex(badge_bg));
  d.Set("badge_text", ColorToHex(badge_text));
  d.Set("close_color", ColorToHex(close_color));
  d.Set("plus_text", ColorToHex(plus_text));
  d.Set("plus_bg", ColorToHex(plus_bg));
  d.Set("plus_hover_bg", ColorToHex(plus_hover_bg));
  d.Set("indent_guide", ColorToHex(indent_guide));
  d.Set("scrollbar_thumb", ColorToHex(scrollbar_thumb));
  d.Set("accent", ColorToHex(accent));
  d.Set("show_close_on_hover", show_close_on_hover);
  d.Set("show_count_badge", show_count_badge);
  d.Set("group_color_chevron", group_color_chevron);
  d.Set("show_header", show_header);
  d.Set("animations", animations);
  d.Set("animation_ms", animation_ms);
  base::DictValue icon_dict;
  for (const auto& [title, glyph] : icons) {
    icon_dict.Set(title, glyph);
  }
  d.Set("icons", std::move(icon_dict));
  std::string out;
  base::JSONWriter::WriteWithOptions(d, base::JSONWriter::OPTIONS_PRETTY_PRINT,
                                     &out);
  return out;
}

bool RailConfig::HasUserKey(std::string_view key) const {
  return present_keys.find(std::string(key)) != present_keys.end();
}

bool RailConfig::FromJson(const std::string& json, RailConfig* out) {
  std::optional<base::Value> v =
      base::JSONReader::Read(json, base::JSON_PARSE_RFC);
  if (!v || !v->is_dict()) {
    return false;
  }
  const base::DictValue& d = v->GetDict();
  out->present_keys.clear();
  for (auto entry : d) {
    out->present_keys.insert(std::string(entry.first));
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
  auto getc = [&](const char* k, SkColor& dst) {
    if (const std::string* s = d.FindString(k)) {
      dst = HexToColor(*s, dst);
    }
  };
  geti("row_height", out->row_height);
  geti("row_gap", out->row_gap);
  geti("top_pad", out->top_pad);
  geti("outer_horizontal_inset", out->outer_horizontal_inset);
  geti("left_pad", out->left_pad);
  geti("right_pad", out->right_pad);
  geti("indent_per_depth", out->indent_per_depth);
  geti("lead_width", out->lead_width);
  geti("row_corner", out->row_corner);
  geti("hover_corner", out->hover_corner);
  geti("pill_inset_v", out->pill_inset_v);
  geti("pill_inset_h", out->pill_inset_h);
  geti("plus_height", out->plus_height);
  geti("header_height", out->header_height);
  geti("traffic_light_clearance", out->traffic_light_clearance);
  geti("header_button_size", out->header_button_size);
  geti("color_dot_size", out->color_dot_size);
  geti("color_dot_gap", out->color_dot_gap);
  geti("scrollbar_width", out->scrollbar_width);
  geti("font_size", out->font_size);
  geti("font_weight", out->font_weight);
  geti("selected_font_weight", out->selected_font_weight);
  if (const std::string* s = d.FindString("font_family")) {
    out->font_family = *s;
  }
  getb("folder_bold", out->folder_bold);
  getc("bg", out->bg);
  getc("sel_bg", out->sel_bg);
  getc("hover_bg", out->hover_bg);
  getc("hover_text", out->hover_text);
  getc("drag_bg", out->drag_bg);
  getc("text", out->text);
  getc("sel_text", out->sel_text);
  getc("folder_text", out->folder_text);
  getc("chevron", out->chevron);
  getc("badge_bg", out->badge_bg);
  getc("badge_text", out->badge_text);
  getc("close_color", out->close_color);
  getc("plus_text", out->plus_text);
  getc("plus_bg", out->plus_bg);
  getc("plus_hover_bg", out->plus_hover_bg);
  getc("indent_guide", out->indent_guide);
  getc("scrollbar_thumb", out->scrollbar_thumb);
  getc("accent", out->accent);
  getb("show_close_on_hover", out->show_close_on_hover);
  getb("show_count_badge", out->show_count_badge);
  getb("group_color_chevron", out->group_color_chevron);
  getb("show_header", out->show_header);
  getb("animations", out->animations);
  geti("animation_ms", out->animation_ms);
  if (const base::DictValue* id = d.FindDict("icons")) {
    out->icons.clear();
    for (const auto [title, glyph] : *id) {
      if (glyph.is_string()) {
        out->icons[title] = glyph.GetString();
      }
    }
  }
  return true;
}

std::string RailConfigPath() {
  if (const char* p = std::getenv("CMUX_RAIL_CONFIG")) {
    if (*p) {
      return std::string(p);
    }
  }
  base::FilePath home;
  if (base::PathService::Get(base::DIR_HOME, &home)) {
    // AppendASCII (not Append): FilePath is wchar_t-based on Windows.
    return home.AppendASCII(".cmux_rail.json").AsUTF8Unsafe();
  }
  return "/tmp/.cmux_rail.json";
}

void LoadRailConfigAsync(
    RailConfig defaults,
    base::OnceCallback<void(std::optional<RailConfig>)> callback) {
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(&LoadRailConfigOnBackgroundSequence, std::move(defaults)),
      std::move(callback));
}

bool SaveRailConfig(const RailConfig& config) {
  RailConfigWriter().WriteNow(config.ToJson());
  return true;
}

}  // namespace cmux
