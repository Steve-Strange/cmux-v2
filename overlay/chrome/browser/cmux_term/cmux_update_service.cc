// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "chrome/browser/cmux_term/cmux_update_service.h"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "base/base64.h"
#include "base/containers/span.h"
#include "base/environment.h"
#include "base/files/file.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/logging.h"
#include "base/memory/weak_ptr.h"
#include "base/no_destructor.h"
#include "base/observer_list.h"
#include "base/path_service.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/task/thread_pool.h"
#include "base/task/task_traits.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "build/build_config.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/cmux_term/cmux_update_installer.h"
#include "chrome/browser/cmux_term/cmux_update_network.h"
#include "chrome/browser/lifetime/application_lifetime.h"
#include "chrome/browser/net/system_network_context_manager.h"
#include "chrome/common/chrome_paths.h"
#include "components/version_info/version_info.h"
#include "crypto/secure_hash.h"
#include "crypto/signature_verifier.h"
#include "net/base/load_flags.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "services/network/public/cpp/simple_url_loader.h"
#include "services/network/public/mojom/fetch_api.mojom.h"
#include "third_party/zlib/google/zip.h"
#include "url/gurl.h"

namespace cmux {
namespace {

constexpr char kDefaultFeedUrl[] =
    "https://github.com/manaflow-ai/cmux-v2/releases/latest/download/"
    "update.json";
constexpr char kInstalledFeedFile[] = "cmux-update-feed-url";
constexpr char kNightlyFeedUrl[] =
    "https://github.com/manaflow-ai/cmux-v2/releases/download/nightly/"
    "update.json";
constexpr char kLegacyNightlyFeedUrl[] =
    "https://github.com/manaflow-ai/cmux-browser/releases/download/nightly/"
    "update.json";
constexpr size_t kMaxManifestBytes = 256 * 1024;
constexpr int64_t kMaxPackageBytes = INT64_C(2) * 1024 * 1024 * 1024;
constexpr base::TimeDelta kCheckInterval = base::Hours(6);

// P-256 SubjectPublicKeyInfo. The corresponding private key is generated once
// into the gitignored .release-keys directory and is consumed only by the
// release manifest tool. Fingerprint:
// SHA-256 8feeefcadd6c55cfeb3dc3aeb52ca95649d914cc1430c9c798c9567374b6f657
constexpr std::array<uint8_t, 91> kUpdatePublicKey = {
    0x30, 0x59, 0x30, 0x13, 0x06, 0x07, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x02,
    0x01, 0x06, 0x08, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x03, 0x01, 0x07, 0x03,
    0x42, 0x00, 0x04, 0xcd, 0xe1, 0x39, 0xfc, 0x7b, 0x6d, 0x8c, 0xbc, 0xf6,
    0xee, 0x15, 0x37, 0x2c, 0x55, 0x34, 0x87, 0xff, 0x3c, 0x13, 0x1e, 0xe2,
    0xa2, 0xa6, 0x63, 0x32, 0x76, 0x33, 0xdc, 0x03, 0x7f, 0x8b, 0x27, 0xcd,
    0x6f, 0x3d, 0x66, 0x79, 0x92, 0xff, 0xf0, 0xac, 0x9d, 0xdc, 0xfb, 0x2f,
    0x0a, 0xfa, 0x22, 0x00, 0x8e, 0xb6, 0x77, 0x30, 0x1d, 0x00, 0x4f, 0xa0,
    0x36, 0x46, 0xcd, 0xad, 0xfc, 0x3a, 0xfd};

const net::NetworkTrafficAnnotationTag kUpdateTrafficAnnotation =
    net::DefineNetworkTrafficAnnotation("cmux_background_update", R"(
      semantics {
        sender: "cmux automatic updater"
        description:
          "Checks cmux's signed release manifest and downloads a complete "
          "desktop update only while the OS reports an unmetered connection."
        trigger: "Browser startup, every six hours, or becoming unmetered."
        data: "No user data. The request identifies only the platform build."
        destination: WEBSITE
      }
      policy {
        cookies_allowed: NO
        setting:
          "Automatic update checks are part of cmux desktop distribution."
        policy_exception_justification:
          "No enterprise policy is currently exposed by this early fork."
      })");

struct PackageEntry {
  std::string version;
  GURL url;
  std::string sha256;
  int64_t size = 0;
  std::string archive_root;
  std::string executable;
};

struct PackagePathPreparation {
  bool success = false;
  bool cached_archive = false;
};

std::string EnvironmentValue(base::cstring_view name) {
  return base::Environment::Create()->GetVar(name).value_or(std::string());
}

std::optional<std::string> InstalledFeedFrom(
    const base::FilePath& directory) {
  std::string feed;
  if (!base::ReadFileToStringWithMaxSize(
          directory.AppendASCII(kInstalledFeedFile), &feed, 4096)) {
    return std::nullopt;
  }
  base::TrimWhitespaceASCII(feed, base::TRIM_ALL, &feed);
  if (feed.empty()) {
    return std::nullopt;
  }
  // Early nightly packages pointed at the private source repository.
  // Anonymous updater requests cannot read private GitHub release assets, so
  // migrate that one shipped selector to the public distribution repo.
  return feed == kLegacyNightlyFeedUrl ? kNightlyFeedUrl : feed;
}

std::string ConfiguredFeedUrl() {
  std::string feed = EnvironmentValue("CMUX_UPDATE_FEED_URL");
  if (!feed.empty()) {
    return feed;
  }

#if BUILDFLAG(IS_MAC)
  // Plain data in Contents/MacOS requires extended-attribute code-signing
  // metadata, which normal ZIP extraction drops. Keep the selector in the
  // bundle's sealed Resources directory. Retain the executable-dir lookup
  // below for any early local packages.
  base::FilePath resources_dir;
  if (base::PathService::Get(chrome::DIR_RESOURCES, &resources_dir)) {
    if (std::optional<std::string> installed =
            InstalledFeedFrom(resources_dir)) {
      return *installed;
    }
  }
#endif

  base::FilePath executable_dir;
  if (base::PathService::Get(base::DIR_EXE, &executable_dir)) {
    if (std::optional<std::string> installed =
            InstalledFeedFrom(executable_dir)) {
      return *installed;
    }
  }
  return kDefaultFeedUrl;
}

std::string CurrentVersion() {
  std::string value = EnvironmentValue("CMUX_UPDATE_CURRENT_VERSION");
  return value.empty() ? std::string(version_info::GetVersionNumber()) : value;
}

std::string PlatformKey() {
#if BUILDFLAG(IS_MAC)
#if defined(ARCH_CPU_ARM64)
  return "mac-arm64";
#else
  return "mac-x64";
#endif
#elif BUILDFLAG(IS_WIN)
#if defined(ARCH_CPU_ARM64)
  return "windows-arm64";
#else
  return "windows-x64";
#endif
#else
#if defined(ARCH_CPU_ARM64)
  return "linux-arm64";
#else
  return "linux-x64";
#endif
#endif
}

bool SafeRelativePath(const std::string& value) {
  if (value.empty()) {
    return false;
  }
  const base::FilePath path = base::FilePath::FromUTF8Unsafe(value);
  return !path.IsAbsolute() && !path.ReferencesParent();
}

bool VerifyEnvelope(const std::string& envelope, std::string* payload) {
  std::optional<base::DictValue> outer =
      base::JSONReader::ReadDict(envelope, base::JSON_PARSE_RFC);
  if (!outer) {
    return false;
  }
  const std::string* encoded_payload = outer->FindString("payload");
  const std::string* encoded_signature = outer->FindString("signature");
  std::string signature;
  if (!encoded_payload || !encoded_signature ||
      !base::Base64Decode(*encoded_payload, payload) ||
      !base::Base64Decode(*encoded_signature, &signature)) {
    return false;
  }
  crypto::SignatureVerifier verifier;
  if (!verifier.VerifyInit(crypto::SignatureVerifier::ECDSA_SHA256,
                           base::as_byte_span(signature), kUpdatePublicKey)) {
    return false;
  }
  verifier.VerifyUpdate(base::as_byte_span(*payload));
  return verifier.VerifyFinal();
}

std::optional<PackageEntry> ParseManifest(const std::string& envelope) {
  std::string payload;
  if (!VerifyEnvelope(envelope, &payload)) {
    return std::nullopt;
  }
  std::optional<base::DictValue> parsed =
      base::JSONReader::ReadDict(payload, base::JSON_PARSE_RFC);
  if (!parsed) {
    return std::nullopt;
  }
  const base::DictValue& manifest = *parsed;
  if (manifest.FindInt("schema").value_or(0) != 1) {
    return std::nullopt;
  }
  const std::string* version = manifest.FindString("version");
  const base::DictValue* platforms = manifest.FindDict("platforms");
  const base::DictValue* platform =
      platforms ? platforms->FindDict(PlatformKey()) : nullptr;
  if (!version || !platform ||
      CompareUpdateVersions(*version, CurrentVersion()) <= 0) {
    return std::nullopt;
  }
  const std::string* url = platform->FindString("url");
  const std::string* sha256 = platform->FindString("sha256");
  const std::string* size_string = platform->FindString("size");
  const std::string* archive_root = platform->FindString("archive_root");
  const std::string* executable = platform->FindString("executable");
  PackageEntry entry;
  entry.version = *version;
  entry.url = url ? GURL(*url) : GURL();
  entry.sha256 = sha256 ? base::ToLowerASCII(*sha256) : std::string();
  if (!size_string || !base::StringToInt64(*size_string, &entry.size) ||
      !entry.url.is_valid() || !entry.url.SchemeIsHTTPOrHTTPS() ||
      entry.sha256.size() != 64 || entry.size <= 0 ||
      entry.size > kMaxPackageBytes || !archive_root || !executable ||
      !SafeRelativePath(*archive_root) || !SafeRelativePath(*executable)) {
    return std::nullopt;
  }
  entry.archive_root = *archive_root;
  entry.executable = *executable;
  return entry;
}

base::FilePath CacheDirectoryFor(const PackageEntry& entry) {
  base::FilePath user_data;
  if (!base::PathService::Get(chrome::DIR_USER_DATA, &user_data)) {
    return {};
  }
  return user_data.Append(FILE_PATH_LITERAL("cmux-updates"))
      .Append(base::FilePath::FromUTF8Unsafe(entry.version))
      .Append(base::FilePath::FromUTF8Unsafe(PlatformKey()));
}

PackagePathPreparation PrepareDownloadPath(const base::FilePath& directory,
                                           const base::FilePath& archive,
                                           const base::FilePath& partial) {
  PackagePathPreparation result;
  result.success = !directory.empty() && base::CreateDirectory(directory);
  result.cached_archive = result.success && base::PathExists(archive);
  if (result.success) {
    base::DeleteFile(partial);
  }
  return result;
}

bool PromoteDownloadedPackage(const base::FilePath& partial,
                              const base::FilePath& archive) {
  base::DeleteFile(archive);
  return base::Move(partial, archive);
}

std::optional<ReadyUpdate> VerifyAndStagePackage(
    const PackageEntry& entry,
    const base::FilePath& archive,
    const base::FilePath& ready_dir) {
  base::File file(archive, base::File::FLAG_OPEN | base::File::FLAG_READ);
  if (!file.IsValid() || file.GetLength() != entry.size) {
    base::DeleteFile(archive);
    return std::nullopt;
  }
  std::unique_ptr<crypto::SecureHash> hash =
      crypto::SecureHash::Create(crypto::SecureHash::SHA256);
  std::array<uint8_t, 1024 * 1024> buffer;
  while (true) {
    std::optional<size_t> read = file.ReadAtCurrentPos(base::span(buffer));
    if (!read) {
      base::DeleteFile(archive);
      return std::nullopt;
    }
    if (*read == 0) {
      break;
    }
    hash->Update(base::span(buffer).first(*read));
  }
  std::array<uint8_t, 32> digest;
  hash->Finish(base::span(digest));
  if (base::ToLowerASCII(base::HexEncode(base::span(digest))) !=
      entry.sha256) {
    base::DeleteFile(archive);
    return std::nullopt;
  }

  base::DeletePathRecursively(ready_dir);
  const zip::UnzipSymlinkOption symlink_option =
#if BUILDFLAG(IS_POSIX)
      zip::UnzipSymlinkOption::PRESERVE;
#else
      zip::UnzipSymlinkOption::DONT_PRESERVE;
#endif
  if (!base::CreateDirectory(ready_dir) ||
      !zip::Unzip(archive, ready_dir, {}, symlink_option)) {
    base::DeletePathRecursively(ready_dir);
    return std::nullopt;
  }
  const base::FilePath root = ready_dir.Append(
      base::FilePath::FromUTF8Unsafe(entry.archive_root));
  const base::FilePath executable =
      root.Append(base::FilePath::FromUTF8Unsafe(entry.executable));
  if (!base::PathExists(root) || !base::PathExists(executable)) {
    base::DeletePathRecursively(ready_dir);
    return std::nullopt;
  }
  if (!CurrentInstallCanBeReplaced()) {
    base::DeletePathRecursively(ready_dir);
    return std::nullopt;
  }
  return ReadyUpdate{entry.version, root, entry.executable};
}

class CmuxUpdateServiceImpl {
 public:
  CmuxUpdateServiceImpl()
      : network_monitor_(CreateCmuxUpdateNetworkMonitor()) {}
  ~CmuxUpdateServiceImpl() = default;

