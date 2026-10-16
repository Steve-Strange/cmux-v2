// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "chrome/browser/cmux_term/cmux_tui_client.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <string_view>
#include <utility>

#include "build/build_config.h"

#if BUILDFLAG(IS_WIN)
#include <winsock2.h>

// afunix.h uses types declared by winsock2.h and must follow it.
#include <afunix.h>
#else
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#include "base/base64.h"
#include "base/base_paths.h"
#include "base/check.h"
#include "base/command_line.h"
#include "base/containers/span.h"
#include "base/environment.h"
#include "base/files/file_util.h"
#include "base/files/scoped_file.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/memory/ref_counted.h"
#include "base/path_service.h"
#include "base/process/launch.h"
#include "base/process/process.h"
#include "base/process/process_handle.h"
#include "base/strings/cstring_view.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/strings/string_view_util.h"
#include "base/strings/stringprintf.h"
#include "base/synchronization/lock.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/single_thread_task_runner.h"
#include "base/task/task_traits.h"
#include "base/task/thread_pool.h"
#include "base/threading/platform_thread.h"
#include "base/time/time.h"
#include "chrome/browser/cmux_term/cmux_terminal_host_connection.h"
#include "chrome/browser/cmux_term/cmux_tui_protocol.h"
#include "chrome/browser/cmux_term/cmux_tui_revision.h"

#if BUILDFLAG(IS_WIN)
#include "net/base/winsock_init.h"
#endif

