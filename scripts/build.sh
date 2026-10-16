#!/bin/bash
# Build the full chrome target on the builder. Warm incremental = minutes.
#
# Failure-proof by construction: the remote autoninja's exit code is captured
# explicitly (NOT by piping autoninja through grep/tail, which swallows it,
# which produced false-green builds three separate times; see memory
# builder-reset-before-apply). The script prints CMUX_BUILD_OK or
# CMUX_BUILD_FAILED and exits nonzero on failure, so callers can gate on either
# the marker or the exit code. Logs and temporary files stay inside the
# isolated workspace instead of competing for the builder's system volume.
set -euo pipefail
. "$(dirname "$0")/builder-transport.sh"
HOST="${CMUX_BUILDER:-ec2-user@aws-m4pro-1}"
SRC="${CHROMIUM_SRC:-/Volumes/recpass123/cmux-ci/cmux-browser-benchmark/src}"
DEPOT_TOOLS="${CMUX_DEPOT_TOOLS:-/Volumes/recpass123/cmux-ci/cmux-browser-benchmark/depot_tools}"
"$CMUX_SCRIPTS_DIR/set-alloc-args.sh"
TARGET="${CMUX_BUILD_TARGET:-chrome}"
ssh -o BatchMode=yes "$HOST" /bin/bash -s -- \
  "$DEPOT_TOOLS" "$SRC" "$TARGET" <<'REMOTE_BUILD'
  set -u -o pipefail
  DEPOT_TOOLS="$1"
  SRC="$2"
  TARGET="$3"
  export PATH="$DEPOT_TOOLS:$HOME/depot_tools:$PATH"
  cd "$SRC" || exit 90
  log='out/Release/cmux_build.log'
  crash_log='out/Release/cmux_build.siso-crash.log'
  mkdir -p out/Release/cmux-tmp
  export TMPDIR="$PWD/out/Release/cmux-tmp"
  run_build() {
    autoninja -C out/Release "$TARGET" > "$log" 2>&1 &
    build_pid=$!
    next_progress=10
    while kill -0 "$build_pid" 2>/dev/null; do
      sleep "${CMUX_BUILD_PROGRESS_INTERVAL:-15}"
      marker=$(tail -n 2000 "$log" | grep -aEo '\[[[:space:]]*[0-9]+/[[:space:]]*[0-9]+\]' | tail -1 || true)
      if [ -n "$marker" ]; then
        values=$(printf '%s' "$marker" | tr -d '[] ')
        done_count=${values%/*}
        total_count=${values#*/}
        if [ "${total_count:-0}" -gt 0 ]; then
          percent=$((done_count * 100 / total_count))
          if [ "$percent" -ge "$next_progress" ]; then
            printf '%s CMUX_BUILD_PROGRESS percent=%s\n' "$marker" "$percent"
            while [ "$next_progress" -le "$percent" ]; do
              next_progress=$((next_progress + 10))
            done
          fi
        fi
      fi
    done
    wait "$build_pid"
  }
  run_build
  rc=$?
  if [ "$rc" -ne 0 ] && grep -q 'SIGABRT: abort' "$log"; then
    cp "$log" "$crash_log"
    echo 'Siso aborted; retrying once with the completed object cache'
    run_build
    rc=$?
  fi
  summary_log='out/Release/cmux_build.summary.log'
  grep -aE 'error:|FAILED|build stopped|Build Succeeded|Build Failure|ninja: Entering' \
    "$log" > "$summary_log" || true
  tail -12 "$summary_log"
  if [ "$rc" -eq 0 ]; then echo CMUX_BUILD_OK; else
    echo '--- last 20 log lines ---'; tail -20 "$log"
    echo CMUX_BUILD_FAILED
  fi
  exit "$rc"
REMOTE_BUILD
