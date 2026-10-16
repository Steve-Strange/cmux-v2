#!/usr/bin/env python3
"""Host-side tests for Linux target notice generation."""

from __future__ import annotations

import json
from pathlib import Path
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parent.parent


def run(*args: object, check: bool = True) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [str(value) for value in args],
        check=check,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )


def main() -> int:
    with tempfile.TemporaryDirectory() as raw_tmp:
        tmp = Path(raw_tmp)
        ghostty = tmp / "ghostty"
        cache = tmp / "cache"
        collected = tmp / "collected"
        (ghostty / "vendor/libxev").mkdir(parents=True)
        (ghostty / "LICENSE").write_text("Ghostty MIT\n", encoding="utf-8")
        (ghostty / "vendor/libxev/LICENSE").write_text(
            "libxev MIT\n", encoding="utf-8"
        )
        for index in range(4):
            package = cache / "p" / f"package-{index}"
            package.mkdir(parents=True)
            (package / ("COPYING" if index % 2 else "LICENSE.txt")).write_text(
                f"package {index} license\n", encoding="utf-8"
            )

        run(
            sys.executable,
            ROOT / "scripts/collect-ghostty-licenses.py",
            "--ghostty-source",
            ghostty,
            "--zig-cache",
            cache,
            "--output",
            collected,
            "--revision",
            "a" * 40,
        )
        manifest = json.loads(
            (collected / "SOURCE-MANIFEST.json").read_text(encoding="utf-8")
        )
        assert manifest["ghostty_revision"] == "a" * 40
        assert not manifest["unresolved_packages"]
        assert len(manifest["license_files"]) == 6

        unresolved = cache / "p/unresolved-package"
        unresolved.mkdir()
        rejected = run(
            sys.executable,
            ROOT / "scripts/collect-ghostty-licenses.py",
            "--ghostty-source",
            ghostty,
            "--zig-cache",
            cache,
            "--output",
            tmp / "rejected",
            "--revision",
            "a" * 40,
            check=False,
        )
        assert rejected.returncode != 0
        unresolved.rmdir()

        license_root = tmp / "license-input"
        (license_root / "ghostty").mkdir(parents=True)
        for name in (
            "cmux-GPL-3.0-or-later.txt",
            "source-provenance.md",
            "Bonsplit-LICENSE.txt",
            "Chromium-LICENSE.txt",
            "Ghostty-LICENSE.txt",
            "Helium-GPL-3.0-only.txt",
        ):
            (license_root / name).write_text(f"{name}\n", encoding="utf-8")
        credits = "<html><body>" + ("Chromium license text " * 7000) + "</body></html>"
        (license_root / "Chromium-about-credits.html").write_text(
            credits, encoding="utf-8"
        )
        for path in collected.iterdir():
            target = license_root / "ghostty" / path.name
            if path.is_dir():
                subprocess.run(["cp", "-R", path, target], check=True)
            else:
                target.write_bytes(path.read_bytes())

        output_html = tmp / "THIRD_PARTY_NOTICES.html"
        output_spdx = tmp / "third-party-notices.spdx.json"
        run(
            sys.executable,
            ROOT / "scripts/generate-linux-release-compliance.py",
            "--version",
            "151.0.7922.35",
            "--source-sha",
            "b" * 40,
            "--license-root",
            license_root,
            "--output-html",
            output_html,
            "--output-spdx",
            output_spdx,
        )
        html = output_html.read_text(encoding="utf-8")
        spdx = json.loads(output_spdx.read_text(encoding="utf-8"))
        assert "cmux-browser-source-151.0.7922.35.tar.zst" in html
        assert "combined cmux Browser work is GPL-3.0-only" in html
        assert "cmux material remains GPL-3.0-or-later" in html
        assert "Chromium-about-credits.html" in html
        assert "does not bundle the uBlock Origin CRX" in html
        assert "Ghostty CLI, Ghostty themes, or Ghostty shell-integration" in html
        assert "does contain compiled Ghostty terminfo" in html
        assert spdx["spdxVersion"] == "SPDX-2.3"
        packages = {
            package["name"]: package["licenseDeclared"]
            for package in spdx["packages"]
        }
        assert packages["cmux Browser"] == "GPL-3.0-only"
        assert packages["cmux-authored material"] == "GPL-3.0-or-later"
        assert packages["Helium-derived material"] == "GPL-3.0-only"
        assert len(spdx["packages"]) >= 7
        assert len(spdx["files"]) >= 8

    print("Linux release compliance tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
