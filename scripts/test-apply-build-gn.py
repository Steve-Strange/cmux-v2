#!/usr/bin/env python3
"""Regression test for the M149-M151 chrome/browser/BUILD.gn patch."""

import os
from pathlib import Path
import tempfile


ROOT = Path(__file__).resolve().parent.parent
APPLY = (ROOT / "scripts/apply.sh").read_text()
# apply.sh is itself streamed through a shell command on remote builders.
# Quotes inside the embedded Python string must survive that outer shell.
assert r'\"cmux_term/cmux_term.cc\"' in APPLY
START = APPLY.index("# 2. chrome/browser/BUILD.gn: register cmux sources.")
END = APPLY.index("# 2b. A cmux workspace", START)
PATCH = "MODE = 'full'\nreg = 'cmux-reg'\n" + APPLY[START:END]
GHOSTTY_MIGRATION_START = APPLY.index(
    "# Linux and Windows compile the Ghostty theme callers", START
)
GHOSTTY_MIGRATION = (
    "p = 'chrome/browser/BUILD.gn'\n"
    "s = open(p).read()\n"
    + APPLY[GHOSTTY_MIGRATION_START:END]
)


def validate(name: str, first_source: str, include_early_mac_target: bool) -> None:
    early = ""
    if include_early_mac_target:
        early = """if (is_mac) {
  source_set("chrome_browser_main_mac") {
    public = [ "chrome_browser_main_mac.h" ]
    deps = [ "//chrome/browser/mac" ]
  }
}

"""
    build_gn = early + f"""static_library("browser") {{
  sources = []

  if (is_mac) {{
    sources += [
      "{first_source}",
      "app_controller_mac.mm",
      "chrome_browser_main_mac.h",
      "chrome_browser_main_mac.mm",
    ]

    deps += [
      "//chrome/browser/mac",
    ]
  }}
}}
"""

    with tempfile.TemporaryDirectory() as temp:
        checkout = Path(temp)
        browser = checkout / "chrome/browser"
        browser.mkdir(parents=True)
        path = browser / "BUILD.gn"
        path.write_text(build_gn)
        (browser / "chrome_browser_main.cc").write_text("cmux-reg")
        old_cwd = Path.cwd()
        try:
            os.chdir(checkout)
            exec(compile(PATCH, "apply.sh BUILD.gn patch", "exec"), {})
        finally:
            os.chdir(old_cwd)

        patched = path.read_text()
        assert patched.count("# cmux cross-platform") == 1
        assert patched.count("cmux_term/cmux_views.cc") == 1
        assert patched.count("cmux_term/cmux_views_mac.mm") == 1
        assert patched.count('"cmux_term/cmux_term.cc"') == 1
        assert patched.count('"cmux_term/cmux_term_mac.mm"') == 1
        assert patched.count('"cmux_term/cmux_theme_ghostty.cc"') == 1
        dependency = '"//chrome/browser/cmux_term:cmux_ghostty",'
        assert patched.count(dependency) == 1
        resource_dependency = (
            '"//chrome/services/cmux_terminal_renderer/public/cpp:'
            'ghostty_resources",'
        )
        assert patched.count(resource_dependency) == 1
        # A four-byte insertion-offset regression once placed the dependency
        # before the `+= [` suffix in M150, producing invalid GN:
        #   "//chrome/browser/cmux_term:cmux_ghostty",+= [
        assert dependency + "+= [" not in patched
        assert "    deps += [\n      " + dependency in patched

        browser_mac = patched.index(
            "  if (is_mac) {\n    sources += [", patched.index("# cmux cross-platform")
        )
        browser_mac_end = patched.index("\n  }", browser_mac)
        assert browser_mac < patched.index("cmux_term/cmux_views_mac.mm") < browser_mac_end
        assert browser_mac < patched.index("cmux_term/cmux_term_mac.mm") < browser_mac_end
        assert browser_mac < patched.index(dependency) < browser_mac_end

        cross_platform = patched.index("# cmux cross-platform")
        cross_platform_end = patched.index("\n  }", cross_platform)
        nonmac = patched.index("# cmux non-mac implementations")
        nonmac_end = patched.index("\n  }", nonmac)
        assert "cmux_term/cmux_term.cc" not in patched[cross_platform:cross_platform_end]
        assert "cmux_term/cmux_theme_ghostty.cc" not in patched[cross_platform:cross_platform_end]
        assert nonmac < patched.index("cmux_term/cmux_term.cc") < nonmac_end
        assert nonmac < patched.index("cmux_term/cmux_theme_ghostty.cc") < nonmac_end
        assert nonmac < patched.index(resource_dependency) < nonmac_end
        assert "cmux_term/cmux_term_mac.mm" not in patched[nonmac:nonmac_end]
        if include_early_mac_target:
            early_target = patched[: patched.index('static_library("browser")')]
            assert dependency not in early_target

        # Release builders retain their patched Chromium checkout between
        # runs. Recreate the pre-fix graph and prove a warm apply upgrades it
        # without duplicating any source or dependency.
        legacy_resource_block = (
            "    deps += [\n"
            f"      {resource_dependency}\n"
            "    ]\n"
        )
        assert legacy_resource_block in patched
        path.write_text(patched.replace(legacy_resource_block, "", 1))
        old_cwd = Path.cwd()
        try:
            os.chdir(checkout)
            exec(
                compile(
                    GHOSTTY_MIGRATION,
                    "apply.sh warm Ghostty dependency migration",
                    "exec",
                ),
                {},
            )
        finally:
            os.chdir(old_cwd)
        warm_patched = path.read_text()
        warm_nonmac = warm_patched.index("# cmux non-mac implementations")
        warm_nonmac_end = warm_patched.index("\n  }", warm_nonmac)
        assert warm_patched.count(resource_dependency) == 1
        assert (
            warm_nonmac
            < warm_patched.index(resource_dependency)
            < warm_nonmac_end
        )
        print(f"PASS {name}")


validate(
    "Chromium 149 BUILD.gn",
    "accessibility/caption_settings_dialog.h",
    include_early_mac_target=False,
)
validate(
    "Chromium 150 BUILD.gn",
    "app_controller_mac.mm",
    include_early_mac_target=True,
)
validate(
    "Chromium 151 BUILD.gn",
    "app_controller_mac.mm",
    include_early_mac_target=True,
)
