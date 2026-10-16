#!/usr/bin/env python3
"""End-to-end smoke test for the durable cmux-tui browser backend.

The test speaks the browser JSON-lines protocol directly to the exact staged
helper.  It deliberately loses mutation responses, kills and restarts the mux
daemon, and verifies the state that must survive in the workspace registry and
per-terminal host processes.

The runtime is POSIX-only because durable terminal hosts and AF_UNIX are the
production implementation on macOS and Linux.
"""

from __future__ import annotations

import argparse
import base64
import errno
import json
import os
from pathlib import Path
import re
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time
from typing import Any, Callable, TypeVar
import uuid


ROOT = Path(__file__).resolve().parent.parent
REVISION_HEADER = ROOT / "overlay/chrome/browser/cmux_term/cmux_tui_revision.h"
PIN_PATTERN = re.compile(r'"([0-9a-f]{40})"')
ORIGIN = "cmux-browser-durable-smoke"
T = TypeVar("T")


class SmokeFailure(RuntimeError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise SmokeFailure(message)


class JsonLineClient:
    def __init__(self, path: Path, timeout: float) -> None:
        self._socket = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self._socket.settimeout(timeout)
        self._socket.connect(str(path))
        self._reader = self._socket.makefile("rb")
        self._next_id = 1
        self._closed = False

    def close(self) -> None:
        if self._closed:
            return
        self._closed = True
        try:
            self._reader.close()
        finally:
            self._socket.close()

    def read(self) -> dict[str, Any]:
        try:
            line = self._reader.readline()
        except TimeoutError as error:
            raise SmokeFailure("timed out reading a cmux-tui JSON-lines response") from error
        if not line:
            raise SmokeFailure("cmux-tui closed a JSON-lines connection")
        try:
            value = json.loads(line)
        except json.JSONDecodeError as error:
            raise SmokeFailure(f"invalid JSON-lines response: {line!r}") from error
        if not isinstance(value, dict):
            raise SmokeFailure(f"JSON-lines response is not an object: {value!r}")
        return value

    def request(self, command: str, **fields: Any) -> tuple[dict[str, Any], list[dict[str, Any]]]:
        request_id = self._next_id
        self._next_id += 1
        payload = {"id": request_id, "cmd": command, **fields}
        self._socket.sendall(json.dumps(payload, separators=(",", ":")).encode() + b"\n")
        events: list[dict[str, Any]] = []
        while True:
            value = self.read()
            if "event" in value:
                events.append(value)
                continue
            if value.get("id") != request_id:
                continue
            if value.get("ok") is not True:
                raise SmokeFailure(f"{command} failed: {value.get('error', 'unknown error')}")
            data = value.get("data", {})
            if not isinstance(data, dict):
                raise SmokeFailure(f"{command} returned non-object data: {data!r}")
            return data, events


def send_without_reading_response(
    socket_path: Path,
    timeout: float,
    command: str,
    **fields: Any,
) -> socket.socket:
    """Send one complete mutation, then model a frontend losing its reply."""

    stream = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    try:
        stream.settimeout(timeout)
        stream.connect(str(socket_path))
        payload = {"id": 1, "cmd": command, **fields}
        stream.sendall(json.dumps(payload, separators=(",", ":")).encode() + b"\n")
        # Keep the connection alive while a second client observes the commit.
        # Its success response may enter this socket's receive buffer, but is
        # intentionally never consumed. Closing it after observation models a
        # frontend that disconnected before learning the outcome without
        # racing server-side connection teardown against request dispatch.
        return stream
    except BaseException:
        stream.close()
        raise


def pinned_revisions() -> tuple[str, str]:
    revisions = PIN_PATTERN.findall(REVISION_HEADER.read_text())
    if len(revisions) != 2:
        raise SmokeFailure(f"could not read both revisions from {REVISION_HEADER}")
    cmux_revision, ghostty_revision = revisions
    root_ghostty_revision = (ROOT / "ghostty-revision.txt").read_text().strip()
    if ghostty_revision != root_ghostty_revision:
        raise SmokeFailure("runtime and root Ghostty pins disagree")
    return cmux_revision, ghostty_revision


def validate_identity(
    client: JsonLineClient,
    cmux_revision: str,
    ghostty_revision: str,
) -> dict[str, Any]:
    identity, _ = client.request("identify")
    expected = {
        "app": "cmux-tui",
        "protocol": 9,
        "build_commit": cmux_revision,
        "ghostty_commit": ghostty_revision,
    }
    for key, value in expected.items():
        if identity.get(key) != value:
            raise SmokeFailure(
                f"identity {key} mismatch: expected {value!r}, got {identity.get(key)!r}"
            )
    for key in ("registry_id", "generation"):
        require(isinstance(identity.get(key), str) and bool(identity[key]), f"missing {key}")
    return identity


def decode_event_bytes(event: dict[str, Any], field: str = "data") -> bytes:
    encoded = event.get(field)
    if not isinstance(encoded, str):
        raise SmokeFailure(f"{event.get('event')} is missing base64 {field}")
    try:
        return base64.b64decode(encoded, validate=True)
    except ValueError as error:
        raise SmokeFailure(f"{event.get('event')} contains invalid base64 {field}") from error


def wait_until(callback: Callable[[], T | None], description: str, timeout: float) -> T:
    deadline = time.monotonic() + timeout
    last_error: Exception | None = None
    while time.monotonic() < deadline:
        try:
            result = callback()
        except (OSError, SmokeFailure) as error:
            last_error = error
        else:
            if result is not None:
                return result
        time.sleep(0.03)
    suffix = f": {last_error}" if last_error is not None else ""
    raise SmokeFailure(f"timed out waiting for {description}{suffix}")


def start_daemon(
    binary: Path,
    session: str,
    socket_path: Path,
    state_path: Path,
    timeout: float,
) -> subprocess.Popen[str]:
    try:
        socket_path.unlink()
    except FileNotFoundError:
        pass
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
        stderr=subprocess.PIPE,
        text=True,
    )
    deadline = time.monotonic() + timeout
    last_error: OSError | None = None
    while time.monotonic() < deadline:
        if process.poll() is not None:
            stderr = process.stderr.read().strip() if process.stderr is not None else ""
            if process.stderr is not None:
                process.stderr.close()
            raise SmokeFailure(f"cmux-tui exited before accepting control: {stderr}")
        if socket_path.exists():
            try:
                probe = JsonLineClient(socket_path, min(timeout, 1.0))
            except OSError as error:
                last_error = error
            else:
                probe.close()
                return process
        time.sleep(0.02)
    process.kill()
    process.wait()
    stderr = process.stderr.read().strip() if process.stderr is not None else ""
    if process.stderr is not None:
        process.stderr.close()
    suffix = f": {last_error}" if last_error is not None else ""
    if stderr:
        suffix += f"; stderr: {stderr}"
    raise SmokeFailure(f"timed out waiting for the cmux-tui socket{suffix}")


