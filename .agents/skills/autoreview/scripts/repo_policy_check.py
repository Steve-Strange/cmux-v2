#!/usr/bin/env python3
from __future__ import annotations

import argparse
import io
import re
import shlex
import sys
import tokenize
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Iterable

sys.dont_write_bytecode = True
SCRIPT_DIR = Path(__file__).resolve().parent
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

import autoreview


@dataclass(frozen=True)
class Finding:
    priority: str
    title: str
    path: str
    line: int
    body: str


def target_diff_lines(
    repo: Path, target: str, base: str | None, commit: str
) -> list[Iterable[str]]:
    return [
        autoreview.git_lines(repo, *args)
        for args in autoreview.target_diff_commands(
            repo, target, base, commit
        )
    ]


def policy_line(text: str) -> bool:
    return "ScopedAllowBlockingForTesting" in text or bool(
        re.search(
            r"\bclass\s+\w+|\b(?:make_unique\s*<|new\s+)\w+",
            text,
        )
    )


def parse_added_lines(
    diff: str | Iterable[str],
) -> dict[str, dict[int, set[str]]]:
    result: dict[str, dict[int, set[str]]] = {}
    path: str | None = None
    new_line: int | None = None
    in_hunk = False
    for raw in autoreview._diff_lines(diff):
        if raw.startswith("diff --git "):
            path = None
            new_line = None
            in_hunk = False
            continue
        if raw.startswith("@@ "):
            match = re.search(r"\+(\d+)(?:,(\d+))?", raw)
            new_line = int(match.group(1)) if match else None
            in_hunk = new_line is not None
            continue
        if in_hunk and new_line is not None:
            if raw.startswith("+"):
                text = raw[1:]
                if path is not None and policy_line(text):
                    result.setdefault(path, {}).setdefault(new_line, set()).add(text)
                new_line += 1
            elif raw.startswith("-") or raw.startswith("\\"):
                pass
            elif raw.startswith(" "):
                new_line += 1
            else:
                in_hunk = False
            continue
        if raw.startswith("+++ "):
            path = autoreview.normalize_diff_path(raw[4:])
    return result


def add_untracked_lines(repo: Path, added: dict[str, dict[int, set[str]]]) -> None:
    for rel in autoreview.git_records(
        repo, "ls-files", "-z", "--others", "--exclude-standard"
    ):
        path = repo / rel
        if path.is_symlink() or not path.is_file():
            continue
        autoreview.require_contained_path(repo, path, rel)
        try:
            lines: dict[int, set[str]] = {}
            for index, line in enumerate(
                autoreview.bounded_file_lines(path), 1
            ):
                if policy_line(line):
                    lines[index] = {line}
        except OSError:
            continue
        added[rel] = lines


def local_snapshot_diff(repo: Path, *, index: bool) -> Iterable[str]:
    args = ["diff"]
    if index:
        args.append("--cached")
    args.extend(["--unified=0", "--find-renames", "--no-ext-diff", "HEAD"])
    return autoreview.git_lines(repo, *args)


def local_snapshot_paths(repo: Path, *, index: bool) -> set[str]:
    args = ["diff"]
    if index:
        args.append("--cached")
    args.extend(["--name-only", "-z", "HEAD"])
    paths = set(autoreview.git_records(repo, *args))
    if not index:
        paths.update(
            autoreview.git_records(
                repo, "ls-files", "-z", "--others", "--exclude-standard"
            )
        )
    return paths


def parse_name_status(
    records: Iterable[str],
) -> tuple[set[str], set[str]]:
    created: set[str] = set()
    retired: set[str] = set()
    iterator = iter(records)
    for status in iterator:
        if not status or status[0] not in "ACDMRTUXB":
            raise autoreview.InfrastructureError(
                f"malformed Git name-status record: {status!r}"
            )
        kind = status[0]
        try:
            first = next(iterator)
            if kind in {"R", "C"}:
                second = next(iterator)
            else:
                second = None
        except StopIteration as exc:
            raise autoreview.InfrastructureError(
                "truncated Git name-status output"
            ) from exc
        if kind == "A":
            created.add(first)
        elif kind == "D":
            retired.add(first)
        elif kind == "R":
            assert second is not None
            retired.add(first)
            created.add(second)
        elif kind == "C":
            assert second is not None
            created.add(second)
    return created, retired


def source_path_changes(
    repo: Path,
    target: str,
    base: str | None,
    commit: str,
    *,
    local_index: bool | None = None,
) -> tuple[set[str], set[str]]:
    args: list[str]
    if target == "local":
        if local_index is None:
            raise autoreview.InfrastructureError(
                "local source changes require an index/worktree selector"
            )
        args = ["diff"]
        if local_index:
            args.append("--cached")
        args.extend(
            ["--name-status", "-z", "--find-renames=100%", "HEAD"]
        )
        created, retired = parse_name_status(
            autoreview.git_records(repo, *args)
        )
        if not local_index:
            created.update(
                autoreview.git_records(
                    repo, "ls-files", "-z", "--others", "--exclude-standard"
                )
            )
            retired.difference_update(created)
        return created, retired
    if target == "branch":
        if not base:
            raise autoreview.InfrastructureError(
                "branch source changes require a base"
            )
        args = [
            "diff",
            "--name-status",
            "-z",
            "--find-renames=100%",
            f"{base}...HEAD",
        ]
    else:
        parent = autoreview.first_parent(repo, commit)
        if parent:
            args = [
                "diff",
                "--name-status",
                "-z",
                "--find-renames=100%",
                parent,
                commit,
            ]
        else:
            args = [
                "show",
                "--root",
                "--format=",
                "--name-status",
                "-z",
                "--find-renames=100%",
                commit,
            ]
    return parse_name_status(autoreview.git_records(repo, *args))


