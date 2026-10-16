// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "chrome/browser/cmux_term/cmux_extensions.h"

#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <utility>

#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/memory/raw_ptr.h"
#include "base/no_destructor.h"
#include "base/path_service.h"
#include "base/scoped_observation.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/single_thread_task_runner.h"
#include "base/task/task_traits.h"
#include "base/task/thread_pool.h"
#include "base/time/time.h"
#include "base/values.h"
#include "build/branding_buildflags.h"
#include "build/build_config.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/cmux_term/cmux_browser_finder.h"
#include "chrome/browser/cmux_term/cmux_extensions_container.h"
#include "chrome/browser/cmux_term/cmux_views.h"
#include "chrome/browser/extensions/extension_view_host.h"
#include "chrome/browser/extensions/external_provider_manager.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/profile_manager.h"
#include "chrome/browser/ui/extensions/extension_action_delegate.h"
#include "chrome/browser/ui/extensions/extension_action_view_model.h"
#include "chrome/common/chrome_version.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/common/chrome_paths.h"
#include "chrome/common/pref_names.h"
#include "components/prefs/pref_service.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/web_contents.h"
#include "extensions/browser/disable_reason.h"
#include "extensions/browser/extension_action.h"
#include "extensions/browser/extension_action_manager.h"
#include "extensions/browser/extension_prefs.h"
#include "extensions/browser/extension_registrar.h"
#include "extensions/browser/extension_registry.h"
#include "extensions/browser/extension_registry_observer.h"
#include "extensions/browser/pending_extension_manager.h"
#include "extensions/browser/pref_names.h"
#include "extensions/browser/unpacked_installer.h"
#include "extensions/common/extension.h"
#include "extensions/common/extension_set.h"
#include "extensions/common/extension_urls.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "ui/base/models/image_model.h"
#include "ui/gfx/geometry/size.h"
#include "ui/gfx/native_ui_types.h"
#include "ui/views/widget/widget.h"

#if BUILDFLAG(IS_WIN)
#include "base/win/registry.h"
#endif

