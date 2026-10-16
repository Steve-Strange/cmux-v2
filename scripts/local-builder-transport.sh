#!/usr/bin/env bash
# SSH-compatible transport that executes a warm Chromium builder command on
# the current self-hosted Linux runner. It lets the existing sync/apply/build
# scripts keep one transport contract for remote macOS and local Linux.
set -euo pipefail

while [ "$#" -gt 0 ]; do
  case "$1" in
    -l|-o) shift 2 ;;
    --) shift; break ;;
    -*) shift ;;
    *) break ;;
  esac
done

[ "$#" -ge 1 ] || { echo "local transport: host is required" >&2; exit 2; }
shift  # The host is an intentional placeholder (normally "local").

if [ "$#" -eq 0 ]; then
  exec "${SHELL:-/bin/bash}"
elif [ "$#" -eq 1 ]; then
  exec /bin/bash -lc "$1"
else
  exec "$@"
fi
