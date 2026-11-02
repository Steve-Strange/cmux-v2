#!/usr/bin/env python3
"""Execute the SSH link manager's lifecycle with an isolated carrier."""

from pathlib import Path
import os
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parent.parent


class SSHSharedCarrierPauseTests(unittest.TestCase):
    def test_disconnect_blocks_implicit_carrier_restart(self):
        environment = {key: value for key, value in os.environ.items()
                       if key in {"PATH", "HOME", "TMPDIR", "DEVELOPER_DIR"}}
        with tempfile.TemporaryDirectory(prefix="cmux-ssh-carrier-pause-") as directory:
            build = Path(directory)
            for module in ["CmuxCore", "CmuxCloudTui"]:
                source = build / f"{module}.swift"
                source.write_text("// Empty dependency for the isolated link host.\n")
                self.compile(environment, ["-emit-module", "-module-name", module,
                    str(source), "-emit-module-path", str(build / f"{module}.swiftmodule")])
            self.compile(environment, ["-emit-module", "-emit-library", "-module-name", "CmuxCloud",
                str(ROOT / "tests/fixtures/SSHSharedCarrierCloudFixture.swift"),
                "-o", str(build / "libCmuxCloud.dylib"),
                "-emit-module-path", str(build / "CmuxCloud.swiftmodule")])
            sources = [ROOT / "Sources/RemoteTui/SSHTuiLinkManager.swift"]
            intent = ROOT / "Sources/RemoteTui/SSHTuiTransportIntent.swift"
            if intent.exists():
                sources.append(intent)
            self.compile(environment, ["-parse-as-library", "-I", str(build), "-L", str(build),
                "-lCmuxCloud", "-Xlinker", "-rpath", "-Xlinker", str(build),
                *map(str, sources), str(ROOT / "tests/fixtures/SSHSharedCarrierPauseFixture.swift"),
                "-o", str(build / "probe")])
            result = subprocess.run([str(build / "probe")], capture_output=True, text=True, env=environment)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def compile(self, environment, arguments):
        result = subprocess.run(["xcrun", "swiftc", "-swift-version", "6", *arguments],
            capture_output=True, text=True, env=environment)
        self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()