namespace cmux {

const char kUBlockOriginExtensionId[] = "cjpalhdlnbpafiamejdnhcphjbkeiagm";
const char kBitwardenExtensionId[] = "nngceckbapebfimnlniiiahkandclblb";

namespace {

#if BUILDFLAG(IS_WIN)
// Keep this in sync with
// chrome/browser/extensions/external_registry_loader_win.cc:37-50,86-110,
// 132-140. This tree hard-codes the Google\Chrome key path even for Chromium
// branding, scans HKLM then HKCU, and reads the update_url value.
constexpr wchar_t kWindowsExternalExtensionsRegistryPath[] =
    L"Software\\Google\\Chrome\\Extensions";
constexpr wchar_t kWindowsExternalExtensionUpdateUrlValue[] = L"update_url";
#endif

base::FilePath GetProfilePath(Profile* profile) {
  return profile ? profile->GetPath() : base::FilePath();
}

Profile* ResolveProfileByPath(const base::FilePath& profile_path) {
  if (!g_browser_process || !g_browser_process->profile_manager()) {
    return nullptr;
  }
  return g_browser_process->profile_manager()->GetProfileByPath(profile_path);
}

bool GetBundledUBlockPath(base::FilePath* path) {
#if BUILDFLAG(IS_MAC)
  base::FilePath outer_bundle;
  if (!base::PathService::Get(chrome::DIR_OUTER_BUNDLE, &outer_bundle)) {
    return false;
  }
  *path = outer_bundle.Append(FILE_PATH_LITERAL("Contents"))
              .Append(FILE_PATH_LITERAL("Resources"))
              .Append(FILE_PATH_LITERAL("cmux-extensions"))
              .Append(FILE_PATH_LITERAL("ublock"));
  return true;
#else
  base::FilePath resources;
  if (!base::PathService::Get(chrome::DIR_RESOURCES, &resources)) {
    return false;
  }
  // chrome/common/chrome_paths.cc:263-272 maps DIR_RESOURCES to
  // base::DIR_ASSETS/resources on non-mac; base/base_paths.cc:68-75 maps
  // DIR_ASSETS to DIR_MODULE, e.g. <dir_module>\resources on Windows.
  *path = resources.Append(FILE_PATH_LITERAL("cmux-extensions"))
              .Append(FILE_PATH_LITERAL("ublock"));
  return true;
#endif
}

const char* BitwardenExternalInstallMechanism() {
#if BUILDFLAG(IS_WIN)
  return "registry";
#elif BUILDFLAG(IS_MAC) || BUILDFLAG(IS_CHROMEOS) || \
    (BUILDFLAG(IS_LINUX) && BUILDFLAG(CHROMIUM_BRANDING))
  return "external-json";
#else
  return "unsupported";
#endif
}

void CheckBitwardenExternalUpdatesForLoadedProfiles() {
  if (!g_browser_process || !g_browser_process->profile_manager()) {
    return;
  }

  for (Profile* profile :
       g_browser_process->profile_manager()->GetLoadedProfiles()) {
    if (!profile || profile->IsOffTheRecord()) {
      continue;
    }
    extensions::ExternalProviderManager::Get(profile)
        ->CheckForExternalUpdates();
  }
}

// The user-dir External Extensions JSON mechanism only exists on these
// platforms. Windows uses the registry instead; keep the helpers under their
// platform gates or they are defined-but-unused (-Werror).
#if BUILDFLAG(IS_MAC) || BUILDFLAG(IS_CHROMEOS) || \
    (BUILDFLAG(IS_LINUX) && BUILDFLAG(CHROMIUM_BRANDING))
struct ExternalJsonWriteResult {
  bool success = false;
  base::FilePath json_path;
  std::string error;
};

ExternalJsonWriteResult WriteBitwardenExternalJsonOnWorker(
    base::FilePath external_dir,
    std::string json) {
  ExternalJsonWriteResult result;
  result.json_path =
      external_dir.AppendASCII(std::string(kBitwardenExtensionId) + ".json");

  if (!base::CreateDirectory(external_dir)) {
    result.error = "failed to create " + external_dir.AsUTF8Unsafe();
    return result;
  }

  std::string existing;
  if (base::ReadFileToString(result.json_path, &existing) && existing == json) {
    result.success = true;
    return result;
  }

  if (!base::WriteFile(result.json_path, json)) {
    result.error = "failed to write " + result.json_path.AsUTF8Unsafe();
    return result;
  }

  result.success = true;
  return result;
}

void OnBitwardenExternalJsonWritten(ExternalJsonWriteResult result) {
  if (!result.success) {
    LOG(WARNING) << "cmux-ext: Bitwarden external install registration failed: "
                 << result.error;
    return;
  }

  LOG(WARNING) << "cmux-ext: Bitwarden external install registered at "
               << result.json_path.LossyDisplayName();

  CheckBitwardenExternalUpdatesForLoadedProfiles();
}
#endif  // IS_MAC || IS_CHROMEOS || (IS_LINUX && CHROMIUM_BRANDING)

#if BUILDFLAG(IS_WIN)
struct ExternalRegistryWriteResult {
  bool success = false;
  bool changed = false;
  std::wstring key_path;
  LONG result_code = ERROR_SUCCESS;
  std::string error;
};

ExternalRegistryWriteResult WriteBitwardenExternalRegistryOnWorker(
    std::string update_url) {
  ExternalRegistryWriteResult result;
  result.key_path = std::wstring(kWindowsExternalExtensionsRegistryPath)
                        .append(L"\\")
                        .append(base::ASCIIToWide(kBitwardenExtensionId));

  base::win::RegKey key;
  result.result_code = key.Create(HKEY_CURRENT_USER, result.key_path.c_str(),
                                  KEY_READ | KEY_SET_VALUE);
  if (result.result_code != ERROR_SUCCESS) {
    result.error =
        "failed to create/open HKCU\\" + base::WideToUTF8(result.key_path);
    return result;
  }

  const std::wstring expected_update_url = base::UTF8ToWide(update_url);
  std::wstring existing_update_url;
  if (key.ReadValue(kWindowsExternalExtensionUpdateUrlValue,
                    &existing_update_url) == ERROR_SUCCESS &&
      existing_update_url == expected_update_url) {
    result.success = true;
    return result;
  }

  result.result_code = key.WriteValue(kWindowsExternalExtensionUpdateUrlValue,
                                      expected_update_url.c_str());
  if (result.result_code != ERROR_SUCCESS) {
    result.error = "failed to write HKCU\\" +
                   base::WideToUTF8(result.key_path) + "\\update_url";
    return result;
  }

  result.success = true;
  result.changed = true;
  return result;
}

void OnBitwardenExternalRegistryWritten(ExternalRegistryWriteResult result) {
  if (!result.success) {
    LOG(WARNING) << "cmux-ext: Bitwarden registry external install "
                    "registration failed: "
                 << result.error << " error=" << result.result_code;
    return;
  }

  LOG(WARNING) << "cmux-ext: Bitwarden registry external install "
               << (result.changed ? "registered at " : "already registered at ")
               << "HKCU\\" << base::WideToUTF8(result.key_path);

  CheckBitwardenExternalUpdatesForLoadedProfiles();
}
#endif  // IS_WIN

bool IsBitwardenInstalled(Profile* profile) {
  if (!profile || profile->IsOffTheRecord()) {
    return false;
  }
  extensions::ExtensionRegistry* registry =
      extensions::ExtensionRegistry::Get(profile);
  return registry && registry->GetExtensionById(
                         kBitwardenExtensionId,
                         extensions::ExtensionRegistry::EVERYTHING) != nullptr;
}

void EnsureBitwardenExternalInstallRegistration(Profile* profile) {
  if (!profile || profile->IsOffTheRecord()) {
    return;
  }
  if (IsBitwardenInstalled(profile)) {
    LOG(WARNING) << "cmux-ext: Bitwarden already installed, skipping external "
                    "registration";
    return;
  }

#if BUILDFLAG(IS_MAC) || BUILDFLAG(IS_CHROMEOS) || \
    (BUILDFLAG(IS_LINUX) && BUILDFLAG(CHROMIUM_BRANDING))
  base::FilePath external_dir;
  if (!base::PathService::Get(chrome::DIR_USER_EXTERNAL_EXTENSIONS,
                              &external_dir)) {
    LOG(WARNING) << "cmux-ext: user external extension dir unavailable";
    return;
  }

  base::DictValue config;
  config.Set("external_update_url",
             extension_urls::GetWebstoreUpdateUrl().spec());
  config.Set("is_from_webstore", true);
  config.Set("may_be_untrusted", false);

  std::string json;
  base::JSONWriter::WriteWithOptions(
      config, base::JSONWriter::OPTIONS_PRETTY_PRINT, &json);
  json.push_back('\n');

  if (!base::ThreadPool::PostTaskAndReplyWithResult(
          FROM_HERE, {base::MayBlock()},
          base::BindOnce(&WriteBitwardenExternalJsonOnWorker, external_dir,
                         std::move(json)),
          base::BindOnce(&OnBitwardenExternalJsonWritten))) {
    LOG(WARNING) << "cmux-ext: failed to post Bitwarden external install "
                    "registration task";
  }
#elif BUILDFLAG(IS_WIN)
  if (!base::ThreadPool::PostTaskAndReplyWithResult(
          FROM_HERE, {base::MayBlock()},
          base::BindOnce(&WriteBitwardenExternalRegistryOnWorker,
                         extension_urls::GetWebstoreUpdateUrl().spec()),
          base::BindOnce(&OnBitwardenExternalRegistryWritten))) {
    LOG(WARNING) << "cmux-ext: failed to post Bitwarden registry external "
                    "install registration task";
  }
#else
  LOG(WARNING) << "cmux-ext: Bitwarden external install provider is not "
                  "available on this platform/build";
#endif
}

class BitwardenExternalInstallWatcher
    : public extensions::ExtensionRegistryObserver {
 public:
  explicit BitwardenExternalInstallWatcher(Profile* profile)
      : profile_(profile), profile_path_(GetProfilePath(profile)) {
    if (extensions::ExtensionRegistry* registry =
            extensions::ExtensionRegistry::Get(profile_)) {
      registry_observation_.Observe(registry);
    }
    AcknowledgeAndEnableIfPresent();
  }

  BitwardenExternalInstallWatcher(const BitwardenExternalInstallWatcher&) =
      delete;
  BitwardenExternalInstallWatcher& operator=(
      const BitwardenExternalInstallWatcher&) = delete;
  ~BitwardenExternalInstallWatcher() override = default;

  bool IsWatchingProfile(Profile* profile) const { return profile_ == profile; }

  void OnExtensionInstalled(content::BrowserContext*,
                            const extensions::Extension* extension,
                            bool) override {
    if (extension && extension->id() == kBitwardenExtensionId) {
      AcknowledgeAndEnableIfPresent();
    }
  }

  void OnExtensionLoaded(content::BrowserContext*,
                         const extensions::Extension* extension) override {
    if (extension && extension->id() == kBitwardenExtensionId) {
      AcknowledgeAndEnableIfPresent();
    }
  }

  void OnShutdown(extensions::ExtensionRegistry*) override {
    registry_observation_.Reset();
    // The watcher map intentionally survives until process exit. Release its
    // BackupRefPtr while the Profile is still alive so shutdown does not leave
    // a dangling reference for PartitionAlloc's AtExit check.
    profile_ = nullptr;
  }

 private:
  void AcknowledgeAndEnableIfPresent() {
    if (!profile_ || profile_->IsOffTheRecord()) {
      return;
    }

    extensions::ExtensionRegistry* registry =
        extensions::ExtensionRegistry::Get(profile_);
    if (!registry) {
      return;
    }

    const extensions::Extension* bitwarden = registry->GetExtensionById(
        kBitwardenExtensionId, extensions::ExtensionRegistry::EVERYTHING);
    if (!bitwarden) {
      return;
    }

    extensions::ExtensionPrefs* prefs =
        extensions::ExtensionPrefs::Get(profile_);
    prefs->AcknowledgeExternalExtension(kBitwardenExtensionId);
    const auto disable_reasons =
        prefs->GetDisableReasons(kBitwardenExtensionId);
    const bool only_external_prompt_disable =
        disable_reasons.size() == 1 &&
        disable_reasons.contains(
            extensions::disable_reason::DISABLE_EXTERNAL_EXTENSION);
    if (registry->disabled_extensions().GetByID(kBitwardenExtensionId) &&
        only_external_prompt_disable) {
      extensions::ExtensionRegistrar::Get(profile_)->EnableExtension(
          kBitwardenExtensionId);
      LOG(WARNING) << "cmux-ext: acknowledged and enabled Bitwarden external "
                      "install for "
                   << profile_path_.LossyDisplayName();
      return;
    }

    LOG(WARNING) << "cmux-ext: acknowledged Bitwarden external install for "
                 << profile_path_.LossyDisplayName();
  }

  raw_ptr<Profile> profile_;
  base::FilePath profile_path_;
  base::ScopedObservation<extensions::ExtensionRegistry,
                          extensions::ExtensionRegistryObserver>
      registry_observation_{this};
};

using WatcherMap = std::map<base::FilePath::StringType,
                            std::unique_ptr<BitwardenExternalInstallWatcher>>;

WatcherMap& GetBitwardenWatchers() {
  static base::NoDestructor<WatcherMap> watchers;
  return *watchers;
}

void WatchBitwardenExternalInstall(Profile* profile) {
  if (!profile || profile->IsOffTheRecord()) {
    return;
  }
  WatcherMap& watchers = GetBitwardenWatchers();
  const base::FilePath profile_path = GetProfilePath(profile);
  const base::FilePath::StringType profile_key = profile_path.value();
  auto existing = watchers.find(profile_key);
  if (existing != watchers.end()) {
    if (existing->second->IsWatchingProfile(profile)) {
      return;
    }
    // A Profile with this path previously shut down. Its watcher released the
    // old Profile pointer in OnShutdown(), so replace it for the reopened
    // Profile instead of leaving the path permanently unwatched.
    existing->second =
        std::make_unique<BitwardenExternalInstallWatcher>(profile);
    return;
  }
  watchers.emplace(profile_key,
                   std::make_unique<BitwardenExternalInstallWatcher>(profile));
}

void MaybeLoadBundledUBlock(Profile* profile) {
  if (!profile || profile->IsOffTheRecord()) {
    return;
  }

  base::FilePath ublock_path;
  if (!GetBundledUBlockPath(&ublock_path)) {
    LOG(WARNING) << "cmux-ext: unable to resolve bundled uBlock path";
    return;
  }

  extensions::ExtensionRegistry* registry =
      extensions::ExtensionRegistry::Get(profile);
  if (!registry) {
    LOG(WARNING) << "cmux-ext: no ExtensionRegistry for uBlock load";
    return;
  }

  const extensions::Extension* installed = registry->GetExtensionById(
      kUBlockOriginExtensionId, extensions::ExtensionRegistry::EVERYTHING);
  if (installed) {
    if (registry->disabled_extensions().GetByID(kUBlockOriginExtensionId)) {
      extensions::ExtensionRegistrar::Get(profile)->EnableExtension(
          kUBlockOriginExtensionId);
      LOG(WARNING) << "cmux-ext: re-enabled bundled uBlock Origin";
    } else {
      LOG(WARNING) << "cmux-ext: bundled uBlock Origin already loaded version "
                   << installed->VersionString();
    }
    return;
  }

  scoped_refptr<extensions::UnpackedInstaller> installer =
      extensions::UnpackedInstaller::Create(profile);
  installer->set_be_noisy_on_failure(false);
  installer->set_require_modern_manifest_version(false);
  installer->set_completion_callback(base::BindOnce(
      [](const extensions::Extension* extension, const base::FilePath& path,
         const std::u16string& error) {
        if (!extension) {
          LOG(WARNING) << "cmux-ext: bundled uBlock missing or unloadable at "
                       << path.LossyDisplayName()
                       << " (dev run without deploy bundle?): "
                       << base::UTF16ToUTF8(error);
          return;
        }
        LOG(WARNING) << "cmux-ext: loaded bundled " << extension->name()
                     << " id=" << extension->id()
                     << " version=" << extension->VersionString()
                     << " manifest_version=" << extension->manifest_version();
      }));
  installer->Load(ublock_path);
}

const extensions::Extension* FindExtension(
    extensions::ExtensionRegistry* registry,
    const std::string& id) {
  return registry ? registry->GetExtensionById(
                        id, extensions::ExtensionRegistry::EVERYTHING)
                  : nullptr;
}

std::string ExtensionState(extensions::ExtensionRegistry* registry,
                           const std::string& id) {
  if (!registry) {
    return "no-registry";
  }
  if (registry->enabled_extensions().GetByID(id)) {
    return "enabled";
  }
  if (registry->disabled_extensions().GetByID(id)) {
    return "disabled";
  }
  if (registry->terminated_extensions().GetByID(id)) {
    return "terminated";
  }
  if (registry->blocklisted_extensions().GetByID(id)) {
    return "blocklisted";
  }
  if (registry->blocked_extensions().GetByID(id)) {
    return "blocked";
  }
  return "not-installed";
}

int ActionCountFor(Profile* profile, const extensions::Extension& extension) {
  extensions::ExtensionActionManager* manager =
      extensions::ExtensionActionManager::Get(profile);
  return manager && manager->GetExtensionAction(extension) ? 1 : 0;
}

class SelfTestExtensionActionDelegate : public ExtensionActionDelegate {
 public:
  SelfTestExtensionActionDelegate() = default;
  SelfTestExtensionActionDelegate(const SelfTestExtensionActionDelegate&) =
      delete;
  SelfTestExtensionActionDelegate& operator=(
      const SelfTestExtensionActionDelegate&) = delete;
  ~SelfTestExtensionActionDelegate() override = default;

