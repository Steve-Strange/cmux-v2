#!/usr/bin/env python3
"""Run the production attachment retry against an isolated provider host."""

from pathlib import Path
import os
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parent.parent
CLOUD = ROOT / "Packages/macOS/CmuxCloud/Sources/CmuxCloud/Surfaces"


class SSHAttachmentReconnectIntentTests(unittest.TestCase):
    def test_queued_retry_obeys_current_user_intent(self):
        environment = {
            key: value for key, value in os.environ.items()
            if key in {"PATH", "HOME", "TMPDIR", "DEVELOPER_DIR"}
        }
        with tempfile.TemporaryDirectory(prefix="cmux-ssh-retry-intent-") as directory:
            build = Path(directory)
            compiled = subprocess.run([
                "xcrun", "swiftc", "-swift-version", "6", "-emit-module", "-emit-library",
                "-module-name", "CmuxCloud",
                str(CLOUD / "CloudTerminalAttachmentRetryPolicy.swift"),
                str(CLOUD / "CloudTerminalAttachmentRetryScheduler.swift"),
                str(CLOUD / "CloudProviderRefreshCoordinator.swift"),
                "-o", str(build / "libCmuxCloud.dylib"),
                "-emit-module-path", str(build / "CmuxCloud.swiftmodule"),
            ], capture_output=True, text=True, env=environment)
            self.assertEqual(compiled.returncode, 0, compiled.stderr)
            compiled = subprocess.run([
                "xcrun", "swiftc", "-swift-version", "6", "-parse-as-library",
                "-I", str(build), "-L", str(build), "-lCmuxCloud",
                "-Xlinker", "-rpath", "-Xlinker", str(build),
                str(ROOT / "Sources/Surfaces/CmuxTuiSurfaceProvider+AttachmentRecovery.swift"),
                str(ROOT / "cmuxTests/CloudReadManualClock.swift"),
                str(ROOT / "tests/fixtures/SSHAttachmentReconnectIntentFixture.swift"),
                "-o", str(build / "probe"),
            ], capture_output=True, text=True, env=environment)
            self.assertEqual(compiled.returncode, 0, compiled.stderr)
            result = subprocess.run(
                [str(build / "probe")], capture_output=True, text=True, env=environment
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
