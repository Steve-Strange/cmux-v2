# Desktop auto-updates

cmux has one process-global updater on macOS, Windows, and Linux. It checks a
small signed manifest at startup and every six hours. Package bytes are allowed
to flow only after the platform reports an explicitly unmetered connection:

- macOS: Network.framework path is neither `expensive` nor `constrained`.
- Windows: Network List Manager reports `CONNECTION_COST_UNMETERED`.
- Linux: NetworkManager reports `GENERAL.METERED:no` or `guess-no` for a
  connected device.

An unknown result is blocked. If a connection becomes metered during transfer,
the loader is cancelled and its partial file is discarded. A previously
completed package may still be verified while offline or metered because that
does not use the network.

The rail shows no checking, downloading, or restart affordance. `Update now`
appears only after all of these gates pass:

1. the ECDSA-P256 manifest signature verifies against
   [`update-public-key.pem`](update-public-key.pem);
2. the selected artifact is newer and matches the running OS/architecture;
3. the complete file size and SHA-256 digest match the signed payload;
4. the ZIP safely extracts, including internal app-bundle symlinks;
5. the staged executable exists and the current install can be replaced.

Clicking `Update now` is the only user action. A detached helper waits for cmux
to exit, keeps a rollback copy, swaps the staged app directory into the current
path, relaunches it, then removes the rollback copy. This supports a `.app` on
macOS and per-user application directories on Windows/Linux. A protected
machine-wide install is intentionally not advertised as ready because it would
need a second elevation/UAC action.

## Signing key

The build trusts this public-key fingerprint:

```
SHA-256 8feeefcadd6c55cfeb3dc3aeb52ca95649d914cc1430c9c798c9567374b6f657
```

The matching private key was generated at
`.release-keys/cmux-update-private.pem`, which is gitignored and mode `0600`.
Back it up in the team's secret manager before publishing the first release.
Losing it requires shipping a browser build with a new public key; exposing it
allows an attacker to publish a trusted update.

OS signing remains required before archiving: Developer ID + notarization for
macOS and Authenticode for Windows. The manifest signature protects the release
transport and Linux package as well; it does not replace the platform trust UI.

## Build a release feed

First archive each already-signed distribution root. The root becomes
`archive_root` in the signed manifest:

```bash
python3 scripts/build-update-archive.py \
  --input dist/cmux-browser.app \
  --output release/cmux-mac-arm64.zip

python3 scripts/build-update-archive.py \
  --input release/cmux-browser-windows \
  --output release/cmux-windows-x64.zip

python3 scripts/build-update-archive.py \
  --input release/cmux-browser-linux \
  --output release/cmux-linux-x64.zip
```

Then generate the exact `update.json` consumed by the browser:

```bash
python3 scripts/package-update.py \
  --version 139.0.0.0 \
  --base-url https://github.com/manaflow-ai/cmux-v2/releases/download/v139.0.0.0 \
  --private-key .release-keys/cmux-update-private.pem \
  --output release/update.json \
  --artifact mac-arm64=release/cmux-mac-arm64.zip,cmux-browser.app,Contents/MacOS/cmux \
  --artifact windows-x64=release/cmux-windows-x64.zip,cmux-browser,chrome.exe \
  --artifact linux-x64=release/cmux-linux-x64.zip,cmux-browser,chrome
```

Upload the three ZIPs and `update.json` to the public
[`manaflow-ai/cmux-v2`](https://github.com/manaflow-ai/cmux-v2) release. Also attach
`update.json` with the stable name expected by the `/latest/download/` URL.
The build's Chromium version must match `--version`; the updater uses
`version_info::GetVersionNumber()` to prevent downgrade/update loops.

Automated Windows/Linux stable and nightly publication is documented in
[`releases.md`](releases.md). Stable builds use the default latest-release feed.
Nightly packages contain a `cmux-update-feed-url` channel selector beside the
executable on Windows/Linux and in the sealed app Resources directory on
macOS. It selects
`https://github.com/manaflow-ai/cmux-v2/releases/download/nightly/update.json`
without weakening signature checks. Release assets must remain public because
the updater deliberately sends anonymous, cookie-free requests.
Each binary release also carries its exact source archive. A separate
compliance-readiness gate prevents publication until target-specific notices
and the remaining license obligations are complete.

For a local feed test, set `CMUX_UPDATE_FEED_URL` and, when testing against a
newer synthetic version, `CMUX_UPDATE_CURRENT_VERSION`. There is no unsigned
bypass: local manifests must be signed by the production key so test behavior
cannot accidentally weaken release builds.

## Verification

Run:

```bash
./scripts/run-host-tests.sh
python3 scripts/test-updater.py
```

The first suite locks the metered-network and downloaded-only UI state machine,
plus rollback/relaunch script construction. The second creates macOS, Windows,
and Linux archives, signs a multi-platform feed, verifies it with OpenSSL,
checks file metadata and manifest fields, then proves a changed payload is
rejected. `.github/workflows/updater.yml` runs the protocol test natively on all
three operating systems and the C++ policy suite on macOS/Linux.
