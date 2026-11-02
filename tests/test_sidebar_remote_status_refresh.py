#!/usr/bin/env python3
"""Execute the production menu-open sidebar refresh policy without the app."""

from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parent.parent


class SidebarRemoteStatusRefreshTests(unittest.TestCase):
    def test_disconnect_updates_identity_and_actions_while_menu_is_open(self):
        with tempfile.TemporaryDirectory(prefix="cmux-sidebar-status-") as directory:
            build = Path(directory)
            empty_module = build / "CmuxWorkspaces.swift"
            empty_module.write_text("// The policy only needs value types from its host.\n")
            subprocess.run([
                "xcrun", "swiftc", "-emit-module", "-module-name", "CmuxWorkspaces",
                str(empty_module), "-emit-module-path", str(build / "CmuxWorkspaces.swiftmodule"),
            ], check=True, capture_output=True, text=True)
            result = subprocess.run([
                "xcrun", "swiftc", "-swift-version", "6", "-parse-as-library", "-I", str(build),
                str(ROOT / "Sources/Sidebar/SidebarWorkspaceSnapshotRefreshPolicy.swift"),
                str(ROOT / "tests/fixtures/SidebarRemoteStatusRefreshFixture.swift"),
                "-o", str(build / "probe"),
            ], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(build / "probe")], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
