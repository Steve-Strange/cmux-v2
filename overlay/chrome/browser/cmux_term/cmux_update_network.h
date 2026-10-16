// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef CHROME_BROWSER_CMUX_TERM_CMUX_UPDATE_NETWORK_H_
#define CHROME_BROWSER_CMUX_TERM_CMUX_UPDATE_NETWORK_H_

#include <memory>

#include "base/functional/callback.h"
#include "chrome/browser/cmux_term/cmux_update_model.h"

namespace cmux {

// Platform adapters report whether a large background transfer is safe. The
// implementations use NWPath's expensive/constrained flags on macOS, Windows'
// Network List Manager cost through Chromium, and NetworkManager's metered
// state on Linux. Unknown always remains blocked by the policy model.
class CmuxUpdateNetworkMonitor {
 public:
  using Callback = base::RepeatingCallback<void(UpdateConnectionCost)>;

  virtual ~CmuxUpdateNetworkMonitor() = default;
  virtual void Start(Callback callback) = 0;
};

std::unique_ptr<CmuxUpdateNetworkMonitor> CreateCmuxUpdateNetworkMonitor();

}  // namespace cmux

#endif  // CHROME_BROWSER_CMUX_TERM_CMUX_UPDATE_NETWORK_H_
