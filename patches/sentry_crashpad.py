#!/usr/bin/env python3
"""Route unbranded Windows/Linux Chromium Crashpad uploads to Sentry."""

from __future__ import annotations

import argparse
from pathlib import Path
from urllib.parse import urlsplit, urlunsplit


# Sentry DSNs contain only a public client key. They are intentionally shipped
# in client applications and do not authorize reads, project changes, or symbol
# uploads.
CMUX_BROWSER_SENTRY_DSN = (
    "https://57f477545589ac21ff2d89a90d6df629"
    "@o4509253353930752.ingest.us.sentry.io/4511815527104512"
)
CRASH_REPORTER_CLIENT = Path(
    "components/crash/core/app/crash_reporter_client.cc"
)
CRASHPAD_CORE = Path("components/crash/core/app/crashpad.cc")
CRASHPAD_HEADER = Path("components/crash/core/app/crashpad.h")
CRASHPAD_STARTUP_FILES = {
    "linux": Path("components/crash/core/app/crashpad_linux.cc"),
    "windows": Path("components/crash/core/app/crashpad_win.cc"),
}
CMUX_MARKER = (
    "  // cmux: route Windows/Linux Crashpad uploads to the dedicated "
    "Sentry project.\n"
)
EARLY_CONSENT_MARKER = (
    "    // cmux: only the process holding the consent-owner lock may change "
    "shared upload state.\n"
)
INITIAL_CONSENT_MARKER = (
    "    // cmux: platform startup already fail-closed the primary process; "
    "secondary launches\n"
)
CONSENT_POLICY_MARKER = (
    "  // cmux: app consent is a hard ceiling even when reporting policy is "
    "managed.\n"
)
DISABLED_MARKER_GUARD = (
    "  // cmux: keep app consent authoritative across later Chromium metrics "
    "writes.\n"
)
SYNC_CONSENT_MARKER = (
    "// cmux: resolve the durable app crash-upload decision before starting "
    "the handler.\n"
)
SYNC_CONSENT_DECLARATION = (
    "// Returns the final cmux crash-upload decision for this launch. The "
    "implementation\n"
    "// synchronously reads the shared privacy setting before Crashpad starts.\n"
    "bool GetCmuxCrashUploadConsent();\n"
)
BASE_FILE_INCLUDE = '#include "base/files/file.h"\n'
BASE_FILE_UTIL_INCLUDE = '#include "base/files/file_util.h"\n'
CORE_BASE_INCLUDE_BLOCK = (
    '#include "base/command_line.h"\n'
    '#include "base/environment.h"\n'
    + BASE_FILE_INCLUDE
    + BASE_FILE_UTIL_INCLUDE
    + '#include "base/json/json_reader.h"\n'
    + '#include "base/path_service.h"\n'
)
LINUX_BASE_FILE_INCLUDE_BLOCK = (
    '#include "base/command_line.h"\n'
    + BASE_FILE_INCLUDE
    + BASE_FILE_UTIL_INCLUDE
)
WINDOWS_BASE_FILE_INCLUDE_BLOCK = (
    BASE_FILE_INCLUDE + BASE_FILE_UTIL_INCLUDE
)
CRASH_DATABASE_INCLUDE = (
    '#include "third_party/crashpad/crashpad/client/crash_report_database.h"\n'
)
CRASHPAD_CLIENT_INCLUDE = (
    '#include "third_party/crashpad/crashpad/client/crashpad_client.h"\n'
)
CRASH_SETTINGS_INCLUDE = (
    '#include "third_party/crashpad/crashpad/client/settings.h"\n'
)
CRASHPAD_INFO_INCLUDE = (
    '#include "third_party/crashpad/crashpad/client/crashpad_info.h"\n'
)
START_HANDLER_ANCHORS = {
    "linux": "    CHECK(client.StartHandler(handler_path, *database_path, metrics_path, url,\n",
    "windows": "    initialized = GetCrashpadClient().StartHandler(\n",
}
ORIGINAL_FUNCTION = """std::string CrashReporterClient::GetUploadUrl() {
#if BUILDFLAG(GOOGLE_CHROME_BRANDING) && defined(OFFICIAL_BUILD)
  return kDefaultUploadURL;
#else
  return std::string();
#endif
}
"""
ORIGINAL_INITIAL_CONSENT = """    CrashReporterClient* crash_reporter_client = GetCrashReporterClient();
    SetUploadConsent(crash_reporter_client->GetCollectStatsConsent());
"""
ORIGINAL_POLICY_BRANCH = """  bool enable_uploads = false;
  CrashReporterClient* crash_reporter_client = GetCrashReporterClient();
  if (!crash_reporter_client->ReportingIsEnforcedByPolicy(&enable_uploads)) {
"""
ORIGINAL_SETTINGS_WRITE = """  crashpad::Settings* settings = g_database->GetSettings();
  settings->SetUploadsEnabled(enable_uploads &&
                              crash_reporter_client->GetCollectStatsInSample());
"""
CRASHPAD_HEADER_CONSENT_ANCHOR = """void SetUploadConsent(bool consent);
"""
CRASHPAD_SYNC_CONSENT_ANCHOR = """namespace crash_reporter {

#if BUILDFLAG(IS_IOS)
"""


