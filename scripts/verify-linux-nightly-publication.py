#!/usr/bin/env python3
"""Fail-closed verification for a Linux nightly publication candidate."""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import subprocess
import tarfile
import tempfile
from typing import Any
import zipfile


SOURCE_REPOSITORY = "manaflow-ai/cmux-browser"
SOURCE_WORKFLOW = ".github/workflows/release-linux-nightly.yml"
SMOKE_WORKFLOW = ".github/workflows/release-linux-smoke.yml"
BUNDLE_ARTIFACT = "cmux-linux-nightly-release"
SMOKE_RECEIPT_NAME = "linux-nightly-smoke-receipt.json"
UPDATE_BASE_URL = (
    "https://github.com/manaflow-ai/cmux-v2/releases/download/nightly"
)
SHA_RE = re.compile(r"[0-9a-f]{40}")
VERSION_RE = re.compile(r"[0-9]+(?:\.[0-9]+){3}")
CHECKSUM_RE = re.compile(r"([0-9a-f]{64})  ([^\r\n]+)")
MIN_SOURCE_REGULAR_FILES = 300
REQUIRED_SOURCE_REGULAR_FILES = {
    ".chromium-version",
    "LICENSE",
    "SOURCE-MANIFEST.json",
    "THIRD_PARTY_NOTICES.md",
    "docs/releases.md",
    "overlay/chrome/browser/cmux_term/BUILD.gn",
    "patches/cmux-pinned-toolbar-actions-chromium-151.patch",
    "patches/helium-media-toolbar.patch",
    "patches/helium-new-tab.patch",
    "patches/helium-omnibar-chromium-151.patch",
    "patches/helium-settings-chromium-151.patch",
    "scripts/apply.sh",
    "scripts/bootstrap-release-linux.sh",
    "scripts/build-release-linux.sh",
    "scripts/build-release-source.sh",
    "scripts/vendor-ghostty-linux-local.sh",
}


class VerificationError(RuntimeError):
    """A candidate violated a publication invariant."""


def require(condition: bool, message: str) -> None:
    if not condition:
        raise VerificationError(message)


def load_json_object(path: Path, label: str) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
        raise VerificationError(f"invalid {label}: {error}") from error
    require(isinstance(value, dict), f"{label} must be a JSON object")
    return value


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        while chunk := source.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def parse_identifier(value: str, label: str) -> int:
    require(value.isascii() and value.isdigit(), f"{label} must be numeric")
    parsed = int(value)
    require(parsed > 0, f"{label} must be positive")
    require(str(parsed) == value, f"{label} must use canonical decimal form")
    return parsed


def validate_inputs(
    version: str,
    source_sha: str,
    source_run_id: str,
    smoke_run_id: str,
) -> tuple[int, int]:
    require(VERSION_RE.fullmatch(version) is not None,
            "expected version must have four numeric components")
    components = tuple(int(component) for component in version.split("."))
    require(all(component <= 65535 for component in components),
            "expected version component exceeds 65535")
    require(SHA_RE.fullmatch(source_sha) is not None,
            "expected source SHA must be 40 lowercase hex characters")
    source_id = parse_identifier(source_run_id, "source run ID")
    smoke_id = parse_identifier(smoke_run_id, "smoke run ID")
    require(source_id != smoke_id, "source and smoke run IDs must differ")
    return source_id, smoke_id


def verify_run(
    run: dict[str, Any],
    *,
    expected_id: int,
    expected_path: str,
    expected_sha: str | None,
    label: str,
) -> None:
    require(run.get("id") == expected_id, f"{label} run ID mismatch")
    require(run.get("path") == expected_path, f"unexpected {label} workflow")
    require(run.get("event") == "workflow_dispatch",
            f"{label} was not manually dispatched")
    require(run.get("status") == "completed", f"{label} is not completed")
    require(run.get("conclusion") == "success", f"{label} did not succeed")
    head_sha = run.get("head_sha")
    require(isinstance(head_sha, str) and SHA_RE.fullmatch(head_sha) is not None,
            f"{label} workflow SHA is invalid")
    if expected_sha is not None:
        require(head_sha == expected_sha, f"{label} source SHA mismatch")
    require(run.get("head_branch") == "main",
            f"{label} was not dispatched from main")
    require(run.get("repository", {}).get("full_name") == SOURCE_REPOSITORY,
            f"{label} repository mismatch")
    require(run.get("head_repository", {}).get("full_name") == SOURCE_REPOSITORY,
            f"{label} head repository mismatch")
    require(run.get("html_url") == (
        f"https://github.com/{SOURCE_REPOSITORY}/actions/runs/{expected_id}"
    ), f"{label} URL mismatch")
    attempt = run.get("run_attempt")
    require(isinstance(attempt, int) and attempt > 0,
            f"{label} run attempt is invalid")


