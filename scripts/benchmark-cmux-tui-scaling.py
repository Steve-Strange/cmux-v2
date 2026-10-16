#!/usr/bin/env python3
"""Benchmark isolated cmux-tui daemon and terminal-host process scaling.

The benchmark drives the browser JSON-lines protocol directly. It creates one
durable workspace, grows it through the requested terminal-count checkpoints,
and records create-to-running, ping, snapshot, process-count, RSS, and CPU
measurements. Every run uses a new temporary socket and state root; cleanup is
limited to terminal hosts whose PID-reuse-safe liveness records live below that
root.
"""

from __future__ import annotations

import argparse
import errno
import fcntl
import json
import math
import os
from pathlib import Path
import re
import shutil
import signal
import socket
import statistics
import subprocess
import sys
import tempfile
import time
from typing import Any, Callable, TypeVar
import uuid


ROOT = Path(__file__).resolve().parent.parent
REVISION_HEADER = ROOT / "overlay/chrome/browser/cmux_term/cmux_tui_revision.h"
PIN_PATTERN = re.compile(r'"([0-9a-f]{40})"')
ORIGIN = "cmux-browser-scaling-benchmark"
T = TypeVar("T")


class BenchmarkFailure(RuntimeError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise BenchmarkFailure(message)


class JsonLineClient:
    def __init__(self, path: Path, timeout: float) -> None:
        self._socket = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self._socket.settimeout(timeout)
        self._socket.connect(str(path))
        self._reader = self._socket.makefile("rb")
        self._next_id = 1

    def close(self) -> None:
        try:
            self._reader.close()
        finally:
            self._socket.close()

    def request(self, command: str, **fields: Any) -> dict[str, Any]:
        request_id = self._next_id
        self._next_id += 1
        payload = {"id": request_id, "cmd": command, **fields}
        self._socket.sendall(json.dumps(payload, separators=(",", ":")).encode() + b"\n")
        while True:
            try:
                line = self._reader.readline()
            except TimeoutError as error:
                raise BenchmarkFailure(f"timed out waiting for {command}") from error
            if not line:
                raise BenchmarkFailure(f"cmux-tui closed the connection during {command}")
            try:
                value = json.loads(line)
            except json.JSONDecodeError as error:
                raise BenchmarkFailure(f"invalid JSON response: {line!r}") from error
            if not isinstance(value, dict) or "event" in value:
                continue
            if value.get("id") != request_id:
                continue
            if value.get("ok") is not True:
                raise BenchmarkFailure(
                    f"{command} failed: {value.get('error', 'unknown error')}"
                )
            data = value.get("data", {})
            if not isinstance(data, dict):
                raise BenchmarkFailure(f"{command} returned non-object data")
            return data


def wait_until(callback: Callable[[], T | None], description: str, timeout: float) -> T:
    deadline = time.monotonic() + timeout
    last_error: Exception | None = None
    while time.monotonic() < deadline:
        try:
            result = callback()
        except (OSError, BenchmarkFailure) as error:
            last_error = error
        else:
            if result is not None:
                return result
        time.sleep(0.02)
    suffix = f": {last_error}" if last_error is not None else ""
    raise BenchmarkFailure(f"timed out waiting for {description}{suffix}")


def stable_token(value: str) -> str:
    result = 0xCBF29CE484222325
    for byte in value.encode():
        result ^= byte
        result = (result * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return f"{result:016x}"


def pinned_revisions() -> tuple[str, str]:
    revisions = PIN_PATTERN.findall(REVISION_HEADER.read_text())
    if len(revisions) != 2:
        raise BenchmarkFailure(f"could not read both revisions from {REVISION_HEADER}")
    cmux_revision, ghostty_revision = revisions
    require(
        (ROOT / "ghostty-revision.txt").read_text().strip() == ghostty_revision,
        "runtime and root Ghostty pins disagree",
    )
    return cmux_revision, ghostty_revision


def percentile(values: list[float], percent: float) -> float:
    require(bool(values), "cannot summarize an empty sample")
    ordered = sorted(values)
    index = max(0, min(len(ordered) - 1, math.ceil(len(ordered) * percent / 100) - 1))
    return ordered[index]


def latency_summary(values: list[float]) -> dict[str, float | int]:
    return {
        "samples": len(values),
        "min_ms": round(min(values), 3),
        "median_ms": round(statistics.median(values), 3),
        "p95_ms": round(percentile(values, 95), 3),
        "max_ms": round(max(values), 3),
    }


def timed_request(
    client: JsonLineClient, command: str, **fields: Any
) -> tuple[dict[str, Any], float]:
    started = time.perf_counter_ns()
    data = client.request(command, **fields)
    elapsed_ms = (time.perf_counter_ns() - started) / 1_000_000
    return data, elapsed_ms


def list_terminals(client: JsonLineClient) -> dict[str, Any]:
    data = client.request("list-terminals")
    require(isinstance(data.get("terminals"), list), "list-terminals omitted terminals")
    require(isinstance(data.get("terminal_revision"), int), "invalid terminal revision")
    return data


def read_host_records(host_root: Path) -> list[tuple[Path, dict[str, Any]]]:
    records: list[tuple[Path, dict[str, Any]]] = []
    if not host_root.is_dir():
        return records
    for path in sorted(host_root.glob("*.json")):
        try:
            value = json.loads(path.read_text())
        except (OSError, json.JSONDecodeError) as error:
            raise BenchmarkFailure(f"invalid terminal-host record {path}: {error}") from error
        require(isinstance(value, dict), f"terminal-host record is not an object: {path}")
        require(value.get("terminal_id") == path.stem, f"host identity mismatch: {path}")
        records.append((path, value))
    return records


def pid_is_alive(pid: int) -> bool:
    try:
        os.kill(pid, 0)
    except OSError as error:
        return error.errno == errno.EPERM
    return True


def host_record_proves_live(path: Path, record: dict[str, Any]) -> bool:
    incarnation = record.get("incarnation")
    nonce = record.get("host_start_nonce")
    if not isinstance(incarnation, str) or not isinstance(nonce, str):
        return False
    proof = path.with_suffix(f".{incarnation}-{nonce}.live")
    try:
        descriptor = os.open(proof, os.O_RDWR)
    except OSError:
        return False
    try:
        try:
            fcntl.flock(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            return True
        fcntl.flock(descriptor, fcntl.LOCK_UN)
        return False
    finally:
        os.close(descriptor)


def live_host_records(host_root: Path) -> list[tuple[Path, dict[str, Any]]]:
    records = read_host_records(host_root)
    for path, record in records:
        pid = record.get("host_pid")
        require(isinstance(pid, int) and pid > 0, f"host record omitted PID: {path}")
        require(pid_is_alive(pid), f"recorded terminal host is not alive: {pid}")
        require(host_record_proves_live(path, record), f"host liveness proof is not held: {path}")
    return records


def process_table() -> dict[int, dict[str, Any]]:
    completed = subprocess.run(
        ["ps", "-axo", "pid=,ppid=,rss=,%cpu=,command="],
        check=True,
        capture_output=True,
        text=True,
    )
    result: dict[int, dict[str, Any]] = {}
    for line in completed.stdout.splitlines():
        fields = line.strip().split(None, 4)
        if len(fields) < 4:
            continue
        try:
            pid, ppid, rss_kib = (int(fields[index]) for index in range(3))
            cpu_percent = float(fields[3])
        except ValueError:
            continue
        result[pid] = {
            "pid": pid,
            "ppid": ppid,
            "rss_kib": rss_kib,
            "cpu_percent": cpu_percent,
            "command": fields[4] if len(fields) == 5 else "",
        }
    return result


def descendants(table: dict[int, dict[str, Any]], roots: set[int]) -> set[int]:
    found: set[int] = set()
    frontier = set(roots)
    while frontier:
        children = {
            pid
            for pid, item in table.items()
            if item["ppid"] in frontier and pid not in roots and pid not in found
        }
        found.update(children)
        frontier = children
    return found


def resource_snapshot(daemon_pid: int, host_root: Path, expected: int) -> dict[str, Any]:
    records = live_host_records(host_root)
    host_pids = [record["host_pid"] for _, record in records]
    require(len(records) == expected, f"expected {expected} terminal hosts, found {len(records)}")
    require(len(set(host_pids)) == expected, "terminal-host records reuse a process")
    table = process_table()
    require(daemon_pid in table, "daemon is absent from the process table")
    require(all(pid in table for pid in host_pids), "terminal host is absent from process table")
    child_pids = descendants(table, set(host_pids))
    session_pids = {daemon_pid, *host_pids, *child_pids}
    host_rss = [table[pid]["rss_kib"] for pid in host_pids]
    host_cpu = [table[pid]["cpu_percent"] for pid in host_pids]
    return {
        "daemon": {
            "process_count": 1,
            "rss_kib": table[daemon_pid]["rss_kib"],
            "cpu_percent": table[daemon_pid]["cpu_percent"],
        },
        "terminal_hosts": {
            "process_count": len(host_pids),
            "rss_total_kib": sum(host_rss),
            "rss_median_kib": round(statistics.median(host_rss), 1),
            "rss_max_kib": max(host_rss),
            "cpu_total_percent": round(sum(host_cpu), 3),
        },
        "host_descendants": {
            "process_count": len(child_pids),
            "rss_total_kib": sum(table[pid]["rss_kib"] for pid in child_pids),
            "cpu_total_percent": round(sum(table[pid]["cpu_percent"] for pid in child_pids), 3),
        },
        "session_total": {
            "process_count": len(session_pids),
            "rss_total_kib": sum(table[pid]["rss_kib"] for pid in session_pids),
            "cpu_total_percent": round(
                sum(table[pid]["cpu_percent"] for pid in session_pids), 3
            ),
        },
    }


def start_daemon(
    binary: Path,
    session: str,
    socket_path: Path,
    state_path: Path,
    stderr_path: Path,
    timeout: float,
) -> tuple[subprocess.Popen[bytes], Any]:
    stderr_file = stderr_path.open("wb")
    process = subprocess.Popen(
        [
            str(binary),
            "--session",
            session,
            "--headless",
            "--socket",
            str(socket_path),
            "--state",
            str(state_path),
        ],
        stdin=subprocess.DEVNULL,
        stdout=subprocess.DEVNULL,
        stderr=stderr_file,
        start_new_session=True,
    )
    try:
        wait_until(
            lambda: True if socket_path.exists() and process.poll() is None else None,
            "daemon control socket",
            timeout,
        )
        wait_until(
            lambda: connected_probe(socket_path, min(timeout, 1.0)),
            "daemon to accept a connection",
            timeout,
        )
    except BaseException:
        if process.poll() is None:
            process.kill()
            process.wait()
        stderr_file.close()
        stderr = stderr_path.read_text(errors="replace")
        raise BenchmarkFailure(f"cmux-tui failed to start: {stderr.strip()}")
    return process, stderr_file


def connected_probe(socket_path: Path, timeout: float) -> bool | None:
    try:
        probe = JsonLineClient(socket_path, timeout)
    except OSError:
        return None
    probe.close()
    return True


def stop_process(process: subprocess.Popen[bytes], timeout: float) -> None:
    if process.poll() is None:
        process.send_signal(signal.SIGTERM)
        try:
            process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()


def cleanup_recorded_hosts(host_root: Path, timeout: float) -> None:
    records = read_host_records(host_root)
    for path, record in records:
        pid = record.get("host_pid")
        if (
            isinstance(pid, int)
            and pid > 0
            and pid_is_alive(pid)
            and host_record_proves_live(path, record)
        ):
            try:
                os.kill(pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if not any(host_record_proves_live(path, record) for path, record in records):
            return
        time.sleep(0.02)
    for path, record in records:
        pid = record.get("host_pid")
        if (
            isinstance(pid, int)
            and pid > 0
            and pid_is_alive(pid)
            and host_record_proves_live(path, record)
        ):
            try:
                os.kill(pid, signal.SIGKILL)
            except ProcessLookupError:
                pass


def identity_matches(
    identity: dict[str, Any], cmux_revision: str, ghostty_revision: str
) -> None:
    expected = {
        "app": "cmux-tui",
        "protocol": 9,
        "build_commit": cmux_revision,
        "ghostty_commit": ghostty_revision,
    }
    for key, value in expected.items():
        require(identity.get(key) == value, f"identity {key} mismatch: {identity.get(key)!r}")
    require(isinstance(identity.get("generation"), str), "identity omitted generation")


def run_benchmark(
    binary: Path, counts: tuple[int, ...], samples: int, timeout: float
) -> dict[str, Any]:
    require(os.name == "posix" and hasattr(socket, "AF_UNIX"), "POSIX AF_UNIX required")
    require(binary.is_file() and os.access(binary, os.X_OK), f"not executable: {binary}")
    cat = shutil.which("cat")
    require(cat is not None, "cat is required for the deterministic idle PTY workload")
    cmux_revision, ghostty_revision = pinned_revisions()
    session = f"cmux-browser-bench-{os.getpid()}-{uuid.uuid4().hex[:8]}"
    workspace_key = f"benchmark-{uuid.uuid4().hex}"
    root = Path(tempfile.mkdtemp(prefix="cmux-browser-scaling-benchmark-"))
    socket_path = root / "control.sock"
    state_path = root / "state"
    host_root = state_path / f"terminal-hosts-{stable_token(session)}"
    stderr_path = root / "daemon.stderr.log"
    daemon: subprocess.Popen[bytes] | None = None
    stderr_file: Any = None
    control: JsonLineClient | None = None
    terminal_ids: list[str] = []
    started_wall = time.time()
    result: dict[str, Any] = {}
    try:
        daemon, stderr_file = start_daemon(
            binary, session, socket_path, state_path, stderr_path, timeout
        )
        control = JsonLineClient(socket_path, timeout)
        identity = control.request("identify")
        identity_matches(identity, cmux_revision, ghostty_revision)
        require(identity.get("terminal_revision") == 0, "fresh terminal registry is not empty")
        require(identity.get("workspace_revision") == 0, "fresh workspace registry is not empty")
        control.request("set-client-info", name=ORIGIN, kind="benchmark")
        control.request(
            "create-workspace",
            key=workspace_key,
            name="Scaling benchmark",
            origin=ORIGIN,
            mutation_id="create-workspace",
            expected_generation=identity["generation"],
            expected_revision=0,
        )

        checkpoints: list[dict[str, Any]] = []
        cumulative_create_started = time.perf_counter_ns()
        previous_count = 0
        for target in counts:
            create_latencies: list[float] = []
            ready_latencies: list[float] = []
            batch_started = time.perf_counter_ns()
            for index in range(previous_count, target):
                terminal_id = uuid.uuid4().hex
                revision = list_terminals(control)["terminal_revision"]
                create_started = time.perf_counter_ns()
                created, create_ms = timed_request(
                    control,
                    "create-terminal",
                    key=workspace_key,
                    argv=[cat],
                    terminal_id=terminal_id,
                    origin=ORIGIN,
                    mutation_id=f"create-terminal-{index}",
                    expected_generation=identity["generation"],
                    expected_terminal_revision=revision,
                    cols=80,
                    rows=24,
                )
                require(created.get("terminal_id") == terminal_id, "created wrong terminal")
                create_latencies.append(create_ms)

                def terminal_running() -> bool | None:
                    terminals = list_terminals(control)["terminals"]
                    terminal = next(
                        (item for item in terminals if item.get("terminal_id") == terminal_id),
                        None,
                    )
                    return True if terminal and terminal.get("lifecycle") == "running" else None

                wait_until(terminal_running, f"terminal {terminal_id} to run", timeout)
                ready_latencies.append((time.perf_counter_ns() - create_started) / 1_000_000)
                terminal_ids.append(terminal_id)

            batch_ms = (time.perf_counter_ns() - batch_started) / 1_000_000

            wait_until(
                lambda: (
                    live_host_records(host_root)
                    if len(read_host_records(host_root)) == target
                    else None
                ),
                f"{target} live terminal-host records",
                timeout,
            )
            snapshot_latencies: list[float] = []
            snapshot: dict[str, Any] = {}
            for _ in range(samples):
                snapshot, elapsed = timed_request(control, "list-terminals")
                snapshot_latencies.append(elapsed)
            require(
                len(snapshot.get("terminals", [])) == target,
                "snapshot terminal count mismatch",
            )
            require(
                {item.get("terminal_id") for item in snapshot["terminals"]}
                == set(terminal_ids),
                "snapshot terminal identities mismatch",
            )
            ping_latencies = [timed_request(control, "ping")[1] for _ in range(samples)]
            checkpoints.append(
                {
                    "terminals": target,
                    "created_in_batch": target - previous_count,
                    "create": {
                        "batch_ms": round(batch_ms, 3),
                        "requests": latency_summary(create_latencies),
                        "to_running": latency_summary(ready_latencies),
                        "cumulative_ms": round(
                            (time.perf_counter_ns() - cumulative_create_started) / 1_000_000,
                            3,
                        ),
                    },
                    "list_terminals": {
                        **latency_summary(snapshot_latencies),
                        "payload_bytes": len(
                            json.dumps(snapshot, separators=(",", ":")).encode()
                        ),
                    },
                    "ping": latency_summary(ping_latencies),
                    "resources": resource_snapshot(daemon.pid, host_root, target),
                }
            )
            previous_count = target

        result = {
            "schema_version": 1,
            "binary": str(binary),
            "version": subprocess.run(
                [str(binary), "--version"], check=True, capture_output=True, text=True
            ).stdout.strip(),
            "build_commit": cmux_revision,
            "ghostty_commit": ghostty_revision,
            "protocol": 9,
            "workload": {
                "command": cat,
                "terminal_size": "80x24",
                "checkpoint_counts": list(counts),
                "latency_samples_per_checkpoint": samples,
                "isolated_temporary_state": True,
            },
            "started_unix_seconds": round(started_wall, 3),
            "duration_ms": round((time.time() - started_wall) * 1000, 3),
            "checkpoints": checkpoints,
            "result": "ok",
        }

        return result
    finally:
        if control is not None:
            control.close()
        if daemon is not None:
            stop_process(daemon, timeout)
        if stderr_file is not None:
            stderr_file.close()
        cleanup_recorded_hosts(host_root, timeout)
        shutil.rmtree(root, ignore_errors=True)


def parse_counts(value: str) -> tuple[int, ...]:
    try:
        counts = tuple(int(item) for item in value.split(","))
    except ValueError as error:
        raise argparse.ArgumentTypeError("counts must be comma-separated integers") from error
    if not counts or any(item <= 0 for item in counts):
        raise argparse.ArgumentTypeError("counts must all be positive")
    if tuple(sorted(set(counts))) != counts:
        raise argparse.ArgumentTypeError("counts must be unique and strictly increasing")
    return counts


def write_result(path: Path, result: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(f".{path.name}.{os.getpid()}.tmp")
    temporary.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")
    temporary.replace(path)


def main() -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Benchmark cmux-tui daemon + one-host-per-terminal scaling in an "
            "isolated temporary session."
        ),
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument(
        "--binary",
        type=Path,
        required=True,
        help="exact staged or bundled cmux-tui executable to benchmark",
    )
    parser.add_argument("--counts", type=parse_counts, default=parse_counts("1,10,50,100"))
    parser.add_argument("--samples", type=int, default=10)
    parser.add_argument("--timeout", type=float, default=20.0)
    parser.add_argument("--output", type=Path, help="also atomically write the JSON result")
    parser.add_argument(
        "--self-check",
        action="store_true",
        help="run the full harness with one terminal and two latency samples",
    )
    args = parser.parse_args()
    if args.samples <= 0:
        parser.error("--samples must be positive")
    if args.timeout <= 0:
        parser.error("--timeout must be positive")
    counts = (1,) if args.self_check else args.counts
    samples = 2 if args.self_check else args.samples
    try:
        result = run_benchmark(args.binary.resolve(), counts, samples, args.timeout)
        if args.output is not None:
            write_result(args.output.resolve(), result)
        print(json.dumps(result, indent=2, sort_keys=True))
    except (BenchmarkFailure, OSError, subprocess.SubprocessError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
