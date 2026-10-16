// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "chrome/browser/cmux_term/cmux_telemetry_model.h"

#include <array>
#include <cstdint>
#include <string>

namespace cmux {
namespace {

uint64_t StableHash64(std::string_view value, uint64_t seed) {
  constexpr uint64_t kFnvPrime = 1099511628211ULL;
  uint64_t hash = seed;
  for (char character : value) {
    hash ^= static_cast<unsigned char>(character);
    hash *= kFnvPrime;
  }
  // Avalanche the FNV state so adjacent UTC buckets spread across every byte.
  hash ^= hash >> 33;
  hash *= 0xff51afd7ed558ccdULL;
  hash ^= hash >> 33;
  hash *= 0xc4ceb9fe1a85ec53ULL;
  hash ^= hash >> 33;
  return hash;
}

}  // namespace

ActiveBucketDecision DueActiveBuckets(std::string_view last_day_utc,
                                      std::string_view last_hour_utc,
                                      std::string_view current_day_utc,
                                      std::string_view current_hour_utc) {
  ActiveBucketDecision decision;
  decision.capture_daily =
      !current_day_utc.empty() && last_day_utc != current_day_utc;
  decision.capture_hourly =
      !current_hour_utc.empty() && last_hour_utc != current_hour_utc;
  return decision;
}

std::string ActiveEventInsertId(std::string_view distinct_id,
                                std::string_view event_name,
                                std::string_view bucket) {
  std::string insert_id = "cmux-browser:";
  insert_id.append(distinct_id);
  insert_id.push_back(':');
  insert_id.append(event_name);
  insert_id.push_back(':');
  insert_id.append(bucket);
  return insert_id;
}

std::string ActiveEventUuid(std::string_view distinct_id,
                            std::string_view event_name,
                            std::string_view bucket) {
  const std::string insertion_id =
      ActiveEventInsertId(distinct_id, event_name, bucket);
  constexpr uint64_t kFnvOffsetBasis = 14695981039346656037ULL;
  constexpr uint64_t kSecondDomainSeed = 7809847782465536322ULL;
  const uint64_t high = StableHash64(insertion_id, kFnvOffsetBasis);
  const uint64_t low = StableHash64(insertion_id, kSecondDomainSeed);

  std::array<uint8_t, 16> bytes;
  for (size_t index = 0; index < 8; ++index) {
    const size_t shift = (7 - index) * 8;
    bytes[index] = static_cast<uint8_t>(high >> shift);
    bytes[index + 8] = static_cast<uint8_t>(low >> shift);
  }
  // RFC 9562 version 8 (application-defined) with the RFC 4122 variant.
  bytes[6] = static_cast<uint8_t>((bytes[6] & 0x0f) | 0x80);
  bytes[8] = static_cast<uint8_t>((bytes[8] & 0x3f) | 0x80);

  constexpr std::array<char, 16> kHex = {
      '0', '1', '2', '3', '4', '5', '6', '7',
      '8', '9', 'a', 'b', 'c', 'd', 'e', 'f',
  };
  std::string uuid;
  uuid.reserve(36);
  for (size_t index = 0; index < bytes.size(); ++index) {
    if (index == 4 || index == 6 || index == 8 || index == 10) {
      uuid.push_back('-');
    }
    uuid.push_back(kHex[bytes[index] >> 4]);
    uuid.push_back(kHex[bytes[index] & 0x0f]);
  }
  return uuid;
}

}  // namespace cmux
