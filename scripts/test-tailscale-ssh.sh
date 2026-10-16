#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
scratch="$(mktemp -d "${TMPDIR:-/tmp}/cmux-tailscale-ssh-test.XXXXXX")"
trap 'rm -rf "$scratch"' EXIT
mkdir -p "$scratch/bin" "$scratch/cache"

ssh-keygen -q -t ed25519 -N "" -f "$scratch/host-key"
host_key="$(awk '{print $1 " " $2}' "$scratch/host-key.pub")"
host_key_type="${host_key%% *}"
host_key_data="${host_key#* }"

cat >"$scratch/status.json" <<EOF
{
  "Peer": {
    "node-key": {
      "HostName": "aws-m4pro-1",
      "TailscaleIPs": ["100.112.53.28", "fd7a:115c:a1e0::8f36:351c"],
      "sshHostKeys": ["$host_key_type $host_key_data"]
    }
  }
}
EOF

cat >"$scratch/bin/tailscale" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
case "${1:-}" in
  status)
    test "${2:-}" = "--json"
    cat "$CMUX_TEST_STATUS"
    ;;
  ssh)
    shift
    printf '%s\n' "$@" >"$CMUX_TEST_TAILSCALE_LOG"
    ;;
  *)
    exit 64
    ;;
esac
EOF

cat >"$scratch/bin/ssh" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
printf '%s\n' "$@" >"$CMUX_TEST_SSH_LOG"
known_hosts=""
for arg in "$@"; do
  case "$arg" in
    UserKnownHostsFile=*) known_hosts="${arg#UserKnownHostsFile=}" ;;
  esac
done
test -n "$known_hosts"
cp "$known_hosts" "$CMUX_TEST_CAPTURED_KNOWN_HOSTS"
EOF
chmod +x "$scratch/bin/tailscale" "$scratch/bin/ssh"

run_numeric() {
  local status="${1:-$scratch/status.json}"
  PATH="$scratch/bin:/usr/bin:/bin" \
  CMUX_SYSTEM_SSH="$scratch/bin/ssh" \
  CMUX_PYTHON3="$(command -v python3)" \
  CMUX_TAILSCALE_KNOWN_HOSTS_DIR="$scratch/cache" \
  CMUX_TEST_STATUS="$status" \
  CMUX_TEST_SSH_LOG="$scratch/ssh.log" \
  CMUX_TEST_TAILSCALE_LOG="$scratch/tailscale.log" \
  CMUX_TEST_CAPTURED_KNOWN_HOSTS="$scratch/captured-known-hosts" \
    "$root/scripts/tailscale-ssh.sh" ec2-user@100.112.53.28 true
}

run_numeric
grep -Fxq "StrictHostKeyChecking=yes" "$scratch/ssh.log"
grep -Fxq "GlobalKnownHostsFile=/dev/null" "$scratch/ssh.log"
grep -Fxq "UpdateHostKeys=no" "$scratch/ssh.log"
grep -Fxq "VerifyHostKeyDNS=no" "$scratch/ssh.log"
grep -Fxq "ec2-user@100.112.53.28" "$scratch/ssh.log"
grep -Fq "sudo /usr/bin/ssh" "$scratch/ssh.log"
grep -Fq "ec2-user@127.0.0.1" "$scratch/ssh.log"
if grep -Fxq "StrictHostKeyChecking=no" "$scratch/ssh.log"; then
  echo "numeric fallback disabled strict host-key checking" >&2
  exit 1
fi
printf '%s\n' "100.112.53.28 $host_key" >"$scratch/expected-known-hosts"
cmp "$scratch/expected-known-hosts" "$scratch/captured-known-hosts"

