// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "chrome/browser/cmux_term/cmux_telemetry.h"

#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "base/environment.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/important_file_writer.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/memory/weak_ptr.h"
#include "base/path_service.h"
#include "base/strings/stringprintf.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/task_traits.h"
#include "base/task/thread_pool.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "base/uuid.h"
#include "build/build_config.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/cmux_term/cmux_layout_config.h"
#include "chrome/browser/cmux_term/cmux_release_build.h"
#include "chrome/browser/cmux_term/cmux_telemetry_model.h"
#include "chrome/browser/cmux_term/cmux_views.h"
#include "chrome/browser/net/system_network_context_manager.h"
#include "chrome/common/chrome_paths.h"
#include "components/version_info/version_info.h"
#include "net/base/load_flags.h"
#include "net/http/http_response_headers.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "services/network/public/cpp/simple_url_loader.h"
#include "services/network/public/mojom/fetch_api.mojom.h"
#include "services/network/public/mojom/url_response_head.mojom.h"
#include "url/gurl.h"

namespace cmux {
namespace {

constexpr char kPostHogApiKey[] =
    "phc_opOVu7oFzR9wD3I6ZahFGOV2h3mqGpl5EHyQvmHciDP";
constexpr char kPostHogBatchUrl[] = "https://us.i.posthog.com/batch/";
constexpr char kTelemetryStateFilename[] = "cmux-telemetry.json";
constexpr size_t kMaxTelemetryStateBytes = 16 * 1024;
constexpr size_t kMaxPostHogResponseBytes = 64 * 1024;
constexpr base::TimeDelta kActiveCheckInterval = base::Minutes(30);
constexpr base::TimeDelta kPostHogRequestTimeout = base::Seconds(30);

const net::NetworkTrafficAnnotationTag kTelemetryTrafficAnnotation =
    net::DefineNetworkTrafficAnnotation("cmux_anonymous_telemetry", R"(
      semantics {
        sender: "cmux anonymous desktop telemetry"
        description:
          "Sends at most one daily and one hourly activity event while cmux "
          "is in use. Events contain a random install ID, UTC bucket, app "
          "version, and operating system. Browser history, URLs, terminal "
          "contents, file paths, account data, and user-entered text are never "
          "included."
        trigger:
          "cmux startup and a 30-minute check while a cmux window is active."
        data:
          "Random install ID, UTC day/hour, app version, operating system, "
          "and launch or active-timer reason."
        destination: WEBSITE
      }
      policy {
        cookies_allowed: NO
        setting:
          "Set app.sendAnonymousTelemetry to false in cmux.json or set "
          "CMUX_TELEMETRY_ENABLE=0."
        policy_exception_justification:
          "cmux does not yet expose enterprise policy for its early desktop "
          "distribution."
      })");

struct TelemetryState {
  std::string distinct_id;
  std::string last_day_utc;
  std::string last_hour_utc;
};

struct UtcBuckets {
  std::string day;
  std::string hour;
};

std::string EnvironmentValue(base::cstring_view name) {
  return base::Environment::Create()->GetVar(name).value_or(std::string());
}

bool TelemetryEnabledForBuild(bool configured) {
  if (!configured || EnvironmentValue("CMUX_TELEMETRY_ENABLE") == "0") {
    return false;
  }
  // Only the stable/nightly release pipeline stamps this compile-time bit.
  // Every developer and dogfood build requires a deliberate runtime opt-in.
  return kCmuxOfficialReleaseBuild ||
         EnvironmentValue("CMUX_TELEMETRY_ENABLE") == "1";
}

base::FilePath TelemetryStatePath() {
  base::FilePath user_data;
  if (!base::PathService::Get(chrome::DIR_USER_DATA, &user_data)) {
    return {};
  }
  return user_data.AppendASCII(kTelemetryStateFilename);
}

TelemetryState ReadTelemetryState(const base::FilePath& path) {
  TelemetryState state;
  std::string json;
  if (base::ReadFileToStringWithMaxSize(path, &json,
                                        kMaxTelemetryStateBytes)) {
    std::optional<base::DictValue> parsed = base::JSONReader::ReadDict(
        json, base::JSON_PARSE_RFC);
    if (parsed) {
      if (const std::string* value = parsed->FindString("distinct_id");
          value && value->size() <= 64) {
        state.distinct_id = *value;
      }
      if (const std::string* value = parsed->FindString("last_day_utc");
          value && value->size() <= 16) {
        state.last_day_utc = *value;
      }
      if (const std::string* value = parsed->FindString("last_hour_utc");
          value && value->size() <= 20) {
        state.last_hour_utc = *value;
      }
    }
  }
  if (state.distinct_id.empty()) {
    state.distinct_id = base::Uuid::GenerateRandomV4().AsLowercaseString();
  }
  return state;
}

bool WriteTelemetryState(const base::FilePath& path, TelemetryState state) {
  base::DictValue value;
  value.Set("schema", 1);
  value.Set("distinct_id", std::move(state.distinct_id));
  value.Set("last_day_utc", std::move(state.last_day_utc));
  value.Set("last_hour_utc", std::move(state.last_hour_utc));
  std::string json;
  if (!base::JSONWriter::WriteWithOptions(
          value, base::JSONWriter::OPTIONS_PRETTY_PRINT, &json) ||
      !base::CreateDirectory(path.DirName()) ||
      !base::ImportantFileWriter::WriteFileAtomically(path, json)) {
    VLOG(1) << "cmux-telemetry: could not persist anonymous activity state";
    return false;
  }
  return true;
}

UtcBuckets CurrentUtcBuckets() {
  base::Time::Exploded exploded;
  base::Time::Now().UTCExplode(&exploded);
  UtcBuckets buckets;
  buckets.day =
      base::StringPrintf("%04d-%02d-%02d", exploded.year, exploded.month,
                         exploded.day_of_month);
  buckets.hour = base::StringPrintf(
      "%04d-%02d-%02dT%02d", exploded.year, exploded.month,
      exploded.day_of_month, exploded.hour);
  return buckets;
}

const char* OperatingSystem() {
#if BUILDFLAG(IS_WIN)
  return "windows";
#elif BUILDFLAG(IS_LINUX)
  return "linux";
#elif BUILDFLAG(IS_MAC)
  return "macos";
#else
  return "unknown";
#endif
}

base::DictValue ActiveEvent(const char* name,
                            const char* bucket_key,
                            const std::string& bucket,
                            const std::string& reason,
                            const TelemetryState& state) {
  base::DictValue properties;
  properties.Set("distinct_id", state.distinct_id);
  properties.Set("$insert_id",
                 ActiveEventInsertId(state.distinct_id, name, bucket));
  properties.Set("$lib", "cmux-browser");
  properties.Set("$lib_version", version_info::GetVersionNumber());
  properties.Set("$process_person_profile", false);
  properties.Set("platform", "cmux-browser");
  properties.Set("operating_system", OperatingSystem());
  properties.Set("app_version", version_info::GetVersionNumber());
  properties.Set("reason", reason);
  properties.Set(bucket_key, bucket);

  base::DictValue event;
  event.Set("uuid", ActiveEventUuid(state.distinct_id, name, bucket));
  event.Set("event", name);
  event.Set("properties", std::move(properties));
  return event;
}

std::string BuildPostHogPayload(const ActiveBucketDecision& due,
                                const UtcBuckets& buckets,
                                const std::string& reason,
                                const TelemetryState& state) {
  base::ListValue batch;
  if (due.capture_daily) {
    batch.Append(ActiveEvent("cmux_daily_active", "day_utc", buckets.day,
                             reason, state));
  }
  if (due.capture_hourly) {
    batch.Append(ActiveEvent("cmux_hourly_active", "hour_utc", buckets.hour,
                             reason, state));
  }
  base::DictValue payload;
  payload.Set("api_key", kPostHogApiKey);
  payload.Set("batch", std::move(batch));
  std::string json;
  base::JSONWriter::Write(payload, &json);
  return json;
}

class CmuxTelemetryService {
 public:
  CmuxTelemetryService()
      : state_task_runner_(base::ThreadPool::CreateSequencedTaskRunner(
            {base::MayBlock(), base::TaskPriority::BEST_EFFORT,
             base::TaskShutdownBehavior::SKIP_ON_SHUTDOWN})),
        state_path_(TelemetryStatePath()) {}
  ~CmuxTelemetryService() = default;

