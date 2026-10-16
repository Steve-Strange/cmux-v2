#!/bin/bash
# Deterministic XCUITest E2E. By default this launches the locally deployed app
# with CDP for the legacy keyboard suite. Set CMUX_XCUI_EXTERNAL_APP=1 to drive
# an installed signed dogfood bundle named by CMUX_XCUI_BUNDLE_ID; the terminal
# render/color suite owns that app's clean launch and does not require CDP.
# Requires (one time):
#   sudo automationmodetool enable-automationmode-without-authentication
set -uo pipefail
cd "$(dirname "$0")/.."
ROOT="$PWD"
# The UI-test runner is sandboxed and its current directory resolves inside
# the xctrunner container. Publish this checkout by default so source-backed
# probes never accidentally resolve relative to that container. Callers may
# still override the root when intentionally testing another source snapshot.
CMUX_XCUI_REPO_ROOT="${CMUX_XCUI_REPO_ROOT:-$ROOT}"
export CMUX_XCUI_REPO_ROOT
PORT="${CMUX_PORT:-9300}"
if [ -n "${CMUX_XCUI_APP:-}" ]; then
  CMUX_XCUI_APP_PATH="${CMUX_XCUI_APP_PATH:-$CMUX_XCUI_APP}"
  CMUX_XCUI_EXTERNAL_APP=1
  export CMUX_XCUI_APP_PATH CMUX_XCUI_EXTERNAL_APP
fi
if [ "${CMUX_XCUI_ONLY_SIDEBAR:-0}" = "1" ] &&
   [ -z "${CMUX_XCUI_ONLY_TESTING:-}" ]; then
  CMUX_XCUI_ONLY_TESTING=\
"CmuxUITests/CmuxKeyboardUITests/testSidebarModifierClickSelection"
  export CMUX_XCUI_ONLY_TESTING
fi
SHELL_SELF_TEST=0
if [ "${1:-}" = "--shell-self-test" ]; then
  SHELL_SELF_TEST=1
  shift
  if [ "$#" -ne 0 ]; then
    echo "--shell-self-test does not accept additional arguments" >&2
    exit 2
  fi
fi
if [ "$SHELL_SELF_TEST" -eq 1 ]; then
  RUN_TOKEN="000000000000000000000000"
  PYTHON3=""
