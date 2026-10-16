# Durable workspace and terminal-host implementation

Status: implementation, crash, resize, mouse/color, 1/10/50/100 cold-policy
performance dogfood, review-stack publication, and signed direct/attached
interaction dogfood for the synchronized current pins are complete
Last updated: 2026-07-21

This branch is stacked on `codex/cmux-durable-terminal-hosts`. Its current
cmux TUI dependency is `codex/cmux-tui-frontend-parity` (cmux PR
[#8476](https://github.com/manaflow-ai/cmux/pull/8476)), stacked on the viewer
lease PR [#8475](https://github.com/manaflow-ai/cmux/pull/8475) and the durable
state-root, input-barrier, resize-ack, short-socket,
canonical-terminal-placement, and workspace
registry branches. Review stacks:

- cmux TUI: [manaflow-ai/cmux#8422](https://github.com/manaflow-ai/cmux/pull/8422)
- cmux TUI short-socket follow-up: [manaflow-ai/cmux#8444](https://github.com/manaflow-ai/cmux/pull/8444)
- cmux TUI resize/input/state/frontend follow-ups: [#8452](https://github.com/manaflow-ai/cmux/pull/8452), [#8458](https://github.com/manaflow-ai/cmux/pull/8458), [#8460](https://github.com/manaflow-ai/cmux/pull/8460), [#8475](https://github.com/manaflow-ai/cmux/pull/8475), [#8476](https://github.com/manaflow-ai/cmux/pull/8476)
- cmux Browser: [manaflow-ai/cmux-browser#13](https://github.com/manaflow-ai/cmux-browser/pull/13)
- cmux Browser integration follow-up: [manaflow-ai/cmux-browser#14](https://github.com/manaflow-ai/cmux-browser/pull/14)
- Ghostty: [manaflow-ai/ghostty#123](https://github.com/manaflow-ai/ghostty/pull/123)

## Invariants

- cmux TUI is the only writer for shared workspace and terminal lifecycle
  state. Browser models are disposable projections.
- Every durable entity is addressed by a stable UUID. Runtime numeric IDs are
  scoped to a server generation and never cross a restart boundary.
- A workspace mutation is committed durably before its ordered event or reply
  is published. A retry with the same mutation ID returns the original result.
- Browser destruction, renderer destruction, attachment loss, and daemon loss
  do not implicitly terminate a shell.
- Each terminal host owns exactly one PTY and child process. Each realized
  Ghostty renderer is independently disposable.
- Workspace/control traffic is typed and ordered. Terminal data bypasses the
  Chromium browser process. Shared memory is restricted to leased IOSurfaces.
- Every terminal checkpoint, output chunk, resize, and frame is tagged with an
  epoch/sequence so stale or missing data is detected rather than displayed.

## Work list

### 1. Durable ordered workspace service

- [x] Replace concurrent mutate-then-publish paths with one serialized command
  actor that owns commit and event order.
- [x] Add a durable session generation UUID and monotonic registry revision.
- [x] Add mutation UUID and origin client UUID to every shared mutation.
- [x] Persist committed mutations and their results for retry idempotency.
- [x] Recover registry records, tombstones, revision, and dedupe results after
  daemon restart.
- [x] Add a barriered watch API that returns either a snapshot or contiguous
  deltas from a requested generation/revision.
- [x] Add schema migration, corrupt-tail recovery, atomic checkpointing, and
  crash-injection tests.

### 2. Canonical workspace projection

- [x] Scope workspaces by stable browser profile/window-group UUID.
- [x] Persist schema-versioned frontend layout payloads beside workspace
  records so lifecycle and presentation updates are transactional.
- [x] Persist stable terminal UUID placement; do not persist daemon-local IDs.
- [x] Make Chromium create pending projections and materialize only committed
  server records.
- [x] Remove placeholder matching by order and local tombstone retry state.
- [x] Rebuild `WindowModel` entirely from the barriered snapshot/delta stream.

### 3. Per-terminal host process

- [x] Introduce a terminal-host executable/mode owning one PTY and child.
- [x] Move replay/checkpoint, output sequencing, resize arbitration, and child
  lifecycle behind the host protocol.
- [x] Keep hosts alive when cmux TUI or a frontend exits.
- [x] Persist host identity/endpoint metadata and adopt surviving hosts after a
  daemon restart.
- [x] Require an explicit committed terminate operation to kill the child.
- [x] Bound replay/output storage and define deterministic gap recovery.

### 4. Direct renderer data plane

- [x] Add a capability-scoped binary framed attachment for one terminal UUID.
- [x] Broker an already-connected attachment handle into the renderer process.
- [x] Send checkpoint/output/input/resize messages directly between terminal
  host and renderer; keep browser UI out of the byte path.
- [x] Keep clipboard, accessibility, drag/drop, and privileged actions on Mojo.
- [x] Retain the JSON protocol as a compatibility/control surface without
  base64 on the renderer hot path.
- [x] Add flow-control, malformed-frame, partial-write, disconnect, overflow,
  and capability-isolation tests.
- [x] Serialize renderer output/state/control/semantic operations, fence
  semantic encoders behind older output, let cancel/detach bypass an invalid
  exact-state gate, and preserve final state before direct Exit detaches.
- [x] Retain idempotent per-button mouse-release tombstones and token-match
  non-blocking release acknowledgments across renderer/direct-stream
  replacement.

### 5. Grid and frame epochs

- [x] Give every viewer resize request a sequence and every committed PTY grid
  a grid epoch.
- [x] Tag checkpoints/output affected by resize and every rendered frame with
  the committed grid epoch.
- [x] Reject stale geometry without stretching or clearing the last good frame.
- [x] Add an explicit Ghostty IOSurface acquire/release lease so a surface is
  never reused before the browser acknowledges it.
- [x] Test continuous grow/shrink, competing viewer minima, display-scale
  changes, delayed frames, dropped acknowledgements, and renderer restart.

### 6. End-to-end validation

- [x] cmux TUI unit and protocol conformance suites.
- [x] Browser host/unit tests and full Chromium build.
- [x] Exact pinned Ghostty ABI build and tests.
- [ ] Workspace create/move/close through the GUI/TUI protocol paths under
  live concurrency. Retry-safe GUI rename transport and registry concurrency
  are covered independently; the combined interactive stress gate remains.
- [x] Browser, renderer, daemon, and terminal-host crash/restart matrices.
- [x] Mouse press/release/drag/wheel and keyboard parity, including BackTab, on
  GUI and attached-TUI frontends.
- [ ] Native IME composition dogfood on both frontends.
- [x] Pixel/color comparison and continuous-resize visual inspection.
- [x] Multi-window/profile isolation and restart persistence.
- [x] Publish stacked draft PRs with exact dependency revisions and evidence.

## Current synchronized validation status

The browser now pins cmux TUI
`cbac0eb05454ed02a50a24675c62dd7265f17fef`, synchronized on canonical cmux
main `4daa93725694d51feae628426e878ccb73eabe6f`, and Ghostty
`d6f611a3077aa12510761ca10e2a5e0a93979536`. Browser commit `255a9ae` fixes a
physical-key AppKit bug in which a mutable `insertText:` value aliased marked
text and was cleared before delivery. Scripted Computer Use `type_text` does
not exercise that `NSEvent` path.

A partial remote incremental also demonstrated that a newer helper can be
packaged beside a stale compiled Framework. Runtime identity verification
rejected that bundle, and deployment now runs
`scripts/verify-cmux-runtime-pins.sh` before helper installation/signing to
require both checkout pins in the built Framework or `libchrome_dll.dylib`.
A fresh complete build produced the deep-signed
`/Users/cmux-lawrence/Applications/cmux-browser-physical-typing-v2.app` and
the independently built, deep-signed
`/Users/cmux-lawrence/Applications/Ghostty-cmux-pinned.app`. Runtime checks
confirmed the exact cmux/Ghostty pair in the compiled image and helper.

Individual Computer Use `press_key` actions entered `echo physok` directly and
`echo tuiok` through the pinned Ghostty attached TUI; both outputs rendered.
The accessibility-enabled launch also found and fixed a first-paint DCHECK:
the focusable rail row and native terminal host now publish valid role/name
metadata.

The direct and attached mouse probes each recorded exactly one press/release
pair at `46,15` and `42,20`, respectively. Direct grids changed
`175x60 -> 142x51 -> 135x48`; attached grids changed
`88x37 -> 71x29 -> 124x42`, with immediate native-size reflow and no stretched
frame or scale flash. All 256 sampled indexed colors classified to the correct
canonical entry in both captures; their mean cross-frontend maximum-channel
delta was 1.16 and the JPEG-edge maximum was 15. OSC reset restored exact
`#272822` background pixels and the configured thin bar cursor in both paths.
The signed-v4 evidence below remains historical.

## Prior signed v4 evidence

The prior signed main application is
`/Users/cmux-lawrence/Applications/cmux-browser-frontend-scaling-v4.app`, with
bundle ID `com.cmuxterm.app.dogfood.build-frontend-scaling-v4` and isolated
profile `cmux-browser-dogfood-frontend-scaling-v4`. The independent embedded-
Ghostty attachment host is
`/Users/cmux-lawrence/Applications/cmux-browser-v4host.app`, with deliberately
short bundle ID `com.cmuxterm.app.dogfood.v4host` and profile
`cmux-browser-dogfood-v4host`. A longer companion bundle ID made Chromium's
derived app-shim bootstrap endpoint exceed the Mach bootstrap name limit and
fail in `MachBootstrapAcceptor::Start`; the short ID is required for a working
signed companion.

Both apps pass deep strict code-sign verification and contain signed helper
SHA-256
`3f3ffdc2e200165ed61a7270d2b4b74d0fece24a8a1d098a1d99744514e05db9`.
The corresponding unsigned staged helper SHA-256 begins `1e1fcb11`. Its
receipts pin cmux TUI `541d4dd908c9a1265b6abe038c959e3f271de33e` and
Ghostty `8c645641a1dd1cbff9aa73189629682c01b1a233`, and its exact manifest
contains 585 runtime resources.

The attach command used for that historical run was:

```sh
/Users/cmux-lawrence/Applications/cmux-browser-frontend-scaling-v4.app/Contents/Helpers/cmux-tui attach \
  --session cmux-browser-e1b0b468b090a95d7fef3826ab18a43d \
  --socket /tmp/cmux-tui-501/cmux-browser-e1b0b468b090a95d7fef3826ab18a43d.sock
```

The run retained registry identity
`1e3a8a8a-b6ea-46b4-bff4-cb15413b88a8`, daemon generation
`175578a5-535a-48be-aa7d-b2851c896614`, and Home workspace UUID
`3a2e31da-5199-4f34-b1bc-5dfa9ec6f9a6`. Computer Use exercised the direct
Browser Ghostty renderer and the signed attached-TUI Ghostty frontend against
the same terminal. Their 256-cell palette captures matched at tolerance 15;
left-click, drag/release, wheel, and right-click reached both paths. The direct
shell exposed its native Copy/Paste context menu, the attached TUI exposed its
pane context menu, and repeated grow/shrink retained native-size frames over
the resolved terminal background without stretching or transient scale flash.

Killing only renderer utility PID 32274 produced replacement PID 35182 while
Browser PID 26073, daemon PID 26106, and terminal-host PID 26119 remained.
Terminal UUID `05386a44205145fcb1a2e66bf7beac8d`, incarnation
`e1056602192243ff90b30fd1048f6fc3`, scrollback, post-crash input, and the
attached view also remained. This validates the intended boundary: one
disposable Ghostty OS process can fail without replacing the Browser, cmux TUI
daemon, PTY-owning host, or canonical registry state.

The complete signed-v4 frontend run measured 801,063,544 bytes at one terminal;
824,989,104 bytes for 10 initially-cold terminals; 1,381,351,056 bytes for 10
all-warm renderers; 942,871,984 bytes after their 30-second eviction;
1,064,201,392 bytes for 50 cold-hidden terminals; and 1,249,106,120 bytes for
100 cold-hidden terminals. The final checkpoint retained 100 PTY-owning hosts
and only one Ghostty renderer, using 75.5% less physical footprint than the
historical 5,101,539,184-byte 100-renderer run. Computer Use observed queued
input in an evicted terminal within a 610 ms action-to-fresh-state upper bound;
sidebar switch bounds remained at or below 1,002 ms per leg at all four scales.
Cleanup removed all 99 benchmark-owned terminals and restored the original
terminal-host identity. The atomic harness state and checkpoints are at
`/Users/cmux-lawrence/fun/cmux-browser-artifacts/cmux-workspace-idempotency/frontend-scaling-v4/scaling-state.json`.

## Historical signed evidence

Computer Use drove both signed Chromium bundles: the direct embedded Ghostty
frontend and a separate embedded Ghostty terminal attached to the same cmux TUI
session. All 256 indexed-color cells matched with zero mismatches at a JPEG
capture tolerance of 15; exact sparse OSC 4 index 1 rendered as `#ff3562` in
both paths. Real click, drag, release, and wheel events reached the same probe
through each frontend. Repeated window shrink/grow operations produced only
native-size retained frames plus the resolved OSC 11 background, with no
stretched palette frame. The standalone Ghostty bundle remains outside the
Computer Use safety boundary, so the signed attached-TUI-host app supplies the
independent Ghostty frontend for this matrix.

The previously deployed 691 MB review app at
`/Users/cmux-lawrence/Applications/cmux-browser-canonical-terminal-20260718.app`
uses cmux TUI `8e4965dd2b0bb671daafe305b102768f264294ef` and Ghostty
`8c645641a1dd1cbff9aa73189629682c01b1a233`; its 39-step M4 rebuild and deep
code-sign validation are inherited base evidence only. At that checkpoint, the
stack pinned cmux TUI `541d4dd908c9a1265b6abe038c959e3f271de33e`.
Its code head passed 645
tests across 21 suites plus formatting and diff checks; the final
documentation-only head preserves that code. Browser's 20 host suites pass,
including 127 renderer-ordering checks and all 82 TUI protocol checks. The
signed `d4dcef408...` visual/crash app remains historical evidence rather than
the artifact for that checkpoint.

The isolated signed apps are
`/Users/cmux-lawrence/Applications/cmux-browser-workspace-idempotency.app`
(`com.cmuxterm.app.dogfood.build-workspace-idempotency`) and
`/Users/cmux-lawrence/Applications/cmux-browser-tui-host.app`
(`com.cmuxterm.app.dogfood.tui-host`). Both pass deep strict code-sign
verification and contain the same helper SHA-256
`b4d42913467bfde12100474f896948af4ca231d92bb699910c972f925a8fc233`,
the pinned Ghostty receipt, and the 585-entry runtime-resource manifest.

Live upgrade dogfood rejected replacement while a second `native-browser`
owned the session, then replaced the allowlisted legacy daemon without changing
the registry identity or any pre-existing terminal-host PID. Empty GUI
workspaces appeared immediately in TUI, TUI workspace/tab creation and
selection appeared immediately in GUI, and the detailed `tab-added` event now
triggers the same selection refresh as `tree-changed`. Finally, killing the
selected renderer PID 29183 created replacement PID 35430 while browser PID
28547, daemon PID 18862, terminal-host PID 29182, terminal UUID/incarnation,
palette, scrollback, and attached-TUI view all survived.