def verify_smoke_ancestry(
    comparison: dict[str, Any],
    *,
    smoke_sha: str,
    publisher_sha: str,
) -> None:
    require(SHA_RE.fullmatch(publisher_sha) is not None,
            "publisher workflow SHA is invalid")
    status = comparison.get("status")
    require(status in {"ahead", "identical"},
            "smoke workflow commit is not an ancestor of publisher main")
    require(comparison.get("base_commit", {}).get("sha") == smoke_sha,
            "smoke comparison base mismatch")
    require(comparison.get("merge_base_commit", {}).get("sha") == smoke_sha,
            "smoke workflow commit is not the comparison merge base")
    if status == "identical":
        require(publisher_sha == smoke_sha,
                "identical comparison has different workflow commits")
    else:
        require(comparison.get("head_commit", {}).get("sha") == publisher_sha,
                "publisher comparison head mismatch")
    require(comparison.get("behind_by") == 0,
            "publisher main is behind the smoke workflow commit")


def expected_inventory(version: str) -> set[str]:
    return {
        "RELEASE-METADATA.json",
        "SHA256SUMS",
        "THIRD_PARTY_NOTICES.html",
        f"cmux-browser-source-{version}.tar.zst",
        "cmux-linux-x64-installer.run",
        "cmux-linux-x64.deb",
        "cmux-linux-x64.zip",
        "corresponding-source.json",
        "provenance.intoto.jsonl",
        "third-party-notices.spdx.json",
        "update.json",
    }


def verify_inventory(root: Path, version: str) -> set[str]:
    require(root.is_dir() and not root.is_symlink(),
            "bundle directory is missing or unsafe")
    actual = {path.name for path in root.iterdir()}
    expected = expected_inventory(version)
    require(actual == expected, (
        "bundle inventory mismatch: "
        f"missing={sorted(expected - actual)} extra={sorted(actual - expected)}"
    ))
    for name in sorted(expected):
        path = root / name
        require(path.is_file() and not path.is_symlink(),
                f"bundle asset is not a regular file: {name}")
        require(path.stat().st_size > 0, f"bundle asset is empty: {name}")
    return expected


def verify_checksums(root: Path, inventory: set[str]) -> str:
    manifest = root / "SHA256SUMS"
    entries: dict[str, str] = {}
    try:
        lines = manifest.read_text(encoding="ascii").splitlines()
    except (OSError, UnicodeDecodeError) as error:
        raise VerificationError(f"invalid SHA256SUMS: {error}") from error
    for line in lines:
        match = CHECKSUM_RE.fullmatch(line)
        require(match is not None, f"malformed SHA256SUMS line: {line!r}")
        digest, name = match.groups()
        candidate = PurePosixPath(name)
        require(not candidate.is_absolute() and len(candidate.parts) == 1,
                f"unsafe checksum path: {name!r}")
        require(name not in entries, f"duplicate checksum entry: {name}")
        entries[name] = digest
    expected_names = inventory - {"SHA256SUMS"}
    require(set(entries) == expected_names, "SHA256SUMS inventory mismatch")
    for name, expected_digest in entries.items():
        require(sha256_file(root / name) == expected_digest,
                f"SHA256 mismatch: {name}")
    return sha256_file(manifest)


def verify_release_metadata(
    root: Path,
    *,
    version: str,
    source_sha: str,
    source_run_id: int,
) -> None:
    metadata = load_json_object(root / "RELEASE-METADATA.json",
                                "release metadata")
    require(metadata == {
        "channel": "nightly",
        "release_tag": "nightly",
        "run_url": (
            f"https://github.com/{SOURCE_REPOSITORY}/actions/runs/"
            f"{source_run_id}"
        ),
        "source_repository": SOURCE_REPOSITORY,
        "source_sha": source_sha,
        "version": version,
    }, "release metadata does not match the approved source run")