def stop_daemon(process: subprocess.Popen[str], timeout: float, kill: bool = False) -> str:
    if process.poll() is None:
        if kill:
            process.kill()
        else:
            process.send_signal(signal.SIGTERM)
        try:
            process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
    stderr = process.stderr.read() if process.stderr is not None else ""
    if process.stderr is not None:
        process.stderr.close()
    return stderr


def list_workspaces(client: JsonLineClient) -> dict[str, Any]:
    data, _ = client.request("list-workspaces")
    require(isinstance(data.get("workspaces"), list), "list-workspaces omitted workspaces")
    require(isinstance(data.get("workspace_revision"), int), "invalid workspace revision")
    return data


def list_terminals(client: JsonLineClient) -> dict[str, Any]:
    data, _ = client.request("list-terminals")
    require(isinstance(data.get("terminals"), list), "list-terminals omitted terminals")
    require(isinstance(data.get("terminal_revision"), int), "invalid terminal revision")
    return data


def workspace_with_key(snapshot: dict[str, Any], key: str) -> dict[str, Any] | None:
    return next((item for item in snapshot["workspaces"] if item.get("key") == key), None)


def terminal_with_id(snapshot: dict[str, Any], terminal_id: str) -> dict[str, Any] | None:
    return next(
        (item for item in snapshot["terminals"] if item.get("terminal_id") == terminal_id),
        None,
    )


def wait_for_terminal(
    client: JsonLineClient,
    terminal_id: str,
    predicate: Callable[[dict[str, Any]], bool],
    description: str,
    timeout: float,
) -> tuple[dict[str, Any], dict[str, Any]]:
    def check() -> tuple[dict[str, Any], dict[str, Any]] | None:
        snapshot = list_terminals(client)
        terminal = terminal_with_id(snapshot, terminal_id)
        if terminal is not None and predicate(terminal):
            return snapshot, terminal
        return None

    return wait_until(check, description, timeout)


