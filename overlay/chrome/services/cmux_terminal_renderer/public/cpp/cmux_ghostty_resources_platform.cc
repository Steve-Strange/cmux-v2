// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "chrome/services/cmux_terminal_renderer/public/cpp/cmux_ghostty_resources.h"

#include <memory>
#include <string>
#include <vector>

#include "base/base_paths.h"
#include "base/environment.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/path_service.h"
#include "build/build_config.h"

#if BUILDFLAG(IS_MAC)
#include "base/apple/bundle_locations.h"
#endif

namespace cmux {
namespace {

bool IsCompleteGhosttyResourcesDirectory(std::string_view directory) {
  const base::FilePath root =
      base::FilePath::FromUTF8Unsafe(std::string(directory));
  if (!base::DirectoryExists(root.AppendASCII("themes")) ||
      !base::DirectoryExists(root.AppendASCII("shell-integration"))) {
    return false;
  }
  base::FilePath terminfo = root.DirName().AppendASCII("terminfo");
#if BUILDFLAG(IS_MAC)
  terminfo = terminfo.AppendASCII("78").AppendASCII("xterm-ghostty");
#elif BUILDFLAG(IS_WIN)
  terminfo = terminfo.AppendASCII("ghostty.terminfo");
#else
  terminfo = terminfo.AppendASCII("x").AppendASCII("xterm-ghostty");
#endif
  return base::PathExists(terminfo);
}

}  // namespace

bool EnsureGhosttyResourcesDirectory() {
  std::unique_ptr<base::Environment> environment = base::Environment::Create();
  const std::optional<std::string> configured =
      environment->GetVar("GHOSTTY_RESOURCES_DIR");
  // In particular, renderer processes on Linux inherit this value from the
  // browser. Honor the documented explicit-value fast path before PathService
  // or any completeness probes touch the filesystem.
  if (configured && !configured->empty()) {
    return true;
  }

  std::string outer_bundle;
  std::vector<std::string> fallbacks;
#if BUILDFLAG(IS_MAC)
  outer_bundle = base::apple::OuterBundlePath().AsUTF8Unsafe();
#else
  base::FilePath executable_directory;
  if (base::PathService::Get(base::DIR_EXE, &executable_directory)) {
    fallbacks.push_back(
        executable_directory.AppendASCII("ghostty").AsUTF8Unsafe());
  }
#endif
  const std::optional<std::string> selected =
      SelectGhosttyResourcesDirectory("", outer_bundle, fallbacks,
                                      IsCompleteGhosttyResourcesDirectory);
  if (!selected) {
    return false;
  }
  environment->SetVar("GHOSTTY_RESOURCES_DIR", *selected);
  return true;
}

}  // namespace cmux
