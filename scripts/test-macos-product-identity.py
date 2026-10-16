#!/usr/bin/env python3
"""Fixture tests for the compiled macOS identity patch."""

import os
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
PATCH = ROOT / "patches" / "macos_product_identity.py"


class MacOSProductIdentityTest(unittest.TestCase):
    def setUp(self) -> None:
        self.tempdir = tempfile.TemporaryDirectory()
        self.checkout = Path(self.tempdir.name)

        branding = self.checkout / "chrome/app/theme/chromium/BRANDING"
        branding.parent.mkdir(parents=True)
        branding.write_text(
            "\n".join(
                [
                    "COMPANY_FULLNAME=The Chromium Authors",
                    "COMPANY_SHORTNAME=The Chromium Authors",
                    "PRODUCT_FULLNAME=Chromium",
                    "PRODUCT_SHORTNAME=Chromium",
                    "PRODUCT_INSTALLER_FULLNAME=Chromium Installer",
                    "PRODUCT_INSTALLER_SHORTNAME=Chromium Installer",
                    "COPYRIGHT=Copyright @LASTCHANGE_YEAR@ The Chromium Authors.",
                    "MAC_BUNDLE_ID=org.chromium.Chromium",
                    "MAC_CREATOR_CODE=Cr24",
                    "MAC_TEAM_ID=",
                ]
            )
            + "\n",
            encoding="utf-8",
        )

        tweak = self.checkout / "build/apple/tweak_info_plist.py"
        tweak.parent.mkdir(parents=True)
        tweak.write_text(
            "def scheme_for(bundle_identifier):\n"
            "  if bundle_identifier == 'com.google.Chrome':\n"
            "    scheme = 'googlechrome'\n"
            "  elif bundle_identifier == 'org.chromium.Chromium':\n"
            "    scheme = 'chromium'\n"
            "  return scheme\n",
            encoding="utf-8",
        )

        shell = self.checkout / "chrome/browser/shell_integration_mac.mm"
        shell.parent.mkdir(parents=True)
        shell.write_text(
            "std::string GetDirectLaunchUrlScheme() {\n"
            '  return "chromium";\n'
            "}\n\n"
            "namespace internal {\n",
            encoding="utf-8",
        )

    def tearDown(self) -> None:
        self.tempdir.cleanup()

    def run_patch(self, bundle_id: str = "com.cmux.app") -> None:
        env = os.environ.copy()
        env["CMUX_MAC_BUNDLE_ID"] = bundle_id
        subprocess.run(
            ["python3", str(PATCH)],
            cwd=self.checkout,
            env=env,
            check=True,
            capture_output=True,
            text=True,
        )

    def test_default_identity_and_idempotency(self) -> None:
        self.run_patch()
        first = {
            path: path.read_bytes()
            for path in (
                self.checkout / "chrome/app/theme/chromium/BRANDING",
                self.checkout / "build/apple/tweak_info_plist.py",
                self.checkout / "chrome/browser/shell_integration_mac.mm",
            )
        }
        self.run_patch()
        self.assertEqual(first, {path: path.read_bytes() for path in first})

        branding = first[self.checkout / "chrome/app/theme/chromium/BRANDING"]
        self.assertIn(b"PRODUCT_FULLNAME=cmux", branding)
        self.assertIn(b"MAC_BUNDLE_ID=com.cmux.app", branding)
        self.assertIn(b"MAC_TEAM_ID=7WLXT3NR37", branding)
        self.assertIn(
            b"elif bundle_identifier == 'com.cmux.app':",
            first[self.checkout / "build/apple/tweak_info_plist.py"],
        )
        self.assertIn(
            b'return "cmux";',
            first[self.checkout / "chrome/browser/shell_integration_mac.mm"],
        )

    def test_channel_identity_replaces_existing_cmux_branch(self) -> None:
        self.run_patch()
        self.run_patch("com.cmux.app.dogfood.build-cast")
        tweak = (
            self.checkout / "build/apple/tweak_info_plist.py"
        ).read_text(encoding="utf-8")
        self.assertIn(
            "elif bundle_identifier == 'com.cmux.app.dogfood.build-cast':",
            tweak,
        )
        self.assertEqual(tweak.count("scheme = 'cmux'"), 1)

    def test_rejects_non_cmux_bundle_id(self) -> None:
        env = os.environ.copy()
        env["CMUX_MAC_BUNDLE_ID"] = "com.example.browser"
        result = subprocess.run(
            ["python3", str(PATCH)],
            cwd=self.checkout,
            env=env,
            capture_output=True,
            text=True,
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("CMUX_MAC_BUNDLE_ID must be", result.stderr)


if __name__ == "__main__":
    unittest.main()