# A numeric transport override for a named peer uses the same authenticated
# host-key materialization. It must never regress to an unverified direct SSH
# connection.
rm -f "$scratch/ssh.log"
PATH="$scratch/bin:/usr/bin:/bin" \
CMUX_SYSTEM_SSH="$scratch/bin/ssh" \
CMUX_PYTHON3="$(command -v python3)" \
CMUX_BUILDER_LOOPBACK_SSH=0 \
CMUX_TAILSCALE_NATIVE_IP=100.112.53.28 \
CMUX_TAILSCALE_KNOWN_HOSTS_DIR="$scratch/cache" \
CMUX_TEST_STATUS="$scratch/status.json" \
CMUX_TEST_SSH_LOG="$scratch/ssh.log" \
CMUX_TEST_TAILSCALE_LOG="$scratch/tailscale.log" \
CMUX_TEST_CAPTURED_KNOWN_HOSTS="$scratch/captured-known-hosts" \
  "$root/scripts/tailscale-ssh.sh" ec2-user@aws-m4pro-1 true
grep -Fxq "StrictHostKeyChecking=yes" "$scratch/ssh.log"
grep -Fxq "ec2-user@100.112.53.28" "$scratch/ssh.log"
if grep -Fxq "StrictHostKeyChecking=no" "$scratch/ssh.log"; then
  echo "numeric override disabled strict host-key checking" >&2
  exit 1
fi

# A numeric override must remain bound to the named builder the caller
# requested, even though both the key and hostname come from authenticated
# tailscaled state.
sed 's/"HostName": "aws-m4pro-1"/"HostName": "aws-m4pro-2"/' \
  "$scratch/status.json" >"$scratch/wrong-host-status.json"
rm -f "$scratch/ssh.log"
if PATH="$scratch/bin:/usr/bin:/bin" \
  CMUX_SYSTEM_SSH="$scratch/bin/ssh" \
  CMUX_PYTHON3="$(command -v python3)" \
  CMUX_BUILDER_LOOPBACK_SSH=0 \
  CMUX_TAILSCALE_NATIVE_IP=100.112.53.28 \
  CMUX_TAILSCALE_KNOWN_HOSTS_DIR="$scratch/cache" \
  CMUX_TEST_STATUS="$scratch/wrong-host-status.json" \
  CMUX_TEST_SSH_LOG="$scratch/ssh.log" \
  CMUX_TEST_TAILSCALE_LOG="$scratch/tailscale.log" \
  CMUX_TEST_CAPTURED_KNOWN_HOSTS="$scratch/captured-known-hosts" \
    "$root/scripts/tailscale-ssh.sh" ec2-user@aws-m4pro-1 true \
    >"$scratch/wrong-host.out" 2>&1; then
  echo "numeric override accepted a different authenticated peer" >&2
  exit 1
fi
grep -Fq "resolves to aws-m4pro-2, not requested host aws-m4pro-1" \
  "$scratch/wrong-host.out"
test ! -e "$scratch/ssh.log"

# A numeric peer absent from the authenticated network map must fail before
# direct OpenSSH is invoked.
sed 's/100[.]112[.]53[.]28/100.112.53.29/' \
  "$scratch/status.json" >"$scratch/unknown-status.json"
rm -f "$scratch/ssh.log"
if run_numeric "$scratch/unknown-status.json" \
    >"$scratch/unknown.out" 2>&1; then
  echo "unknown numeric peer unexpectedly passed host-key verification" >&2
  exit 1
fi
grep -Fq "expected one Tailscale peer for 100.112.53.28, found 0" \
  "$scratch/unknown.out"
test ! -e "$scratch/ssh.log"

# Named peers retain Tailscale's identity-aware SSH wrapper.
PATH="$scratch/bin:/usr/bin:/bin" \
CMUX_SYSTEM_SSH="$scratch/bin/ssh" \
CMUX_BUILDER_LOOPBACK_SSH=0 \
CMUX_TEST_STATUS="$scratch/status.json" \
CMUX_TEST_SSH_LOG="$scratch/ssh.log" \
CMUX_TEST_TAILSCALE_LOG="$scratch/tailscale.log" \
CMUX_TEST_CAPTURED_KNOWN_HOSTS="$scratch/captured-known-hosts" \
  "$root/scripts/tailscale-ssh.sh" ec2-user@aws-m4pro-6 true
grep -Fxq "ec2-user@aws-m4pro-6" "$scratch/tailscale.log"

echo "tailscale ssh transport tests passed"
