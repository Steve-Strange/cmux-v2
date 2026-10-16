#!/bin/bash
# Publish one already-verified Linux nightly bundle to the public download repo.
# The release stays draft while every asset is replaced and rechecked.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
RELEASE_DIR="${CMUX_RELEASE_DIR:-$ROOT/candidate}"
REPOSITORY="${CMUX_DISTRIBUTION_REPOSITORY:-manaflow-ai/cmux-v2}"
VERSION="${CMUX_VERSION:?CMUX_VERSION is required}"
SOURCE_SHA="${CMUX_SOURCE_SHA:?CMUX_SOURCE_SHA is required}"
SOURCE_RUN_ID="${CMUX_SOURCE_RUN_ID:?CMUX_SOURCE_RUN_ID is required}"
SMOKE_RUN_ID="${CMUX_SMOKE_RUN_ID:?CMUX_SMOKE_RUN_ID is required}"
TAG=nightly

: "${GH_TOKEN:?GH_TOKEN is required}"
[[ "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$ ]] || {
  echo "invalid release version: $VERSION" >&2
  exit 2
}
[[ "$SOURCE_SHA" =~ ^[0-9a-f]{40}$ ]] || {
  echo "invalid source SHA" >&2
  exit 2
}

source_archive="$RELEASE_DIR/cmux-browser-source-$VERSION.tar.zst"
non_update_assets=(
  "$RELEASE_DIR/RELEASE-METADATA.json"
  "$RELEASE_DIR/SHA256SUMS"
  "$RELEASE_DIR/THIRD_PARTY_NOTICES.html"
  "$source_archive"
  "$RELEASE_DIR/cmux-linux-x64-installer.run"
  "$RELEASE_DIR/cmux-linux-x64.deb"
  "$RELEASE_DIR/cmux-linux-x64.zip"
  "$RELEASE_DIR/corresponding-source.json"
  "$RELEASE_DIR/provenance.intoto.jsonl"
  "$RELEASE_DIR/third-party-notices.spdx.json"
)
update_asset="$RELEASE_DIR/update.json"
for asset in "${non_update_assets[@]}" "$update_asset"; do
  [ -f "$asset" ] && [ ! -L "$asset" ] && [ -s "$asset" ] || {
    echo "missing, empty, or unsafe release asset: $asset" >&2
    exit 2
  }
done

temp="$(mktemp -d "${RUNNER_TEMP:-${TMPDIR:-/tmp}}/cmux-publish.XXXXXX")"
trap 'rm -r "$temp"' EXIT

target_sha="$(
  gh api "repos/$REPOSITORY/git/ref/heads/main" --jq .object.sha
)"
[[ "$target_sha" =~ ^[0-9a-f]{40}$ ]] || {
  echo "public repository main ref is invalid" >&2
  exit 1
}

title="cmux Browser Nightly $VERSION — Linux x64"
body="$temp/release-notes.md"
cat > "$body" <<EOF
cmux Browser Linux x64 nightly $VERSION.

