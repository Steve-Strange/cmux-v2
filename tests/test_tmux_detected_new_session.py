#!/usr/bin/env python3
"""Run production tmux process detection against an isolated attached client."""

from pathlib import Path
import json
import os
import pty
import select
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parent.parent


class TmuxDetectedNewSessionTests(unittest.TestCase):
    def test_attached_new_session_is_saved_as_attach_only(self):
        tmux = shutil.which("tmux") or "/opt/homebrew/bin/tmux"
        self.assertTrue(Path(tmux).is_file(), "tmux is required for this runtime regression")
        tmux = str(Path(tmux).resolve())
        environment = {key: value for key, value in os.environ.items()
                       if key in {"PATH", "HOME", "TMPDIR", "DEVELOPER_DIR"}}
        environment["TERM"] = "xterm-256color"
        with tempfile.TemporaryDirectory(prefix="cmux-tmux-resume-") as directory:
            build = Path(directory)
            # Compile the production extension verbatim; the other vault/agent
            # scanners in its source file are outside this tmux-only boundary.
            source = (ROOT / "Sources/VaultAgentProcessScanner.swift").read_text()
            extension = source.split("extension SurfaceResumeBindingIndex {", 1)[1]
            extension = extension.split("\nprivate struct VaultAgentSessionIDResolution {", 1)[0]
            scanner = build / "TmuxProcessDetection.swift"
            scanner.write_text("import Foundation\nextension SurfaceResumeBindingIndex {" + extension)
            sources = [ROOT / "Sources/TmuxResumeParser.swift", scanner,
                       ROOT / "tests/fixtures/TmuxDetectedNewSessionFixture.swift"]
            resolver = ROOT / "Sources/TmuxAttachedSessionResolver.swift"
            if resolver.exists():
                sources.append(resolver)
            result = subprocess.run(["xcrun", "swiftc", "-swift-version", "6", "-parse-as-library",
                                     *map(str, sources), "-o", str(build / "probe")],
                                    capture_output=True, text=True, env=environment, timeout=60)
            self.assertEqual(result.returncode, 0, result.stderr)

            socket = str(build / "server.sock")
            master, slave = pty.openpty()
            client = subprocess.Popen([tmux, "-S", socket, "-f", "/dev/null", "new", "-s", "cmux"],
                                      stdin=slave, stdout=slave, stderr=slave,
                                      env=environment, start_new_session=True)
            os.close(slave)
            try:
                self.assertTrue(select.select([master], [], [], 5)[0], "tmux never opened its terminal")
                os.read(master, 65536)
                clients = subprocess.run([tmux, "-u", "-N", "-S", socket, "list-clients", "-F",
                                          "#{client_pid}\t#{session_id}\t#{session_name}"],
                                         capture_output=True, text=True, env=environment, timeout=5)
                self.assertEqual(clients.returncode, 0, clients.stderr)
                row = clients.stdout.strip().split("\t")
                self.assertEqual(row[0], str(client.pid), clients.stdout)
                self.assertEqual(row[2], "cmux")
                probe = subprocess.run([str(build / "probe"), tmux, socket, str(client.pid)],
                                       capture_output=True, text=True, env=environment, timeout=5)
                self.assertEqual(probe.returncode, 0, probe.stdout + probe.stderr)
                binding = json.loads(probe.stdout)
                self.assertIsNotNone(binding, "an attached `tmux new -s cmux` was omitted from restore")
                self.assertEqual(binding["checkpointId"], "cmux")
                saved = binding["launchCommand"]["arguments"]
                self.assertEqual(saved, [tmux, "-S", socket, "attach", "-t", row[1]])

                # A real server alone is insufficient: the observed process
                # must still own an attached client on that exact server.
                missing = subprocess.run([str(build / "probe"), tmux, socket, "2147483647"],
                                         capture_output=True, text=True, env=environment, timeout=5)
                self.assertEqual(missing.returncode, 0, missing.stdout + missing.stderr)
                self.assertIsNone(json.loads(missing.stdout), "an unrelated PID borrowed a live tmux session")

                subprocess.run([tmux, "-S", socket, "kill-server"],
                               capture_output=True, env=environment, timeout=5, check=True)
                client.wait(timeout=5)
                restored = subprocess.run(saved, stdin=subprocess.DEVNULL,
                                          capture_output=True, text=True, env=environment, timeout=5)
                self.assertNotEqual(restored.returncode, 0, "restore unexpectedly created a replacement session")
                absent = subprocess.run([tmux, "-N", "-S", socket, "list-sessions"],
                                        capture_output=True, env=environment, timeout=5)
                self.assertNotEqual(absent.returncode, 0, "attach-only restore started a new server")
            finally:
                subprocess.run([tmux, "-N", "-S", socket, "kill-server"],
                               capture_output=True, env=environment, timeout=5)
                if client.poll() is None:
                    client.terminate()
                    client.wait(timeout=5)
                os.close(master)


if __name__ == "__main__":
    unittest.main()