def verify_corresponding_source(
    root: Path,
    *,
    version: str,
    source_sha: str,
) -> None:
    receipt_path = root / "corresponding-source.json"
    receipt = load_json_object(receipt_path, "corresponding-source receipt")
    require(set(receipt) == {
        "build_instructions",
        "dependencies",
        "license",
        "license_scope",
        "notices",
        "schema",
        "source",
        "version",
    }, "unexpected corresponding-source receipt shape")
    require(receipt["schema"] == 1, "unsupported source receipt schema")
    require(receipt["version"] == version, "source receipt version mismatch")
    require(receipt["license"] == "GPL-3.0-only",
            "combined work must be GPL-3.0-only")
    require(receipt["license_scope"] == {
        "combined_work": "GPL-3.0-only",
        "cmux_authored_material": "GPL-3.0-or-later",
        "helium_derived_material": "GPL-3.0-only",
    }, "source receipt license scope mismatch")
    require(receipt["source"] == {
        "commit": source_sha,
        "included_in_archive": True,
        "repository": SOURCE_REPOSITORY,
    }, "source receipt commit or inclusion claim mismatch")
    require(receipt["build_instructions"] == "docs/releases.md",
            "source receipt build instructions mismatch")
    require(receipt["notices"] == "THIRD_PARTY_NOTICES.md",
            "source receipt notices path mismatch")
    dependencies = receipt["dependencies"]
    require(isinstance(dependencies, list) and dependencies,
            "source receipt dependency inventory is empty")
    required_dependencies = {"Chromium", "cmux and cmux-tui", "Ghostty",
                             "uBlock Origin", "Helium"}
    names = {
        dependency.get("name")
        for dependency in dependencies
        if isinstance(dependency, dict)
    }
    require(names == required_dependencies,
            "source receipt dependency inventory mismatch")
    for dependency in dependencies:
        require(isinstance(dependency, dict), "invalid source dependency")
        require(SHA_RE.fullmatch(str(dependency.get("commit", ""))) is not None,
                f"invalid source dependency commit: {dependency.get('name')}")
        require(str(dependency.get("repository", "")).startswith("https://"),
                f"invalid source dependency repository: {dependency.get('name')}")

    archive = root / f"cmux-browser-source-{version}.tar.zst"
    prefix = f"cmux-browser-source-{version}/"
    archive_root = prefix.rstrip("/")
    required_members = {
        prefix + name for name in REQUIRED_SOURCE_REGULAR_FILES
    }
    found: set[str] = set()
    regular_files: set[str] = set()
    archived_receipt: dict[str, Any] | None = None
    process = subprocess.Popen(
        ["zstd", "--quiet", "--decompress", "--stdout", str(archive)],
        stdout=subprocess.PIPE,
    )
    try:
        require(process.stdout is not None, "could not read source archive")
        with tarfile.open(fileobj=process.stdout, mode="r|") as source_tar:
            require(source_tar.pax_headers.get("comment") == source_sha,
                    "corresponding source Git archive commit marker mismatch")
            for member in source_tar:
                path = PurePosixPath(member.name)
                require(not path.is_absolute() and ".." not in path.parts,
                        f"unsafe corresponding-source member: {member.name}")
                require(path.parts and path.parts[0] == archive_root,
                        f"source member is outside archive root: {member.name}")
                if member.isfile():
                    require(member.name not in regular_files,
                            f"duplicate corresponding-source file: {member.name}")
                    regular_files.add(member.name)
                if member.name not in required_members:
                    continue
                require(member.isfile(),
                        f"required source member is not a file: {member.name}")
                found.add(member.name)
                if member.name.endswith("/SOURCE-MANIFEST.json"):
                    extracted = source_tar.extractfile(member)
                    require(extracted is not None, "source manifest is unreadable")
                    data = extracted.read(1024 * 1024 + 1)
                    require(len(data) <= 1024 * 1024,
                            "source manifest is unexpectedly large")
                    try:
                        value = json.loads(data)
                    except (UnicodeDecodeError, json.JSONDecodeError) as error:
                        raise VerificationError(
                            f"invalid archived source manifest: {error}"
                        ) from error
                    require(isinstance(value, dict),
                            "archived source manifest must be an object")
                    archived_receipt = value
        process.stdout.close()
        return_code = process.wait()
    except BaseException:
        process.kill()
        process.wait()
        raise
    require(return_code == 0, "could not decompress corresponding source")
    require(found == required_members,
            "corresponding source omits required rebuild files")
    require(len(regular_files) >= MIN_SOURCE_REGULAR_FILES,
            "corresponding source tree is implausibly small")
    require(archived_receipt == receipt,
            "archived source manifest differs from public source receipt")


