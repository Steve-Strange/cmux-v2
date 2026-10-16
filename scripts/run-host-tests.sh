#!/bin/bash
# Run cmux_term pure-model host tests without a Chromium checkout or gtest.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TEST_DIR="$ROOT/overlay/chrome/browser/cmux_term"
TERMINAL_HOST_TEST_DIR="$ROOT/overlay/chrome/services/cmux_terminal_renderer/public/cpp"

python3 "$ROOT/scripts/test_sentry_crashpad.py"
TMP_OUT="$(mktemp -d)"
trap 'rm -rf "$TMP_OUT"' EXIT

CXX="${CXX:-c++}"
# GCC treats a documentation-only trailing backslash in a // build example as
# a continued comment and promotes it under -Werror; Clang does not. Keep the
# cross-platform gate focused on compiled code until those historical examples
# are rewritten.
CXXFLAGS=(-std=c++17 -Wall -Wextra -Werror -Wno-comment -pthread -I "$ROOT/overlay")

tests=(
  cmux_extension_slots_test.cc
  cmux_easing_test.cc
  cmux_keymap_test.cc
  cmux_renderer_ordering_test.cc
  cmux_terminal_recovery_test.cc
  cmux_terminal_host_connection_test.cc
  cmux_ghostty_resources_test.cc
  cmux_terminal_input_cutover_test.cc
  cmux_terminal_host_protocol_test.cc
  cmux_terminal_host_socket_test.cc
  cmux_terminal_host_stream_test.cc
  cmux_terminal_placement_test.cc
  cmux_tui_protocol_test.cc
  cmux_workspace_projection_test.cc
  cmux_native_window_registry_test.cc
  cmux_new_tab_page_test.cc
  cmux_sidebar_metrics_test.cc
  cmux_theme_test.cc
  cmux_telemetry_model_test.cc
  cmux_update_model_test.cc
  cmux_update_script_test.cc
  window_model_test.cc
  window_layout_test.cc
)

set_sources_for_test() {
  case "$1" in
    cmux_extension_slots_test.cc)
      sources=(
        "$TEST_DIR/cmux_extension_slots.cc" \
        "$TEST_DIR/cmux_extension_slots_test.cc"
      )
      ;;
    cmux_easing_test.cc)
      sources=(
        "$TEST_DIR/cmux_easing.cc"
        "$TEST_DIR/cmux_easing_test.cc"
      )
      ;;
    cmux_keymap_test.cc)
      sources=(
        "$TEST_DIR/cmux_keymap.cc"
        "$TEST_DIR/cmux_keymap_test.cc"
      )
      ;;
    cmux_renderer_ordering_test.cc)
      sources=(
        "$TEST_DIR/cmux_renderer_ordering_test.cc"
      )
      ;;
    cmux_terminal_recovery_test.cc)
      sources=(
        "$TERMINAL_HOST_TEST_DIR/cmux_terminal_host_protocol.cc"
        "$TEST_DIR/cmux_terminal_recovery.cc"
        "$TEST_DIR/cmux_terminal_recovery_test.cc"
      )
      ;;
    cmux_terminal_host_connection_test.cc)
      sources=(
        "$TERMINAL_HOST_TEST_DIR/cmux_terminal_host_protocol.cc"
        "$TEST_DIR/cmux_terminal_host_connection.cc"
        "$TEST_DIR/cmux_terminal_host_connection_test.cc"
      )
      ;;
    cmux_ghostty_resources_test.cc)
      sources=(
        "$TERMINAL_HOST_TEST_DIR/cmux_ghostty_resources.cc"
        "$TERMINAL_HOST_TEST_DIR/cmux_ghostty_resources_test.cc"
      )
      ;;
    cmux_terminal_input_cutover_test.cc)
      sources=(
        "$TERMINAL_HOST_TEST_DIR/cmux_terminal_input_cutover.cc"
        "$TERMINAL_HOST_TEST_DIR/cmux_terminal_input_cutover_test.cc"
      )
      ;;
    cmux_terminal_host_protocol_test.cc)
      sources=(
        "$TERMINAL_HOST_TEST_DIR/cmux_terminal_host_protocol.cc"
        "$TERMINAL_HOST_TEST_DIR/cmux_terminal_host_protocol_test.cc"
      )
      ;;
    cmux_terminal_host_socket_test.cc)
      sources=(
        "$TERMINAL_HOST_TEST_DIR/cmux_terminal_host_protocol.cc"
        "$TERMINAL_HOST_TEST_DIR/cmux_terminal_host_stream.cc"
        "$TERMINAL_HOST_TEST_DIR/cmux_terminal_host_socket.cc"
        "$TERMINAL_HOST_TEST_DIR/cmux_terminal_host_socket_test.cc"
      )
      ;;
    cmux_terminal_host_stream_test.cc)
      sources=(
        "$TERMINAL_HOST_TEST_DIR/cmux_terminal_host_protocol.cc"
        "$TERMINAL_HOST_TEST_DIR/cmux_terminal_host_stream.cc"
        "$TERMINAL_HOST_TEST_DIR/cmux_terminal_host_stream_test.cc"
      )
      ;;
    cmux_terminal_placement_test.cc)
      sources=(
        "$TEST_DIR/cmux_terminal_placement.cc"
        "$TEST_DIR/cmux_terminal_placement_test.cc"
      )
      ;;
    cmux_tui_protocol_test.cc)
      sources=(
        "$TEST_DIR/cmux_tui_protocol.cc"
        "$TEST_DIR/cmux_tui_protocol_test.cc"
      )
      ;;
    cmux_workspace_projection_test.cc)
      sources=(
        "$TEST_DIR/cmux_workspace_projection.cc"
        "$TEST_DIR/cmux_workspace_projection_test.cc"
      )
      ;;
    cmux_native_window_registry_test.cc)
      sources=(
        "$TEST_DIR/cmux_native_window_registry.cc"
        "$TEST_DIR/cmux_native_window_registry_test.cc"
      )
      ;;
    cmux_new_tab_page_test.cc)
      sources=(
        "$TEST_DIR/cmux_new_tab_page_test.cc"
      )
      ;;
    cmux_sidebar_metrics_test.cc)
      sources=(
        "$TEST_DIR/cmux_sidebar_metrics_test.cc"
      )
      ;;
    cmux_theme_test.cc)
      sources=(
        "$TEST_DIR/cmux_theme.cc"
        "$TEST_DIR/cmux_theme_test.cc"
      )
      ;;
    cmux_telemetry_model_test.cc)
      sources=(
        "$TEST_DIR/cmux_telemetry_model.cc"
        "$TEST_DIR/cmux_telemetry_model_test.cc"
      )
      ;;
    cmux_update_model_test.cc)
      sources=(
        "$TEST_DIR/cmux_update_model.cc"
        "$TEST_DIR/cmux_update_model_test.cc"
      )
      ;;
    cmux_update_script_test.cc)
      sources=(
        "$TEST_DIR/cmux_update_script.cc"
        "$TEST_DIR/cmux_update_script_test.cc"
      )
      ;;
    window_model_test.cc)
      sources=(
        "$TEST_DIR/window_model.cc"
        "$TEST_DIR/window_model_test.cc"
      )
      ;;
    window_layout_test.cc)
      sources=(
        "$TEST_DIR/window_model.cc"
        "$TEST_DIR/window_layout.cc"
        "$TEST_DIR/window_layout_test.cc"
      )
      ;;
    *)
      return 1
      ;;
  esac
}

