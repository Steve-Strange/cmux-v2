#!/usr/bin/env bash
set -euo pipefail

SENTRY_CLI_VERSION="3.3.0"
SENTRY_CLI_ASSET="sentry-cli-Linux-x86_64"
SENTRY_CLI_SHA256="5563b71cc28cd8b1e2cd1037f8e377083992ec112c9bf1ff0efd26dfe3b1e67e"
TEMP_ROOT="${RUNNER_TEMP:-${TMPDIR:-/tmp}}"
INSTALL_DIR="$TEMP_ROOT/sentry-cli-bin"
DOWNLOAD_PATH="$TEMP_ROOT/${SENTRY_CLI_ASSET}-${SENTRY_CLI_VERSION}"

mkdir -p "$INSTALL_DIR"
echo "Installing sentry-cli $SENTRY_CLI_VERSION into $INSTALL_DIR" >&2
curl -fsSL --connect-timeout 20 --max-time 180 \
  "https://github.com/getsentry/sentry-cli/releases/download/${SENTRY_CLI_VERSION}/${SENTRY_CLI_ASSET}" \
  --output "$DOWNLOAD_PATH"
ACTUAL_SHA256="$(sha256sum "$DOWNLOAD_PATH" | awk '{ print $1 }')"
if [[ "$ACTUAL_SHA256" != "$SENTRY_CLI_SHA256" ]]; then
  echo "sentry-cli checksum mismatch: expected $SENTRY_CLI_SHA256, got $ACTUAL_SHA256" >&2
  exit 1
fi
install -m 0755 "$DOWNLOAD_PATH" "$INSTALL_DIR/sentry-cli"

SENTRY_CLI="$INSTALL_DIR/sentry-cli"
"$SENTRY_CLI" --version >&2
printf '%s\n' "$SENTRY_CLI"