  void Start() {
    network_monitor_->Start(base::BindRepeating(
        &CmuxUpdateServiceImpl::OnConnectionCost, weak_factory_.GetWeakPtr()));
    check_timer_.Start(FROM_HERE, kCheckInterval, this,
                       &CmuxUpdateServiceImpl::CheckForUpdates);
    CheckForUpdates();
  }

  void AddObserver(CmuxUpdateObserver* observer) {
    observers_.AddObserver(observer);
    observer->OnCmuxUpdateChanged(snapshot_);
  }

  void RemoveObserver(CmuxUpdateObserver* observer) {
    observers_.RemoveObserver(observer);
  }

  UpdateSnapshot snapshot() const { return snapshot_; }

  void ApplyReadyUpdate() {
    if (!ready_update_ || !ShouldShowUpdateNow(snapshot_.state)) {
      return;
    }
    SetState(UpdateState::kApplying, ready_update_->version);
    base::ThreadPool::PostTaskAndReplyWithResult(
        FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
        base::BindOnce(&LaunchReadyUpdateInstaller, *ready_update_),
        base::BindOnce(&CmuxUpdateServiceImpl::OnInstallerLaunched,
                       weak_factory_.GetWeakPtr()));
  }

 private:
  void SetState(UpdateState state, const std::string& version = {}) {
    snapshot_ = {state, version};
    for (CmuxUpdateObserver& observer : observers_) {
      observer.OnCmuxUpdateChanged(snapshot_);
    }
  }

