#!/bin/bash
# Download and unpack the full Chrome Web Store uBlock Origin CRX into the
# user-land cache used by deploy.sh. No extension payloads are checked into git.
set -euo pipefail

UBLOCK_ID="cjpalhdlnbpafiamejdnhcphjbkeiagm"
UBLOCK_EXPECTED_NAME="uBlock Origin"
# Keep this pin textually identical to scripts/fetch-extensions.ps1.
UBLOCK_EXPECTED_VERSION="${UBLOCK_ORIGIN_VERSION:-1.72.2}"
UBLOCK_EXPECTED_CRX_SHA256="6e02d8e6dce569eec721b531f9c7d28403e5426442417d5a855a17dd288b56f8"
CHROME_PRODVERSION="${CHROME_PRODVERSION:-138.0.0.0}"
EXT_ROOT="${CMUX_EXTENSIONS_DIR:-$HOME/cmux-extensions}"
TARGET="$EXT_ROOT/ublock"
CRX_URL="https://clients2.google.com/service/update2/crx?response=redirect&prodversion=${CHROME_PRODVERSION}&acceptformat=crx3&x=id%3D${UBLOCK_ID}%26installsource%3Dondemand%26uc"

print_manifest() {
  local manifest="$1"
  python3 - "$manifest" "$UBLOCK_ID" "$UBLOCK_EXPECTED_NAME" \
      "$UBLOCK_EXPECTED_VERSION" <<'PY'
import base64
import hashlib
import json
import sys
from pathlib import Path

manifest = Path(sys.argv[1])
expected_id = sys.argv[2]
expected_name = sys.argv[3]
expected_version = sys.argv[4]

data = json.loads(manifest.read_text())
name = data.get("name")
version = data.get("version")
manifest_version = data.get("manifest_version")
key = data.get("key")

def extension_id_for_key(encoded_key):
    public_key = base64.b64decode(encoded_key)
    digest = hashlib.sha256(public_key).digest()[:16]
    return "".join(chr(ord("a") + (byte >> 4)) +
                   chr(ord("a") + (byte & 0x0f)) for byte in digest)

actual_id = extension_id_for_key(key) if key else ""
ok = (name == expected_name and version == expected_version and
      manifest_version == 2 and actual_id == expected_id)
print(f"{name} {version} manifest_version={manifest_version} id={actual_id}")
if not ok:
    raise SystemExit(
        f"manifest validation failed: expected {expected_name} "
        f"{expected_version} manifest_version=2 id={expected_id}")
PY
}

if [ -f "$TARGET/manifest.json" ]; then
  rm -rf "$TARGET/_metadata"
  if [ "$(cat "$TARGET/.cmux-crx-sha256" 2>/dev/null || true)" = \
       "$UBLOCK_EXPECTED_CRX_SHA256" ] &&
      print_manifest "$TARGET/manifest.json" \
        >/tmp/cmux-ublock-manifest.$$ 2>/tmp/cmux-ublock-manifest-err.$$; then
    cat /tmp/cmux-ublock-manifest.$$
    rm -f /tmp/cmux-ublock-manifest.$$ /tmp/cmux-ublock-manifest-err.$$
    exit 0
  fi
  cat /tmp/cmux-ublock-manifest-err.$$ >&2 || true
  rm -f /tmp/cmux-ublock-manifest.$$ /tmp/cmux-ublock-manifest-err.$$
fi

tmp="$(mktemp -d "${TMPDIR:-/tmp}/cmux-ublock.XXXXXX")"
trap 'rm -rf "$tmp"' EXIT

mkdir -p "$EXT_ROOT"
echo "fetching uBlock Origin $UBLOCK_EXPECTED_VERSION from Chrome Web Store"
curl -fL --retry 2 -A "Mozilla/5.0" -o "$tmp/ublock.crx" "$CRX_URL"
ACTUAL_CRX_SHA256="$(shasum -a 256 "$tmp/ublock.crx" | awk '{print $1}')"
if [ "$ACTUAL_CRX_SHA256" != "$UBLOCK_EXPECTED_CRX_SHA256" ]; then
  echo "error: uBlock Origin CRX digest mismatch: $ACTUAL_CRX_SHA256" >&2
  exit 1
fi