else
  RUN_TOKEN="${CMUX_XCUI_RUN_TOKEN:-$(uuidgen | tr -d '-' | tr '[:upper:]' '[:lower:]')}"
  if ! [[ "$RUN_TOKEN" =~ ^[a-f0-9]{24,64}$ ]]; then
    echo "CMUX_XCUI_RUN_TOKEN must be 24-64 hex digits" >&2
    exit 2
  fi
  export CMUX_XCUI_RUN_TOKEN="$RUN_TOKEN"
  PYTHON3="$(command -v python3 2>/dev/null || true)"
  if [ -z "$PYTHON3" ] || [[ "$PYTHON3" != /* ]] || [ ! -x "$PYTHON3" ]; then
    echo "Could not resolve an absolute executable python3" >&2
    exit 2
  fi
fi
CRASHPAD_APP_PATH=""
PREEXISTING_CRASHPAD_PIDS=()
BROKER_PID=""
BROKER_CONFIG="/tmp/cmux-xcui-broker-$RUN_TOKEN.json"
BROKER_LOG="/tmp/cmux-xcui-broker-$RUN_TOKEN.log"
SESSION_MANIFESTS=()
SESSION_MANIFEST_TEST_NAMES=()
SESSION_MANIFEST_PATHS=()
LEGACY_APP_PID=""
LEGACY_APP_EXECUTABLE=""
LEGACY_PROFILE=""
APP_PID_SELECTOR="$RUN_TOKEN:"

remember_preexisting_crashpad_pid() {
  local index="${#PREEXISTING_CRASHPAD_PIDS[@]}"
  PREEXISTING_CRASHPAD_PIDS[$index]="$1"
}

is_preexisting_crashpad_pid() {
  local expected="$1"
  local index
  for ((index = 0; index < ${#PREEXISTING_CRASHPAD_PIDS[@]}; index++)); do
    [ "${PREEXISTING_CRASHPAD_PIDS[$index]}" = "$expected" ] && return 0
  done
  return 1
}

session_manifest_for_test() {
  local expected="$1"
  local index
  [ "${#SESSION_MANIFEST_TEST_NAMES[@]}" -eq \
    "${#SESSION_MANIFEST_PATHS[@]}" ] || return 2
  for ((index = 0; index < ${#SESSION_MANIFEST_TEST_NAMES[@]}; index++)); do
    if [ "${SESSION_MANIFEST_TEST_NAMES[$index]}" = "$expected" ]; then
      printf '%s\n' "${SESSION_MANIFEST_PATHS[$index]}"
      return 0
    fi
  done
  return 1
}

remember_session_manifest() {
  local test_name="$1"
  local manifest="$2"
  local index
  if session_manifest_for_test "$test_name" >/dev/null; then
    echo "duplicate XCUITest session manifest for $test_name" >&2
    return 1
  fi
  index="${#SESSION_MANIFEST_TEST_NAMES[@]}"
  SESSION_MANIFEST_TEST_NAMES[$index]="$test_name"
  SESSION_MANIFEST_PATHS[$index]="$manifest"
}

run_shell_self_test() {
  remember_preexisting_crashpad_pid 101
  remember_preexisting_crashpad_pid 202
  if ! is_preexisting_crashpad_pid 101 ||
     ! is_preexisting_crashpad_pid 202 ||
     is_preexisting_crashpad_pid 10 ||
     is_preexisting_crashpad_pid 2020; then
    echo "indexed crashpad PID ownership lookup is not exact" >&2
    return 1
  fi

  remember_session_manifest "mouse-events" "/tmp/owned manifest mouse"
  remember_session_manifest "wheel-counter" "/tmp/owned manifest wheel"
  remember_session_manifest "window-resize" "/tmp/owned manifest resize"
  if [ "$(session_manifest_for_test mouse-events)" != \
       "/tmp/owned manifest mouse" ] ||
     [ "$(session_manifest_for_test wheel-counter)" != \
       "/tmp/owned manifest wheel" ] ||
     [ "$(session_manifest_for_test window-resize)" != \
       "/tmp/owned manifest resize" ] ||
     session_manifest_for_test "mouse" >/dev/null; then
    echo "indexed session manifest lookup is not exact" >&2
    return 1
  fi
  if remember_session_manifest "mouse-events" "/tmp/replacement" \
       >/dev/null 2>&1; then
    echo "duplicate session manifest was accepted" >&2
    return 1
  fi
  echo "xcuitest runner shell self-test: PASS"
}

if [ "$SHELL_SELF_TEST" -eq 1 ]; then
  run_shell_self_test
  exit $?
fi

selector_matches() {
  local path="$1"
  local expected="$2"
  local current
  [ -f "$path" ] || return 1
  current="$(<"$path")" || return 1
  [ "$current" = "$expected" ]
}

remove_matching_selector() {
  local path="$1"
  local expected="$2"
  selector_matches "$path" "$expected" || return 0
  rm -f -- "$path"
}

crashpad_pids_for_app() {
  [ -n "$CRASHPAD_APP_PATH" ] || return 0
  local pid command
  while read -r pid command; do
    [[ "$pid" =~ ^[0-9]+$ ]] || continue
    case "$command" in
      "$CRASHPAD_APP_PATH"/Contents/Frameworks/*/Helpers/chrome_crashpad_handler\ *)
        printf '%s\n' "$pid"
        ;;
    esac
  done < <(ps -axo pid=,command=)
}