  void AttachToModel(ExtensionActionViewModel* model) override {}
  void DetachFromModel() override {}
  void RegisterCommand() override {}
  void UnregisterCommand() override {}
  bool IsShowingPopup() const override { return false; }
  void HidePopup() override {}
  gfx::NativeView GetPopupNativeViewForTesting() override {
    return gfx::NativeView();
  }
  void TriggerPopup(std::unique_ptr<extensions::ExtensionViewHost> host,
                    PopupShowAction show_action,
                    bool by_user,
                    ShowPopupCallback callback) override {
    if (callback) {
      std::move(callback).Run(nullptr);
    }
  }
  void ShowContextMenuAsFallback() override {}
#if CHROME_VERSION_MAJOR >= 151
  void CloseExtensionsMenuIfOpen() override {}
#else
  bool CloseOverflowMenuIfOpen() override { return false; }
#endif
};

void LogExtensionSet(Profile* profile,
                     const char* state,
                     const extensions::ExtensionSet& set) {
  for (const auto& extension : set) {
    LOG(WARNING) << "cmux-ext-selftest: extension state=" << state
                 << " id=" << extension->id() << " name=" << extension->name()
                 << " version=" << extension->VersionString()
                 << " manifest_version=" << extension->manifest_version()
                 << " action_count=" << ActionCountFor(profile, *extension);
  }
}

void RunActionToggleSelfTest(base::FilePath profile_path, int phase) {
  Profile* profile = ResolveProfileByPath(profile_path);
  CmuxExtensionsContainer* container =
      profile ? GetCmuxExtensionsContainerForSelfTest(profile) : nullptr;
  ToolbarActionViewModel* action =
      container ? container->GetActionForId(kUBlockOriginExtensionId)
                : nullptr;
  if (!action) {
    LOG(ERROR) << "cmux-ext-selftest: FAIL action toggle no native action";
    return;
  }

  if (phase == 0) {
    if (!container->ClickActionForSelfTest(kUBlockOriginExtensionId)) {
      LOG(ERROR) << "cmux-ext-selftest: FAIL action first click no button";
      return;
    }
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
        FROM_HERE,
        base::BindOnce(&RunActionToggleSelfTest, std::move(profile_path), 1),
        base::Milliseconds(500));
    return;
  }

