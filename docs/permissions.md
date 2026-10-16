# Web permissions and security UI

cmux does not implement a parallel permission store or invent its own prompts.
Each web pane belongs to a real Chromium `Browser`, uses Chromium's
`PermissionRequestManager` and content settings, and exposes its production
`LocationBarView` through the generic `BrowserWindow` contract. Decisions
therefore persist, expire, embargo, and appear in page info/site settings in
the same way as the pinned desktop Chromium revision.

The custom-window compatibility layer exists only because a few Chromium Views
call sites still look up `BrowserView` directly. cmux intentionally has no
`BrowserView`: several pane-local browser surfaces share one top-level Widget.
`patches/custom_window_permissions.py` makes those call sites use
the requesting `WebContents` to resolve the matching pane's location bar and
provides safe custom-window fallbacks. Ordinary `BrowserView` windows retain
their action-specific Chromium anchors. The patch is version-gated and fails
closed if an upstream Chromium 151 anchor moves.

The same Chromium 151 hunks are mirrored at the stable
`helium-settings-chromium-151.patch` path. Supported older branches already
reverse that path before refreshing a warm builder, so switching away from a
newer branch restores the shared checkout even when the older branch predates
the dedicated permission restorer.

## Supported request families

The support target is every request surface available in pinned desktop
Chromium, not a hand-maintained subset of individual websites.

| Family | Examples | cmux integration |
| --- | --- | --- |
| Standard permission requests | geolocation; notifications; camera, microphone, and camera pan/tilt/zoom; clipboard; sensors; MIDI SysEx; idle detection; local fonts; local and loopback network; window management | Chromium permission chip, bubble, quiet UI, decision persistence, and content-setting icon through the pane's real `LocationBarView` |
| Immersive/device capability requests | AR, VR, hand tracking, captured-surface control, pointer lock, keyboard lock | Chromium standard or exclusive-access permission UI; the pane's native WebContents view and shared native window remain the security parent |
| Storage and identity requests | top-level storage access, Storage Access API, identity provider, disk quota, File System Access | Chromium standard prompts; File System Access usage/restore bubbles use the generic location-bar anchor |
| Browser behavior requests | multiple downloads, protocol-handler registration, web-app installation | Chromium standard prompts and page actions; `CmuxBrowserWindow` refreshes the pane location bar when Chromium updates a page action |
| Device chooser requests | WebUSB, WebHID, Web Serial, Web Bluetooth | Chromium chooser controller and device list; website and extension requests use a security-level bubble anchored to the pane location bar |
| Element-based permissions | the HTML `<permission>` element and its secondary prompt | Chromium embedded prompt; element coordinates fall back to the custom WebContents container when no `ContentsWebView` registry element exists |
| Tab-modal security UI | WebAuthn/passkeys and security keys, File System Access confirmations, collected site data, and other constrained web dialogs | a pane-aware `WebContentsModalDialogHost` supplies native parenting, maximum size, focus policy, reposition notifications, and host-destruction notification |
| Capture and presentation | screen/window/tab capture picker, Presentation API, Cast/global-media UI, media-remoting confirmation | Chromium's native picker and existing no-`BrowserView` content-bounds fallback; remoting confirmation uses the generic location-bar anchor |
| Site controls | blocked-content icons, cookies/site data, media settings, page info, per-site resets | the production `BrowserContentSettingBubbleModelDelegate`, not the previous no-op stub |

The desktop request list is defined upstream in
`components/permissions/request_type.h`. Platform-only types remain
platform-only: for example, Web NFC is not a desktop Chromium request, while
protected-media identifiers are available only on the desktop platforms for
which Chromium enables them. cmux should neither advertise nor emulate a Web
Platform feature that its pinned Chromium build does not provide.

## Browser permission versus OS permission

A web decision and an operating-system decision are distinct. Allowing camera,
microphone, geolocation, Bluetooth, USB, or screen capture in a site prompt
does not override a denial in macOS Privacy & Security, Windows privacy
settings, Linux device access rules, or a portal picker. Chromium owns that
second-stage request and reports denial back to the page.

On macOS, `docs/entitlements.md` is the source of truth for the bundle's TCC
usage descriptions, unrestricted browser entitlements, signing tiers, and
WebAuthn keychain groups. The standard dogfood tier supports external/hybrid
FIDO transports; Touch ID/iCloud Keychain platform passkeys require the
approved Tier 2 identity and provisioning profile.

Normal Web Platform constraints still apply:

- many capabilities require HTTPS, a top-level or delegated permissions
  policy, and a recent user gesture;
- chooser APIs require a matching physical device and may require OS/driver
  access;
- private/incognito profiles may use different lifetime and persistence rules;
- enterprise policy, Safe Browsing, or Chromium abuse mitigation can suppress
  or embargo a request;
- an OS-level denial may require the user to re-enable cmux in system settings.

## Regression coverage

`scripts/test-custom-window-permissions.py` verifies every pinned-upstream
transformation, reversible cross-branch warm-workspace refresh,
BrowserWindow-to-pane plumbing, the production content-settings delegate, and
the modal-host lifecycle contract. It runs from `scripts/test-build.sh`.

`CMUX_PERMISSION_SELFTEST=1` performs a runtime structural check after startup:
it locates attached web surfaces (creating a visible peer pane when needed),
verifies that active and non-active visible `WebContents` resolve to their own
production location bars, rejects fully ancestor-clipped surfaces as hidden,
refreshes content-setting icons, obtains the WebContents modal host, validates
its native parent, geometry, activation, and response to pane movement and
ancestor clipping, including fully visible translations, and confirms the
production content-settings delegate is installed.

For a UI handoff, also exercise at least:

1. `speedtest.net` → **Go** → geolocation prompt can be allowed or denied
   without a crash and the test proceeds.
2. Camera and microphone request → site prompt, then any applicable OS prompt.
3. Notifications or clipboard request → decision appears in page info and can
   be reset.
4. One chooser-capable flow (USB/HID/Serial/Bluetooth) when hardware is
   available; cancel must be safe when none is available.
5. A File System Access picker/confirmation and its retained-access page
   action.
6. WebAuthn with an external security key or hybrid transport; run a Tier 2
   build separately for Touch ID platform-passkey verification.

When the Chromium pin changes, re-audit permission, file-system-access,
WebAuthn, desktop-capture, and media-router Views for new direct
`BrowserView::GetBrowserViewForBrowser()` assumptions before updating the
patch anchors.
