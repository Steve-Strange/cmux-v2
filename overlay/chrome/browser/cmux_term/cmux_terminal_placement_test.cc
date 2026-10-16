// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later

// Host-compilable tests for canonical terminal placement. No Chromium or
// gtest dependencies.

#include "chrome/browser/cmux_term/cmux_terminal_placement.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr char kRegistryA[] = "registry-a";
constexpr char kRegistryB[] = "registry-b";
constexpr char kGenerationA[] = "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa";
constexpr char kGenerationB[] = "bbbbbbbb-bbbb-4bbb-9bbb-bbbbbbbbbbbb";
constexpr char kGenerationC[] = "cccccccc-cccc-4ccc-accc-cccccccccccc";

constexpr char kTerminalA[] = "11111111111141118111111111111111";
constexpr char kTerminalB[] = "22222222222242229222222222222222";
constexpr char kTerminalC[] = "3333333333334333a333333333333333";
constexpr char kTerminalD[] = "4444444444444444b444444444444444";
constexpr char kIncarnationA[] = "aaaaaaaaaaaa4aaa8aaaaaaaaaaaaaaa";
constexpr char kIncarnationB[] = "bbbbbbbbbbbb4bbb9bbbbbbbbbbbbbbb";
constexpr char kIncarnationC[] = "cccccccccccc4cccaccccccccccccccc";
constexpr char kIncarnationD[] = "dddddddddddd4dddbddddddddddddddd";

int checks = 0;
int failures = 0;

void Check(bool condition, const char* message) {
  ++checks;
  if (!condition) {
    ++failures;
    std::fprintf(stderr, "FAIL: %s\n", message);
  }
}

cmux::CmuxCanonicalTerminalPlacement Placement(
    const std::string& terminal_id,
    const std::string& incarnation,
    const std::string& workspace_key,
    cmux::CmuxTerminalLifecycle lifecycle) {
  return {terminal_id, incarnation, workspace_key, lifecycle};
}

cmux::CmuxCanonicalTerminalPlacement Launching(
    const std::string& terminal_id,
    const std::string& workspace_key) {
  return Placement(terminal_id, "", workspace_key,
                   cmux::CmuxTerminalLifecycle::kLaunching);
}

cmux::CmuxCanonicalTerminalPlacement Adopting(
    const std::string& terminal_id,
    const std::string& incarnation,
    const std::string& workspace_key) {
  return Placement(terminal_id, incarnation, workspace_key,
                   cmux::CmuxTerminalLifecycle::kAdopting);
}

cmux::CmuxCanonicalTerminalPlacement Running(const std::string& terminal_id,
                                             const std::string& incarnation,
                                             const std::string& workspace_key) {
  return Placement(terminal_id, incarnation, workspace_key,
                   cmux::CmuxTerminalLifecycle::kRunning);
}

cmux::CmuxCanonicalTerminalPlacement Exited(const std::string& terminal_id,
                                            const std::string& incarnation,
                                            const std::string& workspace_key) {
  return Placement(terminal_id, incarnation, workspace_key,
                   cmux::CmuxTerminalLifecycle::kExited);
}

cmux::CmuxCanonicalTerminalPlacement Tombstoned(
    const std::string& terminal_id,
    const std::string& incarnation = std::string()) {
  return Placement(terminal_id, incarnation, "",
                   cmux::CmuxTerminalLifecycle::kTombstoned);
}

cmux::CmuxLocalTerminalPlacement Local(
    const std::string& local_key,
    const std::string& terminal_id,
    const std::string& incarnation,
    const std::string& workspace_key,
    const std::string& runtime_surface_id = std::string(),
    const std::string& pending_mutation_id = std::string(),
    bool create_pending = false) {
  return {local_key,          terminal_id,         incarnation,   workspace_key,
          runtime_surface_id, pending_mutation_id, create_pending};
}

cmux::CmuxPendingTerminalClose PendingClose(const std::string& terminal_id,
                                            const std::string& incarnation,
                                            const std::string& mutation_id) {
  return {terminal_id, incarnation, mutation_id};
}

cmux::CmuxTerminalPlacementSnapshot Snapshot(
    uint64_t revision,
    std::vector<cmux::CmuxCanonicalTerminalPlacement> terminals,
    const std::string& registry_id = kRegistryA,
    const std::string& generation = kGenerationA) {
  return {registry_id, generation, revision, std::move(terminals)};
}

cmux::CmuxTerminalPlacementEvent Event(
    uint64_t revision,
    cmux::CmuxCanonicalTerminalPlacement terminal,
    const std::string& registry_id = kRegistryA,
    const std::string& generation = kGenerationA) {
  return {registry_id, generation, revision, std::move(terminal)};
}

size_t CountKind(const cmux::CmuxTerminalPlacementPlan& plan,
                 cmux::CmuxTerminalPlacementActionKind kind) {
  return static_cast<size_t>(
      std::count_if(plan.actions.begin(), plan.actions.end(),
                    [kind](const cmux::CmuxTerminalPlacementAction& action) {
                      return action.kind == kind;
                    }));
}