  void CheckForUpdates() {
    if (manifest_loader_ || snapshot_.state == UpdateState::kApplying) {
      return;
    }
    const GURL url(ConfiguredFeedUrl());
    if (!url.is_valid() || !url.SchemeIsHTTPOrHTTPS() || !g_browser_process ||
        !g_browser_process->system_network_context_manager()) {
      SetState(UpdateState::kFailed);
      return;
    }
    SetState(UpdateState::kChecking);
    auto request = std::make_unique<network::ResourceRequest>();
    request->url = url;
    request->method = "GET";
    request->credentials_mode = network::mojom::CredentialsMode::kOmit;
    request->load_flags = net::LOAD_BYPASS_CACHE;
    manifest_loader_ = network::SimpleURLLoader::Create(
        std::move(request), kUpdateTrafficAnnotation);
    manifest_loader_->SetRetryOptions(
        2, network::SimpleURLLoader::RETRY_ON_NETWORK_CHANGE |
               network::SimpleURLLoader::RETRY_ON_5XX);
    auto factory = g_browser_process->system_network_context_manager()
                       ->GetSharedURLLoaderFactory();
    manifest_loader_->DownloadToString(
        factory.get(),
        base::BindOnce(&CmuxUpdateServiceImpl::OnManifest,
                       weak_factory_.GetWeakPtr()),
        kMaxManifestBytes);
  }

