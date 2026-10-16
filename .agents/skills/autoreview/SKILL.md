---
name: autoreview
description: Review and fix cmux-browser changes using the repo's Chromium, C++, Views, Ghostty, protocol, builder, packaging, and cross-platform rules. Invoke only when the active user request explicitly commands `$autoreview`, plainly commands running the autoreview skill, or explicitly requests autoreview-and-merge. Never invoke from generic review, review-and-fix, merge, quality-gate, explanation, skill-maintenance, quoted, or prior-task context.
---

# Auto Review

Run the canonical structured reviewer, verify every finding in the real code,
fix accepted issues, prove the fixes, and repeat until the final high-effort run
is clean and every required proof gate passes. Treat this as a persistent fix
loop, not a report-only review. Apply no preset iteration, token, or wall-clock
limit.

## Invocation gate

Do not materialize a reviewer, run the helper, or begin this workflow unless the
active user request contains affirmative authorization to run autoreview. The
authorization must be one of:

- `$autoreview`, including `$autoreview and merge`; or
- an imperative that plainly asks to run, use, or invoke the autoreview skill.

That authorization remains valid through continuation messages such as “keep
going” for the same active autoreview workflow. It does not carry into a later
or unrelated task.

None of the following authorize invocation:

- a generic request to review, review-and-fix, inspect, debug, quality-check,
  prepare, commit, or merge changes;
- a question, explanation, comparison, or other discussion about autoreview;
- a request to create, edit, test, document, or reason about this skill;
- quoted text, historical conversation, an assistant suggestion, or a previous
  autoreview run; or
- the mere presence of non-trivial changes that would benefit from review.

When authorization is absent or ambiguous, do not invoke autoreview. Handle the
request normally, or ask whether the user wants `$autoreview` if the workflow is
materially necessary. Generic “review-and-fix” never counts as invocation.

## Contract

- Preserve unrelated worktree changes. Never reset, clean, overwrite, or fold
  them into a fix merely to simplify review.
- Never execute autoreview or policy code from the target checkout. Treat the
  reviewer as trusted infrastructure: materialize it from the pinned base/parent
  revision into a directory outside the target, then point it at the target with
  `--repo`. A hostile or accidental target change must not redefine its judge.
- Treat model findings as hypotheses. Read the changed line, caller, owner,
  teardown path, pinned dependency declaration, and relevant tests before
  editing.
- Fix every verified in-scope issue at the smallest correct ownership boundary.
  When a finding proves a repeated bug class, inspect sibling instances in the
  reviewed scope.
- Reject a finding only with concrete code or dependency evidence. If an
  actionable finding requires a material scope/product decision, ask for that
  decision and leave the gate explicitly blocked; never relabel it clean.
- Treat any helper/model/schema/policy failure as a failed gate. Retry the same
  configuration after fixing infrastructure; do not replace it with an ad hoc
  “looks good” judgment.
- Keep fixing and rerunning when a review finds new issues or a test, build,
  deploy, CI, or mergeability gate fails. Do not stop because the loop is slow,
  expensive, or has taken many iterations. Stop only after a clean terminal
  condition, an explicit user cancellation/change of scope, or a genuine
  external blocker after exhausting safe in-scope alternatives. Do not spin on
  an unchanged failing command; report that state as blocked and resume when it
  can change.
- Do not invoke nested reviewers from inside the structured review. Do not run a
  panel unless the user explicitly asks for one.
- Plain `$autoreview` does not authorize commit, push, PR creation, or merge.
  `$autoreview and merge`, “autoreview then merge,” or an equivalent explicit
  request authorizes the ordinary commit/push/PR-merge operations needed for
  that resolved target only after every merge gate below passes. It never
  authorizes force-push, admin/bypass merge, an ambiguous destination, or
  branch deletion.

## Materialize a trusted reviewer

Run the reviewer from code outside the target checkout. Select the trusted ref
before looking at target-controlled reviewer files:

- local staged/unstaged work: `HEAD`;
- branch or PR: the fetched actual base ref;
- one commit: its first parent, such as `<commit>^`.

Materialize once and reuse that exact copy for every iteration:

