// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef CHROME_BROWSER_CMUX_TERM_CMUX_TUI_CLIENT_H_
#define CHROME_BROWSER_CMUX_TERM_CMUX_TUI_CLIENT_H_

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/memory/ref_counted.h"
#include "base/memory/weak_ptr.h"
#include "base/observer_list.h"
#include "base/observer_list_types.h"
#include "base/sequence_checker.h"
#include "base/time/time.h"
#include "base/values.h"
#include "chrome/browser/cmux_term/cmux_terminal_placement.h"
#include "chrome/browser/cmux_term/cmux_terminal_recovery.h"
#include "chrome/browser/cmux_term/cmux_workspace_projection.h"
#include "chrome/services/cmux_terminal_renderer/public/cpp/cmux_terminal_host_protocol.h"
#include "mojo/public/cpp/platform/platform_handle.h"

namespace cmux {

using CmuxTuiWorkspaceId = uint64_t;

struct CmuxTuiWorkspace {
  CmuxTuiWorkspace();
  CmuxTuiWorkspace(const CmuxTuiWorkspace&);
  CmuxTuiWorkspace& operator=(const CmuxTuiWorkspace&);
  CmuxTuiWorkspace(CmuxTuiWorkspace&&);
  CmuxTuiWorkspace& operator=(CmuxTuiWorkspace&&);
  ~CmuxTuiWorkspace();

  CmuxTuiWorkspaceId id = 0;
  std::string key;
  std::string name;
};

struct CmuxTuiWorkspaceSnapshot {
  CmuxTuiWorkspaceSnapshot();
  CmuxTuiWorkspaceSnapshot(const CmuxTuiWorkspaceSnapshot&);
  CmuxTuiWorkspaceSnapshot& operator=(const CmuxTuiWorkspaceSnapshot&);
  CmuxTuiWorkspaceSnapshot(CmuxTuiWorkspaceSnapshot&&);
  CmuxTuiWorkspaceSnapshot& operator=(CmuxTuiWorkspaceSnapshot&&);
  ~CmuxTuiWorkspaceSnapshot();

  uint64_t revision = 0;
  std::string registry_id;
  std::string generation;
  // True only for a lifecycle delta projected onto the latest snapshot before
  // the confirming list-workspaces barrier arrives.
  bool provisional = false;
  // Ephemeral owner-mux selection. Workspace identity is stable; terminal
  // identity is present only when the active TUI tab is a canonical terminal.
  // Chromium consumes this as an invalidation-driven focus projection, never
  // as workspace/terminal lifecycle authority.
  std::string active_workspace_key;
  std::string active_terminal_id;
  std::vector<CmuxTuiWorkspace> workspaces;
};

// Exactly-once workspace commands use `(origin, mutation_id)` as their
// durable replay key. The generation/revision preconditions fence a new
// mutation to the authoritative snapshot observed by the GUI. A retry keeps
// the same replay identity even when it refreshes those snapshot fences.
struct CmuxTuiWorkspaceMutation {
  CmuxTuiWorkspaceMutation();
  CmuxTuiWorkspaceMutation(const CmuxTuiWorkspaceMutation&);
  CmuxTuiWorkspaceMutation& operator=(const CmuxTuiWorkspaceMutation&);
  CmuxTuiWorkspaceMutation(CmuxTuiWorkspaceMutation&&);
  CmuxTuiWorkspaceMutation& operator=(CmuxTuiWorkspaceMutation&&);
  ~CmuxTuiWorkspaceMutation();

  std::string origin;
  std::string mutation_id;
  std::optional<std::string> expected_generation;
  std::optional<uint64_t> expected_revision;
};

// Exactly-once terminal commands use `(origin, mutation_id)` as their durable
// replay key. Optional epoch/revision preconditions prevent a fresh mutation
// from being applied to state the GUI has never observed; retrying the same
// mutation remains safe because cmux-tui checks its replay table first.
struct CmuxTuiTerminalMutation {
  CmuxTuiTerminalMutation();
  CmuxTuiTerminalMutation(const CmuxTuiTerminalMutation&);
  CmuxTuiTerminalMutation& operator=(const CmuxTuiTerminalMutation&);
  CmuxTuiTerminalMutation(CmuxTuiTerminalMutation&&);
  CmuxTuiTerminalMutation& operator=(CmuxTuiTerminalMutation&&);
  ~CmuxTuiTerminalMutation();

