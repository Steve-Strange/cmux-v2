#!/usr/bin/env bash
# Repository-owned SSH transport for the shared builders. Works both as a
# direct shell command and as rsync's remote shell.
set -euo pipefail
script_dir="$(cd "$(dirname "$0")" && pwd)"

tailscale_cli=""
if command -v tailscale >/dev/null 2>&1; then
  tailscale_cli="$(command -v tailscale)"
else
  for candidate in \
    /Applications/Tailscale.app/Contents/MacOS/Tailscale \
    /opt/homebrew/bin/tailscale \
    /usr/local/bin/tailscale; do
    if [ -x "$candidate" ]; then
      tailscale_cli="$candidate"
      break
    fi
  done
fi

if [ -z "$tailscale_cli" ]; then
  echo "Tailscale CLI not found. Install Tailscale and join the Manaflow tailnet." >&2
  exit 127
fi

# rsync may express user@host as `-l user host`. Tailscale SSH accepts the
# combined form, so normalize the small subset of SSH transport flags rsync
# and our scripts use.
user=""
while [ $# -gt 0 ]; do
  case "$1" in
    -l) [ $# -ge 2 ] || exit 2; user="$2"; shift 2 ;;
    -o) [ $# -ge 2 ] || exit 2; shift 2 ;;
    --) shift; break ;;
    -*) echo "unsupported Tailscale SSH transport option: $1" >&2; exit 2 ;;
    *) break ;;
  esac
done

[ $# -ge 1 ] || { echo "usage: tailscale-ssh.sh [user@]host [command ...]" >&2; exit 2; }
host="$1"
shift
if [[ "$host" != *@* ]]; then
  user="${user:-${CMUX_BUILDER_USER:-ec2-user}}"
  host="$user@$host"
fi

# The M4 Pro scratch volume is protected from a direct Tailscale SSH login by
# macOS privacy controls. The fleet's loopback SSH service has the required
# volume access, so transparently hop through it for this host. This also works
# as rsync's remote shell and keeps the workaround out of personal SSH config.
host_name="${host#*@}"
requested_host_name="$host_name"
loopback_host="${CMUX_BUILDER_LOOPBACK_HOST:-aws-m4pro-1}"

# `tailscale ssh` resolves 100.x destinations back through MagicDNS before
# invoking OpenSSH.  Some macOS resolver states can reach the peer by address
# while failing that synthesized hostname lookup.  A normal SSH connection to
# the Tailscale address uses the same tailnet path and avoids the lookup.
transport=("$tailscale_cli" ssh)
transport_host="$host"
native_tailscale_ip="${CMUX_TAILSCALE_NATIVE_IP:-}"
numeric_host="$host_name"
if [ -n "$native_tailscale_ip" ]; then
  if [[ ! "$native_tailscale_ip" =~ ^100\.([0-9]{1,3}\.){2}[0-9]{1,3}$ ]]; then
    echo "CMUX_TAILSCALE_NATIVE_IP must be a numeric 100.x Tailscale address." >&2
    exit 2
  fi
  numeric_host="$native_tailscale_ip"
  transport_host="${host%@*}@$native_tailscale_ip"
fi

if [[ "$numeric_host" =~ ^100\.([0-9]{1,3}\.){2}[0-9]{1,3}$ ]]; then
  python3_cli="${CMUX_PYTHON3:-$(command -v python3 || true)}"
  if [ -z "$python3_cli" ]; then
    echo "python3 is required to verify a numeric Tailscale peer's SSH host keys." >&2
    exit 127
  fi

  known_hosts_dir="${CMUX_TAILSCALE_KNOWN_HOSTS_DIR:-${XDG_CACHE_HOME:-$HOME/Library/Caches}/cmux/tailscale-ssh}"
  mkdir -p "$known_hosts_dir"
  chmod 700 "$known_hosts_dir"
  known_hosts_file="$known_hosts_dir/known-hosts-$numeric_host"
  known_hosts_tmp="$(mktemp "$known_hosts_dir/.known-hosts-$numeric_host.XXXXXX")"
  status_tmp="$(mktemp "$known_hosts_dir/.status-$numeric_host.XXXXXX")"
  cleanup_known_hosts_tmp() {
    rm -f "$known_hosts_tmp" "$status_tmp"
  }
  trap cleanup_known_hosts_tmp EXIT HUP INT TERM

  # `tailscale status` is authenticated by the local tailscaled connection to
  # the coordination server. Materialize only that peer's advertised SSH host
  # keys and canonical hostname; never learn either from the unauthenticated
  # direct SSH connection.
  if ! "$tailscale_cli" status --json >"$status_tmp" ||
      ! canonical_host_name="$(
        "$python3_cli" "$script_dir/tailscale-known-hosts.py" \
          --hostname "$numeric_host" <"$status_tmp"
      )" ||
      ! "$python3_cli" "$script_dir/tailscale-known-hosts.py" "$numeric_host" \
        <"$status_tmp" >"$known_hosts_tmp"; then
    echo "Unable to obtain verified SSH host keys for Tailscale peer $numeric_host." >&2
    exit 255
  fi
  if [ -n "$native_tailscale_ip" ] &&
     [[ ! "$requested_host_name" =~ ^100\.([0-9]{1,3}\.){2}[0-9]{1,3}$ ]] &&
     [ "$canonical_host_name" != "${requested_host_name%.}" ]; then
    echo "Numeric Tailscale override $numeric_host resolves to $canonical_host_name, not requested host $requested_host_name." >&2
    exit 255
  fi
  host_name="$canonical_host_name"
  chmod 600 "$known_hosts_tmp"
  mv -f "$known_hosts_tmp" "$known_hosts_file"
  rm -f "$status_tmp"
  trap - EXIT HUP INT TERM

  transport=(
    "${CMUX_SYSTEM_SSH:-/usr/bin/ssh}"
    -o BatchMode=yes
    -o ConnectTimeout=15
    -o StrictHostKeyChecking=yes
    -o "UserKnownHostsFile=$known_hosts_file"
    -o GlobalKnownHostsFile=/dev/null
    -o UpdateHostKeys=no
    -o VerifyHostKeyDNS=no
    -o LogLevel=ERROR
  )
