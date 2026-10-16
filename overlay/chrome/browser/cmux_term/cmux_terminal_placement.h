// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef CHROME_BROWSER_CMUX_TERM_CMUX_TERMINAL_PLACEMENT_H_
#define CHROME_BROWSER_CMUX_TERM_CMUX_TERMINAL_PLACEMENT_H_

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "chrome/browser/cmux_term/cmux_terminal_lifecycle.h"

namespace cmux {

// One canonical terminal-registry record. `terminal_id` and a non-empty
// `incarnation` use the terminal-host protocol's compact lowercase UUIDv4
// encoding. Tombstoned records are event-only and never appear in snapshots.
struct CmuxCanonicalTerminalPlacement {
  CmuxCanonicalTerminalPlacement();
  CmuxCanonicalTerminalPlacement(std::string terminal_id,
                                 std::string incarnation,
                                 std::string workspace_key,
                                 CmuxTerminalLifecycle lifecycle);
  CmuxCanonicalTerminalPlacement(const CmuxCanonicalTerminalPlacement&);
  CmuxCanonicalTerminalPlacement& operator=(
      const CmuxCanonicalTerminalPlacement&);
  CmuxCanonicalTerminalPlacement(CmuxCanonicalTerminalPlacement&&);
  CmuxCanonicalTerminalPlacement& operator=(CmuxCanonicalTerminalPlacement&&);
  ~CmuxCanonicalTerminalPlacement();

  std::string terminal_id;
  std::string incarnation;
  std::string workspace_key;
  CmuxTerminalLifecycle lifecycle = CmuxTerminalLifecycle::kLaunching;

  bool operator==(const CmuxCanonicalTerminalPlacement& other) const;
};

// Current browser projection for one terminal pane. A client-reserved
// terminal ID may exist before an incarnation. `create_pending` is trusted
// only with a non-empty mutation ID and retains an absent pane while that
// mutation is in flight. Runtime surface IDs are never materialization
// authority.
struct CmuxLocalTerminalPlacement {
  CmuxLocalTerminalPlacement();
  CmuxLocalTerminalPlacement(std::string local_key,
                             std::string terminal_id,
                             std::string incarnation,
                             std::string workspace_key,
                             std::string runtime_surface_id,
                             std::string pending_mutation_id,
                             bool create_pending);
  CmuxLocalTerminalPlacement(const CmuxLocalTerminalPlacement&);
  CmuxLocalTerminalPlacement& operator=(const CmuxLocalTerminalPlacement&);
  CmuxLocalTerminalPlacement(CmuxLocalTerminalPlacement&&);
  CmuxLocalTerminalPlacement& operator=(CmuxLocalTerminalPlacement&&);
  ~CmuxLocalTerminalPlacement();

  std::string local_key;
  std::string terminal_id;
  std::string incarnation;
  std::string workspace_key;
  std::string runtime_surface_id;
  std::string pending_mutation_id;
  bool create_pending = false;
};

// Browser-owned optimistic close intent. The close protocol uses
// `(origin, mutation_id)` as its exactly-once key and fingerprints the exact
// terminal ID/incarnation. A non-empty incarnation is exact, so a failed CAS
// cannot hide a replacement host. Empty is the safe ID-only wildcard needed
// when a close initiated during Launching races the first host Ready event;
// client-reserved terminal IDs are never reused.
struct CmuxPendingTerminalClose {
  CmuxPendingTerminalClose();
  CmuxPendingTerminalClose(std::string terminal_id,
                           std::string incarnation,
                           std::string mutation_id);
  CmuxPendingTerminalClose(const CmuxPendingTerminalClose&);
  CmuxPendingTerminalClose& operator=(const CmuxPendingTerminalClose&);
  CmuxPendingTerminalClose(CmuxPendingTerminalClose&&);
  CmuxPendingTerminalClose& operator=(CmuxPendingTerminalClose&&);
  ~CmuxPendingTerminalClose();