def minidump_url_from_dsn(dsn: str) -> str:
    parsed = urlsplit(dsn)
    if (
        parsed.scheme != "https"
        or not parsed.hostname
        or not parsed.username
        or parsed.password
        or parsed.query
        or parsed.fragment
    ):
        raise ValueError("Sentry DSN must be a public HTTPS DSN")

    path_parts = [part for part in parsed.path.split("/") if part]
    if not path_parts or not path_parts[-1].isdigit():
        raise ValueError("Sentry DSN must end with a numeric project ID")
    project_id = path_parts[-1]
    path_prefix = "/" + "/".join(path_parts[:-1]) if len(path_parts) > 1 else ""
    host = parsed.hostname
    if parsed.port is not None:
        host = f"{host}:{parsed.port}"
    return urlunsplit(
        (
            "https",
            host,
            f"{path_prefix}/api/{project_id}/minidump/",
            f"sentry_key={parsed.username}",
            "",
        )
    )


def sync_consent_implementation() -> str:
    return (
        "#if BUILDFLAG(IS_WIN) || BUILDFLAG(IS_LINUX)\n"
        + SYNC_CONSENT_MARKER
        + "namespace {\n\n"
        + "constexpr bool kCmuxOfficialCrashUploadBuild = false;\n\n"
        + "bool CmuxCrashUploadClientSupported() {\n"
        + "#if BUILDFLAG(IS_WIN)\n"
        + "  base::FilePath executable;\n"
        + "  return base::PathService::Get(base::FILE_EXE, &executable) &&\n"
        + "         base::EqualsCaseInsensitiveASCII(\n"
        + '             executable.BaseName().AsUTF8Unsafe(), "chrome.exe");\n'
        + "#else\n"
        + "  return true;\n"
        + "#endif\n"
        + "}\n\n"
        + "std::optional<base::FilePath> CmuxCrashUploadConfigPath(\n"
        + "    base::Environment& environment) {\n"
        + "  if (std::optional<std::string> configured =\n"
        + '          environment.GetVar("CMUX_CONFIG");\n'
        + "      configured && !configured->empty()) {\n"
        + "    const base::FilePath configured_path =\n"
        + "        base::FilePath::FromUTF8Unsafe(*configured);\n"
        + "    if (!configured_path.IsAbsolute()) {\n"
        + "      return std::nullopt;\n"
        + "    }\n"
        + "    return configured_path;\n"
        + "  }\n"
        + "  if (std::optional<std::string> xdg =\n"
        + '          environment.GetVar("XDG_CONFIG_HOME");\n'
        + "      xdg && !xdg->empty()) {\n"
        + "    const base::FilePath xdg_path =\n"
        + "        base::FilePath::FromUTF8Unsafe(*xdg);\n"
        + "    if (xdg_path.IsAbsolute()) {\n"
        + '      return xdg_path.AppendASCII("cmux").AppendASCII("cmux.json");\n'
        + "    }\n"
        + "  }\n"
        + "  base::FilePath home;\n"
        + "  if (base::PathService::Get(base::DIR_HOME, &home)) {\n"
        + '    return home.AppendASCII(".config")\n'
        + '        .AppendASCII("cmux")\n'
        + '        .AppendASCII("cmux.json");\n'
        + "  }\n"
        + '  return base::FilePath::FromUTF8Unsafe("/tmp/cmux/cmux.json");\n'
        + "}\n\n"
        + "bool CmuxConfigAllowsCrashUploads(base::Environment& environment) {\n"
        + "  const std::optional<base::FilePath> path =\n"
        + "      CmuxCrashUploadConfigPath(environment);\n"
        + "  if (!path) {\n"
        + "    return false;\n"
        + "  }\n"
        + "  base::File readable_file(\n"
        + "      *path, base::File::FLAG_OPEN | base::File::FLAG_READ);\n"
        + "  if (!readable_file.IsValid()) {\n"
        + "    return readable_file.error_details() ==\n"
        + "           base::File::FILE_ERROR_NOT_FOUND;\n"
        + "  }\n"
        + "  readable_file.Close();\n\n"
        + "  std::string json;\n"
        + "  if (!base::ReadFileToString(*path, &json)) {\n"
        + "    return false;\n"
        + "  }\n"
        + "  std::optional<base::Value> value = base::JSONReader::Read(\n"
        + "      json, base::JSON_PARSE_CHROMIUM_EXTENSIONS |\n"
        + "                base::JSON_ALLOW_TRAILING_COMMAS);\n"
        + "  if (!value || !value->is_dict()) {\n"
        + "    return false;\n"
        + "  }\n"
        + '  const base::Value* app = value->GetDict().Find("app");\n'
        + "  if (!app) {\n"
        + "    return true;\n"
        + "  }\n"
        + "  if (!app->is_dict()) {\n"
        + "    return false;\n"
        + "  }\n"
        + "  const base::Value* telemetry =\n"
        + '      app->GetDict().Find("sendAnonymousTelemetry");\n'
        + "  return !telemetry ||\n"
        + "         (telemetry->is_bool() && telemetry->GetBool());\n"
        + "}\n\n"
        + "}  // namespace\n\n"
        + "bool GetCmuxCrashUploadConsent() {\n"
        + "  if (!CmuxCrashUploadClientSupported()) {\n"
        + "    return false;\n"
        + "  }\n"
        + "  std::unique_ptr<base::Environment> environment =\n"
        + "      base::Environment::Create();\n"
        + "  const std::string override_value =\n"
        + '      environment->GetVar("CMUX_TELEMETRY_ENABLE")\n'
        + "          .value_or(std::string());\n"
        + '  if (override_value == "0" ||\n'
        + "      (!kCmuxOfficialCrashUploadBuild && override_value != \"1\") ||\n"
        + "      !CmuxConfigAllowsCrashUploads(*environment)) {\n"
        + "    return false;\n"
        + "  }\n\n"
        + "  bool enable_uploads = false;\n"
        + "  CrashReporterClient* crash_reporter_client =\n"
        + "      GetCrashReporterClient();\n"
        + "  if (!crash_reporter_client->ReportingIsEnforcedByPolicy(\n"
        + "          &enable_uploads)) {\n"
        + "    enable_uploads =\n"
        + "        !crash_reporter_client->IsRunningUnattended();\n"
        + "  }\n"
        + "  return enable_uploads &&\n"
        + "         crash_reporter_client->GetCollectStatsInSample();\n"
        + "}\n"
        + "#endif  // BUILDFLAG(IS_WIN) || BUILDFLAG(IS_LINUX)\n\n"
    )


