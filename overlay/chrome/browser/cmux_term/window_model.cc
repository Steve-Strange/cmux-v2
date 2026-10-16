// Copyright (c) 2026 Alasdair Monk
// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later AND MIT
//
// Contains Bonsplit-derived regions; see docs/source-provenance.md.

#include "chrome/browser/cmux_term/window_model.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <set>
#include <utility>

namespace cmux {

WorkspaceSelectionState::WorkspaceSelectionState() = default;

WorkspaceSelectionState::WorkspaceSelectionState(
    const WorkspaceSelectionState&) = default;

WorkspaceSelectionState& WorkspaceSelectionState::operator=(
    const WorkspaceSelectionState&) = default;

WorkspaceSelectionState::WorkspaceSelectionState(
    WorkspaceSelectionState&&) = default;

WorkspaceSelectionState& WorkspaceSelectionState::operator=(
    WorkspaceSelectionState&&) = default;

WorkspaceSelectionState::WorkspaceSelectionState(
    std::vector<WorkspaceId> selected,
    WorkspaceId active,
    WorkspaceId anchor)
    : selected(std::move(selected)), active(active), anchor(anchor) {}

WorkspaceSelectionState::~WorkspaceSelectionState() = default;

DuplicateWorkspaceResult::DuplicateWorkspaceResult() = default;

DuplicateWorkspaceResult::DuplicateWorkspaceResult(
    const DuplicateWorkspaceResult&) = default;

DuplicateWorkspaceResult& DuplicateWorkspaceResult::operator=(
    const DuplicateWorkspaceResult&) = default;

DuplicateWorkspaceResult::DuplicateWorkspaceResult(
    DuplicateWorkspaceResult&&) = default;

DuplicateWorkspaceResult& DuplicateWorkspaceResult::operator=(
    DuplicateWorkspaceResult&&) = default;

DuplicateWorkspaceResult::~DuplicateWorkspaceResult() = default;

// Clamp a split ratio to a sane range so neither child collapses to nothing.
double ClampSplitRatio(double ratio) {
  return std::max(kMinSplitRatio, std::min(kMaxSplitRatio, ratio));
}

bool SubtreeHasPane(const LayoutNode& node, PaneId id) {
  if (node.is_pane()) {
    return node.pane->id == id;
  }
  return SubtreeHasPane(node.split->first, id) ||
         SubtreeHasPane(node.split->second, id);
}

int ColumnOf(const Workspace& ws, PaneId id) {
  for (size_t i = 0; i < ws.columns.size(); ++i) {
    if (SubtreeHasPane(ws.columns[i], id)) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

namespace {

// A new workspace starts as one full-bleed pane. Additional columns keep the
// 0 sentinel so they use the renderer's normal niri column fraction.
constexpr double kNewWorkspaceFirstColumnFraction = 1.0;

// Returns the LayoutNode in `node`'s subtree that IS the pane with `id`
// (i.e. node.pane->id == id), or nullptr. Returns the node so callers can
// mutate it in place (e.g. turn a pane node into a split node).
LayoutNode* NodeOfPane(LayoutNode& node, PaneId id) {
  if (node.is_pane()) {
    return node.pane->id == id ? &node : nullptr;
  }
  if (LayoutNode* r = NodeOfPane(node.split->first, id)) {
    return r;
  }
  return NodeOfPane(node.split->second, id);
}

// The node holding the pane `id` in any of `ws`'s columns, or nullptr.
LayoutNode* NodeOfPaneIn(Workspace& ws, PaneId id) {
  for (LayoutNode& column : ws.columns) {
    if (LayoutNode* r = NodeOfPane(column, id)) {
      return r;
    }
  }
  return nullptr;
}

Split* SplitById(LayoutNode& node, SplitId id) {
  if (node.is_pane()) {
    return nullptr;
  }
  if (node.split->id == id) {
    return node.split.get();
  }
  if (Split* r = SplitById(node.split->first, id)) {
    return r;
  }
  return SplitById(node.split->second, id);
}

// Removes the pane `id` from the tree, collapsing its parent split into the
// sibling. Returns the node that absorbed the freed space (the subtree that
// replaced the collapsed split), or nullptr if `id` has no split parent here.
// Does NOT handle the root-is-the-pane case (the caller removes the whole
// column then -- you can't collapse a lone pane).
LayoutNode* CollapsePane(LayoutNode& node, PaneId id) {
  if (!node.is_split()) {
    return nullptr;
  }
  Split* s = node.split.get();
  if (s->first.is_pane() && s->first.pane->id == id) {
    LayoutNode keep = std::move(s->second);
    node = std::move(keep);
    return &node;
  }
  if (s->second.is_pane() && s->second.pane->id == id) {
    LayoutNode keep = std::move(s->first);
    node = std::move(keep);
    return &node;
  }
  if (LayoutNode* r = CollapsePane(s->first, id)) {
    return r;
  }
  return CollapsePane(s->second, id);
}

// Extracts `id` from a split subtree, collapsing its parent into the sibling,
// and returns the removed pane. Root-is-the-pane is handled by the caller.
std::unique_ptr<Pane> ExtractPane(LayoutNode& node, PaneId id) {
  if (!node.is_split()) {
    return nullptr;
  }
  Split* s = node.split.get();
  if (s->first.is_pane() && s->first.pane->id == id) {
    std::unique_ptr<Pane> out = std::move(s->first.pane);
    LayoutNode keep = std::move(s->second);
    node = std::move(keep);
    return out;
  }
  if (s->second.is_pane() && s->second.pane->id == id) {
    std::unique_ptr<Pane> out = std::move(s->second.pane);
    LayoutNode keep = std::move(s->first);
    node = std::move(keep);
    return out;
  }
  if (std::unique_ptr<Pane> out = ExtractPane(s->first, id)) {
    return out;
  }
  return ExtractPane(s->second, id);
}

void CollectPanes(const LayoutNode& node, std::vector<PaneId>* out) {
  if (node.is_pane()) {
    out->push_back(node.pane->id);
    return;
  }
  CollectPanes(node.split->first, out);
  CollectPanes(node.split->second, out);
}

const Pane* FirstPane(const LayoutNode& node) {
  if (node.is_pane()) {
    return node.pane.get();
  }
  if (const Pane* p = FirstPane(node.split->first)) {
    return p;
  }
  return FirstPane(node.split->second);
}

void RepairSelectionAfterRemove(Pane* pane,
                                SurfaceTabId removed,
                                int removed_index) {
  if (!pane || pane->tabs.empty() || pane->selected != removed) {
    return;
  }
  const int next =
      std::min(removed_index, static_cast<int>(pane->tabs.size()) - 1);
  pane->selected = pane->tabs[next].id;
}

std::unique_ptr<Pane> NewPaneHoldingTab(PaneId pane_id, SurfaceTab tab) {
  auto pane = std::make_unique<Pane>();
  pane->id = pane_id;
  pane->selected = tab.id;
  pane->tabs.push_back(std::move(tab));
  return pane;
}

int ClampColumnIndex(int index, int column_count) {
  if (index < 0 || index > column_count) {
    return column_count;
  }
  return index;
}

void InsertColumn(Workspace& ws, int index, LayoutNode column, double width) {
  const int at = ClampColumnIndex(index, static_cast<int>(ws.columns.size()));
  ws.columns.insert(ws.columns.begin() + at, std::move(column));
  ws.column_widths.insert(ws.column_widths.begin() + at, width);
}

void EraseColumn(Workspace& ws, int index) {
  if (index < 0 || index >= static_cast<int>(ws.columns.size())) {
    return;
  }
  ws.columns.erase(ws.columns.begin() + index);
  if (index < static_cast<int>(ws.column_widths.size())) {
    ws.column_widths.erase(ws.column_widths.begin() + index);
  }
}

double WidthForColumn(const Workspace& ws, int index) {
  if (index < 0 || index >= static_cast<int>(ws.column_widths.size())) {
    return 0.0;
  }
  return ws.column_widths[index];
}

void RepairColumnWidthCount(Workspace& ws) {
  if (ws.column_widths.size() < ws.columns.size()) {
    ws.column_widths.resize(ws.columns.size(), 0.0);
  } else if (ws.column_widths.size() > ws.columns.size()) {
    ws.column_widths.resize(ws.columns.size());
  }
}

}  // namespace

// ---- Out-of-line special members -------------------------------------------
// Chromium's clang-style plugin requires complex structs (those with vector /
// unique_ptr members) to declare ctor/dtor out-of-line; the move ops keep the
// move-only tree types movable (and avoid the implicit-copy deprecation).

Pane::Pane() = default;
Pane::~Pane() = default;
Pane::Pane(Pane&&) = default;
Pane& Pane::operator=(Pane&&) = default;

LayoutNode::LayoutNode() = default;
LayoutNode::~LayoutNode() = default;
LayoutNode::LayoutNode(LayoutNode&&) = default;
LayoutNode& LayoutNode::operator=(LayoutNode&&) = default;

Split::Split() = default;
Split::~Split() = default;
Split::Split(Split&&) = default;
Split& Split::operator=(Split&&) = default;

Workspace::Workspace() = default;
Workspace::~Workspace() = default;
Workspace::Workspace(Workspace&&) = default;
Workspace& Workspace::operator=(Workspace&&) = default;

WorkspaceGroup::WorkspaceGroup() = default;
WorkspaceGroup::~WorkspaceGroup() = default;
WorkspaceGroup::WorkspaceGroup(WorkspaceGroup&&) = default;
WorkspaceGroup& WorkspaceGroup::operator=(WorkspaceGroup&&) = default;

// ---- SurfaceTab / Pane helpers ---------------------------------------------

const SurfaceTab* Pane::SelectedTab() const {
  for (const SurfaceTab& t : tabs) {
    if (t.id == selected) {
      return &t;
    }
  }
  return nullptr;
}

SurfaceTab* Pane::FindTab(SurfaceTabId tab) {
  for (SurfaceTab& t : tabs) {
    if (t.id == tab) {
      return &t;
    }
  }
  return nullptr;
}

int Pane::IndexOfTab(SurfaceTabId tab) const {
  for (size_t i = 0; i < tabs.size(); ++i) {
    if (tabs[i].id == tab) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

// ---- LayoutNode factories ---------------------------------------------------

LayoutNode LayoutNode::MakePane(std::unique_ptr<Pane> p) {
  LayoutNode n;
  n.pane = std::move(p);
  return n;
}

LayoutNode LayoutNode::MakeSplit(std::unique_ptr<Split> s) {
  LayoutNode n;
  n.split = std::move(s);
  return n;
}

// ---- WindowModel ------------------------------------------------------------

WindowModel::WindowModel() = default;
WindowModel::~WindowModel() = default;

std::unique_ptr<Pane> WindowModel::NewPaneWithTab(SurfaceKind kind,
                                                  const std::string& title) {
  auto pane = std::make_unique<Pane>();
  pane->id = NextId();
  SurfaceTab tab;
  tab.id = NextId();
  tab.kind = kind;
  tab.title = title;
  pane->selected = tab.id;
  pane->tabs.push_back(std::move(tab));
  return pane;
}

int WindowModel::IndexOfWorkspace(WorkspaceId id) const {
  for (size_t i = 0; i < workspaces_.size(); ++i) {
    if (workspaces_[i].id == id) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

// ---- Workspaces + shallow groups -------------------------------------------

WorkspaceId WindowModel::AddWorkspace(SurfaceKind first_tab_kind,
                                      const std::string& title,
                                      const std::string& registry_key) {
  return AddChildWorkspace(kInvalidId, first_tab_kind, title, registry_key);
}

WorkspaceId WindowModel::AddChildWorkspace(WorkspaceId parent,
                                           SurfaceKind first_tab_kind,
                                           const std::string& title,
                                           const std::string& registry_key) {
  const WorkspaceId target = GetWorkspace(parent) ? parent : kInvalidId;
  Workspace ws;
  ws.id = NextId();
  ws.registry_key = registry_key;
  ws.title = title;
  std::unique_ptr<Pane> pane = NewPaneWithTab(first_tab_kind, title);
  ws.focused = pane->id;
  ws.columns.push_back(LayoutNode::MakePane(std::move(pane)));
  ws.column_widths.push_back(kNewWorkspaceFirstColumnFraction);
  const WorkspaceId id = ws.id;
  workspaces_.push_back(std::move(ws));
  roots_.push_back(id);
  selected_ = id;
  selection_anchor_ = id;
  selected_workspace_ids_.clear();
  selected_workspace_ids_.insert(id);
  if (target != kInvalidId) {
    MoveWorkspace(id, target, -1);
  }
  return id;
}

LayoutNode WindowModel::DuplicateLayoutNode(
    const LayoutNode& source,
    DuplicateWorkspaceResult* result) {
  if (source.is_pane()) {
    auto duplicate = std::make_unique<Pane>();
    duplicate->id = NextId();
    result->panes.emplace_back(source.pane->id, duplicate->id);
    for (const SurfaceTab& source_tab : source.pane->tabs) {
      SurfaceTab duplicate_tab = source_tab;
      duplicate_tab.id = NextId();
      result->tabs.emplace_back(source_tab.id, duplicate_tab.id);
      if (source.pane->selected == source_tab.id) {
        duplicate->selected = duplicate_tab.id;
      }
      duplicate->tabs.push_back(std::move(duplicate_tab));
    }
    return LayoutNode::MakePane(std::move(duplicate));
  }

  auto duplicate = std::make_unique<Split>();
  duplicate->id = NextId();
  duplicate->orientation = source.split->orientation;
  duplicate->ratio = source.split->ratio;
  duplicate->first = DuplicateLayoutNode(source.split->first, result);
  duplicate->second = DuplicateLayoutNode(source.split->second, result);
  return LayoutNode::MakeSplit(std::move(duplicate));
}

DuplicateWorkspaceResult WindowModel::DuplicateWorkspace(
    WorkspaceId source,
    std::string registry_key) {
  DuplicateWorkspaceResult result;
  const Workspace* source_workspace = GetWorkspace(source);
  if (!source_workspace) {
    return result;
  }

  Workspace duplicate;
  duplicate.id = NextId();
  duplicate.registry_key = std::move(registry_key);
  duplicate.group = source_workspace->group;
  duplicate.preferred_group_color = source_workspace->preferred_group_color;
  duplicate.title = source_workspace->title;
  duplicate.column_widths = source_workspace->column_widths;
  duplicate.layout_mode = source_workspace->layout_mode;
  for (const LayoutNode& column : source_workspace->columns) {
    duplicate.columns.push_back(DuplicateLayoutNode(column, &result));
  }
  const auto focused =
      std::find_if(result.panes.begin(), result.panes.end(),
                   [&](const auto& ids) {
                     return ids.first == source_workspace->focused;
                   });
  duplicate.focused =
      focused == result.panes.end() ? kInvalidId : focused->second;

  result.workspace = duplicate.id;
  workspaces_.push_back(std::move(duplicate));
  const auto source_root = std::find(roots_.begin(), roots_.end(), source);
  roots_.insert(source_root == roots_.end() ? roots_.end()
                                            : std::next(source_root),
                result.workspace);
  selected_ = result.workspace;
  selection_anchor_ = result.workspace;
  selected_workspace_ids_.clear();
  selected_workspace_ids_.insert(result.workspace);
  RevealWorkspace(result.workspace);
  return result;
}

void WindowModel::CloseWorkspace(WorkspaceId id) {
  Workspace* ws = GetWorkspace(id);
  if (!ws) {
    return;
  }
  const int pos = IndexInRailOrder(id);
  const WorkspaceGroupId old_group = ws->group;
  const bool was_active = selected_ == id;
  roots_.erase(std::remove(roots_.begin(), roots_.end(), id), roots_.end());
  const int idx = IndexOfWorkspace(id);
  if (idx >= 0) {
    workspaces_.erase(workspaces_.begin() + idx);
  }
  RemoveEmptyGroup(old_group);
  selected_workspace_ids_.erase(id);

  if (roots_.empty()) {
    selected_ = kInvalidId;
    selection_anchor_ = kInvalidId;
    selected_workspace_ids_.clear();
    return;
  }

  if (was_active) {
    // Chromium preserves any remaining multi-selection and promotes its first
    // item. Only when the removed active item was the complete selection do we
    // fall back to the adjacent row.
    selected_ = FirstSelectedWorkspace();
    if (selected_ == kInvalidId) {
      selected_ = roots_[std::min(std::max(0, pos),
                                  static_cast<int>(roots_.size()) - 1)];
      selected_workspace_ids_.insert(selected_);
    }
    selection_anchor_ = selected_;
    RevealWorkspace(selected_);
  } else if (selection_anchor_ == id ||
             selected_workspace_ids_.find(selection_anchor_) ==
                 selected_workspace_ids_.end()) {
    selection_anchor_ = selected_;
  }
}

void WindowModel::SelectWorkspace(WorkspaceId id) {
  if (!GetWorkspace(id)) {
    return;
  }
  RevealWorkspace(id);
  selected_ = id;
  selection_anchor_ = id;
  selected_workspace_ids_.clear();
  selected_workspace_ids_.insert(id);
}

void WindowModel::ExtendWorkspaceSelectionTo(WorkspaceId id) {
  if (!GetWorkspace(id)) {
    return;
  }
  const int anchor_index = IndexInRailOrder(selection_anchor_);
  const int target_index = IndexInRailOrder(id);
  if (anchor_index < 0 || target_index < 0) {
    SelectWorkspace(id);
    return;
  }

  selected_workspace_ids_.clear();
  const int first = std::min(anchor_index, target_index);
  const int last = std::max(anchor_index, target_index);
  for (int i = first; i <= last; ++i) {
    selected_workspace_ids_.insert(roots_[i]);
  }
  selected_ = id;
  RevealWorkspace(id);
}

void WindowModel::AddWorkspaceSelectionFromAnchorTo(WorkspaceId id) {
  if (!GetWorkspace(id)) {
    return;
  }
  const int anchor_index = IndexInRailOrder(selection_anchor_);
  const int target_index = IndexInRailOrder(id);
  if (anchor_index < 0 || target_index < 0) {
    SelectWorkspace(id);
    return;
  }

  const int first = std::min(anchor_index, target_index);
  const int last = std::max(anchor_index, target_index);
  for (int i = first; i <= last; ++i) {
    selected_workspace_ids_.insert(roots_[i]);
  }
  selected_ = id;
  RevealWorkspace(id);
}

bool WindowModel::ToggleWorkspaceSelection(WorkspaceId id) {
  if (!GetWorkspace(id)) {
    return false;
  }
  if (selected_workspace_ids_.find(id) == selected_workspace_ids_.end()) {
    selected_workspace_ids_.insert(id);
    selected_ = id;
    selection_anchor_ = id;
    RevealWorkspace(id);
    return true;
  }
  if (selected_workspace_ids_.size() == 1u) {
    return false;
  }

  selected_workspace_ids_.erase(id);
  const WorkspaceId first_selected = FirstSelectedWorkspace();
  if (selected_ == id) {
    selected_ = first_selected;
    RevealWorkspace(selected_);
  }
  if (selection_anchor_ == id) {
    selection_anchor_ = first_selected;
  }
  return true;
}

void WindowModel::ActivateWorkspaceInSelection(WorkspaceId id) {
  if (!IsWorkspaceSelected(id)) {
    return;
  }
  selected_ = id;
  selection_anchor_ = id;
  RevealWorkspace(id);
}

WorkspaceSelectionState WindowModel::GetWorkspaceSelectionState() const {
  return WorkspaceSelectionState{selected_workspaces(), selected_,
                                 selection_anchor_};
}

void WindowModel::RestoreWorkspaceSelectionState(
    const WorkspaceSelectionState& state) {
  if (roots_.empty()) {
    selected_ = kInvalidId;
    selection_anchor_ = kInvalidId;
    selected_workspace_ids_.clear();
    return;
  }

  std::set<WorkspaceId> restored;
  for (WorkspaceId id : state.selected) {
    if (IndexInRailOrder(id) >= 0) {
      restored.insert(id);
    }
  }
  if (restored.empty()) {
    const WorkspaceId fallback = GetWorkspace(state.active)
                                     ? state.active
                                     : roots_.front();
    restored.insert(fallback);
  }

  WorkspaceId fallback = roots_.front();
  for (WorkspaceId id : roots_) {
    if (restored.find(id) != restored.end()) {
      fallback = id;
      break;
    }
  }
  selected_workspace_ids_ = std::move(restored);
  selected_ = selected_workspace_ids_.find(state.active) !=
                      selected_workspace_ids_.end()
                  ? state.active
                  : fallback;
  selection_anchor_ = selected_workspace_ids_.find(state.anchor) !=
                              selected_workspace_ids_.end()
                          ? state.anchor
                          : selected_;
}

bool WindowModel::IsWorkspaceSelected(WorkspaceId id) const {
  return selected_workspace_ids_.find(id) != selected_workspace_ids_.end();
}

std::vector<WorkspaceId> WindowModel::selected_workspaces() const {
  std::vector<WorkspaceId> selected;
  selected.reserve(selected_workspace_ids_.size());
  for (WorkspaceId id : roots_) {
    if (selected_workspace_ids_.find(id) != selected_workspace_ids_.end()) {
      selected.push_back(id);
    }
  }
  return selected;
}

std::vector<WorkspaceId> WindowModel::WorkspacesForCommand(
    WorkspaceId context) const {
  if (!GetWorkspace(context)) {
    return {};
  }
  return IsWorkspaceSelected(context) ? selected_workspaces()
                                      : std::vector<WorkspaceId>{context};
}

void WindowModel::MoveWorkspace(WorkspaceId id,
                                WorkspaceId new_parent,
                                int index) {
  Workspace* moving = GetWorkspace(id);
  if (!moving) {
    return;
  }
  if (new_parent == kInvalidId) {
    roots_.erase(std::remove(roots_.begin(), roots_.end(), id), roots_.end());
    RemoveWorkspaceFromGroup(id);
    const int at = ClampFlatInsertionIndexToGroupBoundary(
        index < 0 ? static_cast<int>(roots_.size()) : index);
    roots_.insert(roots_.begin() + at, id);
    return;
  }

  Workspace* target = GetWorkspace(new_parent);
  if (!target || new_parent == id) {
    return;
  }
  const WorkspaceGroupId destination_group =
      EnsureGroupForWorkspace(new_parent);
  roots_.erase(std::remove(roots_.begin(), roots_.end(), id), roots_.end());
  RemoveWorkspaceFromGroup(id);

  moving = GetWorkspace(id);
  moving->group = destination_group;
  const int member_count =
      static_cast<int>(WorkspacesInGroup(destination_group).size());
  const int member_index =
      index < 0 ? member_count : std::clamp(index, 0, member_count);
  const int start = GroupStartIndex(destination_group);
  const int at =
      start < 0 ? static_cast<int>(roots_.size()) : start + member_index;
  roots_.insert(roots_.begin() + at, id);

  // Adding a member to a collapsed group reveals it, matching Chromium's
  // VerticalTabGroupView::AttachChildView behavior.
  if (WorkspaceGroup* group = GetWorkspaceGroup(destination_group)) {
    group->collapsed = false;
  }
}

void WindowModel::MoveWorkspaces(const std::vector<WorkspaceId>& ids,
                                 WorkspaceId new_parent,
                                 int index) {
  std::set<WorkspaceId> requested;
  for (WorkspaceId id : ids) {
    if (GetWorkspace(id)) {
      requested.insert(id);
    }
  }
  if (requested.empty()) {
    return;
  }

  // Normalize to current rail order, matching TabStripModel's selected-index
  // move behavior regardless of the caller's input ordering.
  std::vector<WorkspaceId> moving;
  moving.reserve(requested.size());
  for (WorkspaceId id : roots_) {
    if (requested.find(id) != requested.end()) {
      moving.push_back(id);
    }
  }
  if (moving.empty()) {
    return;
  }

  WorkspaceGroupId destination_group = kInvalidId;
  if (new_parent != kInvalidId) {
    const Workspace* target = GetWorkspace(new_parent);
    // Rail drag may join only a pre-existing group. A selected destination
    // cannot serve as the fixed group anchor while that selection is moving.
    if (!target || target->group == kInvalidId ||
        requested.find(new_parent) != requested.end()) {
      return;
    }
    destination_group = target->group;
  }

  // A fully selected group dragged at the flat level retains its identity,
  // title, and color. Partial selections detach only the selected members.
  std::set<WorkspaceGroupId> retained_groups;
  if (destination_group == kInvalidId) {
    for (WorkspaceId id : moving) {
      const Workspace* workspace = GetWorkspace(id);
      if (!workspace || workspace->group == kInvalidId ||
          retained_groups.find(workspace->group) != retained_groups.end()) {
        continue;
      }
      const std::vector<WorkspaceId> members =
          WorkspacesInGroup(workspace->group);
      if (!members.empty() &&
          std::all_of(members.begin(), members.end(), [&](WorkspaceId member) {
            return requested.find(member) != requested.end();
          })) {
        retained_groups.insert(workspace->group);
      }
    }
  }

  std::set<WorkspaceGroupId> previous_groups;
  for (WorkspaceId id : moving) {
    Workspace* workspace = GetWorkspace(id);
    if (workspace->group != kInvalidId &&
        retained_groups.find(workspace->group) == retained_groups.end()) {
      previous_groups.insert(workspace->group);
      workspace->group = kInvalidId;
    }
  }
  roots_.erase(std::remove_if(roots_.begin(), roots_.end(), [&](WorkspaceId id) {
                 return requested.find(id) != requested.end();
               }),
               roots_.end());
  for (WorkspaceGroupId group : previous_groups) {
    RemoveEmptyGroup(group);
  }

  int at = 0;
  if (destination_group == kInvalidId) {
    at = ClampFlatInsertionIndexToGroupBoundary(
        index < 0 ? static_cast<int>(roots_.size()) : index);
  } else {
    const int member_count =
        static_cast<int>(WorkspacesInGroup(destination_group).size());
    const int member_index =
        index < 0 ? member_count : std::clamp(index, 0, member_count);
    const int start = GroupStartIndex(destination_group);
    if (start < 0) {
      return;
    }
    at = start + member_index;
    for (WorkspaceId id : moving) {
      GetWorkspace(id)->group = destination_group;
    }
    if (WorkspaceGroup* group = GetWorkspaceGroup(destination_group)) {
      group->collapsed = false;
    }
  }
  roots_.insert(roots_.begin() + at, moving.begin(), moving.end());
}

WorkspaceGroupId WindowModel::CreateWorkspaceGroup(WorkspaceId id) {
  Workspace* workspace = GetWorkspace(id);
  if (!workspace) {
    return kInvalidId;
  }
  if (workspace->group != kInvalidId) {
    // Chromium's "Add to new group" pulls an already-grouped item into a
    // fresh group; it is not a no-op merely because a group already exists.
    // Use the same extraction path as explicit ungrouping so removing a middle
    // member cannot leave its previous group split around the new group.
    UngroupWorkspace(id);
  }
  return EnsureGroupForWorkspace(id);
}

void WindowModel::UngroupWorkspace(WorkspaceId id) {
  UngroupWorkspaces({id});
}

void WindowModel::UngroupWorkspaces(const std::vector<WorkspaceId>& ids) {
  std::set<WorkspaceId> requested;
  std::set<WorkspaceGroupId> affected_groups;
  for (WorkspaceId id : ids) {
    const Workspace* workspace = GetWorkspace(id);
    if (!workspace || workspace->group == kInvalidId) {
      continue;
    }
    requested.insert(id);
    affected_groups.insert(workspace->group);
  }
  if (requested.empty()) {
    return;
  }

  struct GroupReplacement {
    WorkspaceGroupId group = kInvalidId;
    std::vector<WorkspaceId> members;
  };
  std::vector<GroupReplacement> replacements;
  replacements.reserve(affected_groups.size());
  for (WorkspaceGroupId group : affected_groups) {
    const std::vector<WorkspaceId> members = WorkspacesInGroup(group);
    auto first_remaining =
        std::find_if(members.begin(), members.end(), [&](WorkspaceId member) {
          return requested.find(member) == requested.end();
        });

    GroupReplacement replacement;
    replacement.group = group;
    if (first_remaining == members.end()) {
      // Removing an entire group changes only membership, not rail order.
      replacement.members = members;
    } else {
      // Boundary extractions can stay where they are. Collect every extraction
      // that would otherwise split the surviving group at its trailing edge,
      // preserving the extracted workspaces' current rail order as one batch.
      replacement.members.insert(replacement.members.end(), members.begin(),
                                 first_remaining);
      for (auto member = first_remaining; member != members.end(); ++member) {
        if (requested.find(*member) == requested.end()) {
          replacement.members.push_back(*member);
        }
      }
      for (auto member = first_remaining; member != members.end(); ++member) {
        if (requested.find(*member) != requested.end()) {
          replacement.members.push_back(*member);
        }
      }
    }
    replacements.push_back(std::move(replacement));
  }

  // Rebuild all affected group spans from one snapshot. Applying singleton
  // extractions in a loop would feed each mutation into the next operation and
  // can reverse a selected run (A,B,C; remove B,C -> A,C,B).
  std::set<WorkspaceGroupId> emitted_groups;
  std::vector<WorkspaceId> reordered;
  reordered.reserve(roots_.size());
  for (WorkspaceId id : roots_) {
    const Workspace* workspace = GetWorkspace(id);
    const auto replacement =
        workspace
            ? std::find_if(replacements.begin(), replacements.end(),
                           [&](const GroupReplacement& candidate) {
                             return candidate.group == workspace->group;
                           })
            : replacements.end();
    if (replacement == replacements.end()) {
      reordered.push_back(id);
      continue;
    }
    if (emitted_groups.insert(replacement->group).second) {
      reordered.insert(reordered.end(), replacement->members.begin(),
                       replacement->members.end());
    }
  }
  roots_ = std::move(reordered);

  for (WorkspaceId id : requested) {
    GetWorkspace(id)->group = kInvalidId;
  }
  for (WorkspaceGroupId group : affected_groups) {
    RemoveEmptyGroup(group);
  }
}

void WindowModel::SetWorkspaceExpanded(WorkspaceId id, bool expanded) {
  const Workspace* ws = GetWorkspace(id);
  if (ws) {
    SetWorkspaceGroupCollapsed(ws->group, !expanded);
  }
}

void WindowModel::SetWorkspaceTitle(WorkspaceId id, const std::string& title) {
  if (Workspace* ws = GetWorkspace(id)) {
    ws->title = title;
  }
}

void WindowModel::SetWorkspaceColor(WorkspaceId id, GroupColor color) {
  if (Workspace* ws = GetWorkspace(id)) {
    ws->preferred_group_color = color;
    if (WorkspaceGroup* group = GetWorkspaceGroup(ws->group)) {
      group->color = color;
    }
  }
}

void WindowModel::SetWorkspaceGroupTitle(WorkspaceGroupId id,
                                         const std::string& title) {
  if (WorkspaceGroup* group = GetWorkspaceGroup(id)) {
    group->title = title;
  }
}

void WindowModel::SetWorkspaceGroupCollapsed(WorkspaceGroupId id,
                                             bool collapsed) {
  WorkspaceGroup* group = GetWorkspaceGroup(id);
  if (!group || group->collapsed == collapsed) {
    return;
  }
  group->collapsed = collapsed;
  if (!collapsed) {
    return;
  }

  const Workspace* selected = GetWorkspace(selected_);
  if (!selected || selected->group != id) {
    // Chromium re-activates the already-active tab when collapsing some other
    // group. Besides keeping that tab active, this deliberately clears any
    // additional selected tabs that are about to become hidden.
    if (selected) {
      SelectWorkspace(selected_);
    }
    return;
  }

  // Chromium's TabStripModel::GetNextExpandedActiveTab() searches forward
  // from the end of the collapsing group first, then backwards from its start.
  const int start = GroupStartIndex(id);
  const int end = GroupEndIndex(id);
  auto is_visible = [&](WorkspaceId candidate) {
    const Workspace* ws = GetWorkspace(candidate);
    const WorkspaceGroup* candidate_group =
        ws ? GetWorkspaceGroup(ws->group) : nullptr;
    return ws && ws->group != id &&
           (!candidate_group || !candidate_group->collapsed);
  };
  for (int i = std::max(start, end); i < static_cast<int>(roots_.size()); ++i) {
    if (is_visible(roots_[i])) {
      SelectWorkspace(roots_[i]);
      return;
    }
  }
  for (int i = start - 1; i >= 0; --i) {
    if (is_visible(roots_[i])) {
      SelectWorkspace(roots_[i]);
      return;
    }
  }

  // The browser-facing controller creates an ungrouped workspace before
  // collapsing a group that owns the whole window, matching Chromium's new-tab
  // fallback. Keep the pure model safe if a non-UI caller omits that step.
  group->collapsed = false;
}

void WindowModel::SetWorkspaceLayoutMode(WorkspaceId id,
                                         WorkspaceLayoutMode mode) {
  if (Workspace* ws = GetWorkspace(id)) {
    ws->layout_mode = mode;
  }
}

WorkspaceLayoutMode WindowModel::ToggleWorkspaceLayoutMode(WorkspaceId id) {
  Workspace* ws = GetWorkspace(id);
  if (!ws) {
    return WorkspaceLayoutMode::kStrip;
  }
  ws->layout_mode = ws->layout_mode == WorkspaceLayoutMode::kStrip
                        ? WorkspaceLayoutMode::kTiled
                        : WorkspaceLayoutMode::kStrip;
  return ws->layout_mode;
}

const Workspace* WindowModel::GetWorkspace(WorkspaceId id) const {
  for (const Workspace& ws : workspaces_) {
    if (ws.id == id) {
      return &ws;
    }
  }
  return nullptr;
}

Workspace* WindowModel::GetWorkspace(WorkspaceId id) {
  for (Workspace& ws : workspaces_) {
    if (ws.id == id) {
      return &ws;
    }
  }
  return nullptr;
}

const Workspace* WindowModel::FindWorkspaceByRegistryKey(
    const std::string& key) const {
  if (key.empty()) {
    return nullptr;
  }
  for (const Workspace& workspace : workspaces_) {
    if (workspace.registry_key == key) {
      return &workspace;
    }
  }
  return nullptr;
}

Workspace* WindowModel::FindWorkspaceByRegistryKey(const std::string& key) {
  return const_cast<Workspace*>(
      std::as_const(*this).FindWorkspaceByRegistryKey(key));
}

void WindowModel::ReorderWorkspaceSiblingsByRegistryKeys(
    const std::vector<std::string>& keys) {
  const auto rank = [this, &keys](WorkspaceId id) {
    const Workspace* workspace = GetWorkspace(id);
    if (!workspace) {
      return keys.size();
    }
    const auto found =
        std::find(keys.begin(), keys.end(), workspace->registry_key);
    return found == keys.end() ? keys.size()
                               : static_cast<size_t>(found - keys.begin());
  };

  std::vector<std::vector<WorkspaceId>> units;
  std::set<WorkspaceGroupId> seen_groups;
  for (WorkspaceId id : roots_) {
    const Workspace* workspace = GetWorkspace(id);
    if (workspace && workspace->group != kInvalidId) {
      if (seen_groups.insert(workspace->group).second) {
        units.push_back(WorkspacesInGroup(workspace->group));
      }
    } else {
      units.push_back({id});
    }
  }
  for (std::vector<WorkspaceId>& unit : units) {
    std::stable_sort(unit.begin(), unit.end(),
                     [&rank](WorkspaceId left, WorkspaceId right) {
                       return rank(left) < rank(right);
                     });
  }
  const auto unit_rank = [&rank](const std::vector<WorkspaceId>& unit) {
    size_t result = std::numeric_limits<size_t>::max();
    for (WorkspaceId id : unit) {
      result = std::min(result, rank(id));
    }
    return result;
  };
  std::stable_sort(
      units.begin(), units.end(),
      [&unit_rank](const std::vector<WorkspaceId>& left,
                   const std::vector<WorkspaceId>& right) {
        return unit_rank(left) < unit_rank(right);
      });
  roots_.clear();
  for (const std::vector<WorkspaceId>& unit : units) {
    roots_.insert(roots_.end(), unit.begin(), unit.end());
  }
}

const WorkspaceGroup* WindowModel::GetWorkspaceGroup(
    WorkspaceGroupId id) const {
  for (const WorkspaceGroup& group : workspace_groups_) {
    if (group.id == id) {
      return &group;
    }
  }
  return nullptr;
}

WorkspaceGroup* WindowModel::GetWorkspaceGroup(WorkspaceGroupId id) {
  for (WorkspaceGroup& group : workspace_groups_) {
    if (group.id == id) {
      return &group;
    }
  }
  return nullptr;
}

std::vector<WorkspaceId> WindowModel::WorkspacesInGroup(
    WorkspaceGroupId id) const {
  std::vector<WorkspaceId> members;
  if (id == kInvalidId) {
    return members;
  }
  for (WorkspaceId workspace : roots_) {
    const Workspace* ws = GetWorkspace(workspace);
    if (ws && ws->group == id) {
      members.push_back(workspace);
    }
  }
  return members;
}

// ---- Group helpers + rail ---------------------------------------------------

int WindowModel::IndexInRailOrder(WorkspaceId id) const {
  auto it = std::find(roots_.begin(), roots_.end(), id);
  return it == roots_.end() ? -1 : static_cast<int>(it - roots_.begin());
}

WorkspaceGroupId WindowModel::EnsureGroupForWorkspace(WorkspaceId id) {
  Workspace* ws = GetWorkspace(id);
  if (!ws) {
    return kInvalidId;
  }
  if (GetWorkspaceGroup(ws->group)) {
    return ws->group;
  }
  WorkspaceGroup group;
  group.id = NextId();
  group.title = ws->title;
  group.color = ws->preferred_group_color;
  const WorkspaceGroupId group_id = group.id;
  workspace_groups_.push_back(std::move(group));
  GetWorkspace(id)->group = group_id;
  return group_id;
}

void WindowModel::RemoveWorkspaceFromGroup(WorkspaceId id) {
  Workspace* ws = GetWorkspace(id);
  if (!ws || ws->group == kInvalidId) {
    return;
  }
  const WorkspaceGroupId group = ws->group;
  ws->group = kInvalidId;
  RemoveEmptyGroup(group);
}

void WindowModel::RemoveEmptyGroup(WorkspaceGroupId id) {
  if (id == kInvalidId || !WorkspacesInGroup(id).empty()) {
    return;
  }
  workspace_groups_.erase(
      std::remove_if(
          workspace_groups_.begin(), workspace_groups_.end(),
          [id](const WorkspaceGroup& group) { return group.id == id; }),
      workspace_groups_.end());
}

int WindowModel::GroupStartIndex(WorkspaceGroupId id) const {
  for (size_t i = 0; i < roots_.size(); ++i) {
    const Workspace* ws = GetWorkspace(roots_[i]);
    if (ws && ws->group == id) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

int WindowModel::GroupEndIndex(WorkspaceGroupId id) const {
  int end = -1;
  for (size_t i = 0; i < roots_.size(); ++i) {
    const Workspace* ws = GetWorkspace(roots_[i]);
    if (ws && ws->group == id) {
      end = static_cast<int>(i) + 1;
    }
  }
  return end;
}

int WindowModel::ClampFlatInsertionIndexToGroupBoundary(int index) const {
  const int at = std::clamp(index, 0, static_cast<int>(roots_.size()));
  if (at == 0 || at == static_cast<int>(roots_.size())) {
    return at;
  }

  const Workspace* before = GetWorkspace(roots_[at - 1]);
  const Workspace* after = GetWorkspace(roots_[at]);
  if (!before || !after || before->group == kInvalidId ||
      before->group != after->group) {
    return at;
  }

  const int start = GroupStartIndex(before->group);
  const int end = GroupEndIndex(before->group);
  if (start < 0 || end <= start) {
    return at;
  }
  return at - start < end - at ? start : end;
}

WorkspaceId WindowModel::FirstSelectedWorkspace() const {
  for (WorkspaceId id : roots_) {
    if (selected_workspace_ids_.find(id) != selected_workspace_ids_.end()) {
      return id;
    }
  }
  return kInvalidId;
}

void WindowModel::RevealWorkspace(WorkspaceId id) {
  const Workspace* workspace = GetWorkspace(id);
  if (!workspace) {
    return;
  }
  // Chromium expands a collapsed group when one of its members becomes active.
  if (WorkspaceGroup* group = GetWorkspaceGroup(workspace->group)) {
    group->collapsed = false;
  }
}

std::vector<RailItem> WindowModel::BuildRail() const {
  std::vector<RailItem> out;
  for (size_t i = 0; i < roots_.size(); ++i) {
    const Workspace* ws = GetWorkspace(roots_[i]);
    if (!ws) {
      continue;
    }
    const WorkspaceGroup* group = GetWorkspaceGroup(ws->group);
    if (group && group->collapsed) {
      continue;
    }
    RailItem row;
    row.workspace = ws->id;
    row.group = group ? group->id : kInvalidId;
    row.depth = group ? 1 : 0;
    if (group) {
      row.first_in_group = i == 0 || !GetWorkspace(roots_[i - 1]) ||
                           GetWorkspace(roots_[i - 1])->group != group->id;
      row.last_in_group = i + 1 == roots_.size() ||
                          !GetWorkspace(roots_[i + 1]) ||
                          GetWorkspace(roots_[i + 1])->group != group->id;
    }
    out.push_back(row);
  }
  return out;
}

// ---- Layout -----------------------------------------------------------------

PaneId WindowModel::AddColumn(WorkspaceId workspace,
                              SurfaceKind kind,
                              int index) {
  Workspace* ws = GetWorkspace(workspace);
  if (!ws) {
    return kInvalidId;
  }
  std::unique_ptr<Pane> pane = NewPaneWithTab(kind, std::string());
  const PaneId id = pane->id;
  const int at = (index < 0 || index > static_cast<int>(ws->columns.size()))
                     ? static_cast<int>(ws->columns.size())
                     : index;
  InsertColumn(*ws, at, LayoutNode::MakePane(std::move(pane)), 0.0);
  ws->focused = id;
  return id;
}

PaneId WindowModel::SplitPane(WorkspaceId workspace,
                              PaneId pane,
                              SplitOrientation orientation,
                              double ratio,
                              SurfaceKind new_tab_kind) {
  Workspace* ws = GetWorkspace(workspace);
  if (!ws) {
    return kInvalidId;
  }
  LayoutNode* node = NodeOfPaneIn(*ws, pane);
  if (!node || !node->is_pane()) {
    return kInvalidId;
  }
  std::unique_ptr<Pane> existing = std::move(node->pane);
  std::unique_ptr<Pane> fresh = NewPaneWithTab(new_tab_kind, std::string());
  PaneId new_id = fresh->id;

  auto split = std::make_unique<Split>();
  split->id = NextId();
  split->orientation = orientation;
  split->ratio = ClampSplitRatio(ratio);
  split->first = LayoutNode::MakePane(std::move(existing));
  split->second = LayoutNode::MakePane(std::move(fresh));
  *node = LayoutNode::MakeSplit(std::move(split));

  ws->focused = new_id;
  return new_id;
}

void WindowModel::ClosePane(WorkspaceId workspace, PaneId pane) {
  Workspace* ws = GetWorkspace(workspace);
  if (!ws) {
    return;
  }
  const int col = ColumnOf(*ws, pane);
  if (col < 0) {
    return;
  }
  LayoutNode& root = ws->columns[col];
  if (root.is_pane()) {
    // The column's only pane: remove the whole column -- unless it holds the
    // workspace's only pane (the workspace must always keep one).
    if (ws->columns.size() == 1) {
      return;
    }
    EraseColumn(*ws, col);
    if (ws->focused == pane) {
      const int fallback =
          std::min(col, static_cast<int>(ws->columns.size()) - 1);
      const Pane* p = FirstPane(ws->columns[fallback]);
      ws->focused = p ? p->id : kInvalidId;
    }
    return;
  }
  LayoutNode* absorbed = CollapsePane(root, pane);
  if (!absorbed) {
    return;
  }
  if (ws->focused == pane) {
    const Pane* p = FirstPane(*absorbed);
    ws->focused = p ? p->id : kInvalidId;
  }
}

void WindowModel::SetSplitRatio(WorkspaceId workspace,
                                SplitId split,
                                double ratio) {
  Workspace* ws = GetWorkspace(workspace);
  if (!ws) {
    return;
  }
  for (LayoutNode& column : ws->columns) {
    if (Split* s = SplitById(column, split)) {
      s->ratio = ClampSplitRatio(ratio);
      return;
    }
  }
}

void WindowModel::FocusPane(WorkspaceId workspace, PaneId pane) {
  Workspace* ws = GetWorkspace(workspace);
  if (ws && FindPane(workspace, pane)) {
    ws->focused = pane;
  }
}

double WindowModel::CycleColumnWidth(WorkspaceId workspace,
                                     PaneId pane,
                                     double default_fraction) {
  const std::vector<double> kDefaultModes = {1.0, 2.0 / 3.0, 0.5, 1.0 / 3.0};
  return CycleColumnWidth(workspace, pane, kDefaultModes, default_fraction);
}

double WindowModel::CycleColumnWidth(WorkspaceId workspace,
                                     PaneId pane,
                                     const std::vector<double>& modes,
                                     double default_fraction) {
  Workspace* ws = GetWorkspace(workspace);
  if (!ws || modes.empty()) {
    return 0.0;
  }
  RepairColumnWidthCount(*ws);
  const int col = ColumnOf(*ws, pane);
  if (col < 0) {
    return 0.0;
  }
  constexpr double kEpsilon = 1e-6;
  const double stored = ws->column_widths[col];
  const double effective = stored > 0.0 ? stored : default_fraction;
  size_t next_index = 0;
  for (size_t i = 0; i < modes.size(); ++i) {
    if (std::abs(modes[i] - effective) <= kEpsilon) {
      next_index = (i + 1) % modes.size();
      break;
    }
  }
  ws->column_widths[col] = std::clamp(modes[next_index], 0.1, 2.0);
  return ws->column_widths[col];
}

double WindowModel::SetColumnWidth(WorkspaceId workspace,
                                   PaneId pane,
                                   double fraction) {
  Workspace* ws = GetWorkspace(workspace);
  if (!ws) {
    return 0.0;
  }
  RepairColumnWidthCount(*ws);
  const int col = ColumnOf(*ws, pane);
  if (col < 0) {
    return 0.0;
  }
  ws->column_widths[col] = std::clamp(fraction, 0.1, 2.0);
  return ws->column_widths[col];
}

// ---- Tabs -------------------------------------------------------------------

SurfaceTabId WindowModel::AddTab(WorkspaceId workspace,
                                 PaneId pane,
                                 SurfaceKind kind,
                                 const std::string& title) {
  Pane* p = FindPane(workspace, pane);
  if (!p) {
    return kInvalidId;
  }
  SurfaceTab tab;
  tab.id = NextId();
  tab.kind = kind;
  tab.title = title;
  p->selected = tab.id;
  p->tabs.push_back(std::move(tab));
  return p->selected;
}

void WindowModel::CloseTab(WorkspaceId workspace,
                           PaneId pane,
                           SurfaceTabId tab) {
  Pane* p = FindPane(workspace, pane);
  if (!p) {
    return;
  }
  int idx = p->IndexOfTab(tab);
  if (idx < 0) {
    return;
  }
  if (p->tabs.size() == 1 && PanesOf(workspace).size() <= 1) {
    return;
  }
  p->tabs.erase(p->tabs.begin() + idx);
  if (p->tabs.empty()) {
    ClosePane(workspace, pane);
    return;
  }
  if (p->selected == tab) {
    int next = std::min(idx, static_cast<int>(p->tabs.size()) - 1);
    p->selected = p->tabs[next].id;
  }
}

void WindowModel::SelectTab(WorkspaceId workspace,
                            PaneId pane,
                            SurfaceTabId tab) {
  Pane* p = FindPane(workspace, pane);
  if (p && p->FindTab(tab)) {
    p->selected = tab;
  }
}

bool WindowModel::SetTabLoading(WorkspaceId workspace,
                                PaneId pane,
                                SurfaceTabId tab,
                                bool loading) {
  Pane* p = FindPane(workspace, pane);
  SurfaceTab* t = p ? p->FindTab(tab) : nullptr;
  if (!t || t->loading == loading) {
    return false;
  }
  t->loading = loading;
  return true;
}

void WindowModel::MoveTab(WorkspaceId from_workspace,
                          PaneId from_pane,
                          SurfaceTabId tab,
                          WorkspaceId to_workspace,
                          PaneId to_pane,
                          int index) {
  WorkspaceId resolved_from_workspace = from_workspace;
  if (resolved_from_workspace == kInvalidId) {
    for (const Workspace& ws : workspaces_) {
      if (ColumnOf(ws, from_pane) >= 0) {
        resolved_from_workspace = ws.id;
        break;
      }
    }
    if (resolved_from_workspace == kInvalidId) {
      return;
    }
  }
  WorkspaceId resolved_to_workspace = to_workspace;
  if (resolved_to_workspace == kInvalidId) {
    for (const Workspace& ws : workspaces_) {
      if (ColumnOf(ws, to_pane) >= 0) {
        resolved_to_workspace = ws.id;
        break;
      }
    }
    if (resolved_to_workspace == kInvalidId) {
      return;
    }
  }

  Pane* from = FindPane(resolved_from_workspace, from_pane);
  if (!from) {
    return;
  }
  int idx = from->IndexOfTab(tab);
  if (idx < 0) {
    return;
  }
  Pane* to = FindPane(resolved_to_workspace, to_pane);
  if (!to) {
    return;
  }
  if (from == to) {
    const SurfaceTabId selected = from->selected;
    SurfaceTab moved = std::move(from->tabs[idx]);
    int insert_at = index < 0 || index > static_cast<int>(from->tabs.size())
                        ? static_cast<int>(from->tabs.size())
                        : index;
    from->tabs.erase(from->tabs.begin() + idx);
    if (idx < insert_at) {
      --insert_at;
    }
    from->tabs.insert(from->tabs.begin() + insert_at, std::move(moved));
    from->selected = selected;
    return;
  }
  if (from->tabs.size() == 1 && PanesOf(resolved_from_workspace).size() == 1) {
    return;
  }

  SurfaceTab moved = std::move(from->tabs[idx]);
  from->tabs.erase(from->tabs.begin() + idx);
  bool from_selected_removed = (from->selected == tab);

  SurfaceTabId moved_id = moved.id;
  int insert_at = index < 0 || index > static_cast<int>(to->tabs.size())
                      ? static_cast<int>(to->tabs.size())
                      : index;
  to->tabs.insert(to->tabs.begin() + insert_at, std::move(moved));
  to->selected = moved_id;

  if (from->tabs.empty()) {
    ClosePane(resolved_from_workspace, from_pane);
  } else if (from_selected_removed) {
    int next = std::min(idx, static_cast<int>(from->tabs.size()) - 1);
    from->selected = from->tabs[next].id;
  }
}

SurfaceTabId WindowModel::ReorderTab(WorkspaceId workspace,
                                     PaneId pane,
                                     SurfaceTabId tab,
                                     int to_index) {
  Pane* p = FindPane(workspace, pane);
  if (!p) {
    return kInvalidId;
  }
  const int from = p->IndexOfTab(tab);
  if (from < 0) {
    return kInvalidId;
  }
  if (p->tabs.size() <= 1) {
    return tab;
  }
  const int clamped =
      std::clamp(to_index, 0, static_cast<int>(p->tabs.size()) - 1);
  if (from == clamped) {
    return tab;
  }
  SurfaceTab moved = std::move(p->tabs[from]);
  p->tabs.erase(p->tabs.begin() + from);
  p->tabs.insert(p->tabs.begin() + clamped, std::move(moved));
  return tab;
}

PaneId WindowModel::SplitPaneWithTab(WorkspaceId workspace,
                                     PaneId target,
                                     SplitOrientation orientation,
                                     bool insert_first,
                                     PaneId from_pane,
                                     SurfaceTabId tab) {
  Workspace* ws = GetWorkspace(workspace);
  if (!ws) {
    return kInvalidId;
  }
  LayoutNode* target_node = NodeOfPaneIn(*ws, target);
  Pane* from = FindPane(workspace, from_pane);
  if (!target_node || !target_node->is_pane() || !from) {
    return kInvalidId;
  }
  const int tab_index = from->IndexOfTab(tab);
  if (tab_index < 0) {
    return kInvalidId;
  }
  if (from_pane == target && from->tabs.size() == 1) {
    return kInvalidId;
  }

  SurfaceTab moved = std::move(from->tabs[tab_index]);
  from->tabs.erase(from->tabs.begin() + tab_index);
  RepairSelectionAfterRemove(from, tab, tab_index);

  const PaneId fresh_id = NextId();
  std::unique_ptr<Pane> fresh = NewPaneHoldingTab(fresh_id, std::move(moved));
  std::unique_ptr<Pane> existing = std::move(target_node->pane);

  auto split = std::make_unique<Split>();
  split->id = NextId();
  split->orientation = orientation;
  split->ratio = 0.5;
  if (insert_first) {
    split->first = LayoutNode::MakePane(std::move(fresh));
    split->second = LayoutNode::MakePane(std::move(existing));
  } else {
    split->first = LayoutNode::MakePane(std::move(existing));
    split->second = LayoutNode::MakePane(std::move(fresh));
  }
  *target_node = LayoutNode::MakeSplit(std::move(split));
  ws->focused = fresh_id;

  if (from_pane != target && from->tabs.empty()) {
    ClosePane(workspace, from_pane);
    if (FindPane(workspace, fresh_id)) {
      ws->focused = fresh_id;
    }
  }
  return fresh_id;
}

PaneId WindowModel::MoveTabToNewColumn(WorkspaceId workspace,
                                       PaneId from_pane,
                                       SurfaceTabId tab,
                                       int column_index) {
  Workspace* ws = GetWorkspace(workspace);
  Pane* from = FindPane(workspace, from_pane);
  if (!ws || !from) {
    return kInvalidId;
  }
  const int tab_index = from->IndexOfTab(tab);
  if (tab_index < 0) {
    return kInvalidId;
  }
  const int pre_count = static_cast<int>(ws->columns.size());
  const int insert_at = ClampColumnIndex(column_index, pre_count);

  SurfaceTab moved = std::move(from->tabs[tab_index]);
  from->tabs.erase(from->tabs.begin() + tab_index);
  const bool source_empty = from->tabs.empty();
  RepairSelectionAfterRemove(from, tab, tab_index);

  const PaneId fresh_id = NextId();
  std::unique_ptr<Pane> fresh = NewPaneHoldingTab(fresh_id, std::move(moved));
  InsertColumn(*ws, insert_at, LayoutNode::MakePane(std::move(fresh)), 0.0);
  ws->focused = fresh_id;

  if (source_empty) {
    ClosePane(workspace, from_pane);
    if (FindPane(workspace, fresh_id)) {
      ws->focused = fresh_id;
    }
  }
  return fresh_id;
}

bool WindowModel::MergePaneInto(WorkspaceId workspace,
                                PaneId from_pane,
                                PaneId to_pane,
                                int index) {
  if (from_pane == to_pane) {
    return false;
  }
  Pane* from = FindPane(workspace, from_pane);
  Pane* to = FindPane(workspace, to_pane);
  if (!from || !to || from->tabs.empty()) {
    return false;
  }
  const SurfaceTabId selected = from->selected;
  const int insert_at = index < 0 || index > static_cast<int>(to->tabs.size())
                            ? static_cast<int>(to->tabs.size())
                            : index;
  to->tabs.insert(to->tabs.begin() + insert_at,
                  std::make_move_iterator(from->tabs.begin()),
                  std::make_move_iterator(from->tabs.end()));
  from->tabs.clear();
  to->selected = selected;
  ClosePane(workspace, from_pane);
  if (Workspace* ws = GetWorkspace(workspace)) {
    ws->focused = to_pane;
  }
  return true;
}

PaneId WindowModel::MovePaneToSplit(WorkspaceId workspace,
                                    PaneId pane,
                                    PaneId target,
                                    SplitOrientation orientation,
                                    bool insert_first) {
  if (pane == target) {
    return kInvalidId;
  }
  Workspace* ws = GetWorkspace(workspace);
  if (!ws || !FindPane(workspace, pane) || !FindPane(workspace, target)) {
    return kInvalidId;
  }

  const int source_col = ColumnOf(*ws, pane);
  if (source_col < 0) {
    return kInvalidId;
  }
  std::unique_ptr<Pane> moved;
  double moved_width = 0.0;
  LayoutNode& source_root = ws->columns[source_col];
  if (source_root.is_pane() && source_root.pane->id == pane) {
    moved_width = WidthForColumn(*ws, source_col);
    moved = std::move(source_root.pane);
    EraseColumn(*ws, source_col);
  } else {
    moved = ExtractPane(source_root, pane);
  }
  if (!moved) {
    return kInvalidId;
  }

  LayoutNode* target_node = NodeOfPaneIn(*ws, target);
  if (!target_node || !target_node->is_pane()) {
    InsertColumn(*ws, std::min<int>(source_col, ws->columns.size()),
                 LayoutNode::MakePane(std::move(moved)), moved_width);
    return kInvalidId;
  }
  std::unique_ptr<Pane> existing = std::move(target_node->pane);
  auto split = std::make_unique<Split>();
  split->id = NextId();
  split->orientation = orientation;
  split->ratio = 0.5;
  if (insert_first) {
    split->first = LayoutNode::MakePane(std::move(moved));
    split->second = LayoutNode::MakePane(std::move(existing));
  } else {
    split->first = LayoutNode::MakePane(std::move(existing));
    split->second = LayoutNode::MakePane(std::move(moved));
  }
  *target_node = LayoutNode::MakeSplit(std::move(split));
  ws->focused = pane;
  return pane;
}

PaneId WindowModel::MovePaneToNewColumn(WorkspaceId workspace,
                                        PaneId pane,
                                        int column_index) {
  Workspace* ws = GetWorkspace(workspace);
  if (!ws || !FindPane(workspace, pane)) {
    return kInvalidId;
  }
  const int pre_count = static_cast<int>(ws->columns.size());
  const int requested = ClampColumnIndex(column_index, pre_count);
  const int source_col = ColumnOf(*ws, pane);
  if (source_col < 0) {
    return kInvalidId;
  }

  std::unique_ptr<Pane> moved;
  double moved_width = 0.0;
  bool removed_column = false;
  LayoutNode& source_root = ws->columns[source_col];
  if (source_root.is_pane() && source_root.pane->id == pane) {
    moved_width = WidthForColumn(*ws, source_col);
    moved = std::move(source_root.pane);
    EraseColumn(*ws, source_col);
    removed_column = true;
  } else {
    moved = ExtractPane(source_root, pane);
  }
  if (!moved) {
    return kInvalidId;
  }

  int insert_at = requested;
  if (removed_column && requested > source_col) {
    --insert_at;
  }
  insert_at = std::clamp(insert_at, 0, static_cast<int>(ws->columns.size()));
  InsertColumn(*ws, insert_at, LayoutNode::MakePane(std::move(moved)),
               moved_width);
  ws->focused = pane;
  return pane;
}

// ---- Lookups ----------------------------------------------------------------

Pane* WindowModel::FindPane(WorkspaceId workspace, PaneId pane) {
  if (workspace != kInvalidId) {
    Workspace* ws = GetWorkspace(workspace);
    if (!ws) {
      return nullptr;
    }
    LayoutNode* n = NodeOfPaneIn(*ws, pane);
    return n ? n->pane.get() : nullptr;
  }
  for (Workspace& ws : workspaces_) {
    if (LayoutNode* n = NodeOfPaneIn(ws, pane)) {
      return n->pane.get();
    }
  }
  return nullptr;
}

const Pane* WindowModel::FindPane(WorkspaceId workspace, PaneId pane) const {
  return const_cast<WindowModel*>(this)->FindPane(workspace, pane);
}

std::vector<PaneId> WindowModel::PanesOf(WorkspaceId workspace) const {
  std::vector<PaneId> out;
  const Workspace* ws = GetWorkspace(workspace);
  if (ws) {
    for (const LayoutNode& column : ws->columns) {
      CollectPanes(column, &out);
    }
  }
  return out;
}

int WindowModel::ColumnIndexOf(WorkspaceId workspace, PaneId pane) const {
  const Workspace* ws = GetWorkspace(workspace);
  return ws ? ColumnOf(*ws, pane) : -1;
}

size_t WindowModel::ColumnCountOf(WorkspaceId workspace) const {
  const Workspace* ws = GetWorkspace(workspace);
  return ws ? ws->columns.size() : 0;
}

PaneId WindowModel::NeighborPane(WorkspaceId workspace,
                                 PaneId pane,
                                 Direction d) const {
  const Workspace* ws = GetWorkspace(workspace);
  if (!ws) {
    return kInvalidId;
  }
  const int col = ColumnOf(*ws, pane);
  if (col < 0) {
    return kInvalidId;
  }
  if (d == Direction::kLeft || d == Direction::kRight) {
    const int adjacent = col + (d == Direction::kRight ? 1 : -1);
    if (adjacent < 0 || adjacent >= static_cast<int>(ws->columns.size())) {
      return kInvalidId;
    }
    const Pane* p = FirstPane(ws->columns[adjacent]);
    return p ? p->id : kInvalidId;
  }
  std::vector<PaneId> order;
  CollectPanes(ws->columns[col], &order);
  int at = -1;
  for (size_t i = 0; i < order.size(); ++i) {
    if (order[i] == pane) {
      at = static_cast<int>(i);
      break;
    }
  }
  if (at < 0) {
    return kInvalidId;
  }
  const int next = at + (d == Direction::kDown ? 1 : -1);
  if (next < 0 || next >= static_cast<int>(order.size())) {
    return kInvalidId;
  }
  return order[next];
}

}  // namespace cmux