def content_at_tip(
    repo: Path, path: str, target: str, commit: str
) -> str:
    if target == "local":
        file = repo / path
        if not file.is_symlink() and file.is_file():
            autoreview.require_contained_path(repo, file, path)
            content, truncated = autoreview.read_file_prefix(
                file, autoreview.MAX_POLICY_FILE_BYTES
            )
            if truncated:
                raise autoreview.InfrastructureError(
                    f"policy input exceeds {autoreview.MAX_POLICY_FILE_BYTES} "
                    f"bytes: {path}"
                )
            return content
        return ""
    ref = "HEAD" if target == "branch" else commit
    return content_from_git(repo, f"{ref}:{path}")


def content_from_git(repo: Path, object_spec: str) -> str:
    content, truncated = autoreview.git_output_prefix(
        repo,
        autoreview.MAX_POLICY_FILE_BYTES,
        "show",
        object_spec,
        check=False,
    )
    if truncated:
        raise autoreview.InfrastructureError(
            f"policy input exceeds {autoreview.MAX_POLICY_FILE_BYTES} bytes: "
            f"{object_spec}"
        )
    return content


CMUX_BUILD = "overlay/chrome/browser/cmux_term/BUILD.gn"
APPLY = "scripts/apply.sh"
APPLY_WIN = "scripts/apply_win_chrome.py"
HOST_TESTS = "scripts/run-host-tests.sh"
REGISTRATION_MANIFESTS = (CMUX_BUILD, APPLY, APPLY_WIN, HOST_TESTS)
DEMO_ONLY_BUILD_SOURCES = {"cmux_strip_view.cc"}
LINUX_WINDOWS_SHARED_BUILD_SOURCES = {"cmux_ghostty_opengl_host.cc"}


def registration_manifests(
    repo: Path, target: str, commit: str, *, local_index: bool = False
) -> dict[str, str]:
    manifests: dict[str, str] = {}
    ref = "HEAD" if target == "branch" else commit
    for rel in REGISTRATION_MANIFESTS:
        if target == "local":
            if local_index:
                manifests[rel] = content_from_git(repo, f":{rel}")
                continue
            path = repo / rel
            if path.is_symlink() or not path.is_file():
                manifests[rel] = ""
                continue
            try:
                manifests[rel] = content_at_tip(
                    repo, rel, "local", commit
                )
            except OSError:
                manifests[rel] = ""
        else:
            manifests[rel] = content_from_git(repo, f"{ref}:{rel}")
    return manifests


def registration_manifests_at_ref(
    repo: Path, ref: str | None
) -> dict[str, str]:
    if not ref:
        return {rel: "" for rel in REGISTRATION_MANIFESTS}
    return {
        rel: content_from_git(repo, f"{ref}:{rel}")
        for rel in REGISTRATION_MANIFESTS
    }


def base_registration_manifests(
    repo: Path, target: str, base: str | None, commit: str
) -> dict[str, str]:
    if target == "local":
        ref = "HEAD"
    elif target == "branch":
        if not base:
            raise autoreview.InfrastructureError(
                "branch registration scan requires a base"
            )
        ref = autoreview.git(repo, "merge-base", base, "HEAD").strip()
        if not ref:
            raise autoreview.InfrastructureError(
                f"branch registration scan has no merge base with {base}"
            )
    else:
        ref = autoreview.first_parent(repo, commit)
    return registration_manifests_at_ref(repo, ref)


def manifest_has_entry(content: str, token: str, *, quoted: bool = True) -> bool:
    # A filename in a comment or as a substring of another source is not a
    # registration. The authoritative manifests use quoted literals except
    # for the shell host-test array/case labels.
    code = "\n".join(line.split("#", 1)[0] for line in content.splitlines())
    escaped = re.escape(token)
    if quoted:
        return bool(
            re.search(rf"(['\"]){escaped}\1", code)
            or re.search(
                rf"\\(?P<escaped_quote>['\"]){escaped}"
                rf"\\(?P=escaped_quote)",
                code,
            )
        )
    return bool(re.search(rf"(?<![A-Za-z0-9_./-]){escaped}(?![A-Za-z0-9_./-])", code))


def host_test_registered(content: str, name: str) -> bool:
    code = "\n".join(line.split("#", 1)[0] for line in content.splitlines())
    tests = re.search(
        r"(?ms)^\s*tests\s*=\s*\((.*?)^\s*\)",
        code,
    )
    if not tests or not manifest_has_entry(tests.group(1), name, quoted=False):
        return False
    selector = re.search(
        r"(?ms)^\s*set_sources_for_test\(\)\s*\{(.*?)^\s*\}",
        code,
    )
    if not selector:
        return False
    case = re.search(
        rf"(?ms)^\s*{re.escape(name)}\)\s*(.*?)^\s*;;",
        selector.group(1),
    )
    if not case:
        return False
    # The case must actually compile the test, not just accept its name.
    return bool(
        re.search(
            rf"(?<![A-Za-z0-9_.-]){re.escape(name)}(?![A-Za-z0-9_.-])",
            case.group(1),
        )
    )