  void Start() {
    LoadBrowserConfigWithStatusAsync(base::BindOnce(
        &CmuxTelemetryService::OnBrowserConfig, weak_factory_.GetWeakPtr()));
  }

  void Stop() {
    active_timer_.Stop();
    loader_.reset();
  }

 private:
  void OnBrowserConfig(BrowserConfigLoadResult result) {
    const bool configured =
        result.status == BrowserConfigLoadStatus::kAbsent ||
        (result.status == BrowserConfigLoadStatus::kLoaded && result.config &&
         result.config->send_anonymous_telemetry);
    if (result.status == BrowserConfigLoadStatus::kError) {
      VLOG(1) << "cmux-telemetry: config read failed; telemetry disabled";
    }
    enabled_ = TelemetryEnabledForBuild(configured);
    if (!enabled_ || state_path_.empty()) {
      return;
    }
    state_task_runner_->PostTaskAndReplyWithResult(
        FROM_HERE, base::BindOnce(&ReadTelemetryState, state_path_),
        base::BindOnce(&CmuxTelemetryService::OnStateLoaded,
                       weak_factory_.GetWeakPtr()));
  }

  void OnStateLoaded(TelemetryState state) {
    state_ = std::move(state);
    // The random identity must be durable before it appears in a request.
    // Otherwise an unwritable profile would generate a new distinct_id and
    // new insert IDs on every launch, defeating PostHog deduplication.
    PersistState(base::BindOnce(
        &CmuxTelemetryService::OnInitialStatePersisted,
        weak_factory_.GetWeakPtr()));
  }

