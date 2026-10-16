# cmux terminal process and state architecture

Current pinned integration revisions:

- canonical cmux main: `4daa93725694d51feae628426e878ccb73eabe6f`
- cmux TUI review-stack top: `cbac0eb05454ed02a50a24675c62dd7265f17fef`
- Manaflow Ghostty: `d6f611a3077aa12510761ca10e2a5e0a93979536`

Review stack: [cmux TUI #8505](https://github.com/manaflow-ai/cmux/pull/8505)
(replacement for closed #8422),
[#8444](https://github.com/manaflow-ai/cmux/pull/8444),
[#8452](https://github.com/manaflow-ai/cmux/pull/8452), and
[#8458](https://github.com/manaflow-ai/cmux/pull/8458), and
[#8460](https://github.com/manaflow-ai/cmux/pull/8460), and
[#8475](https://github.com/manaflow-ai/cmux/pull/8475), and
[#8476](https://github.com/manaflow-ai/cmux/pull/8476);
[cmux Browser #13](https://github.com/manaflow-ai/cmux-browser/pull/13) and
[#14](https://github.com/manaflow-ai/cmux-browser/pull/14); and
[Ghostty #128](https://github.com/manaflow-ai/ghostty/pull/128), stacked on
#127, #123, and #122.

## Authority boundaries

cmux TUI is the only writer for shared workspace and terminal lifecycle state.
It stores that state in the session's `workspace-registry.sqlite3` database.
The Chromium and attached-TUI models are disposable projections of the same
registry stream.

The durable database stores:

- stable workspace UUID, name, group, order, tombstone, and workspace revision;
- stable terminal UUID, canonical workspace UUID, host incarnation, lifecycle,
  launch specification, exit metadata, tombstone, and terminal revision;
- exactly-once mutation fingerprints/results and ordered events;
- schema-versioned frontend presentation projections (workspace hierarchy,
  color, expansion, tiled/strip mode, selection) that never own workspace or
  terminal lifecycle.

It deliberately does not store Chromium tab ids, cmux daemon surface ids,
process ids, file descriptors, renderer ids, or IOSurface handles.

Each terminal host writes a private JSON discovery record after binding its
Unix socket and before reporting Ready. The record contains its stable terminal
UUID, current incarnation, endpoint, and owner capability. It is not placement
authority. Its legacy `workspace_key` is ignored when a canonical registry row
exists.

## Runtime and durable paths

On macOS, the browser-started daemon keeps its short Unix socket under
`/tmp/cmux-tui-$UID/<session>.sock`, while durable state defaults to
`~/Library/Application Support/cmux-tui/sessions`. A stable, filesystem-safe
session component below that root contains `workspace-registry.sqlite3` and
`writer.lock`; per-session terminal-host discovery records live under the same
durable root. `CMUX_TUI_STATE_DIR` or explicit `--state` overrides the root.
Changing `--socket` does not move state.

The `writer.lock` lease rejects a second daemon for the same durable session.
Inside the owner daemon, one registry actor serializes GUI, attached-TUI, and
recovery mutations; SQLite commit happens before the reply/event is published.
Revision CAS plus `(origin, mutation_id)` fingerprinting makes a retry return
the original result instead of applying twice. Frontends immediately apply the
committed registry event and never write the database directly.

Owner-mux active selection is ephemeral rather than lifecycle state. A fresh
attached-TUI selection is projected into Chromium once; Chromium's local web
tab/folder selection remains in its schema-versioned frontend projection and
is not silently promoted into shared owner focus. This prevents unrelated
registry refreshes from undoing a local GUI choice while preserving immediate
TUI-to-GUI focus intent.

Protocol-v7 detailed subscriptions emit specific topology deltas rather than
only `tree-changed`. Chromium classifies `tree-changed` plus every workspace,
screen, pane, and tab add/close/rename/move delta as a tree refresh. This is
required for selection ordering: `Ctrl-b t` emits `tab-added`, so materializing
the terminal from `terminal-registry-changed` alone is insufficient to learn
the newly active tab.

## Daemon upgrade handoff

`identify` advertises the fenced handoff capability. Before a cold-start
replacement, Chromium binds the claimed daemon PID to the Unix socket peer,
validates PID/generation and the expected build identity, and fails closed if
another declared native-browser client owns the session. The daemon then
atomically reserves handoff under the client-registry lock, rejecting both an
existing browser owner and any late browser ownership announcement. It queues
the acknowledgement, exits through normal shutdown, and leaves every durable
terminal host available for the replacement to adopt. Chromium treats either
the acknowledgement or a post-request EOF as provisional and waits for the old
PID/executable writer-lease boundary before launching.

One transition from the explicitly allowlisted pre-capability durable builds
uses a peer-PID- and executable-verified SIGTERM. That legacy fallback cannot
make owner admission atomic because the old daemon lacks the API; it is limited
to the exact shipped commits and disappears once those builds age out.

## Backend scaling baseline

The isolated `d4dcef408` benchmark creates 1, 10, 50, then 100 live `/bin/cat`
terminals against the exact staged helper. At 100 terminals it measured 201
session processes (one daemon, 100 terminal hosts, 100 children) and 584,576
KiB aggregate RSS. Terminal hosts used 418,048 KiB total with a 4,176 KiB
median; the daemon used 30,496 KiB. Median create-to-running latency was 61.97
ms (p95 67.78 ms), `list-terminals` was 0.414 ms (p95 0.520 ms), and ping was
0.020 ms (p95 0.028 ms). This is a backend/process baseline only; the
implemented cold-hidden renderer policy is measured separately below. The
reproducible backend harness is `scripts/benchmark-cmux-tui-scaling.py`.

## Frontend scaling and cold residency

The cleaned pre-eviction Browser run at cmux TUI
`50f4ed0b6be20335d9b586f2f8f8a4fc9ccf0b15` projected 1, 10, 50, then 100
durable terminals. At 100 it kept 100 terminal hosts and 100 Ghostty utilities
alive: 311 scoped processes, a 5,101,539,184-byte physical footprint, and
4,099,211,608 bytes attributed to the terminal renderer utilities. Aggregate
RSS was 12,057,440 KiB, which double-counts shared pages. This is the explicit
warm-renderer baseline, not evidence for the current cold policy.

The current Browser implementation starts a hidden terminal renderer cold but
registers its durable GUI observer and creates/adopts its cmux terminal host
immediately; "cold" never delays the PTY or workspace lifecycle. A previously
visible renderer gets a 30-second warm grace. Eviction begins only with
a same-generation live/final surface lease and no input, resize, direct-attach,
mouse-capture, IME, or recovery transition in flight. `PrepareForColdEviction`
orders behind older semantic operations; the renderer flushes its direct
socket and `ReleaseViewer`, then publishes a same-pipe Ready marker. That Ready
is the commit point: Browser suspends only the high-volume renderer stream,
keeps the durable UI observer for immediate title/exit updates, clears the
hidden IOSurface, and terminates the disposable utility. A 10-second watchdog
restarts only a wedged utility.

Keys, committed text, paste, mouse presses/releases/motion/scroll, and preedit
that arrive while evicting or waking enter a bounded Browser queue. They flush
in physical order only after a fresh renderer has applied and presented its
authoritative replay; durable mouse-release tombstones remain ordered before a
new press. Visibility derived at Ready time decides whether to stay cold or
wake immediately, so a transient show/hide cannot strand a hidden hot process.
An exited terminal can rehydrate its final VT only while its daemon-generation
surface lease remains valid.

The prior signed-v4 frontend harness
`scripts/benchmark-cmux-browser-scaling.py` recorded:

| Checkpoint | Hosts | Renderers | Same-bundle physical footprint |
| --- | ---: | ---: | ---: |
| 1 terminal | 1 | 1 | 801,063,544 bytes |
| 10 initially cold | 10 | 1 | 824,989,104 bytes |
| 10 all warm | 10 | 10 | 1,381,351,056 bytes |
| 10 after eviction | 10 | 1 | 942,871,984 bytes |
| 50 cold-hidden | 50 | 1 | 1,064,201,392 bytes |
| 100 cold-hidden | 100 | 1 | 1,249,106,120 bytes |

The 100-terminal footprint is 4.08x smaller than the 5,101,539,184-byte
all-warm baseline (75.5% less) while preserving one PTY-owning OS process per
terminal. A cold BackTab plus immediately queued input was visibly echoed
within a 610 ms Computer Use upper bound. Empty-workspace/sidebar switch bounds
remained no worse than 1,002 ms per leg across the 1/10/50/100 checkpoints,
including Computer Use state-capture overhead. Cleanup removed all 99 owned
benchmark terminals and restored the original single terminal.

## Current synchronized build and validation status

The source stack is synchronized at cmux TUI
`cbac0eb05454ed02a50a24675c62dd7265f17fef`, based on canonical cmux main
`4daa93725694d51feae628426e878ccb73eabe6f`, with Manaflow Ghostty
`d6f611a3077aa12510761ca10e2a5e0a93979536`. The exact pair has passed signed
direct-Browser and pinned-Ghostty attached-TUI Computer Use dogfood.

Physical printable-key actions found a browser-side AppKit aliasing bug that
scripted `type_text` input did not exercise. `insertText:` received a mutable
`__NSCFString`; retaining that object and then clearing marked text emptied the
same object before committed text was sent. Browser commit `255a9ae` copies the
incoming string before clearing marked text. Final typing verification must use
physical `NSEvent`-producing actions such as Computer Use `press_key` or
XCUITest `typeKey`; `type_text` alone is insufficient. The current build was
validated with per-key `press_key`: direct `echo physok` and attached
`echo tuiok` both rendered their expected output.

The current Browser and attached-TUI probes each recorded exactly one mouse
press/release pair at `46,15` and `42,20`. Native-size resize reflow traversed
`175x60 -> 142x51 -> 135x48` directly and
`88x37 -> 71x29 -> 124x42` through the attached TUI without stretch or scale
flash. Every one of 256 sampled swatches classified to its canonical color in
both captures; the exact `#272822` background and configured thin bar cursor
returned after OSC reset.

One partial remote incremental assembled an invalid split-brain bundle. Its
compiled Chromium Framework contained cmux TUI
`0b34c26ddd546ed782e05d613838b0b61fc6e668` and Ghostty
`71ed4f8f6fea1c9581cb45cdd26521f0ef4334f6`, while the bundled helper reported
cmux TUI `b3e87d44c25d3cfadd8e136df38c6c55f1a79a05` and Ghostty
`d6f611a3077aa12510761ca10e2a5e0a93979536`. Runtime identity verification
correctly rejected it. Deployment now runs
`scripts/verify-cmux-runtime-pins.sh` before installing or signing the helper;
the guard scans the built Framework or `libchrome_dll.dylib` for both checkout
pins and rejects stale or one-file incremental runtime images.

## Historical signed input-v3 dogfood evidence (2026-07-19)

The prior signed pair was:

| Role | Application | Bundle ID | Isolated profile |
| --- | --- | --- | --- |
| Direct Browser frontend | `/Users/cmux-lawrence/Applications/cmux-browser-main-sync-input-v3.app` | `com.cmuxterm.app.dogfood.build-main-sync-input-v3` | `cmux-browser-dogfood-main-sync-input-v3` |
| Independent embedded-Ghostty attached-TUI host | `/Users/cmux-lawrence/Applications/cmux-browser-msync3host.app` | `com.cmuxterm.app.dogfood.msync3host` | `cmux-browser-dogfood-msync3host` |

Both applications pass deep strict code-sign verification, install cmux TUI
`b3e87d44c25d3cfadd8e136df38c6c55f1a79a05` and Manaflow Ghostty
`d6f611a3077aa12510761ca10e2a5e0a93979536`, and contain the same signed
helper SHA-256
`eb6ef4c56cddb24e51d7ddcbd4a1d724ac6cbfe8c9d9b0e8c212cc11ff779d4d`.
The GhosttyKit archive staged from the M4 builder has SHA-256
`a768d7d2a5b73c22bbbca91e4c71c02a6e5100e340c5479115a47c514cfee05b`.

The direct Browser session was attached from the companion terminal with:

```sh
"/Users/cmux-lawrence/Applications/cmux-browser-msync3host.app/Contents/Helpers/cmux-tui" attach \
  --session "cmux-browser-3ea3a5e684cf2de6d69bb65be2063d74" \
  --socket "/tmp/cmux-tui-501/cmux-browser-3ea3a5e684cf2de6d69bb65be2063d74.sock"
```

Computer Use's scripted text injector produced ASCII markers through both the
direct renderer and the attached TUI; the run also covered Backspace and
Ctrl-C, including after renderer recovery. Because the printable markers used
`type_text`, this historical run did not validate physical AppKit printable-key
delivery. The hosted daemon's Ghostty surface is a response-silent mirror: the
durable terminal host is the sole authority for terminal-generated
Kitty/DA/DSR replies, so a fresh attachment cannot inject a duplicate query
response into the shell.

The parity probe showed matching dynamic background/cursor overrides and an
immediate reset in both views. After reset and probe exit, the authoritative
state was foreground `#fdfff1`, background `#272822`, cursor `#c0c1b5`, bar
shape, and blinking enabled. The installed and bundled `Monokai Classic` theme
files are byte-identical with SHA-256
`78cf0958f5e57750eedfd68ef44d3f8655c42035d3732741dc6b59593ebcc6a9`.

Direct and attached mouse press/release, drag, and right-click input reached
the PTY. Direct resize moved the canonical grid from `150x57` through
`132x57` and `101x48` to `146x48`; the attached view followed the
smallest-viewer grid and remained a crisp native-size letterbox instead of
stretching an old frame. Killing renderer PID 49553 launched PID 54519 in
under one second while Browser PID 49476, daemon PID 49511, and terminal-host
PID 49521 survived with terminal contents, colors, cursor state, attachment,
and subsequent input intact.

Release-only mouse tombstones are intentionally eligible for replay after a
renderer/socket recovery. This prevents a permanently stuck button when an
accepted release was not yet flushed downstream, but an application can
observe an idempotent release without a new press. A tokenized terminal-host
input acknowledgement/deduplication extension is the follow-up if exact-once
release observation becomes more important than the current recovery rule.

## Prior signed v4 dogfood evidence

The prior signed-v4 pair is:

| Role | Application | Bundle ID | Isolated profile |
| --- | --- | --- | --- |
| Direct Browser frontend | `/Users/cmux-lawrence/Applications/cmux-browser-frontend-scaling-v4.app` | `com.cmuxterm.app.dogfood.build-frontend-scaling-v4` | `cmux-browser-dogfood-frontend-scaling-v4` |
| Independent embedded-Ghostty attached-TUI host | `/Users/cmux-lawrence/Applications/cmux-browser-v4host.app` | `com.cmuxterm.app.dogfood.v4host` | `cmux-browser-dogfood-v4host` |

Both applications pass deep strict code-sign verification and install the same
signed helper SHA-256
`3f3ffdc2e200165ed61a7270d2b4b74d0fece24a8a1d098a1d99744514e05db9`.
That helper was signed from the staged unsigned helper whose SHA-256 begins
`1e1fcb11`, with cmux TUI receipt
`541d4dd908c9a1265b6abe038c959e3f271de33e`, Ghostty receipt
`8c645641a1dd1cbff9aa73189629682c01b1a233`, and all 585 manifest resources.
The first companion signing attempt used a longer bundle ID whose derived
app-shim bootstrap endpoint exceeded the macOS Mach bootstrap name limit and
failed in `MachBootstrapAcceptor::Start`. The short
`com.cmuxterm.app.dogfood.v4host` identity is therefore intentional, not an
abbreviation of the main application's durable identity.

The direct Browser session can be attached from the companion terminal with:

```sh
/Users/cmux-lawrence/Applications/cmux-browser-frontend-scaling-v4.app/Contents/Helpers/cmux-tui attach \
  --session cmux-browser-e1b0b468b090a95d7fef3826ab18a43d \
  --socket /tmp/cmux-tui-501/cmux-browser-e1b0b468b090a95d7fef3826ab18a43d.sock
```

That session retained canonical registry identity
`1e3a8a8a-b6ea-46b4-bff4-cb15413b88a8`, daemon generation
`175578a5-535a-48be-aa7d-b2851c896614`, and Home workspace UUID
`3a2e31da-5199-4f34-b1bc-5dfa9ec6f9a6`. Its baseline terminal retained UUID
`05386a44205145fcb1a2e66bf7beac8d` and host incarnation
`e1056602192243ff90b30fd1048f6fc3` throughout renderer recovery.

Computer Use drove the direct terminal and the independently signed attached
TUI against that same terminal. The two 256-cell palette captures matched at
the established capture tolerance of 15. Left-click, drag/release, wheel, and
right-click events arrived through both frontends; direct-shell right-click
showed the native Copy/Paste fallback, while the attached TUI consumed the
event for its pane context menu. Repeated grow/shrink operations retained the
last native-size IOSurface and filled exposed space with the resolved terminal
background rather than stretching the frame or showing the transient resize
scale flash.

The live process boundary behaved as designed: killing only Ghostty renderer
utility PID 32274 launched replacement PID 35182. Browser PID 26073, cmux TUI
daemon PID 26106, terminal-host PID 26119, the terminal UUID/incarnation,
scrollback, post-crash input, and the attached view all survived. This is
prior signed-v4 recovery evidence; the completed frontend cold-residency
benchmark is recorded in the preceding section and its durable JSON artifact.

## Historical signed dogfood evidence

The isolated main app at
`/Users/cmux-lawrence/Applications/cmux-browser-workspace-idempotency.app` and
attached-TUI host at
`/Users/cmux-lawrence/Applications/cmux-browser-tui-host.app` both pass deep
strict code-sign verification. Their bundle IDs are respectively
`com.cmuxterm.app.dogfood.build-workspace-idempotency` and
`com.cmuxterm.app.dogfood.tui-host`; both contain helper SHA-256
`b4d42913467bfde12100474f896948af4ca231d92bb699910c972f925a8fc233`,
cmux TUI receipt `d4dcef408e71755fe43f758b71ba677caca24049`, Ghostty receipt
`8c645641a1dd1cbff9aa73189629682c01b1a233`, and a 585-entry exact resource
manifest.

The historical pre-cold dogfood session was attached with:

```sh
/Users/cmux-lawrence/Applications/cmux-browser-tui-host.app/Contents/Helpers/cmux-tui attach \
  --session cmux-browser-578d60b3c3e1e9309776a34a1b59aaae \
  --socket /tmp/cmux-tui-501/cmux-browser-578d60b3c3e1e9309776a34a1b59aaae.sock
```

The upgrade path failed closed with a second declared native-browser, then
replaced an allowlisted legacy daemon while preserving the registry identity,
every pre-existing terminal-host PID, and terminal contents. Live registry
dogfood covered empty GUI workspace projection, TUI workspace/terminal
creation, mouse and BackTab selection, and immediate TUI-to-GUI focus.

Computer Use drove the same 256-cell palette probe directly and through the
attached TUI: all 256 cells matched at capture tolerance 15, including sparse
OSC 4 index 1 `#ff3562`. Both paths delivered click, drag, release, and wheel
events. Continuous main/attached window resize stayed crisp and used the exact
resolved OSC 11 background around retained native-size frames. Killing selected
renderer PID 29183 caused Chromium to launch PID 35430; browser PID 28547,
daemon PID 18862, PTY host PID 29182, stable terminal identity/incarnation,
scrollback, and the attached view remained unchanged.

Legacy prototypes that inferred `<socket>.state` are not migrated implicitly.
They can be opened with `--state <legacy-path>` or moved only while the daemon
and its durable terminal hosts are stopped.

## Processes and transports

| Producer | Consumer | Transport | Contents |
| --- | --- | --- | --- |
| Browser/TUI frontend | cmux TUI daemon | framed JSON over the session Unix socket | workspace/terminal mutations, snapshots, ordered lifecycle events, renderer-grant requests |
| cmux TUI daemon | terminal host | authenticated framed Unix stream | owner control, adoption, metadata, one-use renderer capability minting |
| terminal host | Ghostty renderer utility | already-authenticated framed Unix stream passed as a Mojo platform handle | checkpoint, output, exact sparse colors, committed resize, title/pwd/bell/exit; input and viewer-size in the reverse direction |
| Ghostty renderer utility | Chromium compositor | leased IOSurface Mach send right over Mojo | one completed frame plus geometry epoch and release token |

PTY bytes, keyboard/IME input, mouse-encoded bytes, and resize requests do not
traverse the Chromium browser process in direct mode. Shared memory is used
only for explicitly leased IOSurface frames; no lifecycle state lives in shared
memory.

The switch into direct mode is ordered rather than best-effort. The renderer
quiesces new Ghostty input into a bounded buffer and emits a same-Mojo-pipe
marker after every older compatibility `OnInput`. The browser drains those
frames and then mints the renderer capability on the same terminal-host
owner/admin stream, which is the host-side barrier. The renderer applies the
authoritative snapshot and colors, queues the buffered prefix first on its
socket, and only then commits subsequent input to that socket. cmux PR #8458
locks this ordering contract with fragmented pre-barrier input.

Within each renderer, compatibility output, direct-host state, lifecycle
controls, and semantic key/text/paste/mouse actions share one bounded service
lane. A semantic action waits for every older output-worker task before asking
Ghostty to encode it. During an exact-state transition, semantic actions remain
held; state restore and cancellation/teardown may pass them without opening the
gate against stale modes. Direct `Exit` is itself an output barrier and records
the final inspectable state before Browser can synchronously detach the socket.
An attach watchdog requests ordered cancellation after five seconds and, if
that RPC is still wedged two seconds later, restarts only the disposable
one-terminal renderer process.

Viewer size uses a negotiated lightweight acknowledgment. When the canonical
grid and backing scale are unchanged, `ResizeAck` advances the renderer's frame
context without mutating Ghostty's grid, refreshing, or waiting for a redundant
replacement frame. Changed grids still publish the atomic resized replay and
colors before their acknowledgment; backing-scale changes retain the exact new
frame boundary. Zero pixel metrics from an ioctl retain the last authoritative
cell-pixel dimensions, and older hosts retain the replay fallback.

Occlusion suppresses renderer frames immediately, while viewer-lease release
is debounced for 100 ms so rapid workspace switches can cancel it. If release
has already reached the host, re-show keeps Ghostty publication occluded through
the matching re-add acknowledgment/replay and then refreshes once; the retained
IOSurface stays visible, so the release-induced intermediate grid cannot flash.

The renderer utility is a real crash and scheduling boundary, but it currently
uses Chromium's `kNoSandbox` service policy. Moving it into a constrained
renderer sandbox is a security-hardening follow-up; this process split alone
must not be described as exploit containment.

## Transaction and crash rules

1. A create reserves a client-known terminal UUID in `launching` state before
   any process is spawned.
2. The host binds its private endpoint, writes its discovery record atomically,
   and only then reports its UUID/incarnation as Ready.
3. Startup reconciliation changes a matching row to `adopting`, authenticates
   the record, materializes a daemon-local surface, then commits `running`.
4. Close commits the terminal tombstone before signaling the host. A lost
   reply retry returns success and can never recreate the shell.
5. Closing a workspace tombstones every child terminal in the same SQLite
   transaction before any host is signaled.
6. A terminal UUID is never reused. Daemon-local numeric ids are re-resolved
   from the stable UUID after every daemon generation change.
7. A tombstone with a live host record causes host termination. A live row with
   a matching record is adopted. A reservation without a record remains an
   explicit pending/recovery state; it is never replaced by an unrelated shell.
8. Registry mutations are serialized, committed before publication, guarded by
   generation/revision CAS, and deduplicated by `(origin, mutation_id)` plus a
   canonical payload fingerprint.

Terminal hosts start in independent sessions and close every inherited daemon
file descriptor before accepting traffic. The process split therefore makes
failures local: a renderer crash loses only disposable pixels, a Browser crash
loses only its projection, a daemon crash leaves hosts and PTYs alive for
adoption, and a terminal-host crash affects only that terminal.

Mouse capture also stays in the browser across renderer-incarnation loss. A
release generated after a renderer crash is replayed only after the replacement
publishes an authoritative grid, and new presses are gated until then. The last
acknowledged release per button is retained as an idempotent crash-replay
tombstone. Release acknowledgments are token-matched but never gate later
release RPCs, ensuring every release enters the ordered Mojo pipe before the
next physical press even while older acknowledgments are outstanding.

The remaining crash boundary is input already accepted by the old direct
renderer/socket but not flushed to the PTY. The protocol has no per-input
sequence, acknowledgment, or reconnect deduplication yet, so that narrow window
is not exactly-once. The hard watchdog restart can likewise lose a semantic
intent held only inside a wedged renderer. Closing both windows requires a
Browser/host-visible input journal with `InputAck` plus deduplication, or one
host-owned serialized ingress path.

## Runtime resource identity

The pinned Ghostty static library does not embed named themes, shell
integration, or terminfo. The build generates those resources from the exact
pinned Manaflow Ghostty checkout, stages them with both revision receipts and
an exact SHA-256 manifest, and installs them into
`Contents/Resources/{ghostty,terminfo}` before the outer application is signed.
Missing, modified, unlisted, symlinked, or special files are rejected. The app
bundle is authoritative on macOS, so an incomplete cmux bundle fails closed
instead of borrowing resources from an unrelated Ghostty installation.