is_owned_crashpad_pid() {
  local pid="$1"
  local command
  [[ "$pid" =~ ^[0-9]+$ ]] || return 1
  command="$(ps -p "$pid" -o command= 2>/dev/null || true)"
  case "$command" in
    "$CRASHPAD_APP_PATH"/Contents/Frameworks/*/Helpers/chrome_crashpad_handler\ *) return 0 ;;
  esac
  return 1
}

is_owned_broker_pid() {
  local pid="$1"
  local command
  [[ "$pid" =~ ^[0-9]+$ ]] || return 1
  command="$(ps -p "$pid" -o command= 2>/dev/null || true)"
  case "$command" in
    *"$ROOT/scripts/xcuitest-cmux-session.py serve-broker --run-token $RUN_TOKEN "*)
      return 0
      ;;
  esac
  return 1
}

broker_pid_is_running() {
  local pid="$1"
  local state
  kill -0 "$pid" 2>/dev/null || return 1
  state="$(ps -p "$pid" -o state= 2>/dev/null || true)"
  [ -n "$state" ] && [[ "$state" != Z* ]]
}

legacy_app_pid_is_owned() {
  local pid="$1"
  local command
  [ -n "$LEGACY_APP_EXECUTABLE" ] && [ -n "$LEGACY_PROFILE" ] || return 1
  [[ "$pid" =~ ^[0-9]+$ ]] || return 1
  command="$(ps -p "$pid" -o command= 2>/dev/null || true)"
  case "$command" in
    "$LEGACY_APP_EXECUTABLE"\ *"--user-data-dir=$LEGACY_PROFILE"*) return 0 ;;
  esac
  return 1
}

stop_legacy_app() {
  local pid="$LEGACY_APP_PID"
  [ -n "$pid" ] || return 0
  if legacy_app_pid_is_owned "$pid"; then
    kill -TERM "$pid" 2>/dev/null || true
  fi
  for _ in $(seq 1 40); do
    legacy_app_pid_is_owned "$pid" || break
    sleep 0.05
  done
  if legacy_app_pid_is_owned "$pid"; then
    kill -KILL "$pid" 2>/dev/null || true
  fi
  wait "$pid" 2>/dev/null || true
  LEGACY_APP_PID=""
}

stop_broker() {
  local pid="$BROKER_PID"
  [ -n "$pid" ] || return 0
  if [ -s "$BROKER_CONFIG" ]; then
    "$PYTHON3" "$ROOT/scripts/xcuitest-cmux-session.py" broker-request \
      --broker-config "$BROKER_CONFIG" --operation shutdown --timeout 2 \
      >/dev/null 2>&1 || true
  fi
  for _ in $(seq 1 40); do
    broker_pid_is_running "$pid" || break
    sleep 0.05
  done
  if broker_pid_is_running "$pid" && is_owned_broker_pid "$pid"; then
    kill -TERM "$pid" 2>/dev/null || true
  fi
  for _ in $(seq 1 40); do
    broker_pid_is_running "$pid" || break
    sleep 0.05
  done
  if broker_pid_is_running "$pid" && is_owned_broker_pid "$pid"; then
    kill -KILL "$pid" 2>/dev/null || true
  fi
  wait "$pid" 2>/dev/null || true
  BROKER_PID=""
}

cleanup_xcui() {
  local incoming_status="${1:-0}"
  local cleanup_status=0
  local prefix="cmux-xcui-session-${RUN_TOKEN:0:12}-"
  local manifest
  trap - EXIT HUP INT TERM

  # Serialize behind any cleanup request that XCTest already sent. Stopping
  # the broker before the direct fallback prevents two teardown clients from
  # racing on the same topology after an interrupted test runner.
  if [ -s "$BROKER_CONFIG" ]; then
    "$PYTHON3" "$ROOT/scripts/xcuitest-cmux-session.py" broker-request \
      --broker-config "$BROKER_CONFIG" --operation cleanup-all --timeout 30 \
      >/dev/null 2>&1 || true
  fi
  stop_broker
  stop_legacy_app

  while IFS= read -r -d '' manifest; do
    if ! "$PYTHON3" "$ROOT/scripts/xcuitest-cmux-session.py" cleanup \
      --manifest "$manifest" --timeout 10; then
      echo "failed to clean owned XCUITest cmux session: $manifest" >&2
      cleanup_status=1
    fi
  done < <(find /private/tmp -maxdepth 2 -type f \
    -path "/private/tmp/${prefix}*/ownership.json" -print0)

  # Crashpad handlers reparent to launchd before Chromium exits. Preserve every
  # pre-existing handler and only terminate PIDs created during this harness
  # run whose live executable command still belongs to the exact tested app.
  local pid
  while IFS= read -r pid; do
    is_preexisting_crashpad_pid "$pid" && continue
    is_owned_crashpad_pid "$pid" || continue
    kill -TERM "$pid" 2>/dev/null || true
    for _ in $(seq 1 40); do
      is_owned_crashpad_pid "$pid" || break
      sleep 0.05
    done
    if is_owned_crashpad_pid "$pid"; then
      kill -KILL "$pid" 2>/dev/null || true
    fi
  done < <(crashpad_pids_for_app)

  # Only remove deterministic selector files if this run still owns the global
  # run-token selector. A later concurrently-started harness wins ownership;
  # this exiting run must not erase its configuration.
  if selector_matches /tmp/cmux-xcui-run-token "$RUN_TOKEN"; then
    remove_matching_selector /tmp/cmux-xcui-python3 "$PYTHON3"
    [ -z "${CMUX_XCUI_BUNDLE_ID:-}" ] || \
      remove_matching_selector /tmp/cmux-xcui-bundle-id "$CMUX_XCUI_BUNDLE_ID"
    [ -z "${CMUX_XCUI_REPO_ROOT:-}" ] || \
      remove_matching_selector /tmp/cmux-xcui-repo-root "$CMUX_XCUI_REPO_ROOT"
    [ -z "${CMUX_XCUI_GHOSTTY_BUNDLE_ID:-}" ] || \
      remove_matching_selector /tmp/cmux-xcui-ghostty-bundle-id \
        "$CMUX_XCUI_GHOSTTY_BUNDLE_ID"
    [ -z "${CMUX_XCUI_GHOSTTY_APP_PATH:-}" ] || \
      remove_matching_selector /tmp/cmux-xcui-ghostty-app-path \
        "$CMUX_XCUI_GHOSTTY_APP_PATH"
    [ -z "${CMUX_XCUI_APP_PATH:-}" ] || \
      remove_matching_selector /tmp/cmux-xcui-app-path "$CMUX_XCUI_APP_PATH"
    remove_matching_selector /tmp/cmux-xcui-app-pid "$APP_PID_SELECTOR"
    local test_name expected_manifest
    for test_name in \
      render-burst wheel-counter physical-typing mouse-events color-parity attached-ghostty \
      window-resize; do
      expected_manifest="$(session_manifest_for_test "$test_name" || true)"
      [ -z "$expected_manifest" ] || \
        remove_matching_selector "/tmp/cmux-xcui-manifest-$test_name" \
          "$expected_manifest"
    done
    remove_matching_selector /tmp/cmux-xcui-run-token "$RUN_TOKEN"
  fi
  rm -f "$BROKER_CONFIG" "$BROKER_LOG"

  if [ "$incoming_status" -eq 0 ] && [ "$cleanup_status" -ne 0 ]; then
    incoming_status=3
  fi
  exit "$incoming_status"
}
trap 'cleanup_xcui $?' EXIT
trap 'cleanup_xcui 129' HUP
trap 'cleanup_xcui 130' INT
trap 'cleanup_xcui 143' TERM

