// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "chrome/browser/cmux_term/cmux_update_network.h"

#include <string>

#include "base/command_line.h"
#include "base/functional/bind.h"
#include "base/memory/weak_ptr.h"
#include "base/process/launch.h"
#include "base/task/thread_pool.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "net/base/network_change_notifier.h"

namespace cmux {
namespace {

UpdateConnectionCost ProbeNetworkManagerCost() {
  base::CommandLine command(base::FilePath("nmcli"));
  command.AppendArg("--terse");
  command.AppendArg("--fields");
  command.AppendArg("GENERAL.STATE,GENERAL.METERED");
  command.AppendArg("device");
  command.AppendArg("show");
  std::string output;
  if (!base::GetAppOutput(command, &output)) {
    return UpdateConnectionCost::kUnknown;
  }

  bool connected = false;
  bool explicitly_unmetered = false;
  size_t start = 0;
  while (start < output.size()) {
    const size_t end = output.find('\n', start);
    const std::string line = output.substr(
        start, end == std::string::npos ? output.size() - start : end - start);
    if (line.find("GENERAL.STATE:100") != std::string::npos) {
      connected = true;
    }
    if (line.find("GENERAL.METERED:yes") != std::string::npos ||
        line.find("GENERAL.METERED:guess-yes") != std::string::npos) {
      return UpdateConnectionCost::kMetered;
    }
    if (line.find("GENERAL.METERED:no") != std::string::npos ||
        line.find("GENERAL.METERED:guess-no") != std::string::npos) {
      explicitly_unmetered = true;
    }
    if (end == std::string::npos) {
      break;
    }
    start = end + 1;
  }
  return connected && explicitly_unmetered
             ? UpdateConnectionCost::kUnmetered
             : UpdateConnectionCost::kUnknown;
}

class LinuxUpdateNetworkMonitor final
    : public CmuxUpdateNetworkMonitor,
      public net::NetworkChangeNotifier::NetworkChangeObserver {
 public:
  LinuxUpdateNetworkMonitor() = default;
  ~LinuxUpdateNetworkMonitor() override {
    net::NetworkChangeNotifier::RemoveNetworkChangeObserver(this);
  }

  void Start(Callback callback) override {
    callback_ = std::move(callback);
    net::NetworkChangeNotifier::AddNetworkChangeObserver(this);
    timer_.Start(FROM_HERE, base::Seconds(30), this,
                 &LinuxUpdateNetworkMonitor::Probe);
    Probe();
  }

  void OnNetworkChanged(
      net::NetworkChangeNotifier::ConnectionType) override {
    Probe();
  }

 private:
  void Probe() {
    if (probe_in_flight_) {
      return;
    }
    probe_in_flight_ = true;
    base::ThreadPool::PostTaskAndReplyWithResult(
        FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
        base::BindOnce(&ProbeNetworkManagerCost),
        base::BindOnce(&LinuxUpdateNetworkMonitor::OnProbeComplete,
                       weak_factory_.GetWeakPtr()));
  }

  void OnProbeComplete(UpdateConnectionCost cost) {
    probe_in_flight_ = false;
    callback_.Run(cost);
  }

  Callback callback_;
  bool probe_in_flight_ = false;
  base::RepeatingTimer timer_;
  base::WeakPtrFactory<LinuxUpdateNetworkMonitor> weak_factory_{this};
};

}  // namespace

std::unique_ptr<CmuxUpdateNetworkMonitor> CreateCmuxUpdateNetworkMonitor() {
  return std::make_unique<LinuxUpdateNetworkMonitor>();
}

}  // namespace cmux