  std::string terminal_id;
  std::string incarnation;
  std::string mutation_id;
};

// Terminal revisions are independent from workspace revisions, but both
// streams are fenced by the daemon's durable registry ID and boot generation.
struct CmuxTerminalPlacementSnapshot {
  CmuxTerminalPlacementSnapshot();
  CmuxTerminalPlacementSnapshot(
      std::string registry_id,
      std::string generation,
      uint64_t terminal_revision,
      std::vector<CmuxCanonicalTerminalPlacement> terminals);
  CmuxTerminalPlacementSnapshot(const CmuxTerminalPlacementSnapshot&);
  CmuxTerminalPlacementSnapshot& operator=(
      const CmuxTerminalPlacementSnapshot&);
  CmuxTerminalPlacementSnapshot(CmuxTerminalPlacementSnapshot&&);
  CmuxTerminalPlacementSnapshot& operator=(CmuxTerminalPlacementSnapshot&&);
  ~CmuxTerminalPlacementSnapshot();

  std::string registry_id;
  std::string generation;
  uint64_t terminal_revision = 0;
  std::vector<CmuxCanonicalTerminalPlacement> terminals;
};

struct CmuxTerminalPlacementEvent {
  CmuxTerminalPlacementEvent();
  CmuxTerminalPlacementEvent(std::string registry_id,
                             std::string generation,
                             uint64_t terminal_revision,
                             CmuxCanonicalTerminalPlacement terminal);
  CmuxTerminalPlacementEvent(const CmuxTerminalPlacementEvent&);
  CmuxTerminalPlacementEvent& operator=(const CmuxTerminalPlacementEvent&);
  CmuxTerminalPlacementEvent(CmuxTerminalPlacementEvent&&);
  CmuxTerminalPlacementEvent& operator=(CmuxTerminalPlacementEvent&&);
  ~CmuxTerminalPlacementEvent();

  std::string registry_id;
  std::string generation;
  uint64_t terminal_revision = 0;
  CmuxCanonicalTerminalPlacement terminal;
};

enum class CmuxTerminalPlacementActionKind {
  // Execution order is significant: discard stale panes before resolving,
  // moving, or materializing canonical identities.
  kRemove,
  kResolve,
  kMove,
  kMaterialize,
};

enum class CmuxTerminalResolveReason {
  kNone,
  // The pane has no client-reserved terminal ID. Resolve it through the host
  // registry before allowing any missing terminal to materialize.
  kStableIdentityMissing,
  // A client-reserved ID is retained while its idempotent create mutation is
  // in flight, even if an earlier snapshot does not contain it yet.
  kPendingMutation,
  // A canonical pending lifecycle exists, or the canonical incarnation
  // differs from the pane's binding. Retain the pane and wait/rebind in place.
  kCanonicalBinding,
  // A canonical terminal targets a workspace not yet in the GUI projection.
  // The workspace registry must materialize it; terminal placement never
  // creates or removes workspaces implicitly.
  kWorkspaceMissing,
};

struct CmuxTerminalPlacementAction {
  CmuxTerminalPlacementAction();
  CmuxTerminalPlacementAction(const CmuxTerminalPlacementAction&);
  CmuxTerminalPlacementAction& operator=(const CmuxTerminalPlacementAction&);
  CmuxTerminalPlacementAction(CmuxTerminalPlacementAction&&);
  CmuxTerminalPlacementAction& operator=(CmuxTerminalPlacementAction&&);
  ~CmuxTerminalPlacementAction();

  CmuxTerminalPlacementActionKind kind =
      CmuxTerminalPlacementActionKind::kResolve;
  CmuxTerminalResolveReason resolve_reason = CmuxTerminalResolveReason::kNone;
  std::string local_key;
  std::string terminal_id;
  std::string incarnation;
  std::string workspace_key;
  std::string runtime_surface_id;
  std::string pending_mutation_id;
  // Meaningful for canonical Resolve and Materialize actions.
  CmuxTerminalLifecycle lifecycle = CmuxTerminalLifecycle::kLaunching;

  bool operator==(const CmuxTerminalPlacementAction& other) const;
};

enum class CmuxTerminalPlacementDisposition {
  kApplied,
  kIgnored,
  kNeedsSnapshot,
};

struct CmuxTerminalPlacementPlan {
  CmuxTerminalPlacementPlan();
  CmuxTerminalPlacementPlan(const CmuxTerminalPlacementPlan&);
  CmuxTerminalPlacementPlan& operator=(const CmuxTerminalPlacementPlan&);
  CmuxTerminalPlacementPlan(CmuxTerminalPlacementPlan&&);
  CmuxTerminalPlacementPlan& operator=(CmuxTerminalPlacementPlan&&);
  ~CmuxTerminalPlacementPlan();

