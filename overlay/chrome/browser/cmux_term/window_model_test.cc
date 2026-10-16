// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Host-compilable unit test for the pure WindowModel (no Chromium, no gtest).
// Build + run locally without the ~10-minute builder:
//
//   c++ -std=c++17 -I overlay \
//     overlay/chrome/browser/cmux_term/window_model.cc \
//     overlay/chrome/browser/cmux_term/window_model_test.cc \
//     -o /tmp/window_test && /tmp/window_test

#include "chrome/browser/cmux_term/window_model.h"

#include <algorithm>
#include <cstdio>
#include <initializer_list>
#include <set>
#include <string>

using namespace cmux;

namespace {

int g_failures = 0;
int g_checks = 0;

void Check(bool ok, const char* what) {
  ++g_checks;
  if (!ok) {
    ++g_failures;
    std::printf("  FAIL: %s\n", what);
  }
}

// Count panes in a workspace's tree.
size_t PaneCount(const WindowModel& m, WorkspaceId ws) {
  return m.PanesOf(ws).size();
}

bool CheckNodeInvariants(const LayoutNode& node,
                         std::set<SurfaceTabId>* seen_tabs) {
  if (node.is_pane()) {
    const Pane& pane = *node.pane;
    if (pane.tabs.empty()) {
      return false;
    }
    bool selected_found = false;
    for (const SurfaceTab& tab : pane.tabs) {
      if (!seen_tabs->insert(tab.id).second) {
        return false;
      }
      if (tab.id == pane.selected) {
        selected_found = true;
      }
    }
    return selected_found;
  }
  return CheckNodeInvariants(node.split->first, seen_tabs) &&
         CheckNodeInvariants(node.split->second, seen_tabs);
}

void CheckInvariants(const WindowModel& m, const char* what) {
  std::set<SurfaceTabId> seen_tabs;
  bool ok = true;
  std::set<WorkspaceId> rail_workspaces;
  std::set<WorkspaceGroupId> completed_group_runs;
  WorkspaceGroupId current_group = kInvalidId;
  ok = ok && m.roots().size() == m.workspace_count();
  for (WorkspaceId id : m.roots()) {
    const Workspace* workspace = m.GetWorkspace(id);
    ok = ok && workspace && rail_workspaces.insert(id).second;
    if (!workspace) {
      continue;
    }
    ok = ok && (workspace->group == kInvalidId ||
                m.GetWorkspaceGroup(workspace->group));
    if (workspace->group == current_group) {
      continue;
    }
    if (current_group != kInvalidId) {
      completed_group_runs.insert(current_group);
    }
    current_group = workspace->group;
    ok = ok && (current_group == kInvalidId ||
                completed_group_runs.find(current_group) ==
                    completed_group_runs.end());
  }
  std::set<WorkspaceGroupId> group_ids;
  for (const WorkspaceGroup& group : m.workspace_groups()) {
    ok = ok && group_ids.insert(group.id).second &&
         !m.WorkspacesInGroup(group.id).empty();
  }
  for (const Workspace& ws : m.workspaces()) {
    ok = ok && rail_workspaces.find(ws.id) != rail_workspaces.end();
    ok = ok && !ws.columns.empty();
    ok = ok && ws.columns.size() == ws.column_widths.size();
    bool focused_found = false;
    for (const LayoutNode& column : ws.columns) {
      ok = ok && CheckNodeInvariants(column, &seen_tabs);
    }
    for (PaneId pane : m.PanesOf(ws.id)) {
      if (pane == ws.focused) {
        focused_found = true;
      }
    }
    ok = ok && focused_found;
  }
  const std::vector<WorkspaceId> selection = m.selected_workspaces();
  if (m.workspace_count() == 0) {
    ok = ok && selection.empty() &&
         m.selected_workspace() == kInvalidId &&
         m.workspace_selection_anchor() == kInvalidId;
  } else {
    std::set<WorkspaceId> unique_selection(selection.begin(), selection.end());
    ok = ok && !selection.empty() &&
         unique_selection.size() == selection.size() &&
         m.GetWorkspace(m.selected_workspace()) &&
         m.GetWorkspace(m.workspace_selection_anchor()) &&
         m.IsWorkspaceSelected(m.selected_workspace()) &&
         m.IsWorkspaceSelected(m.workspace_selection_anchor());
    for (WorkspaceId selected : selection) {
      ok = ok && m.GetWorkspace(selected) != nullptr &&
           m.IsWorkspaceSelected(selected);
    }
  }
  Check(ok, what);
}

bool SelectionIs(const WindowModel& m,
                 std::initializer_list<WorkspaceId> expected) {
  return m.selected_workspaces() == std::vector<WorkspaceId>(expected);
}

std::string TabOrder(const Pane* pane) {
  std::string out;
  if (!pane) {
    return out;
  }
  for (const SurfaceTab& tab : pane->tabs) {
    if (!out.empty()) {
      out += ",";
    }
    out += tab.title;
  }
  return out;
}

template <typename Id>
Id DuplicateIdFor(const std::vector<std::pair<Id, Id>>& mappings, Id source) {
  const auto match =
      std::find_if(mappings.begin(), mappings.end(),
                   [&](const auto& ids) { return ids.first == source; });
  return match == mappings.end() ? kInvalidId : match->second;
}

bool EquivalentDuplicateNode(const LayoutNode& source,
                             const LayoutNode& duplicate,
                             const DuplicateWorkspaceResult& result) {
  if (source.is_pane() != duplicate.is_pane() ||
      source.is_split() != duplicate.is_split()) {
    return false;
  }
  if (source.is_split()) {
    return source.split->id != duplicate.split->id &&
           source.split->orientation == duplicate.split->orientation &&
           source.split->ratio == duplicate.split->ratio &&
           EquivalentDuplicateNode(source.split->first, duplicate.split->first,
                                   result) &&
           EquivalentDuplicateNode(source.split->second,
                                   duplicate.split->second, result);
  }

  if (DuplicateIdFor(result.panes, source.pane->id) != duplicate.pane->id ||
      source.pane->tabs.size() != duplicate.pane->tabs.size() ||
      DuplicateIdFor(result.tabs, source.pane->selected) !=
          duplicate.pane->selected) {
    return false;
  }
  for (size_t i = 0; i < source.pane->tabs.size(); ++i) {
    const SurfaceTab& source_tab = source.pane->tabs[i];
    const SurfaceTab& duplicate_tab = duplicate.pane->tabs[i];
    if (DuplicateIdFor(result.tabs, source_tab.id) != duplicate_tab.id ||
        source_tab.kind != duplicate_tab.kind ||
        source_tab.title != duplicate_tab.title ||
        source_tab.loading != duplicate_tab.loading) {
      return false;
    }
  }
  return true;
}

void CollectNodeIds(const LayoutNode& node, std::set<int64_t>* ids) {
  if (node.is_split()) {
    ids->insert(node.split->id);
    CollectNodeIds(node.split->first, ids);
    CollectNodeIds(node.split->second, ids);
    return;
  }
  ids->insert(node.pane->id);
  for (const SurfaceTab& tab : node.pane->tabs) {
    ids->insert(tab.id);
  }
}

// ---- workspaces -------------------------------------------------------------

void TestWorkspaceLifecycle() {
  WindowModel m;
  Check(m.workspace_count() == 0, "starts empty");
  Check(m.selected_workspace() == kInvalidId, "no selection when empty");

  WorkspaceId a = m.AddWorkspace(SurfaceKind::kWeb, "a");
  WorkspaceId b = m.AddWorkspace(SurfaceKind::kTerminal, "b");
  WorkspaceId c = m.AddWorkspace(SurfaceKind::kWeb, "c");
  Check(m.workspace_count() == 3, "three workspaces");
  Check(m.selected_workspace() == c &&
            m.workspace_selection_anchor() == c && SelectionIs(m, {c}),
        "newest is the sole selected active and anchor");
  Check(a != b && b != c && a != c, "ids unique");

  // Each new workspace has exactly one column holding one pane with one tab.
  Check(PaneCount(m, a) == 1, "new workspace has one pane");
  const Workspace* wa = m.GetWorkspace(a);
  Check(wa && wa->columns.size() == 1, "new workspace has one column");
  Check(wa && wa->column_widths.size() == 1 && wa->column_widths[0] == 1.0,
        "new workspace first column is full width");
  Check(wa->columns[0].is_pane(), "the column is a lone pane");
  Check(wa->columns[0].pane->tabs.size() == 1, "lone pane has one tab");
  Check(wa->columns[0].pane->tabs[0].kind == SurfaceKind::kWeb,
        "tab kind honored");
  Check(wa->focused == wa->columns[0].pane->id, "focus on the lone pane");

  m.SelectWorkspace(a);
  Check(m.selected_workspace() == a &&
            m.workspace_selection_anchor() == a && SelectionIs(m, {a}),
        "select works");
  m.SelectWorkspace(99999);  // nonexistent
  Check(m.selected_workspace() == a, "selecting missing id is a no-op");

  // Closing the selected workspace moves selection to a neighbor.
  m.SelectWorkspace(b);
  m.CloseWorkspace(b);
  Check(m.workspace_count() == 2, "closed one");
  Check(m.selected_workspace() == c &&
            m.workspace_selection_anchor() == c && SelectionIs(m, {c}),
        "selection moved to neighbor and repaired anchor");
  Check(m.GetWorkspace(b) == nullptr, "closed workspace is gone");

  m.CloseWorkspace(a);
  m.CloseWorkspace(c);
  Check(m.workspace_count() == 0, "all closed");
  Check(m.selected_workspace() == kInvalidId &&
            m.workspace_selection_anchor() == kInvalidId &&
            m.selected_workspaces().empty(),
        "selection cleared when empty");
}

void TestDuplicateWorkspace() {
  WindowModel m;
  WorkspaceId source = m.AddWorkspace(SurfaceKind::kWeb, "source");
  PaneId upper = m.GetWorkspace(source)->focused;
  const SurfaceTabId first_web = m.FindPane(source, upper)->selected;
  const SurfaceTabId upper_terminal =
      m.AddTab(source, upper, SurfaceKind::kTerminal, "upper terminal");
  const SurfaceTabId upper_web =
      m.AddTab(source, upper, SurfaceKind::kWeb, "upper web");
  m.SetTabLoading(source, upper, upper_web, true);
  m.SelectTab(source, upper, upper_terminal);

  PaneId lower = m.SplitPane(source, upper, SplitOrientation::kVertical, 0.37,
                             SurfaceKind::kTerminal);
  const SurfaceTabId lower_terminal = m.FindPane(source, lower)->selected;
  m.AddTab(source, lower, SurfaceKind::kWeb, "lower web");
  m.SelectTab(source, lower, lower_terminal);
  PaneId nested =
      m.SplitPane(source, lower, SplitOrientation::kHorizontal, 0.71,
                  SurfaceKind::kWeb);
  PaneId second_column = m.AddColumn(source, SurfaceKind::kTerminal);
  m.AddTab(source, second_column, SurfaceKind::kWeb, "second-column web");
  m.SetColumnWidth(source, upper, 0.44);
  m.SetColumnWidth(source, second_column, 1.35);
  m.SetWorkspaceLayoutMode(source, WorkspaceLayoutMode::kTiled);
  m.FocusPane(source, nested);
  m.SetWorkspaceColor(source, GroupColor::kPurple);

  WorkspaceId peer = m.AddWorkspace(SurfaceKind::kWeb, "peer");
  const WorkspaceGroupId group = m.CreateWorkspaceGroup(source);
  m.MoveWorkspace(peer, source, -1);

  const DuplicateWorkspaceResult result =
      m.DuplicateWorkspace(source, "duplicate-key");
  const Workspace* source_workspace = m.GetWorkspace(source);
  const Workspace* duplicate = m.GetWorkspace(result.workspace);
  Check(result.workspace != kInvalidId && duplicate,
        "duplicate workspace returns a live workspace");
  Check(duplicate && duplicate->id != source_workspace->id &&
            duplicate->registry_key == "duplicate-key" &&
            duplicate->group == group &&
            duplicate->preferred_group_color ==
                source_workspace->preferred_group_color &&
            duplicate->title == source_workspace->title &&
            duplicate->layout_mode == source_workspace->layout_mode &&
            duplicate->column_widths == source_workspace->column_widths,
        "duplicate preserves workspace metadata and column widths");
  Check(m.roots() == std::vector<WorkspaceId>(
                         {source, result.workspace, peer}) &&
            m.WorkspacesInGroup(group) ==
                std::vector<WorkspaceId>({source, result.workspace, peer}),
        "duplicate is inserted immediately after source in its existing group");
  Check(m.selected_workspace() == result.workspace &&
            m.workspace_selection_anchor() == result.workspace &&
            SelectionIs(m, {result.workspace}),
        "duplicate becomes the sole selected workspace");

  bool same_layout =
      duplicate && source_workspace &&
      duplicate->columns.size() == source_workspace->columns.size();
  if (same_layout) {
    for (size_t i = 0; i < source_workspace->columns.size(); ++i) {
      same_layout =
          same_layout &&
          EquivalentDuplicateNode(source_workspace->columns[i],
                                  duplicate->columns[i], result);
    }
  }
  Check(same_layout, "duplicate preserves the complete split and tab graph");
  Check(duplicate &&
            DuplicateIdFor(result.panes, source_workspace->focused) ==
                duplicate->focused &&
            DuplicateIdFor(result.tabs, first_web) != kInvalidId &&
            result.tabs.size() == 8,
        "duplicate maps focused pane and every surface to fresh ids");

  std::set<int64_t> source_ids = {source};
  std::set<int64_t> duplicate_ids = {result.workspace};
  for (const LayoutNode& column : source_workspace->columns) {
    CollectNodeIds(column, &source_ids);
  }
  for (const LayoutNode& column : duplicate->columns) {
    CollectNodeIds(column, &duplicate_ids);
  }
  std::vector<int64_t> aliases;
  std::set_intersection(source_ids.begin(), source_ids.end(),
                        duplicate_ids.begin(), duplicate_ids.end(),
                        std::back_inserter(aliases));
  Check(aliases.empty(), "duplicate shares no workspace graph ids");

  Workspace* mutable_duplicate = m.GetWorkspace(result.workspace);
  mutable_duplicate->title = "changed duplicate";
  mutable_duplicate->columns[0].split->ratio = 0.9;
  const PaneId duplicate_upper = DuplicateIdFor(result.panes, upper);
  m.FindPane(result.workspace, duplicate_upper)->tabs[0].title =
      "changed duplicate tab";
  Check(m.GetWorkspace(source)->title == "source" &&
            m.GetWorkspace(source)->columns[0].split->ratio == 0.37 &&
            m.FindPane(source, upper)->tabs[0].title !=
                "changed duplicate tab",
        "duplicate graph ownership is independent from its source");

  const size_t count_before_invalid = m.workspace_count();
  Check(m.DuplicateWorkspace(99999).workspace == kInvalidId &&
            m.workspace_count() == count_before_invalid,
        "duplicating a missing workspace is a no-op");
  CheckInvariants(m, "workspace duplication preserves model invariants");
}

void TestMoveWorkspace() {
  WindowModel m;
  WorkspaceId a = m.AddWorkspace(SurfaceKind::kWeb, "a");
  WorkspaceId b = m.AddWorkspace(SurfaceKind::kWeb, "b");
  WorkspaceId c = m.AddWorkspace(SurfaceKind::kWeb, "c");
  // roots order: a, b, c
  m.MoveWorkspace(c, kInvalidId, 0);  // -> c, a, b
  Check(m.roots().size() == 3 && m.roots()[0] == c && m.roots()[1] == a &&
            m.roots()[2] == b,
        "reordered c to front of roots");
  m.MoveWorkspace(c, kInvalidId, 99);  // clamp to end -> a, b, c
  Check(m.roots()[2] == c && m.roots()[0] == a, "clamped move to end");
}

void TestMoveWorkspaces() {
  WindowModel m;
  WorkspaceId a = m.AddWorkspace(SurfaceKind::kWeb, "a");
  WorkspaceId b = m.AddWorkspace(SurfaceKind::kWeb, "b");
  WorkspaceId c = m.AddWorkspace(SurfaceKind::kWeb, "c");
  WorkspaceId d = m.AddWorkspace(SurfaceKind::kWeb, "d");
  WorkspaceId e = m.AddWorkspace(SurfaceKind::kWeb, "e");

  m.SelectWorkspace(b);
  m.ToggleWorkspaceSelection(d);
  // Input order is deliberately reversed; batch moves normalize to rail order.
  m.MoveWorkspaces({d, b}, kInvalidId, 3);
  Check(m.roots() == std::vector<WorkspaceId>({a, c, e, b, d}),
        "batch flat move preserves selected rail order");
  Check(SelectionIs(m, {b, d}) && m.selected_workspace() == d,
        "batch flat move preserves selection and active workspace");

  m.MoveWorkspace(c, a, -1);
  const WorkspaceGroupId group = m.GetWorkspace(a)->group;
  m.MoveWorkspaces({d, b}, a, 1);
  Check(m.roots() == std::vector<WorkspaceId>({a, b, d, c, e}) &&
            m.GetWorkspace(b)->group == group &&
            m.GetWorkspace(d)->group == group,
        "batch move inserts selected workspaces into an existing group");

  m.MoveWorkspaces({b, d}, kInvalidId, 2);
  Check(m.roots() == std::vector<WorkspaceId>({a, c, b, d, e}) &&
            m.GetWorkspace(b)->group == kInvalidId &&
            m.GetWorkspace(d)->group == kInvalidId,
        "batch flat move ungroups every moved workspace atomically");

  m.MoveWorkspaces({c, a}, kInvalidId, 3);
  Check(m.roots() == std::vector<WorkspaceId>({b, d, e, a, c}) &&
            m.GetWorkspace(a)->group == group &&
            m.GetWorkspace(c)->group == group &&
            m.GetWorkspaceGroup(group),
        "flat move retains a fully selected group's identity and order");

  const std::vector<WorkspaceId> before = m.roots();
  m.MoveWorkspaces({a, b}, a, 0);
  Check(m.roots() == before,
        "batch move rejects a destination anchor that is itself moving");
  CheckInvariants(m, "batch workspace move invariants");
}

void TestFlatMovesRespectWorkspaceGroupBoundaries() {
  {
    WindowModel m;
    WorkspaceId a = m.AddWorkspace(SurfaceKind::kWeb, "a");
    WorkspaceId b = m.AddWorkspace(SurfaceKind::kWeb, "b");
    WorkspaceId c = m.AddWorkspace(SurfaceKind::kWeb, "c");
    WorkspaceId d = m.AddWorkspace(SurfaceKind::kWeb, "d");
    WorkspaceId moving = m.AddWorkspace(SurfaceKind::kWeb, "moving");
    m.MoveWorkspace(b, a, -1);
    m.MoveWorkspace(c, a, -1);
    m.MoveWorkspace(d, a, -1);

    m.MoveWorkspace(moving, kInvalidId, 1);
    Check(m.roots() ==
              std::vector<WorkspaceId>({moving, a, b, c, d}),
          "single flat move clamps an interior slot to the nearer group start");
    m.MoveWorkspace(moving, kInvalidId, 3);
    Check(m.roots() ==
              std::vector<WorkspaceId>({a, b, c, d, moving}),
          "single flat move clamps an interior slot to the nearer group end");
    m.MoveWorkspace(moving, kInvalidId, 0);
    Check(m.roots() ==
              std::vector<WorkspaceId>({moving, a, b, c, d}),
          "single flat move preserves the exact slot before a group");
    m.MoveWorkspace(moving, kInvalidId, 4);
    Check(m.roots() ==
              std::vector<WorkspaceId>({a, b, c, d, moving}),
          "single flat move preserves the exact slot after a group");
    CheckInvariants(m, "single flat group-boundary moves preserve invariants");
  }

  {
    WindowModel m;
    WorkspaceId a = m.AddWorkspace(SurfaceKind::kWeb, "a");
    WorkspaceId b = m.AddWorkspace(SurfaceKind::kWeb, "b");
    WorkspaceId c = m.AddWorkspace(SurfaceKind::kWeb, "c");
    WorkspaceId moving = m.AddWorkspace(SurfaceKind::kWeb, "moving");
    m.MoveWorkspace(b, a, -1);
    m.MoveWorkspace(c, a, -1);

    m.MoveWorkspace(moving, kInvalidId, 1);
    Check(m.roots() ==
              std::vector<WorkspaceId>({moving, a, b, c}),
          "single flat move uses the nearer start of a three-member group");

    m.MoveWorkspace(b, kInvalidId, 2);
    Check(m.roots() ==
              std::vector<WorkspaceId>({moving, a, c, b}) &&
              m.GetWorkspace(b)->group == kInvalidId &&
              m.GetWorkspace(a)->group == m.GetWorkspace(c)->group,
          "single flat move clamps after source removal and membership update");
    CheckInvariants(m, "single source-removal boundary move invariants");
  }

  {
    WindowModel m;
    WorkspaceId a = m.AddWorkspace(SurfaceKind::kWeb, "a");
    WorkspaceId b = m.AddWorkspace(SurfaceKind::kWeb, "b");
    WorkspaceId moving = m.AddWorkspace(SurfaceKind::kWeb, "moving");
    m.MoveWorkspace(b, a, -1);

    m.MoveWorkspace(moving, kInvalidId, 1);
    Check(m.roots() == std::vector<WorkspaceId>({a, b, moving}),
          "single flat move resolves an equal-distance tie after the group");
    CheckInvariants(m, "single flat tie move preserves invariants");
  }

  {
    WindowModel m;
    WorkspaceId left = m.AddWorkspace(SurfaceKind::kWeb, "left");
    WorkspaceId a = m.AddWorkspace(SurfaceKind::kWeb, "a");
    WorkspaceId b = m.AddWorkspace(SurfaceKind::kWeb, "b");
    WorkspaceId c = m.AddWorkspace(SurfaceKind::kWeb, "c");
    WorkspaceId d = m.AddWorkspace(SurfaceKind::kWeb, "d");
    WorkspaceId first = m.AddWorkspace(SurfaceKind::kWeb, "first");
    WorkspaceId second = m.AddWorkspace(SurfaceKind::kWeb, "second");
    WorkspaceId right = m.AddWorkspace(SurfaceKind::kWeb, "right");
    m.MoveWorkspace(b, a, -1);
    m.MoveWorkspace(c, a, -1);
    m.MoveWorkspace(d, a, -1);

    m.MoveWorkspaces({second, first}, kInvalidId, 2);
    Check(m.roots() == std::vector<WorkspaceId>(
                           {left, first, second, a, b, c, d, right}),
          "batch flat move clamps an interior slot to the nearer group start");
    m.MoveWorkspaces({first, second}, kInvalidId, 4);
    Check(m.roots() == std::vector<WorkspaceId>(
                           {left, a, b, c, d, first, second, right}),
          "batch flat move clamps an interior slot to the nearer group end");
    m.MoveWorkspaces({first, second}, kInvalidId, 1);
    Check(m.roots() == std::vector<WorkspaceId>(
                           {left, first, second, a, b, c, d, right}),
          "batch flat move preserves the exact slot before a group");
    m.MoveWorkspaces({first, second}, kInvalidId, 5);
    Check(m.roots() == std::vector<WorkspaceId>(
                           {left, a, b, c, d, first, second, right}),
          "batch flat move preserves the exact slot after a group");
    CheckInvariants(m, "batch flat group-boundary moves preserve invariants");
  }

  {
    WindowModel m;
    WorkspaceId a = m.AddWorkspace(SurfaceKind::kWeb, "a");
    WorkspaceId b = m.AddWorkspace(SurfaceKind::kWeb, "b");
    WorkspaceId c = m.AddWorkspace(SurfaceKind::kWeb, "c");
    WorkspaceId d = m.AddWorkspace(SurfaceKind::kWeb, "d");
    WorkspaceId x = m.AddWorkspace(SurfaceKind::kWeb, "x");
    WorkspaceId y = m.AddWorkspace(SurfaceKind::kWeb, "y");
    m.MoveWorkspace(b, a, -1);
    m.MoveWorkspace(c, a, -1);
    m.MoveWorkspace(d, a, -1);
    const WorkspaceGroupId group = m.GetWorkspace(a)->group;

    m.MoveWorkspaces({x, b}, kInvalidId, 1);
    Check(m.roots() ==
              std::vector<WorkspaceId>({b, x, a, c, d, y}) &&
              m.GetWorkspace(b)->group == kInvalidId &&
              m.GetWorkspace(x)->group == kInvalidId &&
              m.WorkspacesInGroup(group) ==
                  std::vector<WorkspaceId>({a, c, d}),
          "batch flat move resolves against the surviving post-removal group");
    CheckInvariants(m, "batch source-removal boundary move invariants");
  }

  {
    WindowModel m;
    WorkspaceId a = m.AddWorkspace(SurfaceKind::kWeb, "a");
    WorkspaceId b = m.AddWorkspace(SurfaceKind::kWeb, "b");
    WorkspaceId c = m.AddWorkspace(SurfaceKind::kWeb, "c");
    WorkspaceId d = m.AddWorkspace(SurfaceKind::kWeb, "d");
    m.MoveWorkspace(b, a, -1);
    m.MoveWorkspace(d, c, -1);
    const WorkspaceGroupId first_group = m.GetWorkspace(a)->group;

    m.MoveWorkspaces({b, a}, kInvalidId, 1);
    Check(m.roots() == std::vector<WorkspaceId>({c, d, a, b}) &&
              m.WorkspacesInGroup(first_group) ==
                  std::vector<WorkspaceId>({a, b}),
          "full-group batch move keeps identity and resolves a tie after group");
    CheckInvariants(m, "full-group boundary move preserves invariants");
  }
}

void TestWorkspaceMultiSelection() {
  WindowModel m;
  WorkspaceId a = m.AddWorkspace(SurfaceKind::kWeb, "a");
  WorkspaceId b = m.AddWorkspace(SurfaceKind::kWeb, "b");
  WorkspaceId c = m.AddWorkspace(SurfaceKind::kWeb, "c");
  WorkspaceId d = m.AddWorkspace(SurfaceKind::kWeb, "d");
  WorkspaceId e = m.AddWorkspace(SurfaceKind::kWeb, "e");

  Check(SelectionIs(m, {e}) && m.selected_workspace() == e &&
            m.workspace_selection_anchor() == e,
        "adding a workspace selects only the new active workspace");

  // Plain selection resets active, anchor, and the selected set.
  m.SelectWorkspace(b);
  Check(SelectionIs(m, {b}) && m.selected_workspace() == b &&
            m.workspace_selection_anchor() == b,
        "plain workspace selection is exclusive and resets the anchor");

  // Shift replaces the selected set with the anchor-to-target range. Moving
  // back through the anchor must discard the previous range, not add to it.
  m.ExtendWorkspaceSelectionTo(d);
  Check(SelectionIs(m, {b, c, d}) && m.selected_workspace() == d &&
            m.workspace_selection_anchor() == b,
        "shift selects the forward inclusive anchor range");
  m.ExtendWorkspaceSelectionTo(a);
  Check(SelectionIs(m, {a, b}) && m.selected_workspace() == a &&
            m.workspace_selection_anchor() == b,
        "shift replaces selection with a reverse inclusive range");

  // Command/Ctrl+Shift only adds its range and leaves the anchor fixed.
  m.AddWorkspaceSelectionFromAnchorTo(d);
  Check(SelectionIs(m, {a, b, c, d}) && m.selected_workspace() == d &&
            m.workspace_selection_anchor() == b,
        "modified shift adds its range to the existing selection");

  // A drag captures selection before mouse-down changes it. Canceling must
  // restore all three ListSelectionModel dimensions, including a distinct
  // active item and range anchor.
  const WorkspaceSelectionState pre_drag = m.GetWorkspaceSelectionState();
  m.SelectWorkspace(e);
  m.RestoreWorkspaceSelectionState(pre_drag);
  Check(SelectionIs(m, {a, b, c, d}) && m.selected_workspace() == d &&
            m.workspace_selection_anchor() == b,
        "drag cancel restores selected set, active workspace, and anchor");

  m.ActivateWorkspaceInSelection(c);
  Check(SelectionIs(m, {a, b, c, d}) && m.selected_workspace() == c &&
            m.workspace_selection_anchor() == c,
        "drag activation changes active and anchor without changing selection");
  m.ActivateWorkspaceInSelection(e);
  Check(SelectionIs(m, {a, b, c, d}) && m.selected_workspace() == c &&
            m.workspace_selection_anchor() == c,
        "drag activation ignores an unselected workspace");

  // Command/Ctrl toggling an unselected item makes it active and anchor.
  Check(m.ToggleWorkspaceSelection(e) && SelectionIs(m, {a, b, c, d, e}) &&
            m.selected_workspace() == e &&
            m.workspace_selection_anchor() == e,
        "toggle adds an unselected workspace and activates it");
  Check(m.ToggleWorkspaceSelection(c) && SelectionIs(m, {a, b, d, e}) &&
            m.selected_workspace() == e &&
            m.workspace_selection_anchor() == e,
        "toggle removes an ordinary selected workspace");

  // Removing the active anchor promotes the first selected row, exactly as
  // TabStripModel repairs its selection after deselection/removal.
  Check(m.ToggleWorkspaceSelection(e) && SelectionIs(m, {a, b, d}) &&
            m.selected_workspace() == a &&
            m.workspace_selection_anchor() == a,
        "toggle repairs active and anchor after removing both");
  Check(m.ToggleWorkspaceSelection(a) && SelectionIs(m, {b, d}) &&
            m.selected_workspace() == b &&
            m.workspace_selection_anchor() == b,
        "a second active-anchor removal promotes the next selected row");
  Check(m.ToggleWorkspaceSelection(d) && SelectionIs(m, {b}),
        "toggle can reduce a multi-selection to one workspace");
  Check(!m.ToggleWorkspaceSelection(b) && SelectionIs(m, {b}) &&
            m.selected_workspace() == b &&
            m.workspace_selection_anchor() == b,
        "the sole selected workspace cannot be deselected");

  m.ToggleWorkspaceSelection(d);
  Check(m.WorkspacesForCommand(b) == std::vector<WorkspaceId>({b, d}) &&
            m.WorkspacesForCommand(e) == std::vector<WorkspaceId>({e}) &&
            m.WorkspacesForCommand(99999).empty() && SelectionIs(m, {b, d}),
        "context commands use selection only when context row is selected");
  m.ToggleWorkspaceSelection(d);

  const std::vector<WorkspaceId> before_invalid = m.selected_workspaces();
  m.ExtendWorkspaceSelectionTo(99999);
  m.AddWorkspaceSelectionFromAnchorTo(99999);
  Check(!m.ToggleWorkspaceSelection(99999) &&
            m.selected_workspaces() == before_invalid,
        "unknown selection targets are no-ops");

  // Selection is ID-based: reordering changes only the order returned to UI.
  m.SelectWorkspace(a);
  m.ToggleWorkspaceSelection(c);
  m.ToggleWorkspaceSelection(e);
  m.MoveWorkspace(e, kInvalidId, 0);
  Check(SelectionIs(m, {e, a, c}) && m.selected_workspace() == e &&
            m.workspace_selection_anchor() == e,
        "selection survives workspace reorder and reports new rail order");
  m.MoveWorkspace(c, b, -1);
  Check(SelectionIs(m, {e, a, c}) && m.selected_workspace() == e &&
            m.workspace_selection_anchor() == e,
        "selection survives moving a workspace into a group");

  // Range selection follows the complete logical roots order, including the
  // members of a collapsed group between two visible endpoints.
  WorkspaceGroupId group = m.GetWorkspace(b)->group;
  m.SetWorkspaceGroupCollapsed(group, true);
  m.SelectWorkspace(e);
  m.ExtendWorkspaceSelectionTo(d);
  Check(SelectionIs(m, {e, a, b, c, d}) &&
            m.GetWorkspaceGroup(group)->collapsed,
        "range selection includes hidden group members in logical order");

  const WorkspaceSelectionState before_group_drag =
      m.GetWorkspaceSelectionState();
  m.RestoreWorkspaceSelectionState({{b, c}, c, c});
  Check(SelectionIs(m, {b, c}) && m.selected_workspace() == c &&
            m.workspace_selection_anchor() == c &&
            m.GetWorkspaceGroup(group)->collapsed,
        "group drag selection does not expand a collapsed group");
  m.RestoreWorkspaceSelectionState(before_group_drag);

  // Closing a non-active selected workspace keeps active/anchor stable.
  m.SelectWorkspace(e);
  m.ToggleWorkspaceSelection(a);
  m.ToggleWorkspaceSelection(d);
  const WorkspaceSelectionState selection_before_drag_member_removal =
      m.GetWorkspaceSelectionState();
  m.CloseWorkspace(a);
  Check(SelectionIs(m, {e, d}) && m.selected_workspace() == d &&
            m.workspace_selection_anchor() == d,
        "closing an ordinary selected workspace preserves active and anchor");

  // CmuxRail cancels a multi-row drag when any dragged member disappears and
  // restores the selection snapshot captured on mouse-down. The snapshot may
  // contain the just-removed member; restoring it must filter that stale ID
  // while retaining the surviving selected rows, active item, and anchor.
  m.RestoreWorkspaceSelectionState(selection_before_drag_member_removal);
  Check(SelectionIs(m, {e, d}) && m.selected_workspace() == d &&
            m.workspace_selection_anchor() == d,
        "drag cancel filters a removed secondary selection member");

  // Closing the active anchor with another selection promotes the first
  // selected item in current rail order instead of choosing an unselected
  // adjacent workspace.
  m.CloseWorkspace(d);
  Check(SelectionIs(m, {e}) && m.selected_workspace() == e &&
            m.workspace_selection_anchor() == e,
        "closing the active anchor promotes the first remaining selection");

  // Removing a non-active anchor repairs it to the active selection.
  m.SelectWorkspace(e);
  m.ToggleWorkspaceSelection(b);
  m.ExtendWorkspaceSelectionTo(c);
  Check(SelectionIs(m, {b, c}) && m.selected_workspace() == c &&
            m.workspace_selection_anchor() == b,
        "test setup has a distinct non-active anchor");
  m.CloseWorkspace(b);
  Check(SelectionIs(m, {c}) && m.selected_workspace() == c &&
            m.workspace_selection_anchor() == c,
        "closing a non-active anchor repairs it to the active workspace");

  CheckInvariants(m, "multi-workspace selection invariants");
}

void TestWorkspaceGroupCollapseSelectionParity() {
  WindowModel m;
  const WorkspaceId before = m.AddWorkspace(SurfaceKind::kWeb, "before");
  const WorkspaceId first = m.AddWorkspace(SurfaceKind::kWeb, "first");
  const WorkspaceId second = m.AddWorkspace(SurfaceKind::kWeb, "second");
  const WorkspaceId after = m.AddWorkspace(SurfaceKind::kWeb, "after");
  const WorkspaceGroupId group = m.CreateWorkspaceGroup(first);
  m.MoveWorkspace(second, first, -1);

  m.SelectWorkspace(first);
  m.SetWorkspaceGroupCollapsed(group, true);
  Check(m.GetWorkspaceGroup(group)->collapsed && SelectionIs(m, {after}) &&
            m.selected_workspace() == after,
        "collapsing the active group prefers the first expanded workspace "
        "after it");

  m.SetWorkspaceGroupCollapsed(group, false);
  m.SelectWorkspace(first);
  m.ToggleWorkspaceSelection(before);
  Check(SelectionIs(m, {before, first}) && m.selected_workspace() == before,
        "collapse selection setup keeps an active workspace outside the group");
  m.SetWorkspaceGroupCollapsed(group, true);
  Check(m.GetWorkspaceGroup(group)->collapsed && SelectionIs(m, {before}) &&
            m.workspace_selection_anchor() == before,
        "collapsing another group reselects the active workspace and clears "
        "hidden multi-selection");

  WindowModel trailing;
  const WorkspaceId only_before =
      trailing.AddWorkspace(SurfaceKind::kWeb, "before");
  const WorkspaceId trailing_first =
      trailing.AddWorkspace(SurfaceKind::kWeb, "first");
  const WorkspaceId trailing_second =
      trailing.AddWorkspace(SurfaceKind::kWeb, "second");
  const WorkspaceGroupId trailing_group =
      trailing.CreateWorkspaceGroup(trailing_first);
  trailing.MoveWorkspace(trailing_second, trailing_first, -1);
  trailing.SelectWorkspace(trailing_second);
  trailing.SetWorkspaceGroupCollapsed(trailing_group, true);
  Check(trailing.GetWorkspaceGroup(trailing_group)->collapsed &&
            SelectionIs(trailing, {only_before}),
        "collapsing a trailing active group falls back to the preceding "
        "expanded workspace");

  WindowModel lone;
  const WorkspaceId lone_member = lone.AddWorkspace(SurfaceKind::kWeb, "only");
  const WorkspaceGroupId lone_group = lone.CreateWorkspaceGroup(lone_member);
  lone.SetWorkspaceGroupCollapsed(lone_group, true);
  Check(!lone.GetWorkspaceGroup(lone_group)->collapsed &&
            SelectionIs(lone, {lone_member}),
        "pure model refuses to hide a lone active group without the "
        "controller-created fallback workspace");
  CheckInvariants(m, "group collapse selection invariants");
  CheckInvariants(trailing, "trailing group collapse selection invariants");
  CheckInvariants(lone, "lone group collapse selection invariants");
}

void TestWorkspaceRegistryIdentityAndOrder() {
  WindowModel m;
  WorkspaceId first = m.AddWorkspace(SurfaceKind::kWeb, "first", "key-b");
  WorkspaceId grouped = m.AddWorkspace(SurfaceKind::kWeb, "grouped", "key-c");
  WorkspaceId last = m.AddWorkspace(SurfaceKind::kWeb, "last", "key-a");
  WorkspaceId child_b =
      m.AddChildWorkspace(grouped, SurfaceKind::kWeb, "child-b", "child-b");
  WorkspaceId child_a =
      m.AddChildWorkspace(grouped, SurfaceKind::kWeb, "child-a", "child-a");
  const WorkspaceGroupId group = m.GetWorkspace(grouped)->group;

  Check(m.FindWorkspaceByRegistryKey("key-b") == m.GetWorkspace(first),
        "stable key finds workspace");
  Check(m.FindWorkspaceByRegistryKey("missing") == nullptr,
        "unknown stable key is absent");
  m.ReorderWorkspaceSiblingsByRegistryKeys(
      {"key-a", "child-a", "key-b", "child-b", "key-c"});
  Check(m.roots() == std::vector<WorkspaceId>(
                         {last, child_a, child_b, grouped, first}),
        "registry order sorts shallow-group units and their members");
  Check(m.GetWorkspace(child_a)->group == group &&
            m.GetWorkspace(child_b)->group == group &&
            m.GetWorkspace(grouped)->group == group &&
            m.WorkspacesInGroup(group) ==
                std::vector<WorkspaceId>({child_a, child_b, grouped}),
        "registry ordering preserves contiguous shallow-group membership");
  CheckInvariants(m, "registry ordering preserves model invariants");
}

// ---- tabs -------------------------------------------------------------------

void TestTabs() {
  WindowModel m;
  WorkspaceId ws = m.AddWorkspace(SurfaceKind::kWeb, "w");
  PaneId p = m.GetWorkspace(ws)->columns[0].pane->id;

  SurfaceTabId t0 = m.GetWorkspace(ws)->columns[0].pane->tabs[0].id;
  SurfaceTabId t1 = m.AddTab(ws, p, SurfaceKind::kWeb, "t1");
  SurfaceTabId t2 = m.AddTab(ws, p, SurfaceKind::kTerminal, "t2");
  Pane* pane = m.FindPane(ws, p);
  Check(pane->tabs.size() == 3, "three tabs");
  Check(pane->selected == t2, "adding selects the new tab");

  m.SelectTab(ws, p, t0);
  Check(m.FindPane(ws, p)->selected == t0, "select tab");

  // Closing the selected tab picks a neighbor; pane survives.
  m.CloseTab(ws, p, t0);
  Check(m.FindPane(ws, p)->tabs.size() == 2, "closed one tab");
  Check(m.FindPane(ws, p)->selected == t1, "selection moved to neighbor");

  m.CloseTab(ws, p, t1);
  Check(m.FindPane(ws, p)->tabs.size() == 1, "one tab left");
  // Closing the final tab of the workspace's only pane refuses without
  // mutating, preserving the invariant that every pane has at least one tab.
  m.CloseTab(ws, p, t2);
  Check(PaneCount(m, ws) == 1, "lone pane persists after last-tab close");
  Check(m.FindPane(ws, p)->tabs.size() == 1,
        "last-tab close keeps one tab in the lone pane");
  Check(m.FindPane(ws, p)->tabs[0].id == t2,
        "last-tab close leaves the surviving tab unchanged");
  Check(m.FindPane(ws, p)->selected == t2,
        "last-tab close leaves selection unchanged");
}

// ---- splits -----------------------------------------------------------------

void TestSplitAndCollapse() {
  WindowModel m;
  WorkspaceId ws = m.AddWorkspace(SurfaceKind::kWeb, "w");
  PaneId p0 = m.GetWorkspace(ws)->columns[0].pane->id;

  PaneId p1 = m.SplitPane(ws, p0, SplitOrientation::kHorizontal, 0.5,
                          SurfaceKind::kTerminal);
  Check(p1 != kInvalidId, "split produced a new pane");
  Check(PaneCount(m, ws) == 2, "two panes after split");
  Check(m.ColumnCountOf(ws) == 1, "split stays within the column");
  Check(m.GetWorkspace(ws)->columns[0].is_split(),
        "column root is now a split");
  Check(m.GetWorkspace(ws)->focused == p1, "focus moved to new pane");

  // The new pane's tab is a terminal.
  Pane* np = m.FindPane(ws, p1);
  Check(
      np && np->tabs.size() == 1 && np->tabs[0].kind == SurfaceKind::kTerminal,
      "new pane carries a terminal tab");

  // Split the new pane again -> 3 panes in a nested tree, still one column.
  PaneId p2 = m.SplitPane(ws, p1, SplitOrientation::kVertical, 0.3);
  Check(PaneCount(m, ws) == 3, "three panes after second split");
  Check(m.ColumnCountOf(ws) == 1, "nested split adds no column");
  Check(m.ColumnIndexOf(ws, p2) == 0, "nested pane in the same column");

  // Collapse the middle pane; its sibling takes its place; 2 panes remain.
  m.ClosePane(ws, p1);
  Check(PaneCount(m, ws) == 2, "two panes after collapse");
  Check(m.FindPane(ws, p1) == nullptr, "collapsed pane is gone");
  Check(m.FindPane(ws, p0) != nullptr, "p0 survives");
  Check(m.FindPane(ws, p2) != nullptr, "p2 survives");

  // Collapse again -> back to a lone pane; the column root is a pane again.
  // p2 is focused, so focus falls to the sibling that absorbs the space.
  m.ClosePane(ws, p2);
  Check(PaneCount(m, ws) == 1, "one pane after second collapse");
  Check(m.GetWorkspace(ws)->columns[0].is_pane(),
        "column collapsed back to a pane");
  Check(m.GetWorkspace(ws)->focused == p0, "focus fell to absorbing sibling");

  // Closing the workspace's only pane is a no-op.
  m.ClosePane(ws, p0);
  Check(PaneCount(m, ws) == 1, "the workspace's last pane cannot close");
}

void TestSplitRatio() {
  WindowModel m;
  WorkspaceId ws = m.AddWorkspace();
  PaneId p0 = m.GetWorkspace(ws)->columns[0].pane->id;
  m.SplitPane(ws, p0, SplitOrientation::kHorizontal, 0.5);
  SplitId sid = m.GetWorkspace(ws)->columns[0].split->id;

  m.SetSplitRatio(ws, sid, 0.7);
  Check(m.GetWorkspace(ws)->columns[0].split->ratio == 0.7, "ratio set");
  m.SetSplitRatio(ws, sid, 5.0);  // clamp
  Check(m.GetWorkspace(ws)->columns[0].split->ratio <= 0.95,
        "ratio clamped high");
  m.SetSplitRatio(ws, sid, -1.0);
  Check(m.GetWorkspace(ws)->columns[0].split->ratio >= 0.05,
        "ratio clamped low");

  // Ratio lookup works in a non-first column too.
  PaneId c1 = m.AddColumn(ws, SurfaceKind::kWeb);
  m.SplitPane(ws, c1, SplitOrientation::kVertical, 0.5);
  SplitId sid1 = m.GetWorkspace(ws)->columns[1].split->id;
  m.SetSplitRatio(ws, sid1, 0.25);
  Check(m.GetWorkspace(ws)->columns[1].split->ratio == 0.25,
        "ratio set in a later column");
}

// ---- columns
// ------------------------------------------------------------------

void TestAddColumn() {
  WindowModel m;
  WorkspaceId ws = m.AddWorkspace(SurfaceKind::kWeb, "w");
  PaneId p0 = m.GetWorkspace(ws)->columns[0].pane->id;
  Check(m.ColumnCountOf(ws) == 1, "workspace starts with one column");
  Check(m.ColumnIndexOf(ws, p0) == 0, "seed pane in column 0");

  PaneId c1 = m.AddColumn(ws, SurfaceKind::kTerminal);
  Check(c1 != kInvalidId, "AddColumn returns the new pane");
  Check(m.ColumnCountOf(ws) == 2, "appended a column");
  Check(m.ColumnIndexOf(ws, c1) == 1, "appended at the end");
  Check(m.GetWorkspace(ws)->focused == c1, "AddColumn focuses the new pane");
  const Pane* cp = m.FindPane(ws, c1);
  Check(
      cp && cp->tabs.size() == 1 && cp->tabs[0].kind == SurfaceKind::kTerminal,
      "new column's pane carries one tab of the requested kind");

  PaneId c2 = m.AddColumn(ws, SurfaceKind::kWeb, 0);
  Check(m.ColumnIndexOf(ws, c2) == 0, "insert at index 0");
  Check(m.ColumnIndexOf(ws, p0) == 1 && m.ColumnIndexOf(ws, c1) == 2,
        "existing columns shifted right");

  PaneId c3 = m.AddColumn(ws, SurfaceKind::kWeb, 99);
  Check(m.ColumnIndexOf(ws, c3) == 3, "out-of-range index clamps to append");

  // PanesOf: flat in-order, columns left to right.
  std::vector<PaneId> order = m.PanesOf(ws);
  Check(order.size() == 4 && order[0] == c2 && order[1] == p0 &&
            order[2] == c1 && order[3] == c3,
        "PanesOf walks columns left to right");

  Check(m.AddColumn(99999, SurfaceKind::kWeb) == kInvalidId,
        "AddColumn on a missing workspace fails");
  Check(m.ColumnIndexOf(ws, 424242) == -1, "missing pane has no column");
  Check(m.ColumnCountOf(99999) == 0, "missing workspace has no columns");
}

void TestPanesOfOrderWithSplits() {
  WindowModel m;
  WorkspaceId ws = m.AddWorkspace(SurfaceKind::kWeb, "w");
  PaneId p0 = m.GetWorkspace(ws)->columns[0].pane->id;
  PaneId c1 = m.AddColumn(ws, SurfaceKind::kWeb);

  // Split column 0 twice: its in-order panes all precede column 1's.
  PaneId s0 = m.SplitPane(ws, p0, SplitOrientation::kVertical, 0.5,
                          SurfaceKind::kTerminal);
  PaneId s1 = m.SplitPane(ws, s0, SplitOrientation::kHorizontal, 0.3);
  Check(m.ColumnCountOf(ws) == 2, "splits created no columns");
  Check(m.ColumnIndexOf(ws, s0) == 0 && m.ColumnIndexOf(ws, s1) == 0,
        "split panes stay in their column");

  std::vector<PaneId> order = m.PanesOf(ws);
  Check(order.size() == 4, "four panes");
  Check(order[0] == p0 && order[1] == s0 && order[2] == s1 && order[3] == c1,
        "in-column panes precede the next column");
}

void TestClosePaneColumns() {
  WindowModel m;
  WorkspaceId ws = m.AddWorkspace(SurfaceKind::kWeb, "w");
  PaneId p0 = m.GetWorkspace(ws)->columns[0].pane->id;
  PaneId c1 = m.AddColumn(ws, SurfaceKind::kWeb);
  PaneId c2 = m.AddColumn(ws, SurfaceKind::kWeb);

  // Split inside the middle column, then close the new pane: the sibling
  // absorbs the space and takes focus; the column stays.
  PaneId s = m.SplitPane(ws, c1, SplitOrientation::kVertical, 0.5);
  m.ClosePane(ws, s);
  Check(m.ColumnCountOf(ws) == 3, "in-column collapse keeps the column");
  Check(m.FindPane(ws, s) == nullptr, "closed pane is gone");
  Check(m.GetWorkspace(ws)->focused == c1, "focus fell to absorbing sibling");

  // Close a column's only pane: the column disappears; focus falls to the
  // nearest column's first pane.
  m.ClosePane(ws, c1);
  Check(m.ColumnCountOf(ws) == 2, "emptied column removed");
  Check(m.ColumnIndexOf(ws, c2) == 1, "later column shifted left");
  Check(m.GetWorkspace(ws)->focused == c2, "focus fell to the nearest column");

  // Closing the LAST column's focused pane falls back to the previous column.
  m.ClosePane(ws, c2);
  Check(m.ColumnCountOf(ws) == 1, "one column left");
  Check(m.GetWorkspace(ws)->focused == p0, "focus fell to the previous column");

  // The workspace's only pane refuses to close.
  m.ClosePane(ws, p0);
  Check(m.ColumnCountOf(ws) == 1 && m.FindPane(ws, p0) != nullptr,
        "the workspace's last pane cannot close");
}

void TestCloseTabRemovesColumn() {
  WindowModel m;
  WorkspaceId ws = m.AddWorkspace(SurfaceKind::kWeb, "w");
  PaneId c0 = m.GetWorkspace(ws)->columns[0].pane->id;
  PaneId c1 = m.AddColumn(ws, SurfaceKind::kTerminal);
  PaneId c2 = m.AddColumn(ws, SurfaceKind::kTerminal);

  // Closing a middle column focuses the column that slides left into its
  // place, matching the pane-space inheritance rule used by Cmd-W.
  m.FocusPane(ws, c1);
  SurfaceTabId t = m.FindPane(ws, c1)->tabs[0].id;
  m.CloseTab(ws, c1, t);
  Check(m.ColumnCountOf(ws) == 2, "closing the last tab removed the column");
  Check(m.FindPane(ws, c1) == nullptr, "the pane went with its column");
  Check(m.GetWorkspace(ws)->focused == c2,
        "closing a middle column focused its right neighbor");

  // Closing the final column has no right neighbor, so focus falls left.
  t = m.FindPane(ws, c2)->tabs[0].id;
  m.CloseTab(ws, c2, t);
  Check(m.ColumnCountOf(ws) == 1, "closing the final column removed it");
  Check(m.GetWorkspace(ws)->focused == c0,
        "closing the final column focused its left neighbor");
}

void TestColumnWidthInvariantAfterColumnOps() {
  WindowModel m;
  WorkspaceId ws = m.AddWorkspace(SurfaceKind::kWeb, "w");
  PaneId a = m.GetWorkspace(ws)->columns[0].pane->id;
  Check(m.GetWorkspace(ws)->column_widths[0] == 1.0,
        "workspace seed column starts full width");
  CheckInvariants(m, "seed workspace initializes column widths");

  PaneId b = m.AddColumn(ws, SurfaceKind::kWeb);
  Check(m.GetWorkspace(ws)->column_widths[1] == 0.0,
        "later columns keep default-width sentinel");
  m.GetWorkspace(ws)->column_widths[1] = 0.5;
  CheckInvariants(m, "AddColumn keeps width vector in lockstep");

  PaneId c = m.SplitPane(ws, b, SplitOrientation::kVertical, 0.5);
  CheckInvariants(m, "SplitPane does not change column width count");

  m.ClosePane(ws, c);
  CheckInvariants(m, "in-column ClosePane keeps column width count");

  SurfaceTabId tab = m.AddTab(ws, a, SurfaceKind::kWeb, "moved");
  PaneId fresh = m.MoveTabToNewColumn(ws, a, tab, 1);
  Check(fresh != kInvalidId, "MoveTabToNewColumn returns a pane");
  CheckInvariants(m, "MoveTabToNewColumn inserts a width entry");

  PaneId moved = m.MovePaneToNewColumn(ws, b, 0);
  Check(moved == b, "MovePaneToNewColumn returns moved pane");
  CheckInvariants(m, "MovePaneToNewColumn preserves width lockstep");

  m.MovePaneToSplit(ws, fresh, b, SplitOrientation::kHorizontal, false);
  CheckInvariants(m, "MovePaneToSplit removes a source width entry");

  m.ClosePane(ws, a);
  CheckInvariants(m, "root ClosePane removes a width entry");
}

void TestCycleColumnWidth() {
  WindowModel m;
  WorkspaceId ws = m.AddWorkspace(SurfaceKind::kWeb, "w");
  PaneId a = m.GetWorkspace(ws)->columns[0].pane->id;
  PaneId b = m.AddColumn(ws, SurfaceKind::kWeb);

  Check(m.CycleColumnWidth(99999, a) == 0.0,
        "CycleColumnWidth rejects a missing workspace");
  Check(m.CycleColumnWidth(ws, 424242) == 0.0,
        "CycleColumnWidth rejects a missing pane");

  double f = m.CycleColumnWidth(ws, a);
  Check(f == 2.0 / 3.0, "full-width first column advances to two-thirds");
  Check(m.GetWorkspace(ws)->column_widths[m.ColumnIndexOf(ws, b)] == 0.0,
        "cycling the first column leaves the second column unchanged");

  f = m.CycleColumnWidth(ws, b);
  Check(f == 1.0, "a non-mode default starts at the first mode");
  Check(m.GetWorkspace(ws)->column_widths[m.ColumnIndexOf(ws, a)] == 2.0 / 3.0,
        "cycling the second column leaves the first column unchanged");
  f = m.CycleColumnWidth(ws, b);
  Check(f == 2.0 / 3.0, "full advances to two-thirds");
  f = m.CycleColumnWidth(ws, b);
  Check(f == 0.5, "two-thirds advances to half");
  f = m.CycleColumnWidth(ws, b);
  Check(f == 1.0 / 3.0, "half advances to one-third");
  f = m.CycleColumnWidth(ws, b);
  Check(f == 1.0, "one-third wraps to full");

  m.GetWorkspace(ws)->column_widths[m.ColumnIndexOf(ws, b)] = 0.61;
  const std::vector<double> custom_modes = {0.8, 0.4};
  f = m.CycleColumnWidth(ws, b, custom_modes);
  Check(f == 0.8, "custom modes start at their first declared value");
  f = m.CycleColumnWidth(ws, b, custom_modes);
  Check(f == 0.4, "custom mode count and order drive cycling");

  f = m.SetColumnWidth(ws, b, 0.73);
  Check(f == 0.73, "freeform mouse width is stored");
  f = m.SetColumnWidth(ws, b, 9.0);
  Check(f == 2.0, "freeform mouse width is clamped at 200 percent");
  Check(m.SetColumnWidth(99999, b, 0.5) == 0.0,
        "SetColumnWidth rejects a missing workspace");
  CheckInvariants(m, "CycleColumnWidth preserves invariants");
}

void TestNeighborPane() {
  WindowModel m;
  WorkspaceId ws = m.AddWorkspace(SurfaceKind::kWeb, "w");
  PaneId a = m.GetWorkspace(ws)->columns[0].pane->id;
  PaneId b = m.AddColumn(ws, SurfaceKind::kWeb);
  PaneId c = m.SplitPane(ws, b, SplitOrientation::kVertical, 0.5);
  PaneId d = m.AddColumn(ws, SurfaceKind::kWeb);
  // Columns: [a] [b over c] [d].

  Check(m.NeighborPane(ws, a, Direction::kRight) == b,
        "right of a is the next column's first pane");
  Check(m.NeighborPane(ws, c, Direction::kRight) == d,
        "right works from any pane in the column");
  Check(m.NeighborPane(ws, c, Direction::kLeft) == a,
        "left of the middle column is a");
  Check(m.NeighborPane(ws, d, Direction::kLeft) == b,
        "left lands on the adjacent column's FIRST pane");
  Check(m.NeighborPane(ws, a, Direction::kLeft) == kInvalidId,
        "no column left of the first");
  Check(m.NeighborPane(ws, d, Direction::kRight) == kInvalidId,
        "no column right of the last");

  Check(m.NeighborPane(ws, b, Direction::kDown) == c,
        "down moves within the column");
  Check(m.NeighborPane(ws, c, Direction::kUp) == b,
        "up moves within the column");
  Check(m.NeighborPane(ws, b, Direction::kUp) == kInvalidId,
        "no pane above the column's first");
  Check(m.NeighborPane(ws, c, Direction::kDown) == kInvalidId,
        "no pane below the column's last");
  Check(m.NeighborPane(ws, a, Direction::kUp) == kInvalidId &&
            m.NeighborPane(ws, a, Direction::kDown) == kInvalidId,
        "single-pane column has no vertical neighbors");
  Check(m.NeighborPane(ws, 424242, Direction::kLeft) == kInvalidId,
        "missing pane has no neighbors");
}

// ---- move tab across panes --------------------------------------------------

void TestMoveTabAcrossPanes() {
  WindowModel m;
  WorkspaceId ws = m.AddWorkspace(SurfaceKind::kWeb, "w");
  PaneId p0 = m.GetWorkspace(ws)->columns[0].pane->id;
  SurfaceTabId keep = m.GetWorkspace(ws)->columns[0].pane->tabs[0].id;
  SurfaceTabId mover = m.AddTab(ws, p0, SurfaceKind::kWeb, "mover");
  PaneId p1 = m.SplitPane(ws, p0, SplitOrientation::kHorizontal, 0.5);

  // p0 has [keep, mover]; move `mover` into p1 (append).
  m.MoveTab(ws, p0, mover, ws, p1, -1);
  Check(m.FindPane(ws, p0)->tabs.size() == 1, "source lost the tab");
  Check(m.FindPane(ws, p0)->tabs[0].id == keep, "source kept the other tab");
  Pane* dst = m.FindPane(ws, p1);
  Check(dst->IndexOfTab(mover) >= 0, "dest received the tab");
  Check(dst->selected == mover, "dest selects the moved tab");

  // Move the only remaining tab out of p1's original tab, emptying it... p1 had
  // its own seed tab + mover = 2 tabs. Move both out -> p1 collapses.
  SurfaceTabId seed =
      dst->tabs[0].id == mover ? dst->tabs[1].id : dst->tabs[0].id;
  m.MoveTab(ws, p1, mover, ws, p0, -1);
  m.MoveTab(ws, p1, seed, ws, p0, -1);
  Check(PaneCount(m, ws) == 1, "emptied pane collapsed away");
  Check(m.FindPane(ws, p0)->tabs.size() == 3, "all tabs landed in p0");
}

bool TabOrderIs(const Pane* pane, std::initializer_list<SurfaceTabId> ids) {
  if (!pane || pane->tabs.size() != ids.size()) {
    return false;
  }
  size_t i = 0;
  for (SurfaceTabId id : ids) {
    if (pane->tabs[i].id != id) {
      return false;
    }
    ++i;
  }
  return true;
}

void TestCloseTabLastTabCollapsesSplitPane() {
  WindowModel m;
  WorkspaceId ws = m.AddWorkspace(SurfaceKind::kWeb, "w");
  PaneId p0 = m.GetWorkspace(ws)->columns[0].pane->id;
  PaneId p1 = m.SplitPane(ws, p0, SplitOrientation::kHorizontal, 0.5);
  SurfaceTabId t = m.FindPane(ws, p1)->tabs[0].id;

  m.CloseTab(ws, p1, t);
  Check(PaneCount(m, ws) == 1, "last-tab close collapsed the split pane");
  Check(m.FindPane(ws, p1) == nullptr, "closed split pane is gone");
  Check(m.FindPane(ws, p0) != nullptr, "split sibling survives");
  Check(m.GetWorkspace(ws)->columns[0].is_pane(),
        "split collapsed back to a pane");
}

void TestMoveTabWildcardLonePaneLastTabRefuses() {
  WindowModel m;
  WorkspaceId source_ws = m.AddWorkspace(SurfaceKind::kWeb, "source");
  PaneId source_pane = m.GetWorkspace(source_ws)->columns[0].pane->id;
  SurfaceTabId source_tab = m.FindPane(source_ws, source_pane)->tabs[0].id;
  WorkspaceId dest_ws = m.AddWorkspace(SurfaceKind::kWeb, "dest");
  PaneId dest_pane = m.GetWorkspace(dest_ws)->columns[0].pane->id;
  SurfaceTabId dest_tab = m.FindPane(dest_ws, dest_pane)->tabs[0].id;

  m.MoveTab(kInvalidId, source_pane, source_tab, dest_ws, dest_pane, -1);
  Check(PaneCount(m, source_ws) == 1,
        "wildcard move keeps source workspace's lone pane");
  Check(m.FindPane(source_ws, source_pane)->tabs.size() == 1,
        "wildcard move refuses to empty the lone source pane");
  Check(m.FindPane(source_ws, source_pane)->tabs[0].id == source_tab,
        "wildcard move leaves source tab in place");
  Check(m.FindPane(source_ws, source_pane)->selected == source_tab,
        "wildcard move leaves source selection unchanged");
  Check(TabOrderIs(m.FindPane(dest_ws, dest_pane), {dest_tab}),
        "wildcard refused move leaves destination unchanged");
}

void TestMoveTabSamePaneEarlierToLaterSelected() {
  WindowModel m;
  WorkspaceId ws = m.AddWorkspace(SurfaceKind::kWeb, "w");
  PaneId p = m.GetWorkspace(ws)->columns[0].pane->id;
  SurfaceTabId t0 = m.FindPane(ws, p)->tabs[0].id;
  SurfaceTabId t1 = m.AddTab(ws, p, SurfaceKind::kWeb, "t1");
  SurfaceTabId t2 = m.AddTab(ws, p, SurfaceKind::kWeb, "t2");

  m.SelectTab(ws, p, t0);
  m.MoveTab(ws, p, t0, ws, p, 2);
  Check(TabOrderIs(m.FindPane(ws, p), {t1, t0, t2}),
        "same-pane selected earlier-to-later adjusts insertion index");
  Check(m.FindPane(ws, p)->selected == t0,
        "same-pane selected earlier-to-later follows moved tab");
}

void TestMoveTabSamePaneLaterToEarlierSelected() {
  WindowModel m;
  WorkspaceId ws = m.AddWorkspace(SurfaceKind::kWeb, "w");
  PaneId p = m.GetWorkspace(ws)->columns[0].pane->id;
  SurfaceTabId t0 = m.FindPane(ws, p)->tabs[0].id;
  SurfaceTabId t1 = m.AddTab(ws, p, SurfaceKind::kWeb, "t1");
  SurfaceTabId t2 = m.AddTab(ws, p, SurfaceKind::kWeb, "t2");

  m.SelectTab(ws, p, t2);
  m.MoveTab(ws, p, t2, ws, p, 0);
  Check(TabOrderIs(m.FindPane(ws, p), {t2, t0, t1}),
        "same-pane selected later-to-earlier moves to target index");
  Check(m.FindPane(ws, p)->selected == t2,
        "same-pane selected later-to-earlier follows moved tab");
}

void TestMoveTabSamePaneEarlierToLaterNonSelected() {
  WindowModel m;
  WorkspaceId ws = m.AddWorkspace(SurfaceKind::kWeb, "w");
  PaneId p = m.GetWorkspace(ws)->columns[0].pane->id;
  SurfaceTabId t0 = m.FindPane(ws, p)->tabs[0].id;
  SurfaceTabId t1 = m.AddTab(ws, p, SurfaceKind::kWeb, "t1");
  SurfaceTabId t2 = m.AddTab(ws, p, SurfaceKind::kWeb, "t2");

  m.SelectTab(ws, p, t1);
  m.MoveTab(ws, p, t0, ws, p, 2);
  Check(TabOrderIs(m.FindPane(ws, p), {t1, t0, t2}),
        "same-pane non-selected earlier-to-later adjusts insertion index");
  Check(m.FindPane(ws, p)->selected == t1,
        "same-pane non-selected earlier-to-later preserves selection");
}

void TestMoveTabSamePaneLaterToEarlierNonSelected() {
  WindowModel m;
  WorkspaceId ws = m.AddWorkspace(SurfaceKind::kWeb, "w");
  PaneId p = m.GetWorkspace(ws)->columns[0].pane->id;
  SurfaceTabId t0 = m.FindPane(ws, p)->tabs[0].id;
  SurfaceTabId t1 = m.AddTab(ws, p, SurfaceKind::kWeb, "t1");
  SurfaceTabId t2 = m.AddTab(ws, p, SurfaceKind::kWeb, "t2");

  m.SelectTab(ws, p, t1);
  m.MoveTab(ws, p, t2, ws, p, 0);
  Check(TabOrderIs(m.FindPane(ws, p), {t2, t0, t1}),
        "same-pane non-selected later-to-earlier moves to target index");
  Check(m.FindPane(ws, p)->selected == t1,
        "same-pane non-selected later-to-earlier preserves selection");
}

void TestReorderTab() {
  WindowModel m;
  WorkspaceId ws = m.AddWorkspace(SurfaceKind::kWeb, "w");
  PaneId p = m.GetWorkspace(ws)->columns[0].pane->id;
  SurfaceTabId a = m.FindPane(ws, p)->tabs[0].id;
  m.FindPane(ws, p)->tabs[0].title = "a";
  SurfaceTabId b = m.AddTab(ws, p, SurfaceKind::kWeb, "b");
  SurfaceTabId c = m.AddTab(ws, p, SurfaceKind::kWeb, "c");
  SurfaceTabId d = m.AddTab(ws, p, SurfaceKind::kWeb, "d");
  m.SelectTab(ws, p, c);

  Check(m.ReorderTab(ws, p, b, 3) == b, "reorder returns moved tab");
  Check(TabOrder(m.FindPane(ws, p)) == "a,c,d,b",
        "to_index is the desired final index when moving right");
  Check(m.FindPane(ws, p)->selected == c, "reorder keeps selection by id");
  Check(m.ReorderTab(ws, p, d, 0) == d, "reorder left returns moved tab");
  Check(TabOrder(m.FindPane(ws, p)) == "d,a,c,b",
        "to_index is the desired final index when moving left");
  Check(m.ReorderTab(ws, p, a, 99) == a, "reorder clamps high");
  Check(TabOrder(m.FindPane(ws, p)) == "d,c,b,a", "clamped high appends");
  Check(m.ReorderTab(ws, p, a, -5) == a, "reorder clamps low");
  Check(TabOrder(m.FindPane(ws, p)) == "a,d,c,b", "clamped low prepends");
  Check(m.ReorderTab(ws, p, 424242, 0) == kInvalidId,
        "reorder missing tab fails");
  CheckInvariants(m, "reorder preserves invariants");
}

void TestSetTabLoading() {
  WindowModel m;
  WorkspaceId ws = m.AddWorkspace(SurfaceKind::kWeb, "w");
  PaneId p = m.GetWorkspace(ws)->columns[0].pane->id;
  SurfaceTabId tab = m.FindPane(ws, p)->tabs[0].id;

  Check(!m.FindPane(ws, p)->tabs[0].loading, "tab starts not loading");
  Check(m.SetTabLoading(ws, p, tab, true), "loading change returns true");
  Check(m.FindPane(ws, p)->tabs[0].loading, "loading value changed");
  Check(!m.SetTabLoading(ws, p, tab, true), "same loading is a no-op");
  Check(m.SetTabLoading(ws, p, tab, false), "loading can clear");
  Check(!m.FindPane(ws, p)->tabs[0].loading, "loading value cleared");
  Check(!m.SetTabLoading(ws, p, 424242, true),
        "missing tab loading is a no-op");
  CheckInvariants(m, "SetTabLoading preserves invariants");
}

void TestSplitPaneWithTab() {
  {
    WindowModel m;
    WorkspaceId ws = m.AddWorkspace(SurfaceKind::kWeb, "w");
    PaneId p = m.GetWorkspace(ws)->columns[0].pane->id;
    SurfaceTabId keep = m.FindPane(ws, p)->tabs[0].id;
    SurfaceTabId moved = m.AddTab(ws, p, SurfaceKind::kTerminal, "moved");
    PaneId fresh = m.SplitPaneWithTab(ws, p, SplitOrientation::kHorizontal,
                                      true, p, moved);
    Check(fresh != kInvalidId, "same-pane split with one tab succeeds");
    Check(m.FindPane(ws, p)->tabs.size() == 1 &&
              m.FindPane(ws, p)->tabs[0].id == keep,
          "same-pane split leaves the source pane with its other tab");
    Check(m.FindPane(ws, fresh)->tabs.size() == 1 &&
              m.FindPane(ws, fresh)->tabs[0].id == moved,
          "fresh split pane holds only the moved tab");
    std::vector<PaneId> order = m.PanesOf(ws);
    Check(order.size() == 2 && order[0] == fresh && order[1] == p,
          "insert_first puts the fresh pane first");
    Check(m.GetWorkspace(ws)->focused == fresh, "split-with-tab focuses fresh");
    CheckInvariants(m, "same-pane SplitPaneWithTab preserves invariants");
  }

  {
    WindowModel m;
    WorkspaceId ws = m.AddWorkspace(SurfaceKind::kWeb, "w");
    PaneId a = m.GetWorkspace(ws)->columns[0].pane->id;
    PaneId b = m.SplitPane(ws, a, SplitOrientation::kHorizontal, 0.5);
    SurfaceTabId moved = m.FindPane(ws, a)->tabs[0].id;
    PaneId fresh =
        m.SplitPaneWithTab(ws, b, SplitOrientation::kVertical, false, a, moved);
    Check(fresh != kInvalidId, "same-column source collapse succeeds");
    Check(m.FindPane(ws, a) == nullptr, "emptied same-column source closed");
    Check(m.FindPane(ws, b) != nullptr && m.FindPane(ws, fresh) != nullptr,
          "target and fresh pane survive source collapse");
    Check(m.ColumnCountOf(ws) == 1 && PaneCount(m, ws) == 2,
          "source collapse keeps the target split in the column");
    CheckInvariants(m, "same-column SplitPaneWithTab preserves invariants");
  }

  {
    WindowModel m;
    WorkspaceId ws = m.AddWorkspace(SurfaceKind::kWeb, "w");
    PaneId a = m.GetWorkspace(ws)->columns[0].pane->id;
    PaneId b = m.AddColumn(ws, SurfaceKind::kWeb);
    SurfaceTabId moved = m.FindPane(ws, a)->tabs[0].id;
    PaneId fresh = m.SplitPaneWithTab(ws, b, SplitOrientation::kHorizontal,
                                      true, a, moved);
    Check(fresh != kInvalidId, "cross-column split-with-tab succeeds");
    Check(m.FindPane(ws, a) == nullptr, "emptied source column removed");
    Check(m.ColumnCountOf(ws) == 1 && PaneCount(m, ws) == 2,
          "target column remains and contains the split");
    CheckInvariants(m, "cross-column SplitPaneWithTab preserves invariants");
  }

  {
    WindowModel m;
    WorkspaceId ws = m.AddWorkspace(SurfaceKind::kWeb, "w");
    PaneId p = m.GetWorkspace(ws)->columns[0].pane->id;
    SurfaceTabId only = m.FindPane(ws, p)->tabs[0].id;
    Check(m.SplitPaneWithTab(ws, p, SplitOrientation::kHorizontal, true, p,
                             only) == kInvalidId,
          "guard rejects emptying the target being split");
    Check(PaneCount(m, ws) == 1 && m.FindPane(ws, p)->tabs.size() == 1,
          "guard leaves the model unchanged");
    CheckInvariants(m, "guarded SplitPaneWithTab preserves invariants");
  }
}

void TestMoveTabToNewColumn() {
  {
    WindowModel m;
    WorkspaceId ws = m.AddWorkspace(SurfaceKind::kWeb, "w");
    PaneId a = m.GetWorkspace(ws)->columns[0].pane->id;
    SurfaceTabId keep = m.FindPane(ws, a)->tabs[0].id;
    SurfaceTabId moved = m.AddTab(ws, a, SurfaceKind::kWeb, "moved");
    PaneId fresh = m.MoveTabToNewColumn(ws, a, moved, 0);
    Check(fresh != kInvalidId, "move tab to new column succeeds");
    Check(m.ColumnIndexOf(ws, fresh) == 0, "new column inserted at index");
    Check(m.FindPane(ws, a)->tabs.size() == 1 &&
              m.FindPane(ws, a)->tabs[0].id == keep,
          "source pane keeps remaining tab");
    CheckInvariants(m, "MoveTabToNewColumn with non-empty source preserves");
  }

  {
    WindowModel m;
    WorkspaceId ws = m.AddWorkspace(SurfaceKind::kWeb, "w");
    PaneId a = m.GetWorkspace(ws)->columns[0].pane->id;
    PaneId b = m.AddColumn(ws, SurfaceKind::kWeb);
    SurfaceTabId moved = m.FindPane(ws, a)->tabs[0].id;
    PaneId fresh = m.MoveTabToNewColumn(ws, a, moved, -1);
    std::vector<PaneId> order = m.PanesOf(ws);
    Check(order.size() == 2 && order[0] == b && order[1] == fresh,
          "dragging A to the gap right of B yields [B,new]");
    Check(m.FindPane(ws, a) == nullptr, "source column A removed");
    CheckInvariants(m, "append MoveTabToNewColumn preserves invariants");
  }

  {
    WindowModel m;
    WorkspaceId ws = m.AddWorkspace(SurfaceKind::kWeb, "w");
    PaneId a = m.GetWorkspace(ws)->columns[0].pane->id;
    PaneId b = m.AddColumn(ws, SurfaceKind::kWeb);
    SurfaceTabId moved = m.FindPane(ws, a)->tabs[0].id;
    PaneId fresh = m.MoveTabToNewColumn(ws, a, moved, 0);
    std::vector<PaneId> order = m.PanesOf(ws);
    Check(order.size() == 2 && order[0] == fresh && order[1] == b,
          "dragging A to the gap left of A yields [new,B]");
    CheckInvariants(m, "left-gap MoveTabToNewColumn preserves invariants");
  }

  {
    WindowModel m;
    WorkspaceId ws = m.AddWorkspace(SurfaceKind::kWeb, "w");
    PaneId a = m.GetWorkspace(ws)->columns[0].pane->id;
    SurfaceTabId moved = m.FindPane(ws, a)->tabs[0].id;
    PaneId fresh = m.MoveTabToNewColumn(ws, a, moved, -1);
    Check(fresh != kInvalidId && m.FindPane(ws, a) == nullptr,
          "moving the only tab of the only pane creates a replacement pane");
    Check(m.ColumnCountOf(ws) == 1 && PaneCount(m, ws) == 1,
          "only-pane new-column move degenerates to one valid column");
    CheckInvariants(m, "only-pane MoveTabToNewColumn preserves invariants");
  }
}

void TestMergePaneInto() {
  WindowModel m;
  WorkspaceId ws = m.AddWorkspace(SurfaceKind::kWeb, "w");
  PaneId from = m.GetWorkspace(ws)->columns[0].pane->id;
  m.FindPane(ws, from)->tabs[0].title = "a";
  SurfaceTabId b = m.AddTab(ws, from, SurfaceKind::kWeb, "b");
  SurfaceTabId c = m.AddTab(ws, from, SurfaceKind::kWeb, "c");
  m.SelectTab(ws, from, b);
  PaneId to = m.AddColumn(ws, SurfaceKind::kWeb);
  m.FindPane(ws, to)->tabs[0].title = "x";

  Check(m.MergePaneInto(ws, from, to, 0), "merge pane succeeds");
  Check(m.FindPane(ws, from) == nullptr, "source pane closed after merge");
  Check(TabOrder(m.FindPane(ws, to)) == "a,b,c,x",
        "merge inserts all tabs in order at the clamped index");
  Check(m.FindPane(ws, to)->selected == b,
        "merge selects the source's previously selected tab");
  Check(!m.MergePaneInto(ws, to, to, -1), "merge onto self is rejected");
  CheckInvariants(m, "MergePaneInto preserves invariants");
  (void)c;
}

void TestMovePaneOps() {
  {
    WindowModel m;
    WorkspaceId ws = m.AddWorkspace(SurfaceKind::kWeb, "w");
    PaneId a = m.GetWorkspace(ws)->columns[0].pane->id;
    PaneId b = m.AddColumn(ws, SurfaceKind::kWeb);
    PaneId moved =
        m.MovePaneToSplit(ws, a, b, SplitOrientation::kVertical, false);
    Check(moved == a, "MovePaneToSplit returns the moved pane");
    Check(m.ColumnCountOf(ws) == 1 && PaneCount(m, ws) == 2,
          "moving a column pane to a split removes its old column");
    Check(m.GetWorkspace(ws)->focused == a, "MovePaneToSplit focuses moved");
    CheckInvariants(m, "MovePaneToSplit preserves invariants");
  }

  {
    WindowModel m;
    WorkspaceId ws = m.AddWorkspace(SurfaceKind::kWeb, "w");
    PaneId a = m.GetWorkspace(ws)->columns[0].pane->id;
    PaneId b = m.SplitPane(ws, a, SplitOrientation::kHorizontal, 0.5);
    PaneId moved = m.MovePaneToNewColumn(ws, b, 0);
    Check(moved == b, "MovePaneToNewColumn returns the moved pane");
    Check(m.ColumnIndexOf(ws, b) == 0 && m.ColumnIndexOf(ws, a) == 1,
          "nested pane moved into a new column at the requested slot");
    CheckInvariants(m, "MovePaneToNewColumn preserves invariants");
  }
}

// ---- shallow workspace groups + rail ---------------------------------------

void TestWorkspaceGroups() {
  WindowModel m;
  WorkspaceId a = m.AddWorkspace(SurfaceKind::kWeb, "a");
  WorkspaceId b = m.AddWorkspace(SurfaceKind::kWeb, "b");
  WorkspaceId c = m.AddWorkspace(SurfaceKind::kWeb, "c");
  WorkspaceId d = m.AddWorkspace(SurfaceKind::kWeb, "d");

  // Flat workspaces are ordinary depth-0 rows.
  std::vector<RailItem> rail = m.BuildRail();
  Check(rail.size() == 4, "flat rail has 4 workspace rows");
  Check(rail[0].workspace == a && rail[0].depth == 0 &&
            rail[0].group == kInvalidId,
        "row a is an ungrouped workspace");

  // Group creation is an explicit command. The rail's drag path only targets
  // an already-existing group and never invokes this operation implicitly.
  WorkspaceGroupId explicit_group = m.CreateWorkspaceGroup(a);
  Check(explicit_group != kInvalidId &&
            m.GetWorkspace(a)->group == explicit_group,
        "explicit command creates a one-workspace group");
  WorkspaceGroupId replacement_group = m.CreateWorkspaceGroup(a);
  Check(replacement_group != kInvalidId &&
            replacement_group != explicit_group &&
            m.GetWorkspace(a)->group == replacement_group &&
            m.GetWorkspaceGroup(explicit_group) == nullptr,
        "add to new group replaces an existing one-workspace group");
  m.UngroupWorkspace(a);
  Check(m.GetWorkspace(a)->group == kInvalidId &&
            m.GetWorkspaceGroup(replacement_group) == nullptr,
        "explicit ungroup removes an empty group");

  // Moving b onto a creates a dedicated group containing both workspaces.
  m.MoveWorkspace(b, a, -1);
  WorkspaceGroupId group = m.GetWorkspace(a)->group;
  Check(group != kInvalidId, "moving onto a workspace creates a group");
  Check(m.GetWorkspace(b)->group == group,
        "source and target share the new group");
  Check(m.GetWorkspaceGroup(group) && m.GetWorkspaceGroup(group)->title == "a",
        "new group inherits the target workspace title");

  // Additional drops join the existing group; groups never nest.
  m.MoveWorkspace(c, a, -1);
  m.MoveWorkspace(c, b, -1);
  rail = m.BuildRail();
  Check(rail.size() == 4 && rail[0].first_in_group && rail[0].workspace == a &&
            rail[0].depth == 1 && rail[1].workspace == b &&
            rail[1].depth == 1 && rail[2].workspace == c &&
            rail[2].last_in_group,
        "group members stay contiguous at one shallow depth");

  // Collapsing an active group selects the preceding visible workspace when
  // possible, otherwise the following one (d here), and hides only members.
  m.SelectWorkspace(c);
  m.SetWorkspaceExpanded(a, false);
  rail = m.BuildRail();
  Check(m.GetWorkspaceGroup(group)->collapsed,
        "group stores collapsed state independently");
  Check(m.selected_workspace() == d &&
            m.workspace_selection_anchor() == d && SelectionIs(m, {d}),
        "collapsing active leading group exclusively selects following row");
  Check(rail.size() == 1 && rail[0].workspace == d,
        "collapsed group hides member workspace rows");

  // A successful Chromium group-header drag resets selection to the group's
  // active member. The workspace adaptation must use ordinary selection for
  // that final step so moving a collapsed group cannot leave its active
  // workspace hidden.
  m.MoveWorkspaces({a, b, c}, kInvalidId, 4);
  m.SelectWorkspace(b);
  Check(m.roots() == std::vector<WorkspaceId>({d, a, b, c}) &&
            m.GetWorkspace(a)->group == group &&
            m.GetWorkspace(b)->group == group &&
            m.GetWorkspace(c)->group == group &&
            !m.GetWorkspaceGroup(group)->collapsed &&
            SelectionIs(m, {b}) && m.selected_workspace() == b &&
            m.workspace_selection_anchor() == b && m.BuildRail().size() == 4,
        "successful collapsed-group drag reveals its active member");

  // Ungrouping c preserves the group for a/b and moves c as a flat item.
  m.MoveWorkspace(c, kInvalidId, -1);
  Check(m.GetWorkspace(c)->group == kInvalidId,
        "moving to top level ungroups workspace");
  Check(m.WorkspacesInGroup(group).size() == 2,
        "remaining workspaces stay grouped");
  Check(m.roots().back() == c, "ungrouped workspace appended in rail order");

  // Closing one group member does not dissolve or repurpose the group.
  m.CloseWorkspace(a);
  Check(m.GetWorkspace(a) == nullptr, "group member a closed");
  Check(m.GetWorkspace(b) && m.GetWorkspace(b)->group == group &&
            m.GetWorkspaceGroup(group),
        "single-workspace group remains valid");

  m.SetWorkspaceColor(b, GroupColor::kPurple);
  m.SetWorkspaceGroupTitle(group, "Project");
  Check(m.GetWorkspaceGroup(group)->color == GroupColor::kPurple &&
            m.GetWorkspaceGroup(group)->title == "Project",
        "group visual data is independent and editable");

  // Closing a selected flat workspace may choose the first member of a
  // collapsed group; that group must open so selection never becomes hidden.
  WindowModel close_model;
  WorkspaceId g0 = close_model.AddWorkspace(SurfaceKind::kWeb, "g0");
  WorkspaceId g1 = close_model.AddWorkspace(SurfaceKind::kWeb, "g1");
  WorkspaceId flat = close_model.AddWorkspace(SurfaceKind::kWeb, "flat");
  close_model.MoveWorkspace(g1, g0, -1);
  close_model.SetWorkspaceExpanded(g0, false);
  close_model.SelectWorkspace(flat);
  close_model.CloseWorkspace(flat);
  Check(close_model.selected_workspace() == g1 &&
            SelectionIs(close_model, {g1}) &&
            close_model.workspace_selection_anchor() == g1 &&
            !close_model.GetWorkspaceGroup(close_model.GetWorkspace(g1)->group)
                 ->collapsed,
        "closing into a collapsed neighbor reveals the new selection");
}

void TestWorkspaceGroupMiddleExtraction() {
  {
    WindowModel m;
    WorkspaceId a = m.AddWorkspace(SurfaceKind::kWeb, "a");
    WorkspaceId b = m.AddWorkspace(SurfaceKind::kWeb, "b");
    WorkspaceId c = m.AddWorkspace(SurfaceKind::kWeb, "c");
    m.MoveWorkspace(b, a, -1);
    m.MoveWorkspace(c, a, -1);
    const WorkspaceGroupId group = m.GetWorkspace(a)->group;

    m.UngroupWorkspace(b);

    Check(m.roots() == std::vector<WorkspaceId>({a, c, b}),
          "ungrouping a middle member moves it after the remaining group");
    Check(m.WorkspacesInGroup(group) == std::vector<WorkspaceId>({a, c}) &&
              m.GetWorkspace(b)->group == kInvalidId,
          "middle ungroup preserves remaining member order and contiguity");
    const std::vector<RailItem> rail = m.BuildRail();
    Check(rail.size() == 3 && rail[0].workspace == a &&
              rail[0].first_in_group && rail[1].workspace == c &&
              rail[1].last_in_group && rail[2].workspace == b &&
              rail[2].group == kInvalidId,
          "middle ungroup visual order matches the contiguous model order");
    CheckInvariants(m, "middle ungroup preserves model invariants");
  }

  {
    WindowModel m;
    WorkspaceId a = m.AddWorkspace(SurfaceKind::kWeb, "a");
    WorkspaceId b = m.AddWorkspace(SurfaceKind::kWeb, "b");
    WorkspaceId c = m.AddWorkspace(SurfaceKind::kWeb, "c");
    m.MoveWorkspace(b, a, -1);
    m.MoveWorkspace(c, a, -1);
    const WorkspaceGroupId previous_group = m.GetWorkspace(a)->group;

    const WorkspaceGroupId new_group = m.CreateWorkspaceGroup(b);

    Check(m.roots() == std::vector<WorkspaceId>({a, c, b}),
          "new-group extraction moves a middle member after its old group");
    Check(new_group != kInvalidId && new_group != previous_group &&
              m.WorkspacesInGroup(previous_group) ==
                  std::vector<WorkspaceId>({a, c}) &&
              m.WorkspacesInGroup(new_group) ==
                  std::vector<WorkspaceId>({b}),
          "new-group extraction keeps both groups contiguous and ordered");
    const std::vector<RailItem> rail = m.BuildRail();
    Check(rail.size() == 3 && rail[0].workspace == a &&
              rail[0].first_in_group && rail[1].workspace == c &&
              rail[1].last_in_group && rail[2].workspace == b &&
              rail[2].first_in_group && rail[2].last_in_group,
          "new-group extraction visual order matches the contiguous model");
    CheckInvariants(m, "middle new-group extraction preserves invariants");
  }
}

void TestWorkspaceBatchUngroup() {
  WindowModel m;
  WorkspaceId a = m.AddWorkspace(SurfaceKind::kWeb, "a");
  WorkspaceId b = m.AddWorkspace(SurfaceKind::kWeb, "b");
  WorkspaceId c = m.AddWorkspace(SurfaceKind::kWeb, "c");
  WorkspaceId d = m.AddWorkspace(SurfaceKind::kWeb, "d");
  m.MoveWorkspace(b, a, -1);
  m.MoveWorkspace(c, a, -1);
  m.MoveWorkspace(d, a, -1);
  const WorkspaceGroupId group = m.GetWorkspace(a)->group;
  m.SelectWorkspace(b);
  m.ToggleWorkspaceSelection(c);

  // Reverse the request order to prove that an atomic extraction uses the
  // existing rail order. Sequential singleton extraction would produce
  // A,D,C,B here, reversing the selected B/C run.
  m.UngroupWorkspaces({c, b});

  Check(m.roots() == std::vector<WorkspaceId>({a, d, b, c}),
        "batch ungroup preserves detached workspace rail order");
  Check(m.WorkspacesInGroup(group) == std::vector<WorkspaceId>({a, d}) &&
            m.GetWorkspace(b)->group == kInvalidId &&
            m.GetWorkspace(c)->group == kInvalidId,
        "batch ungroup keeps the surviving group contiguous");
  Check(SelectionIs(m, {b, c}) && m.selected_workspace() == c &&
            m.workspace_selection_anchor() == c,
        "batch ungroup preserves selection, active workspace, and anchor");
  const std::vector<RailItem> rail = m.BuildRail();
  Check(rail.size() == 4 && rail[0].workspace == a &&
            rail[0].first_in_group && rail[1].workspace == d &&
            rail[1].last_in_group && rail[2].workspace == b &&
            rail[2].group == kInvalidId && rail[3].workspace == c &&
            rail[3].group == kInvalidId,
        "batch ungroup rail mirrors the atomic model order");
  CheckInvariants(m, "batch ungroup preserves model invariants");

  m.UngroupWorkspaces({d, b, a, d, kInvalidId});
  Check(m.roots() == std::vector<WorkspaceId>({a, d, b, c}) &&
            m.GetWorkspace(a)->group == kInvalidId &&
            m.GetWorkspace(d)->group == kInvalidId &&
            m.GetWorkspaceGroup(group) == nullptr,
        "batch ungroup removes an entire group without reordering");
}

void TestWorkspaceLayoutMode() {
  WindowModel m;
  WorkspaceId a = m.AddWorkspace(SurfaceKind::kWeb, "a");
  WorkspaceId b = m.AddWorkspace(SurfaceKind::kWeb, "b");

  Check(m.GetWorkspace(a)->layout_mode == WorkspaceLayoutMode::kStrip,
        "new workspaces default to strip layout");
  Check(m.ToggleWorkspaceLayoutMode(a) == WorkspaceLayoutMode::kTiled,
        "toggle moves strip to tiled");
  Check(m.GetWorkspace(a)->layout_mode == WorkspaceLayoutMode::kTiled,
        "toggle stores tiled on the selected workspace");
  Check(m.GetWorkspace(b)->layout_mode == WorkspaceLayoutMode::kStrip,
        "layout mode is per-workspace");
  Check(m.ToggleWorkspaceLayoutMode(a) == WorkspaceLayoutMode::kStrip,
        "toggle moves tiled back to strip");

  m.SetWorkspaceLayoutMode(b, WorkspaceLayoutMode::kTiled);
  Check(m.GetWorkspace(b)->layout_mode == WorkspaceLayoutMode::kTiled,
        "explicit layout mode set works");
  Check(m.ToggleWorkspaceLayoutMode(99999) == WorkspaceLayoutMode::kStrip,
        "toggle of missing workspace returns strip");
}

}  // namespace

