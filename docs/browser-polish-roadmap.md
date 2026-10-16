# Browser polish roadmap

This document turns the July 2026 dogfood list into independently shippable
changes with observable acceptance criteria. It is deliberately organized by
behavior rather than by source file so that custom cmux UI does not silently
replace working Chromium behavior.

## Product invariants

- A web pane is a real Chromium tab owned by a real `Browser` and
  `TabStripModel`.
- Chrome compatibility is the baseline: cmux is a full browser with
  first-class terminals and workspaces, not a workspace shell that happens to
  embed web content.
- A workspace is exposed to Chromium and extensions as a logical `Browser`
  window. A cmux native window is a higher-level container that can host
  multiple workspace Browsers; multiple native containers must be supported.
- Browser features should reuse Chromium models/controllers where possible;
  cmux-specific actions should extend those surfaces rather than fork them.
- Normal launches must not opt into automation, remote debugging, or false
  browser-branding fingerprints. Test-only switches remain explicit opt-ins.
- Vertical and horizontal tab presentations share one interaction model and
  differ only in layout/styling.

## Workstreams and acceptance criteria

### 1. Real-browser network behavior

- A normal launch has no automation, remote-debugging, or UA-CH spoofing
  switches.
- `navigator.webdriver` is false without suppressing Blink automation
  features.
- Chromium branding remains Chromium unless the binary is built with official
  Chrome branding.
- Google search, sign-in, cookie persistence, downloads, popups, permissions,
  and WebAuthn are smoke-tested from a clean non-test profile.
- Rate limits are diagnosed from response status and profile/run provenance;
  a stale Google `/sorry` page is not treated as a browser fingerprint failure.

### 2. Toolbar hit targets

- Extension actions, the extensions puzzle button, downloads, profile, and app
  menu use the same square visual bounds, circular hover background, icon size,
  spacing, focus ring, tooltip delay, and accessibility naming.
- The target is at least 32x32 DIP and remains stable when an extension badge
  appears.

### 3. Tab motion

- Insertion grows/slides from the insertion edge while neighboring tabs move
  continuously.
- Removal shrinks/fades the closing tab before remaining tabs settle; rapid
  close operations coalesce without stale view pointers.
- Selection, close buttons, favicons, throbbers, drag gaps, and hit testing stay
  correct mid-animation.
- Reduced-motion or disabled-animation settings produce the final layout
  immediately.

### 4. Shortcut modifier scheme

- macOS offers both `Command = tabs, Control = workspaces` and
  `Control = tabs, Command = workspaces`.
- The choice changes generated defaults without destroying explicit custom
  bindings.
- Shortcut configuration lives in the JSONC `cmux.json` `keybindings` array.
- The setting has a typed parse/stringify API usable by `cmux://configure`.

### 5. Complete browser and cmux menus

- The app menu covers Chromium's applicable sections: tabs/windows/incognito,
  profile, passwords/autofill, history/downloads/bookmarks, extensions, delete
  browsing data, zoom/fullscreen, find/edit/print, cast/save/share, developer
  tools, settings/help/about, and exit.
- A cmux section covers workspace creation/navigation/closing, web and terminal
  columns/tabs, splits, layout mode, sidebar, and configuration.
- Chromium commands use the active workspace `Browser`; cmux commands use the
  strip controller. Unsupported BrowserView-only commands are disabled rather
  than crashing.
- Dedicated downloads and profile buttons expose their normal Chromium models,
  not just links to settings pages.
- “New window” creates a new native cmux container, while “New workspace” adds
  another logical Browser to the current container. Extension-created Browser
  windows are adopted into the invoking container when possible; popup and
  presentation window types may require their own native surface.

### 6. Site information

- Clicking the leading omnibox icon opens Chromium's page-info bubble anchored
  to that pane's location icon.
- Secure connection details, cookies/site data, permissions, certificate
  details, and site-settings links work for the active pane.
- Opening and closing the bubble after switching tabs/workspaces cannot retain
  stale anchors or `WebContents` pointers.

### 7. Sidebar

- The rail remains useful at narrow widths and has clear workspace hierarchy,
  selected/focused states, unread/activity state, drag targets, context menus,
  and keyboard navigation.
- Collapse/expand preserves the focused pane and does not shift traffic-light
  hit targets.
- Workspace naming, colors, nesting, reordering, close/restore, and overflow
  behavior are explicitly designed and tested.

### 8. `cmux://configure`

- First run offers `Use defaults` and `Configure`; both choices are reversible.
- Configuration includes shortcut scheme and custom bindings, default search
  engine, browser-data import, default-browser registration, sidebar/layout,
  appearance, downloads, privacy, and startup behavior.
- Import reports each supported source and data type (bookmarks, history,
  passwords, cookies where the platform/browser permits extraction) with clear
  partial-failure results.
- Settings are backed by typed native preferences/config models; the WebUI is
  not the source of truth.
- The page is reachable later from the app menu and settings.

### 9. Helium tab parity

- Pin the Helium revision used as the visual reference.
- Record tokens for height, radius, spacing, typography, colors, separators,
  favicon/close geometry, hover/active states, and vertical-tree indentation.
- Match those tokens in both orientations while retaining cmux loading, drag,
  split, terminal, theme, and accessibility behavior.
- Verify with same-size screenshot comparisons in light and dark themes.

## Autoreview gate

Every workstream must pass:

1. Focused host/unit tests and `git diff --check`.
2. A Chromium compile on the pinned builder revision.
3. A clean-profile dogfood run without test/debug switches.
4. Keyboard, mouse, accessibility, light/dark theme, and reduced-motion checks
   for touched UI.
5. Review of custom-window boundaries (`BrowserWindow::AsBrowserView()` is
   intentionally null) so upstream code cannot assume a `BrowserView`.
6. A second diff review for ownership, observer lifetime, animation teardown,
   and asynchronous close races.

## Decisions still being grilled

- Exact policy for standard/extension `New window`: native cmux container vs.
  workspace adoption, including popup and incognito exceptions.
- What “better sidebar” means in terms of information density and hierarchy.
- Whether onboarding is mandatory once, shown until completed, or entirely
  optional.
- Which browser-data import sources and cookie migrations are acceptable from
  a security/product perspective.
