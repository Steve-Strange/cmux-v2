#!/usr/bin/env python3
from __future__ import annotations

import importlib.util
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parent
SPEC = importlib.util.spec_from_file_location(
    "prepare_ghostty_linux_pic",
    ROOT / "prepare-ghostty-linux-pic.py",
)
assert SPEC and SPEC.loader
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


def build_fixture(root: Path) -> None:
    for relative in MODULE.STATIC_PACKAGE_BUILDS:
        path = root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("prefix\n" + MODULE.STATIC_LIBRARY_ANCHOR + "suffix\n")

    ghostty_library = root / MODULE.GHOSTTY_LIBRARY_FILE
    ghostty_library.parent.mkdir(parents=True, exist_ok=True)
    ghostty_library.write_text(
        "prefix\n" + MODULE.GHOSTTY_LIBRARY_ANCHOR + "suffix\n"
    )

    fontconfig = root / MODULE.FONTCONFIG_FILE
    fontconfig.write_text(
        fontconfig.read_text() + MODULE.LIBXML2_ANCHOR
    )


def main() -> int:
    with tempfile.TemporaryDirectory() as raw:
        fixture = Path(raw)
        build_fixture(fixture)
        MODULE.apply_pic_patch(fixture)
        MODULE.apply_pic_patch(fixture)

        for relative in MODULE.STATIC_PACKAGE_BUILDS:
            text = (fixture / relative).read_text()
            assert text.count(MODULE.STATIC_LIBRARY_MARKER) == 1
        assert (
            "Chromium's position-independent Linux binary"
            in (fixture / MODULE.GHOSTTY_LIBRARY_FILE).read_text()
        )
        assert (
            "libxml2 is bundled transitively"
            in (fixture / MODULE.FONTCONFIG_FILE).read_text()
        )

        broken = fixture / MODULE.STATIC_PACKAGE_BUILDS[0]
        broken.write_text("unexpected upstream contents\n")
        try:
            MODULE.apply_pic_patch(fixture)
        except MODULE.PatchError:
            pass
        else:
            raise AssertionError("changed upstream input did not fail closed")

    print("Ghostty Linux PIC preparation tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
