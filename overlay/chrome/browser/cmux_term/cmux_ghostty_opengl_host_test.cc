// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later

// Exercise the real Chromium adapter: closing one pane must leave the other
// panes' EGL contexts usable, including after resizing and creating a new pane.
#include "chrome/browser/cmux_term/cmux_ghostty_opengl_host.h"

#include <EGL/egl.h>

#include <array>
#include <cstdio>

#include "base/at_exit.h"
#include "base/allocator/partition_alloc_support.h"
#include "base/check.h"
#include "base/command_line.h"
#include "base/feature_list.h"

namespace {

void Draw(cmux::CmuxGhosttyOpenGLHost& host) {
  ghostty_surface_config_s config = {};
  host.PopulateSurfaceConfig(&config);
  const auto& gl = config.platform.opengl;
  CHECK(gl.make_current(gl.userdata))
      << "A live pane lost its EGL context; EGL error " << eglGetError();
  gl.swap_buffers(gl.userdata);
  gl.clear_current(gl.userdata);
}

}  // namespace

int main(int argc, char** argv) {
  base::AtExitManager at_exit;
  base::CommandLine::Init(argc, argv);
  base::allocator::PartitionAllocSupport::Get()->ReconfigureForTests();
  base::FeatureList::InitInstance("PartitionAllocDanglingPtr", "");
  base::allocator::PartitionAllocSupport::Get()->ReconfigureAfterFeatureListInit(
      "");
  for (int cycle = 0; cycle < 30; ++cycle) {
    std::array<std::unique_ptr<cmux::CmuxGhosttyOpenGLHost>, 3> hosts;
    for (auto& host : hosts) {
      host = cmux::CmuxGhosttyOpenGLHost::Create(64, 64, {});
      CHECK(host);
      Draw(*host);
    }
    hosts[1].reset();
    Draw(*hosts[0]);
    Draw(*hosts[2]);
    hosts[1] = cmux::CmuxGhosttyOpenGLHost::Create(64, 64, {});
    CHECK(hosts[1]);
    hosts[0]->SetSize(96, 80);
    Draw(*hosts[0]);
    Draw(*hosts[0]);
    hosts[2].reset();
    Draw(*hosts[0]);
    Draw(*hosts[1]);
    hosts[0].reset();
    Draw(*hosts[1]);
    hosts[1].reset();
    std::printf("PASS OpenGL host lifecycle cycle %d\n", cycle + 1);
  }
  std::puts("PASS 120 real Chromium OpenGL hosts created and released");
}