def patch_sync_consent_declaration(source: str) -> str:
    expected = (
        CRASHPAD_HEADER_CONSENT_ANCHOR
        + "\n#if BUILDFLAG(IS_WIN) || BUILDFLAG(IS_LINUX)\n"
        + SYNC_CONSENT_DECLARATION
        + "#endif\n"
    )
    if SYNC_CONSENT_DECLARATION in source:
        if source.count(SYNC_CONSENT_DECLARATION) != 1 or expected not in source:
            raise AssertionError(
                "existing cmux synchronous consent declaration differs from "
                "the reviewed pre-handler contract"
            )
        return source
    if source.count(CRASHPAD_HEADER_CONSENT_ANCHOR) != 1:
        raise AssertionError(
            "Crashpad consent declaration anchor changed or is ambiguous"
        )
    return source.replace(CRASHPAD_HEADER_CONSENT_ANCHOR, expected, 1)


def patch_sync_consent_implementation(source: str) -> str:
    implementation = sync_consent_implementation()
    stamped_implementation = implementation.replace(
        "constexpr bool kCmuxOfficialCrashUploadBuild = false;",
        "constexpr bool kCmuxOfficialCrashUploadBuild = true;",
        1,
    )
    if SYNC_CONSENT_MARKER in source:
        normalized = source
        if stamped_implementation in normalized:
            normalized = normalized.replace(
                "constexpr bool kCmuxOfficialCrashUploadBuild = true;",
                "constexpr bool kCmuxOfficialCrashUploadBuild = false;",
                1,
            )
        if (
            normalized.count(SYNC_CONSENT_MARKER) != 1
            or implementation not in normalized
            or CORE_BASE_INCLUDE_BLOCK not in normalized
            or "#include <memory>\n" not in normalized
            or "#include <string>\n" not in normalized
        ):
            raise AssertionError(
                "existing cmux synchronous Crashpad consent implementation "
                "differs from the reviewed fail-closed loader"
            )
        return normalized

    if source.count(CRASHPAD_SYNC_CONSENT_ANCHOR) != 1:
        raise AssertionError(
            "Crashpad synchronous consent source anchor changed or is ambiguous"
        )
    if CORE_BASE_INCLUDE_BLOCK not in source:
        include_anchor = '#include "base/command_line.h"\n'
        if source.count(include_anchor) != 1:
            raise AssertionError(
                "Crashpad synchronous consent include anchor changed or is "
                "ambiguous"
            )
        source = source.replace(
            include_anchor, CORE_BASE_INCLUDE_BLOCK, 1
        )
    if "#include <memory>\n" not in source:
        include_anchor = "#include <map>\n"
        if source.count(include_anchor) != 1:
            raise AssertionError(
                "Crashpad <memory> include anchor changed or is ambiguous"
            )
        source = source.replace(
            include_anchor, include_anchor + "#include <memory>\n", 1
        )
    if "#include <string>\n" not in source:
        include_anchor = "#include <optional>\n"
        if source.count(include_anchor) != 1:
            raise AssertionError(
                "Crashpad <string> include anchor changed or is ambiguous"
            )
        source = source.replace(
            include_anchor, include_anchor + "#include <string>\n", 1
        )
    return source.replace(
        CRASHPAD_SYNC_CONSENT_ANCHOR,
        "namespace crash_reporter {\n\n"
        + implementation
        + "#if BUILDFLAG(IS_IOS)\n",
        1,
    )


