// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "chrome/browser/cmux_term/cmux_terminal_placement.h"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <map>
#include <set>
#include <tuple>
#include <utility>

namespace cmux {

namespace {

using TerminalIncarnation = std::pair<std::string, std::string>;

bool IsSnapshotLifecycle(CmuxTerminalLifecycle lifecycle) {
  switch (lifecycle) {
    case CmuxTerminalLifecycle::kLaunching:
    case CmuxTerminalLifecycle::kAdopting:
    case CmuxTerminalLifecycle::kRunning:
    case CmuxTerminalLifecycle::kExited:
      return true;
    case CmuxTerminalLifecycle::kTombstoned:
      return false;
  }
  return false;
}

bool IsValidSnapshotRecord(const CmuxCanonicalTerminalPlacement& terminal) {
  if (!IsCmuxStableTerminalUuid(terminal.terminal_id) ||
      !IsSnapshotLifecycle(terminal.lifecycle) ||
      terminal.workspace_key.empty()) {
    return false;
  }
  switch (terminal.lifecycle) {
    case CmuxTerminalLifecycle::kLaunching:
      return terminal.incarnation.empty();
    case CmuxTerminalLifecycle::kAdopting:
    case CmuxTerminalLifecycle::kRunning:
      return IsCmuxStableTerminalUuid(terminal.incarnation);
    case CmuxTerminalLifecycle::kExited:
      return terminal.incarnation.empty() ||
             IsCmuxStableTerminalUuid(terminal.incarnation);
    case CmuxTerminalLifecycle::kTombstoned:
      return false;
  }
  return false;
}

bool IsValidEventRecord(const CmuxCanonicalTerminalPlacement& terminal) {
  if (terminal.lifecycle != CmuxTerminalLifecycle::kTombstoned) {
    return IsValidSnapshotRecord(terminal);
  }
  return IsCmuxStableTerminalUuid(terminal.terminal_id) &&
         (terminal.incarnation.empty() ||
          IsCmuxStableTerminalUuid(terminal.incarnation));
}

bool HasTrackedCreate(const CmuxLocalTerminalPlacement& terminal) {
  return terminal.create_pending &&
         IsCmuxStableTerminalUuid(terminal.terminal_id) &&
         !terminal.pending_mutation_id.empty();
}

bool IsValidPendingClose(const CmuxPendingTerminalClose& pending_close) {
  return IsCmuxStableTerminalUuid(pending_close.terminal_id) &&
         (pending_close.incarnation.empty() ||
          IsCmuxStableTerminalUuid(pending_close.incarnation)) &&
         !pending_close.mutation_id.empty();
}

std::vector<std::string> NormalizeWorkspaceKeys(
    const std::vector<std::string>& keys) {
  std::vector<std::string> result;
  std::set<std::string> seen;
  for (const std::string& key : keys) {
    if (!key.empty() && seen.insert(key).second) {
      result.push_back(key);
    }
  }
  return result;
}

CmuxTerminalPlacementAction RemoveAction(
    const CmuxLocalTerminalPlacement& local) {
  CmuxTerminalPlacementAction action;
  action.kind = CmuxTerminalPlacementActionKind::kRemove;
  action.local_key = local.local_key;
  action.terminal_id = local.terminal_id;
  action.incarnation = local.incarnation;
  action.workspace_key = local.workspace_key;
  action.runtime_surface_id = local.runtime_surface_id;
  action.pending_mutation_id = local.pending_mutation_id;
  return action;
}

CmuxTerminalPlacementAction ResolveLocalAction(
    const CmuxLocalTerminalPlacement& local,
    CmuxTerminalResolveReason reason) {
  CmuxTerminalPlacementAction action;
  action.kind = CmuxTerminalPlacementActionKind::kResolve;
  action.resolve_reason = reason;
  action.local_key = local.local_key;
  action.terminal_id = local.terminal_id;
  action.incarnation = local.incarnation;
  action.workspace_key = local.workspace_key;
  action.runtime_surface_id = local.runtime_surface_id;
  action.pending_mutation_id = local.pending_mutation_id;
  return action;
}

CmuxTerminalPlacementAction ResolveCanonicalAction(
    const CmuxLocalTerminalPlacement& local,
    const CmuxCanonicalTerminalPlacement& terminal) {
  CmuxTerminalPlacementAction action;
  action.kind = CmuxTerminalPlacementActionKind::kResolve;
  action.resolve_reason = CmuxTerminalResolveReason::kCanonicalBinding;
  action.local_key = local.local_key;
  action.terminal_id = terminal.terminal_id;
  action.incarnation = terminal.incarnation;
  action.workspace_key = terminal.workspace_key;
  action.runtime_surface_id = local.runtime_surface_id;
  action.pending_mutation_id = local.pending_mutation_id;
  action.lifecycle = terminal.lifecycle;
  return action;
}

CmuxTerminalPlacementAction ResolveWorkspaceAction(
    const CmuxCanonicalTerminalPlacement& terminal,
    const std::string& local_key) {
  CmuxTerminalPlacementAction action;
  action.kind = CmuxTerminalPlacementActionKind::kResolve;
  action.resolve_reason = CmuxTerminalResolveReason::kWorkspaceMissing;
  action.local_key = local_key;
  action.terminal_id = terminal.terminal_id;
  action.incarnation = terminal.incarnation;
  action.workspace_key = terminal.workspace_key;
  action.lifecycle = terminal.lifecycle;
  return action;
}

CmuxTerminalPlacementAction MoveAction(
    const CmuxLocalTerminalPlacement& local,
    const CmuxCanonicalTerminalPlacement& terminal) {
  CmuxTerminalPlacementAction action;
  action.kind = CmuxTerminalPlacementActionKind::kMove;
  action.local_key = local.local_key;
  action.terminal_id = terminal.terminal_id;
  action.incarnation = terminal.incarnation;
  action.workspace_key = terminal.workspace_key;
  action.lifecycle = terminal.lifecycle;
  return action;
}

CmuxTerminalPlacementAction MaterializeAction(
    const CmuxCanonicalTerminalPlacement& terminal) {
  CmuxTerminalPlacementAction action;
  action.kind = CmuxTerminalPlacementActionKind::kMaterialize;
  action.terminal_id = terminal.terminal_id;
  action.incarnation = terminal.incarnation;
  action.workspace_key = terminal.workspace_key;
  action.lifecycle = terminal.lifecycle;
  return action;
}

bool ActionLess(const CmuxTerminalPlacementAction& left,
                const CmuxTerminalPlacementAction& right) {
  return std::tie(left.kind, left.local_key, left.terminal_id, left.incarnation,
                  left.workspace_key, left.runtime_surface_id,
                  left.pending_mutation_id, left.resolve_reason,
                  left.lifecycle) <
         std::tie(right.kind, right.local_key, right.terminal_id,
                  right.incarnation, right.workspace_key,
                  right.runtime_surface_id, right.pending_mutation_id,
                  right.resolve_reason, right.lifecycle);
}

bool BuildSnapshotState(
    const std::vector<CmuxCanonicalTerminalPlacement>& terminals,
    std::map<std::string, CmuxCanonicalTerminalPlacement>* state) {
  if (!state) {
    return false;
  }
  for (const CmuxCanonicalTerminalPlacement& terminal : terminals) {
    if (!IsValidSnapshotRecord(terminal)) {
      return false;
    }
    const auto [existing, inserted] =
        state->emplace(terminal.terminal_id, terminal);
    if (!inserted && !(existing->second == terminal)) {
      return false;
    }
  }
  return true;
}

int BindingRank(const CmuxLocalTerminalPlacement& local,
                const CmuxCanonicalTerminalPlacement& canonical) {
  if (local.incarnation == canonical.incarnation) {
    return 0;
  }
  if (local.incarnation.empty()) {
    return 1;
  }
  return 2;
}

bool NeedsCanonicalResolve(const CmuxLocalTerminalPlacement& local,
                           const CmuxCanonicalTerminalPlacement& canonical) {
  return canonical.lifecycle == CmuxTerminalLifecycle::kLaunching ||
         canonical.lifecycle == CmuxTerminalLifecycle::kAdopting ||
         local.incarnation != canonical.incarnation;
}

TerminalIncarnation IncarnationOf(
    const CmuxCanonicalTerminalPlacement& terminal) {
  return {terminal.terminal_id, terminal.incarnation};
}

}  // namespace

CmuxCanonicalTerminalPlacement::CmuxCanonicalTerminalPlacement() = default;
CmuxCanonicalTerminalPlacement::CmuxCanonicalTerminalPlacement(
    std::string terminal_id,
    std::string incarnation,
    std::string workspace_key,
    CmuxTerminalLifecycle lifecycle)
    : terminal_id(std::move(terminal_id)),
      incarnation(std::move(incarnation)),
      workspace_key(std::move(workspace_key)),
      lifecycle(lifecycle) {}
CmuxCanonicalTerminalPlacement::CmuxCanonicalTerminalPlacement(
    const CmuxCanonicalTerminalPlacement&) = default;
CmuxCanonicalTerminalPlacement& CmuxCanonicalTerminalPlacement::operator=(
    const CmuxCanonicalTerminalPlacement&) = default;
CmuxCanonicalTerminalPlacement::CmuxCanonicalTerminalPlacement(
    CmuxCanonicalTerminalPlacement&&) = default;
CmuxCanonicalTerminalPlacement& CmuxCanonicalTerminalPlacement::operator=(
    CmuxCanonicalTerminalPlacement&&) = default;
CmuxCanonicalTerminalPlacement::~CmuxCanonicalTerminalPlacement() = default;

bool CmuxCanonicalTerminalPlacement::operator==(
    const CmuxCanonicalTerminalPlacement& other) const {
  return terminal_id == other.terminal_id && incarnation == other.incarnation &&
         workspace_key == other.workspace_key && lifecycle == other.lifecycle;
}

CmuxLocalTerminalPlacement::CmuxLocalTerminalPlacement() = default;
CmuxLocalTerminalPlacement::CmuxLocalTerminalPlacement(
    std::string local_key,
    std::string terminal_id,
    std::string incarnation,
    std::string workspace_key,
    std::string runtime_surface_id,
    std::string pending_mutation_id,
    bool create_pending)
    : local_key(std::move(local_key)),
      terminal_id(std::move(terminal_id)),
      incarnation(std::move(incarnation)),
      workspace_key(std::move(workspace_key)),
      runtime_surface_id(std::move(runtime_surface_id)),
      pending_mutation_id(std::move(pending_mutation_id)),
      create_pending(create_pending) {}
CmuxLocalTerminalPlacement::CmuxLocalTerminalPlacement(
    const CmuxLocalTerminalPlacement&) = default;
CmuxLocalTerminalPlacement& CmuxLocalTerminalPlacement::operator=(
    const CmuxLocalTerminalPlacement&) = default;
CmuxLocalTerminalPlacement::CmuxLocalTerminalPlacement(
    CmuxLocalTerminalPlacement&&) = default;
CmuxLocalTerminalPlacement& CmuxLocalTerminalPlacement::operator=(
    CmuxLocalTerminalPlacement&&) = default;
CmuxLocalTerminalPlacement::~CmuxLocalTerminalPlacement() = default;

CmuxPendingTerminalClose::CmuxPendingTerminalClose() = default;
CmuxPendingTerminalClose::CmuxPendingTerminalClose(std::string terminal_id,
                                                   std::string incarnation,
                                                   std::string mutation_id)
    : terminal_id(std::move(terminal_id)),
      incarnation(std::move(incarnation)),
      mutation_id(std::move(mutation_id)) {}
CmuxPendingTerminalClose::CmuxPendingTerminalClose(
    const CmuxPendingTerminalClose&) = default;
CmuxPendingTerminalClose& CmuxPendingTerminalClose::operator=(
    const CmuxPendingTerminalClose&) = default;
CmuxPendingTerminalClose::CmuxPendingTerminalClose(CmuxPendingTerminalClose&&) =
    default;
CmuxPendingTerminalClose& CmuxPendingTerminalClose::operator=(
    CmuxPendingTerminalClose&&) = default;
CmuxPendingTerminalClose::~CmuxPendingTerminalClose() = default;

CmuxTerminalPlacementSnapshot::CmuxTerminalPlacementSnapshot() = default;
CmuxTerminalPlacementSnapshot::CmuxTerminalPlacementSnapshot(
    std::string registry_id,
    std::string generation,
    uint64_t terminal_revision,
    std::vector<CmuxCanonicalTerminalPlacement> terminals)
    : registry_id(std::move(registry_id)),
      generation(std::move(generation)),
      terminal_revision(terminal_revision),
      terminals(std::move(terminals)) {}
CmuxTerminalPlacementSnapshot::CmuxTerminalPlacementSnapshot(
    const CmuxTerminalPlacementSnapshot&) = default;
CmuxTerminalPlacementSnapshot& CmuxTerminalPlacementSnapshot::operator=(
    const CmuxTerminalPlacementSnapshot&) = default;
CmuxTerminalPlacementSnapshot::CmuxTerminalPlacementSnapshot(
    CmuxTerminalPlacementSnapshot&&) = default;
CmuxTerminalPlacementSnapshot& CmuxTerminalPlacementSnapshot::operator=(
    CmuxTerminalPlacementSnapshot&&) = default;
CmuxTerminalPlacementSnapshot::~CmuxTerminalPlacementSnapshot() = default;

CmuxTerminalPlacementEvent::CmuxTerminalPlacementEvent() = default;
CmuxTerminalPlacementEvent::CmuxTerminalPlacementEvent(
    std::string registry_id,
    std::string generation,
    uint64_t terminal_revision,
    CmuxCanonicalTerminalPlacement terminal)
    : registry_id(std::move(registry_id)),
      generation(std::move(generation)),
      terminal_revision(terminal_revision),
      terminal(std::move(terminal)) {}
CmuxTerminalPlacementEvent::CmuxTerminalPlacementEvent(
    const CmuxTerminalPlacementEvent&) = default;
CmuxTerminalPlacementEvent& CmuxTerminalPlacementEvent::operator=(
    const CmuxTerminalPlacementEvent&) = default;
CmuxTerminalPlacementEvent::CmuxTerminalPlacementEvent(
    CmuxTerminalPlacementEvent&&) = default;
CmuxTerminalPlacementEvent& CmuxTerminalPlacementEvent::operator=(
    CmuxTerminalPlacementEvent&&) = default;
CmuxTerminalPlacementEvent::~CmuxTerminalPlacementEvent() = default;

CmuxTerminalPlacementAction::CmuxTerminalPlacementAction() = default;
CmuxTerminalPlacementAction::CmuxTerminalPlacementAction(
    const CmuxTerminalPlacementAction&) = default;
CmuxTerminalPlacementAction& CmuxTerminalPlacementAction::operator=(
    const CmuxTerminalPlacementAction&) = default;
CmuxTerminalPlacementAction::CmuxTerminalPlacementAction(
    CmuxTerminalPlacementAction&&) = default;
CmuxTerminalPlacementAction& CmuxTerminalPlacementAction::operator=(
    CmuxTerminalPlacementAction&&) = default;
CmuxTerminalPlacementAction::~CmuxTerminalPlacementAction() = default;

bool CmuxTerminalPlacementAction::operator==(
    const CmuxTerminalPlacementAction& other) const {
  return kind == other.kind && resolve_reason == other.resolve_reason &&
         local_key == other.local_key && terminal_id == other.terminal_id &&
         incarnation == other.incarnation &&
         workspace_key == other.workspace_key &&
         runtime_surface_id == other.runtime_surface_id &&
         pending_mutation_id == other.pending_mutation_id &&
         lifecycle == other.lifecycle;
}

CmuxTerminalPlacementPlan::CmuxTerminalPlacementPlan() = default;
CmuxTerminalPlacementPlan::CmuxTerminalPlacementPlan(
    const CmuxTerminalPlacementPlan&) = default;
CmuxTerminalPlacementPlan& CmuxTerminalPlacementPlan::operator=(
    const CmuxTerminalPlacementPlan&) = default;
CmuxTerminalPlacementPlan::CmuxTerminalPlacementPlan(
    CmuxTerminalPlacementPlan&&) = default;
CmuxTerminalPlacementPlan& CmuxTerminalPlacementPlan::operator=(
    CmuxTerminalPlacementPlan&&) = default;
CmuxTerminalPlacementPlan::~CmuxTerminalPlacementPlan() = default;

CmuxCanonicalTerminalPlacementModel::CmuxCanonicalTerminalPlacementModel() =
    default;
CmuxCanonicalTerminalPlacementModel::~CmuxCanonicalTerminalPlacementModel() =
    default;

CmuxTerminalPlacementPlan CmuxCanonicalTerminalPlacementModel::ApplySnapshot(
    const CmuxTerminalPlacementSnapshot& snapshot,
    const std::vector<CmuxLocalTerminalPlacement>& local,
    const std::vector<std::string>& gui_workspace_keys,
    const std::vector<CmuxPendingTerminalClose>& pending_closes) {
  if (snapshot.registry_id.empty() || snapshot.generation.empty()) {
    needs_snapshot_ = true;
    return BuildPlan(CmuxTerminalPlacementDisposition::kNeedsSnapshot, local,
                     gui_workspace_keys, pending_closes, false);
  }

  const bool same_registry =
      has_snapshot_ && snapshot.registry_id == registry_id_;
  const bool same_epoch = same_registry && snapshot.generation == generation_;
  if (same_epoch && snapshot.terminal_revision < terminal_revision_) {
    return BuildPlan(CmuxTerminalPlacementDisposition::kIgnored, local,
                     gui_workspace_keys, pending_closes, false);
  }
  if (same_epoch && snapshot.terminal_revision == terminal_revision_) {
    return BuildPlan(needs_snapshot_
                         ? CmuxTerminalPlacementDisposition::kNeedsSnapshot
                         : CmuxTerminalPlacementDisposition::kIgnored,
                     local, gui_workspace_keys, pending_closes, false);
  }

  std::map<std::string, CmuxCanonicalTerminalPlacement> next_terminals;
  if (!BuildSnapshotState(snapshot.terminals, &next_terminals)) {
    needs_snapshot_ = true;
    return BuildPlan(CmuxTerminalPlacementDisposition::kNeedsSnapshot, local,
                     gui_workspace_keys, pending_closes, false);
  }

  std::set<std::string> next_tombstones =
      same_registry ? tombstoned_terminal_ids_ : std::set<std::string>();
  std::set<TerminalIncarnation> next_retired =
      same_registry ? retired_incarnations_ : std::set<TerminalIncarnation>();
  if (same_registry) {
    for (const auto& entry : terminals_) {
      const auto next = next_terminals.find(entry.first);
      if (next == next_terminals.end()) {
        next_tombstones.insert(entry.first);
        if (!entry.second.incarnation.empty()) {
          next_retired.insert(IncarnationOf(entry.second));
        }
        continue;
      }
      if (!entry.second.incarnation.empty() &&
          entry.second.incarnation != next->second.incarnation) {
        next_retired.insert(IncarnationOf(entry.second));
      }
    }
    for (const auto& entry : next_terminals) {
      if (next_tombstones.find(entry.first) != next_tombstones.end() ||
          (!entry.second.incarnation.empty() &&
           next_retired.find(IncarnationOf(entry.second)) !=
               next_retired.end())) {
        needs_snapshot_ = true;
        return BuildPlan(CmuxTerminalPlacementDisposition::kNeedsSnapshot,
                         local, gui_workspace_keys, pending_closes, false);
      }
    }
  }

  has_snapshot_ = true;
  needs_snapshot_ = false;
  registry_id_ = snapshot.registry_id;
  generation_ = snapshot.generation;
  terminal_revision_ = snapshot.terminal_revision;
  terminals_ = std::move(next_terminals);
  tombstoned_terminal_ids_ = std::move(next_tombstones);
  retired_incarnations_ = std::move(next_retired);
  return BuildPlan(CmuxTerminalPlacementDisposition::kApplied, local,
                   gui_workspace_keys, pending_closes, true);
}

CmuxTerminalPlacementPlan CmuxCanonicalTerminalPlacementModel::ApplyEvent(
    const CmuxTerminalPlacementEvent& event,
    const std::vector<CmuxLocalTerminalPlacement>& local,
    const std::vector<std::string>& gui_workspace_keys,
    const std::vector<CmuxPendingTerminalClose>& pending_closes) {
  if (!has_snapshot_ || event.registry_id.empty() || event.generation.empty()) {
    needs_snapshot_ = true;
    return BuildPlan(CmuxTerminalPlacementDisposition::kNeedsSnapshot, local,
                     gui_workspace_keys, pending_closes, false);
  }
  if (event.registry_id != registry_id_ || event.generation != generation_) {
    needs_snapshot_ = true;
    return BuildPlan(CmuxTerminalPlacementDisposition::kNeedsSnapshot, local,
                     gui_workspace_keys, pending_closes, false);
  }
  if (needs_snapshot_) {
    return BuildPlan(CmuxTerminalPlacementDisposition::kNeedsSnapshot, local,
                     gui_workspace_keys, pending_closes, false);
  }
  if (event.terminal_revision <= terminal_revision_) {
    return BuildPlan(CmuxTerminalPlacementDisposition::kIgnored, local,
                     gui_workspace_keys, pending_closes, false);
  }
  if (terminal_revision_ == std::numeric_limits<uint64_t>::max() ||
      event.terminal_revision != terminal_revision_ + 1 ||
      !IsValidEventRecord(event.terminal)) {
    needs_snapshot_ = true;
    return BuildPlan(CmuxTerminalPlacementDisposition::kNeedsSnapshot, local,
                     gui_workspace_keys, pending_closes, false);
  }

  terminal_revision_ = event.terminal_revision;
  if (event.terminal.lifecycle == CmuxTerminalLifecycle::kTombstoned) {
    const auto existing = terminals_.find(event.terminal.terminal_id);
    if (existing != terminals_.end() && !existing->second.incarnation.empty()) {
      retired_incarnations_.insert(IncarnationOf(existing->second));
    }
    terminals_.erase(event.terminal.terminal_id);
    tombstoned_terminal_ids_.insert(event.terminal.terminal_id);
  } else if (tombstoned_terminal_ids_.find(event.terminal.terminal_id) ==
                 tombstoned_terminal_ids_.end() &&
             (event.terminal.incarnation.empty() ||
              retired_incarnations_.find(IncarnationOf(event.terminal)) ==
                  retired_incarnations_.end())) {
    const auto existing = terminals_.find(event.terminal.terminal_id);
    if (existing != terminals_.end() && !existing->second.incarnation.empty() &&
        existing->second.incarnation != event.terminal.incarnation) {
      retired_incarnations_.insert(IncarnationOf(existing->second));
    }
    terminals_.insert_or_assign(event.terminal.terminal_id, event.terminal);
  }

  return BuildPlan(CmuxTerminalPlacementDisposition::kApplied, local,
                   gui_workspace_keys, pending_closes, true);
}

CmuxTerminalPlacementPlan CmuxCanonicalTerminalPlacementModel::Reconcile(
    const std::vector<CmuxLocalTerminalPlacement>& local,
    const std::vector<std::string>& gui_workspace_keys,
    const std::vector<CmuxPendingTerminalClose>& pending_closes) const {
  const bool ready = has_snapshot_ && !needs_snapshot_;
  return BuildPlan(ready ? CmuxTerminalPlacementDisposition::kApplied
                         : CmuxTerminalPlacementDisposition::kNeedsSnapshot,
                   local, gui_workspace_keys, pending_closes, ready);
}

CmuxTerminalPlacementPlan CmuxCanonicalTerminalPlacementModel::BuildPlan(
    CmuxTerminalPlacementDisposition disposition,
    const std::vector<CmuxLocalTerminalPlacement>& local,
    const std::vector<std::string>& gui_workspace_keys,
    const std::vector<CmuxPendingTerminalClose>& pending_closes,
    bool include_actions) const {
  CmuxTerminalPlacementPlan plan;
  plan.disposition = disposition;
  plan.registry_id = registry_id_;
  plan.generation = generation_;
  plan.terminal_revision = terminal_revision_;
  plan.retained_workspace_keys = NormalizeWorkspaceKeys(gui_workspace_keys);
  if (!include_actions || !has_snapshot_ || needs_snapshot_) {
    return plan;
  }

  const std::set<std::string> workspaces(plan.retained_workspace_keys.begin(),
                                         plan.retained_workspace_keys.end());
  std::set<TerminalIncarnation> pending_close_identities;
  std::set<std::string> wildcard_pending_close_ids;
  for (const CmuxPendingTerminalClose& pending_close : pending_closes) {
    if (IsValidPendingClose(pending_close)) {
      if (pending_close.incarnation.empty()) {
        // A close initiated while Launching must survive the Ready race. The
        // client-reserved terminal ID cannot be reused, so null safely means
        // this terminal across its first host-incarnation transition.
        wildcard_pending_close_ids.insert(pending_close.terminal_id);
      } else {
        pending_close_identities.emplace(pending_close.terminal_id,
                                         pending_close.incarnation);
      }
    }
  }
  std::set<std::string> suppressed_terminal_ids;
  for (const auto& entry : terminals_) {
    if (wildcard_pending_close_ids.find(entry.first) !=
            wildcard_pending_close_ids.end() ||
        pending_close_identities.find(IncarnationOf(entry.second)) !=
            pending_close_identities.end()) {
      suppressed_terminal_ids.insert(entry.first);
    }
  }

  std::vector<const CmuxLocalTerminalPlacement*> ordered_local;
  ordered_local.reserve(local.size());
  for (const CmuxLocalTerminalPlacement& placement : local) {
    ordered_local.push_back(&placement);
  }
  std::sort(
      ordered_local.begin(), ordered_local.end(),
      [](const CmuxLocalTerminalPlacement* left,
         const CmuxLocalTerminalPlacement* right) {
        return std::tie(left->local_key, left->terminal_id, left->incarnation,
                        left->workspace_key, left->runtime_surface_id,
                        left->pending_mutation_id, left->create_pending) <
               std::tie(right->local_key, right->terminal_id,
                        right->incarnation, right->workspace_key,
                        right->runtime_surface_id, right->pending_mutation_id,
                        right->create_pending);
      });

  bool has_unresolved_runtime_local = false;
  std::map<std::string, std::vector<const CmuxLocalTerminalPlacement*>>
      local_by_terminal_id;
  for (const CmuxLocalTerminalPlacement* placement : ordered_local) {
    if (suppressed_terminal_ids.find(placement->terminal_id) !=
        suppressed_terminal_ids.end()) {
      plan.actions.push_back(RemoveAction(*placement));
      continue;
    }
    if (!IsCmuxStableTerminalUuid(placement->terminal_id)) {
      has_unresolved_runtime_local = true;
      plan.actions.push_back(ResolveLocalAction(
          *placement, CmuxTerminalResolveReason::kStableIdentityMissing));
      continue;
    }
    local_by_terminal_id[placement->terminal_id].push_back(placement);
  }

  std::map<std::string, std::vector<const CmuxLocalTerminalPlacement*>>
      matching_local;
  for (const auto& entry : local_by_terminal_id) {
    const std::string& terminal_id = entry.first;
    const std::vector<const CmuxLocalTerminalPlacement*>& placements =
        entry.second;
    if (tombstoned_terminal_ids_.find(terminal_id) !=
        tombstoned_terminal_ids_.end()) {
      for (const CmuxLocalTerminalPlacement* placement : placements) {
        plan.actions.push_back(RemoveAction(*placement));
      }
      continue;
    }
    const auto canonical = terminals_.find(terminal_id);
    if (canonical != terminals_.end()) {
      matching_local.emplace(terminal_id, placements);
      continue;
    }

    const CmuxLocalTerminalPlacement* pending_survivor = nullptr;
    for (const CmuxLocalTerminalPlacement* placement : placements) {
      if (!pending_survivor && HasTrackedCreate(*placement)) {
        pending_survivor = placement;
        continue;
      }
      plan.actions.push_back(RemoveAction(*placement));
    }
    if (pending_survivor) {
      plan.actions.push_back(ResolveLocalAction(
          *pending_survivor, CmuxTerminalResolveReason::kPendingMutation));
    }
  }

  for (const auto& entry : terminals_) {
    const CmuxCanonicalTerminalPlacement& terminal = entry.second;
    if (suppressed_terminal_ids.find(terminal.terminal_id) !=
        suppressed_terminal_ids.end()) {
      continue;
    }
    const auto found = matching_local.find(terminal.terminal_id);
    const CmuxLocalTerminalPlacement* survivor = nullptr;
    if (found != matching_local.end() && !found->second.empty()) {
      std::vector<const CmuxLocalTerminalPlacement*> candidates = found->second;
      std::sort(candidates.begin(), candidates.end(),
                [&terminal](const CmuxLocalTerminalPlacement* left,
                            const CmuxLocalTerminalPlacement* right) {
                  return std::make_tuple(
                             BindingRank(*left, terminal),
                             left->workspace_key != terminal.workspace_key,
                             left->local_key, left->incarnation,
                             left->runtime_surface_id) <
                         std::make_tuple(
                             BindingRank(*right, terminal),
                             right->workspace_key != terminal.workspace_key,
                             right->local_key, right->incarnation,
                             right->runtime_surface_id);
                });
      survivor = candidates.front();
      for (size_t index = 1; index < candidates.size(); ++index) {
        plan.actions.push_back(RemoveAction(*candidates[index]));
      }
    }

    if (survivor) {
      if (NeedsCanonicalResolve(*survivor, terminal)) {
        plan.actions.push_back(ResolveCanonicalAction(*survivor, terminal));
      }
      if (workspaces.find(terminal.workspace_key) == workspaces.end()) {
        plan.actions.push_back(
            ResolveWorkspaceAction(terminal, survivor->local_key));
      } else if (survivor->workspace_key != terminal.workspace_key) {
        plan.actions.push_back(MoveAction(*survivor, terminal));
      }
      continue;
    }

    if (workspaces.find(terminal.workspace_key) == workspaces.end()) {
      plan.actions.push_back(ResolveWorkspaceAction(terminal, std::string()));
    } else if (!has_unresolved_runtime_local) {
      plan.actions.push_back(MaterializeAction(terminal));
    }
  }

  std::sort(plan.actions.begin(), plan.actions.end(), ActionLess);
  plan.actions.erase(std::unique(plan.actions.begin(), plan.actions.end()),
                     plan.actions.end());
  return plan;
}

bool IsCmuxStableTerminalUuid(const std::string& value) {
  if (value.size() != 32 || value[12] != '4' ||
      (value[16] != '8' && value[16] != '9' && value[16] != 'a' &&
       value[16] != 'b')) {
    return false;
  }
  for (const char character : value) {
    if (!((character >= '0' && character <= '9') ||
          (character >= 'a' && character <= 'f'))) {
      return false;
    }
  }
  return true;
}

}  // namespace cmux