def gn_target_has_entry(content: str, target: str, name: str) -> bool:
    marker = f'source_set("{target}")'
    start = content.find(marker)
    if start < 0:
        return False
    brace = content.find("{", start + len(marker))
    if brace < 0:
        return False
    depth = 0
    end = len(content)
    for index in range(brace, len(content)):
        if content[index] == "{":
            depth += 1
        elif content[index] == "}":
            depth -= 1
            if depth == 0:
                end = index
                break
    return manifest_has_entry(content[brace:end], name)


def line_for(content: str, needle: str) -> int:
    for index, line in enumerate(content.splitlines(), 1):
        if needle in line:
            return index
    return 1


def scan_runtime_blocking(
    path: str, lines: dict[int, set[str]]
) -> list[Finding]:
    if "_test." in path or not path.startswith("overlay/"):
        return []
    return [
        Finding(
            "P1",
            "Production code suppresses Chromium's blocking-I/O guard",
            path,
            line,
            "ScopedAllowBlockingForTesting does not move work off the UI sequence. "
            "Use a MayBlock worker and reply through the owning sequence.",
        )
        for line, variants in lines.items()
        for text in variants
        if "ScopedAllowBlockingForTesting" in text
    ]


def scan_views_metadata(
    path: str,
    lines: dict[int, set[str]],
    content: str,
    related_contents: Iterable[str] = (),
) -> list[Finding]:
    if not path.endswith((".cc", ".h", ".mm")):
        return []
    findings: list[Finding] = []
    context = "\n".join((content, *related_contents))
    candidates: dict[str, int] = {}
    for line, variants in lines.items():
        for text in variants:
            declaration = re.search(r"\bclass\s+(\w+)", text)
            construction = re.search(
                r"\b(?:make_unique\s*<|new\s+)(\w+)", text
            )
            if declaration:
                candidates.setdefault(declaration.group(1), line)
            if construction:
                candidates.setdefault(construction.group(1), line)

    for name, line in candidates.items():
        subclass = re.search(
            rf"\bclass\s+{re.escape(name)}\b[^{{;]*:\s*"
            r"[^\{;]*\bpublic\s+views::"
            r"(?:View|Textfield|Button|Label)\b",
            context,
            re.DOTALL,
        )
        if not subclass:
            continue
        has_header = re.search(
            rf"METADATA_HEADER\(\s*{re.escape(name)}\b", context
        )
        has_definition = re.search(
            rf"BEGIN_METADATA\(\s*{re.escape(name)}\b[^)]*\).*?\bEND_METADATA\b",
            context,
            re.DOTALL,
        )
        if has_header and has_definition:
            continue
        instantiated = re.search(
            rf"(?:make_unique\s*<\s*{re.escape(name)}\b|"
            rf"new\s+{re.escape(name)}\b)",
            context,
        )
        if not instantiated:
            continue
        findings.append(
            Finding(
                "P1",
                f"Views subclass {name} has incomplete class metadata",
                path,
                line,
                "The class is instantiated as a Views child but lacks "
                "METADATA_HEADER and its metadata definition; the pinned "
                "builder can assert or fail.",
            )
        )
    return findings


def logical_shell_commands(content: str) -> list[tuple[int, str]]:
    def shell_code(line: str, quote: str | None) -> tuple[str, str | None]:
        output: list[str] = []
        index = 0
        while index < len(line):
            char = line[index]
            if quote == "single":
                output.append(char)
                if char == "'":
                    quote = None
                index += 1
                continue
            if quote == "double":
                output.append(char)
                if char == "\\" and index + 1 < len(line):
                    output.append(line[index + 1])
                    index += 2
                    continue
                if char == '"':
                    quote = None
                index += 1
                continue
            if char == "#" and (
                not output or output[-1].isspace() or output[-1] in ";|&()"
            ):
                break
            output.append(char)
            if char == "\\" and index + 1 < len(line):
                output.append(line[index + 1])
                index += 2
                continue
            if char == "'":
                quote = "single"
            elif char == '"':
                quote = "double"
            index += 1
        return "".join(output), quote

    logical_commands: list[tuple[int, str]] = []
    command_start = 1
    command_parts: list[str] = []
    quote: str | None = None
    for line_number, raw in enumerate(content.splitlines(), 1):
        code, quote = shell_code(raw, quote)
        code = code.rstrip()
        trailing_backslashes = len(code) - len(code.rstrip("\\"))
        continued = quote != "single" and trailing_backslashes % 2 == 1
        if not command_parts:
            command_start = line_number
        if continued:
            command_parts.append(code[:-1])
            continue
        command_parts.append(code)
        if quote is None:
            logical_commands.append((command_start, "\n".join(command_parts)))
            command_parts = []
    if command_parts:
        logical_commands.append((command_start, "\n".join(command_parts)))
    return logical_commands


