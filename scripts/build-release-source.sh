#!/bin/bash
# Build the exact cmux-browser source archive and dependency receipt published
# beside a GPL-covered binary release. Public upstream dependencies are pinned
# by commit in the receipt; cmux-authored/private-repository inputs are included
# directly in the archive.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VERSION="${CMUX_RELEASE_VERSION:?CMUX_RELEASE_VERSION is required}"
OUT="${CMUX_RELEASE_OUTPUT:-$ROOT/release}"
SOURCE_SHA="${CMUX_RELEASE_SOURCE_SHA:-$(git -C "$ROOT" rev-parse HEAD)}"
ARCHIVE="$OUT/cmux-browser-source-$VERSION.tar.zst"
RECEIPT="$OUT/corresponding-source.json"

case "$VERSION" in
  [0-9]*.[0-9]*.[0-9]*.[0-9]*) ;;
  *)
    echo "error: CMUX_RELEASE_VERSION must be a four-part version" >&2
    exit 2
    ;;
esac
git -C "$ROOT" cat-file -e "$SOURCE_SHA^{commit}" 2>/dev/null || {
  echo "error: source commit does not exist: $SOURCE_SHA" >&2
  exit 2
}
if [ "$SOURCE_SHA" != "$(git -C "$ROOT" rev-parse HEAD)" ]; then
  echo "error: source archive commit must match the checked-out release commit" >&2
  exit 2
fi
command -v zstd >/dev/null || {
  echo "error: zstd is required to package corresponding source" >&2
  exit 2
}

mkdir -p "$OUT"
TMP="$(mktemp -d "${TMPDIR:-/tmp}/cmux-release-source.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT
RAW_TAR="$TMP/source.tar"
PREFIX="cmux-browser-source-$VERSION/"
COMMIT_TIME="$(git -C "$ROOT" show -s --format=%ct "$SOURCE_SHA")"

python3 - "$ROOT" "$VERSION" "$SOURCE_SHA" "$RECEIPT" <<'PY'
import json
from pathlib import Path
import re
import sys

root = Path(sys.argv[1])
version = sys.argv[2]
source_sha = sys.argv[3]
output = Path(sys.argv[4])
notices = (root / "THIRD_PARTY_NOTICES.md").read_text(encoding="utf-8")
revision_header = (
    root / "overlay/chrome/browser/cmux_term/cmux_tui_revision.h"
).read_text(encoding="utf-8")

def require(pattern: str, value: str, label: str) -> str:
    match = re.search(pattern, value, re.MULTILINE)
    if not match:
        raise SystemExit(f"could not resolve {label}")
    return match.group(1)

chromium_commit = require(
    r"Current Browser base: `([0-9a-f]{40})`", notices, "Chromium commit"
)
cmux_commit = require(
    r'kPinnedCmuxTuiBuildCommit\[\]\s*=\s*\n\s*"([0-9a-f]{40})"',
    revision_header,
    "cmux commit",
)
ghostty_commit = (root / "ghostty-revision.txt").read_text().strip()
ublock_commit = require(
    r"Source commit: `([0-9a-f]{40})`", notices, "uBlock Origin commit"
)
helium_commit = require(
    r"- `([0-9a-f]{40})` \(Helium `main` reviewed", notices, "Helium commit"
)
helium_side_panel_commit = require(
    r"- `([0-9a-f]{40})` \(Helium revision 8,",
    notices,
    "Helium side-panel commit",
)

receipt = {
    "schema": 1,
    "version": version,
    "license": "GPL-3.0-only",
    "license_scope": {
        "combined_work": "GPL-3.0-only",
        "cmux_authored_material": "GPL-3.0-or-later",
        "helium_derived_material": "GPL-3.0-only",
    },
    "source": {
        "repository": "manaflow-ai/cmux-browser",
        "commit": source_sha,
        "included_in_archive": True,
    },
    "dependencies": [
        {
            "name": "Chromium",
            "repository": "https://chromium.googlesource.com/chromium/src",
            "commit": chromium_commit,
            "version": (root / ".chromium-version").read_text().strip(),
        },
        {
            "name": "cmux and cmux-tui",
            "repository": "https://github.com/manaflow-ai/cmux",
            "commit": cmux_commit,
        },
        {
            "name": "Ghostty",
            "repository": "https://github.com/manaflow-ai/ghostty",
            "commit": ghostty_commit,
        },
        {
            "name": "uBlock Origin",
            "repository": "https://github.com/gorhill/uBlock",
            "commit": ublock_commit,
        },
        {
            "name": "Helium",
            "repository": "https://github.com/imputnet/helium",
            "commit": helium_commit,
            "additional_commits": [helium_side_panel_commit],
        },
    ],
    "build_instructions": "docs/releases.md",
    "notices": "THIRD_PARTY_NOTICES.md",
}
output.write_text(
    json.dumps(receipt, indent=2, sort_keys=True) + "\n", encoding="utf-8"
)
PY

git -C "$ROOT" archive \
  --format=tar --prefix="$PREFIX" "$SOURCE_SHA" > "$RAW_TAR"
python3 - "$RAW_TAR" "$RECEIPT" "$PREFIX" "$COMMIT_TIME" <<'PY'
import io
from pathlib import Path
import sys
import tarfile

archive = Path(sys.argv[1])
receipt = Path(sys.argv[2]).read_bytes()
prefix = sys.argv[3]
commit_time = int(sys.argv[4])
info = tarfile.TarInfo(f"{prefix}SOURCE-MANIFEST.json")
info.size = len(receipt)
info.mtime = commit_time
info.mode = 0o644
info.uid = 0
info.gid = 0
info.uname = "root"
info.gname = "root"
with tarfile.open(archive, "a") as output:
    output.addfile(info, io.BytesIO(receipt))
PY

zstd --quiet --threads=0 -19 --force "$RAW_TAR" -o "$ARCHIVE"
zstd --quiet --decompress --stdout "$ARCHIVE" | tar -tf - \
  > "$TMP/archive-contents.txt"
grep -Fqx "${PREFIX}LICENSE" "$TMP/archive-contents.txt"
grep -Fqx "${PREFIX}THIRD_PARTY_NOTICES.md" "$TMP/archive-contents.txt"
grep -Fqx "${PREFIX}SOURCE-MANIFEST.json" "$TMP/archive-contents.txt"

echo "corresponding source archive: $ARCHIVE"
echo "corresponding source receipt: $RECEIPT"
