#!/usr/bin/env python3
"""Interactive color, mouse, and resize probe for cmux terminal frontends.

Run the same command in a Browser-hosted Ghostty pane and in an attached cmux
TUI pane. The probe uses only terminal protocols, so differences identify a
frontend/data-plane bug rather than an application-specific rendering path.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import selectors
import signal
import sys
import termios
import tempfile
import time
import tty
from dataclasses import dataclass
from pathlib import Path


CSI = "\x1b["
OSC = "\x1b]"
ST = "\x1b\\"
MOUSE_RE = re.compile(rb"\x1b\[<(?P<button>\d+);(?P<x>\d+);(?P<y>\d+)(?P<state>[Mm])")
PALETTE_SWATCH_WIDTH = 3
PALETTE_TOP = 4
PALETTE_LEFT = 5
ROLE_TOP = 4
ROLE_SLOT_COLUMNS = (1, 27, 53)
ROLE_LABEL_WIDTH = 15
ROLE_SWATCH_OFFSET = 16
ROLE_SWATCH_WIDTH = 6


@dataclass(frozen=True)
class ColorRole:
    name: str
    sgr: str


def color_roles() -> tuple[ColorRole, ...]:
    """Return stable flat color-role fixtures for screenshot sampling.

    Foreground roles use inverse video over blank cells. That turns the
    effective foreground into a solid cell background, avoiding glyph
    antialiasing while still exercising Ghostty's SGR attribute resolution.
    """
    roles = [
        ColorRole("registration", "0;48;2;19;220;164"),
        ColorRole("default-bg", "0"),
        ColorRole("default-fg", "0;7"),
        ColorRole("truecolor-bg", "0;48;2;255;53;98"),
        ColorRole("truecolor-fg", "0;38;2;94;234;212;7"),
    ]
    roles.extend(
        ColorRole(f"ansi-normal-{index}", f"0;{30 + index};7")
        for index in range(8)
    )
    roles.extend(
        ColorRole(f"ansi-bright-{index}", f"0;{90 + index};7")
        for index in range(8)
    )
    roles.extend(
        ColorRole(f"ansi-bold-{index}", f"0;1;{30 + index};7")
        for index in range(8)
    )
    roles.extend(
        ColorRole(f"ansi-dim-{index}", f"0;2;{30 + index};7")
        for index in range(8)
    )
    roles.extend(
        (
            ColorRole("ansi-inverse-fg", "0;31;44;7"),
            ColorRole("ansi-inverse-bg", "0;31;44"),
        )
    )
    return tuple(roles)


COLOR_ROLES = color_roles()
ROLE_ROWS = (
    len(COLOR_ROLES) + len(ROLE_SLOT_COLUMNS) - 1
) // len(ROLE_SLOT_COLUMNS)
ROLE_CURSOR_ROW = ROLE_TOP + ROLE_ROWS + 1
ROLE_CURSOR_REFERENCE_COL = ROLE_SLOT_COLUMNS[0] + ROLE_SWATCH_OFFSET
ROLE_CURSOR_SAMPLE_COL = ROLE_CURSOR_REFERENCE_COL + 3
ROLE_CURSOR_COL = ROLE_CURSOR_SAMPLE_COL + 1
ROLE_GLYPH_TOP = ROLE_CURSOR_ROW + 2
WHEEL_MARKER_TOP = 4
WHEEL_MARKER_LEFT = 3
WHEEL_MARKER_WIDTH = 64
WHEEL_MARKER_HEIGHT = 12


def write_all(descriptor: int, payload: bytes, write=os.write) -> None:
    """Write a complete payload, including across interrupted/short writes."""
    offset = 0
    while offset < len(payload):
        try:
            written = write(descriptor, payload[offset:])
        except InterruptedError:
            continue
        if written <= 0:
            raise OSError("zero-length write while publishing terminal output")
        offset += written


def wheel_marker_index(wheel_events: int) -> int:
    """Map a wheel generation to one of the 216 distinct xterm cube colors."""
    if wheel_events < 0:
        raise ValueError("wheel event count cannot be negative")
    return 16 + ((180 + wheel_events * 73) % 216)


def role_swatch_cell(name: str) -> tuple[int, int]:
    """Return the 1-based first cell for a flat role swatch."""
    try:
        index = next(
            index for index, role in enumerate(COLOR_ROLES) if role.name == name
        )
    except StopIteration as error:
        raise ValueError(f"unknown color role: {name}") from error
    row = ROLE_TOP + index // len(ROLE_SLOT_COLUMNS)
    col = ROLE_SLOT_COLUMNS[index % len(ROLE_SLOT_COLUMNS)] + ROLE_SWATCH_OFFSET
    return row, col


def palette_chart_rows() -> list[str]:
    """Return a deterministic 16x16 indexed-color chart.

    Row 0 contains palette entries 0x00..0x0f, row 15 contains
    0xf0..0xff. Each swatch is three blank cells so screenshot sampling can
    target its center without touching a label or antialiased glyph.
    """
    rows: list[str] = []
    for high in range(16):
        swatches = []
        for low in range(16):
            index = high * 16 + low
            swatches.append(f"{CSI}48;5;{index}m{' ' * PALETTE_SWATCH_WIDTH}")
        rows.append(f"{high:x}0  {''.join(swatches)}{CSI}0m")
    return rows


def palette_swatch_cell(index: int) -> tuple[int, int]:
    """Return the 1-based terminal cell at the center of one swatch."""
    if not 0 <= index <= 255:
        raise ValueError("palette index must be between 0 and 255")
    row = PALETTE_TOP + index // 16
    col = PALETTE_LEFT + (index % 16) * PALETTE_SWATCH_WIDTH + 1
    return row, col


@dataclass(frozen=True)
class MouseEvent:
    button: int
    x: int
    y: int
    pressed: bool

    @property
    def kind(self) -> str:
        if self.button & 64:
            return "wheel-down" if self.button & 1 else "wheel-up"
        if self.button & 32:
            return "drag" if self.pressed else "motion"
        return "press" if self.pressed else "release"


class ClaimedPidFile:
    """An exclusive pid file that only removes the inode it created."""

    def __init__(self, path: Path) -> None:
        self.path = path
        self.descriptor: int | None = None
        self.identity: tuple[int, int] | None = None

    def claim(self) -> None:
        if self.descriptor is not None:
            raise RuntimeError("pid file is already claimed")
        flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, "O_CLOEXEC", 0)
        flags |= getattr(os, "O_NOFOLLOW", 0)
        descriptor = os.open(self.path, flags, 0o600)
        self.descriptor = descriptor
        try:
            identity = os.fstat(descriptor)
            self.identity = (identity.st_dev, identity.st_ino)
            payload = f"{os.getpid()}\n".encode("ascii")
            write_all(descriptor, payload)
            os.fsync(descriptor)
        except BaseException:
            self.release()
            raise

    def release(self) -> None:
        descriptor = self.descriptor
        self.descriptor = None
        identity = self.identity
        self.identity = None
        if identity is None:
            if descriptor is not None:
                os.close(descriptor)
            return
        try:
            try:
                current = self.path.lstat()
            except FileNotFoundError:
                return
            if (current.st_dev, current.st_ino) == identity:
                self.path.unlink()
        finally:
            if descriptor is not None:
                os.close(descriptor)


def replace_json_file(path: Path, value: object) -> None:
    """Publish one complete state generation without exposing partial JSON."""
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=f".{path.name}.",
        dir=path.parent,
    )
    temporary_path = Path(temporary_name)
    try:
        payload = (json.dumps(value, sort_keys=True) + "\n").encode("utf-8")
        write_all(descriptor, payload)
        os.fsync(descriptor)
        os.close(descriptor)
        descriptor = -1
        os.replace(temporary_path, path)
    finally:
        if descriptor >= 0:
            os.close(descriptor)
        try:
            temporary_path.unlink()
        except FileNotFoundError:
            pass


def parse_mouse(data: bytes) -> list[MouseEvent]:
    return [
        MouseEvent(
            button=int(match.group("button")),
            x=int(match.group("x")),
            y=int(match.group("y")),
            pressed=match.group("state") == b"M",
        )
        for match in MOUSE_RE.finditer(data)
    ]


class Probe:
    def __init__(
        self,
        log_path: Path | None,
        palette_page: bool = False,
        roles_page: bool = False,
        wheel_counter_page: bool = False,
        pid_path: Path | None = None,
        state_path: Path | None = None,
    ) -> None:
        self.log_path = log_path
        self.pid_path = pid_path
        self.state_path = state_path
        self.render_generation = 0
        self.fence_epoch = 0
        self.resize_count = 0
        self.wheel_events = 0
        self.wheel_balance = 0
        self.events: list[str] = []
        self.running = True
        self.overrides = True
        self.page = "summary"
        if palette_page:
            self.page = "palette"
        elif roles_page:
            self.page = "roles"
        elif wheel_counter_page:
            self.page = "wheel-counter"
        self.pending = bytearray()
        self.old_termios: list[int | list[bytes]] | None = None

    def write(self, value: str) -> None:
        write_all(sys.stdout.fileno(), value.encode("utf-8"))

    def size(self) -> os.terminal_size:
        return os.get_terminal_size(sys.stdout.fileno())

    def enter(self) -> None:
        self.old_termios = termios.tcgetattr(sys.stdin.fileno())
        tty.setraw(sys.stdin.fileno())
        self.write(
            CSI
            + "?1049h"
            + CSI
            + "?25h"
            + CSI
            + "6 q"
            + CSI
            + "?1000h"
            + CSI
            + "?1002h"
            + CSI
            + "?1006h"
        )
        self.apply_overrides()

    def leave(self) -> None:
        # Restore all palette/default roles touched by the probe before leaving
        # the alternate screen, even when interrupted.
        self.write(
            OSC
            + "104;1"
            + ST
            + OSC
            + "110"
            + ST
            + OSC
            + "111"
            + ST
            + OSC
            + "112"
            + ST
            + CSI
            + "?1006l"
            + CSI
            + "?1002l"
            + CSI
            + "?1000l"
            + CSI
            + "?25h"
            + CSI
            + "0 q"
            + CSI
            + "?1049l"
        )
        if self.old_termios is not None:
            termios.tcsetattr(sys.stdin.fileno(), termios.TCSADRAIN, self.old_termios)
        if self.log_path:
            self.log_path.write_text("\n".join(self.events) + "\n", encoding="utf-8")

    def apply_overrides(self) -> None:
        if self.overrides:
            # Deliberately unusual values make dropped sparse/default-color
            # propagation obvious in both screenshots and pixel samples.
            self.write(
                OSC
                + "4;1;rgb:ff/35/62"
                + ST
                + OSC
                + "10;rgb:d7/e0/ff"
                + ST
                + OSC
                + "11;rgb:17/1b/2e"
                + ST
                + OSC
                + "12;rgb:5e/ea/d4"
                + ST
            )
        else:
            self.write(OSC + "104;1" + ST + OSC + "110" + ST + OSC + "111" + ST + OSC + "112" + ST)

    @staticmethod
    def at(row: int, col: int, text: str) -> str:
        return f"{CSI}{row};{col}H{text}"

    def redraw_summary(self, columns: int, rows: int, out: list[str]) -> None:
        if rows >= 4:
            out.append(self.at(4, 1, f"default fg/bg  {CSI}1mbold{CSI}0m  {CSI}2mdim{CSI}0m  {CSI}7minverse{CSI}0m"))
        if rows >= 6:
            blocks = []
            for index in range(16):
                blocks.append(f"{CSI}48;5;{index}m{CSI}38;5;{15 if index < 8 else 0}m {index:02d} {CSI}0m")
            out.append(self.at(6, 1, "".join(blocks)))
        if rows >= 8:
            samples = (16, 21, 46, 51, 82, 118, 154, 190, 196, 201, 208, 214, 226, 231, 244, 255)
            blocks = [f"{CSI}48;5;{index}m {index:03d} {CSI}0m" for index in samples]
            out.append(self.at(8, 1, "".join(blocks)))
        if rows >= 10:
            out.append(
                self.at(
                    10,
                    1,
                    f"truecolor {CSI}38;2;94;234;212mFG-5eead4{CSI}0m "
                    f"{CSI}48;2;255;53;98m BG-ff3562 {CSI}0m palette-1={CSI}31mRED{CSI}0m",
                )
            )
        if rows >= 11:
            out.append(self.at(11, 1, "cursor OSC 12 sample ->"))

        grid_top = 12
        if rows >= grid_top:
            cell = 1
            for row in range(grid_top, min(rows - 3, grid_top + 4) + 1):
                line = []
                for _ in range(min(10, max(1, columns // 6))):
                    line.append(f"[{cell:02d}] ")
                    cell += 1
                out.append(self.at(row, 1, "".join(line)[:columns]))

        # Leave a visible steady bar cursor in a stable sample location so
        # OSC 12 is part of the color comparison instead of being hidden by
        # the probe UI itself.
        out.append(CSI + "?25h")

    def redraw_palette(self, columns: int, rows: int, out: list[str]) -> None:
        required_columns = PALETTE_LEFT - 1 + 16 * PALETTE_SWATCH_WIDTH
        required_rows = PALETTE_TOP + 16 + 1
        if columns < required_columns or rows < required_rows:
            warning = (
                f"need at least {required_columns}x{required_rows} "
                "to show all 256 swatches"
            )
            out.append(self.at(3, 1, f"{CSI}0m{warning[:columns]}"))
            out.append(CSI + "?25l")
            return
        header = "    " + "".join(f" {value:x} " for value in range(16))
        out.append(self.at(3, 1, header))
        for offset, line in enumerate(palette_chart_rows()):
            row = PALETTE_TOP + offset
            out.append(self.at(row, 1, line))
        # The cursor would cover one indexed swatch and make pixel sampling
        # nondeterministic. It remains visible on the summary page.
        out.append(CSI + "?25l")

    def redraw_roles(self, columns: int, rows: int, out: list[str]) -> bool:
        required_columns = (
            ROLE_SLOT_COLUMNS[-1] + ROLE_SWATCH_OFFSET + ROLE_SWATCH_WIDTH - 1
        )
        required_rows = ROLE_GLYPH_TOP + 3
        if columns < required_columns or rows < required_rows:
            warning = (
                f"need at least {required_columns}x{required_rows} "
                "to show all role fixtures"
            )
            out.append(self.at(3, 1, f"{CSI}0m{warning[:columns]}"))
            out.append(CSI + "?25l")
            return False

        out.append(
            self.at(
                3,
                1,
                "flat roles: foreground samples use inverse-video blank cells",
            )
        )
        for index, role in enumerate(COLOR_ROLES):
            slot = index % len(ROLE_SLOT_COLUMNS)
            row = ROLE_TOP + index // len(ROLE_SLOT_COLUMNS)
            col = ROLE_SLOT_COLUMNS[slot]
            label = f"{role.name:<{ROLE_LABEL_WIDTH}} "
            swatch = " " * ROLE_SWATCH_WIDTH
            out.append(
                self.at(
                    row,
                    col,
                    f"{CSI}0m{label}{CSI}{role.sgr}m{swatch}{CSI}0m",
                )
            )

        # Two identical "blank + known glyph" pairs let the XCUITest subtract
        # glyph rendering and isolate a cursor bar drawn on either edge of its
        # target cell. The leading blank also captures bars centered one pixel
        # outside the nominal cell rectangle.
        cursor_content = f"{CSI}38;2;245;245;245;48;2;9;14;28m X{CSI}0m"
        out.append(self.at(ROLE_CURSOR_ROW, 1, "cursor-content"))
        out.append(
            self.at(ROLE_CURSOR_ROW, ROLE_CURSOR_REFERENCE_COL, cursor_content)
        )
        out.append(self.at(ROLE_CURSOR_ROW, ROLE_CURSOR_SAMPLE_COL, cursor_content))

        glyph_samples = (
            ("glyph-default", "0"),
            ("glyph-bold", "0;1"),
            ("glyph-dim", "0;2"),
            ("glyph-inverse", "0;31;44;7"),
            ("glyph-truecolor", "0;38;2;94;234;212;48;2;9;14;28"),
        )
        for index, (name, sgr) in enumerate(glyph_samples):
            slot = index % len(ROLE_SLOT_COLUMNS)
            row = ROLE_GLYPH_TOP + index // len(ROLE_SLOT_COLUMNS)
            col = ROLE_SLOT_COLUMNS[slot]
            label = f"{name:<{ROLE_LABEL_WIDTH}} "
            out.append(self.at(row, col, f"{CSI}0m{label}{CSI}{sgr}mMg{CSI}0m"))
        out.append(CSI + "?25h" + CSI + "6 q")
        return True

    def redraw_wheel_counter(self, columns: int, rows: int, out: list[str]) -> None:
        """Paint a timer-free marker derived only from received wheel input."""
        marker_index = wheel_marker_index(self.wheel_events)
        marker_width = min(
            WHEEL_MARKER_WIDTH,
            max(1, columns - WHEEL_MARKER_LEFT + 1),
        )
        marker_height = min(
            WHEEL_MARKER_HEIGHT,
            max(1, rows - WHEEL_MARKER_TOP - 2),
        )
        for offset in range(marker_height):
            row = WHEEL_MARKER_TOP + offset
            out.append(
                self.at(
                    row,
                    WHEEL_MARKER_LEFT,
                    f"{CSI}48;5;{marker_index}m{' ' * marker_width}{CSI}0m",
                )
            )
        label = (
            f"wheel_events={self.wheel_events} balance={self.wheel_balance:+d} "
            f"marker={marker_index}"
        )
        label_row = WHEEL_MARKER_TOP + marker_height // 2
        out.append(
            self.at(
                label_row,
                WHEEL_MARKER_LEFT + 2,
                f"{CSI}0;30;47m {label} {CSI}0m",
            )
        )
        # A hidden cursor makes an otherwise idle frame pixel-stable. This
        # page has no clock: only input and SIGWINCH can redraw its marker.
        out.append(CSI + "?25l")

    def publish_state(self, columns: int, rows: int) -> None:
        if self.state_path is None:
            return
        replace_json_file(
            self.state_path,
            {
                "generation": self.render_generation,
                "fence_epoch": self.fence_epoch,
                "columns": columns,
                "rows": rows,
                "page": self.page,
                "overrides": self.overrides,
                "wheel_events": self.wheel_events,
                "wheel_balance": self.wheel_balance,
                "wheel_marker_index": wheel_marker_index(self.wheel_events),
                "monotonic_ns": time.monotonic_ns(),
            },
        )

    def redraw(self) -> None:
        columns, rows = self.size()
        self.resize_count += 1
        self.render_generation += 1
        out = [CSI + "0m" + CSI + "2J" + CSI + "H"]
        page = "palette 16x16" if self.page == "palette" else self.page
        override_state = "on" if self.overrides else "off"
        title = (
            f"cmux parity probe  page={page}  grid={columns}x{rows}  "
            f"resize={self.resize_count}  OSC overrides={override_state}"
        )
        out.append(self.at(1, 1, title[:columns]))
        controls = (
            "q quit | p summary/palette | r roles | c toggle OSC 4/10/11/12 | "
            "f publish fence | "
            "mouse | resize rapidly"
        )
        out.append(self.at(2, 1, controls[:columns]))
        roles_visible = False
        if self.page == "palette":
            self.redraw_palette(columns, rows, out)
        elif self.page == "roles":
            roles_visible = self.redraw_roles(columns, rows, out)
        elif self.page == "wheel-counter":
            self.redraw_wheel_counter(columns, rows, out)
        else:
            self.redraw_summary(columns, rows, out)

        if rows >= 2:
            recent = " | ".join(self.events[-3:]) or "no mouse events yet"
            out.append(self.at(max(1, rows - 1), 1, (CSI + "0m" + recent)[: columns + len(CSI + "0m")]))
        # Exact single-cell border corners make scale-first resize frames easy
        # to spot. Do not draw full borders over the palette/mouse canvas.
        out.extend(
            [
                self.at(1, columns, "┐"),
                self.at(rows, 1, "└"),
                self.at(rows, columns, "┘"),
            ]
        )
        if self.page == "summary":
            cursor_row = 11 if rows >= 11 else max(1, min(rows, 3))
            cursor_col = min(columns, 25)
            out.append(self.at(cursor_row, cursor_col, ""))
        elif self.page == "roles" and roles_visible:
            out.append(self.at(ROLE_CURSOR_ROW, ROLE_CURSOR_COL, ""))
        self.write("".join(out))
        self.publish_state(columns, rows)

    def record_mouse(self, event: MouseEvent) -> None:
        line = f"{time.monotonic_ns()} {event.kind} button={event.button} x={event.x} y={event.y}"
        self.events.append(line)
        if event.kind == "wheel-up":
            self.wheel_events += 1
            self.wheel_balance -= 1
        elif event.kind == "wheel-down":
            self.wheel_events += 1
            self.wheel_balance += 1
        self.redraw()

    def consume(self, data: bytes) -> None:
        self.pending.extend(data)
        events = parse_mouse(bytes(self.pending))
        for event in events:
            self.record_mouse(event)
        self.pending = bytearray(MOUSE_RE.sub(b"", bytes(self.pending)))
        # Mouse sequences can be split across reads; retain a possible prefix.
        escape = self.pending.rfind(b"\x1b")
        ordinary = self.pending if escape < 0 else self.pending[:escape]
        for byte in ordinary:
            if byte in (ord("q"), 3):
                self.running = False
            elif byte == ord("c"):
                self.overrides = not self.overrides
                self.apply_overrides()
                self.redraw()
            elif byte == ord("p"):
                self.page = "summary" if self.page == "palette" else "palette"
                self.redraw()
            elif byte == ord("r"):
                self.page = "roles"
                self.redraw()
            elif byte == ord("f"):
                # XCUITest sends this only after Browser has published its
                # resize-settled product epoch. Reflecting a distinct marker
                # in the atomically replaced state file proves the sampled
                # PTY grid was read after that product boundary; an unrelated
                # mouse redraw or SIGWINCH cannot satisfy the fence.
                self.fence_epoch += 1
                columns, rows = self.size()
                self.publish_state(columns, rows)
        if escape < 0:
            self.pending.clear()
        elif escape > 0:
            del self.pending[:escape]
        if len(self.pending) > 128:
            self.pending.clear()

    def run(self) -> int:
        selector = selectors.DefaultSelector()
        selector.register(sys.stdin.fileno(), selectors.EVENT_READ)
        resize_pending = True

        def resized(_signum: int, _frame: object) -> None:
            nonlocal resize_pending
            resize_pending = True

        signal.signal(signal.SIGWINCH, resized)
        self.enter()
        pid_file = ClaimedPidFile(self.pid_path) if self.pid_path is not None else None
        try:
            if pid_file is not None:
                # Claim only after raw mode and mouse reporting are active, so
                # observing this file is also a readiness barrier for UI tests.
                pid_file.claim()
            while self.running:
                if resize_pending:
                    resize_pending = False
                    self.redraw()
                for key, _ in selector.select(timeout=0.05):
                    chunk = os.read(key.fd, 4096)
                    if not chunk:
                        self.running = False
                        break
                    self.consume(chunk)
        finally:
            if pid_file is not None:
                pid_file.release()
            self.leave()
        return 0


def self_test() -> int:
    sink = bytearray()
    write_attempts = 0

    def short_write(_descriptor: int, payload: bytes) -> int:
        nonlocal write_attempts
        write_attempts += 1
        if write_attempts == 1:
            raise InterruptedError
        count = min(3, len(payload))
        sink.extend(payload[:count])
        return count

    write_all(123, b"complete-pty-frame", write=short_write)
    assert sink == b"complete-pty-frame"
    assert write_attempts > 2
    assert wheel_marker_index(0) == 196
    assert len({wheel_marker_index(index) for index in range(216)}) == 216
    assert wheel_marker_index(216) == wheel_marker_index(0)
    try:
        wheel_marker_index(-1)
    except ValueError:
        pass
    else:
        raise AssertionError("negative wheel generation was accepted")

    fence_probe = Probe(None)
    published_fences: list[tuple[int, int, int]] = []
    fence_probe.size = lambda: os.terminal_size((120, 40))
    fence_probe.publish_state = lambda columns, rows: published_fences.append(
        (fence_probe.fence_epoch, columns, rows)
    )
    fence_probe.consume(b"f")
    assert fence_probe.fence_epoch == 1
    assert published_fences == [(1, 120, 40)]
    fence_probe.consume(b"x")
    assert published_fences == [(1, 120, 40)]

    parsed = parse_mouse(b"noise\x1b[<0;4;12M\x1b[<32;5;13M\x1b[<0;5;13m\x1b[<65;8;9M")
    assert [(event.kind, event.x, event.y) for event in parsed] == [
        ("press", 4, 12),
        ("drag", 5, 13),
        ("release", 5, 13),
        ("wheel-down", 8, 9),
    ]
    chart = palette_chart_rows()
    assert len(chart) == 16
    encoded = "".join(chart)
    for index in range(256):
        assert encoded.count(f"{CSI}48;5;{index}m") == 1
        expected_cell = (
            PALETTE_TOP + index // 16,
            PALETTE_LEFT + (index % 16) * PALETTE_SWATCH_WIDTH + 1,
        )
        assert palette_swatch_cell(index) == expected_cell
    assert palette_swatch_cell(0) == (4, 6)
    assert palette_swatch_cell(255) == (19, 51)
    try:
        palette_swatch_cell(256)
    except ValueError:
        pass
    else:
        raise AssertionError("out-of-range palette index was accepted")
    assert len(COLOR_ROLES) == 39
    assert len({role.name for role in COLOR_ROLES}) == len(COLOR_ROLES)
    assert role_swatch_cell("registration") == (4, 17)
    assert role_swatch_cell("default-bg") == (4, 43)
    assert role_swatch_cell("default-fg") == (4, 69)
    assert role_swatch_cell("ansi-inverse-bg") == (16, 69)
    assert ROLE_CURSOR_ROW == 18
    assert ROLE_CURSOR_REFERENCE_COL == 17
    assert ROLE_CURSOR_SAMPLE_COL == 20
    assert ROLE_CURSOR_COL == 21
    assert ROLE_GLYPH_TOP == 20
    try:
        role_swatch_cell("missing")
    except ValueError:
        pass
    else:
        raise AssertionError("unknown color role was accepted")
    with tempfile.TemporaryDirectory(prefix="cmux-parity-probe-self-test-") as directory:
        state_path = Path(directory) / "state.json"
        replace_json_file(
            state_path,
            {
                "generation": 7,
                "fence_epoch": 3,
                "columns": 120,
                "rows": 40,
                "page": "roles",
                "overrides": False,
            },
        )
        assert json.loads(state_path.read_text(encoding="utf-8")) == {
            "generation": 7,
            "fence_epoch": 3,
            "columns": 120,
            "rows": 40,
            "page": "roles",
            "overrides": False,
        }
        path = Path(directory) / "probe.pid"
        claim = ClaimedPidFile(path)
        claim.claim()
        assert path.read_text(encoding="ascii") == f"{os.getpid()}\n"
        try:
            ClaimedPidFile(path).claim()
        except FileExistsError:
            pass
        else:
            raise AssertionError("pid file allowed a second owner")
        path.unlink()
        path.write_text("replacement\n", encoding="ascii")
        claim.release()
        assert path.read_text(encoding="ascii") == "replacement\n"
        path.unlink()
        claim = ClaimedPidFile(path)
        claim.claim()
        claim.release()
        assert not path.exists()
    print("terminal parity probe self-test: PASS")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--log", type=Path, help="write ordered mouse events on exit")
    parser.add_argument(
        "--pid-file",
        type=Path,
        help="exclusively publish this process id after terminal modes are active",
    )
    parser.add_argument(
        "--state-file",
        type=Path,
        help="atomically publish render generation, page, colors, and PTY grid",
    )
    parser.add_argument(
        "--palette", action="store_true", help="start on the 16x16 palette page"
    )
    parser.add_argument(
        "--roles", action="store_true", help="start on the flat color-role page"
    )
    parser.add_argument(
        "--wheel-counter",
        action="store_true",
        help="start on a timer-free marker page driven only by wheel events",
    )
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    if not sys.stdin.isatty() or not sys.stdout.isatty():
        parser.error("interactive mode requires a TTY")
    selected_pages = sum((args.palette, args.roles, args.wheel_counter))
    if selected_pages > 1:
        parser.error("--palette, --roles, and --wheel-counter are mutually exclusive")
    return Probe(
        args.log,
        palette_page=args.palette,
        roles_page=args.roles,
        wheel_counter_page=args.wheel_counter,
        pid_path=args.pid_file,
        state_path=args.state_file,
    ).run()


if __name__ == "__main__":
    raise SystemExit(main())
