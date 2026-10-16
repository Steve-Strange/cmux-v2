#!/usr/bin/env python3
"""Build the ZIP consumed by cmux's updater while preserving POSIX metadata."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import stat
import zipfile


def add_path(archive: zipfile.ZipFile, source: Path, arcname: Path) -> None:
    mode = source.lstat().st_mode
    name = arcname.as_posix()
    if stat.S_ISLNK(mode):
        info = zipfile.ZipInfo(name)
        info.create_system = 3
        info.external_attr = (mode & 0xFFFF) << 16
        archive.writestr(info, os.readlink(source).encode("utf-8"))
        return
    if source.is_dir():
        info = zipfile.ZipInfo(name.rstrip("/") + "/")
        info.create_system = 3
        info.external_attr = ((mode & 0xFFFF) << 16) | 0x10
        archive.writestr(info, b"")
        for child in sorted(source.iterdir(), key=lambda item: item.name):
            add_path(archive, child, arcname / child.name)
        return
    info = zipfile.ZipInfo.from_file(source, name)
    info.create_system = 3
    info.external_attr = (mode & 0xFFFF) << 16
    with source.open("rb") as handle, archive.open(info, "w") as output:
        while chunk := handle.read(1024 * 1024):
            output.write(chunk)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    source = args.input.resolve()
    if not source.exists() or source.parent == source:
        parser.error(f"invalid input: {source}")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(
        args.output, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9
    ) as archive:
        add_path(archive, source, Path(source.name))
    print(f"wrote {args.output} (archive_root={source.name})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