namespace cmux {

CmuxTuiWorkspace::CmuxTuiWorkspace() = default;
CmuxTuiWorkspace::CmuxTuiWorkspace(const CmuxTuiWorkspace&) = default;
CmuxTuiWorkspace& CmuxTuiWorkspace::operator=(const CmuxTuiWorkspace&) =
    default;
CmuxTuiWorkspace::CmuxTuiWorkspace(CmuxTuiWorkspace&&) = default;
CmuxTuiWorkspace& CmuxTuiWorkspace::operator=(CmuxTuiWorkspace&&) = default;
CmuxTuiWorkspace::~CmuxTuiWorkspace() = default;

CmuxTuiWorkspaceSnapshot::CmuxTuiWorkspaceSnapshot() = default;
CmuxTuiWorkspaceSnapshot::CmuxTuiWorkspaceSnapshot(
    const CmuxTuiWorkspaceSnapshot&) = default;
CmuxTuiWorkspaceSnapshot& CmuxTuiWorkspaceSnapshot::operator=(
    const CmuxTuiWorkspaceSnapshot&) = default;
CmuxTuiWorkspaceSnapshot::CmuxTuiWorkspaceSnapshot(CmuxTuiWorkspaceSnapshot&&) =
    default;
CmuxTuiWorkspaceSnapshot& CmuxTuiWorkspaceSnapshot::operator=(
    CmuxTuiWorkspaceSnapshot&&) = default;
CmuxTuiWorkspaceSnapshot::~CmuxTuiWorkspaceSnapshot() = default;

CmuxTuiWorkspaceMutation::CmuxTuiWorkspaceMutation() = default;
CmuxTuiWorkspaceMutation::CmuxTuiWorkspaceMutation(
    const CmuxTuiWorkspaceMutation&) = default;
CmuxTuiWorkspaceMutation& CmuxTuiWorkspaceMutation::operator=(
    const CmuxTuiWorkspaceMutation&) = default;
CmuxTuiWorkspaceMutation::CmuxTuiWorkspaceMutation(
    CmuxTuiWorkspaceMutation&&) = default;
CmuxTuiWorkspaceMutation& CmuxTuiWorkspaceMutation::operator=(
    CmuxTuiWorkspaceMutation&&) = default;
CmuxTuiWorkspaceMutation::~CmuxTuiWorkspaceMutation() = default;

CmuxTuiTerminalMutation::CmuxTuiTerminalMutation() = default;
CmuxTuiTerminalMutation::CmuxTuiTerminalMutation(
    const CmuxTuiTerminalMutation&) = default;
CmuxTuiTerminalMutation& CmuxTuiTerminalMutation::operator=(
    const CmuxTuiTerminalMutation&) = default;
CmuxTuiTerminalMutation::CmuxTuiTerminalMutation(CmuxTuiTerminalMutation&&) =
    default;
CmuxTuiTerminalMutation& CmuxTuiTerminalMutation::operator=(
    CmuxTuiTerminalMutation&&) = default;
CmuxTuiTerminalMutation::~CmuxTuiTerminalMutation() = default;

CmuxTuiTerminalRegistryEvent::CmuxTuiTerminalRegistryEvent() = default;
CmuxTuiTerminalRegistryEvent::CmuxTuiTerminalRegistryEvent(
    const CmuxTuiTerminalRegistryEvent&) = default;
CmuxTuiTerminalRegistryEvent& CmuxTuiTerminalRegistryEvent::operator=(
    const CmuxTuiTerminalRegistryEvent&) = default;
CmuxTuiTerminalRegistryEvent::CmuxTuiTerminalRegistryEvent(
    CmuxTuiTerminalRegistryEvent&&) = default;
CmuxTuiTerminalRegistryEvent& CmuxTuiTerminalRegistryEvent::operator=(
    CmuxTuiTerminalRegistryEvent&&) = default;
CmuxTuiTerminalRegistryEvent::~CmuxTuiTerminalRegistryEvent() = default;

CmuxTuiTerminalResolution::CmuxTuiTerminalResolution() = default;
CmuxTuiTerminalResolution::CmuxTuiTerminalResolution(
    const CmuxTuiTerminalResolution&) = default;
CmuxTuiTerminalResolution& CmuxTuiTerminalResolution::operator=(
    const CmuxTuiTerminalResolution&) = default;
CmuxTuiTerminalResolution::CmuxTuiTerminalResolution(
    CmuxTuiTerminalResolution&&) = default;
CmuxTuiTerminalResolution& CmuxTuiTerminalResolution::operator=(
    CmuxTuiTerminalResolution&&) = default;
CmuxTuiTerminalResolution::~CmuxTuiTerminalResolution() = default;

CmuxTuiFrontendProjection::CmuxTuiFrontendProjection() = default;
CmuxTuiFrontendProjection::CmuxTuiFrontendProjection(
    CmuxTuiFrontendProjection&&) = default;
CmuxTuiFrontendProjection& CmuxTuiFrontendProjection::operator=(
    CmuxTuiFrontendProjection&&) = default;
CmuxTuiFrontendProjection::~CmuxTuiFrontendProjection() = default;

CmuxTuiColors::CmuxTuiColors() = default;
CmuxTuiColors::CmuxTuiColors(const CmuxTuiColors&) = default;
CmuxTuiColors& CmuxTuiColors::operator=(const CmuxTuiColors&) = default;
CmuxTuiColors::CmuxTuiColors(CmuxTuiColors&&) = default;
CmuxTuiColors& CmuxTuiColors::operator=(CmuxTuiColors&&) = default;
CmuxTuiColors::~CmuxTuiColors() = default;

CmuxTuiEvent::CmuxTuiEvent() = default;
CmuxTuiEvent::CmuxTuiEvent(const CmuxTuiEvent&) = default;
CmuxTuiEvent& CmuxTuiEvent::operator=(const CmuxTuiEvent&) = default;
CmuxTuiEvent::CmuxTuiEvent(CmuxTuiEvent&&) = default;
CmuxTuiEvent& CmuxTuiEvent::operator=(CmuxTuiEvent&&) = default;
CmuxTuiEvent::~CmuxTuiEvent() = default;

CmuxTuiCommandResult::CmuxTuiCommandResult() = default;
CmuxTuiCommandResult::CmuxTuiCommandResult(CmuxTuiCommandResult&&) = default;
CmuxTuiCommandResult& CmuxTuiCommandResult::operator=(
    CmuxTuiCommandResult&&) = default;
CmuxTuiCommandResult::~CmuxTuiCommandResult() = default;

CmuxTuiRendererConnection::CmuxTuiRendererConnection(
    mojo::PlatformHandle socket,
    TerminalHostId terminal_id,
    TerminalHostIncarnation incarnation,
    TerminalHostCapabilityRights rights,
    uint32_t protocol_flags,
    uint32_t ttl_ms)
    : socket(std::move(socket)),
      terminal_id(terminal_id),
      incarnation(incarnation),
      rights(rights),
      protocol_flags(protocol_flags),
      ttl_ms(ttl_ms) {}
CmuxTuiRendererConnection::CmuxTuiRendererConnection(
    CmuxTuiRendererConnection&&) = default;
CmuxTuiRendererConnection& CmuxTuiRendererConnection::operator=(
    CmuxTuiRendererConnection&&) = default;
CmuxTuiRendererConnection::~CmuxTuiRendererConnection() = default;

CmuxTuiClient::Options::Options() = default;
CmuxTuiClient::Options::Options(const Options&) = default;
CmuxTuiClient::Options& CmuxTuiClient::Options::operator=(const Options&) =
    default;
CmuxTuiClient::Options::Options(Options&&) = default;
CmuxTuiClient::Options& CmuxTuiClient::Options::operator=(Options&&) = default;
CmuxTuiClient::Options::~Options() = default;

namespace {

#if BUILDFLAG(IS_WIN)
using SocketHandle = SOCKET;
constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
#else
using SocketHandle = int;
constexpr SocketHandle kInvalidSocket = -1;
#endif

constexpr int kMaxInitialConnectAttempts = 50;
constexpr double kMaxExactJsonInteger = 9007199254740991.0;
constexpr base::TimeDelta kInitialReconnectDelay = base::Milliseconds(100);
constexpr base::TimeDelta kLiveReconnectDelay = base::Milliseconds(500);

double ProtocolSurfaceId(CmuxTuiSurfaceId surface) {
  CHECK_LE(surface, static_cast<uint64_t>(kMaxExactJsonInteger));
  return static_cast<double>(surface);
}

int LastSocketError() {
#if BUILDFLAG(IS_WIN)
  return WSAGetLastError();
#else
  return errno;
#endif
}

void CloseSocket(SocketHandle socket) {
  if (socket == kInvalidSocket) {
    return;
  }
#if BUILDFLAG(IS_WIN)
  closesocket(socket);
#else
  close(socket);
#endif
}

void ShutdownSocket(SocketHandle socket) {
  if (socket == kInvalidSocket) {
    return;
  }
#if BUILDFLAG(IS_WIN)
  shutdown(socket, SD_BOTH);
#else
  shutdown(socket, SHUT_RDWR);
#endif
}

std::string SocketError(std::string_view operation, int error) {
  return base::StringPrintf("%.*s failed (socket error %d)",
                            static_cast<int>(operation.size()),
                            operation.data(), error);
}

std::optional<std::string> EnvironmentValue(base::Environment* environment,
                                            base::cstring_view name) {
  if (!environment) {
    return std::nullopt;
  }
  std::optional<std::string> value = environment->GetVar(name);
  if (!value || value->empty()) {
    return std::nullopt;
  }
  return value;
}

base::FilePath DefaultRuntimeBase(base::Environment* environment) {
#if BUILDFLAG(IS_WIN)
  if (auto value = EnvironmentValue(environment, "TEMP")) {
    return base::FilePath::FromUTF8Unsafe(*value);
  }
  if (auto value = EnvironmentValue(environment, "TMP")) {
    return base::FilePath::FromUTF8Unsafe(*value);
  }
#else
  if (auto value = EnvironmentValue(environment, "XDG_RUNTIME_DIR")) {
    return base::FilePath::FromUTF8Unsafe(*value);
  }
  if (auto value = EnvironmentValue(environment, "TMPDIR")) {
    return base::FilePath::FromUTF8Unsafe(*value);
  }
#endif
#if BUILDFLAG(IS_WIN)
  base::FilePath temporary;
  if (base::GetTempDir(&temporary)) {
    return temporary;
  }
#endif
  return base::FilePath(FILE_PATH_LITERAL("/tmp"));
}

std::string UserIdComponent(base::Environment* environment) {
#if BUILDFLAG(IS_WIN)
  return EnvironmentValue(environment, "USERNAME").value_or("user");
#else
  return base::NumberToString(getuid());
#endif
}

base::FilePath DefaultSocketPath(const std::string& session,
                                 base::Environment* environment) {
  const std::string runtime_dir = "cmux-tui-" + UserIdComponent(environment);
  const base::FilePath relative =
      base::FilePath::FromUTF8Unsafe(runtime_dir)
          .Append(base::FilePath::FromUTF8Unsafe(session + ".sock"));
  const base::FilePath preferred =
      DefaultRuntimeBase(environment).Append(relative);
#if BUILDFLAG(IS_WIN)
  return preferred;
#else
  const base::FilePath fallback =
      base::FilePath(FILE_PATH_LITERAL("/tmp")).Append(relative);
  sockaddr_un address = {};
  return base::FilePath::FromUTF8Unsafe(SelectCmuxTuiSocketPath(
      preferred.value(), fallback.value(), sizeof(address.sun_path)));
#endif
}

std::optional<uint64_t> ReadUnsigned(const base::DictValue& dict,
                                     std::string_view key) {
  const base::Value* value = dict.Find(key);
  if (!value) {
    return std::nullopt;
  }
  if (value->is_int()) {
    const int integer = value->GetInt();
    return integer >= 0 ? std::optional<uint64_t>(integer) : std::nullopt;
  }
  if (!value->is_double()) {
    return std::nullopt;
  }
  const double number = value->GetDouble();
  if (!std::isfinite(number) || number < 0 || std::floor(number) != number ||
      number > kMaxExactJsonInteger) {
    return std::nullopt;
  }
  return static_cast<uint64_t>(number);
}

std::optional<uint16_t> ReadUint16(const base::DictValue& dict,
                                   std::string_view key) {
  const std::optional<uint64_t> value = ReadUnsigned(dict, key);
  if (!value || *value > std::numeric_limits<uint16_t>::max()) {
    return std::nullopt;
  }
  return static_cast<uint16_t>(*value);
}

std::optional<CmuxTerminalBinding> ParseTerminalBinding(
    const base::DictValue& data) {
  const std::optional<uint64_t> surface = ReadUnsigned(data, "surface");
  const std::string* terminal_id_text = data.FindString("terminal_id");
  const std::string* incarnation_text = data.FindString("terminal_incarnation");
  CmuxTerminalBinding binding;
  if (!surface || *surface == 0 || !terminal_id_text || !incarnation_text ||
      !DecodeTerminalHostUuidV4(*terminal_id_text, &binding.terminal_id) ||
      !DecodeTerminalHostUuidV4(*incarnation_text,
                                &binding.terminal_incarnation)) {
    return std::nullopt;
  }
  binding.surface = *surface;
  return binding;
}

std::optional<std::string> OptionalString(const base::DictValue& dict,
                                          std::string_view key) {
  const std::string* value = dict.FindString(key);
  return value ? std::optional<std::string>(*value) : std::nullopt;
}

void ParseActiveWorkspaceSelection(const base::DictValue& workspace,
                                   CmuxTuiWorkspaceSnapshot* snapshot) {
  if (!snapshot || !workspace.FindBool("active").value_or(false) ||
      !snapshot->active_workspace_key.empty()) {
    return;
  }
  const std::string* workspace_key = workspace.FindString("key");
  if (!workspace_key || workspace_key->empty()) {
    return;
  }
  snapshot->active_workspace_key = *workspace_key;

  const base::ListValue* screens = workspace.FindList("screens");
  if (!screens) {
    return;
  }
  for (const base::Value& screen_value : *screens) {
    if (!screen_value.is_dict()) {
      continue;
    }
    const base::DictValue& screen = screen_value.GetDict();
    if (!screen.FindBool("active").value_or(false)) {
      continue;
    }
    const std::optional<uint64_t> active_pane =
        ReadUnsigned(screen, "active_pane");
    const base::ListValue* panes = screen.FindList("panes");
    if (!active_pane || !panes) {
      return;
    }
    for (const base::Value& pane_value : *panes) {
      if (!pane_value.is_dict()) {
        continue;
      }
      const base::DictValue& pane = pane_value.GetDict();
      if (ReadUnsigned(pane, "id") != active_pane) {
        continue;
      }
      const std::optional<uint64_t> active_tab =
          ReadUnsigned(pane, "active_tab");
      const base::ListValue* tabs = pane.FindList("tabs");
      if (!active_tab || !tabs || *active_tab >= tabs->size()) {
        return;
      }
      const base::Value& tab = (*tabs)[static_cast<size_t>(*active_tab)];
      if (!tab.is_dict()) {
        return;
      }
      const std::string* terminal_id =
          tab.GetDict().FindString("terminal_id");
      if (terminal_id && !terminal_id->empty()) {
        snapshot->active_terminal_id = *terminal_id;
      }
      return;
    }
    return;
  }
}

std::optional<CmuxTuiWorkspaceSnapshot> ParseWorkspaceSnapshot(
    const base::DictValue& data) {
  const std::optional<uint64_t> revision =
      ReadUnsigned(data, "workspace_revision");
  const base::ListValue* workspaces = data.FindList("workspaces");
  const std::string* registry_id = data.FindString("registry_id");
  const std::string* generation = data.FindString("generation");
  if (!revision || !workspaces || !registry_id || registry_id->empty() ||
      !generation || generation->empty()) {
    return std::nullopt;
  }
  CmuxTuiWorkspaceSnapshot snapshot;
  snapshot.revision = *revision;
  snapshot.registry_id = *registry_id;
  snapshot.generation = *generation;
  snapshot.workspaces.reserve(workspaces->size());
  for (const base::Value& value : *workspaces) {
    if (!value.is_dict()) {
      return std::nullopt;
    }
    const base::DictValue& workspace = value.GetDict();
    const std::optional<uint64_t> id = ReadUnsigned(workspace, "id");
    const std::string* key = workspace.FindString("key");
    const std::string* name = workspace.FindString("name");
    if (!id || *id == 0 || !key || !IsCmuxUuid(*key) || !name) {
      return std::nullopt;
    }
    CmuxTuiWorkspace parsed;
    parsed.id = *id;
    parsed.key = *key;
    parsed.name = *name;
    ParseActiveWorkspaceSelection(workspace, &snapshot);
    snapshot.workspaces.push_back(std::move(parsed));
  }
  return snapshot;
}

std::optional<CmuxTerminalLifecycle> ParseTerminalLifecycle(
    std::string_view lifecycle) {
  if (lifecycle == "launching") {
    return CmuxTerminalLifecycle::kLaunching;
  }
  if (lifecycle == "adopting") {
    return CmuxTerminalLifecycle::kAdopting;
  }
  if (lifecycle == "running") {
    return CmuxTerminalLifecycle::kRunning;
  }
  if (lifecycle == "exited") {
    return CmuxTerminalLifecycle::kExited;
  }
  if (lifecycle == "tombstoned") {
    return CmuxTerminalLifecycle::kTombstoned;
  }
  return std::nullopt;
}

bool IsStableTerminalIdentity(std::string_view value) {
  TerminalHostId decoded;
  return DecodeTerminalHostUuidV4(value, &decoded);
}

bool IsValidCanonicalTerminal(const CmuxCanonicalTerminalPlacement& terminal,
                              bool allow_tombstone) {
  if (!IsStableTerminalIdentity(terminal.terminal_id) ||
      terminal.workspace_key.empty()) {
    return false;
  }
  switch (terminal.lifecycle) {
    case CmuxTerminalLifecycle::kLaunching:
      return terminal.incarnation.empty();
    case CmuxTerminalLifecycle::kAdopting:
    case CmuxTerminalLifecycle::kRunning:
      return IsStableTerminalIdentity(terminal.incarnation);
    case CmuxTerminalLifecycle::kExited:
      return terminal.incarnation.empty() ||
             IsStableTerminalIdentity(terminal.incarnation);
    case CmuxTerminalLifecycle::kTombstoned:
      return allow_tombstone &&
             (terminal.incarnation.empty() ||
              IsStableTerminalIdentity(terminal.incarnation));
  }
  return false;
}

std::optional<CmuxCanonicalTerminalPlacement> ParseCanonicalTerminal(
    const base::DictValue& data,
    std::string_view incarnation_key,
    bool allow_tombstone) {
  const std::string* terminal_id = data.FindString("terminal_id");
  const std::string* workspace_key = data.FindString("workspace_key");
  const std::string* lifecycle_text = data.FindString("lifecycle");
  if (!lifecycle_text) {
    lifecycle_text = data.FindString("state");
  }
  const std::optional<CmuxTerminalLifecycle> lifecycle =
      lifecycle_text ? ParseTerminalLifecycle(*lifecycle_text) : std::nullopt;
  if (!terminal_id || !workspace_key || !lifecycle) {
    return std::nullopt;
  }
  CmuxCanonicalTerminalPlacement terminal;
  terminal.terminal_id = *terminal_id;
  terminal.workspace_key = *workspace_key;
  terminal.lifecycle = *lifecycle;
  if (const std::string* incarnation = data.FindString(incarnation_key)) {
    terminal.incarnation = *incarnation;
  } else if (const base::Value* incarnation_value = data.Find(incarnation_key);
             incarnation_value && !incarnation_value->is_none()) {
    return std::nullopt;
  }
  if (!IsValidCanonicalTerminal(terminal, allow_tombstone)) {
    return std::nullopt;
  }
  return terminal;
}

std::optional<CmuxTerminalPlacementSnapshot> ParseTerminalSnapshot(
    const base::DictValue& data) {
  const std::optional<uint64_t> revision =
      ReadUnsigned(data, "terminal_revision");
  const std::string* registry_id = data.FindString("registry_id");
  const std::string* generation = data.FindString("generation");
  const base::ListValue* terminals = data.FindList("terminals");
  if (!revision || !registry_id || registry_id->empty() || !generation ||
      generation->empty() || !terminals) {
    return std::nullopt;
  }

  std::vector<CmuxCanonicalTerminalPlacement> parsed_terminals;
  parsed_terminals.reserve(terminals->size());
  std::set<std::string> seen;
  for (const base::Value& value : *terminals) {
    if (!value.is_dict()) {
      return std::nullopt;
    }
    std::optional<CmuxCanonicalTerminalPlacement> terminal =
        ParseCanonicalTerminal(value.GetDict(), "terminal_incarnation",
                               /*allow_tombstone=*/false);
    if (!terminal || !seen.insert(terminal->terminal_id).second) {
      return std::nullopt;
    }
    parsed_terminals.push_back(std::move(*terminal));
  }
  return CmuxTerminalPlacementSnapshot(*registry_id, *generation, *revision,
                                       std::move(parsed_terminals));
}

struct ParsedTerminalEventBatch {
  std::string registry_id;
  std::string generation;
  uint64_t revision = 0;
  std::vector<CmuxTuiTerminalRegistryEvent> events;
};

std::optional<ParsedTerminalEventBatch> ParseTerminalEventBatch(
    const base::DictValue& data,
    uint64_t after_revision) {
  const std::optional<uint64_t> revision =
      ReadUnsigned(data, "terminal_revision");
  const std::string* registry_id = data.FindString("registry_id");
  const std::string* generation = data.FindString("generation");
  const base::ListValue* events = data.FindList("events");
  if (!revision || !registry_id || registry_id->empty() || !generation ||
      generation->empty() || !events) {
    return std::nullopt;
  }

  ParsedTerminalEventBatch batch;
  batch.registry_id = *registry_id;
  batch.generation = *generation;
  batch.revision = *revision;
  batch.events.reserve(events->size());
  std::vector<uint64_t> event_revisions;
  event_revisions.reserve(events->size());
  for (const base::Value& value : *events) {
    if (!value.is_dict()) {
      return std::nullopt;
    }
    const base::DictValue& row = value.GetDict();
    const std::optional<uint64_t> event_revision =
        ReadUnsigned(row, "terminal_revision");
    const std::string* kind = row.FindString("kind");
    const std::string* terminal_id = row.FindString("terminal_id");
    const std::string* workspace_key = row.FindString("workspace_key");
    const std::string* origin = row.FindString("origin");
    const std::string* mutation_id = row.FindString("mutation_id");
    const base::DictValue* result = row.FindDict("result");
    if (!event_revision || !kind || kind->empty() || !terminal_id ||
        !workspace_key || workspace_key->empty() || !origin ||
        origin->empty() || !mutation_id || mutation_id->empty() || !result) {
      return std::nullopt;
    }
    if (const std::string* result_terminal_id =
            result->FindString("terminal_id");
        result_terminal_id && *result_terminal_id != *terminal_id) {
      return std::nullopt;
    }
    if (const std::string* result_workspace =
            result->FindString("workspace_key");
        result_workspace && *result_workspace != *workspace_key) {
      return std::nullopt;
    }

    CmuxCanonicalTerminalPlacement terminal;
    terminal.terminal_id = *terminal_id;
    terminal.workspace_key = *workspace_key;
    if (const std::string* incarnation = result->FindString("incarnation")) {
      terminal.incarnation = *incarnation;
    } else if (const base::Value* incarnation_value =
                   result->Find("incarnation");
               incarnation_value && !incarnation_value->is_none()) {
      return std::nullopt;
    }
    if (*kind == "terminal-closed") {
      terminal.lifecycle = CmuxTerminalLifecycle::kTombstoned;
    } else {
      const std::string* state = result->FindString("state");
      const std::optional<CmuxTerminalLifecycle> lifecycle =
          state ? ParseTerminalLifecycle(*state) : std::nullopt;
      if (!lifecycle || *lifecycle == CmuxTerminalLifecycle::kTombstoned) {
        return std::nullopt;
      }
      terminal.lifecycle = *lifecycle;
    }
    if (!IsValidCanonicalTerminal(terminal, /*allow_tombstone=*/true)) {
      return std::nullopt;
    }

    CmuxTuiTerminalRegistryEvent event;
    event.placement.registry_id = *registry_id;
    event.placement.generation = *generation;
    event.placement.terminal_revision = *event_revision;
    event.placement.terminal = std::move(terminal);
    event.kind = *kind;
    event.origin = *origin;
    event.mutation_id = *mutation_id;
    event_revisions.push_back(*event_revision);
    batch.events.push_back(std::move(event));
  }
  if (!ValidateCmuxTuiTerminalEventRevisions(after_revision, *revision,
                                             event_revisions)) {
    return std::nullopt;
  }
  return batch;
}

std::optional<CmuxTuiTerminalResolution> ParseTerminalResolution(
    const base::DictValue& data) {
  const std::optional<uint64_t> revision =
      ReadUnsigned(data, "terminal_revision");
  const std::string* registry_id = data.FindString("registry_id");
  const std::string* generation = data.FindString("generation");
  std::optional<CmuxCanonicalTerminalPlacement> terminal =
      ParseCanonicalTerminal(data, "terminal_incarnation",
                             /*allow_tombstone=*/true);
  if (!revision || !registry_id || registry_id->empty() || !generation ||
      generation->empty() || !terminal) {
    return std::nullopt;
  }

  CmuxTuiTerminalResolution resolution;
  resolution.registry_id = *registry_id;
  resolution.generation = *generation;
  resolution.terminal_revision = *revision;
  resolution.terminal = std::move(*terminal);
  resolution.changed = data.FindBool("changed").value_or(false);
  resolution.replayed = data.FindBool("replayed").value_or(false);
  const std::optional<uint64_t> surface = ReadUnsigned(data, "surface");
  if (surface && *surface != 0 && !resolution.terminal.incarnation.empty()) {
    CmuxTerminalBinding binding;
    if (!DecodeTerminalHostUuidV4(resolution.terminal.terminal_id,
                                  &binding.terminal_id) ||
        !DecodeTerminalHostUuidV4(resolution.terminal.incarnation,
                                  &binding.terminal_incarnation)) {
      return std::nullopt;
    }
    binding.surface = *surface;
    resolution.binding = binding;
  } else if (const base::Value* surface_value = data.Find("surface");
             surface_value && !surface_value->is_none() && !surface) {
    return std::nullopt;
  }
  return resolution;
}

std::optional<std::string> ValidateTerminalMutation(
    const CmuxTuiTerminalMutation& mutation) {
  if (mutation.mutation_id.empty()) {
    return "invalid_mutation_id";
  }
  if (mutation.expected_generation && mutation.expected_generation->empty()) {
    return "invalid_expected_generation";
  }
  if (mutation.expected_terminal_revision &&
      *mutation.expected_terminal_revision >
          static_cast<uint64_t>(kMaxExactJsonInteger)) {
    return "expected_terminal_revision_exceeds_json_integer_range";
  }
  return std::nullopt;
}

std::optional<std::string> ValidateWorkspaceMutation(
    const CmuxTuiWorkspaceMutation& mutation) {
  if (mutation.origin.empty()) {
    return "invalid_origin";
  }
  if (mutation.mutation_id.empty()) {
    return "invalid_mutation_id";
  }
  if (mutation.expected_generation && mutation.expected_generation->empty()) {
    return "invalid_expected_generation";
  }
  if (mutation.expected_revision &&
      *mutation.expected_revision >
          static_cast<uint64_t>(kMaxExactJsonInteger)) {
    return "expected_revision_exceeds_json_integer_range";
  }
  return std::nullopt;
}

void SetWorkspaceMutation(base::DictValue* request,
                          CmuxTuiWorkspaceMutation mutation) {
  request->Set("origin", std::move(mutation.origin));
  request->Set("mutation_id", std::move(mutation.mutation_id));
  if (mutation.expected_generation) {
    request->Set("expected_generation",
                 std::move(*mutation.expected_generation));
  }
  if (mutation.expected_revision) {
    request->Set("expected_revision",
                 ProtocolSurfaceId(*mutation.expected_revision));
  }
}

void SetTerminalMutation(base::DictValue* request,
                         CmuxTuiTerminalMutation mutation,
                         std::string_view origin) {
  request->Set("origin", std::string(origin));
  request->Set("mutation_id", std::move(mutation.mutation_id));
  if (mutation.expected_generation) {
    request->Set("expected_generation",
                 std::move(*mutation.expected_generation));
  }
  if (mutation.expected_terminal_revision) {
    request->Set("expected_terminal_revision",
                 ProtocolSurfaceId(*mutation.expected_terminal_revision));
  }
}

std::optional<CmuxTuiFrontendProjection> ParseFrontendProjection(
    const base::DictValue& data) {
  const std::optional<uint64_t> revision =
      ReadUnsigned(data, "projection_revision");
  const std::optional<uint64_t> schema_version =
      ReadUnsigned(data, "schema_version");
  const base::DictValue* projection = data.FindDict("projection");
  if (!revision || !schema_version || (*schema_version != 0 && !projection)) {
    return std::nullopt;
  }
  CmuxTuiFrontendProjection parsed;
  parsed.revision = *revision;
  parsed.schema_version = *schema_version;
  if (projection) {
    parsed.projection = projection->Clone();
  }
  return parsed;
}

std::string IdentityErrorMessage(CmuxTuiIdentityError error,
                                 std::string_view required_build_commit,
                                 std::string_view required_ghostty_commit) {
  switch (error) {
    case CmuxTuiIdentityError::kNone:
      return std::string();
    case CmuxTuiIdentityError::kInvalidEndpoint:
      return "socket endpoint is not supported cmux-tui protocol " +
             base::NumberToString(kMinCmuxTuiProtocolVersion) + "-" +
             base::NumberToString(kCmuxTuiProtocolVersion);
    case CmuxTuiIdentityError::kBuildCommitMissing:
      return "cmux-tui server has no build commit; expected " +
             std::string(required_build_commit);
    case CmuxTuiIdentityError::kBuildCommitMismatch:
      return "cmux-tui server build does not match required commit " +
             std::string(required_build_commit);
    case CmuxTuiIdentityError::kGhosttyCommitMissing:
      return "cmux-tui server has no Ghostty build commit; expected " +
             std::string(required_ghostty_commit);
    case CmuxTuiIdentityError::kGhosttyCommitMismatch:
      return "cmux-tui server Ghostty build does not match required commit " +
             std::string(required_ghostty_commit);
  }
}

CmuxTuiColors ParseColors(const base::DictValue& dict) {
  CmuxTuiColors colors;
  colors.foreground = OptionalString(dict, "fg");
  colors.background = OptionalString(dict, "bg");
  colors.cursor = OptionalString(dict, "cursor");
  colors.selection_background = OptionalString(dict, "selection_bg");
  colors.selection_foreground = OptionalString(dict, "selection_fg");
  if (const base::DictValue* palette = dict.FindDict("palette")) {
    for (const auto [key, value] : *palette) {
      unsigned index = 0;
      if (base::StringToUint(key, &index) && index < 256 && value.is_string()) {
        colors.palette.emplace(static_cast<uint16_t>(index), value.GetString());
      }
    }
  }
  colors.cursor_style = OptionalString(dict, "cursor_style");
  colors.cursor_blink = dict.FindBool("cursor_blink");
  return colors;
}

CmuxTuiEvent ParseEvent(base::Value message,
                        std::optional<std::vector<uint8_t>> decoded_bytes,
                        std::string decode_error,
                        CmuxTuiSurfaceId fallback_surface = 0) {
  CmuxTuiEvent event;
  if (!message.is_dict()) {
    event.error = "cmux-tui event is not an object";
    return event;
  }
  const base::DictValue& dict = message.GetDict();
  const std::string* name = dict.FindString("event");
  if (!name) {
    event.error = "cmux-tui event has no name";
    return event;
  }
  event.name = *name;
  event.surface = ReadUnsigned(dict, "surface").value_or(fallback_surface);
  event.cols = ReadUint16(dict, "cols").value_or(0);
  event.rows = ReadUint16(dict, "rows").value_or(0);
  event.bytes =
      decoded_bytes ? std::move(*decoded_bytes) : std::vector<uint8_t>();
  event.error = std::move(decode_error);
  if (const std::string* error = dict.FindString("error"); error) {
    event.error = *error;
  }
  event.workspace_revision = ReadUnsigned(dict, "workspace_revision");
  event.terminal_revision = ReadUnsigned(dict, "terminal_revision");
  event.projection_revision = ReadUnsigned(dict, "projection_revision");
  event.registry_id = OptionalString(dict, "registry_id").value_or("");
  event.generation = OptionalString(dict, "generation").value_or("");
  event.origin = OptionalString(dict, "origin").value_or("");
  event.mutation_id = OptionalString(dict, "mutation_id").value_or("");
  event.frontend = OptionalString(dict, "frontend").value_or("");
  event.frontend_scope = OptionalString(dict, "scope").value_or("");
  event.projection_subject_key =
      OptionalString(dict, "subject_key").value_or("");

  if (*name == "workspace-added" || *name == "workspace-closed" ||
      *name == "workspace-renamed" || *name == "workspace-moved") {
    event.workspace_index = ReadUnsigned(dict, "index");
    const base::DictValue* entity = dict.FindDict("entity");
    if (!entity) {
      entity = &dict;
    }
    const std::optional<uint64_t> id = ReadUnsigned(*entity, "id");
    const std::string* key = entity->FindString("key");
    const std::string* workspace_name = entity->FindString("name");
    if (id && *id != 0 && key && IsCmuxUuid(*key) && workspace_name) {
      CmuxTuiWorkspace workspace;
      workspace.id = *id;
      workspace.key = *key;
      workspace.name = *workspace_name;
      event.workspace = std::move(workspace);
    }
  }

  if (*name == "vt-state") {
    event.type = CmuxTuiEvent::Type::kVtState;
    if (const base::DictValue* colors = dict.FindDict("colors")) {
      event.colors = ParseColors(*colors);
    }
  } else if (*name == "output") {
    event.type = CmuxTuiEvent::Type::kOutput;
  } else if (*name == "resized") {
    event.type = CmuxTuiEvent::Type::kResized;
  } else if (*name == "colors-changed") {
    event.type = CmuxTuiEvent::Type::kColorsChanged;
    event.colors = ParseColors(dict);
  } else if (*name == "detached") {
    event.type = CmuxTuiEvent::Type::kDetached;
  } else if (*name == "overflow") {
    event.type = CmuxTuiEvent::Type::kOverflow;
  } else if (*name == "title-changed") {
    event.type = CmuxTuiEvent::Type::kTitleChanged;
    if (const std::string* title = dict.FindString("title")) {
      event.title = *title;
    }
  } else if (*name == "surface-exited") {
    event.type = CmuxTuiEvent::Type::kSurfaceExited;
  } else if (IsCmuxTuiTreeEventName(*name)) {
    event.type = CmuxTuiEvent::Type::kTreeChanged;
  } else if (*name == "terminal-registry-changed") {
    event.type = CmuxTuiEvent::Type::kTerminalRegistryChanged;
  }
  return event;
}

bool ApplyWorkspaceDelta(const CmuxTuiEvent& event,
                         CmuxTuiWorkspaceSnapshot* snapshot) {
  // Lifecycle events are immediately projected only when they form the exact
  // next revision in the same durable registry epoch. Any missing metadata,
  // duplicate, or gap falls back to the list-workspaces barrier below.
  if (!snapshot || !event.workspace || event.registry_id.empty() ||
      event.generation.empty() || !event.workspace_revision ||
      !IsNextCmuxWorkspaceRevision(
          snapshot->registry_id, snapshot->generation, snapshot->revision,
          event.registry_id, event.generation, *event.workspace_revision)) {
    return false;
  }
  auto& workspaces = snapshot->workspaces;
  const auto existing =
      std::find_if(workspaces.begin(), workspaces.end(),
                   [&event](const CmuxTuiWorkspace& workspace) {
                     return workspace.key == event.workspace->key;
                   });
  if (event.name == "workspace-closed") {
    if (existing == workspaces.end()) {
      return false;
    }
    workspaces.erase(existing);
  } else if (event.name == "workspace-renamed") {
    if (existing == workspaces.end()) {
      return false;
    }
    *existing = *event.workspace;
  } else if (event.name == "workspace-added" ||
             event.name == "workspace-moved") {
    CmuxTuiWorkspace workspace = *event.workspace;
    if (existing != workspaces.end()) {
      workspaces.erase(existing);
    } else if (event.name == "workspace-moved") {
      return false;
    }
    const size_t index = std::min(
        event.workspace_index.value_or(workspaces.size()), workspaces.size());
    workspaces.insert(workspaces.begin() + index, std::move(workspace));
  } else {
    return false;
  }
  snapshot->revision = *event.workspace_revision;
  snapshot->provisional = true;
  return true;
}

struct IncomingMessage {
  base::Value message;
  std::optional<std::vector<uint8_t>> bytes;
  std::string decode_error;
};

std::optional<IncomingMessage> ParseIncomingLine(std::string line,
                                                 std::string* error) {
  std::optional<base::Value> message =
      base::JSONReader::Read(line, base::JSON_PARSE_RFC);
  if (!message || !message->is_dict()) {
    *error = "cmux-tui sent invalid JSON object";
    return std::nullopt;
  }

  IncomingMessage incoming{std::move(*message), std::nullopt, std::string()};
  const base::DictValue& dict = incoming.message.GetDict();
  const std::string* event = dict.FindString("event");
  const std::string* encoded = nullptr;
  if (event && (*event == "vt-state" || *event == "output")) {
    encoded = dict.FindString("data");
  } else if (event && *event == "resized") {
    encoded = dict.FindString("replay");
  }
  if (event &&
      (*event == "vt-state" || *event == "output" || *event == "resized")) {
    if (!encoded) {
      incoming.decode_error = "cmux-tui byte event is missing base64 data";
    } else {
      incoming.bytes = base::Base64Decode(*encoded);
      if (!incoming.bytes) {
        incoming.decode_error = "cmux-tui byte event contains invalid base64";
      }
    }
  }
  return incoming;
}

class SocketState : public base::RefCountedThreadSafe<SocketState> {
 public:
  bool Adopt(SocketHandle socket, std::optional<uint64_t> peer_pid) {
    base::AutoLock lock(state_lock_);
    if (closing_) {
      CloseSocket(socket);
      return false;
    }
    CHECK_EQ(socket_, kInvalidSocket);
    socket_ = socket;
    peer_pid_ = peer_pid;
    return true;
  }

