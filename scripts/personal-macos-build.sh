#!/usr/bin/env bash
# Build and archive an isolated personal Debug app without launching it.
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$PROJECT_DIR"
# shellcheck source=scripts/lib/mobile-attach.sh
source "$PROJECT_DIR/scripts/lib/mobile-attach.sh"

BUILD_TAG=steve-ssh
OUTPUT_DIR="$PROJECT_DIR/build/personal-macos"
while (( $# )); do
  case "$1" in
    --tag) BUILD_TAG="${2:?--tag needs a value}"; shift 2 ;;
    --output-dir) OUTPUT_DIR="${2:?--output-dir needs a value}"; shift 2 ;;
    -h|--help)
      echo 'Usage: scripts/personal-macos-build.sh [--tag steve-ssh] [--output-dir <directory>]'
      echo 'Requires full Xcode 26, Metal, Rust, and authenticated gh; see scripts/personal-macos.md.'
      exit 0
      ;;
    *) echo "Unknown argument: $1" >&2; exit 64 ;;
  esac
done
[[ "$BUILD_TAG" =~ ^[a-z0-9][a-z0-9-]{0,39}$ ]] || {
  echo 'Build tag must contain 1-40 lowercase letters, digits, or hyphens.' >&2
  exit 64
}
[[ "$(uname -s)" == Darwin ]] || { echo 'A macOS builder is required.' >&2; exit 1; }
xcodebuild -version
xcrun metal --version
command -v rustup >/dev/null
command -v gh >/dev/null
gh auth token >/dev/null

# The personal branch has a separate root from upstream. Do not resolve a
# rolling or incompatible client from that history; compare the pinned inputs.
CLIENT_COMMIT="$(python3 - <<'PY'
import json
import subprocess

with open("scripts/personal-macos-client.json") as source:
    pin = json.load(source)
expected_url = f"https://files.cmux.com/cmux-tui/{pin['clientCommit']}/manifest.json"
if pin["manifestURL"] != expected_url:
    raise SystemExit("Personal client manifest must use the exact official commit URL.")
if pin["signerWorkflow"] != "manaflow-ai/cmux/.github/workflows/cmux-tui-artifacts.yml":
    raise SystemExit("Personal client must use the official signing workflow.")
for path, expected in pin["inputs"].items():
    actual = subprocess.check_output(["git", "rev-parse", f"HEAD:{path}"], text=True).strip()
    if actual != expected:
        raise SystemExit(f"Pinned cmux-tui input changed: {path}; review and update the personal client pin.")
    subprocess.run(["git", "diff", "--exit-code", "HEAD", "--", path], check=True)
print(pin["clientCommit"])
PY
)"
MANIFEST_URL="https://files.cmux.com/cmux-tui/$CLIENT_COMMIT/manifest.json"
SIGNER_WORKFLOW=manaflow-ai/cmux/.github/workflows/cmux-tui-artifacts.yml

mkdir -p "$OUTPUT_DIR"
OUTPUT_DIR="$(cd "$OUTPUT_DIR" && pwd)"
BUILD_LOG="$OUTPUT_DIR/build.log"
ZIP_PATH="$OUTPUT_DIR/cmux-$BUILD_TAG.zip"
# Remove only this helper's prior published outputs so a failed rebuild cannot
# leave a previous app next to the new build log.
rm -f "$ZIP_PATH" "$ZIP_PATH.sha256" "$OUTPUT_DIR/build-receipt.json" "$OUTPUT_DIR/portable-bundle.json"
echo 'Checking personal build inputs and installing dependencies' | tee "$BUILD_LOG"
python3 scripts/verify-local.py --only project --only test-wiring --only localization 2>&1 | tee -a "$BUILD_LOG"
git submodule update --init --depth 1 -- ghostty vendor/bonsplit 2>&1 | tee -a "$BUILD_LOG"
./scripts/install-rust-ci.sh 2>&1 | tee -a "$BUILD_LOG"
export PATH="$HOME/.cargo/bin:$PATH"

# Use the upstream checksum and archive validator. A missing prebuilt fails
# explicitly rather than silently attempting a costly source build with Zig.
GHOSTTY_SHA="$(git rev-parse HEAD:ghostty)" \
  GHOSTTYKIT_DOWNLOAD_RETRIES=3 GHOSTTYKIT_DOWNLOAD_RETRY_DELAY=5 \
  GHOSTTYKIT_DOWNLOAD_MAX_TIME=300 ./scripts/download-prebuilt-ghosttykit.sh 2>&1 | tee -a "$BUILD_LOG"