- Installer: \`cmux-linux-x64-installer.run\`
- License: GPL-3.0-only for the combined work
- Exact Corresponding Source: \`cmux-browser-source-$VERSION.tar.zst\`
- Source commit: \`$SOURCE_SHA\`
- Build run: https://github.com/manaflow-ai/cmux-browser/actions/runs/$SOURCE_RUN_ID
- Smoke run: https://github.com/manaflow-ai/cmux-browser/actions/runs/$SMOKE_RUN_ID
EOF

# Include drafts so an earlier empty pre-publication release can be adopted.
gh api --method GET --paginate --slurp \
  "repos/$REPOSITORY/releases?per_page=100" > "$temp/releases.json"
python3 "$ROOT/scripts/linux_nightly_release_state.py" select \
  --releases "$temp/releases.json" \
  --output "$temp/selection.json"

release_id="$(python3 - "$temp/selection.json" <<'PY'
import json
import sys
value = json.load(open(sys.argv[1], encoding="utf-8"))["id"]
print("" if value is None else value)
PY
)"

export CMUX_PUBLIC_TARGET_SHA="$target_sha"
export CMUX_RELEASE_BODY="$(cat "$body")"
export CMUX_RELEASE_TITLE="$title"
export CMUX_RELEASE_VERSION="$VERSION"
python3 - "$temp/normalize.json" <<'PY'
import json
import os
from pathlib import Path
import sys

Path(sys.argv[1]).write_text(
    json.dumps(
        {
            "body": os.environ["CMUX_RELEASE_BODY"],
            "draft": True,
            "make_latest": "false",
            "name": os.environ["CMUX_RELEASE_TITLE"],
            "prerelease": True,
            "tag_name": "nightly",
            "target_commitish": os.environ["CMUX_PUBLIC_TARGET_SHA"],
        }
    )
    + "\n",
    encoding="utf-8",
)
PY

if [ -n "$release_id" ]; then
  # Re-read the selected release immediately before changing it. This rejects
  # a stale-draft race and converts an existing rolling nightly back to draft
  # before any public asset can change.
  gh api "repos/$REPOSITORY/releases/$release_id" > "$temp/selected.json"
  python3 "$ROOT/scripts/linux_nightly_release_state.py" revalidate \
    --release "$temp/selected.json" \
    --release-id "$release_id"
  gh api --method PATCH "repos/$REPOSITORY/releases/$release_id" \
    --input "$temp/normalize.json" > "$temp/normalized.json"
else
  gh api --method POST "repos/$REPOSITORY/releases" \
    --input "$temp/normalize.json" > "$temp/normalized.json"
  release_id="$(python3 - "$temp/normalized.json" <<'PY'
import json
import sys
print(json.load(open(sys.argv[1], encoding="utf-8"))["id"])
PY
)"
fi

python3 - "$temp/normalized.json" "$release_id" "$title" "$target_sha" <<'PY'
import json
import sys
release = json.load(open(sys.argv[1], encoding="utf-8"))
expected_id = int(sys.argv[2])
if release.get("id") != expected_id:
    raise SystemExit("normalized release ID mismatch")
if release.get("tag_name") != "nightly":
    raise SystemExit("normalized release tag mismatch")
if release.get("name") != sys.argv[3]:
    raise SystemExit("normalized release title mismatch")
if release.get("draft") is not True or release.get("prerelease") is not True:
    raise SystemExit("release must be a hidden prerelease during upload")
if release.get("target_commitish") not in {"main", sys.argv[4]}:
    raise SystemExit("normalized release target mismatch")
PY

fetch_assets() {
  gh api --method GET --paginate --slurp \
    "repos/$REPOSITORY/releases/$release_id/assets?per_page=100"
}

# Start from an empty hidden release. This also removes an older update.json,
# so an updater can never observe a manifest during replacement.
fetch_assets > "$temp/old-assets.json"
python3 - "$temp/old-assets.json" <<'PY' > "$temp/old-asset-ids"
import json
import sys
pages = json.load(open(sys.argv[1], encoding="utf-8"))
for page in pages:
    for asset in page:
        value = asset.get("id")
        if not isinstance(value, int) or value <= 0:
            raise SystemExit("invalid existing release asset ID")
        print(value)
PY
while IFS= read -r asset_id; do
  [ -n "$asset_id" ] || continue
  gh api --method DELETE "repos/$REPOSITORY/releases/assets/$asset_id"
done < "$temp/old-asset-ids"

# Package, source, notices, receipts, provenance, and checksums always precede
# update.json. The release is still draft and therefore not downloadable.
gh release upload "$TAG" --repo "$REPOSITORY" --clobber \
  "${non_update_assets[@]}"

fetch_assets > "$temp/non-update-assets.json"
python3 - "$temp/non-update-assets.json" "$RELEASE_DIR" "$VERSION" <<'PY'
import json
from pathlib import Path
import sys

pages = json.load(open(sys.argv[1], encoding="utf-8"))
assets = [asset for page in pages for asset in page]
root = Path(sys.argv[2])
version = sys.argv[3]
expected_names = {
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
}
if len(assets) != len(expected_names) or {asset.get("name") for asset in assets} != expected_names:
    raise SystemExit("hidden release non-update inventory mismatch")
for asset in assets:
    path = root / asset["name"]
    if asset.get("state") != "uploaded" or asset.get("size") != path.stat().st_size:
        raise SystemExit(f"hidden release asset verification failed: {path.name}")
PY

gh release upload "$TAG" --repo "$REPOSITORY" --clobber "$update_asset"

fetch_assets > "$temp/final-assets.json"
python3 - "$temp/final-assets.json" "$RELEASE_DIR" "$VERSION" <<'PY'
import hashlib
import json
from pathlib import Path
import sys

pages = json.load(open(sys.argv[1], encoding="utf-8"))
assets = [asset for page in pages for asset in page]
root = Path(sys.argv[2])
version = sys.argv[3]
expected_names = {
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
def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as source:
        while chunk := source.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()

if len(assets) != len(expected_names) or {asset.get("name") for asset in assets} != expected_names:
    raise SystemExit("hidden release final inventory mismatch")
for asset in assets:
    path = root / asset["name"]
    if asset.get("state") != "uploaded":
        raise SystemExit(f"release asset is not uploaded: {path.name}")
    if asset.get("size") != path.stat().st_size:
        raise SystemExit(f"release asset size mismatch: {path.name}")
    digest = asset.get("digest")
    if digest is not None:
        expected = "sha256:" + sha256(path)
        if digest != expected:
            raise SystemExit(f"release asset digest mismatch: {path.name}")
PY

# This is the only transition that makes the release public, and it occurs
# only after exact inventory and byte-size verification with update.json last.
gh api "repos/$REPOSITORY/releases/$release_id" > "$temp/prepublish.json"
python3 - "$temp/prepublish.json" "$release_id" "$title" <<'PY'
import json
import sys
release = json.load(open(sys.argv[1], encoding="utf-8"))
if release.get("id") != int(sys.argv[2]):
    raise SystemExit("pre-publication release ID mismatch")
if release.get("tag_name") != "nightly" or release.get("name") != sys.argv[3]:
    raise SystemExit("pre-publication release identity mismatch")
if release.get("draft") is not True or release.get("prerelease") is not True:
    raise SystemExit("release became public before final verification")
PY
printf '%s\n' '{"draft":false,"prerelease":true,"make_latest":"false"}' \
  > "$temp/publish.json"
gh api --method PATCH "repos/$REPOSITORY/releases/$release_id" \
  --input "$temp/publish.json" > "$temp/published.json"
python3 - "$temp/published.json" "$release_id" "$title" <<'PY'
import json
import sys
release = json.load(open(sys.argv[1], encoding="utf-8"))
if release.get("id") != int(sys.argv[2]):
    raise SystemExit("published release ID mismatch")
if release.get("tag_name") != "nightly" or release.get("name") != sys.argv[3]:
    raise SystemExit("published release identity mismatch")
if release.get("draft") is not False or release.get("prerelease") is not True:
    raise SystemExit("nightly release did not publish as a prerelease")
PY

echo "published https://github.com/$REPOSITORY/releases/tag/$TAG"
