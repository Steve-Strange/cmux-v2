// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "chrome/browser/cmux_term/cmux_update_installer.h"

#include <string>

#include "base/command_line.h"
#include "base/files/file_util.h"
#include "base/path_service.h"
#include "base/process/launch.h"
#include "base/process/process_handle.h"
#include "base/strings/string_number_conversions.h"
#include "build/build_config.h"
#include "chrome/browser/cmux_term/cmux_update_script.h"

namespace cmux {
namespace {

base::FilePath CurrentInstallRoot() {
  base::FilePath executable_dir;
  if (!base::PathService::Get(base::DIR_EXE, &executable_dir)) {
    return {};
  }
#if BUILDFLAG(IS_MAC)
  base::FilePath candidate = executable_dir;
  while (!candidate.empty() && candidate != candidate.DirName()) {
    if (candidate.Extension() == FILE_PATH_LITERAL(".app")) {
      return candidate;
    }
    candidate = candidate.DirName();
  }
  return {};
#else
  return executable_dir;
#endif
}

}  // namespace

bool CurrentInstallCanBeReplaced() {
  const base::FilePath current = CurrentInstallRoot();
  return !current.empty() && base::PathExists(current) &&
         base::PathIsWritable(current.DirName());
}

bool LaunchReadyUpdateInstaller(const ReadyUpdate& update) {
  const base::FilePath current = CurrentInstallRoot();
  if (current.empty() || update.staged_root.empty() ||
      !base::PathExists(update.staged_root)) {
    return false;
  }

  const base::FilePath helper_dir = update.staged_root.DirName();
#if BUILDFLAG(IS_WIN)
  const base::FilePath script = helper_dir.Append(FILE_PATH_LITERAL("apply.ps1"));
  if (!base::WriteFile(
          script, BuildUpdateInstallScript(UpdateInstallPlatform::kWindows))) {
    return false;
  }
  base::CommandLine command(base::FilePath(FILE_PATH_LITERAL("powershell.exe")));
  command.AppendArg("-NoProfile");
  command.AppendArg("-NonInteractive");
  command.AppendArg("-ExecutionPolicy");
  command.AppendArg("Bypass");
  command.AppendArg("-File");
  command.AppendArgPath(script);
  command.AppendArg("-ParentPid");
  command.AppendArg(base::NumberToString(base::GetCurrentProcId()));
  command.AppendArg("-Current");
  command.AppendArgPath(current);
  command.AppendArg("-Staged");
  command.AppendArgPath(update.staged_root);
  command.AppendArg("-Relaunch");
  command.AppendArg(update.relaunch_path);
#else
  const base::FilePath script = helper_dir.Append(FILE_PATH_LITERAL("apply.sh"));
  const UpdateInstallPlatform platform =
#if BUILDFLAG(IS_MAC)
      UpdateInstallPlatform::kMac;
#else
      UpdateInstallPlatform::kLinux;
#endif
  if (!base::WriteFile(script, BuildUpdateInstallScript(platform)) ||
      !base::SetPosixFilePermissions(script, 0700)) {
    return false;
  }
  base::CommandLine command(script);
  command.AppendArg(base::NumberToString(base::GetCurrentProcId()));
  command.AppendArgPath(current);
  command.AppendArgPath(update.staged_root);
  command.AppendArg(update.relaunch_path);
#endif

  base::LaunchOptions options;
  options.current_directory = helper_dir;
#if BUILDFLAG(IS_WIN)
  options.start_hidden = true;
#else
  options.new_process_group = true;
#endif
  return base::LaunchProcess(command, options).IsValid();
}

}  // namespace cmux
