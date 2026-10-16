#!/usr/bin/env python3
"""Exercise macOS and Linux Ghostty link patch states."""

import json
import os
from pathlib import Path
import tempfile


ROOT = Path(__file__).resolve().parent.parent
APPLY = (ROOT / "scripts/apply.sh").read_text()
START = APPLY.index("# 3b. chrome/BUILD.gn:")
END = APPLY.index("# 3c. Register", START)
PATCH = APPLY[START:END]

COMPONENT_PRISTINE = """    if (is_component_build) {
      frameworks = [ "Carbon.framework" ]
    }

    ldflags = [ "-ObjC" ]
"""

COMPONENT_LEGACY = """    if (is_component_build) {
      frameworks = [ "Carbon.framework" ]
    }

# cmux ghostty chrome_dll link
    if (!is_component_build) {
      frameworks = []
    }
    libs = [
      rebase_path("//third_party/cmux_ghostty/lib/ghostty-internal.a"),
    ]
    frameworks += [
      "Metal.framework",
      "MetalKit.framework",
      "QuartzCore.framework",
      "CoreText.framework",
      "CoreGraphics.framework",
      "IOSurface.framework",
      "AppKit.framework",
      "GameController.framework",
    ]

    ldflags = [ "-ObjC", "-lc++" ]
"""

STATIC_PRISTINE = """    ldflags = [
      "-compatibility_version",
      chrome_dylib_version,
      "-current_version",
      chrome_dylib_version,
    ]
"""

BUILD_TEMPLATE = """shared_library("chrome_dll") {{
{component}
    configs += [
      ":chrome_dll_symbol_order",
    ]
  }}

  mac_framework_bundle("chrome_framework") {{
{static}
    if (!is_component_build) {{
      ldflags += [ "-install-name" ]
    }}
  }}
"""

LINUX_PRISTINE = """group("chrome") {
  public_deps = [ ":chrome_initial" ]
}

executable("chrome_initial") {
  sources = [ "app/chrome_exe_resource.h" ]

    if (is_linux) {
      sources += [
        "app/chrome_main_linux.cc",
        "app/chrome_main_linux.h",
      ]
    }
}
"""

LINUX_DISTRO_VERSIONS = {
    "Debian 11 (Bullseye)": "1.3.2-1",
    "Debian 12 (Bookworm)": "1.6.0-1",
    "Ubuntu 18.04 (Bionic)": "1.0.0-2ubuntu2.3",
    "Ubuntu 20.04 (Focal)": "1.3.2-1~ubuntu0.20.04.2",
    "Ubuntu 22.04 (Jammy)": "1.4.0-1",
}


def apply_patch(source: str, mode: str = "full") -> str:
    with tempfile.TemporaryDirectory() as temp:
        checkout = Path(temp)
        path = checkout / "chrome/BUILD.gn"
        path.parent.mkdir(parents=True)
        path.write_text(source)
        distro_versions_path = (
            checkout
            / "chrome/installer/linux/debian/dist_package_versions.json"
        )
        if mode == "chrome_linux":
            distro_versions_path.parent.mkdir(parents=True)
            distro_versions_path.write_text(
                json.dumps(
                    {distro: {} for distro in LINUX_DISTRO_VERSIONS},
                    indent=4,
                    sort_keys=True,
                )
                + "\n"
            )
        old_cwd = Path.cwd()
        try:
            os.chdir(checkout)
            source_patch = f"import json\nMODE = {mode!r}\n" + PATCH
            exec(
                compile(source_patch, "apply.sh Ghostty link patch", "exec"),
                {},
            )
        finally:
            os.chdir(old_cwd)
        if mode == "chrome_linux":
            distro_versions = json.loads(distro_versions_path.read_text())
            assert {
                distro: packages["libegl1"]
                for distro, packages in distro_versions.items()
            } == LINUX_DISTRO_VERSIONS
        return path.read_text()


def assert_current(source: str) -> None:
    assert source.count("# cmux Ghostty component link") == 1
    assert "# cmux ghostty chrome_dll link" not in source
    assert source.count("cmux_ghostty/lib/ghostty-internal.a") == 2
    assert source.count('"GameController.framework"') == 2


pristine = BUILD_TEMPLATE.format(
    component=COMPONENT_PRISTINE, static=STATIC_PRISTINE
)
current = apply_patch(pristine)
assert_current(current)
assert apply_patch(current) == current
print("PASS pristine/current Ghostty link patch")

legacy = BUILD_TEMPLATE.format(
    component=COMPONENT_LEGACY, static=STATIC_PRISTINE
)
migrated = apply_patch(legacy)
assert_current(migrated)
assert apply_patch(migrated) == migrated
print("PASS legacy-warm Ghostty link migration")

linux_current = apply_patch(LINUX_PRISTINE, "chrome_linux")
assert linux_current.count("# cmux Linux Ghostty executable link") == 1
assert linux_current.count("cmux_ghostty/lib/ghostty-internal.a") == 1
assert "      libs = [" in linux_current
assert "      libs += [" not in linux_current
linux_link_start = linux_current.index("# cmux Linux Ghostty executable link")
linux_link_end = linux_current.index("\n    }\n", linux_link_start)
linux_link = linux_current[linux_link_start:linux_link_end]
assert '"//third_party/cmux_ghostty/lib/ghostty-internal.a"' in linux_link
assert "rebase_path(" not in linux_link
for library in ("EGL", "GL", "dl", "fontconfig", "util"):
    assert f'"{library}"' in linux_current
assert apply_patch(linux_current, "chrome_linux") == linux_current

browser_linux_start = APPLY.index("    linux_block = (")
browser_linux_end = APPLY.index("    # Windows-only:", browser_linux_start)
assert "    libs += [" not in APPLY[browser_linux_start:browser_linux_end]
print("PASS Linux final-executable Ghostty link")
