#!/usr/bin/env python3
"""Regression test for the M150 TabStripModel include drift."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MODEL_CALL = "->GetTabStripModel()"
MODEL_HEADER = '#include "chrome/browser/ui/tabs/tab_strip_model.h"'


def add_model_header(source: str) -> str:
    if MODEL_CALL in source and MODEL_HEADER not in source:
        source = source.replace(
            '#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"\n',
            '#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"\n'
            f"{MODEL_HEADER}\n",
            1,
        )
    return source


m149 = '#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"\n'
m150 = m149 + "browser_->GetTabStripModel()->GetActiveWebContents();\n"
assert MODEL_HEADER not in add_model_header(m149)
assert add_model_header(m150).count(MODEL_HEADER) == 1
assert add_model_header(add_model_header(m150)).count(MODEL_HEADER) == 1

for relative in ("scripts/apply.sh", "scripts/apply_win_chrome.py"):
    patcher = (ROOT / relative).read_text()
    assert MODEL_CALL in patcher, relative
    assert MODEL_HEADER.replace('"', '\\"') in patcher or MODEL_HEADER in patcher, relative

print("extension-menu M149/M150 TabStripModel compatibility: PASS")