def shell_command_segments(code: str) -> list[str]:
    segments: list[str] = []
    current: list[str] = []
    quote: str | None = None
    index = 0
    while index < len(code):
        char = code[index]
        if quote == "single":
            current.append(char)
            if char == "'":
                quote = None
            index += 1
            continue
        if quote == "double":
            current.append(char)
            if char == "\\" and index + 1 < len(code):
                current.append(code[index + 1])
                index += 2
                continue
            if char == '"':
                quote = None
            index += 1
            continue
        if char == "#" and (
            not current or current[-1].isspace() or current[-1] in ";|&()"
        ):
            while index < len(code) and code[index] != "\n":
                index += 1
            continue
        if char in ";\n" or (
            char in "&|" and index + 1 < len(code) and code[index + 1] == char
        ):
            segments.append("".join(current).strip())
            current = []
            index += 1 if char in ";\n" else 2
            continue
        current.append(char)
        if char == "\\" and index + 1 < len(code):
            current.append(code[index + 1])
            index += 2
            continue
        if char == "'":
            quote = "single"
        elif char == '"':
            quote = "double"
        index += 1
    segments.append("".join(current).strip())
    return [segment for segment in segments if segment]


@dataclass(frozen=True)
class QuotedShellString:
    quote: str
    content: str
    start: int
    end: int


def quoted_shell_strings(code: str) -> list[QuotedShellString]:
    strings: list[QuotedShellString] = []
    index = 0
    while index < len(code):
        quote = code[index]
        if quote not in {'"', "'"}:
            if code[index] == "\\" and index + 1 < len(code):
                index += 2
            else:
                index += 1
            continue
        start = index
        index += 1
        content: list[str] = []
        while index < len(code):
            char = code[index]
            if char == quote:
                strings.append(
                    QuotedShellString(
                        quote=quote,
                        content="".join(content),
                        start=start,
                        end=index + 1,
                    )
                )
                index += 1
                break
            if quote == '"' and char == "\\" and index + 1 < len(code):
                escaped = code[index + 1]
                if escaped in "$`\"\\\n":
                    if escaped != "\n":
                        content.append(escaped)
                    index += 2
                    continue
            content.append(char)
            index += 1
        else:
            break
    return strings


def script_shell_dialect(content: str) -> str | None:
    first_line = content.splitlines()[0] if content.splitlines() else ""
    if not first_line.startswith("#!"):
        return None
    try:
        words = shlex.split(first_line[2:].strip())
    except ValueError:
        return None
    if not words:
        return None
    executable = Path(words[0]).name
    if executable in {"bash", "zsh"}:
        return executable
    if executable != "env":
        return None
    for word in words[1:]:
        if word.startswith("-") or re.match(r"^[A-Za-z_][A-Za-z0-9_]*=", word):
            continue
        candidate = Path(word).name
        return candidate if candidate in {"bash", "zsh"} else None
    return None


