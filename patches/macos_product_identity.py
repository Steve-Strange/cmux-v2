#!/usr/bin/env python3
"""Apply cmux's compiled macOS product identity to a Chromium checkout."""

import os
import re
from pathlib import Path


DEFAULT_MAC_BUNDLE_ID = "com.cmux.app"
MAC_BUNDLE_ID = os.environ.get("CMUX_MAC_BUNDLE_ID", DEFAULT_MAC_BUNDLE_ID)
if not re.fullmatch(r"com\.cmux\.app(?:\.[A-Za-z0-9-]+)*", MAC_BUNDLE_ID):
    raise SystemExit(
        "CMUX_MAC_BUNDLE_ID must be com.cmux.app or a com.cmux.app.* channel ID"
    )


def replace_required(text: str, old: str, new: str, label: str) -> str:
    if new in text:
        return text
    if old not in text:
        raise AssertionError(f"{label}: expected source text not found")
    return text.replace(old, new, 1)


def write_if_changed(path: str, original: str, updated: str) -> None:
    if original != updated:
        Path(path).write_text(updated, encoding="utf-8")


def patch_branding() -> None:
    path = "chrome/app/theme/chromium/BRANDING"
    original = Path(path).read_text(encoding="utf-8")
    values = {
        "COMPANY_FULLNAME": "Manaflow, Inc.",
        "COMPANY_SHORTNAME": "Manaflow",
        "PRODUCT_FULLNAME": "cmux",
        "PRODUCT_SHORTNAME": "cmux",
        "PRODUCT_INSTALLER_FULLNAME": "cmux Installer",
        "PRODUCT_INSTALLER_SHORTNAME": "cmux Installer",
        "COPYRIGHT": "Copyright @LASTCHANGE_YEAR@ Manaflow, Inc. All rights reserved.",
        "MAC_BUNDLE_ID": MAC_BUNDLE_ID,
        "MAC_CREATOR_CODE": "Cmux",
        "MAC_TEAM_ID": "7WLXT3NR37",
    }
    lines = original.splitlines()
    found = set()
    for index, line in enumerate(lines):
        key, separator, _ = line.partition("=")
        if separator and key in values:
            lines[index] = f"{key}={values[key]}"
            found.add(key)
    missing = values.keys() - found
    if missing:
        raise AssertionError(f"BRANDING keys missing: {sorted(missing)}")
    write_if_changed(path, original, "\n".join(lines) + "\n")


def patch_info_plist_scheme() -> None:
    path = "build/apple/tweak_info_plist.py"
    original = Path(path).read_text(encoding="utf-8")
    chromium_branch = """  elif bundle_identifier == 'org.chromium.Chromium':
    scheme = 'chromium'
"""
    cmux_branch = f"""  elif bundle_identifier == '{MAC_BUNDLE_ID}':
    scheme = 'cmux'
"""
    existing_cmux_branch = re.compile(
        r"  elif bundle_identifier == 'com\.cmux\.app"
        r"(?:\.[A-Za-z0-9-]+)*':\n"
        r"    scheme = 'cmux'\n"
    )
    if existing_cmux_branch.search(original):
        updated = existing_cmux_branch.sub(cmux_branch, original, count=1)
    else:
        updated = replace_required(
            original,
            chromium_branch,
            cmux_branch + chromium_branch,
            "mac plist URL scheme",
        )
    write_if_changed(path, original, updated)


def patch_runtime_scheme() -> None:
    path = "chrome/browser/shell_integration_mac.mm"
    original = Path(path).read_text(encoding="utf-8")
    start = original.index("std::string GetDirectLaunchUrlScheme()")
    end = original.index("\n}\n\nnamespace internal", start)
    body = original[start:end]
    if 'return "cmux";' not in body:
        if 'return "chromium";' not in body:
            raise AssertionError("mac runtime direct-launch scheme anchor missing")
        body = body.replace('return "chromium";', 'return "cmux";', 1)
    write_if_changed(path, original, original[:start] + body + original[end:])


def main() -> None:
    patch_branding()
    patch_info_plist_scheme()
    patch_runtime_scheme()
    print(f"macos-product-identity: cmux ({MAC_BUNDLE_ID})")


if __name__ == "__main__":
    main()