def wait_for_terminal_absence(
    client: JsonLineClient,
    terminal_id: str,
    timeout: float,
) -> dict[str, Any]:
    def check() -> dict[str, Any] | None:
        snapshot = list_terminals(client)
        return snapshot if terminal_with_id(snapshot, terminal_id) is None else None

    return wait_until(check, f"terminal {terminal_id} to disappear", timeout)


def wait_for_resolved_surface(
    client: JsonLineClient,
    terminal_id: str,
    incarnation: str,
    timeout: float,
) -> tuple[int, dict[str, Any]]:
    def check() -> tuple[int, dict[str, Any]] | None:
        data, _ = client.request("resolve-terminal", terminal_id=terminal_id)
        surface = data.get("surface")
        if (
            data.get("lifecycle") == "running"
            and isinstance(surface, int)
            and not isinstance(surface, bool)
            and surface > 0
        ):
            require(data.get("terminal_incarnation") == incarnation, "incarnation changed")
            return surface, data
        return None

    return wait_until(check, f"terminal {terminal_id} to resolve as running", timeout)


def read_screen(control: JsonLineClient, surface: int) -> str:
    data, _ = control.request("read-screen", surface=surface)
    text = data.get("text")
    if not isinstance(text, str):
        raise SmokeFailure("read-screen did not return text")
    return text


def wait_for_screen(
    control: JsonLineClient,
    surface: int,
    marker: str,
    timeout: float,
) -> str:
    return wait_until(
        lambda: (text if marker in (text := read_screen(control, surface)) else None),
        f"{marker!r} on surface {surface}",
        timeout,
    )


def send_marker(control: JsonLineClient, surface: int, marker: str) -> None:
    control.request(
        "send",
        surface=surface,
        bytes=base64.b64encode(f"{marker}\n".encode()).decode(),
    )


def vt_size(control: JsonLineClient, surface: int) -> tuple[int, int]:
    data, _ = control.request("vt-state", surface=surface)
    cols, rows = data.get("cols"), data.get("rows")
    require(isinstance(cols, int) and isinstance(rows, int), "vt-state omitted its size")
    return cols, rows


def wait_for_vt_size(
    control: JsonLineClient,
    surface: int,
    expected: tuple[int, int],
    timeout: float,
) -> tuple[int, int]:
    return wait_until(
        lambda: (size if (size := vt_size(control, surface)) == expected else None),
        f"surface {surface} size {expected[0]}x{expected[1]}",
        timeout,
    )


def attach_surface(
    socket_path: Path,
    timeout: float,
    surface: int,
    cols: int,
    rows: int,
    cmux_revision: str,
    ghostty_revision: str,
) -> tuple[JsonLineClient, dict[str, Any]]:
    attachment = JsonLineClient(socket_path, timeout)
    validate_identity(attachment, cmux_revision, ghostty_revision)
    _, events = attachment.request("attach-surface", surface=surface, cols=cols, rows=rows)
    initial = next((event for event in events if event.get("event") == "vt-state"), None)
    if initial is None:
        attachment.close()
        raise SmokeFailure("attach response arrived without an initial vt-state event")
    decode_event_bytes(initial)
    return attachment, initial


