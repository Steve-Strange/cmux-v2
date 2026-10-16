#!/usr/bin/env python3
"""Host tests for the cmux Crashpad-to-Sentry Chromium patch."""

from __future__ import annotations

import importlib.util
from pathlib import Path
import tempfile


ROOT = Path(__file__).resolve().parent.parent
SPEC = importlib.util.spec_from_file_location(
    "sentry_crashpad", ROOT / "patches/sentry_crashpad.py"
)
assert SPEC is not None and SPEC.loader is not None
SENTRY_CRASHPAD = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(SENTRY_CRASHPAD)


def check(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> int:
    expected_url = (
        "https://o4509253353930752.ingest.us.sentry.io"
        "/api/4511815527104512/minidump/"
        "?sentry_key=57f477545589ac21ff2d89a90d6df629"
    )
    check(
        SENTRY_CRASHPAD.minidump_url_from_dsn(
            SENTRY_CRASHPAD.CMUX_BROWSER_SENTRY_DSN
        )
        == expected_url,
        "the cmux-browser DSN derives the reviewed Crashpad endpoint",
    )
    check(
        SENTRY_CRASHPAD.minidump_url_from_dsn(
            "https://public@example.test/prefix/123"
        )
        == "https://example.test/prefix/api/123/minidump/?sentry_key=public",
        "self-hosted DSN path prefixes are preserved",
    )
    for invalid in (
        "http://public@example.test/123",
        "https://public:secret@example.test/123",
        "https://example.test/123",
        "https://public@example.test/project",
    ):
        try:
            SENTRY_CRASHPAD.minidump_url_from_dsn(invalid)
        except ValueError:
            pass
        else:
            raise AssertionError(f"invalid public DSN accepted: {invalid}")

    with tempfile.TemporaryDirectory() as raw_tmp:
        chromium_src = Path(raw_tmp)
        target = chromium_src / SENTRY_CRASHPAD.CRASH_REPORTER_CLIENT
        target.parent.mkdir(parents=True)
        target.write_text(
            "// before\n"
            + SENTRY_CRASHPAD.ORIGINAL_FUNCTION
            + "// after\n",
            encoding="utf-8",
        )
        core_target = chromium_src / SENTRY_CRASHPAD.CRASHPAD_CORE
        core_target.write_text(
            "#include <map>\n"
            "#include <optional>\n"
            "#include <string_view>\n"
            '#include "base/command_line.h"\n'
            "namespace crash_reporter {\n\n"
            "#if BUILDFLAG(IS_IOS)\n"
            "#endif\n"
            "// before\n"
            + SENTRY_CRASHPAD.ORIGINAL_INITIAL_CONSENT
            + "\nvoid SetUploadConsent(bool consent) {\n"
            + "  if (!g_database)\n"
            + "    return;\n\n"
            + SENTRY_CRASHPAD.ORIGINAL_POLICY_BRANCH
            + "    enable_uploads = consent;\n"
            + "  }\n\n"
            + SENTRY_CRASHPAD.ORIGINAL_SETTINGS_WRITE
            + "}\n"
            + "}  // namespace crash_reporter\n"
            + "// after\n",
            encoding="utf-8",
        )
        header_target = chromium_src / SENTRY_CRASHPAD.CRASHPAD_HEADER
        header_target.write_text(
            "// before\n"
            + SENTRY_CRASHPAD.CRASHPAD_HEADER_CONSENT_ANCHOR
            + "// after\n",
            encoding="utf-8",
        )
        for platform, relative_path in (
            SENTRY_CRASHPAD.CRASHPAD_STARTUP_FILES.items()
        ):
            startup_target = chromium_src / relative_path
            platform_includes = {
                "linux": (
                    "#include <pthread.h>\n"
                    "#include <limits>\n"
                    '#include "base/command_line.h"\n'
                    "#if BUILDFLAG(IS_CHROMEOS_DEVICE)\n"
                    + SENTRY_CRASHPAD.BASE_FILE_INCLUDE
                    + SENTRY_CRASHPAD.BASE_FILE_UTIL_INCLUDE
                    + "#endif\n"
                ),
                "windows": (
                    "#include <memory>\n"
                    '#include "base/files/file_util.h"\n'
                ),
            }[platform]
            startup_target.write_text(
                platform_includes
                + SENTRY_CRASHPAD.CRASHPAD_CLIENT_INCLUDE
                + SENTRY_CRASHPAD.CRASHPAD_INFO_INCLUDE
                + "\nvoid Start(bool initial_client) {\n"
                + SENTRY_CRASHPAD.START_HANDLER_ANCHORS[platform]
                + "}\n",
                encoding="utf-8",
            )

        check(
            SENTRY_CRASHPAD.patch_chromium(chromium_src),
            "first application patches Chromium",
        )
        patched = target.read_text(encoding="utf-8")
        check(expected_url in patched, "patched Chromium contains the endpoint")
        check(
            "#elif BUILDFLAG(IS_WIN) || BUILDFLAG(IS_LINUX)" in patched,
            "only Windows and Linux use the Sentry endpoint",
        )
        patched_core = core_target.read_text(encoding="utf-8")
        check(
            SENTRY_CRASHPAD.INITIAL_CONSENT_MARKER in patched_core,
            "Chromium's initial unbranded consent write is guarded",
        )
        check(
            SENTRY_CRASHPAD.SYNC_CONSENT_MARKER in patched_core
            and "kCmuxOfficialCrashUploadBuild = false" in patched_core
            and 'executable.BaseName().AsUTF8Unsafe(), "chrome.exe"'
            in patched_core
            and "if (!CmuxCrashUploadClientSupported())" in patched_core
            and 'environment->GetVar("CMUX_TELEMETRY_ENABLE")' in patched_core
            and "if (!configured_path.IsAbsolute())" in patched_core
            and 'value->GetDict().Find("app")' in patched_core
            and "IsRunningUnattended()" in patched_core,
            "the final privacy and unattended decision is resolved "
            "synchronously before handler startup, and standalone Windows "
            "helpers remain disabled",
        )
        check(
            SENTRY_CRASHPAD.SYNC_CONSENT_DECLARATION
            in header_target.read_text(encoding="utf-8"),
            "platform startup can call the synchronous consent resolver",
        )
        check(
            "#if !BUILDFLAG(IS_WIN) && !BUILDFLAG(IS_LINUX)"
            in patched_core,
            "Windows/Linux secondary launches cannot overwrite shared consent",
        )
        check(
            SENTRY_CRASHPAD.CONSENT_POLICY_MARKER in patched_core
            and "enable_uploads = consent && enable_uploads;" in patched_core,
            "managed reporting policy cannot bypass an explicit app opt-out",
        )
        check(
            SENTRY_CRASHPAD.DISABLED_MARKER_GUARD in patched_core
            and 'AppendASCII("cmux-upload-disabled")' in patched_core
            and "else if (g_database_path && !cmux_reporting_enforced)"
            in patched_core
            and "!crash_reporter_client->IsRunningUnattended()"
            in patched_core,
            "later Chromium consent writes preserve the authoritative app "
            "opt-in and opt-out without bypassing unattended suppression",
        )
        for platform, relative_path in (
            SENTRY_CRASHPAD.CRASHPAD_STARTUP_FILES.items()
        ):
            startup = (chromium_src / relative_path).read_text(
                encoding="utf-8"
            )
            marker_offset = startup.index(SENTRY_CRASHPAD.EARLY_CONSENT_MARKER)
            handler_offset = startup.index(
                SENTRY_CRASHPAD.START_HANDLER_ANCHORS[platform]
            )
            check(
                marker_offset < handler_offset,
                f"{platform} disables uploads before starting Crashpad",
            )
            check(
                "if (initial_client)" in startup,
                f"{platform} fail-closes every initial Crashpad client",
            )
            check(
                "cmux-upload-consent.lock" in startup,
                f"{platform} serializes shared Crashpad consent ownership",
            )
            check(
                'AppendASCII("cmux-upload-disabled")' in startup,
                f"{platform} clears any stale shared opt-out before enabling",
            )
            check(
                "SetUploadsEnabled(true)" in startup
                and "SetUploadsEnabled(false)" in startup
                and "GetCmuxCrashUploadConsent()" in startup
                and "if (!cmux_client_uploads_enabled)" in startup,
                f"{platform} resolves every client's consent before the handler",
            )
            check(
                'AppendASCII("cmux-sentry-v1")' in startup
                and startup.index('AppendASCII("cmux-sentry-v1")')
                < startup.index("CrashReportDatabase::Initialize")
                < startup.index(
                    SENTRY_CRASHPAD.START_HANDLER_ANCHORS[platform]
                ),
                f"{platform} never exposes legacy pending reports to Sentry",
            )
            if platform == "linux":
                check(
                    SENTRY_CRASHPAD.LINUX_BASE_FILE_INCLUDE_BLOCK in startup
                    and startup.count(SENTRY_CRASHPAD.BASE_FILE_INCLUDE) == 2
                    and startup.count(
                        SENTRY_CRASHPAD.BASE_FILE_UTIL_INCLUDE
                    ) == 2,
                    "Linux adds desktop-visible file headers despite the "
                    "upstream ChromeOS-only copies",
                )
                check(
                    "F_SETLKW" in startup
                    and "EACCES" in startup
                    and "EAGAIN" in startup,
                    "Linux serializes startup and recognizes native contention",
                )
                check(
                    "#if !BUILDFLAG(IS_CHROMEOS)" in startup,
                    "Linux consent patch leaves ChromeOS crash policy unchanged",
                )
                check(
                    'AppendASCII("cmux-no-upload")' in startup
                    and "url.clear()" in startup
                    and startup.index("url.clear()")
                    < startup.index("CrashReportDatabase::Initialize")
                    < startup.index("SetUploadsEnabled(false)")
                    < startup.index(
                        SENTRY_CRASHPAD.START_HANDLER_ANCHORS[platform]
                    ),
                    "opted-out Linux keeps its required handler on a "
                    "local-only database with no upload URL",
                )
            else:
                check(
                    "::LockFileEx(" in startup
                    and "::UnlockFileEx(" in startup
                    and "ERROR_LOCK_VIOLATION" in startup,
                    "Windows serializes startup and recognizes native contention",
                )
                check(
                    startup.index("if (!cmux_client_uploads_enabled)")
                    < startup.index("CHECK(env->UnSetVar(kPipeNameVar))")
                    < startup.index(
                        "return false;",
                        startup.index("CHECK(env->UnSetVar(kPipeNameVar))"),
                    )
                    < startup.index("CrashReportDatabase::Initialize"),
                    "Windows clears inherited handler IPC before declining "
                    "non-consenting browser clients",
                )
            check(
                startup.index("if (cmux_lock_acquired)")
                < startup.index("SetUploadsEnabled(true)"),
                f"{platform} changes shared consent only after ownership",
            )
            check(
                startup.index("GetCmuxCrashUploadConsent()")
                < startup.index(
                    "cmux_startup_barrier.l_type = F_UNLCK"
                    if platform == "linux"
                    else "::UnlockFileEx("
                ),
                f"{platform} releases startup peers only after consent resolves",
            )
        check(
            not SENTRY_CRASHPAD.patch_chromium(chromium_src),
            "second application is idempotent",
        )
        core_target.write_text(
            patched_core.replace(
                "kCmuxOfficialCrashUploadBuild = false",
                "kCmuxOfficialCrashUploadBuild = true",
                1,
            ),
            encoding="utf-8",
        )
        check(
            SENTRY_CRASHPAD.patch_chromium(chromium_src),
            "ordinary patching clears a stale release crash-upload stamp",
        )
        check(
            "kCmuxOfficialCrashUploadBuild = false"
            in core_target.read_text(encoding="utf-8")
            and not SENTRY_CRASHPAD.patch_chromium(chromium_src),
            "the reset developer state is idempotent",
        )

        target.write_text(
            patched.replace(expected_url, "https://example.test/wrong", 1),
            encoding="utf-8",
        )
        try:
            SENTRY_CRASHPAD.patch_chromium(chromium_src)
        except AssertionError:
            pass
        else:
            raise AssertionError("a stale compiled Crashpad endpoint was accepted")

    print("sentry_crashpad tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
