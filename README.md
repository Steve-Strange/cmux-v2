# cmux-browser

A Chromium fork that is one app: real Chrome web tabs + omnibar (unmodified engine UX), our own custom browser UI on top, and **Ghostty terminals as first-class surfaces** in the same window/compositor.

This is the alternate to [cmux-owl](https://github.com/manaflow-ai/cmux-owl). OWL kept Chromium out-of-process and read pixels back into a Swift shell; every UX rough edge lived in that readback seam. cmux-browser deletes the seam by building our UI and the terminal directly into the Chromium browser process, so web rendering, resize, input, IME, and the omnibar are Chrome-quality for free. The cost is maintaining a browser-UI fork. See [docs/DESIGN.md](docs/DESIGN.md).

## Layout

- `overlay/` — our added/modified Chromium sources, mirrored at their real tree paths (e.g. `overlay/chrome/browser/cmux_term/...`). `scripts/sync.sh` rsyncs these into the Chromium checkout before building.
- `patches/` — edits to existing Chromium files (BUILD.gn, browser-main registration) applied by `scripts/apply.sh`.
- `scripts/` — `sync.sh`, `apply.sh`, `build.sh` (autoninja chrome on the builder), `run.sh`.
- `docs/DESIGN.md` — architecture + staged plan. `STATUS.md` — live board.
- `docs/permissions.md` — web permission, chooser, security-dialog, and OS permission support contract.

Warm Chromium trees live on the reviewed company-owned 48 GB Mac mini fleet.
The sibling `cmux-browser-hq` repository owns inventory, admission, leases,
worker paths, balancing, and rotating build telemetry. This repository remains
the version-controlled home for product inputs.

Third-party code and design provenance, including Helium source revisions,
authors, licenses, and cmux adaptation points, is recorded in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## License

Except where a file or accompanying notice states otherwise, independently
authored material contributed under the cmux project license is licensed under
GPL-3.0-or-later; see [LICENSE](LICENSE). Imported or derived material retains
its original license and notices. In particular, the Helium-derived portions
remain GPL-3.0-only, so the current combined browser work is distributed under
GPL-3.0-only. That combined-work classification does not narrow the
GPL-3.0-or-later grant for independently authored cmux material. Detailed
provenance is recorded in the third-party notices above.

## Build

Install Tailscale and join the Manaflow tailnet. Agents must schedule the whole
sync/apply/build/deploy operation through HQ; do not select or probe an AWS
builder:

```sh
HQ=/Users/cmux-lawrence/fun/cmux-browser-hq/bin/hq
CMUX_AGENT_ID=my-tag "$HQ" monitor-build --label my-tag -- \
  "$HQ" run --repo "$PWD" --sync --job my-tag --agent my-tag -- \
  bash -c 'set -e; ./scripts/build.sh; \
    DEST="$HOME/Applications/cmux-browser-my-tag.app" \
    CMUX_TAG=my-tag ./scripts/deploy.sh'
```

HQ chooses an eligible owned mini, holds one lease across the operation, keeps
the warm output cache, and releases it on success or failure. Omit `--host` so
recent builder time is spread across the fleet. Use `hq fleet-status` for
capacity and `hq logs list|show|summary` for retained diagnostics and
performance. `aws-m4pro-*`, personal Macs, and direct standalone build-script
defaults are not fallback capacity.

All macOS builder scripts use this Tailscale SSH transport automatically. The
tailnet policy grants direct `@manaflow.ai` members access, including new team
members, without distributing keys or editing SSH config. When a numeric
Tailscale address is required to bypass a broken MagicDNS resolver, the helper
pins OpenSSH to that peer's host keys from the authenticated Tailscale network
map; it never learns or accepts a host key from the direct connection.

The tagged app is downloaded to
`~/Applications/cmux-browser-my-tag.app`. Run it directly (no OWL environment)
and it is the full browser; `CMUX_TERM=1` opens a Ghostty terminal surface.
Use a unique tag so existing dogfood sessions remain untouched.

## New tab page

New web tabs, columns, workspaces, and windows open `about:blank` and focus the
omnibox by default. To use Chrome's New Tab page instead, set
`browser.newTabPage` in `~/.config/cmux/cmux.json` (or the file selected by an
absolute `CMUX_CONFIG` path):

```json
{
  "browser": {
    "newTabPage": "new_tab"
  }
}
```

The file is JSONC, so comments and trailing commas are accepted. Supported
values are `"blank"` and `"new_tab"`. Restart cmux-browser after changing this
setting.

## Keyboard shortcuts

The same `cmux.json` file accepts VS Code-style keybinding rules and reloads
them while cmux is running:

```jsonc
{
  "$schema": "https://raw.githubusercontent.com/manaflow-ai/cmux-browser/main/docs/cmux.schema.json",
  "keybindings": [
    { "key": "cmd+t", "command": "tab.newWeb" },
    { "key": "cmd+ctrl+t", "command": "tab.newTerminal" },
    { "key": "cmd+shift+t", "command": "tab.restore" }
  ]
}
```

Rules support `when`, `args`, two-stroke chords, removal commands, scan codes,
and `runCommands`. See `docs/keybindings.md` for the command list and contexts.

## Automatic updates

Desktop builds check a signed release feed in the background. Large package
downloads run only on an explicitly unmetered connection, and the rail shows
`Update now` only after the complete package has been signature/hash verified
and staged. One click swaps, relaunches, and retains rollback safety on macOS,
Windows, and Linux. Release packaging, signing-key handling, and verification
are documented in [docs/auto-updates.md](docs/auto-updates.md).

Windows/Linux stable releases and scheduled nightlies are built and published
by GitHub Actions on warm release runners. Runner setup, signing secrets, tags,
channels, and assets are documented in [docs/releases.md](docs/releases.md).

## Anonymous telemetry

Release builds send at most one anonymous daily-active event and one
hourly-active event to the existing cmux PostHog project. The payload contains
only a random installation ID, UTC day/hour, app version, operating system, and
whether the check happened at launch or while an app window was active. This
PostHog path never includes URLs, browsing history, terminal contents, file
paths, account data, or user-entered text. As with any direct HTTPS request,
PostHog receives the connection IP and may derive coarse geographic properties
from it.

Crashpad uploads native crash reports to the cmux-browser Sentry project under
the same setting. A crash report contains process and stack state needed to
diagnose the failure, including a minidump with limited memory around the
crashing threads. That data can contain fragments of content that was in memory
when the process failed. Sentry's project-side data scrubbers provide a second
filter, but disabling anonymous telemetry is the only way to prevent crash
reports from leaving the machine. Opted-out Linux launches keep Chromium's
required local crash handler, but give it no upload URL and isolate its reports
in a database that is never reused by opted-in launches. Opted-in releases also
use a new versioned database, so pending crash reports from older builds are
never migrated to Sentry.

Both are enabled by default, matching the native cmux app. To disable them, set
`app.sendAnonymousTelemetry` in `~/.config/cmux/cmux.json` (or the file selected
by an absolute `CMUX_CONFIG` path) and restart:

```jsonc
{
  "app": {
    "sendAnonymousTelemetry": false
  }
}
```

`CMUX_TELEMETRY_ENABLE=0` is an environment-level opt-out. Developer builds are
silent unless explicitly started with `CMUX_TELEMETRY_ENABLE=1`.