DERIVED_DATA="${CMUX_PERSONAL_DERIVED_DATA:-$HOME/Library/Developer/Xcode/DerivedData/cmux-$BUILD_TAG}"
unset CMUX_TUI_CLIENT_LOCAL CMUX_SKIP_CMUX_TUI_CLIENT CMUX_ALLOW_UNSIGNED_DEV_APP
export CMUX_DEV_BACKEND_MODE=local
export CMUX_GHOSTTYKIT_PREPROVISIONED=1
export CMUX_SKIP_ZIG_BUILD=1
./scripts/reload.sh --tag "$BUILD_TAG" --no-global-cli-links --swift-frontend-workaround \
  --derived-data "$DERIVED_DATA" --cmux-tui-manifest-url "$MANIFEST_URL" \
  2>&1 | tee -a "$BUILD_LOG"

APP_PATH="$(awk '/^App path:/{getline; sub(/^  /, ""); print; exit}' "$BUILD_LOG")"
[[ -n "$APP_PATH" && -d "$APP_PATH/Contents" ]] || { echo 'Build produced no app.' >&2; exit 1; }
EXPECTED_BUNDLE_ID="$(cmux_attach_mac_bundle_id "$BUILD_TAG")"
ACTUAL_BUNDLE_ID="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' "$APP_PATH/Contents/Info.plist")"
[[ "$ACTUAL_BUNDLE_ID" == "$EXPECTED_BUNDLE_ID" ]] || { echo 'Unexpected bundle identity.' >&2; exit 1; }

# reload checks provenance before using manifest bytes. Also bind the bundled
# copy to the exact pinned source commit before publishing the personal app.
MANIFEST="$APP_PATH/Contents/Resources/bin/cmux-tui-ssh/manifest.json"
gh attestation verify "$MANIFEST" --repo manaflow-ai/cmux \
  --signer-workflow "$SIGNER_WORKFLOW" --source-digest "$CLIENT_COMMIT"
PROBE="$("$APP_PATH/Contents/Resources/bin/cmux-tui" remote-probe --json)"
python3 - "$MANIFEST" "$CLIENT_COMMIT" "$PROBE" <<'PY'
import json
import sys

with open(sys.argv[1]) as source:
    manifest = json.load(source)
probe = json.loads(sys.argv[3])
if manifest["commit"] != sys.argv[2] or probe.get("build_identity") != sys.argv[2]:
    raise SystemExit("Bundled cmux-tui does not match the personal client pin.")
PY
python3 scripts/personal-macos-portable.py "$APP_PATH" "$ACTUAL_BUNDLE_ID" \
  --receipt "$OUTPUT_DIR/portable-bundle.json"
ditto -c -k --sequesterRsrc --keepParent "$APP_PATH" "$ZIP_PATH"
(
  cd "$OUTPUT_DIR"
  shasum -a 256 "cmux-$BUILD_TAG.zip" > "cmux-$BUILD_TAG.zip.sha256"
)
python3 - "$OUTPUT_DIR/build-receipt.json" "$BUILD_TAG" "$ACTUAL_BUNDLE_ID" "$CLIENT_COMMIT" "$OUTPUT_DIR/portable-bundle.json" <<'PY'
import datetime
import json
import subprocess
import sys

receipt = {
    "sourceCommit": subprocess.check_output(["git", "rev-parse", "HEAD"], text=True).strip(),
    "sourceDirty": bool(subprocess.check_output(["git", "status", "--porcelain"], text=True).strip()),
    "builtAt": datetime.datetime.now(datetime.timezone.utc).isoformat(),
    "tag": sys.argv[2],
    "bundleIdentifier": sys.argv[3],
    "clientCommit": sys.argv[4],
    "xcode": subprocess.check_output(["xcodebuild", "-version"], text=True).strip(),
    "architecture": subprocess.check_output(["uname", "-m"], text=True).strip(),
    "signing": "ad-hoc",
    "backend": "local",
    "ghosttyCLIHelper": "stub (upstream CI mode)",
    "swiftFrontendWorkaround": True,
    "appLaunched": False,
    "appHostTestTargetCompiled": False,
    "appHostTestsExecuted": False,
}
with open(sys.argv[5]) as source:
    receipt.update(json.load(source))
with open(sys.argv[1], "w") as output:
    json.dump(receipt, output, indent=2)
    output.write("\n")
PY
echo "Personal build archive: $ZIP_PATH"
