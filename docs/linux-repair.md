# Linux repair patch set

This is a community repair of the published cmux Browser corresponding source.
It is not an official release. The original source manifest and licenses remain
unchanged.

## Source identity and scope

- Published binary: `151.0.7922.64`.
- Browser source: `dd7984e7ea68ee42768feebe3691b7b7ffdca6e6`.
- Pinned Chromium and locally built version: `151.0.7922.34`.
- cmux-tui: `f6085bbb9259767f94ed4890b0eb8df609209936`.
- Ghostty: `50ad1963d9c73ee957932ccb4d26bf6d15575ee7`.
- Verified desktop: Ubuntu 24.04.3, KDE/X11, RTX 5090, 1.75 device scale.

The local Git import is not the original private upstream Git commit. Source
comes from the release archive linked by `manaflow-ai/cmux-v2`; the backend
binary is reused at the pinned revision. Related public reports are
[manaflow-ai/cmux-v2#15](https://github.com/manaflow-ai/cmux-v2/issues/15) and
[#16](https://github.com/manaflow-ai/cmux-v2/issues/16).

## Resulting behavior

- Non-macOS frontends no longer attempt the permanently unsupported direct
  renderer every 200 ms. Mirror replay resets existing VT state in place.
- EGL displays are retained until the final host releases them; unloading GL
  clears its tracked handle first. Browser/window/extension teardown releases
  references before the referenced objects disappear. Closed surfaces are freed.
- Ghostty content scale follows the compositor; the framebuffer uses physical
  pixels. At 1.75 scale, a 426x398 logical pane produced a 746x697 framebuffer.
- Clipboard paste, X11 PRIMARY and writes are connected to Chromium, including
  asynchronous completion and Ghostty confirmation. Clipboard shortcuts retain
  printable key identity under Control.
- The terminal implements Chromium TextInputClient for composition, candidate
  position and committed text. Sogou commits are typing, not bracketed paste.
- Terminal Control chords reach the PTY. Ctrl+, opens or reuses settings.
  Appearance exposes sidebar/tab font size 10-22, terminal size 8-32, reset,
  and light/dark presets; changes apply immediately and persist.
- Initial canonical workspace projection uses an empty pane, without inserting
  an unwanted browser tab. Explicit web-tab/URL opening remains available.
- Linux update/install cleanup preserves an old install while a process is
  executing from it. A surviving daemon therefore retains a valid executable
  path for spawning new terminal hosts. There is no automatic garbage collector
  for directories retained this way.
- Locally marked builds skip official update checks and reject cached official
  installer handoff. Official packages without the marker keep their updater.

## Validation

These are recorded local results, not upstream CI or a claim about every driver.

| Check | Observed result |
| --- | --- |
| Host/model suite | 28 checks passed |
| Update-script regressions | 19 checks passed, including a real surviving executable |
| User installer | Passed live-executable preservation and subsequent unused-directory cleanup |
| Real Chromium EGL adapter | Mesa and system graphics each passed 30 cycles / 120 host creations and releases |
| Native desktop lifecycle, before the final input changes | Four terminals, 30 close/create/resize cycles, 300 seconds output; total 330.343 seconds; exit 0; zero unsupported-renderer retries |
| Final package input test | Qt/X11, Xvfb/Mesa, 1.75 scale; Chinese/emoji/multiline bracketed paste, PRIMARY, Control keys, clipboard write, settings reuse and persistence; 20.151 seconds; both GUI exits 0 |
| Final installed Sogou test | Native KDE/X11; Ctrl+Space activation, `nihao` + Space; PTY received exactly UTF-8 `e4bda0e5a5bd` (你好), without bracketed-paste wrappers; 38.045 seconds; both GUI exits 0 |
| GUI restart in final tests | Four existing terminal identities preserved, zero placeholder web pages |

GUI restart kept backend/PTY processes alive. **Cold boot, tmux resurrection,
SSH reconnection, Codex conversation resume, and AI completion notifications are
not implemented or validated by this patch set.** Wayland, Windows and macOS
build/runtime behavior are untested. Some changes touch shared source; maintainers
must run their platform builds before integration. Short local lifecycle tests
do not establish permanent stability. Existing final test evidence predates
this publication; publication reruns host/installer and patch-application checks.

## Reproduce

Follow the published source's build instructions and its pinned dependencies.
Do not substitute a different backend or Ghostty ABI. For a local Linux build,
`scripts/package-linux-local.py --build-root BUILD_ROOT --runtime PINNED_INSTALL`
packages verified build outputs and marks the result as a local build.

```sh
bash scripts/run-host-tests.sh
bash scripts/test-linux-user-installer.sh
python3 scripts/test-linux-terminal-lifecycle.py \
  --browser /absolute/path/to/chrome \
  --backend /absolute/path/to/cmux-tui \
  --output /absolute/path/to/new-isolated-run \
  --scale 1.75 --input-checks --hold-seconds 20
```

For real Sogou validation, use an existing X11 desktop with Sogou/Fcitx configured,
`--display :DISPLAY --toolkit native --native-rendering --ime-check`. The optional
`--ime-toggle-shift` is only appropriate when Sogou initially uses its internal
English mode. Use XTEST, not XSendEvent, for IME testing. The harness temporarily
selects Sogou and changes activation state; run it on a dedicated test desktop.

The GUI harness uses isolated browser/config/backend state and cleans only its
own test processes. It requires Xvfb, xdotool, ImageMagick, and (for a managed
desktop) wmctrl; clipboard checks require GTK Python bindings. The real EGL
adapter is a separate Chromium GN target, `cmux_ghostty_opengl_host_test`, and
must be compiled/run in the pinned Chromium tree.

Publish only synthetic-test summaries. User profiles, terminal captures,
workspace databases and credentials are not part of this contribution.
