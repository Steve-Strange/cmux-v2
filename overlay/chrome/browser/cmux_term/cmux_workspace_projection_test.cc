// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later

// Host-compilable tests for canonical workspace projection. No Chromium or
// gtest dependencies.

#include "chrome/browser/cmux_term/cmux_workspace_projection.h"

#include <cstdio>
#include <set>
#include <string>
#include <vector>

namespace {

int checks = 0;
int failures = 0;

void Check(bool condition, const char* message) {
  ++checks;
  if (!condition) {
    ++failures;
    std::fprintf(stderr, "FAIL: %s\n", message);
  }
}

}  // namespace

int main() {
  using cmux::CmuxCanonicalWorkspace;
  using cmux::CmuxLocalWorkspace;
  using cmux::CmuxPendingWorkspaceRename;
  using cmux::CmuxPendingWorkspaceMove;
  using cmux::CmuxPendingWorkspaceMutations;
  using cmux::CmuxWorkspaceProjection;
  using cmux::CmuxWorkspaceProjectionEntry;
  using cmux::CmuxWorkspaceProjectionReloadBarrier;

  {
    CmuxPendingWorkspaceMutations pending;
    Check(pending.RememberCreate("create", "create-mutation-1") &&
              !pending.RememberCreate("create", "create-mutation-2") &&
              pending.creates().at("create") == "create-mutation-1",
          "retrying a pending create retains its first mutation ID");
    Check(pending.RememberClose("close", "close-mutation-1") &&
              !pending.RememberClose("close", "close-mutation-2") &&
              pending.closes().at("close") == "close-mutation-1",
          "retrying a pending close retains its first mutation ID");
    Check(pending.RememberRename("rename", "First", "rename-mutation-1") &&
              !pending.RememberRename("rename", "First",
                                      "rename-mutation-2") &&
              pending.renames().at("rename") ==
                  CmuxPendingWorkspaceRename{"First", "rename-mutation-1"},
          "retrying an unchanged rename retains its first mutation ID");
    Check(pending.RememberRename("rename", "Second", "rename-mutation-3") &&
              pending.renames().at("rename") ==
                  CmuxPendingWorkspaceRename{"Second", "rename-mutation-3"},
          "a changed rename starts a new mutation intent");
    pending.SetMove("move", 3, "move-mutation-1");
    pending.SetMove("move", 3, "move-mutation-2");
    Check(pending.move() ==
              CmuxPendingWorkspaceMove{"move", 3, "move-mutation-1"},
          "retrying an unchanged move retains its first mutation ID");
    pending.SetMove("move", 4, "move-mutation-3");
    Check(pending.move() ==
              CmuxPendingWorkspaceMove{"move", 4, "move-mutation-3"},
          "a changed move target starts a new mutation intent");
    Check(pending.CreateKeys() == std::set<std::string>{"create"} &&
              pending.CloseKeys() == std::set<std::string>{"close"},
          "pending registry reconciliation exposes keyed intent sets");
    pending.ForgetCreate("create");
    pending.ForgetClose("close");
    pending.ForgetRename("rename");
    pending.ForgetMove();
    Check(pending.creates().empty() && pending.closes().empty() &&
              pending.renames().empty() && !pending.move(),
          "confirmed workspace intents leave no retry state");
    Check(!pending.RememberCreate("", "mutation") &&
              !pending.RememberClose("workspace", "") &&
              !pending.RememberRename("", "Name", "mutation") &&
              !pending.RememberRename("workspace", "", "mutation") &&
              !pending.RememberRename("workspace", "Name", "") &&
              pending.creates().empty() && pending.closes().empty() &&
              pending.renames().empty(),
          "invalid pending workspace intents fail closed");
    pending.RememberRename("rename", "Third", "rename-mutation-4");
    pending.Clear();
    Check(pending.renames().empty(),
          "registry replacement clears pending rename intents");
  }

  {
    const cmux::CmuxFrontendIdentity first = cmux::BuildCmuxFrontendIdentity(
        "/Users/test/Library/Application Support/cmux/Default", "normal",
        "primary");
    const cmux::CmuxFrontendIdentity again = cmux::BuildCmuxFrontendIdentity(
        "/Users/test/Library/Application Support/cmux/Default", "normal",
        "primary");
    const cmux::CmuxFrontendIdentity other_profile =
        cmux::BuildCmuxFrontendIdentity("/tmp/another-profile", "normal",
                                        "primary");
    const cmux::CmuxFrontendIdentity incognito =
        cmux::BuildCmuxFrontendIdentity(
            "/Users/test/Library/Application Support/cmux/Default",
            "incognito", "primary");
    const cmux::CmuxFrontendIdentity other_group =
        cmux::BuildCmuxFrontendIdentity(
            "/Users/test/Library/Application Support/cmux/Default", "normal",
            "window-2");
    Check(first == again, "durable identity is deterministic");
    Check(first.session != other_profile.session &&
              first.subject_key != other_profile.subject_key,
          "profiles have isolated daemon sessions and scopes");
    Check(first.session != incognito.session &&
              first.subject_key != incognito.subject_key,
          "normal and incognito profiles cannot share registry state");
    Check(first.session != other_group.session &&
              first.subject_key != other_group.subject_key,
          "window groups have isolated registry state");
    Check(first.frontend == "cmux-browser" &&
              first.scope == "window-group" &&
              first.session.rfind("cmux-browser-", 0) == 0,
          "identity uses the cmux frontend protocol namespace");
    Check(first.subject_key.find("Application Support") == std::string::npos,
          "profile paths never cross the protocol boundary");
    Check(first.session.size() == std::string("cmux-browser-").size() + 32,
          "durable identity uses a 128-bit opaque token");

    const std::string primary = cmux::BuildCmuxPrimaryWindowGroupId(
        "/Users/test/Library/Application Support/cmux/Default", "normal");
    Check(cmux::IsCmuxUuid(primary) &&
              primary == cmux::BuildCmuxPrimaryWindowGroupId(
                             "/Users/test/Library/Application Support/cmux/Default",
                             "normal"),
          "primary window group is a deterministic profile UUID");
    Check(primary != cmux::BuildCmuxPrimaryWindowGroupId(
                         "/Users/test/Library/Application Support/cmux/Default",
                         "incognito"),
          "privacy modes have distinct primary window groups");
  }

  {
    const std::vector<CmuxCanonicalWorkspace> canonical = {
        {"a", "Canonical"}, {"b", "Beta"}};
    const std::vector<CmuxLocalWorkspace> optimistic = {
        {"a", "Desired"}, {"b", "Beta"}};
    CmuxPendingWorkspaceMutations pending;
    pending.RememberRename("a", "Desired", "rename-mutation");
    const cmux::CmuxWorkspaceReconcilePlan stale_plan =
        cmux::BuildCmuxWorkspaceReconcilePlan(
            canonical, optimistic, {}, {}, pending.renames());
    Check(stale_plan.rename.empty(),
          "a stale canonical snapshot cannot roll back an optimistic rename");

    const std::vector<CmuxCanonicalWorkspace> confirmed = {
        {"a", "Desired"}, {"b", "Beta"}};
    const cmux::CmuxWorkspaceReconcilePlan confirmed_plan =
        cmux::BuildCmuxWorkspaceReconcilePlan(
            confirmed, optimistic, {}, {}, pending.renames());
    Check(confirmed_plan.rename.empty(),
          "a confirming rename snapshot needs no local correction");

    const std::vector<CmuxLocalWorkspace> divergent = {
        {"a", "Unexpected"}, {"b", "Beta"}};
    const cmux::CmuxWorkspaceReconcilePlan divergent_plan =
        cmux::BuildCmuxWorkspaceReconcilePlan(
            canonical, divergent, {}, {}, pending.renames());
    Check(divergent_plan.rename ==
              std::vector<std::pair<std::string, std::string>>{
                  {"a", "Canonical"}},
          "pending rename suppression applies only to its exact desired name");

    const std::vector<CmuxCanonicalWorkspace> absent = {{"b", "Beta"}};
    const cmux::CmuxWorkspaceReconcilePlan absent_plan =
        cmux::BuildCmuxWorkspaceReconcilePlan(
            absent, optimistic, {}, {}, pending.renames());
    Check(absent_plan.remove == std::vector<std::string>{"a"},
          "a pending rename cannot preserve a canonically absent workspace");
  }

  {
    const cmux::CmuxDurableWorkspaceAddress first{
        "11111111-1111-4111-8111-111111111111",
        "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"};
    const cmux::CmuxDurableWorkspaceAddress second{
        "22222222-2222-4222-8222-222222222222",
        "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"};
    const std::string first_token = cmux::CmuxDurableWorkspaceToken(first);
    const std::string second_token = cmux::CmuxDurableWorkspaceToken(second);
    Check(cmux::CmuxDurableWorkspaceFromToken(first_token) == first,
          "v2 durable workspace token round-trips");
    Check(first_token != second_token &&
              cmux::CmuxDurableWorkspaceFromToken(second_token) == second,
          "same workspace UUID restores into distinct window groups");
    Check(!cmux::CmuxDurableWorkspaceFromToken(
               "cmux-workspace:v2:not-a-uuid:also-not-a-uuid"),
          "malformed v2 tokens fail closed");
    Check(cmux::CmuxDurableWorkspaceToken({"bad", "bad"}).empty(),
          "invalid durable addresses are never persisted");
    Check(cmux::CmuxDurableWorkspaceToken(
              {first.window_group,
               "frontend-scaling-796e1f60d5144959aa6fafa782410bbb"})
              .empty(),
          "descriptive canonical keys cannot poison durable workspace hosts");
  }

  {
    Check(cmux::IsCurrentCmuxWorkspaceProjectionEvent(
              "registry", "generation", "registry", "generation"),
          "projection events from the active registry generation are accepted");
    Check(!cmux::IsCurrentCmuxWorkspaceProjectionEvent(
               "registry", "generation", "", "") &&
              !cmux::IsCurrentCmuxWorkspaceProjectionEvent(
                  "registry", "generation", "other", "generation") &&
              !cmux::IsCurrentCmuxWorkspaceProjectionEvent(
                  "registry", "generation", "registry", "restarted"),
          "pre-snapshot and stale-generation projection events fail closed");
  }

  {
    CmuxWorkspaceProjectionReloadBarrier reloads(7);
    reloads.Observe(7, 2);
    reloads.Observe(7, 4);
    Check(!reloads.TakeReload(7, 1, true) &&
              reloads.max_observed_revision() == 4,
          "in-flight projection work retains the maximum invalidation");
    Check(reloads.TakeReload(7, 1, false) &&
              reloads.max_scheduled_revision() == 4,
          "completion drains one reload for the newest blocked revision");
    Check(!reloads.TakeReload(7, 1, false),
          "one observed maximum schedules exactly one reload");

    // A newer event racing that follow-up GET needs one more barrier fetch;
    // receiving an older result for the already-scheduled target must not
    // spin indefinitely.
    reloads.Observe(7, 5);
    Check(!reloads.TakeReload(7, 4, true) &&
              reloads.TakeReload(7, 4, false) &&
              !reloads.TakeReload(7, 4, false),
          "a newer in-flight event drains once after request completion");

    reloads.Observe(6, 99);
    Check(reloads.max_observed_revision() == 5,
          "stale generations cannot raise the reload target");
    reloads.Reset(8);
    Check(reloads.generation() == 8 &&
              reloads.max_observed_revision() == 0 &&
              !reloads.TakeReload(7, 0, false),
          "generation replacement clears pending invalidations and old "
          "completions");
    reloads.Observe(8, 3);
    Check(reloads.TakeReload(8, 0, false) &&
              !reloads.TakeReload(8, 2, false),
          "a lower response cannot loop an already scheduled generation "
          "target");
  }

  {
    Check(cmux::IsNextCmuxWorkspaceRevision("registry", "generation", 7,
                                            "registry", "generation", 8),
          "exact next revision in one registry epoch projects immediately");
    Check(!cmux::IsNextCmuxWorkspaceRevision("registry", "generation", 7,
                                             "registry", "generation", 9),
          "revision gaps wait for the authoritative barrier");
    Check(!cmux::IsNextCmuxWorkspaceRevision("registry", "generation", 7,
                                             "other", "generation", 8),
          "events from another durable registry cannot mutate the GUI");
    Check(!cmux::IsNextCmuxWorkspaceRevision("registry", "generation", 7,
                                             "registry", "restarted", 8),
          "events from another boot generation cannot mutate the GUI");
  }

  {
    const std::vector<CmuxCanonicalWorkspace> canonical = {
        {"a", "Alpha"}, {"b", "Beta"}, {"c", "Gamma"}};
    const std::vector<CmuxLocalWorkspace> local = {
        {"a", "old alpha"}, {"local-create", "Pending"}, {"gone", "Gone"}};
    const cmux::CmuxWorkspaceReconcilePlan plan =
        cmux::BuildCmuxWorkspaceReconcilePlan(
            canonical, local, std::set<std::string>{"local-create"},
            std::set<std::string>{"c"});
    Check(plan.materialize ==
              std::vector<CmuxCanonicalWorkspace>{{"b", "Beta"}},
          "every missing canonical workspace is materialized unless closing");
    Check(plan.rename ==
              std::vector<std::pair<std::string, std::string>>{
                  {"a", "Alpha"}},
          "canonical names immediately replace projected names");
    Check(plan.remove == std::vector<std::string>{"gone"},
          "stale projected workspaces are removed");
    Check(plan.canonical_order ==
              std::vector<std::string>({"a", "b", "c"}),
          "canonical order is preserved");
  }

  {
    const std::vector<CmuxCanonicalWorkspace> canonical = {
        {"a", "Alpha"}, {"b", "Beta"}, {"c", "Gamma"}, {"b", "duplicate"}};
    CmuxWorkspaceProjection candidate;
    candidate.selected_key = "stale";
    candidate.workspaces = {
        {"a", "b", false, 2, true, "Focus"},
        {"b", "a", true, 99, false, std::nullopt},
        {"b", "", false, 4, true, std::nullopt},
        {"stale", "", false, 3, true, std::nullopt},
    };
    const CmuxWorkspaceProjection normalized =
        cmux::NormalizeCmuxWorkspaceProjection(canonical, candidate);
    Check(normalized.selected_key == "a",
          "stale selection falls back to first canonical workspace");
    Check(normalized.workspaces.size() == 3,
          "projection contains exactly one entry per canonical key");
    Check(normalized.workspaces[0] ==
              CmuxWorkspaceProjectionEntry{"a", "", false, 2, true, "Focus"},
          "normalization breaks parent cycles without losing group titles");
    Check(normalized.workspaces[1] ==
              CmuxWorkspaceProjectionEntry{"b", "a", true, 0, false,
                                           std::nullopt},
          "valid parent survives and invalid color is reset");
    Check(normalized.workspaces[2] ==
              CmuxWorkspaceProjectionEntry{"c", "", true, 0, false,
                                           std::nullopt},
          "empty canonical workspaces receive browser projection defaults");
  }

  {
    Check(cmux::BuildCmuxWorkspaceOrderMoves(
              {"a", "b", "c", "d"}, {"c", "d", "a", "b"}) ==
              std::vector<std::pair<std::string, size_t>>{
                  {"c", 0}, {"d", 1}},
          "rightward block moves serialize as a stable left-to-right prefix");
    Check(cmux::BuildCmuxWorkspaceOrderMoves(
              {"a", "b"}, {"a", "new", "b"}) ==
              std::vector<std::pair<std::string, size_t>>{{"new", 1}},
          "a pending create receives its final canonical insertion index");
    Check(cmux::BuildCmuxWorkspaceOrderMoves(
              {"a", "b", "c"}, {"a", "b", "c"}).empty(),
          "an already canonical workspace order emits no CAS moves");
  }

  {
    const std::vector<CmuxCanonicalWorkspace> canonical = {
        {"a", "Alpha"}, {"b", "Beta"}, {"c", "Gamma"}};
    CmuxWorkspaceProjection legacy;
    legacy.workspaces = {
        {"a", "b", true, 0, false, std::nullopt},
        {"b", "c", true, 0, false, std::nullopt},
        {"c", "", true, 0, false, std::nullopt},
    };
    const CmuxWorkspaceProjection normalized =
        cmux::NormalizeCmuxWorkspaceProjection(canonical, legacy);
    Check(normalized.workspaces[0].parent_key == "c" &&
              normalized.workspaces[1].parent_key == "c" &&
              normalized.workspaces[2].parent_key.empty(),
          "legacy nested projections flatten to one shallow representative");
  }

  std::printf("cmux-workspace-projection: %d checks, %d failures\n", checks,
              failures);
  return failures == 0 ? 0 : 1;
}