  if (phase == 1) {
    if (!action->IsShowingPopup()) {
      LOG(ERROR) << "cmux-ext-selftest: FAIL action first click did not open";
      return;
    }
    LOG(WARNING) << "cmux-ext-selftest: PASS action first click opened popup";
    if (!container->ClickActionForSelfTest(kUBlockOriginExtensionId)) {
      LOG(ERROR) << "cmux-ext-selftest: FAIL action second click no button";
      return;
    }
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
        FROM_HERE,
        base::BindOnce(&RunActionToggleSelfTest, std::move(profile_path), 2),
        base::Milliseconds(250));
    return;
  }

  if (action->IsShowingPopup()) {
    LOG(ERROR) << "cmux-ext-selftest: FAIL action second click stayed open";
  } else {
    LOG(WARNING) << "cmux-ext-selftest: PASS action second click closed popup";
  }
}

void RunExtensionSelfTestForProfilePath(base::FilePath profile_path) {
  Profile* profile = ResolveProfileByPath(profile_path);
  if (!profile || profile->IsOffTheRecord()) {
    return;
  }
  extensions::ExtensionRegistry* registry =
      extensions::ExtensionRegistry::Get(profile);
  if (!registry) {
    LOG(ERROR) << "cmux-ext-selftest: FAIL no ExtensionRegistry";
    return;
  }

  LogExtensionSet(profile, "enabled", registry->enabled_extensions());
  LogExtensionSet(profile, "disabled", registry->disabled_extensions());
  LogExtensionSet(profile, "terminated", registry->terminated_extensions());
  LogExtensionSet(profile, "blocklisted", registry->blocklisted_extensions());
  LogExtensionSet(profile, "blocked", registry->blocked_extensions());

  const extensions::Extension* ublock =
      FindExtension(registry, kUBlockOriginExtensionId);
  if (!ublock) {
    LOG(ERROR) << "cmux-ext-selftest: FAIL uBlock Origin missing";
  } else if (ublock->manifest_version() != 2) {
    LOG(ERROR) << "cmux-ext-selftest: FAIL uBlock Origin manifest_version="
               << ublock->manifest_version();
  } else if (!registry->enabled_extensions().GetByID(
                 kUBlockOriginExtensionId)) {
    LOG(ERROR) << "cmux-ext-selftest: FAIL uBlock Origin state="
               << ExtensionState(registry, kUBlockOriginExtensionId);
  } else if (ublock->name() != "uBlock Origin") {
    LOG(ERROR) << "cmux-ext-selftest: FAIL unexpected uBlock name="
               << ublock->name();
  } else {
    LOG(WARNING) << "cmux-ext-selftest: PASS uBlock Origin full MV2 enabled";
  }

  CmuxExtensionsContainer* container =
      GetCmuxExtensionsContainerForSelfTest(profile);
  if (!container ||
      !container->IsActionContainerHighlightAlignedForTesting(
          kUBlockOriginExtensionId)) {
    LOG(ERROR)
        << "cmux-ext-selftest: FAIL extension highlight border misaligned";
  } else {
    LOG(WARNING)
        << "cmux-ext-selftest: PASS extension highlight border aligned";
  }

  content::WebContents* web_contents = GetActiveCmuxWebContents();
  Browser* browser = cmux::FindBrowserWithTab(web_contents);
  extensions::ExtensionActionManager* action_manager =
      extensions::ExtensionActionManager::Get(profile);
  if (!browser) {
    LOG(ERROR) << "cmux-ext-selftest: FAIL chrome action viewmodel no Browser";
  } else if (!ublock ||
             !registry->enabled_extensions().GetByID(
                 kUBlockOriginExtensionId) ||
             !action_manager || !action_manager->GetExtensionAction(*ublock)) {
    LOG(ERROR) << "cmux-ext-selftest: FAIL chrome action viewmodel no action";
  } else {
    auto view_model = ExtensionActionViewModel::Create(
        kUBlockOriginExtensionId, browser,
        std::make_unique<SelfTestExtensionActionDelegate>());
    if (!view_model) {
      LOG(ERROR) << "cmux-ext-selftest: FAIL chrome action viewmodel no model";
    } else {
      ui::ImageModel icon =
          view_model->GetIcon(web_contents, gfx::Size(24, 24));
      const std::u16string accessible_name =
          view_model->GetAccessibleName(web_contents);
      if (icon.IsEmpty() || accessible_name.empty()) {
        LOG(ERROR)
            << "cmux-ext-selftest: FAIL chrome action viewmodel icon_empty="
            << icon.IsEmpty()
            << " accessible_empty=" << accessible_name.empty();
      } else {
        LOG(WARNING) << "cmux-ext-selftest: PASS chrome action viewmodel";
      }
      view_model->HidePopup();
    }
  }

  RunCmuxExtensionsMenuSelfTest(profile);
  RunActionToggleSelfTest(profile_path, 0);

  if (ublock &&
      registry->enabled_extensions().GetByID(kUBlockOriginExtensionId)) {
    scoped_refptr<const extensions::Extension> ublock_ref(ublock);
    if (!ShowExtensionPostInstallDialogAtPuzzle(profile, std::move(ublock_ref),
                                                SkBitmap())) {
      LOG(ERROR) << "cmux-ext-selftest: FAIL post-install dialog did not open";
    } else {
      container = GetCmuxExtensionsContainerForSelfTest(profile);
      views::Widget* widget =
          container ? container->GetPostInstallDialogWidgetForTesting()
                    : nullptr;
      if (!container || !container->IsPostInstallDialogShowingForTesting() ||
          !widget || widget->IsClosed()) {
        LOG(ERROR) << "cmux-ext-selftest: FAIL post-install dialog no widget";
      } else {
        LOG(WARNING) << "cmux-ext-selftest: PASS post-install dialog";
      }
      if (widget && !widget->IsClosed()) {
        widget->CloseNow();
      }
    }
  }

  const extensions::Extension* bitwarden =
      FindExtension(registry, kBitwardenExtensionId);
  extensions::PendingExtensionManager* pending =
      extensions::PendingExtensionManager::Get(profile);
  LOG(WARNING) << "cmux-ext-selftest: Bitwarden state="
               << ExtensionState(registry, kBitwardenExtensionId)
               << " mechanism=" << BitwardenExternalInstallMechanism()
               << " pending_external_install="
               << (pending && pending->IsIdPending(kBitwardenExtensionId))
               << " version="
               << (bitwarden ? bitwarden->VersionString() : std::string());
}

