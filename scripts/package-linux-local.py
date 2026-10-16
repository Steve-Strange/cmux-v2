#!/usr/bin/env python3
"""Package a verified local Linux build without stamping an official release."""

import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import shutil
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-root", type=Path, required=True)
    parser.add_argument("--runtime", type=Path, required=True,
                        help="Existing official installation supplying the pinned backend")
    args = parser.parse_args()
    project = Path(__file__).resolve().parent.parent
    build = args.build_root.resolve()
    out = build / "chromium/src/out/Release"
    runtime = args.runtime.resolve()
    source = json.loads((project / "SOURCE-MANIFEST.json").read_text())
    expected = {d["name"]: d["commit"] for d in source["dependencies"]}
    for filename, revision in (("cmux-tui.REVISION", expected["cmux and cmux-tui"]),
                               ("cmux-tui.GHOSTTY_REVISION", expected["Ghostty"])):
        if (runtime / filename).read_text().strip() != revision:
            raise RuntimeError("Runtime revision mismatch: " + filename)
    required = {
        "chrome": "chrome", "chrome_crashpad_handler": "chrome_crashpad_handler",
        "chrome_sandbox": "chrome-sandbox", "chrome_management_service": "chrome-management-service",
        "resources.pak": "resources.pak", "icudtl.dat": "icudtl.dat",
        "chrome_100_percent.pak": "chrome_100_percent.pak",
        "chrome_200_percent.pak": "chrome_200_percent.pak",
        "v8_context_snapshot.bin": "v8_context_snapshot.bin",
    }
    for filename in required:
        if not (out / filename).is_file():
            raise RuntimeError("Build output missing: " + filename)
    for filename in ("locales/zh-CN.pak", "locales/en-US.pak", "terminfo/x/xterm-ghostty"):
        if not (out / filename).is_file():
            raise RuntimeError("Runtime output missing: " + filename)
    credits = out / "gen/components/resources/about_credits.html"
    if not credits.is_file() or "To get correct credits page" in credits.read_text():
        raise RuntimeError("Generated Chromium license credits missing")
    package = build / "package"
    package.mkdir(exist_ok=True)
    destination = package / "cmux-browser"
    if destination.exists() and not (destination / "cmux-local-build.json").is_file():
        raise RuntimeError("Refusing to replace an unrecognized package directory")
    with tempfile.TemporaryDirectory(prefix=".local-stage-", dir=package) as scratch:
        stage = Path(scratch) / "cmux-browser"
        stage.mkdir()
        stripped = []
        for filename, target in required.items():
            shutil.copy2(out / filename, stage / target)
            if filename in ("chrome", "chrome_crashpad_handler", "chrome_sandbox", "chrome_management_service"):
                (stage / target).chmod(0o755)
                stripped.append(stage / target)
        for filename in ("libqt5_shim.so", "libqt6_shim.so", "libvk_swiftshader.so",
                         "libvulkan.so.1", "vk_swiftshader_icd.json"):
            if (out / filename).is_file():
                shutil.copy2(out / filename, stage / filename)
                if ".so" in filename:
                    stripped.append(stage / filename)
        shutil.copytree(out / "locales", stage / "locales")
        for dirname in ("MEIPreload", "PrivacySandboxAttestationsPreloaded", "IwaKeyDistribution",
                        "ghostty", "terminfo"):
            if (out / dirname).is_dir():
                shutil.copytree(out / dirname, stage / dirname, symlinks=True)
        # Public Linux resources ship an empty themes directory. Include two
        # small presets so the Appearance picker works in the local package.
        for theme in (project / "packaging/linux/themes").iterdir():
            for resources in (stage, out):
                (resources / "ghostty/themes").mkdir(parents=True, exist_ok=True)
                shutil.copy2(theme, resources / "ghostty/themes" / theme.name)
        for filename in ("cmux-tui", "cmux-tui.REVISION", "cmux-tui.GHOSTTY_REVISION",
                         "cmux-tui.GHOSTTY_RESOURCES_MANIFEST.sha256"):
            shutil.copy2(runtime / filename, stage / filename)
        for path in runtime.glob("product_logo_*.png"):
            shutil.copy2(path, stage / path.name)
        for dirname in ("cmux-licenses", "extensions"):
            if (runtime / dirname).is_dir():
                shutil.copytree(runtime / dirname, stage / dirname, symlinks=True)
        (stage / "cmux-licenses").mkdir(exist_ok=True)
        shutil.copy2(credits, stage / "cmux-licenses/Chromium-about-credits.html")
        strip = build / "chromium/src/third_party/llvm-build/Release+Asserts/bin/llvm-strip"
        for path in stripped:
            subprocess.run([str(strip), "--strip-debug", str(path)], check=True)
        version = dict(line.split("=", 1) for line in
                       (build / "chromium/src/chrome/VERSION").read_text().splitlines())
        metadata = {
            "kind": "local Linux repair build, not an official release",
            "created_at_utc": datetime.now(timezone.utc).isoformat(),
            "public_source_revision": source["source"]["commit"],
            "chromium_version": ".".join(version[k] for k in ("MAJOR", "MINOR", "BUILD", "PATCH")),
            "source_worktree": str(project),
            "source_base_commit": subprocess.check_output(
                ["git", "-C", str(project), "rev-parse", "HEAD"], text=True).strip(),
            "local_changes_present": bool(subprocess.check_output(
                ["git", "-C", str(project), "status", "--porcelain"], text=True).strip()),
            "frontend_ghostty_revision": expected["Ghostty"],
            "reused_backend_revision": expected["cmux and cmux-tui"],
            "gn_args": (out / "args.gn").read_text(),
            "official_updates_disabled": True,
            "repair_notes": str(project / "docs/linux-repair.md"),
        }
        metadata_text = json.dumps(metadata, indent=2) + "\n"
        (stage / "cmux-local-build.json").write_text(metadata_text)
        # Protect direct launches from the build tree as well as the package.
        (out / "cmux-local-build.json").write_text(metadata_text)
        if destination.exists():
            shutil.rmtree(destination)
        shutil.move(stage, destination)
    print(destination)


if __name__ == "__main__":
    main()