printf '%s\n' "$RUN_TOKEN" > /tmp/cmux-xcui-run-token
printf '%s\n' "$PYTHON3" > /tmp/cmux-xcui-python3
# The standard runner fills in the PID after its exact owned launch. External
# app mode deliberately leaves the PID empty so the legacy keyboard tests skip
# instead of targeting an arbitrary process with the same bundle identifier.
printf '%s\n' "$APP_PID_SELECTOR" > /tmp/cmux-xcui-app-pid

# Xcode's UI-test runner does not reliably inherit arbitrary shell variables.
# Mirror the non-sensitive test selectors into deterministic /tmp files;
# the Swift suite prefers its environment and falls back to these values.
if [ -n "${CMUX_XCUI_BUNDLE_ID:-}" ]; then
  printf '%s\n' "$CMUX_XCUI_BUNDLE_ID" > /tmp/cmux-xcui-bundle-id
else
  rm -f /tmp/cmux-xcui-bundle-id
fi
if [ -n "${CMUX_XCUI_REPO_ROOT:-}" ]; then
  printf '%s\n' "$CMUX_XCUI_REPO_ROOT" > /tmp/cmux-xcui-repo-root
else
  rm -f /tmp/cmux-xcui-repo-root
fi
if [ -n "${CMUX_XCUI_GHOSTTY_BUNDLE_ID:-}" ]; then
  printf '%s\n' "$CMUX_XCUI_GHOSTTY_BUNDLE_ID" > /tmp/cmux-xcui-ghostty-bundle-id