  std::optional<uint64_t> PeerPid() {
    base::AutoLock lock(state_lock_);
    return peer_pid_;
  }

  int Read(base::span<uint8_t> buffer) {
    SocketHandle socket = kInvalidSocket;
    {
      base::AutoLock lock(state_lock_);
      socket = socket_;
    }
    if (socket == kInvalidSocket) {
      return 0;
    }
#if BUILDFLAG(IS_WIN)
    return recv(socket, reinterpret_cast<char*>(buffer.data()),
                static_cast<int>(buffer.size()), 0);
#else
    return static_cast<int>(recv(socket, buffer.data(), buffer.size(), 0));
#endif
  }

  bool Write(std::string_view bytes, std::string* error) {
    // `write_lock_` keeps the descriptor alive until this send finishes.
    // Shutdown intentionally does not take it, so it can interrupt a blocked
    // send; the reader's final close waits here before releasing the handle.
    base::AutoLock write_lock(write_lock_);
    SocketHandle socket = kInvalidSocket;
    {
      base::AutoLock state_lock(state_lock_);
      if (closing_ || socket_ == kInvalidSocket) {
        *error = "cmux-tui socket is closed";
        return false;
      }
      socket = socket_;
    }
    while (!bytes.empty()) {
#if BUILDFLAG(IS_WIN)
      const int written =
          send(socket, bytes.data(), static_cast<int>(bytes.size()), 0);
#else
      const int flags =
#if BUILDFLAG(IS_LINUX)
          MSG_NOSIGNAL;
#else
          0;
#endif
      const ssize_t written = send(socket, bytes.data(), bytes.size(), flags);
#endif
      if (written < 0) {
        const int socket_error = LastSocketError();
#if !BUILDFLAG(IS_WIN)
        if (socket_error == EINTR) {
          continue;
        }
#endif
        *error = SocketError("send", socket_error);
        return false;
      }
      if (written == 0) {
        *error = "cmux-tui socket closed while writing";
        return false;
      }
      bytes.remove_prefix(static_cast<size_t>(written));
    }
    return true;
  }

  void Shutdown() {
    base::AutoLock lock(state_lock_);
    if (closing_) {
      return;
    }
    closing_ = true;
    // Do not take `write_lock_`: shutdown must be able to wake a blocked send.
    // The reader owns the final close after both I/O paths unwind.
    ShutdownSocket(socket_);
  }

  void CloseAfterRead() {
    {
      base::AutoLock state_lock(state_lock_);
      closing_ = true;
      ShutdownSocket(socket_);
    }
    base::AutoLock write_lock(write_lock_);
    base::AutoLock state_lock(state_lock_);
    CloseSocket(socket_);
    socket_ = kInvalidSocket;
  }

 private:
  friend class base::RefCountedThreadSafe<SocketState>;
  ~SocketState() {
    base::AutoLock write_lock(write_lock_);
    base::AutoLock state_lock(state_lock_);
    CloseSocket(socket_);
    socket_ = kInvalidSocket;
  }