  std::string mutation_id;
  std::optional<std::string> expected_generation;
  std::optional<uint64_t> expected_terminal_revision;
};

// One contiguous row from `terminal-events`, already fenced to the snapshot
// epoch and validated as the exact next terminal revision.
struct CmuxTuiTerminalRegistryEvent {
  CmuxTuiTerminalRegistryEvent();
  CmuxTuiTerminalRegistryEvent(const CmuxTuiTerminalRegistryEvent&);
  CmuxTuiTerminalRegistryEvent& operator=(const CmuxTuiTerminalRegistryEvent&);
  CmuxTuiTerminalRegistryEvent(CmuxTuiTerminalRegistryEvent&&);
  CmuxTuiTerminalRegistryEvent& operator=(CmuxTuiTerminalRegistryEvent&&);
  ~CmuxTuiTerminalRegistryEvent();

  CmuxTerminalPlacementEvent placement;
  std::string kind;
  std::string origin;
  std::string mutation_id;
};

// Typed result shared by resolve-terminal and move-terminal. Pending/exited
// terminals intentionally have no runtime binding but still return canonical
// placement, so callers never mistake a missing daemon-local surface for an
// unknown durable terminal.
struct CmuxTuiTerminalResolution {
  CmuxTuiTerminalResolution();
  CmuxTuiTerminalResolution(const CmuxTuiTerminalResolution&);
  CmuxTuiTerminalResolution& operator=(const CmuxTuiTerminalResolution&);
  CmuxTuiTerminalResolution(CmuxTuiTerminalResolution&&);
  CmuxTuiTerminalResolution& operator=(CmuxTuiTerminalResolution&&);
  ~CmuxTuiTerminalResolution();

  std::string registry_id;
  std::string generation;
  uint64_t terminal_revision = 0;
  CmuxCanonicalTerminalPlacement terminal;
  std::optional<CmuxTerminalBinding> binding;
  bool changed = false;
  bool replayed = false;
};

struct CmuxTuiFrontendProjection {
  CmuxTuiFrontendProjection();
  CmuxTuiFrontendProjection(const CmuxTuiFrontendProjection&) = delete;
  CmuxTuiFrontendProjection& operator=(const CmuxTuiFrontendProjection&) =
      delete;
  CmuxTuiFrontendProjection(CmuxTuiFrontendProjection&&);
  CmuxTuiFrontendProjection& operator=(CmuxTuiFrontendProjection&&);
  ~CmuxTuiFrontendProjection();

  uint64_t revision = 0;
  uint64_t schema_version = 0;
  base::DictValue projection;
};

struct CmuxTuiColors {
  CmuxTuiColors();
  CmuxTuiColors(const CmuxTuiColors&);
  CmuxTuiColors& operator=(const CmuxTuiColors&);
  CmuxTuiColors(CmuxTuiColors&&);
  CmuxTuiColors& operator=(CmuxTuiColors&&);
  ~CmuxTuiColors();

  std::optional<std::string> foreground;
  std::optional<std::string> background;
  std::optional<std::string> cursor;
  std::optional<std::string> selection_background;
  std::optional<std::string> selection_foreground;
  // Only OSC 4 palette entries authored by the PTY. Missing entries retain
  // the frontend Ghostty config instead of adopting cmux-tui's parser theme.
  std::map<uint16_t, std::string> palette;
  std::optional<std::string> cursor_style;
  std::optional<bool> cursor_blink;
};

struct CmuxTuiEvent {
  enum class Type {
    kVtState,
    kOutput,
    kResized,
    kColorsChanged,
    kDetached,
    kOverflow,
    kTitleChanged,
    kSurfaceExited,
    kSurfaceMissing,
    kTreeChanged,
    kTerminalRegistryChanged,
    kDisconnected,
    kUnknown,
  };

