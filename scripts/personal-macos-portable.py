#!/usr/bin/env python3
"""Prepare a tagged personal app for relocation, preserving its entitlements."""

import argparse
import json
import os
from pathlib import Path
import plistlib
import subprocess


BUILD_ENVIRONMENT_KEYS = (
    "CMUXD_UNIX_PATH",
    "CMUXTERM_REPO_ROOT",
    "CMUX_BUNDLED_CLI_PATH",
    "CMUX_SHELL_INTEGRATION_DIR",
    "CMUX_REMOTE_DAEMON_ALLOW_LOCAL_BUILD",
    "CMUX_AUTH_CREDENTIALS_FILE",
    "CMUX_DEV_AUTH_PROFILE",
    "CMUX_DEV_AUTH_REPLACE_SESSION",
)


def signed_entitlements(app):
    result = subprocess.run(
        ["/usr/bin/codesign", "-d", "--entitlements", ":-", str(app)],
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    return plistlib.loads(result.stdout) if result.stdout.strip() else {}


def prepare(app, bundle_identifier):
    info_path = app / "Contents/Info.plist"
    with info_path.open("rb") as source:
        info = plistlib.load(source)
    if info.get("CFBundleIdentifier") != bundle_identifier:
        raise ValueError("Personal app bundle identifier does not match the build tag.")
    if not bundle_identifier.startswith("com.cmuxterm.app.debug."):
        raise ValueError("Only an isolated tagged Debug app can be prepared.")
    executable = app / "Contents/MacOS" / info["CFBundleExecutable"]
    cli = app / "Contents/Resources/bin/cmux"
    integration = app / "Contents/Resources/shell-integration"
    if not os.access(executable, os.X_OK) or not os.access(cli, os.X_OK) or not integration.is_dir():
        raise ValueError("Personal app is missing its executable, CLI, or shell integration.")
    # Validate the original seal before changing anything, including nested code.
    subprocess.run(["/usr/bin/codesign", "--verify", "--deep", "--strict", str(app)], check=True)
    entitlements = signed_entitlements(app)
    environment = info.setdefault("LSEnvironment", {})
    removed = [key for key in BUILD_ENVIRONMENT_KEYS if key in environment]
    for key in BUILD_ENVIRONMENT_KEYS:
        environment.pop(key, None)
    environment["CMUX_SOCKET_MODE"] = "cmuxOnly"
    # Tagged sockets and logs live under /tmp. Home/worktree paths must not
    # escape into the archive through a future reload environment addition.
    for key, value in environment.items():
        if isinstance(value, str) and ("/Users/" in value or "/home/" in value):
            raise ValueError(f"Personal app still contains a machine-specific environment path: {key}")
    with info_path.open("wb") as output:
        plistlib.dump(info, output, fmt=plistlib.FMT_BINARY, sort_keys=False)
    # Match upstream's rpath relocation signer. Sign only the enclosing app;
    # --deep signing would replace nested helpers' independent identities.
    subprocess.run(
        [
            "/usr/bin/codesign", "--force", "--sign", "-", "--timestamp=none",
            "--generate-entitlement-der",
            "--preserve-metadata=identifier,entitlements,flags,runtime", str(app),
        ],
        check=True,
    )
    if signed_entitlements(app) != entitlements:
        raise ValueError("Personal app entitlements changed while preparing its archive.")
    subprocess.run(["/usr/bin/codesign", "--verify", "--deep", "--strict", str(app)], check=True)
    return {
        "portableBundle": True,
        "socketMode": environment["CMUX_SOCKET_MODE"],
        "removedBuildEnvironmentKeys": removed,
        "entitlementsPreserved": True,
        "legacyRemoteDaemonManifestEmbedded": bool(info.get("CMUXRemoteDaemonManifestJSON")),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("app", type=Path)
    parser.add_argument("bundle_identifier")
    parser.add_argument("--receipt", type=Path, required=True)
    args = parser.parse_args()
    receipt = prepare(args.app.resolve(), args.bundle_identifier)
    args.receipt.write_text(json.dumps(receipt, indent=2) + "\n")


if __name__ == "__main__":
    main()