  base::Lock write_lock_;
  base::Lock state_lock_;
  SocketHandle socket_ GUARDED_BY(state_lock_) = kInvalidSocket;
  std::optional<uint64_t> peer_pid_ GUARDED_BY(state_lock_);
  bool closing_ GUARDED_BY(state_lock_) = false;
};

bool ConnectSocket(const base::FilePath& path,
                   SocketState* state,
                   std::string* error) {
#if BUILDFLAG(IS_WIN)
  net::EnsureWinsockInit();
#endif
  const std::string native_path = path.AsUTF8Unsafe();
  sockaddr_un address = {};
  address.sun_family = AF_UNIX;
  if (native_path.empty() || native_path.size() >= sizeof(address.sun_path)) {
    *error = "cmux-tui socket path is empty or too long";
    return false;
  }
  base::span<char> sun_path(address.sun_path);
  sun_path.copy_prefix_from(base::span(native_path));
  sun_path[native_path.size()] = '\0';

  SocketHandle socket_handle = socket(AF_UNIX, SOCK_STREAM, 0);
  if (socket_handle == kInvalidSocket) {
    *error = SocketError("socket", LastSocketError());
    return false;
  }
#if BUILDFLAG(IS_MAC)
  int enabled = 1;
  setsockopt(socket_handle, SOL_SOCKET, SO_NOSIGPIPE, &enabled,
             sizeof(enabled));
#endif

  int result;
  do {
    result = connect(socket_handle, reinterpret_cast<sockaddr*>(&address),
                     sizeof(address));
  } while (
#if BUILDFLAG(IS_WIN)
      false
#else
      result < 0 && errno == EINTR
#endif
  );
  if (result < 0) {
    *error = SocketError("connect", LastSocketError());
    CloseSocket(socket_handle);
    return false;
  }
  std::optional<uint64_t> peer_pid;
#if BUILDFLAG(IS_MAC)
  pid_t native_peer_pid = 0;
  socklen_t native_peer_pid_size = sizeof(native_peer_pid);
  if (getsockopt(socket_handle, SOL_LOCAL, LOCAL_PEERPID, &native_peer_pid,
                 &native_peer_pid_size) != 0 ||
      native_peer_pid <= 0) {
    *error = SocketError("getsockopt(LOCAL_PEERPID)", LastSocketError());
    CloseSocket(socket_handle);
    return false;
  }
  peer_pid = static_cast<uint64_t>(native_peer_pid);
#elif BUILDFLAG(IS_LINUX)
  struct ucred credentials = {};
  socklen_t credentials_size = sizeof(credentials);
  if (getsockopt(socket_handle, SOL_SOCKET, SO_PEERCRED, &credentials,
                 &credentials_size) != 0 ||
      credentials.pid <= 0) {
    *error = SocketError("getsockopt(SO_PEERCRED)", LastSocketError());
    CloseSocket(socket_handle);
    return false;
  }
  peer_pid = static_cast<uint64_t>(credentials.pid);
#endif
  return state->Adopt(socket_handle, peer_pid);
}

base::FilePath ResolveBinary(const CmuxTuiClient::Options& options) {
  if (!options.binary_path.empty()) {
    return options.binary_path;
  }

  std::vector<base::FilePath> candidates;
  base::FilePath executable_dir;
  if (base::PathService::Get(base::DIR_EXE, &executable_dir)) {
#if BUILDFLAG(IS_MAC)
    candidates.push_back(executable_dir.DirName()
                             .Append(FILE_PATH_LITERAL("Helpers"))
                             .Append(FILE_PATH_LITERAL("cmux-tui")));
#elif BUILDFLAG(IS_WIN)
    candidates.push_back(
        executable_dir.Append(FILE_PATH_LITERAL("cmux-tui.exe")));
#else
    candidates.push_back(executable_dir.Append(FILE_PATH_LITERAL("cmux-tui")));
#endif
  }
#if BUILDFLAG(IS_MAC)
  candidates.emplace_back(FILE_PATH_LITERAL("/opt/homebrew/bin/cmux-tui"));
  candidates.emplace_back(FILE_PATH_LITERAL("/usr/local/bin/cmux-tui"));
#elif BUILDFLAG(IS_LINUX)
  candidates.emplace_back(FILE_PATH_LITERAL("/usr/local/bin/cmux-tui"));
  candidates.emplace_back(FILE_PATH_LITERAL("/usr/bin/cmux-tui"));
#endif

  std::unique_ptr<base::Environment> environment = base::Environment::Create();
  if (auto path = EnvironmentValue(environment.get(), "PATH")) {
#if BUILDFLAG(IS_WIN)
    constexpr char kPathSeparator[] = ";";
    constexpr char kBinaryName[] = "cmux-tui.exe";
#else
    constexpr char kPathSeparator[] = ":";
    constexpr char kBinaryName[] = "cmux-tui";
#endif
    for (const std::string& directory :
         base::SplitString(*path, kPathSeparator, base::TRIM_WHITESPACE,
                           base::SPLIT_WANT_NONEMPTY)) {
      candidates.push_back(base::FilePath::FromUTF8Unsafe(directory).Append(
          base::FilePath::FromUTF8Unsafe(kBinaryName)));
    }
  }

  for (const base::FilePath& candidate : candidates) {
    if (base::PathExists(candidate)) {
      return candidate;
    }
  }
  return base::FilePath();
}

struct LegacyServerTerminationResult {
  bool terminated = false;
  std::string error;
};

constexpr std::array<std::string_view, 4> kKnownDurableLegacyBuilds = {
    "8e4965dd2b0bb671daafe305b102768f264294ef",
    "50e2616ce4da1b333e1993b75ab89487d1222563",
    "18f39a65735312afce7dd116435ada074b5c3f06",
    // Last shipped frontend-parity helper before shutdown-daemon landed.
    "1e0e529a8eb2d3a9953a6cb44982fa9404a32261",
};

bool IsKnownDurableLegacyBuild(std::string_view build_commit) {
  return std::find(kKnownDurableLegacyBuilds.begin(),
                   kKnownDurableLegacyBuilds.end(), build_commit) !=
         kKnownDurableLegacyBuilds.end();
}

bool IsValidServerPid(uint64_t pid) {
  return pid != 0 && pid <= std::numeric_limits<uint32_t>::max() &&
         pid <= static_cast<uint64_t>(
                    std::numeric_limits<base::ProcessId>::max());
}

LegacyServerTerminationResult TerminateVerifiedLegacyServer(
    CmuxTuiClient::Options options,
    uint64_t pid,
    std::string build_commit) {
#if BUILDFLAG(IS_POSIX)
  // Protocol 7 predates durable per-terminal hosts. Only commits we shipped
  // after that boundary may receive the one-release SIGTERM fallback; an
  // unknown old daemon must be restarted manually so this browser never kills
  // a PTY-owning mux.
  if (!IsKnownDurableLegacyBuild(build_commit)) {
    return {false,
            "legacy cmux-tui build is not on the durable-host allowlist; "
            "restart that daemon manually once"};
  }
  if (!IsValidServerPid(pid)) {
    return {false, "legacy cmux-tui server returned an invalid pid"};
  }
  const base::ProcessId process_id = static_cast<base::ProcessId>(pid);
  base::Process process = base::Process::Open(process_id);
  if (!process.IsValid()) {
    // The fenced process exited between its identify response and this legacy
    // fallback. That is already the desired handoff boundary.
    return {true, std::string()};
  }

  const base::FilePath expected =
      base::MakeAbsoluteFilePath(ResolveBinary(options));
  const base::FilePath actual =
      base::MakeAbsoluteFilePath(base::GetProcessExecutablePath(process_id));
  if (actual.empty()) {
    // The peer can exit after list-clients and before this worker runs. Never
    // turn that successful handoff boundary into an attempt to signal a
    // recycled PID.
    if (kill(process_id, 0) != 0 && errno == ESRCH) {
      return {true, std::string()};
    }
    return {false, "could not revalidate the legacy cmux-tui executable"};
  }
  if (expected.empty() || expected != actual) {
    return {false,
            "refusing to signal legacy server whose executable is not the "
            "configured cmux-tui binary"};
  }
  if (!process.Terminate(/*exit_code=*/0, /*wait=*/false)) {
    return {false, "failed to terminate verified legacy cmux-tui server"};
  }
  return {true, std::string()};
#else
  return {false,
          "automatic replacement of pre-handoff cmux-tui is unavailable on "
          "this platform"};
#endif
}

LegacyServerTerminationResult WaitForVerifiedServerExit(
    CmuxTuiClient::Options options,
    uint64_t pid) {
  if (!IsValidServerPid(pid)) {
    return {false, "cmux-tui server returned an invalid pid"};
  }
  const base::ProcessId process_id = static_cast<base::ProcessId>(pid);
#if BUILDFLAG(IS_POSIX)
  const base::FilePath expected =
      base::MakeAbsoluteFilePath(ResolveBinary(options));
  if (expected.empty()) {
    return {false, "could not resolve the configured cmux-tui binary"};
  }
  const base::TimeTicks deadline = base::TimeTicks::Now() + base::Seconds(5);
  for (;;) {
    errno = 0;
    if (kill(process_id, 0) != 0 && errno == ESRCH) {
      return {true, std::string()};
    }
    const base::FilePath actual =
        base::MakeAbsoluteFilePath(base::GetProcessExecutablePath(process_id));
    if (actual.empty() || actual != expected) {
      // Empty means the old process is already gone (or a zombie); a different
      // path means the PID was recycled. In either case its writer lease is no
      // longer held, and the replacement must not wait on the unrelated PID.
      return {true, std::string()};
    }
    if (base::TimeTicks::Now() >= deadline) {
      break;
    }
    base::PlatformThread::Sleep(base::Milliseconds(25));
  }
#else
  base::Process process = base::Process::Open(process_id);
  if (!process.IsValid()) {
    return {true, std::string()};
  }
  if (process.WaitForExitWithTimeout(base::Seconds(5), nullptr)) {
    return {true, std::string()};
  }
#endif
  {
    return {false,
            "timed out waiting for the old cmux-tui daemon to release state"};
  }
}

struct LaunchResult {
  bool launched = false;
  std::string error;
};

LaunchResult LaunchHeadlessServer(CmuxTuiClient::Options options) {
  const base::FilePath binary = ResolveBinary(options);
  if (binary.empty()) {
    return {false,
            "cannot find cmux-tui; set CMUX_TUI_BINARY to its absolute path"};
  }
  base::CommandLine command(binary);
  command.AppendSwitch("headless");
  // cmux-tui's parser currently accepts `--key value`, not Chromium's
  // `AppendSwitch*` spelling of `--key=value`.
  command.AppendArg("--session");
  command.AppendArg(options.session);
  command.AppendArg("--socket");
  command.AppendArgPath(options.socket_path);
  base::Process process = base::LaunchProcess(command, base::LaunchOptions());
  if (!process.IsValid()) {
    return {false, "failed to launch " + binary.AsUTF8Unsafe()};
  }
  VLOG(1) << "cmux-tui: launched headless server pid=" << process.Pid()
          << " socket=" << options.socket_path;
  return {true, std::string()};
}

}  // namespace

class CmuxTuiConnection {
 public:
  using ConnectedCallback =
      base::OnceCallback<void(bool connected, std::string error)>;
  using MessageCallback =
      base::RepeatingCallback<void(base::Value message,
                                   std::optional<std::vector<uint8_t>> bytes,
                                   std::string decode_error)>;
  using DisconnectedCallback = base::RepeatingCallback<void(std::string)>;
  using CommandCallback = CmuxTuiClient::CommandCallback;

  CmuxTuiConnection(base::FilePath socket_path,
                    MessageCallback message_callback,
                    DisconnectedCallback disconnected_callback)
      : socket_path_(std::move(socket_path)),
        state_(base::MakeRefCounted<SocketState>()),
        ui_runner_(base::SingleThreadTaskRunner::GetCurrentDefault()),
        writer_runner_(base::ThreadPool::CreateSequencedTaskRunner(
            {base::MayBlock(), base::TaskPriority::USER_VISIBLE,
             base::TaskShutdownBehavior::CONTINUE_ON_SHUTDOWN})),
        message_callback_(std::move(message_callback)),
        disconnected_callback_(std::move(disconnected_callback)) {
    DETACH_FROM_SEQUENCE(sequence_checker_);
  }

  CmuxTuiConnection(const CmuxTuiConnection&) = delete;
  CmuxTuiConnection& operator=(const CmuxTuiConnection&) = delete;

  ~CmuxTuiConnection() {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    weak_factory_.InvalidateWeakPtrs();
    state_->Shutdown();
    pending_.clear();
  }

  void Start(ConnectedCallback callback) {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    CHECK(!started_);
    started_ = true;
    connected_callback_ = std::move(callback);
    base::ThreadPool::PostTask(
        FROM_HERE,
        {base::MayBlock(), base::TaskPriority::USER_VISIBLE,
         base::TaskShutdownBehavior::CONTINUE_ON_SHUTDOWN},
        base::BindOnce(&CmuxTuiConnection::RunReader, socket_path_, state_,
                       ui_runner_, weak_factory_.GetWeakPtr()));
  }

  void Close() {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    state_->Shutdown();
  }

  std::optional<uint64_t> peer_pid() {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    return state_->PeerPid();
  }

  void SendCommand(base::DictValue request, CommandCallback callback) {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    if (!connected_) {
      CmuxTuiCommandResult result;
      result.error = "cmux-tui is not connected";
      if (callback) {
        std::move(callback).Run(std::move(result));
      }
      return;
    }
    const int id = next_id_++;
    request.Set("id", id);
    // Track even fire-and-forget requests so their mandatory responses are
    // consumed quietly instead of being reported as unknown request ids.
    pending_.emplace(id, std::move(callback));
    base::Value value(std::move(request));
    std::string json;
    // Surface ids are uint64_t on the wire but base::Value only has int32_t
    // and double numeric storage. ProtocolSurfaceId therefore stores exact
    // JSON-safe integers as doubles. The default writer preserves that C++
    // type by emitting e.g. `7.0`, which serde rejects for cmux-tui's u64 id
    // fields. Omit only the artificial `.0`; non-integral values (none are
    // valid in this protocol) would still be serialized as JSON decimals.
    if (!base::JSONWriter::WriteWithOptions(
            value, base::JSONWriter::OPTIONS_OMIT_DOUBLE_TYPE_PRESERVATION,
            &json)) {
      auto pending = pending_.find(id);
      if (pending != pending_.end()) {
        CmuxTuiCommandResult result;
        result.error = "failed to encode cmux-tui request";
        CommandCallback failed = std::move(pending->second);
        pending_.erase(pending);
        if (failed) {
          std::move(failed).Run(std::move(result));
        }
      }
      return;
    }
    json.push_back('\n');
    writer_runner_->PostTask(
        FROM_HERE,
        base::BindOnce(&CmuxTuiConnection::RunWrite, state_, std::move(json),
                       ui_runner_, weak_factory_.GetWeakPtr()));
  }

 private:
  static void RunReader(base::FilePath socket_path,
                        scoped_refptr<SocketState> state,
                        scoped_refptr<base::SingleThreadTaskRunner> ui_runner,
                        base::WeakPtr<CmuxTuiConnection> connection) {
    std::string error;
    if (!ConnectSocket(socket_path, state.get(), &error)) {
      ui_runner->PostTask(FROM_HERE,
                          base::BindOnce(&CmuxTuiConnection::OnConnectResult,
                                         connection, false, std::move(error)));
      return;
    }
    ui_runner->PostTask(FROM_HERE,
                        base::BindOnce(&CmuxTuiConnection::OnConnectResult,
                                       connection, true, std::string()));

    CmuxTuiLineFramer framer;
    std::array<uint8_t, 64 * 1024> buffer;
    for (;;) {
      const int read = state->Read(base::span(buffer));
      if (read == 0) {
        error = "cmux-tui socket closed";
        break;
      }
      if (read < 0) {
        const int socket_error = LastSocketError();
#if !BUILDFLAG(IS_WIN)
        if (socket_error == EINTR) {
          continue;
        }
#endif
        error = SocketError("recv", socket_error);
        break;
      }
      std::vector<std::string> lines;
      if (framer.Push(base::as_string_view(
                          base::span(buffer).first(static_cast<size_t>(read))),
                      &lines) == CmuxTuiLineFramer::Result::kLineTooLarge) {
        error = "cmux-tui sent a JSON line larger than 32 MiB";
        break;
      }
      for (std::string& line : lines) {
        if (base::TrimWhitespaceASCII(line, base::TRIM_ALL).empty()) {
          continue;
        }
        std::string parse_error;
        std::optional<IncomingMessage> incoming =
            ParseIncomingLine(std::move(line), &parse_error);
        if (!incoming) {
          error = std::move(parse_error);
          break;
        }
        ui_runner->PostTask(FROM_HERE,
                            base::BindOnce(&CmuxTuiConnection::OnIncoming,
                                           connection, std::move(*incoming)));
      }
      if (!error.empty()) {
        break;
      }
    }
    state->CloseAfterRead();
    ui_runner->PostTask(FROM_HERE,
                        base::BindOnce(&CmuxTuiConnection::OnReaderDisconnected,
                                       connection, std::move(error)));
  }

  static void RunWrite(scoped_refptr<SocketState> state,
                       std::string json,
                       scoped_refptr<base::SingleThreadTaskRunner> ui_runner,
                       base::WeakPtr<CmuxTuiConnection> connection) {
    std::string error;
    if (state->Write(json, &error)) {
      return;
    }
    state->Shutdown();
    ui_runner->PostTask(
        FROM_HERE, base::BindOnce(&CmuxTuiConnection::OnWriteFailed, connection,
                                  std::move(error)));
  }

  void OnConnectResult(bool connected, std::string error) {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    connected_ = connected;
    if (connected_callback_) {
      std::move(connected_callback_).Run(connected, std::move(error));
    }
  }

