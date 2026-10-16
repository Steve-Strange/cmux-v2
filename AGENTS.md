# cmux-browser agent instructions

## Superrepo worktrees and remote landing

Start future cmux-browser tasks in
`/Users/cmux-lawrence/fun/cmux-browser-hq`. It is the control repository for
worktree creation, fleet builds, and the operating rules for parallel tasks.
Read its `AGENTS.md` before creating a worktree or requesting a build.

If the task already has an owned worktree, start directly in
`/Users/cmux-lawrence/fun/cmux-browser-worktrees/<task>`. Otherwise create one
from the latest reviewed base:

```bash
cd /Users/cmux-lawrence/fun/cmux-browser-hq
git -C /Users/cmux-lawrence/fun/cmux-browser fetch origin
./scripts/new-feature-worktree.sh <task-slug> origin/main
cd /Users/cmux-lawrence/fun/cmux-browser-worktrees/<task-slug>
```

- `/Users/cmux-lawrence/fun/cmux-browser` is only the shared control checkout
  and Git worktree registry. Do not make task edits or perform landing work
  there. Its dirty state may belong to several concurrent or older tasks and
  is never evidence that the current task cannot land.
- All implementation, tests, commits, branch updates, and conflict resolution
  happen in the owned task worktree.
- If a mutating task starts in the shared checkout, inspect it but do not
  stash, reset, clean, switch, commit, or bulk-copy its files. Create an owned
  worktree, identify the exact task-owned diff, and reproduce only that diff
  there. Preserve all unrelated changes.

When the user authorizes landing—including “merge,” “land,” “get it into
main,” “use your best judgment,” or “do it safely”—carry the owned change
through the normal task-branch, test, review, push, pull-request, required
checks, and remote-merge workflow:

- Never push directly to, force-push, or rewrite `main`; never bypass required
  gates. Landing authorization does not authorize unrelated cleanup, branch
  deletion, or broadening the change.
- `origin/main` and the pull request are the integration authority. The local
  `main` checkout does not need to be clean, current, or checked out.
- If `origin/main` advances, update the task branch in its own worktree and
  rerun proportionate checks. Preserve both sides of overlapping conflicts and
  resolve them deliberately there.
- After merging, verify that GitHub reports the PR as merged and that the
  reviewed head is reachable from a freshly fetched `origin/main`.

Do not stop with "I did not merge because the main worktree has uncommitted
changes" when the task worktree and branch are ready. A dirty shared control
checkout is not a blocker. Real blockers include unresolved ownership inside
the task worktree, a conflict requiring a product decision, a required gate
that remains failed after in-scope fixes are exhausted, unavailable GitHub
credentials or permissions, or unavailable external infrastructure. Cleanup
of an owned worktree is optional follow-up and must never be required for
landing.

## Fleet builds are mandatory

- Every macOS Chromium sync, apply, build, and deploy must run through
  `/Users/cmux-lawrence/fun/cmux-browser-hq/bin/hq run`. Read the HQ
  repository's `AGENTS.md` before starting a fleet operation.
- Let HQ select the worker. Do not pass `--host` for an ordinary build. HQ
  balances recent builder time across the reviewed pool of company-owned
  48 GB Mac minis and records rotating performance/error telemetry.
- `aws-m4pro-*` is retired for cmux-browser builds. Do not ping it, wait for it,
  use another AWS M4 as a fallback, or report an offline AWS host as a build
  blocker. Do not use personal machines, `cmux-mac-mini`, or Austin-owned
  machines for Chromium builds.
- Do not invoke `scripts/sync.sh`, `apply.sh`, `build.sh`, `deploy.sh`, or
  `build-dogfood.sh` directly unless they are inside the command held by one
  `hq run` lease. Their historical standalone defaults are not fleet
  discovery.
- A real capacity blocker exists only when `hq fleet-status` shows no eligible
  reviewed worker and `hq run` exits with the documented no-capacity status.
  Include both outputs when reporting that blocker.
- Set `CMUX_AGENT_ID` or pass `--agent`, use a descriptive `--job`, and keep
  `hq monitor-build` attached for long builds. On failure, inspect the durable
  record with `hq logs show JOB_ID`; do not start a duplicate compile.
- Preserve live dogfood processes unless the user asks to close them. Build and
  deploy a uniquely tagged app instead of overwriting or relaunching their
  bundle.

Canonical shape:

```sh
HQ=/Users/cmux-lawrence/fun/cmux-browser-hq/bin/hq
CMUX_AGENT_ID=my-task "$HQ" monitor-build --label my-task -- \
  "$HQ" run --repo "$PWD" --sync --job my-task --agent my-task -- \
  bash -c 'set -e; ./scripts/build.sh; \
    DEST="$HOME/Applications/cmux-browser-my-task.app" \
    CMUX_TAG=my-task ./scripts/deploy.sh'
```

## Definition of done for shipped app changes

- Changes that affect the shipped browser app are not complete after source or
  asset validation alone. Unless the user explicitly asks not to build, run
  the repository's sync, apply, build, and deploy workflow through HQ.
- Verify the resulting packaged app, including the exact resource or behavior
  changed. For app-icon changes, checking `app.icns` is insufficient because
  `CFBundleIconName` can make macOS prefer `Assets.car`. Verify the icon macOS
  resolves for the deployed bundle and visually inspect the live Dock or Finder
  icon after launching the rebuilt app.
- Preserve unrelated worktree changes and do not reset this repository's
  files.
- Report the selected worker, released lease, build tag, durable telemetry ID,
  and absolute packaged `.app` path in the handoff.