fi

if [ "${CMUX_BUILDER_LOOPBACK_SSH:-1}" != "0" ] && [ "$host_name" = "$loopback_host" ]; then
  loopback_key="${CMUX_BUILDER_LOOPBACK_KEY:-/var/root/.ssh/cmux-tart-ci-host-key}"
  remote=(
    sudo /usr/bin/ssh
    -i "$loopback_key"
    -o BatchMode=yes
    -o IdentitiesOnly=yes
    -o StrictHostKeyChecking=no
    -o UserKnownHostsFile=/dev/null
    -o LogLevel=ERROR
    "${CMUX_BUILDER_USER:-ec2-user}@127.0.0.1"
  )

  # OpenSSH joins command arguments before sending them to the remote shell.
  # Preserve the original argv as one safely quoted inner command so a second
  # round of shell parsing cannot split `bash -c`, rsync arguments, or paths.
  inner_command=""
  if [ "$#" -eq 1 ]; then
    # Match normal ssh semantics: one argument is already a shell command.
    inner_command="$1"
  else
    for arg in "$@"; do
      printf -v quoted '%q' "$arg"
      inner_command+="${inner_command:+ }$quoted"
    done
  fi
  if [ -n "$inner_command" ]; then
    remote+=("$inner_command")
  fi

  remote_command=""
  for arg in "${remote[@]}"; do
    printf -v quoted '%q' "$arg"
    remote_command+="${remote_command:+ }$quoted"
  done
  exec "${transport[@]}" "$transport_host" "$remote_command"
fi

exec "${transport[@]}" "$transport_host" "$@"