def verify_provenance(
    root: Path,
    *,
    version: str,
    source_sha: str,
    source_run_id: int,
) -> None:
    statement = load_json_object(root / "provenance.intoto.jsonl",
                                 "provenance statement")
    require(statement.get("_type") == "https://in-toto.io/Statement/v1",
            "unexpected provenance statement type")
    require(statement.get("predicateType") == "https://slsa.dev/provenance/v1",
            "unexpected provenance predicate type")
    subject_names = {
        "THIRD_PARTY_NOTICES.html",
        f"cmux-browser-source-{version}.tar.zst",
        "cmux-linux-x64-installer.run",
        "cmux-linux-x64.deb",
        "cmux-linux-x64.zip",
        "corresponding-source.json",
        "third-party-notices.spdx.json",
        "update.json",
    }
    subjects = statement.get("subject")
    require(isinstance(subjects, list) and
            {item.get("name") for item in subjects if isinstance(item, dict)} ==
            subject_names and len(subjects) == len(subject_names),
            "provenance subject inventory mismatch")
    for subject in subjects:
        require(isinstance(subject, dict), "invalid provenance subject")
        name = subject["name"]
        require(subject.get("digest") == {"sha256": sha256_file(root / name)},
                f"provenance digest mismatch: {name}")
    try:
        definition = statement["predicate"]["buildDefinition"]
        run_details = statement["predicate"]["runDetails"]
    except (KeyError, TypeError) as error:
        raise VerificationError("provenance structure is incomplete") from error
    require(definition.get("externalParameters") == {
        "channel": "nightly",
        "releaseTag": "nightly",
        "version": version,
    }, "provenance release parameters mismatch")
    require(definition.get("resolvedDependencies") == [{
        "digest": {"gitCommit": source_sha},
        "uri": f"git+https://github.com/{SOURCE_REPOSITORY}@{source_sha}",
    }], "provenance source dependency mismatch")
    require(run_details.get("metadata") == {
        "invocationId": (
            f"https://github.com/{SOURCE_REPOSITORY}/actions/runs/"
            f"{source_run_id}"
        )
    }, "provenance invocation mismatch")


