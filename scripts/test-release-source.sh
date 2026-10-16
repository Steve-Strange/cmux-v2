#!/bin/bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TMP="$(mktemp -d "${TMPDIR:-/tmp}/cmux-release-source-test.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT
VERSION="151.0.7922.65535"

CMUX_RELEASE_VERSION="$VERSION" \
CMUX_RELEASE_OUTPUT="$TMP/release" \
  "$ROOT/scripts/build-release-source.sh"

ARCHIVE="$TMP/release/cmux-browser-source-$VERSION.tar.zst"
RECEIPT="$TMP/release/corresponding-source.json"
test -f "$ARCHIVE"
test -f "$RECEIPT"
python3 - "$ROOT" "$RECEIPT" <<'PY'
import json
from pathlib import Path
import subprocess
import sys

root = Path(sys.argv[1])
receipt = json.loads(Path(sys.argv[2]).read_text())
assert receipt["schema"] == 1
assert receipt["license"] == "GPL-3.0-only"
assert receipt["license_scope"] == {
    "combined_work": "GPL-3.0-only",
    "cmux_authored_material": "GPL-3.0-or-later",
    "helium_derived_material": "GPL-3.0-only",
}
assert receipt["source"]["commit"] == subprocess.check_output(
    ["git", "-C", str(root), "rev-parse", "HEAD"], text=True
).strip()
dependencies = {item["name"]: item for item in receipt["dependencies"]}
assert dependencies["Chromium"]["version"] == (
    root / ".chromium-version"
).read_text().strip()
assert dependencies["Ghostty"]["commit"] == (
    root / "ghostty-revision.txt"
).read_text().strip()
assert dependencies["Helium"]["additional_commits"] == [
    "dee5600297344dd25b2dddb2ba343e19be7723f7"
]
assert all(len(item["commit"]) == 40 for item in dependencies.values())
PY

echo "release source packaging tests passed"
