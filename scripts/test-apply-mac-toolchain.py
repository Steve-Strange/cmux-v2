#!/usr/bin/env python3
"""Regression test for checkout-relative system Xcode tool discovery."""

import os
from pathlib import Path
import tempfile


ROOT = Path(__file__).resolve().parent.parent
APPLY = (ROOT / "scripts/apply.sh").read_text()
START = APPLY.index("# 2c. The AWS scratch mount")
END = APPLY.index("# 3b. chrome/BUILD.gn", START)
PATCH = APPLY[START:END]

UPSTREAM = """if (use_system_xcode) {
  mac_bin_path = find_sdk_lines[1]
} else {
  mac_bin_path = _dev + "/Toolchains/XcodeDefault.xctoolchain/usr/bin/"
  mac_bin_path = rebase_path(mac_bin_path, root_build_dir)
}
"""

with tempfile.TemporaryDirectory() as temp:
    checkout = Path(temp)
    path = checkout / "build/config/mac/mac_sdk.gni"
    path.parent.mkdir(parents=True)
    path.write_text(UPSTREAM)
    old_cwd = Path.cwd()
    try:
        os.chdir(checkout)
        for _ in range(2):
            exec(compile(PATCH, "apply.sh mac toolchain patch", "exec"), {})
    finally:
        os.chdir(old_cwd)

    patched = path.read_text()
    assert patched.count("# cmux: checkout-relative system Xcode tools") == 1
    assert "}\n\n# cmux: checkout-relative system Xcode tools" in patched
    assert 'mac_sdk_path == "//build/mac_files/system_sdk"' in patched
    assert "//build/mac_files/system_xcode/Contents/Developer/Toolchains/" in patched
    assert "XcodeDefault.xctoolchain/usr/bin/" in patched
    override = patched.split("# cmux: checkout-relative system Xcode tools", 1)[1]
    assert "root_build_dir" not in override

print("PASS checkout-relative system Xcode tools")
