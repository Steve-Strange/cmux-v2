# cmux-browser review rules

## Contents

- Review standard
- Lifetime and ownership
- Sequences, callbacks, and shutdown
- Terminal protocol and renderer state
- Model, Views, input, and animation
- Chromium API and overlay integration
- Builder, patch, and deployment scripts
- Cross-platform, packaging, and security
- Performance and proof

## Review standard

- Report only defects introduced or exposed by the reviewed change. Do not
  report style preferences, speculative edge cases, or broad rewrites.
- Trace each candidate through its caller, owner, teardown path, and relevant
  tests. Inspect the pinned Chromium/Ghostty source or declarations when an API
  contract is material; do not rely on memory of another revision.
- Treat P0 as data loss, security compromise, persistent corruption, or a
  broadly unusable app; P1 as a crash, hang, UAF, incorrect security decision,
  false-green build, protocol corruption, or major workflow failure; P2 as a
  concrete scoped bug or missing proof likely to escape; omit P3/nits.
- Prefer the smallest fix at the actual ownership boundary. When one finding
  proves a repeated bug class, inspect sibling instances in the reviewed scope.
- A build or test result from a stale overlay, different Chromium checkout,
  different bundle, or older commit is not proof for the reviewed bytes.

## Lifetime and ownership

- Reject `base::Unretained(this)` when a callback can be posted, queued,
  repeated by another owner, or invoked during teardown. Prefer
  `base::WeakPtr`, and invalidate weak pointers before destroying objects that
  can re-enter the owner. Accept `Unretained` only with a local, verifiable
  synchronous or strict owner-before-callback lifetime.
- Audit callbacks that can delete their receiver or mutate the view/model that
  owns the current stack frame. Defer follow-up work through an owning weak
  pointer and re-fetch objects by stable ID after mutation.
- Detach observers, subscriptions, event monitors, widget delegates,
  `WebContents`, and native child views in the correct order. Child `View`
  destruction happens after the derived destructor body; clear child-held
  aliases before owned members die. Clear process globals on user-driven
  window close as well as programmatic shutdown.
- Use Chromium ownership types consistently: `raw_ptr<T>` for non-owning
  dereferenceable fields, smart pointers for ownership, scoped wrappers for CF
  and platform resources, and integer identity keys for pointers that are never
  dereferenced. Do not let a raw CF/Objective-C/resource handle cross a posted
  task without retaining it for the task.
- Make shutdown safe in every order. A callback-thread stop flag must be under
  the same lock as callback state, and destruction must prove the producer has
  stopped or joined before storage disappears.

## Sequences, callbacks, and shutdown

- Never perform filesystem, config, network, process, or other blocking work on
  Chromium's UI sequence. `ScopedAllowBlockingForTesting` in production UI code
  is a red flag, not a fix. Use `ThreadPool` with `MayBlock`, reply on the owning
  sequence through a weak pointer, and serialize related operations when order
  matters.
- Add `DCHECK_CURRENTLY_ON`/`SEQUENCE_CHECKER` at ownership boundaries, not as a
  substitute for correct marshaling. Verify the documented thread of Ghostty,
  Mojo, AppKit, compositor, and observer callbacks.
- Preserve operation order across asynchronous cutovers: input, resize,
  replay, frame publication, scale, selection, and reconnect events need an
  explicit generation/epoch or a single sequenced owner. Do not publish a new
  generation until all state that defines it is applied.
- Bound producer-to-UI queues. For frames or terminal output, coalesce to the
  newest state or apply explicit backpressure; a stalled UI thread must not
  create unbounded pixel buffers, tasks, or byte queues.
- Bound UI-to-daemon writes after the connection becomes ready too. Input and
  resize RPCs need at most one in-flight write plus a capped/coalesced pending
  queue and generation-fenced completion. A pre-connect byte cap does not bound
  ready-state Mojo/socket requests, preserve input order, or prevent late
  completions from mutating a replacement connection.
- Treat cancellation and expected rejection as distinct from transport or
  protocol failure. A denied paste, stale frame, duplicate release, or canceled
  UI action must not tear down an otherwise healthy session unless the protocol
  explicitly requires it.

## Terminal protocol and renderer state

- Fail closed at identity, version, rights, and capability boundaries. Unknown
  enum values, missing build/Ghostty stamps, wrong protocol versions, malformed
  frames, and arbitrary endpoints must never gain replacement or control
  authority by falling through a switch.
- Keep renderer/browser/TUI/Ghostty protocol declarations, Mojom callbacks,
  rights, version constants, build pins, and tests synchronized. A compatibility
  fallback must preserve legacy semantics rather than partly applying newer
  state.
