#!/usr/bin/env python3
"""Make the pinned Ghostty static graph safe to embed in Chromium's Linux PIE."""

from __future__ import annotations

import argparse
import subprocess
from pathlib import Path


STATIC_PACKAGE_BUILDS = (
    "pkg/dcimgui/build.zig",
    "pkg/fontconfig/build.zig",
    "pkg/freetype/build.zig",
    "pkg/glslang/build.zig",
    "pkg/harfbuzz/build.zig",
    "pkg/highway/build.zig",
    "pkg/libpng/build.zig",
    "pkg/oniguruma/build.zig",
    "pkg/simdutf/build.zig",
    "pkg/spirv-cross/build.zig",
    "pkg/zlib/build.zig",
)

STATIC_LIBRARY_ANCHOR = """\
        .linkage = .static,
    });
"""
STATIC_LIBRARY_MARKER = """\
    // cmux embeds this archive in Chromium's position-independent Linux binary.
    lib.root_module.pic = true;
"""

GHOSTTY_LIBRARY_FILE = "src/build/GhosttyLib.zig"
GHOSTTY_LIBRARY_ANCHOR = """\
        .use_llvm = true,
    });
    lib.linkLibC();
"""
GHOSTTY_LIBRARY_REPLACEMENT = """\
        .use_llvm = true,
    });
    // cmux embeds this archive in Chromium's position-independent Linux binary.
    lib.root_module.pic = true;
    lib.linkLibC();
"""

FONTCONFIG_FILE = "pkg/fontconfig/build.zig"
LIBXML2_ANCHOR = """\
            })) |libxml2_dep| {
                lib.linkLibrary(libxml2_dep.artifact("xml2"));
            }
"""
LIBXML2_REPLACEMENT = """\
            })) |libxml2_dep| {
                // libxml2 is bundled transitively into the cmux Ghostty archive.
                const libxml2_lib = libxml2_dep.artifact("xml2");
                libxml2_lib.root_module.pic = true;
                lib.linkLibrary(libxml2_lib);
            }
"""

EXPECTED_CHANGED_FILES = frozenset(
    (*STATIC_PACKAGE_BUILDS, GHOSTTY_LIBRARY_FILE)
)


class PatchError(RuntimeError):
    pass


def _insert_after_once(path: Path, anchor: str, marker: str) -> None:
    text = path.read_text()
    if marker in text:
        if text.count(marker) != 1:
            raise PatchError(f"{path}: PIC marker occurs more than once")
        return
    if text.count(anchor) != 1:
        raise PatchError(f"{path}: expected one static-library anchor")
    path.write_text(text.replace(anchor, anchor + marker, 1))


def _replace_once(
    path: Path,
    anchor: str,
    replacement: str,
    marker: str,
) -> None:
    text = path.read_text()
    if marker in text:
        if text.count(marker) != 1:
            raise PatchError(f"{path}: patch marker occurs more than once")
        return
    if text.count(anchor) != 1:
        raise PatchError(f"{path}: expected one patch anchor")
    path.write_text(text.replace(anchor, replacement, 1))


def apply_pic_patch(source: Path) -> None:
    for relative in STATIC_PACKAGE_BUILDS:
        _insert_after_once(
            source / relative,
            STATIC_LIBRARY_ANCHOR,
            STATIC_LIBRARY_MARKER,
        )

    _replace_once(
        source / GHOSTTY_LIBRARY_FILE,
        GHOSTTY_LIBRARY_ANCHOR,
        GHOSTTY_LIBRARY_REPLACEMENT,
        "Chromium's position-independent Linux binary",
    )
    _replace_once(
        source / FONTCONFIG_FILE,
        LIBXML2_ANCHOR,
        LIBXML2_REPLACEMENT,
        "libxml2 is bundled transitively",
    )


def _git(source: Path, *args: str) -> str:
    return subprocess.check_output(
        ("git", "-C", str(source), *args),
        text=True,
    ).strip()


def prepare_checkout(source: Path, expected_revision: str) -> None:
    actual_revision = _git(source, "rev-parse", "HEAD")
    if actual_revision != expected_revision:
        raise PatchError(
            f"{source}: Ghostty is {actual_revision}; expected {expected_revision}"
        )

    dirty_before = {
        line
        for line in _git(source, "diff", "--name-only").splitlines()
        if line
    }
    unexpected = dirty_before - EXPECTED_CHANGED_FILES
    if unexpected:
        raise PatchError(
            f"{source}: unexpected pre-existing changes: {sorted(unexpected)}"
        )

    apply_pic_patch(source)

    dirty_after = {
        line
        for line in _git(source, "diff", "--name-only").splitlines()
        if line
    }
    if dirty_after != EXPECTED_CHANGED_FILES:
        raise PatchError(
            f"{source}: PIC patch changed unexpected files: {sorted(dirty_after)}"
        )
    subprocess.run(
        ("git", "-C", str(source), "diff", "--check"),
        check=True,
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--revision", required=True)
    args = parser.parse_args()

    prepare_checkout(args.source.resolve(), args.revision)
    print(
        "Prepared pinned Ghostty static dependencies for Linux PIE embedding: "
        f"{args.revision}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
