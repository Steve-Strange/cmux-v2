# cmux TUI workspace registry and renderer-process plan

Status: durable registry, direct renderer data plane, per-terminal isolation,
cold-residency implementation, and current review-stack/source validation are
complete; the final in-place signed current-pin rebuild and direct/attached
Computer Use dogfood remain pending
Last updated: 2026-07-22

Related work:

- cmux-browser PR [#4](https://github.com/manaflow-ai/cmux-browser/pull/4)
- stacked cmux-browser PR [#7](https://github.com/manaflow-ai/cmux-browser/pull/7)
- durable terminal-host/renderer PR [#13](https://github.com/manaflow-ai/cmux-browser/pull/13)
- stacked workspace-idempotency/dogfood PR [#14](https://github.com/manaflow-ai/cmux-browser/pull/14)
- cmux TUI browser-backend PR [#8294](https://github.com/manaflow-ai/cmux/pull/8294) (merged)
- cmux TUI protocol-v7/custom-frontend PR [#8031](https://github.com/manaflow-ai/cmux/pull/8031) (merged)
- cmux TUI stable-split-ID PR [#8023](https://github.com/manaflow-ai/cmux/pull/8023) (merged)
- cmux TUI smallest-viewer resize PR [#8302](https://github.com/manaflow-ai/cmux/pull/8302) (merged)
- stacked cmux TUI workspace-registry PR [#8340](https://github.com/manaflow-ai/cmux/pull/8340)
- closed cmux TUI canonical-terminal PR [#8422](https://github.com/manaflow-ai/cmux/pull/8422)
- current-main replacement canonical-terminal PR [#8505](https://github.com/manaflow-ai/cmux/pull/8505), head `43b52ba3e67bbd54f4240f8782c0ee435ed34765`
- canonical cmux main `49a43bfa0fd1c33ec458a0bb4ecfbd7a6619e16e`
- stacked cmux TUI short-socket PR [#8444](https://github.com/manaflow-ai/cmux/pull/8444), head `02f7680f4c00960c3d59ab62e80113a98a76dc6b`
- stacked cmux TUI resize-ack PR [#8452](https://github.com/manaflow-ai/cmux/pull/8452), head `43e971711daff8aa9eefd99766142738b6302baa`
- stacked cmux TUI input-barrier regression PR [#8458](https://github.com/manaflow-ai/cmux/pull/8458), head `bb38dd9f66054ffeb36dd59f8ffe896ad731e9c3`
- stacked cmux TUI durable-state-root PR [#8460](https://github.com/manaflow-ai/cmux/pull/8460), head `5dbd5762cc131989d87aafbcfa95e0e59b8b2c61`
- stacked cmux TUI viewer-lease release PR [#8475](https://github.com/manaflow-ai/cmux/pull/8475), head `59dfa84d2eb69e7e0e0f41a17a865000490bdaec`
- stacked cmux TUI frontend-parity PR [#8476](https://github.com/manaflow-ai/cmux/pull/8476), head `ab5028a2c352117908b8cd5af6935916477e5c27`
- cmux TUI integration candidate `ab5028a2c352117908b8cd5af6935916477e5c27`
- Ghostty completed-frame PR [#121](https://github.com/manaflow-ai/ghostty/pull/121)
- Ghostty external-Metal-presenter PR [#122](https://github.com/manaflow-ai/ghostty/pull/122)
- stacked Ghostty integration PR [#123](https://github.com/manaflow-ai/ghostty/pull/123)
- stacked Ghostty cursor/replay API PR [#127](https://github.com/manaflow-ai/ghostty/pull/127)
- stacked Ghostty mutable-default color-reset PR [#128](https://github.com/manaflow-ai/ghostty/pull/128)
- Ghostty integration candidate `d6f611a3077aa12510761ca10e2a5e0a93979536`

Browser stack point: stacked integration PR
[#14](https://github.com/manaflow-ai/cmux-browser/pull/14), based on durable-host
PR #13. Browser main is `5bd08a31ad239b9a9cb8c9b794354b06c1e28bb1`;
the latest local #14 product commit is
`a234c9ff383c66ca0994266ce6901d62f62f96e3`, while the published #14 head
remains `2b2317c` pending final verification and push.

Feasibility conclusion: the landed protocol-v7 attach surface plus the stacked
workspace registry, canonical terminal placement, frontend projection, color,
selection, viewer-size, and fenced-handoff APIs are sufficient for this
backend. Browser PR #14 needs cmux TUI PR #8476, but it does not need another
broad custom-frontend abstraction PR.

## Implementation checklist

- [x] Create browser PR #7 as a new worktree/branch stacked on PR #4.
- [x] Add canonical workspace UUIDs, ordered revisions, empty workspaces,
  bidirectional GUI/TUI create/rename/move/close, and terminal creation by
  workspace key in cmux PR #8340.
- [x] Make attachment sockets own their per-surface viewer-size claims and
  release those claims on disconnect.
- [x] Combine the manaflow Ghostty manual-mirror and external-Metal ABIs, pin
  the exact commit in both browser and TUI, and stamp both revisions into the
  bundled helper.
- [x] Add the first one-utility-process-per-terminal renderer service, semantic
  key/mouse IPC, latest-frame IOSurface delivery, resize coalescing, and
  per-renderer crash restart.
- [x] Move the high-volume cmux attachment socket into the renderer utility;
  Browser brokers a fresh one-use connected handle and leaves terminal data on
  the direct host/renderer stream.
- [x] Add the durable TUI-owned SQLite workspace/terminal journal, contiguous
  snapshots/events, mutation IDs, payload fingerprints, and retry idempotency.
- [x] Build and deploy the prior-pin Chromium app; dogfood exact helper identity,
  bidirectional workspace lifecycle, attachment resize minima/release, one
  utility process per realized terminal, and one-renderer crash recovery.
- [x] Build runtime themes, shell integration, and terminfo from the exact
  pinned Manaflow Ghostty fork; verify and install the complete tree before
  signing instead of borrowing resources from another Ghostty installation.
- [x] Negotiate lightweight viewer-size acknowledgments and order the
  compatibility-to-direct input cutover across both Mojo and terminal-host
  streams.
- [x] Finish prior-pin Computer Use visual dogfood for all 256 colors, mouse
  press/release/drag/wheel, BackTab, and continuous live-resize frame behavior
  through the direct browser renderer and independent signed attached-TUI host.
- [x] Measure daemon/terminal-host latency and CPU/memory at 1/10/50/100
  terminals with the exact pinned helper.
- [x] Measure the 1/10/50/100 all-warm Browser baseline and implement the
  initially-cold/30-second-grace renderer policy it motivates.
- [x] Complete then-current-pin before/after-eviction footprint, cold-wake, and
  sidebar interaction measurement.
- [x] Synchronize the cmux review stack onto canonical main
  `4daa93725694d51feae628426e878ccb73eabe6f` and advance the integration pin to
  `cbac0eb05454ed02a50a24675c62dd7265f17fef` while retaining Ghostty
  `d6f611a3077aa12510761ca10e2a5e0a93979536`.
- [x] Fix physical printable-key delivery in browser commit `255a9ae` and add
  a deploy-time compiled-pin guard for the Chromium Framework/runtime image.
- [x] Build and sign the then-current synchronized runtime/helper pair, then
  record historical direct and attached input dogfood with physical key events
  rather than scripted `type_text`, plus all-color, mouse, pointer/cursor, and
  resize verification.
- [x] Complete the intermediate synchronization of the cmux TUI stack onto
  canonical cmux main
  `b5631833741d0a1a2c25ede49988eadf2f412dd2` and advance the Browser pin to
  frontend-parity top `768c8d3f99a3b04294a6305a555cc48af638e7a1`.
- [x] Synchronize Browser PRs #4, #7, #13, and #14 bottom-up onto Browser main
  `5bd08a31ad239b9a9cb8c9b794354b06c1e28bb1`, preserving canonical-registry
  startup and durable-host teardown while incorporating the updater service.
- [x] Add display-cadence frame admission, atomic Ghostty wakeup coalescing,
  renderer-worker output batching, unchanged-PWD deduplication, bounded
  socket-ingress coalescing, BGRA IOSurface validation, and deterministic
  burst/lifecycle model coverage.
- [x] Add an XCUITest rapid-htop regression and a modal-rejecting exact
  Display-P3 palette comparator; all 256 Browser/reference swatches match.
- [x] Resolve Ghostty `config-file` includes before finalization in every
  Browser renderer/theme loader, matching standalone file semantics while
  intentionally excluding Chromium CLI arguments.
- [x] Differentially validate the cmux stack against canonical main, clear all
  stack-only Clippy/smoke failures, restore live selection bounds, and restage
  the exact 585-resource artifact at `768c8d3`.
- [x] Extend the signed-app XCUITest gate with physical printable-key delivery
  after click focus and end-to-end SGR mouse press/release delivery into the
  TUI-owned PTY.
- [x] Propagate canonical cmux main
  `49a43bfa0fd1c33ec458a0bb4ecfbd7a6619e16e` bottom-up through all seven
  review branches, publish top `ab5028a2c352117908b8cd5af6935916477e5c27`,
  and stage its exact 585-resource Ghostty runtime plus standalone resolver.
- [ ] Complete the final in-place signed Browser rebuild and repeat the htop,
  palette, typing, live-resize, and Computer Use dogfood gates on that exact
  bundle without installing a second Browser app.

## Recommendation

1. Make cmux TUI the canonical shared workspace registry for workspace UUID,
   name, ordering, creation, and close. Both GUI and TUI mutations enter one
   sequenced server mutation log and are broadcast to every frontend.
2. Keep the browser authoritative for richer Chrome-only state within each
   workspace: nested folder presentation, niri columns, split layout, mixed
   web/terminal tabs, web navigation state, color, and expansion.
3. Keep cmux TUI authoritative for terminal runtime state: PTYs, child
   processes, VT state, scrollback, placement, and live surface IDs.
4. Register every GUI workspace eagerly in cmux TUI, including workspaces with
   no terminals. A TUI-created workspace must appear in the running GUI and be
   persisted for the next GUI launch.
5. Run each active/warm Ghostty terminal renderer in its own Chrome utility
   process. cmux TUI remains a separate headless process and never renders GPU
   frames. Hidden terminals start cold; a renderer that was visible stays warm
   for 30 seconds, then exits after an ordered input/viewer drain.
6. Use a Chrome/Mojo-first renderer service. The native Swift cmux frontend is
   expected to be deprecated when this integration is ready, so an XPC/shared
   Swift transport is not a launch requirement.
7. Build cmux work on PR #8302, but extend its connection-keyed sizing into
   per-surface viewer leases before using it from cmux-browser.

The existing protocol is sufficient for the already-built byte-mirror terminal
backend. It is not sufficient for durable workspace association or correct
per-terminal smallest-viewer sizing with cmux-browser's multi-socket topology.
A focused cmux TUI protocol PR is required. PR #8023 becomes relevant only if
the requested bidirectional behavior includes exact pane/split mutations, not
merely workspace identity/name/order/lifecycle.

## Current architecture

```text
cmux-browser process
  CmuxWindowView
    WindowModel (projection of canonical workspaces + Chrome-only layout)
    CmuxTuiClient (canonical registry/control client)
    CmuxTerminalBackend (one per terminal tab)
      control socket: create/close/mutations and compatibility input
      mints one-use renderer capabilities
    CmuxGhosttyTerminalView (one per realized terminal view)
      semantic key/mouse/geometry Mojo
      composites the latest leased IOSurface

one Chrome utility process per realized terminal
  one libghostty app/manual-mirror surface
  direct capability-limited terminal-host socket
  VT replay + colors -> Ghostty parser/Metal renderer
  completed Display-P3 IOSurface -> browser compositor

cmux-tui daemon process
  canonical SQLite workspace/terminal registry and mutation log
  one durable terminal-host process per terminal
    PTY + child process + authoritative Ghostty VT state
    owner/admin stream + one-use renderer streams
```

The old architecture kept Ghostty parsing/rendering in the browser process and
targeted whichever TUI pane happened to be active. That is now historical.
Terminal creation uses a stable terminal UUID and canonical workspace UUID;
the browser view is a disposable projection of the cmux TUI registry. The
browser still owns richer Chrome-only pane/tab/web presentation keyed by those
canonical UUIDs.

Frame admission now follows the physical presentation clock. The renderer
utility allows one delivered frame plus one newest pending completion; Browser
installs an accepted IOSurface immediately but does not acknowledge that
delivery until the next NSView display-link tick. All Ghostty completions in
between therefore collapse in the renderer process instead of producing one
Browser main-thread Mojo/IOSurface/Core Animation transaction each. Lease
ownership remains independent: an invalid frame token is released immediately,
while a displayed token is retained until the replacement layer transaction
completes. Ghostty wake callbacks use an atomic scheduled/requested handoff so
a burst posts at most one tick task plus one successor for wakeups racing the
active tick.

The terminal data path is bounded before it reaches presentation as well.
Contiguous renderer-worker output is parsed in batches, stable OSC-7/PWD state
is not republished, and socket callbacks feed one size-bounded, barrier-aware
ingress FIFO drained in bounded time slices. Snapshot, ready, color, PWD,
resize, exit, and close boundaries remain ordered while adjacent output can
collapse into one parser operation.

## State ownership

| State | Authoritative owner | Durable key/store | Other side's copy |
|---|---|---|---|
| Shared workspace UUID/name/order/lifecycle | cmux TUI | durable session journal + snapshot | browser frontend model |
| Browser folders/niri columns/splits/mixed tabs | browser frontend schema | opaque `frontend_projections` row in `workspace-registry.sqlite3`, keyed by frontend/scope | cmux TUI stores and CASes the payload but does not interpret it as lifecycle authority |
| PTY/process/VT/scrollback | cmux TUI | live daemon/session | no browser copy |
| canonical terminal -> workspace placement | cmux TUI | terminal row in `workspace-registry.sqlite3` | browser terminal projection |
| terminal -> GUI pane/tab presentation | browser frontend schema | frontend projection keyed by stable workspace/terminal UUIDs | opaque payload stored by cmux TUI |
| cmux numeric IDs | cmux TUI | ephemeral only | reconnect cache, never durable |
| Ghostty rendered frame/parser mirror | renderer helper | ephemeral only | latest frame in compositor |

The stable UUID is duplicated intentionally as an identity/join key. The cmux
TUI registry owns shared workspace/terminal lifecycle and is also the durable
container for an opaque, schema-versioned Chrome layout projection. Chromium
authors that projection, but cannot use it to independently create, rename,
reorder, delete, or place canonical lifecycle objects.

Active selection is intentionally asymmetric today. An attached TUI selection
updates owner-mux focus; Chromium applies each distinct, fresh non-provisional
owner selection once. A later unrelated registry refresh cannot snap the GUI
back after the user selects a local web tab. GUI-only selection remains in its
frontend projection and is not written into owner-mux focus under stable IDs.
Bidirectional global selection would require explicit stable workspace/terminal
selection commands plus a selection revision/origin; lifecycle correctness does
not depend on that follow-up.

## Shared-workspace protocol target

The list below records the durability/idempotency contract implemented by the
stacked workspace-registry, canonical-terminal, and frontend-projection PRs.

Add a focused protocol extension with these semantics (exact command names are
provisional):

1. Persisted stable workspace UUIDs
   - cmux TUI allocates the canonical UUID and journals workspace create,
     rename, move, and close independently of runtime numeric IDs
   - the record includes its browser native-window/group UUID if multiple GUI
     windows share one cmux session
2. Empty workspaces
   - add a workspace without implicitly spawning a PTY
   - allow zero screens/panes/surfaces and render a stable empty state
   - current `new-workspace` always creates a screen, pane, terminal surface,
     so this requires a core model change rather than a frontend-only command
3. `create-workspace`, `rename-workspace`, `move-workspace`, `close-workspace`
   - accept `mutation_id` and optional `expected_revision`
   - return canonical UUID, runtime IDs when present, and committed revision
   - GUI commands and local TUI actions use the same mutation implementation
4. Targeted terminal creation
   - target by stable workspace/pane key, never active selection
   - accept stable terminal-tab UUID
   - return workspace/screen/pane/surface IDs in one response
5. complete ordered workspace deltas
   - add `workspace-moved`; merged PR #8031 currently provides add/close/rename
     but `move_workspace` only emits a coarse tree change
   - include canonical UUID, origin, mutation ID, and committed revision
6. monotonic server `tree_revision` and command `expected_revision`
   - mismatches return the current revision/snapshot instead of silently
     applying against stale placement
7. origin echo
   - events include `origin_mutation_id`, so the browser can acknowledge its
     own mutation without replaying it
8. durable session registry
   - append mutations to a cmux TUI-owned journal and periodically write an
     atomic snapshot
   - restore workspace records even when the previous daemon and PTYs are gone
   - browser layout state is joined by UUID after server restore

Protocol-v7 tree deltas from merged PR #8031 are a useful base, but they do not
provide stable UUIDs, move deltas, mutation origin, idempotency, durable
workspace records, or compare-and-swap revision semantics by themselves.

### GUI creation flow

1. The browser sends `create-workspace(mutation_id, window_group, name)` without
   committing an independent durable workspace record.
2. cmux TUI serializes and journals the mutation, assigns the UUID/revision,
   and publishes an ordered workspace-added delta.
3. The browser creates the GUI `WindowModel` entry and default Chrome-only
   layout keyed by the committed UUID. It may show a pending placeholder while
   the local socket round trip is in flight.
4. The new empty workspace appears in TUI immediately without spawning a PTY.
5. A terminal is later created directly in that workspace by stable UUID.
6. Browser layout persistence stores only the Chrome payload keyed by the
   canonical UUID.

A TUI-created workspace takes the same server path. A running browser consumes
the delta immediately; if the browser is absent, the journal retains it and the
next browser launch creates a default Chrome layout for that UUID.

### Reconnect and race rules

- Serialize all shared workspace mutations in cmux TUI. Browser UI sequences
  enqueue commands but never independently establish the committed order.
- Every command has a unique mutation ID and the server assigns the monotonic
  committed revision. Retrying after a lost response is safe.
- Every server instance has a generation and tree revision. Never reuse cached
  cmux numeric IDs across a generation change.
- On reconnect, fetch the cmux TUI workspace snapshot after a revision and
  reconcile the browser model by UUID. Do not infer identity from names or
  positions.
- A browser layout payload whose UUID no longer exists in the server registry
  is a tombstoned local orphan and may be garbage-collected after a grace
  period. It must not recreate the workspace by itself.
- Explicit GUI close may terminate its terminal surfaces after the browser
  commits the close. Browser crash, view destruction, renderer crash, socket
  loss, and temporary workspace hiding must not terminate PTYs.
- A TUI-created workspace is part of the shared registry and appears in the
  running GUI immediately. Behavior when no browser is running is a round-2
  confirmation, with journal-and-create-on-next-launch recommended.

## Smallest-viewer resize base

PR #8302 is the correct semantic base:

- the PTY grid is the component-wise minimum across participating viewers;
- resize reports/removals are serialized;
- attach recovery republishes the unchanged viewport;
- a disconnected viewer stops constraining the surface;
- client sizing participation can be toggled.

It cannot be consumed unchanged by cmux-browser:

- the browser sends `resize-surface` on its control connection;
- each terminal's `attach-surface` stream uses a separate connection/client;
- PR #8302 keys size claims by connection client ID;
- a resize from the unattached control connection takes the direct fallback
  and is not the attachment's smallest-viewer claim;
- `set-client-sizing` is client-global, while one browser control client owns
  many surfaces that can be independently visible or hidden.

Add an explicit per-surface viewer lease:

```text
attach-surface -> viewer_lease + capability
report-view-size(surface, viewer_lease, cols, rows, active)
detach/connection loss -> release that lease
```

Each realized renderer helper owns one scoped cmux TUI attachment connection,
including that connection's viewer lease. The browser passes only the socket,
surface capability, and desired pixel geometry when it launches the helper;
the helper reports its measured cell grid on its own attachment. Suspending or
destroying the helper closes the connection and releases the lease atomically.

PR #8340 implements this without a second capability namespace: optional
`cols`/`rows` on `attach-surface` register the attachment connection as the
viewer before its initial snapshot, subsequent `resize-surface` commands use
that same socket, and disconnect releases its contribution. This composes with
PR #8302's component-wise minimum and avoids an old-grid frame during attach.

## Proposed renderer-process architecture

```text
cmux-tui daemon
  shared workspace registry + PTY + authoritative VT state
          |                         ^
          | scoped attachment      | input + viewer lease
          v                         |
one TerminalRendererProcess per active/warm terminal
  direct capability-limited cmux attachment
  one libghostty app/manual-mirror surface
  input encoding + parser + fonts/glyph atlas + Metal render thread
          |
          | IOSurface Mach right + frame token/metadata
          v
cmux-browser UI process
  WindowModel + ordered workspace delta consumer
  semantic input IPC + process supervision
  SharedImage -> TransferableResource -> ui::Layer compositor
```

This follows the useful part of Chrome's architecture: expensive content work
and frame production do not run in the browser UI process, and the UI composites
the latest shared GPU frame. The product decision is stronger than Chrome's
usual process reuse: each active/warm terminal gets its own OS process and crash
domain. We accept duplicated Ghostty apps, font caches, glyph atlases, Metal
objects, and IPC endpoints in exchange for resilience.

### Feasibility evidence

- Ghostty's macOS Metal renderer already renders each target into an
  IOSurface-backed MTLTexture and presents that IOSurface through a CALayer.
- cmux-browser already has a tested compositor path that wraps an IOSurface in
  a Chromium SharedImage and presents it with
  `ui::Layer::SetTransferableResource`.
- cmux TUI already owns authoritative VT replay, so a renderer helper can be
  discarded/restarted and rebuilt without losing the shell.

### Ghostty API delivered by PR #123

The original macOS ABI required an in-process `NSView` and could not cross the
process boundary. The stacked Manaflow Ghostty work now provides the external
Metal/manual-mirror host contract needed here:

- host-supplied pixel dimensions, scale, focus, and visibility;
- a frame-ready callback carrying an exportable IOSurface handle/token,
  dimensions, color space, frame sequence, and optional damage;
- explicit frame release/backpressure so Ghostty's triple buffer is not reused
  while Chromium still displays it;
- completion synchronization (publish after command-buffer completion first;
  optimize with a shared Metal event only if measurement requires it);
- cell/grid size query after font metrics settle;
- no `NSView`, CALayer, WindowServer callback, or CPU readback requirement.

Preserve color metadata end to end. Ghostty's current Metal target creates a
Display-P3 IOSurface, while the browser compositor test currently imports its
test IOSurface as sRGB. The production frame contract must declare and import
the same color space or the earlier color-fidelity work will regress.

The pinned candidate exposes `GHOSTTY_PLATFORM_METAL_EXTERNAL` (ABI tag 4). It
calls the host only after Metal command-buffer completion with a borrowed
Display-P3 IOSurface and exact pixel dimensions. The renderer utility creates a
Mach send right during that callback; the browser assigns the reconstructed
IOSurface directly to a CALayer while preserving its color-space attachment.
Browser IPC is one-frame-in-flight/latest-frame-pending; a future Ghostty ABI
revision should additionally make host release constrain reuse of Ghostty's
own triple buffer.

### Pinned Ghostty runtime resources

`GhosttyKit.xcframework` does not contain the named themes, shell integration,
or terminfo needed at runtime. The browser build therefore runs Ghostty's
resource-only build at the same pinned commit and stages:

```text
ghostty/   # themes and shell-integration
terminfo/  # compiled ghostty/xterm-ghostty entries and source forms
```

The helper artifact carries both source revisions and an exact SHA-256 manifest
covering every regular file. Installation rejects missing, modified, unlisted,
symlinked, or special entries, deletes stale files within the two managed
trees, and preserves unrelated application resources. On macOS the containing
cmux bundle is authoritative: an incomplete bundle fails closed instead of
falling back to `/Applications/Ghostty.app`. Non-macOS builds resolve resources
relative to their executable.

### Renderer IPC surface

- lifecycle: create/destroy/suspend/resume surface
- output: authoritative replay generation, ordered byte chunks, sparse palette
  changes
- geometry: pixel size, backing scale, visibility, focus; returned cell grid
- input: physical key, action/repeat, text and consumed modifiers; mouse button,
  position, click count, drag, precise scroll/momentum; committed text/paste
- host services: clipboard read/write/confirmation, open URL, notification,
  title/bell if needed
- frame: IOSurface handle, frame sequence, size, color space, completion token,
  release acknowledgment
- recovery: helper generation and per-surface replay generation; stale frames
  and callbacks are dropped

To actually decouple terminal work from the UI process, the renderer helper
should consume its cmux attachment directly rather than bounce high-volume
terminal output through the browser. The browser asks cmux TUI for a
capability-scoped attachment endpoint/token restricted to one surface and one
viewer lease, then transfers that endpoint to the helper over Mojo. Workspace
control and process supervision remain in the browser. A compromised/crashed
helper must not enumerate, close, or mutate unrelated surfaces/workspaces.

### Hidden terminals and failure behavior

- Product direction is one process per active/warm terminal. A never-visible
  hidden terminal starts renderer-cold while its durable GUI observer and cmux
  terminal host register immediately. A renderer that becomes hidden keeps a
  30-second warm grace, then drops its helper after an ordered semantic-input
  and `ReleaseViewer` drain. A 10-second watchdog replaces a wedged helper.
- Ready is an irrevocable drain commit, not another preflight. Browser retains
  any input/resize that arrived after Prepare and flushes it on wake. Current
  effective visibility, not a sticky historical flag, decides whether Ready
  remains cold or immediately creates the replacement renderer.
- cmux TUI continues owning the PTY and VT state. Showing the terminal creates
  a helper surface and sends a fresh authoritative replay. The Browser keeps a
  durable UI observer while the renderer stream is suspended, so title and
  exit changes still update the GUI immediately.
- Keys, text, paste, preedit, and mouse press/release/move/scroll are bounded
  and deferred while evicting/cold/waking. They flush in physical order only
  after the authoritative replay frame is presented. Durable release
  tombstones stay ahead of a later press.
- Cold retirement clears the hidden IOSurface rather than retaining one full
  viewport per tab; the exact resolved terminal background remains until the
  wake frame arrives.
- Latest-frame-wins: at most one pending frame notification per surface.
- A helper crash invalidates exactly one terminal renderer. Restart it and
  replay; the shell survives.
- The UI paints the last accepted native-size frame during warm restart/resize
  and a stable resolved background after cold retirement, never a stretched
  or reset intermediate frame.

## Current pre-final validation status (2026-07-22)

- Source integration now targets cmux TUI
  `ab5028a2c352117908b8cd5af6935916477e5c27`, based on canonical cmux main
  `49a43bfa0fd1c33ec458a0bb4ecfbd7a6619e16e`, and Manaflow Ghostty
  `d6f611a3077aa12510761ca10e2a5e0a93979536`. Browser main is
  `5bd08a31ad239b9a9cb8c9b794354b06c1e28bb1`; the latest local #14 product
  commit is `a234c9ff383c66ca0994266ce6901d62f62f96e3`, and remote #14 is
  still `2b2317c` until the final push.
- Browser passes 26/26 host suites, 178/178 renderer-ordering checks, the same
  178/178 checks under ASan+UBSan, and all 87 TUI protocol checks. The focused
  reset-barrier, exact SGR mouse press/release, and physical printable-typing
  test is green.
- cmux passes all 800 tests in the locked serial workspace run, plus strict
  Clippy and the TUI/attach smoke gates. The final stage contains exactly 585
  manifest-verified resources and a standalone config resolver reporting the
  exact Ghostty fork revision above.
- Fast scrolling is bounded by display-cadence admission, worker-output
  batching, PWD deduplication, and socket-ingress coalescing. On the stale
  installed app, the rapid-scroll XCUITest captured 8/8 expected intermediate
  frames over 14.780 seconds; that validates the trace but not the unbuilt
  current source.
- The new live-resize trace exercises alternating macro resizes and retained
  frames across sub-cell changes. It still awaits the rebuilt app, together
  with final palette/color-role verification and Computer Use inspection of
  htop, mouse, physical typing, cursor, and direct/attached resizing.
- Installed-bundle evidence associated with helper
  `cbac0eb05454ed02a50a24675c62dd7265f17fef`, intermediate stack top
  `768c8d3f99a3b04294a6305a555cc48af638e7a1`, or canonical main
  `b5631833741d0a1a2c25ede49988eadf2f412dd2` is historical only. It must not
  be read as validation of the current `ab5028a2` integration candidate or
  Browser product commit `a234c9ff`.
- Final completion requires rebuilding and deep-signing the existing canonical
  app at `/Users/cmux-lawrence/Applications/cmux-browser-physical-typing-v2.app`,
  checking its packaged pin/resource receipts, rerunning the focused and
  live-resize XCUITests, and dogfooding Browser plus attached TUI through
  Computer Use. The workflow must overwrite that app and must not install a
  second Browser app.

## Historical dogfood evidence (2026-07-17)

- Built and deployed `cmux ▸ workspace-renderers` with cmux TUI
  `b3cae767aab16ead130af39f8f9cbd232ced591e` and Ghostty
  `d0dc34b2aaff370491a1dbc557177755ea974400`; strict codesign and the browser's
  protocol identity check both passed.
- Attached the deployed helper from Ghostty with
  `cmux-tui attach --session workspace-renderers-20260717`; the TUI and browser
  control/attachment clients remained connected to the same daemon.
- Browser startup registered Home, Work, Docs, Code, Scratch, and additional
  empty GUI workspaces in the TUI registry. Creating `TUI-Dogfood` from TUI
  increased Chrome renderer-backed GUI workspaces from 14 to 15; rename was
  observed in the registry and close returned the count to 14.
- With the browser attachment reporting 117x60, a temporary attachment at
  80x50 produced the component-wise minimum 80x50. Disconnecting that socket
  removed only its lease and restored 117x60.
- Killing terminal renderer utility PID 55515 launched replacement PID 59835
  in roughly 0.6 seconds. The cmux daemon PID, surface ID, PTY, and an exported
  shell sentinel all survived; replay rebuilt the renderer without a shell
  restart.
- One realized browser terminal produced exactly one
  `cmux.mojom.CmuxTerminalRenderer` utility process.
- The exact pinned cmux TUI candidate passed all 241 binary unit tests on the
  M4 builder. Its mouse coverage includes pane/control hit-testing plus PTY
  press/release, drag ownership, horizontal/vertical wheel events, Shift
  selection override, focus-loss cleanup, and ordered release recovery.
- A standalone check against the exact vendored Ghostty archive used the same
  external-Metal/manual-mirror ABI as the renderer utility and emitted SGR
  left press, matching release, and wheel sequences (`ESC[<0;13;3M`,
  `ESC[<0;13;3m`, and `ESC[<65;13;3M`) through `io_write_cb`. The full
  Chromium build validates the semantic mouse Mojo transport around that ABI.
- At this historical checkpoint, pixel-level 256-color comparison, real UI
  mouse clicks/scroll, and visual continuous-resize inspection remained
  pending the Computer Use runtime; the
  source-level color metadata, semantic mouse IPC, frame-size filtering,
  disabled implicit contents animations, and 16 ms resize coalescing are
  implemented and build-tested. After dogfood showed visible zooming, retained
  frames no longer stretch: they remain native-sized at the top-left while the
  exact resolved Ghostty/OSC-11 background fills newly exposed space until an
  exact-size frame arrives.

## Historical signed input-v3 dogfood evidence (2026-07-19)

- The direct app is
  `/Users/cmux-lawrence/Applications/cmux-browser-main-sync-input-v3.app` and
  the independent attached host is
  `/Users/cmux-lawrence/Applications/cmux-browser-msync3host.app`. Both pass
  deep strict code-sign verification.
- Both apps install cmux TUI
  `b3e87d44c25d3cfadd8e136df38c6c55f1a79a05`, Ghostty
  `d6f611a3077aa12510761ca10e2a5e0a93979536`, and signed helper SHA-256
  `eb6ef4c56cddb24e51d7ddcbd4a1d724ac6cbfe8c9d9b0e8c212cc11ff779d4d`.
  The staged GhosttyKit archive SHA-256 is
  `a768d7d2a5b73c22bbbca91e4c71c02a6e5100e340c5479115a47c514cfee05b`.
- Computer Use's scripted text injector produced exact ASCII markers in both
  the direct and attached frontends; the run also covered Backspace, Ctrl-C,
  dynamic color override/reset, theme cursor restoration, mouse
  press/release/drag/right click, continuous shrink/grow, and post-crash input.
  Because printable markers used `type_text`, this run did not validate
  physical `NSEvent` printable-key delivery. Protocol tests retain non-ASCII
  coverage.
- The hosted daemon mirror no longer generates terminal responses. The
  durable terminal host is the one Kitty/DA/DSR response authority; this
  removed the duplicate response bytes previously visible at a fresh attached
  shell prompt.
- The installed and bundled `Monokai Classic` theme files are byte-identical.
  After dynamic reset and probe exit, both paths resolved foreground
  `#fdfff1`, background `#272822`, cursor `#c0c1b5`, bar shape, and blinking.
- The canonical grid converged through
  `150x57 -> 132x57 -> 101x48 -> 146x48` without a stretched final frame.
  Renderer PID 49553 was replaced by PID 54519 while Browser PID 49476,
  daemon PID 49511, terminal-host PID 49521, contents, state, and attachment
  survived.
- Recovery may replay an idempotent release-only mouse tombstone so an
  uncertain downstream flush cannot leave a button stuck. Tokenized host-side
  input acknowledgement/deduplication is the exact-once follow-up.
- Automated gates are green: 311/311 cmux core tests, 301/301 TUI binary tests,
  the separate 20/20 terminal-host recovery rerun, 20/20 Browser host suites,
  Ghostty `test-lib-vt`, pin/resource audits, formatting, and diff checks.

## Prior signed v4 dogfood evidence (2026-07-19)

- The prior signed main app is
  `/Users/cmux-lawrence/Applications/cmux-browser-frontend-scaling-v4.app`
  (bundle `com.cmuxterm.app.dogfood.build-frontend-scaling-v4`, profile
  `cmux-browser-dogfood-frontend-scaling-v4`). The independent signed attached
  host is `/Users/cmux-lawrence/Applications/cmux-browser-v4host.app` (short
  bundle `com.cmuxterm.app.dogfood.v4host`, profile
  `cmux-browser-dogfood-v4host`). Deep strict code-sign verification passes for
  both bundles.
- Both apps contain cmux TUI
  `541d4dd908c9a1265b6abe038c959e3f271de33e`, Ghostty
  `8c645641a1dd1cbff9aa73189629682c01b1a233`, and the installed signed helper
  SHA-256
  `3f3ffdc2e200165ed61a7270d2b4b74d0fece24a8a1d098a1d99744514e05db9`.
- The shared run used registry
  `1e3a8a8a-b6ea-46b4-bff4-cb15413b88a8`, generation
  `175578a5-535a-48be-aa7d-b2851c896614`, session
  `cmux-browser-e1b0b468b090a95d7fef3826ab18a43d`, and socket
  `/tmp/cmux-tui-501/cmux-browser-e1b0b468b090a95d7fef3826ab18a43d.sock`.
  The companion attached with the explicit `--session` and `--socket`; a live
  `list-clients` then showed both `native-browser` and `tui` clients on the
  same daemon rather than a second implicit session.
- Workspace lifecycle was bidirectional in the live apps. A GUI-created empty
  workspace appeared in the TUI without implicitly creating a terminal. A
  workspace created in the attached TUI appeared immediately in the running
  GUI, and attached-TUI selection immediately updated GUI selection.
- Computer Use compared corresponding cells from the direct and attached
  256-color probes. All 256 cells were within 9 in every RGB channel. Both
  frontends delivered click, release, drag, and wheel events. The attached-TUI
  right click was consumed and opened its pane context menu; an unconsumed
  direct Chrome right click opened the native Copy/Paste fallback.
- Viewer widths `150 -> 134 -> 117 -> 150` were exercised in sequence. Each
  settled frame was crisp, retained frames stayed native-sized, and there was
  no stretched transition or persistent reset/background flash.
- Killing renderer utility PID 32274 launched replacement PID 35182 in under
  one second. Browser PID 26073, daemon PID 26106, terminal-host PID 26119,
  terminal `05386a44205145fcb1a2e66bf7beac8d`, surface 2, registry identity,
  shell content, and input from both the direct and attached frontends
  survived.

The prior signed-v4 performance run completed against the same session and
artifact:

| Checkpoint | Durable terminals/hosts | Renderer utilities | Physical footprint |
| --- | ---: | ---: | ---: |
| 1 terminal | 1 | 1 | 801,063,544 bytes |
| 10, initially cold | 10 | 1 | 824,989,104 bytes |
| 10, all warm | 10 | 10 | 1,381,351,056 bytes |
| 10, after eviction | 10 | 1 | 942,871,984 bytes |
| 50, cold-hidden | 50 | 1 | 1,064,201,392 bytes |
| 100, cold-hidden | 100 | 1 | 1,249,106,120 bytes |

The 100-terminal cold-hidden result is 4.08x smaller than the historical
5,101,539,184-byte all-warm result, a 75.5% reduction. It keeps 100 durable
terminal hosts and 100 children but only the visible terminal's renderer. A
Computer Use BackTab into an evicted `/bin/cat` terminal followed immediately
by queued input produced visible echo within a 610 ms action-to-fresh-state
upper bound. Sidebar round trips through an empty workspace remained responsive
at every checkpoint: the per-leg upper bounds were 675/714-937 ms at 1,
1,002/712 ms at 10, 785/715 ms at 50, and 721/946 ms at 100 terminals. These
include Computer Use capture/settling overhead and are deliberately reported
as upper bounds rather than raw event latency. The harness closed all 99 owned
terminals and its latency workspace, leaving only the original terminal-host
identity. Full checkpoint data is persisted in
`/Users/cmux-lawrence/fun/cmux-browser-artifacts/cmux-workspace-idempotency/frontend-scaling-v4/scaling-state.json`.

## Prior durable-stack build and test evidence (2026-07-19)

- At that checkpoint, the cmux TUI code head passed 645 tests across 21 result
  blocks with zero failures; formatting and diff checks also pass. The final
  pinned commit
  adds documentation only.
- Browser's 20 host suites passed, including renderer ordering, recovery,
  direct host connection/protocol, canonical placement, TUI protocol, and
  workspace-projection coverage. The renderer-ordering model has 127 checks and
  the strict input-cutover model has 19 checks. TUI protocol coverage has 82
  checks, including every detailed workspace/screen/pane/tab delta name so a
  `tab-added` refresh cannot leave GUI selection behind terminal creation.
- The exact staged helper passed pin verification and a two-terminal smoke
  across two daemon generations. Registry identity stayed stable, 12 terminal
  events were contiguous, and competing resize leases settled from 70x20 to
  100x40 after the smaller viewer detached.
- The prior shared M4 app rebuild completed 39 steps in 53.84 seconds. Its
  bundled helper and receipts identify cmux TUI
  `8e4965dd2b0bb671daafe305b102768f264294ef` and Ghostty
  `8c645641a1dd1cbff9aa73189629682c01b1a233`; deep strict code-sign
  verification passed. This is inherited base evidence, not evidence for the
  new short-socket cmux pin.
- That checkpoint pinned cmux TUI
  `541d4dd908c9a1265b6abe038c959e3f271de33e`. PR #8452 passed its unit,
  integration, recovery, protocol-golden, all-target Clippy, and formatting
  gates. PR #8458 proves that capability minting fences fragmented owner/admin
  `Input` frames before a renderer's direct input; its focused regression,
  16/16 recovery tests, full workspace, all-target Clippy, formatting, and diff
  checks pass. PR #8460 decouples the short runtime socket from the durable
  state root; its focused regression, full CLI integration suite, full
  workspace tests, all-target Clippy, formatting, and diff checks pass. PR
  #8475 releases the daemon Admin viewer when no GUI/TUI viewer remains; its
  red/green recovery regression passed 20 stress repetitions, all 17 recovery
  tests, and the full 267-test core/integration/doc suite. PR #8476 forwards
  source terminal colors and attached-client selection, fixes BackTab decoding,
  adds local PID/generation-fenced daemon handoff, requires canonical lowercase
  workspace UUIDs, and preserves armed workspace/tab mouse releases across a
  concurrent routing refresh. Its rendered-path tests also prove orphan mouse
  release is a no-op.
- Before the signed-v4 packaging above, the freshly staged helper
  (`1e1fcb11a7461db8283c9ab429e277374061c1a3029e33d7579b06ef43c87c49`)
  and 585-file Ghostty runtime tree match the pinned cmux and Ghostty revisions.
  Artifact tests cover a good install, stale-file
  deletion, paths with spaces, unrelated-resource preservation, missing or
  tampered resources, unlisted files, symlinks, and invalid destinations. The
  prior signed main and attached-TUI-host apps passed deep strict code-sign
  verification and contained the older identical helper SHA-256
  `b4d42913467bfde12100474f896948af4ca231d92bb699910c972f925a8fc233`.
  These are preserved as historical predecessor artifacts; the signed-v4
  bundle and helper identities above supersede them.
- Live upgrade dogfood rejected replacement with a second native-browser, then
  replaced the allowlisted legacy daemon without changing registry identity or
  any pre-existing terminal-host PID. Empty GUI workspaces appeared in TUI;
  TUI workspace/terminal creation and selection appeared immediately in GUI.
- An earlier Computer Use pass compared the direct and attached-TUI 256-cell
  probes with zero mismatches at JPEG tolerance 15, including sparse OSC 4
  index 1 `#ff3562`.
  Both paths delivered click, drag, release, and wheel events; BackTab selected
  the previous tab; repeated shrink/grow operations stayed crisp without a
  stretched retained frame.
- In an earlier recovery pass, killing selected renderer PID 29183 launched PID
  35430 while browser PID 28547, daemon PID 18862, terminal-host PID 29182,
  stable terminal identity, content, and attached-TUI rendering all survived.

### Input cutover and crash boundary

The compatibility-to-direct handoff is an ordered two-sided barrier. The
renderer first buffers new Ghostty input and emits a same-client-pipe marker,
the browser drains every older compatibility `Input`, and `MintCapability` on
the same host owner/admin stream fences that complete prefix. After applying
the authoritative snapshot and colors, the renderer queues its buffered prefix
first and only then commits subsequent input to the direct stream. Cancellation
republishes buffered bytes before its ordered marker. Paste uses Ghostty's
encoder and this same service-sequence FIFO, so it cannot overtake earlier key
or mouse input.

All compatibility output, direct state, lifecycle controls, and semantic input
also pass through one bounded renderer lane. Semantic encoders fence older
output-worker tasks. Exact-state transitions hold semantic actions while
restore and cancel/detach controls can make progress without treating stale
modes as authoritative. A direct `Exit` waits behind prior output and preserves
the final mirror before Browser detaches. The five-second attach watchdog first
requests this ordered cancellation; only a still-wedged cancellation after two
more seconds replaces the renderer utility while leaving the terminal host and
PTY alive.

Mouse capture survives renderer-incarnation loss. A release generated after
the crash is retained and replayed only after the replacement renderer has an
authoritative grid, while new presses remain gated until that point. Releases
use token-matched acknowledgments that never gate later release RPCs and retain
the newest accepted release per button as an idempotent crash-replay tombstone.
Every release therefore enters the ordered Mojo pipe before the next physical
press, even while older acknowledgments are outstanding.

This does not yet provide exactly-once input across a renderer/socket crash.
The direct host stream has no input sequence, acknowledgment, or deduplication;
bytes accepted by the old renderer but not flushed to the PTY can still be
lost. A forced watchdog replacement can also lose a semantic intent journaled
only in the wedged renderer. Full losslessness requires a Browser/host-visible
intent journal, `InputAck`, and reconnect deduplication, or a single host-owned
serialized ingress.

## Process granularity

Use one Chrome utility process per active/warm terminal. The process owns one
Ghostty app/surface and one capability-limited cmux attachment. This maximizes
fault containment: a parser, font, renderer, Metal, or IPC failure can blank at
most one terminal and cannot kill its PTY.

The cleaned pre-cold run quantified the cost of keeping every helper: at 100
terminals, 100 renderer utilities used 4,099,211,608 bytes of a
5,101,539,184-byte physical footprint, with 311 scoped processes. The selected
default is therefore an initially cold hidden renderer plus a 30-second warm
cache for a renderer the user actually saw. PTY/VT resilience remains in cmux
TUI; wake constructs a disposable helper from replay.

## Historical worktree and PR sequence

The completed stack was developed in this order; the bullets below retain the
execution plan for review provenance rather than describe current next steps.

1. **cmux resize base**
   - preserve the existing #8294 worktree for review;
   - create a fresh cmux worktree from PR #8302 head `6d69f89ed2`;
   - replay only #8294's still-needed Ghostty pin, build stamps, cursor replay,
     and sparse palette commits;
   - drop/replace #8294's last-writer-wins assumptions;
   - add per-surface viewer leases and tests for browser control/attach socket
     separation.
2. **cmux shared workspace registry**
   - persistent canonical UUIDs and empty workspace support;
   - one mutation sequencer/journal for both TUI and GUI commands;
   - complete add/move/rename/close deltas with revisions and mutation IDs;
   - targeted surface creation and concurrent GUI/TUI/reconnect tests.
3. **browser stable identity and reconciliation**
   - consume the canonical cmux workspace registry/delta stream;
   - persist Chrome-only layout payloads keyed by canonical UUID;
   - pending GUI mutations, create/move/close flows, and orphan recovery;
   - one resize lease per realized terminal.
4. **Ghostty offscreen Metal PR**
   - split `Metal` host target from the current NSView/CALayer presentation;
   - export IOSurface frames with release/synchronization;
   - keep native NSView behavior unchanged for existing hosts;
   - standalone cross-process frame harness before Chromium integration.
5. **browser renderer service PRs**
   - one helper per active/warm terminal, Mojo protocol, sandbox, and
     capability-scoped direct cmux attachment;
   - semantic input + clipboard bridge;
   - replace the native NSView with the existing SharedImage texture layer;
   - pool, suspension, crash replay, metrics, and dogfood.

The launch implementation is Chrome/Mojo-only. Native Swift cmux is expected to
be deprecated after this integration is ready, so a shared XPC transport would
add cost without serving the target architecture.

## Acceptance gates

- GUI- and TUI-created workspaces use one canonical UUID/revision stream and
  appear in the other frontend immediately; terminals never land in a merely
  active TUI pane.
- empty GUI workspaces appear in TUI without implicitly spawning a terminal.
- retries, lost responses, browser reconnect, and two browser windows do not
  duplicate workspaces or terminal surfaces.
- a TUI attach cannot silently diverge browser-owned topology.
- two viewers at 120x40 and 80x50 settle at 80x40; detach expands correctly;
  independently hiding one browser terminal releases only its lease.
- 100 hidden terminal tabs retain 100 durable hosts but converge to the visible
  renderer set after the 30-second grace; acceptance records before/after
  physical footprint and switch latency.
- renderer helper crash/restart preserves PTYs and reconstructs visible frames.
- rapid live resize is latest-size/latest-frame-wins, retains the last
  native-size warm frame, and has no stretch or reset flash; a fully cold tab
  uses the resolved background until its exact wake frame.
- all 256 colors, cursor styles, mouse press/release, scroll, IME/text, paste,
  focus reporting, and alternate-screen behavior match the in-process build.
- measure sidebar input latency, browser UI main-thread time, helper CPU/GPU,
  frame latency, and memory for 1/10/50/100 terminals to tune the cold-hidden
  renderer lifetime. One OS renderer process per active/warm terminal is
  already the chosen model.

## Grilling decisions

Round 1 resolved:

- TUI workspace mutations must immediately mutate the GUI.
- every GUI workspace, including empty ones, must appear in TUI.
- native Swift cmux will be deprecated; build the renderer service for Chrome.
- use one OS renderer process per terminal for maximum resilience.

Round 2 questions:

1. Does bidirectional mutation cover only workspace create/move/rename/close,
   or must TUI pane/tab/split mutations also reproduce the exact GUI layout?
2. A TUI-created workspace currently spawns a terminal, while the new shared
   registry needs empty workspaces. What should its initial GUI content be:
   empty shell, blank web tab, or no pane until the first explicit action?
3. When the browser is not running, may TUI workspace mutations persist and
   appear in the GUI on its next launch?
4. Resolved: a never-visible hidden terminal starts cold; a previously visible
   renderer exits after a 30-second hidden grace and reconstructs from cmux TUI
   replay.
5. How do multiple Chrome native windows and nested GUI workspace folders map
   into cmux TUI's currently flat workspace list?
