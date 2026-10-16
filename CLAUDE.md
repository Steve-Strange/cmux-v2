# cmux-browser — agent guide

A fork of Chromium that renders a niri-style scrollable strip of mixed panes (real Chrome web pages with a real `LocationBarView` omnibox + live Ghostty terminals) in ONE window. All custom UI is Chromium Views (C++/Obj-C++) under `overlay/chrome/browser/cmux_term/`. Read `STATUS.md` for the running history.

## Authoritative workflow

Read [`AGENTS.md`](AGENTS.md) before editing, building, committing, or landing
changes. Its superrepo, worktree, fleet, and remote-landing rules override any
conflicting historical detail in this file. In particular, a dirty shared
`cmux-browser` control checkout never blocks a clean task branch: work in the
task worktree and, when the user authorized landing, merge its PR remotely
without checking out or modifying local `main`.

## ⛔ NEVER rely on OWL — it is a failed/abandoned direction

cmux-browser is its **own** project with its **own clean Chromium base**. OWL (owl-wire / OwlFresh / owl_fresh_host) is a SEPARATE, abandoned direction. Do **not**:
- copy, depend on, or "reuse" any OWL code (`owl_wire/`, `content/shell/browser/owl_*`, OWL-modified files like the OWL versions of `shell_devtools_bindings.cc`, `render_widget_host_impl.cc`, `web_contents_view_mac.mm`, etc.);
- build cmux on top of OWL's branch/commits.

When you need a Chromium reference implementation (e.g. content_shell's DevTools bindings), use the **pristine upstream** version (pre-OWL: at or below commit `0bd9366db7`), never the OWL-patched copy in a shared tree. Historical note: the original builder tree (`cmux-aws-mac:~/chromium/src`, branch `cmux-browser`) was OWL's dev branch with OWL's uncommitted content-layer changes — cmux was inadvertently built on it. We are moving cmux to its own clean checkout at `~/cmux-chromium/src` (off pristine `0bd9366db7`). See memory `cmux-clean-chromium-base` and [[builder-host]].

## Build / deploy

Follow [`AGENTS.md`](AGENTS.md). Work on a unique task branch in a linked
worktree, and run every macOS Chromium sync/apply/build/deploy inside one
unpinned HQ lease. Let HQ select a reviewed worker; retired AWS builders and
standalone script defaults are not fleet discovery.

`scripts/deploy.sh` pulls the compiled `out/Release/cmux.app`, verifies its
bundle/executable/framework/helper identity and platform metadata, signs it
inside-out, and verifies the final bundle. Do not manually copy or rename the
app or bypass that verifier. Use a unique `DEST` and `CMUX_TAG` so live dogfood
sessions remain untouched.

## Landing changes

Follow the safe landing policy in [`AGENTS.md`](AGENTS.md). Never push directly
to or rewrite `main`. When the user has authorized landing (including “use your
best judgment,” “do it safely,” or “get it into main”), complete the normal
task-branch, test, review, HQ-build, push, and PR/fast-forward integration
without asking for a redundant confirmation. Otherwise ask before creating an
external change.
