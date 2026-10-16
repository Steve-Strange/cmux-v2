#!/usr/bin/env bash
# Verify that the built Chromium image embeds the terminal-backend identities
# from this checkout. This catches stale remote Frameworks before a newer
# cmux-tui helper is installed into an otherwise valid-looking app bundle.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
RUNTIME_ROOT="${1:-}"
if [ -z "$RUNTIME_ROOT" ]; then
  echo "usage: $0 <active-Chromium-runtime-image>" >&2
  exit 2
fi

REVISION_HEADER="$ROOT/overlay/chrome/browser/cmux_term/cmux_tui_revision.h"
EXPECTED_CMUX_REVISION="$(sed -n '/kPinnedCmuxTuiBuildCommit/{n;s/^    "\([0-9a-f]*\)";$/\1/p;}' "$REVISION_HEADER")"
EXPECTED_GHOSTTY_REVISION="$(sed -n '/kPinnedCmuxTuiGhosttyCommit/{n;s/^    "\([0-9a-f]*\)";$/\1/p;}' "$REVISION_HEADER")"
if [[ ! "$EXPECTED_CMUX_REVISION" =~ ^[0-9a-f]{40}$ ]] ||
   [[ ! "$EXPECTED_GHOSTTY_REVISION" =~ ^[0-9a-f]{40}$ ]]; then
  echo "error: could not read both 40-character runtime pins from $REVISION_HEADER" >&2
  exit 1
fi

matches_image() {
  local image="$1"
  # Consume the complete strings stream in one pass. With pipefail, an
  # early-exiting grep can SIGPIPE strings on a multi-gigabyte Framework and
  # turn a real match into status 141.
  LC_ALL=C strings -a "$image" | awk \
    -v cmux="$EXPECTED_CMUX_REVISION" \
    -v ghostty="$EXPECTED_GHOSTTY_REVISION" '
      $0 == cmux { found_cmux = 1 }
      $0 == ghostty { found_ghostty = 1 }
      END { exit !(found_cmux && found_ghostty) }
    '
}

if [ ! -f "$RUNTIME_ROOT" ]; then
  echo "error: active Chromium runtime image is not a regular file: $RUNTIME_ROOT" >&2
  exit 1
fi

if matches_image "$RUNTIME_ROOT"; then
  echo "compiled terminal backend pins match overlay: $RUNTIME_ROOT"
  exit 0
fi

echo "error: built Chromium runtime does not contain this checkout's cmux-tui/Ghostty identity pins" >&2
echo "expected cmux-tui: $EXPECTED_CMUX_REVISION" >&2
echo "expected Ghostty:  $EXPECTED_GHOSTTY_REVISION" >&2
echo "rebuild after applying the complete overlay; do not deploy a one-file incremental Framework" >&2
exit 1
