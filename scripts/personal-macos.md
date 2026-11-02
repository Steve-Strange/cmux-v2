# Personal macOS build

The `Personal macOS build` workflow builds this native branch in
`Steve-Strange/cmux-v2` on GitHub's `macos-26` runner. Push to
`codex/macos-ssh-persistence` or a `macos/` branch, then download the
`cmux-steve-ssh-<commit>` artifact from the completed Actions run. The artifact
contains a zipped app, its SHA-256 checksum, build log, and JSON build receipt.
It runs the focused SSH, snapshot, and connection-label tests
before building. After the app build it compiles the native `cmuxTests` target
through `scripts/test-unit.sh build-for-testing`, including
`RemoteWorkspaceReconnectIntentTests`, with separate test DerivedData. The JSON
receipt records successful test-target compilation. It does not execute
the UI test target. It then executes `RemoteWorkspaceReconnectIntentTests`,
`SSHTuiAttachRetryTests`, `SSHTuiOpenTests`, `RemoteDisconnectLifecycleTests`, and
`SidebarWorkspaceSnapshotRefreshPolicyTests`, plus the SSH restore cases in
`CloudStaleInventoryMaterializationTests`, through the existing isolated
app-host and console-session wrappers. Every suite must report a positive test
count. Test logs and a separate result artifact preserve the actual run evidence;
the build receipt records completion only after all selected suites pass.

The app uses tag `steve-ssh`, the isolated identifier
`com.cmuxterm.app.debug.steve.ssh`, and the local backend configuration. Extract
the zip and open the tagged app alongside the installed cmux. The app is signed
ad hoc and is not notarized; if macOS asks for permission to open it, use Finder's
Open action or Privacy & Security's Open Anyway for this build. Cloud login and
services require a separately running local backend. SSH connections use your
normal SSH configuration. Remote sessions need tmux to preserve running jobs.

The build uses the upstream CI setting `CMUX_SKIP_ZIG_BUILD=1`. Terminal rendering
uses the verified GhosttyKit framework; the separate Ghostty command helper is
a stub in this build. The computer-use helper and other native Rust components
are built from their upstream pinned sources.

For a local build, install full Xcode 26 with its Metal compiler, Rust through
rustup, and GitHub CLI with a read-capable authenticated session. Command Line
Tools alone cannot build the macOS app. From this trusted checkout:

```sh
./scripts/personal-macos-build.sh --tag steve-ssh
CMUX_TAG=steve-ssh scripts/cmux-debug-cli.sh list-workspaces
```

The helper checks prerequisites, initializes the pinned Ghostty and Bonsplit
submodules, provisions the upstream Rust toolchains, downloads checksum-verified
GhosttyKit, and invokes the tagged `reload.sh` without launching the app. A local
rebuild terminates an existing instance with the same tag, as `reload.sh` normally
does. Use another tag to keep an existing personal test session open. It does not
replace the installed cmux or change global CLI links. Build artifacts default
to `build/personal-macos`; `--output-dir` changes that directory.

The native import has separate history from the older Linux branch.
`personal-macos-client.json` pins the official cmux-tui client whose four input
trees exactly match upstream source `9d01f956c51cd895e10e7d56b6dd7b5f6fcce625`.
The official resolver selected client commit
`a9b104f3cb7d3808cf7eb5ae08b4819333b25870` from the original upstream history.
The helper rejects changed client inputs, uses that immutable manifest, and
requires the official workflow's attestation and exact source digest. A future
client or Ghostty change needs a reviewed pin update. CI uses only its read-only
GitHub token, without developer signing secrets or the gpusp SSH key.

After placing the personal app in a permanent location, enable startup after
macOS login with `python3 scripts/personal-macos-login.py --enable '<app path>'`.
This installs a user LaunchAgent and opens the app without launch arguments, so
normal session restoration runs. It does not keep reopening an app you quit.
Remove this setting with `python3 scripts/personal-macos-login.py --disable`.
Saved managed SSH workspaces retry network failures; explicit Disconnect stays
paused across relaunch until Reconnect is selected. Authentication failures need
manual attention. Plain SSH typed into a local shell is not a managed workspace.