const cmux::CmuxTerminalPlacementAction* FindAction(
    const cmux::CmuxTerminalPlacementPlan& plan,
    cmux::CmuxTerminalPlacementActionKind kind,
    const std::string& local_key,
    const std::string& terminal_id) {
  const auto found = std::find_if(
      plan.actions.begin(), plan.actions.end(),
      [&](const cmux::CmuxTerminalPlacementAction& action) {
        return action.kind == kind && action.local_key == local_key &&
               action.terminal_id == terminal_id;
      });
  return found == plan.actions.end() ? nullptr : &*found;
}

bool SamePlan(const cmux::CmuxTerminalPlacementPlan& left,
              const cmux::CmuxTerminalPlacementPlan& right) {
  return left.disposition == right.disposition &&
         left.registry_id == right.registry_id &&
         left.generation == right.generation &&
         left.terminal_revision == right.terminal_revision &&
         left.actions == right.actions &&
         left.retained_workspace_keys == right.retained_workspace_keys;
}

void TestIdentityAndLifecycleMaterialization() {
  Check(cmux::IsCmuxStableTerminalUuid(kTerminalA),
        "compact terminal UUIDv4 is accepted");
  Check(cmux::IsCmuxStableTerminalUuid(kIncarnationD),
        "compact incarnation UUIDv4 is accepted");
  Check(!cmux::IsCmuxStableTerminalUuid("42"),
        "daemon-local numeric surface ID is not stable identity");
  Check(!cmux::IsCmuxStableTerminalUuid("1111111111114111811111111111111A"),
        "uppercase UUID text is rejected");
  Check(!cmux::IsCmuxStableTerminalUuid("11111111111151118111111111111111"),
        "non-v4 terminal UUID is rejected");
  Check(!cmux::IsCmuxStableTerminalUuid("11111111111141117111111111111111"),
        "terminal UUID without RFC variant is rejected");

  cmux::CmuxCanonicalTerminalPlacementModel model;
  const cmux::CmuxTerminalPlacementPlan plan = model.ApplySnapshot(
      Snapshot(5, {Exited(kTerminalB, kIncarnationB, "workspace-b"),
                   Adopting(kTerminalD, kIncarnationD, "workspace-d"),
                   Launching(kTerminalC, "workspace-c"),
                   Running(kTerminalA, kIncarnationA, "workspace-a")}),
      {},
      {"workspace-empty", "workspace-b", "workspace-a", "workspace-c",
       "workspace-d", "workspace-b", ""});
  Check(plan.disposition == cmux::CmuxTerminalPlacementDisposition::kApplied,
        "valid terminal snapshot applies");
  Check(plan.registry_id == kRegistryA && plan.generation == kGenerationA &&
            plan.terminal_revision == 5,
        "plan carries the terminal registry epoch and revision");
  Check(plan.retained_workspace_keys ==
            std::vector<std::string>({"workspace-empty", "workspace-b",
                                      "workspace-a", "workspace-c",
                                      "workspace-d"}),
        "all GUI workspaces, including empty ones, are retained in GUI order");
  Check(
      CountKind(plan, cmux::CmuxTerminalPlacementActionKind::kMaterialize) == 4,
      "every non-tombstoned lifecycle materializes a placeholder");
  Check(plan.actions.size() == 4,
        "initial materialization emits no unrelated actions");

  const auto* running =
      FindAction(plan, cmux::CmuxTerminalPlacementActionKind::kMaterialize, "",
                 kTerminalA);
  const auto* exited =
      FindAction(plan, cmux::CmuxTerminalPlacementActionKind::kMaterialize, "",
                 kTerminalB);
  const auto* launching =
      FindAction(plan, cmux::CmuxTerminalPlacementActionKind::kMaterialize, "",
                 kTerminalC);
  const auto* adopting =
      FindAction(plan, cmux::CmuxTerminalPlacementActionKind::kMaterialize, "",
                 kTerminalD);
  Check(running && running->lifecycle == cmux::CmuxTerminalLifecycle::kRunning,
        "running terminal materialization carries lifecycle");
  Check(exited && exited->lifecycle == cmux::CmuxTerminalLifecycle::kExited,
        "exited terminal remains canonical and materializable");
  Check(launching && launching->incarnation.empty() &&
            launching->lifecycle == cmux::CmuxTerminalLifecycle::kLaunching,
        "launching placeholder does not invent a host incarnation");
  Check(adopting && adopting->incarnation == kIncarnationD &&
            adopting->lifecycle == cmux::CmuxTerminalLifecycle::kAdopting,
        "adopting placeholder carries its known host identity");

  cmux::CmuxCanonicalTerminalPlacementModel failed_launch_model;
  const cmux::CmuxTerminalPlacementPlan failed_launch =
      failed_launch_model.ApplySnapshot(
          Snapshot(1, {Exited(kTerminalA, "", "workspace-a")}), {},
          {"workspace-a"});
  const auto* failed_launch_placeholder = FindAction(
      failed_launch, cmux::CmuxTerminalPlacementActionKind::kMaterialize, "",
      kTerminalA);
  Check(failed_launch_placeholder &&
            failed_launch_placeholder->incarnation.empty() &&
            failed_launch_placeholder->lifecycle ==
                cmux::CmuxTerminalLifecycle::kExited,
        "pre-host launch failure remains an exited canonical placeholder");
}

