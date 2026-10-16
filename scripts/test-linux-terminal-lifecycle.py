#!/usr/bin/env python3
"""Exercise the real Linux browser in an isolated X server and backend session."""

import argparse
import base64
import importlib.util
import json
import math
import os
from pathlib import Path
import re
import select
import signal
import subprocess
import sys
import tempfile
import shlex
import urllib.request
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--browser", type=Path, required=True)
    parser.add_argument("--backend", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--hold-seconds", type=float, default=60)
    parser.add_argument("--cycles", type=int, default=10)
    parser.add_argument("--record-seconds", type=float, default=0,
                        help="Record the isolated Xvfb display during steady state")
    parser.add_argument("--ffmpeg", default="ffmpeg",
                        help="FFmpeg executable with X11 capture and libx264 support")
    parser.add_argument("--display", help="Use an existing X display instead of Xvfb")
    parser.add_argument("--toolkit", choices=("qt", "gtk", "native"), default="qt")
    parser.add_argument("--native-rendering", action="store_true",
                        help="Use the system's normal graphics configuration")
    parser.add_argument("--scale", type=float,
                        help="Force Chromium device scale for HiDPI regression")
    parser.add_argument("--input-checks", action="store_true",
                        help="Test clipboard, appearance settings, and same-session restart")
    parser.add_argument("--ime-check", action="store_true",
                        help="Test the current desktop's fcitx Sogou input in this test window")
    parser.add_argument("--ime-toggle-shift", action="store_true",
                        help="Sogou starts in English mode; toggle Shift before typing and restore it")
    args = parser.parse_args()
    if args.ime_check and (not args.display or not args.input_checks):
        parser.error("--ime-check requires --display and --input-checks")
    if args.cycles < 1 or args.hold_seconds < 0 or args.record_seconds < 0:
        parser.error("cycles must be positive and durations must be nonnegative")
    if args.display and args.record_seconds:
        parser.error("recording is limited to this test's isolated Xvfb display")
    if args.scale is not None and (not math.isfinite(args.scale) or args.scale < 1):
        parser.error("scale must be finite and at least 1")
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    support_path = Path(__file__).with_name("smoke-cmux-tui-backend.py")
    spec = importlib.util.spec_from_file_location("cmux_backend_smoke", support_path)
    support = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = support
    spec.loader.exec_module(support)
    env = os.environ.copy()
    ime_env = {**env, "DISPLAY": args.display} if args.ime_check else env
    if args.ime_check:
        # Fcitx 4 locates its private D-Bus address through XDG_CONFIG_HOME.
        # Resolve it before isolating the browser's config below.
        env["FCITX_DBUS_ADDRESS"] = subprocess.check_output(
            ["fcitx-remote", "-a"], env=ime_env,
            text=True, timeout=5).strip()
    for key in ("LD_LIBRARY_PATH", "LD_PRELOAD", "QT_PLUGIN_PATH",
                "QT_QPA_PLATFORM_PLUGIN_PATH", "QT_QPA_PLATFORM",
                "GHOSTTY_RESOURCES_DIR",
                "__EGL_VENDOR_LIBRARY_FILENAMES", "LIBGL_ALWAYS_SOFTWARE",
                "GALLIUM_DRIVER", "LP_NUM_THREADS"):
        env.pop(key, None)
    runtime = Path(tempfile.mkdtemp(prefix="cmux-test-", dir=env.get("XDG_RUNTIME_DIR", "/tmp")))
    socket_path = runtime / "control.sock"
    for name in ("state", "config", "cache"):
        (out / name).mkdir()
        env["XDG_" + name.upper() + "_HOME"] = str(out / name)
    config = out / "cmux.json"
    config.write_text(json.dumps({
        "app": {"sendAnonymousTelemetry": False},
        "keybindings": [
            {"key": "f8", "command": "tab.newTerminal", "args": {
                "command": "exec /bin/bash --noprofile --norc", "title": "Lifecycle test"}},
            {"key": "f9", "command": "tab.close"},
            {"key": "f10", "command": "window.close"}]}))
    env.update(CMUX_LAYOUT_CONFIG=str(out / "layout.json"),
               CMUX_CONFIG=str(config), CMUX_TELEMETRY_ENABLE="0",
               CMUX_TUI_SESSION=runtime.name, CMUX_TUI_SOCKET=str(socket_path),
               CMUX_TUI_BINARY=str(args.backend.resolve()), CMUX_TAG=runtime.name,
               HISTFILE="/dev/null")
    # Keep the default startup zsh from reading personal startup commands.
    zsh_config = out / "config/zsh"
    zsh_config.mkdir()
    # An empty startup directory triggers zsh-newuser-install and consumes
    # the first command as a wizard response instead of executing it.
    (zsh_config / ".zshrc").write_text("# Isolated lifecycle-test shell.\n")
    env["ZDOTDIR"] = str(zsh_config)
    if not args.native_rendering:
        env.update(__EGL_VENDOR_LIBRARY_FILENAMES="/usr/share/glvnd/egl_vendor.d/50_mesa.json",
                   LIBGL_ALWAYS_SOFTWARE="1", GALLIUM_DRIVER="llvmpipe", LP_NUM_THREADS="4")
    processes, files = [], []
    client, browser = None, None
    result = {"browser": str(args.browser.resolve()), "backend": str(args.backend.resolve()),
              "runtime": str(runtime), "passed": False, "operations": [],
              "toolkit": args.toolkit, "native_rendering": args.native_rendering,
              "external_display": args.display, "cycles": args.cycles,
              "hold_seconds": args.hold_seconds, "record_seconds": args.record_seconds}
    started = time.monotonic()
    original_clipboard = None
    original_selection = None
    original_ime_state = None
    clipboard_owners = []

    def launch(command, log, **kwargs):
        stream = (out / log).open("w")
        files.append(stream)
        process = subprocess.Popen(command, env=env, stdout=stream, stderr=subprocess.STDOUT,
                                   stdin=subprocess.DEVNULL, start_new_session=True, **kwargs)
        processes.append(process)
        return process

    def wait_for(check, description, timeout=10):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if browser is not None and browser.poll() is not None:
                raise RuntimeError(f"browser exited {browser.returncode} during {description}")
            value = check()
            if value:
                return value
            time.sleep(0.1)
        raise RuntimeError("timed out: " + description)

    def xdo(*command):
        return subprocess.check_output(["xdotool", *map(str, command)], env=env,
                                       text=True, stderr=subprocess.DEVNULL).strip()

    def close_window(window):
        if args.display:
            subprocess.run(["wmctrl", "-ic", hex(int(window))], env=env, check=True)
            return "window-manager EWMH close request"
        # Xvfb has no window manager. Send the standard graceful close protocol
        # directly; F10 is a platform menu key and depends on focus/toolkit.
        import ctypes as ct
        class ClientMessage(ct.Structure):
            _fields_ = [("type", ct.c_int), ("serial", ct.c_ulong),
                        ("send_event", ct.c_int), ("display", ct.c_void_p),
                        ("window", ct.c_ulong), ("message_type", ct.c_ulong),
                        ("format", ct.c_int), ("data", ct.c_long * 5)]
        class Event(ct.Union):
            _fields_ = [("message", ClientMessage), ("pad", ct.c_long * 24)]
        x11 = ct.CDLL("libX11.so.6")
        x11.XOpenDisplay.argtypes, x11.XOpenDisplay.restype = [ct.c_char_p], ct.c_void_p
        x11.XInternAtom.argtypes = [ct.c_void_p, ct.c_char_p, ct.c_int]
        x11.XInternAtom.restype = ct.c_ulong
        x11.XSendEvent.argtypes = [ct.c_void_p, ct.c_ulong, ct.c_int, ct.c_long, ct.POINTER(Event)]
        x11.XCloseDisplay.argtypes = [ct.c_void_p]
        display = x11.XOpenDisplay(env["DISPLAY"].encode())
        if not display:
            raise RuntimeError("cannot connect to test display for graceful close")
        try:
            event = Event()
            event.message = ClientMessage(33, 0, 1, display, int(window),
                x11.XInternAtom(display, b"WM_PROTOCOLS", 0), 32,
                (ct.c_long * 5)(x11.XInternAtom(display, b"WM_DELETE_WINDOW", 0), 0, 0, 0, 0))
            if not x11.XSendEvent(display, int(window), 0, 0, ct.byref(event)):
                raise RuntimeError("WM_DELETE_WINDOW could not be sent")
        finally:
            x11.XCloseDisplay(display)
        return "WM_DELETE_WINDOW (isolated Xvfb)"

    try:
        if args.display:
            env["DISPLAY"] = args.display
        else:
            reader, writer = os.pipe()
            try:
                launch(["Xvfb", "-displayfd", str(writer), "-screen", "0", "1280x900x24",
                        "-nolisten", "tcp"], "xvfb.log", pass_fds=(writer,))
                os.close(writer)
                writer = None
                if not select.select([reader], [], [], 10)[0]:
                    raise RuntimeError("Xvfb did not allocate a display")
                env["DISPLAY"] = ":" + os.read(reader, 40).decode().strip()
            finally:
                os.close(reader)
                if writer is not None:
                    os.close(writer)
        launch([str(args.backend.resolve()), "--headless", "--socket",
                str(socket_path), "--state", str(out / "state"), "--session", runtime.name],
               "backend.log")
        wait_for(socket_path.exists, "isolated backend")
        client = support.JsonLineClient(socket_path, 5)
        flags = ["--user-data-dir=" + str(out / "profile"), "--no-first-run",
                 "--no-default-browser-check", "--ozone-platform=x11",
                 "--enable-logging=stderr", "--window-size=1280,900",
                 "--vmodule=cmux_terminal_backend=1,cmux_terminal_pane_linux=1,cmux_views=1"]
        if args.toolkit != "native":
            flags.append("--ui-toolkit=" + args.toolkit)
        if args.scale is not None:
            flags.append("--force-device-scale-factor=" + str(args.scale))
        if not args.native_rendering:
            flags.append("--disable-gpu")
        if args.input_checks:
            flags.append("--remote-debugging-port=0")
        if args.ime_check:
            env.update(GTK_IM_MODULE="fcitx", QT_IM_MODULE="fcitx", XMODIFIERS="@im=fcitx")
        result["flags"] = flags
        browser = launch([str(args.browser.resolve()), *flags], "browser.log")

        def windows():
            try:
                # Chromium's ordinary BrowserView and helper windows can share
                # this PID. Only the cmux window carries our unique build tag.
                return xdo("search", "--all", "--onlyvisible", "--pid", browser.pid,
                           "--name", "^cmux.*" + runtime.name + "$").splitlines()
            except subprocess.CalledProcessError:
                return []

        window = wait_for(windows, "browser window")[-1]
        xdo("windowfocus", "--sync", window)

        def terminals():
            return client.request("list-terminals")[0]["terminals"]

        def key(keyname, expected):
            nonlocal window
            # Chromium may replace its startup window with the cmux window.
            # Resolve a live window again and keep events scoped to this app.
            window = wait_for(windows, "current browser window")[-1]
            xdo("windowfocus", "--sync", window)
            try:
                xdo("key", "--window", window, keyname)
            except subprocess.CalledProcessError:
                # The key can close/replace its target before X sends KeyUp.
                # The backend count below verifies whether it took effect.
                pass
            wait_for(lambda: len(terminals()) == expected, f"{keyname}: {expected} terminals")
            time.sleep(0.25)
            result["operations"].append({"key": keyname, "terminals": expected,
                                           "elapsed_s": round(time.monotonic() - started, 3)})

        # A brand-new canonical registry asynchronously seeds one terminal.
        # Starting shortcuts before that races the startup column/focus and
        # makes terminal counts depend on scheduling rather than the action.
        wait_for(lambda: len(terminals()) == 1, "startup terminal", timeout=20)
        wait_for(lambda: "cmux-keymap: loaded 3 user rules" in
                 (out / "browser.log").read_text(errors="replace"),
                 "configured shortcuts")
        time.sleep(0.5)
        result["startup_terminals"] = len(terminals())
        if args.input_checks:
            debug_file = out / "profile/DevToolsActivePort"
            wait_for(debug_file.exists, "debug endpoint")
            def targets():
                port = int(debug_file.read_text().splitlines()[0])
                opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
                with opener.open(f"http://127.0.0.1:{port}/json/list", timeout=3) as reply:
                    return json.load(reply)
            def page_targets():
                return [t for t in targets() if t["type"] == "page"]
            result["startup_web_pages"] = [t["url"] for t in page_targets()]
        for count in range(2, 5):
            key("F8", count)
        def check_shell(terminal, marker):
            identity = terminal["terminal_id"]
            resolved = wait_for(lambda: client.request("resolve-terminal", terminal_id=identity)[0]
                                .get("surface"), "running terminal")
            result["shell_probe"] = {"terminal_id": identity, "surface": resolved,
                                     "marker": marker}
            command = f"printf '%s\\n' '{marker}'\n"
            client.request("send", surface=resolved,
                           bytes=base64.b64encode(command.encode()).decode())
            wait_for(lambda: marker in (line.rstrip() for line in client.request(
                "read-screen", surface=resolved)[0].get("text", "").splitlines()),
                     "shell output")

        for index, terminal in enumerate(terminals()):
            check_shell(terminal, f"CMUX_SMOKE_{index}")
        # Backend readiness can precede Chromium's startup focus changes.
        # Click inside the visible terminal, as a user would, before testing
        # its shortcuts; setting X focus alone does not select a Views child.
        window = wait_for(windows, "terminal focus window")[-1]
        xdo("mousemove", "--window", window, 400, 300)
        xdo("click", "--window", window, 1)
        time.sleep(0.5)
        for cycle in range(args.cycles):
            key("F9", 3)
            survivors = {t["terminal_id"] for t in terminals()}
            key("F8", 4)
            created = [t for t in terminals() if t["terminal_id"] not in survivors]
            if len(created) != 1:
                raise RuntimeError("new terminal identity was not unique")
            check_shell(created[0], f"CMUX_SMOKE_REPLACED_{cycle}")
            xdo("windowsize", window, 1100 if cycle % 2 else 1280,
                760 if cycle % 2 else 900)
        # Also exercise keyboard input through the visible frontend. Backend
        # shell output alone does not prove the terminal view accepts input.
        surface = client.request("resolve-terminal", terminal_id=created[0]["terminal_id"])[0]["surface"]
        window = wait_for(windows, "keyboard target window")[-1]
        xdo("windowfocus", "--sync", window)
        # Let the last X resize and debounced PTY geometry update complete.
        # Keep this command short enough for a high-DPI terminal's narrow grid.
        time.sleep(0.5)
        xdo("type", "--window", window, "--clearmodifiers", "--delay", 1,
            "echo CMUX_GUI_KEYBOARD")
        xdo("key", "--window", window, "Return")
        wait_for(lambda: "CMUX_GUI_KEYBOARD" in (line.rstrip() for line in client.request(
            "read-screen", surface=surface)[0].get("text", "").splitlines()),
                 "frontend keyboard input")
        result["frontend_keyboard_input"] = True
        if args.input_checks:
            # GTK provides a real X11 clipboard owner in the isolated display.
            # Keep the user's clipboard intact when testing on their desktop.
            gtk_clipboard = """
import sys, gi
from pathlib import Path
gi.require_version('Gtk', '3.0')
from gi.repository import Gtk, Gdk, GLib
clipboard = Gtk.Clipboard.get(Gdk.SELECTION_PRIMARY if sys.argv[1] == 'primary' else Gdk.SELECTION_CLIPBOARD)
if sys.argv[2] == 'get':
    sys.stdout.write(clipboard.wait_for_text() or '')
else:
    clipboard.set_text(sys.argv[2], -1)
    if len(sys.argv) > 3: Path(sys.argv[3]).touch()
    GLib.MainLoop().run()
"""
            def read_clipboard(selection="clipboard"):
                return subprocess.check_output(["/usr/bin/python3", "-c", gtk_clipboard,
                                                selection, "get"], env=env, text=True, timeout=5)
            def own_clipboard(text, selection="clipboard"):
                ready = out / f"clipboard-ready-{len(clipboard_owners)}"
                owner = launch(["/usr/bin/python3", "-c", gtk_clipboard, selection,
                                text, str(ready)], f"clipboard-{len(clipboard_owners)}.log")
                clipboard_owners.append(owner)
                wait_for(ready.exists, "clipboard owner")
            def send(command):
                client.request("send", surface=surface,
                               bytes=base64.b64encode(command.encode()).decode())
            def capture_input(name, action, expected, bracketed=False):
                destination = out / (name + ".bin")
                ready_marker = "INPUT_READY_" + name
                capture = r"""
import os, select, time, tty, termios
from pathlib import Path
original = termios.tcgetattr(0)
received = b''
try:
 tty.setraw(0)
 os.write(1, b'\x1b[2J\x1b[H' + (%r).encode() + (b'\x1b[?2004h' if %r else b'\x1b[?2004l'))
 deadline = time.monotonic() + 20
 while time.monotonic() < deadline:
  if select.select([0], [], [], .1)[0]:
   chunk = os.read(0, 65536)
   if b'\x04' in chunk:
    received += chunk.split(b'\x04')[0]
    break
   received += chunk
finally:
 termios.tcsetattr(0, termios.TCSANOW, original)
 os.write(1, b'\x1b[?2004l\r\n')
 Path(%r).write_bytes(received)
""" % (ready_marker, bracketed, str(destination))
                compile(capture, "capture-input", "exec")
                capture_path = out / "capture-input.py"
                capture_path.write_text(capture)
                send("python3 " + shlex.quote(str(capture_path)) + "\n")
                wait_for(lambda: ready_marker in (line.rstrip() for line in client.request(
                    "read-screen", surface=surface)[0].get("text", "").splitlines()), "raw input reader")
                action()
                time.sleep(.5)
                xdo("key", "--window", window, "ctrl+d")
                wait_for(destination.exists, name + " result", timeout=25)
                actual = destination.read_bytes()
                result[name] = {"expected_hex": expected.hex(), "actual_hex": actual.hex(),
                                "passed": actual == expected}
                if actual != expected:
                    raise RuntimeError(name + " input mismatch: " + repr(actual))
            if not args.display:
                payload = "中文粘贴🙂\nsecond line"
                own_clipboard(payload)
                capture_input("clipboard_bracketed", lambda: xdo("key", "--window", window, "ctrl+shift+v"),
                              b"\x1b[200~" + payload.encode() + b"\x1b[201~", bracketed=True)
                own_clipboard("ShiftInsert中文", "primary")
                capture_input("clipboard_shift_insert", lambda: xdo("key", "--window", window, "shift+Insert"),
                              "ShiftInsert中文".encode())
                capture_input("control_keys", lambda: xdo("key", "--window", window,
                              "ctrl+c", "ctrl+a", "Left", "BackSpace", "Return"), b"\x03\x01\x1b[D\x7f\r")
                # OSC 52 exercises the Ghostty -> system clipboard callback without
                # changing the shell or relying on pixel-dependent selection.
                copied = "CMUX_COPY_中文"
                send("printf '\\033]52;c;" + base64.b64encode(copied.encode()).decode() + "\\007'\n")
                wait_for(lambda: read_clipboard() == copied, "terminal clipboard write")
                result["clipboard_write"] = True
            if args.ime_check:
                original_ime_state = subprocess.check_output(["fcitx-remote"], env=ime_env, text=True).strip()
                subprocess.run(["fcitx-remote", "-s", "sogoupinyin"], env=ime_env, check=True)
                subprocess.run(["fcitx-remote", "-c"], env=ime_env, check=True)
                def ime_key(*arguments):
                    # XSendEvent (--window) marks keys EF_FINAL, deliberately
                    # bypassing GTK IMEs. XTEST is required for real IME input.
                    xdo("windowactivate", "--sync", window)
                    xdo("windowfocus", "--sync", window)
                    if xdo("getwindowfocus").strip() != str(window):
                        raise RuntimeError("test window lost focus before IME input")
                    xdo(*arguments)
                def type_chinese():
                    result["ime_modules"] = sorted({line.split()[-1] for line in
                        Path(f"/proc/{browser.pid}/maps").read_text().splitlines()
                        if "fcitx" in line or "libgtk" in line})
                    result["ime_before_trigger"] = subprocess.check_output(
                        ["fcitx-remote"], env=ime_env, text=True).strip()
                    ime_key("key", "ctrl+space")
                    time.sleep(.5)
                    result["ime_after_trigger"] = subprocess.check_output(
                        ["fcitx-remote"], env=ime_env, text=True).strip()
                    result["ime_selected"] = subprocess.check_output([
                        "dbus-send", "--bus=" + env["FCITX_DBUS_ADDRESS"],
                        "--print-reply", "--dest=org.fcitx.Fcitx", "/inputmethod",
                        "org.fcitx.Fcitx.InputMethod.GetCurrentIM"], text=True)
                    if args.ime_toggle_shift:
                        ime_key("key", "Shift_L")
                    ime_key("type", "--delay", 150, "nihao")
                    time.sleep(.8)
                    geometry = dict(line.split("=", 1) for line in
                        xdo("getwindowgeometry", "--shell", window).splitlines())
                    crop = (f"{geometry['WIDTH']}x{geometry['HEIGHT']}"
                            f"+{geometry['X']}+{geometry['Y']}")
                    # Capture only the test window's screen rectangle, including
                    # its IME popup; XGetImage(window) blacks out popup overlaps.
                    subprocess.run(["import", "-window", "root", "-crop", crop,
                                    str(out / "ime-preedit.png")],
                                   env=env, check=True, capture_output=True)
                    ime_key("key", "space")
                    time.sleep(.5)
                    if args.ime_toggle_shift:
                        ime_key("key", "Shift_L")
                    ime_key("key", "ctrl+space")
                capture_input("sogou_commit", type_chinese, "你好".encode(), bracketed=True)
            # Exercise the shipped settings UI and its persistence, through the
            # isolated browser's debugging endpoint only.
            import websocket
            port = int(debug_file.read_text().splitlines()[0])
            opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
            xdo("key", "--window", window, "ctrl+comma")
            def settings_page():
                return next((t for t in page_targets() if
                    t["url"].startswith(("chrome://cmux-configure", "cmux://settings"))), None)
            settings_target = wait_for(settings_page, "settings shortcut")
            xdo("key", "--window", window, "ctrl+comma")
            time.sleep(.3)
            if len(page_targets()) != 1:
                raise RuntimeError("settings shortcut created a duplicate tab")
            result["settings_shortcut_reuses_tab"] = True
            connection = websocket.create_connection(settings_target["webSocketDebuggerUrl"],
                                                       timeout=10, suppress_origin=True)
            request_id = 0
            def evaluate(expression):
                nonlocal request_id
                request_id += 1
                connection.send(json.dumps({"id": request_id, "method": "Runtime.evaluate", "params": {
                    "expression": expression, "awaitPromise": True, "returnByValue": True,
                    "userGesture": True}}))
                while True:
                    reply = json.loads(connection.recv())
                    if reply.get("id") == request_id:
                        if "exceptionDetails" in reply.get("result", {}):
                            raise RuntimeError(str(reply))
                        return reply.get("result", {}).get("result", {}).get("value")
            wait_for(lambda: evaluate("!!document.querySelector('#ui-font-size') && !document.querySelector('#ui-font-size').disabled"),
                     "settings ready")
            result["appearance_initial"] = evaluate("({ui:document.querySelector('#ui-font-size').value,terminal:document.querySelector('#terminal-font-size').value})")
            evaluate("document.querySelector('#ui-font-size').value='18';document.querySelector('#terminal-font-size').value='20';document.querySelector('#ghostty-theme').value='Cmux Light';document.querySelector('#ghostty-theme').dispatchEvent(new Event('change'));true")
            wait_for(lambda: (out / "layout.json").exists() and
                     json.loads((out / "layout.json").read_text()).get("terminal_font_size") == 20,
                     "settings saved")
            wait_for(lambda: evaluate("document.querySelector('#customization-status').textContent.includes('Using Cmux Light')"),
                     "theme applied")
            result["appearance_saved"] = json.loads((out / "layout.json").read_text())
            xdo("windowraise", window)
            time.sleep(.5)
            subprocess.run(["import", "-window", window, str(out / "settings.png")], env=env,
                           check=True, capture_output=True)
            connection.close()
            with opener.open(f"http://127.0.0.1:{port}/json/close/{settings_target['id']}", timeout=5):
                pass
            time.sleep(.5)
            xdo("windowfocus", "--sync", window)
            xdo("mousemove", "--window", window, 400, 300)
            xdo("click", "--window", window, 1)
        # Exercise ongoing rendering and CJK fallback, not just an idle prompt.
        command = ("printf '中文字体测试 ASCII 0123456789\\n'; "
                   "i=0; while :; do i=$((i+1)); "
                   "printf '\\rCMUX_LIVE_%06d' \"$i\"; sleep 0.1; done\n")
        client.request("send", surface=surface,
                       bytes=base64.b64encode(command.encode()).decode())
        live_counter = 0
        recording = None
        if args.record_seconds:
            recording = launch([
                args.ffmpeg, "-y", "-nostdin", "-loglevel", "warning",
                "-video_size", "1280x900", "-framerate", "20", "-f", "x11grab",
                "-draw_mouse", "0", "-i", env["DISPLAY"],
                "-t", str(args.record_seconds), "-c:v", "libx264",
                "-preset", "ultrafast", "-crf", "23", "-pix_fmt", "yuv420p",
                "-movflags", "+faststart", str(out / "steady-state.mp4")],
                "recording.log")
        deadline = time.monotonic() + max(args.hold_seconds, args.record_seconds)
        def read_live_counter():
            text = client.request("read-screen", surface=surface)[0].get("text", "")
            matches = re.findall(r"CMUX_LIVE_(\d+)", text)
            return int(matches[-1]) if matches else 0

        while time.monotonic() < deadline:
            wait_for(lambda: len(terminals()) == 4, "steady state", timeout=2)
            live_counter = wait_for(
                lambda: (value := read_live_counter()) > live_counter and value,
                "continuous terminal output", timeout=2)
            time.sleep(0.25)
        result["continuous_output_counter"] = live_counter
        if recording is not None and recording.wait(timeout=10) != 0:
            raise RuntimeError("steady-state recording failed; see recording.log")
        # XGetImage can return black/occluded pixels for a covered X11 window.
        # Raise only this test's window before collecting visual evidence.
        xdo("windowraise", window)
        time.sleep(0.25)
        subprocess.run(["import", "-window", window, str(out / "window.png")], env=env,
                       check=True, capture_output=True)
        log = (out / "browser.log").read_text(errors="replace")
        geometry = [tuple(map(float, match)) for match in re.findall(
            r"cmux-term-geometry: scale=([\d.]+) logical=(\d+)x(\d+) "
            r"pixels=(\d+)x(\d+) cells=(\d+)x(\d+)", log)]
        frames = set(re.findall(r"cmux-term-frame: pixels=(\d+x\d+)", log))
        result["terminal_geometry"] = geometry[-8:]
        result["rendered_frame_sizes"] = sorted(frames)
        if args.scale is not None:
            scaled = [g for g in geometry if abs(g[0] - args.scale) < .01]
            if not scaled or not any(f"{int(g[3])}x{int(g[4])}" in frames for g in scaled):
                raise RuntimeError("no rendered terminal frame at requested device scale")
            if any(abs(g[3] - math.ceil(g[1] * g[0])) > 1 or
                   abs(g[4] - math.ceil(g[2] * g[0])) > 1 for g in scaled):
                raise RuntimeError("terminal framebuffer did not match physical pixel size")
        result["unsupported_direct_retries"] = log.count("retrying direct renderer")
        if result["unsupported_direct_retries"]:
            raise RuntimeError("unsupported direct renderer is still being retried")
        result["close_method"] = close_window(window)
        result["graceful_exit_code"] = browser.wait(timeout=15)
        if result["graceful_exit_code"] != 0:
            raise RuntimeError(f"browser exited {result['graceful_exit_code']} on window close")
        if args.input_checks:
            # Browser close must preserve the daemon and exactly the same PTYs.
            preserved = {t["terminal_id"] for t in terminals()}
            browser = launch([str(args.browser.resolve()), *flags], "restart-browser.log")
            window = wait_for(windows, "restart window")[-1]
            time.sleep(3)
            wait_for(lambda: not page_targets(), "restart without a new web tab")
            result["restart_terminal_ids_preserved"] = preserved == {t["terminal_id"] for t in terminals()}
            result["restart_web_pages"] = [t["url"] for t in page_targets()]
            if not result["restart_terminal_ids_preserved"]:
                raise RuntimeError("restart changed terminal identities")
            subprocess.run(["import", "-window", window, str(out / "restart.png")], env=env,
                           check=True, capture_output=True)
            result["restart_close_method"] = close_window(window)
            result["restart_exit_code"] = browser.wait(timeout=15)
            if result["restart_exit_code"] != 0:
                raise RuntimeError("restart did not exit cleanly")
        if args.input_checks and result["startup_web_pages"]:
            raise RuntimeError("unexpected startup web tab")
        result["passed"] = True
    except Exception as error:
        result["error"] = str(error)
        if browser is not None and browser.poll() is None:
            try:
                if result.get("shell_probe"):
                    result["shell_probe_screen"] = client.request(
                        "read-screen", surface=result["shell_probe"]["surface"])[0]
                result["x_keyboard_focus"] = xdo("getwindowfocus")
                result["terminal_snapshot"] = client.request("list-terminals")[0]
                candidates = xdo("search", "--onlyvisible", "--pid", browser.pid).splitlines()
                result["visible_windows"] = {
                    identity: xdo("getwindowname", identity) for identity in candidates}
                target = windows()
                if target:
                    subprocess.run(["import", "-window", target[-1],
                                    str(out / "failure-window.png")], env=env,
                                   check=True, capture_output=True)
            except Exception as diagnostic_error:
                result["diagnostic_error"] = str(diagnostic_error)
    finally:
        if args.ime_check and original_ime_state in ("1", "2"):
            subprocess.run(["fcitx-remote", "-o" if original_ime_state == "2" else "-c"], env=ime_env)
        if args.display and original_clipboard is not None:
            # Klipper retains the original text after this short-lived owner exits.
            try:
                own_clipboard(original_clipboard)
                own_clipboard(original_selection or "", "primary")
                time.sleep(.4)
            except Exception as error:
                result["clipboard_restore_error"] = str(error)
        result["elapsed_s"] = round(time.monotonic() - started, 3)
        result["browser_exit_before_cleanup"] = browser.poll() if browser else None
        # Snapshot only this test's descendants and exact session environment.
        # Crashpad can reparent itself to PID 1 before this cleanup starts.
        # Terminal hosts create their own process groups; never pkill cmux.
        roots = {p.pid for p in processes}
        owned = set(roots)
        parents = {}
        session_marker = ("CMUX_TUI_SESSION=" + runtime.name).encode()
        for path in Path("/proc").iterdir():
            try:
                fields = (path / "stat").read_text().rsplit(")", 1)[1].split()
                parents[int(path.name)] = int(fields[1])
                if session_marker in (path / "environ").read_bytes().split(b"\0"):
                    owned.add(int(path.name))
            except (OSError, ValueError, IndexError):
                continue
        while True:
            children = {pid for pid, parent in parents.items() if parent in owned} - owned
            if not children:
                break
            owned.update(children)
        result["cleanup_pids"] = sorted(owned)
        if client:
            client.close()
        for pid in sorted(owned - roots, reverse=True):
            try:
                os.kill(pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
        for process in reversed(processes):
            if process.poll() is None:
                process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)
        for stream in files:
            stream.close()
        (out / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
