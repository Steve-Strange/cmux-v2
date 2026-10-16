#!/usr/bin/env python3
"""Focused positive and fail-closed tests for Linux nightly publication."""

from __future__ import annotations

import hashlib
import importlib.util
import io
import json
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile
import zipfile

from linux_nightly_release_state import (
    ReleaseStateError,
    revalidate_selected,
    select_release,
)


ROOT = Path(__file__).resolve().parent.parent
VERSION = "151.0.7922.59"
SOURCE_SHA = "a" * 40
SMOKE_WORKFLOW_SHA = "b" * 40
PUBLISHER_SHA = "c" * 40
SOURCE_RUN_ID = 101
SMOKE_RUN_ID = 202
FIXTURE_REQUIRED_SOURCE_FILES = {
    ".chromium-version": b"151.0.7922.34\n",
    "LICENSE": b"GPL-3.0-only\n",
    "THIRD_PARTY_NOTICES.md": b"Chromium and Ghostty notices\n",
    "docs/releases.md": b"# Rebuilding cmux Browser\n",
    "overlay/chrome/browser/cmux_term/BUILD.gn": b"# cmux sources\n",
    "patches/cmux-pinned-toolbar-actions-chromium-151.patch": b"cmux patch\n",
    "patches/helium-media-toolbar.patch": b"helium patch\n",
    "patches/helium-new-tab.patch": b"helium patch\n",
    "patches/helium-omnibar-chromium-151.patch": b"helium patch\n",
    "patches/helium-settings-chromium-151.patch": b"helium patch\n",
    "scripts/apply.sh": b"#!/bin/bash\n",
    "scripts/bootstrap-release-linux.sh": b"#!/bin/bash\n",
    "scripts/build-release-linux.sh": b"#!/bin/bash\n",
    "scripts/build-release-source.sh": b"#!/bin/bash\n",
    "scripts/vendor-ghostty-linux-local.sh": b"#!/bin/bash\n",
}


def write_json(path: Path, value: object) -> None:
    path.write_text(
        json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )


def add_tar_file(archive: tarfile.TarFile, name: str, data: bytes) -> None:
    info = tarfile.TarInfo(name)
    info.size = len(data)
    info.mode = 0o644
    archive.addfile(info, io.BytesIO(data))


def write_source_archive(
    root: Path,
    bundle: Path,
    receipt: dict[str, object],
    *,
    comment: str | None = SOURCE_SHA,
    omit: set[str] | None = None,
    filler_count: int = 320,
) -> Path:
    omitted = omit or set()
    raw_tar = root / "source.tar"
    prefix = f"cmux-browser-source-{VERSION}/"
    files = dict(FIXTURE_REQUIRED_SOURCE_FILES)
    files["SOURCE-MANIFEST.json"] = (
        json.dumps(receipt, indent=2, sort_keys=True) + "\n"
    ).encode()
    options: dict[str, object] = {
        "format": tarfile.PAX_FORMAT,
    }
    if comment is not None:
        options["pax_headers"] = {"comment": comment}
    with tarfile.open(raw_tar, "w", **options) as archive:
        for name, data in sorted(files.items()):
            if name not in omitted:
                add_tar_file(archive, prefix + name, data)
        for index in range(filler_count):
            add_tar_file(
                archive,
                prefix + f"overlay/chrome/browser/cmux_term/fixture-{index:03}.cc",
                f"// fixture source {index}\n".encode(),
            )
    source_archive = bundle / f"cmux-browser-source-{VERSION}.tar.zst"
    subprocess.run(
        ["zstd", "--quiet", "--force", raw_tar, "-o", source_archive],
        check=True,
    )
    return source_archive


def load_publication_verifier():
    spec = importlib.util.spec_from_file_location(
        "verify_linux_nightly_publication",
        ROOT / "scripts/verify-linux-nightly-publication.py",
    )
    if spec is None or spec.loader is None:
        raise AssertionError("publication verifier module does not load")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def make_run(run_id: int, path: str, head_sha: str) -> dict[str, object]:
    return {
        "conclusion": "success",
        "event": "workflow_dispatch",
        "head_repository": {"full_name": "manaflow-ai/cmux-browser"},
        "head_branch": "main",
        "head_sha": head_sha,
        "html_url": (
            f"https://github.com/manaflow-ai/cmux-browser/actions/runs/{run_id}"
        ),
        "id": run_id,
        "path": path,
        "repository": {"full_name": "manaflow-ai/cmux-browser"},
        "run_attempt": 1,
        "status": "completed",
    }