  CmuxTerminalPlacementDisposition disposition =
      CmuxTerminalPlacementDisposition::kNeedsSnapshot;
  std::string registry_id;
  std::string generation;
  uint64_t terminal_revision = 0;
  std::vector<CmuxTerminalPlacementAction> actions;

  // This is the normalized GUI workspace input, not a set inferred from
  // terminals. Keeping it in every plan makes the empty-workspace invariant
  // explicit: terminal reconciliation never emits workspace removal.
  std::vector<std::string> retained_workspace_keys;

  bool require_snapshot() const {
    return disposition == CmuxTerminalPlacementDisposition::kNeedsSnapshot;
  }
};

// Stateful terminal-registry cursor plus a pure projection diff. Events are
// accepted only at the exact next terminal revision for one registry ID and
// daemon generation. A revision gap or epoch mismatch closes mutation until
// an authoritative snapshot arrives.
class CmuxCanonicalTerminalPlacementModel {
 public:
  CmuxCanonicalTerminalPlacementModel();
  CmuxCanonicalTerminalPlacementModel(
      const CmuxCanonicalTerminalPlacementModel&) = delete;
  CmuxCanonicalTerminalPlacementModel& operator=(
      const CmuxCanonicalTerminalPlacementModel&) = delete;
  CmuxCanonicalTerminalPlacementModel(CmuxCanonicalTerminalPlacementModel&&) =
      delete;
  CmuxCanonicalTerminalPlacementModel& operator=(
      CmuxCanonicalTerminalPlacementModel&&) = delete;
  ~CmuxCanonicalTerminalPlacementModel();

  CmuxTerminalPlacementPlan ApplySnapshot(
      const CmuxTerminalPlacementSnapshot& snapshot,
      const std::vector<CmuxLocalTerminalPlacement>& local,
      const std::vector<std::string>& gui_workspace_keys,
      const std::vector<CmuxPendingTerminalClose>& pending_closes = {});

  CmuxTerminalPlacementPlan ApplyEvent(
      const CmuxTerminalPlacementEvent& event,
      const std::vector<CmuxLocalTerminalPlacement>& local,
      const std::vector<std::string>& gui_workspace_keys,
      const std::vector<CmuxPendingTerminalClose>& pending_closes = {});

  CmuxTerminalPlacementPlan Reconcile(
      const std::vector<CmuxLocalTerminalPlacement>& local,
      const std::vector<std::string>& gui_workspace_keys,
      const std::vector<CmuxPendingTerminalClose>& pending_closes = {}) const;

  bool has_snapshot() const { return has_snapshot_; }
  bool needs_snapshot() const { return needs_snapshot_; }
  const std::string& registry_id() const { return registry_id_; }
  const std::string& generation() const { return generation_; }
  uint64_t terminal_revision() const { return terminal_revision_; }

 private:
  CmuxTerminalPlacementPlan BuildPlan(
      CmuxTerminalPlacementDisposition disposition,
      const std::vector<CmuxLocalTerminalPlacement>& local,
      const std::vector<std::string>& gui_workspace_keys,
      const std::vector<CmuxPendingTerminalClose>& pending_closes,
      bool include_actions) const;

  bool has_snapshot_ = false;
  bool needs_snapshot_ = true;
  std::string registry_id_;
  std::string generation_;
  uint64_t terminal_revision_ = 0;
  std::map<std::string, CmuxCanonicalTerminalPlacement> terminals_;

  // Tombstones and retired host incarnations are retained for the lifetime of
  // one durable registry, including daemon-generation changes. They prevent a
  // late higher-revision completion from resurrecting closed/stale state.
  std::set<std::string> tombstoned_terminal_ids_;
  std::set<std::pair<std::string, std::string>> retired_incarnations_;
};

bool IsCmuxStableTerminalUuid(const std::string& value);

}  // namespace cmux

#endif  // CHROME_BROWSER_CMUX_TERM_CMUX_TERMINAL_PLACEMENT_H_
