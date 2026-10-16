#!/bin/bash
# Launch the deployed cmux-browser under lldb so any crash is captured with a
# full backtrace to /tmp/cmux-crash-<timestamp>.log (our build is symbol_level=0
# but keeps function names in the symtab, so stacks are readable). The app now
# defaults to the cmux Views UI and ~/.cmux-profile; this script just adds lldb,
# stderr logging, and optional DevTools extras.
#
#   bash ~/fun/cmux-browser/scripts/run.sh                 # normal run, crash-captured
#   CMUX_NO_LLDB=1 bash ~/fun/cmux-browser/scripts/run.sh  # run without the debugger
#
# When it crashes, tell me and I'll read the crash log + the run log.
set -uo pipefail
cd "$(dirname "$0")/.."

APP_BUNDLE="$PWD/dist/cmux-browser.app"
APP_EXECUTABLE="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleExecutable' \
  "$APP_BUNDLE/Contents/Info.plist")"
APP="$APP_BUNDLE/Contents/MacOS/$APP_EXECUTABLE"
# Persistent by default: the app also defaults to this profile for plain
# Finder/Dock launches. Passing it here remains intentional so CMUX_PROFILE can
# still override it for throwaway runs/selftests.
PROFILE="${CMUX_PROFILE:-$HOME/.cmux-profile}"
PORT="${CMUX_PORT:-9333}"
TS="$(date +%Y%m%d-%H%M%S)"
RUNLOG="/tmp/cmux-run-$TS.log"
mkdir -p "$PROFILE"

ARGS=(
  --user-data-dir="$PROFILE"
  --enable-logging=stderr --log-level=0
  --no-first-run --no-default-browser-check
)

# DevTools/remote-debugging is OFF by default (it's a detection vector and only
# needed for agent-side CDP verification). Opt in with CMUX_DEBUG_PORT=1.
if [ "${CMUX_DEBUG_PORT:-0}" = "1" ]; then
  ARGS+=( --remote-debugging-port="$PORT" --remote-allow-origins="http://127.0.0.1:$PORT" )
  echo "devtools: http://127.0.0.1:$PORT/json"
fi

echo "log: $RUNLOG  (run output + crash backtrace; grep 'stop reason' / 'frame #')"

# cmux Views is default-on in the app; this remains harmless for older builds.
# Use CMUX_VIEWS=0 to debug stock Chromium startup.
export CMUX_VIEWS="${CMUX_VIEWS:-1}"

if [ "${CMUX_NO_LLDB:-0}" = "1" ]; then
  # NOT exec: exec on the left of a pipe runs in a subshell, so the script
  # would continue past the fi and ALSO launch the lldb instance after the
  # app exits. Run the pipeline, then exit with the app's status.
  "$APP" "${ARGS[@]}" 2>&1 | tee "$RUNLOG"
  exit "${PIPESTATUS[0]}"
fi

# lldb batch: run the app; on a crash (Mach exception / fatal signal) the -k
# commands dump all thread backtraces, then quit. Everything (app stderr logs +
# lldb's backtrace) goes to one combined log. SIGPIPE is passed through
# (Chromium uses it internally) so lldb doesn't stop on it.
lldb --batch \
  -o "process handle SIGPIPE -n true -p true -s false" \
  -o "process handle SIGTERM SIGINT SIGHUP -n true -p true -s false" \
  -o "run" \
  -k "bt all" \
  -k "quit" \
  -- "$APP" "${ARGS[@]}" 2>&1 | tee "$RUNLOG"