void TestRuntimeBarrierAndMissingWorkspace() {
  cmux::CmuxCanonicalTerminalPlacementModel model;
  const cmux::CmuxTerminalPlacementPlan plan = model.ApplySnapshot(
      Snapshot(1, {Running(kTerminalA, kIncarnationA, "workspace-a"),
                   Running(kTerminalB, kIncarnationB, "workspace-b")}),
      {Local("runtime-pane", "", "", "workspace-a", "surface-77")},
      {"workspace-a", "workspace-b"});
  const auto* resolve =
      FindAction(plan, cmux::CmuxTerminalPlacementActionKind::kResolve,
                 "runtime-pane", "");
  Check(resolve &&
            resolve->resolve_reason ==
                cmux::CmuxTerminalResolveReason::kStableIdentityMissing &&
            resolve->runtime_surface_id == "surface-77",
        "runtime-only pane resolves through the durable registry");
  Check(
      CountKind(plan, cmux::CmuxTerminalPlacementActionKind::kMaterialize) == 0,
      "unresolved runtime identity blocks duplicate materialization");
  Check(plan.actions.size() == 1,
        "runtime identity barrier does not destructively guess");

  cmux::CmuxCanonicalTerminalPlacementModel missing_workspace_model;
  const cmux::CmuxTerminalPlacementPlan missing =
      missing_workspace_model.ApplySnapshot(
          Snapshot(1,
                   {Running(kTerminalC, kIncarnationC, "workspace-missing")}),
          {}, {"workspace-empty"});
  const auto* workspace_resolve = FindAction(
      missing, cmux::CmuxTerminalPlacementActionKind::kResolve, "", kTerminalC);
  Check(workspace_resolve &&
            workspace_resolve->resolve_reason ==
                cmux::CmuxTerminalResolveReason::kWorkspaceMissing &&
            workspace_resolve->workspace_key == "workspace-missing",
        "terminal placement delegates missing workspace creation");
  Check(CountKind(missing,
                  cmux::CmuxTerminalPlacementActionKind::kMaterialize) == 0,
        "terminal is not materialized into an invented workspace");
  Check(missing.retained_workspace_keys ==
            std::vector<std::string>({"workspace-empty"}),
        "unrelated empty GUI workspace survives missing-workspace resolve");
}

