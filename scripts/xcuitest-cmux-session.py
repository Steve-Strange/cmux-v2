#!/usr/bin/env python3
"""Own, inspect, and tear down cmux-tui sessions created by XCUITest.

The browser deliberately leaves cmux-tui workspaces and per-terminal hosts
alive when a frontend disconnects. UI tests therefore need an explicit,
session-scoped teardown: close the canonical topology, wait for every durable
host to exit, and only then request the daemon's pid/generation-fenced exit.

Every destructive operation is rooted in an unguessable ownership manifest
under /tmp. The helper never scans or signals unrelated cmux or user processes.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import hmac
import json
import os
from pathlib import Path
import re
import secrets
import shutil
import signal
import socket
import subprocess
import sys
import time
from typing import Any, Iterable


class SessionError(RuntimeError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise SessionError(message)


def stable_token(value: str) -> str:
    result = 0xCBF29CE484222325
    for byte in value.encode():
        result ^= byte
        result = (result * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return f"{result:016x}"


def positive_pid(value: Any, description: str) -> int:
    require(isinstance(value, int) and not isinstance(value, bool), f"invalid {description}")
    require(1 < value <= 2**31 - 1, f"invalid {description}")
    return value


def positive_id(value: Any, description: str) -> int:
    require(isinstance(value, int) and not isinstance(value, bool), f"invalid {description}")
    require(0 < value <= 2**63 - 1, f"invalid {description}")
    return value


@dataclass(frozen=True)
class OwnedSession:
    manifest: Path
    root: Path
    socket_path: Path
    state_dir: Path
    session: str
    binary: Path
    run_token: str

    @staticmethod
    def load(path: Path) -> "OwnedSession":
        manifest = path.resolve(strict=True)
        try:
            value = json.loads(manifest.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as error:
            raise SessionError(f"invalid XCUITest session manifest {manifest}: {error}") from error
        require(isinstance(value, dict), "XCUITest session manifest is not an object")

        def text_field(name: str) -> str:
            field = value.get(name)
            require(isinstance(field, str) and field, f"manifest omitted {name}")
            return field

        run_token = text_field("run_token")
        require(re.fullmatch(r"[a-f0-9]{24,64}", run_token) is not None, "bad run token")
        session = text_field("session")
        require(
            re.fullmatch(r"cmux-xcui-[a-f0-9]{8}-[a-f0-9]{12}", session) is not None,
            "bad XCUITest session name",
        )
        require(session.startswith(f"cmux-xcui-{run_token[:8]}-"), "session/run token mismatch")

        root = Path(text_field("root")).resolve()
        socket_path = Path(text_field("socket")).resolve()
        state_dir = Path(text_field("state_dir")).resolve()
        binary = Path(text_field("binary")).resolve()
        tmp = Path("/tmp").resolve()
        require(root.parent == tmp, "XCUITest session root must be a direct /tmp child")
        require(
            root.name.startswith(f"cmux-xcui-session-{run_token[:12]}-"),
            "session root/run token mismatch",
        )
        require(manifest == root / "ownership.json", "manifest is outside its owned root")
        require(socket_path == root / "control.sock", "socket is outside its owned root")
        require(state_dir == root / "state", "state directory is outside its owned root")
        require(binary.is_absolute(), "cmux-tui binary is not absolute")
        return OwnedSession(
            manifest=manifest,
            root=root,
            socket_path=socket_path,
            state_dir=state_dir,
            session=session,
            binary=binary,
            run_token=run_token,
        )

    @property
    def host_root(self) -> Path:
        return self.state_dir / f"terminal-hosts-{stable_token(self.session)}"


def prepare_owned_session(run_token: str, binary: Path, test_name: str) -> OwnedSession:
    run_token = run_token.lower()
    require(re.fullmatch(r"[a-f0-9]{24,64}", run_token) is not None, "bad run token")
    require(re.fullmatch(r"[a-z0-9-]{1,32}", test_name) is not None, "bad test name")
    binary = binary.resolve(strict=True)
    require(binary.is_file() and os.access(binary, os.X_OK), "cmux-tui binary is not executable")
    unique = stable_token(f"{run_token}:{test_name}")[:12]
    root = Path("/tmp") / f"cmux-xcui-session-{run_token[:12]}-{unique}"
    require(not root.exists(), f"owned XCUITest session root already exists: {root}")
    root.mkdir(mode=0o700)
    manifest = root / "ownership.json"
    value = {
        "binary": str(binary),
        "root": str(root),
        "run_token": run_token,
        "session": f"cmux-xcui-{run_token[:8]}-{unique}",
        "socket": str(root / "control.sock"),
        "state_dir": str(root / "state"),
        "test_name": test_name,
        "version": 1,
    }
    descriptor = os.open(
        manifest,
        os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, "O_CLOEXEC", 0),
        0o600,
    )
    try:
        payload = (json.dumps(value, sort_keys=True, separators=(",", ":")) + "\n").encode()
        with os.fdopen(descriptor, "wb", closefd=False) as stream:
            stream.write(payload)
            stream.flush()
            os.fsync(stream.fileno())
    finally:
        os.close(descriptor)
    return OwnedSession.load(manifest)


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

    def request(
        self,
        command: str,
        *,
        allow_disconnect: bool = False,
        **fields: Any,
    ) -> dict[str, Any]:
        request_id = self._next_id
        self._next_id += 1
        payload = {"id": request_id, "cmd": command, **fields}
        self._socket.sendall(json.dumps(payload, separators=(",", ":")).encode() + b"\n")
        while True:
            try:
                line = self._reader.readline()
            except (TimeoutError, OSError):
                if allow_disconnect:
                    return {}
                raise
            if not line:
                if allow_disconnect:
                    return {}
                raise SessionError(f"cmux-tui disconnected during {command}")
            try:
                value = json.loads(line)
            except json.JSONDecodeError as error:
                raise SessionError(f"invalid cmux-tui response: {line!r}") from error
            require(isinstance(value, dict), "cmux-tui response is not an object")
            if "event" in value or value.get("id") != request_id:
                continue
            if value.get("ok") is not True:
                raise SessionError(f"{command} failed: {value.get('error', 'unknown error')}")
            data = value.get("data", {})
            require(isinstance(data, dict), f"{command} returned non-object data")
            return data


def topology_ids(snapshot: dict[str, Any]) -> tuple[list[int], list[int]]:
    workspaces = snapshot.get("workspaces")
    require(isinstance(workspaces, list), "list-workspaces omitted workspaces")
    workspace_ids: list[int] = []
    surface_ids: list[int] = []
    for workspace in workspaces:
        require(isinstance(workspace, dict), "workspace is not an object")
        workspace_ids.append(positive_id(workspace.get("id"), "workspace id"))
        screens = workspace.get("screens", [])
        require(isinstance(screens, list), "workspace screens are not an array")
        for screen in screens:
            require(isinstance(screen, dict), "screen is not an object")
            panes = screen.get("panes", [])
            require(isinstance(panes, list), "screen panes are not an array")
            for pane in panes:
                require(isinstance(pane, dict), "pane is not an object")
                tabs = pane.get("tabs", [])
                require(isinstance(tabs, list), "pane tabs are not an array")
                for tab in tabs:
                    require(isinstance(tab, dict), "tab is not an object")
                    surface_ids.append(positive_id(tab.get("surface"), "surface id"))
    return sorted(set(workspace_ids)), sorted(set(surface_ids))


def close_topology(client: JsonLineClient) -> None:
    snapshot = client.request("list-workspaces")
    _, surfaces = topology_ids(snapshot)
    for surface in surfaces:
        client.request("close-surface", surface=surface)

    # Closing a last surface may collapse panes/screens, but canonical empty
    # workspaces intentionally remain. Refresh before closing those identities.
    snapshot = client.request("list-workspaces")
    workspaces, remaining_surfaces = topology_ids(snapshot)
    require(not remaining_surfaces, "surface cleanup left live tabs")
    for workspace in workspaces:
        client.request("close-workspace", workspace=workspace)

    final_workspaces = client.request("list-workspaces")
    final_ids = topology_ids(final_workspaces)
    require(final_ids == ([], []), "workspace cleanup left live topology")
    terminals = client.request("list-terminals").get("terminals")
    require(terminals == [], "workspace cleanup left live terminal registry rows")


@dataclass(frozen=True)
class HostRecord:
    pid: int
    endpoint: Path
    path: Path


def host_records(root: Path) -> list[HostRecord]:
    records: list[HostRecord] = []
    if not root.exists():
        return records
    require(root.is_dir(), f"terminal host root is not a directory: {root}")
    for path in sorted(root.glob("*.json")):
        try:
            value = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as error:
            raise SessionError(f"invalid terminal host record {path}: {error}") from error
        require(isinstance(value, dict), f"terminal host record is not an object: {path}")
        endpoint = value.get("endpoint")
        require(isinstance(endpoint, str) and endpoint, f"host record omitted endpoint: {path}")
        records.append(
            HostRecord(
                pid=positive_pid(value.get("host_pid"), f"host pid in {path.name}"),
                endpoint=Path(endpoint),
                path=path,
            )
        )
    return records


def pid_exists(pid: int) -> bool:
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    return True


def wait_until(predicate: Any, description: str, timeout: float) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(0.03)
    if predicate():
        return
    raise SessionError(f"timed out waiting for {description}")


def connect_owned_session(owned: OwnedSession, timeout: float) -> JsonLineClient:
    deadline = time.monotonic() + timeout
    last_error: OSError | None = None
    while time.monotonic() < deadline:
        try:
            client = JsonLineClient(owned.socket_path, min(timeout, 1.0))
            identity = client.request("identify")
            require(identity.get("session") == owned.session, "socket belongs to another session")
            return client
        except OSError as error:
            last_error = error
            time.sleep(0.03)
    suffix = f": {last_error}" if last_error is not None else ""
    raise SessionError(f"could not connect to owned cmux-tui socket{suffix}")


def restart_owned_session(owned: OwnedSession, timeout: float) -> None:
    require(owned.binary.is_file() and os.access(owned.binary, os.X_OK), "cmux-tui binary unavailable")
    owned.root.mkdir(mode=0o700, parents=False, exist_ok=True)
    try:
        owned.socket_path.unlink()
    except FileNotFoundError:
        pass
    environment = os.environ.copy()
    environment["CMUX_TUI_STATE_DIR"] = str(owned.state_dir)
    subprocess.Popen(
        [
            str(owned.binary),
            "--headless",
            "--session",
            owned.session,
            "--socket",
            str(owned.socket_path),
            "--state",
            str(owned.state_dir),
        ],
        stdin=subprocess.DEVNULL,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        env=environment,
        start_new_session=True,
    )
    client = connect_owned_session(owned, timeout)
    client.close()


def cleanup_owned_session(owned: OwnedSession, timeout: float) -> None:
    if not owned.root.exists():
        return
    initial_records = host_records(owned.host_root)
    if not owned.socket_path.exists() and not owned.state_dir.exists() and not initial_records:
        shutil.rmtree(owned.root)
        return
    if not owned.socket_path.exists():
        restart_owned_session(owned, timeout)

    client = connect_owned_session(owned, timeout)
    identity = client.request("identify")
    daemon_pid = positive_pid(identity.get("pid"), "daemon pid")
    generation = identity.get("generation")
    require(isinstance(generation, str) and generation, "identify omitted generation")
    try:
        close_topology(client)
        records = initial_records + [item for item in host_records(owned.host_root) if item not in initial_records]
        wait_until(lambda: not host_records(owned.host_root), "terminal host records to disappear", timeout)
        for record in records:
            wait_until(lambda pid=record.pid: not pid_exists(pid), f"terminal host {record.pid} to exit", timeout)
            require(not record.endpoint.exists(), f"terminal host endpoint survived: {record.endpoint}")
        client.request(
            "shutdown-daemon",
            allow_disconnect=True,
            pid=daemon_pid,
            generation=generation,
        )
    finally:
        client.close()

    wait_until(lambda: not pid_exists(daemon_pid), f"cmux-tui daemon {daemon_pid} to exit", timeout)
    wait_until(lambda: not owned.socket_path.exists(), "cmux-tui socket to disappear", timeout)
    require(not host_records(owned.host_root), "terminal host records reappeared after shutdown")
    shutil.rmtree(owned.root)


@dataclass(frozen=True)
class BrokerConfig:
    path: Path
    host: str
    port: int
    secret: str
    run_token: str

    @staticmethod
    def load(path: Path) -> "BrokerConfig":
        config_path = path.resolve(strict=True)
        try:
            value = json.loads(config_path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as error:
            raise SessionError(f"invalid XCUITest broker config {config_path}: {error}") from error
        require(isinstance(value, dict), "XCUITest broker config is not an object")
        run_token = value.get("run_token")
        require(
            isinstance(run_token, str)
            and re.fullmatch(r"[a-f0-9]{24,64}", run_token) is not None,
            "broker config has a bad run token",
        )
        tmp = Path("/tmp").resolve()
        require(config_path.parent == tmp, "broker config must be a direct /tmp child")
        require(
            config_path.name == f"cmux-xcui-broker-{run_token}.json",
            "broker config path/run token mismatch",
        )
        host = value.get("host")
        port = value.get("port")
        secret = value.get("secret")
        require(host == "127.0.0.1", "broker must use IPv4 loopback")
        require(isinstance(port, int) and 0 < port <= 65535, "broker has a bad port")
        require(
            isinstance(secret, str) and re.fullmatch(r"[a-f0-9]{64}", secret) is not None,
            "broker has a bad secret",
        )
        return BrokerConfig(config_path, host, port, secret, run_token)


def write_exclusive_json(path: Path, value: dict[str, Any]) -> tuple[int, int]:
    descriptor = os.open(
        path,
        os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, "O_CLOEXEC", 0),
        0o600,
    )
    try:
        payload = (json.dumps(value, sort_keys=True, separators=(",", ":")) + "\n").encode()
        with os.fdopen(descriptor, "wb", closefd=False) as stream:
            stream.write(payload)
            stream.flush()
            os.fsync(stream.fileno())
        info = os.fstat(descriptor)
        return info.st_dev, info.st_ino
    finally:
        os.close(descriptor)


def remove_same_inode(path: Path, identity: tuple[int, int]) -> None:
    try:
        info = path.stat()
    except FileNotFoundError:
        return
    if (info.st_dev, info.st_ino) == identity:
        path.unlink()


def broker_owned_session(
    request: dict[str, Any],
    sessions: dict[str, OwnedSession],
) -> OwnedSession:
    manifest = request.get("manifest")
    require(isinstance(manifest, str) and manifest, "broker request omitted manifest")
    key = str(Path(manifest).resolve())
    owned = sessions.get(key)
    require(owned is not None, "broker request named an unowned manifest")
    return owned


def dispatch_broker_request(
    request: dict[str, Any],
    sessions: dict[str, OwnedSession],
) -> tuple[dict[str, Any], bool]:
    operation = request.get("operation")
    require(isinstance(operation, str), "broker request omitted operation")
    if operation == "ping":
        return {"pong": True}, False
    if operation == "shutdown":
        return {"stopping": True}, True

    timeout_value = request.get("timeout", 8.0)
    require(
        isinstance(timeout_value, (int, float))
        and not isinstance(timeout_value, bool)
        and 0.1 <= float(timeout_value) <= 30.0,
        "broker request has a bad timeout",
    )
    timeout = float(timeout_value)
    if operation == "cleanup-all":
        cleaned: list[str] = []
        for owned in sessions.values():
            cleanup_owned_session(owned, timeout)
            cleaned.append(owned.session)
        return {"cleaned_sessions": cleaned}, False

    owned = broker_owned_session(request, sessions)
    if operation == "cleanup":
        cleanup_owned_session(owned, timeout)
        return {"cleaned": True, "session": owned.session}, False
    if operation == "owned-processes":
        command = request.get("process_command")
        require(
            isinstance(command, str) and 0 < len(command) <= 4096,
            "broker request has a bad process command",
        )
        return {"pids": owned_processes(owned, command, timeout)}, False
    if operation == "owns-pid":
        pid = positive_pid(request.get("pid"), "broker requested pid")
        return {"owned": owns_pid(owned, pid, timeout)}, False
    if operation == "list-clients":
        return {"clients": packaged_list_clients(owned, timeout)}, False
    raise SessionError(f"unknown broker operation {operation}")


def serve_broker(
    run_token: str,
    ready_file: Path,
    manifests: list[Path],
) -> None:
    require(re.fullmatch(r"[a-f0-9]{24,64}", run_token) is not None, "bad run token")
    ready_file = ready_file.resolve()
    tmp = Path("/tmp").resolve()
    require(ready_file.parent == tmp, "broker ready file must be a direct /tmp child")
    require(
        ready_file.name == f"cmux-xcui-broker-{run_token}.json",
        "broker ready file/run token mismatch",
    )
    require(not ready_file.exists(), "broker ready file already exists")
    require(manifests, "broker has no owned manifests")

    sessions: dict[str, OwnedSession] = {}
    for manifest in manifests:
        owned = OwnedSession.load(manifest)
        require(owned.run_token == run_token, "broker manifest belongs to another run")
        sessions[str(owned.manifest)] = owned
    require(len(sessions) == len(manifests), "broker manifests contain duplicates")

    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(("127.0.0.1", 0))
    listener.listen(8)
    listener.settimeout(0.2)
    secret = secrets.token_hex(32)
    identity = write_exclusive_json(
        ready_file,
        {
            "host": "127.0.0.1",
            "pid": os.getpid(),
            "port": listener.getsockname()[1],
            "run_token": run_token,
            "secret": secret,
            "version": 1,
        },
    )
    stopping = False

    def request_stop(_signum: int, _frame: Any) -> None:
        nonlocal stopping
        stopping = True

    old_term = signal.signal(signal.SIGTERM, request_stop)
    old_int = signal.signal(signal.SIGINT, request_stop)
    try:
        while not stopping:
            try:
                connection, _ = listener.accept()
            except TimeoutError:
                continue
            with connection:
                connection.settimeout(30.0)
                response: dict[str, Any]
                should_stop = False
                try:
                    reader = connection.makefile("rb")
                    line = reader.readline(65537)
                    require(line.endswith(b"\n") and len(line) <= 65536, "bad broker request frame")
                    request = json.loads(line)
                    require(isinstance(request, dict), "broker request is not an object")
                    supplied_secret = request.get("secret")
                    require(
                        isinstance(supplied_secret, str)
                        and hmac.compare_digest(supplied_secret, secret),
                        "broker authentication failed",
                    )
                    data, should_stop = dispatch_broker_request(request, sessions)
                    response = {"ok": True, "data": data}
                except (OSError, SessionError, ValueError, json.JSONDecodeError) as error:
                    response = {"ok": False, "error": str(error)}
                try:
                    connection.sendall(
                        json.dumps(response, separators=(",", ":")).encode() + b"\n"
                    )
                except OSError:
                    # A timed-out XCTest request must not take down the broker;
                    # the shell fallback still needs it for later diagnostics.
                    pass
                if should_stop:
                    stopping = True
    finally:
        signal.signal(signal.SIGTERM, old_term)
        signal.signal(signal.SIGINT, old_int)
        listener.close()
        remove_same_inode(ready_file, identity)


def broker_request(
    config_path: Path,
    operation: str,
    manifest: Path | None,
    timeout: float,
    process_command: str | None,
    pid: int | None,
) -> dict[str, Any]:
    config = BrokerConfig.load(config_path)
    request: dict[str, Any] = {
        "operation": operation,
        "secret": config.secret,
        "timeout": timeout,
    }
    if manifest is not None:
        request["manifest"] = str(manifest.resolve())
    if process_command is not None:
        request["process_command"] = process_command
    if pid is not None:
        request["pid"] = pid
    client = socket.create_connection((config.host, config.port), timeout=min(timeout, 5.0))
    try:
        client.settimeout(timeout + 1.0)
        client.sendall(json.dumps(request, separators=(",", ":")).encode() + b"\n")
        reader = client.makefile("rb")
        line = reader.readline(65537)
    finally:
        client.close()
    require(line.endswith(b"\n") and len(line) <= 65536, "bad broker response frame")
    response = json.loads(line)
    require(isinstance(response, dict), "broker response is not an object")
    require(response.get("ok") is True, f"broker request failed: {response.get('error', 'unknown error')}")
    data = response.get("data")
    require(isinstance(data, dict), "broker response omitted data")
    return data


def process_table(output: str) -> dict[int, tuple[int, str]]:
    table: dict[int, tuple[int, str]] = {}
    pattern = re.compile(r"^\s*(\d+)\s+(\d+)\s+(.*)$")
    for line in output.splitlines():
        match = pattern.match(line)
        if match is None:
            continue
        pid = positive_id(int(match.group(1)), "ps pid")
        ppid = int(match.group(2))
        table[pid] = (ppid, match.group(3).strip())
    return table


def descendant_pids(table: dict[int, tuple[int, str]], roots: Iterable[int]) -> set[int]:
    descendants = set(roots)
    changed = True
    while changed:
        changed = False
        for pid, (parent, _) in table.items():
            if parent in descendants and pid not in descendants:
                descendants.add(pid)
                changed = True
    return descendants


def session_process_roots(client: JsonLineClient) -> set[int]:
    _, surfaces = topology_ids(client.request("list-workspaces"))
    roots: set[int] = set()
    for surface in surfaces:
        info = client.request("process-info", surface=surface)
        pid = info.get("pid")
        if pid is not None:
            roots.add(positive_pid(pid, f"surface {surface} process pid"))
    return roots


def owned_processes(owned: OwnedSession, command: str, timeout: float) -> list[int]:
    client = connect_owned_session(owned, timeout)
    try:
        roots = session_process_roots(client)
    finally:
        client.close()
    result = subprocess.run(
        ["/bin/ps", "-axo", "pid=,ppid=,command="],
        check=True,
        capture_output=True,
        text=True,
    )
    table = process_table(result.stdout)
    owned_pids = descendant_pids(table, roots)
    return sorted(pid for pid in owned_pids if table.get(pid, (0, ""))[1] == command)


def owns_pid(owned: OwnedSession, pid: int, timeout: float) -> bool:
    client = connect_owned_session(owned, timeout)
    try:
        roots = session_process_roots(client)
    finally:
        client.close()
    result = subprocess.run(
        ["/bin/ps", "-axo", "pid=,ppid=,command="],
        check=True,
        capture_output=True,
        text=True,
    )
    return pid in descendant_pids(process_table(result.stdout), roots)


def parse_client_list(payload: bytes) -> list[dict[str, Any]]:
    try:
        value = json.loads(payload)
    except json.JSONDecodeError as error:
        raise SessionError("packaged cmux-tui returned invalid list-clients JSON") from error
    require(isinstance(value, list), "packaged cmux-tui list-clients result is not an array")
    require(all(isinstance(client, dict) for client in value), "list-clients contains a non-object")
    return value


def packaged_list_clients(owned: OwnedSession, timeout: float) -> list[dict[str, Any]]:
    require(
        owned.binary.is_file() and os.access(owned.binary, os.X_OK),
        "cmux-tui binary unavailable",
    )
    try:
        result = subprocess.run(
            [
                str(owned.binary),
                "--socket",
                str(owned.socket_path),
                "--json",
                "list-clients",
            ],
            check=False,
            capture_output=True,
            timeout=timeout,
        )
    except subprocess.TimeoutExpired as error:
        raise SessionError("packaged cmux-tui list-clients timed out") from error
    require(
        result.returncode == 0,
        "packaged cmux-tui list-clients failed: "
        + result.stderr.decode("utf-8", errors="replace").strip(),
    )
    return parse_client_list(result.stdout)


def self_test() -> None:
    require(stable_token("cmux-xcui-test") == "0ff3a5c00c5c9307", "stable token drifted")
    snapshot = {
        "workspaces": [
            {
                "id": 1,
                "screens": [
                    {"panes": [{"tabs": [{"surface": 7}, {"surface": 8}]}]},
                ],
            },
            {"id": 2, "screens": []},
        ]
    }
    require(topology_ids(snapshot) == ([1, 2], [7, 8]), "topology flattening failed")

    table = process_table(" 10 1 /bin/zsh\n 11 10 sleep 42421\n 12 1 sleep 42421\n 13 11 child\n")
    require(descendant_pids(table, {10}) == {10, 11, 13}, "process ancestry escaped its session")
    require(
        sorted(pid for pid in descendant_pids(table, {10}) if table[pid][1] == "sleep 42421") == [11],
        "session-owned process match included an unrelated command",
    )
    require(
        parse_client_list(b'[{"kind":"tui","size_participating":true}]')
        == [{"kind": "tui", "size_participating": True}],
        "packaged list-clients parsing failed",
    )
    try:
        parse_client_list(b'{"clients":[]}')
    except SessionError:
        pass
    else:
        raise SessionError("packaged list-clients parser accepted a non-array")

    run_token = "0123456789abcdef0123456789abcdef"
    expected_root = Path("/tmp") / (
        f"cmux-xcui-session-{run_token[:12]}-"
        + stable_token(f"{run_token}:self-test")[:12]
    )
    if expected_root.exists():
        shutil.rmtree(expected_root)
    prepared = prepare_owned_session(run_token, Path("/bin/echo"), "self-test")
    root = prepared.root
    manifest = prepared.manifest
    try:
        owned = OwnedSession.load(manifest)
        require(owned.root == root.resolve(), "owned root changed during validation")
        bad = json.loads(manifest.read_text(encoding="utf-8"))
        bad["state_dir"] = "/tmp/not-owned"
        manifest.write_text(json.dumps(bad), encoding="utf-8")
        try:
            OwnedSession.load(manifest)
        except SessionError:
            pass
        else:
            raise SessionError("manifest accepted state outside its owned root")
    finally:
        shutil.rmtree(root)

    broker_token = secrets.token_hex(16)
    broker_owned = prepare_owned_session(broker_token, Path("/bin/echo"), "broker-self-test")
    broker_config = Path("/tmp") / f"cmux-xcui-broker-{broker_token}.json"
    broker = subprocess.Popen(
        [
            sys.executable,
            str(Path(__file__).resolve()),
            "serve-broker",
            "--run-token",
            broker_token,
            "--ready-file",
            str(broker_config),
            "--manifest",
            str(broker_owned.manifest),
        ],
        stdin=subprocess.DEVNULL,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    try:
        wait_until(
            lambda: broker_config.exists() or broker.poll() is not None,
            "self-test broker readiness",
            3.0,
        )
        require(broker.poll() is None and broker_config.exists(), "self-test broker exited early")
        require(
            broker_config.stat().st_mode & 0o777 == 0o600,
            "broker config permissions are not private",
        )
        require(
            broker_request(broker_config, "ping", None, 2.0, None, None) == {"pong": True},
            "broker ping failed",
        )
        try:
            broker_request(
                broker_config,
                "owned-processes",
                Path("/tmp/not-an-owned-manifest"),
                2.0,
                "sleep 42421",
                None,
            )
        except SessionError:
            pass
        else:
            raise SessionError("broker accepted an unowned manifest")
        result = broker_request(
            broker_config,
            "cleanup-all",
            None,
            2.0,
            None,
            None,
        )
        require(
            result.get("cleaned_sessions") == [broker_owned.session],
            "broker cleanup-all failed",
        )
        require(not broker_owned.root.exists(), "broker cleanup left its owned root")
        broker_request(broker_config, "shutdown", None, 2.0, None, None)
        require(broker.wait(timeout=3.0) == 0, "self-test broker did not exit cleanly")
        require(not broker_config.exists(), "self-test broker left its config")
    finally:
        if broker.poll() is None:
            broker.terminate()
            try:
                broker.wait(timeout=3.0)
            except subprocess.TimeoutExpired:
                broker.kill()
                broker.wait()
        if broker_owned.root.exists():
            shutil.rmtree(broker_owned.root)
    print("xcuitest cmux session helper self-test: PASS")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    subparsers = parser.add_subparsers(dest="command", required=True)
    for name in ("cleanup", "owned-processes", "owns-pid"):
        command = subparsers.add_parser(name)
        command.add_argument("--manifest", type=Path, required=True)
        command.add_argument("--timeout", type=float, default=8.0)
    subparsers.choices["owned-processes"].add_argument("--process-command", required=True)
    subparsers.choices["owns-pid"].add_argument("--pid", type=int, required=True)
    prepare = subparsers.add_parser("prepare")
    prepare.add_argument("--run-token", required=True)
    prepare.add_argument("--binary", type=Path, required=True)
    prepare.add_argument("--test-name", required=True)
    serve = subparsers.add_parser("serve-broker")
    serve.add_argument("--run-token", required=True)
    serve.add_argument("--ready-file", type=Path, required=True)
    serve.add_argument("--manifest", type=Path, action="append", required=True)
    request = subparsers.add_parser("broker-request")
    request.add_argument("--broker-config", type=Path, required=True)
    request.add_argument(
        "--operation",
        choices=(
            "ping",
            "cleanup",
            "cleanup-all",
            "owned-processes",
            "owns-pid",
            "list-clients",
            "shutdown",
        ),
        required=True,
    )
    request.add_argument("--manifest", type=Path)
    request.add_argument("--timeout", type=float, default=8.0)
    request.add_argument("--process-command")
    request.add_argument("--pid", type=int)
    subparsers.add_parser("--self-test")
    return parser.parse_args()


def main() -> int:
    # Keep compatibility with the other repository probes' simple self-test
    # invocation while retaining argparse subcommands for destructive actions.
    if sys.argv[1:] == ["--self-test"]:
        self_test()
        return 0
    args = parse_args()
    if args.command == "prepare":
        owned = prepare_owned_session(args.run_token, args.binary, args.test_name)
        print(str(owned.manifest))
        return 0
    if args.command == "serve-broker":
        serve_broker(args.run_token, args.ready_file, args.manifest)
        return 0
    if args.command == "broker-request":
        data = broker_request(
            args.broker_config,
            args.operation,
            args.manifest,
            args.timeout,
            args.process_command,
            args.pid,
        )
        print(json.dumps(data, separators=(",", ":")))
        return 0
    owned = OwnedSession.load(args.manifest)
    if args.command == "cleanup":
        cleanup_owned_session(owned, args.timeout)
        print(json.dumps({"cleaned": True, "session": owned.session}, separators=(",", ":")))
        return 0
    if args.command == "owned-processes":
        pids = owned_processes(owned, args.process_command, args.timeout)
        print(json.dumps({"pids": pids}, separators=(",", ":")))
        return 0
    if args.command == "owns-pid":
        pid = positive_pid(args.pid, "requested pid")
        print(json.dumps({"owned": owns_pid(owned, pid, args.timeout)}, separators=(",", ":")))
        return 0
    raise SessionError(f"unknown command {args.command}")


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, SessionError, subprocess.SubprocessError) as error:
        print(f"xcuitest cmux session error: {error}", file=sys.stderr)
        raise SystemExit(1)