  void OnIncoming(IncomingMessage incoming) {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    if (!incoming.message.is_dict()) {
      return;
    }
    const base::DictValue& dict = incoming.message.GetDict();
    if (dict.FindString("event")) {
      message_callback_.Run(std::move(incoming.message),
                            std::move(incoming.bytes),
                            std::move(incoming.decode_error));
      return;
    }

    const std::optional<uint64_t> response_id = ReadUnsigned(dict, "id");
    if (!response_id || *response_id > std::numeric_limits<int>::max()) {
      LOG(WARNING) << "cmux-tui: response without a usable request id";
      return;
    }
    auto pending = pending_.find(static_cast<int>(*response_id));
    if (pending == pending_.end()) {
      LOG(WARNING) << "cmux-tui: response for unknown request " << *response_id;
      return;
    }
    CommandCallback callback = std::move(pending->second);
    pending_.erase(pending);
    CmuxTuiCommandResult result;
    result.ok = dict.FindBool("ok").value_or(false);
    if (const base::DictValue* data = dict.FindDict("data")) {
      result.data = data->Clone();
    } else if (const base::ListValue* list_data = dict.FindList("data")) {
      result.list_data.emplace(list_data->Clone());
    }
    if (const std::string* response_error = dict.FindString("error")) {
      result.error = *response_error;
    }
    if (!result.ok && result.error.empty()) {
      result.error = "cmux-tui command failed without an error message";
    }
    if (callback) {
      std::move(callback).Run(std::move(result));
    }
  }

  void OnWriteFailed(std::string error) {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    ReportDisconnected(std::move(error));
  }

  void OnReaderDisconnected(std::string error) {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    ReportDisconnected(std::move(error));
  }

  void ReportDisconnected(std::string error) {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    if (disconnected_reported_) {
      return;
    }
    disconnected_reported_ = true;
    connected_ = false;
    std::map<int, CommandCallback> pending = std::move(pending_);
    DisconnectedCallback disconnected_callback = disconnected_callback_;
    for (auto& entry : pending) {
      CmuxTuiCommandResult result;
      result.error = error;
      if (entry.second) {
        std::move(entry.second).Run(std::move(result));
      }
    }
    if (disconnected_callback) {
      disconnected_callback.Run(std::move(error));
    }
  }

  const base::FilePath socket_path_;
  const scoped_refptr<SocketState> state_;
  const scoped_refptr<base::SingleThreadTaskRunner> ui_runner_;
  const scoped_refptr<base::SequencedTaskRunner> writer_runner_;
  MessageCallback message_callback_;
  DisconnectedCallback disconnected_callback_;
  ConnectedCallback connected_callback_;
  std::map<int, CommandCallback> pending_;
  int next_id_ = 1;
  bool started_ = false;
  bool connected_ = false;
  bool disconnected_reported_ = false;
  SEQUENCE_CHECKER(sequence_checker_);
  base::WeakPtrFactory<CmuxTuiConnection> weak_factory_{this};
};

// static
CmuxTuiClient::Options CmuxTuiClient::Options::FromEnvironment() {
  Options options;
  options.required_build_commit = kPinnedCmuxTuiBuildCommit;
  options.required_ghostty_commit = kPinnedCmuxTuiGhosttyCommit;
  std::unique_ptr<base::Environment> environment = base::Environment::Create();
  if (auto session = EnvironmentValue(environment.get(), "CMUX_TUI_SESSION")) {
    options.session = *session;
  }
  if (auto socket = EnvironmentValue(environment.get(), "CMUX_TUI_SOCKET")) {
    options.socket_path = base::FilePath::FromUTF8Unsafe(*socket);
  } else {
    options.socket_path = DefaultSocketPath(options.session, environment.get());
  }
  if (auto binary = EnvironmentValue(environment.get(), "CMUX_TUI_BINARY")) {
    options.binary_path = base::FilePath::FromUTF8Unsafe(*binary);
  }
  if (auto commit =
          EnvironmentValue(environment.get(), "CMUX_TUI_EXPECTED_COMMIT")) {
    options.required_build_commit = *commit;
  }
  if (auto commit = EnvironmentValue(environment.get(),
                                     "CMUX_TUI_EXPECTED_GHOSTTY_COMMIT")) {
    options.required_ghostty_commit = *commit;
  }
  if (auto allow_unpinned =
          EnvironmentValue(environment.get(), "CMUX_TUI_ALLOW_UNPINNED");
      allow_unpinned && *allow_unpinned == "1") {
    options.required_build_commit.clear();
    options.required_ghostty_commit.clear();
  }
  return options;
}

// static
CmuxTuiClient::Options CmuxTuiClient::Options::FromEnvironment(
    const CmuxFrontendIdentity& identity) {
  Options options = FromEnvironment();
  options.frontend = identity.frontend;
  options.frontend_scope = identity.scope;
  options.projection_subject_key = identity.subject_key;
  options.origin = identity.origin;

  std::unique_ptr<base::Environment> environment = base::Environment::Create();
  if (!EnvironmentValue(environment.get(), "CMUX_TUI_SESSION")) {
    options.session = identity.session;
  }
  if (!EnvironmentValue(environment.get(), "CMUX_TUI_SOCKET")) {
    options.socket_path = DefaultSocketPath(options.session, environment.get());
  }
  return options;
}

CmuxTuiClient::CmuxTuiClient(Options options) : options_(std::move(options)) {
  DETACH_FROM_SEQUENCE(sequence_checker_);
}

CmuxTuiClient::~CmuxTuiClient() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  weak_factory_.InvalidateWeakPtrs();
  for (auto& entry : renderer_connect_cancellations_) {
    if (entry.second) {
      std::move(entry.second).Run();
    }
  }
  renderer_connect_cancellations_.clear();
  control_.reset();
}

void CmuxTuiClient::Start(ReadyCallback callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  CHECK(!started_);
  started_ = true;
  start_callback_ = std::move(callback);
  ConnectControl();
}

bool CmuxTuiClient::ready() const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  return ready_;
}

uint64_t CmuxTuiClient::server_generation() const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  return server_generation_;
}

void CmuxTuiClient::AddObserver(Observer* observer) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  observers_.AddObserver(observer);
}

void CmuxTuiClient::RemoveObserver(Observer* observer) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  observers_.RemoveObserver(observer);
}

void CmuxTuiClient::ConnectControl() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  reconnect_scheduled_ = false;
  ++connect_attempts_;
  control_ = std::make_unique<CmuxTuiConnection>(
      options_.socket_path,
      base::BindRepeating(&CmuxTuiClient::OnControlMessage,
                          weak_factory_.GetWeakPtr()),
      base::BindRepeating(&CmuxTuiClient::OnControlDisconnected,
                          weak_factory_.GetWeakPtr()));
  control_->Start(base::BindOnce(&CmuxTuiClient::OnControlConnected,
                                 weak_factory_.GetWeakPtr()));
}

void CmuxTuiClient::OnControlConnected(bool connected, std::string error) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!connected) {
    if (!launch_attempted_ && options_.spawn_if_missing) {
      LaunchServer();
      return;
    }
    if (connect_attempts_ < kMaxInitialConnectAttempts) {
      ScheduleReconnect();
      return;
    }
    SetReady(false, std::move(error));
    return;
  }

  connect_attempts_ = 0;
  base::DictValue identify;
  identify.Set("cmd", "identify");
  Request(std::move(identify), base::BindOnce(&CmuxTuiClient::VerifyIdentity,
                                              weak_factory_.GetWeakPtr()));
}

void CmuxTuiClient::OnControlDisconnected(std::string error) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  workspace_refresh_in_flight_ = false;
  workspace_refresh_pending_ = false;
  workspace_snapshot_ready_ = false;
  latest_workspace_snapshot_.reset();
  terminal_refresh_in_flight_ = false;
  terminal_refresh_pending_ = false;
  terminal_snapshot_ready_ = false;
  latest_terminal_snapshot_.reset();
  terminal_event_registry_id_.clear();
  terminal_event_generation_.clear();
  terminal_event_revision_ = 0;
  if (server_replacement_in_progress_) {
    // A cold-start handoff deliberately closes this connection. Do not
    // consume the one-shot startup callback with that expected disconnect;
    // reconnect until the pinned replacement owns the socket instead.
    if (!server_replacement_waiting_for_exit_) {
      ScheduleReconnect();
    }
    return;
  }
  const bool was_ready = ready_;
  const bool should_reconnect =
      !protocol_rejected_ &&
      (was_ready || connect_attempts_ < kMaxInitialConnectAttempts);
  if (should_reconnect) {
    ScheduleReconnect(was_ready);
  }
  // This can invoke the one-shot startup callback and release the final client
  // reference, so it must be the last operation in this method.
  SetReady(false, std::move(error));
}

void CmuxTuiClient::OnControlMessage(base::Value message,
                                     std::optional<std::vector<uint8_t>> bytes,
                                     std::string decode_error) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  CmuxTuiEvent event =
      ParseEvent(std::move(message), std::move(bytes), std::move(decode_error));
  if (event.name == "frontend-projection-changed" &&
      event.registry_id.empty() && event.generation.empty() &&
      latest_workspace_snapshot_) {
    // Protocol 7's projection event predates registry identity fields. Bind
    // that hint to the authoritative snapshot already established on this
    // exact control stream. Before the first snapshot we leave it unbound so
    // observers fail closed; the startup projection GET is the safe barrier.
    event.registry_id = latest_workspace_snapshot_->registry_id;
    event.generation = latest_workspace_snapshot_->generation;
  }
  if (event.type == CmuxTuiEvent::Type::kOverflow) {
    // The control subscription may have dropped lifecycle events. Reconnect
    // and subscribe from a known boundary instead of continuing with a stale
    // view of titles/exits/tree state. Attach-stream overflow is handled by
    // CmuxTerminalBackend on its independent socket.
    if (control_) {
      control_->Close();
    }
    return;
  }
  if (event.type == CmuxTuiEvent::Type::kTreeChanged &&
      latest_workspace_snapshot_) {
    // Tree events include active workspace/tab changes, but their compact
    // delta does not carry the complete active path. Invalidate selection
    // authority immediately so an interleaved terminal reconciliation cannot
    // reapply the previous active terminal before list-workspaces returns.
    latest_workspace_snapshot_->provisional = true;
  }
  base::WeakPtr<CmuxTuiClient> alive = weak_factory_.GetWeakPtr();
  for (Observer& observer : observers_) {
    observer.OnCmuxTuiEvent(event);
    if (!alive) {
      return;
    }
  }
  if (event.type == CmuxTuiEvent::Type::kTreeChanged) {
    if (latest_workspace_snapshot_) {
      CmuxTuiWorkspaceSnapshot projected = *latest_workspace_snapshot_;
      if (ApplyWorkspaceDelta(event, &projected)) {
        latest_workspace_snapshot_ = projected;
        base::WeakPtr<CmuxTuiClient> delta_alive = weak_factory_.GetWeakPtr();
        for (Observer& observer : observers_) {
          observer.OnCmuxTuiWorkspaceSnapshot(projected);
          if (!delta_alive) {
            return;
          }
        }
      }
    }
    RefreshWorkspaces();
  } else if (event.type == CmuxTuiEvent::Type::kTerminalRegistryChanged) {
    // The subscription event is an invalidation barrier, never state. Fetch
    // exact deltas for immediate projection and always finish with an
    // authoritative list-terminals snapshot.
    RefreshTerminals();
  }
}

void CmuxTuiClient::RejectControlProtocol(std::string error) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  protocol_rejected_ = true;
  if (control_) {
    control_->Close();
  }
  SetReady(false, std::move(error));
}

void CmuxTuiClient::VerifyIdentity(CmuxTuiCommandResult result) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  const std::string* app = result.data.FindString("app");
  const std::optional<uint64_t> protocol =
      ReadUnsigned(result.data, "protocol");
  const std::optional<uint64_t> pid = ReadUnsigned(result.data, "pid");
  const std::optional<uint64_t> daemon_handoff =
      ReadUnsigned(result.data, "daemon_handoff");
  const std::string* generation = result.data.FindString("generation");
  const std::string* build_commit = result.data.FindString("build_commit");
  const std::string* ghostty_commit = result.data.FindString("ghostty_commit");
  CmuxTuiIdentityError identity_error =
      result.ok && app && protocol && pid
          ? ValidateCmuxTuiIdentity(
                *app, *protocol, *pid,
                build_commit ? std::optional<std::string_view>(*build_commit)
                             : std::nullopt,
                options_.required_build_commit,
                ghostty_commit
                    ? std::optional<std::string_view>(*ghostty_commit)
                    : std::nullopt,
                options_.required_ghostty_commit)
          : CmuxTuiIdentityError::kInvalidEndpoint;
  const std::optional<uint64_t> peer_pid =
      control_ ? control_->peer_pid() : std::nullopt;
  if (pid && peer_pid && *pid != *peer_pid) {
    identity_error = CmuxTuiIdentityError::kInvalidEndpoint;
  }
  if (identity_error != CmuxTuiIdentityError::kNone) {
    std::string error =
        !result.error.empty()
            ? result.error
            : IdentityErrorMessage(identity_error,
                                   options_.required_build_commit,
                                   options_.required_ghostty_commit);
    if (server_replacement_in_progress_) {
      if (++server_replacement_identity_retries_ < 20) {
        if (control_) {
          control_->Close();
        }
        ScheduleReconnect();
        return;
      }
      server_replacement_in_progress_ = false;
      RejectControlProtocol("cmux-tui replacement did not acquire the socket: " +
                            error);
      return;
    }
    if (!server_replacement_attempted_ && !ever_ready_ &&
        options_.spawn_if_missing && pid && IsValidServerPid(*pid) &&
        peer_pid && *peer_pid == *pid && generation &&
        !generation->empty() &&
        IsReplaceableCmuxTuiIdentityError(identity_error)) {
      RequestServerReplacement(
          *pid, *generation, build_commit ? *build_commit : std::string(),
          daemon_handoff.value_or(0) >= 1,
          std::move(error));
      return;
    }
    RejectControlProtocol(std::move(error));
    return;
  }

  server_replacement_attempted_ = false;
  server_replacement_in_progress_ = false;
  server_replacement_waiting_for_exit_ = false;
  server_replacement_identity_retries_ = 0;

  if (!server_pid_ || *server_pid_ != *pid) {
    server_pid_ = *pid;
    ++server_generation_;
  }

  base::DictValue client_info;
  client_info.Set("cmd", "set-client-info");
  client_info.Set("name", "cmux-browser");
  client_info.Set("kind", "native-browser");
  Request(std::move(client_info),
          base::BindOnce(
              [](base::WeakPtr<CmuxTuiClient> client,
                 CmuxTuiCommandResult info_result) {
                if (!client) {
                  return;
                }
                if (!info_result.ok) {
                  client->RejectControlProtocol(
                      info_result.error.empty()
                          ? "cmux-tui protocol " +
                                base::NumberToString(kCmuxTuiProtocolVersion) +
                                " rejected set-client-info"
                          : std::move(info_result.error));
                  return;
                }
                base::DictValue subscribe;
                subscribe.Set("cmd", "subscribe");
                subscribe.Set("tree_events", "deltas");
                client->Request(
                    std::move(subscribe),
                    base::BindOnce(&CmuxTuiClient::OnSubscribed, client));
              },
              weak_factory_.GetWeakPtr()));
}

void CmuxTuiClient::RequestServerReplacement(uint64_t pid,
                                             std::string generation,
                                             std::string build_commit,
                                             bool daemon_handoff_supported,
                                             std::string identity_error) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  server_replacement_attempted_ = true;
  server_replacement_in_progress_ = true;
  server_replacement_waiting_for_exit_ = true;
  server_replacement_identity_retries_ = 0;
  base::DictValue request;
  request.Set("cmd", "list-clients");
  Request(std::move(request),
          base::BindOnce(&CmuxTuiClient::OnServerReplacementClientsListed,
                         weak_factory_.GetWeakPtr(), pid,
                         std::move(generation), std::move(build_commit),
                         daemon_handoff_supported,
                         std::move(identity_error)));
}