void TestPendingCreateAndTombstoneRace() {
  cmux::CmuxCanonicalTerminalPlacementModel model;
  const cmux::CmuxLocalTerminalPlacement pending =
      Local("pane-a", kTerminalA, "", "workspace-a", "", "mutation-1", true);
  const cmux::CmuxTerminalPlacementPlan before_create =
      model.ApplySnapshot(Snapshot(10, {}), {pending}, {"workspace-a"});
  const auto* pending_action =
      FindAction(before_create, cmux::CmuxTerminalPlacementActionKind::kResolve,
                 "pane-a", kTerminalA);
  Check(pending_action &&
            pending_action->resolve_reason ==
                cmux::CmuxTerminalResolveReason::kPendingMutation &&
            pending_action->pending_mutation_id == "mutation-1",
        "in-flight client-reserved terminal survives an earlier snapshot");
  Check(CountKind(before_create,
                  cmux::CmuxTerminalPlacementActionKind::kRemove) == 0,
        "in-flight create is not removed before command resolution");

  const cmux::CmuxLocalTerminalPlacement duplicate =
      Local("pane-z", kTerminalA, "", "workspace-a", "", "mutation-1", true);
  const cmux::CmuxTerminalPlacementPlan duplicate_pending =
      model.Reconcile({duplicate, pending}, {"workspace-a"});
  Check(FindAction(duplicate_pending,
                   cmux::CmuxTerminalPlacementActionKind::kResolve, "pane-a",
                   kTerminalA) != nullptr,
        "deterministic first pending pane owns a replayed mutation");
  Check(FindAction(duplicate_pending,
                   cmux::CmuxTerminalPlacementActionKind::kRemove, "pane-z",
                   kTerminalA) != nullptr,
        "duplicate pending pane cannot launch the same terminal twice");

  const cmux::CmuxTerminalPlacementPlan launching =
      model.ApplyEvent(Event(11, Launching(kTerminalA, "workspace-a")),
                       {pending}, {"workspace-a"});
  const auto* launching_resolve =
      FindAction(launching, cmux::CmuxTerminalPlacementActionKind::kResolve,
                 "pane-a", kTerminalA);
  Check(launching_resolve &&
            launching_resolve->resolve_reason ==
                cmux::CmuxTerminalResolveReason::kCanonicalBinding &&
            launching_resolve->incarnation.empty() &&
            launching_resolve->lifecycle ==
                cmux::CmuxTerminalLifecycle::kLaunching,
        "launching event claims pending pane without duplicating it");
  Check(CountKind(launching,
                  cmux::CmuxTerminalPlacementActionKind::kMaterialize) == 0,
        "client-reserved ID suppresses launching placeholder duplication");

  const cmux::CmuxTerminalPlacementPlan duplicate_event =
      model.ApplyEvent(Event(11, Launching(kTerminalA, "workspace-a")),
                       {pending}, {"workspace-a"});
  Check(duplicate_event.disposition ==
                cmux::CmuxTerminalPlacementDisposition::kIgnored &&
            duplicate_event.actions.empty(),
        "idempotent replay at the same revision cannot spawn again");

  const cmux::CmuxTerminalPlacementPlan adopting = model.ApplyEvent(
      Event(12, Adopting(kTerminalA, kIncarnationA, "workspace-a")), {pending},
      {"workspace-a"});
  const auto* adopting_resolve =
      FindAction(adopting, cmux::CmuxTerminalPlacementActionKind::kResolve,
                 "pane-a", kTerminalA);
  Check(
      adopting_resolve && adopting_resolve->incarnation == kIncarnationA &&
          adopting_resolve->lifecycle == cmux::CmuxTerminalLifecycle::kAdopting,
      "Ready identity binds onto the existing pending pane");

  const cmux::CmuxLocalTerminalPlacement bound =
      Local("pane-a", kTerminalA, kIncarnationA, "workspace-a");
  const cmux::CmuxTerminalPlacementPlan running = model.ApplyEvent(
      Event(13, Running(kTerminalA, kIncarnationA, "workspace-a")), {bound},
      {"workspace-a"});
  Check(running.actions.empty(),
        "bound running pane needs no projection mutation");
  const cmux::CmuxTerminalPlacementPlan exited = model.ApplyEvent(
      Event(14, Exited(kTerminalA, kIncarnationA, "workspace-a")), {bound},
      {"workspace-a"});
  Check(exited.actions.empty(),
        "already-open exited pane retains its last frame in place");
  const cmux::CmuxTerminalPlacementPlan exited_reopen =
      model.Reconcile({}, {"workspace-a"});
  const auto* exited_materialize = FindAction(
      exited_reopen, cmux::CmuxTerminalPlacementActionKind::kMaterialize, "",
      kTerminalA);
  Check(exited_materialize && exited_materialize->lifecycle ==
                                  cmux::CmuxTerminalLifecycle::kExited,
        "fresh GUI gets an exited placeholder for canonical history");

  const cmux::CmuxTerminalPlacementPlan tombstoned =
      model.ApplyEvent(Event(15, Tombstoned(kTerminalA, kIncarnationA)),
                       {bound}, {"workspace-a"});
  Check(FindAction(tombstoned, cmux::CmuxTerminalPlacementActionKind::kRemove,
                   "pane-a", kTerminalA) != nullptr,
        "explicit tombstone removes the logical terminal pane");
  const cmux::CmuxTerminalPlacementPlan late_running = model.ApplyEvent(
      Event(16, Running(kTerminalA, kIncarnationA, "workspace-a")), {},
      {"workspace-a"});
  Check(late_running.disposition ==
                cmux::CmuxTerminalPlacementDisposition::kApplied &&
            late_running.actions.empty(),
        "late higher-revision completion cannot resurrect a tombstone");

  cmux::CmuxCanonicalTerminalPlacementModel absent_model;
  absent_model.ApplySnapshot(Snapshot(1, {}), {}, {"workspace-a"});
  const cmux::CmuxTerminalPlacementPlan untracked = absent_model.Reconcile(
      {Local("untracked", kTerminalB, "", "workspace-a")}, {"workspace-a"});
  const cmux::CmuxTerminalPlacementPlan missing_mutation =
      absent_model.Reconcile(
          {Local("bad-pending", kTerminalC, "", "workspace-a", "", "", true)},
          {"workspace-a"});
  Check(FindAction(untracked, cmux::CmuxTerminalPlacementActionKind::kRemove,
                   "untracked", kTerminalB) != nullptr,
        "snapshot absence removes an untracked reserved ID");
  Check(FindAction(missing_mutation,
                   cmux::CmuxTerminalPlacementActionKind::kRemove,
                   "bad-pending", kTerminalC) != nullptr,
        "pending flag without idempotency key cannot retain stale state");
}