  void OnManifest(std::optional<std::string> body) {
    manifest_loader_.reset();
    if (!body) {
      SetState(UpdateState::kFailed);
      return;
    }
    std::optional<PackageEntry> entry = ParseManifest(*body);
    if (!entry) {
      pending_.reset();
      SetState(UpdateState::kIdle);
      return;
    }
    pending_ = std::move(*entry);
    BeginPackagePreparation();
  }

  void BeginPackagePreparation() {
    if (!pending_ || preparing_path_ || package_loader_ ||
        snapshot_.state == UpdateState::kVerifying ||
        snapshot_.state == UpdateState::kReady) {
      return;
    }
    package_directory_ = CacheDirectoryFor(*pending_);
    if (package_directory_.empty()) {
      SetState(UpdateState::kFailed, pending_->version);
      return;
    }
    archive_path_ = package_directory_.Append(FILE_PATH_LITERAL("package.zip"));
    partial_path_ = package_directory_.Append(FILE_PATH_LITERAL("package.part"));
    preparing_path_ = true;
    base::ThreadPool::PostTaskAndReplyWithResult(
        FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
        base::BindOnce(&PrepareDownloadPath, package_directory_, archive_path_,
                       partial_path_),
        base::BindOnce(&CmuxUpdateServiceImpl::OnPackagePathPrepared,
                       weak_factory_.GetWeakPtr()));
  }