- Treat snapshots, replay, colors, cursor state, geometry, scale, and displayed
  frame identity as coherent state. Sparse updates must explicitly define
  whether omission means preserve or reset. Gate input and sizing on the frame
  actually displayed, not merely the newest request.
- Make GUI workspace mutations idempotent and retry-safe. Define the lifetime of
  pending mutation state, retain invalidations until consumed, bind projection
  events to the correct registry, and seed state only when the registry is
  genuinely new.
- Preserve user text across AppKit/IME aliasing and composition. Test physical
  key events separately from injected committed text; the latter can bypass the
  broken path.

## Model, Views, input, and animation

- Preserve `WindowModel` invariants after every close, move, split, reorder,
  retry, and wildcard operation: valid unique IDs, a selected tab that exists,
  no empty live pane, correct same-pane index adjustment, and a valid last
  pane/workspace policy. Add pure host tests for boundary transitions.
- Any `views::View` subclass instantiated through Views must carry the required
  `METADATA_HEADER` and `BEGIN_METADATA`/`END_METADATA`. Nested helper view
  classes are especially easy to miss; the pinned builder may assert
  `kHasClassMetadata` in `AddChildView`.
- Match layer contracts: a layer marked opaque must paint every pixel of its
  bounds; translucent/partial painters must call
  `SetFillsBoundsOpaquely(false)`. Schedule the needed paint/layout after theme,
  focus, hover, bounds, and content changes.
- Keep geometry in the coordinate space and integer/subpixel representation the
  target Views API expects. Clamp animated label/view bounds where fractional
  interpolation can trip DCHECKs. Recompute hit targets after layout changes.
- Layout must not silently stop or overwrite active `BoundsAnimator` state.
  Animation proof must be event-driven from progress/completion callbacks;
  fixed-delay mid-flight samples race the same UI loop as the animator.
- During drag/capture, do not assume `OnMouseExited` fires. Model teardown,
  reparenting, and live reordering can invalidate indexes and pointers
  mid-drag; cancel synchronously or re-resolve stable IDs before continuing.
- Route renderer-owned shortcuts/input to the renderer before consuming global
  app shortcuts, except for deliberately reserved chords. Preserve printable
  text, IME, modifier, key-up/down, focus, accessibility, and platform parity.

## Chromium API and overlay integration

- Compile against the repository's pinned Chromium snapshot. Verify exact
  virtual signatures, constness, return types, explicit constructors,
  destructor virtuality, `final` constraints, and feature/build flags in that
  source. Similar APIs from another milestone are not evidence.
- Treat Chromium warnings and plugins as errors: `raw_ptr` field rules,
  `-Wshadow`, range-loop binding, missing `override`, incomplete definitions,
  and class metadata commonly fail only on the real builder.
- Check a Views subclass declaration together with its candidate sibling header
  and implementation. `METADATA_HEADER`, `BEGIN_METADATA`/`END_METADATA`, and
  construction are commonly split between `.h` and `.cc`/`.mm`; inspecting only
  the changed declaration file can miss a builder assertion.
- Register every added, renamed, moved, or retired overlay source in all owning
  places: local `BUILD.gn`, the patched Chromium `chrome/browser/BUILD.gn`
  source lists, platform blocks, service/Mojom targets, test runners, and demo
  targets. Include registered headers when renaming or retiring files. Check
  macOS, Linux, and Windows conditions independently. An entry
  in a demo/test-only GN target does not prove the shipped browser links the
  source; trace the production target or the platform patchers explicitly.
- Keep `.mm` and framework use macOS-only. Cross-platform source sets must not
  acquire AppKit or platform-only dependencies. Conversely, do not accidentally
  register a required cross-platform source only in Linux or one builder.
- Patch anchors must be unique, version-aware, idempotent, and fail closed when
  upstream changes. Re-running apply must not duplicate sources; refreshing a
  patched warm tree must add new sources and prune retired ones. Register each
  source in every applicable clean-construction list and warm-refresh list for
  macOS, Linux, and Windows; appearing elsewhere in the same script is not proof
  that both paths are current. In `apply.sh`, keep cross-platform files in both
  `xplat` and `xplat_missing`, macOS/Linux refreshes in `platform_sources`, and
  Windows refreshes in `win_source`/`win_sources`. In
  `apply_win_chrome.py`, pair `xplat` with `xplat_missing` (or `xplat_refresh`)
  and `win_block` with `win_missing`/`win_sources_missing`; add the warm branch
  when introducing the first source that needs it.

## Builder, patch, and deployment scripts