void RunExtensionSelfTestWhenReady(base::FilePath profile_path, int attempt) {
  Profile* profile = ResolveProfileByPath(profile_path);
  CmuxExtensionsContainer* container =
      profile ? GetCmuxExtensionsContainerForSelfTest(profile) : nullptr;
  if (container &&
      container->GetActionForId(kUBlockOriginExtensionId)) {
    RunExtensionSelfTestForProfilePath(std::move(profile_path));
    return;
  }

  constexpr int kMaxReadinessAttempts = 60;
  if (attempt >= kMaxReadinessAttempts) {
    LOG(ERROR) << "cmux-ext-selftest: FAIL active toolbar did not become ready";
    return;
  }
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
      FROM_HERE,
      base::BindOnce(&RunExtensionSelfTestWhenReady, std::move(profile_path),
                     attempt + 1),
      base::Milliseconds(500));
}

}  // namespace

void RegisterUserExternalExtensions() {
  // Bitwarden registration is intentionally deferred to
  // RegisterProfileExtensions() so warm profiles can check
  // ExtensionRegistry::EVERYTHING and skip the external JSON/registry write
  // plus CheckForExternalUpdates().
}

void EnsureDefaultPinnedExtensions(Profile* profile) {
  if (!profile || profile->IsOffTheRecord() || !profile->GetPrefs()) {
    return;
  }

  const PrefService::Preference* pref = profile->GetPrefs()->FindPreference(
      extensions::pref_names::kPinnedExtensions);
  if (!pref || !pref->IsDefaultValue()) {
    return;
  }

  extensions::ExtensionPrefs::Get(profile)->SetPinnedExtensions(
      {kUBlockOriginExtensionId, kBitwardenExtensionId});
}

