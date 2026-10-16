#!/usr/bin/env python3
"""Fast host-side invariants for Windows/Linux release automation."""

from __future__ import annotations

import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parent.parent


def check(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> int:
    spec = importlib.util.spec_from_file_location(
        "release_version", ROOT / "scripts/release_version.py"
    )
    check(spec is not None and spec.loader is not None, "version module loads")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    stamp_spec = importlib.util.spec_from_file_location(
        "stamp_release_build", ROOT / "scripts/stamp-release-build.py"
    )
    check(stamp_spec is not None and stamp_spec.loader is not None,
          "release-build stamp module loads")
    stamp_module = importlib.util.module_from_spec(stamp_spec)
    stamp_spec.loader.exec_module(stamp_module)

    check(module.parse_version("151.0.7922.42") == (151, 0, 7922, 42),
          "four-part release version parses")
    for invalid in ("151.0.7922", "v151.0.7922.1", "151.0.7922.-1",
                    "151.0.7922.65536"):
        try:
            module.parse_version(invalid)
        except ValueError:
            pass
        else:
            raise AssertionError(f"invalid version accepted: {invalid}")

    with tempfile.TemporaryDirectory() as raw_tmp:
        tmp = Path(raw_tmp)
        repository = tmp / "repository"
        source = tmp / "chromium"
        (repository / "scripts").mkdir(parents=True)
        (source / "chrome").mkdir(parents=True)
        (repository / ".chromium-version").write_text(
            "151.0.7922.34\n", encoding="utf-8"
        )
        (source / "chrome/VERSION").write_text(
            "MAJOR=151\nMINOR=0\nBUILD=7922\nPATCH=34\n", encoding="utf-8"
        )
        subprocess.run(
            [sys.executable, ROOT / "scripts/release_version.py",
             "--repository", repository, "--chromium-src", source,
             "--version", "151.0.7922.35"],
            check=True,
        )
        check((source / "chrome/VERSION").read_text(encoding="utf-8").endswith(
            "PATCH=35\n"), "release version is written to Chromium")
        rejected = subprocess.run(
            [sys.executable, ROOT / "scripts/release_version.py",
             "--repository", repository, "--chromium-src", source,
             "--version", "151.0.7922.34"],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        check(rejected.returncode != 0, "baseline and downgrade releases fail")

    with tempfile.TemporaryDirectory() as raw_tmp:
        chromium_src = Path(raw_tmp) / "chromium"
        stamp_header = chromium_src / stamp_module.RELATIVE_HEADER
        stamp_crashpad = chromium_src / stamp_module.RELATIVE_CRASHPAD_CORE
        stamp_header.parent.mkdir(parents=True)
        stamp_crashpad.parent.mkdir(parents=True)
        (chromium_src / "chrome/VERSION").write_text(
            "MAJOR=151\nMINOR=0\nBUILD=7922\nPATCH=34\n", encoding="utf-8"
        )
        checked_in_header = (
            ROOT
            / "overlay/chrome/browser/cmux_term/cmux_release_build.h"
        ).read_text(encoding="utf-8")
        stamp_header.write_text(checked_in_header, encoding="utf-8")
        stamp_crashpad.write_text(
            stamp_module.CRASHPAD_UNSTAMPED + "\n", encoding="utf-8"
        )
        check(stamp_module.UNSTAMPED in checked_in_header,
              "checked-in developer builds default telemetry off")
        check(stamp_module.stamp_release_build(chromium_src),
              "release pipeline stamps its disposable Chromium overlay")
        check(stamp_module.STAMPED in stamp_header.read_text(encoding="utf-8"),
              "release-build stamp is compiled into the release")
        check(stamp_module.CRASHPAD_STAMPED in
              stamp_crashpad.read_text(encoding="utf-8"),
              "pre-handler crash consent receives the same release stamp")
        check(not stamp_module.stamp_release_build(chromium_src),
              "release-build stamping is idempotent")

    reusable = (ROOT / ".github/workflows/release-build.yml").read_text()
    nightly = (ROOT / ".github/workflows/nightly.yml").read_text()
    stable = (ROOT / ".github/workflows/release.yml").read_text()
    linux_nightly = (
        ROOT / ".github/workflows/release-linux-nightly.yml"
    ).read_text()
    linux_publish = (
        ROOT / ".github/workflows/publish-linux-nightly.yml"
    ).read_text()
    linux_publish_script = (
        ROOT / "scripts/publish-linux-nightly-release.sh"
    ).read_text()
    linux_release_state = (
        ROOT / "scripts/linux_nightly_release_state.py"
    ).read_text()
    linux_publish_verifier = (
        ROOT / "scripts/verify-linux-nightly-publication.py"
    ).read_text()
    updater = (ROOT / ".github/workflows/updater.yml").read_text()
    service = (ROOT / "overlay/chrome/browser/cmux_term/cmux_update_service.cc").read_text()
    telemetry = (ROOT / "overlay/chrome/browser/cmux_term/cmux_telemetry.cc").read_text()
    config = (ROOT / "overlay/chrome/browser/cmux_term/cmux_layout_config.cc").read_text()
    windows = (ROOT / "scripts/build-release-windows.ps1").read_text()
    linux = (ROOT / "scripts/build-release-linux.sh").read_text()
    linux_bootstrap = (
        ROOT / "scripts/bootstrap-release-linux.sh"
    ).read_text()
    ghostty_vendor = (
        ROOT / "scripts/vendor-ghostty-linux-local.sh"
    ).read_text()
    ghostty_vendor_legacy = (
        ROOT / "scripts/build-ghostty-linux.sh"
    ).read_text()
    cmux_tui_build = (
        ROOT / "scripts/build-cmux-tui.sh"
    ).read_text()
    linux_installer = (
        ROOT / "scripts/build-linux-user-installer.sh"
    ).read_text()
    macos = (ROOT / "scripts/build-release-macos.sh").read_text()
    macos_hq = (ROOT / "scripts/build-release-macos-hq.sh").read_text()
    release_source = (ROOT / "scripts/build-release-source.sh").read_text()
    linux_smoke = (
        ROOT / ".github/workflows/release-linux-smoke.yml"
    ).read_text()
    linux_terminal_pane = (
        ROOT / "overlay/chrome/browser/cmux_term/cmux_terminal_pane_linux.cc"
    ).read_text()
    cmux_tab_strip = (
        ROOT / "overlay/chrome/browser/cmux_term/cmux_tab_strip.cc"
    ).read_text()
    cmux_extension_strip = (
        ROOT / "overlay/chrome/browser/cmux_term/cmux_extension_strip.cc"
    ).read_text()
    cmux_views = (
        ROOT / "overlay/chrome/browser/cmux_term/cmux_views.cc"
    ).read_text()
    linux_compliance = (
        ROOT / "scripts/generate-linux-release-compliance.py"
    ).read_text()
    release_provenance = (
        ROOT / "scripts/generate-release-provenance.py"
    ).read_text()
    sentry_linux = (ROOT / "scripts/ensure-sentry-cli.sh").read_text()
    sentry_windows = (ROOT / "scripts/ensure-sentry-cli.ps1").read_text()
    sentry_crashpad = (ROOT / "patches/sentry_crashpad.py").read_text()
    extensions_posix = (ROOT / "scripts/fetch-extensions.sh").read_text()
    extensions_windows = (ROOT / "scripts/fetch-extensions.ps1").read_text()
    release_docs = (ROOT / "docs/releases.md").read_text()

    check("blacksmith-32vcpu-ubuntu-2404" in reusable and
          "useblacksmith/stickydisk@v1" in reusable and
          "hashFiles('.chromium-version')" in reusable and
          "./scripts/bootstrap-release-linux.sh" in reusable and
          "mlugg/setup-zig@d1434d08867e3ee9daa34448df10607b98908d29" in
          reusable,
          "Linux releases use the revision-keyed Blacksmith build disk")
    check("workflow_dispatch:" in linux_nightly and
          "blacksmith-32vcpu-ubuntu-2404" in linux_nightly and
          "CMUX_UPDATE_PRIVATE_KEY_B64" in linux_nightly and
          "--artifact linux-x64=" in linux_nightly and
          "cmux-browser-source-*.tar.zst" in linux_nightly and
          "cmux-linux-nightly-release" in linux_nightly,
          "Linux-only nightly candidates produce a signed publishable bundle")
    check(
        "workflow_dispatch:" in linux_publish
        and all(
            f"{name}:" in linux_publish
            for name in (
                "source_run_id",
                "smoke_run_id",
                "expected_version",
                "expected_source_sha",
            )
        )
        and "environment: release" in linux_publish
        and "secrets.CMUX_RELEASE_REPO_TOKEN" in linux_publish
        and "actions: read" in linux_publish
        and "contents: read" in linux_publish
        and "actions/runs/$SOURCE_RUN_ID" in linux_publish
        and "actions/runs/$SMOKE_RUN_ID" in linux_publish
        and "compare/${smoke_workflow_sha}...${GITHUB_SHA}" in linux_publish
        and 'os.environ["GITHUB_REF"] == "refs/heads/main"' in linux_publish
        and "run-records/source.json" in linux_publish
        and "run-records/smoke.json" in linux_publish
        and "name: cmux-linux-nightly-release" in linux_publish
        and "name: cmux-linux-nightly-smoke-receipt" in linux_publish
        and "verify-linux-nightly-publication.py" in linux_publish,
        "manual Linux publication binds successful API runs to one smoke receipt",
    )
    check(
        '"draft": True' in linux_publish_script
        and "release.get(\"draft\") is not True" in linux_publish_script
        and "linux_nightly_release_state.py" in linux_publish_script
        and 'tag == "nightly"' in linux_release_state
        and 'release.get("prerelease") is not True' in linux_release_state
        and "is_linux_nightly_title" in linux_release_state
        and "the existing nightly tag is not a prerelease" in
        linux_release_state
        and "old-asset-ids" in linux_publish_script
        and '"${non_update_assets[@]}"' in linux_publish_script
        and '"$update_asset"' in linux_publish_script
        and linux_publish_script.index('"${non_update_assets[@]}"')
        < linux_publish_script.index('"$update_asset"',
                                     linux_publish_script.index(
                                         '"${non_update_assets[@]}"'))
        and "hidden release final inventory mismatch" in linux_publish_script
        and "release asset size mismatch" in linux_publish_script
        and linux_publish_script.index("hidden release final inventory mismatch")
        < linux_publish_script.index(
            "{\"draft\":false,\"prerelease\":true"
        ),
        "Linux publication remains draft until exact assets land with feed last",
    )
    check(
        'pax_headers.get("comment") == source_sha' in linux_publish_verifier
        and "MIN_SOURCE_REGULAR_FILES = 300" in linux_publish_verifier
        and "corresponding source omits required rebuild files" in
        linux_publish_verifier
        and all(
            path in linux_publish_verifier
            for path in (
                ".chromium-version",
                "docs/releases.md",
                "scripts/apply.sh",
                "scripts/bootstrap-release-linux.sh",
                "scripts/build-release-linux.sh",
                "scripts/build-release-source.sh",
                "scripts/vendor-ghostty-linux-local.sh",
                "overlay/chrome/browser/cmux_term/BUILD.gn",
                "patches/cmux-pinned-toolbar-actions-chromium-151.patch",
                "patches/helium-media-toolbar.patch",
                "patches/helium-new-tab.patch",
                "patches/helium-omnibar-chromium-151.patch",
                "patches/helium-settings-chromium-151.patch",
            )
        ),
        "publication proves the source archive is a rebuildable Git snapshot",
    )
    check(
        all(
            "linux-compile:" in workflow
            and "CMUX_RELEASE_PHASE: all" in workflow
            and "Compile and package Linux x64 while preserving build cache"
            in workflow
            and "linux-x64-status" in workflow
            and "Fail closed after the build disk is committed" in workflow
            and "steps.linux_build.outputs.package_ok == 'true'" in workflow
            and "steps.sentry_upload.outcome" in workflow
            and "steps.linux_artifact.outcome" in workflow
            and "linux-x64-gate-status" in workflow
            and "Mount compiled revision-keyed Chromium build disk"
            not in workflow
            for workflow in (linux_nightly, reusable)
        )
        and "needs: version" in linux_nightly
        and "linux-prepare:" not in linux_nightly
        and "needs: [validate, linux-compile]" in reusable
        and "linux-release-compile-v1:" in linux
        and "Linux release compile receipt is missing or stale" in linux
        and 'autoninja -C "$SRC/out/Release" chrome' in linux
        and "chrome/installer/linux:installer_deps" in linux
        and "chrome/installer/linux:merge_deb_dependencies" in linux
        and "compute_build_timestamp.py" in linux
        and "chrome/installer/linux/debian/build.py" in linux
        and "chrome/installer/linux:stable_deb" not in linux
        and 'cmux-browser-stable_${VERSION}-1_amd64.deb' in linux
        and 'chromium-browser-stable_${VERSION}-1_amd64.deb' not in linux
        and 'test -s "$deb"' in linux,
        "Linux compilation and packaging share the cache-owning runner",
    )
    linux_packaging_tools = (
        "binutils",
        "devscripts",
        "dpkg-dev",
        "fakeroot",
        "file",
        "gzip",
        "xz-utils",
    )
    check(
        all(tool in linux_nightly for tool in linux_packaging_tools)
        and all(tool in reusable for tool in linux_packaging_tools)
        and "Install Linux packaging tools" in linux_nightly
        and "Install Linux packaging tools" in reusable,
        "Linux binary runners install Chromium's complete Debian toolchain",
    )
    check("generate_about_credits = true" in linux_bootstrap and
          "GHOSTTY_BUILD_KEY=" in linux_bootstrap and
          "GHOSTTY_ZIG_CACHE_DIR=" in linux_bootstrap and
          "CMUX_TUI_ZIG_CACHE_DIR=" in linux_bootstrap and
          'ZIG_GLOBAL_CACHE_DIR="$GHOSTTY_ZIG_CACHE_DIR"' in
          linux_bootstrap and
          'ZIG_GLOBAL_CACHE_DIR="$CMUX_TUI_ZIG_CACHE_DIR"' in
          linux_bootstrap and
          'ZIG_GLOBAL_CACHE_DIR="$BUILD_ROOT/zig-cache/$GHOSTTY_REVISION"'
          not in linux_bootstrap and
          "prepare-ghostty-linux-pic.py" in linux_bootstrap and
          "zig build" in linux_bootstrap and
          "vendor-ghostty-linux-local.sh" in linux_bootstrap and
          "CMUX_TUI_REVISION=" in linux_bootstrap and
          "https://github.com/manaflow-ai/cmux.git" in linux_bootstrap and
          "submodule update --init --depth 1 ghostty" in linux_bootstrap and
          "build-cmux-tui.sh" in linux_bootstrap and
          "Rust 1.96.0" in linux_bootstrap and
          "RUST_COMPILER=" in linux_bootstrap and
          "-Dapp-runtime=none" in cmux_tui_build and
          "-Demit-exe=false" in cmux_tui_build and
          "CMUX_TUI_RESOURCE_PROFILE" in cmux_tui_build and
          "terminfo-only" in cmux_tui_build and
          '"-Demit-themes=$EMIT_THEMES"' in cmux_tui_build and
          "CMUX_TUI_RESOURCE_PROFILE=terminfo-only" in linux_bootstrap and
          "contains an unverified Ghostty theme or shell-integration resource"
          in linux_bootstrap and
          "-Di18n=false" in cmux_tui_build and
          "CMUX_GHOSTTY_VT_ZIG_CPU=baseline" in cmux_tui_build and
          "--copy-links" in cmux_tui_build and
          "collect-ghostty-licenses.py" in ghostty_vendor and
          all(
              "nm -g --undefined-only -P" in script
              and "nm -g --defined-only -P" in script
              and "symbol_prefix=cmux_ghostty_" in script
              and "objcopy --prefix-symbols=" in script
              and "objcopy --redefine-syms=" in script
              and "ghostty-public.before" in script
              and "ghostty-public.after" in script
              and "cmp -s" in script
              and "objcopy --wildcard --keep-global-symbol" not in script
              for script in (ghostty_vendor, ghostty_vendor_legacy)
          ),
          "fresh Linux release disks build pinned Ghostty and cmux-tui inputs")
    check("test-prepare-ghostty-linux-pic.py" in updater and
          "test-ghostty-linux-symbol-isolation.sh" in updater,
          "release CI verifies fail-closed Ghostty PIC and symbol isolation")
    check("self-hosted, Windows, X64, cmux-release" in reusable,
          "Windows release runner contract is present")
    check("self-hosted, macOS, ARM64, cmux-release" in reusable and
          "./scripts/build-release-macos-hq.sh" in reusable and
          '"$HQ" monitor-build' in macos_hq and
          '"$HQ" run' in macos_hq and
          "--sync" in macos_hq,
          "macOS releases compile only through a monitored HQ lease")
    check("Developer ID Application:" in macos and
          "spctl --assess --type execute" in macos and
          "xcrun stapler validate" in macos and
          "build-update-archive.py" in macos and
          "hdiutil create" in macos,
          "macOS packaging requires native trust checks and emits DMG plus ZIP")
    check("notarize-release-macos.sh" in macos_hq and
          "CMUX_SIGN_PROVISION_PROFILE" in macos_hq and
          "CMUX_NOTARYTOOL_KEYCHAIN_PROFILE" in macos_hq,
          "macOS HQ release requires signing, entitlement, and notarization inputs")
    check("CMUX_UPDATE_PRIVATE_KEY_B64" in reusable,
          "manifest signing key is mandatory")
    check("CMUX_RELEASE_REPO_TOKEN" in reusable and
          "CMUX_DISTRIBUTION_REPOSITORY: manaflow-ai/cmux-v2" in reusable and
          "secrets.CMUX_RELEASE_REPO_TOKEN" in reusable,
          "publication uses a scoped token for the public distribution repo")
    check("CMUX_RELEASE_COMPLIANCE_READY == 'true'" in reusable,
          "public upload remains gated on the independent compliance review")
    check("test -s release-compliance/THIRD_PARTY_NOTICES.html" in reusable and
          "test -s release-compliance/third-party-notices.spdx.json" in reusable and
          "name: cmux-compliance" in reusable and
          "needs: [compliance, linux-x64, macos-arm64, source, windows-x64]" in
          reusable and
          "release/THIRD_PARTY_NOTICES.html" in reusable and
          "release/third-party-notices.spdx.json" in reusable,
          "publication fails closed without reviewed human and machine notices")
    check("build-release-source.sh" in reusable and
          "cmux-browser-source-*.tar.zst" in reusable and
          "corresponding-source.json" in reusable and
          "git -C \"$ROOT\" archive" in release_source and
          "SOURCE-MANIFEST.json" in release_source and
          '\"license\": \"GPL-3.0-only\"' in release_source and
          '\"cmux_authored_material\": \"GPL-3.0-or-later\"' in
          release_source and
          'receipt[\"license\"] == \"GPL-3.0-only\"' in linux_smoke and
          '(\"cmux Browser\", \"GPL-3.0-only\")' in linux_compliance and
          '(\"cmux-authored material\", \"GPL-3.0-or-later\")' in
          linux_compliance,
          "every binary release carries its exact private-repository source")
    check("generate-linux-release-compliance.py" in linux and
          "Chromium-about-credits.html" in linux and
          "third-party-notices.spdx.json" in linux and
          "dependency-licenses" in linux,
          "Linux packages embed generated Chromium and Ghostty notices")
    check("generate-release-provenance.py" in reusable and
          "provenance.intoto.jsonl" in reusable and
          "https://in-toto.io/Statement/v1" in release_provenance and
          "https://slsa.dev/provenance/v1" in release_provenance,
          "release assets receive checksummed in-toto provenance")
    check("SENTRY_AUTH_TOKEN" in reusable and
          reusable.count("SENTRY_ORG: aurora-7k") == 2 and
          reusable.count("SENTRY_PROJECT: cmux-browser") == 2,
          "both release builders upload symbols to the browser Sentry project")
    check("organization `aurora-7k`" in release_docs and
          "Store Minidumps As Attachments" in release_docs and
          "organization `manaflow`" not in release_docs,
          "release docs match the configured Sentry project and retention")
    check("debug-files upload" in reusable and
          "Missing browser debug files" in reusable and
          "'chrome.exe.pdb'" in reusable and
          "'chrome.dll.pdb'" in reusable and
          "'chrome_elf.dll.pdb'" in reusable,
          "release jobs require uploadable Linux and Windows debug files")
    check(
        'chromium_src="${CHROMIUM_SRC:-/opt/cmux/chromium/src}"' in reusable and
        "'C:\\cr\\src'" in reusable,
        "symbol uploads preserve the release scripts' Chromium source defaults"
    )
    check("3.3.0" in sentry_linux and "3.3.0" in sentry_windows and
          "SENTRY_CLI_SHA256" in sentry_linux and
          "expectedSha256" in sentry_windows,
          "Sentry CLI installers are versioned and checksum-pinned")
    check("4511815527104512" in sentry_crashpad and
          "BUILDFLAG(IS_WIN) || BUILDFLAG(IS_LINUX)" in sentry_crashpad and
          "cmux-upload-consent.lock" in sentry_crashpad and
          "cmux-upload-disabled" in sentry_crashpad and
          "ERROR_LOCK_VIOLATION" in sentry_crashpad and
          "cmux_lock_error == EACCES" in sentry_crashpad and
          "F_SETLKW" in sentry_crashpad and
          "UnlockFileEx" in sentry_crashpad and
          "#if !BUILDFLAG(IS_CHROMEOS)" in sentry_crashpad and
          "LINUX_BASE_FILE_INCLUDE_BLOCK" in sentry_crashpad and
          'if (initial_client)' in sentry_crashpad and
          "enable_uploads = consent && enable_uploads" in sentry_crashpad and
          "GetCmuxCrashUploadConsent" in sentry_crashpad and
          "kCmuxOfficialCrashUploadBuild = false" in sentry_crashpad and
          'executable.BaseName().AsUTF8Unsafe(), "chrome.exe"' in
          sentry_crashpad and
          "if (!CmuxCrashUploadClientSupported())" in sentry_crashpad and
          "CmuxConfigAllowsCrashUploads" in sentry_crashpad and
          "if (!configured_path.IsAbsolute())" in sentry_crashpad and
          "IsRunningUnattended()" in sentry_crashpad and
          "if (!cmux_client_uploads_enabled)" in sentry_crashpad and
          "CHECK(env->UnSetVar(kPipeNameVar))" in sentry_crashpad and
          "cmux-sentry-v1" in sentry_crashpad and
          "cmux-no-upload" in sentry_crashpad and
          "url.clear()" in sentry_crashpad and
          "SetUploadsEnabled(false)" in sentry_crashpad and
          "SetUploadsEnabled(true)" in sentry_crashpad and
          "patch_initial_consent" in sentry_crashpad,
          "every initial client resolves consent before Crashpad starts")
    check("SetUploadConsent_ExportThunk" not in telemetry and
          "UpdateCrashpadUploadDisabledMarker" not in telemetry and
          "SetCrashpadUploadConsent" not in telemetry and
          "kCmuxOfficialReleaseBuild" in telemetry and
          "DCHECK_IS_ON" not in telemetry,
          "PostHog uses the release stamp without racing Crashpad consent")
    check("LoadBrowserConfigWithStatusAsync" in telemetry and
          "BrowserConfigLoadStatus::kAbsent" in telemetry and
          "BrowserConfigLoadStatus::kError" in telemetry and
          "config read failed; telemetry disabled" in telemetry,
          "only a genuinely absent config keeps default telemetry consent")
    check('value->GetDict().Find("app")' in config and
          '#include "base/environment.h"' in config and
          'environment->GetVar("CMUX_CONFIG")' in config and
          'std::getenv("CMUX_CONFIG")' not in config and
          "require_absolute_config && !configured_path.IsAbsolute()" in
          config and
          "ResolveCmuxConfigPath(/*require_absolute_config=*/true)" in config and
          "if (!app_value->is_dict())" in config and
          'app_value->GetDict().Find("sendAnonymousTelemetry")' in config and
          "if (!telemetry->is_bool())" in config,
          "non-object app and non-boolean telemetry values fail config parsing")
    check("bool WriteTelemetryState" in telemetry and
          "ImportantFileWriter::WriteFileAtomically" in telemetry and
          'event.Set("uuid", ActiveEventUuid' in telemetry and
          "TaskShutdownBehavior::SKIP_ON_SHUTDOWN" in telemetry and
          "OnInitialStatePersisted" in telemetry and
          telemetry.index("PersistState(base::BindOnce") <
          telemetry.index("state_loaded_ = true"),
          "activity capture waits for an atomically durable anonymous identity")
    check("WINDOWS_SIGNING_CERTIFICATE_B64" in reusable and
          "signtool sign" in windows, "Authenticode is mandatory")
    expected_ublock_digest = (
        "6e02d8e6dce569eec721b531f9c7d28403e5426442417d5a855a17dd288b56f8"
    )
    check(expected_ublock_digest in extensions_posix and
          expected_ublock_digest in extensions_windows and
          ".cmux-crx-sha256" in extensions_posix and
          ".cmux-crx-sha256" in extensions_windows and
          "shasum -a 256" in extensions_posix and
          "Get-FileHash" in extensions_windows,
          "both extension packagers enforce the reviewed CRX digest")
    check("%(ChromeDir)s" in windows and
          windows.index("signtool sign") <
          windows.index("& autoninja -C out\\Release mini_installer",
                        windows.index("signtool sign")),
          "the signed payload and channel file feed Chromium's installer")
    check("signtool verify /pa /all" in windows,
          "packaged Windows signatures are verified")
    check(reusable.index("release/cmux-windows-x64.zip") <
          reusable.rindex("release/update.json"),
          "package assets are published before the manifest")
    check("--artifact mac-arm64=release/cmux-macos-arm64.zip" in reusable and
          "release/cmux-macos-arm64.dmg" in reusable,
          "signed feed and public release include macOS arm64")
    check("https://github.com/${CMUX_DISTRIBUTION_REPOSITORY}/releases/download/" in
          reusable and
          '--repo "$CMUX_DISTRIBUTION_REPOSITORY"' in reusable,
          "manifest URLs and release uploads target cmux-v2")
    check('cron: "17 9 * * *"' in nightly, "nightly schedule is pinned")
    check("${base##*.} + RUN_NUMBER" in nightly,
          "nightly versions increase from the Chromium patch baseline")
    check('tags:\n      - "v*"' in stable, "stable releases are tag-driven")
    check("CMUX_RELEASES_ENABLED == 'true'" in nightly and
          "CMUX_RELEASES_ENABLED == 'true'" in stable,
          "publishing is gated until release infrastructure is configured")
    check("cmux-update-feed-url" in service and
          "manaflow-ai/cmux-v2/releases/download/nightly/" in
          service and
          "manaflow-ai/cmux-v2/releases/download/nightly/update.json" in
          linux and
          "manaflow-ai/cmux-v2/releases/download/nightly/update.json" in
          windows,
          "nightly builds select the public distribution feed")
    check("kLegacyNightlyFeedUrl" in service and
          "feed == kLegacyNightlyFeedUrl" in service,
          "early private-repository nightly selectors migrate forward")
    check("dpkg-deb --raw-extract" in linux and
          'test -s "$work/extracted/DEBIAN/control"' in linux and
          "dpkg-deb --build --root-owner-group" in linux,
          "the Debian payload and control metadata are repacked together")
    check("build-linux-user-installer.sh" in linux and
          "install-cmux-tui-artifact.sh" in linux and
          'test -x "$OUT/cmux-browser/cmux-tui"' in linux and
          "cmux-tui.GHOSTTY_RESOURCES_MANIFEST.sha256" in linux and
          "cmux-linux-x64-installer.run" in reusable and
          ".local/opt/$install_slug" in linux_installer and
          "CMUX_LINUX_INSTALLER_NO_LAUNCH" in linux_installer and
          "__CMUX_PAYLOAD_BELOW__" in linux_installer,
          "Linux's primary installer carries its pinned terminal backend")
    check(
        all(
            "Install cached Rust toolchain" in workflow
            and 'CMUX_RUST_TOOLCHAIN: "1.96.0"' in workflow
            and 'rustup toolchain install "$CMUX_RUST_TOOLCHAIN"' in workflow
            and "RUSTUP_HOME: /opt/cmux/rustup" in workflow
            and "CARGO_HOME: /opt/cmux/cargo-home" in workflow
            for workflow in (linux_nightly, reusable)
        )
        and '[ "$RUST_VERSION" = "1.96.0" ]' in linux_bootstrap
        and "$RUST_COMPILER:zig-" in linux_bootstrap,
        "Linux releases cache and enforce the compatible Rust toolchain",
    )
    check(
        "xvfb" in linux_smoke
        and "cmux-views: cmux-tui backend ready" in linux_smoke
        and "cmux-views: niri strip up" in linux_smoke
        and "cmux-tui backend unavailable" in linux_smoke
        and r"\[DanglingPtr\]" in linux_smoke
        and r"\[DanglingSignature\]" in linux_smoke
        and "|DanglingPtr|" not in linux_smoke
        and "--headless" in linux_smoke
        and "CMUX_VIEWS=0" not in linux_smoke,
        "Linux smoke covers both native cmux startup and real Chromium headless",
    )
    check(
        "os.O_APPEND" in linux_smoke
        and '"/proc/$browser_pid/status"' in linux_smoke
        and '$1 == "State:"' in linux_smoke
        and '"$state" != Z' in linux_smoke
        and "browser_is_live" in linux_smoke
        and linux_smoke.index("if ! browser_is_live")
        < linux_smoke.index("cmux-views: cmux-tui backend ready")
        and "CMUX_LINUX_SMOKE_DWELL_COMPLETE" in linux_smoke
        and "seq 1 50" in linux_smoke,
        "Linux GUI smoke uses append-only logs and rejects dead or zombie startup",
    )
    check(
        "CMUX_LINUX_SMOKE_PRE_TERM" in linux_smoke
        and "pre_term_line" in linux_smoke
        and 'tail -n "+$((pre_term_line + 1))"' in linux_smoke
        and "Handling shutdown for signal 15." in linux_smoke
        and '[ "$browser_status" -ne 0 ]' in linux_smoke
        and "0|143" not in linux_smoke
        and linux_smoke.index("CMUX_LINUX_SMOKE_DWELL_COMPLETE")
        < linux_smoke.index("CMUX_LINUX_SMOKE_PRE_TERM")
        < linux_smoke.index('kill -TERM "$browser_pid"'),
        "Linux GUI smoke proves graceful shutdown after a five-second dwell",
    )
    check(
        "binutils" in linux_smoke
        and 'nm -D --defined-only "$smoke/install/chrome"' in linux_smoke
        and "^cmux_ghostty_" in linux_smoke,
        "Linux smoke rejects bundled Ghostty symbols in chrome's dynamic exports",
    )
    check(
        "public base::RefCountedDeleteOnSequence<OpenGLFrameMailbox>" in
        linux_terminal_pane
        and "friend class base::DeleteHelper<OpenGLFrameMailbox>" in
        linux_terminal_pane
        and "friend class base::RefCountedDeleteOnSequence<OpenGLFrameMailbox>"
        in linux_terminal_pane
        and "public base::RefCounted<OpenGLFrameMailbox>" not in
        linux_terminal_pane,
        "cross-thread OpenGL frames retain their mailbox and delete it on UI",
    )
    old_icon_remove = "auto old_icon = RemoveChildViewT(icon_.get());"
    old_icon_clear = "icon_ = nullptr;"
    old_icon_destroy = "old_icon.reset();"
    new_icon_install = "icon_ = AddChildViewAt(CreateIconSlot(visual, owner_), 0);"
    removed_tab_capture = "views::View* removed_view = removed.view;"
    removed_tab_clear = "removed.view = nullptr;"
    removed_tab_destroy = "OnCloseTabAnimationCompleted(removed_view);"
    check(
        all(
            marker in cmux_tab_strip
            for marker in (
                old_icon_remove,
                old_icon_clear,
                old_icon_destroy,
                new_icon_install,
                removed_tab_capture,
                removed_tab_clear,
                removed_tab_destroy,
            )
        )
        and cmux_tab_strip.index(old_icon_remove)
        < cmux_tab_strip.index(old_icon_clear, cmux_tab_strip.index(old_icon_remove))
        < cmux_tab_strip.index(old_icon_destroy,
                               cmux_tab_strip.index(old_icon_clear))
        < cmux_tab_strip.index(new_icon_install,
                               cmux_tab_strip.index(old_icon_destroy)),
        "tab favicon replacement clears its raw_ptr before destroying the child",
    )
    check(
        cmux_tab_strip.index(removed_tab_capture)
        < cmux_tab_strip.index(removed_tab_clear,
                               cmux_tab_strip.index(removed_tab_capture))
        < cmux_tab_strip.index(removed_tab_destroy,
                               cmux_tab_strip.index(removed_tab_clear)),
        "non-animated tab removal clears its raw_ptr before deleting the child",
    )
    extension_strip_destructor = cmux_extension_strip.split(
        "CmuxExtensionStrip::~CmuxExtensionStrip()", 1
    )[1].split("\n}\n", 1)[0]
    extension_buttons_clear = "buttons_.clear();"
    extension_menu_clear = "extensions_menu_button_ = nullptr;"
    extension_children_destroy = "RemoveAllChildViews();"
    extension_models_clear = "action_view_models_.clear();"
    check(
        all(
            marker in extension_strip_destructor
            for marker in (
                extension_buttons_clear,
                extension_menu_clear,
                extension_children_destroy,
                extension_models_clear,
            )
        )
        and extension_strip_destructor.index(extension_buttons_clear)
        < extension_strip_destructor.index(extension_menu_clear)
        < extension_strip_destructor.index(extension_children_destroy)
        < extension_strip_destructor.index(extension_models_clear),
        "extension strip clears child raw_ptrs before destroying its children",
    )
    window_view_destructor = cmux_views.split(
        "~CmuxWindowView() override", 1
    )[1].split("\n  }\n", 1)[0]
    pane_teardown = "DestroyPaneView(pane_id, /*notify_focus=*/false);"
    extension_container_teardown = (
        "DestroyCmuxExtensionsContainerForBrowser(browser.get());"
    )
    check(
        "CancelActiveDragForHostState();" in window_view_destructor
        and pane_teardown in window_view_destructor
        and extension_container_teardown in window_view_destructor
        and window_view_destructor.index("CancelActiveDragForHostState();")
        < window_view_destructor.index(pane_teardown)
        < window_view_destructor.index(extension_container_teardown),
        "window teardown destroys extension strips before their containers",
    )
    check(
        "linux-nightly-smoke-receipt.json" in linux_smoke
        and "cmux-linux-nightly-smoke-receipt" in linux_smoke
        and "sha256sums_sha256" in linux_smoke
        and 'int(os.environ["GITHUB_RUN_ID"])' in linux_smoke
        and 'int(os.environ["SOURCE_RUN_ID"])' in linux_smoke
        and '"smoke_workflow_sha": os.environ["GITHUB_SHA"]' in linux_smoke
        and "actions/upload-artifact@v4" in linux_smoke
        and "test-linux-nightly-publication.py" in updater
        and "publish-linux-nightly-release.sh" in updater,
        "successful Linux smoke runs emit a tested candidate-specific receipt",
    )
    check("assert_gn_arg is_component_build false" in linux and
          "Assert-GnArg 'is_component_build' 'false'" in windows,
          "release builders reject incompatible component output trees")
    check("assert_gn_arg symbol_level 1" in linux and
          "Assert-GnArg 'symbol_level' '1'" in windows,
          "release builders require symbol-bearing output trees")
    check("assert_gn_arg use_debug_fission false" in linux and
          "use_debug_fission=false" in release_docs,
          "Linux embeds line data in the ELF files uploaded to Sentry")
    check("stamp-release-build.py" in linux and
          "stamp-release-build.py" in windows,
          "only release builders enable telemetry by default")

    subprocess.run(
        [ROOT / "scripts/local-builder-transport.sh", "local", "printf transport-ok"],
        check=True,
        stdout=subprocess.PIPE,
        text=True,
    )
    check(shutil.which("rsync") is not None, "rsync is installed")
    with tempfile.TemporaryDirectory() as raw_tmp:
        tmp = Path(raw_tmp)
        source = tmp / "source"
        destination = tmp / "destination"
        source.mkdir()
        (source / "transport.txt").write_text("ok\n", encoding="utf-8")
        environment = os.environ.copy()
        environment["RSYNC_RSH"] = str(
            ROOT / "scripts/local-builder-transport.sh"
        )
        subprocess.run(
            ["rsync", "-a", f"{source}/", f"local:{destination}/"],
            check=True,
            env=environment,
        )
        check((destination / "transport.txt").read_text(encoding="utf-8") ==
              "ok\n", "local builder transport supports rsync")
    subprocess.run(
        [sys.executable, "-B", ROOT / "scripts/test_sentry_crashpad.py"],
        check=True,
    )
    print("release pipeline checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
