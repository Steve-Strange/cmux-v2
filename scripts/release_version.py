#!/usr/bin/env python3
"""Validate and apply the four-part version used by release builders."""

from __future__ import annotations

import argparse
from pathlib import Path
import re


VERSION_RE = re.compile(r"^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$")


def parse_version(value: str) -> tuple[int, int, int, int]:
    match = VERSION_RE.fullmatch(value)
    if not match:
        raise ValueError("version must contain exactly four numeric components")
    parts = tuple(int(component) for component in match.groups())
    if any(component > 65535 for component in parts):
        raise ValueError("each version component must fit in an unsigned 16-bit field")
    return parts  # type: ignore[return-value]


def read_pinned_version(repository: Path) -> tuple[int, int, int, int]:
    return parse_version(
        (repository / ".chromium-version").read_text(encoding="utf-8").strip()
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--chromium-src", type=Path, required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--repository", type=Path,
                        default=Path(__file__).resolve().parent.parent)
    args = parser.parse_args()

    requested = parse_version(args.version)
    pinned = read_pinned_version(args.repository)
    if requested[:3] != pinned[:3]:
        parser.error(
            "release version must retain the pinned Chromium major/minor/build "
            f"{pinned[0]}.{pinned[1]}.{pinned[2]}"
        )
    if requested[3] <= pinned[3]:
        parser.error(
            "release patch must be greater than the pinned Chromium baseline "
            f"{pinned[3]}"
        )

    version_file = args.chromium_src.resolve() / "chrome" / "VERSION"
    if not version_file.is_file():
        parser.error(f"Chromium VERSION file does not exist: {version_file}")
    current_lines = version_file.read_text(encoding="utf-8").splitlines()
    current: dict[str, int] = {}
    for line in current_lines:
        key, separator, value = line.partition("=")
        if separator and value.isdigit():
            current[key] = int(value)
    if tuple(current.get(key, -1) for key in ("MAJOR", "MINOR", "BUILD")) != pinned[:3]:
        parser.error(
            f"runner Chromium checkout does not match .chromium-version: {current}"
        )

    version_file.write_text(
        "\n".join(
            f"{key}={value}"
            for key, value in zip(
                ("MAJOR", "MINOR", "BUILD", "PATCH"), requested, strict=True
            )
        ) + "\n",
        encoding="utf-8",
    )
    print(f"release version: {args.version}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
