// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef CHROME_BROWSER_CMUX_TERM_CMUX_WORKSPACE_PROJECTION_H_
#define CHROME_BROWSER_CMUX_TERM_CMUX_WORKSPACE_PROJECTION_H_

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cmux {

// Stable address for one browser frontend projection. The path itself never
// crosses the protocol boundary: only deterministic opaque tokens do.
struct CmuxFrontendIdentity {
  CmuxFrontendIdentity();
  CmuxFrontendIdentity(const CmuxFrontendIdentity&);
  CmuxFrontendIdentity& operator=(const CmuxFrontendIdentity&);
  CmuxFrontendIdentity(CmuxFrontendIdentity&&);
  CmuxFrontendIdentity& operator=(CmuxFrontendIdentity&&);
  ~CmuxFrontendIdentity();

  std::string frontend;
  std::string scope;
  std::string subject_key;
  std::string origin;
  std::string session;

  friend bool operator==(const CmuxFrontendIdentity& left,
                         const CmuxFrontendIdentity& right) {
    return left.frontend == right.frontend && left.scope == right.scope &&
           left.subject_key == right.subject_key &&
           left.origin == right.origin && left.session == right.session;
  }
};

// Derives the daemon session and projection scope from durable browser
// identity, rather than a random per-launch value or a process-local pointer.
// `window_group` is supplied by the browser's window/session restore layer;
// normal startup uses the stable "primary" group.
CmuxFrontendIdentity BuildCmuxFrontendIdentity(
    std::string_view profile_path,
    std::string_view browsing_mode,
    std::string_view window_group);

bool IsCmuxUuid(std::string_view value);
std::string BuildCmuxPrimaryWindowGroupId(std::string_view profile_path,
                                          std::string_view browsing_mode);

struct CmuxDurableWorkspaceAddress {
  std::string window_group;
  std::string workspace_key;

  friend bool operator==(const CmuxDurableWorkspaceAddress& left,
                         const CmuxDurableWorkspaceAddress& right) {
    return left.window_group == right.window_group &&
           left.workspace_key == right.workspace_key;
  }
  friend bool operator<(const CmuxDurableWorkspaceAddress& left,
                        const CmuxDurableWorkspaceAddress& right) {
    return left.window_group < right.window_group ||
           (left.window_group == right.window_group &&
            left.workspace_key < right.workspace_key);
  }
};

std::string CmuxDurableWorkspaceToken(
    const CmuxDurableWorkspaceAddress& address);
std::optional<CmuxDurableWorkspaceAddress> CmuxDurableWorkspaceFromToken(
    std::string_view token);

struct CmuxCanonicalWorkspace {
  std::string key;
  std::string name;

  friend bool operator==(const CmuxCanonicalWorkspace& left,
                         const CmuxCanonicalWorkspace& right) {
    return left.key == right.key && left.name == right.name;
  }
};

struct CmuxLocalWorkspace {
  std::string key;
  std::string name;
};

struct CmuxPendingWorkspaceMove {
  std::string key;
  size_t desired_index = 0;
  std::string mutation_id;

  friend bool operator==(const CmuxPendingWorkspaceMove& left,
                         const CmuxPendingWorkspaceMove& right) {
    return left.key == right.key &&
           left.desired_index == right.desired_index &&
           left.mutation_id == right.mutation_id;
  }
};

struct CmuxPendingWorkspaceRename {
  std::string name;
  std::string mutation_id;

  friend bool operator==(const CmuxPendingWorkspaceRename& left,
                         const CmuxPendingWorkspaceRename& right) {
    return left.name == right.name && left.mutation_id == right.mutation_id;
  }
};

// Ephemeral GUI intents waiting for confirmation from the canonical registry.
// Remembering the same keyed intent twice deliberately retains its first
// mutation ID: losing a response and revisiting the sync loop must replay the
// original mutation instead of submitting a second logical command.
class CmuxPendingWorkspaceMutations {
 public:
  using MutationIds = std::map<std::string, std::string>;
  using Renames = std::map<std::string, CmuxPendingWorkspaceRename>;

  CmuxPendingWorkspaceMutations();
  ~CmuxPendingWorkspaceMutations();

  bool RememberCreate(std::string key, std::string mutation_id);
  void ForgetCreate(std::string_view key);
  bool HasCreate(std::string_view key) const;

  bool RememberClose(std::string key, std::string mutation_id);
  void ForgetClose(std::string_view key);
  bool HasClose(std::string_view key) const;

  bool RememberRename(std::string key,
                      std::string name,
                      std::string mutation_id);
  void ForgetRename(std::string_view key);
  bool HasRename(std::string_view key) const;

  void SetMove(std::string key,
               size_t desired_index,
               std::string mutation_id);
  void ForgetMove();

  void Clear();

  const MutationIds& creates() const { return creates_; }
  const MutationIds& closes() const { return closes_; }
  const Renames& renames() const { return renames_; }
  const std::optional<CmuxPendingWorkspaceMove>& move() const { return move_; }
  std::set<std::string> CreateKeys() const;
  std::set<std::string> CloseKeys() const;

 private:
  MutationIds creates_;
  MutationIds closes_;
  Renames renames_;
  std::optional<CmuxPendingWorkspaceMove> move_;
};

// Pure diff between the authoritative cmux registry and the current browser
// projection. Pending operations are ephemeral CAS operation state, not a
// second workspace registry.
struct CmuxWorkspaceReconcilePlan {
  CmuxWorkspaceReconcilePlan();
  CmuxWorkspaceReconcilePlan(const CmuxWorkspaceReconcilePlan&);
  CmuxWorkspaceReconcilePlan& operator=(const CmuxWorkspaceReconcilePlan&);
  CmuxWorkspaceReconcilePlan(CmuxWorkspaceReconcilePlan&&);
  CmuxWorkspaceReconcilePlan& operator=(CmuxWorkspaceReconcilePlan&&);
  ~CmuxWorkspaceReconcilePlan();

  std::vector<CmuxCanonicalWorkspace> materialize;
  std::vector<std::pair<std::string, std::string>> rename;
  std::vector<std::string> remove;
  std::vector<std::string> canonical_order;
};

CmuxWorkspaceReconcilePlan BuildCmuxWorkspaceReconcilePlan(
    const std::vector<CmuxCanonicalWorkspace>& canonical,
    const std::vector<CmuxLocalWorkspace>& local,
    const std::set<std::string>& pending_creates,
    const std::set<std::string>& pending_closes,
    const CmuxPendingWorkspaceMutations::Renames& pending_renames = {});

// Returns the minimal left-to-right sequence of keyed moves that transforms
// `current` into `desired`. Each returned index is valid after applying all
// preceding moves, so already-fixed prefix entries never shift again.
std::vector<std::pair<std::string, size_t>> BuildCmuxWorkspaceOrderMoves(
    const std::vector<std::string>& current,
    const std::vector<std::string>& desired);

// Coalesces frontend-projection invalidations around asynchronous GET/PUT
// operations. An event can arrive while either request is in flight; retaining
// the maximum observed revision guarantees one follow-up GET without allowing
// an event from a superseded registry generation to create a reload loop.
class CmuxWorkspaceProjectionReloadBarrier {
 public:
  explicit CmuxWorkspaceProjectionReloadBarrier(uint64_t generation = 1);

  void Reset(uint64_t generation);
  void Observe(uint64_t generation, uint64_t revision);

  // Returns true exactly once for each newly observed maximum revision that
  // is newer than `current_revision`. `blocked` covers a GET/PUT already in
  // flight as well as a disconnected or unsupported projection backend.
  bool TakeReload(uint64_t generation,
                  uint64_t current_revision,
                  bool blocked);

  uint64_t generation() const { return generation_; }
  uint64_t max_observed_revision() const { return max_observed_revision_; }
  uint64_t max_scheduled_revision() const { return max_scheduled_revision_; }

 private:
  uint64_t generation_ = 1;
  uint64_t max_observed_revision_ = 0;
  uint64_t max_scheduled_revision_ = 0;
};

// Projection events are invalidation hints, but must still belong to the
// exact durable registry and daemon generation represented by the GUI's
// workspace snapshot. Empty identity (including an event received before the
// first snapshot) fails closed; the initial projection GET is its barrier.
bool IsCurrentCmuxWorkspaceProjectionEvent(
    std::string_view snapshot_registry_id,
    std::string_view snapshot_generation,
    std::string_view event_registry_id,
    std::string_view event_generation);

// Returns true only when a lifecycle event can be safely projected onto the
// current snapshot before the confirming list-workspaces barrier arrives.
bool IsNextCmuxWorkspaceRevision(std::string_view snapshot_registry_id,
                                 std::string_view snapshot_generation,
                                 uint64_t snapshot_revision,
                                 std::string_view event_registry_id,
                                 std::string_view event_generation,
                                 uint64_t event_revision);

// Schema-v1 browser-only workspace presentation. Canonical UUID, name,
// existence, and flat order remain owned by cmux; this projection owns only
// browser hierarchy and rail/layout presentation keyed by canonical UUID.
struct CmuxWorkspaceProjectionEntry {
  std::string key;
  std::string parent_key;
  bool expanded = true;
  int color = 0;
  bool tiled = false;
  // Presence also marks the representative as grouped when it has no child
  // parent_key (including a one-workspace group). Missing means an older
  // projection that predates persisted group titles; an explicitly empty
  // title is meaningful ("Unnamed group") and must survive a restart.
  std::optional<std::string> group_title;

  friend bool operator==(const CmuxWorkspaceProjectionEntry& left,
                         const CmuxWorkspaceProjectionEntry& right) {
    return left.key == right.key && left.parent_key == right.parent_key &&
           left.expanded == right.expanded && left.color == right.color &&
           left.tiled == right.tiled &&
           left.group_title == right.group_title;
  }
};

struct CmuxWorkspaceProjection {
  CmuxWorkspaceProjection();
  CmuxWorkspaceProjection(const CmuxWorkspaceProjection&);
  CmuxWorkspaceProjection& operator=(const CmuxWorkspaceProjection&);
  CmuxWorkspaceProjection(CmuxWorkspaceProjection&&);
  CmuxWorkspaceProjection& operator=(CmuxWorkspaceProjection&&);
  ~CmuxWorkspaceProjection();

  std::string selected_key;
  std::vector<CmuxWorkspaceProjectionEntry> workspaces;

  friend bool operator==(const CmuxWorkspaceProjection& left,
                         const CmuxWorkspaceProjection& right) {
    return left.selected_key == right.selected_key &&
           left.workspaces == right.workspaces;
  }
  friend bool operator!=(const CmuxWorkspaceProjection& left,
                         const CmuxWorkspaceProjection& right) {
    return !(left == right);
  }
};

// Drops stale/duplicate keys, creates defaults for every canonical workspace,
// validates presentation values, and deterministically breaks parent cycles.
// The result has exactly one entry per canonical UUID in canonical order.
CmuxWorkspaceProjection NormalizeCmuxWorkspaceProjection(
    const std::vector<CmuxCanonicalWorkspace>& canonical,
    const CmuxWorkspaceProjection& candidate);

}  // namespace cmux

#endif  // CHROME_BROWSER_CMUX_TERM_CMUX_WORKSPACE_PROJECTION_H_
