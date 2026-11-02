#!/usr/bin/env python3
"""Enable or disable login startup for the isolated personal cmux build."""

import argparse
from pathlib import Path
import os
import plistlib
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    action = parser.add_mutually_exclusive_group(required=True)
    action.add_argument("--enable", metavar="APP", type=Path)
    action.add_argument("--disable", action="store_true")
    args = parser.parse_args()
    label = "com.steve.cmux-ssh.login"
    domain = f"gui/{os.getuid()}"
    target = Path.home() / "Library/LaunchAgents" / f"{label}.plist"
    if args.enable:
        app = args.enable.expanduser().resolve(strict=True)
        with (app / "Contents/Info.plist").open("rb") as source:
            info = plistlib.load(source)
        if info.get("CFBundleIdentifier") != "com.cmuxterm.app.debug.steve-ssh":
            parser.error("Expected the isolated steve-ssh app.")
        # No KeepAlive: quitting the app stays effective until the next login.
        payload = {
            "Label": label,
            "ProgramArguments": ["/usr/bin/open", "-g", "-a", str(app)],
            "RunAtLoad": True,
            "LimitLoadToSessionType": "Aqua",
        }
        target.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(["launchctl", "bootout", f"{domain}/{label}"], capture_output=True)
        target.write_bytes(plistlib.dumps(payload))
        target.chmod(0o644)
        subprocess.run(["launchctl", "bootstrap", domain, str(target)], check=True)
        print(f"Login startup enabled: {app}")
    else:
        subprocess.run(["launchctl", "bootout", f"{domain}/{label}"], capture_output=True)
        target.unlink(missing_ok=True)
        print("Personal cmux login startup disabled.")


if __name__ == "__main__":
    main()
