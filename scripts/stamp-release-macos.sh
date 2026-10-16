#!/bin/bash
# Stamp the leased remote Chromium tree with the release bit and four-part
# version. This must run after HQ's sync/apply phase and before build.sh.
set -euo pipefail

. "$(dirname "$0")/builder-transport.sh"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
HOST="${CMUX_BUILDER:?CMUX_BUILDER is required}"
SRC="${CHROMIUM_SRC:?CHROMIUM_SRC is required}"
VERSION="${CMUX_RELEASE_VERSION:?CMUX_RELEASE_VERSION is required}"
REMOTE_TOOLS="$SRC/out/Release/cmux-release-tools"

case "$REMOTE_TOOLS" in
  */out/Release/cmux-release-tools) ;;
  *)
    echo "error: unsafe remote release-tools path: $REMOTE_TOOLS" >&2
    exit 2
    ;;
esac

ssh -o BatchMode=yes "$HOST" \
  "mkdir -p '$REMOTE_TOOLS/repository/scripts'"
rsync -a \
  "$ROOT/scripts/stamp-release-build.py" \
  "$ROOT/scripts/release_version.py" \
  "$HOST:$REMOTE_TOOLS/repository/scripts/"
rsync -a "$ROOT/.chromium-version" \
  "$HOST:$REMOTE_TOOLS/repository/.chromium-version"

ssh -o BatchMode=yes "$HOST" "set -e
  python3 '$REMOTE_TOOLS/repository/scripts/stamp-release-build.py' \
    --chromium-src '$SRC'
  python3 '$REMOTE_TOOLS/repository/scripts/release_version.py' \
    --repository '$REMOTE_TOOLS/repository' \
    --chromium-src '$SRC' \
    --version '$VERSION'"