void CmuxTuiClient::OnServerReplacementClientsListed(
    uint64_t pid,
    std::string generation,
    std::string build_commit,
    bool daemon_handoff_supported,
    std::string identity_error,
    CmuxTuiCommandResult result) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!result.ok || !result.list_data || result.list_data->empty()) {
    server_replacement_in_progress_ = false;
    server_replacement_waiting_for_exit_ = false;
    RejectControlProtocol(
        std::move(identity_error) +
        "; cannot prove the stale daemon has no live browser owner");
    return;
  }
  bool saw_self = false;
  for (const base::Value& value : *result.list_data) {
    if (!value.is_dict()) {
      server_replacement_in_progress_ = false;
      server_replacement_waiting_for_exit_ = false;
      RejectControlProtocol(std::move(identity_error) +
                            "; stale daemon returned malformed client state");
      return;
    }
    const base::DictValue& client = value.GetDict();
    const std::optional<bool> is_self = client.FindBool("self");
    if (!is_self) {
      server_replacement_in_progress_ = false;
      server_replacement_waiting_for_exit_ = false;
      RejectControlProtocol(std::move(identity_error) +
                            "; stale daemon returned malformed client state");
      return;
    }
    saw_self = saw_self || *is_self;
    if (!*is_self &&
        client.FindString("kind") &&
        *client.FindString("kind") == "native-browser") {
      server_replacement_in_progress_ = false;
      server_replacement_waiting_for_exit_ = false;
      RejectControlProtocol(
          std::move(identity_error) +
          "; close the other browser frontend before upgrading cmux-tui");
      return;
    }
  }
  if (!saw_self) {
    server_replacement_in_progress_ = false;
    server_replacement_waiting_for_exit_ = false;
    RejectControlProtocol(std::move(identity_error) +
                          "; stale daemon omitted the requesting client");
    return;
  }

  if (IsKnownDurableLegacyBuild(build_commit)) {
    StartLegacyServerTermination(pid, std::move(build_commit),
                                 std::move(identity_error));
    return;
  }
  if (!daemon_handoff_supported) {
    server_replacement_in_progress_ = false;
    server_replacement_waiting_for_exit_ = false;
    RejectControlProtocol(
        std::move(identity_error) +
        "; stale daemon does not advertise safe handoff; restart it manually");
    return;
  }

  base::DictValue request;
  request.Set("cmd", "shutdown-daemon");
  request.Set("pid", ProtocolSurfaceId(pid));
  request.Set("generation", std::move(generation));
  Request(std::move(request),
          base::BindOnce(&CmuxTuiClient::OnServerReplacementRequested,
                         weak_factory_.GetWeakPtr(), pid,
                         std::move(identity_error)));
}

void CmuxTuiClient::StartLegacyServerTermination(
    uint64_t pid,
    std::string build_commit,
    std::string identity_error) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  // One release of the browser must be able to replace its immediately
  // preceding durable-host daemon, which predates shutdown-daemon. The worker
  // revalidates the peer-bound PID's exact executable before signalling.
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE,
      {base::MayBlock(), base::TaskPriority::USER_VISIBLE,
       base::TaskShutdownBehavior::CONTINUE_ON_SHUTDOWN},
      base::BindOnce(&TerminateVerifiedLegacyServer, options_, pid,
                     std::move(build_commit)),
      base::BindOnce(
          [](base::WeakPtr<CmuxTuiClient> client,
             uint64_t old_pid,
             std::string original_identity_error,
             LegacyServerTerminationResult termination) {
            if (client) {
              client->OnLegacyServerTerminated(
                  old_pid, std::move(original_identity_error),
                  termination.terminated, std::move(termination.error));
            }
          },
          weak_factory_.GetWeakPtr(), pid, std::move(identity_error)));
}

void CmuxTuiClient::OnServerReplacementRequested(
    uint64_t pid,
    std::string identity_error,
    CmuxTuiCommandResult result) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!result.ok && !result.error.empty()) {
    identity_error += "; daemon handoff acknowledgement was lost or rejected: " +
                      result.error;
  }
  // The daemon queues its ACK before leaving, but process teardown can win the
  // final socket flush. PID/executable polling is therefore the authoritative
  // writer-lease barrier for both an ACK and a post-request EOF.
  WaitForServerReplacementExit(pid, std::move(identity_error));
}

void CmuxTuiClient::OnLegacyServerTerminated(uint64_t pid,
                                             std::string identity_error,
                                             bool terminated,
                                             std::string error) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!terminated) {
    server_replacement_in_progress_ = false;
    server_replacement_waiting_for_exit_ = false;
    RejectControlProtocol(std::move(identity_error) +
                          "; automatic daemon handoff failed: " + error);
    return;
  }
  WaitForServerReplacementExit(pid, std::move(identity_error));
}

void CmuxTuiClient::WaitForServerReplacementExit(
    uint64_t pid,
    std::string identity_error) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE,
      {base::MayBlock(), base::TaskPriority::USER_VISIBLE,
       base::TaskShutdownBehavior::CONTINUE_ON_SHUTDOWN},
      base::BindOnce(&WaitForVerifiedServerExit, options_, pid),
      base::BindOnce(
          [](base::WeakPtr<CmuxTuiClient> client,
             std::string original_identity_error,
             LegacyServerTerminationResult wait_result) {
            if (client) {
              client->OnServerReplacementExitWaited(
                  std::move(original_identity_error), wait_result.terminated,
                  std::move(wait_result.error));
            }
          },
          weak_factory_.GetWeakPtr(), std::move(identity_error)));
}

void CmuxTuiClient::OnServerReplacementExitWaited(std::string identity_error,
                                                  bool exited,
                                                  std::string error) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!exited) {
    server_replacement_in_progress_ = false;
    server_replacement_waiting_for_exit_ = false;
    RejectControlProtocol(std::move(identity_error) +
                          "; automatic daemon handoff failed: " + error);
    return;
  }
  ContinueServerReplacement();
}

void CmuxTuiClient::ContinueServerReplacement() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  server_replacement_waiting_for_exit_ = false;
  connect_attempts_ = 0;
  launch_attempted_ = false;
  protocol_rejected_ = false;
  if (control_) {
    control_->Close();
  }
  ScheduleReconnect();
}

void CmuxTuiClient::OnSubscribed(CmuxTuiCommandResult result) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!result.ok) {
    RejectControlProtocol(result.error.empty()
                              ? "cmux-tui protocol " +
                                    base::NumberToString(
                                        kCmuxTuiProtocolVersion) +
                                    " rejected subscribe"
                              : std::move(result.error));
    return;
  }
  RefreshWorkspaces();
  RefreshTerminals();
}

void CmuxTuiClient::MaybeSetReady() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!ready_ && workspace_snapshot_ready_ && terminal_snapshot_ready_) {
    SetReady(true, std::string());
  }
}

void CmuxTuiClient::RefreshWorkspaces() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!control_) {
    return;
  }
  if (workspace_refresh_in_flight_) {
    workspace_refresh_pending_ = true;
    return;
  }
  workspace_refresh_in_flight_ = true;
  base::DictValue request;
  request.Set("cmd", "list-workspaces");
  Request(std::move(request), base::BindOnce(&CmuxTuiClient::OnWorkspacesListed,
                                             weak_factory_.GetWeakPtr()));
}

void CmuxTuiClient::OnWorkspacesListed(CmuxTuiCommandResult result) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  workspace_refresh_in_flight_ = false;
  const bool refresh_again = workspace_refresh_pending_;
  workspace_refresh_pending_ = false;
  const std::optional<CmuxTuiWorkspaceSnapshot> snapshot =
      result.ok ? ParseWorkspaceSnapshot(result.data) : std::nullopt;
  // A lifecycle event arrived after this request began. Its direct projection
  // is already visible; discard this possibly pre-event response and fetch a
  // clean barrier so the GUI never time-travels backward for one frame.
  if (refresh_again) {
    RefreshWorkspaces();
    return;
  }
  if (!snapshot) {
    const std::string error =
        !result.error.empty()
            ? result.error
            : "cmux-tui returned an invalid workspace registry snapshot";
    if (!ready_) {
      RejectControlProtocol(error);
      return;
    }
    LOG(ERROR) << "cmux-tui: failed to refresh workspaces: " << error;
  } else {
    if (latest_workspace_snapshot_ &&
        latest_workspace_snapshot_->revision > snapshot->revision) {
      RefreshWorkspaces();
      return;
    }
    CmuxTuiWorkspaceSnapshot authoritative = *snapshot;
    authoritative.provisional = false;
    latest_workspace_snapshot_ = authoritative;
    workspace_snapshot_ready_ = true;
    base::WeakPtr<CmuxTuiClient> alive = weak_factory_.GetWeakPtr();
    for (Observer& observer : observers_) {
      observer.OnCmuxTuiWorkspaceSnapshot(authoritative);
      if (!alive) {
        return;
      }
    }
    MaybeSetReady();
  }
}

void CmuxTuiClient::RefreshTerminals() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!control_) {
    return;
  }
  if (terminal_refresh_in_flight_) {
    terminal_refresh_pending_ = true;
    return;
  }
  terminal_refresh_in_flight_ = true;

  if (!latest_terminal_snapshot_ || terminal_event_registry_id_.empty() ||
      terminal_event_generation_.empty()) {
    RequestTerminalSnapshot();
    return;
  }

  const std::string registry_id = terminal_event_registry_id_;
  const std::string generation = terminal_event_generation_;
  const uint64_t after_revision = terminal_event_revision_;
  base::DictValue request;
  request.Set("cmd", "terminal-events");
  request.Set("after_revision", ProtocolSurfaceId(after_revision));
  Request(std::move(request),
          base::BindOnce(&CmuxTuiClient::OnTerminalEventsListed,
                         weak_factory_.GetWeakPtr(), registry_id, generation,
                         after_revision));
}

void CmuxTuiClient::OnTerminalEventsListed(std::string registry_id,
                                           std::string generation,
                                           uint64_t after_revision,
                                           CmuxTuiCommandResult result) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  std::optional<ParsedTerminalEventBatch> batch =
      result.ok ? ParseTerminalEventBatch(result.data, after_revision)
                : std::nullopt;
  if (batch && batch->registry_id == registry_id &&
      batch->generation == generation &&
      terminal_event_registry_id_ == registry_id &&
      terminal_event_generation_ == generation &&
      terminal_event_revision_ == after_revision) {
    for (const CmuxTuiTerminalRegistryEvent& event : batch->events) {
      base::WeakPtr<CmuxTuiClient> alive = weak_factory_.GetWeakPtr();
      for (Observer& observer : observers_) {
        observer.OnCmuxTuiTerminalEvent(event);
        if (!alive) {
          return;
        }
      }
    }
    terminal_event_revision_ = batch->revision;
  } else if (!result.ok) {
    VLOG(1) << "cmux-tui: terminal event refresh fell back to snapshot: "
            << result.error;
  }
  // Deltas improve latency but never establish a barrier. Snapshot after the
  // event query even when the batch was empty, invalid, or crossed an epoch.
  RequestTerminalSnapshot();
}

void CmuxTuiClient::RequestTerminalSnapshot() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  base::DictValue request;
  request.Set("cmd", "list-terminals");
  Request(std::move(request), base::BindOnce(&CmuxTuiClient::OnTerminalsListed,
                                             weak_factory_.GetWeakPtr()));
}

void CmuxTuiClient::OnTerminalsListed(CmuxTuiCommandResult result) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  terminal_refresh_in_flight_ = false;
  const bool refresh_again = terminal_refresh_pending_;
  terminal_refresh_pending_ = false;
  std::optional<CmuxTerminalPlacementSnapshot> snapshot =
      result.ok ? ParseTerminalSnapshot(result.data) : std::nullopt;

  // A later invalidation arrived while either the delta query or snapshot was
  // in flight. Discard this response and start a new delta+snapshot barrier.
  if (refresh_again) {
    RefreshTerminals();
    return;
  }
  if (!snapshot) {
    const std::string error =
        !result.error.empty()
            ? result.error
            : "cmux-tui returned an invalid terminal registry snapshot";
    if (!ready_) {
      RejectControlProtocol(error);
      return;
    }
    LOG(ERROR) << "cmux-tui: failed to refresh terminals: " << error;
    return;
  }

  if (terminal_event_registry_id_ == snapshot->registry_id &&
      terminal_event_generation_ == snapshot->generation &&
      terminal_event_revision_ > snapshot->terminal_revision) {
    RefreshTerminals();
    return;
  }
  const CmuxTuiRegistryFenceDecision fence = FenceCmuxTuiRegistrySnapshot(
      latest_terminal_snapshot_.has_value(),
      latest_terminal_snapshot_
          ? std::string_view(latest_terminal_snapshot_->registry_id)
          : std::string_view(),
      latest_terminal_snapshot_
          ? std::string_view(latest_terminal_snapshot_->generation)
          : std::string_view(),
      latest_terminal_snapshot_ ? latest_terminal_snapshot_->terminal_revision
                                : 0,
      snapshot->registry_id, snapshot->generation, snapshot->terminal_revision);
  if (fence == CmuxTuiRegistryFenceDecision::kInvalid) {
    if (!ready_) {
      RejectControlProtocol("cmux-tui returned an invalid terminal epoch");
    }
    return;
  }
  if (fence == CmuxTuiRegistryFenceDecision::kRefetch) {
    RefreshTerminals();
    return;
  }

  terminal_event_registry_id_ = snapshot->registry_id;
  terminal_event_generation_ = snapshot->generation;
  terminal_event_revision_ = snapshot->terminal_revision;
  terminal_snapshot_ready_ = true;
  if (fence == CmuxTuiRegistryFenceDecision::kAccept) {
    latest_terminal_snapshot_ = *snapshot;
    base::WeakPtr<CmuxTuiClient> alive = weak_factory_.GetWeakPtr();
    for (Observer& observer : observers_) {
      observer.OnCmuxTuiTerminalSnapshot(*snapshot);
      if (!alive) {
        return;
      }
    }
  }
  MaybeSetReady();
}

void CmuxTuiClient::LaunchServer() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  launch_attempted_ = true;
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE,
      {base::MayBlock(), base::TaskPriority::USER_VISIBLE,
       base::TaskShutdownBehavior::CONTINUE_ON_SHUTDOWN},
      base::BindOnce(&LaunchHeadlessServer, options_),
      base::BindOnce(
          [](base::WeakPtr<CmuxTuiClient> client, LaunchResult result) {
            if (client) {
              client->OnServerLaunched(result.launched,
                                       std::move(result.error));
            }
          },
          weak_factory_.GetWeakPtr()));
}

void CmuxTuiClient::OnServerLaunched(bool launched, std::string error) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!launched) {
    SetReady(false, std::move(error));
    return;
  }
  connect_attempts_ = 0;
  ScheduleReconnect();
}

void CmuxTuiClient::ScheduleReconnect(bool live_disconnect) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (reconnect_scheduled_) {
    return;
  }
  reconnect_scheduled_ = true;
  const base::TimeDelta delay =
      live_disconnect ? kLiveReconnectDelay : kInitialReconnectDelay;
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
      FROM_HERE,
      base::BindOnce(&CmuxTuiClient::ConnectControl,
                     weak_factory_.GetWeakPtr()),
      delay);
}