def patched_function(dsn: str = CMUX_BROWSER_SENTRY_DSN) -> str:
    upload_url = minidump_url_from_dsn(dsn)
    return f"""std::string CrashReporterClient::GetUploadUrl() {{
#if BUILDFLAG(GOOGLE_CHROME_BRANDING) && defined(OFFICIAL_BUILD)
  return kDefaultUploadURL;
#elif BUILDFLAG(IS_WIN) || BUILDFLAG(IS_LINUX)
{CMUX_MARKER}  return "{upload_url}";
#else
  return std::string();
#endif
}}
"""


def patch_upload_url(source: str) -> str:
    expected = patched_function()
    if CMUX_MARKER in source:
        if source.count(CMUX_MARKER) != 1 or expected not in source:
            raise AssertionError(
                "existing cmux Crashpad endpoint differs from the reviewed "
                "cmux-browser Sentry DSN"
            )
        return source
    if source.count(ORIGINAL_FUNCTION) != 1:
        raise AssertionError(
            "CrashReporterClient::GetUploadUrl source anchor changed or is "
            "ambiguous"
        )
    return source.replace(ORIGINAL_FUNCTION, expected, 1)


def early_consent_block(platform: str) -> str:
    if platform == "linux":
        disabled_client_block = (
            "      if (!cmux_client_uploads_enabled) {\n"
            "        // Linux children require the browser's Crashpad socket.\n"
            "        // Keep a local-only handler on a database that can never\n"
            "        // be reused by an opted-in launch.\n"
            "        *database_path =\n"
            "            database_path->AppendASCII(\"cmux-no-upload\");\n"
            "        url.clear();\n"
            "      }\n"
        )
        lock_block = (
            "      struct flock cmux_startup_barrier = {};\n"
            "      cmux_startup_barrier.l_type = F_WRLCK;\n"
            "      cmux_startup_barrier.l_whence = SEEK_SET;\n"
            "      cmux_startup_barrier.l_start = 1;\n"
            "      cmux_startup_barrier.l_len = 1;\n"
            "      int cmux_barrier_result;\n"
            "      do {\n"
            "        cmux_barrier_result =\n"
            "            fcntl(cmux_consent_lock->GetPlatformFile(), F_SETLKW,\n"
            "                  &cmux_startup_barrier);\n"
            "      } while (cmux_barrier_result < 0 && errno == EINTR);\n"
            "      if (cmux_barrier_result < 0) {\n"
            "        return false;\n"
            "      }\n"
            "      struct flock cmux_owner_lock = {};\n"
            "      cmux_owner_lock.l_type = F_WRLCK;\n"
            "      cmux_owner_lock.l_whence = SEEK_SET;\n"
            "      cmux_owner_lock.l_start = 0;\n"
            "      cmux_owner_lock.l_len = 1;\n"
            "      const int cmux_lock_result =\n"
            "          fcntl(cmux_consent_lock->GetPlatformFile(), F_SETLK,\n"
            "                &cmux_owner_lock);\n"
            "      const int cmux_lock_error = errno;\n"
            "      const bool cmux_lock_acquired = cmux_lock_result == 0;\n"
            "      const bool cmux_lock_contended =\n"
            "          cmux_lock_result < 0 &&\n"
            "          (cmux_lock_error == EACCES || cmux_lock_error == EAGAIN);\n"
            "      if (!cmux_lock_acquired && !cmux_lock_contended) {\n"
            "        return false;\n"
            "      }\n"
        )
        unlock_block = (
            "      cmux_startup_barrier.l_type = F_UNLCK;\n"
            "      int cmux_unlock_result;\n"
            "      do {\n"
            "        cmux_unlock_result =\n"
            "            fcntl(cmux_consent_lock->GetPlatformFile(), F_SETLK,\n"
            "                  &cmux_startup_barrier);\n"
            "      } while (cmux_unlock_result < 0 && errno == EINTR);\n"
            "      if (cmux_unlock_result < 0) {\n"
            "        return false;\n"
            "      }\n"
        )
    elif platform == "windows":
        disabled_client_block = (
            "      if (!cmux_client_uploads_enabled) {\n"
            "        // Never let opted-out children reuse an inherited,\n"
            "        // upload-capable handler owned by another browser.\n"
            "        CHECK(env->UnSetVar(kPipeNameVar));\n"
            "        return false;\n"
            "      }\n"
        )
        lock_block = (
            "      OVERLAPPED cmux_startup_barrier = {};\n"
            "      cmux_startup_barrier.Offset = 1;\n"
            "      if (!::LockFileEx(cmux_consent_lock->GetPlatformFile(),\n"
            "                        LOCKFILE_EXCLUSIVE_LOCK, 0, 1, 0,\n"
            "                        &cmux_startup_barrier)) {\n"
            "        return false;\n"
            "      }\n"
            "      OVERLAPPED cmux_owner_lock = {};\n"
            "      const BOOL cmux_lock_result = ::LockFileEx(\n"
            "          cmux_consent_lock->GetPlatformFile(),\n"
            "          LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0,\n"
            "          1, 0, &cmux_owner_lock);\n"
            "      const DWORD cmux_lock_error =\n"
            "          cmux_lock_result ? ERROR_SUCCESS : ::GetLastError();\n"
            "      const bool cmux_lock_acquired = cmux_lock_result != FALSE;\n"
            "      const bool cmux_lock_contended =\n"
            "          !cmux_lock_acquired &&\n"
            "          cmux_lock_error == ERROR_LOCK_VIOLATION;\n"
            "      if (!cmux_lock_acquired && !cmux_lock_contended) {\n"
            "        return false;\n"
            "      }\n"
        )
        unlock_block = (
            "      if (!::UnlockFileEx(cmux_consent_lock->GetPlatformFile(), 0,\n"
            "                          1, 0, &cmux_startup_barrier)) {\n"
            "        return false;\n"
            "      }\n"
        )
    else:
        raise ValueError(f"unsupported Crashpad platform: {platform}")

    block = (
        EARLY_CONSENT_MARKER
        + "    if (initial_client) {\n"
        + "      const bool cmux_client_uploads_enabled =\n"
        + "          GetCmuxCrashUploadConsent();\n"
        + disabled_client_block
        + "      if (cmux_client_uploads_enabled) {\n"
        + "        // Never upload pending reports created by pre-telemetry\n"
        + "        // Chromium builds from the legacy database.\n"
        + "        *database_path =\n"
        + "            database_path->AppendASCII(\"cmux-sentry-v1\");\n"
        + "      }\n"
        + "      auto cmux_database =\n"
        + "          crashpad::CrashReportDatabase::Initialize(*database_path);\n"
        + "      if (!cmux_database) {\n"
        + "        return false;\n"
        + "      }\n"
        + "      auto cmux_consent_lock = std::make_unique<base::File>(\n"
        + "          database_path->AppendASCII(\"cmux-upload-consent.lock\"),\n"
        + "          base::File::FLAG_OPEN_ALWAYS | base::File::FLAG_READ |\n"
        + "              base::File::FLAG_WRITE);\n"
        + "      if (!cmux_consent_lock->IsValid()) {\n"
        + "        return false;\n"
        + "      }\n"
        + lock_block
        + "      if (cmux_lock_acquired) {\n"
        + "        const base::FilePath cmux_disabled_marker =\n"
        + "            database_path->AppendASCII(\"cmux-upload-disabled\");\n"
        + "        if (cmux_client_uploads_enabled) {\n"
        + "          if ((base::PathExists(cmux_disabled_marker) &&\n"
        + "               !base::DeleteFile(cmux_disabled_marker)) ||\n"
        + "              !cmux_database->GetSettings()->SetUploadsEnabled(true)) {\n"
        + "            return false;\n"
        + "          }\n"
        + "        } else {\n"
        + "          base::File cmux_disabled_file(\n"
        + "              cmux_disabled_marker,\n"
        + "              base::File::FLAG_OPEN_ALWAYS | base::File::FLAG_WRITE);\n"
        + "          if (!cmux_disabled_file.IsValid() ||\n"
        + "              !cmux_database->GetSettings()->SetUploadsEnabled(false)) {\n"
        + "            return false;\n"
        + "          }\n"
        + "        }\n"
        + "      }\n"
        + unlock_block
        + "      if (cmux_lock_acquired) {\n"
        + "        static std::unique_ptr<base::File> cmux_process_consent_lock;\n"
        + "        cmux_process_consent_lock.swap(cmux_consent_lock);\n"
        + "      }\n"
        + "    }\n\n"
    )
    if platform == "linux":
        return (
            "#if !BUILDFLAG(IS_CHROMEOS)\n"
            + block
            + "#endif  // !BUILDFLAG(IS_CHROMEOS)\n"
        )
    return block


