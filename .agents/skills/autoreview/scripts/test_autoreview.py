#!/usr/bin/env python3
from __future__ import annotations

import contextlib
import io
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock


SCRIPT_DIR = Path(__file__).resolve().parent
SKILL_DIR = SCRIPT_DIR.parent
sys.path.insert(0, str(SCRIPT_DIR))
sys.dont_write_bytecode = True

import autoreview  # noqa: E402
import repo_policy_check as policy  # noqa: E402


class InvocationPolicyTests(unittest.TestCase):
    def test_manifest_disables_implicit_invocation(self) -> None:
        manifest = (SKILL_DIR / "agents" / "openai.yaml").read_text()
        self.assertIn(
            "policy:\n  allow_implicit_invocation: false\n",
            manifest,
        )
        self.assertEqual(manifest.count("allow_implicit_invocation:"), 1)
        self.assertNotIn("allow_implicit_invocation: true", manifest)

    def test_discovery_metadata_requires_explicit_autoreview_command(
        self,
    ) -> None:
        skill = (SKILL_DIR / "SKILL.md").read_text()
        frontmatter = skill.split("---", 2)[1]
        self.assertIn("active user request explicitly commands", frontmatter)
        self.assertIn("`$autoreview`", frontmatter)
        self.assertIn("Never invoke from generic review", frontmatter)
        self.assertNotIn(
            "explicitly asks for autoreview, review-and-fix",
            frontmatter,
        )

    def test_skill_body_rejects_non_invoking_mentions(self) -> None:
        skill = (SKILL_DIR / "SKILL.md").read_text()
        self.assertIn("## Invocation gate", skill)
        self.assertIn(
            "a question, explanation, comparison, or other discussion "
            "about autoreview",
            skill,
        )
        self.assertIn(
            "a request to create, edit, test, document, or reason about "
            "this skill",
            skill,
        )
        self.assertIn(
            "Generic “review-and-fix” never counts as invocation.",
            skill,
        )


def snapshot_metadata(snapshot: Path, tree: str, rel: str) -> dict[str, object]:
    path = autoreview._mode_metadata_path(snapshot / "modes" / tree, rel)
    record = json.loads(path.read_text())
    if record.get("path") != rel:
        raise AssertionError(f"mode record path mismatch: {record!r}")
    return record


def snapshot_mode(snapshot: Path, tree: str, rel: str) -> str:
    return str(snapshot_metadata(snapshot, tree, rel)["mode"])