is_known_test() {
  local test="$1"
  local known
  for known in "${tests[@]}"; do
    if [ "$test" = "$known" ]; then
      return 0
    fi
  done
  return 1
}

shopt -s nullglob
discovered=("$TEST_DIR"/*_test.cc "$TERMINAL_HOST_TEST_DIR"/*_test.cc)
shopt -u nullglob

for path in "${discovered[@]}"; do
  test="$(basename "$path")"
  if ! is_known_test "$test"; then
    echo "ERROR: host test is not covered by scripts/run-host-tests.sh: $path" >&2
    exit 1
  fi
done

passed=0
failed=0

for test in "${tests[@]}"; do
  test_path="$TEST_DIR/$test"
  if [ "$test" = "cmux_terminal_host_protocol_test.cc" ] ||
     [ "$test" = "cmux_ghostty_resources_test.cc" ] ||
     [ "$test" = "cmux_terminal_input_cutover_test.cc" ] ||
     [ "$test" = "cmux_terminal_host_socket_test.cc" ] ||
     [ "$test" = "cmux_terminal_host_stream_test.cc" ]; then
    test_path="$TERMINAL_HOST_TEST_DIR/$test"
  fi
  bin="$TMP_OUT/${test%.cc}"

  if [ ! -f "$test_path" ]; then
    echo "FAIL $test (missing test file)"
    failed=$((failed + 1))
    continue
  fi

  sources=()
  set_sources_for_test "$test"
  if "$CXX" "${CXXFLAGS[@]}" "${sources[@]}" -o "$bin" && "$bin"; then
    echo "PASS $test"
    passed=$((passed + 1))
  else
    echo "FAIL $test"
    failed=$((failed + 1))
  fi
done

if "$ROOT/scripts/test-cmux-tui-artifact.sh"; then
  echo "PASS test-cmux-tui-artifact.sh"
  passed=$((passed + 1))
else
  echo "FAIL test-cmux-tui-artifact.sh"
  failed=$((failed + 1))
fi

if "$ROOT/scripts/test-cmux-runtime-pins.sh"; then
  echo "PASS test-cmux-runtime-pins.sh"
  passed=$((passed + 1))
else
  echo "FAIL test-cmux-runtime-pins.sh"
  failed=$((failed + 1))
fi

if python3 "$ROOT/scripts/terminal-parity-probe.py" --self-test; then
  echo "PASS terminal-parity-probe.py --self-test"
  passed=$((passed + 1))
else
  echo "FAIL terminal-parity-probe.py --self-test"
  failed=$((failed + 1))
fi

if python3 "$ROOT/scripts/xcuitest-cmux-session.py" --self-test; then
  echo "PASS xcuitest-cmux-session.py --self-test"
  passed=$((passed + 1))
else
  echo "FAIL xcuitest-cmux-session.py --self-test"
  failed=$((failed + 1))
fi

if /bin/bash -n "$ROOT/tools/xcuitest-run.sh" &&
   /bin/bash "$ROOT/tools/xcuitest-run.sh" --shell-self-test; then
  echo "PASS xcuitest-run.sh /bin/bash compatibility"
  passed=$((passed + 1))
else
  echo "FAIL xcuitest-run.sh /bin/bash compatibility"
  failed=$((failed + 1))
fi

echo "Summary: $passed passed, $failed failed"

if [ "$failed" -ne 0 ]; then
  exit 1
fi
