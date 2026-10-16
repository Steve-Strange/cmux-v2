// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "chrome/browser/cmux_term/cmux_update_network.h"

#include <Network/Network.h>
#include <dispatch/dispatch.h>

#include "base/functional/bind.h"
#include "base/task/sequenced_task_runner.h"

namespace cmux {
namespace {

class MacUpdateNetworkMonitor final : public CmuxUpdateNetworkMonitor {
 public:
  MacUpdateNetworkMonitor() = default;
  ~MacUpdateNetworkMonitor() override {
    if (monitor_) {
      nw_path_monitor_cancel(monitor_);
    }
    if (queue_) {
      dispatch_sync(queue_, ^{});
    }
  }

  void Start(Callback callback) override {
    if (monitor_) {
      return;
    }
    scoped_refptr<base::SequencedTaskRunner> ui_runner =
        base::SequencedTaskRunner::GetCurrentDefault();
    monitor_ = nw_path_monitor_create();
    queue_ = dispatch_queue_create("ai.manaflow.cmux.update-network",
                                   DISPATCH_QUEUE_SERIAL);
    nw_path_monitor_set_update_handler(monitor_, ^(nw_path_t path) {
      UpdateConnectionCost cost = UpdateConnectionCost::kUnknown;
      if (nw_path_get_status(path) == nw_path_status_satisfied) {
        cost = (nw_path_is_expensive(path) || nw_path_is_constrained(path))
                   ? UpdateConnectionCost::kMetered
                   : UpdateConnectionCost::kUnmetered;
      }
      ui_runner->PostTask(FROM_HERE, base::BindOnce(callback, cost));
    });
    nw_path_monitor_set_queue(monitor_, queue_);
    nw_path_monitor_start(monitor_);
  }

 private:
  nw_path_monitor_t __strong monitor_ = nullptr;
  dispatch_queue_t __strong queue_ = nullptr;
};

}  // namespace

std::unique_ptr<CmuxUpdateNetworkMonitor> CreateCmuxUpdateNetworkMonitor() {
  return std::make_unique<MacUpdateNetworkMonitor>();
}

}  // namespace cmux
