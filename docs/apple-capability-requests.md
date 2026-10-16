# Apple capability request tracker

Last checked: 2026-07-23 (America/Los_Angeles)

Apple Developer team: `7WLXT3NR37` (Manaflow, Inc.)

This file tracks Apple capability requests that block signing or release work.
Keep Apple follow-up/request IDs separate from Developer Portal App ID resource
IDs and Developer Support case numbers.

## Current cmux browser bundle family

| Channel | Bundle ID | Capability | Submitted | Apple follow-up/request ID | Status |
| --- | --- | --- | --- | --- | --- |
| Stable | `com.cmux.app` | Web Browser Public Key Credential Requests | 2026-07-23 (resubmitted) | `7QU68Z5TJY` | Submitted; confirmation page and portal request history verified 2026-07-23 |
| Release candidate | `com.cmux.app.rc` | Web Browser Public Key Credential Requests | 2026-07-22 | `TVQL5SC7VL` | Submitted; portal request history confirmed 2026-07-23 |
| Nightly | `com.cmux.app.nightly` | Web Browser Public Key Credential Requests | 2026-07-22 | `UKP2N354KH` | Submitted; portal request history confirmed 2026-07-23 |

The first stable submission on 2026-07-22 returned to its initial state instead
of showing the same persistent thank-you screen as RC and nightly. On
2026-07-23, the Developer Portal confirmed submitted requests for RC and
nightly but showed `No Requests` for stable. Stable was resubmitted that day
with the reviewer release, direct download, WebAuthn/iCloud Keychain details,
and support case `102838399830`. Apple returned request ID `7QU68Z5TJY`, and
the Developer Portal then showed the request as `Submitted`.

When Apple sends a confirmation or status email, record the ID in the matching
row above before replying. Search the Apple Account mailbox for the bundle ID,
`Web Browser Public Key Credential Requests`, or `entitlement request status`.
No matching Apple email had arrived as of the 2026-07-23 check.

## Follow-up path

- Existing Developer Support case: `102838399830`
- Apple contact: reply to the case email from `devprograms@apple.com`
- Existing Developer Support messaging URL is in that email thread.
- Provide bundle ID, request ID, submission date, business need, and a reviewer
  build/download URL in every follow-up.

Suggested first follow-up date: 2026-07-29 if no new request IDs or status mail
arrives sooner.

## Reviewer build

- Public host repository: `manaflow-ai/cmux`
- Isolated tag: `browser-apple-review-20260722.1`
- Release title: `cmux Browser — Apple capability review (macOS arm64)`
- Public release:
  `https://github.com/manaflow-ai/cmux/releases/tag/browser-apple-review-20260722.1`
- Published: 2026-07-23 as a prerelease (not latest)
- Asset: `cmux-browser-apple-passkey-review-macos-arm64.zip`
- Direct download:
  `https://github.com/manaflow-ai/cmux/releases/download/browser-apple-review-20260722.1/cmux-browser-apple-passkey-review-macos-arm64.zip`
- Size: 579,807,089 bytes
- SHA-256:
  `a6719ce7919fb010d5f97d93c80cb041b975481960e814bffdd0545b8745340e`
- Build: macOS arm64, bundle ID `com.cmux.app`, ad-hoc signed Tier 1;
  intentionally not notarized or production-signed while the managed passkey
  capability remains pending

Verification completed before publication: the ZIP passed an integrity test;
the deployed app passed strict deep code-signature verification; its packaged
framework contains both `7WLXT3NR37.com.cmux.app.webauthn` and
`7WLXT3NR37.com.cmux.app.webauthn-uvk`; and its `Info.plist` registers `http`,
`https`, `file`, and `cmux` URL schemes. The standalone app launched with an
isolated profile and reached the cmux window startup path. Both the public
release page and unauthenticated asset download returned HTTP 200 after
publication.

The reviewer tag intentionally does not start with `v`, `browser-v`,
`browser-release-candidate`, or `browser-nightly`. It therefore stays outside
both the legacy Swift cmux release automation and the cmux-browser updater
channels. Publishing it as a prerelease also prevents it from replacing the
normal latest release.

## Default browser

No Apple entitlement request was filed for macOS default-browser status. Apple's
default-browser entitlement request form is for iOS, iPadOS, and visionOS. On
macOS, cmux declares the `http` and `https` handlers in `Info.plist`, registers
through Launch Services, and the user chooses it in System Settings.

## Legacy Swift cmux history

These IDs concern the old `com.cmuxterm.app*` family. Do not reuse them as the
request IDs for the new `com.cmux.app*` family.

| Bundle ID / scope | Request or case ID | Submitted | Result / notes |
| --- | --- | --- | --- |
| `com.cmuxterm.app` | `348J98Y8QC` | 2026-03-03 | Followed up under support case `102838399830`; entitlement later approved |
| Prior entitlement follow-up | `102843597461` | Before 2026-03-17 | Referenced by Apple Support from case `102838399830` |
| `com.cmuxterm.app.nightly` | `H6Z59XSY57` | 2026-03-19 | Approved; Apple email received 2026-04-06 |
| `com.cmuxterm.app.rc` | `8TV25R6AJ5` | 2026-07-10 | Pending as of the 2026-07-17 follow-up |
| Passkey entitlement support thread | `102838399830` | 2026-03-06 | Active umbrella Developer Support case |

`102940597294` is an unrelated MDM Vendor CSR Signing Certificate support case;
do not cite it in passkey follow-ups.
