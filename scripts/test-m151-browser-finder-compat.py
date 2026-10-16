#!/usr/bin/env python3
"""Regression test for Chromium 151's browser_finder.h removal."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
OVERLAY = ROOT / "overlay/chrome/browser/cmux_term"
HELPER = (OVERLAY / "cmux_browser_finder.h").read_text()

assert "#if CHROME_VERSION_MAJOR >= 151" in HELPER
assert 'chrome/browser/ui/browser_window/public/global_browser_collection.h' in HELPER
assert "GlobalBrowserCollection::GetInstance()->FindBrowserWithTab" in HELPER
assert "GetBrowserForMigrationOnly()" in HELPER
assert '#include "chrome/browser/ui/browser_finder.h"' in HELPER
assert "return chrome::FindBrowserWithTab(web_contents);" in HELPER

callers = (
    "cmux_browser_pane.cc",
    "cmux_extension_strip.cc",
    "cmux_extensions.cc",
    "cmux_extensions_container.cc",
    "cmux_toolbar_menus.cc",
)
for name in callers:
    source = (OVERLAY / name).read_text()
    assert '#include "chrome/browser/cmux_term/cmux_browser_finder.h"' in source, name
    assert "chrome::FindBrowserWithTab" not in source, name
    assert "cmux::FindBrowserWithTab" in source, name

for source in OVERLAY.glob("*.cc"):
    assert '#include "chrome/browser/ui/browser_finder.h"' not in source.read_text(), source

print("browser finder M149/M151 compatibility: PASS")
