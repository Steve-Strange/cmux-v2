#!/usr/bin/env python3
"""Select and revalidate the one public release safe for Linux nightly reuse."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
from typing import Any


LINUX_NIGHTLY_TITLE_RE = re.compile(
    r"cmux Browser Nightly [0-9]+(?:\.[0-9]+){3} — Linux x64"
)
OPAQUE_DRAFT_TAG_RE = re.compile(r"untagged-[0-9a-f]+")


class ReleaseStateError(RuntimeError):
    """The public release state is ambiguous or unsafe to mutate."""


def load_json(path: Path) -> Any:
    return json.loads(path.read_text(encoding="utf-8"))


def is_linux_nightly_title(value: Any) -> bool:
    return LINUX_NIGHTLY_TITLE_RE.fullmatch(str(value or "")) is not None


def select_release(pages: Any) -> int | None:
    if not isinstance(pages, list) or not all(
        isinstance(page, list) for page in pages
    ):
        raise ReleaseStateError("release listing has an unexpected shape")
    releases = [release for page in pages for release in page]
    if not all(isinstance(release, dict) for release in releases):
        raise ReleaseStateError("release listing contains an invalid release")
    tagged = [
        release for release in releases if release.get("tag_name") == "nightly"
    ]
    if len(tagged) > 1:
        raise ReleaseStateError("multiple releases unexpectedly use the nightly tag")
    if tagged:
        value = tagged[0].get("id")
        if not isinstance(value, int) or value <= 0:
            raise ReleaseStateError("nightly release has an invalid ID")
        return value
    stale = [
        release
        for release in releases
        if release.get("draft") is True
        and release.get("prerelease") is True
        and not release.get("assets")
        and is_linux_nightly_title(release.get("name"))
        and OPAQUE_DRAFT_TAG_RE.fullmatch(
            str(release.get("tag_name", ""))
        )
    ]
    if len(stale) > 1:
        raise ReleaseStateError(
            "multiple empty Linux nightly drafts need operator review"
        )
    if not stale:
        return None
    value = stale[0].get("id")
    if not isinstance(value, int) or value <= 0:
        raise ReleaseStateError("stale Linux nightly draft has an invalid ID")
    return value


def revalidate_selected(release: Any, expected_id: int) -> None:
    if not isinstance(release, dict):
        raise ReleaseStateError("selected release is not an object")
    if release.get("id") != expected_id:
        raise ReleaseStateError("selected release changed identity")
    tag = release.get("tag_name")
    if tag == "nightly":
        if release.get("prerelease") is not True:
            raise ReleaseStateError(
                "the existing nightly tag is not a prerelease"
            )
        if not is_linux_nightly_title(release.get("name")):
            raise ReleaseStateError(
                "the existing nightly tag is not a Linux rolling nightly"
            )
        return
    if not (
        release.get("draft") is True
        and release.get("prerelease") is True
        and not release.get("assets")
        and is_linux_nightly_title(release.get("name"))
        and OPAQUE_DRAFT_TAG_RE.fullmatch(str(tag or ""))
    ):
        raise ReleaseStateError("stale nightly draft changed before normalization")


def main() -> int:
    parser = argparse.ArgumentParser()
    subparsers = parser.add_subparsers(dest="command", required=True)
    select = subparsers.add_parser("select")
    select.add_argument("--releases", type=Path, required=True)
    select.add_argument("--output", type=Path, required=True)
    revalidate = subparsers.add_parser("revalidate")
    revalidate.add_argument("--release", type=Path, required=True)
    revalidate.add_argument("--release-id", type=int, required=True)
    args = parser.parse_args()
    try:
        if args.command == "select":
            release_id = select_release(load_json(args.releases))
            args.output.write_text(
                json.dumps({"id": release_id}) + "\n", encoding="utf-8"
            )
        else:
            if args.release_id <= 0:
                raise ReleaseStateError("selected release ID must be positive")
            revalidate_selected(load_json(args.release), args.release_id)
    except (OSError, UnicodeDecodeError, json.JSONDecodeError,
            ReleaseStateError) as error:
        parser.error(str(error))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