def scan_build_status(path: str, content: str) -> list[Finding]:
    if not path.startswith("scripts/") or "build" not in Path(path).name:
        return []

    logical_commands = logical_shell_commands(content)
    line_numbers = [line for line, _ in logical_commands]
    lines = [code for _, code in logical_commands]

    def local_failure_options_enabled(before: int) -> bool:
        pipefail = False
        errexit = False
        for raw in lines[: before + 1]:
            for segment in shell_command_segments(raw):
                match = re.match(
                    r"^set\s+([+-])([A-Za-z]+)(?:\s+([A-Za-z]+))?\s*$",
                    segment,
                )
                if not match:
                    continue
                enabled = match.group(1) == "-"
                flags = match.group(2)
                option = match.group(3)
                if "e" in flags:
                    errexit = enabled
                if "o" in flags and option == "pipefail":
                    pipefail = enabled
                if "o" in flags and option == "errexit":
                    errexit = enabled
        return pipefail and errexit

    def pipeline_is_remote(at: int) -> bool:
        return bool(re.search(r"\bssh\b", lines[at]))

    bash_status_expression = r"\$\{PIPESTATUS\[0\]\}"
    zsh_status_expression = r"\$(?:\{pipestatus\[1\]\}|pipestatus\[1\])"
    status_expression = (
        r"\$(?:\{PIPESTATUS\[0\]\}|\{pipestatus\[1\]\}|pipestatus\[1\])"
    )
    dialect = script_shell_dialect(content)
    local_status_expression = (
        bash_status_expression
        if dialect == "bash"
        else zsh_status_expression
        if dialect == "zsh"
        else r"(?!)"
    )

    def dollar_reaches_remote_shell(code: str, offset: int) -> bool:
        """Whether the local shell preserves the dollar at offset literally."""
        quote: str | None = None
        index = 0
        while index < offset:
            char = code[index]
            if quote == "single":
                if char == "'":
                    quote = None
                index += 1
                continue
            if quote == "double":
                if char == "\\" and index + 1 < offset and code[index + 1] in (
                    "$`\"\\\n"
                ):
                    index += 2
                    continue
                if char == '"':
                    quote = None
                index += 1
                continue
            if char == "\\":
                index += 2
                continue
            if char == "'":
                quote = "single"
            elif char == '"':
                quote = "double"
            index += 1

        if quote == "single":
            return True
        backslashes = 0
        index = offset - 1
        while index >= 0 and code[index] == "\\":
            backslashes += 1
            index -= 1
        return backslashes % 2 == 1

    def variable_ref(name: str) -> str:
        escaped = re.escape(name)
        return rf"\$(?:\{{{escaped}\}}|{escaped}\b)"

    def captured_status_controls_failure(name: str, start: int) -> bool:
        reference = variable_ref(name)
        assignment = re.compile(
            rf"(?:^|[;&]\s*)(?:local\s+)?{re.escape(name)}\s*="
        )
        conditional_depth = 0
        conditional_uses_status = False
        for raw in lines[start:]:
            code = raw.strip()
            if not code:
                continue
            if assignment.search(code):
                return False
            if re.search(
                rf"\b(?:exit|return)\s+['\"]?{reference}['\"]?", code
            ):
                return True
            if re.search(rf"(?:^|;)\s*if\b[^;]*{reference}", code) or re.search(
                rf"^\s*\[[^]]*{reference}[^]]*\]\s*&&", code
            ):
                conditional_depth += 1
                conditional_uses_status = True
            if conditional_uses_status and re.search(
                r"\b(?:exit|return)\s+(?:[1-9][0-9]*|['\"]?\$)", code
            ):
                return True
            if conditional_depth and re.search(r"(?:^|;)\s*fi(?:\s*;|$)", code):
                conditional_depth -= 1
                if conditional_depth == 0:
                    conditional_uses_status = False
            if "autoninja" in code and "|" in code:
                break
        return False

    def captures_this_pipeline(at: int) -> bool:
        current = lines[at]
        pipeline_tail = current[current.find("autoninja") :]
        segments = shell_command_segments(pipeline_tail)
        exit_pattern = re.compile(
            rf"^(?:exit|return)\s+['\"]?{local_status_expression}['\"]?$"
        )
        for index, segment in enumerate(segments):
            if "autoninja" not in segment or "|" not in segment:
                continue
            if index + 1 < len(segments) and exit_pattern.fullmatch(
                segments[index + 1].strip()
            ):
                return True
            if index + 1 < len(segments):
                return False
            break
        for index, raw in enumerate(lines[at + 1 :], at + 1):
            code = raw.strip()
            if not code:
                continue
            capture = re.match(
                rf"(?:local\s+)?([A-Za-z_][A-Za-z0-9_]*)\s*=\s*"
                rf"['\"]?{local_status_expression}['\"]?\s*(?:;|$)",
                code,
            )
            if not capture:
                return False
            return captured_status_controls_failure(capture.group(1), index + 1)
        return False

    def remote_pipeline_propagates_status(at: int) -> bool:
        # A local PIPESTATUS after `ssh host "... | ..."` is only the status
        # of ssh. Accept only a narrow, executable remote script: the caller
        # must preserve the dollar, an explicitly selected Bash/zsh must receive
        # a quoted script without expanding it in the login shell, and that
        # script must execute a direct status exit after the pipeline.
        code = lines[at]
        local_statuses = list(re.finditer(status_expression, code))
        if not any(
            dollar_reaches_remote_shell(code, match.start())
            for match in local_statuses
        ):
            return False
        outer = next(
            (
                item
                for item in quoted_shell_strings(code)
                if "autoninja" in item.content and "|" in item.content
            ),
            None,
        )
        if outer is None:
            return False
        remote = outer.content
        dialect_match = re.search(
            r"(?:^|[;&|]\s*)(?:/bin/)?(?P<shell>bash|zsh)\s+"
            r"-[A-Za-z]*c\b",
            remote,
        )
        if not dialect_match:
            return False
        remote_statuses = list(re.finditer(status_expression, remote))
        if not any(
            dollar_reaches_remote_shell(remote, match.start())
            for match in remote_statuses
        ):
            return False
        script_string = next(
            (
                item
                for item in quoted_shell_strings(remote)
                if "autoninja" in item.content and "|" in item.content
            ),
            None,
        )
        if script_string is None:
            return False
        script = script_string.content
        exit_pattern = re.compile(
            rf"^(?:exit|return)\s+['\"]?"
            rf"(?P<status>{status_expression})['\"]?$"
        )
        segments = shell_command_segments(script)
        for index, segment in enumerate(segments):
            if "autoninja" not in segment or "|" not in segment:
                continue
            if index + 1 >= len(segments):
                return False
            following = segments[index + 1].strip()
            exit_match = exit_pattern.fullmatch(following)
            if not exit_match:
                return False
            status = exit_match.group("status")
            if dialect_match.group("shell") == "bash" and "PIPESTATUS" not in status:
                return False
            if dialect_match.group("shell") == "zsh" and "pipestatus" not in status:
                return False
            if dollar_reaches_remote_shell(
                following, exit_match.start("status")
            ):
                return False
            return True
        return False

    for offset, line in enumerate(lines):
        code = line
        if "autoninja" in code and "|" in code:
            remote = pipeline_is_remote(offset)
            protected = (
                remote_pipeline_propagates_status(offset)
                if remote
                else captures_this_pipeline(offset)
                or local_failure_options_enabled(offset)
            )
            if protected:
                continue
            return [
                Finding(
                    "P1",
                    "Build output pipeline can hide autoninja failure",
                    path,
                    line_numbers[offset],
                    "Capture the remote autoninja status explicitly and exit with it; "
                    "a later grep, tail, or echo must not make the build false-green.",
                )
            ]
    return []


