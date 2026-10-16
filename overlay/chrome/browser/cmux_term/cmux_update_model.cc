// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "chrome/browser/cmux_term/cmux_update_model.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <limits>
#include <string_view>
#include <vector>

namespace cmux {
namespace {

struct ParsedVersion {
  std::vector<uint64_t> parts;
  bool prerelease = false;
};

ParsedVersion ParseVersion(std::string_view value) {
  ParsedVersion parsed;
  size_t end = value.find_first_of("-+");
  if (end != std::string_view::npos) {
    parsed.prerelease = value[end] == '-';
    value = value.substr(0, end);
  }
  size_t start = 0;
  while (start <= value.size()) {
    const size_t dot = value.find('.', start);
    const std::string_view token = value.substr(
        start, dot == std::string_view::npos ? value.size() - start
                                             : dot - start);
    uint64_t number = 0;
    bool valid = !token.empty();
    for (char c : token) {
      if (!std::isdigit(static_cast<unsigned char>(c))) {
        valid = false;
        break;
      }
      const uint64_t digit = static_cast<uint64_t>(c - '0');
      if (number > (std::numeric_limits<uint64_t>::max() - digit) / 10) {
        number = std::numeric_limits<uint64_t>::max();
      } else {
        number = number * 10 + digit;
      }
    }
    parsed.parts.push_back(valid ? number : 0);
    if (dot == std::string_view::npos) {
      break;
    }
    start = dot + 1;
  }
  while (parsed.parts.size() > 1 && parsed.parts.back() == 0) {
    parsed.parts.pop_back();
  }
  return parsed;
}

}  // namespace

int CompareUpdateVersions(const std::string& left, const std::string& right) {
  const ParsedVersion a = ParseVersion(left);
  const ParsedVersion b = ParseVersion(right);
  const size_t count = std::max(a.parts.size(), b.parts.size());
  for (size_t i = 0; i < count; ++i) {
    const uint64_t av = i < a.parts.size() ? a.parts[i] : 0;
    const uint64_t bv = i < b.parts.size() ? b.parts[i] : 0;
    if (av != bv) {
      return av < bv ? -1 : 1;
    }
  }
  if (a.prerelease != b.prerelease) {
    return a.prerelease ? -1 : 1;
  }
  return 0;
}

bool MayDownloadUpdate(UpdateConnectionCost cost) {
  return cost == UpdateConnectionCost::kUnmetered;
}

bool ShouldShowUpdateNow(UpdateState state) {
  return state == UpdateState::kReady;
}

}  // namespace cmux
