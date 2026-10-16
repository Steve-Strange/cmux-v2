#!/usr/bin/env python3
"""Regression test for Windows warm-tree UA-brand cleanup."""

import os
from pathlib import Path
import tempfile


ROOT = Path(__file__).resolve().parent.parent
APPLY = (ROOT / "scripts/apply_win_chrome.py").read_text()
START = APPLY.index("# 6. Keep Chromium's native UA client-hint brands.")
END = APPLY.index("# 6b.", START)
PATCH = APPLY[START:END]
ANCHOR = """  std::optional<std::string> brand;
#if !BUILDFLAG(CHROMIUM_BRANDING)
  brand = version_info::GetProductName();
#endif"""
BRAND = '  brand = "Google Chrome";'
LEGACY_FIXTURES = (
    ANCHOR + "\n" + BRAND,
    ANCHOR + "\n  // cmux: present as Google Chrome brand.\n" + BRAND,
    ANCHOR
    + '\n  // cmux: present as "Google Chrome" so Sec-CH-UA / navigator.userAgentData\n'
    + "  // brands match real Chrome (our build is Chromium-branded otherwise).\n"
    + BRAND,
)


def apply_to(source: str) -> str:
    with tempfile.TemporaryDirectory() as temp:
        checkout = Path(temp)
        target = checkout / "components/embedder_support/user_agent_utils.cc"
        target.parent.mkdir(parents=True)
        target.write_text(source)
        old_cwd = Path.cwd()
        try:
            os.chdir(checkout)
            exec(compile(PATCH, "apply_win_chrome.py UA patch", "exec"), {})
        finally:
            os.chdir(old_cwd)
        return target.read_text()


for fixture in LEGACY_FIXTURES:
    patched = apply_to("before\n" + fixture + "\nafter\n")
    assert patched == "before\n" + ANCHOR + "\nafter\n"
    assert BRAND not in patched
    assert "cmux: present as" not in patched

pristine = "before\n" + ANCHOR + "\nafter\n"
assert apply_to(pristine) == pristine
print("apply-win UA warm-tree migration: PASS")
