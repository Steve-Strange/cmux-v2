# cmux TUI integration goal and TODO

Last updated: 2026-07-24

## Goal

Ship cmux Browser with cmux TUI as the canonical workspace/terminal backend,
the pinned Manaflow Ghostty fork rendering in one disposable OS process per
active/warm terminal, durable PTYs across browser/daemon/renderer crashes, and
matching keyboard, color, mouse, and resize behavior in the direct Chrome and
attached TUI frontends.

## Review stack

- Public cmux TUI/Browser source:
  [manaflow-ai/cmux#8717](https://github.com/manaflow-ai/cmux/pull/8717)
  merged candidate `ef52e63b50fd23a9c13b58d9a9194979e9df5c5e`
  as `ea51d55aa8d24303289f34038a051d9180383747`. Browser artifacts pin
  verified public-main cutoff `f6085bbb9259767f94ed4890b0eb8df609209936`;
  that revision contains the complete public Browser/TUI import and is a
  descendant of the merged candidate. Immediately before review, public main
  advanced to `4d9c2389f682536deac611b1c470bd0e2923bc7b` through four iOS-only
  commits; `cmux-tui/` and the Ghostty gitlink remain byte-identical.
  Superseded public stack PRs are closed.
- Manaflow Ghostty:
  [manaflow-ai/ghostty#128](https://github.com/manaflow-ai/ghostty/pull/128),
  merged at `c55514dd52d806e9aa661ee20381aa19c91c1c09`.
  Current public cmux advances that fork to
  `50ad1963d9c73ee957932ccb4d26bf6d15575ee7`, including the bounded
  renderer-drain, frame-lease, callback-lifetime, and dead-PTY cleanup fixes.
- Private cumulative Browser landing:
  [manaflow-ai/cmux-browser#29](https://github.com/manaflow-ai/cmux-browser/pull/29)
  merged to `main` as `9e87ed1aa21b284bf8d675ddfdc2ef40f537a85e`
  and supersedes stacked PRs #4, #7, #13, #14, and #17. It combines the TUI
  backend, durable workspace registry, one-host-per-terminal process model,
  process-isolated renderers, input/color/scroll/resize fixes, GPL source
  publication, and current Browser main. Follow-up landing record
  [manaflow-ai/cmux-browser#32](https://github.com/manaflow-ai/cmux-browser/pull/32)
  merged as `d1fbb4c9c4b846a6e9a316c1a42bef87984d3baa`; final verified receipt
  follow-up [manaflow-ai/cmux-browser#33](https://github.com/manaflow-ai/cmux-browser/pull/33)
  carries the f608/50ad build and interaction evidence.

## Checklist

- [x] Make cmux TUI the only writer for shared workspace and terminal lifecycle.
- [x] Project every empty or populated registry workspace into the running GUI.
- [x] Persist Chrome-only hierarchy/layout/color/selection as a versioned
  frontend projection without duplicating lifecycle authority.
- [x] Run one durable PTY-owning host per terminal and at most one disposable
  Ghostty renderer OS process per active/warm terminal.
- [x] Keep terminal bytes off the Chromium browser process and lease completed
  Ghostty IOSurfaces into the compositor.
- [x] Forward exact sparse terminal colors, attached-TUI selection, BackTab,
  semantic mouse actions, and viewer-size acknowledgements.
- [x] Add peer-verified, generation-fenced daemon handoff and one-release
  allowlisted legacy replacement while retaining terminal hosts.
- [x] Pass focused Chromium object compilation, 20 browser host suites
  (including 82 TUI-protocol checks), all 645 cmux TUI tests, Clippy,
  formatting, and the 100-terminal backend scaling baseline.
- [x] Complete the fresh signed Chromium build and deploy the isolated main and
  attached-TUI-host dogfood apps.
- [x] Prove fail-closed upgrade with a second browser owner, then prove legacy
  daemon replacement preserves every terminal-host PID and registry identity.
- [x] Dogfood immediate GUI/TUI workspace lifecycle and TUI-to-GUI selection.
- [x] Compare all 256 palette cells between direct and attached renderers and
  exercise BackTab, click, drag, release, wheel, and continuous resize through
  Computer Use.
- [x] Crash one terminal renderer and prove only that disposable process is
  replaced while its PTY/content survives.
- [x] Record the then-current signed bundle/helper identities and complete the
  visual interaction and renderer-recovery gates.
- [x] Record the cleaned 1/10/50/100 all-warm Browser baseline; at 100
  terminals, 100 renderer utilities account for 4,099,211,608 bytes of the
  5,101,539,184-byte physical footprint.
- [x] Implement initially-cold hidden terminals, a 30-second warm grace,
  ordered drain/ReleaseViewer eviction, bounded wake-input deferral, immediate
  durable UI notifications, a 10-second watchdog, and same-generation final-VT
  rehydration.
- [x] Complete the then-current signed-app scaling run and record renderer
  count/footprint before and after eviction plus cold-wake and sidebar latency.
- [x] Add the final scaling measurements, update both PR bodies, commit and
  push browser PR #14, and mark the performance/release gate complete.
- [x] Refresh the browser stack onto `f4fb3b18`, the complete cmux stack onto
  `acdbbcf6`, and the Ghostty stack onto `bb30526c`; preserve the cmux rewrite
  with an exact 39/39 range-diff.
- [x] Expose Ghostty's resolved cursor shape, blink, screen activity, and
  cursor activity; make Colors v2 authoritative while v1 preserves raw VT.
- [x] Propagate complete Ghostty defaults into durable hosts, normalize C1
  controls before fan-out, and force same-pair cursor transitions through
  local, hosted, browser, and attached-TUI paths.
- [x] Pass the final locked serial cmux workspace suite, 311/311 core tests,
  301/301 TUI binary tests, 20/20 recovery rerun, 20/20 browser host suites,
  formatting, artifact fail-closed checks, and exact 585-resource staging.
- [x] Build and deep-sign the prior synchronized direct and attached-host
  dogfood apps with the exact cmux/Ghostty receipts.
- [x] Use Computer Use scripted text injection to exercise prior-pin ASCII
  rendering, Backspace, Ctrl-C, dynamic colors/reset, theme bar cursor, mouse,
  continuous resize, and isolated renderer recovery through both direct and
  attached paths. Scripted `type_text` injection does not validate the
  physical AppKit key-event path.
- [x] Persist that prior signed evidence, update the active PR bodies,
  and confirm the rewritten heads' required checks.
- [x] Synchronize the cmux stack onto canonical cmux main
  `4daa93725694d51feae628426e878ccb73eabe6f` and repin the browser to cmux TUI
  `cbac0eb05454ed02a50a24675c62dd7265f17fef` with Ghostty
  `d6f611a3077aa12510761ca10e2a5e0a93979536`.
- [x] Fix physical printable-key delivery in browser commit `255a9ae`: copy
  AppKit's mutable `insertText:` value before clearing marked text so the
  committed-text accumulator cannot be emptied through an aliased
  `__NSCFString`.
- [x] Add a deploy-time compiled-pin guard that rejects a Chromium Framework
  or `libchrome_dll.dylib` which does not embed both revision pins from the
  current browser checkout.
- [x] Build and deep-sign the then-current Browser and pinned-Ghostty apps;
  record historical verification of physical `NSEvent` printable typing, all
  256 colors, theme/bar-cursor restoration, mouse interaction, and native-size
  resize behavior through the direct Browser and attached-TUI frontends.
- [x] Complete the intermediate synchronization onto canonical cmux main
  `b5631833741d0a1a2c25ede49988eadf2f412dd2` through the complete cmux TUI
  review stack, publish top `768c8d3f99a3b04294a6305a555cc48af638e7a1`,
  and restage all 585 pinned Ghostty resources.
- [x] Merge Browser main `5bd08a31ad239b9a9cb8c9b794354b06c1e28bb1`
  from the bottom Browser PR upward while preserving canonical-registry
  startup, durable terminal hosts, and the updater observer/UI.
- [x] Gate renderer deliveries to one latest frame per NSView display cadence,
  coalesce Ghostty wakeup tasks atomically, batch renderer-worker output,
  deduplicate unchanged PWD updates, coalesce bounded socket ingress, validate
  BGRA IOSurface metadata, and pass the 178-check renderer ordering model.
- [x] Add signed XCUITest coverage for rapid htop wheel bursts and an
  auto-registered, prompt-rejecting Display-P3 comparison; the installed
  pre-fix Browser and pinned Ghostty match exactly at all 256 palette entries.
- [x] Match standalone Ghostty's recursive `config-file` loading in the
  renderer, macOS Chrome-theme projection, and non-mac terminal pane without
  parsing Chromium command-line switches as Ghostty options.
- [x] Compare the synchronized cmux stack directly with canonical main, fix
  every stack-only Clippy failure, restore live rendered selection bounds, and
  pass locked Rust tests, TUI/attach smokes, 41 SDK tests, 150 web tests, and
  the web production build.
- [x] Add signed-app XCUITests that require a click-focused terminal to accept
  individual physical printable keys and require an SGR mouse press/release to
  cross the Browser renderer into the TUI-owned PTY. The pre-rebuild bundle
  reproduces the physical-key failure while its mouse path passes.
- [x] Propagate canonical cmux main
  `49a43bfa0fd1c33ec458a0bb4ecfbd7a6619e16e` bottom-up through all seven
  review branches and publish frontend-parity top
  `ab5028a2c352117908b8cd5af6935916477e5c27`.
- [x] Stage public cmux `3eeed22dec198fec876735f2516cb5b9d8e2c701`
  with all 585 manifest-verified resources and a standalone Ghostty config
  resolver at exact fork revision
  `c55514dd52d806e9aa661ee20381aa19c91c1c09`.
- [x] Complete all seven focused signed-predecessor XCUITest gates: physical
  typing, PTY-observed mouse press/release, fast htop scrolling, deterministic
  wheel bursts, exact direct/attached Ghostty colors and cursor, attached
  Ghostty input/mouse, and retained-frame live resize.
- [x] Rebuild `/Users/cmux-lawrence/Applications/cmux-browser-physical-typing-v2.app`
  in place from cumulative PR #29, verify exact public cmux/Ghostty receipts,
  and rerun all seven focused XCUITests without installing a second Browser
  app.
- [x] Advance the final Browser artifacts to public cmux
  `67d8479e05b9fa14f82dd1b9fdc08ffc701198ee` and its exact Ghostty submodule
  `50ad1963d9c73ee957932ccb4d26bf6d15575ee7`; pass the complete locked cmux
  workspace and Ghostty `zig build test`, restage all 585 manifest-verified
  resources, and rebuild both canonical apps in place.
- [x] Rerun all seven signed XCUITest gates at the advanced pin: physical
  typing, PTY-observed mouse press/release, attached Ghostty color/mouse/input,
  lossless 256-color parity, rapid htop scrolling, per-burst frame advancement,
  and retained-frame live resize with final-grid recovery.
- [x] Advance the final cmux receipt to public main
  `f6085bbb9259767f94ed4890b0eb8df609209936`. Its only delta from the fully
  tested `67d8479e05b9fa14f82dd1b9fdc08ffc701198ee` is Swift Iroh client
  cancellation; `cmux-tui/` is byte-identical and the Ghostty gitlink remains
  `50ad1963d9c73ee957932ccb4d26bf6d15575ee7`. Restage all 585 resources,
  compile and atomically deploy through audited HQ, verify the signed bundle's
  compiled/runtime receipts, and pass all seven focused XCUITests at this
  receipt.
- [x] Merge cumulative Browser PR #29 into `main` as
  `9e87ed1aa21b284bf8d675ddfdc2ef40f537a85e`, verify reviewed head
  `b4f17a1f5393f4f1871f4c1ffe09de538da136bd` is reachable from freshly
  fetched `origin/main`, preserve already-merged PR #4, and close open PRs #7,
  #13, #14, and #17 as superseded.
- [ ] Restore the Computer Use controller/Guardian authentication path and
  repeat visual dogfood. Current native XCUITest evidence is green; Computer
  Use itself fails before app inspection because the Sky Computer Use service
  startup request fails.
- [ ] Before any public binary release, generate the target-specific Chromium,
  Ghostty/Zig, and uBlock notice/source bundle; verify every shipped theme's
  provenance; enforce extension digests; provide the static-LGPL relinking
  path where required; and establish counsel-reviewed, versioned contributor
  agreements for future dual-licensing rights.

## Current cumulative status

- Public cmux `f6085bbb9259767f94ed4890b0eb8df609209936` and Ghostty
  `50ad1963d9c73ee957932ccb4d26bf6d15575ee7` are the only Browser build
  pins. The staged helper reports that exact pair. The freshly fetched public
  head `4d9c2389f682536deac611b1c470bd0e2923bc7b` differs only under
  `Packages/iOS/`; the tested TUI/Ghostty payload is unchanged.
- Browser main is `d1fbb4c9c4b846a6e9a316c1a42bef87984d3baa`; cumulative PR #29
  landed as `9e87ed1aa21b284bf8d675ddfdc2ef40f537a85e`, including the reviewed
  extension-popup, extension-strip build, and nested extension-highlight
  alignment fixes, and landing-record PR #32 followed it.
- `/Users/cmux-lawrence/Applications/Ghostty-cmux-pinned.app` was rebuilt in
  place from Ghostty `50ad1963d9c73ee957932ccb4d26bf6d15575ee7`; its embedded
  revision receipt, executable version, and deep strict signature all match.
- Cumulative Browser PR #29 is merged. Its updater, release-protocol, security,
  and deterministic macOS/Ubuntu repository-policy checks all passed.
- Browser validation passes 26/26 host suites, the build/dogfood tests,
  macOS bundle-identity tests, the Linux/Windows release-pipeline tests,
  updater archive/feed tests, Swift parse, and XCUITest harness self-tests.
- The final-receipt app was compiled through audited HQ telemetry
  `20260725T020755.005583Z_cmux-tui-f608-50ad-main_22462-99ce36` on
  `cmux13s-mac-mini`, then deployed atomically in place after its branded
  runtime, deep signature, platform metadata, exact helper/CLI receipts, and
  all 585 resource hashes passed. The lease was released.
- The rebuilt app passes all seven focused XCUITests: physical typing,
  PTY-observed mouse press/release, attached Ghostty color/mouse/input, lossless
  256-color parity, rapid htop scrolling, per-burst frame advancement, and
  retained-frame live resize with final-grid recovery. The attached-Ghostty
  harness now expands its long attach invocation from the launch environment,
  avoiding macOS's synthetic-event timeout before attachment.
- Independent bundle verification confirms public cmux
  `f6085bbb9259767f94ed4890b0eb8df609209936`, Ghostty
  `50ad1963d9c73ee957932ccb4d26bf6d15575ee7`, and all 585 packaged resource
  hashes.
- Independently authored cmux source has landed under GPL-3.0-or-later; this
  does not change the GPL-3.0-only terms for Helium-derived portions or the
  combined browser. Public binary distribution is not: the target-specific
  notice/source, static-LGPL relinking, extension digest, theme/font
  provenance, and contributor-rights gates above remain open.

## Historical pre-cumulative stack and validation status

- Canonical cmux main is `49a43bfa0fd1c33ec458a0bb4ecfbd7a6619e16e`.
  The seven pushed stack heads for PRs #8505, #8444, #8452, #8458, #8460,
  #8475, and #8476 are, respectively,
  `43b52ba3e67bbd54f4240f8782c0ee435ed34765`,
  `02f7680f4c00960c3d59ab62e80113a98a76dc6b`,
  `43e971711daff8aa9eefd99766142738b6302baa`,
  `bb38dd9f66054ffeb36dd59f8ffe896ad731e9c3`,
  `5dbd5762cc131989d87aafbcfa95e0e59b8b2c61`,
  `59dfa84d2eb69e7e0e0f41a17a865000490bdaec`, and
  `ab5028a2c352117908b8cd5af6935916477e5c27`. The top still pins Manaflow
  Ghostty `d6f611a3077aa12510761ca10e2a5e0a93979536`.
- Browser main is `5bd08a31ad239b9a9cb8c9b794354b06c1e28bb1`. The latest local
  PR #14 product commit is `a234c9ff383c66ca0994266ce6901d62f62f96e3`.
  The published Browser PR remains at `2b2317c` until the final signed-app gate
  succeeds.
- Fast-scroll load is now bounded at four layers: display-cadence frame
  admission, renderer-worker output batching, unchanged-PWD deduplication,
  and a bounded coalescing socket-ingress queue. The cadence model covers
  10,000 completions, rejected leases, hidden ACK flush, renderer
  reincarnation, and Core Animation lease lifetime.
- Browser validation passes 26/26 host suites, 178/178 renderer-ordering
  checks, the same 178/178 ordering checks under ASan+UBSan, and all 87 TUI
  protocol checks. Focused reset-barrier, mouse press/release, and physical
  typing tests are green.
- The locked cmux workspace run passes all 800 serial tests. Strict Clippy and
  the TUI/attach smoke tests also pass on the pushed top.
- Final staging contains exactly 585 manifest-verified Ghostty resources plus
  the standalone config resolver, which reports exact Ghostty revision
  `d6f611a3077aa12510761ca10e2a5e0a93979536` and resolves the user's config
  from its matching staged resources.
- On the stale installed app, the rapid-scroll XCUITest captured all 8/8
  expected intermediate frames over 14.780 seconds. This is useful regression
  evidence for the trace itself, not proof of the unbuilt local fixes.
- The new live-resize trace checks retained-frame geometry throughout repeated
  macro and sub-cell changes. It awaits the in-place rebuilt Browser app, as do
  the final exact-color run and Computer Use dogfood.
- Evidence tied to installed helper `cbac0eb05454ed02a50a24675c62dd7265f17fef`,
  intermediate stack top `768c8d3f99a3b04294a6305a555cc48af638e7a1`,
  or canonical main `b5631833741d0a1a2c25ede49988eadf2f412dd2` is retained
  below only as historical signed evidence. It does not validate the current
  `ab5028a2` integration candidate or Browser product commit `a234c9ff`.
- Final completion still requires rebuilding and deep-signing the one canonical
  app at `/Users/cmux-lawrence/Applications/cmux-browser-physical-typing-v2.app`,
  verifying its packaged pins/resources, rerunning the focused and live-resize
  XCUITests, and dogfooding direct Browser plus attached TUI behavior through
  Computer Use. No second Browser app will be installed for that gate.

## Previous cursor-v2 automated evidence (2026-07-19)

- Browser main is `f4fb3b18bf55d71f6fda5870bf5be3a77ecd1fe5`;
  cmux main is `acdbbcf6ae9dd012c86e9846afa5d7914b8cb45c`;
  Manaflow Ghostty main is
  `bb30526cdab8f5fb08ae43e404e3aacc40d3ffc3`. Each is an ancestor of
  the corresponding published stack.
- cmux TUI `b3e87d44c25d3cfadd8e136df38c6c55f1a79a05`
  passes the full locked serial workspace suite and a separate 20/20
  terminal-host recovery rerun. The 311-test core suite, 301-test TUI binary
  suite, and all cursor, C1-normalization, response-authority, complete-default,
  reconnect, resize, and alternate-screen regressions pass.
- Browser host tests pass 20/20. Colors v1 leaves DECSCUSR/mode 12 untouched;
  Colors v2 requires and force-applies one resolved cursor shape/blink pair,
  including same-pair activity transitions.
- The staged helper reports both exact revisions, has SHA-256
  `eb6ef4c56cddb24e51d7ddcbd4a1d724ac6cbfe8c9d9b0e8c212cc11ff779d4d`,
  and ships 585 manifest-verified Ghostty resource files. The final GhosttyKit
  archive staged on the M4 builder has SHA-256
  `a768d7d2a5b73c22bbbca91e4c71c02a6e5100e340c5479115a47c514cfee05b`.
- The user's current Ghostty configuration selects `Monokai Classic`, a bar
  cursor, and the default blink policy. Its target background is `#272822`
  and cursor color is `#c0c1b5`; the installed and bundled theme files are
  byte-identical (SHA-256
  `78cf0958f5e57750eedfd68ef44d3f8655c42035d3732741dc6b59593ebcc6a9`).
- Non-blocking API follow-up: preserve Ghostty's configured-versus-omitted
  cursor-blink policy and `block_hollow` provenance. The current bar cursor
  with omitted `cursor-style-blink` follows native Ghostty semantics.

## Historical signed input-v3 dogfood evidence (2026-07-19)

- The direct app is
  `/Users/cmux-lawrence/Applications/cmux-browser-main-sync-input-v3.app`;
  the independent attached host is
  `/Users/cmux-lawrence/Applications/cmux-browser-msync3host.app`. Both pass
  deep strict signing and contain the same helper SHA-256 recorded above.
- The recorded historical session was
  `cmux-browser-3ea3a5e684cf2de6d69bb65be2063d74` on
  `/tmp/cmux-tui-501/cmux-browser-3ea3a5e684cf2de6d69bb65be2063d74.sock`.
- Computer Use's scripted text injector produced exact direct and attached
  ASCII markers; the run also covered Backspace, Ctrl-C, dynamic
  background/cursor overrides and reset, bar cursor/theme defaults, left/right
  press and release, drag, resize convergence, and input after isolated
  renderer replacement. Because printable markers used `type_text`, this run
  did not validate physical `NSEvent` printable-key delivery.
- After reset and probe exit, the authoritative colors/cursor were foreground
  `#fdfff1`, background `#272822`, cursor `#c0c1b5`, bar, and blinking. The
  installed and bundled `Monokai Classic` files are byte-identical.
- Direct grid changes `150x57 -> 132x57 -> 101x48 -> 146x48` converged in the
  attached view without a stretched final frame. Renderer PID 49553 was
  replaced by PID 54519 while Browser PID 49476, daemon PID 49511, terminal
  host PID 49521, terminal state, and attached client survived.
- Non-blocking protocol follow-up: replace recovery-time release-only mouse
  tombstone replay with tokenized terminal-host input acknowledgement and
  deduplication if exact-once release observation is required. The current
  idempotent replay prevents a stuck button after a downstream flush race.

## Prior signed v4 dogfood evidence

- The signed main app is
  `/Users/cmux-lawrence/Applications/cmux-browser-frontend-scaling-v4.app`
  (bundle `com.cmuxterm.app.dogfood.build-frontend-scaling-v4`, profile
  `cmux-browser-dogfood-frontend-scaling-v4`). The independent signed attached
  host is `/Users/cmux-lawrence/Applications/cmux-browser-v4host.app` (short
  bundle `com.cmuxterm.app.dogfood.v4host`, profile
  `cmux-browser-dogfood-v4host`). Both pass deep strict code-sign verification.
- Both bundles install cmux TUI
  `541d4dd908c9a1265b6abe038c959e3f271de33e`, Ghostty
  `8c645641a1dd1cbff9aa73189629682c01b1a233`, and signed helper SHA-256
  `3f3ffdc2e200165ed61a7270d2b4b74d0fece24a8a1d098a1d99744514e05db9`.
- The run retained registry
  `1e3a8a8a-b6ea-46b4-bff4-cb15413b88a8`, generation
  `175578a5-535a-48be-aa7d-b2851c896614`, and session
  `cmux-browser-e1b0b468b090a95d7fef3826ab18a43d` on
  `/tmp/cmux-tui-501/cmux-browser-e1b0b468b090a95d7fef3826ab18a43d.sock`.
  Attaching the companion with the explicit session and socket made
  `list-clients` report both `native-browser` and `tui` clients on that same
  daemon.
- A GUI-created empty workspace appeared immediately in the TUI without a
  terminal, while a workspace created from the attached TUI appeared
  immediately in the running GUI; TUI selection also changed the GUI's active
  workspace.
- The direct Ghostty renderer and attached TUI matched on all 256 palette
  cells, with a maximum absolute RGB-channel difference of 9. Both frontends
  delivered click, release, drag, and wheel input. The attached-TUI right click
  was consumed and opened its pane context menu, while an unconsumed
  Chrome-side right click produced the native Copy/Paste fallback.
- Repeated viewer-width changes `150 -> 134 -> 117 -> 150` retained native-size
  frames: no stretched intermediate frame or persistent reset/background flash
  was observed.
- Killing renderer utility PID 32274 launched PID 35182 in under one second.
  Browser PID 26073, daemon PID 26106, terminal-host PID 26119, terminal
  `05386a44205145fcb1a2e66bf7beac8d`, surface 2, registry identity, shell
  content, and input from both the direct and attached frontends survived.

The earlier 100-terminal all-warm result above remains historical baseline
evidence. The prior signed-v4 run completed with the following exact
same-bundle physical-footprint checkpoints:

| Checkpoint | Durable terminals/hosts | Ghostty renderers | Physical footprint |
| --- | ---: | ---: | ---: |
| 1 terminal | 1 | 1 | 801,063,544 bytes |
| 10, initially cold | 10 | 1 | 824,989,104 bytes |
| 10, all warm | 10 | 10 | 1,381,351,056 bytes |
| 10, after 30-second eviction | 10 | 1 | 942,871,984 bytes |
| 50, cold-hidden | 50 | 1 | 1,064,201,392 bytes |
| 100, cold-hidden | 100 | 1 | 1,249,106,120 bytes |

At 100 terminals the current policy is 4.08x smaller, a 75.5% reduction from
the 5,101,539,184-byte all-warm baseline, while still retaining 100 independent
PTY-owning terminal-host processes. Selecting an evicted terminal and queuing
input through Computer Use made the echoed input visible within a 610 ms
action-to-fresh-state upper bound. Sidebar action-to-fresh-state bounds stayed
at or below 1,002 ms per leg at 1, 10, 50, and 100 terminals. Cleanup closed
all 99 benchmark-owned terminals and restored the single baseline terminal.
The durable measurements are in
`/Users/cmux-lawrence/fun/cmux-browser-artifacts/cmux-workspace-idempotency/frontend-scaling-v4/scaling-state.json`.
