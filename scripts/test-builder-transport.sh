#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
scratch="$(mktemp -d "${TMPDIR:-/tmp}/cmux-browser-transport-test.XXXXXX")"
trap 'rm -rf "$scratch"' EXIT

fake_transport="$scratch/fake-tailscale-ssh"
cat >"$fake_transport" <<'SCRIPT'
#!/usr/bin/env bash
set -euo pipefail
printf '%s\n' "$@" >"$FAKE_TRANSPORT_ARGS"
if [ "$#" -gt 1 ]; then
  if [ "${!#}" = "-s" ]; then
    /bin/bash -s
  else
    /bin/bash -c "${!#}"
  fi
fi
SCRIPT
chmod +x "$fake_transport"

export CMUX_TAILSCALE_SSH="$fake_transport"
export FAKE_TRANSPORT_ARGS="$scratch/args"
. "$root/scripts/builder-transport.sh"

ssh test-host
grep -Fxq test-host "$FAKE_TRANSPORT_ARGS"

ssh test-host true
grep -Fxq test-host "$FAKE_TRANSPORT_ARGS"

ssh -o BatchMode=yes test-host true
grep -Fxq -- -o "$FAKE_TRANSPORT_ARGS"
grep -Fxq BatchMode=yes "$FAKE_TRANSPORT_ARGS"

captured="$(ssh test-host "printf 'remote-value\\n'; printf 'transport warning\\n' >&2" 2>"$scratch/stderr")"
grep -Fxq remote-value <<<"$captured"
if grep -Fq 'transport warning' <<<"$captured"; then
  echo "remote stderr contaminated captured stdout" >&2
  exit 1
fi
grep -Fxq 'transport warning' "$scratch/stderr"

if ssh test-host 'exit 23'; then
  echo "remote failure unexpectedly returned success" >&2
  exit 1
else
  rc=$?
fi
test "$rc" -eq 23

large_command="echo oversized-command-streamed"
for _ in {1..1000}; do
  large_command+=$'\n# transport padding'
done
large_output="$(ssh test-host "$large_command")"
grep -Fxq 'bash -s' "$FAKE_TRANSPORT_ARGS"
grep -Fxq oversized-command-streamed <<<"$large_output"

large_fragment="# streamed multi-argument command"
for _ in {1..1000}; do
  large_fragment+=$'\n# multi-argument padding'
done
large_fragment+=$'\necho first-fragment;'
multi_output="$(ssh test-host "$large_fragment" "echo second-fragment")"
grep -Fxq first-fragment <<<"$multi_output"
grep -Fxq second-fragment <<<"$multi_output"

echo "builder transport tests passed"