  CmuxTuiEvent();
  CmuxTuiEvent(const CmuxTuiEvent&);
  CmuxTuiEvent& operator=(const CmuxTuiEvent&);
  CmuxTuiEvent(CmuxTuiEvent&&);
  CmuxTuiEvent& operator=(CmuxTuiEvent&&);
  ~CmuxTuiEvent();

  Type type = Type::kUnknown;
  std::string name;
  CmuxTuiSurfaceId surface = 0;
  uint16_t cols = 0;
  uint16_t rows = 0;
  std::vector<uint8_t> bytes;
  std::optional<CmuxTuiColors> colors;
  std::string title;
  std::string error;
  std::optional<CmuxTuiWorkspace> workspace;
  std::optional<size_t> workspace_index;
  std::optional<uint64_t> workspace_revision;
  std::optional<uint64_t> terminal_revision;
  std::string registry_id;
  std::string generation;
  std::string origin;
  std::string mutation_id;
  std::string frontend;
  std::string frontend_scope;
  std::string projection_subject_key;
  std::optional<uint64_t> projection_revision;
};

struct CmuxTuiCommandResult {
  CmuxTuiCommandResult();
  CmuxTuiCommandResult(const CmuxTuiCommandResult&) = delete;
  CmuxTuiCommandResult& operator=(const CmuxTuiCommandResult&) = delete;
  CmuxTuiCommandResult(CmuxTuiCommandResult&&);
  CmuxTuiCommandResult& operator=(CmuxTuiCommandResult&&);
  ~CmuxTuiCommandResult();

  bool ok = false;
  base::DictValue data;
  // Most commands return an object, but legacy list-clients returns its array
  // directly. Preserve that shape for safe one-release daemon handoff.
  std::optional<base::ListValue> list_data;
  std::string error;
};

// A renderer-scoped, already-authenticated terminal-host stream. The browser
// consumes the one-use credential and validates HostHello before constructing
// this object, so the utility receives no secret and starts by reading
// Snapshot. The handle remains nonblocking and close-on-exec.
struct CmuxTuiRendererConnection {
  CmuxTuiRendererConnection(mojo::PlatformHandle socket,
                            TerminalHostId terminal_id,
                            TerminalHostIncarnation incarnation,
                            TerminalHostCapabilityRights rights,
                            uint32_t protocol_flags,
                            uint32_t ttl_ms);
  CmuxTuiRendererConnection(const CmuxTuiRendererConnection&) = delete;
  CmuxTuiRendererConnection& operator=(const CmuxTuiRendererConnection&) =
      delete;
  CmuxTuiRendererConnection(CmuxTuiRendererConnection&&);
  CmuxTuiRendererConnection& operator=(CmuxTuiRendererConnection&&);
  ~CmuxTuiRendererConnection();

  mojo::PlatformHandle socket;
  TerminalHostId terminal_id;
  TerminalHostIncarnation incarnation;
  TerminalHostCapabilityRights rights;
  uint32_t protocol_flags;
  uint32_t ttl_ms;
};

class CmuxTuiAttachment;
class CmuxTuiConnection;

// Protocol-v7 client shared by the terminal backends in one browser window.
// A durable profile/privacy/window-group identity selects its cmux session and
// frontend-projection scope. The control connection owns request correlation
// and the subscription stream; every terminal attachment uses a separate
// connection so destroying a native frontend can detach its byte stream
// without terminating the TUI-owned PTY.
class CmuxTuiClient : public base::RefCountedThreadSafe<CmuxTuiClient> {
 public:
  static constexpr uint32_t kRequiredProtocol = 7;

  struct Options {
    Options();
    Options(const Options&);
    Options& operator=(const Options&);
    Options(Options&&);
    Options& operator=(Options&&);
    ~Options();

    std::string session = "cmux-browser";
    base::FilePath socket_path;
    base::FilePath binary_path;
    std::string required_build_commit;
    std::string required_ghostty_commit;
    std::string frontend;
    std::string frontend_scope;
    std::string projection_subject_key;
    std::string origin;
    bool spawn_if_missing = true;

    static Options FromEnvironment();
    static Options FromEnvironment(const CmuxFrontendIdentity& identity);
  };

