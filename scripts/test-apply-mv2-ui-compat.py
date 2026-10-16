#!/usr/bin/env python3
"""Exercise M149/M150/M151 MV2 policy and UI patches on every host."""

import os
from pathlib import Path
import tempfile


ROOT = Path(__file__).resolve().parent.parent
APPLY = (ROOT / "scripts/apply.sh").read_text()
WIN_APPLY = (ROOT / "scripts/apply_win_chrome.py").read_text()
PATCH_SOURCES = (
    ("apply.sh", APPLY, "\nPYEOF\""),
    ("apply_win_chrome.py", WIN_APPLY, '\nprint("--- next patch ---")'),
)


def patches_for(path_assignment: str) -> tuple[tuple[str, str], ...]:
    patches = []
    for patcher, source, terminator in PATCH_SOURCES:
        start = source.index(path_assignment)
        try:
            end = source.index(terminator, start)
        except ValueError:
            # The unpacked-warning block is the final Windows MV2 patch.
            end = source.index('\nprint("APPLY_WIN_MV2 OK")', start)
        patches.append((patcher, source[start:end]))
    return tuple(patches)


def execute(
    relative_path: str,
    source: str,
    patch: str,
    extra_files: dict[str, str] | None = None,
) -> str:
    with tempfile.TemporaryDirectory() as temp:
        checkout = Path(temp)
        path = checkout / relative_path
        path.parent.mkdir(parents=True)
        path.write_text(source)
        for extra_relative, extra_source in (extra_files or {}).items():
            extra_path = checkout / extra_relative
            extra_path.parent.mkdir(parents=True, exist_ok=True)
            extra_path.write_text(extra_source)
        old_cwd = Path.cwd()
        try:
            os.chdir(checkout)
            exec(
                compile(patch, "MV2 compatibility patch", "exec"),
                {"Path": Path},
            )
            exec(
                compile(patch, "MV2 compatibility patch rerun", "exec"),
                {"Path": Path},
            )
        finally:
            os.chdir(old_cwd)
        return path.read_text()


def management_fixture(extension_type: str, login_type: str, user_script: str) -> str:
    types = f"""  if (manifest_type != Manifest::Type::{extension_type} &&
      manifest_type != Manifest::Type::{login_type}{user_script}) {{
    return false;
  }}
"""
    return f"""bool ExtensionManagement::IsAllowedManifestVersion(
    int manifest_version,
    const std::string& extension_id,
    Manifest::Type manifest_type) {{
{types}  return manifest_version >= 3;
}}

bool ExtensionManagement::IsAllowedManifestVersion(const Extension* extension) {{
  return false;
}}

bool ExtensionManagement::IsExemptFromMV2DeprecationByPolicy(
    int manifest_version,
    const std::string& extension_id,
    Manifest::Type manifest_type) {{
{types}  return false;
}}

bool ExtensionManagement::IsAllowedByUnpublishedAvailabilityPolicy(
    const Extension* extension) {{
  return true;
}}
"""


management_patches = patches_for(
    "p = 'chrome/browser/extensions/extension_management.cc'"
)
for name, extension_type, login_type, user_script in (
    ("M149", "TYPE_EXTENSION", "TYPE_LOGIN_SCREEN_EXTENSION", ""),
    ("M150", "kExtension", "kLoginScreenExtension", " &&\n      manifest_type != Manifest::Type::kUserScript"),
):
    for patcher, management_patch in management_patches:
        patched = execute(
            "chrome/browser/extensions/extension_management.cc",
            management_fixture(extension_type, login_type, user_script),
            management_patch,
        )
        assert "This keeps Manifest V2" in patched
        assert "MV2 extensions that pass" in patched
        if name == "M150":
            assert "Manifest::Type::kUserScript" in patched
        print(f"PASS {patcher}: Chromium {name[1:]} extension management")

for patcher, management_patch in management_patches:
    patched = execute(
        "chrome/browser/extensions/extension_management.cc",
        "bool unrelated_policy() { return true; }\n",
        management_patch,
        {
            "extensions/browser/manifest_v2_handler.cc":
                "bool ShouldBlockExtensionInstallation();\n"
                "bool ShouldBlockExtensionEnable();\n"
        },
    )
    assert patched == "bool unrelated_policy() { return true; }\n"
    print(f"PASS {patcher}: Chromium 151 extension management")