def create_fixture(root: Path) -> tuple[Path, Path, Path, Path, Path, Path]:
    bundle = root / "candidate"
    receipt_dir = root / "smoke-receipt"
    bundle.mkdir()
    receipt_dir.mkdir()

    private_key = root / "private.pem"
    public_key = root / "public.pem"
    subprocess.run(
        ["openssl", "ecparam", "-name", "prime256v1", "-genkey", "-noout",
         "-out", private_key],
        check=True,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    subprocess.run(
        ["openssl", "ec", "-in", private_key, "-pubout", "-out", public_key],
        check=True,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )

    (bundle / "cmux-linux-x64-installer.run").write_bytes(b"installer\n")
    (bundle / "cmux-linux-x64.deb").write_bytes(b"deb\n")
    (bundle / "THIRD_PARTY_NOTICES.html").write_text(
        "<html>Chromium and Ghostty notices</html>\n", encoding="utf-8"
    )
    write_json(bundle / "third-party-notices.spdx.json", {"spdxVersion": "2.3"})

    update_zip = bundle / "cmux-linux-x64.zip"
    with zipfile.ZipFile(update_zip, "w") as archive:
        for name in (
            "chrome",
            "cmux-tui",
            "cmux-tui.REVISION",
            "cmux-tui.GHOSTTY_REVISION",
            "cmux-tui.GHOSTTY_RESOURCES_MANIFEST.sha256",
            "cmux-update-feed-url",
        ):
            archive.writestr(f"cmux-browser/{name}", name + "\n")

    source_receipt = {
        "build_instructions": "docs/releases.md",
        "dependencies": [
            {
                "commit": "1" * 40,
                "name": "Chromium",
                "repository": "https://chromium.googlesource.com/chromium/src",
                "version": "151.0.7922.34",
            },
            {
                "commit": "2" * 40,
                "name": "cmux and cmux-tui",
                "repository": "https://github.com/manaflow-ai/cmux",
            },
            {
                "commit": "3" * 40,
                "name": "Ghostty",
                "repository": "https://github.com/manaflow-ai/ghostty",
            },
            {
                "commit": "4" * 40,
                "name": "uBlock Origin",
                "repository": "https://github.com/gorhill/uBlock",
            },
            {
                "additional_commits": ["6" * 40],
                "commit": "5" * 40,
                "name": "Helium",
                "repository": "https://github.com/imputnet/helium",
            },
        ],
        "license": "GPL-3.0-only",
        "license_scope": {
            "combined_work": "GPL-3.0-only",
            "cmux_authored_material": "GPL-3.0-or-later",
            "helium_derived_material": "GPL-3.0-only",
        },
        "notices": "THIRD_PARTY_NOTICES.md",
        "schema": 1,
        "source": {
            "commit": SOURCE_SHA,
            "included_in_archive": True,
            "repository": "manaflow-ai/cmux-browser",
        },
        "version": VERSION,
    }
    write_json(bundle / "corresponding-source.json", source_receipt)
    source_archive = write_source_archive(root, bundle, source_receipt)

    subprocess.run(
        [
            sys.executable,
            ROOT / "scripts/package-update.py",
            "--version",
            VERSION,
            "--base-url",
            "https://github.com/manaflow-ai/cmux-v2/releases/download/nightly",
            "--private-key",
            private_key,
            "--output",
            bundle / "update.json",
            "--artifact",
            f"linux-x64={update_zip},cmux-browser,chrome",
        ],
        check=True,
        stdout=subprocess.DEVNULL,
    )

    subjects = [
        bundle / "cmux-linux-x64-installer.run",
        update_zip,
        bundle / "cmux-linux-x64.deb",
        source_archive,
        bundle / "corresponding-source.json",
        bundle / "THIRD_PARTY_NOTICES.html",
        bundle / "third-party-notices.spdx.json",
        bundle / "update.json",
    ]
    provenance_command: list[object] = [
        sys.executable,
        ROOT / "scripts/generate-release-provenance.py",
        "--version",
        VERSION,
        "--channel",
        "nightly",
        "--release-tag",
        "nightly",
        "--source-repository",
        "manaflow-ai/cmux-browser",
        "--source-sha",
        SOURCE_SHA,
        "--run-url",
        (
            "https://github.com/manaflow-ai/cmux-browser/actions/runs/"
            f"{SOURCE_RUN_ID}"
        ),
        "--output",
        bundle / "provenance.intoto.jsonl",
    ]
    for subject in subjects:
        provenance_command.extend(("--subject", subject))
    subprocess.run(
        [str(argument) for argument in provenance_command],
        check=True,
        stdout=subprocess.DEVNULL,
    )
    write_json(
        bundle / "RELEASE-METADATA.json",
        {
            "channel": "nightly",
            "release_tag": "nightly",
            "run_url": (
                "https://github.com/manaflow-ai/cmux-browser/actions/runs/"
                f"{SOURCE_RUN_ID}"
            ),
            "source_repository": "manaflow-ai/cmux-browser",
            "source_sha": SOURCE_SHA,
            "version": VERSION,
        },
    )
    checksum_lines = []
    for path in sorted(bundle.iterdir()):
        checksum_lines.append(
            f"{hashlib.sha256(path.read_bytes()).hexdigest()}  {path.name}"
        )
    (bundle / "SHA256SUMS").write_text(
        "\n".join(checksum_lines) + "\n", encoding="ascii"
    )

    source_run = root / "source-run.json"
    smoke_run = root / "smoke-run.json"
    write_json(
        source_run,
        make_run(
            SOURCE_RUN_ID,
            ".github/workflows/release-linux-nightly.yml",
            SOURCE_SHA,
        ),
    )
    write_json(
        smoke_run,
        make_run(
            SMOKE_RUN_ID,
            ".github/workflows/release-linux-smoke.yml",
            SMOKE_WORKFLOW_SHA,
        ),
    )
    comparison = root / "smoke-to-publisher.json"
    write_json(
        comparison,
        {
            "ahead_by": 1,
            "base_commit": {"sha": SMOKE_WORKFLOW_SHA},
            "behind_by": 0,
            "head_commit": {"sha": PUBLISHER_SHA},
            "merge_base_commit": {"sha": SMOKE_WORKFLOW_SHA},
            "status": "ahead",
        },
    )
    smoke_receipt = receipt_dir / "linux-nightly-smoke-receipt.json"
    write_json(
        smoke_receipt,
        {
            "artifact": {
                "name": "cmux-linux-nightly-release",
                "sha256sums_sha256": hashlib.sha256(
                    (bundle / "SHA256SUMS").read_bytes()
                ).hexdigest(),
            },
            "schema": 1,
            "smoke_run_attempt": 1,
            "smoke_run_id": SMOKE_RUN_ID,
            "smoke_workflow": ".github/workflows/release-linux-smoke.yml",
            "smoke_workflow_sha": SMOKE_WORKFLOW_SHA,
            "source_repository": "manaflow-ai/cmux-browser",
            "source_run_id": SOURCE_RUN_ID,
            "source_sha": SOURCE_SHA,
            "version": VERSION,
        },
    )
    return bundle, source_run, smoke_run, comparison, smoke_receipt, public_key


def verification_command(
    bundle: Path,
    source_run: Path,
    smoke_run: Path,
    comparison: Path,
    smoke_receipt: Path,
    public_key: Path,
) -> list[str]:
    return [
        sys.executable,
        str(ROOT / "scripts/verify-linux-nightly-publication.py"),
        "--bundle",
        str(bundle),
        "--source-run",
        str(source_run),
        "--smoke-run",
        str(smoke_run),
        "--smoke-to-publisher",
        str(comparison),
        "--smoke-receipt",
        str(smoke_receipt),
        "--public-key",
        str(public_key),
        "--expected-version",
        VERSION,
        "--expected-source-sha",
        SOURCE_SHA,
        "--source-run-id",
        str(SOURCE_RUN_ID),
        "--smoke-run-id",
        str(SMOKE_RUN_ID),
        "--publisher-sha",
        PUBLISHER_SHA,
    ]


def expect_rejected(command: list[str], label: str) -> None:
    result = subprocess.run(
        command, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )
    if result.returncode == 0:
        raise AssertionError(f"invalid candidate accepted: {label}")


def expect_release_state_rejected(release: dict[str, object], label: str) -> None:
    try:
        revalidate_selected(release, 77)
    except ReleaseStateError:
        return
    raise AssertionError(f"unsafe public release accepted: {label}")


def expect_source_rejected(verifier, bundle: Path, label: str) -> None:
    try:
        verifier.verify_corresponding_source(
            bundle, version=VERSION, source_sha=SOURCE_SHA
        )
    except verifier.VerificationError:
        return
    raise AssertionError(f"invalid corresponding source accepted: {label}")


def main() -> int:
    opaque_draft: dict[str, object] = {
        "assets": [],
        "draft": True,
        "id": 77,
        "name": "cmux Browser Nightly 151.0.7922.48 — Linux x64",
        "prerelease": True,
        "tag_name": "untagged-7f5c1f345209a6411f9d",
    }
    assert select_release([[opaque_draft]]) == 77
    revalidate_selected(opaque_draft, 77)
    rolling = dict(opaque_draft)
    rolling.update({"draft": False, "tag_name": "nightly"})
    revalidate_selected(rolling, 77)
    stable = dict(rolling)
    stable["prerelease"] = False
    expect_release_state_rejected(stable, "stable release using nightly tag")
    misnamed = dict(rolling)
    misnamed["name"] = "cmux stable 151.0.7922.48"
    expect_release_state_rejected(misnamed, "misnamed release using nightly tag")

    with tempfile.TemporaryDirectory() as raw_tmp:
        root = Path(raw_tmp)
        (
            bundle,
            source_run,
            smoke_run,
            comparison,
            smoke_receipt,
            public_key,
        ) = create_fixture(root)
        verifier = load_publication_verifier()
        verifier.verify_corresponding_source(
            bundle, version=VERSION, source_sha=SOURCE_SHA
        )
        source_receipt = json.loads(
            (bundle / "corresponding-source.json").read_text(encoding="utf-8")
        )
        source_archive = bundle / f"cmux-browser-source-{VERSION}.tar.zst"
        original_source_archive = source_archive.read_bytes()
        write_source_archive(
            root, bundle, source_receipt, comment="f" * 40
        )
        expect_source_rejected(verifier, bundle, "wrong Git commit marker")
        write_source_archive(root, bundle, source_receipt, comment=None)
        expect_source_rejected(verifier, bundle, "missing Git commit marker")
        write_source_archive(
            root,
            bundle,
            source_receipt,
            omit={"scripts/build-release-linux.sh"},
        )
        expect_source_rejected(verifier, bundle, "omitted rebuild source")
        write_source_archive(root, bundle, source_receipt, filler_count=0)
        expect_source_rejected(verifier, bundle, "implausibly small source tree")
        source_archive.write_bytes(original_source_archive)

        command = verification_command(
            bundle,
            source_run,
            smoke_run,
            comparison,
            smoke_receipt,
            public_key,
        )
        subprocess.run(command, check=True, stdout=subprocess.DEVNULL)

        extra = bundle / "unexpected.txt"
        extra.write_text("unexpected\n", encoding="utf-8")
        expect_rejected(command, "extra release asset")
        extra.unlink()

        smoke_record = json.loads(smoke_run.read_text(encoding="utf-8"))
        smoke_record["conclusion"] = "failure"
        write_json(smoke_run, smoke_record)
        expect_rejected(command, "failed smoke run")
        smoke_record["conclusion"] = "success"
        write_json(smoke_run, smoke_record)

        comparison_record = json.loads(comparison.read_text(encoding="utf-8"))
        comparison_record["status"] = "diverged"
        write_json(comparison, comparison_record)
        expect_rejected(command, "unreviewed smoke workflow commit")
        comparison_record["status"] = "ahead"
        write_json(comparison, comparison_record)

        receipt = json.loads(smoke_receipt.read_text(encoding="utf-8"))
        receipt["source_run_id"] = SOURCE_RUN_ID + 1
        write_json(smoke_receipt, receipt)
        expect_rejected(command, "receipt for another source run")
        receipt["source_run_id"] = SOURCE_RUN_ID
        write_json(smoke_receipt, receipt)

        installer = bundle / "cmux-linux-x64-installer.run"
        original = installer.read_bytes()
        installer.write_bytes(original + b"tampered\n")
        expect_rejected(command, "checksum mismatch")
        installer.write_bytes(original)

        subprocess.run(command, check=True, stdout=subprocess.DEVNULL)

    print("Linux nightly publication verification tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
