#!/usr/bin/env python3
"""Regression test for Chromium 151's CommandUpdaterDelegate API change."""

import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PANE = (
    ROOT / "overlay/chrome/browser/cmux_term/cmux_browser_pane.cc"
).read_text()

# Chromium 149/150 used ExecuteCommandWithDisposition(id, disposition).
# Chromium 151 renamed the entry point and added an input-event timestamp.
compat = re.search(
    r"// CommandUpdaterDelegate:.*?\n"
    r"#if CHROME_VERSION_MAJOR >= 151\n"
    r"(?P<m151>.*?)"
    r"#else\n"
    r"(?P<m149>.*?)"
    r"#endif\n"
    r"(?P<body>\s*content::NavigationController& c = web_contents_->GetController\(\);)",
    PANE,
    re.DOTALL,
)
assert compat, "missing CommandUpdaterDelegate M149/M151 compatibility block"

m151 = compat.group("m151")
m149 = compat.group("m149")
assert "HandleCommandWithDisposition" in m151
assert "base::TimeTicks" in m151
assert "ExecuteCommandWithDisposition" not in m151
assert "ExecuteCommandWithDisposition" in m149
assert "HandleCommandWithDisposition" not in m149
assert "base::TimeTicks" not in m149

# Both interface variants must enter the same navigation-controller command
# body; version drift should never fork reload/stop semantics.
assert compat.group("body").strip() == (
    "content::NavigationController& c = web_contents_->GetController();"
)
assert PANE.count("case IDC_RELOAD:") == 1
assert PANE.count("case IDC_STOP:") == 1

print("CommandUpdaterDelegate M149/M151 compatibility: PASS")
