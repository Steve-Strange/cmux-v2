// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "chrome/browser/cmux_term/cmux_update_network.h"

#include "net/base/network_change_notifier.h"

namespace cmux {
namespace {

UpdateConnectionCost ToCmuxCost(
    net::NetworkChangeNotifier::ConnectionCost cost) {
  switch (cost) {
    case net::NetworkChangeNotifier::CONNECTION_COST_METERED:
      return UpdateConnectionCost::kMetered;
    case net::NetworkChangeNotifier::CONNECTION_COST_UNMETERED:
      return UpdateConnectionCost::kUnmetered;
    case net::NetworkChangeNotifier::CONNECTION_COST_UNKNOWN:
    case net::NetworkChangeNotifier::CONNECTION_COST_LAST:
      return UpdateConnectionCost::kUnknown;
  }
  return UpdateConnectionCost::kUnknown;
}

class WindowsUpdateNetworkMonitor final
    : public CmuxUpdateNetworkMonitor,
      public net::NetworkChangeNotifier::ConnectionCostObserver {
 public:
  WindowsUpdateNetworkMonitor() = default;
  ~WindowsUpdateNetworkMonitor() override {
    if (observing_) {
      net::NetworkChangeNotifier::RemoveConnectionCostObserver(this);
    }
  }

  void Start(Callback callback) override {
    callback_ = std::move(callback);
    net::NetworkChangeNotifier::AddConnectionCostObserver(this);
    observing_ = true;
    callback_.Run(
        ToCmuxCost(net::NetworkChangeNotifier::GetConnectionCost()));
  }

  void OnConnectionCostChanged(
      net::NetworkChangeNotifier::ConnectionCost cost) override {
    callback_.Run(ToCmuxCost(cost));
  }

 private:
  Callback callback_;
  bool observing_ = false;
};

}  // namespace

std::unique_ptr<CmuxUpdateNetworkMonitor> CreateCmuxUpdateNetworkMonitor() {
  return std::make_unique<WindowsUpdateNetworkMonitor>();
}

}  // namespace cmux
