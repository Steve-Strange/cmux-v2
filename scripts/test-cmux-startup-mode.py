#!/usr/bin/env python3
"""Static contracts for cmux's startup-mode and profile watcher lifecycles."""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
SOURCES = (
    ROOT / "overlay/chrome/browser/cmux_term/cmux_term.cc",
    ROOT / "overlay/chrome/browser/cmux_term/cmux_term_mac.mm",
)
EXTENSIONS_SOURCE = ROOT / "overlay/chrome/browser/cmux_term/cmux_extensions.cc"


def function_body(source: str, name: str) -> str:
    match = re.search(
        rf"\b(?:bool|void)\s+[^{{;]*\b{re.escape(name)}\([^)]*\)\s*\{{"
        r"(?P<body>.*?)\n\}",
        source,
        re.DOTALL,
    )
    if not match:
        raise AssertionError(f"could not find {name}")
    return match.group("body")


def override_method_body(source: str, name: str) -> str:
    match = re.search(
        rf"\bvoid\s+{re.escape(name)}\([^)]*\)\s+override\s*\{{"
        r"(?P<body>.*?)\n  \}",
        source,
        re.DOTALL,
    )
    if not match:
        raise AssertionError(f"could not find override method {name}")
    return match.group("body")


for path in SOURCES:
    source = path.read_text(encoding="utf-8")
    assert '#include "ui/gfx/switches.h"' in source, path
    enabled = function_body(source, "CmuxViewsEnabled")
    headless = "HasSwitch(switches::kHeadless)"
    assert headless in enabled, path
    assert enabled.index(headless) < enabled.index("CMUX_VIEWS"), path

    post_profile_init = function_body(source, "PostProfileInit")
    register_extensions = "RegisterProfileExtensions(profile)"
    headless_check = (
        "base::CommandLine::ForCurrentProcess()->"
        "HasSwitch(switches::kHeadless)"
    )
    headless_return = re.search(
        rf"if\s*\(\s*{re.escape(headless_check)}\s*\)\s*\{{\s*return;\s*\}}",
        post_profile_init,
    )
    assert headless_return, path
    assert register_extensions in post_profile_init, path
    assert headless_return.start() < post_profile_init.index(
        register_extensions
    ), path

mac_source = SOURCES[1].read_text(encoding="utf-8")
post_browser_start = function_body(mac_source, "PostBrowserStart")
assert post_browser_start.index("if (!CmuxViewsEnabled())") < (
    post_browser_start.index("StartAppIconController()")
)

extensions_source = EXTENSIONS_SOURCE.read_text(encoding="utf-8")
watcher_shutdown = override_method_body(extensions_source, "OnShutdown")
observation_reset = "registry_observation_.Reset()"
profile_release = "profile_ = nullptr"
assert observation_reset in watcher_shutdown
assert profile_release in watcher_shutdown
assert watcher_shutdown.index(observation_reset) < watcher_shutdown.index(
    profile_release
)

watch_profile = function_body(extensions_source, "WatchBitwardenExternalInstall")
same_profile = "existing->second->IsWatchingProfile(profile)"
replace_watcher = (
    "existing->second =\n"
    "        std::make_unique<BitwardenExternalInstallWatcher>(profile)"
)
assert same_profile in watch_profile
assert replace_watcher in watch_profile
assert watch_profile.index(same_profile) < watch_profile.index(replace_watcher)

print("cmux startup mode and extension lifecycle tests passed")