def scan_sync_authority(path: str, content: str) -> list[Finding]:
    if path != "scripts/sync.sh" or "rsync" not in content or "overlay/" not in content:
        return []
    saw_scoped_delete = False
    unsafe_line: int | None = None
    owned_pairs = (
        (
            "overlay/chrome/browser/cmux_term/",
            "/chrome/browser/cmux_term",
        ),
        ("patches/", "/.cmux-patches"),
    )
    for line_number, raw in logical_shell_commands(content):
        if "rsync" not in raw or "--delete" not in raw:
            continue
        try:
            tokens = shlex.split(raw, comments=True, posix=True)
        except ValueError:
            unsafe_line = line_number
            break
        try:
            rsync_index = tokens.index("rsync")
        except ValueError:
            continue
        args = tokens[rsync_index + 1 :]
        delete_options = [
            value
            for value in args
            if value == "--delete" or value.startswith("--delete-")
        ]
        if not delete_options:
            continue
        if len(args) < 2:
            unsafe_line = line_number
            break
        source = args[-2]
        destination = args[-1].rstrip("/")
        matched_pair = next(
            (
                pair
                for pair in owned_pairs
                if source == pair[0] and destination.endswith(pair[1])
            ),
            None,
        )
        if matched_pair is None:
            unsafe_line = line_number
            break
        if matched_pair == owned_pairs[0]:
            saw_scoped_delete = True
    if saw_scoped_delete and unsafe_line is None:
        return []
    return [
        Finding(
            "P2",
            (
                "Scoped sync deletion targets a mixed Chromium directory"
                if unsafe_line is not None
                else "Warm builder keeps retired cmux overlay files"
            ),
            path,
            unsafe_line or line_for(content, "rsync"),
            "Use --delete only for an explicitly owned mirror pair: cmux_term "
            "to chrome/browser/cmux_term/ or patches/ to .cmux-patches/. Never "
            "delete from a mixed upstream directory.",
        )
    ]


def required_registration_manifests(
    path: str, manifests: dict[str, str]
) -> list[tuple[str, str, bool]]:
    name = Path(path).name
    if path.startswith("overlay/ui/views/examples/"):
        return [(APPLY, name, True)]
    if not path.startswith("overlay/chrome/browser/cmux_term/"):
        return []
    if path.endswith("_test.cc"):
        return [(HOST_TESTS, name, False)]
    if path.endswith(".mojom"):
        return [(CMUX_BUILD, name, True)]
    build = manifests.get(CMUX_BUILD, "")
    if gn_target_has_entry(build, "cmux_ghostty", name):
        # Production's macOS target explicitly depends on cmux_ghostty.
        return []
    if name in DEMO_ONLY_BUILD_SOURCES and gn_target_has_entry(
        build, "cmux_chrome_ui", name
    ):
        # This narrow audited allowlist is linked only by views_examples. A new
        # source-set entry must not silently bypass the production patchers.
        return []
    cmux_path = f"cmux_term/{name}"
    if path.endswith(".mm") or path.endswith(("_mac.cc", "_mac.h")):
        return [(APPLY, cmux_path, True)]
    if path.endswith(("_linux.cc", "_linux.h")):
        return [(APPLY, cmux_path, True)]
    if path.endswith(("_win.cc", "_win.h")):
        return [
            (APPLY, cmux_path, True),
            (APPLY_WIN, cmux_path, True),
        ]
    return [
        (APPLY, cmux_path, True),
        (APPLY_WIN, cmux_path, True),
    ]


def python_assignment_values(content: str, name: str) -> list[str]:
    values: list[str] = []
    assignment = re.compile(
        rf"(?m)^[ \t]*{re.escape(name)}[ \t]*="
    )
    for match in assignment.finditer(content):
        source = content[match.end() :]
        parts: list[str] = []
        depth = 0
        try:
            tokens = tokenize.generate_tokens(io.StringIO(source).readline)
            for item in tokens:
                if item.type == tokenize.OP:
                    if item.string in "([{":
                        depth += 1
                    elif item.string in ")]}":
                        depth = max(0, depth - 1)
                if item.type == tokenize.NEWLINE and depth == 0:
                    break
                if item.type == tokenize.ENDMARKER:
                    break
                parts.append(item.string)
        except (IndentationError, tokenize.TokenError):
            continue
        values.append(" ".join(parts))
    return values


def assignment_has_entry(
    content: str,
    names: tuple[str, ...],
    token: str,
    *,
    quoted: bool,
) -> bool:
    def contains(candidates: tuple[str, ...], seen: set[str]) -> bool:
        for name in candidates:
            if name in seen:
                continue
            seen.add(name)
            for value in python_assignment_values(content, name):
                if manifest_has_entry(value, token, quoted=quoted):
                    return True
                references = tuple(
                    re.findall(r"\*\s*([A-Za-z_][A-Za-z0-9_]*)", value)
                )
                if references and contains(references, seen):
                    return True
        return False

    return contains(names, set())


