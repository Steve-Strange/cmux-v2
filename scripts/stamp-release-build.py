#!/usr/bin/env python3
"""Stamp a disposable Chromium overlay copy as an official cmux release."""

from __future__ import annotations

import argparse
from pathlib import Path


RELATIVE_HEADER = Path(
    "chrome/browser/cmux_term/cmux_release_build.h"
)
UNSTAMPED = "inline constexpr bool kCmuxOfficialReleaseBuild = false;"
STAMPED = "inline constexpr bool kCmuxOfficialReleaseBuild = true;"
RELATIVE_CRASHPAD_CORE = Path("components/crash/core/app/crashpad.cc")
CRASHPAD_UNSTAMPED = "constexpr bool kCmuxOfficialCrashUploadBuild = false;"
CRASHPAD_STAMPED = "constexpr bool kCmuxOfficialCrashUploadBuild = true;"


def stamp_release_build(chromium_src: Path) -> bool:
    if not (chromium_src / "chrome/VERSION").is_file():
        raise ValueError(f"not a Chromium src checkout: {chromium_src}")
    changed = False
    targets = (
        (RELATIVE_HEADER, UNSTAMPED, STAMPED),
        (RELATIVE_CRASHPAD_CORE, CRASHPAD_UNSTAMPED, CRASHPAD_STAMPED),
    )
    for relative_path, unstamped, stamped in targets:
        path = chromium_src / relative_path
        original = path.read_text(encoding="utf-8")
        if original.count(stamped) == 1 and unstamped not in original:
            continue
        if original.count(unstamped) != 1 or stamped in original:
            raise AssertionError(
                f"cmux release-build stamp anchor changed: {relative_path}"
            )
        path.write_text(
            original.replace(unstamped, stamped, 1), encoding="utf-8"
        )
        changed = True
    return changed


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--chromium-src", type=Path, required=True)
    args = parser.parse_args()
    changed = stamp_release_build(args.chromium_src.resolve())
    print(f"cmux-release-build: {'stamped' if changed else 'already stamped'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
