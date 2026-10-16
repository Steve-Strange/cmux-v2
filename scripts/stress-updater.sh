#!/usr/bin/env bash
# Repeatedly exercise updater policy, POSIX swap/relaunch, archive metadata,
# signed feed generation, and tamper rejection on macOS or Linux.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ITERATIONS="${ITERATIONS:-25}"

if ! [[ "$ITERATIONS" =~ ^[1-9][0-9]*$ ]]; then
  echo "ITERATIONS must be a positive integer" >&2
  exit 2
fi

for ((iteration = 1; iteration <= ITERATIONS; ++iteration)); do
  "$ROOT/scripts/run-host-tests.sh" >/dev/null
  python3 "$ROOT/scripts/test-updater.py" >/dev/null
  if ((iteration % 5 == 0 || iteration == ITERATIONS)); then
    echo "updater stress: $iteration/$ITERATIONS passed"
  fi
done

echo "updater stress passed: $ITERATIONS iterations"