def registration_branch_requirements(
    path: str, manifest: str
) -> list[tuple[str, tuple[str, ...]]]:
    if not path.startswith("overlay/chrome/browser/cmux_term/"):
        return []
    name = Path(path).name
    linux_windows_shared = name in LINUX_WINDOWS_SHARED_BUILD_SOURCES
    mac = path.endswith(".mm") or path.endswith(("_mac.cc", "_mac.h"))
    linux = path.endswith(("_linux.cc", "_linux.h"))
    windows = path.endswith(("_win.cc", "_win.h"))
    if manifest == APPLY:
        if linux_windows_shared:
            return [
                ("clean Linux block", ("linux_block",)),
                ("warm Linux refresh", ("platform_sources",)),
                ("clean Windows block", ("win_block",)),
                ("warm Windows refresh", ("win_sources",)),
            ]
        if mac:
            return [
                ("clean macOS list", ("maconly",)),
                ("warm macOS refresh", ("platform_sources",)),
            ]
        if linux:
            return [
                ("clean Linux block", ("linux_block",)),
                ("warm Linux refresh", ("platform_sources",)),
            ]
        if windows:
            return [
                ("clean Windows block", ("win_block",)),
                (
                    "warm Windows refresh",
                    ("win_source", "win_sources", "platform_sources"),
                ),
            ]
        return [
            ("clean cross-platform list", ("xplat",)),
            ("warm cross-platform refresh", ("xplat_missing",)),
        ]
    if manifest == APPLY_WIN:
        if linux_windows_shared:
            return [
                ("clean Windows block", ("win_block",)),
                (
                    "warm Windows refresh",
                    ("win_missing", "win_sources_missing", "win_sources"),
                ),
            ]
        if windows:
            return [
                ("clean Windows block", ("win_block",)),
                (
                    "warm Windows refresh",
                    ("win_missing", "win_sources_missing", "win_sources"),
                ),
            ]
        return [
            ("clean cross-platform list", ("xplat",)),
            (
                "warm cross-platform refresh",
                ("xplat_missing", "xplat_refresh"),
            ),
        ]
    return []


def scan_source_registration(
    new_paths: set[str], manifests: dict[str, str]
) -> list[Finding]:
    findings: list[Finding] = []
    for path in sorted(new_paths):
        if not path.endswith((".cc", ".h", ".mm", ".mojom")):
            continue
        name = Path(path).name
        if path.startswith("overlay/chrome/browser/cmux_term/") and path.endswith(
            "_test.cc"
        ):
            if host_test_registered(manifests.get(HOST_TESTS, ""), name):
                continue
            findings.append(
                Finding(
                    "P1",
                    f"New host test {name} is incomplete in its runner",
                    path,
                    1,
                    f"In {HOST_TESTS}, add the test to the tests array and add "
                    "a matching set_sources_for_test case that compiles the "
                    "test source.",
                )
            )
            continue
        required = required_registration_manifests(path, manifests)
        missing: list[str] = []
        for manifest, token, quoted in required:
            content = manifests.get(manifest, "")
            branches = registration_branch_requirements(path, manifest)
            if not branches:
                if not manifest_has_entry(content, token, quoted=quoted):
                    missing.append(manifest)
                continue
            for label, assignment_names in branches:
                if not assignment_has_entry(
                    content,
                    assignment_names,
                    token,
                    quoted=quoted,
                ):
                    missing.append(f"{manifest} ({label})")
        if not missing:
            continue
        findings.append(
            Finding(
                "P1",
                f"New source {name} is absent from an owning manifest",
                path,
                1,
                "Register the source in every applicable owning manifest. Missing: "
                + ", ".join(missing),
            )
        )
    return findings


def possible_registration_entries(
    path: str,
) -> list[tuple[str, str, bool]]:
    name = Path(path).name
    if path.startswith("overlay/ui/views/examples/"):
        return [(APPLY, name, True)]
    if not path.startswith("overlay/chrome/browser/cmux_term/"):
        return []
    entries = [
        (CMUX_BUILD, name, True),
        (APPLY, f"cmux_term/{name}", True),
        (APPLY_WIN, f"cmux_term/{name}", True),
    ]
    if path.endswith("_test.cc"):
        entries.append((HOST_TESTS, name, False))
    return entries


def scan_retired_source_registration(
    retired_paths: set[str],
    base_manifests: dict[str, str],
    candidate_manifests: dict[str, str],
) -> list[Finding]:
    findings: list[Finding] = []
    for path in sorted(retired_paths):
        if not path.endswith((".cc", ".h", ".mm", ".mojom")):
            continue
        old_name = Path(path).name
        for manifest, token, quoted in possible_registration_entries(path):
            if not manifest_has_entry(
                base_manifests.get(manifest, ""), token, quoted=quoted
            ):
                continue
            candidate = candidate_manifests.get(manifest, "")
            if not manifest_has_entry(candidate, token, quoted=quoted):
                continue
            findings.append(
                Finding(
                    "P1",
                    f"Retired source {old_name} remains in an owning manifest",
                    manifest,
                    line_for(candidate, token),
                    f"Remove the stale {token} registration from {manifest}; "
                    "deleted and renamed source paths must retire their old "
                    "owning-manifest entries.",
                )
            )
    return findings


