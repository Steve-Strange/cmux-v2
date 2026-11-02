#!/usr/bin/env python3
"""Execute shared SSH pause/resume against the production browser route models."""

from pathlib import Path
import os
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parent.parent


class SSHBrowserForwardLifecycleTests(unittest.TestCase):
    def test_pause_invalidates_old_listener_and_resume_replaces_it(self):
        environment = {key: value for key, value in os.environ.items()
                       if key in {"PATH", "HOME", "TMPDIR", "DEVELOPER_DIR"}}
        with tempfile.TemporaryDirectory(prefix="cmux-ssh-browser-routes-") as directory:
            build = Path(directory)
            modules = {
                "CmuxCloudBannerCore": [ROOT / "Packages/macOS/CmuxCloudBannerCore/Sources/CmuxCloudBannerCore/CloudTunnelState.swift"],
                "CmuxCore": [ROOT / "Packages/macOS/CmuxCore/Sources/CmuxCore/Remote/RemoteLoopbackProxyAlias.swift"],
                "CmuxCloud": [ROOT / "tests/fixtures/SSHBrowserRoutesCloudFixture.swift",
                    *[ROOT / f"Packages/macOS/CmuxCloud/Sources/CmuxCloud/PortForward/{name}.swift"
                      for name in ["CloudPortAccessKey", "CloudPortAccessRoute", "CloudPortForwardTarget", "CloudPortRoutePolicy"]]],
            }
            for name, sources in modules.items():
                self.compile(environment, ["-emit-module", "-emit-library", "-module-name", name,
                    *map(str, sources), "-o", str(build / f"lib{name}.dylib"),
                    "-emit-module-path", str(build / f"{name}.swiftmodule")])
            empty = build / "CmuxSurfaceCatalogModel.swift"
            empty.write_text("// Value-only provider host supplies the catalog.\n")
            self.compile(environment, ["-emit-module", "-module-name", "CmuxSurfaceCatalogModel",
                str(empty), "-emit-module-path", str(build / "CmuxSurfaceCatalogModel.swiftmodule")])
            sources = [ROOT / path for path in [
                "Sources/Cloud/PortForward/CloudPortAccessModel.swift",
                "Sources/Cloud/PortForward/CloudPortAccessStore.swift",
                "Sources/RemoteTui/SSHTuiTransportIntent.swift",
                "Sources/Surfaces/CmuxTuiSurfaceProvider+SSHTransport.swift",
                "tests/fixtures/SSHBrowserRoutesProviderFixture.swift",
                "tests/fixtures/SSHBrowserRoutesLifecycleFixture.swift",
            ]]
            routes = ROOT / "Sources/Surfaces/CmuxTuiSurfaceProvider+SSHBrowserRoutes.swift"
            if routes.exists():
                sources.append(routes)
            self.compile(environment, ["-parse-as-library", "-I", str(build), "-L", str(build),
                *[f"-l{name}" for name in modules], "-Xlinker", "-rpath", "-Xlinker", str(build),
                *map(str, sources), "-o", str(build / "probe")])
            result = subprocess.run([str(build / "probe")], capture_output=True, text=True,
                                    env=environment, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def compile(self, environment, arguments):
        result = subprocess.run(["xcrun", "swiftc", "-swift-version", "6", *arguments],
            capture_output=True, text=True, env=environment)
        self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()