def patched_initial_consent() -> str:
    return (
        INITIAL_CONSENT_MARKER
        + "    // must not overwrite the lock owner's configured value.\n"
        + "#if !BUILDFLAG(IS_WIN) && !BUILDFLAG(IS_LINUX)\n"
        + ORIGINAL_INITIAL_CONSENT
        + "#endif\n"
    )


def patch_initial_consent(source: str) -> str:
    expected = patched_initial_consent()
    if INITIAL_CONSENT_MARKER in source:
        if source.count(INITIAL_CONSENT_MARKER) != 1 or expected not in source:
            raise AssertionError(
                "existing cmux initial Crashpad consent patch differs from "
                "the reviewed primary-owner guard"
            )
        return source
    if source.count(ORIGINAL_INITIAL_CONSENT) != 1:
        raise AssertionError(
            "Crashpad initial consent source anchor changed or is ambiguous"
        )
    return source.replace(ORIGINAL_INITIAL_CONSENT, expected, 1)


def patched_policy_branch() -> str:
    return (
        "  bool enable_uploads = false;\n"
        "  CrashReporterClient* crash_reporter_client = "
        "GetCrashReporterClient();\n"
        "  const bool cmux_reporting_enforced =\n"
        "      crash_reporter_client->ReportingIsEnforcedByPolicy("
        "&enable_uploads);\n"
        "  if (cmux_reporting_enforced) {\n"
        "#if BUILDFLAG(IS_WIN) || BUILDFLAG(IS_LINUX)\n"
        + CONSENT_POLICY_MARKER
        + "  enable_uploads = consent && enable_uploads;\n"
        "#endif\n"
        "  } else {\n"
    )


