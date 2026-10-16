# Desktop releases

cmux publishes stable and nightly desktop artifacts to the public
[`manaflow-ai/cmux-v2`](https://github.com/manaflow-ai/cmux-v2) distribution
repository. Product source remains in the private `cmux-browser` repository;
the public repository contains downloads, signed feeds, notices, and the exact
corresponding-source archive required for each GPL-covered binary.

Full Chromium builds do not fit reliably on standard GitHub-hosted runners.
The workflows therefore use:

- `self-hosted`, `macOS`, `ARM64`, `cmux-release` on a controller that invokes
  `hq run`; the GitHub runner must never compile directly
- `blacksmith-32vcpu-ubuntu-2404` with a revision-keyed sticky disk for Linux
- `self-hosted`, `Windows`, `X64`, `cmux-release`

Lightweight validation, source packaging, manifest publication, and updater
protocol tests run on small Blacksmith Linux, Windows, and macOS runners. This
keeps release control-plane CI independent of GitHub-hosted runner spending
state; it does not bypass the HQ lease requirement for the physical macOS
Chromium compile.

Publishing is disabled by default. After both runners and all signing secrets
below are ready, set the repository Actions variable
`CMUX_RELEASES_ENABLED=true`. Scheduled nightlies and tag releases skip their
release jobs until that variable is enabled.

The build jobs may run before the release notice inventory is approved, but
the cross-repository upload is independently gated by
`CMUX_RELEASE_COMPLIANCE_READY=true`. Do not set that variable until the
target-specific Chromium/Ghostty/uBlock notices, theme provenance, and LGPL
relinking materials called out in `THIRD_PARTY_NOTICES.md` have been generated
and reviewed. Land the approved files at
`release-compliance/THIRD_PARTY_NOTICES.html` and
`release-compliance/third-party-notices.spdx.json`; the signed-feed job includes
them in provenance and checksums before publication. Source packaging alone
does not make an incomplete notice bundle publishable.

Only one release job should use a warm checkout at a time. GitHub's runner
scheduler provides that serialization per runner, and the workflow concurrency
group prevents two builds of the same channel from publishing concurrently.

## Runner layout

Linux mounts its revision-keyed sticky disk at `/opt/cmux`, keeps Chromium at
`/opt/cmux/chromium/src`, and keeps depot_tools at `/opt/cmux/depot_tools`.
`scripts/bootstrap-release-linux.sh` cold-bootstraps that disk once, including
system dependencies, the exact Chromium commit, Chromium's generated credits,
the pinned Ghostty library built with Zig 0.15.2, and the exact public
`manaflow-ai/cmux` commit used to build `cmux-tui` with Rust 1.96.0. Ghostty
and cmux-tui use revision-isolated dependency caches; the helper, its identity
receipts, and its generated runtime resources are installed beside the browser
before any Linux package is derived. Ghostty's discovered license files are
staged beside the library before the Chromium build starts. The source snapshot's
`scripts/prepare-ghostty-linux-pic.py` applies an exact-revision, fail-closed
build patch so Ghostty and every bundled static dependency are safe to embed in
Chromium's position-independent executable. Subsequent nightlies reuse the
checkout and object cache. A changed `.chromium-version` automatically selects
a new disk key rather than mutating an older revision's cache.

Windows defaults to `C:\cr\src`. Override it with
`CMUX_WINDOWS_CHROMIUM_SRC`. `python`, `autoninja`, `7z`, `signtool`, and the
Windows SDK must be on `PATH`.

The macOS controller runner invokes `scripts/build-release-macos-hq.sh`, which
lets HQ select and lease a reviewed mini for sync/apply/compile, then pulls the
app back to the controller for Developer ID signing, notarization, stapling,
and DMG/ZIP packaging. Configure repository variables `CMUX_HQ_BIN`,
`CMUX_MAC_SIGN_IDENTITY`, `CMUX_MAC_PROVISION_PROFILE`, and
`CMUX_MAC_NOTARY_PROFILE`; the last two name controller-local protected
resources and must not contain their contents.

Both checkouts must be warm, non-component release builds with
`symbol_level=1` for the same major/minor/build baseline as
[`.chromium-version`](../.chromium-version). The jobs restore Chromium's
tracked files, sync the cmux overlay, apply the platform patches, set the
release patch in `chrome/VERSION`, and reuse `out/Release`.

The Linux output must also set `use_debug_fission=false`. This keeps its
minimal DWARF line data in the ELF files uploaded to Sentry instead of leaving
the useful records in per-object `.dwo` files that the release job does not
publish.

## Required secrets

Configure the following GitHub Actions secrets before enabling either workflow:

- `CMUX_UPDATE_PRIVATE_KEY_B64`: base64 of the P-256 manifest private key whose
  public half is checked into `docs/update-public-key.pem`.
- `SENTRY_AUTH_TOKEN`: Sentry organization token with project release/debug
  file write access. Symbols are uploaded to organization `aurora-7k`, project
  `cmux-browser`; the public crash-ingest DSN is compiled into the app and is
  not a secret.
- `WINDOWS_SIGNING_CERTIFICATE_B64`: base64 of the Authenticode `.pfx`.
- `WINDOWS_SIGNING_CERTIFICATE_PASSWORD`: password for that `.pfx`.
- `CMUX_RELEASE_REPO_TOKEN`: fine-grained token or GitHub App token with
  Contents write access only to `manaflow-ai/cmux-v2`.

The jobs fail if signing material or the Sentry token is absent. `sentry-cli`
is pinned and checksum-verified on each runner before uploading Linux ELF debug
information or Windows PDBs. The PFX and manifest key are materialized only in
runner-temporary files and removed after use. Put the `release` GitHub
environment behind required reviewers if production publishing needs an
approval gate.

Both release builders stamp `cmux_release_build.h` and the patched Crashpad
pre-handler consent loader only in their disposable Chromium copy before
compiling. Never change either developer `false` default: that boundary keeps
optimized developer and dogfood builds telemetry-off unless the contributor
explicitly sets `CMUX_TELEMETRY_ENABLE=1`.

## Sentry project

The Sentry **Native** project with slug `cmux-browser` in organization
`aurora-7k` uses public DSN
`https://57f477545589ac21ff2d89a90d6df629@o4509253353930752.ingest.us.sentry.io/4511815527104512`.
The corresponding Crashpad minidump endpoint is compiled into Windows and Linux
builds; a public DSN is an ingest identifier, not an authorization secret.
On Windows, only the shipped `chrome.exe` browser client may start an
upload-capable handler. Independently launched installer, notification, and
other helper executables remain disabled because they cannot reliably inherit a
browser launch's environment-level opt-out; their symbols are therefore outside
the intentional upload set.
Before accepting reports, configure the project under Security & Privacy to:

- enable the data scrubber and default scrubbers;
- scrub IP addresses; and
- disable **Store Minidumps As Attachments**, so Sentry can process a minidump
  without retaining the raw attachment for later download.

Create a CI auth token with `org:ci` (or equivalent project release/debug-file
write access) and save it as the `SENTRY_AUTH_TOKEN` repository secret. Keep the
token out of source; unlike the public DSN, it authorizes debug-file uploads.

## Stable releases

Push an annotated tag whose version retains the pinned Chromium
major/minor/build and increases the patch component beyond the pinned baseline:

```bash
git tag -a v151.0.7922.35 -m "cmux 151.0.7922.35"
git push origin v151.0.7922.35
```

`.github/workflows/release.yml` builds and signs both platforms, creates a
signed `update.json`, uploads installers/update ZIPs/checksums, uploads the
manifest last, and marks the GitHub release as latest. The stable app's default
`/releases/latest/download/update.json` URL ignores prerelease nightlies.

## Nightlies

`.github/workflows/nightly.yml` runs every day at 09:17 UTC and can also be
started manually. It adds the workflow run number to the pinned Chromium patch
to derive a monotonically increasing version, publishes to the prerelease tag
`nightly`, and embeds this channel file in each portable application root:

```text
cmux-update-feed-url
```

That file points only to
`https://github.com/manaflow-ai/cmux-v2/releases/download/nightly/update.json`.
The updater still verifies the normal production manifest signature, so
changing the local feed file cannot authorize an unsigned update. Stable
packages omit the file.

While macOS Developer ID/notarization and the Windows signing runner are being
provisioned, `.github/workflows/release-linux-nightly.yml` is the deliberately
manual Linux-only release path. It creates the same signed Linux feed,
Corresponding Source, generated Chromium/Ghostty notices, SPDX inventory,
provenance, and checksums without weakening the all-platform workflow's
fail-closed gates. Its completed `cmux-linux-nightly-release` Actions artifact
is reviewed and uploaded to the public `nightly` release as one set.

The current combined browser work is distributed under GPL-3.0-only because it
contains Helium-derived GPL-3.0-only portions. Independently authored cmux
material remains GPL-3.0-or-later as recorded in its source headers and the
root license scope notice.

Keeping the development repository private does not make a conveyed GPL binary
private. Every public release therefore includes
`cmux-browser-source-<version>.tar.zst`, containing the exact cmux Browser
commit used for that binary, and `corresponding-source.json`, containing exact
public dependency commits and URLs. Unreleased development may remain in the
private repository; a released source snapshot remains public for as long as
the matching binary is offered.

### Rebuilding the Linux release from its source snapshot

The source snapshot is intentionally sufficient without access to the HQ
control repository or Manaflow's private Git repository. It fetches only the
exact public dependency commits recorded in `corresponding-source.json`. On an
x86-64 Ubuntu host, install Git, Python 3, rsync, zstd, Zig 0.15.2, and Rust
1.96.0, then:

```bash
tar --use-compress-program=unzstd \
  -xf cmux-browser-source-<version>.tar.zst
cd cmux-browser-source-<version>

version="$(python3 -c \
  'import json; print(json.load(open("SOURCE-MANIFEST.json"))["version"])')"
source_sha="$(python3 -c \
  'import json; print(json.load(open("SOURCE-MANIFEST.json"))["source"]["commit"])')"
export CMUX_LINUX_BUILD_ROOT="$PWD/.linux-release-build"
export CHROMIUM_SRC="$CMUX_LINUX_BUILD_ROOT/chromium/src"
export CMUX_DEPOT_TOOLS="$CMUX_LINUX_BUILD_ROOT/depot_tools"
export CMUX_RELEASE_VERSION="$version"
export CMUX_RELEASE_SOURCE_SHA="$source_sha"
export CMUX_RELEASE_CHANNEL=nightly
export RUSTUP_TOOLCHAIN=1.96.0

./scripts/bootstrap-release-linux.sh
./scripts/build-release-linux.sh
```

The bootstrap verifies Chromium, cmux-tui, and both uses of Ghostty against the
recorded commits before applying the included Linux PIC build patch and
building. The output lands in `release/`. `corresponding-source.json` names the
same inputs explicitly for independent fetching or archival.

Published assets are:

- `cmux-macos-arm64.dmg`
- `cmux-macos-arm64.zip`
- `cmux-windows-x64-installer.exe`
- `cmux-windows-x64.zip`
- `cmux-linux-x64-installer.run`
- `cmux-linux-x64.deb`
- `cmux-linux-x64.zip`
- `cmux-browser-source-<version>.tar.zst`
- `corresponding-source.json`
- `provenance.intoto.jsonl`
- `SHA256SUMS`
- `update.json`

The Linux website download is `cmux-linux-x64-installer.run`. It installs into
`~/.local/opt/cmux-browser-nightly` (or the stable equivalent), creates a
launcher and desktop entry below `~/.local`, and needs no root access. That
user-owned location is what makes the signed directory-swap updater work.
Run it with `sh cmux-linux-x64-installer.run`; use
`CMUX_INSTALL_DIR=/absolute/path` only for a different user-owned location.
The `.deb` remains available for managed/package-manager installs, but those
root-owned installations must be updated through the package manager rather
than the in-app updater.

The ZIPs are the per-user, directory-swappable roots consumed by the background
updater. The Windows installer is for first installation. The Debian package is
also a first-install/system-managed artifact; because its application directory
is normally root-owned, it is updated by installing a newer package rather than
by the per-user in-app updater. Use the Linux ZIP for background in-app updates.