  void OnInitialStatePersisted(bool persisted) {
    if (!persisted) {
      return;
    }
    state_loaded_ = true;
    CaptureIfDue("launch");
    active_timer_.Start(FROM_HERE, kActiveCheckInterval, this,
                        &CmuxTelemetryService::CaptureIfActive);
  }

  void CaptureIfActive() {
    if (IsAnyCmuxWindowActive()) {
      CaptureIfDue("activeTimer");
    }
  }

  void CaptureIfDue(const std::string& reason) {
    if (!enabled_ || !state_loaded_ || loader_) {
      return;
    }
    pending_buckets_ = CurrentUtcBuckets();
    pending_due_ =
        DueActiveBuckets(state_.last_day_utc, state_.last_hour_utc,
                         pending_buckets_.day, pending_buckets_.hour);
    if (!pending_due_.capture_daily && !pending_due_.capture_hourly) {
      return;
    }
    if (!g_browser_process ||
        !g_browser_process->system_network_context_manager()) {
      return;
    }

    auto request = std::make_unique<network::ResourceRequest>();
    request->url = GURL(kPostHogBatchUrl);
    request->method = "POST";
    request->credentials_mode = network::mojom::CredentialsMode::kOmit;
    request->load_flags = net::LOAD_BYPASS_CACHE;
    loader_ = network::SimpleURLLoader::Create(
        std::move(request), kTelemetryTrafficAnnotation);
    loader_->AttachStringForUpload(
        BuildPostHogPayload(pending_due_, pending_buckets_, reason, state_),
        "application/json");
    loader_->SetRetryOptions(
        2, network::SimpleURLLoader::RETRY_ON_NETWORK_CHANGE |
               network::SimpleURLLoader::RETRY_ON_5XX);
    loader_->SetTimeoutDuration(kPostHogRequestTimeout);
    auto factory = g_browser_process->system_network_context_manager()
                       ->GetSharedURLLoaderFactory();
    loader_->DownloadToString(
        factory.get(),
        base::BindOnce(&CmuxTelemetryService::OnPostHogResponse,
                       weak_factory_.GetWeakPtr()),
        kMaxPostHogResponseBytes);
  }

  void OnPostHogResponse(std::optional<std::string> response) {
    int response_code = 0;
    if (loader_ && loader_->ResponseInfo() &&
        loader_->ResponseInfo()->headers) {
      response_code = loader_->ResponseInfo()->headers->response_code();
    }
    loader_.reset();
    if (!response || response_code < 200 || response_code >= 300) {
      VLOG(1) << "cmux-telemetry: PostHog capture failed (HTTP "
              << response_code << ")";
      return;
    }
    if (pending_due_.capture_daily) {
      state_.last_day_utc = pending_buckets_.day;
    }
    if (pending_due_.capture_hourly) {
      state_.last_hour_utc = pending_buckets_.hour;
    }
    PersistState(base::DoNothing());
  }

  void PersistState(base::OnceCallback<void(bool)> callback) {
    state_task_runner_->PostTaskAndReplyWithResult(
        FROM_HERE,
        base::BindOnce(&WriteTelemetryState, state_path_, state_),
        std::move(callback));
  }

  bool enabled_ = false;
  bool state_loaded_ = false;
  scoped_refptr<base::SequencedTaskRunner> state_task_runner_;
  base::FilePath state_path_;
  TelemetryState state_;
  UtcBuckets pending_buckets_;
  ActiveBucketDecision pending_due_;
  base::RepeatingTimer active_timer_;
  std::unique_ptr<network::SimpleURLLoader> loader_;
  base::WeakPtrFactory<CmuxTelemetryService> weak_factory_{this};
};

CmuxTelemetryService* g_service = nullptr;

}  // namespace

void StartCmuxTelemetry() {
  if (g_service) {
    return;
  }
  g_service = new CmuxTelemetryService();
  g_service->Start();
}

void StopCmuxTelemetry() {
  if (!g_service) {
    return;
  }
  g_service->Stop();
  delete g_service;
  g_service = nullptr;
}

}  // namespace cmux