def stable_token(value: str) -> str:
    result = 0xCBF29CE484222325
    for byte in value.encode():
        result ^= byte
        result = (result * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return f"{result:016x}"


def read_host_record(host_root: Path, terminal_id: str) -> dict[str, Any] | None:
    path = host_root / f"{terminal_id}.json"
    try:
        value = json.loads(path.read_text())
    except FileNotFoundError:
        return None
    except json.JSONDecodeError as error:
        raise SmokeFailure(f"invalid terminal host record {path}") from error
    require(isinstance(value, dict), f"terminal host record is not an object: {path}")
    require(value.get("terminal_id") == terminal_id, f"host record identity mismatch: {path}")
    return value


def wait_for_host_record(host_root: Path, terminal_id: str, timeout: float) -> dict[str, Any]:
    return wait_until(
        lambda: read_host_record(host_root, terminal_id),
        f"host record for {terminal_id}",
        timeout,
    )


def wait_for_host_record_absence(host_root: Path, terminal_id: str, timeout: float) -> None:
    def check() -> bool | None:
        return True if read_host_record(host_root, terminal_id) is None else None

    wait_until(check, f"host record for {terminal_id} to disappear", timeout)


def pid_is_alive(pid: int) -> bool:
    try:
        os.kill(pid, 0)
    except OSError as error:
        return error.errno == errno.EPERM
    return True


def host_record_proves_live(path: Path, record: dict[str, Any]) -> bool:
    """Mirror the host's PID-reuse-safe liveness lease check."""

    import fcntl

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


def cleanup_recorded_hosts(host_root: Path, timeout: float) -> None:
    """Last-resort cleanup for hosts left alive by an assertion or SIGKILL."""

    records: list[tuple[Path, dict[str, Any]]] = []
    if host_root.is_dir():
        for path in host_root.glob("*.json"):
            try:
                value = json.loads(path.read_text())
            except (OSError, json.JSONDecodeError):
                continue
            if isinstance(value, dict) and isinstance(value.get("host_pid"), int):
                records.append((path, value))
    for path, record in records:
        pid = record["host_pid"]
        if pid > 0 and host_record_proves_live(path, record) and pid_is_alive(pid):
            try:
                os.kill(pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if not any(host_record_proves_live(path, record) for path, record in records):
            break
        time.sleep(0.03)
    for path, record in records:
        pid = record["host_pid"]
        if pid > 0 and host_record_proves_live(path, record) and pid_is_alive(pid):
            try:
                os.kill(pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
        endpoint = record.get("endpoint")
        if isinstance(endpoint, str):
            endpoint_path = Path(endpoint)
            if endpoint_path.name == f"{record.get('terminal_id')}.sock":
                try:
                    endpoint_path.unlink()
                except FileNotFoundError:
                    pass
                except OSError:
                    pass
        try:
            path.unlink()
        except FileNotFoundError:
            pass


def assert_terminal_events_contiguous(
    client: JsonLineClient,
    registry_id: str,
    generation: str,
    after_revision: int = 0,
) -> dict[str, Any]:
    data, _ = client.request("terminal-events", after_revision=after_revision)
    require(data.get("registry_id") == registry_id, "terminal-events registry changed")
    require(data.get("generation") == generation, "terminal-events generation changed")
    revision = data.get("terminal_revision")
    events = data.get("events")
    require(isinstance(revision, int), "terminal-events omitted terminal_revision")
    require(isinstance(events, list), "terminal-events omitted events")
    actual = [event.get("terminal_revision") for event in events]
    expected = list(range(after_revision + 1, revision + 1))
    require(actual == expected, f"terminal event gap: expected {expected}, got {actual}")
    require(all(item <= revision for item in actual), "event escaped the response snapshot fence")
    return data


def create_empty_workspace(
    control: JsonLineClient,
    identity: dict[str, Any],
    key: str,
    name: str,
    mutation_id: str,
) -> dict[str, Any]:
    before = list_workspaces(control)
    data, _ = control.request(
        "create-workspace",
        key=key,
        name=name,
        origin=ORIGIN,
        mutation_id=mutation_id,
        expected_generation=identity["generation"],
        expected_revision=before["workspace_revision"],
    )
    require(data.get("key") == key, f"created the wrong workspace for {key}")
    require(data.get("replayed") is False, f"workspace {key} unexpectedly replayed")
    snapshot = list_workspaces(control)
    workspace = workspace_with_key(snapshot, key)
    require(workspace is not None, f"empty workspace {key} missing from canonical snapshot")
    require(workspace.get("screens") == [], f"workspace {key} was not empty")
    return data


def run_smoke(binary: Path, timeout: float) -> None:
    if os.name != "posix" or not hasattr(socket, "AF_UNIX"):
        raise SmokeFailure("durable cmux-tui hosts require POSIX AF_UNIX support")
    if not binary.is_file() or not os.access(binary, os.X_OK):
        raise SmokeFailure(f"cmux-tui helper is not executable: {binary}")
    cat = shutil.which("cat")
    if cat is None:
        raise SmokeFailure("could not find cat for the deterministic PTY workload")

    cmux_revision, ghostty_revision = pinned_revisions()
    session = f"cmux-browser-smoke-{os.getpid()}-{uuid.uuid4().hex[:8]}"
    workspace_a = "browser-empty-a"
    workspace_b = "browser-empty-b"
    workspace_batch = "browser-batch-close"

    with tempfile.TemporaryDirectory(prefix="cmux-browser-tui-smoke-") as directory:
        root = Path(directory)
        socket_path = root / "control.sock"
        state_path = root / "state"
        host_root = state_path / f"terminal-hosts-{stable_token(session)}"
        daemon: subprocess.Popen[str] | None = None
        clients: list[JsonLineClient] = []
        event_count = 0
        try:
            daemon = start_daemon(binary, session, socket_path, state_path, timeout)
            control = JsonLineClient(socket_path, timeout)
            clients.append(control)
            first_identity = validate_identity(control, cmux_revision, ghostty_revision)
            require(
                first_identity.get("terminal_revision") == 0,
                "fresh terminal registry not empty",
            )
            require(
                first_identity.get("workspace_revision") == 0,
                "fresh workspace registry not empty",
            )
            control.request("set-client-info", name="cmux-browser-smoke", kind="browser")
            control.request("subscribe")

            create_empty_workspace(
                control, first_identity, workspace_a, "Empty A", "workspace-create-a"
            )
            create_empty_workspace(
                control, first_identity, workspace_b, "Empty B", "workspace-create-b"
            )
            empty_snapshot = list_workspaces(control)
            require(
                {item.get("key") for item in empty_snapshot["workspaces"]}
                == {workspace_a, workspace_b},
                "canonical empty workspace set is incomplete",
            )

            # Create exactly once while discarding the first response.
            terminal_id = uuid.uuid4().hex
            create_revision = list_terminals(control)["terminal_revision"]
            create_fields = {
                "key": workspace_a,
                "argv": [cat],
                "terminal_id": terminal_id,
                "origin": ORIGIN,
                "mutation_id": "terminal-create-survivor",
                "expected_generation": first_identity["generation"],
                "expected_terminal_revision": create_revision,
                "cols": 120,
                "rows": 50,
            }
            lost_create = send_without_reading_response(
                socket_path, timeout, "create-terminal", **create_fields
            )
            try:
                created_snapshot, created_terminal = wait_for_terminal(
                    control,
                    terminal_id,
                    lambda item: item.get("lifecycle") == "running",
                    "lost-response terminal create to commit",
                    timeout,
                )
            finally:
                lost_create.close()
            incarnation = created_terminal.get("terminal_incarnation")
            require(
                isinstance(incarnation, str) and bool(incarnation),
                "create omitted incarnation",
            )
            create_retry, _ = control.request("create-terminal", **create_fields)
            require(create_retry.get("replayed") is True, "terminal create retry was not replayed")
            require(
                create_retry.get("terminal_id") == terminal_id,
                "create retry changed terminal id",
            )
            require(
                create_retry.get("terminal_incarnation") == incarnation,
                "create retry changed terminal incarnation",
            )
            require(
                sum(
                    item.get("terminal_id") == terminal_id
                    for item in created_snapshot["terminals"]
                )
                == 1,
                "exactly-once create produced duplicate terminals",
            )
            surface, _ = wait_for_resolved_surface(
                control, terminal_id, incarnation, timeout
            )
            before_marker = "__CMUX_BEFORE_DAEMON_KILL__"
            send_marker(control, surface, before_marker)
            wait_for_screen(control, surface, before_marker, timeout)

            original_record = wait_for_host_record(host_root, terminal_id, timeout)
            require(
                original_record.get("incarnation") == incarnation,
                "host record incarnation mismatch",
            )
            original_pid = original_record.get("host_pid")
            original_endpoint = original_record.get("endpoint")
            original_nonce = original_record.get("host_start_nonce")
            require(
                isinstance(original_pid, int) and original_pid > 0 and pid_is_alive(original_pid),
                "terminal host process is not alive",
            )

            # Two renderer attachments constrain a shared PTY to the component-
            # wise minimum. Releasing the smaller viewer restores the larger.
            large, _ = attach_surface(
                socket_path,
                timeout,
                surface,
                100,
                40,
                cmux_revision,
                ghostty_revision,
            )
            clients.append(large)
            wait_for_vt_size(control, surface, (100, 40), timeout)
            small, _ = attach_surface(
                socket_path,
                timeout,
                surface,
                70,
                20,
                cmux_revision,
                ghostty_revision,
            )
            clients.append(small)
            wait_for_vt_size(control, surface, (70, 20), timeout)
            small.request("release-surface-size", surface=surface)
            wait_for_vt_size(control, surface, (100, 40), timeout)
            small.close()
            clients.remove(small)
            large.close()
            clients.remove(large)

            detached_marker = "__CMUX_DETACHED_FRONTENDS__"
            send_marker(control, surface, detached_marker)
            wait_for_screen(control, surface, detached_marker, timeout)

            # SIGKILL cannot run daemon cleanup. The independent terminal host
            # and its PTY/parser/output must survive and be adopted by a new
            # generation of the same registry.
            control.close()
            clients.remove(control)
            stop_daemon(daemon, timeout, kill=True)
            daemon = None
            require(pid_is_alive(original_pid), "terminal host died with SIGKILLed daemon")
            require(Path(str(original_endpoint)).exists(), "terminal host endpoint disappeared")

            daemon = start_daemon(binary, session, socket_path, state_path, timeout)
            control = JsonLineClient(socket_path, timeout)
            clients.append(control)
            second_identity = validate_identity(control, cmux_revision, ghostty_revision)
            require(
                second_identity["registry_id"] == first_identity["registry_id"],
                "daemon restart changed registry_id",
            )
            require(
                second_identity["generation"] != first_identity["generation"],
                "daemon restart reused its generation",
            )
            control.request("set-client-info", name="cmux-browser-smoke-restarted", kind="browser")
            control.request("subscribe")

            restarted_workspaces = list_workspaces(control)
            recovered_empty = workspace_with_key(restarted_workspaces, workspace_b)
            require(recovered_empty is not None, "empty GUI workspace did not survive restart")
            require(recovered_empty.get("screens") == [], "empty workspace gained phantom topology")
            surface, recovered = wait_for_resolved_surface(
                control, terminal_id, incarnation, timeout
            )
            require(
                recovered.get("workspace_key") == workspace_a,
                "recovered terminal changed workspace",
            )
            recovered_record = wait_for_host_record(host_root, terminal_id, timeout)
            for key, expected in (
                ("host_pid", original_pid),
                ("endpoint", original_endpoint),
                ("host_start_nonce", original_nonce),
                ("incarnation", incarnation),
            ):
                require(recovered_record.get(key) == expected, f"surviving host changed {key}")
            wait_for_screen(control, surface, before_marker, timeout)
            wait_for_screen(control, surface, detached_marker, timeout)
            after_marker = "__CMUX_AFTER_DAEMON_RESTART__"
            send_marker(control, surface, after_marker)
            wait_for_screen(control, surface, after_marker, timeout)

            # Move exactly once after losing the first response. The terminal
            # host, incarnation, and output continue; only canonical placement
            # changes.
            move_before = list_terminals(control)["terminal_revision"]
            move_fields = {
                "terminal_id": terminal_id,
                "terminal_incarnation": incarnation,
                "workspace_key": workspace_b,
                "origin": ORIGIN,
                "mutation_id": "terminal-move-survivor",
                "expected_generation": second_identity["generation"],
                "expected_terminal_revision": move_before,
            }
            lost_move = send_without_reading_response(
                socket_path, timeout, "move-terminal", **move_fields
            )
            try:
                moved_snapshot, _ = wait_for_terminal(
                    control,
                    terminal_id,
                    lambda item: item.get("workspace_key") == workspace_b,
                    "lost-response terminal move to commit",
                    timeout,
                )
            finally:
                lost_move.close()
            move_retry, _ = control.request("move-terminal", **move_fields)
            require(move_retry.get("replayed") is True, "terminal move retry was not replayed")
            require(
                move_retry.get("workspace_key") == workspace_b,
                "move retry changed destination",
            )
            require(
                moved_snapshot["terminal_revision"] == move_before + 1,
                "exactly-once move advanced terminal revision more than once",
            )
            moved_events, _ = control.request("terminal-events", after_revision=move_before)
            require(
                [item.get("kind") for item in moved_events.get("events", [])]
                == ["terminal-moved"],
                "exactly-once move did not produce one terminal-moved event",
            )
            moved_workspace_snapshot = list_workspaces(control)
            source = workspace_with_key(moved_workspace_snapshot, workspace_a)
            require(
                source is not None and source.get("screens") == [],
                "source workspace not empty",
            )
            moved_record = wait_until(
                lambda: (
                    record
                    if (record := read_host_record(host_root, terminal_id)) is not None
                    and record.get("workspace_key") == workspace_b
                    else None
                ),
                "terminal host workspace hint to follow canonical move",
                timeout,
            )
            require(moved_record.get("host_pid") == original_pid, "move replaced terminal host")
            surface, _ = wait_for_resolved_surface(control, terminal_id, incarnation, timeout)
            wait_for_screen(control, surface, after_marker, timeout)

            stable_snapshot = list_terminals(control)
            event_page = assert_terminal_events_contiguous(
                control,
                second_identity["registry_id"],
                second_identity["generation"],
            )
            require(
                event_page["terminal_revision"] == stable_snapshot["terminal_revision"],
                "list/events barrier revisions disagree",
            )
            suffix_after = max(0, event_page["terminal_revision"] - 3)
            assert_terminal_events_contiguous(
                control,
                second_identity["registry_id"],
                second_identity["generation"],
                suffix_after,
            )

            # Close exactly once while discarding the first response.
            close_before = list_terminals(control)["terminal_revision"]
            close_fields = {
                "terminal_id": terminal_id,
                "terminal_incarnation": incarnation,
                "origin": ORIGIN,
                "mutation_id": "terminal-close-survivor",
                "expected_generation": second_identity["generation"],
                "expected_terminal_revision": close_before,
            }
            lost_close = send_without_reading_response(
                socket_path, timeout, "close-terminal", **close_fields
            )
            try:
                closed_snapshot = wait_for_terminal_absence(control, terminal_id, timeout)
            finally:
                lost_close.close()
            close_retry, _ = control.request("close-terminal", **close_fields)
            require(close_retry.get("closed") is True, "terminal close retry was not successful")
            require(close_retry.get("already_closed") is False, "close replay lost original result")
            require(
                close_retry.get("terminal_revision") == close_before + 1
                and closed_snapshot["terminal_revision"] == close_before + 1,
                "exactly-once close advanced terminal revision more than once",
            )
            close_events, _ = control.request("terminal-events", after_revision=close_before)
            require(
                [item.get("kind") for item in close_events.get("events", [])]
                == ["terminal-closed"],
                "exactly-once close did not produce one terminal-closed event",
            )
            tombstone, _ = control.request("resolve-terminal", terminal_id=terminal_id)
            require(
                tombstone.get("surface") is None and tombstone.get("lifecycle") == "tombstoned",
                "closed terminal was not retained as a canonical tombstone",
            )
            wait_for_host_record_absence(host_root, terminal_id, timeout)

            # A workspace close must tombstone all child terminals in the same
            # SQLite transaction and then terminate both independent hosts.
            create_empty_workspace(
                control,
                second_identity,
                workspace_batch,
                "Batch close",
                "workspace-create-batch",
            )
            batch_ids: list[str] = []
            for index in range(2):
                batch_id = uuid.uuid4().hex
                batch_ids.append(batch_id)
                terminal_before = list_terminals(control)["terminal_revision"]
                created, _ = control.request(
                    "create-terminal",
                    key=workspace_batch,
                    argv=[cat],
                    terminal_id=batch_id,
                    origin=ORIGIN,
                    mutation_id=f"terminal-create-batch-{index}",
                    expected_generation=second_identity["generation"],
                    expected_terminal_revision=terminal_before,
                    cols=80,
                    rows=24,
                )
                require(created.get("replayed") is False, "batch terminal unexpectedly replayed")
                wait_for_terminal(
                    control,
                    batch_id,
                    lambda item: item.get("lifecycle") == "running",
                    f"batch terminal {batch_id} to run",
                    timeout,
                )
            batch_records = [wait_for_host_record(host_root, item, timeout) for item in batch_ids]
            require(
                len({record.get("host_pid") for record in batch_records}) == 2,
                "two terminals did not get two host processes",
            )
            require(
                len({record.get("endpoint") for record in batch_records}) == 2,
                "two terminals shared a host endpoint",
            )
            require(
                {path.stem for path in host_root.glob("*.json")} == set(batch_ids),
                "unexpected terminal host records before batch close",
            )

            batch_terminal_before = list_terminals(control)["terminal_revision"]
            batch_workspace_before = list_workspaces(control)["workspace_revision"]
            batch_close_fields = {
                "key": workspace_batch,
                "origin": ORIGIN,
                "mutation_id": "workspace-close-batch",
                "expected_generation": second_identity["generation"],
                "expected_revision": batch_workspace_before,
            }
            batch_close, _ = control.request("close-workspace", **batch_close_fields)
            require(batch_close.get("replayed") is False, "batch close unexpectedly replayed")
            require(
                workspace_with_key(list_workspaces(control), workspace_batch) is None,
                "batch-closed workspace remained in canonical snapshot",
            )
            after_batch = list_terminals(control)
            require(
                not any(terminal_with_id(after_batch, item) for item in batch_ids),
                "batch close left a child terminal live",
            )
            batch_events, _ = control.request(
                "terminal-events", after_revision=batch_terminal_before
            )
            events = batch_events.get("events", [])
            require(
                [item.get("terminal_revision") for item in events]
                == [batch_terminal_before + 1, batch_terminal_before + 2],
                "batch terminal tombstones were not one contiguous transaction",
            )
            require(
                {item.get("terminal_id") for item in events} == set(batch_ids)
                and all(item.get("kind") == "terminal-closed" for item in events)
                and all(
                    item.get("result", {}).get("reason") == "workspace-closed"
                    for item in events
                ),
                "batch close emitted the wrong terminal tombstones",
            )
            for batch_id in batch_ids:
                wait_for_host_record_absence(host_root, batch_id, timeout)

            # Lost close replies are replayed before stale generation/revision
            # guards and before resolving the now-tombstoned workspace.
            stale_batch_close = dict(batch_close_fields)
            stale_batch_close["expected_generation"] = first_identity["generation"]
            stale_batch_close["expected_revision"] = 0
            batch_retry, _ = control.request("close-workspace", **stale_batch_close)
            require(batch_retry.get("replayed") is True, "batch workspace close did not replay")

            final_terminals = list_terminals(control)
            final_events = assert_terminal_events_contiguous(
                control,
                second_identity["registry_id"],
                second_identity["generation"],
            )
            require(
                final_events["terminal_revision"] == final_terminals["terminal_revision"],
                "final list/events terminal revision mismatch",
            )
            event_count = len(final_events["events"])

            # Remove the two deliberately retained empty workspaces, leaving
            # no live registry topology or terminal host process behind.
            for index, key in enumerate((workspace_a, workspace_b)):
                workspace_snapshot = list_workspaces(control)
                require(workspace_with_key(workspace_snapshot, key) is not None, f"missing {key}")
                control.request(
                    "close-workspace",
                    key=key,
                    origin=ORIGIN,
                    mutation_id=f"workspace-cleanup-{index}",
                    expected_generation=second_identity["generation"],
                    expected_revision=workspace_snapshot["workspace_revision"],
                )
            require(list_workspaces(control)["workspaces"] == [], "workspace cleanup incomplete")
            require(list_terminals(control)["terminals"] == [], "terminal cleanup incomplete")
            require(list(host_root.glob("*.json")) == [], "terminal host cleanup incomplete")

            print(
                json.dumps(
                    {
                        "batch_close_terminals": 2,
                        "build_commit": second_identity["build_commit"],
                        "daemon_generations": 2,
                        "ghostty_commit": second_identity["ghostty_commit"],
                        "protocol": second_identity["protocol"],
                        "registry_id_stable": True,
                        "resize_arbitration": "70x20->100x40",
                        "terminal_events": event_count,
                        "result": "ok",
                    },
                    sort_keys=True,
                )
            )
        finally:
            for client in reversed(clients):
                try:
                    client.close()
                except OSError:
                    pass
            if daemon is not None:
                try:
                    stop_daemon(daemon, timeout)
                except (OSError, subprocess.SubprocessError):
                    pass
            cleanup_recorded_hosts(host_root, min(timeout, 2.0))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--binary",
        type=Path,
        default=os.environ.get("CMUX_TUI_BINARY"),
        required="CMUX_TUI_BINARY" not in os.environ,
        help="path to the exact revision-stamped cmux-tui helper",
    )
    parser.add_argument("--timeout", type=float, default=20.0)
    args = parser.parse_args()
    if args.timeout <= 0:
        parser.error("--timeout must be positive")
    try:
        run_smoke(args.binary.resolve(), args.timeout)
    except (OSError, SmokeFailure, subprocess.SubprocessError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
