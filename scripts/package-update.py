#!/usr/bin/env python3
"""Create cmux's signed, deterministic update manifest.

Artifacts are passed as PLATFORM=ZIP,ARCHIVE_ROOT,EXECUTABLE. The payload is
canonical JSON and the outer envelope contains only base64(payload) and its
ECDSA-P256/SHA-256 signature. The browser verifies the signature before it
trusts any URL, path, size, or digest.
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
from pathlib import Path
import subprocess
import sys

SUPPORTED = {
    "mac-arm64",
    "mac-x64",
    "windows-arm64",
    "windows-x64",
    "linux-arm64",
    "linux-x64",
}


def parse_artifact(value: str) -> tuple[str, Path, str, str]:
    platform, separator, remainder = value.partition("=")
    parts = remainder.split(",", 2)
    if not separator or platform not in SUPPORTED or len(parts) != 3:
        raise argparse.ArgumentTypeError(
            "artifact must be PLATFORM=ZIP,ARCHIVE_ROOT,EXECUTABLE"
        )
    archive = Path(parts[0]).resolve()
    if not archive.is_file():
        raise argparse.ArgumentTypeError(f"artifact does not exist: {archive}")
    for label, path in (("archive root", parts[1]), ("executable", parts[2])):
        candidate = Path(path)
        if not path or candidate.is_absolute() or ".." in candidate.parts:
            raise argparse.ArgumentTypeError(f"unsafe {label}: {path!r}")
    return platform, archive, parts[1], parts[2]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--version", required=True)
    parser.add_argument("--base-url", required=True)
    parser.add_argument("--private-key", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--artifact", action="append", type=parse_artifact,
                        required=True)
    args = parser.parse_args()

    base_url = args.base_url.rstrip("/")
    platforms: dict[str, dict[str, str]] = {}
    for platform, archive, archive_root, executable in args.artifact:
        if platform in platforms:
            parser.error(f"duplicate artifact: {platform}")
        data = archive.read_bytes()
        platforms[platform] = {
            "archive_root": archive_root,
            "executable": executable,
            "sha256": hashlib.sha256(data).hexdigest(),
            # JSON strings avoid precision loss in parsers that store numbers
            # as IEEE doubles.
            "size": str(len(data)),
            "url": f"{base_url}/{archive.name}",
        }

    payload = json.dumps(
        {"platforms": platforms, "schema": 1, "version": args.version},
        sort_keys=True,
        separators=(",", ":"),
    ).encode("utf-8")
    if not args.private_key.is_file():
        parser.error(f"private key does not exist: {args.private_key}")
    signature = subprocess.run(
        ["openssl", "dgst", "-sha256", "-sign", str(args.private_key)],
        input=payload,
        stdout=subprocess.PIPE,
        check=True,
    ).stdout
    envelope = {
        "payload": base64.b64encode(payload).decode("ascii"),
        "signature": base64.b64encode(signature).decode("ascii"),
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(
        json.dumps(envelope, sort_keys=True, separators=(",", ":")) + "\n",
        encoding="utf-8",
    )
    print(f"wrote {args.output} ({len(platforms)} platform artifacts)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