python3 - "$tmp/ublock.crx" "$tmp/ublock.zip" "$tmp/ublock.key" "$UBLOCK_ID" <<'PY'
import base64
import hashlib
import sys
from pathlib import Path

crx_path = Path(sys.argv[1])
zip_path = Path(sys.argv[2])
key_path = Path(sys.argv[3])
expected_id = sys.argv[4]
data = crx_path.read_bytes()

if data[:4] != b"Cr24":
    raise SystemExit("download was not a CRX file")
version = int.from_bytes(data[4:8], "little")
if version != 3:
    raise SystemExit(f"expected CRX3, got CRX{version}")

header_size = int.from_bytes(data[8:12], "little")
header = data[12:12 + header_size]
zip_path.write_bytes(data[12 + header_size:])

def read_varint(buf, pos):
    out = 0
    shift = 0
    while True:
        byte = buf[pos]
        pos += 1
        out |= (byte & 0x7f) << shift
        if byte < 0x80:
            return out, pos
        shift += 7

def iter_fields(buf):
    pos = 0
    while pos < len(buf):
        key, pos = read_varint(buf, pos)
        field = key >> 3
        wire_type = key & 7
        if wire_type == 2:
            size, pos = read_varint(buf, pos)
            value = buf[pos:pos + size]
            pos += size
            yield field, value
        elif wire_type == 0:
            _, pos = read_varint(buf, pos)
            yield field, None
        elif wire_type == 1:
            pos += 8
            yield field, None
        elif wire_type == 5:
            pos += 4
            yield field, None
        else:
            raise SystemExit(f"unsupported protobuf wire type {wire_type}")

def extension_id_for_key(public_key):
    digest = hashlib.sha256(public_key).digest()[:16]
    return "".join(chr(ord("a") + (byte >> 4)) +
                   chr(ord("a") + (byte & 0x0f)) for byte in digest)

matched_key = None
for field, proof in iter_fields(header):
    # CRX3 AsymmetricKeyProof fields: 2 = sha256_with_rsa,
    # 3 = sha256_with_ecdsa. The Web Store id is derived from one public key.
    if field not in (2, 3) or proof is None:
        continue
    for proof_field, proof_value in iter_fields(proof):
        if proof_field != 1 or proof_value is None:
            continue
        if extension_id_for_key(proof_value) == expected_id:
            matched_key = proof_value
            break
    if matched_key is not None:
        break

if matched_key is None:
    raise SystemExit(f"CRX3 header did not contain key for {expected_id}")
key_path.write_text(base64.b64encode(matched_key).decode("ascii"))
PY

mkdir -p "$tmp/unpacked"
unzip -q "$tmp/ublock.zip" -d "$tmp/unpacked"
rm -rf "$tmp/unpacked/_metadata"

python3 - "$tmp/unpacked/manifest.json" "$tmp/ublock.key" "$UBLOCK_ID" \
    "$UBLOCK_EXPECTED_NAME" "$UBLOCK_EXPECTED_VERSION" <<'PY'
import base64
import hashlib
import json
import sys
from pathlib import Path

manifest = Path(sys.argv[1])
public_key = Path(sys.argv[2]).read_text().strip()
expected_id = sys.argv[3]
expected_name = sys.argv[4]
expected_version = sys.argv[5]

data = json.loads(manifest.read_text())
data["key"] = public_key
manifest.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n")

decoded = base64.b64decode(public_key)
digest = hashlib.sha256(decoded).digest()[:16]
actual_id = "".join(chr(ord("a") + (byte >> 4)) +
                    chr(ord("a") + (byte & 0x0f)) for byte in digest)

if data.get("name") != expected_name:
    raise SystemExit(f"unexpected extension name: {data.get('name')}")
if data.get("version") != expected_version:
    raise SystemExit(f"unexpected extension version: {data.get('version')}")
if data.get("manifest_version") != 2:
    raise SystemExit(f"unexpected manifest_version: {data.get('manifest_version')}")
if actual_id != expected_id:
    raise SystemExit(f"unexpected extension id from key: {actual_id}")
PY

rm -rf "$TARGET.new"
cp -R "$tmp/unpacked" "$TARGET.new"
printf '%s\n' "$ACTUAL_CRX_SHA256" > "$TARGET.new/.cmux-crx-sha256"
rm -rf "$TARGET"
mv "$TARGET.new" "$TARGET"

print_manifest "$TARGET/manifest.json"