- Preserve the real command exit status. Never pipe `autoninja`, tests, `ssh`,
  or another gate through `grep`/`tail` in a way that can report success from
  the last formatter. Capture the remote status explicitly, print a clear
  success/failure marker, and exit with it. Status expressions embedded in an
  SSH command must be single-quoted or dollar-escaped from the local shell so
  the remote shell—not the caller—expands `PIPESTATUS`/`pipestatus`. Apply the
  same rule when a pipeline is split across backslash-continued lines. A local
  `pipefail` only propagates failure when `errexit` is also active (or status is
  explicitly captured immediately and returned without being overwritten).
  `PIPESTATUS`/`pipestatus` changes after every pipeline, so no unrelated command
  pipeline may intervene before it is captured or returned. For remote
  pipelines, select Bash or zsh explicitly and execute the matching immediate
  status exit as shell code, not quoted text or a comment. Locally, the status
  array must match the script shebang: `PIPESTATUS[0]` for Bash and
  `pipestatus[1]` for zsh; do not assume they are interchangeable.
- Remember the remote login shell can be zsh and macOS automation may still use
  Bash 3.2. Avoid zsh-reserved names such as `status`, unsupported Bash features,
  and quoting that changes after the local shell, SSH transport, and remote
  shell each parse it. Cover empty arguments and large streamed patches.
- `sync.sh` may use `--delete` only when that command's destination is itself a
  wholly cmux-owned directory. For the terminal overlay, the destination must
  end at `chrome/browser/cmux_term/` and the source must be that exact owned
  overlay; never delete from a mixed upstream parent. Validate every delete-
  enabled `rsync` independently—a separate safe command cannot bless a broad
  one. The separate `patches/` to `.cmux-patches/` mirror is also wholly owned
  and may delete within that exact pair. Force authoritative overlay bytes/
  timestamps when stale cross-branch objects could otherwise survive an APFS
  clone or warm builder.
- Build/apply workspaces must be isolated and repeatable. Clean only the scoped
  overlay/patch inputs the workflow owns; never reset unrelated user work.
  Invalid or missing anchors, pins, generated artifacts, or transport setup must
  stop the build rather than silently continue.
- Tagged dogfood apps need distinct bundle identity, product/profile directory,
  and launch state. Verify the app launched is the bundle just built, not an
  already-running process that owns the same profile or ProcessSingleton.
- Packaging mutations must happen before signing; sign nested code inside-out
  and verify the final bundle. Keep update archives, manifests, executable
  layout, entitlements, version, platform/architecture, and signing identity
  coherent.

## Cross-platform, packaging, and security

- Review macOS, Windows, and Linux behavior wherever a supposedly shared file,
  source list, protocol, resource lookup, or shell path changes. Require a
  platform-specific implementation or explicit unsupported path rather than a
  silent partial implementation.
- Do not weaken update signature, archive extraction, path containment, version,
  platform, architecture, redirect, or transport checks for local testing.
  Production and local feeds must exercise the same trust decision.
- Treat remote/apply inputs, paths, archive members, extension packages, URLs,
  and process endpoints as trust boundaries. Prevent shell injection, traversal,
  unsafe overwrite, credential disclosure, and replacement of unrelated
  processes. Validate before mutation and fail closed.
- Preserve browser privacy/branding controls across both platform patchers:
  unofficial builds must not enroll in Chrome field trials, send Chrome
  variation IDs, or claim a release channel they do not own.
- App icons, localized metadata, resources, entitlements, and updater helpers
  must be verified from the packaged and launched app. macOS can resolve an
  asset catalog instead of a standalone `.icns`; inspecting a source asset is
  insufficient.

## Performance and proof

- Treat per-frame, per-keystroke, per-row, layout, paint, and callback-thread
  paths as hot. Flag repeated allocation/formatting, full-buffer copies,
  per-surface tickers, unconditional redraw, or cache/queue growth without a
  demonstrated bound. Reuse frame resources and make redraw demand-driven.
- For app/runtime changes, require focused host/unit tests, the pinned Chromium
  build, deployment, and behavior-specific dogfood proof. Use the repository's
  sync -> apply -> build -> deploy workflow; source checks alone are not done.
- Match proof to the defect: lifecycle tests for teardown/UAF, event-driven
  tests for races, malformed/boundary protocol tests, repeated-run tests for
  idempotency, clean/warm builder tests for stale state, and packaged/live
  inspection for resource or identity changes.
- A test that bypasses the changed path is not proof. Examples include injected
  text instead of physical key events, a clean tree instead of a warm patched
  builder, or checking `app.icns` instead of the icon macOS resolves.