def patched_settings_write() -> str:
    return (
        DISABLED_MARKER_GUARD
        + "#if BUILDFLAG(IS_WIN) || BUILDFLAG(IS_LINUX)\n"
        + "  if (g_database_path &&\n"
        + "      base::PathExists(\n"
        + "          g_database_path->AppendASCII("
        + '"cmux-upload-disabled"))) {\n'
        + "    enable_uploads = false;\n"
        + "  } else if (g_database_path && !cmux_reporting_enforced) {\n"
        + "    enable_uploads =\n"
        + "        !crash_reporter_client->IsRunningUnattended();\n"
        + "  }\n"
        + "#endif\n"
        + ORIGINAL_SETTINGS_WRITE
    )


def patch_consent_policy(source: str) -> str:
    expected_branch = patched_policy_branch()
    expected_settings = patched_settings_write()
    if CONSENT_POLICY_MARKER in source or DISABLED_MARKER_GUARD in source:
        if (
            source.count(CONSENT_POLICY_MARKER) != 1
            or source.count(DISABLED_MARKER_GUARD) != 1
            or expected_branch not in source
            or expected_settings not in source
            or BASE_FILE_UTIL_INCLUDE not in source
        ):
            raise AssertionError(
                "existing cmux Crashpad consent policy patch differs from "
                "the reviewed hard opt-out guard"
            )
        return source
    if source.count(ORIGINAL_POLICY_BRANCH) != 1:
        raise AssertionError(
            "Crashpad upload-policy source anchor changed or is ambiguous"
        )
    if source.count(ORIGINAL_SETTINGS_WRITE) != 1:
        raise AssertionError(
            "Crashpad settings-write source anchor changed or is ambiguous"
        )
    if BASE_FILE_UTIL_INCLUDE not in source:
        include_anchor = '#include "base/command_line.h"\n'
        if source.count(include_anchor) != 1:
            raise AssertionError(
                "Crashpad file-util include anchor changed or is ambiguous"
            )
        source = source.replace(
            include_anchor, include_anchor + BASE_FILE_UTIL_INCLUDE, 1
        )
    source = source.replace(ORIGINAL_POLICY_BRANCH, expected_branch, 1)
    return source.replace(ORIGINAL_SETTINGS_WRITE, expected_settings, 1)


