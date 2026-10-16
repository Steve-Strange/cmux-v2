#!/usr/bin/env python3
"""End-to-end tests for archive metadata and signed multi-platform feeds."""

from __future__ import annotations

import base64
import json
import os
from pathlib import Path
import re
import stat
import subprocess
import sys
import tempfile
import time
import zipfile

ROOT = Path(__file__).resolve().parent.parent


def run(*args: object, **kwargs: object) -> subprocess.CompletedProcess[bytes]:
    return subprocess.run([str(arg) for arg in args], check=True, **kwargs)


def main() -> int:
    # Keep the checked-in PEM and the exact DER bytes compiled into Chromium in
    # lockstep. If the local private key exists, prove it derives that public
    # key too without ever printing or copying the secret.
    service_source = (
        ROOT / "overlay/chrome/browser/cmux_term/cmux_update_service.cc"
    ).read_text(encoding="utf-8")
    key_block = service_source.split("kUpdatePublicKey = {", 1)[1].split("};", 1)[0]
    embedded_der = bytes(int(value, 16) for value in re.findall(r"0x([0-9a-f]{2})", key_block))
    checked_in_der = subprocess.run(
        ["openssl", "pkey", "-pubin", "-in",
         str(ROOT / "docs/update-public-key.pem"), "-outform", "DER"],
        check=True,
        stdout=subprocess.PIPE,
    ).stdout
    assert embedded_der == checked_in_der
    production_private = ROOT / ".release-keys/cmux-update-private.pem"
    if production_private.is_file():
        derived_der = subprocess.run(
            ["openssl", "ec", "-in", str(production_private), "-pubout",
             "-outform", "DER"],
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
        ).stdout
        assert derived_der == checked_in_der

    if os.name == "nt":
        # Execute the exact PowerShell raw string compiled into Chromium. The
        # POSIX generated helper is executed by cmux_update_script_test.cc.
        script_source = (
            ROOT / "overlay/chrome/browser/cmux_term/cmux_update_script.cc"
        ).read_text(encoding="utf-8")
        windows_script = script_source.split('return R"POWERSHELL(', 1)[1]
        windows_script = windows_script.split(')POWERSHELL";', 1)[0]
        with tempfile.TemporaryDirectory() as raw_install_tmp:
            install_tmp = Path(raw_install_tmp)
            current = install_tmp / "current"
            staged = install_tmp / "staged"
            marker = install_tmp / "relaunched.txt"
            current.mkdir()
            staged.mkdir()
            (current / "old.txt").write_text("old\n", encoding="utf-8")
            (staged / "new.txt").write_text("new\n", encoding="utf-8")
            (staged / "cmux-browser.cmd").write_text(
                f'@echo off\r\necho relaunched>"{marker}"\r\n',
                encoding="utf-8",
            )
            apply_script = install_tmp / "apply.ps1"
            apply_script.write_text(windows_script, encoding="utf-8")
            run(
                "powershell.exe", "-NoProfile", "-NonInteractive",
                "-ExecutionPolicy", "Bypass", "-File", apply_script,
                "-ParentPid", "2147483647", "-Current", current,
                "-Staged", staged, "-Relaunch", "cmux-browser.cmd",
            )
            # Start-Process is intentionally asynchronous. Hosted Windows
            # runners can take several seconds to schedule the relaunched
            # command even though the atomic directory swap is already done.
            for _ in range(200):
                if marker.exists():
                    break
                time.sleep(0.05)
            assert (current / "new.txt").is_file()
            assert not (current / "old.txt").exists()
            assert marker.is_file()
            assert not list(install_tmp.glob("current.cmux-old.*"))

    with tempfile.TemporaryDirectory() as raw_tmp:
        tmp = Path(raw_tmp)
        private = tmp / "private.pem"
        public = tmp / "public.pem"
        run("openssl", "ecparam", "-name", "prime256v1", "-genkey", "-noout",
            "-out", private)
        run("openssl", "ec", "-in", private, "-pubout", "-out", public,
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

        specs = {
            # Match the shipped .app layout. The updater validates this exact
            # path after extraction before it offers the install action.
            "mac-arm64": ("cmux-browser.app", "Contents/MacOS/cmux"),
            "windows-x64": ("cmux-browser", "cmux-browser.exe"),
            "linux-x64": ("cmux-browser", "cmux-browser"),
        }
        artifacts: list[tuple[str, Path, str, str]] = []
        for platform, (archive_root, executable) in specs.items():
            source = tmp / platform / archive_root
            binary = source / executable
            binary.parent.mkdir(parents=True)
            binary.write_text(f"{platform} executable\n", encoding="utf-8")
            binary.chmod(0o755)
            link = source / "current"
            try:
                link.symlink_to(executable)
            except OSError:
                # Windows without Developer Mode cannot create symlinks; the
                # executable/permissions and signature coverage still run.
                pass
            archive = tmp / f"cmux-{platform}.zip"
            run(sys.executable, ROOT / "scripts/build-update-archive.py",
                "--input", source, "--output", archive)
            artifacts.append((platform, archive, archive_root, executable))

            with zipfile.ZipFile(archive) as zipped:
                info = zipped.getinfo(f"{archive_root}/{executable}")
                archived_mode = info.external_attr >> 16
                # NTFS/Python cannot represent a POSIX executable bit on the
                # synthetic macOS/Linux inputs. Their real packaging path and
                # mode preservation are exercised on macOS and Linux runners.
                if os.name != "nt" and platform != "windows-x64":
                    assert archived_mode & stat.S_IXUSR

        manifest = tmp / "update.json"
        command: list[object] = [
            sys.executable,
            ROOT / "scripts/package-update.py",
            "--version", "139.0.0.0",
            "--base-url", "https://updates.example.test/cmux",
            "--private-key", private,
            "--output", manifest,
        ]
        for platform, archive, archive_root, executable in artifacts:
            command.extend([
                "--artifact",
                f"{platform}={archive},{archive_root},{executable}",
            ])
        run(*command)

        envelope = json.loads(manifest.read_text(encoding="utf-8"))
        payload = base64.b64decode(envelope["payload"], validate=True)
        signature = base64.b64decode(envelope["signature"], validate=True)
        signature_path = tmp / "signature.der"
        payload_path = tmp / "payload.json"
        signature_path.write_bytes(signature)
        payload_path.write_bytes(payload)
        run("openssl", "dgst", "-sha256", "-verify", public,
            "-signature", signature_path, payload_path,
            stdout=subprocess.DEVNULL)

        decoded = json.loads(payload)
        assert decoded["schema"] == 1
        assert decoded["version"] == "139.0.0.0"
        assert set(decoded["platforms"]) == set(specs)
        for platform, archive, archive_root, executable in artifacts:
            entry = decoded["platforms"][platform]
            assert entry["archive_root"] == archive_root
            assert entry["executable"] == executable
            assert int(entry["size"]) == archive.stat().st_size
            assert len(entry["sha256"]) == 64

        tampered = bytearray(payload)
        tampered[-2] ^= 1
        payload_path.write_bytes(tampered)
        invalid = subprocess.run(
            ["openssl", "dgst", "-sha256", "-verify", str(public),
             "-signature", str(signature_path), str(payload_path)],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        assert invalid.returncode != 0, "tampered payload unexpectedly verified"

    print("test-updater: archive + signed feed checks passed for mac/windows/linux")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
