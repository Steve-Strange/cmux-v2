#!/usr/bin/env python3
"""Restore permission edits using the previously mirrored patch definition.

This helper is streamed to Unix builders and run directly on Windows before
the old `.cmux-patches` directory is replaced. Loading the previous definition
is essential: a new branch may change or remove an anchor and therefore cannot
reliably reverse edits made by the old branch.
"""

from pathlib import Path
import runpy
import sys


def chromium_major(root: Path) -> int:
    version_path = root / "chrome" / "VERSION"
    for line in version_path.read_text().splitlines():
        key, separator, value = line.partition("=")
        if separator and key == "MAJOR":
            return int(value)
    raise RuntimeError(f"{version_path} does not define MAJOR")


def restore_legacy_source(
    relative_path: str, source: str, replacements: tuple
) -> tuple[str, list[str]]:
    """Reverse a pre-restore-API patch definition while failing closed."""
    changed = []
    for replacement in reversed(replacements):
        new_count = source.count(replacement.new)
        old_count = source.count(replacement.old)
        embedded_old_count = replacement.new.count(replacement.old) * new_count
        standalone_old_count = old_count - embedded_old_count

        if new_count == 1 and standalone_old_count == 0:
            source = source.replace(replacement.new, replacement.old, 1)
            changed.append(replacement.label)
        elif new_count == 0 and old_count == 1:
            continue
        else:
            raise AssertionError(
                f"{relative_path}: {replacement.label}: expected exactly one "
                "patched or upstream anchor, got "
                f"patched={new_count}, upstream={standalone_old_count}"
            )
    return source, changed


def restore_previous_tree(root: Path) -> None:
    previous_patcher = (
        root / ".cmux-patches" / "custom_window_permissions.py"
    )
    if not previous_patcher.is_file():
        print(
            "custom-window-permissions-refresh: no previous patch definition"
        )
        return

    previous = runpy.run_path(
        str(previous_patcher),
        run_name="__cmux_previous_custom_window_permissions__",
    )
    minimum_major = previous.get("MINIMUM_CHROMIUM_MAJOR", 151)
    if not isinstance(minimum_major, int):
        raise RuntimeError(
            f"{previous_patcher} has an invalid MINIMUM_CHROMIUM_MAJOR"
        )
    actual_major = chromium_major(root)
    if actual_major < minimum_major:
        print(
            "custom-window-permissions-refresh: previous definition does not "
            f"apply to Chromium {actual_major}"
        )
        return

    restore_tree = previous.get("restore_tree")
    if callable(restore_tree):
        restore_tree(root)
        return

    replacements_by_path = previous.get("REPLACEMENTS")
    if not isinstance(replacements_by_path, dict):
        raise RuntimeError(
            f"{previous_patcher} has no reversible REPLACEMENTS definition"
        )
    for relative_path, replacements in replacements_by_path.items():
        path = root / relative_path
        source = path.read_text()
        restored, changed = restore_legacy_source(
            relative_path, source, replacements
        )
        if changed:
            with path.open("w", newline="\n") as output:
                output.write(restored)
            for label in changed:
                print(f"custom-window-permissions-refresh: {label}: restored")
        else:
            print(
                "custom-window-permissions-refresh: "
                f"{relative_path}: already pristine"
            )


def main(argv: list[str]) -> int:
    if len(argv) > 2:
        print(f"usage: {argv[0]} [chromium-src]", file=sys.stderr)
        return 2
    restore_previous_tree(Path(argv[1]) if len(argv) == 2 else Path.cwd())
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