int main() {
  std::printf("window_model_test\n");
  TestWorkspaceLifecycle();
  TestDuplicateWorkspace();
  TestMoveWorkspace();
  TestMoveWorkspaces();
  TestFlatMovesRespectWorkspaceGroupBoundaries();
  TestWorkspaceMultiSelection();
  TestWorkspaceGroupCollapseSelectionParity();
  TestWorkspaceRegistryIdentityAndOrder();
  TestTabs();
  TestSplitAndCollapse();
  TestSplitRatio();
  TestAddColumn();
  TestPanesOfOrderWithSplits();
  TestClosePaneColumns();
  TestCloseTabRemovesColumn();
  TestColumnWidthInvariantAfterColumnOps();
  TestCycleColumnWidth();
  TestNeighborPane();
  TestMoveTabAcrossPanes();
  TestCloseTabLastTabCollapsesSplitPane();
  TestMoveTabWildcardLonePaneLastTabRefuses();
  TestMoveTabSamePaneEarlierToLaterSelected();
  TestMoveTabSamePaneLaterToEarlierSelected();
  TestMoveTabSamePaneEarlierToLaterNonSelected();
  TestMoveTabSamePaneLaterToEarlierNonSelected();
  TestReorderTab();
  TestSetTabLoading();
  TestSplitPaneWithTab();
  TestMoveTabToNewColumn();
  TestMergePaneInto();
  TestMovePaneOps();
  TestWorkspaceGroups();
  TestWorkspaceGroupMiddleExtraction();
  TestWorkspaceBatchUngroup();
  TestWorkspaceLayoutMode();
  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
