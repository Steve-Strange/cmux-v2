# cmux TUI-backed Ghostty terminals

Status: historical; superseded by `cmux-tui-workspace-renderer-process-plan.md`
Last updated: 2026-07-16

Follow-up architecture for the shared workspace registry and out-of-process
Ghostty rendering is recorded in
[`cmux-tui-workspace-renderer-process-plan.md`](cmux-tui-workspace-renderer-process-plan.md).
It supersedes this document's earlier last-writer-wins resize assumption and
its pre-merge description of cmux PR #8031.

Browser worktree: `/Users/cmux-lawrence/fun/cmux-browser-wt-tui-backend`  
Browser branch: `codex/cmux-tui-terminal-backend`  
Browser base: `07a53c7e98a6c72351884d29bbc7c0f1e019b7d8`  
cmux worktree: `/Users/cmux-lawrence/fun/cmuxterm-hq/worktrees/codex-cmux-tui-browser-backend`  
cmux branch: `codex/cmux-tui-browser-backend`  
cmux base: `2b6ddcbff5b27fb1c91434476e8953786945bc24`  
Ghostty worktree: `/Users/cmux-lawrence/fun/ghostty-wt-cmux-tui-backend`  
Ghostty branch: `codex/cmux-tui-backend-ghostty`  
Ghostty base: `bb30526cdab8f5fb08ae43e404e3aacc40d3ffc3`
Focused Ghostty review worktree: `/Users/cmux-lawrence/fun/ghostty-wt-manual-mirror-pr`
Focused Ghostty review branch: `codex/manual-mirror-io` at `1a0e1361622a57e145ab5e58e11ccf747fe2cc54`