class DiffParsingTests(unittest.TestCase):
    def test_changed_line_ranges_are_compact(self) -> None:
        lines = autoreview.LineRanges()
        lines.add_range(1, 10_000_000)
        self.assertEqual(lines.intervals, [(1, 10_000_000)])
        self.assertIn(9_999_999, lines)
        self.assertNotIn(10_000_001, lines)

    def test_untracked_preview_reads_only_bounded_prefix(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "large.bin"
            with path.open("wb") as handle:
                chunk = b"x" * 100_000
                for _ in range(100):
                    handle.write(chunk)
            preview = autoreview.read_untracked(path, 128)
        self.assertLess(len(preview), 256)
        self.assertIn("truncated", preview)

    def test_untracked_symlink_preview_includes_inert_target(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "link"
            path.symlink_to("../packaging/runtime")
            preview = autoreview.read_untracked(path)
        self.assertEqual(
            preview, '[symlink target: "../packaging/runtime"]'
        )

    def test_non_utf8_untracked_path_is_ascii_safe_in_prompt(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            repo = Path(directory)
            rel = "odd-\udcff.txt"
            with mock.patch.object(
                autoreview.BundleBuilder, "add_git"
            ), mock.patch.object(
                autoreview, "git_records", return_value=iter([rel])
            ):
                bundle = autoreview.local_bundle(repo, 40_000)
            prompt = autoreview.build_prompt(
                Path("/tmp/snapshot"),
                "local",
                None,
                bundle,
                "",
                "context-\udcfe",
            )

        self.assertIn("\\udcff", bundle)
        self.assertIn("\\udcfe", prompt)
        prompt.encode("utf-8", errors="strict")

    def test_added_lines_follow_new_line_numbers(self) -> None:
        diff = """\
diff --git a/example.cc b/example.cc
--- a/example.cc
+++ b/example.cc
@@ -4,2 +4,3 @@
 keep
-old
+new
+class MoreView : public views::View {
"""
        self.assertEqual(
            policy.parse_added_lines(diff),
            {"example.cc": {6: {"class MoreView : public views::View {"}}},
        )

    def test_added_lines_keep_cached_and_unstaged_versions(self) -> None:
        diff = """\
diff --git a/example.cc b/example.cc
--- a/example.cc
+++ b/example.cc
@@ -1 +1 @@
-class SafeView : public views::View {
+class UnsafeView : public views::View {
diff --git a/example.cc b/example.cc
--- a/example.cc
+++ b/example.cc
@@ -1 +1 @@
-class UnsafeView : public views::View {
+class SafeView : public views::View {
"""
        self.assertEqual(
            policy.parse_added_lines(diff),
            {
                "example.cc": {
                    1: {
                        "class SafeView : public views::View {",
                        "class UnsafeView : public views::View {",
                    }
                }
            },
        )

    def test_in_hunk_plus_plus_text_is_not_a_file_header(self) -> None:
        diff = """\
diff --git a/example.cc b/example.cc
--- a/example.cc
+++ b/example.cc
@@ -0,0 +1,2 @@
+++ pretend-header
+class UnsafeView : public views::View {
"""
        self.assertEqual(
            policy.parse_added_lines(diff),
            {
                "example.cc": {
                    2: {"class UnsafeView : public views::View {"}
                }
            },
        )
        self.assertEqual(
            autoreview.parse_changed_lines(diff),
            {"example.cc": {1, 2}},
        )

    def test_changed_lines_include_both_sides_of_replacement(self) -> None:
        diff = """\
diff --git a/example.cc b/example.cc
--- a/example.cc
+++ b/example.cc
@@ -4,2 +4,3 @@
 keep
-old
+new
+more
"""
        self.assertEqual(
            autoreview.parse_changed_lines(diff),
            {"example.cc": {5, 6}},
        )

    def test_git_quoted_diff_path_decodes_tabs_newlines_and_utf8(self) -> None:
        encoded = '"b/odd\\tline\\n\\303\\251.cc"'
        self.assertEqual(
            autoreview.normalize_diff_path(encoded),
            "odd\tline\né.cc",
        )

    def test_changed_paths_preserve_unusual_tracked_name(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            repo = Path(directory)

            def run_git(*args: str) -> None:
                subprocess.run(
                    ["git", *args],
                    cwd=repo,
                    check=True,
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                )

            run_git("init", "-q")
            run_git("config", "user.name", "Autoreview Test")
            run_git("config", "user.email", "autoreview@example.invalid")
            run_git("config", "color.ui", "always")
            rel = "odd\tname\nfile.cc"
            (repo / rel).write_text("old\n")
            run_git("add", rel)
            run_git("commit", "-qm", "fixture")
            (repo / rel).write_text("new\n")

            paths = autoreview.changed_paths(repo, "local", None, "HEAD")
            changed = autoreview.changed_line_map(
                repo, "local", None, "HEAD", paths
            )

        self.assertEqual(paths, {rel})
        self.assertIn(rel, changed)
        self.assertIn(1, changed[rel])

    def test_commit_diff_uses_first_parent(self) -> None:
        with mock.patch.object(
            autoreview, "first_parent", return_value="parent"
        ):
            commands = autoreview.target_diff_commands(
                Path("/repo"), "commit", None, "merge"
            )
        self.assertEqual(
            commands,
            [
                (
                    "diff",
                    "--unified=0",
                    "--find-renames",
                    "--no-ext-diff",
                    "parent",
                    "merge",
                )
            ],
        )

    def test_local_diff_keeps_cached_and_unstaged_hunks_separate(self) -> None:
        result = autoreview.target_diff_commands(
            Path("/repo"), "local", None, "HEAD"
        )
        self.assertEqual(
            result,
            [
                (
                    "diff",
                    "--cached",
                    "--unified=0",
                    "--find-renames",
                    "--no-ext-diff",
                ),
                (
                    "diff",
                    "--unified=0",
                    "--find-renames",
                    "--no-ext-diff",
                ),
            ],
        )

    def test_local_fingerprint_includes_untracked_contents(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            repo = Path(directory)
            source = repo / "new.cc"
            source.write_text("first\n")

            def fake_git(
                unused_repo: Path, *args: str, check: bool = True
            ) -> str:
                del unused_repo, check
                if args == ("rev-parse", "--verify", "HEAD"):
                    return "abc123\n"
                if args == (
                    "diff",
                    "--cached",
                    "--binary",
                    "--full-index",
                    "--no-ext-diff",
                    "HEAD",
                ):
                    return ""
                if args == (
                    "diff",
                    "--binary",
                    "--full-index",
                    "--no-ext-diff",
                ):
                    return ""
                if args == (
                    "ls-files",
                    "-z",
                    "--others",
                    "--exclude-standard",
                ):
                    return "new.cc\0"
                self.fail(f"unexpected git call: {args}")

            with mock.patch.object(
                autoreview, "git", side_effect=fake_git
            ), mock.patch.object(
                autoreview, "require_safe_git_attributes"
            ), mock.patch.object(
                autoreview, "require_resolved_index"
            ), mock.patch.object(
                autoreview, "_hash_git_command"
            ), mock.patch.object(
                autoreview,
                "git_records",
                side_effect=(iter(["new.cc"]), iter(["new.cc"])),
            ):
                before = autoreview.target_fingerprint(
                    repo, "local", None, "HEAD"
                )
                source.write_text("second\n")
                after = autoreview.target_fingerprint(
                    repo, "local", None, "HEAD"
                )
        self.assertNotEqual(before, after)


class GitSafetyTests(unittest.TestCase):
    def test_terminal_renderer_escapes_control_sequences(self) -> None:
        raw = "bad\x1b]8;;https://example.invalid\x07\nname\u202e"
        rendered = autoreview.safe_terminal_text(raw)
        self.assertNotIn("\x1b", rendered)
        self.assertNotIn("\x07", rendered)
        self.assertNotIn("\n", rendered)
        self.assertNotIn("\u202e", rendered)
        self.assertIn("\\x1b", rendered)
        self.assertIn("\\n", rendered)
        self.assertIn("\\u202e", rendered)

    def test_model_report_escapes_target_derived_terminal_controls(self) -> None:
        report = {
            "findings": [
                {
                    "priority": "P1",
                    "title": "unsafe\x1b[2Jtitle",
                    "body": "body\x07text",
                    "code_location": {
                        "file_path": "odd\x1b]0;forged\x07.cc",
                        "line": 1,
                    },
                }
            ],
            "overall_correctness": "patch is incorrect",
            "overall_confidence": 0.99,
            "overall_explanation": "explanation\x1b[H",
        }
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            autoreview.print_report(report, [])
        rendered = output.getvalue()
        self.assertNotIn("\x1b", rendered)
        self.assertNotIn("\x07", rendered)
        self.assertIn("\\x1b", rendered)

    def test_diff_invocation_disables_executable_filters(self) -> None:
        with mock.patch.object(
            autoreview, "command_path", return_value="/usr/bin/git"
        ):
            argv = autoreview.git_argv(Path("/repo"), "diff", "--stat")
            show_argv = autoreview.git_argv(Path("/repo"), "show", "HEAD")
        self.assertIn("core.fsmonitor=false", argv)
        self.assertIn("color.ui=false", argv)
        self.assertIn("core.fileMode=true", argv)
        self.assertIn(f"core.attributesFile={os.devnull}", argv)
        self.assertIn("core.quotePath=true", argv)
        self.assertIn("diff.mnemonicPrefix=false", argv)
        self.assertIn("diff.ignoreSubmodules=none", argv)
        self.assertIn("diff.noprefix=false", argv)
        self.assertIn("log.showSignature=false", argv)
        self.assertIn("credential.helper=", argv)
        self.assertIn("protocol.ext.allow=never", argv)
        self.assertIn("--no-ext-diff", argv)
        self.assertIn("--no-color", argv)
        self.assertIn("--no-textconv", argv)
        self.assertIn("--text", argv)
        self.assertIn("--ignore-submodules=none", argv)
        self.assertIn("--submodule=short", argv)
        self.assertIn("--no-show-signature", show_argv)

    def test_gitlink_change_survives_repo_suppression_and_snapshotting(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            repo = Path(directory)

            def run_git(*args: str) -> str:
                completed = subprocess.run(
                    ["git", *args],
                    cwd=repo,
                    check=True,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.DEVNULL,
                    text=True,
                )
                return completed.stdout.strip()

            run_git("init", "-q")
            run_git("config", "user.name", "Autoreview Test")
            run_git("config", "user.email", "autoreview@example.invalid")
            (repo / "README").write_text("first\n")
            run_git("add", "README")
            run_git("commit", "-qm", "first dependency commit")
            old_oid = run_git("rev-parse", "HEAD")
            (repo / "README").write_text("second\n")
            run_git("commit", "-qam", "second dependency commit")
            new_oid = run_git("rev-parse", "HEAD")

            run_git(
                "update-index",
                "--add",
                "--cacheinfo",
                f"160000,{old_oid},vendor/dependency",
            )
            run_git("commit", "-qm", "add dependency pointer")
            run_git("config", "diff.ignoreSubmodules", "all")
            run_git(
                "update-index",
                "--cacheinfo",
                f"160000,{new_oid},vendor/dependency",
            )

            paths = autoreview.changed_paths(repo, "local", None, "HEAD")
            with autoreview.materialized_review_snapshot(
                repo, "local", None, "HEAD"
            ) as snapshot:
                base = snapshot_metadata(
                    snapshot, "base", "vendor/dependency"
                )
                index = snapshot_metadata(
                    snapshot, "index", "vendor/dependency"
                )

            dependency = repo / "vendor/dependency"
            dependency.mkdir(parents=True)

            def run_dependency_git(*args: str) -> str:
                completed = subprocess.run(
                    ["git", *args],
                    cwd=dependency,
                    check=True,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.DEVNULL,
                    text=True,
                )
                return completed.stdout.strip()

            run_dependency_git("init", "-q")
            run_dependency_git("config", "user.name", "Autoreview Test")
            run_dependency_git(
                "config", "user.email", "autoreview@example.invalid"
            )
            (dependency / "dependency.txt").write_text("committed\n")
            run_dependency_git("add", "dependency.txt")
            run_dependency_git("commit", "-qm", "checked-out dependency")
            worktree_oid = run_dependency_git("rev-parse", "HEAD")
            (dependency / "dependency.txt").write_text("dirty\n")

            with autoreview.materialized_review_snapshot(
                repo, "local", None, "HEAD"
            ) as snapshot:
                worktree = snapshot_metadata(
                    snapshot, "worktree", "vendor/dependency"
                )

        self.assertIn("vendor/dependency", paths)
        self.assertEqual(base, {
            "path": "vendor/dependency",
            "mode": "160000",
            "object": old_oid,
        })
        self.assertEqual(index, {
            "path": "vendor/dependency",
            "mode": "160000",
            "object": new_oid,
        })
        self.assertEqual(worktree, {
            "path": "vendor/dependency",
            "mode": "160000",
            "object": worktree_oid,
            "dirty": True,
        })

    def test_target_binary_attribute_cannot_hide_text_hunks(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            repo = Path(directory)

            def run_git(*args: str) -> None:
                subprocess.run(
                    ["git", *args],
                    cwd=repo,
                    check=True,
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                )

            run_git("init", "-q")
            run_git("config", "user.name", "Autoreview Test")
            run_git("config", "user.email", "autoreview@example.invalid")
            run_git("config", "diff.hidden.binary", "true")
            (repo / ".gitattributes").write_text("*.cc diff=hidden\n")
            (repo / "example.cc").write_text("old bytes\n")
            run_git("add", ".gitattributes", "example.cc")
            run_git("commit", "-qm", "fixture")
            (repo / "example.cc").write_text("new bytes\n")

            rendered = autoreview.git(repo, "diff", "--patch")

        self.assertIn("-old bytes", rendered)
        self.assertIn("+new bytes", rendered)

    def test_target_clean_filter_cannot_execute_during_diff(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            repo = Path(directory)
            marker = repo / "filter-executed"

            def run_git(*args: str) -> None:
                subprocess.run(
                    ["git", *args],
                    cwd=repo,
                    check=True,
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                )

            run_git("init", "-q")
            run_git("config", "user.name", "Autoreview Test")
            run_git("config", "user.email", "autoreview@example.invalid")
            (repo / ".gitattributes").write_text("*.cc filter=evil\n")
            (repo / "example.cc").write_text("old bytes\n")
            run_git("add", ".gitattributes", "example.cc")
            run_git("commit", "-qm", "fixture")
            run_git("config", "filter.evil.clean", f"touch {marker}; cat")
            (repo / "example.cc").write_text("new bytes\n")

            rendered = autoreview.git(repo, "diff", "--patch")
            self.assertFalse(marker.exists())

        self.assertIn("+new bytes", rendered)

    def test_assume_unchanged_index_entry_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            repo = Path(directory)

            def run_git(*args: str) -> None:
                subprocess.run(
                    ["git", *args],
                    cwd=repo,
                    check=True,
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                )

            run_git("init", "-q")
            run_git("config", "user.name", "Autoreview Test")
            run_git("config", "user.email", "autoreview@example.invalid")
            (repo / "hidden.cc").write_text("tracked\n")
            run_git("add", "hidden.cc")
            run_git("commit", "-qm", "fixture")
            run_git("update-index", "--assume-unchanged", "hidden.cc")

            with self.assertRaisesRegex(
                autoreview.InfrastructureError, "hidden index entry"
            ):
                autoreview.require_resolved_index(repo)

    def test_trusted_attribute_tree_does_not_depend_on_target_objects(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            repo = Path(directory)
            subprocess.run(
                ["git", "init", "-q"],
                cwd=repo,
                check=True,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
            target_object = (
                repo
                / ".git/objects"
                / autoreview.TRUSTED_EMPTY_ATTR_TREE[:2]
                / autoreview.TRUSTED_EMPTY_ATTR_TREE[2:]
            )
            self.assertFalse(target_object.exists())
            autoreview.require_safe_git_attributes(repo)

    def test_missing_git_attr_source_capability_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            repo = Path(directory)
            subprocess.run(
                ["git", "init", "-q"],
                cwd=repo,
                check=True,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
            unsupported_env = autoreview.git_environment()
            unsupported_env.pop("GIT_ATTR_SOURCE")
            with mock.patch.object(
                autoreview,
                "git_environment",
                return_value=unsupported_env,
            ):
                with self.assertRaisesRegex(
                    autoreview.InfrastructureError,
                    "does not honor GIT_ATTR_SOURCE",
                ):
                    autoreview.require_git_attr_source_support(repo)

    def test_git_environment_drops_inherited_execution_knobs(self) -> None:
        with mock.patch.dict(
            os.environ,
            {
                "GIT_EXTERNAL_DIFF": "/tmp/target-controlled",
                "GIT_SSH_COMMAND": "/tmp/target-controlled",
                "SSH_ASKPASS": "/tmp/target-controlled",
            },
        ):
            env = autoreview.git_environment()
        self.assertNotIn("GIT_EXTERNAL_DIFF", env)
        self.assertNotIn("GIT_SSH_COMMAND", env)
        self.assertNotIn("SSH_ASKPASS", env)
        self.assertEqual(env["GIT_CONFIG_GLOBAL"], os.devnull)
        self.assertEqual(env["GIT_CONFIG_NOSYSTEM"], "1")
        self.assertEqual(env["GIT_NO_LAZY_FETCH"], "1")
        self.assertTrue(Path(env["GIT_ALTERNATE_OBJECT_DIRECTORIES"]).is_dir())

    def test_branch_target_requires_explicit_trusted_base(self) -> None:
        with mock.patch.object(autoreview, "current_branch", return_value="feature"):
            with self.assertRaises(autoreview.InfrastructureError):
                autoreview.choose_target(Path("/repo"), "branch", None)

    def test_unmerged_index_fails_closed(self) -> None:
        with mock.patch.object(
            autoreview,
            "git_output_prefix",
            return_value=("1", False),
        ):
            with self.assertRaisesRegex(
                autoreview.InfrastructureError, "unmerged index"
            ):
                autoreview.require_resolved_index(Path("/repo"))

    def test_policy_checker_uses_isolated_python(self) -> None:
        completed = mock.Mock(returncode=0, stdout="", stderr="")
        with mock.patch.object(autoreview, "run", return_value=completed) as run:
            self.assertEqual(
                autoreview.run_policy(
                    Path("/repo"),
                    "local",
                    None,
                    "HEAD",
                    Path("/trusted/repo_policy_check.py"),
                ),
                0,
            )
        argv = run.call_args.args[0]
        self.assertEqual(argv[1:4], ["-I", "-S", "-B"])

    def test_json_output_must_be_new_and_outside_target(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            repo = root / "repo"
            repo.mkdir()
            with self.assertRaises(autoreview.InfrastructureError):
                autoreview.resolve_json_output(str(repo / "report.json"), repo)

            existing = root / "existing.json"
            existing.write_text("keep\n")
            with self.assertRaises(autoreview.InfrastructureError):
                autoreview.resolve_json_output(str(existing), repo)

            output = autoreview.resolve_json_output(
                str(root / "new-report.json"), repo
            )
            self.assertEqual(output, (root / "new-report.json").resolve())

    def test_review_snapshot_has_base_candidates_and_omits_symlinks(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            repo = Path(directory)

            def run_git(*args: str) -> None:
                subprocess.run(
                    ["git", *args],
                    cwd=repo,
                    check=True,
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                )

            run_git("init", "-q")
            run_git("config", "user.name", "Autoreview Test")
            run_git("config", "user.email", "autoreview@example.invalid")
            (repo / "README").write_text("base\n")
            run_git("add", "README")
            run_git("commit", "-qm", "fixture")
            (repo / "README").write_text("fixture\n")
            (repo / "outside-link").symlink_to("/etc/passwd")
            run_git("add", "README", "outside-link")
            (repo / "untracked-link").symlink_to("../packaging/runtime")
            (repo / "README").write_text("worktree\n")
            with autoreview.materialized_review_snapshot(
                repo, "local", None, "HEAD"
            ) as snapshot:
                self.assertTrue((snapshot / "base/README").is_file())
                self.assertTrue((snapshot / "index/README").is_file())
                self.assertTrue((snapshot / "worktree/README").is_file())
                self.assertEqual(
                    (snapshot / "base/README").read_text(), "base\n"
                )
                self.assertEqual(
                    (snapshot / "index/README").read_text(), "fixture\n"
                )
                self.assertEqual(
                    (snapshot / "worktree/README").read_text(), "worktree\n"
                )
                self.assertFalse((snapshot / "base/outside-link").exists())
                self.assertFalse((snapshot / "index/outside-link").exists())
                self.assertFalse((snapshot / "worktree/outside-link").exists())
                self.assertEqual(
                    (snapshot / "symlinks/index/outside-link").read_bytes(),
                    b"/etc/passwd",
                )
                self.assertEqual(
                    (snapshot / "symlinks/worktree/outside-link").read_bytes(),
                    b"/etc/passwd",
                )
                self.assertEqual(
                    (snapshot / "symlinks/worktree/untracked-link").read_bytes(),
                    b"../packaging/runtime",
                )
                self.assertEqual(snapshot_mode(snapshot, "base", "README"), "100644")
                self.assertEqual(
                    snapshot_mode(snapshot, "index", "outside-link"),
                    "120000",
                )
                self.assertEqual(
                    snapshot_mode(snapshot, "worktree", "untracked-link"),
                    "120000",
                )
                self.assertNotEqual(snapshot, repo)
                self.assertEqual(snapshot.parent, Path.home().resolve())

    def test_review_snapshot_preserves_executable_mode_metadata(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            repo = Path(directory)

            def run_git(*args: str) -> None:
                subprocess.run(
                    ["git", *args],
                    cwd=repo,
                    check=True,
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                )

            run_git("init", "-q")
            run_git("config", "user.name", "Autoreview Test")
            run_git("config", "user.email", "autoreview@example.invalid")
            script = repo / "script.sh"
            script.write_text("#!/bin/sh\n")
            script.chmod(0o644)
            run_git("add", "script.sh")
            run_git("commit", "-qm", "fixture")
            script.chmod(0o755)

            with autoreview.materialized_review_snapshot(
                repo, "local", None, "HEAD"
            ) as snapshot:
                self.assertEqual(
                    snapshot_mode(snapshot, "base", "script.sh"), "100644"
                )
                self.assertEqual(
                    snapshot_mode(snapshot, "index", "script.sh"), "100644"
                )
                self.assertEqual(
                    snapshot_mode(snapshot, "worktree", "script.sh"), "100755"
                )

    def test_mode_metadata_handles_file_to_directory_replacement(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            repo = Path(directory)

            def run_git(*args: str) -> None:
                subprocess.run(
                    ["git", *args],
                    cwd=repo,
                    check=True,
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                )

            run_git("init", "-q")
            run_git("config", "user.name", "Autoreview Test")
            run_git("config", "user.email", "autoreview@example.invalid")
            node = repo / "node"
            node.write_text("old file\n")
            run_git("add", "node")
            run_git("commit", "-qm", "fixture")
            node.unlink()
            node.mkdir()
            (node / "child.txt").write_text("new child\n")

            with autoreview.materialized_review_snapshot(
                repo, "local", None, "HEAD"
            ) as snapshot:
                self.assertTrue((snapshot / "worktree/node/child.txt").is_file())
                self.assertTrue(snapshot_mode(snapshot, "worktree", "node").startswith("filesystem:"))
                self.assertEqual(
                    snapshot_mode(snapshot, "worktree", "node/child.txt"),
                    "100644",
                )


class PolicyTests(unittest.TestCase):
    def test_irrelevant_large_asset_is_not_loaded_by_policy_scan(self) -> None:
        content_for = mock.Mock(
            side_effect=AssertionError("irrelevant asset content was read")
        )
        self.assertEqual(
            policy.scan_candidate_policy(
                {"assets/large-image.png"}, {}, content_for
            ),
            [],
        )
        content_for.assert_not_called()

    def test_worktree_restoration_is_created_but_not_retired(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            repo = Path(directory)

            def run_git(*args: str) -> None:
                subprocess.run(
                    ["git", *args],
                    cwd=repo,
                    check=True,
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                )

            run_git("init", "-q")
            run_git("config", "user.name", "Autoreview Test")
            run_git("config", "user.email", "autoreview@example.invalid")
            rel = "overlay/chrome/browser/cmux_term/restored.cc"
            source = repo / rel
            source.parent.mkdir(parents=True)
            source.write_text("old\n")
            run_git("add", rel)
            run_git("commit", "-qm", "fixture")
            run_git("rm", "-q", rel)
            source.parent.mkdir(parents=True, exist_ok=True)
            source.write_text("restored\n")

            created, retired = policy.source_path_changes(
                repo,
                "local",
                None,
                "HEAD",
                local_index=False,
            )

        self.assertIn(rel, created)
        self.assertNotIn(rel, retired)

    def test_staged_view_is_checked_against_index_not_unstaged_fix(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            repo = Path(directory)

            def run_git(*args: str) -> None:
                subprocess.run(
                    ["git", *args],
                    cwd=repo,
                    check=True,
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                )

            run_git("init", "-q")
            run_git("config", "user.name", "Autoreview Test")
            run_git("config", "user.email", "autoreview@example.invalid")
            (repo / "README").write_text("fixture\n")
            run_git("add", "README")
            run_git("commit", "-qm", "fixture")

            source = (
                repo
                / "overlay/chrome/browser/cmux_term/staged_view.h"
            )
            source.parent.mkdir(parents=True)
            source.write_text(
                "class ResizeHandle : public views::View {\n"
                " public:\n"
                "  ResizeHandle() = default;\n"
                "};\n"
                "auto handle = std::make_unique<ResizeHandle>();\n"
            )
            run_git("add", str(source.relative_to(repo)))
            source.write_text(
                "class ResizeHandle : public views::View {\n"
                " public:\n"
                "  METADATA_HEADER(ResizeHandle, views::View)\n"
                "};\n"
                "auto handle = std::make_unique<ResizeHandle>();\n"
                "BEGIN_METADATA(ResizeHandle)\n"
                "END_METADATA\n"
            )

            findings = policy.scan(repo, "local", None, "HEAD")
        self.assertTrue(
            any("ResizeHandle" in finding.title for finding in findings)
        )

    def test_runtime_blocking_is_a_finding(self) -> None:
        findings = policy.scan_runtime_blocking(
            "overlay/chrome/browser/cmux_term/config.cc",
            {42: {"base::ScopedAllowBlockingForTesting allow;"}},
        )
        self.assertEqual(len(findings), 1)
        self.assertEqual(findings[0].line, 42)

    def test_test_file_blocking_is_ignored(self) -> None:
        self.assertEqual(
            policy.scan_runtime_blocking(
                "overlay/chrome/browser/cmux_term/config_test.cc",
                {42: {"base::ScopedAllowBlockingForTesting allow;"}},
            ),
            [],
        )

    def test_instantiated_view_requires_metadata(self) -> None:
        content = """\
class ResizeHandle : public views::View {
 public:
  ResizeHandle() = default;
};
auto handle = std::make_unique<ResizeHandle>();
"""
        findings = policy.scan_views_metadata(
            "overlay/chrome/browser/cmux_term/view.cc",
            {1: {"class ResizeHandle : public views::View {"}},
            content,
        )
        self.assertEqual(len(findings), 1)

    def test_view_with_metadata_is_clean(self) -> None:
        content = """\
class ResizeHandle : public views::View {
  METADATA_HEADER(ResizeHandle, views::View)
};
auto handle = std::make_unique<ResizeHandle>();
BEGIN_METADATA(ResizeHandle)
END_METADATA
"""
        self.assertEqual(
            policy.scan_views_metadata(
                "overlay/chrome/browser/cmux_term/view.cc",
                {1: {"class ResizeHandle : public views::View {"}},
                content,
            ),
            [],
        )

    def test_view_header_without_definition_is_a_finding(self) -> None:
        content = """\
class ResizeHandle : public views::View {
  METADATA_HEADER(ResizeHandle, views::View)
};
auto handle = std::make_unique<ResizeHandle>();
"""
        findings = policy.scan_views_metadata(
            "overlay/chrome/browser/cmux_term/view.cc",
            {1: {"class ResizeHandle : public views::View {"}},
            content,
        )
        self.assertEqual(len(findings), 1)

    def test_view_construction_finds_metadata_missing_from_sibling_header(self) -> None:
        source = "auto handle = std::make_unique<ResizeHandle>();\n"
        contents = {
            "overlay/chrome/browser/cmux_term/resize_handle.cc": source,
            "overlay/chrome/browser/cmux_term/resize_handle.h": (
                "class ResizeHandle : public views::View {};\n"
            ),
        }
        findings = policy.scan_candidate_policy(
            {"overlay/chrome/browser/cmux_term/resize_handle.cc"},
            {
                "overlay/chrome/browser/cmux_term/resize_handle.cc": {
                    1: {source.strip()}
                }
            },
            lambda path: contents.get(path, ""),
        )
        self.assertEqual(len(findings), 1)
        self.assertIn("ResizeHandle", findings[0].title)

    def test_view_metadata_can_be_split_across_header_and_source(self) -> None:
        source = (
            "auto handle = std::make_unique<ResizeHandle>();\n"
            "BEGIN_METADATA(ResizeHandle)\n"
            "END_METADATA\n"
        )
        header = (
            "class ResizeHandle : public views::View {\n"
            "  METADATA_HEADER(ResizeHandle, views::View)\n"
            "};\n"
        )
        self.assertEqual(
            policy.scan_views_metadata(
                "overlay/chrome/browser/cmux_term/resize_handle.h",
                {1: {"class ResizeHandle : public views::View {"}},
                header,
                [source],
            ),
            [],
        )

    def test_build_pipeline_is_a_finding(self) -> None:
        content = "autoninja -C out/Release chrome 2>&1 | grep error | tail -8\n"
        self.assertEqual(len(policy.scan_build_status("scripts/build.sh", content)), 1)

    def test_build_pipeline_with_pipefail_is_clean(self) -> None:
        content = (
            "set -euo pipefail\n"
            "autoninja -C out/Release chrome 2>&1 | tee build.log\n"
        )
        self.assertEqual(policy.scan_build_status("scripts/build.sh", content), [])

    def test_pipefail_without_errexit_is_a_finding(self) -> None:
        content = (
            "set -o pipefail\n"
            "autoninja -C out/Release chrome 2>&1 | tee build.log\n"
            "echo done\n"
        )
        self.assertEqual(len(policy.scan_build_status("scripts/build.sh", content)), 1)

    def test_split_build_pipeline_is_a_finding(self) -> None:
        content = (
            "autoninja -C out/Release chrome 2>&1 \\\n"
            "  | grep error \\\n"
            "  | tail -8\n"
        )
        findings = policy.scan_build_status("scripts/build.sh", content)
        self.assertEqual(len(findings), 1)
        self.assertEqual(findings[0].line, 1)

    def test_split_build_pipeline_with_pipefail_is_clean(self) -> None:
        content = (
            "set -euo pipefail\n"
            "autoninja -C out/Release chrome 2>&1 \\\n"
            "  | tee build.log\n"
        )
        self.assertEqual(policy.scan_build_status("scripts/build.sh", content), [])

    def test_local_pipefail_does_not_protect_remote_pipeline(self) -> None:
        content = (
            "set -euo pipefail\n"
            "ssh builder \"autoninja -C out chrome | grep FAILED | tail -8; "
            "echo DONE\"\n"
        )
        self.assertEqual(len(policy.scan_build_status("scripts/build.sh", content)), 1)

    def test_long_quoted_ssh_command_remains_remote(self) -> None:
        setup = "\n".join(f"echo setup-{index}" for index in range(20))
        content = (
            "set -euo pipefail\n"
            "ssh builder '\n"
            f"{setup}\n"
            "autoninja -C out chrome | tail -8\n"
            "'\n"
        )
        findings = policy.scan_build_status("scripts/build.sh", content)
        self.assertEqual(len(findings), 1)
        self.assertEqual(findings[0].line, 2)

    def test_long_quoted_ssh_status_exit_is_clean(self) -> None:
        setup = "\n".join(f"echo setup-{index}" for index in range(20))
        content = (
            "set -euo pipefail\n"
            "ssh builder \"bash -lc '\n"
            f"{setup}\n"
            "autoninja -C out chrome | tee build.log; "
            "exit \\${PIPESTATUS[0]}\n"
            "'\"\n"
        )
        self.assertEqual(policy.scan_build_status("scripts/build.sh", content), [])

    def test_local_status_capture_does_not_protect_remote_pipeline(self) -> None:
        content = (
            'ssh builder "autoninja -C out chrome | tee build.log"\n'
            "build_rc=${PIPESTATUS[0]}\n"
            'exit "$build_rc"\n'
        )
        self.assertEqual(len(policy.scan_build_status("scripts/build.sh", content)), 1)

    def test_remote_double_quoted_status_is_expanded_locally(self) -> None:
        content = (
            'ssh builder "autoninja -C out chrome | tee build.log; '
            'exit ${PIPESTATUS[0]}"\n'
        )
        self.assertEqual(len(policy.scan_build_status("scripts/build.sh", content)), 1)

    def test_remote_status_without_explicit_shell_is_a_finding(self) -> None:
        content = (
            "ssh builder 'autoninja -C out chrome | tee build.log; "
            "exit ${PIPESTATUS[0]}'\n"
        )
        self.assertEqual(len(policy.scan_build_status("scripts/build.sh", content)), 1)

    def test_remote_escaped_status_exit_is_clean(self) -> None:
        content = (
            "ssh builder \"bash -lc 'autoninja -C out chrome | tee build.log; "
            "exit \\${PIPESTATUS[0]}'\"\n"
        )
        self.assertEqual(policy.scan_build_status("scripts/build.sh", content), [])

    def test_remote_status_capture_is_rejected_by_narrow_gate(self) -> None:
        unsafe = (
            'ssh builder "autoninja -C out chrome | tee build.log; '
            'rc=\\${PIPESTATUS[0]}; exit $rc"\n'
        )
        safe = (
            'ssh builder "autoninja -C out chrome | tee build.log; '
            'rc=\\${PIPESTATUS[0]}; exit \\$rc"\n'
        )
        self.assertEqual(len(policy.scan_build_status("scripts/build.sh", unsafe)), 1)
        self.assertEqual(len(policy.scan_build_status("scripts/build.sh", safe)), 1)

    def test_inert_remote_status_text_is_a_finding(self) -> None:
        content = (
            "ssh builder \"bash -lc 'autoninja -C out chrome | tee build.log; "
            "echo \\\"exit \\${PIPESTATUS[0]}\\\"'\"\n"
        )
        self.assertEqual(len(policy.scan_build_status("scripts/build.sh", content)), 1)

    def test_explicit_zsh_remote_status_exit_is_clean(self) -> None:
        content = (
            "ssh builder \"zsh -lc 'autoninja -C out chrome | tee build.log; "
            "exit \\${pipestatus[1]}'\"\n"
        )
        self.assertEqual(policy.scan_build_status("scripts/build.sh", content), [])

    def test_status_capture_only_protects_its_pipeline(self) -> None:
        content = (
            "#!/bin/bash\n"
            "autoninja -C out chrome | tee first.log\n"
            "first_rc=${PIPESTATUS[0]}\n"
            "[ \"$first_rc\" -ne 0 ] && exit \"$first_rc\"\n"
            "ssh builder \"autoninja -C out chrome | grep FAILED | tail -8\"\n"
        )
        findings = policy.scan_build_status("scripts/build.sh", content)
        self.assertEqual(len(findings), 1)
        self.assertEqual(findings[0].line, 5)

    def test_later_pipeline_makes_direct_pipestatus_stale(self) -> None:
        content = (
            "autoninja -C out chrome | tee build.log; "
            "true | true; exit ${PIPESTATUS[0]}\n"
        )
        self.assertEqual(len(policy.scan_build_status("scripts/build.sh", content)), 1)

    def test_inline_pipeline_makes_next_line_capture_stale(self) -> None:
        content = (
            "autoninja -C out chrome | tee build.log; true | true\n"
            "build_rc=${PIPESTATUS[0]}\n"
            "exit $build_rc\n"
        )
        self.assertEqual(len(policy.scan_build_status("scripts/build.sh", content)), 1)

    def test_overwritten_captured_status_is_a_finding(self) -> None:
        content = (
            "autoninja -C out chrome | tee build.log\n"
            "build_rc=${PIPESTATUS[0]}\n"
            "build_rc=0\n"
            "exit $build_rc\n"
        )
        self.assertEqual(len(policy.scan_build_status("scripts/build.sh", content)), 1)

    def test_later_remote_pipeline_makes_pipestatus_stale(self) -> None:
        content = (
            "ssh builder \"bash -lc 'autoninja -C out chrome | tee build.log; "
            "true | true; exit \\${PIPESTATUS[0]}'\"\n"
        )
        self.assertEqual(len(policy.scan_build_status("scripts/build.sh", content)), 1)

    def test_unused_status_capture_does_not_protect_pipeline(self) -> None:
        content = (
            "autoninja -C out chrome | tee build.log\n"
            "build_rc=${PIPESTATUS[0]}\n"
            "echo done\n"
        )
        self.assertEqual(len(policy.scan_build_status("scripts/build.sh", content)), 1)

    def test_status_capture_returned_by_script_is_clean(self) -> None:
        content = (
            "#!/usr/bin/env bash\n"
            "autoninja -C out chrome | tee build.log\n"
            "build_rc=${PIPESTATUS[0]}\n"
            "echo done\n"
            "exit \"$build_rc\"\n"
        )
        self.assertEqual(policy.scan_build_status("scripts/build.sh", content), [])

    def test_direct_pipeline_status_exit_is_clean(self) -> None:
        content = (
            "#!/bin/bash\n"
            "autoninja -C out chrome | tee build.log; "
            "exit ${PIPESTATUS[0]}\n"
        )
        self.assertEqual(policy.scan_build_status("scripts/build.sh", content), [])

    def test_bash_rejects_zsh_pipeline_status(self) -> None:
        content = (
            "#!/bin/bash\n"
            "autoninja -C out chrome | tee build.log; "
            "exit ${pipestatus[1]}\n"
        )
        self.assertEqual(len(policy.scan_build_status("scripts/build.sh", content)), 1)

    def test_zsh_accepts_zsh_pipeline_status(self) -> None:
        content = (
            "#!/usr/bin/env -S zsh -f\n"
            "autoninja -C out chrome | tee build.log; "
            "exit ${pipestatus[1]}\n"
        )
        self.assertEqual(policy.scan_build_status("scripts/build.sh", content), [])

    def test_status_marker_in_comment_does_not_protect_pipeline(self) -> None:
        content = (
            "# Remember to capture ${PIPESTATUS[0]}\n"
            "autoninja -C out chrome | tee build.log\n"
        )
        self.assertEqual(len(policy.scan_build_status("scripts/build.sh", content)), 1)

    def test_pipeline_text_in_comment_is_ignored(self) -> None:
        content = "# autoninja -C out chrome | grep FAILED | tail -8\n"
        self.assertEqual(policy.scan_build_status("scripts/build.sh", content), [])

    def test_deletion_only_sync_change_is_scanned(self) -> None:
        deleted_only = """\
diff --git a/scripts/sync.sh b/scripts/sync.sh
--- a/scripts/sync.sh
+++ b/scripts/sync.sh
@@ -8 +7,0 @@
-rsync -az --delete overlay/chrome/browser/cmux_term/ remote/
"""
        with mock.patch.object(
            policy,
            "target_diff_lines",
            return_value=[deleted_only.splitlines()],
        ), mock.patch.object(
            autoreview, "changed_paths", return_value={"scripts/sync.sh"}
        ), mock.patch.object(
            policy,
            "content_at_tip",
            return_value='rsync -az overlay/ "$HOST:$SRC/"\n',
        ), mock.patch.object(
            policy, "source_path_changes", return_value=(set(), set())
        ), mock.patch.object(
            policy, "registration_manifests", return_value={}
        ), mock.patch.object(
            policy, "base_registration_manifests", return_value={}
        ):
            findings = policy.scan(Path("/repo"), "commit", None, "change")
        self.assertTrue(
            any("retired cmux overlay files" in item.title for item in findings)
        )

    def test_scoped_sync_delete_is_clean(self) -> None:
        content = (
            "rsync -az --delete overlay/chrome/browser/cmux_term/ "
            '"$HOST:$SRC/chrome/browser/cmux_term/"\n'
            'rsync -az overlay/ "$HOST:$SRC/"\n'
        )
        self.assertEqual(policy.scan_sync_authority("scripts/sync.sh", content), [])

    def test_owned_patches_delete_is_clean(self) -> None:
        content = (
            "rsync -az --delete overlay/chrome/browser/cmux_term/ "
            '"$HOST:$SRC/chrome/browser/cmux_term/"\n'
            "rsync -az --delete patches/ "
            '"$HOST:$SRC/.cmux-patches/"\n'
        )
        self.assertEqual(policy.scan_sync_authority("scripts/sync.sh", content), [])

    def test_sync_delete_requires_owned_destination(self) -> None:
        content = (
            "rsync -az --delete overlay/chrome/browser/cmux_term/ "
            '"$HOST:$SRC/chrome/browser/"\n'
        )
        findings = policy.scan_sync_authority("scripts/sync.sh", content)
        self.assertEqual(len(findings), 1)
        self.assertIn("mixed Chromium directory", findings[0].title)

    def test_safe_sync_delete_does_not_mask_broad_overlay_delete(self) -> None:
        content = (
            "rsync -az --delete overlay/chrome/browser/cmux_term/ "
            '"$HOST:$SRC/chrome/browser/cmux_term/"\n'
            "rsync -az --delete overlay/ "
            '"$HOST:$SRC/chrome/"\n'
        )
        findings = policy.scan_sync_authority("scripts/sync.sh", content)
        self.assertEqual(len(findings), 1)
        self.assertEqual(findings[0].line, 2)
        self.assertIn("mixed Chromium directory", findings[0].title)

    def test_multiline_broad_sync_delete_is_a_finding(self) -> None:
        content = (
            "rsync -az --delete overlay/chrome/browser/cmux_term/ "
            '"$HOST:$SRC/chrome/browser/cmux_term/"\n'
            "rsync -az \\\n"
            "  --delete overlay/ \\\n"
            '  "$HOST:$SRC/chrome/"\n'
        )
        findings = policy.scan_sync_authority("scripts/sync.sh", content)
        self.assertEqual(len(findings), 1)
        self.assertEqual(findings[0].line, 2)

    def test_unscoped_sync_retains_stale_files(self) -> None:
        content = 'rsync -az overlay/ "$HOST:$SRC/"\n'
        self.assertEqual(len(policy.scan_sync_authority("scripts/sync.sh", content)), 1)

    def manifests(self) -> dict[str, str]:
        return {name: "" for name in policy.REGISTRATION_MANIFESTS}

    def test_manifest_entry_accepts_escaped_quotes_in_generated_blocks(
        self,
    ) -> None:
        content = r"""linux_block = ('\"cmux_term/shared_host.cc\"')"""
        self.assertTrue(
            policy.assignment_has_entry(
                content,
                ("linux_block",),
                "cmux_term/shared_host.cc",
                quoted=True,
            )
        )

    def test_rename_status_creates_destination_and_retires_source(self) -> None:
        created, retired = policy.parse_name_status(
            [
                "R100",
                "overlay/chrome/browser/cmux_term/old_widget.cc",
                "overlay/chrome/browser/cmux_term/new_widget.cc",
            ]
        )
        self.assertEqual(
            created,
            {"overlay/chrome/browser/cmux_term/new_widget.cc"},
        )
        self.assertEqual(
            retired,
            {"overlay/chrome/browser/cmux_term/old_widget.cc"},
        )

    def test_renamed_source_must_remove_old_manifest_entry(self) -> None:
        base = self.manifests()
        candidate = self.manifests()
        stale = "xplat = ['cmux_term/old_widget.cc']\n"
        base[policy.APPLY] = stale
        candidate[policy.APPLY] = stale
        findings = policy.scan_retired_source_registration(
            {"overlay/chrome/browser/cmux_term/old_widget.cc"},
            base,
            candidate,
        )
        self.assertEqual(len(findings), 1)
        self.assertEqual(findings[0].path, policy.APPLY)

    def test_retired_header_must_remove_old_manifest_entry(self) -> None:
        base = self.manifests()
        candidate = self.manifests()
        stale = "xplat = ['cmux_term/old_widget.h']\n"
        base[policy.APPLY] = stale
        candidate[policy.APPLY] = stale
        findings = policy.scan_retired_source_registration(
            {"overlay/chrome/browser/cmux_term/old_widget.h"},
            base,
            candidate,
        )
        self.assertEqual(len(findings), 1)
        self.assertEqual(findings[0].path, policy.APPLY)

    def test_renamed_source_destination_requires_registration(self) -> None:
        findings = policy.scan_source_registration(
            {"overlay/chrome/browser/cmux_term/new_widget.cc"},
            self.manifests(),
        )
        self.assertEqual(len(findings), 1)
        self.assertIn(policy.APPLY, findings[0].body)

    def test_cross_platform_source_requires_both_apply_manifests(self) -> None:
        manifests = self.manifests()
        manifests[policy.APPLY] = "xplat = ['cmux_term/new_widget.cc']\n"
        findings = policy.scan_source_registration(
            {"overlay/chrome/browser/cmux_term/new_widget.cc"}, manifests
        )
        self.assertEqual(len(findings), 1)
        self.assertIn(policy.APPLY_WIN, findings[0].body)
        self.assertIn("warm cross-platform refresh", findings[0].body)

    def test_cross_platform_source_in_both_apply_manifests_is_clean(self) -> None:
        manifests = self.manifests()
        manifests[policy.APPLY] = """\
xplat = ['cmux_term/new_widget.cc']
xplat_missing = ['cmux_term/new_widget.cc']
"""
        manifests[policy.APPLY_WIN] = """\
xplat = ['cmux_term/new_widget.cc']
xplat_missing = ['cmux_term/new_widget.cc']
"""
        self.assertEqual(
            policy.scan_source_registration(
                {"overlay/chrome/browser/cmux_term/new_widget.cc"}, manifests
            ),
            [],
        )

    def test_platform_headers_use_platform_scoped_manifests(self) -> None:
        manifests = self.manifests()
        manifests[policy.APPLY] = """\
maconly = ['cmux_term/widget_mac.h']
linux_block = ('cmux_term/widget_linux.h')
win_block = ('cmux_term/widget_win.h')
platform_sources = [
  ('cmux_term/widget_mac.h', 'mac anchor'),
  ('cmux_term/widget_linux.h', 'linux anchor'),
]
win_source = 'cmux_term/widget_win.h'
"""
        manifests[policy.APPLY_WIN] = """\
win_block = ('cmux_term/widget_win.h')
win_missing = ['cmux_term/widget_win.h']
"""
        self.assertEqual(
            policy.scan_source_registration(
                {
                    "overlay/chrome/browser/cmux_term/widget_mac.h",
                    "overlay/chrome/browser/cmux_term/widget_linux.h",
                    "overlay/chrome/browser/cmux_term/widget_win.h",
                },
                manifests,
            ),
            [],
        )

    def test_linux_windows_shared_source_uses_both_platform_blocks(self) -> None:
        source = "cmux_term/cmux_ghostty_opengl_host.cc"
        manifests = self.manifests()
        manifests[policy.APPLY] = f"""\
linux_block = ('{source}')
platform_sources = [('{source}', 'linux anchor')]
win_block = ('{source}')
win_sources = ['{source}']
"""
        manifests[policy.APPLY_WIN] = f"""\
win_block = ('{source}')
win_sources = ['{source}']
win_missing = [item for item in win_sources]
"""
        self.assertEqual(
            policy.scan_source_registration(
                {
                    "overlay/chrome/browser/cmux_term/"
                    "cmux_ghostty_opengl_host.cc"
                },
                manifests,
            ),
            [],
        )

    def test_demo_source_set_does_not_replace_production_registration(self) -> None:
        manifests = self.manifests()
        manifests[policy.CMUX_BUILD] = """\
source_set("cmux_chrome_ui") {
  sources = [ "new_widget.cc" ]
}
"""
        findings = policy.scan_source_registration(
            {"overlay/chrome/browser/cmux_term/new_widget.cc"}, manifests
        )
        self.assertEqual(len(findings), 1)
        self.assertIn(policy.APPLY, findings[0].body)
        self.assertIn(policy.APPLY_WIN, findings[0].body)

    def test_production_ghostty_source_set_registration_is_clean(self) -> None:
        manifests = self.manifests()
        manifests[policy.CMUX_BUILD] = """\
source_set("cmux_ghostty") {
  sources = [ "new_backend.mm" ]
}
"""
        self.assertEqual(
            policy.scan_source_registration(
                {"overlay/chrome/browser/cmux_term/new_backend.mm"}, manifests
            ),
            [],
        )

    def test_registration_in_comment_does_not_count(self) -> None:
        manifests = self.manifests()
        manifests[policy.APPLY] = "# xplat += ['cmux_term/new_widget.cc']\n"
        manifests[policy.APPLY_WIN] = "xplat = ['cmux_term/new_widget.cc']\n"
        findings = policy.scan_source_registration(
            {"overlay/chrome/browser/cmux_term/new_widget.cc"}, manifests
        )
        self.assertEqual(len(findings), 1)
        self.assertIn(policy.APPLY, findings[0].body)

    def test_registration_substring_does_not_count(self) -> None:
        self.assertFalse(
            policy.manifest_has_entry(
                'sources = [ "not_new_widget.cc" ]', "new_widget.cc"
            )
        )

    def test_registration_branch_follows_starred_source_list(self) -> None:
        content = """\
promoted_sources = ['cmux_term/new_widget.cc']
xplat = [*promoted_sources]
"""
        self.assertTrue(
            policy.assignment_has_entry(
                content,
                ("xplat",),
                "cmux_term/new_widget.cc",
                quoted=True,
            )
        )

    def test_new_host_test_requires_complete_test_runner_registration(self) -> None:
        manifests = self.manifests()
        findings = policy.scan_source_registration(
            {"overlay/chrome/browser/cmux_term/new_widget_test.cc"}, manifests
        )
        self.assertEqual(len(findings), 1)
        self.assertIn(policy.HOST_TESTS, findings[0].body)

        manifests[policy.HOST_TESTS] = "tests=( new_widget_test.cc )\n"
        findings = policy.scan_source_registration(
            {"overlay/chrome/browser/cmux_term/new_widget_test.cc"}, manifests
        )
        self.assertEqual(len(findings), 1)

        manifests[policy.HOST_TESTS] = """\
tests=(
  new_widget_test.cc
)
set_sources_for_test() {
  case "$1" in
    new_widget_test.cc)
      sources=("$TEST_DIR/new_widget_test.cc")
      ;;
  esac
}
"""
        self.assertEqual(
            policy.scan_source_registration(
                {"overlay/chrome/browser/cmux_term/new_widget_test.cc"}, manifests
            ),
            [],
        )


class ReportValidationTests(unittest.TestCase):
    def report(self) -> dict:
        return {
            "findings": [
                {
                    "title": "Concrete bug",
                    "body": "This changed line loses the operation result.",
                    "priority": "P1",
                    "confidence": 0.9,
                    "category": "bug",
                    "code_location": {"file_path": "changed.cc", "line": 7},
                }
            ],
            "overall_correctness": "patch is incorrect",
            "overall_explanation": "One actionable issue.",
            "overall_confidence": 0.9,
        }

    def test_valid_changed_path_is_kept(self) -> None:
        report, ignored = autoreview.validate_report(
            self.report(), {"changed.cc": {7}}
        )
        self.assertEqual(len(report["findings"]), 1)
        self.assertEqual(ignored, [])

    def test_out_of_scope_path_fails_closed(self) -> None:
        with self.assertRaises(autoreview.InfrastructureError):
            autoreview.validate_report(self.report(), {"other.cc": {7}})

    def test_unchanged_line_fails_closed(self) -> None:
        with self.assertRaises(autoreview.InfrastructureError):
            autoreview.validate_report(self.report(), {"changed.cc": {8}})

    def test_low_confidence_output_fails_closed(self) -> None:
        report = self.report()
        report["overall_confidence"] = 0.79
        with self.assertRaises(autoreview.InfrastructureError):
            autoreview.validate_report(report, {"changed.cc": {7}})


if __name__ == "__main__":
    unittest.main()
