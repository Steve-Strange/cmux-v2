# Chrome extension architecture

cmux workspaces are Chromium windows, not a parallel Browser-less tab system.
Each workspace owns one real `Browser`, and every web surface is a presentation
of a `WebContents` owned by that Browser's `TabStripModel`.

## Object mapping

| cmux concept | Chromium concept |
| --- | --- |
| workspace | `Browser` / `BrowserWindow` |
| web tab | `TabStripModel` entry and Browser-owned `WebContents` |
| pane | spatial presentation of one or more Browser tabs |
| cmux app window | shared physical Views `Widget` hosting the workspaces |

`CmuxBrowserWindow` is the production `BrowserWindow` adapter. Window
operations select, focus, size, or close the corresponding workspace in the
shared widget. Chromium-created normal windows (including
`chrome.windows.create`) are adopted as new workspaces by the browser-window
factory hook. Non-normal Browser types retain Chromium's ordinary BrowserView
factory path.

## Tabs and extension APIs

The Browser tab model is authoritative for web-tab identity, lifetime, active
selection, and extension API visibility. cmux observes native insert, remove,
replace, move, and selection changes and projects them into pane tabs. cmux tab
operations call the native Browser/tab APIs and synchronize the Browser's
linear order from the spatial pane ordering.

Terminal tabs remain cmux-only and are intentionally absent from
`chrome.tabs`. Removing the final web tab does not close a workspace that still
contains a terminal, but an explicit `chrome.windows.remove` still closes it.

For an extension such as OneTab, one workspace is therefore one browser
window. It sees every web tab across that workspace's panes in the Browser's
linear tab order, can replace them with its single extension summary tab using
the ordinary `chrome.tabs` API, and does not see terminal tabs. A pane is a
layout projection, not another extension-visible window.

## Extension actions

Pinned buttons use Chromium's `ToolbarActionView`,
`ExtensionActionViewModel`, and `ExtensionActionDelegateDesktop`. This keeps
the standard action execution, `activeTab` grant, popup host, context menu,
API-triggered popup, and click-again-to-close behavior.

Multiple workspace Browsers and pane toolbars share one physical Views focus
manager. `CmuxExtensionAcceleratorRouter` therefore registers each extension
shortcut once per physical focus manager and dispatches it to the action model
for the active workspace; registering every native delegate independently
would violate Chromium's one high-priority handler invariant.

`CmuxExtensionsContainer` is per Browser and is attached to that Browser's
`BrowserWindowInterface` user-data host. There is no fake BrowserWindow shim or
custom extension-popup implementation.

## Extension side panels

The workspace Browser owns Chrome's ordinary global `SidePanelRegistry` and
`ExtensionSidePanelManager`. Each web-tab presentation owns a small cmux host
for the selected entry's upstream `ExtensionViewHost`; cmux does not implement
an extension-webview substitute.

Contextual entries are tab-scoped and follow their `WebContents` when a tab
moves between panes. Because several panes can show selected web tabs at once,
their contextual panels can also be visible at once. A global entry remains
workspace-scoped and is presented only beside the active web tab in the
focused pane. Terminal tabs have no side-panel or extension-visible tab state.

The host is a child of the tab surface's horizontal layout. Opening it consumes
page width inside the existing pane instead of changing pane, column, workspace,
or native-window dimensions. It starts on the right, follows Chrome's side-panel
alignment preference, and shrinks before the page when the pane is narrow.

cmux keeps its own browser data directory. On macOS and Linux, native messaging
therefore checks cmux's product directory first and Google Chrome's standard
per-user `NativeMessagingHosts` directory second. The normal Chromium manifest
parser still validates the calling extension against `allowed_origins` before
launching the host. This lets an existing Claude Code host registration work
without copying or mutating its manifest.

## Startup

Chrome may construct an ordinary bootstrap BrowserView before the cmux widget
and workspace Browsers exist. A short-lived startup suppressor hides that one
window, then unregisters and closes it immediately after workspace Browser
construction. It is not used for normal cmux operation.

## Known UI-adapter boundary

Features whose BrowserWindow contract requires a dedicated Chrome frame UI
(for example bookmark, sharing, download, permission, or translate bubbles)
need a pane-local cmux presentation before their BrowserWindow methods can be
non-no-op. This is a UI-hosting boundary, not a tab or extension execution
boundary; extension background logic, content scripts, messaging, DNR,
`chrome.tabs`, `chrome.windows`, and native extension actions run against real
Chromium Browser state.
