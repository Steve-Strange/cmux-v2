#!/usr/bin/env python3
"""Focused tests for release provenance generation."""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent.parent


def main() -> int:
    with tempfile.TemporaryDirectory() as raw_tmp:
        tmp = Path(raw_tmp)
        first = tmp / "b.zip"
        second = tmp / "a.dmg"
        first.write_bytes(b"second subject\n")
        second.write_bytes(b"first subject\n")
        output = tmp / "provenance.intoto.jsonl"
        sha = "1" * 40
        subprocess.run(
            [
                sys.executable,
                ROOT / "scripts/generate-release-provenance.py",
                "--version",
                "151.0.7922.35",
                "--channel",
                "nightly",
                "--release-tag",
                "nightly",
                "--source-repository",
                "manaflow-ai/cmux-browser",
                "--source-sha",
                sha,
                "--run-url",
                "https://github.com/manaflow-ai/cmux-browser/actions/runs/1",
                "--output",
                output,
                "--subject",
                first,
                "--subject",
                second,
            ],
            check=True,
        )
        statement = json.loads(output.read_text(encoding="utf-8"))
        assert statement["_type"] == "https://in-toto.io/Statement/v1"
        assert statement["predicateType"] == "https://slsa.dev/provenance/v1"
        assert [item["name"] for item in statement["subject"]] == [
            "a.dmg",
            "b.zip",
        ]
        assert statement["subject"][0]["digest"]["sha256"] == hashlib.sha256(
            second.read_bytes()
        ).hexdigest()
        assert (
            statement["predicate"]["buildDefinition"]["resolvedDependencies"][0][
                "digest"
            ]["gitCommit"]
            == sha
        )

    print("release provenance tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