else
  rm -f /tmp/cmux-xcui-ghostty-bundle-id
fi
if [ -n "${CMUX_XCUI_GHOSTTY_APP_PATH:-}" ]; then
  printf '%s\n' "$CMUX_XCUI_GHOSTTY_APP_PATH" \
    > /tmp/cmux-xcui-ghostty-app-path
else
  rm -f /tmp/cmux-xcui-ghostty-app-path
fi
if [ -n "${CMUX_XCUI_APP_PATH:-}" ]; then
  printf '%s\n' "$CMUX_XCUI_APP_PATH" > /tmp/cmux-xcui-app-path
else
  rm -f /tmp/cmux-xcui-app-path
fi

if [ "${CMUX_XCUI_EXTERNAL_APP:-0}" = "1" ]; then
  APP_PATH="${CMUX_XCUI_APP_PATH:-}"
  if [ -z "$APP_PATH" ]; then
    while IFS= read -r candidate; do
      [ -f "$candidate/Contents/Info.plist" ] || continue
      candidate_id="$(plutil -extract CFBundleIdentifier raw -o - \
        "$candidate/Contents/Info.plist" 2>/dev/null || true)"
      if [ "$candidate_id" = "${CMUX_XCUI_BUNDLE_ID:-com.cmux.app}" ]; then
        APP_PATH="$candidate"
        break
      fi
    done < <(
      find "$HOME/Applications" /Applications -maxdepth 1 -type d -name '*.app' \
        -print 2>/dev/null
    )
  fi
  if [ -z "$APP_PATH" ] || [ ! -x "$APP_PATH/Contents/Helpers/cmux-tui" ]; then
    echo "Could not resolve external app helper; set CMUX_XCUI_APP_PATH" >&2
    exit 2
  fi
  if [[ "$APP_PATH" != /* ]]; then
    echo "CMUX_XCUI_APP_PATH must be absolute" >&2
    exit 2
  fi
  APP_PATH="$(/bin/realpath "$APP_PATH")" || exit 2
  APP_BUNDLE_ID="$(plutil -extract CFBundleIdentifier raw -o - \
    "$APP_PATH/Contents/Info.plist" 2>/dev/null || true)"
  if [ -z "$APP_BUNDLE_ID" ] || \
      { [ -n "${CMUX_XCUI_BUNDLE_ID:-}" ] && \
        [ "$CMUX_XCUI_BUNDLE_ID" != "$APP_BUNDLE_ID" ]; }; then
    echo "CMUX_XCUI_APP_PATH bundle identifier does not match CMUX_XCUI_BUNDLE_ID" >&2
    exit 2
  fi
  export CMUX_XCUI_APP_PATH="$APP_PATH"
  export CMUX_XCUI_BUNDLE_ID="$APP_BUNDLE_ID"
  printf '%s\n' "$CMUX_XCUI_APP_PATH" > /tmp/cmux-xcui-app-path
  printf '%s\n' "$CMUX_XCUI_BUNDLE_ID" > /tmp/cmux-xcui-bundle-id

  # deploy.sh rewrites the dogfood bundle identifier in place. LaunchServices
  # can otherwise retain the source Chromium identifier until after the first
  # launch, causing XCUIApplication(url:) to launch the correct executable but
  # wait forever for a process under the stale identifier. Force-register the
  # exact tested path before Xcode creates its application proxy.
  LSREGISTER=/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister
  if [ ! -x "$LSREGISTER" ]; then
    echo "Could not resolve the system LaunchServices registrar" >&2
    exit 2
  fi
  if ! "$LSREGISTER" -f "$CMUX_XCUI_APP_PATH" >/dev/null; then
    echo "Could not register the exact external app with LaunchServices" >&2
    exit 2
  fi

  if [ -n "${CMUX_XCUI_GHOSTTY_APP_PATH:-}" ]; then
    if [[ "$CMUX_XCUI_GHOSTTY_APP_PATH" != /* ]]; then
      echo "CMUX_XCUI_GHOSTTY_APP_PATH must be absolute" >&2
      exit 2
    fi
    GHOSTTY_APP_PATH="$(/bin/realpath "$CMUX_XCUI_GHOSTTY_APP_PATH")" || exit 2
    GHOSTTY_INFO="$GHOSTTY_APP_PATH/Contents/Info.plist"
    GHOSTTY_BUNDLE_ID="$(plutil -extract CFBundleIdentifier raw -o - \
      "$GHOSTTY_INFO" 2>/dev/null || true)"
    GHOSTTY_EXECUTABLE="$(plutil -extract CFBundleExecutable raw -o - \
      "$GHOSTTY_INFO" 2>/dev/null || true)"
    if [ -z "$GHOSTTY_BUNDLE_ID" ] || [ -z "$GHOSTTY_EXECUTABLE" ] || \
        [ ! -x "$GHOSTTY_APP_PATH/Contents/MacOS/$GHOSTTY_EXECUTABLE" ] || \
        { [ -n "${CMUX_XCUI_GHOSTTY_BUNDLE_ID:-}" ] && \
          [ "$CMUX_XCUI_GHOSTTY_BUNDLE_ID" != "$GHOSTTY_BUNDLE_ID" ]; }; then
      echo "CMUX_XCUI_GHOSTTY_APP_PATH is not the configured executable bundle" >&2
      exit 2
    fi
    export CMUX_XCUI_GHOSTTY_APP_PATH="$GHOSTTY_APP_PATH"
    export CMUX_XCUI_GHOSTTY_BUNDLE_ID="$GHOSTTY_BUNDLE_ID"
    printf '%s\n' "$CMUX_XCUI_GHOSTTY_APP_PATH" \
      > /tmp/cmux-xcui-ghostty-app-path
    printf '%s\n' "$CMUX_XCUI_GHOSTTY_BUNDLE_ID" \
      > /tmp/cmux-xcui-ghostty-bundle-id
  fi
  CRASHPAD_APP_PATH="$APP_PATH"
  while IFS= read -r pid; do
    remember_preexisting_crashpad_pid "$pid"
  done < <(crashpad_pids_for_app)
  for test_name in \
    render-burst wheel-counter physical-typing mouse-events color-parity attached-ghostty \
    window-resize; do
    manifest="$("$PYTHON3" "$ROOT/scripts/xcuitest-cmux-session.py" prepare \
      --run-token "$RUN_TOKEN" \
      --binary "$APP_PATH/Contents/Helpers/cmux-tui" \
      --test-name "$test_name")" || exit 2
    SESSION_MANIFESTS+=("$manifest")
    remember_session_manifest "$test_name" "$manifest" || exit 2
    printf '%s\n' "$manifest" > "/tmp/cmux-xcui-manifest-$test_name"
  done

  BROKER_ARGS=(
    serve-broker
    --run-token "$RUN_TOKEN"
    --ready-file "$BROKER_CONFIG"
  )
  for manifest in "${SESSION_MANIFESTS[@]}"; do
    BROKER_ARGS+=(--manifest "$manifest")
  done
  "$PYTHON3" "$ROOT/scripts/xcuitest-cmux-session.py" "${BROKER_ARGS[@]}" \
    >"$BROKER_LOG" 2>&1 &
  BROKER_PID=$!
  for _ in $(seq 1 100); do
    [ -s "$BROKER_CONFIG" ] && break
    kill -0 "$BROKER_PID" 2>/dev/null || break
    sleep 0.03
  done
  if [ ! -s "$BROKER_CONFIG" ] || \
      ! "$PYTHON3" "$ROOT/scripts/xcuitest-cmux-session.py" broker-request \
        --broker-config "$BROKER_CONFIG" --operation ping --timeout 2 \
        >/dev/null; then
    echo "XCUITest session broker failed to start" >&2
    cat "$BROKER_LOG" >&2 2>/dev/null || true
    exit 2
  fi
fi

if [ "${CMUX_XCUI_EXTERNAL_APP:-0}" != "1" ]; then
  APP_PATH="${CMUX_XCUI_APP_PATH:-$ROOT/dist/cmux-browser.app}"
  if [[ "$APP_PATH" != /* ]]; then
    APP_PATH="$ROOT/$APP_PATH"
  fi
  APP_PATH="$(/bin/realpath "$APP_PATH" 2>/dev/null || true)"
  if [ -z "$APP_PATH" ] || [ ! -f "$APP_PATH/Contents/Info.plist" ]; then
    echo "Could not resolve the local app; set CMUX_XCUI_APP_PATH" >&2
    exit 2
  fi
  LEGACY_APP_EXECUTABLE_NAME="$(plutil -extract CFBundleExecutable raw -o - \
    "$APP_PATH/Contents/Info.plist" 2>/dev/null || true)"
  LEGACY_APP_EXECUTABLE="$APP_PATH/Contents/MacOS/$LEGACY_APP_EXECUTABLE_NAME"
  if [ -z "$LEGACY_APP_EXECUTABLE_NAME" ] ||
     [ ! -x "$LEGACY_APP_EXECUTABLE" ]; then
    echo "Local app executable is missing: $LEGACY_APP_EXECUTABLE" >&2
    exit 2
  fi
  EXISTING_LISTENERS="$(lsof -nP -tiTCP:"$PORT" -sTCP:LISTEN 2>/dev/null \
    | sort -u || true)"
  if [ -n "$EXISTING_LISTENERS" ]; then
    echo "CDP port $PORT is already owned by PID(s): $EXISTING_LISTENERS" >&2
    echo "Choose another CMUX_PORT; no existing process was terminated." >&2
    exit 2
  fi
  LEGACY_PROFILE="/tmp/cmux-xcui-profile-$RUN_TOKEN"
  mkdir -p "$LEGACY_PROFILE"
  CRASHPAD_APP_PATH="$APP_PATH"
  while IFS= read -r pid; do
    remember_preexisting_crashpad_pid "$pid"
  done < <(crashpad_pids_for_app)

  echo "== launching $APP_PATH (CDP :$PORT) =="
  CMUX_VIEWS="${CMUX_VIEWS:-1}" "$LEGACY_APP_EXECUTABLE" \
    "--user-data-dir=$LEGACY_PROFILE" \
    "--remote-debugging-port=$PORT" \
    "--remote-allow-origins=http://127.0.0.1:$PORT" \
    --no-first-run \
    --no-default-browser-check \
    > /tmp/cmux-xcui.log 2>&1 &
  LEGACY_APP_PID=$!
  for _ in $(seq 1 30); do
    curl -s --max-time 1 "http://127.0.0.1:$PORT/json" >/dev/null 2>&1 && break
    if ! legacy_app_pid_is_owned "$LEGACY_APP_PID"; then
      echo "cmux exited before CDP became ready" >&2
      tail -40 /tmp/cmux-xcui.log >&2
      exit 1
    fi
    sleep 1
  done
  if ! curl -s --max-time 2 "http://127.0.0.1:$PORT/json" >/dev/null 2>&1; then
    echo "cmux failed to come up on :$PORT"; tail -20 /tmp/cmux-xcui.log; exit 1
  fi
  LISTENER_PIDS="$(lsof -nP -tiTCP:"$PORT" -sTCP:LISTEN 2>/dev/null \
    | sort -u)"
  if [ "$LISTENER_PIDS" != "$LEGACY_APP_PID" ]; then
    echo "CDP listener does not belong to this exact launch: $LISTENER_PIDS" >&2
    exit 1
  fi
  CMUX_XCUI_APP_PID="$LEGACY_APP_PID"
  export CMUX_XCUI_APP_PID
  APP_PID_SELECTOR="$RUN_TOKEN:$CMUX_XCUI_APP_PID"
  printf '%s\n' "$APP_PID_SELECTOR" > /tmp/cmux-xcui-app-pid
  echo "cmux up: PID $LEGACY_APP_PID"
fi

echo "== generating + running XCUITest =="
cd tests/xcuitest
/opt/homebrew/bin/xcodegen generate >/dev/null
XCODE_ARGS=()
if [ -n "${CMUX_XCUI_ONLY_TESTING:-}" ]; then
  XCODE_ARGS+=("-only-testing:$CMUX_XCUI_ONLY_TESTING")
fi
DEVELOPMENT_TEAM="${CMUX_XCUI_DEVELOPMENT_TEAM:-}"
if [ -z "$DEVELOPMENT_TEAM" ]; then
  CERT_SUBJECT="$(security find-certificate -c "Apple Development" -p 2>/dev/null \
    | openssl x509 -noout -subject -nameopt RFC2253 2>/dev/null || true)"
  DEVELOPMENT_TEAM="$(printf '%s\n' "$CERT_SUBJECT" \
    | sed -n 's/.*OU=\([^,]*\).*/\1/p' | head -1)"
fi
if [ -z "$DEVELOPMENT_TEAM" ]; then
  echo "No Apple Development team found; set CMUX_XCUI_DEVELOPMENT_TEAM" >&2
  exit 2
fi
XCODE_ARGS+=("DEVELOPMENT_TEAM=$DEVELOPMENT_TEAM")
rm -rf /tmp/cmux-xcui-result
xcrun xcodebuild test \
  -project CmuxUITests.xcodeproj \
  -scheme CmuxUITests \
  -destination 'platform=macOS' \
  -resultBundlePath /tmp/cmux-xcui-result \
  "${XCODE_ARGS[@]}" \
  2>&1 | grep -iE "Test Case|passed|failed|error:|XCTAssert|cmux|omnibox|DevTools|Ctrl-C|htop|palette|resize|\*\* TEST" | tail -80
XCODE_STATUS=$?

# XCTRunner is sandboxed from writing arbitrary /tmp artifacts. Preserve its
# lossless screenshots and text diagnostics by exporting kept attachments from
# the result bundle after the test process exits.
rm -rf /tmp/cmux-xcui-attachments
xcrun xcresulttool export attachments \
  --path /tmp/cmux-xcui-result \
  --output-path /tmp/cmux-xcui-attachments >/dev/null 2>&1 || true

exit "$XCODE_STATUS"
