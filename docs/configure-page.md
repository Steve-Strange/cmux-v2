# `cmux://settings`

`cmux://settings` is a cmux-owned trusted WebUI. `cmux://configure` remains a
compatibility alias. The URL handler rewrites both hosts to the private
`chrome://cmux-configure` implementation; neither aliases an upstream page.
Its config is installed from Chromium's canonical
`RegisterChromeWebUIConfigs()` pass before profile creation. cmux also
reasserts the idempotent registration immediately before installing its URL
rewrite, so a visible cmux settings URL can never resolve to an unregistered
private host. Chromium's registered Chrome WebUI factory also recognizes that
private host directly, matching the dispatch path used by built-in trusted
pages.

## Implemented vertical slice

- First-run entry points for viewing shortcuts and browser setup.
- An Appearance tab with Automatic, Light, and Dark app-icon modes. Automatic
  follows macOS appearance changes live; the explicit choices persist in the
  app's defaults and update the running Dock icon immediately.
- A default-on rounded-content-frame switch backed by the profile preference.
  It updates every open terminal and browser pane immediately and is
  automatically suppressed in fullscreen. Turning it off removes the frame's
  inset/padding as well as its corner mask and outline.
- Searchable, read-only inspection of built-in and custom shortcuts, including
  exact key/condition overlaps and an **Open cmux.json** action.
- Reads run on a sequenced `MayBlock` task runner, never the UI thread.
- Malformed, unreadable, or oversized (over 1 MiB) files are reported and are
  never overwritten.
- Renderer replies are guarded by handler weak pointers and WebUI JavaScript
  lifecycle checks.
- The page uses a self-only CSP plus Chromium's `chrome://resources` promise
  bridge; it permits no inline script or network content.

The shortcut source of truth is `~/.config/cmux/cmux.json` (or an absolute
`CMUX_CONFIG`),
not Web Storage. The page never rewrites that JSONC document, so comments and
unrelated settings remain untouched; external edits reload automatically in
every cmux window. App-icon mode is stored in the macOS application defaults;
the rounded-frame option is stored in the Chromium profile. Reload the
settings page itself to refresh its shortcut inspector snapshot.

## Follow-up integration

Default search, browser-data import, and default-browser registration currently
open Chromium's native settings pages and are explicitly labeled **Next
steps**. They still need cmux-native status/results in this page, especially a
per-source import report and partial-failure UI. Sidebar/layout, appearance,
downloads, privacy, and startup behavior still have additional settings to be
bridged. Individual
binding editing intentionally remains an external JSONC workflow.

The app menu navigates its cmux configuration item to `cmux://settings`.