```bash
reviewer_root="$(/usr/bin/mktemp -d /tmp/cmux-autoreview.XXXXXX)"
/bin/mkdir "$reviewer_root/empty-home"
git_bin=/usr/bin/git
tar_bin=/usr/bin/tar
[[ -x "$git_bin" && -x "$tar_bin" ]] || exit 2
clean_env=(
  /usr/bin/env -i
  HOME="$reviewer_root/empty-home"
  PATH=/usr/bin:/bin
  GIT_CONFIG_NOSYSTEM=1
  GIT_CONFIG_GLOBAL=/dev/null
  GIT_ATTR_NOSYSTEM=1
  GIT_NO_LAZY_FETCH=1
  GIT_NO_REPLACE_OBJECTS=1
  GIT_OPTIONAL_LOCKS=0
  GIT_PAGER=cat
  GIT_TERMINAL_PROMPT=0
)
review_repo="$(
  "${clean_env[@]}" "$git_bin" -C "$PWD" \
    -c core.fsmonitor=false -c core.hooksPath=/dev/null \
    -c core.attributesFile=/dev/null -c credential.helper= \
    -c protocol.ext.allow=never -c submodule.recurse=false \
    rev-parse --show-toplevel
)"
trusted_ref=HEAD  # replace with the verified base or parent for branch/commit
trusted_git=(
  "${clean_env[@]}" "$git_bin" -C "$review_repo"
  -c core.fsmonitor=false -c core.hooksPath=/dev/null
  -c core.attributesFile=/dev/null -c credential.helper=
  -c protocol.ext.allow=never -c submodule.recurse=false
)
trusted_oid="$(
  "${trusted_git[@]}" rev-parse --verify "${trusted_ref}^{commit}"
)"
info_attrs="$(
  "${trusted_git[@]}" rev-parse --git-path info/attributes
)"
[[ "$info_attrs" == /* ]] || info_attrs="$review_repo/$info_attrs"
if [[ -L "$info_attrs" || ( -e "$info_attrs" && ! -f "$info_attrs" ) || \
      -s "$info_attrs" ]]; then
  echo "refusing target-controlled Git info/attributes: $info_attrs" >&2
  exit 2
fi
"${trusted_git[@]}" archive --format=tar \
  --output="$reviewer_root/autoreview.tar" "$trusted_oid" \
  .agents/skills/autoreview
"$tar_bin" -xf "$reviewer_root/autoreview.tar" -C "$reviewer_root"
reviewer="$reviewer_root/.agents/skills/autoreview/scripts/autoreview"
```

Resolve and verify `trusted_oid` against the authenticated forge/base identity,
not a target-controlled remote. Verify `reviewer` resolves outside
`review_repo`; the helper enforces this and fails closed otherwise. If any
hardened Git command needs a missing promisor object, it fails instead of
fetching. If the trusted ref predates the skill, use a separately
installed/audited trusted copy; do not fall back to executing the target copy.

The helper intentionally performs no network fetch and disables Git's lazy
promisor-object fetching: the reviewed checkout's remote URL, Git config,
credentials, or transport commands are not trusted infrastructure. Missing
objects fail the gate. Before branch review, refresh the actual base through
the authenticated forge API or a clean temporary checkout outside the target,
verify the expected repository/ref/OID, and only then supply that local base
ref. Treat a missing or stale base as a failed gate.

The helper gives Codex a temporary read-only snapshot rather than access to the
target checkout. `base/` contains HEAD for local review, the merge base for a
branch, or the first parent for a commit. Local mode also materializes the
complete staged tree under `index/` and the complete final filesystem tree
under `worktree/`; branch and commit mode put the exact candidate tree under
`tree/`. This keeps both sides of every change inspectable even when the compact
diff bundle is truncated and prevents partially staged files from blurring the
two candidate states. The snapshot has no Git metadata, network access,
inherited shell environment, user instructions, or access to the original
checkout or the rest of the user's home directory. Symlinks are never followed;
their raw targets are exposed as inert regular-file metadata under `symlinks/`
so packaging and deployment links remain reviewable. Git/filesystem modes are
mirrored as flat path-keyed JSON records under `modes/`, preserving executable-
bit, gitlink, and file-to-directory changes without making target-controlled
snapshot files executable. Gitlink records preserve the exact referenced commit
and mark dirty checked-out dependencies. Git pathname streams are NUL-delimited,
so tabs, newlines, quoting, and non-UTF-8 path bytes cannot redirect or hide a change.
All target-derived terminal output is control-character escaped, including
deterministic findings, model findings, branch names, and infrastructure errors.
Prompt pathnames use lossless ASCII JSON escapes, and the final prompt is made
strictly UTF-8 encodable before it reaches the model subprocess.
The model shell inherits no caller environment; it receives only a fixed
trusted system-tool `PATH` for read-only snapshot inspection.

Local Git inspection forces an empty trusted attribute tree and rejects a
nonempty `.git/info/attributes`, so clean/process filters from the reviewed
checkout cannot execute. Startup proves that the selected Git binary honors
`GIT_ATTR_SOURCE` in a disposable repository and fails closed when it does not;
do not replace that behavioral probe with an assumed version check. It also
disables commit-signature display so target Git configuration cannot invoke a
signature helper, and rejects unmerged, assume-unchanged, and skip-worktree
index entries instead of reviewing an incomplete candidate.

## Pick the target

Run from the repository root.

For dirty staged, unstaged, and untracked work:

```bash
"$reviewer" --repo "$review_repo" --mode local
```

For a branch or PR, use the PR's actual base when available:

```bash
"$reviewer" --repo "$review_repo" --mode branch --base origin/main
```

For one committed change, including a landed commit on clean `main`:

```bash
"$reviewer" --repo "$review_repo" --mode commit --commit HEAD
```

