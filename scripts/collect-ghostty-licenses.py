#!/usr/bin/env python3
"""Collect license material for the exact Ghostty library dependency cache."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import sys
import urllib.request


LICENSE_NAME = re.compile(
    r"^(license|licence|copying|copyright|notice|authors|ofl|unlicense)"
    r"([._-].*)?$",
    re.IGNORECASE,
)
MAX_LICENSE_BYTES = 2 * 1024 * 1024
KNOWN_LICENSES = {
    "dear-bindings": {
        "url": (
            "https://raw.githubusercontent.com/dearimgui/dear_bindings/"
            "d27d0824350dcaa08c09b1e882ea1ce13f2eb90d/LICENSE.txt"
        ),
        "sha256": "b08fd9947ec127fd7785801cac8b1eea2ab4282008cdaae8e50331d870f30d0d",
        "filename": "DearBindings-LICENSE.txt",
    },
    "unicode-16": {
        "url": "https://www.unicode.org/license.txt",
        "sha256": "e7a93b009565cfce55919a381437ac4db883e9da2126fa28b91d12732bc53d96",
        "filename": "Unicode-LICENSE-3.0.txt",
    },
    "z2d": {
        "url": (
            "https://raw.githubusercontent.com/vancluever/z2d/"
            "6d1d7bda6b696c0941d204e6042f1e8ee900e001/LICENSE"
        ),
        "sha256": "3ca3ed7707db3f5561bff7c2eec62ee1c127947af928189572148f122917b474",
        "filename": "z2d-LICENSE.txt",
    },
}


def digest(path: Path) -> str:
    value = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            value.update(chunk)
    return value.hexdigest()


def license_files(root: Path) -> list[Path]:
    result: list[Path] = []
    for path in root.rglob("*"):
        if (
            path.is_file()
            and ".git" not in path.parts
            and LICENSE_NAME.match(path.name)
            and path.stat().st_size <= MAX_LICENSE_BYTES
        ):
            result.append(path)
    return sorted(result, key=lambda value: value.relative_to(root).as_posix())


def safe_component(value: str) -> str:
    cleaned = re.sub(r"[^A-Za-z0-9._-]+", "-", value).strip("-")
    return cleaned or "package"


def known_license_for(root: Path) -> tuple[str, list[Path]] | None:
    if (root / "UnicodeData.txt").is_file() and (root / "ReadMe.txt").is_file():
        readme = root / "ReadMe.txt"
        if digest(readme) != (
            "14cafa23788d3a20dd21d6b0cdcb8d6dab520781fcd9ad9392f3b88ea607e633"
        ):
            return None
        return "unicode-16", [readme]
    if (root / "dcimgui.cpp").is_file() and (root / "dcimgui.h").is_file():
        return "dear-bindings", []
    zon = root / "build.zig.zon"
    if zon.is_file():
        value = zon.read_text(encoding="utf-8", errors="replace")
        if ".name = .z2d" in value and "SPDX-License-Identifier: MPL-2.0" in value:
            return "z2d", [zon]
    return None


def download_known_license(name: str) -> tuple[bytes, str, str]:
    metadata = KNOWN_LICENSES[name]
    request = urllib.request.Request(
        str(metadata["url"]),
        headers={"User-Agent": "cmux-browser-release-license-collector/1"},
    )
    with urllib.request.urlopen(request, timeout=30) as response:
        content = response.read(MAX_LICENSE_BYTES + 1)
    if len(content) > MAX_LICENSE_BYTES:
        raise ValueError(f"known license is unexpectedly large: {name}")
    actual = hashlib.sha256(content).hexdigest()
    if actual != metadata["sha256"]:
        raise ValueError(
            f"known license digest changed for {name}: {actual} "
            f"(expected {metadata['sha256']})"
        )
    return content, str(metadata["filename"]), str(metadata["url"])


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--ghostty-source", type=Path, required=True)
    parser.add_argument("--zig-cache", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--revision", required=True)
    args = parser.parse_args()

    source = args.ghostty_source.resolve()
    package_root = (args.zig_cache.resolve() / "p")
    output = args.output.resolve()
    if not (source / "LICENSE").is_file():
        parser.error(f"Ghostty LICENSE is missing: {source / 'LICENSE'}")
    if not package_root.is_dir():
        parser.error(f"Zig package cache is missing: {package_root}")

    if output.exists():
        shutil.rmtree(output)
    output.mkdir(parents=True)

    entries: list[dict[str, object]] = []
    unresolved: list[str] = []
    roots: list[tuple[str, Path]] = [("ghostty-source", source)]
    roots.extend(
        (f"zig-package-{safe_component(package.name)}", package)
        for package in sorted(package_root.iterdir(), key=lambda value: value.name)
        if package.is_dir()
    )

    for label, root in roots:
        files = license_files(root)
        known_license = None
        if label.startswith("zig-package-") and not files:
            known_license = known_license_for(root)
            if known_license is None:
                unresolved.append(root.name)
                continue
            _, extra_files = known_license
            files.extend(extra_files)
        for path in files:
            relative = path.relative_to(root)
            destination = Path(label) / relative
            target = output / destination
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(path, target)
            entries.append(
                {
                    "bytes": target.stat().st_size,
                    "destination": destination.as_posix(),
                    "package": root.name,
                    "sha256": digest(target),
                    "source": relative.as_posix(),
                    "source_kind": (
                        "ghostty" if label == "ghostty-source" else "zig-cache"
                    ),
                }
            )
        if known_license is not None:
            known_name, _ = known_license
            try:
                content, filename, source_url = download_known_license(known_name)
            except (OSError, ValueError) as error:
                print(f"could not retrieve {known_name} license: {error}", file=sys.stderr)
                return 1
            destination = Path(label) / filename
            target = output / destination
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(content)
            entries.append(
                {
                    "bytes": len(content),
                    "destination": destination.as_posix(),
                    "package": root.name,
                    "sha256": digest(target),
                    "source": source_url,
                    "source_kind": "verified-upstream-license",
                }
            )

    manifest = {
        "schema": 1,
        "ghostty_revision": args.revision,
        "license_files": entries,
        "unresolved_packages": unresolved,
    }
    (output / "SOURCE-MANIFEST.json").write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    if unresolved:
        print(
            "Ghostty dependency packages without discoverable license files:\n  "
            + "\n  ".join(unresolved),
            file=sys.stderr,
        )
        return 1
    if len(entries) < 5:
        print(
            f"expected multiple Ghostty dependency licenses, found {len(entries)}",
            file=sys.stderr,
        )
        return 1
    print(f"collected {len(entries)} Ghostty license files into {output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
