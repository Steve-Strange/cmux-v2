// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "chrome/browser/cmux_term/cmux_update_model.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

int checks = 0;

void Check(bool condition, const std::string& message) {
  ++checks;
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}

}  // namespace

int main() {
  using cmux::CompareUpdateVersions;
  using cmux::MayDownloadUpdate;
  using cmux::ShouldShowUpdateNow;
  using cmux::UpdateConnectionCost;
  using cmux::UpdateState;

  Check(CompareUpdateVersions("138.0.7204.100", "138.0.7204.99") > 0,
        "numeric components are compared numerically");
  Check(CompareUpdateVersions("2.0", "2.0.0") == 0,
        "trailing zero components are equal");
  Check(CompareUpdateVersions("2.0-beta", "2.0") < 0,
        "a prerelease sorts before its release");
  Check(CompareUpdateVersions("2.0+build7", "2.0") == 0,
        "build metadata does not create an update loop");

  Check(!MayDownloadUpdate(UpdateConnectionCost::kMetered),
        "metered connections block package bytes");
  Check(!MayDownloadUpdate(UpdateConnectionCost::kUnknown),
        "unknown cost is conservatively blocked");
  Check(MayDownloadUpdate(UpdateConnectionCost::kUnmetered),
        "explicitly unmetered connections allow background download");

  for (UpdateState state : {UpdateState::kIdle, UpdateState::kChecking,
                            UpdateState::kWaitingForUnmetered,
                            UpdateState::kDownloading, UpdateState::kVerifying,
                            UpdateState::kApplying, UpdateState::kFailed}) {
    Check(!ShouldShowUpdateNow(state),
          "Update now remains hidden before verified readiness");
  }
  Check(ShouldShowUpdateNow(UpdateState::kReady),
        "Update now is visible only for a verified complete package");

  std::cout << "cmux_update_model_test: " << checks << " checks\n";
  return 0;
}
