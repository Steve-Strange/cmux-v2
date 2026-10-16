# Multi-native-window foundation

## Product semantics

- **New Window** creates a new OS-native cmux container (`views::Widget`) with
  its own `CmuxWindowView`, rail, `WindowModel`, and logical workspaces.
- **New Workspace** creates another logical normal `Browser` inside the
  invoking native container. It never creates a second OS window.
- **New Incognito Window** creates a new native cmux container in an
  off-the-record profile domain. Normal, incognito, and guest Browsers are not
  mixed in one container.
- A normal Browser created through `chrome.windows.create()` is adopted as a
  workspace only when the initiating cmux container is explicitly propagated
  and its profile/privacy domain matches. If that context is unavailable, it
  becomes a new native container instead of being attached to whichever cmux
  window happens to be active.
- Popup, app, and DevTools Browser types keep Chromium's `BrowserView` path in
  the first multi-window release. Popup bounds and reduced chrome do not map to
  a cmux workspace without a separate product/UI design.

`cmux_native_window_registry.{h,cc}` encodes these decisions without owning UI
objects. `cmux_native_window_registry_test.cc` protects the policy.

## Identity and ownership

`WorkspaceId` is local to `WindowModel`: every new model starts allocating at
1. A process-wide bare `WorkspaceId -> host` map therefore collides as soon as
two native containers exist. Runtime Browser tokens and lookups must carry a
`WorkspaceLocator { NativeWindowId, local WorkspaceId }`.

The production owner should be a process-lifetime coordinator with this shape:

1. The coordinator owns a map of `NativeWindowId -> NativeWindowState`.
2. Each `NativeWindowState` owns one native `views::Widget`; its delegate owns
   one `CmuxWindowView`. The view owns workspace presentation state, while each
   logical Chromium `Browser` owns its `TabStripModel` and web `WebContents`.
3. BrowserWindow adapters retain a weak container host plus a composite
   workspace locator. They never own the Widget or view.
4. The pure registry owns IDs and routing metadata only. Host pointers stored
   alongside it are weak/non-owning and are removed before Widget destruction.
5. Closing one native container first marks it closing and removes it as a
   routing target, closes its anchored extension UI, asks each workspace
   Browser to close, then destroys that Widget after Browser notifications
   unwind. Process keep-alive is released only when the final container closes.
6. App exit snapshots the container list and closes every container; closing a
   workspace closes only its logical Browser.

Session tokens should become `cmux-workspace:<container>:<workspace>` (or an
equivalent opaque registry token). A stale, forged, or profile-incompatible
token must fail closed to Chromium's normal window path, never attach to an
unrelated container.

## Current singleton blockers

- `cmux_views.cc` stores one `g_views_widget`, `g_window`, and `g_strip`.
- `CmuxWidgetDestroyObserver` observes only one Widget.
- `ShowViewsWebWindow()` and `CloseViewsWebWindow()` create/close only the
  singleton, and delayed self-tests capture singleton globals.
- `cmux_browser_window.cc` has one `FactoryHost()` and a process-wide host map
  keyed by colliding bare `WorkspaceId` values.
- macOS input glue stores one event monitor target (`g_key_monitor_strip` and
  `g_key_monitor_window`). One process-wide monitor can remain, but it must
  route by the event's native window through the registry.
- global accessors such as `GetStripController()`,
  `GetActiveCmuxWebContents()`, and `GetCmuxViewsWidget()` have no invocation
  context. Toolbar actions should route from their Browser/WebContents; global
  accelerators should route from the active native window.
- theme application consults only `g_window`; it must resolve the owning
  container or broadcast to registered containers.
- extension containers are now per Browser, but single-window teardown calls
  `DestroyCmuxExtensionsContainer()` globally. Container teardown must destroy
  only extension containers belonging to that native container.
- the process keep-alive and shutdown callbacks assume one Widget. Their owner
  must move to the coordinator and use container count.

## Migration sequence

1. Land and keep the pure registry policy tests green.
2. Introduce a coordinator that owns `NativeWindowState` records, but initially
   register the existing singleton through it. Do not change user behavior.
3. Replace workspace tokens and `Hosts()` with composite locators. Migrate
   BrowserWindow creation before enabling any second Widget.
4. Replace singleton widget/view/strip accessors with explicit container,
   Browser, WebContents, or native-window lookups. Convert teardown and theme
   code to per-container operations.
5. Route the macOS event monitor by `event.window`; make non-mac accelerator
   registration container-local.
6. Add a scoped Browser-creation context carrying intent and invoking
   `NativeWindowId`. Wire cmux New Workspace and extension creation first.
7. Refactor `ShowViewsWebWindow()` into `CreateNativeWindow(profile_domain)`;
   retain a separate `ShowOrCreateInitialWindow()` startup entry point.
8. Enable standard New Window and New Incognito Window only after close,
   shutdown, profile teardown, extension popup, and session-restore tests cover
   two simultaneous native containers.

The current menu should keep native New Window and Incognito disabled until
steps 2-8 are complete; the registry model alone does not make those actions
safe.
