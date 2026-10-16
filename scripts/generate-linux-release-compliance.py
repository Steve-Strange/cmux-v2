#!/usr/bin/env python3
"""Generate the Linux release's human-readable and SPDX notice inventories."""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import html
import json
from pathlib import Path
import re
import sys


def sha256(path: Path) -> str:
    value = hashlib.sha256()
    value.update(path.read_bytes())
    return value.hexdigest()


def spdx_id(prefix: str, value: str) -> str:
    digest = hashlib.sha256(value.encode("utf-8")).hexdigest()[:20]
    return f"SPDXRef-{prefix}-{digest}"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--version", required=True)
    parser.add_argument("--source-sha", required=True)
    parser.add_argument("--license-root", type=Path, required=True)
    parser.add_argument("--output-html", type=Path, required=True)
    parser.add_argument("--output-spdx", type=Path, required=True)
    args = parser.parse_args()

    root = args.license_root.resolve()
    credits = root / "Chromium-about-credits.html"
    ghostty_manifest_path = root / "ghostty/SOURCE-MANIFEST.json"
    if not credits.is_file() or not ghostty_manifest_path.is_file():
        parser.error("Chromium credits and Ghostty license manifest are required")
    credits_text = credits.read_text(encoding="utf-8", errors="replace")
    if (
        "To get correct credits page" in credits_text
        or len(credits_text) < 100_000
    ):
        parser.error("Chromium generated credits are missing or incomplete")
    ghostty_manifest = json.loads(ghostty_manifest_path.read_text())
    if ghostty_manifest.get("unresolved_packages"):
        parser.error("Ghostty license manifest contains unresolved packages")

    files = sorted(
        (
            path
            for path in root.rglob("*")
            if path.is_file() and path != ghostty_manifest_path
        ),
        key=lambda path: path.relative_to(root).as_posix(),
    )
    if len(files) < 8:
        parser.error(f"license inventory is unexpectedly small: {len(files)}")

    source_url = (
        "https://github.com/manaflow-ai/cmux-v2/releases/download/nightly/"
        f"cmux-browser-source-{args.version}.tar.zst"
    )
    page: list[str] = [
        "<!doctype html>",
        '<html lang="en"><head><meta charset="utf-8">',
        "<title>cmux Browser Linux third-party notices</title>",
        "<style>body{font:16px system-ui;max-width:78rem;margin:2rem auto;"
        "padding:0 1rem;line-height:1.45}pre{white-space:pre-wrap;"
        "overflow-wrap:anywhere;background:#f5f5f5;padding:1rem}"
        "summary{cursor:pointer;font-weight:600}code{overflow-wrap:anywhere}"
        "</style></head><body>",
        "<h1>cmux Browser Linux third-party notices</h1>",
        f"<p>Release <code>{html.escape(args.version)}</code>, source commit "
        f"<code>{html.escape(args.source_sha)}</code>.</p>",
        "<p>The combined cmux Browser work is GPL-3.0-only because it "
        "contains Helium-derived GPL-3.0-only material. Independently authored "
        "cmux material remains GPL-3.0-or-later. The exact "
        f'Corresponding Source is available <a href="{html.escape(source_url)}">'
        "beside this binary</a>. Third-party material remains under the terms "
        "reproduced below.</p>",
        "<p>This Linux artifact does not bundle the uBlock Origin CRX, the "
        "Ghostty CLI, Ghostty themes, or Ghostty shell-integration resources. "
        "It does contain compiled Ghostty terminfo, the linked Ghostty library, "
        "and its embedded fonts and static dependencies; their discovered "
        "license files are included below. Chromium's exact generated credits "
        "are included in full.</p>",
        "<h2>License files</h2>",
    ]
    for path in files:
        relative = path.relative_to(root).as_posix()
        content = path.read_text(encoding="utf-8", errors="replace")
        page.extend(
            [
                "<details>",
                f"<summary>{html.escape(relative)}</summary>",
                f"<pre>{html.escape(content)}</pre>",
                "</details>",
            ]
        )
    page.append("</body></html>\n")
    args.output_html.parent.mkdir(parents=True, exist_ok=True)
    args.output_html.write_text("\n".join(page), encoding="utf-8")

    packages: list[tuple[str, str]] = [
        ("cmux Browser", "GPL-3.0-only"),
        ("cmux-authored material", "GPL-3.0-or-later"),
        ("Chromium", "BSD-3-Clause"),
        ("Bonsplit-derived material", "MIT"),
        ("Ghostty core", "MIT"),
        ("Ghostty linked dependency bundle", "NOASSERTION"),
        ("Helium-derived material", "GPL-3.0-only"),
    ]
    ghostty_packages = sorted(
        {
            str(item["package"])
            for item in ghostty_manifest["license_files"]
            if item["source_kind"] != "ghostty"
        }
    )
    packages.extend(
        (f"Ghostty dependency: {package}", "NOASSERTION")
        for package in ghostty_packages
    )
    package_items = []
    relationships = []
    for name, license_id in packages:
        package_id = spdx_id("Package", name)
        package_items.append(
            {
                "SPDXID": package_id,
                "copyrightText": "NOASSERTION",
                "downloadLocation": "NOASSERTION",
                "filesAnalyzed": False,
                "licenseConcluded": license_id,
                "licenseDeclared": license_id,
                "name": name,
                "versionInfo": args.version if name == "cmux Browser" else None,
            }
        )
        relationships.append(
            {
                "spdxElementId": "SPDXRef-DOCUMENT",
                "relationshipType": "DESCRIBES",
                "relatedSpdxElement": package_id,
            }
        )
    for item in package_items:
        if item["versionInfo"] is None:
            del item["versionInfo"]

    file_items = []
    ghostty_package_ids = {
        package: spdx_id("Package", f"Ghostty dependency: {package}")
        for package in ghostty_packages
    }
    file_packages = {
        f"ghostty/{item['destination']}": str(item["package"])
        for item in ghostty_manifest["license_files"]
        if item["source_kind"] != "ghostty"
    }
    for path in files:
        relative = path.relative_to(root).as_posix()
        file_id = spdx_id("File", relative)
        file_items.append(
            {
                "SPDXID": file_id,
                "checksums": [
                    {"algorithm": "SHA256", "checksumValue": sha256(path)}
                ],
                "copyrightText": "NOASSERTION",
                "fileName": f"./{relative}",
                "licenseConcluded": "NOASSERTION",
                "licenseInfoInFiles": ["NOASSERTION"],
            }
        )
        package = file_packages.get(relative)
        if package is not None:
            relationships.append(
                {
                    "spdxElementId": ghostty_package_ids[package],
                    "relationshipType": "CONTAINS",
                    "relatedSpdxElement": file_id,
                }
            )

    namespace_sha = re.sub(r"[^A-Za-z0-9.-]", "-", args.source_sha)
    document = {
        "SPDXID": "SPDXRef-DOCUMENT",
        "creationInfo": {
            "created": datetime.now(timezone.utc)
            .replace(microsecond=0)
            .isoformat()
            .replace("+00:00", "Z"),
            "creators": [
                "Organization: Manaflow, Inc.",
                "Tool: generate-linux-release-compliance.py",
            ],
        },
        "dataLicense": "CC0-1.0",
        "documentNamespace": (
            "https://github.com/manaflow-ai/cmux-v2/spdx/"
            f"{args.version}/{namespace_sha}"
        ),
        "files": file_items,
        "name": f"cmux Browser Linux {args.version}",
        "packages": package_items,
        "relationships": relationships,
        "spdxVersion": "SPDX-2.3",
    }
    args.output_spdx.parent.mkdir(parents=True, exist_ok=True)
    args.output_spdx.write_text(
        json.dumps(document, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    print(
        f"generated Linux notices with {len(files)} license files: "
        f"{args.output_html}"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