def patch_prelaunch_consent(source: str, platform: str) -> str:
    if platform not in START_HANDLER_ANCHORS:
        raise ValueError(f"unsupported Crashpad platform: {platform}")

    block = early_consent_block(platform)
    anchor = START_HANDLER_ANCHORS[platform]
    if EARLY_CONSENT_MARKER in source:
        base_include_block = {
            "linux": LINUX_BASE_FILE_INCLUDE_BLOCK,
            "windows": WINDOWS_BASE_FILE_INCLUDE_BLOCK,
        }[platform]
        if (
            source.count(EARLY_CONSENT_MARKER) != 1
            or block not in source
            or base_include_block not in source
            or CRASH_DATABASE_INCLUDE not in source
            or CRASH_SETTINGS_INCLUDE not in source
        ):
            raise AssertionError(
                f"existing {platform} pre-handler consent patch differs from "
                "the reviewed fail-closed block"
            )
        return source

    include_anchor = CRASHPAD_CLIENT_INCLUDE + CRASHPAD_INFO_INCLUDE
    if source.count(include_anchor) != 1:
        raise AssertionError(
            f"{platform} Crashpad include anchor changed or is ambiguous"
        )
    if source.count(anchor) != 1:
        raise AssertionError(
            f"{platform} Crashpad StartHandler anchor changed or is ambiguous"
        )
    if "#include <memory>\n" not in source:
        memory_include_anchor = "#include <limits>\n"
        if platform != "linux" or source.count(memory_include_anchor) != 1:
            raise AssertionError(
                f"{platform} <memory> include anchor changed or is ambiguous"
            )
        source = source.replace(
            memory_include_anchor,
            memory_include_anchor + "#include <memory>\n",
            1,
        )
    if platform == "linux":
        system_include_anchor = "#include <pthread.h>\n"
        system_includes = "#include <errno.h>\n#include <fcntl.h>\n"
        if system_includes not in source:
            if source.count(system_include_anchor) != 1:
                raise AssertionError(
                    "linux native-lock include anchor changed or is ambiguous"
                )
            source = source.replace(
                system_include_anchor,
                system_includes + system_include_anchor,
                1,
            )
    base_include_block = {
        "linux": LINUX_BASE_FILE_INCLUDE_BLOCK,
        "windows": WINDOWS_BASE_FILE_INCLUDE_BLOCK,
    }[platform]
    if base_include_block not in source:
        if platform == "linux":
            base_include_anchor = '#include "base/command_line.h"\n'
            replacement = LINUX_BASE_FILE_INCLUDE_BLOCK
        else:
            base_include_anchor = BASE_FILE_UTIL_INCLUDE
            replacement = WINDOWS_BASE_FILE_INCLUDE_BLOCK
        if source.count(base_include_anchor) != 1:
            raise AssertionError(
                f"{platform} base::File include anchor changed or is ambiguous"
            )
        source = source.replace(
            base_include_anchor,
            replacement,
            1,
        )
    source = source.replace(
        include_anchor,
        CRASH_DATABASE_INCLUDE
        + CRASHPAD_CLIENT_INCLUDE
        + CRASHPAD_INFO_INCLUDE
        + CRASH_SETTINGS_INCLUDE,
        1,
    )
    return source.replace(anchor, block + anchor, 1)