void RegisterProfileExtensions(Profile* profile) {
  // The bundled uBlock Origin loads UNPACKED (mojom::ManifestLocation::
  // kUnpacked). Upstream's kExtensionDisableUnsupportedDeveloper enforcement
  // disables unpacked extensions unless the profile is in developer mode
  // (ExtensionManagement::IsAllowedByUnpackedDeveloperModePolicy reads
  // prefs::kExtensionsUIDeveloperMode; CheckManagementPolicy re-evaluates on
  // pref change, which also re-enables an already-disabled install). cmux
  // ships an unpacked extension on purpose, so developer mode is part of the
  // product configuration, not a debug toggle.
  if (profile && profile->GetPrefs()) {
    profile->GetPrefs()->SetBoolean(prefs::kExtensionsUIDeveloperMode, true);
  }
  MaybeLoadBundledUBlock(profile);
  WatchBitwardenExternalInstall(profile);
  EnsureBitwardenExternalInstallRegistration(profile);

  if (std::getenv("CMUX_EXT_SELFTEST")) {
    base::FilePath profile_path = GetProfilePath(profile);
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
        FROM_HERE,
        base::BindOnce(&RunExtensionSelfTestWhenReady,
                       std::move(profile_path), 0),
        base::Seconds(8));
  }
}

}  // namespace cmux
