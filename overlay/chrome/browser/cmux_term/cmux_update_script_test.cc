// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "chrome/browser/cmux_term/cmux_update_script.h"

#include <cstdlib>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

#if !defined(_WIN32)
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {
int checks = 0;
void Check(bool condition, const char* message) {
  ++checks;
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}
}  // namespace

int main() {
  using cmux::BuildUpdateInstallScript;
  using cmux::UpdateInstallPlatform;
  const std::string mac =
      BuildUpdateInstallScript(UpdateInstallPlatform::kMac);
  const std::string linux =
      BuildUpdateInstallScript(UpdateInstallPlatform::kLinux);
  const std::string windows =
      BuildUpdateInstallScript(UpdateInstallPlatform::kWindows);

  Check(mac.find("open \"$current\"") != std::string::npos,
        "macOS relaunches the replaced app bundle");
  Check(linux.find("nohup \"$current/$relaunch\"") != std::string::npos,
        "Linux relaunches the staged executable");
  Check(windows.find("Wait-Process -Id $ParentPid") != std::string::npos,
        "Windows waits for browser shutdown");
  Check(windows.find("Start-Process -FilePath") != std::string::npos,
        "Windows relaunches in one flow");
  for (const std::string* script : {&mac, &linux, &windows}) {
    Check(script->find("cmux-old") != std::string::npos,
          "installer keeps a rollback copy");
  }
  Check(mac.find("mv -- \"$backup\" \"$current\"") != std::string::npos,
        "POSIX installer rolls back a failed swap");
  Check(windows.find("Move-Item -LiteralPath $backup -Destination $Current") !=
            std::string::npos,
        "Windows installer rolls back a failed swap");

#if !defined(_WIN32)
  // Execute the exact generated POSIX helper against an isolated fake install.
  // This proves the wait/swap/relaunch/cleanup path in addition to inspecting
  // its construction above.
  const std::filesystem::path test_root =
      std::filesystem::temp_directory_path() /
      ("cmux_update_script_test_" + std::to_string(getpid()));
  const std::filesystem::path current = test_root / "current";
  const std::filesystem::path staged = test_root / "staged";
  const std::filesystem::path marker = test_root / "relaunched";
  const std::filesystem::path installer = test_root / "apply.sh";
  std::filesystem::remove_all(test_root);
  std::filesystem::create_directories(current);
  std::filesystem::create_directories(staged);
  std::ofstream(current / "old.txt") << "old\n";
  std::ofstream(staged / "new.txt") << "new\n";
  std::ofstream(staged / "cmux-browser")
      << "#!/bin/sh\n"
      << "touch '" << marker.string() << "'\n";
  std::ofstream(installer) << linux;
  chmod((staged / "cmux-browser").c_str(), 0700);
  chmod(installer.c_str(), 0700);
  const std::string command =
      "'" + installer.string() + "' 2147483647 '" + current.string() +
      "' '" + staged.string() + "' cmux-browser";
  Check(std::system(command.c_str()) == 0,
        "generated POSIX installer completed");
  for (int attempt = 0; attempt < 40 && !std::filesystem::exists(marker);
       ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  Check(std::filesystem::exists(current / "new.txt"),
        "POSIX installer swapped the staged root into place");
  Check(!std::filesystem::exists(current / "old.txt"),
        "POSIX installer removed the old root");
  Check(std::filesystem::exists(marker),
        "POSIX installer relaunched the new executable");
  bool backup_remains = false;
  for (const auto& entry : std::filesystem::directory_iterator(test_root)) {
    if (entry.path().filename().string().find("current.cmux-old") == 0) {
      backup_remains = true;
    }
  }
  Check(!backup_remains, "POSIX installer removed its rollback copy");
  std::filesystem::remove_all(test_root);
#endif

  std::cout << "cmux_update_script_test: " << checks << " checks\n";
  return 0;
}