void CmuxTuiClient::SetReady(bool ready, std::string error) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  const bool changed = ready_ != ready;
  ready_ = ready;
  if (ready) {
    ever_ready_ = true;
    launch_attempted_ = false;
  }
  if (changed || !error.empty()) {
    base::WeakPtr<CmuxTuiClient> alive = weak_factory_.GetWeakPtr();
    for (Observer& observer : observers_) {
      observer.OnCmuxTuiConnectionChanged(ready, error);
      if (!alive) {
        return;
      }
    }
  }
  if (start_callback_) {
    std::move(start_callback_).Run(ready, error);
  }
}

void CmuxTuiClient::Request(base::DictValue request, CommandCallback callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!control_) {
    CmuxTuiCommandResult result;
    result.error = "cmux-tui control connection is unavailable";
    if (callback) {
      std::move(callback).Run(std::move(result));
    }
    return;
  }
  control_->SendCommand(std::move(request), std::move(callback));
}

void CmuxTuiClient::CreateSurface(std::string command,
                                  std::string name,
                                  std::string workspace_key,
                                  TerminalHostId terminal_id,
                                  std::string mutation_id,
                                  uint16_t cols,
                                  uint16_t rows,
                                  SurfaceCallback callback) {
  CmuxTuiTerminalMutation mutation;
  mutation.mutation_id = std::move(mutation_id);
  CreateSurface(std::move(command), std::move(name), std::move(workspace_key),
                terminal_id, std::move(mutation), cols, rows,
                std::move(callback));
}

void CmuxTuiClient::CreateSurface(std::string command,
                                  std::string name,
                                  std::string workspace_key,
                                  TerminalHostId terminal_id,
                                  CmuxTuiTerminalMutation mutation,
                                  uint16_t cols,
                                  uint16_t rows,
                                  SurfaceCallback callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!callback) {
    return;
  }
  if (!IsValidTerminalHostUuidV4(terminal_id)) {
    std::move(callback).Run(std::nullopt, "invalid_terminal_id");
    return;
  }
  if (std::optional<std::string> error = ValidateTerminalMutation(mutation)) {
    std::move(callback).Run(std::nullopt, *error);
    return;
  }
  if (workspace_key.empty()) {
    std::move(callback).Run(std::nullopt, "missing_canonical_workspace_key");
    return;
  }
  base::DictValue request;
  request.Set("cmd", "create-terminal");
  request.Set("key", std::move(workspace_key));
  if (!command.empty()) {
    request.Set("command", std::move(command));
  }
  if (!name.empty()) {
    request.Set("name", std::move(name));
  }
  request.Set("terminal_id", EncodeTerminalHostId(terminal_id));
  SetTerminalMutation(&request, std::move(mutation),
                      options_.origin.empty() ? "chrome-gui" : options_.origin);
  request.Set("cols", static_cast<int>(std::max<uint16_t>(cols, 1)));
  request.Set("rows", static_cast<int>(std::max<uint16_t>(rows, 1)));
  Request(
      std::move(request),
      base::BindOnce(
          [](TerminalHostId expected_terminal_id, SurfaceCallback callback,
             CmuxTuiCommandResult result) {
            if (!callback) {
              return;
            }
            std::optional<CmuxTerminalBinding> binding =
                result.ok ? ParseTerminalBinding(result.data) : std::nullopt;
            if (!binding) {
              std::move(callback).Run(
                  std::nullopt,
                  !result.error.empty()
                      ? result.error
                      : "cmux-tui did not return a valid durable terminal "
                        "binding");
              return;
            }
            if (binding->terminal_id != expected_terminal_id) {
              std::move(callback).Run(
                  std::nullopt, "cmux-tui returned a different terminal_id");
              return;
            }
            std::move(callback).Run(std::move(binding), std::string());
          },
          terminal_id, std::move(callback)));
}

void CmuxTuiClient::ResolveTerminal(TerminalHostId terminal_id,
                                    SurfaceCallback callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!callback) {
    return;
  }
  ResolveTerminalRecord(
      terminal_id,
      base::BindOnce(
          [](SurfaceCallback callback,
             std::optional<CmuxTuiTerminalResolution> resolution,
             const std::string& error) {
            if (!resolution) {
              std::move(callback).Run(std::nullopt, error);
              return;
            }
            if (!resolution->binding) {
              std::move(callback).Run(
                  std::nullopt,
                  "cmux-tui terminal has no running surface binding");
              return;
            }
            std::move(callback).Run(std::move(resolution->binding),
                                    std::string());
          },
          std::move(callback)));
}

void CmuxTuiClient::ResolveTerminalRecord(TerminalHostId terminal_id,
                                          TerminalResolutionCallback callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!callback) {
    return;
  }
  if (!IsValidTerminalHostUuidV4(terminal_id)) {
    std::move(callback).Run(std::nullopt, "invalid_terminal_id");
    return;
  }
  base::DictValue request;
  request.Set("cmd", "resolve-terminal");
  request.Set("terminal_id", EncodeTerminalHostId(terminal_id));
  Request(
      std::move(request),
      base::BindOnce(
          [](TerminalHostId expected_terminal_id,
             TerminalResolutionCallback callback, CmuxTuiCommandResult result) {
            if (!result.ok) {
              std::move(callback).Run(std::nullopt,
                                      result.error.empty()
                                          ? "cmux-tui terminal resolve failed"
                                          : result.error);
              return;
            }
            std::optional<CmuxTuiTerminalResolution> resolution =
                ParseTerminalResolution(result.data);
            TerminalHostId parsed_id;
            if (!resolution ||
                !DecodeTerminalHostUuidV4(resolution->terminal.terminal_id,
                                          &parsed_id) ||
                parsed_id != expected_terminal_id) {
              std::move(callback).Run(
                  std::nullopt,
                  "cmux-tui returned an invalid terminal resolution");
              return;
            }
            std::move(callback).Run(std::move(resolution), std::string());
          },
          terminal_id, std::move(callback)));
}

void CmuxTuiClient::CloseTerminal(
    TerminalHostId terminal_id,
    std::optional<TerminalHostIncarnation> terminal_incarnation,
    std::string mutation_id,
    CommandCallback callback) {
  CmuxTuiTerminalMutation mutation;
  mutation.mutation_id = std::move(mutation_id);
  CloseTerminal(terminal_id, terminal_incarnation, std::move(mutation),
                std::move(callback));
}

void CmuxTuiClient::CloseTerminal(
    TerminalHostId terminal_id,
    std::optional<TerminalHostIncarnation> terminal_incarnation,
    CmuxTuiTerminalMutation mutation,
    CommandCallback callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  const std::optional<std::string> mutation_error =
      ValidateTerminalMutation(mutation);
  if (!IsValidTerminalHostUuidV4(terminal_id) ||
      (terminal_incarnation &&
       !IsValidTerminalHostUuidV4(*terminal_incarnation)) ||
      mutation_error) {
    if (callback) {
      CmuxTuiCommandResult result;
      if (!IsValidTerminalHostUuidV4(terminal_id)) {
        result.error = "invalid_terminal_id";
      } else if (terminal_incarnation) {
        result.error = "invalid_terminal_incarnation";
      } else {
        result.error = *mutation_error;
      }
      std::move(callback).Run(std::move(result));
    }
    return;
  }
  base::DictValue request;
  request.Set("cmd", "close-terminal");
  request.Set("terminal_id", EncodeTerminalHostId(terminal_id));
  if (terminal_incarnation) {
    request.Set("terminal_incarnation",
                EncodeTerminalHostId(*terminal_incarnation));
  }
  SetTerminalMutation(&request, std::move(mutation),
                      options_.origin.empty() ? "chrome-gui" : options_.origin);
  Request(std::move(request), std::move(callback));
}

void CmuxTuiClient::MoveTerminal(
    TerminalHostId terminal_id,
    std::optional<TerminalHostIncarnation> terminal_incarnation,
    std::string workspace_key,
    CmuxTuiTerminalMutation mutation,
    TerminalResolutionCallback callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!callback) {
    return;
  }
  const std::optional<std::string> mutation_error =
      ValidateTerminalMutation(mutation);
  if (!IsValidTerminalHostUuidV4(terminal_id)) {
    std::move(callback).Run(std::nullopt, "invalid_terminal_id");
    return;
  }
  if (terminal_incarnation &&
      !IsValidTerminalHostUuidV4(*terminal_incarnation)) {
    std::move(callback).Run(std::nullopt, "invalid_terminal_incarnation");
    return;
  }
  if (workspace_key.empty()) {
    std::move(callback).Run(std::nullopt, "missing_canonical_workspace_key");
    return;
  }
  if (mutation_error) {
    std::move(callback).Run(std::nullopt, *mutation_error);
    return;
  }
  base::DictValue request;
  request.Set("cmd", "move-terminal");
  request.Set("terminal_id", EncodeTerminalHostId(terminal_id));
  if (terminal_incarnation) {
    request.Set("terminal_incarnation",
                EncodeTerminalHostId(*terminal_incarnation));
  }
  request.Set("workspace_key", std::move(workspace_key));
  SetTerminalMutation(&request, std::move(mutation),
                      options_.origin.empty() ? "chrome-gui" : options_.origin);
  Request(
      std::move(request),
      base::BindOnce(
          [](TerminalHostId expected_terminal_id,
             TerminalResolutionCallback callback, CmuxTuiCommandResult result) {
            if (!result.ok) {
              std::move(callback).Run(std::nullopt,
                                      result.error.empty()
                                          ? "cmux-tui terminal move failed"
                                          : result.error);
              return;
            }
            std::optional<CmuxTuiTerminalResolution> resolution =
                ParseTerminalResolution(result.data);
            TerminalHostId parsed_id;
            if (!resolution ||
                !DecodeTerminalHostUuidV4(resolution->terminal.terminal_id,
                                          &parsed_id) ||
                parsed_id != expected_terminal_id) {
              std::move(callback).Run(
                  std::nullopt,
                  "cmux-tui returned an invalid terminal move result");
              return;
            }
            std::move(callback).Run(std::move(resolution), std::string());
          },
          terminal_id, std::move(callback)));
}

void CmuxTuiClient::CreateWorkspace(std::string name,
                                    std::string key,
                                    CmuxTuiWorkspaceMutation mutation,
                                    CommandCallback callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!IsCmuxUuid(key)) {
    if (callback) {
      CmuxTuiCommandResult result;
      result.error = "invalid_workspace_key";
      std::move(callback).Run(std::move(result));
    }
    return;
  }
  if (const std::optional<std::string> error =
          ValidateWorkspaceMutation(mutation)) {
    if (callback) {
      CmuxTuiCommandResult result;
      result.error = *error;
      std::move(callback).Run(std::move(result));
    }
    return;
  }
  base::DictValue request;
  request.Set("cmd", "create-workspace");
  request.Set("name", std::move(name));
  request.Set("key", std::move(key));
  SetWorkspaceMutation(&request, std::move(mutation));
  Request(std::move(request), std::move(callback));
}

void CmuxTuiClient::RenameWorkspace(std::string key,
                                    std::string name,
                                    CmuxTuiWorkspaceMutation mutation,
                                    CommandCallback callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (const std::optional<std::string> error =
          ValidateWorkspaceMutation(mutation)) {
    if (callback) {
      CmuxTuiCommandResult result;
      result.error = *error;
      std::move(callback).Run(std::move(result));
    }
    return;
  }
  base::DictValue request;
  request.Set("cmd", "rename-workspace");
  request.Set("key", std::move(key));
  request.Set("name", std::move(name));
  SetWorkspaceMutation(&request, std::move(mutation));
  Request(std::move(request), std::move(callback));
}

void CmuxTuiClient::MoveWorkspace(std::string key,
                                  size_t index,
                                  CmuxTuiWorkspaceMutation mutation,
                                  CommandCallback callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (const std::optional<std::string> error =
          ValidateWorkspaceMutation(mutation)) {
    if (callback) {
      CmuxTuiCommandResult result;
      result.error = *error;
      std::move(callback).Run(std::move(result));
    }
    return;
  }
  base::DictValue request;
  request.Set("cmd", "move-workspace");
  request.Set("key", std::move(key));
  request.Set("index", ProtocolSurfaceId(index));
  SetWorkspaceMutation(&request, std::move(mutation));
  Request(std::move(request), std::move(callback));
}

void CmuxTuiClient::CloseWorkspace(std::string key,
                                   CmuxTuiWorkspaceMutation mutation,
                                   CommandCallback callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (const std::optional<std::string> error =
          ValidateWorkspaceMutation(mutation)) {
    if (callback) {
      CmuxTuiCommandResult result;
      result.error = *error;
      std::move(callback).Run(std::move(result));
    }
    return;
  }
  base::DictValue request;
  request.Set("cmd", "close-workspace");
  request.Set("key", std::move(key));
  SetWorkspaceMutation(&request, std::move(mutation));
  Request(std::move(request), std::move(callback));
}

void CmuxTuiClient::RefreshWorkspaceRegistry() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  RefreshWorkspaces();
}

void CmuxTuiClient::RefreshTerminalRegistry() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  RefreshTerminals();
}

void CmuxTuiClient::GetFrontendProjection(FrontendProjectionCallback callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (options_.frontend.empty() || options_.frontend_scope.empty() ||
      options_.projection_subject_key.empty()) {
    std::move(callback).Run(std::nullopt,
                            "cmux-tui frontend projection identity is empty");
    return;
  }
  base::DictValue request;
  request.Set("cmd", "get-frontend-projection");
  request.Set("frontend", options_.frontend);
  request.Set("scope", options_.frontend_scope);
  request.Set("subject_key", options_.projection_subject_key);
  Request(
      std::move(request),
      base::BindOnce(
          [](FrontendProjectionCallback callback, CmuxTuiCommandResult result) {
            std::optional<CmuxTuiFrontendProjection> projection =
                result.ok ? ParseFrontendProjection(result.data) : std::nullopt;
            if (!projection) {
              const std::string error =
                  !result.error.empty()
                      ? std::move(result.error)
                      : "cmux-tui returned an invalid frontend projection";
              std::move(callback).Run(std::nullopt, error);
              return;
            }
            std::move(callback).Run(std::move(projection), std::string());
          },
          std::move(callback)));
}

void CmuxTuiClient::PutFrontendProjection(uint64_t schema_version,
                                          uint64_t expected_projection_revision,
                                          base::DictValue projection,
                                          std::string mutation_id,
                                          FrontendProjectionCallback callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (options_.frontend.empty() || options_.frontend_scope.empty() ||
      options_.projection_subject_key.empty() || options_.origin.empty() ||
      mutation_id.empty()) {
    std::move(callback).Run(std::nullopt,
                            "cmux-tui frontend projection identity is empty");
    return;
  }
  base::DictValue request;
  request.Set("cmd", "put-frontend-projection");
  request.Set("frontend", options_.frontend);
  request.Set("scope", options_.frontend_scope);
  request.Set("subject_key", options_.projection_subject_key);
  request.Set("schema_version", ProtocolSurfaceId(schema_version));
  request.Set("expected_projection_revision",
              ProtocolSurfaceId(expected_projection_revision));
  request.Set("projection", std::move(projection));
  request.Set("origin", options_.origin);
  request.Set("mutation_id", std::move(mutation_id));
  Request(
      std::move(request),
      base::BindOnce(
          [](FrontendProjectionCallback callback, CmuxTuiCommandResult result) {
            std::optional<CmuxTuiFrontendProjection> projection =
                result.ok ? ParseFrontendProjection(result.data) : std::nullopt;
            if (!projection) {
              const std::string error =
                  !result.error.empty()
                      ? std::move(result.error)
                      : "cmux-tui returned an invalid frontend projection";
              std::move(callback).Run(std::nullopt, error);
              return;
            }
            std::move(callback).Run(std::move(projection), std::string());
          },
          std::move(callback)));
}

