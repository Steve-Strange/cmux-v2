#!/usr/bin/env python3
"""Exercise the macOS utility-service patch across Chromium layout drift."""

import os
from pathlib import Path
import tempfile


ROOT = Path(__file__).resolve().parent.parent
APPLY = (ROOT / "scripts/apply.sh").read_text()
START = APPLY.index("# 3c. Register the one-terminal-per-process")
END = APPLY.index("# 3. chrome/BUILD.gn", START)
PATCH = "MODE = 'full'\n" + APPLY[START:END]


def validate(name: str, non_android_factories: str) -> None:
    services = f'''#include "chrome/utility/services.h"

#if BUILDFLAG(IS_MAC)
#include "chrome/services/mac_notifications/mac_notification_provider_impl.h"
#endif

namespace {{
#if BUILDFLAG(IS_MAC)
auto RunMacNotificationService(
    mojo::PendingReceiver<mac_notifications::mojom::MacNotificationProvider>
        receiver) {{
  return std::make_unique<mac_notifications::MacNotificationProviderImpl>(
      std::move(receiver));
}}
#endif  // BUILDFLAG(IS_MAC)

#if !BUILDFLAG(IS_ANDROID)
{non_android_factories}
auto RunScreenAIServiceFactory(
    mojo::PendingReceiver<screen_ai::mojom::ScreenAIServiceFactory> receiver) {{
  return std::make_unique<screen_ai::ScreenAIService>(std::move(receiver));
}}
#endif
}}  // namespace

void RegisterMainThreadServices(mojo::ServiceFactory& services) {{
#if BUILDFLAG(IS_MAC)
  services.Add(RunMacNotificationService);
#endif  // BUILDFLAG(IS_MAC)
}}
'''
    utility_build = '''source_set("utility") {
  if (is_mac) {
    deps += [
      "//chrome/services/mac_notifications",
    ]
  }
}
'''

    with tempfile.TemporaryDirectory() as temp:
        checkout = Path(temp)
        utility = checkout / "chrome/utility"
        utility.mkdir(parents=True)
        services_path = utility / "services.cc"
        build_path = utility / "BUILD.gn"
        services_path.write_text(services)
        build_path.write_text(utility_build)
        old_cwd = Path.cwd()
        try:
            os.chdir(checkout)
            compiled = compile(PATCH, "apply.sh utility service patch", "exec")
            exec(compiled, {})
            exec(compiled, {})
        finally:
            os.chdir(old_cwd)

        patched = services_path.read_text()
        assert patched.count("auto RunCmuxTerminalRenderer(") == 1
        assert patched.count("services.Add(RunCmuxTerminalRenderer);") == 1
        assert patched.count("cmux_terminal_renderer_service.h") == 1
        assert patched.count("cmux_terminal_renderer.mojom.h") == 1
        mac_factory = '''#if BUILDFLAG(IS_MAC)
auto RunCmuxTerminalRenderer(
    mojo::PendingReceiver<cmux::mojom::CmuxTerminalRenderer> receiver) {
  return std::make_unique<cmux::CmuxTerminalRendererService>(
      std::move(receiver));
}

auto RunMacNotificationService('''
        assert mac_factory in patched
        assert "RunCmuxTerminalRenderer(" not in patched.split(
            "#if !BUILDFLAG(IS_ANDROID)", 1
        )[1]
        registration = '''#if BUILDFLAG(IS_MAC)
  services.Add(RunCmuxTerminalRenderer);
  services.Add(RunMacNotificationService);
#endif  // BUILDFLAG(IS_MAC)'''
        assert registration in patched

        patched_build = build_path.read_text()
        dependency = '"//chrome/services/cmux_terminal_renderer",'
        assert patched_build.count(dependency) == 1
        print(f"PASS {name}")


validate("Chromium 149/150 services layout", "")
validate(
    "Chromium 151 services layout",
    "auto RunReadingModeMetricsService() { return 0; }\n\n",
)