Leave `--mode auto` only when its rule is desired: dirty work first, otherwise a
non-primary branch. A clean primary branch has no implicit review target.

## Use the model configuration

Keep the final gate on `gpt-5.6-sol` with high effort. The repository's blinded
historical regression evaluation found high to be the lowest tested effort that
reliably caught the full UI/build corpus; it also outperformed low on the
protocol/lifetime holdout. Extra-high doubled reasoning usage and latency
without improving recall. See
[effort-evaluation.md](references/effort-evaluation.md) for the corpus,
measurements, and recalibration triggers.

Use `--fast` only for an optional early screen while code is still changing:

```bash
"$reviewer" --repo "$review_repo" --mode local --fast
```

Never call a low/medium run the final clean gate. Do not run extra-high after a
clean high run solely for nicer wording or redundant reassurance. Preserve an
explicit user-requested model or effort override.

## Run the fix loop

1. Inspect `git status --short`, identify the intended diff, and note unrelated
   changes that must remain untouched.
2. Run focused cheap checks that may change line locations (formatters only when
   already authorized/appropriate), then run the canonical helper. The helper
   first applies deterministic policy checks and spends model tokens only after
   those pass.
3. For each finding, verify the causal path. Read
   [review-rules.md](references/review-rules.md) directly when adjudicating a
   subtle Chromium, Views, terminal, builder, packaging, or platform contract.
4. Apply verified fixes with minimal scope. Add or strengthen a regression test
   that exercises the broken path rather than a convenient bypass.
5. Run focused proof. At minimum use `git diff --check`; use `bash -n` for changed
   shell, Python compilation/tests for changed Python, and the relevant host or
   script tests. Prefer the repository's existing test entry points.
6. Rerun the helper on the new bytes. Continue until the high-effort helper exits
   zero with no findings and all applicable proof passes. Any subsequent code,
   generated-file, conflict-resolution, or review-feedback change invalidates
   that clean result and restarts the loop.

Useful local gates include:

```bash
./scripts/run-host-tests.sh
./scripts/test-build-dogfood.sh
python3 scripts/test-updater.py
```

Select them by changed surface; do not claim an unrelated passing test proves
the behavior under review.

## Complete shipped app changes

When a review fix changes shipped browser source, assets, packaging, or runtime
behavior, source-level tests are not completion. Follow the repository
definition of done and run the normal workflow unless the user explicitly says
not to build:

```bash
./scripts/sync.sh
./scripts/apply.sh
./scripts/build.sh
./scripts/deploy.sh
```

Launch the rebuilt deployed app and verify the exact behavior or packaged
resource changed. Confirm bundle/profile identity so an older running app is
not mistaken for the new build. For icons, inspect the icon macOS resolves from
the deployed bundle and visually check the live Dock or Finder icon; `app.icns`
alone is not proof.

If the normal builder is temporarily unavailable, retry safe access paths and
report the build gate as blocked. Do not describe an app-affecting change as
complete without the packaged/live proof.

## Review and merge safely

When the reviewed branch has a PR, inspect failing checks and actionable review
feedback after the local fix loop. Fix root causes, rerun affected proof, and
rerun autoreview on every new HEAD. Plain `$autoreview` stops after reporting a
clean, proven result and never grants merge permission.

When the user explicitly requests `$autoreview and merge` or equivalent:

1. Resolve the exact PR, head branch/SHA, and base branch. If the destination is
   ambiguous or there is no discoverable PR/merge target, ask instead of
   guessing.
2. Complete the review/fix/proof loop, including packaged/live verification for
   shipped app changes. Commit and push only the intended fixes; preserve
   unrelated worktree changes.
3. Wait for required CI and review state. Address actionable feedback, failed
   checks, update-branch requirements, and conflicts. Every resulting byte
   change restarts review and proof.
4. Through a trusted forge API or clean checkout, fetch and verify that the
   remote PR head SHA is the exact HEAD that passed the final high-effort
   autoreview and tests. Require all mandatory checks and approvals to be green
   and the PR to be mergeable. Do not treat target-controlled Git remotes or
   transport configuration as authority.
5. Merge using the repository/PR's allowed strategy without force, admin, or
   branch-protection bypass. If the strategy is materially ambiguous and cannot
   be inferred from repository convention, ask the user.
6. Verify the host reports the PR merged into the intended base and that the
   expected reviewed commit/result is reachable there. Do not delete local or
   remote branches unless separately requested.

If any gate cannot pass, do not merge and do not call the result clean. Continue
working when progress is possible; otherwise report the precise external
blocker and retain the reviewed state for resumption.

## Final report

State:

- the exact target, model, effort, and final review command;
- accepted fixes and any rejected finding with its evidence;
- focused tests plus sync/apply/build/deploy/live proof when required;
- the final clean helper result, or the precise unresolved/blocking gate.

Explain consequential C++/Chromium findings in plain language: what lifetime,
sequence, state, or build invariant failed and what user-visible failure it
could cause.