Browser review PR: [#4](https://github.com/manaflow-ai/cmux-browser/pull/4)
cmux TUI candidate: `7b3304c44283196ce69acc35db731f5e30a5df3f`
manaflow Ghostty candidate: `90ee78676e0df3b6ee4523386d06cb6ccd33ed8f`
Chromium verification base: `0bd9366db7ef5eebf3cd6e3927b53b86f7b95899`

## Goal

Make cmux TUI the process, PTY, session-lifetime, and authoritative terminal-state backend for cmux-browser terminal tabs while retaining libghostty as the browser renderer and input encoder.

cmux-browser remains authoritative for workspaces, niri columns, split presentation, tab placement, and native Chromium `WebContents`.

## Fixed architecture decisions

- Use the `manaflow-ai/ghostty` fork for both cmux TUI and cmux-browser.
- Pin the Ghostty source revision explicitly; the browser header and linked library must come from the same revision.
- Connect the browser to a local headless cmux TUI session over the Unix JSON-lines socket.
- Keep terminal backend lifetime independent from native Ghostty view lifetime.
- Use Ghostty manual IO mode and feed server output through `ghostty_surface_process_output`.
- Suppress parser-generated terminal responses in the browser mirror while preserving user keyboard, mouse, text, and paste output.
- Keep the browser's `WindowModel` authoritative. Do not block terminal integration on cmux TUI split-tree ownership, PR #8023, or PR #8031.

## Current findings

- [x] cmux's Ghostty submodule URL is `https://github.com/manaflow-ai/ghostty.git`.
- [x] cmux TUI builds `libghostty-vt` from that submodule.
- [x] The inspected cmux Ghostty pins expose manual IO and `ghostty_surface_process_output`.
- [x] Mirror response suppression exists as Ghostty commit `350c96b7437b2befd558abcd3f585f7c4854ea4a` on `feat/electron-embedded-cross-platform`.
- [x] Port response suppression onto current manaflow Ghostty main in the isolated worktree.
- [x] Keep response suppression out of `ghostty_surface_config_s` by exposing it as `GHOSTTY_SURFACE_IO_MANUAL_MIRROR`.
- [x] Create the initial local Ghostty candidate commits `581dbf2` (`embedded: add manual mirror IO mode`) and `0d23cf5` (`fix: compile embedded OpenGL realize hook`).
- [x] Evaluate the browser's prior `GHOSTTY_PLATFORM_OFFSCREEN` and `ghostty_surface_set_frame_callback` dependency. That ABI is not present in current manaflow branches, local clones, or the produced native library and should not be reconstructed as a private dependency.
- [x] Port the published host-owned OpenGL ABI from manaflow Ghostty PR [#110](https://github.com/manaflow-ai/ghostty/pull/110), including its Linux packaging and Windows/WGL lifetime fixes.
- [x] Advance the local Ghostty candidate to `90ee78676e0df3b6ee4523386d06cb6ccd33ed8f`; its embedded `ghostty_surface_config_s` ABI is 152 bytes on the pinned 64-bit targets because the OpenGL callback payload is inline.
- [x] Publish the Ghostty candidate branch before committing the dedicated cmux worktree's parent gitlink.
- [x] Compile and run the filtered Linux-musl Ghostty test binary: all 71 tests passed, including every parser-reply class and encoded user-input preservation.
- [x] Compile the embedded libghostty path far enough to evaluate the manual-mirror ABI/default compile-time checks.
- [x] Verify the merged cmux custom-frontend transport provides the commands and events required by the byte-mirror design.
- [x] Port the focused final-cursor replay fix from the render-state work into current cmux main as local commit `e573ec8fb3` (`fix(tui): restore cursor after VT replay`).
- [x] Run all 34 `ghostty-vt` unit and integration tests against the current cmux Ghostty pin; all passed.
- [x] Verify cmux TUI's Windows backend uses AF_UNIX (`uds_windows`), the same runtime socket-path convention, and `portable-pty`; browser and TUI transports are compatible on Windows 10 1809+.
- [x] Implement the browser protocol client, persistent backend ownership seam, and manual Ghostty mirror frontends as local browser commit `a149129` (`feat: back terminal surfaces with cmux TUI`).
- [x] Add package-time cmux and Ghostty build stamps to cmux TUI in `887c34a7ff`, then advance the local candidate through multi-client resize coverage at `d5b064ac8114094906e340d786ad1ca930a5c9f1`.
- [x] Pin the browser to that candidate and reject cmux or Ghostty identity mismatches on control and attach sockets in browser commits `a6ad210` and `26160c8`.
- [x] Add guarded cmux TUI build/stage/install flows and bundle the verified helper into macOS deployment; the same installer supports Linux and Windows product directories.
- [x] Add the missing Windows Ghostty artifact revision guard.
- [x] Bound each surface to one input request and one resize request in flight, with a 1 MiB pending-input cap and latest-size coalescing, as browser commit `929689e`.
- [x] Pass all seven browser host-test suites after the transport integration (including 44 protocol, source-identity, and outbound-backpressure checks).
- [x] Port the established embedded OpenGL `displayRealized` no-op from the local Windows/Linux integration history so non-GTK library instantiation no longer hits an intentional compile error.
- [x] Advance the browser root pin, runtime identity pin, Windows artifact guard, and local cmux submodule checkout to Ghostty candidate `0d23cf5588f966ce277e4e555ec1f05cc37790e1`.
- [x] Advance those browser pins and the dedicated cmux submodule checkout together to the host-owned OpenGL candidate `90ee78676e0df3b6ee4523386d06cb6ccd33ed8f`; keep the cmux parent gitlink uncommitted until that revision is canonical.
- [x] Diagnose this host's Zig 0.15.2 build-runner failure: the active Xcode 26.5 SDK omits ordinary `arm64` from its `libSystem.tbd` targets. Use a temporary `xcrun` SDK-path shim to select the installed Command Line Tools 15.4 SDK without changing the machine-wide developer directory.
- [x] Build the full x86_64-linux-gnu/OpenGL embedded library graph for Ghostty `90ee78676`: all 195 steps passed, including shared/static `ghostty-internal` and `ghostty-vt` artifacts.
- [x] Run `cargo test --workspace --locked` for cmux TUI against Ghostty `0d23cf5`: all 402 tests in nonempty suites passed.
- [x] Re-run all 402 cmux TUI tests after advancing the submodule checkout to Ghostty `90ee78676`; all passed with the Command Line Tools SDK shim.
- [x] Rebase the cmux candidate onto current main, rerun the complete workspace test suite, publish commit `64b67c925484eb35a1692d81474fb0d741330ad3`, and open draft PR [#8294](https://github.com/manaflow-ai/cmux/pull/8294).
- [x] Build, stage, install, hash-compare, and revision-check the native macOS cmux-tui helper; it reports cmux `d5b064ac8114094906e340d786ad1ca930a5c9f1` and Ghostty `0d23cf5588f966ce277e4e555ec1f05cc37790e1`.
- [x] Cross-build and stage the same stamped helper as x86-64 ELF and PE artifacts. Both passed install-manifest, embedded-stamp, binary-format, and staged/installed hash checks; native Linux/Windows execution remains pending.
- [x] Build Ghostty `90ee78676`'s full x86_64-windows-gnu/OpenGL embedded-library graph: all 97 steps passed, including the DLL, import library, static library, and `ghostty-vt` artifacts.
- [x] Run the exact x86-64 Linux candidate under a disposable Linux runtime with Mesa llvmpipe: the published host-owned EGL/OpenGL callbacks created a 4.3 context, manual-mirror output rendered, and two nonempty RGBA frames were read back successfully.
- [x] Add and pass a real-daemon JSON-lines smoke against the exact stamped helper. It verifies both identities, separate control/attach sockets, live byte output, authoritative resize replay, PTY survival while detached, replay after reattach, and close isolation between sibling surfaces.
- [x] Install/validate the macOS Metal toolchain and build the native arm64 GhosttyKit XCFramework for Ghostty `90ee78676` on the M4 builder: all 120 steps passed.
- [x] Inspect that native artifact: it exports `ghostty_surface_new` and `ghostty_surface_process_output`, exposes manual-mirror plus OpenGL constants in the packaged header, reports the expected 152-byte config ABI, and intentionally does not export the abandoned frame-callback symbol.
- [x] Rebuild, stage, install, and hash-check the macOS, x86-64 Linux, and x86-64 Windows cmux-tui helpers after the Ghostty advance. Every manifest and embedded stamp now pairs cmux `d5b064ac8114094906e340d786ad1ca930a5c9f1` with Ghostty `90ee78676e0df3b6ee4523386d06cb6ccd33ed8f`.
- [x] Rebuild, stage, install, hash-check, and lifecycle-smoke the final native macOS helper from cmux `64b67c925484eb35a1692d81474fb0d741330ad3` and Ghostty `90ee78676e0df3b6ee4523386d06cb6ccd33ed8f`.
- [x] Re-run the real-daemon lifecycle smoke against that rebuilt native helper; protocol 7 identity, replay, resize, detach/reattach, PTY survival, and close isolation all passed.
- [x] Compile and link the browser integration in a full arm64 macOS Chromium checkout on the M4 builder. `autoninja -C out/Release chrome` produced `Chromium.app` after the final component-library link fixes.
- [x] Run the resulting `Chromium 148.0.7776.0` executable, inspect the Ghostty definitions and native framework dependencies in `libchrome_dll.dylib`, and install/run the exact stamped cmux TUI helper from the app bundle.
- [x] Make component-build deployment self-contained by bundling the builder's `out/Release` dylibs under `Contents/Frameworks`, keep revision receipts out of the nested-code `Contents/Helpers` directory, and pass strict deep codesign verification.
- [x] Deploy and launch `/Applications/cmux-browser-tui-dogfood.app`; verify its live Chromium children, native-browser control connection, exact helper identity, active PTY surface, and a terminal command/read-screen round trip.
- [x] Fix the first live attachment failure: Chromium's default JSON writer preserved an exact JSON-safe surface ID stored in `base::Value` as `8.0`, while cmux TUI correctly requires a JSON integer for its `u64`. Serialize protocol requests with `OPTIONS_OMIT_DOUBLE_TYPE_PRESERVATION` so the wire value is `8`.
- [x] Fix the first live replay deadlock: a large `vt-state` replay synchronously parsed on Chromium's UI thread filled Ghostty's bounded app mailbox, while its wakeup tick needed the same UI thread. Feed each mirror from a bounded serial PTY-reader-equivalent queue and explicitly tick/drain before replacing or freeing the surface.
- [x] Dogfood the repaired app through Computer Use: focus the embedded Ghostty column, type `echo __CUA_TUI_BACKEND_FIXED_20260716__`, see the command and output render, and read the identical marker from cmux TUI surface 1.
- [x] Use Computer Use to render all 256 indexed colors and compare their exact replay values with Ghostty 1.3.1's resolved Monokai Classic config. Slots 16-255 matched; slots 0-15 were all incorrectly replaced by cmux TUI's compiled Tomorrow palette.
- [x] Fix palette ownership in cmux candidate `7b3304c442`: attach metadata now reports only PTY-authored OSC 4 overrides. Browser mirrors remove the replay's complete parser palette, retain their own Ghostty theme for untouched entries, and reapply the authoritative overrides.
- [x] Rebuild GhosttyKit from the clean pinned Manaflow checkout, link Chromium 149 against it, and use Computer Use to render the corrected 256-color chart in the signed dogfood app. Add a host regression that removes all 256 replay OSC 4 slots while preserving OSC 10.
- [x] Match Ghostty's AppKit mouse path for entered/exited, drag, extra-button, right-click fallback, precise-scroll, and click-position events. Capture a real Computer Use click as SGR mouse press `ESC[<0;114;31M` plus its matching release through the cmux TUI PTY.
- [x] Debounce resize storms for 100 ms and apply RIS plus authoritative replay to the existing Metal surface as one parser batch. Three Computer Use window-edge drags preserved visible content without a blank replacement frame and settled the PTY from 179x60 to 115x43.
- [x] Publish the Ghostty and cmux dependency branches for review.
- [x] Publish the macOS-required Ghostty manual-mirror change as the focused branch `codex/manual-mirror-io` at `1a0e136`; its filtered native manual-mirror tests pass. Repository instructions prohibit this agent from opening the Ghostty PR, so a human must open it from the pushed branch.
- [ ] Select reviewed canonical cmux and Ghostty revisions.

## Verification snapshot

- The macOS verification checkout used Chromium `0bd9366db7ef5eebf3cd6e3927b53b86f7b95899` with `is_debug=false`, `symbol_level=0`, `is_component_build=true`, and `target_cpu="arm64"`.
- Chromium compatibility and full-link fixes are recorded in browser commits `ff810d8`, `a246e9e`, `3071965`, `78078d3`, `9042aec`, `34df7dd`, and `f1726dc`.
- The final full `chrome` invocation completed successfully against current browser main. After the exact cmux pin changed, the incremental rebuild recompiled `cmux_tui_client.o` and relinked `libchrome_dll.dylib` successfully. The earlier long compile was resumed from the same output directory after relocating Siso temporary logs from the nearly full system volume to `/Volumes/ephemeral0`; this was builder infrastructure, not a source failure.
- `libchrome_dll.dylib` defines `ghostty_init`, `ghostty_app_new`, `ghostty_surface_new`, and `ghostty_surface_process_output` from the pinned Ghostty archive. Its native dependencies include Metal, MetalKit, GameController, AppKit, and system libc++.
- `Chromium.app/Contents/Helpers/cmux-tui --version` reports cmux `64b67c925484eb35a1692d81474fb0d741330ad3` and Ghostty `90ee78676e0df3b6ee4523386d06cb6ccd33ed8f`. The bundled macOS helper SHA-256 is `ca553fd0a952f709f2ac558538626d42dc34587050a0529f9b1c1cda29c8855c`.
- The installed arm64 dogfood bundle is 799 MiB because the component build needs 512 adjacent dylibs. It passes `codesign --verify --deep --strict`, launches through LaunchServices as `cmux ▸ tui-backend`, and is running from `/Applications/cmux-browser-tui-dogfood.app`.
- The live app launched its bundled helper at protocol 7 with the two pinned commits, created an active PTY surface at 110×60, and returned `__CMUX_APP_TUI_BACKEND_OK_20260716__` after the marker was sent through its control socket.
- After the integer-serialization and asynchronous-feed fixes, Computer Use drove real browser-terminal input through Ghostty's encoder into cmux TUI. The embedded pane rendered `__CUA_TUI_BACKEND_FIXED_20260716__`; `list-clients` reported the browser attachment on surface 1 and `read-screen --surface 1` returned the same marker.
- The pre-fix 256-color audit found exactly 16 mismatches: Ghostty Monokai Classic `#272822/#f92672/...` in ANSI slots 0-15 had been replaced by the headless parser's `#1d1f21/#cc6666/...`; every xterm-cube/grayscale entry from 16 through 255 already matched. Candidate `7b3304c442` carries sparse palette-override metadata so the frontend theme remains authoritative unless the PTY explicitly changes a slot.
- The post-fix Chromium 149 build linked successfully against a newly built GhosttyKit from the clean pinned `90ee78676` worktree. `/Applications/cmux-browser-tui-colors.app` is 692 MiB, passes strict deep code-sign verification, and bundles cmux TUI SHA-256 `62f5d218f0f1f9ea72afde8ad860f9f76ccf6dc9d38936763d339d5a601f62c8`.
- Computer Use rendered the corrected 256-color chart, captured SGR mouse press and release through the live PTY, and performed three window-edge resize drags without a blank replacement frame. The authoritative surface changed from 179x60 to 115x43 after the final debounce.
- The final local rerun passed all nine host-test executables, all 48 protocol checks, the browser/cmux/Ghostty pin verifier, and the real protocol-7 lifecycle smoke (`replay_bytes=5682`).
- Full Chromium targets remain to be compiled on Linux and Windows. The Ghostty renderer ABI and cmux helper have already been built for both, the Windows host path syntax-compiles to COFF, and the Linux renderer ran end to end under Mesa llvmpipe.

## Feasibility decision

The integration is feasible with the custom-frontend APIs already on current cmux main. PR [#7890](https://github.com/manaflow-ai/cmux/pull/7890) landed the WebSocket transport for third-party frontends and PR [#7891](https://github.com/manaflow-ai/cmux/pull/7891) landed the reference frontend. The browser can use the lower-overhead local Unix JSON-lines transport exposed by the same server; no additional custom-frontend PR is required for the initial Ghostty byte-mirror architecture.

The currently implemented protocol provides the required contract:

- `identify`, workspace discovery, and subscription for compatibility and lifecycle discovery.
- `new-tab`, `new-workspace`, and `run`, including initial rows and columns.
- `attach-surface`, with an authoritative `vt-state` replay followed by ordered `output`, `resized`, color-metadata, and `detached` events.
- Gap-free replay-to-live handoff under the same terminal lock.
- Bounded attach queues with explicit overflow and reattach semantics.
- `send` for raw base64-encoded Ghostty input bytes, `send-key`, `resize-surface`, and `close-surface`.
- Title, exit, and tree events needed for browser tab lifecycle presentation.

The important limits are understood and do not block the first integration:

- Replay is capped at 8 MiB and retains the newest complete rows. This is adequate for reconstructing a live frontend, but it is not a promise of lossless styled historical scrollback.
- A backend `resized` event requires generation-safe mirror reset and replay because libghostty does not expose a supported terminal-state import API. On macOS the browser now sends RIS plus the authoritative replay as one parser batch to the existing Metal surface, with surface recreation retained only as a failure fallback.
- The browser must apply the attached color/cursor metadata that is not encoded in the byte replay.
- Attach queue overflow requires a fresh attach and replay rather than continuing from a partial stream.

The renderer host decision is also settled. macOS keeps Ghostty's native embedded `NSView`/Metal path. Linux and Windows use the published host-owned `GHOSTTY_PLATFORM_OPENGL` callbacks from manaflow Ghostty PR #110. The browser's old unpublished `GHOSTTY_PLATFORM_OFFSCREEN` plus frame-callback path must be removed; it is not a prerequisite to recover or land. This renderer choice is independent of cmux's custom-frontend protocol, so it does not change the conclusion about cmux PRs #8023 or #8031.

Do not land open PR [#8023](https://github.com/manaflow-ai/cmux/pull/8023) for this work. Stable split IDs matter only if cmux TUI later owns the browser layout tree. Do not land all of open PR [#8031](https://github.com/manaflow-ai/cmux/pull/8031) for the initial backend either. Its render-state and paged styled-scrollback APIs are useful only if we choose a server-authored renderer or require lossless styled history. Extract a focused scrollback API later if that becomes a product requirement.

## Phase 0: isolate and record the work

- [x] Create an active implementation goal.
- [x] Create a dedicated cmux-browser worktree.
- [x] Create branch `codex/cmux-tui-terminal-backend` from browser commit `07a53c7e98a6c72351884d29bbc7c0f1e019b7d8`.
- [x] Persist this implementation plan and checklist.
- [x] Create dedicated Ghostty and cmux worktrees before modifying those repositories.

## Phase 1: establish the Ghostty baseline

- [x] Create a Ghostty worktree and integration branch from current `manaflow-ai/ghostty` main.
- [x] Port `suppress_terminal_responses` from commit `350c96b7437b2befd558abcd3f585f7c4854ea4a`.
- [x] Verify `GHOSTTY_SURFACE_IO_MANUAL` remains available.
- [x] Verify `ghostty_surface_process_output` remains available.
- [x] Test that suppression defaults to false and preserves the existing manual-mode/config ABI behavior.
- [x] Test representative parser-generated DA, DSR, OSC, size, color-scheme, and focus reply classes are suppressed in mirror mode.
- [x] Test representative keyboard, committed-text, SGR-mouse, and bracketed-paste bytes reach the manual backend's `io_write_cb` unchanged.
- [x] Make the OpenGL renderer's generic `displayRealized` hook compile for the embedded runtime without changing GTK behavior.
- [x] Port and harden the published host-owned OpenGL surface ABI from manaflow Ghostty PR #110.
- [x] Test the x86_64 Linux host-owned OpenGL embedded-library build.
- [x] Test the x86_64 Windows host-owned OpenGL embedded-library build.
- [x] Test the native arm64 macOS embedded Metal-surface build and inspect its packaged ABI.
- [ ] Merge the Ghostty change and record the canonical commit SHA.

## Phase 2: pin cmux TUI and its artifacts

- [x] Create a dedicated cmux worktree and branch from current cmux main.
- [x] Restore the authoritative cursor at the end of full and bounded VT replay.
- [x] Test full and bounded replay through a second Ghostty VT instance.
- [ ] Update the cmux Ghostty submodule to the canonical revision.
- [ ] Rebuild or publish the matching GhosttyKit artifact.
- [ ] Update the pinned GhosttyKit checksum manifest.
- [ ] Add an integration-contract check for manual IO, `process_output`, and response suppression.
- [x] Run `cargo test --workspace --locked` in `cmux-tui`.
- [x] Run TUI attach, detach, resize, replay, and close-isolation smoke tests through `scripts/smoke-cmux-tui-backend.py`.
- [ ] Run valgrind and terminal corpus coverage.
- [ ] Run macOS and iOS package tests affected by the shared submodule update.
- [x] Run Linux and Windows libghostty-vt builds as part of the full embedded-library builds.
- [ ] Land the cmux submodule update and cursor-replay fix, then record the canonical cmux commit.
- [x] Build and stage `cmux-tui` for macOS, Linux, and Windows from that exact commit.
- [x] Install and fully execute the exact native macOS helper candidate; install and statically inspect the Linux/Windows cross-builds.
- [x] Add a browser-side cmux TUI and Ghostty binary revision manifest/guard instead of accepting an arbitrary protocol-7 binary for production packaging.
- [x] Make cmux TUI expose both package stamps through `identify`/`ping` and report both through `--version` for artifact verification.

## Phase 3: pin cmux-browser to the same Ghostty revision

- [x] Make the browser Ghostty candidate source and revision explicit in `ghostty-revision.txt`.
- [x] Require clean source at the exact pinned revision and a `manaflow-ai/ghostty` origin in the macOS and Linux artifact scripts.
- [x] Build the Ghostty header and static library from the pinned candidate revision.
- [x] Record the revision in the staged browser artifact.
- [x] Fail macOS and Linux vendoring when the staged artifact revision differs from the browser pin.
- [x] Add the equivalent revision guard to the Windows Ghostty artifact flow.
- [x] Add compile-time ABI checks for the manual-mirror enum and surface config; update all pinned consumers together when the host-owned OpenGL callbacks move the 64-bit config size from 120 to 152 bytes.
- [x] Verify the Linux host-owned OpenGL Ghostty artifact build.
- [x] Verify the Windows host-owned OpenGL Ghostty artifact build.
- [x] Verify the native arm64 macOS Ghostty artifact build.

## Phase 4: add the browser-side cmux TUI backend

- [x] Add a `CmuxTuiClient` using the local Unix JSON-lines transport.
- [x] Implement headless server startup or existing-session discovery.
- [x] Implement `identify` and protocol compatibility checks.
- [x] Require the exact pinned cmux and manaflow Ghostty package stamps in production; allow unpinned development only with `CMUX_TUI_ALLOW_UNPINNED=1`.
- [x] Implement request ID correlation and interleaved event routing.
- [x] Implement `new-tab` and `run` with the initial cell size.
- [x] Map browser `SurfaceTabId` values to TUI surface IDs.
- [x] Introduce a persistent terminal backend object independent from the native view.
- [x] Implement `attach-surface` and its `vt-state`, `output`, `resized`, and `detached` stream.
- [x] Implement `send`, `resize-surface`, `close-surface`, and title/lifecycle events.
- [x] Keep explicit browser tab close separate from view destruction.
- [x] Add and pass host-compilable JSON-lines framing, identity, and backpressure tests (44 checks; all seven host suites pass).
- [x] Add and pass an artifact-backed daemon smoke using the browser's separate control and attachment socket topology.
- [x] Compile the transport/backend ownership seam in the full macOS Chromium target.
- [ ] Compile the transport/backend ownership seam in full Linux and Windows Chromium targets.

## Phase 5: convert browser Ghostty surfaces to manual mode

- [x] Convert the macOS Ghostty source from EXEC to `GHOSTTY_SURFACE_IO_MANUAL_MIRROR` mode.
- [x] Assert the browser header exposes the manual-mirror enum value and expected config ABI.
- [x] Feed initial replay and live output through `ghostty_surface_process_output`.
- [x] Forward Ghostty `io_write_cb` bytes to TUI `send`.
- [x] Forward pixel-to-cell geometry changes through `resize-surface`.
- [x] Implement focus, title, explicit-close, and process-exit routing in the ownership seam.
- [x] Make custom terminal commands create TUI `run` surfaces rather than Ghostty child processes.
- [x] Keep the macOS production terminal on Ghostty's native embedded `NSView`/Metal surface; only the optional compositor-test source still references the obsolete offscreen ABI.
- [x] Replace the Linux/Windows unpublished offscreen/frame-callback source with host-owned EGL/WGL adapters for the published OpenGL callbacks; syntax-compile both platform branches and run the Linux renderer contract end to end.
- [x] Bound renderer-to-UI frame delivery to one posted task and a latest-frame-wins slot so OpenGL readback cannot accumulate unbounded pixel buffers while Chromium's UI sequence is stalled.
- [x] Remove the macOS compositor-test-only offscreen implementation; retain its gradient/IOSurface SharedImage validation without linking any unpublished Ghostty API.
- [x] Compile and link these frontend changes in the full macOS Chromium target.
- [ ] Compile and exercise these frontend changes in full Linux and Windows Chromium targets.

## Phase 6: replay, resize, and concurrency hardening

- [x] Put all Ghostty output processing on each surface's serialized owning sequence.
- [x] Add server/surface and replay generations and reject stale attachment work.
- [x] Buffer ordered live output while a native mirror is unavailable, with a 16 MiB cap and replay recovery.
- [x] On backend `resized`, drain older parser work and apply RIS plus the fresh replay as one batch to the existing Metal surface; reconstruct only if that enqueue fails.
- [x] Debounce live-resize storms while still delivering the final authoritative cell grid.
- [x] Add bounded queues and per-surface backpressure: one input/resize request in flight, capped pending input, latest-size coalescing, and generation-safe cancellation.
- [x] Handle attach overflow, decode failure, or disconnect by ignoring the damaged stream and reattaching from an authoritative replay.
- [x] Reconnect and resubscribe after control overflow or server loss; recreate surfaces only when the identified server generation changed.
- [x] Define multi-client sizing as last-writer-wins without browser feedback loops and add server-level coverage in cmux commit `bcbb1b9aec`.
- [x] Execute the new multi-client resize test on the SDK-shimmed Zig-backed Rust toolchain.
- [x] Remove normal resize-time surface recreation and its visible flash without adding a new Ghostty API.

## Phase 7: lifecycle and fidelity validation

- [x] Drive a visible terminal command through the browser's embedded Ghostty frontend with Computer Use and verify its output independently through cmux TUI `read-screen`.
- [ ] Verify the shell survives native Ghostty view destruction and recreation.
- [ ] Verify the shell survives pane moves, workspace changes, hiding, and reparenting.
- [ ] Verify explicit tab close terminates only the intended TUI surface.
- [ ] Verify alternate-screen entry and exit.
- [ ] Verify cursor position, visibility, style, and blinking.
- [ ] Verify bracketed paste, focus reporting, and Kitty keyboard mode.
- [x] Verify SGR mouse press and release reporting through a real Computer Use click.
- [ ] Verify no duplicate DA, DSR, OSC, or other terminal-protocol replies reach the PTY.
- [ ] Test shells, `vim`, `tmux`, and Claude Code.
- [x] Test rapid interactive resize with retained output and an authoritative final PTY grid.
- [ ] Test sustained output storms during resize.
- [ ] Implement daemon crash, socket disconnect, and reconnect behavior.
- [ ] Decide browser-shutdown session preservation behavior.

## Phase 8: scrollback and follow-ups

- [ ] Define whether lossless styled historical scrollback must survive frontend reattachment.
- [ ] Document the landed protocol's current replay and plain-text scrollback behavior.
- [ ] If required, design or extract a focused server-paged styled scrollback API.
- [ ] Do not land all of cmux PR #8031 solely for scrollback.
- [ ] Re-evaluate stable split IDs and cmux PR #8023 only if TUI-owned browser layout becomes a separate goal.

## Definition of done

- cmux TUI owns every browser terminal's process, PTY, and authoritative terminal state.
- cmux-browser renders and accepts input through a manual-mode Ghostty surface built from the same fork revision.
- Destroying or recreating the frontend view does not terminate or corrupt the terminal session.
- Resize/replay is generation-safe and free of duplicate terminal replies.
- macOS integration is validated, with the same backend contract working on Linux and Windows.
- Remaining scrollback or full-tree limitations are explicitly documented and separated from the terminal-backend integration.
