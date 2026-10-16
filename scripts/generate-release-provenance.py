#!/usr/bin/env python3
"""Generate an in-toto/SLSA v1 statement for cmux release assets."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        while chunk := source.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--version", required=True)
    parser.add_argument("--channel", choices=("stable", "nightly"), required=True)
    parser.add_argument("--release-tag", required=True)
    parser.add_argument("--source-repository", required=True)
    parser.add_argument("--source-sha", required=True)
    parser.add_argument("--run-url", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--subject", action="append", type=Path, required=True)
    args = parser.parse_args()

    subjects: list[dict[str, object]] = []
    seen: set[str] = set()
    for path in args.subject:
        if not path.is_file():
            parser.error(f"subject does not exist: {path}")
        name = path.name
        if name in seen:
            parser.error(f"duplicate subject basename: {name}")
        seen.add(name)
        subjects.append({"name": name, "digest": {"sha256": sha256(path)}})
    subjects.sort(key=lambda item: str(item["name"]))

    workflow_uri = (
        f"https://github.com/{args.source_repository}"
        f"/.github/workflows/release-build.yml@{args.source_sha}"
    )
    statement = {
        "_type": "https://in-toto.io/Statement/v1",
        "subject": subjects,
        "predicateType": "https://slsa.dev/provenance/v1",
        "predicate": {
            "buildDefinition": {
                "buildType": workflow_uri,
                "externalParameters": {
                    "channel": args.channel,
                    "releaseTag": args.release_tag,
                    "version": args.version,
                },
                "internalParameters": {},
                "resolvedDependencies": [
                    {
                        "uri": (
                            f"git+https://github.com/{args.source_repository}"
                            f"@{args.source_sha}"
                        ),
                        "digest": {"gitCommit": args.source_sha},
                    }
                ],
            },
            "runDetails": {
                "builder": {"id": "https://github.com/actions/runner"},
                "metadata": {"invocationId": args.run_url},
            },
        },
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(
        json.dumps(statement, sort_keys=True, separators=(",", ":")) + "\n",
        encoding="utf-8",
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