def verify_update_manifest(root: Path, *, version: str, public_key: Path) -> None:
    envelope = load_json_object(root / "update.json", "update envelope")
    require(set(envelope) == {"payload", "signature"},
            "unexpected update envelope shape")
    try:
        payload_bytes = base64.b64decode(envelope["payload"], validate=True)
        signature_bytes = base64.b64decode(envelope["signature"], validate=True)
        payload = json.loads(payload_bytes)
    except (KeyError, TypeError, ValueError, json.JSONDecodeError) as error:
        raise VerificationError(f"invalid signed update manifest: {error}") from error
    require(isinstance(payload, dict), "update payload must be an object")
    canonical = json.dumps(payload, sort_keys=True,
                           separators=(",", ":")).encode("utf-8")
    require(payload_bytes == canonical, "update payload is not canonical JSON")
    require(public_key.is_file() and not public_key.is_symlink(),
            "update public key is missing or unsafe")
    with tempfile.TemporaryDirectory() as raw_tmp:
        temp = Path(raw_tmp)
        payload_path = temp / "payload.json"
        signature_path = temp / "signature.der"
        payload_path.write_bytes(payload_bytes)
        signature_path.write_bytes(signature_bytes)
        verified = subprocess.run(
            ["openssl", "dgst", "-sha256", "-verify", str(public_key),
             "-signature", str(signature_path), str(payload_path)],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
    require(verified.returncode == 0, "update signature verification failed")
    require(payload.get("schema") == 1, "unsupported update payload schema")
    require(payload.get("version") == version, "update version mismatch")
    platforms = payload.get("platforms")
    require(isinstance(platforms, dict) and set(platforms) == {"linux-x64"},
            "update manifest must contain only linux-x64")
    linux = platforms["linux-x64"]
    archive = root / "cmux-linux-x64.zip"
    require(linux == {
        "archive_root": "cmux-browser",
        "executable": "chrome",
        "sha256": sha256_file(archive),
        "size": str(archive.stat().st_size),
        "url": f"{UPDATE_BASE_URL}/cmux-linux-x64.zip",
    }, "Linux update entry mismatch")
    try:
        with zipfile.ZipFile(archive) as zipped:
            names = set(zipped.namelist())
    except (OSError, zipfile.BadZipFile) as error:
        raise VerificationError(f"invalid Linux update archive: {error}") from error
    required_paths = {
        "cmux-browser/chrome",
        "cmux-browser/cmux-tui",
        "cmux-browser/cmux-tui.REVISION",
        "cmux-browser/cmux-tui.GHOSTTY_REVISION",
        "cmux-browser/cmux-tui.GHOSTTY_RESOURCES_MANIFEST.sha256",
        "cmux-browser/cmux-update-feed-url",
    }
    require(required_paths <= names, "Linux update archive is incomplete")


def verify_smoke_receipt(
    path: Path,
    *,
    version: str,
    source_sha: str,
    source_run_id: int,
    smoke_run_id: int,
    smoke_run_attempt: int,
    smoke_workflow_sha: str,
    checksums_digest: str,
) -> None:
    require(path.parent.is_dir(), "smoke receipt directory is missing")
    siblings = {entry.name for entry in path.parent.iterdir()}
    require(siblings == {SMOKE_RECEIPT_NAME},
            "smoke receipt artifact inventory mismatch")
    require(path.is_file() and not path.is_symlink(),
            "smoke receipt is missing or unsafe")
    receipt = load_json_object(path, "smoke receipt")
    require(receipt == {
        "artifact": {
            "name": BUNDLE_ARTIFACT,
            "sha256sums_sha256": checksums_digest,
        },
        "schema": 1,
        "smoke_run_attempt": smoke_run_attempt,
        "smoke_run_id": smoke_run_id,
        "smoke_workflow": SMOKE_WORKFLOW,
        "smoke_workflow_sha": smoke_workflow_sha,
        "source_repository": SOURCE_REPOSITORY,
        "source_run_id": source_run_id,
        "source_sha": source_sha,
        "version": version,
    }, "smoke receipt does not authorize this exact candidate")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--bundle", type=Path, required=True)
    parser.add_argument("--source-run", type=Path, required=True)
    parser.add_argument("--smoke-run", type=Path, required=True)
    parser.add_argument("--smoke-to-publisher", type=Path, required=True)
    parser.add_argument("--smoke-receipt", type=Path, required=True)
    parser.add_argument("--public-key", type=Path, required=True)
    parser.add_argument("--expected-version", required=True)
    parser.add_argument("--expected-source-sha", required=True)
    parser.add_argument("--source-run-id", required=True)
    parser.add_argument("--smoke-run-id", required=True)
    parser.add_argument("--publisher-sha", required=True)
    args = parser.parse_args()

    try:
        source_id, smoke_id = validate_inputs(
            args.expected_version,
            args.expected_source_sha,
            args.source_run_id,
            args.smoke_run_id,
        )
        source_run = load_json_object(args.source_run, "source run API record")
        smoke_run = load_json_object(args.smoke_run, "smoke run API record")
        smoke_to_publisher = load_json_object(
            args.smoke_to_publisher, "smoke-to-publisher comparison"
        )
        verify_run(
            source_run,
            expected_id=source_id,
            expected_path=SOURCE_WORKFLOW,
            expected_sha=args.expected_source_sha,
            label="source",
        )
        verify_run(
            smoke_run,
            expected_id=smoke_id,
            expected_path=SMOKE_WORKFLOW,
            expected_sha=None,
            label="smoke",
        )
        verify_smoke_ancestry(
            smoke_to_publisher,
            smoke_sha=smoke_run["head_sha"],
            publisher_sha=args.publisher_sha,
        )
        inventory = verify_inventory(args.bundle, args.expected_version)
        checksums_digest = verify_checksums(args.bundle, inventory)
        verify_release_metadata(
            args.bundle,
            version=args.expected_version,
            source_sha=args.expected_source_sha,
            source_run_id=source_id,
        )
        verify_corresponding_source(
            args.bundle,
            version=args.expected_version,
            source_sha=args.expected_source_sha,
        )
        verify_provenance(
            args.bundle,
            version=args.expected_version,
            source_sha=args.expected_source_sha,
            source_run_id=source_id,
        )
        verify_update_manifest(
            args.bundle,
            version=args.expected_version,
            public_key=args.public_key,
        )
        verify_smoke_receipt(
            args.smoke_receipt,
            version=args.expected_version,
            source_sha=args.expected_source_sha,
            source_run_id=source_id,
            smoke_run_id=smoke_id,
            smoke_run_attempt=smoke_run["run_attempt"],
            smoke_workflow_sha=smoke_run["head_sha"],
            checksums_digest=checksums_digest,
        )
    except VerificationError as error:
        parser.error(str(error))
    print("Linux nightly publication candidate verified")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