void CmuxTuiClient::SendBytes(CmuxTuiSurfaceId surface,
                              std::vector<uint8_t> bytes,
                              CommandCallback callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  base::DictValue request;
  request.Set("cmd", "send");
  request.Set("surface", ProtocolSurfaceId(surface));
  request.Set("bytes", base::Base64Encode(base::span(bytes)));
  Request(std::move(request), std::move(callback));
}

void CmuxTuiClient::ResizeSurface(CmuxTuiSurfaceId surface,
                                  uint16_t cols,
                                  uint16_t rows,
                                  CommandCallback callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  base::DictValue request;
  request.Set("cmd", "resize-surface");
  request.Set("surface", ProtocolSurfaceId(surface));
  request.Set("cols", static_cast<int>(std::max<uint16_t>(cols, 1)));
  request.Set("rows", static_cast<int>(std::max<uint16_t>(rows, 1)));
  Request(std::move(request), std::move(callback));
}

void CmuxTuiClient::CloseSurface(CmuxTuiSurfaceId surface,
                                 CommandCallback callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  base::DictValue request;
  request.Set("cmd", "close-surface");
  request.Set("surface", ProtocolSurfaceId(surface));
  Request(std::move(request), std::move(callback));
}

void CmuxTuiClient::MintTerminalRenderer(CmuxTuiSurfaceId surface,
                                         base::TimeDelta ttl,
                                         RendererConnectionCallback callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!callback) {
    return;
  }
  const int64_t ttl_microseconds = ttl.InMicroseconds();
  if (surface == 0 || surface > static_cast<uint64_t>(kMaxExactJsonInteger) ||
      ttl_microseconds <= 0 || ttl_microseconds % 1000 != 0) {
    std::move(callback).Run(
        std::nullopt,
        "terminal renderer requires a surface and integral positive TTL");
    return;
  }
  const uint64_t ttl_ms = static_cast<uint64_t>(ttl_microseconds / 1000);
  if (ttl_ms > kTerminalHostMaxRendererCapabilityTtlMs) {
    std::move(callback).Run(
        std::nullopt, "terminal renderer TTL must be between 1ms and 60000ms");
    return;
  }

  base::DictValue request;
  request.Set("cmd", "mint-terminal-renderer");
  request.Set("surface", ProtocolSurfaceId(surface));
  request.Set("ttl_ms", static_cast<int>(ttl_ms));
  Request(
      std::move(request),
      base::BindOnce(
          [](base::WeakPtr<CmuxTuiClient> client, uint64_t expected_ttl_ms,
             RendererConnectionCallback callback, CmuxTuiCommandResult result) {
            if (!client) {
              return;
            }
            if (!result.ok) {
              std::move(callback).Run(
                  std::nullopt,
                  result.error.empty()
                      ? "cmux-tui rejected terminal renderer grant"
                      : result.error);
              return;
            }
            const std::string* endpoint = result.data.FindString("endpoint");
            const std::string* terminal_id =
                result.data.FindString("terminal_id");
            const std::string* incarnation =
                result.data.FindString("incarnation");
            const std::string* token = result.data.FindString("token");
            const std::optional<uint64_t> rights =
                ReadUnsigned(result.data, "rights");
            const std::optional<uint64_t> response_ttl =
                ReadUnsigned(result.data, "ttl_ms");
            TerminalHostRendererGrant grant;
            const TerminalHostRendererGrantError grant_error =
                ValidateTerminalHostRendererGrant(
                    endpoint ? std::string_view(*endpoint) : std::string_view(),
                    terminal_id ? std::string_view(*terminal_id)
                                : std::string_view(),
                    incarnation ? std::string_view(*incarnation)
                                : std::string_view(),
                    token ? std::string_view(*token) : std::string_view(),
                    rights.value_or(0), response_ttl.value_or(0),
                    expected_ttl_ms, &grant);
            if (grant_error != TerminalHostRendererGrantError::kNone) {
              std::move(callback).Run(
                  std::nullopt,
                  std::string("cmux-tui returned an invalid renderer grant: ") +
                      TerminalHostRendererGrantErrorMessage(grant_error));
              return;
            }

            const uint64_t attempt = client->next_renderer_connect_attempt_++;
            auto cancellation =
                std::make_shared<TerminalHostConnectCancellation>();
            client->renderer_connect_cancellations_.emplace(
                attempt, base::BindOnce(
                             [](std::shared_ptr<TerminalHostConnectCancellation>
                                    cancellation) { cancellation->Cancel(); },
                             cancellation));
            const uint64_t timeout_ms =
                std::min<uint64_t>(expected_ttl_ms, 2000);
            base::ThreadPool::PostTaskAndReplyWithResult(
                FROM_HERE,
                {base::MayBlock(), base::TaskPriority::USER_VISIBLE,
                 base::TaskShutdownBehavior::CONTINUE_ON_SHUTDOWN},
                base::BindOnce(
                    &ConnectAndAuthenticateTerminalHost, std::move(grant),
                    std::chrono::milliseconds(timeout_ms), cancellation),
                base::BindOnce(
                    [](base::WeakPtr<CmuxTuiClient> client, uint64_t attempt,
                       uint32_t ttl_ms, RendererConnectionCallback callback,
                       TerminalHostConnectResult connect_result) {
                      if (!client) {
                        return;
                      }
                      client->renderer_connect_cancellations_.erase(attempt);
                      if (connect_result.error !=
                              TerminalHostConnectError::kNone ||
                          !connect_result.socket) {
                        std::move(callback).Run(
                            std::nullopt,
                            connect_result.message.empty()
                                ? "terminal-host authentication failed"
                                : connect_result.message);
                        return;
                      }
#if BUILDFLAG(IS_WIN)
                      std::move(callback).Run(
                          std::nullopt,
                          "direct terminal hosts are unsupported on Windows");
#else
                      TerminalHostAuthenticatedSocket authenticated =
                          std::move(*connect_result.socket);
                      base::ScopedFD descriptor(authenticated.TakeDescriptor());
                      if (!descriptor.is_valid()) {
                        std::move(callback).Run(
                            std::nullopt,
                            "terminal-host returned an invalid descriptor");
                        return;
                      }
                      CmuxTuiRendererConnection connection(
                          mojo::PlatformHandle(std::move(descriptor)),
                          authenticated.terminal_id(),
                          authenticated.incarnation(), authenticated.rights(),
                          authenticated.protocol_flags(), ttl_ms);
                      std::move(callback).Run(std::move(connection),
                                              std::string());
#endif
                    },
                    client, attempt, static_cast<uint32_t>(expected_ttl_ms),
                    std::move(callback)));
          },
          weak_factory_.GetWeakPtr(), ttl_ms, std::move(callback)));
}

std::unique_ptr<CmuxTuiAttachment> CmuxTuiClient::AttachSurface(
    CmuxTuiSurfaceId surface,
    uint16_t cols,
    uint16_t rows,
    EventCallback callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  auto attachment = std::unique_ptr<CmuxTuiAttachment>(new CmuxTuiAttachment(
      options_.socket_path, surface, cols, rows, options_.required_build_commit,
      options_.required_ghostty_commit, std::move(callback)));
  attachment->Start();
  return attachment;
}

CmuxTuiAttachment::CmuxTuiAttachment(base::FilePath socket_path,
                                     CmuxTuiSurfaceId surface,
                                     uint16_t cols,
                                     uint16_t rows,
                                     std::string required_build_commit,
                                     std::string required_ghostty_commit,
                                     CmuxTuiClient::EventCallback callback)
    : socket_path_(std::move(socket_path)),
      surface_(surface),
      initial_cols_(std::max<uint16_t>(cols, 1)),
      initial_rows_(std::max<uint16_t>(rows, 1)),
      required_build_commit_(std::move(required_build_commit)),
      required_ghostty_commit_(std::move(required_ghostty_commit)),
      callback_(std::move(callback)) {
  DETACH_FROM_SEQUENCE(sequence_checker_);
}

CmuxTuiAttachment::~CmuxTuiAttachment() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  weak_factory_.InvalidateWeakPtrs();
  connection_.reset();
}

void CmuxTuiAttachment::Start() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  connection_ = std::make_unique<CmuxTuiConnection>(
      socket_path_,
      base::BindRepeating(&CmuxTuiAttachment::OnMessage,
                          weak_factory_.GetWeakPtr()),
      base::BindRepeating(&CmuxTuiAttachment::OnDisconnected,
                          weak_factory_.GetWeakPtr()));
  connection_->Start(base::BindOnce(&CmuxTuiAttachment::OnConnected,
                                    weak_factory_.GetWeakPtr()));
}

void CmuxTuiAttachment::ResizeSurface(uint16_t cols,
                                      uint16_t rows,
                                      CmuxTuiClient::CommandCallback callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  cols = std::max<uint16_t>(cols, 1);
  rows = std::max<uint16_t>(rows, 1);
  if (!attached_) {
    if (pending_resize_callback_) {
      CmuxTuiCommandResult replaced;
      replaced.error = "cmux-tui viewer resize was superseded";
      std::move(pending_resize_callback_).Run(std::move(replaced));
    }
    pending_resize_cols_ = cols;
    pending_resize_rows_ = rows;
    pending_resize_callback_ = std::move(callback);
    return;
  }
  SendResize(cols, rows, std::move(callback));
}

void CmuxTuiAttachment::SendResize(uint16_t cols,
                                   uint16_t rows,
                                   CmuxTuiClient::CommandCallback callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  base::DictValue request;
  request.Set("cmd", "resize-surface");
  request.Set("surface", ProtocolSurfaceId(surface_));
  request.Set("cols", static_cast<int>(cols));
  request.Set("rows", static_cast<int>(rows));
  connection_->SendCommand(std::move(request), std::move(callback));
}

void CmuxTuiAttachment::OnConnected(bool connected, std::string error) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!connected) {
    EmitDisconnected(std::move(error));
    return;
  }
  base::DictValue identify;
  identify.Set("cmd", "identify");
  connection_->SendCommand(std::move(identify),
                           base::BindOnce(&CmuxTuiAttachment::VerifyIdentity,
                                          weak_factory_.GetWeakPtr()));
}

void CmuxTuiAttachment::VerifyIdentity(CmuxTuiCommandResult result) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  const std::string* app = result.data.FindString("app");
  const std::optional<uint64_t> protocol =
      ReadUnsigned(result.data, "protocol");
  const std::optional<uint64_t> pid = ReadUnsigned(result.data, "pid");
  const std::string* build_commit = result.data.FindString("build_commit");
  const std::string* ghostty_commit = result.data.FindString("ghostty_commit");
  const CmuxTuiIdentityError identity_error =
      result.ok && app && protocol && pid
          ? ValidateCmuxTuiIdentity(
                *app, *protocol, *pid,
                build_commit ? std::optional<std::string_view>(*build_commit)
                             : std::nullopt,
                required_build_commit_,
                ghostty_commit
                    ? std::optional<std::string_view>(*ghostty_commit)
                    : std::nullopt,
                required_ghostty_commit_)
          : CmuxTuiIdentityError::kInvalidEndpoint;
  if (identity_error != CmuxTuiIdentityError::kNone) {
    base::WeakPtr<CmuxTuiAttachment> alive = weak_factory_.GetWeakPtr();
    EmitDisconnected(!result.error.empty()
                         ? std::move(result.error)
                         : IdentityErrorMessage(identity_error,
                                                required_build_commit_,
                                                required_ghostty_commit_));
    if (alive && connection_) {
      connection_->Close();
    }
    return;
  }
  base::DictValue attach;
  attach.Set("cmd", "attach-surface");
  attach.Set("surface", ProtocolSurfaceId(surface_));
  attach.Set("cols", static_cast<int>(initial_cols_));
  attach.Set("rows", static_cast<int>(initial_rows_));
  connection_->SendCommand(std::move(attach),
                           base::BindOnce(&CmuxTuiAttachment::OnAttached,
                                          weak_factory_.GetWeakPtr()));
}

void CmuxTuiAttachment::OnAttached(CmuxTuiCommandResult result) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!result.ok) {
    base::WeakPtr<CmuxTuiAttachment> alive = weak_factory_.GetWeakPtr();
    const CmuxTuiEvent::Type type =
        base::StartsWith(result.error, "unknown surface ",
                         base::CompareCase::SENSITIVE)
            ? CmuxTuiEvent::Type::kSurfaceMissing
            : CmuxTuiEvent::Type::kDisconnected;
    EmitDisconnected(std::move(result.error), type);
    if (alive && connection_) {
      connection_->Close();
    }
    return;
  }
  attached_ = true;
  if (!saw_initial_state_) {
    base::WeakPtr<CmuxTuiAttachment> alive = weak_factory_.GetWeakPtr();
    EmitDisconnected("attach response arrived before vt-state");
    if (alive && connection_) {
      connection_->Close();
    }
    return;
  }
  if (pending_resize_cols_ != 0 && pending_resize_rows_ != 0) {
    const uint16_t cols = pending_resize_cols_;
    const uint16_t rows = pending_resize_rows_;
    pending_resize_cols_ = 0;
    pending_resize_rows_ = 0;
    SendResize(cols, rows, std::move(pending_resize_callback_));
  }
}

void CmuxTuiAttachment::OnDisconnected(std::string error) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  EmitDisconnected(std::move(error));
}

void CmuxTuiAttachment::OnMessage(base::Value message,
                                  std::optional<std::vector<uint8_t>> bytes,
                                  std::string decode_error) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  CmuxTuiEvent event = ParseEvent(std::move(message), std::move(bytes),
                                  std::move(decode_error), surface_);
  if (event.type == CmuxTuiEvent::Type::kVtState) {
    saw_initial_state_ = true;
  } else if ((event.type == CmuxTuiEvent::Type::kOutput ||
              event.type == CmuxTuiEvent::Type::kResized) &&
             !saw_initial_state_) {
    base::WeakPtr<CmuxTuiAttachment> alive = weak_factory_.GetWeakPtr();
    EmitDisconnected("attach stream emitted data before vt-state");
    if (alive && connection_) {
      connection_->Close();
    }
    return;
  }
  if (callback_) {
    callback_.Run(std::move(event));
  }
}

void CmuxTuiAttachment::EmitDisconnected(std::string error,
                                         CmuxTuiEvent::Type type) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (disconnected_emitted_) {
    return;
  }
  disconnected_emitted_ = true;
  if (pending_resize_callback_) {
    CmuxTuiCommandResult failed;
    failed.error = error;
    std::move(pending_resize_callback_).Run(std::move(failed));
  }
  if (callback_) {
    CmuxTuiEvent event;
    event.type = type;
    event.name = "disconnected";
    event.surface = surface_;
    event.error = std::move(error);
    callback_.Run(std::move(event));
  }
}

}  // namespace cmux