  void OnPackagePathPrepared(PackagePathPreparation result) {
    preparing_path_ = false;
    if (!result.success || !pending_) {
      SetState(UpdateState::kFailed,
               pending_ ? pending_->version : std::string());
      return;
    }
    if (result.cached_archive) {
      VerifyPackage();
      return;
    }
    if (!MayDownloadUpdate(connection_cost_)) {
      SetState(UpdateState::kWaitingForUnmetered, pending_->version);
      return;
    }
    StartPackageDownload();
  }

  void StartPackageDownload() {
    if (!pending_ || !MayDownloadUpdate(connection_cost_) || package_loader_) {
      return;
    }
    SetState(UpdateState::kDownloading, pending_->version);
    auto request = std::make_unique<network::ResourceRequest>();
    request->url = pending_->url;
    request->method = "GET";
    request->credentials_mode = network::mojom::CredentialsMode::kOmit;
    package_loader_ = network::SimpleURLLoader::Create(
        std::move(request), kUpdateTrafficAnnotation);
    package_loader_->SetRetryOptions(
        2, network::SimpleURLLoader::RETRY_ON_NETWORK_CHANGE |
               network::SimpleURLLoader::RETRY_ON_5XX);
    auto factory = g_browser_process->system_network_context_manager()
                       ->GetSharedURLLoaderFactory();
    package_loader_->DownloadToFile(
        factory.get(),
        base::BindOnce(&CmuxUpdateServiceImpl::OnPackageDownloaded,
                       weak_factory_.GetWeakPtr()),
        partial_path_, pending_->size);
  }

  void OnPackageDownloaded(base::FilePath path) {
    package_loader_.reset();
    if (!pending_) {
      return;
    }
    if (path.empty()) {
      SetState(MayDownloadUpdate(connection_cost_)
                   ? UpdateState::kFailed
                   : UpdateState::kWaitingForUnmetered,
               pending_->version);
      return;
    }
    SetState(UpdateState::kVerifying, pending_->version);
    base::ThreadPool::PostTaskAndReplyWithResult(
        FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
        base::BindOnce(&PromoteDownloadedPackage, path, archive_path_),
        base::BindOnce(&CmuxUpdateServiceImpl::OnPackagePromoted,
                       weak_factory_.GetWeakPtr()));
  }

  void OnPackagePromoted(bool promoted) {
    if (!pending_) {
      return;
    }
    if (!promoted) {
      SetState(UpdateState::kFailed, pending_->version);
      return;
    }
    VerifyPackage();
  }