  class Observer : public base::CheckedObserver {
   public:
    virtual void OnCmuxTuiConnectionChanged(bool /*connected*/,
                                            const std::string& /*error*/) {}
    virtual void OnCmuxTuiEvent(const CmuxTuiEvent& /*event*/) {}
    virtual void OnCmuxTuiWorkspaceSnapshot(
        const CmuxTuiWorkspaceSnapshot& /*snapshot*/) {}
    virtual void OnCmuxTuiTerminalEvent(
        const CmuxTuiTerminalRegistryEvent& /*event*/) {}
    virtual void OnCmuxTuiTerminalSnapshot(
        const CmuxTerminalPlacementSnapshot& /*snapshot*/) {}
  };

  using ReadyCallback =
      base::OnceCallback<void(bool ready, const std::string& error)>;
  using SurfaceCallback =
      base::OnceCallback<void(std::optional<CmuxTerminalBinding> binding,
                              const std::string& error)>;
  using CommandCallback = base::OnceCallback<void(CmuxTuiCommandResult)>;
  using TerminalResolutionCallback = base::OnceCallback<void(
      std::optional<CmuxTuiTerminalResolution> resolution,
      const std::string& error)>;
  using FrontendProjectionCallback =
      base::OnceCallback<void(std::optional<CmuxTuiFrontendProjection>,
                              const std::string& error)>;
  using RendererConnectionCallback = base::OnceCallback<void(
      std::optional<CmuxTuiRendererConnection> connection,
      const std::string& error)>;
  using EventCallback = base::RepeatingCallback<void(CmuxTuiEvent)>;

  explicit CmuxTuiClient(Options options);
  CmuxTuiClient(const CmuxTuiClient&) = delete;
  CmuxTuiClient& operator=(const CmuxTuiClient&) = delete;

  void Start(ReadyCallback callback);
  bool ready() const;
  uint64_t server_generation() const;
  const Options& options() const { return options_; }

  void AddObserver(Observer* observer);
  void RemoveObserver(Observer* observer);

  // Creates inside a canonical workspace with a client-reserved identity.
  // Browser-managed terminals deliberately never fall back to new-tab/run:
  // those commands cannot provide exactly-once terminal identity.
  void CreateSurface(std::string command,
                     std::string name,
                     std::string workspace_key,
                     TerminalHostId terminal_id,
                     std::string mutation_id,
                     uint16_t cols,
                     uint16_t rows,
                     SurfaceCallback callback);
  void CreateSurface(std::string command,
                     std::string name,
                     std::string workspace_key,
                     TerminalHostId terminal_id,
                     CmuxTuiTerminalMutation mutation,
                     uint16_t cols,
                     uint16_t rows,
                     SurfaceCallback callback);
  // Resolves a stable process identity into the current daemon generation's
  // routing surface without creating a shell.
  void ResolveTerminal(TerminalHostId terminal_id, SurfaceCallback callback);
  void ResolveTerminalRecord(TerminalHostId terminal_id,
                             TerminalResolutionCallback callback);
  // Atomically verifies the incarnation and terminates/removes that exact
  // terminal. Callers must never fall back to close-surface after learning a
  // stable identity.
  void CloseTerminal(
      TerminalHostId terminal_id,
      std::optional<TerminalHostIncarnation> terminal_incarnation,
      std::string mutation_id,
      CommandCallback callback = CommandCallback());
  void CloseTerminal(
      TerminalHostId terminal_id,
      std::optional<TerminalHostIncarnation> terminal_incarnation,
      CmuxTuiTerminalMutation mutation,
      CommandCallback callback = CommandCallback());
  // Moves a stable terminal without changing the daemon's active TUI focus.
  // A replay can report a workspace different from `workspace_key` when a
  // newer move already won; the typed canonical result is therefore returned
  // rather than assuming the requested destination.
  void MoveTerminal(TerminalHostId terminal_id,
                    std::optional<TerminalHostIncarnation> terminal_incarnation,
                    std::string workspace_key,
                    CmuxTuiTerminalMutation mutation,
                    TerminalResolutionCallback callback);
  void CreateWorkspace(std::string name,
                       std::string key,
                       CmuxTuiWorkspaceMutation mutation,
                       CommandCallback callback);
  void RenameWorkspace(std::string key,
                       std::string name,
                       CmuxTuiWorkspaceMutation mutation,
                       CommandCallback callback);
  void MoveWorkspace(std::string key,
                     size_t index,
                     CmuxTuiWorkspaceMutation mutation,
                     CommandCallback callback);
  void CloseWorkspace(std::string key,
                      CmuxTuiWorkspaceMutation mutation,
                      CommandCallback callback);
  void RefreshWorkspaceRegistry();
  void RefreshTerminalRegistry();
  void GetFrontendProjection(FrontendProjectionCallback callback);
  void PutFrontendProjection(uint64_t schema_version,
                             uint64_t expected_projection_revision,
                             base::DictValue projection,
                             std::string mutation_id,
                             FrontendProjectionCallback callback);
  void SendBytes(CmuxTuiSurfaceId surface,
                 std::vector<uint8_t> bytes,
                 CommandCallback callback = CommandCallback());
  void ResizeSurface(CmuxTuiSurfaceId surface,
                     uint16_t cols,
                     uint16_t rows,
                     CommandCallback callback = CommandCallback());
  void CloseSurface(CmuxTuiSurfaceId surface,
                    CommandCallback callback = CommandCallback());
  // Requests a short-lived one-use renderer grant, validates every returned
  // field, then connects and authenticates on a blocking worker. `ttl` must be
  // an integral number of milliseconds in the host's 1..60000ms range.
  void MintTerminalRenderer(CmuxTuiSurfaceId surface,
                            base::TimeDelta ttl,
                            RendererConnectionCallback callback);