void TestDuplicateSelectionMoveAndIncarnationRace() {
  const std::vector<cmux::CmuxLocalTerminalPlacement> local = {
      Local("pane-q", kTerminalA, kIncarnationB, "workspace-a"),
      Local("pane-a", kTerminalA, "", "workspace-a", "", "mutation-a", true),
      Local("pane-z", kTerminalA, kIncarnationA, "workspace-b")};
  std::vector<cmux::CmuxLocalTerminalPlacement> reversed = local;
  std::reverse(reversed.begin(), reversed.end());
  cmux::CmuxCanonicalTerminalPlacementModel first;
  cmux::CmuxCanonicalTerminalPlacementModel second;
  const auto snapshot =
      Snapshot(1, {Running(kTerminalA, kIncarnationA, "workspace-a")});
  const cmux::CmuxTerminalPlacementPlan first_plan =
      first.ApplySnapshot(snapshot, local, {"workspace-a", "workspace-b"});
  const cmux::CmuxTerminalPlacementPlan second_plan =
      second.ApplySnapshot(snapshot, reversed, {"workspace-a", "workspace-b"});
  Check(SamePlan(first_plan, second_plan),
        "local input permutation produces an identical placement plan");
  Check(
      FindAction(first_plan, cmux::CmuxTerminalPlacementActionKind::kRemove,
                 "pane-a", kTerminalA) != nullptr &&
          FindAction(first_plan, cmux::CmuxTerminalPlacementActionKind::kRemove,
                     "pane-q", kTerminalA) != nullptr,
      "exact host incarnation wins over pending and stale duplicates");
  Check(FindAction(first_plan, cmux::CmuxTerminalPlacementActionKind::kMove,
                   "pane-z", kTerminalA) != nullptr,
        "surviving terminal immediately moves to canonical workspace");
  Check(CountKind(first_plan,
                  cmux::CmuxTerminalPlacementActionKind::kMaterialize) == 0,
        "duplicate local panes never trigger another materialization");

  cmux::CmuxCanonicalTerminalPlacementModel restart;
  const cmux::CmuxLocalTerminalPlacement old_binding =
      Local("pane", kTerminalA, kIncarnationA, "workspace-a");
  restart.ApplySnapshot(
      Snapshot(20, {Running(kTerminalA, kIncarnationA, "workspace-a")}),
      {old_binding}, {"workspace-a"});
  const cmux::CmuxTerminalPlacementPlan relaunch =
      restart.ApplyEvent(Event(21, Launching(kTerminalA, "workspace-a")),
                         {old_binding}, {"workspace-a"});
  Check(FindAction(relaunch, cmux::CmuxTerminalPlacementActionKind::kResolve,
                   "pane", kTerminalA) != nullptr &&
            CountKind(relaunch,
                      cmux::CmuxTerminalPlacementActionKind::kRemove) == 0,
        "host relaunch rebinds the pane instead of destroying GUI identity");
  const cmux::CmuxTerminalPlacementPlan replacement = restart.ApplyEvent(
      Event(22, Adopting(kTerminalA, kIncarnationB, "workspace-a")),
      {old_binding}, {"workspace-a"});
  const auto* replacement_action =
      FindAction(replacement, cmux::CmuxTerminalPlacementActionKind::kResolve,
                 "pane", kTerminalA);
  Check(replacement_action && replacement_action->incarnation == kIncarnationB,
        "new host incarnation atomically replaces old binding");

  const cmux::CmuxLocalTerminalPlacement new_binding =
      Local("pane", kTerminalA, kIncarnationB, "workspace-a");
  Check(restart
            .ApplyEvent(
                Event(23, Running(kTerminalA, kIncarnationB, "workspace-a")),
                {new_binding}, {"workspace-a"})
            .actions.empty(),
        "new running incarnation settles without pane churn");
  const cmux::CmuxTerminalPlacementPlan delayed_old = restart.ApplyEvent(
      Event(24, Running(kTerminalA, kIncarnationA, "workspace-a")),
      {new_binding}, {"workspace-a"});
  Check(delayed_old.disposition ==
                cmux::CmuxTerminalPlacementDisposition::kApplied &&
            delayed_old.terminal_revision == 24 && delayed_old.actions.empty(),
        "retired incarnation completion advances revision but cannot regress");
  const cmux::CmuxTerminalPlacementPlan stale_snapshot = restart.ApplySnapshot(
      Snapshot(25, {Running(kTerminalA, kIncarnationA, "workspace-a")}),
      {new_binding}, {"workspace-a"});
  Check(stale_snapshot.require_snapshot(),
        "snapshot cannot resurrect a retired host incarnation");
}

