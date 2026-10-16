# cmux application metadata and entitlements

cmux uses one product identity on every platform. On macOS that identity is
stable bundle ID `com.cmux.app`, Apple team `7WLXT3NR37`, and URL scheme `cmux`.
Those values are compiled into Chromium, not merely rewritten after the build,
because Chromium derives its Touch ID/passkey keychain groups from the
compile-time team and bundle identifiers.

The registered macOS channel IDs are `com.cmux.app` (stable),
`com.cmux.app.rc` (release candidate), and `com.cmux.app.nightly` (nightly).
Set `CMUX_MAC_BUNDLE_ID` before `scripts/apply.sh` and keep it set through the
build for a prerelease channel. Tagged dogfood builds do this automatically so
their compiled WebAuthn/keychain identity matches their packaged bundle ID.
`CMUX_BUNDLE_ID` at deploy time is only an assertion against that compiled
identity; deployment never rewrites `CFBundleIdentifier`. Before modifying or
signing an app, deployment also verifies that its framework contains the two
WebAuthn access groups derived from the same bundle ID.

`patches/platform_metadata.py` is applied before every Chromium build. It owns
the native metadata equivalents across platforms:

| Platform | Native identity surface | cmux values |
| --- | --- | --- |
| macOS | `BRANDING`, `Info.plist`, code signature | `com.cmux.app` (or registered channel suffix), team `7WLXT3NR37`, `cmux:` plus HTTP/HTTPS handlers |
| Windows | PE version resources and Chromium install/shell constants | `Manaflow.Cmux` AUMID components, `CmuxHTM` / `CmuxPDF` ProgIDs, `cmux:` protocol, unique installer/COM IDs |
| Linux | desktop entry, package info, XDG integration | `cmux-browser.desktop`, `cmux-browser` executable/icon, `cmux:` plus HTTP/HTTPS MIME handlers |

## macOS signing tiers

The macOS signing setup is tiered because
`com.apple.developer.web-browser.public-key-credential` is a managed
entitlement. The default deploy remains ad-hoc and launchable; passkey-capable
Developer ID signing is opt-in and requires the approved provisioning profile.

`scripts/deploy.sh` signs nested code inside-out, then signs the outer
`cmux-browser.app`. It does not use a blanket `codesign --deep` signing pass.
The helper mapping mirrors Chromium's `chrome/installer/mac/signing/parts.py`:

| Bundle part | Entitlements | Source |
| --- | --- | --- |
| Outer app, Tier 1 | `scripts/entitlements/cmux-app.plist` | Ghostty TCC set plus Chromium unrestricted app entitlements |
| Outer app, Tier 2 | generated from `scripts/entitlements/cmux-app-restricted.plist` | Tier 1 plus cmux/Chromium keychain groups and the managed passkey entitlement |
| `* Helper (Renderer).app` | `scripts/entitlements/helper-renderer-entitlements.plist` | Chromium renderer helper entitlements |
| `* Helper (GPU).app` | `scripts/entitlements/helper-gpu-entitlements.plist` | Chromium GPU helper entitlements |
| Other helpers, tools, frameworks, dylibs | none | Matches Chromium's signing parts |

### Outer-app entitlements

| Entitlement | Tier 1 | Tier 2 | Why/source |
| --- | --- | --- | --- |
| `com.apple.security.automation.apple-events` | yes | yes | Ghostty terminal-side automation/TCC behavior |
| `com.apple.security.device.audio-input` | yes | yes | Ghostty and Chromium microphone access |
| `com.apple.security.device.camera` | yes | yes | Ghostty and Chromium camera access |
| `com.apple.security.personal-information.addressbook` | yes | yes | Ghostty TCC set |
| `com.apple.security.personal-information.calendars` | yes | yes | Ghostty TCC set |
| `com.apple.security.personal-information.location` | yes | yes | Ghostty and Chromium location access |
| `com.apple.security.personal-information.photos-library` | yes | yes | Ghostty and Chromium Photos access |
| `com.apple.security.cs.disable-library-validation` | yes | yes | Ghostty `GhosttyReleaseLocal.entitlements`; appropriate for the vendored libghostty/local signing setup |
| `com.apple.security.device.bluetooth` | yes | yes | Chromium unrestricted browser entitlement |
| `com.apple.security.device.print` | yes | yes | Chromium unrestricted browser entitlement |
| `com.apple.security.device.usb` | yes | yes | Chromium unrestricted browser entitlement |
| `com.apple.application-identifier` | no | yes | Chromium restricted Chrome browser entitlement |
| `keychain-access-groups` | no | yes | Chromium restricted Chrome browser entitlement |
| `com.apple.developer.associated-domains.applinks.read-write` | no | yes | Chromium restricted Chrome browser entitlement |
| `com.apple.developer.web-browser.public-key-credential` | no | yes | Chromium passkey/platform authenticator entitlement |

Tier 1 intentionally contains no `com.apple.developer.*` keys. Chromium's mac
signing README says Chrome-specific entitlements are tied to the official
Google signing identity/certificate and can make local branded builds fail to
launch. Chromium's signing pipeline can embed an outer-app provisioning profile
when its config supplies one; the public Chromium development config returns no
profile. The Chromium comments do not state a passkey-specific provisioning
rule, so cmux exposes `CMUX_SIGN_PROVISION_PROFILE` for the case where Apple
requires a profile for the granted entitlement.

