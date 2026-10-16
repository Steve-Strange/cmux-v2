#!/usr/bin/env python3

from __future__ import annotations

import importlib.util
import fcntl
import json
from pathlib import Path
import stat
import sys
import tempfile
import unittest
from unittest import mock


SCRIPT = Path(__file__).with_name("benchmark-cmux-browser-scaling.py")
SPEC = importlib.util.spec_from_file_location("cmux_scaling_benchmark", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
benchmark = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = benchmark
SPEC.loader.exec_module(benchmark)


class BenchmarkHarnessTest(unittest.TestCase):
    def test_uuid_validation_is_canonical(self) -> None:
        value = "2142caf5-6f4e-4d41-a051-33b21c1a6356"
        self.assertTrue(benchmark.is_canonical_uuid(value))
        self.assertFalse(benchmark.is_canonical_uuid(value.replace("-", "")))
        self.assertFalse(benchmark.is_canonical_uuid("workspace-home"))

    def test_descendants_and_process_categories_are_disjoint(self) -> None:
        table = {
            10: {"ppid": 1, "command": "/App/Contents/Helpers/cmux-tui --headless"},
            11: {"ppid": 10, "command": "/usr/libexec/daemon-helper"},
            20: {"ppid": 1, "command": "/App/Contents/MacOS/cmux"},
            21: {
                "ppid": 20,
                "command": "Chromium --utility-sub-type=cmux.mojom.CmuxTerminalRenderer",
            },
            22: {"ppid": 20, "command": "Chromium --type=renderer"},
            23: {"ppid": 20, "command": "Chromium --type=gpu-process"},
            24: {"ppid": 20, "command": "Chromium --type=utility"},
            30: {"ppid": 1, "command": "cmux-tui __terminal-host"},
            31: {"ppid": 30, "command": "/bin/cat"},
            40: {"ppid": 1, "command": "/App/Contents/Frameworks/crashpad"},
        }
        for item in table.values():
            item.update(rss_kib=1, cpu_percent=0.0, start_token="start")
        topology = benchmark.RunTopology(
            app=Path("/App"),
            socket=Path("/tmp/socket"),
            registry_id="registry",
            generation="generation",
            session="session",
            daemon_pid=10,
            daemon_start_token="start",
            browser_pid=20,
            browser_start_token="start",
            cmux_tui_commit="a" * 40,
            ghostty_commit="b" * 40,
            terminal_host_root=Path("/tmp/hosts"),
            invocation_bundles=("/App",),
        )
        original_hosts = benchmark.recorded_terminal_host_pids
        try:
            benchmark.recorded_terminal_host_pids = lambda _topology, _table: {30}
            categories = benchmark.app_categories(topology, table)
        finally:
            benchmark.recorded_terminal_host_pids = original_hosts

        self.assertEqual(categories["terminal_hosts"], [30])
        self.assertEqual(categories["terminal_children"], [31])
        self.assertEqual(categories["terminal_renderers"], [21])
        self.assertEqual(categories["web_renderers"], [22])
        self.assertEqual(categories["gpu"], [23])
        self.assertEqual(categories["other_app_processes"], [11, 24])
        self.assertEqual(categories["unscoped_same_bundle"], [40])
        scoped = [
            pid
            for name, pids in categories.items()
            if name != "unscoped_same_bundle"
            for pid in pids
        ]
        self.assertEqual(len(scoped), len(set(scoped)))

    def test_state_write_is_atomic_and_lock_is_exclusive(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            state = Path(temporary) / "state.json"
            value = {
                "schema_version": benchmark.SCHEMA_VERSION,
                "app": "/App",
                "socket": "/tmp/socket",
                "registry_id": "registry",
                "source_workspace_key": "source",
                "target_workspace_key": "target",
                "baseline_terminal_ids": [],
                "created_terminal_ids": [],
                "closed_terminal_ids": [],
                "cleanup_complete": False,
                "checkpoints": [],
            }
            with mock.patch.object(
                benchmark.os, "fsync", wraps=benchmark.os.fsync
            ) as synced:
                benchmark.atomic_write(state, value)
            self.assertGreaterEqual(synced.call_count, 2)
            self.assertEqual(stat.S_IMODE(state.stat().st_mode), 0o600)
            self.assertEqual(benchmark.load_state(state), value)
            first = benchmark.acquire_state_lock(state)
            try:
                with self.assertRaises(benchmark.BenchmarkFailure):
                    benchmark.acquire_state_lock(state)
            finally:
                first.close()
            second = benchmark.acquire_state_lock(state)
            second.close()

    def test_session_lock_is_exclusive_across_state_files(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            socket = Path(temporary) / "daemon.sock"
            first = benchmark.acquire_session_lock(socket)
            try:
                with self.assertRaises(benchmark.BenchmarkFailure):
                    benchmark.acquire_session_lock(socket)
            finally:
                first.close()
            second = benchmark.acquire_session_lock(socket)
            second.close()

    def test_cleanup_identity_allows_build_change_but_not_registry_change(self) -> None:
        state = {
            "registry_id": "registry",
            "session": "session",
            "cmux_tui_commit": "a" * 40,
            "ghostty_commit": "b" * 40,
        }
        rebuilt = {
            "protocol": 9,
            "registry_id": "registry",
            "session": "session",
            "build_commit": "c" * 40,
            "ghostty_commit": "d" * 40,
        }
        benchmark.validate_state_identity(state, rebuilt, enforce_build=False)
        with self.assertRaises(benchmark.BenchmarkFailure):
            benchmark.validate_state_identity(state, rebuilt, enforce_build=True)
        rebuilt["registry_id"] = "other"
        with self.assertRaises(benchmark.BenchmarkFailure):
            benchmark.validate_state_identity(state, rebuilt, enforce_build=False)

    def test_external_terminal_drift_is_rejected(self) -> None:
        state = {
            "baseline_terminal_ids": ["baseline"],
            "created_terminal_ids": ["owned"],
            "observed_terminal_ids": ["owned"],
        }
        benchmark.validate_owned_terminal_ids(
            state, {"baseline", "owned"}, active_run=True
        )
        with self.assertRaises(benchmark.BenchmarkFailure):
            benchmark.validate_owned_terminal_ids(
                state, {"baseline", "owned", "external"}, active_run=True
            )
        with self.assertRaises(benchmark.BenchmarkFailure):
            benchmark.validate_owned_terminal_ids(
                state, {"baseline"}, active_run=True
            )

    def test_sampled_resources_rejects_count_churn(self) -> None:
        topology = mock.Mock()
        topology.terminal_host_root = Path("/tmp/unused")
        snapshots = [
            {
                "terminal_hosts": {
                    "count": 2,
                    "rss_total_kib": 2,
                    "cpu_total_percent": 0,
                },
                "terminal_renderers": {
                    "count": 1,
                    "rss_total_kib": 1,
                    "cpu_total_percent": 0,
                },
            },
            {
                "terminal_hosts": {
                    "count": 2,
                    "rss_total_kib": 2,
                    "cpu_total_percent": 0,
                },
                "terminal_renderers": {
                    "count": 2,
                    "rss_total_kib": 2,
                    "cpu_total_percent": 0,
                },
            },
        ]
        with mock.patch.object(benchmark, "resource_snapshot", side_effect=snapshots):
            with self.assertRaises(benchmark.BenchmarkFailure):
                benchmark.sampled_resources(
                    topology,
                    2,
                    0,
                    expected_hosts=2,
                    expected_renderers=1,
                )

    def test_footprint_rejects_silently_omitted_pid(self) -> None:
        topology = mock.Mock()
        categories = {
            "browser_main": [10],
            "cmux_daemon": [11],
            "terminal_hosts": [],
            "terminal_children": [],
            "terminal_renderers": [],
            "web_renderers": [],
            "gpu": [],
            "other_app_processes": [],
            "unscoped_same_bundle": [],
        }
        calls = 0

        def fake_footprint(command: list[str], **_kwargs: object) -> mock.Mock:
            nonlocal calls
            calls += 1
            output = Path(command[2])
            output.write_text(
                '{"total footprint":10,"processes":[{"pid":10,"footprint":10}]}'
            )
            return mock.Mock(returncode=0)

        with (
            mock.patch.object(benchmark, "process_table", return_value={}),
            mock.patch.object(benchmark, "app_categories", return_value=categories),
            mock.patch.object(benchmark.subprocess, "run", side_effect=fake_footprint),
            mock.patch.object(benchmark.time, "sleep"),
        ):
            with self.assertRaises(benchmark.TopologyFailure):
                benchmark.physical_footprint(topology)
        self.assertEqual(calls, benchmark.FOOTPRINT_ATTEMPTS)

    def test_host_exit_check_uses_nonce_identity(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            terminal_id = "terminal"
            record = {
                "terminal_id": terminal_id,
                "incarnation": "incarnation-new",
                "host_pid": 42,
                "host_start_nonce": "nonce-new",
            }
            (root / f"{terminal_id}.json").write_text(json.dumps(record))
            marker_path = root / f"{terminal_id}.incarnation-new-nonce-new.live"
            marker_path.touch()
            with marker_path.open("r+") as marker:
                fcntl.flock(marker.fileno(), fcntl.LOCK_EX)
                table = {42: {"command": "cmux-tui __terminal-host"}}
                old = {
                    **record,
                    "incarnation": "incarnation-old",
                    "host_start_nonce": "nonce-old",
                }
                self.assertFalse(benchmark.exact_host_identity_is_live(root, old, table))
                self.assertTrue(benchmark.exact_host_identity_is_live(root, record, table))

    def test_percentile_uses_nearest_rank(self) -> None:
        values = [1.0, 2.0, 3.0, 4.0, 5.0]
        self.assertEqual(benchmark.percentile(values, 50), 3.0)
        self.assertEqual(benchmark.percentile(values, 95), 5.0)


if __name__ == "__main__":
    unittest.main()