void TestOptimisticCloseRace() {
  cmux::CmuxCanonicalTerminalPlacementModel model;
  const cmux::CmuxLocalTerminalPlacement local =
      Local("pane-a", kTerminalA, kIncarnationA, "workspace-a");
  model.ApplySnapshot(
      Snapshot(10, {Running(kTerminalA, kIncarnationA, "workspace-a"),
                    Running(kTerminalB, kIncarnationB, "workspace-b")}),
      {local}, {"workspace-a", "workspace-b"});
  const cmux::CmuxPendingTerminalClose close =
      PendingClose(kTerminalA, kIncarnationA, "close-mutation-1");

  const cmux::CmuxTerminalPlacementPlan immediate =
      model.Reconcile({local}, {"workspace-a", "workspace-b"}, {close});
  Check(FindAction(immediate, cmux::CmuxTerminalPlacementActionKind::kRemove,
                   "pane-a", kTerminalA) != nullptr,
        "in-flight close removes its exact pane immediately");
  Check(
      FindAction(immediate, cmux::CmuxTerminalPlacementActionKind::kMaterialize,
                 "", kTerminalA) == nullptr &&
          FindAction(immediate,
                     cmux::CmuxTerminalPlacementActionKind::kMaterialize, "",
                     kTerminalB) != nullptr,
      "optimistic close suppresses only its exact canonical terminal");
  const cmux::CmuxTerminalPlacementPlan held =
      model.Reconcile({}, {"workspace-a", "workspace-b"}, {close});
  Check(FindAction(held, cmux::CmuxTerminalPlacementActionKind::kMaterialize,
                   "", kTerminalA) == nullptr,
        "absent pane stays absent while exact close mutation is in flight");

  const cmux::CmuxTerminalPlacementPlan failed =
      model.Reconcile({}, {"workspace-a", "workspace-b"});
  Check(FindAction(failed, cmux::CmuxTerminalPlacementActionKind::kMaterialize,
                   "", kTerminalA) != nullptr,
        "failed close restores still-canonical terminal after intent clears");

  const cmux::CmuxTerminalPlacementPlan failed_cas =
      model.Reconcile({}, {"workspace-a", "workspace-b"},
                      {PendingClose(kTerminalA, kIncarnationB, "stale-close")});
  Check(FindAction(failed_cas,
                   cmux::CmuxTerminalPlacementActionKind::kMaterialize, "",
                   kTerminalA) != nullptr,
        "close for another incarnation cannot hide current host");
  const cmux::CmuxTerminalPlacementPlan malformed =
      model.Reconcile({}, {"workspace-a", "workspace-b"},
                      {PendingClose(kTerminalA, kIncarnationA, "")});
  Check(
      FindAction(malformed, cmux::CmuxTerminalPlacementActionKind::kMaterialize,
                 "", kTerminalA) != nullptr,
      "close without idempotency key has no optimistic authority");

  const cmux::CmuxTerminalPlacementPlan confirmed =
      model.ApplyEvent(Event(11, Tombstoned(kTerminalA, kIncarnationA)),
                       {local}, {"workspace-a", "workspace-b"}, {close});
  Check(FindAction(confirmed, cmux::CmuxTerminalPlacementActionKind::kRemove,
                   "pane-a", kTerminalA) != nullptr &&
            FindAction(confirmed,
                       cmux::CmuxTerminalPlacementActionKind::kMaterialize, "",
                       kTerminalA) == nullptr,
        "tombstone confirmation converges optimistic close without flicker");
  const cmux::CmuxTerminalPlacementPlan after_confirmation =
      model.Reconcile({}, {"workspace-a", "workspace-b"});
  Check(FindAction(after_confirmation,
                   cmux::CmuxTerminalPlacementActionKind::kMaterialize, "",
                   kTerminalA) == nullptr,
        "clearing confirmed close intent cannot resurrect tombstoned terminal");

  cmux::CmuxCanonicalTerminalPlacementModel launching_model;
  launching_model.ApplySnapshot(
      Snapshot(1, {Launching(kTerminalC, "workspace-a")}), {}, {"workspace-a"});
  const cmux::CmuxTerminalPlacementPlan close_before_host =
      launching_model.Reconcile(
          {}, {"workspace-a"},
          {PendingClose(kTerminalC, "", "close-launching")});
  Check(close_before_host.actions.empty(),
        "nullable close fingerprint suppresses pre-host launching placeholder");
  const cmux::CmuxTerminalPlacementPlan ready_during_close =
      launching_model.ApplyEvent(
          Event(2, Running(kTerminalC, kIncarnationC, "workspace-a")), {},
          {"workspace-a"}, {PendingClose(kTerminalC, "", "close-launching")});
  Check(FindAction(ready_during_close,
                   cmux::CmuxTerminalPlacementActionKind::kMaterialize, "",
                   kTerminalC) == nullptr,
        "launch-time close survives Ready race without pane resurrection");
}