  std::unique_ptr<CmuxTuiAttachment> AttachSurface(CmuxTuiSurfaceId surface,
                                                   uint16_t cols,
                                                   uint16_t rows,
                                                   EventCallback callback);

 private:
  friend class base::RefCountedThreadSafe<CmuxTuiClient>;
  friend class CmuxTuiAttachment;
  ~CmuxTuiClient();

  void ConnectControl();
  void OnControlConnected(bool connected, std::string error);
  void OnControlDisconnected(std::string error);
  void OnControlMessage(base::Value message,
                        std::optional<std::vector<uint8_t>> bytes,
                        std::string decode_error);
  void RejectControlProtocol(std::string error);
  void VerifyIdentity(CmuxTuiCommandResult result);
  void RequestServerReplacement(uint64_t pid,
                                std::string generation,
                                std::string build_commit,
                                bool daemon_handoff_supported,
                                std::string identity_error);
  void OnServerReplacementClientsListed(uint64_t pid,
                                        std::string generation,
                                        std::string build_commit,
                                        bool daemon_handoff_supported,
                                        std::string identity_error,
                                        CmuxTuiCommandResult result);
  void StartLegacyServerTermination(uint64_t pid,
                                    std::string build_commit,
                                    std::string identity_error);
  void OnServerReplacementRequested(uint64_t pid,
                                    std::string identity_error,
                                    CmuxTuiCommandResult result);
  void OnLegacyServerTerminated(uint64_t pid,
                                std::string identity_error,
                                bool terminated,
                                std::string error);
  void WaitForServerReplacementExit(uint64_t pid,
                                    std::string identity_error);
  void OnServerReplacementExitWaited(std::string identity_error,
                                     bool exited,
                                     std::string error);
  void ContinueServerReplacement();
  void OnSubscribed(CmuxTuiCommandResult result);
  void MaybeSetReady();
  void RefreshWorkspaces();
  void OnWorkspacesListed(CmuxTuiCommandResult result);
  void RefreshTerminals();
  void OnTerminalEventsListed(std::string registry_id,
                              std::string generation,
                              uint64_t after_revision,
                              CmuxTuiCommandResult result);
  void RequestTerminalSnapshot();
  void OnTerminalsListed(CmuxTuiCommandResult result);
  void LaunchServer();
  void OnServerLaunched(bool launched, std::string error);
  void ScheduleReconnect(bool live_disconnect = false);
  void SetReady(bool ready, std::string error);

  void Request(base::DictValue request, CommandCallback callback);

