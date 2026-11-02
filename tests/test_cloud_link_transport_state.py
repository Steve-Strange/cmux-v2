#!/usr/bin/env python3
"""Run the production link against a controllable headless child process.

Only the control-service boundary is substituted. Process lifetime, stdout
parsing, link state, event forwarding and socket reuse execute production code.
"""

import os
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parent.parent


class CloudLinkTransportStateTests(unittest.TestCase):
    def test_transport_state_preserves_the_recovering_carrier(self):
        environment = {key: value for key, value in os.environ.items()
                       if key in {"PATH", "HOME", "TMPDIR", "DEVELOPER_DIR"}}
        with tempfile.TemporaryDirectory(prefix="cmux-link-state-build-") as directory:
            build = Path(directory)
            for module in ["CmuxCore", "CmuxCloudTui", "CmuxSurfaceCatalogModel"]:
                source = build / f"{module}.swift"
                source.write_text("// Dependency boundary for the isolated link process test.\n")
                self.compile(environment, ["-emit-module", "-module-name", module,
                    str(source), "-emit-module-path", str(build / f"{module}.swiftmodule")])
            self.compile(environment, ["-parse-as-library", "-I", str(build),
                str(ROOT / "Packages/macOS/CmuxCloud/Sources/CmuxCloud/Link/CloudMachineLink.swift"),
                str(ROOT / "Packages/macOS/CmuxCloud/Sources/CmuxCloud/Link/CloudMachineLink+StderrDrain.swift"),
                str(ROOT / "Packages/macOS/CmuxCloud/Sources/CmuxCloud/Link/CloudMachineLinkEventsRecoveryPolicy.swift"),
                str(ROOT / "tests/fixtures/CloudLinkTransportBoundaryFixture.swift"),
                str(ROOT / "tests/fixtures/CloudLinkTransportStateFixture.swift"),
                "-o", str(build / "probe")])
            result = subprocess.run([str(build / "probe")], capture_output=True, text=True,
                                    env=environment, timeout=25)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def compile(self, environment, arguments):
        # Match CmuxCloud's declared language mode; the inherited pipe reader
        # predates strict Sendable checking and is not changed by this repair.
        result = subprocess.run(["xcrun", "swiftc", "-swift-version", "5", *arguments],
                                capture_output=True, text=True, env=environment)
        self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()