void TestRevisionAndRegistryEpochFences() {
  cmux::CmuxCanonicalTerminalPlacementModel model;
  model.ApplySnapshot(
      Snapshot(7, {Running(kTerminalA, kIncarnationA, "workspace-a")}), {},
      {"workspace-a", "workspace-b"});
  const cmux::CmuxTerminalPlacementPlan stale = model.ApplyEvent(
      Event(7, Running(kTerminalB, kIncarnationB, "workspace-b")), {},
      {"workspace-a", "workspace-b"});
  Check(stale.disposition == cmux::CmuxTerminalPlacementDisposition::kIgnored &&
            stale.actions.empty(),
        "duplicate terminal revision is ignored without projection churn");

  const cmux::CmuxTerminalPlacementPlan gap = model.ApplyEvent(
      Event(9, Running(kTerminalB, kIncarnationB, "workspace-b")), {},
      {"workspace-a", "workspace-b"});
  Check(gap.require_snapshot() && model.terminal_revision() == 7,
        "revision gap freezes mutations at last causal revision");
  Check(model
            .ApplyEvent(
                Event(8, Running(kTerminalB, kIncarnationB, "workspace-b")), {},
                {"workspace-a", "workspace-b"})
            .require_snapshot(),
        "later event cannot repair a revision gap without a snapshot");
  Check(
      model
          .ApplySnapshot(
              Snapshot(7, {Running(kTerminalA, kIncarnationA, "workspace-a")}),
              {}, {"workspace-a", "workspace-b"})
          .require_snapshot(),
      "equal snapshot does not cover a missing revision");
  const cmux::CmuxTerminalPlacementPlan recovered = model.ApplySnapshot(
      Snapshot(9, {Running(kTerminalA, kIncarnationA, "workspace-a"),
                   Running(kTerminalB, kIncarnationB, "workspace-b")}),
      {}, {"workspace-a", "workspace-b"});
  Check(recovered.disposition ==
                cmux::CmuxTerminalPlacementDisposition::kApplied &&
            !model.needs_snapshot() && model.terminal_revision() == 9,
        "covering snapshot repairs a revision gap");

  const cmux::CmuxTerminalPlacementPlan wrong_generation = model.ApplyEvent(
      Event(10, Running(kTerminalC, kIncarnationC, "workspace-a"), kRegistryA,
            kGenerationB),
      {}, {"workspace-a", "workspace-b"});
  Check(
      wrong_generation.require_snapshot() && model.generation() == kGenerationA,
      "event from another daemon generation cannot mutate current state");
  const cmux::CmuxTerminalPlacementPlan restarted = model.ApplySnapshot(
      Snapshot(0, {Running(kTerminalB, kIncarnationB, "workspace-b")},
               kRegistryA, kGenerationB),
      {}, {"workspace-a", "workspace-b"});
  Check(restarted.disposition ==
                cmux::CmuxTerminalPlacementDisposition::kApplied &&
            model.generation() == kGenerationB &&
            model.terminal_revision() == 0,
        "new daemon generation accepts its independent lower revision");

  Check(model
            .ApplyEvent(
                Event(1, Running(kTerminalC, kIncarnationC, "workspace-a"),
                      kRegistryA, kGenerationA),
                {}, {"workspace-a", "workspace-b"})
            .require_snapshot(),
        "late event from superseded daemon generation fails closed");
  model.ApplySnapshot(
      Snapshot(1, {Running(kTerminalB, kIncarnationB, "workspace-b")},
               kRegistryA, kGenerationB),
      {}, {"workspace-a", "workspace-b"});

  const cmux::CmuxTerminalPlacementPlan reused_socket = model.ApplySnapshot(
      Snapshot(0, {Running(kTerminalA, kIncarnationA, "workspace-a")},
               kRegistryB, kGenerationC),
      {}, {"workspace-a", "workspace-b"});
  Check(reused_socket.disposition ==
                cmux::CmuxTerminalPlacementDisposition::kApplied &&
            model.registry_id() == kRegistryB &&
            model.generation() == kGenerationC,
        "different durable registry replaces state after socket reuse");
  const cmux::CmuxTerminalPlacementPlan equal = model.ApplySnapshot(
      Snapshot(0, {Running(kTerminalA, kIncarnationA, "workspace-a")},
               kRegistryB, kGenerationC),
      {}, {"workspace-a"});
  Check(equal.disposition == cmux::CmuxTerminalPlacementDisposition::kIgnored &&
            equal.actions.empty(),
        "equal authoritative snapshot is idempotent");

  const cmux::CmuxTerminalPlacementPlan malformed_event =
      model.ApplyEvent(Event(1, Adopting(kTerminalA, "", "workspace-a"),
                             kRegistryB, kGenerationC),
                       {}, {"workspace-a"});
  Check(malformed_event.require_snapshot() && model.terminal_revision() == 0,
        "malformed exact-next event cannot advance causal cursor");
  Check(
      model.ApplySnapshot(
               Snapshot(1, {Running(kTerminalA, kIncarnationA, "workspace-a")},
                        kRegistryB, kGenerationC),
               {}, {"workspace-a"})
              .disposition == cmux::CmuxTerminalPlacementDisposition::kApplied,
      "newer valid snapshot recovers malformed event");
}

