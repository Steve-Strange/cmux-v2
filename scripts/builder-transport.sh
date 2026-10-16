#!/usr/bin/env bash
# Source this from macOS builder scripts. It makes both shell commands and
# rsync use Tailscale SSH, with no dependency on ~/.ssh/config or SSH keys.

CMUX_SCRIPTS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CMUX_TAILSCALE_SSH="${CMUX_TAILSCALE_SSH:-$CMUX_SCRIPTS_DIR/tailscale-ssh.sh}"
CMUX_BUILDER="${CMUX_BUILDER:-ec2-user@aws-m4pro-1}"
CMUX_CHROMIUM_BASE="${CMUX_CHROMIUM_BASE:-/Volumes/recpass123/cmux-ci/cmux-browser-benchmark}"
CMUX_DEPOT_TOOLS="${CMUX_DEPOT_TOOLS:-$CMUX_CHROMIUM_BASE/depot_tools}"
CHROMIUM_SRC="${CHROMIUM_SRC:-$CMUX_CHROMIUM_BASE/src}"
export CMUX_BUILDER CMUX_CHROMIUM_BASE CMUX_DEPOT_TOOLS CMUX_TAILSCALE_SSH CHROMIUM_SRC
export RSYNC_RSH="${RSYNC_RSH:-$CMUX_TAILSCALE_SSH}"

ssh() {
  local -a options=()
  local -a transport=()
  while [ "$#" -gt 0 ] && [[ "$1" == -* ]]; do
    case "$1" in
      -o|-l) options+=("$1" "$2"); shift 2 ;;
      *) options+=("$1"); shift ;;
    esac
  done

  local host="${1:-}"
  [ -n "$host" ] || { echo "ssh: host is required" >&2; return 2; }
  shift
  local remote_argc="$#"

  transport=("$CMUX_TAILSCALE_SSH")
  if [ "${#options[@]}" -gt 0 ]; then
    transport+=("${options[@]}")
  fi
  transport+=("$host")

  if [ "$#" -eq 0 ]; then
    "${transport[@]}"
    return
  fi

  local command="" raw_command="" quoted marker rc_file line remote_rc="" transport_rc
  if [ "$#" -eq 1 ]; then
    command="$1"
  else
    for arg in "$@"; do
      raw_command+="${raw_command:+ }$arg"
      printf -v quoted '%q' "$arg"
      command+="${command:+ }$quoted"
    done
    # OpenSSH's command-line interface joins multiple command arguments with
    # spaces. Preserve that behavior for scripts large enough to use stdin;
    # percent-quoting a heredoc fragment would turn its newlines into one huge
    # `$'...'` argument and recreate the very limit this path avoids.
    if [ "${#command}" -gt 2048 ]; then
      command="$raw_command"
    fi
  fi

  # Tailscale SSH currently reports local success even when a remote command
  # fails. Emit and consume our own exit marker so set -e remains trustworthy.
  marker="__CMUX_REMOTE_RC_$$_${RANDOM}__="
  rc_file="$(mktemp "${TMPDIR:-/tmp}/cmux-remote-rc.XXXXXX")"
  command="(
$command
)
__cmux_rc=\$?
printf '\\n${marker}%s\\n' \"\$__cmux_rc\""

  filter_remote_output() {
    while IFS= read -r line; do
      case "$line" in
        "$marker"*) printf '%s\n' "${line#"$marker"}" >"$rc_file" ;;
        *) printf '%s\n' "$line" ;;
      esac
    done
  }

  # Large generated Python/heredoc patches exceed the remote macOS login
  # shell's command-argument limit. Stream those scripts to `bash -s`; keep
  # short commands as arguments so callers that intentionally use stdin retain
  # normal SSH semantics.
  if [ "${#command}" -ge "${CMUX_SSH_STREAM_THRESHOLD:-1024}" ]; then
    transport+=("bash -s")
    printf '%s\n' "$command" | "${transport[@]}" | filter_remote_output
    transport_rc="${PIPESTATUS[1]}"
  else
    transport+=("$command")
    "${transport[@]}" | filter_remote_output
    transport_rc="${PIPESTATUS[0]}"
  fi
  if [ -s "$rc_file" ]; then
    remote_rc="$(cat "$rc_file")"
  fi
  rm -f "$rc_file"

  if [[ "$remote_rc" =~ ^[0-9]+$ ]]; then
    return "$remote_rc"
  fi
  [ "$transport_rc" -ne 0 ] && return "$transport_rc"
  echo "ssh: remote command ended without an exit marker" >&2
  return 255
}
