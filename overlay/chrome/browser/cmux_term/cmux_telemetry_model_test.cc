// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "chrome/browser/cmux_term/cmux_telemetry_model.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

int failures = 0;

void Expect(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

}  // namespace

int main() {
  {
    const cmux::ActiveBucketDecision due =
        cmux::DueActiveBuckets("", "", "2026-07-27", "2026-07-27T19");
    Expect(due.capture_daily, "fresh install captures daily activity");
    Expect(due.capture_hourly, "fresh install captures hourly activity");
  }
  {
    const cmux::ActiveBucketDecision due = cmux::DueActiveBuckets(
        "2026-07-27", "2026-07-27T19", "2026-07-27", "2026-07-27T19");
    Expect(!due.capture_daily, "same UTC day is deduplicated");
    Expect(!due.capture_hourly, "same UTC hour is deduplicated");
  }
  {
    const cmux::ActiveBucketDecision due = cmux::DueActiveBuckets(
        "2026-07-27", "2026-07-27T19", "2026-07-27", "2026-07-27T20");
    Expect(!due.capture_daily, "new hour does not duplicate daily activity");
    Expect(due.capture_hourly, "new UTC hour captures hourly activity");
  }
  {
    const cmux::ActiveBucketDecision due = cmux::DueActiveBuckets(
        "2026-07-27", "2026-07-27T23", "2026-07-28", "2026-07-28T00");
    Expect(due.capture_daily, "UTC midnight captures a new daily event");
    Expect(due.capture_hourly, "UTC midnight captures a new hourly event");
  }
  {
    const std::string first = cmux::ActiveEventInsertId(
        "install-id", "cmux_hourly_active", "2026-07-28T00");
    const std::string retry = cmux::ActiveEventInsertId(
        "install-id", "cmux_hourly_active", "2026-07-28T00");
    const std::string next_hour = cmux::ActiveEventInsertId(
        "install-id", "cmux_hourly_active", "2026-07-28T01");
    Expect(first == retry, "retries reuse a deterministic insertion ID");
    Expect(first != next_hour, "new UTC buckets get new insertion IDs");
    Expect(first == "cmux-browser:install-id:cmux_hourly_active:"
                    "2026-07-28T00",
           "insertion ID includes product, install, event, and bucket");

    const std::string event_uuid = cmux::ActiveEventUuid(
        "install-id", "cmux_hourly_active", "2026-07-28T00");
    const std::string retry_uuid = cmux::ActiveEventUuid(
        "install-id", "cmux_hourly_active", "2026-07-28T00");
    const std::string next_uuid = cmux::ActiveEventUuid(
        "install-id", "cmux_hourly_active", "2026-07-28T01");
    Expect(event_uuid == retry_uuid,
           "ambiguous retries reuse a deterministic event UUID");
    Expect(event_uuid != next_uuid,
           "new UTC buckets get new event UUIDs");
    Expect(event_uuid == "728d8e07-e7d1-8bf8-93d1-2c6042bc8264",
           "event UUID remains stable across app versions");
    Expect(event_uuid.size() == 36 && event_uuid[8] == '-' &&
               event_uuid[13] == '-' && event_uuid[14] == '8' &&
               event_uuid[18] == '-' &&
               (event_uuid[19] == '8' || event_uuid[19] == '9' ||
                event_uuid[19] == 'a' || event_uuid[19] == 'b') &&
               event_uuid[23] == '-',
           "event deduplication key is a valid UUIDv8");
  }

  if (failures != 0) {
    return EXIT_FAILURE;
  }
  std::cout << "cmux_telemetry_model_test: PASS\n";
  return EXIT_SUCCESS;
}