void TestSnapshotValidationAndAuthoritativeAbsence() {
  cmux::CmuxCanonicalTerminalPlacementModel duplicate_model;
  const cmux::CmuxCanonicalTerminalPlacement duplicate =
      Running(kTerminalA, kIncarnationA, "workspace-a");
  const cmux::CmuxTerminalPlacementPlan duplicate_plan =
      duplicate_model.ApplySnapshot(Snapshot(1, {duplicate, duplicate}), {},
                                    {"workspace-a"});
  Check(duplicate_plan.disposition ==
                cmux::CmuxTerminalPlacementDisposition::kApplied &&
            CountKind(duplicate_plan,
                      cmux::CmuxTerminalPlacementActionKind::kMaterialize) == 1,
        "identical duplicate snapshot record is harmless and deduplicated");

  cmux::CmuxCanonicalTerminalPlacementModel conflict_model;
  Check(
      conflict_model
          .ApplySnapshot(
              Snapshot(1, {Running(kTerminalA, kIncarnationA, "workspace-a"),
                           Running(kTerminalA, kIncarnationB, "workspace-b")}),
              {}, {"workspace-a", "workspace-b"})
          .require_snapshot(),
      "conflicting canonical records fail closed");
  cmux::CmuxCanonicalTerminalPlacementModel tombstone_snapshot_model;
  Check(tombstone_snapshot_model
            .ApplySnapshot(Snapshot(1, {Tombstoned(kTerminalA)}), {},
                           {"workspace-a"})
            .require_snapshot(),
        "event-only tombstone is rejected from ordinary snapshot");
  cmux::CmuxCanonicalTerminalPlacementModel launching_incarnation_model;
  Check(
      launching_incarnation_model
          .ApplySnapshot(
              Snapshot(1, {Placement(kTerminalA, kIncarnationA, "workspace-a",
                                     cmux::CmuxTerminalLifecycle::kLaunching)}),
              {}, {"workspace-a"})
          .require_snapshot(),
      "launching snapshot cannot claim a premature host identity");
  cmux::CmuxCanonicalTerminalPlacementModel adopting_without_host_model;
  Check(
      adopting_without_host_model
          .ApplySnapshot(Snapshot(1, {Adopting(kTerminalA, "", "workspace-a")}),
                         {}, {"workspace-a"})
          .require_snapshot(),
      "adopting snapshot requires a host incarnation");
  cmux::CmuxCanonicalTerminalPlacementModel empty_epoch_model;
  Check(
      empty_epoch_model
          .ApplySnapshot({"",
                          kGenerationA,
                          1,
                          {Running(kTerminalA, kIncarnationA, "workspace-a")}},
                         {}, {"workspace-a"})
          .require_snapshot(),
      "snapshot without durable registry identity fails closed");

  cmux::CmuxCanonicalTerminalPlacementModel absence;
  const cmux::CmuxLocalTerminalPlacement local =
      Local("pane", kTerminalA, kIncarnationA, "workspace-a");
  absence.ApplySnapshot(
      Snapshot(1, {Running(kTerminalA, kIncarnationA, "workspace-a")}), {local},
      {"workspace-a"});
  const cmux::CmuxTerminalPlacementPlan removed =
      absence.ApplySnapshot(Snapshot(2, {}), {local}, {"workspace-a"});
  Check(
      removed.disposition == cmux::CmuxTerminalPlacementDisposition::kApplied &&
          FindAction(removed, cmux::CmuxTerminalPlacementActionKind::kRemove,
                     "pane", kTerminalA) != nullptr,
      "authoritative snapshot absence removes prior canonical terminal");
  const cmux::CmuxTerminalPlacementPlan late = absence.ApplyEvent(
      Event(3, Running(kTerminalA, kIncarnationA, "workspace-a")), {},
      {"workspace-a"});
  Check(late.disposition == cmux::CmuxTerminalPlacementDisposition::kApplied &&
            late.actions.empty(),
        "omitted terminal is internally tombstoned against late completion");
  Check(
      absence
          .ApplySnapshot(
              Snapshot(4, {Running(kTerminalA, kIncarnationA, "workspace-a")}),
              {}, {"workspace-a"})
          .require_snapshot(),
      "same registry cannot reintroduce an authoritatively absent ID");
  const cmux::CmuxTerminalPlacementPlan new_generation = absence.ApplySnapshot(
      Snapshot(0, {Running(kTerminalA, kIncarnationA, "workspace-a")},
               kRegistryA, kGenerationB),
      {}, {"workspace-a"});
  Check(new_generation.require_snapshot(),
        "new daemon generation cannot resurrect durable tombstoned ID");
  const cmux::CmuxTerminalPlacementPlan new_registry = absence.ApplySnapshot(
      Snapshot(0, {Running(kTerminalA, kIncarnationA, "workspace-a")},
               kRegistryB, kGenerationB),
      {}, {"workspace-a"});
  Check(new_registry.disposition ==
                cmux::CmuxTerminalPlacementDisposition::kApplied &&
            FindAction(new_registry,
                       cmux::CmuxTerminalPlacementActionKind::kMaterialize, "",
                       kTerminalA) != nullptr,
        "new durable registry owns an independent terminal ID namespace");

  cmux::CmuxCanonicalTerminalPlacementModel no_snapshot;
  Check(no_snapshot
            .ApplyEvent(Event(1, Launching(kTerminalA, "workspace-a")), {},
                        {"workspace-a"})
            .require_snapshot(),
        "event cannot bootstrap terminal authority without a snapshot");
}

}  // namespace

int main() {
  TestIdentityAndLifecycleMaterialization();
  TestRuntimeBarrierAndMissingWorkspace();
  TestPendingCreateAndTombstoneRace();
  TestDuplicateSelectionMoveAndIncarnationRace();
  TestOptimisticCloseRace();
  TestRevisionAndRegistryEpochFences();
  TestSnapshotValidationAndAuthoritativeAbsence();

  std::printf("cmux-terminal-placement: %d checks, %d failures\n", checks,
              failures);
  return failures == 0 ? 0 : 1;
}
