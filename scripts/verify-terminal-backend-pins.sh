#!/bin/bash
# Fast, build-independent validation for duplicated pins and source worktrees.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CMUX_SRC="${CMUX_SRC:-$HOME/fun/cmuxterm-hq/worktrees/codex-cmux-tui-viewer-lease-release}"
GHOSTTY_SRC="${GHOSTTY_SRC:-$HOME/fun/ghostty-wt-cursor-visual-api}"
REVISION_HEADER="$ROOT/overlay/chrome/browser/cmux_term/cmux_tui_revision.h"
CMUX_PIN="$(sed -n '/kPinnedCmuxTuiBuildCommit/{n;s/^    "\([0-9a-f]*\)";$/\1/p;}' "$REVISION_HEADER")"
HEADER_GHOSTTY_PIN="$(sed -n '/kPinnedCmuxTuiGhosttyCommit/{n;s/^    "\([0-9a-f]*\)";$/\1/p;}' "$REVISION_HEADER")"
GHOSTTY_PIN="$(tr -d '[:space:]' < "$ROOT/ghostty-revision.txt")"
WINDOWS_GHOSTTY_PIN="$(sed -n "s/^EXPECTED_GHOSTTY_REVISION = '\([0-9a-f]*\)'$/\1/p" "$ROOT/scripts/apply_win_chrome.py")"

if [ "$WINDOWS_GHOSTTY_PIN" != "$GHOSTTY_PIN" ]; then
  echo "ERROR: apply_win_chrome.py Ghostty pin differs from ghostty-revision.txt" >&2
  exit 1
fi
if [ "$HEADER_GHOSTTY_PIN" != "$GHOSTTY_PIN" ]; then
  echo "ERROR: cmux_tui_revision.h Ghostty pin differs from ghostty-revision.txt" >&2
  exit 1
fi
if [ "$(git -C "$CMUX_SRC" rev-parse HEAD)" != "$CMUX_PIN" ]; then
  echo "ERROR: cmux worktree does not match browser cmux-tui pin $CMUX_PIN" >&2
  exit 1
fi
if [ "$(git -C "$CMUX_SRC/ghostty" rev-parse HEAD)" != "$GHOSTTY_PIN" ]; then
  echo "ERROR: cmux submodule does not match browser Ghostty pin $GHOSTTY_PIN" >&2
  exit 1
fi
if [ "$(git -C "$GHOSTTY_SRC" rev-parse HEAD)" != "$GHOSTTY_PIN" ]; then
  echo "ERROR: Ghostty worktree does not match browser pin $GHOSTTY_PIN" >&2
  exit 1
fi

echo "cmux-tui pin: $CMUX_PIN"
echo "manaflow Ghostty pin: $GHOSTTY_PIN"
echo "terminal backend pins agree"
