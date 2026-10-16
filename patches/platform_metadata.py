#!/usr/bin/env python3
"""Apply cmux application identity metadata to a Chromium checkout.

Run from the Chromium source root. The edits cover the macOS bundle identity,
Windows installer/shell registration identity, and Linux desktop/package
identity. They are intentionally kept together so the three platforms cannot
silently drift back to Chromium defaults.
"""

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
    updated = "\n".join(lines) + "\n"
    write_if_changed(path, original, updated)


def patch_macos() -> None:
    path = "build/apple/tweak_info_plist.py"
    original = Path(path).read_text(encoding="utf-8")
    chromium_branch = """  elif bundle_identifier == 'org.chromium.Chromium':
    scheme = 'chromium'
"""
    cmux_branch = f"""  elif bundle_identifier == '{MAC_BUNDLE_ID}':
    scheme = 'cmux'
"""
    existing_cmux_branch = re.compile(
        r"  elif bundle_identifier == 'com\.cmux(?:term)?\.app(?:\.[A-Za-z0-9-]+)*':\n"
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

    path = "chrome/browser/shell_integration_mac.mm"
    original = Path(path).read_text(encoding="utf-8")
    start = original.index("std::string GetDirectLaunchUrlScheme()")
    end = original.index("\n}\n\nnamespace internal", start)
    body = original[start:end]
    if 'return "cmux";' not in body:
        if 'return "chromium";' not in body:
            raise AssertionError("mac runtime direct-launch scheme anchor missing")
        body = body.replace('return "chromium";', 'return "cmux";', 1)
    updated = original[:start] + body + original[end:]
    write_if_changed(path, original, updated)


def patch_windows() -> None:
    path = "chrome/install_static/chromium_install_modes.h"
    original = Path(path).read_text(encoding="utf-8")
    updated = original
    if 'browser_prog_id_prefix = L"CmuxHTM"' not in updated:
        replacements = [
            ('kCompanyPathName[] = L""', 'kCompanyPathName[] = L"Manaflow"'),
            ('kProductPathName[] = L"Chromium"', 'kProductPathName[] = L"cmux"'),
            ('kSafeBrowsingName[] = "chromium"', 'kSafeBrowsingName[] = "cmux"'),
            ('L"",  // Empty app_guid since no integration with Google Update.',
             'L"{7A5BCB55-0095-44C1-ADE9-C12A0B63E80E}",  // cmux application GUID.'),
            ('base_app_name = L"Chromium"', 'base_app_name = L"cmux"'),
            ('base_app_id = L"Chromium"', 'base_app_id = L"Cmux"'),
            ('browser_prog_id_prefix = L"ChromiumHTM"',
             'browser_prog_id_prefix = L"CmuxHTM"'),
            ('L"Chromium HTML Document"', 'L"cmux HTML Document"'),
            ('direct_launch_url_scheme = "chromium"',
             'direct_launch_url_scheme = "cmux"'),
            ('pdf_prog_id_prefix = L"ChromiumPDF"',
             'pdf_prog_id_prefix = L"CmuxPDF"'),
            ('L"Chromium PDF Document"', 'L"cmux PDF Document"'),
            ('{7D2B3E1D-D096-4594-9D8F-A6667F12E0AC}',
             '{CED80F6C-CEBC-436D-A92E-B59760950D1D}'),
            ('{A2DF06F9-A21A-44A8-8A99-8B9C84F29160}',
             '{F411487F-4D04-4521-A027-3EADD3DBC57A}'),
            (""".toast_activator_clsid = {0x635EFA6F,
                                  0x08D6,
                                  0x4EC9,
                                  {0xBD, 0x14, 0x8A, 0x0F, 0xDE, 0x97, 0x51,
                                   0x59}}""",
             """.toast_activator_clsid = {0xC5017873,
                                  0x85DF,
                                  0x414F,
                                  {0x80, 0xA3, 0x72, 0x6F, 0x7E, 0xF2, 0x70,
                                   0x4D}}"""),
            (""".elevator_clsid = {0xD133B120,
                           0x6DB4,
                           0x4D6B,
                           {0x8B, 0xFE, 0x83, 0xBF, 0x8C, 0xA1, 0xB1,
                            0xB0}}""",
             """.elevator_clsid = {0xF15D065E,
                           0xE436,
                           0x4774,
                           {0xAA, 0x1D, 0x61, 0xC4, 0x79, 0xFF, 0xA9,
                            0xB3}}"""),
            (""".elevator_iid = {0xbb19a0e5,
                         0xc6,
                         0x4966,
                         {0x94, 0xb2, 0x5a, 0xfe, 0xc6, 0xfe, 0xd9,
                          0x3a}}""",
             """.elevator_iid = {0x11CC97B1,
                         0x611D,
                         0x455F,
                         {0x99, 0x77, 0x42, 0x61, 0x28, 0xE5, 0x17,
                          0xCB}}"""),
            ('{BB19A0E5-00C6-4966-94B2-5AFEC6FED93A}',
             '{11CC97B1-611D-455F-9977-426128E517CB}'),
            (""".tracing_service_clsid = {0x83f69367,
                                  0x442d,
                                  0x447f,
                                  {0x8b, 0xcc, 0x0e, 0x3f, 0x97, 0xbe, 0x9c,
                                   0xf2}}""",
             """.tracing_service_clsid = {0x0FD4B38A,
                                  0x5464,
                                  0x4E16,
                                  {0x99, 0xD8, 0xBA, 0xBB, 0xD5, 0x0A, 0x2F,
                                   0xB5}}"""),
            (""".tracing_service_iid = {0xa3fd580a,
                                0xffd4,
                                0x4075,
                                {0x91, 0x74, 0x75, 0xd0, 0xb1, 0x99, 0xd3,
                                 0xcb}}""",
             """.tracing_service_iid = {0x31AEE30F,
                                0x2259,
                                0x4A34,
                                {0x9F, 0x87, 0x29, 0x6E, 0xCB, 0xB2, 0xAE,
                                 0x2F}}"""),
            ('L"924012148-"', 'L"924012154-"'),
        ]
        for old, new in replacements:
            # Chromium 151 removed the legacy app-container profile GUID from
            # InstallConstants. Older supported trees still carry it and need
            # the cmux-specific value; its absence on M151 is intentional.
            if (old == '{A2DF06F9-A21A-44A8-8A99-8B9C84F29160}' and
                    old not in updated and new not in updated):
                continue
            updated = replace_required(updated, old, new, f"Windows identity {old}")
    write_if_changed(path, original, updated)

    policy_files = {
        "chrome/installer/setup/installer_crash_reporter_client.cc": [
            ('L"SOFTWARE\\\\Policies\\\\Chromium"',
             'L"SOFTWARE\\\\Policies\\\\cmux"'),
        ],
        "components/policy/tools/generate_policy_source.py": [
            ("CHROMIUM_POLICY_KEY = 'SOFTWARE\\\\\\\\Policies\\\\\\\\Chromium'",
             "CHROMIUM_POLICY_KEY = 'SOFTWARE\\\\\\\\Policies\\\\\\\\cmux'"),
        ],
    }
    for path, replacements in policy_files.items():
        original = Path(path).read_text(encoding="utf-8")
        updated = original
        for old, new in replacements:
            updated = replace_required(updated, old, new, f"Windows policy {path}")
        write_if_changed(path, original, updated)


def patch_linux() -> None:
    path = "chrome/common/channel_info_posix.cc"
    original = Path(path).read_text(encoding="utf-8")
    updated = replace_required(
        original,
        'return "chromium-browser.desktop";',
        'return "cmux-browser.desktop";',
        "Linux desktop ID",
    )
    write_if_changed(path, original, updated)

    path = "chrome/browser/shell_integration_linux.cc"
    original = Path(path).read_text(encoding="utf-8")
    updated = replace_required(
        original,
        'return "chromium-browser";',
        'return "cmux-browser";',
        "Linux icon name",
    )
    write_if_changed(path, original, updated)

    path = "chrome/common/chrome_paths_linux.cc"
    original = Path(path).read_text(encoding="utf-8")
    old = """#else
  std::string data_dir_basename = "chromium";
#endif
"""
    new = """#else
  std::string data_dir_basename = "cmux";
#endif
"""
    updated = replace_required(original, old, new, "Linux profile directory")
    write_if_changed(path, original, updated)

    path = "chrome/installer/linux/common/installer.py"
    original = Path(path).read_text(encoding="utf-8")
    updated = replace_required(
        original,
        'self.uri_scheme = "x-scheme-handler/chromium;"',
        'self.uri_scheme = "x-scheme-handler/cmux;"',
        "Linux URL scheme",
    )
    write_if_changed(path, original, updated)

    path = "chrome/installer/linux/common/chromium-browser.info"
    original = Path(path).read_text(encoding="utf-8")
    # Migrate warm builder trees patched with the retired Swift-era product
    # namespace before applying the normal idempotent upstream replacements.
    updated = original.replace(
        'RDN="com.cmuxterm.app"', 'RDN="com.cmux.app"', 1
    )
    replacements = [
        ('PACKAGE="chromium-browser"', 'PACKAGE="cmux-browser"'),
        ('INSTALLDIR=/opt/chromium.org/chromium', 'INSTALLDIR=/opt/cmux/browser'),
        ('ENROLLMENTDIR=/etc/chromium/policies/enrollment',
         'ENROLLMENTDIR=/etc/cmux/policies/enrollment'),
        ('MENUNAME="Chromium Web Browser"', 'MENUNAME="cmux"'),
        ('SHORTDESC="The web browser from the Chromium projects"',
         'SHORTDESC="The browser and terminal workspace from Manaflow"'),
        ('FULLDESC="Chromium is a browser that combines a minimal design with sophisticated technology to make the web faster, safer, and easier."',
         'FULLDESC="cmux combines a Chromium browser and Ghostty terminal surfaces in one workspace."'),
        ('MAINTNAME="Chromium Linux Team"', 'MAINTNAME="Manaflow, Inc."'),
        ('MAINTMAIL="chromium-packagers@chromium.org"', 'MAINTMAIL="support@cmux.com"'),
        ('PRODUCTURL="https://www.chromium.org/Home"', 'PRODUCTURL="https://cmux.com"'),
        ('DEVELOPER_NAME="The Chromium Authors"', 'DEVELOPER_NAME="Manaflow, Inc."'),
        ('BUGTRACKERURL="https://www.chromium.org/for-testers/bug-reporting-guidelines"',
         'BUGTRACKERURL="https://github.com/manaflow-ai/cmux-browser/issues"'),
        ('HELPURL="https://chromium.googlesource.com/chromium/src/+/main/docs/linux/debugging.md"',
         'HELPURL="https://github.com/manaflow-ai/cmux-browser"'),
        ('RDN="org.chromium.Chromium"', 'RDN="com.cmux.app"'),
    ]
    for old, new in replacements:
        updated = replace_required(updated, old, new, f"Linux package {old}")
    write_if_changed(path, original, updated)


def main() -> None:
    patch_branding()
    patch_macos()
    patch_windows()
    patch_linux()
    print("platform-metadata: cmux identity applied for macOS, Windows, and Linux")


if __name__ == "__main__":
    main()