## Using Developer ID Tier 2

All examples in this section are environment fragments for the deploy step
inside the HQ build lease required by [`AGENTS.md`](../AGENTS.md); do not run a
standalone deploy against a script's historical builder default.

cmux compiles its WebAuthn keychain access groups with Apple Team ID
`7WLXT3NR37`. A Tier 2 signing certificate and any provisioning profile must
belong to that same team and must authorize the compiled `com.cmux.app` bundle
ID (or the exact compiled `com.cmux.app.*` channel ID). Deployment rejects both
a configured Team ID and the final code signature if either disagrees with the
compiled team.

Developer ID deploy step:

```bash
CMUX_SIGN_IDENTITY="Developer ID Application: Manaflow, Inc. (7WLXT3NR37)" \
CMUX_SIGN_TEAM_ID="7WLXT3NR37" \
DEST="/path/to/unique-cmux-build.app" \
CMUX_TAG="release-candidate" \
  ./scripts/deploy.sh
```

If the identity string already ends in `(7WLXT3NR37)`, the explicit
`CMUX_SIGN_TEAM_ID` is optional; retaining it makes the compiled/signing
contract visible in automation. If the identity is supplied by hash or has a
different display format, set the fixed Team ID explicitly:

```bash
CMUX_SIGN_IDENTITY="<identity name or hash>" \
CMUX_SIGN_TEAM_ID="7WLXT3NR37" \
DEST="/path/to/unique-cmux-build.app" \
CMUX_TAG="release-candidate" \
  ./scripts/deploy.sh
```

Tier 2 validates that the profile's application identifier is exactly
`7WLXT3NR37.com.cmux.app` (or the selected channel ID) and that it grants
`com.apple.developer.web-browser.public-key-credential` before signing.
Deployment rejects any signing identity/profile team other than
`7WLXT3NR37`, because Chromium compiles that team into both WebAuthn keychain
access groups; changing only the signing team would produce a valid-looking
bundle whose passkeys cannot access their keychain entries.

The released Swift cmux app already embeds a profile for this App ID with the
managed passkey entitlement and wildcard team keychain access. Inspect it with:

```bash
CMUX_SIGN_IDENTITY="Developer ID Application: Manaflow, Inc. (7WLXT3NR37)" \
CMUX_SIGN_TEAM_ID="7WLXT3NR37" \
CMUX_SIGN_PROVISION_PROFILE="/path/to/profile.provisionprofile" \
DEST="/path/to/unique-cmux-build.app" \
CMUX_TAG="release-candidate" \
  ./scripts/deploy.sh
```

Export the approved profile to a private build-secret location and pass its
path through `CMUX_SIGN_PROVISION_PROFILE`; do not commit signing assets. The
build machine also needs the matching Developer ID Application certificate and
private key.

The Tier 2 keychain groups include both the existing cmux group and Chromium's
`.webauthn` / `.webauthn-uvk` groups. The first preserves access to items from
the existing app; the latter two match Chromium's compiled Touch ID and enclave
keychain lookups. Touch ID/iCloud Keychain passkeys are unavailable in Tier 1
by design, while USB/Bluetooth security-key flows can still use Chromium's
non-platform FIDO transports.

## Windows

There is no Windows entitlement or Appx manifest requirement for this
unpackaged Win32 browser. Chromium's installer registration is the
`Info.plist` equivalent: it writes RegisteredApplications/Capabilities, URL
and file associations, ProgIDs, and the AppUserModelID using cmux's install
mode constants.

Passkeys use Chromium's native Windows WebAuthn path through Windows Hello:

- `device/fido/BUILD.gn` includes `win/authenticator.cc`, `win/discovery.cc`,
  `win/webauthn_api.cc`, and `//third_party/microsoft_webauthn` under `is_win`.
- `WebAuthenticationUseNativeWinApi` is enabled by default.
- `device/fido/win/webauthn_api.cc` loads `webauthn.dll` from System32 and
  binds `WebAuthNAuthenticatorMakeCredential` and
  `WebAuthNAuthenticatorGetAssertion`.
- Windows 10 version 1903 or later supplies the Win32 WebAuthn API. Package
  identity is not required.

Windows microphone, camera, and location prompts are handled by per-app privacy
settings for desktop apps. There is no deploy-time entitlement to add.

## Linux

There is no Linux entitlement system. The checked-in
`packaging/linux/cmux-browser.desktop` declares the browser MIME/protocol
handlers, desktop actions, categories, and executable/icon identity for raw
release packages. Chromium's Linux packaging metadata is patched to the same
values.

Chromium's Linux WebAuthn support uses the normal FIDO stack:

- `device/fido/BUILD.gn` includes CTAP/FIDO HID sources when Blink is enabled
  and the platform is not Android.
- `device/BUILD.gn` requires udev for FIDO HID on Linux; builds with
  `use_udev=false` do not provide that transport.
- caBLE/hybrid support is present in `device/fido/cable/*`.

CTAP2 security keys need the distribution's normal udev/hidraw access rules.
Linux does not provide a macOS-style system platform-authenticator entitlement;
hardware keys, hybrid/phone flows, and browser/password-manager-provided
passkeys are the relevant paths. PipeWire and xdg-desktop-portal are separate
runtime permission surfaces for media and screen capture, not app metadata.
