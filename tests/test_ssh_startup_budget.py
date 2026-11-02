#!/usr/bin/env python3
"""Verify the native startup budget reaches a real SSH carrier child argv."""

import os
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parent.parent


class SSHStartupBudgetTests(unittest.TestCase):
    def test_carrier_startup_uses_native_deadline_and_keeps_auth_fast(self):
        environment = {key: value for key, value in os.environ.items()
                       if key in {"PATH", "HOME", "TMPDIR", "DEVELOPER_DIR"}}
        with tempfile.TemporaryDirectory(prefix="cmux-ssh-budget-build-") as directory:
            build = Path(directory)
            for module in ["CmuxCore", "CmuxCloudTui", "CmuxSurfaceCatalogModel", "CmuxFoundation", "CmuxSettings"]:
                source = build / f"{module}.swift"
                source.write_text("// Dependency boundary for the isolated SSH startup test.\n")
                self.compile(environment, ["-emit-module", "-module-name", module,
                    str(source), "-emit-module-path", str(build / f"{module}.swiftmodule")])
            link = ROOT / "Packages/macOS/CmuxCloud/Sources/CmuxCloud/Link"
            self.compile(environment, ["-parse-as-library", "-D", "CMUX_REAL_SSH_ARGUMENTS", "-I", str(build),
                str(link / "CloudMachineLink.swift"), str(link / "CloudMachineLink+StderrDrain.swift"),
                str(link / "CloudMachineLinkEventsRecoveryPolicy.swift"),
                str(link / "SSHTuiConnection.swift"), str(link / "SSHTuiPreflight.swift"),
                str(ROOT / "tests/fixtures/CloudLinkTransportBoundaryFixture.swift"),
                str(ROOT / "tests/fixtures/SSHStartupBudgetFixture.swift"), "-o", str(build / "probe")])
            result = subprocess.run([str(build / "probe")], capture_output=True, text=True,
                                    env=environment, timeout=20)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def compile(self, environment, arguments):
        # Match the production CmuxCloud package's declared language mode.
        result = subprocess.run(["xcrun", "swiftc", "-swift-version", "5", *arguments],
                                capture_output=True, text=True, env=environment, timeout=60)
        self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()