def scan_candidate_policy(
    paths: set[str],
    added: dict[str, dict[int, set[str]]],
    content_for: Callable[[str], str],
) -> list[Finding]:
    ordered_paths = sorted(paths)
    cpp_suffixes = {".cc", ".h", ".mm"}
    policy_content_paths = {
        path
        for path in ordered_paths
        if Path(path).suffix in cpp_suffixes
        or path == "scripts/sync.sh"
        or (path.startswith("scripts/") and "build" in Path(path).name)
    }
    contents = {
        path: content_for(path) if path in policy_content_paths else ""
        for path in ordered_paths
    }
    cpp_contents = {
        path: content
        for path, content in contents.items()
        if Path(path).suffix in cpp_suffixes
    }
    findings: list[Finding] = []
    for path in ordered_paths:
        lines = added.get(path, {})
        content = contents[path]
        related: list[str] = []
        source_path = Path(path)
        if source_path.suffix in cpp_suffixes:
            # Views subclasses are normally declared in foo.h and constructed
            # or given their BEGIN_METADATA block in foo.cc/.mm. Compare the
            # complete candidate versions of same-stem siblings, plus other
            # changed C++ files in the same owning directory.
            for other, other_content in cpp_contents.items():
                if other != path and Path(other).parent == source_path.parent:
                    related.append(other_content)
            for suffix in cpp_suffixes:
                sibling = str(source_path.with_suffix(suffix))
                if sibling == path or sibling in cpp_contents:
                    continue
                sibling_content = content_for(sibling)
                if sibling_content:
                    related.append(sibling_content)
        findings.extend(
            scan_views_metadata(path, lines, content, related)
        )
        findings.extend(scan_runtime_blocking(path, lines))
        findings.extend(scan_build_status(path, content))
        findings.extend(scan_sync_authority(path, content))
    return findings


def scan(
    repo: Path,
    target: str,
    base: str | None,
    commit: str,
) -> list[Finding]:
    if target == "local":
        index_added = parse_added_lines(local_snapshot_diff(repo, index=True))
        worktree_added = parse_added_lines(local_snapshot_diff(repo, index=False))
        add_untracked_lines(repo, worktree_added)
        snapshots = [
            (
                local_snapshot_paths(repo, index=True),
                index_added,
                lambda path: content_from_git(repo, f":{path}"),
            ),
            (
                local_snapshot_paths(repo, index=False),
                worktree_added,
                lambda path: content_at_tip(repo, path, "local", commit),
            ),
        ]
        findings: list[Finding] = []
        for paths, added, content_for in snapshots:
            findings.extend(
                scan_candidate_policy(paths, added, content_for)
            )
        base_manifests = base_registration_manifests(
            repo, "local", None, commit
        )
        for local_index in (True, False):
            candidate_manifests = registration_manifests(
                repo, "local", commit, local_index=local_index
            )
            created, retired = source_path_changes(
                repo,
                "local",
                None,
                commit,
                local_index=local_index,
            )
            findings.extend(
                scan_source_registration(created, candidate_manifests)
            )
            findings.extend(
                scan_retired_source_registration(
                    retired, base_manifests, candidate_manifests
                )
            )
        return list(dict.fromkeys(findings))

    added: dict[str, dict[int, set[str]]] = {}
    for stream in target_diff_lines(repo, target, base, commit):
        parsed = parse_added_lines(stream)
        for path, lines in parsed.items():
            destination = added.setdefault(path, {})
            for line, variants in lines.items():
                destination.setdefault(line, set()).update(variants)
    findings = []
    paths = autoreview.changed_paths(repo, target, base, commit)
    findings.extend(
        scan_candidate_policy(
            paths,
            added,
            lambda path: content_at_tip(repo, path, target, commit),
        )
    )
    candidate_manifests = registration_manifests(repo, target, commit)
    created, retired = source_path_changes(
        repo, target, base, commit
    )
    findings.extend(
        scan_source_registration(created, candidate_manifests)
    )
    findings.extend(
        scan_retired_source_registration(
            retired,
            base_registration_manifests(repo, target, base, commit),
            candidate_manifests,
        )
    )
    return list(dict.fromkeys(findings))


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Deterministic cmux-browser review checks."
    )
    parser.add_argument(
        "--mode", choices=("local", "branch", "commit"), required=True
    )
    parser.add_argument("--base")
    parser.add_argument("--commit", default="HEAD")
    args = parser.parse_args()
    if args.mode == "branch" and not args.base:
        parser.error("--mode branch requires --base")
    return args


def main() -> int:
    args = parse_args()
    repo = autoreview.repo_root()
    findings = scan(repo, args.mode, args.base, args.commit)
    if not findings:
        print("repo policy: clean")
        return 0
    print(f"repo policy findings: {len(findings)}")
    for finding in findings:
        print(
            f"[{finding.priority}] "
            f"{autoreview.safe_terminal_text(finding.title)}"
        )
        print(
            f"{autoreview.safe_terminal_text(finding.path)}:{finding.line}"
        )
        print(
            autoreview.safe_terminal_text(
                finding.body, preserve_newlines=True
            )
        )
        print()
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