  const Options options_;
  std::unique_ptr<CmuxTuiConnection> control_;
  base::ObserverList<Observer> observers_;
  ReadyCallback start_callback_;
  bool started_ = false;
  bool ready_ = false;
  bool ever_ready_ = false;
  bool launch_attempted_ = false;
  bool protocol_rejected_ = false;
  bool server_replacement_attempted_ = false;
  bool server_replacement_in_progress_ = false;
  bool server_replacement_waiting_for_exit_ = false;
  int server_replacement_identity_retries_ = 0;
  bool reconnect_scheduled_ = false;
  bool workspace_refresh_in_flight_ = false;
  bool workspace_refresh_pending_ = false;
  bool workspace_snapshot_ready_ = false;
  std::optional<CmuxTuiWorkspaceSnapshot> latest_workspace_snapshot_;
  bool terminal_refresh_in_flight_ = false;
  bool terminal_refresh_pending_ = false;
  bool terminal_snapshot_ready_ = false;
  std::optional<CmuxTerminalPlacementSnapshot> latest_terminal_snapshot_;
  std::string terminal_event_registry_id_;
  std::string terminal_event_generation_;
  uint64_t terminal_event_revision_ = 0;
  int connect_attempts_ = 0;
  std::optional<uint64_t> server_pid_;
  uint64_t server_generation_ = 0;
  uint64_t next_renderer_connect_attempt_ = 1;
  std::map<uint64_t, base::OnceClosure> renderer_connect_cancellations_;
  SEQUENCE_CHECKER(sequence_checker_);
  base::WeakPtrFactory<CmuxTuiClient> weak_factory_{this};
};

// One independently closable attach stream. Events and callbacks run on the
// creating (browser UI) sequence. Destroying this object closes only this
// socket; the authoritative TUI surface and its PTY continue running.
class CmuxTuiAttachment {
 public:
  CmuxTuiAttachment(const CmuxTuiAttachment&) = delete;
  CmuxTuiAttachment& operator=(const CmuxTuiAttachment&) = delete;
  ~CmuxTuiAttachment();

  // Resizes on this attach connection so cmux-tui treats the frontend as a
  // viewer size lease. Destroying the attachment releases that lease.
  void ResizeSurface(uint16_t cols,
                     uint16_t rows,
                     CmuxTuiClient::CommandCallback callback);

 private:
  friend class CmuxTuiClient;
  CmuxTuiAttachment(base::FilePath socket_path,
                    CmuxTuiSurfaceId surface,
                    uint16_t cols,
                    uint16_t rows,
                    std::string required_build_commit,
                    std::string required_ghostty_commit,
                    CmuxTuiClient::EventCallback callback);

  void Start();
  void OnConnected(bool connected, std::string error);
  void OnDisconnected(std::string error);
  void OnMessage(base::Value message,
                 std::optional<std::vector<uint8_t>> bytes,
                 std::string decode_error);
  void VerifyIdentity(CmuxTuiCommandResult result);
  void OnAttached(CmuxTuiCommandResult result);
  void SendResize(uint16_t cols,
                  uint16_t rows,
                  CmuxTuiClient::CommandCallback callback);
  void EmitDisconnected(
      std::string error,
      CmuxTuiEvent::Type type = CmuxTuiEvent::Type::kDisconnected);

  const base::FilePath socket_path_;
  const CmuxTuiSurfaceId surface_;
  const uint16_t initial_cols_;
  const uint16_t initial_rows_;
  const std::string required_build_commit_;
  const std::string required_ghostty_commit_;
  CmuxTuiClient::EventCallback callback_;
  std::unique_ptr<CmuxTuiConnection> connection_;
  CmuxTuiClient::CommandCallback pending_resize_callback_;
  uint16_t pending_resize_cols_ = 0;
  uint16_t pending_resize_rows_ = 0;
  bool saw_initial_state_ = false;
  bool attached_ = false;
  bool disconnected_emitted_ = false;
  SEQUENCE_CHECKER(sequence_checker_);
  base::WeakPtrFactory<CmuxTuiAttachment> weak_factory_{this};
};

}  // namespace cmux

#endif  // CHROME_BROWSER_CMUX_TERM_CMUX_TUI_CLIENT_H_
