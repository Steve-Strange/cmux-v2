// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef CHROME_BROWSER_CMUX_TERM_CMUX_UPDATE_SERVICE_H_
#define CHROME_BROWSER_CMUX_TERM_CMUX_UPDATE_SERVICE_H_

#include <string>

#include "chrome/browser/cmux_term/cmux_update_model.h"

namespace cmux {

struct UpdateSnapshot {
  UpdateState state = UpdateState::kIdle;
  std::string version;
};

class CmuxUpdateObserver {
 public:
  virtual ~CmuxUpdateObserver() = default;
  virtual void OnCmuxUpdateChanged(const UpdateSnapshot& snapshot) = 0;
};

// Process-global lifecycle. Startup is idempotent and intentionally occurs
// after BrowserProcess has initialized its system network context.
void StartCmuxUpdateService();
void StopCmuxUpdateService();
void AddCmuxUpdateObserver(CmuxUpdateObserver* observer);
void RemoveCmuxUpdateObserver(CmuxUpdateObserver* observer);
UpdateSnapshot GetCmuxUpdateSnapshot();
void ApplyReadyCmuxUpdate();

}  // namespace cmux

#endif  // CHROME_BROWSER_CMUX_TERM_CMUX_UPDATE_SERVICE_H_
