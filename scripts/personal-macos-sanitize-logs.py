#!/usr/bin/env python3
"""Preserve the personal tagged reload log and redact CI credentials from logs."""

import argparse
import json
import os
from pathlib import Path
import re
import shlex
from urllib.parse import quote, quote_plus


def secret_forms(environment):
    forms = set()
    for name, value in environment.items():
        if not value or not name.startswith(("GH_", "GITHUB_", "ACTIONS_")):
            continue
        if not re.search(r"TOKEN|SECRET|PASSWORD|CREDENTIAL|PRIVATE_KEY", name):
            continue
        forms.update((value, json.dumps(value)[1:-1], quote(value, safe=""), quote_plus(value)))
        forms.add(shlex.quote(value))
        forms.add(re.sub(r"([^A-Za-z0-9_./-])", r"\\\1", value))
    return sorted(forms, key=len, reverse=True)


def sanitize(text, secrets):
    # Xcode script phases print their inherited environment by default. These
    # rows add no compiler evidence, so omit them instead of keeping a partial
    # list of redacted variable names.
    text = "".join(
        line for line in text.splitlines(keepends=True)
        if not re.match(r"^\s*(?:export|declare\s+-x)\s+[A-Za-z_][A-Za-z0-9_]*(?:=|\s|$)", line)
    )
    for value in secrets:
        text = text.replace(value, "***")
    return text


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--log-dir", type=Path, default=Path("build/personal-macos"))
    parser.add_argument("--reload-log", type=Path, default=Path("/tmp/cmux-reload-steve-ssh.log"))
    args = parser.parse_args()
    args.log_dir.mkdir(parents=True, exist_ok=True)
    secrets = secret_forms(os.environ)
    # Copy through the sanitizer so an unredacted copy never enters artifacts.
    if args.reload_log.is_file():
        text = args.reload_log.read_text(errors="replace")
        (args.log_dir / "reload.log").write_text(sanitize(text, secrets))
    for log in args.log_dir.rglob("*.log"):
        if log.is_file() and not log.is_symlink():
            log.write_text(sanitize(log.read_text(errors="replace"), secrets))


if __name__ == "__main__":
    main()