  void VerifyPackage() {
    if (!pending_) {
      return;
    }
    SetState(UpdateState::kVerifying, pending_->version);
    const base::FilePath ready_dir =
        package_directory_.Append(FILE_PATH_LITERAL("ready"));
    base::ThreadPool::PostTaskAndReplyWithResult(
        FROM_HERE,
        {base::MayBlock(), base::TaskPriority::USER_VISIBLE,
         base::TaskShutdownBehavior::BLOCK_SHUTDOWN},
        base::BindOnce(&VerifyAndStagePackage, *pending_, archive_path_,
                       ready_dir),
        base::BindOnce(&CmuxUpdateServiceImpl::OnPackageVerified,
                       weak_factory_.GetWeakPtr()));
  }

  void OnPackageVerified(std::optional<ReadyUpdate> ready) {
    if (!pending_) {
      return;
    }
    if (!ready) {
      SetState(UpdateState::kFailed, pending_->version);
      return;
    }
    ready_update_ = std::move(*ready);
    SetState(UpdateState::kReady, ready_update_->version);
  }

  void OnConnectionCost(UpdateConnectionCost cost) {
    connection_cost_ = cost;
    if (!MayDownloadUpdate(cost) && package_loader_) {
      package_loader_.reset();
      if (pending_) {
        SetState(UpdateState::kWaitingForUnmetered, pending_->version);
      }
      return;
    }
    if (MayDownloadUpdate(cost) && pending_ &&
        snapshot_.state == UpdateState::kWaitingForUnmetered) {
      BeginPackagePreparation();
    }
  }

  void OnInstallerLaunched(bool launched) {
    if (launched) {
      // The user explicitly chose the one-click restart after all bytes were
      // staged. Do not leave the helper waiting behind a beforeunload prompt.
      chrome::ExitIgnoreUnloadHandlers();
      return;
    }
    if (ready_update_) {
      SetState(UpdateState::kReady, ready_update_->version);
    } else {
      SetState(UpdateState::kFailed);
    }
  }

  UpdateSnapshot snapshot_;
  UpdateConnectionCost connection_cost_ = UpdateConnectionCost::kUnknown;
  base::ObserverList<CmuxUpdateObserver>::Unchecked observers_;
  std::unique_ptr<CmuxUpdateNetworkMonitor> network_monitor_;
  std::unique_ptr<network::SimpleURLLoader> manifest_loader_;
  std::unique_ptr<network::SimpleURLLoader> package_loader_;
  std::optional<PackageEntry> pending_;
  std::optional<ReadyUpdate> ready_update_;
  base::FilePath package_directory_;
  base::FilePath archive_path_;
  base::FilePath partial_path_;
  bool preparing_path_ = false;
  base::RepeatingTimer check_timer_;
  base::WeakPtrFactory<CmuxUpdateServiceImpl> weak_factory_{this};
};

std::unique_ptr<CmuxUpdateServiceImpl>& UpdateServiceStorage() {
  static base::NoDestructor<std::unique_ptr<CmuxUpdateServiceImpl>> service;
  return *service;
}

}  // namespace

void StartCmuxUpdateService() {
  std::unique_ptr<CmuxUpdateServiceImpl>& service = UpdateServiceStorage();
  if (service) {
    return;
  }
  service = std::make_unique<CmuxUpdateServiceImpl>();
  service->Start();
}

void StopCmuxUpdateService() {
  UpdateServiceStorage().reset();
}

void AddCmuxUpdateObserver(CmuxUpdateObserver* observer) {
  std::unique_ptr<CmuxUpdateServiceImpl>& service = UpdateServiceStorage();
  if (service && observer) {
    service->AddObserver(observer);
  }
}

void RemoveCmuxUpdateObserver(CmuxUpdateObserver* observer) {
  std::unique_ptr<CmuxUpdateServiceImpl>& service = UpdateServiceStorage();
  if (service && observer) {
    service->RemoveObserver(observer);
  }
}

UpdateSnapshot GetCmuxUpdateSnapshot() {
  const std::unique_ptr<CmuxUpdateServiceImpl>& service =
      UpdateServiceStorage();
  return service ? service->snapshot() : UpdateSnapshot();
}

void ApplyReadyCmuxUpdate() {
  std::unique_ptr<CmuxUpdateServiceImpl>& service = UpdateServiceStorage();
  if (service) {
    service->ApplyReadyUpdate();
  }
}

}  // namespace cmux