def patch_chromium(chromium_src: Path) -> bool:
    originals = {
        CRASH_REPORTER_CLIENT: (
            chromium_src / CRASH_REPORTER_CLIENT
        ).read_text(encoding="utf-8"),
        CRASHPAD_CORE: (chromium_src / CRASHPAD_CORE).read_text(
            encoding="utf-8"
        ),
        CRASHPAD_HEADER: (chromium_src / CRASHPAD_HEADER).read_text(
            encoding="utf-8"
        ),
        **{
            path: (chromium_src / path).read_text(encoding="utf-8")
            for path in CRASHPAD_STARTUP_FILES.values()
        },
    }
    updated = {
        CRASH_REPORTER_CLIENT: patch_upload_url(
            originals[CRASH_REPORTER_CLIENT]
        ),
        CRASHPAD_CORE: patch_consent_policy(
            patch_initial_consent(
                patch_sync_consent_implementation(originals[CRASHPAD_CORE])
            )
        ),
        CRASHPAD_HEADER: patch_sync_consent_declaration(
            originals[CRASHPAD_HEADER]
        ),
    }
    for platform, path in CRASHPAD_STARTUP_FILES.items():
        updated[path] = patch_prelaunch_consent(originals[path], platform)

    changed = False
    for path, text in updated.items():
        if text == originals[path]:
            continue
        (chromium_src / path).write_text(text, encoding="utf-8")
        changed = True
    return changed


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--chromium-src",
        type=Path,
        default=Path.cwd(),
        help="Chromium src checkout (defaults to the current directory)",
    )
    args = parser.parse_args()
    changed = patch_chromium(args.chromium_src.resolve())
    print(f"sentry-crashpad: {'patched' if changed else 'already patched'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
