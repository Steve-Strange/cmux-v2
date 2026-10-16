// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef CHROME_BROWSER_CMUX_TERM_CMUX_TELEMETRY_H_
#define CHROME_BROWSER_CMUX_TERM_CMUX_TELEMETRY_H_

namespace cmux {

// Starts anonymous PostHog activity telemetry after Chromium's system network
// context is available. Startup is idempotent. Crashpad independently reads
// the same app.sendAnonymousTelemetry setting synchronously before its handler
// starts, so startup reports cannot race the privacy decision.
void StartCmuxTelemetry();
void StopCmuxTelemetry();

}  // namespace cmux

#endif  // CHROME_BROWSER_CMUX_TERM_CMUX_TELEMETRY_H_