developer_patches = patches_for(
    "p = 'chrome/browser/extensions/api/developer_private/extension_info_generator.cc'"
)
developer_common = '''  // MV2 deprecation.
  ManifestV2ExperimentManager* mv2_experiment_manager =
      ManifestV2ExperimentManager::Get(profile);
  CHECK(mv2_experiment_manager);
  info.is_affected_by_mv2_deprecation =
      mv2_experiment_manager->IsExtensionAffected(extension);
'''
developer_149 = developer_common + '''  info.did_acknowledge_mv2_deprecation_notice =
      mv2_experiment_manager->DidUserAcknowledgeNotice(extension.id());
  if (info.web_store_url.length() > 0) {
    info.recommendations_url =
        extension_urls::GetNewWebstoreItemRecommendationsUrl(extension.id())
            .spec();
  }
'''
developer_150 = developer_common + '''  if (info.web_store_url.length() > 0) {
    info.recommendations_url =
        extension_urls::GetNewWebstoreItemRecommendationsUrl(extension.id())
            .spec();
  }
'''
developer_151 = '''  // MV2 deprecation.
  ManifestV2Handler* mv2_handler = ManifestV2Handler::Get(profile);
  CHECK(mv2_handler);
  info.is_affected_by_mv2_deprecation =
      mv2_handler->IsExtensionAffected(extension);
''' + developer_150.split(
    "  info.is_affected_by_mv2_deprecation =\n"
    "      mv2_experiment_manager->IsExtensionAffected(extension);\n", 1
)[1]
for name, source in (
    ("149", developer_149),
    ("150", developer_150),
    ("151", developer_151),
):
    for patcher, developer_patch in developer_patches:
        patched = execute(
            "chrome/browser/extensions/api/developer_private/extension_info_generator.cc",
            source,
            developer_patch,
        )
        assert "is_affected_by_mv2_deprecation = false" in patched
        assert "recommendations_url" not in patched
        assert ("did_acknowledge_mv2_deprecation_notice = true" in patched) == (
            name == "149"
        )
        print(f"PASS {patcher}: Chromium {name} developer-private MV2 UI")


webui_patches = patches_for("p = 'chrome/browser/ui/webui/extensions/extensions_ui.cc'")
webui_149 = '''  // MV2 deprecation.
  auto* mv2_experiment_manager = ManifestV2ExperimentManager::Get(profile);
  MV2ExperimentStage experiment_stage =
      mv2_experiment_manager->GetCurrentExperimentStage();
  source->AddInteger("MV2ExperimentStage", static_cast<int>(experiment_stage));
  source->AddBoolean(
      "MV2DeprecationNoticeDismissed",
      mv2_experiment_manager->DidUserAcknowledgeNoticeGlobally());
'''
webui_150 = '''  // MV2 deprecation.
  auto* mv2_experiment_manager = ManifestV2ExperimentManager::Get(profile);
  source->AddBoolean(
      "MV2DeprecationNoticeDismissed",
      mv2_experiment_manager->DidUserAcknowledgeNoticeGlobally());
'''
webui_151 = '''  // MV2 deprecation.
  auto* mv2_handler = ManifestV2Handler::Get(profile);
  source->AddBoolean("MV2DeprecationNoticeDismissed",
                     mv2_handler->DidUserAcknowledgeNoticeGlobally());
'''
for name, source in (
    ("149", webui_149),
    ("150", webui_150),
    ("151", webui_151),
):
    for patcher, webui_patch in webui_patches:
        patched = execute(
            "chrome/browser/ui/webui/extensions/extensions_ui.cc", source, webui_patch
        )
        assert 'source->AddBoolean("MV2DeprecationNoticeDismissed", true)' in patched
        assert ("MV2ExperimentStage::kWarning" in patched) == (name == "149")
        assert "DidUserAcknowledgeNoticeGlobally" not in patched
        print(f"PASS {patcher}: Chromium {name} extensions WebUI")


warning_patches = patches_for("p = 'extensions/common/extension.cc'")
warning_150 = '''    // Emit a warning for unpacked extensions on Manifest V2 warning that
    // MV2 is deprecated.
    if (type == Manifest::Type::kExtension && manifest_version == 2 &&
        Manifest::IsUnpackedLocation(location) &&
        !g_silence_deprecated_manifest_version_warnings) {
      *warning = errors::kManifestV2IsDeprecatedWarning;
    }
'''
warning_151 = '''    // Emit a warning for unpacked extensions on Manifest V2 warning that
    // MV2 is deprecated.
    if (type == Manifest::Type::kExtension && manifest_version == 2 &&
        Manifest::IsUnpackedLocation(location)) {
      *warning = errors::kManifestV2IsDeprecatedWarning;
    }
'''
for name, source in (("150", warning_150), ("151", warning_151)):
    for patcher, warning_patch in warning_patches:
        patched = execute("extensions/common/extension.cc", source, warning_patch)
        assert "kManifestV2IsDeprecatedWarning" not in patched
        assert "full MV2 extensions remain supported" in patched
        print(f"PASS {patcher}: Chromium {name} unpacked MV2 warning")
