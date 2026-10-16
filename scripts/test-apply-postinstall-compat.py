#!/usr/bin/env python3
"""Regression test for M149/M150 post-install dialog builder calls."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
OLD_CALL = "ConfigurePostInstallDialogModel(dialog_model_builder,"
NEW_CALL = "ConfigurePostInstallDialogModel(profile, dialog_model_builder,"
SIGNATURE = "void ConfigurePostInstallDialogModel(\n    Profile* profile,"


def emitted_call(source: str) -> str:
    builder = OLD_CALL
    if SIGNATURE in source:
        builder = builder.replace(OLD_CALL, NEW_CALL, 1)
    return builder


assert emitted_call("void ConfigurePostInstallDialogModel(\n    ui::DialogModel::Builder&") == OLD_CALL
assert emitted_call(SIGNATURE) == NEW_CALL

for relative in ("scripts/apply.sh", "scripts/apply_win_chrome.py"):
    patcher = (ROOT / relative).read_text()
    assert SIGNATURE.replace("\n", "\\n") in patcher, relative
    assert "builder = builder.replace(" in patcher, relative
    assert repr(OLD_CALL)[1:-1] in patcher, relative
    assert repr(NEW_CALL)[1:-1] in patcher, relative

print("post-install dialog M149/M150 compatibility: PASS")
