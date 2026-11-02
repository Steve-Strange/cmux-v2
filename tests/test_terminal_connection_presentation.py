#!/usr/bin/env python3
"""Execute terminal chrome projections against a value-only workspace host."""

from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parent.parent


class TerminalConnectionPresentationTests(unittest.TestCase):
    def test_per_pane_context_and_disconnected_truth(self):
        with tempfile.TemporaryDirectory(prefix="cmux-tab-context-") as directory:
            build = Path(directory)
            core = ROOT / "Packages/macOS/CmuxCore/Sources/CmuxCore/Remote/WorkspaceRemoteConnectionState.swift"
            compiled = subprocess.run([
                "xcrun", "swiftc", "-swift-version", "6", "-emit-module", "-emit-library",
                "-module-name", "CmuxCore", str(core), "-o", str(build / "libCmuxCore.dylib"),
                "-emit-module-path", str(build / "CmuxCore.swiftmodule"),
            ], capture_output=True, text=True)
            self.assertEqual(compiled.returncode, 0, compiled.stderr)
            result = subprocess.run([
                "xcrun", "swiftc", "-swift-version", "6", "-parse-as-library",
                "-I", str(build), "-L", str(build), "-lCmuxCore", "-Xlinker", "-rpath", "-Xlinker", str(build),
                str(ROOT / "Sources/TerminalConnectionPresentation.swift"),
                str(ROOT / "Sources/Workspace+TerminalConnectionPresentation.swift"),
                str(ROOT / "Sources/WorkspaceRemoteConnectionState+DisplayLabel.swift"),
                str(ROOT / "tests/fixtures/TerminalConnectionPresentationFixture.swift"),
                "-o", str(build / "probe"),
            ], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(build / "probe")], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
