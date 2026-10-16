// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef CHROME_BROWSER_CMUX_TERM_CMUX_UPDATE_MODEL_H_
#define CHROME_BROWSER_CMUX_TERM_CMUX_UPDATE_MODEL_H_

#include <string>

namespace cmux {

// Deliberately Chromium-free so the update policy is exercised by the host
// test suite on macOS, Windows, and Linux.
enum class UpdateState {
  kIdle,
  kChecking,
  kWaitingForUnmetered,
  kDownloading,
  kVerifying,
  kReady,
  kApplying,
  kFailed,
};

enum class UpdateConnectionCost {
  kUnknown,
  kMetered,
  kUnmetered,
};

// Versions are dot-separated non-negative integers. A '-' prerelease suffix
// sorts before the corresponding release; '+' build metadata is ignored.
// Returns -1, 0, or 1.
int CompareUpdateVersions(const std::string& left, const std::string& right);

// Package bytes may flow only on an explicitly unmetered connection. Unknown
// is intentionally conservative because neither a hotspot nor its cost should
// be guessed from "Wi-Fi" alone.
bool MayDownloadUpdate(UpdateConnectionCost cost);

// The update action is a strict readiness gate: merely finding or downloading
// an update must never expose an install control.
bool ShouldShowUpdateNow(UpdateState state);

}  // namespace cmux

#endif  // CHROME_BROWSER_CMUX_TERM_CMUX_UPDATE_MODEL_H_
