#!/usr/bin/env python3
"""Benchmark a running cmux Browser at projected-terminal checkpoints.

The harness drives only protocol-v9 canonical registry mutations.  It records
every workspace/terminal identity that it creates in an atomic state file, so
`cleanup` can close exactly that test data without touching pre-existing user
state.  UI interaction latency is intentionally measured separately through
Computer Use; `snapshot` covers process count, RSS, CPU, and optional macOS
physical footprint for the signed app.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import errno
import fcntl
import json
import os
from pathlib import Path
import shlex
import socket
import statistics
import struct
import subprocess
import sys
import tempfile
import time
from typing import Any, Callable, TypeVar
import uuid


SCHEMA_VERSION = 1
ORIGIN = "cmux-browser-frontend-scaling-benchmark"
POLL_INTERVAL_SECONDS = 0.25
RESOURCE_STABLE_SECONDS = 1.0
FOOTPRINT_ATTEMPTS = 3
T = TypeVar("T")
HOST_EXECUTABLE_CACHE: dict[tuple[int, str], Path] = {}


class BenchmarkFailure(RuntimeError):
    pass


class TopologyFailure(BenchmarkFailure):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise BenchmarkFailure(message)


class JsonLineClient:
    def __init__(self, path: Path, timeout: float) -> None:
        self.path = path.resolve()
        self._socket = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self._socket.settimeout(timeout)
        self._socket.connect(str(self.path))
        self._reader = self._socket.makefile("rb")
        self._next_id = 1

    def close(self) -> None:
        try:
            self._reader.close()
        finally:
            self._socket.close()

    def peer_pid(self) -> int | None:
        if sys.platform != "darwin":
            return None
        # Darwin's SOL_LOCAL and LOCAL_PEERPID are 0 and 2 respectively, but
        # Python's socket module does not publish those constants.
        raw = self._socket.getsockopt(0, 2, struct.calcsize("i"))
        return int(struct.unpack("i", raw)[0])

    def request_value(self, command: str, **fields: Any) -> Any:
        request_id = self._next_id
        self._next_id += 1
        payload = {"id": request_id, "cmd": command, **fields}
        self._socket.sendall(json.dumps(payload, separators=(",", ":")).encode() + b"\n")
        while True:
            line = self._reader.readline()
            if not line:
                raise BenchmarkFailure(f"connection closed during {command}")
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
            return value.get("data", {})

    def request(self, command: str, **fields: Any) -> dict[str, Any]:
        data = self.request_value(command, **fields)
        if not isinstance(data, dict):
            raise BenchmarkFailure(f"{command} returned non-object data")
        return data

    def request_list(self, command: str, **fields: Any) -> list[Any]:
        data = self.request_value(command, **fields)
        if not isinstance(data, list):
            raise BenchmarkFailure(f"{command} returned non-array data")
        return data


def wait_until(
    callback: Callable[[], T | None],
    description: str,
    timeout: float,
    *,
    interval: float = POLL_INTERVAL_SECONDS,
) -> T:
    deadline = time.monotonic() + timeout
    last_error: Exception | None = None
    while time.monotonic() < deadline:
        try:
            result = callback()
        except TopologyFailure:
            raise
        except (OSError, BenchmarkFailure) as error:
            last_error = error
        else:
            if result is not None:
                return result
        time.sleep(interval)
    suffix = f": {last_error}" if last_error is not None else ""
    raise BenchmarkFailure(f"timed out waiting for {description}{suffix}")


def atomic_write(path: Path, value: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(f".{path.name}.{os.getpid()}.tmp")
    payload = json.dumps(value, indent=2, sort_keys=True) + "\n"
    try:
        descriptor = os.open(
            temporary,
            os.O_WRONLY
            | os.O_CREAT
            | os.O_TRUNC
            | getattr(os, "O_CLOEXEC", 0)
            | getattr(os, "O_NOFOLLOW", 0),
            0o600,
        )
        os.fchmod(descriptor, 0o600)
        with os.fdopen(descriptor, "w", encoding="utf-8") as writer:
            writer.write(payload)
            writer.flush()
            os.fsync(writer.fileno())
        os.replace(temporary, path)
        directory_flags = os.O_RDONLY | getattr(os, "O_DIRECTORY", 0)
        directory = os.open(path.parent, directory_flags)
        try:
            os.fsync(directory)
        except OSError as error:
            # Some filesystems do not implement directory fsync. The file
            # itself is still durable; reject every other sync failure.
            if error.errno not in {errno.EINVAL, errno.ENOTSUP}:
                raise
        finally:
            os.close(directory)
    finally:
        try:
            temporary.unlink()
        except FileNotFoundError:
            pass


def load_state(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text())
    except (OSError, json.JSONDecodeError) as error:
        raise BenchmarkFailure(f"could not read state file {path}: {error}") from error
    require(isinstance(value, dict), "benchmark state is not an object")
    require(value.get("schema_version") == SCHEMA_VERSION, "benchmark state schema mismatch")
    for field in (
        "app",
        "socket",
        "registry_id",
        "source_workspace_key",
        "target_workspace_key",
    ):
        require(
            isinstance(value.get(field), str) and bool(value[field]),
            f"benchmark state omitted {field}",
        )
    for field in ("baseline_terminal_ids", "created_terminal_ids", "closed_terminal_ids"):
        items = value.get(field)
        require(
            isinstance(items, list)
            and all(isinstance(item, str) and item for item in items),
            f"benchmark state has invalid {field}",
        )
    require(
        isinstance(value.get("cleanup_complete"), bool),
        "benchmark state has invalid cleanup_complete",
    )
    return value


def acquire_lock(path: Path, description: str) -> Any:
    path.parent.mkdir(parents=True, exist_ok=True)
    lock = path.open("a+")
    try:
        fcntl.flock(lock.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError as error:
        lock.close()
        raise BenchmarkFailure(f"{description} is already in use: {path}") from error
    return lock


def acquire_state_lock(path: Path) -> Any:
    lock_path = path.with_name(f".{path.name}.lock")
    return acquire_lock(lock_path, "benchmark state")


def session_lock_path(socket_path: Path) -> Path:
    socket_path = socket_path.expanduser().resolve()
    return socket_path.with_name(f".{socket_path.name}.benchmark.lock")


def acquire_session_lock(socket_path: Path) -> Any:
    return acquire_lock(session_lock_path(socket_path), "benchmark daemon session")


def validate_state_identity(
    state: dict[str, Any], identity: dict[str, Any], *, enforce_build: bool
) -> None:
    require(identity.get("protocol") == 9, "running daemon is not protocol 9")
    require(identity.get("registry_id") == state["registry_id"], "registry identity changed")
    if state.get("session") is not None:
        require(identity.get("session") == state["session"], "daemon session changed")
    if enforce_build and state.get("cmux_tui_commit") is not None:
        require(
            identity.get("build_commit") == state["cmux_tui_commit"],
            "daemon build changed during benchmark",
        )
    if enforce_build and state.get("ghostty_commit") is not None:
        require(
            identity.get("ghostty_commit") == state["ghostty_commit"],
            "daemon Ghostty build changed during benchmark",
        )


def connect_state(
    state: dict[str, Any], timeout: float, *, enforce_build: bool = True
) -> tuple[JsonLineClient, dict[str, Any]]:
    client = JsonLineClient(Path(state["socket"]), timeout)
    identity = client.request("identify")
    validate_state_identity(state, identity, enforce_build=enforce_build)
    client.request("set-client-info", name=ORIGIN, kind="benchmark")
    state["generation"] = identity.get("generation")
    return client, identity


def list_terminals(client: JsonLineClient) -> dict[str, Any]:
    result = client.request("list-terminals")
    require(isinstance(result.get("terminals"), list), "list-terminals omitted terminals")
    require(isinstance(result.get("terminal_revision"), int), "invalid terminal revision")
    return result


def live_terminal_ids(result: dict[str, Any]) -> set[str]:
    return {
        item["terminal_id"]
        for item in result["terminals"]
        if isinstance(item, dict)
        and isinstance(item.get("terminal_id"), str)
        and item.get("lifecycle") not in {"closed", "tombstoned"}
    }


def list_workspaces(client: JsonLineClient) -> dict[str, Any]:
    result = client.request("list-workspaces")
    require(isinstance(result.get("workspaces"), list), "list-workspaces omitted workspaces")
    require(isinstance(result.get("workspace_revision"), int), "invalid workspace revision")
    return result


@dataclass(frozen=True)
class RunTopology:
    app: Path
    socket: Path
    registry_id: str
    generation: str
    session: str
    daemon_pid: int
    daemon_start_token: str
    browser_pid: int | None
    browser_start_token: str | None
    cmux_tui_commit: str
    ghostty_commit: str
    terminal_host_root: Path
    invocation_bundles: tuple[str, ...]


@dataclass(frozen=True)
class DaemonConnection:
    pid: int
    start_token: str
    executable: Path
    command: str
    argv: tuple[str, ...]
    table: dict[int, dict[str, Any]]


def read_revision(path: Path, label: str) -> str:
    try:
        revision = path.read_text().strip()
    except OSError as error:
        raise BenchmarkFailure(f"could not read {label} receipt {path}: {error}") from error
    require(
        len(revision) == 40 and all(character in "0123456789abcdef" for character in revision),
        f"invalid {label} receipt: {path}",
    )
    return revision


def validate_app_bundle(app: Path) -> tuple[str, str]:
    main = app / "Contents/MacOS/cmux"
    helper = app / "Contents/Helpers/cmux-tui"
    require(main.is_file() and os.access(main, os.X_OK), f"invalid Browser executable: {main}")
    require(helper.is_file() and os.access(helper, os.X_OK), f"missing cmux-tui helper: {helper}")
    if sys.platform == "darwin":
        verified = subprocess.run(
            ["codesign", "--verify", "--deep", "--strict", str(app)],
            capture_output=True,
            text=True,
        )
        detail = verified.stderr.strip().splitlines()
        require(
            verified.returncode == 0,
            "signed app verification failed"
            + (f": {detail[-1]}" if detail else ""),
        )
    receipts = app / "Contents/Resources/cmux-tui"
    return (
        read_revision(receipts / "cmux-tui.REVISION", "cmux-tui"),
        read_revision(receipts / "cmux-tui.GHOSTTY_REVISION", "Ghostty"),
    )


def process_table() -> dict[int, dict[str, Any]]:
    completed = subprocess.run(
        ["ps", "-axo", "pid=,ppid=,rss=,%cpu=,lstart=,command="],
        check=True,
        capture_output=True,
        text=True,
    )
    result: dict[int, dict[str, Any]] = {}
    for line in completed.stdout.splitlines():
        # lstart is always five whitespace-delimited fields on BSD ps. Keep
        # it in this one table so repeated settling polls do not fork two
        # additional ps processes merely to revalidate daemon/browser PIDs.
        fields = line.strip().split(None, 9)
        if len(fields) < 9:
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
            "start_token": " ".join(fields[4:9]),
            "command": fields[9] if len(fields) == 10 else "",
        }
    return result


def process_value(pid: int, field: str, label: str) -> str:
    completed = subprocess.run(
        ["ps", "-p", str(pid), "-o", f"{field}="],
        capture_output=True,
        text=True,
    )
    value = completed.stdout.strip()
    require(completed.returncode == 0 and bool(value), f"{label} process {pid} is not alive")
    return value


def process_executable(pid: int, label: str) -> Path:
    if sys.platform == "darwin":
        completed = subprocess.run(
            ["lsof", "-a", "-p", str(pid), "-d", "txt", "-Fn"],
            capture_output=True,
            text=True,
        )
        for line in completed.stdout.splitlines():
            if line.startswith("n") and len(line) > 1:
                return Path(line[1:])
    return Path(process_value(pid, "comm", label))


def process_executables(pids: set[int], label: str) -> dict[int, Path]:
    if not pids:
        return {}
    if sys.platform == "darwin":
        completed = subprocess.run(
            ["lsof", "-a", "-p", ",".join(map(str, sorted(pids))), "-d", "txt", "-Fn"],
            capture_output=True,
            text=True,
        )
        result: dict[int, Path] = {}
        current_pid: int | None = None
        text_entry = False
        for line in completed.stdout.splitlines():
            if line.startswith("p"):
                try:
                    current_pid = int(line[1:])
                except ValueError:
                    current_pid = None
                text_entry = False
            elif line == "ftxt":
                text_entry = True
            elif (
                line.startswith("n")
                and len(line) > 1
                and current_pid in pids
                and text_entry
                and current_pid not in result
            ):
                result[current_pid] = Path(line[1:])
                text_entry = False
        missing = sorted(pids - result.keys())
        require(not missing, f"could not identify {label} executables: {missing}")
        return result
    return {pid: process_executable(pid, label) for pid in pids}


def executable_matches(actual: Path, expected: Path) -> bool:
    try:
        return actual.resolve() == expected.resolve()
    except OSError:
        return actual == expected


def require_table_process_identity(
    table: dict[int, dict[str, Any]], pid: int, start_token: str, label: str
) -> None:
    item = table.get(pid)
    if item is None:
        raise TopologyFailure(f"{label} process exited: {pid}")
    if item.get("start_token") != start_token:
        raise TopologyFailure(f"{label} process identity changed: {pid}")


def option_value(argv: list[str], option: str) -> str | None:
    for index, argument in enumerate(argv):
        if argument == option and index + 1 < len(argv):
            return argv[index + 1]
        if argument.startswith(f"{option}="):
            return argument[len(option) + 1 :]
    return None


def command_bundle(command: str) -> str | None:
    marker = "/Contents/"
    marker_index = command.find(marker)
    if marker_index <= 0:
        return None
    return command[:marker_index]


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


def discover_terminal_host_root(
    client: JsonLineClient, daemon_argv: list[str]
) -> Path:
    terminals = list_terminals(client)["terminals"]
    live = {
        item["terminal_id"]: item["terminal_incarnation"]
        for item in terminals
        if isinstance(item, dict)
        and item.get("lifecycle") not in {"closed", "tombstoned"}
        and isinstance(item.get("terminal_id"), str)
        and isinstance(item.get("terminal_incarnation"), str)
    }
    require(bool(live), "cannot identify terminal-host records without a live terminal")

    state_roots: list[Path] = []
    explicit_state = option_value(daemon_argv, "--state")
    if explicit_state:
        state_roots.append(Path(explicit_state).expanduser())
    inherited_state = os.environ.get("CMUX_TUI_STATE_DIR")
    if inherited_state:
        state_roots.append(Path(inherited_state).expanduser())
    if sys.platform == "darwin":
        state_roots.append(
            Path.home()
            / "Library"
            / "Application Support"
            / "cmux-tui"
            / "sessions"
        )
    elif sys.platform.startswith("linux"):
        xdg_state = os.environ.get("XDG_STATE_HOME")
        state_roots.append(
            (Path(xdg_state).expanduser() if xdg_state else Path.home() / ".local/state")
            / "cmux-tui"
            / "sessions"
        )

    matches: list[Path] = []
    for state_root in dict.fromkeys(path.resolve() for path in state_roots):
        for candidate in state_root.glob("terminal-hosts-*"):
            if not candidate.is_dir():
                continue
            valid = True
            for terminal_id, incarnation in live.items():
                try:
                    record = json.loads((candidate / f"{terminal_id}.json").read_text())
                except (OSError, json.JSONDecodeError):
                    valid = False
                    break
                if (
                    not isinstance(record, dict)
                    or record.get("terminal_id") != terminal_id
                    or record.get("incarnation") != incarnation
                ):
                    valid = False
                    break
            if valid:
                matches.append(candidate.resolve())
    matches = sorted(set(matches))
    require(
        len(matches) == 1,
        f"expected one terminal-host record directory, found {matches}",
    )
    return matches[0]


def recorded_terminal_host_pids(
    topology: RunTopology, table: dict[int, dict[str, Any]]
) -> set[int]:
    host_pids: set[int] = set()
    cache_keys_by_pid: dict[int, tuple[int, str]] = {}
    expected_helper = topology.app / "Contents/Helpers/cmux-tui"
    unresolved: dict[tuple[int, str], int] = {}
    try:
        record_paths = sorted(topology.terminal_host_root.glob("*.json"))
    except OSError as error:
        raise TopologyFailure(f"could not scan terminal-host records: {error}") from error
    for record_path in record_paths:
        try:
            record = json.loads(record_path.read_text())
        except (OSError, json.JSONDecodeError) as error:
            raise TopologyFailure(f"invalid terminal-host record {record_path}: {error}") from error
        if not isinstance(record, dict):
            raise TopologyFailure(f"terminal-host record is not an object: {record_path}")
        terminal_id = record.get("terminal_id")
        incarnation = record.get("incarnation")
        start_nonce = record.get("host_start_nonce")
        host_pid = record.get("host_pid")
        if (
            record_path.stem != terminal_id
            or not isinstance(incarnation, str)
            or not isinstance(start_nonce, str)
            or not isinstance(host_pid, int)
            or isinstance(host_pid, bool)
            or host_pid <= 1
        ):
            raise TopologyFailure(f"malformed terminal-host record: {record_path}")
        live_marker = topology.terminal_host_root / (
            f"{terminal_id}.{incarnation}-{start_nonce}.live"
        )
        if not liveness_marker_is_locked(live_marker):
            raise TopologyFailure(
                f"terminal-host liveness proof is not held: {live_marker}"
            )
        if host_pid not in table:
            raise TopologyFailure(f"recorded terminal-host process exited: {host_pid}")
        cache_key = (host_pid, start_nonce)
        if host_pid in host_pids:
            raise TopologyFailure(f"multiple terminal-host records claim pid {host_pid}")
        if cache_key not in HOST_EXECUTABLE_CACHE:
            unresolved[cache_key] = host_pid
        host_pids.add(host_pid)
        cache_keys_by_pid[host_pid] = cache_key

    if unresolved:
        try:
            executables = process_executables(set(unresolved.values()), "terminal host")
        except BenchmarkFailure as error:
            raise TopologyFailure(str(error)) from error
        for cache_key, host_pid in unresolved.items():
            HOST_EXECUTABLE_CACHE[cache_key] = executables[host_pid]

    for host_pid in host_pids:
        executable = HOST_EXECUTABLE_CACHE[cache_keys_by_pid[host_pid]]
        if not executable_matches(executable, expected_helper):
            raise TopologyFailure(
                f"recorded terminal-host pid {host_pid} is not the signed app helper"
            )
        if "cmux-tui __terminal-host" not in table[host_pid]["command"]:
            raise TopologyFailure(f"recorded pid {host_pid} is not a terminal host")
    return host_pids


def terminal_host_identity_at_root(
    terminal_host_root: Path, terminal_id: str
) -> dict[str, Any] | None:
    record_path = terminal_host_root / f"{terminal_id}.json"
    try:
        record = json.loads(record_path.read_text())
    except FileNotFoundError:
        return None
    except (OSError, json.JSONDecodeError) as error:
        raise TopologyFailure(f"invalid terminal-host record {record_path}: {error}") from error
    if not isinstance(record, dict):
        raise TopologyFailure(f"terminal-host record is not an object: {record_path}")
    host_pid = record.get("host_pid")
    start_nonce = record.get("host_start_nonce")
    incarnation = record.get("incarnation")
    if (
        record.get("terminal_id") != terminal_id
        or not isinstance(host_pid, int)
        or isinstance(host_pid, bool)
        or host_pid <= 1
        or not isinstance(start_nonce, str)
        or not start_nonce
        or not isinstance(incarnation, str)
        or not incarnation
    ):
        raise TopologyFailure(f"malformed terminal-host record: {record_path}")
    return {
        "terminal_id": terminal_id,
        "incarnation": incarnation,
        "host_pid": host_pid,
        "host_start_nonce": start_nonce,
    }


def liveness_marker_is_locked(path: Path) -> bool:
    try:
        marker = path.open("r+")
    except OSError:
        return False
    try:
        try:
            fcntl.flock(marker.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            return True
        else:
            fcntl.flock(marker.fileno(), fcntl.LOCK_UN)
            return False
    finally:
        marker.close()


def terminal_host_identity(
    topology: RunTopology, terminal_id: str
) -> dict[str, Any] | None:
    return terminal_host_identity_at_root(topology.terminal_host_root, terminal_id)


def exact_host_identity_is_live(
    terminal_host_root: Path,
    expected: dict[str, Any],
    table: dict[int, dict[str, Any]],
) -> bool:
    terminal_id = expected.get("terminal_id")
    if not isinstance(terminal_id, str):
        return False
    current = terminal_host_identity_at_root(terminal_host_root, terminal_id)
    if current is None:
        return False
    identity_fields = ("terminal_id", "incarnation", "host_pid", "host_start_nonce")
    if any(current.get(field) != expected.get(field) for field in identity_fields):
        return False
    pid = current["host_pid"]
    marker = terminal_host_root / (
        f"{terminal_id}.{current['incarnation']}-{current['host_start_nonce']}.live"
    )
    return (
        liveness_marker_is_locked(marker)
        and pid in table
        and "cmux-tui __terminal-host" in table[pid]["command"]
    )


def settled_terminal_host_identities(
    terminal_host_root: Path, expected_terminal_ids: set[str]
) -> dict[str, dict[str, Any]] | None:
    try:
        record_paths = sorted(terminal_host_root.glob("*.json"))
    except OSError as error:
        raise TopologyFailure(f"could not scan terminal-host records: {error}") from error
    record_ids = {path.stem for path in record_paths}
    if record_ids != expected_terminal_ids:
        return None
    records: dict[str, dict[str, Any]] = {}
    for terminal_id in sorted(record_ids):
        identity = terminal_host_identity_at_root(terminal_host_root, terminal_id)
        if identity is None:
            return None
        records[terminal_id] = identity
    table = process_table()
    seen_pids: set[int] = set()
    for record in records.values():
        pid = record["host_pid"]
        if pid in seen_pids:
            raise TopologyFailure(f"multiple terminal-host records claim pid {pid}")
        seen_pids.add(pid)
        if not exact_host_identity_is_live(terminal_host_root, record, table):
            return None
    return records


def terminal_host_record_ids(terminal_host_root: Path) -> set[str]:
    try:
        return {path.stem for path in terminal_host_root.glob("*.json")}
    except OSError as error:
        raise TopologyFailure(f"could not scan terminal-host records: {error}") from error


def validate_owned_terminal_ids(
    state: dict[str, Any], current_ids: set[str], *, active_run: bool
) -> None:
    baseline = set(state.get("baseline_terminal_ids", []))
    created = set(state.get("created_terminal_ids", []))
    unexpected = current_ids - baseline - created
    require(
        not unexpected,
        f"terminal registry changed outside this benchmark: {sorted(unexpected)}",
    )
    if not active_run:
        return
    require(baseline <= current_ids, "a baseline terminal disappeared")
    observed = set(state.get("observed_terminal_ids", []))
    missing = observed - current_ids
    require(
        not missing,
        f"benchmark-owned terminals disappeared: {sorted(missing)}",
    )


def adopt_ambiguous_owned_terminals(
    state: dict[str, Any], current_ids: set[str]
) -> bool:
    observed = set(state.get("observed_terminal_ids", []))
    observed.update(set(state.get("created_terminal_ids", [])) & current_ids)
    normalized = sorted(observed)
    if normalized == state.get("observed_terminal_ids"):
        return False
    state["observed_terminal_ids"] = normalized
    return True


def validate_daemon_connection(
    client: JsonLineClient,
    identity: dict[str, Any],
    *,
    expected_executable: Path | None = None,
) -> DaemonConnection:
    require(identity.get("app") == "cmux-tui", "socket peer is not cmux-tui")
    require(identity.get("protocol") == 9, "running daemon is not protocol 9")
    for field in ("registry_id", "generation", "session"):
        require(
            isinstance(identity.get(field), str) and bool(identity[field]),
            f"daemon identity omitted {field}",
        )
    daemon_pid = identity.get("pid")
    require(
        isinstance(daemon_pid, int) and not isinstance(daemon_pid, bool) and daemon_pid > 1,
        "daemon identity omitted a valid pid",
    )
    peer_pid = client.peer_pid()
    if peer_pid is not None:
        require(peer_pid == daemon_pid, "socket peer pid does not match daemon identity")

    executable = process_executable(daemon_pid, "daemon")
    if expected_executable is not None:
        require(
            executable_matches(executable, expected_executable),
            f"daemon pid {daemon_pid} is not the signed app helper",
        )
    else:
        require(executable.name == "cmux-tui", "socket daemon executable is not cmux-tui")

    table = process_table()
    require(daemon_pid in table, f"daemon process {daemon_pid} is not alive")
    daemon_command = table[daemon_pid]["command"]
    try:
        daemon_argv = shlex.split(daemon_command)
    except ValueError as error:
        raise BenchmarkFailure(f"could not parse daemon command line: {error}") from error
    require("--headless" in daemon_argv, "socket daemon is not headless")
    require(
        option_value(daemon_argv, "--session") == identity["session"],
        "daemon session does not match its identity",
    )
    daemon_socket = option_value(daemon_argv, "--socket")
    require(bool(daemon_socket), "daemon command line omitted --socket")
    require(
        Path(daemon_socket).resolve() == client.path,
        "daemon command line socket does not match the connected socket",
    )
    return DaemonConnection(
        pid=daemon_pid,
        start_token=table[daemon_pid]["start_token"],
        executable=executable,
        command=daemon_command,
        argv=tuple(daemon_argv),
        table=table,
    )


def cleanup_terminal_host_root(
    state: dict[str, Any], client: JsonLineClient, daemon: DaemonConnection
) -> Path:
    stored_value = state.get("terminal_host_root")
    stored = (
        Path(stored_value).expanduser().resolve()
        if isinstance(stored_value, str) and stored_value
        else None
    )
    try:
        discovered = discover_terminal_host_root(client, list(daemon.argv))
    except BenchmarkFailure:
        if stored is None or not stored.is_dir():
            raise
        return stored
    if stored is not None:
        require(
            stored == discovered,
            "stored terminal-host root does not match the connected daemon",
        )
    return discovered


def build_run_topology(
    app: Path,
    client: JsonLineClient,
    identity: dict[str, Any],
    *,
    require_browser: bool = True,
) -> RunTopology:
    cmux_tui_commit, ghostty_commit = validate_app_bundle(app)
    require(
        identity.get("build_commit") == cmux_tui_commit,
        "daemon build does not match the signed app receipt",
    )
    require(
        identity.get("ghostty_commit") == ghostty_commit,
        "daemon Ghostty build does not match the signed app receipt",
    )
    expected_helper = app / "Contents/Helpers/cmux-tui"
    daemon = validate_daemon_connection(
        client, identity, expected_executable=expected_helper
    )
    daemon_pid = daemon.pid
    table = daemon.table
    daemon_command = daemon.command
    daemon_argv = list(daemon.argv)

    main = app / "Contents/MacOS/cmux"
    browser_candidates: list[int] = []
    for pid, item in table.items():
        command = item["command"]
        if str(main) not in command or "/Frameworks/" in command:
            continue
        try:
            executable = process_executable(pid, "Browser candidate")
        except BenchmarkFailure:
            continue
        if executable_matches(executable, main):
            browser_candidates.append(pid)
    require(
        len(browser_candidates) <= 1,
        f"multiple signed-app Browser main processes are live: {browser_candidates}",
    )
    clients = client.request_list("list-clients")
    native_browser_attached = any(
        isinstance(item, dict)
        and not item.get("self")
        and item.get("name") == "cmux-browser"
        and item.get("kind") == "native-browser"
        for item in clients
    )
    if require_browser:
        require(bool(browser_candidates), "signed app has no live Browser main process")
        require(native_browser_attached, "daemon has no live native-browser client")
    browser_pid = browser_candidates[0] if browser_candidates and native_browser_attached else None
    if browser_pid is not None:
        browser_executable = process_executable(browser_pid, "Browser")
        require(
            any(
                executable_matches(browser_executable, expected)
                for expected in allowed_mains
            ),
            f"Browser pid {browser_pid} is not the signed app executable",
        )

    invocation_bundles = {
        str(app),
        *(
            bundle
            for bundle in (
                command_bundle(daemon_command),
                command_bundle(table[browser_pid]["command"])
                if browser_pid is not None
                else None,
            )
            if bundle is not None
        ),
    }

    terminal_host_root = discover_terminal_host_root(client, daemon_argv)

    return RunTopology(
        app=app,
        socket=client.path,
        registry_id=identity["registry_id"],
        generation=identity["generation"],
        session=identity["session"],
        daemon_pid=daemon_pid,
        daemon_start_token=daemon.start_token,
        browser_pid=browser_pid,
        browser_start_token=(
            table[browser_pid]["start_token"] if browser_pid is not None else None
        ),
        cmux_tui_commit=cmux_tui_commit,
        ghostty_commit=ghostty_commit,
        terminal_host_root=terminal_host_root,
        invocation_bundles=tuple(sorted(invocation_bundles)),
    )


def app_categories(
    topology: RunTopology, table: dict[int, dict[str, Any]]
) -> dict[str, list[int]]:
    if topology.daemon_pid not in table:
        raise TopologyFailure(f"daemon process exited: {topology.daemon_pid}")
    require_table_process_identity(
        table, topology.daemon_pid, topology.daemon_start_token, "daemon"
    )
    if topology.browser_pid is not None:
        if topology.browser_pid not in table:
            raise TopologyFailure(
                f"Browser main process exited: {topology.browser_pid}"
            )
        require_table_process_identity(
            table,
            topology.browser_pid,
            topology.browser_start_token or "",
            "Browser",
        )

    host_pids = recorded_terminal_host_pids(topology, table)
    terminal_child_pids = descendants(table, host_pids) - host_pids
    backend_pids = {
        topology.daemon_pid,
        *descendants(table, {topology.daemon_pid}),
        *host_pids,
        *descendants(table, host_pids),
    }
    frontend_pids: set[int] = set()
    if topology.browser_pid is not None:
        frontend_pids = {
            topology.browser_pid,
            *descendants(table, {topology.browser_pid}),
        }
    # On first launch the daemon is a Browser child. It becomes a ppid-1
    # durable backend after a Browser relaunch, so backend ownership wins in
    # both shapes and every process appears in exactly one category.
    frontend_pids.difference_update(backend_pids)
    scoped_pids = backend_pids | frontend_pids

    categories: dict[str, list[int]] = {
        "browser_main": [],
        "cmux_daemon": [],
        "terminal_hosts": [],
        "terminal_children": [],
        "terminal_renderers": [],
        "web_renderers": [],
        "gpu": [],
        "other_app_processes": [],
        "unscoped_same_bundle": [],
    }
    for pid in sorted(scoped_pids):
        command = table[pid]["command"]
        if pid == topology.daemon_pid:
            category = "cmux_daemon"
        elif pid == topology.browser_pid:
            category = "browser_main"
        elif pid in host_pids:
            category = "terminal_hosts"
        elif pid in terminal_child_pids:
            category = "terminal_children"
        elif (
            pid in frontend_pids
            and "--utility-sub-type=cmux.mojom.CmuxTerminalRenderer" in command
        ):
            category = "terminal_renderers"
        elif pid in frontend_pids and "--type=renderer" in command:
            category = "web_renderers"
        elif pid in frontend_pids and "--type=gpu-process" in command:
            category = "gpu"
        else:
            category = "other_app_processes"
        categories[category].append(pid)

    bundle_prefixes = tuple(
        f"{bundle}/Contents/" for bundle in topology.invocation_bundles
    )
    categories["unscoped_same_bundle"] = sorted(
        pid
        for pid, item in table.items()
        if pid not in scoped_pids
        and item["command"].startswith(bundle_prefixes)
    )
    for pids in categories.values():
        pids.sort()
    return categories


def summarize_category(pids: list[int], table: dict[int, dict[str, Any]]) -> dict[str, Any]:
    rss = [table[pid]["rss_kib"] for pid in pids]
    cpu = [table[pid]["cpu_percent"] for pid in pids]
    return {
        "count": len(pids),
        "pids": pids,
        "rss_total_kib": sum(rss),
        "rss_median_kib": round(statistics.median(rss), 1) if rss else 0,
        "rss_max_kib": max(rss, default=0),
        "cpu_total_percent": round(sum(cpu), 3),
    }


def scoped_category_names(categories: dict[str, list[int]]) -> list[str]:
    return [name for name in categories if name != "unscoped_same_bundle"]


def resource_snapshot(topology: RunTopology) -> dict[str, Any]:
    table = process_table()
    categories = app_categories(topology, table)
    summarized = {
        name: summarize_category(pids, table) for name, pids in categories.items()
    }
    scoped_pids = sorted(
        {
            pid
            for name in scoped_category_names(categories)
            for pid in categories[name]
        }
    )
    same_bundle_pids = sorted(
        {*scoped_pids, *categories["unscoped_same_bundle"]}
    )
    summarized["all_scoped_processes"] = summarize_category(scoped_pids, table)
    summarized["all_scoped_and_same_bundle_processes"] = summarize_category(
        same_bundle_pids, table
    )
    return summarized


def category_pid_set(categories: dict[str, list[int]]) -> set[int]:
    category_names = [
        *scoped_category_names(categories),
        "unscoped_same_bundle",
    ]
    return {pid for name in category_names for pid in categories[name]}


def physical_footprint(
    topology: RunTopology,
    *,
    expected_hosts: int | None = None,
    expected_renderers: int | None = None,
    expected_terminal_ids: set[str] | None = None,
) -> dict[str, Any]:
    last_detail = "process topology changed"
    for attempt in range(1, FOOTPRINT_ATTEMPTS + 1):
        table = process_table()
        categories = app_categories(topology, table)
        if expected_hosts is not None:
            require(
                len(categories["terminal_hosts"]) == expected_hosts,
                "durable host count changed before footprint collection",
            )
        if expected_renderers is not None:
            require(
                len(categories["terminal_renderers"]) == expected_renderers,
                "renderer count changed before footprint collection",
            )
        if expected_terminal_ids is not None:
            require(
                terminal_host_record_ids(topology.terminal_host_root)
                == expected_terminal_ids,
                "terminal-host identities changed before footprint collection",
            )
        category_names = [
            *scoped_category_names(categories),
            "unscoped_same_bundle",
        ]
        requested_pids = category_pid_set(categories)
        require(bool(requested_pids), "signed app has no live processes")
        with tempfile.TemporaryDirectory(prefix="cmux-browser-footprint-") as temporary:
            output = Path(temporary) / "footprint.json"
            subprocess.run(
                ["footprint", "-j", str(output), *map(str, sorted(requested_pids))],
                check=True,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.PIPE,
                text=True,
            )
            try:
                value = json.loads(output.read_text())
            except (OSError, json.JSONDecodeError) as error:
                raise BenchmarkFailure(f"invalid footprint output: {error}") from error
        require(isinstance(value, dict), "footprint output is not an object")
        processes = value.get("processes")
        require(isinstance(processes, list), "footprint output omitted processes")
        returned_pids = {
            process.get("pid")
            for process in processes
            if isinstance(process, dict) and isinstance(process.get("pid"), int)
        }
        after_table = process_table()
        try:
            after_categories = app_categories(topology, after_table)
            after_pids = category_pid_set(after_categories)
            if expected_hosts is not None:
                require(
                    len(after_categories["terminal_hosts"]) == expected_hosts,
                    "durable host count changed during footprint collection",
                )
            if expected_renderers is not None:
                require(
                    len(after_categories["terminal_renderers"])
                    == expected_renderers,
                    "renderer count changed during footprint collection",
                )
            if expected_terminal_ids is not None:
                require(
                    terminal_host_record_ids(topology.terminal_host_root)
                    == expected_terminal_ids,
                    "terminal-host identities changed during footprint collection",
                )
        except BenchmarkFailure as error:
            last_detail = str(error)
            after_pids = set()
        if (
            returned_pids != requested_pids
            or len(processes) != len(returned_pids)
            or after_pids != requested_pids
        ):
            last_detail = (
                f"requested={sorted(requested_pids)} returned={sorted(returned_pids)} "
                f"after={sorted(after_pids)}"
            )
            if attempt < FOOTPRINT_ATTEMPTS:
                time.sleep(POLL_INTERVAL_SECONDS)
                continue
            raise TopologyFailure(
                f"footprint process set did not stabilize after {attempt} attempts: "
                f"{last_detail}"
            )

        total = value.get("total footprint")
        require(isinstance(total, int), "footprint output omitted total footprint")
        per_category: dict[str, int] = {name: 0 for name in category_names}
        pid_category = {
            pid: name for name in category_names for pid in categories[name]
        }
        for process in processes:
            pid = process.get("pid")
            footprint = process.get("footprint")
            require(
                isinstance(pid, int)
                and pid in pid_category
                and isinstance(footprint, int),
                "footprint output contained a malformed process",
            )
            per_category[pid_category[pid]] += footprint
        return {
            "total_bytes": total,
            "by_category_bytes": per_category,
            "process_count": len(processes),
            "requested_pids": sorted(requested_pids),
            "included_same_bundle_pids": categories["unscoped_same_bundle"],
        }
    raise AssertionError(last_detail)


def percentile(values: list[float], percent: float) -> float:
    ordered = sorted(values)
    index = max(0, min(len(ordered) - 1, int((len(ordered) * percent + 99) // 100) - 1))
    return ordered[index]


def sampled_resources(
    topology: RunTopology,
    samples: int,
    interval: float,
    *,
    expected_hosts: int | None = None,
    expected_renderers: int | None = None,
    expected_terminal_ids: set[str] | None = None,
) -> dict[str, Any]:
    snapshots: list[dict[str, Any]] = []
    sample_offsets: list[float] = []
    sample_durations: list[float] = []
    started_monotonic = time.monotonic()
    started_unix = time.time()
    for index in range(samples):
        sample_started = time.monotonic()
        sample_offsets.append(round(sample_started - started_monotonic, 6))
        snapshot = resource_snapshot(topology)
        if expected_hosts is not None:
            require(
                snapshot["terminal_hosts"]["count"] == expected_hosts,
                f"durable host count changed during sampling: "
                f"expected {expected_hosts}, found {snapshot['terminal_hosts']['count']}",
            )
        if expected_renderers is not None:
            require(
                snapshot["terminal_renderers"]["count"] == expected_renderers,
                f"renderer count changed during sampling: expected {expected_renderers}, "
                f"found {snapshot['terminal_renderers']['count']}",
            )
        if expected_terminal_ids is not None:
            require(
                terminal_host_record_ids(topology.terminal_host_root)
                == expected_terminal_ids,
                "terminal-host record identities changed during sampling",
            )
        snapshots.append(snapshot)
        sample_durations.append(round(time.monotonic() - sample_started, 6))
        if index + 1 < samples:
            time.sleep(interval)
    names = snapshots[0].keys()
    summary: dict[str, Any] = {}
    for name in names:
        counts = [snapshot[name]["count"] for snapshot in snapshots]
        rss = [float(snapshot[name]["rss_total_kib"]) for snapshot in snapshots]
        cpu = [float(snapshot[name]["cpu_total_percent"]) for snapshot in snapshots]
        summary[name] = {
            "count_min": min(counts),
            "count_max": max(counts),
            "rss_total_kib_median": round(statistics.median(rss), 1),
            "rss_total_kib_p95": round(percentile(rss, 95), 1),
            "cpu_total_percent_median": round(statistics.median(cpu), 3),
            "cpu_total_percent_p95": round(percentile(cpu, 95), 3),
            "final": snapshots[-1][name],
        }
    summary["_sampling"] = {
        "started_at_unix": started_unix,
        "sample_offsets_seconds": sample_offsets,
        "sample_durations_seconds": sample_durations,
        "elapsed_seconds": round(time.monotonic() - started_monotonic, 6),
    }
    return summary


def wait_for_resource_counts(
    topology: RunTopology,
    *,
    expected_hosts: int | None,
    expected_renderers: int | None,
    expected_terminal_ids: set[str] | None,
    timeout: float,
) -> dict[str, Any]:
    matched_since: float | None = None
    latest: dict[str, Any] | None = None

    def stable_snapshot() -> dict[str, Any] | None:
        nonlocal matched_since, latest
        latest = resource_snapshot(topology)
        hosts_match = (
            expected_hosts is None
            or latest["terminal_hosts"]["count"] == expected_hosts
        )
        renderers_match = (
            expected_renderers is None
            or latest["terminal_renderers"]["count"] == expected_renderers
        )
        ids_match = (
            expected_terminal_ids is None
            or terminal_host_record_ids(topology.terminal_host_root)
            == expected_terminal_ids
        )
        if not (hosts_match and renderers_match and ids_match):
            matched_since = None
            return None
        now = time.monotonic()
        if matched_since is None:
            matched_since = now
            return None
        return latest if now - matched_since >= RESOURCE_STABLE_SECONDS else None

    descriptions: list[str] = []
    if expected_hosts is not None:
        descriptions.append(f"{expected_hosts} durable terminal hosts")
    if expected_renderers is not None:
        descriptions.append(f"{expected_renderers} renderer utilities")
    if expected_terminal_ids is not None:
        descriptions.append("the exact terminal-host identity set")
    return wait_until(
        stable_snapshot,
        " and ".join(descriptions) or "a stable process topology",
        timeout,
    )


def is_canonical_uuid(value: Any) -> bool:
    if not isinstance(value, str):
        return False
    try:
        return str(uuid.UUID(value)) == value
    except ValueError:
        return False


def command_init(args: argparse.Namespace) -> dict[str, Any]:
    require(not args.state.exists(), f"state file already exists: {args.state}")
    app = args.app.resolve()
    client = JsonLineClient(args.socket.resolve(), args.timeout)
    try:
        identity = client.request("identify")
        client.request("set-client-info", name=ORIGIN, kind="benchmark")
        topology = build_run_topology(app, client, identity)
        terminals = list_terminals(client)
        baseline_ids = sorted(live_terminal_ids(terminals))
        require(
            len(baseline_ids) == args.expected_baseline,
            f"expected {args.expected_baseline} baseline terminals, found {len(baseline_ids)}",
        )
        workspaces = list_workspaces(client)
        workspace_items = [
            item
            for item in workspaces["workspaces"]
            if isinstance(item, dict)
        ]
        require(
            len(workspace_items) == len(workspaces["workspaces"]),
            "workspace registry contains a non-object entry",
        )
        invalid_keys = [
            item.get("key") for item in workspace_items if not is_canonical_uuid(item.get("key"))
        ]
        require(
            not invalid_keys,
            f"workspace registry contains noncanonical keys: {invalid_keys!r}",
        )
        sources = [item for item in workspace_items if item.get("name") == args.source_name]
        require(
            len(sources) == 1,
            f"expected one workspace named {args.source_name!r}, found {len(sources)}",
        )
        source = sources[0]
        wait_for_resource_counts(
            topology,
            expected_hosts=len(baseline_ids),
            expected_renderers=None,
            expected_terminal_ids=set(baseline_ids),
            timeout=args.timeout,
        )
        require(
            live_terminal_ids(list_terminals(client)) == set(baseline_ids),
            "terminal registry changed during benchmark initialization",
        )
        # Canonical workspace keys are durable-address UUIDs shared by cmux
        # TUI and Browser. A descriptive/non-UUID key can be stored by older
        # daemons but cannot be represented by Browser's workspace host map.
        target_key = str(uuid.uuid4())
        mutation_id = f"create-workspace-{uuid.uuid4().hex}"
        # Persist the exact cleanup identity before sending the mutation. If
        # the client dies after the daemon commits but before the response is
        # observed, `cleanup` can still find and close this workspace.
        state = {
            "schema_version": SCHEMA_VERSION,
            "app": str(app),
            "socket": str(args.socket.resolve()),
            "registry_id": identity.get("registry_id"),
            "generation": identity.get("generation"),
            "session": topology.session,
            "daemon_pid_at_init": topology.daemon_pid,
            "browser_pid_at_init": topology.browser_pid,
            "cmux_tui_commit": topology.cmux_tui_commit,
            "ghostty_commit": topology.ghostty_commit,
            "terminal_host_root": str(topology.terminal_host_root),
            "baseline_terminal_ids": baseline_ids,
            "source_workspace_key": source["key"],
            "target_workspace_key": target_key,
            "target_workspace_name": args.target_name,
            "workspace_mutation_id": mutation_id,
            "workspace_created": False,
            "created_terminal_ids": [],
            "observed_terminal_ids": [],
            "created_terminal_hosts": [],
            "closed_terminal_ids": [],
            "checkpoints": [],
            "cleanup_complete": False,
        }
        atomic_write(args.state, state)
        created = client.request(
            "create-workspace",
            key=target_key,
            name=args.target_name,
            origin=ORIGIN,
            mutation_id=mutation_id,
            expected_generation=identity.get("generation"),
            expected_revision=workspaces["workspace_revision"],
        )
        require(created.get("key") == target_key, "created the wrong workspace")
        state["workspace_created"] = True
        atomic_write(args.state, state)
        return {"result": "ok", "operation": "init", "state": state}
    finally:
        client.close()


def command_grow(args: argparse.Namespace) -> dict[str, Any]:
    state = load_state(args.state)
    require(not state.get("cleanup_complete"), "benchmark run was already cleaned up")
    app = Path(state["app"])
    client, identity = connect_state(state, args.timeout)
    try:
        topology = build_run_topology(app, client, identity)
        terminals = list_terminals(client)
        current_ids = live_terminal_ids(terminals)
        baseline = set(state["baseline_terminal_ids"])
        if adopt_ambiguous_owned_terminals(state, current_ids):
            atomic_write(args.state, state)
        validate_owned_terminal_ids(state, current_ids, active_run=True)
        require(args.target >= len(baseline), "target is below the baseline")
        require(args.target >= len(current_ids), "target is below the current terminal count")
        while len(current_ids) < args.target:
            terminal_id = uuid.uuid4().hex
            mutation_id = f"create-terminal-{terminal_id}"
            # Write ahead for the same ambiguous-response window as workspace
            # creation. A nonexistent planned id is harmless during cleanup;
            # an id committed before a client interruption is not leaked.
            state["created_terminal_ids"].append(terminal_id)
            atomic_write(args.state, state)
            created = client.request(
                "create-terminal",
                key=state["source_workspace_key"],
                argv=[args.command],
                terminal_id=terminal_id,
                origin=ORIGIN,
                mutation_id=mutation_id,
                expected_generation=identity.get("generation"),
                expected_terminal_revision=terminals["terminal_revision"],
                cols=args.cols,
                rows=args.rows,
            )
            require(created.get("terminal_id") == terminal_id, "created the wrong terminal")

            def terminal_running() -> dict[str, Any] | None:
                latest = list_terminals(client)
                for item in latest["terminals"]:
                    if item.get("terminal_id") == terminal_id and item.get("lifecycle") == "running":
                        return latest
                return None

            terminals = wait_until(
                terminal_running, f"terminal {terminal_id} to become running", args.timeout
            )
            current_ids = live_terminal_ids(terminals)
            validate_owned_terminal_ids(state, current_ids, active_run=True)
            observed = set(state.get("observed_terminal_ids", []))
            observed.add(terminal_id)
            state["observed_terminal_ids"] = sorted(observed)
            host_identity = wait_until(
                lambda: terminal_host_identity(topology, terminal_id),
                f"terminal-host identity for {terminal_id}",
                args.timeout,
            )
            state.setdefault("created_terminal_hosts", []).append(host_identity)
            atomic_write(args.state, state)

        terminals = list_terminals(client)
        current_ids = live_terminal_ids(terminals)
        validate_owned_terminal_ids(state, current_ids, active_run=True)
        require(
            len(current_ids) == args.target,
            f"terminal count changed before measurement: expected {args.target}, "
            f"found {len(current_ids)}",
        )
        snapshot = wait_for_resource_counts(
            topology,
            expected_hosts=args.target,
            expected_renderers=args.expected_renderers,
            expected_terminal_ids=current_ids,
            timeout=args.renderer_timeout,
        )
        final_ids = live_terminal_ids(list_terminals(client))
        validate_owned_terminal_ids(state, final_ids, active_run=True)
        require(final_ids == current_ids, "terminal registry changed while processes settled")
        return {
            "result": "ok",
            "operation": "grow",
            "target_terminals": args.target,
            "live_terminals": len(final_ids),
            "resources": snapshot,
        }
    finally:
        client.close()


def command_snapshot(args: argparse.Namespace) -> dict[str, Any]:
    state = load_state(args.state)
    app = Path(state["app"])
    client, identity = connect_state(state, args.timeout)
    try:
        topology = build_run_topology(app, client, identity)
        terminals = list_terminals(client)
        current_ids = live_terminal_ids(terminals)
        validate_owned_terminal_ids(
            state, current_ids, active_run=not state.get("cleanup_complete", False)
        )
        expected_hosts = (
            args.expected_hosts
            if args.expected_hosts is not None
            else len(current_ids)
        )
        wait_for_resource_counts(
            topology,
            expected_hosts=expected_hosts,
            expected_renderers=args.expected_renderers,
            expected_terminal_ids=current_ids,
            timeout=args.timeout,
        )
        result: dict[str, Any] = {
            "result": "ok",
            "operation": "snapshot",
            "label": args.label,
            "live_terminals": len(current_ids),
            "samples": args.samples,
            "sample_interval_seconds": args.interval,
            "resources": sampled_resources(
                topology,
                args.samples,
                args.interval,
                expected_hosts=expected_hosts,
                expected_renderers=args.expected_renderers,
                expected_terminal_ids=current_ids,
            ),
        }
        if args.footprint:
            result["physical_footprint"] = physical_footprint(
                topology,
                expected_hosts=expected_hosts,
                expected_renderers=args.expected_renderers,
                expected_terminal_ids=current_ids,
            )
        final_ids = live_terminal_ids(list_terminals(client))
        validate_owned_terminal_ids(
            state, final_ids, active_run=not state.get("cleanup_complete", False)
        )
        require(
            final_ids == current_ids,
            "terminal registry changed during snapshot collection",
        )
        if args.label:
            state.setdefault("checkpoints", []).append(result)
            atomic_write(args.state, state)
        return result
    finally:
        client.close()


def command_cleanup(args: argparse.Namespace) -> dict[str, Any]:
    state = load_state(args.state)
    if state.get("cleanup_complete"):
        return {"result": "ok", "operation": "cleanup", "already_clean": True}
    app = Path(state["app"])
    # Cleanup is an ownership operation, not a benchmark measurement. A
    # development rebuild must not strand 100 write-ahead-recorded terminals,
    # so accept a changed binary while retaining registry/session/socket and
    # peer-process identity checks.
    client, identity = connect_state(state, args.timeout, enforce_build=False)
    try:
        daemon = validate_daemon_connection(client, identity)
        terminal_host_root = cleanup_terminal_host_root(state, client, daemon)
        for terminal_id in state["created_terminal_ids"]:
            terminals = list_terminals(client)
            if terminal_id in live_terminal_ids(terminals):
                client.request(
                    "close-terminal",
                    terminal_id=terminal_id,
                    origin=ORIGIN,
                    mutation_id=f"close-terminal-{terminal_id}",
                    expected_generation=identity.get("generation"),
                    expected_terminal_revision=terminals["terminal_revision"],
                )
            if terminal_id not in state.setdefault("closed_terminal_ids", []):
                state["closed_terminal_ids"].append(terminal_id)
                atomic_write(args.state, state)

        created_ids = set(state["created_terminal_ids"])

        def terminals_clean() -> set[str] | None:
            current = live_terminal_ids(list_terminals(client))
            return current if current.isdisjoint(created_ids) else None

        remaining_ids = wait_until(
            terminals_clean, "created terminals to close", args.renderer_timeout
        )

        created_hosts = state.get("created_terminal_hosts", [])

        def created_host_processes_exited() -> bool | None:
            table = process_table()
            for host in created_hosts:
                if isinstance(host, dict) and exact_host_identity_is_live(
                    terminal_host_root, host, table
                ):
                    return None
            return True

        wait_until(
            created_host_processes_exited,
            "created terminal-host processes to exit",
            args.renderer_timeout,
        )

        wait_until(
            lambda: settled_terminal_host_identities(
                terminal_host_root, remaining_ids
            ),
            "remaining terminal-host records and exact processes to settle",
            args.renderer_timeout,
        )
        workspaces = list_workspaces(client)
        if any(
            item.get("key") == state["target_workspace_key"]
            for item in workspaces["workspaces"]
            if isinstance(item, dict)
        ):
            client.request(
                "close-workspace",
                key=state["target_workspace_key"],
                expected_revision=workspaces["workspace_revision"],
            )
            workspaces = list_workspaces(client)
            require(
                not any(
                    isinstance(item, dict)
                    and item.get("key") == state["target_workspace_key"]
                    for item in workspaces["workspaces"]
                ),
                "benchmark workspace remained after close-workspace",
            )
        baseline_count = len(state["baseline_terminal_ids"])
        remaining_count = len(remaining_ids)
        state["cleanup_complete"] = True
        atomic_write(args.state, state)
        result: dict[str, Any] = {
            "result": "ok",
            "operation": "cleanup",
            "baseline_terminals": baseline_count,
            "remaining_terminals": remaining_count,
            "remaining_terminal_host_ids": sorted(remaining_ids),
        }
        try:
            topology = build_run_topology(
                app, client, identity, require_browser=False
            )
            result["resources"] = resource_snapshot(topology)
        except (BenchmarkFailure, OSError, subprocess.SubprocessError) as error:
            result["resources"] = None
            result["resources_unavailable"] = str(error)
        return result
    finally:
        client.close()


def add_common(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--state", type=Path, required=True)
    parser.add_argument("--timeout", type=float, default=20.0)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="operation", required=True)

    init = subparsers.add_parser("init", help="record baseline and create an empty latency workspace")
    add_common(init)
    init.add_argument("--app", type=Path, required=True)
    init.add_argument("--socket", type=Path, required=True)
    init.add_argument("--expected-baseline", type=int, default=1)
    init.add_argument("--source-name", default="Home")
    init.add_argument("--target-name", default="Latency Target")
    init.set_defaults(handler=command_init)

    grow = subparsers.add_parser("grow", help="grow canonical terminals to an absolute total")
    add_common(grow)
    grow.add_argument("--target", type=int, required=True)
    grow.add_argument("--renderer-timeout", type=float, default=180.0)
    grow.add_argument("--expected-renderers", type=int)
    grow.add_argument("--command", default="/bin/cat")
    grow.add_argument("--cols", type=int, default=80)
    grow.add_argument("--rows", type=int, default=24)
    grow.set_defaults(handler=command_grow)

    snapshot = subparsers.add_parser("snapshot", help="sample signed-app process resources")
    add_common(snapshot)
    snapshot.add_argument("--label")
    snapshot.add_argument("--samples", type=int, default=5)
    snapshot.add_argument("--interval", type=float, default=0.5)
    snapshot.add_argument("--footprint", action="store_true")
    snapshot.add_argument("--expected-hosts", type=int)
    snapshot.add_argument("--expected-renderers", type=int)
    snapshot.set_defaults(handler=command_snapshot)

    cleanup = subparsers.add_parser("cleanup", help="close only this run's terminals/workspace")
    add_common(cleanup)
    cleanup.add_argument("--renderer-timeout", type=float, default=180.0)
    cleanup.set_defaults(handler=command_cleanup)

    args = parser.parse_args()
    if args.timeout <= 0:
        parser.error("--timeout must be positive")
    if hasattr(args, "target") and args.target <= 0:
        parser.error("--target must be positive")
    if hasattr(args, "samples") and args.samples <= 0:
        parser.error("--samples must be positive")
    if hasattr(args, "interval") and args.interval < 0:
        parser.error("--interval cannot be negative")
    for name in ("expected_hosts", "expected_renderers"):
        value = getattr(args, name, None)
        if value is not None and value < 0:
            parser.error(f"--{name.replace('_', '-')} cannot be negative")
    state_lock = None
    session_lock = None
    try:
        state_lock = acquire_state_lock(args.state)
        if hasattr(args, "socket"):
            socket_path = args.socket
        else:
            socket_path = Path(load_state(args.state)["socket"])
        session_lock = acquire_session_lock(socket_path)
        result = args.handler(args)
        print(json.dumps(result, indent=2, sort_keys=True))
    except (BenchmarkFailure, OSError, subprocess.SubprocessError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 1
    finally:
        if session_lock is not None:
            session_lock.close()
        if state_lock is not None:
            state_lock.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
