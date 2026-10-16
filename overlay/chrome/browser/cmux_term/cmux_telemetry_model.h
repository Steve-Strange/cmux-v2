// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef CHROME_BROWSER_CMUX_TERM_CMUX_TELEMETRY_MODEL_H_
#define CHROME_BROWSER_CMUX_TERM_CMUX_TELEMETRY_MODEL_H_

#include <string>
#include <string_view>

namespace cmux {

struct ActiveBucketDecision {
  bool capture_daily = false;
  bool capture_hourly = false;
};

// Determines which deduplicated activity events are due. Buckets use UTC
// strings (`YYYY-MM-DD` and `YYYY-MM-DDTHH`) so the result is stable across
// daylight-saving and local time-zone changes.
ActiveBucketDecision DueActiveBuckets(std::string_view last_day_utc,
                                      std::string_view last_hour_utc,
                                      std::string_view current_day_utc,
                                      std::string_view current_hour_utc);

// Returns the stable PostHog insertion ID for one install/event/UTC bucket.
// Retrying a request after an ambiguous network failure therefore cannot count
// the same active bucket twice.
std::string ActiveEventInsertId(std::string_view distinct_id,
                                std::string_view event_name,
                                std::string_view bucket);

// Returns a deterministic RFC 9562 UUIDv8 for PostHog's top-level event UUID.
// PostHog uses this field to deduplicate ambiguous retries and later launches.
std::string ActiveEventUuid(std::string_view distinct_id,
                            std::string_view event_name,
                            std::string_view bucket);

}  // namespace cmux

#endif  // CHROME_BROWSER_CMUX_TERM_CMUX_TELEMETRY_MODEL_H_
