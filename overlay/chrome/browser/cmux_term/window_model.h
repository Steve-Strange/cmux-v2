// Copyright 2019 The Chromium Authors
// Copyright (c) 2026 Alasdair Monk
// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later AND MIT AND BSD-3-Clause
//
// Contains Bonsplit- and Chromium-derived regions; see
// docs/source-provenance.md.

#ifndef CHROME_BROWSER_CMUX_TERM_WINDOW_MODEL_H_
#define CHROME_BROWSER_CMUX_TERM_WINDOW_MODEL_H_

#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

// Pure, UI-free model for the cmux window. It is the single cross-platform
// spine that a Chromium-Views renderer drives identically on macOS / Windows /
// Linux, and that a host compiler can unit-test directly (no Chromium, no
// Views, no gtest -- see window_model_test.cc). This is the "reimplement
// everything" foundation: the split-tree algebra is ours, ported in spirit from
// Bonsplit but depending on NOTHING. The renderer keeps a parallel tree of
// native views and applies the structure this model computes.
//
// The window is a hierarchy:
//   WindowModel
//     -> Workspace        (a vertical tab in the rail; optionally grouped)
//          -> columns     (the niri strip: an ordered row of column roots)
//               -> LayoutNode  (each column is a split tree: Split | Pane)
//                    -> Pane   (a leaf: a stack of SurfaceTabs, one selected)
//                         -> SurfaceTab (one surface: web page or terminal)
//
// The niri "strip" and the tiled "splits" COMPOSE: a workspace is a row of
// fixed-width columns (scrolled horizontally), and each column is its own
// bonsplit tree of panes tiled within the column. Invariant: a workspace
// always has >= 1 column, a column always has >= 1 pane, and a pane always
// has >= 1 tab.

namespace cmux {

// Opaque, process-unique ids. 0 means "none/invalid". Ids are never reused
// within a WindowModel's lifetime, so a stale id is always detectably invalid.
using WorkspaceId = int64_t;
using WorkspaceGroupId = int64_t;
using PaneId = int64_t;
using SplitId = int64_t;
using SurfaceTabId = int64_t;

inline constexpr int64_t kInvalidId = 0;
inline constexpr double kMinSplitRatio = 0.05;
inline constexpr double kMaxSplitRatio = 0.95;

enum class SurfaceKind { kWeb, kTerminal };

// First == side by side (a vertical divider); Second == stacked (a horizontal
// divider). Named by the divider's content arrangement, matching niri/Bonsplit.
enum class SplitOrientation { kHorizontal, kVertical };

// Spatial focus movement across a workspace. kLeft/kRight step between
// columns; kUp/kDown step between panes within the same column (in-order).
enum class Direction { kLeft, kRight, kUp, kDown };

// Per-workspace column policy. kStrip is the niri-style fixed-width scrollable
// column strip. kTiled keeps the same ordered columns and per-column bonsplit
// trees, but fits every column into the viewport at once by normalizing each
// column's stored width fraction into a share. Runtime-only for now; Stage-2
// session restore should persist this with the workspace.
enum class WorkspaceLayoutMode { kStrip, kTiled };

// Mirrors Chromium's tab_groups::TabGroupColorId (the 9-color palette). Kept as
// our own enum so the model stays Chromium-free; the renderer maps it to the
// real TabGroupColorId to get Chrome-exact group colors.
enum class GroupColor {
  kGrey,
  kBlue,
  kRed,
  kYellow,
  kGreen,
  kPink,
  kPurple,
  kCyan,
  kOrange,
};

// One internal tab = one surface. Metadata only; the renderer binds the actual
// WebContents / Ghostty surface by id.
struct SurfaceTab {
  SurfaceTabId id = kInvalidId;
  SurfaceKind kind = SurfaceKind::kWeb;
  std::string title;
  bool loading = false;
};

// A leaf of the layout tree: a non-empty stack of tabs with one selected.
struct Pane {
  PaneId id = kInvalidId;
  std::vector<SurfaceTab> tabs;
  SurfaceTabId selected = kInvalidId;

  Pane();
  ~Pane();
  Pane(Pane&&);
  Pane& operator=(Pane&&);

  const SurfaceTab* SelectedTab() const;
  SurfaceTab* FindTab(SurfaceTabId tab);
  int IndexOfTab(SurfaceTabId tab) const;  // -1 if absent
};

struct Split;

// A node in a workspace's layout tree: EXACTLY one of pane/split is set.
struct LayoutNode {
  std::unique_ptr<Pane> pane;
  std::unique_ptr<Split> split;

  LayoutNode();
  ~LayoutNode();
  LayoutNode(LayoutNode&&);
  LayoutNode& operator=(LayoutNode&&);

  bool is_pane() const { return pane != nullptr; }
  bool is_split() const { return split != nullptr; }

  static LayoutNode MakePane(std::unique_ptr<Pane> p);
  static LayoutNode MakeSplit(std::unique_ptr<Split> s);
};

// An internal node: two children divided at `ratio` (first's fraction, 0..1).
struct Split {
  SplitId id = kInvalidId;
  SplitOrientation orientation = SplitOrientation::kHorizontal;
  double ratio = 0.5;
  LayoutNode first;
  LayoutNode second;

  Split();
  ~Split();
  Split(Split&&);
  Split& operator=(Split&&);
};

// One vertical workspace row. Grouping is deliberately separate from the
// workspace itself, matching Chromium's tab collection model: a workspace is
// either an ungrouped top-level item or a member of one shallow group.
struct Workspace {
  WorkspaceId id = kInvalidId;
  // Stable identity shared with cmux-tui. Browser-only layout remains keyed
  // by this value while daemon-local numeric ids may change across restarts.
  std::string registry_key;
  WorkspaceGroupId group = kInvalidId;
  GroupColor preferred_group_color = GroupColor::kGrey;
  std::string title;
  // The strip, left to right: one split tree per column. Always >= 1 column,
  // every column holds >= 1 pane, and every pane holds >= 1 tab.
  std::vector<LayoutNode> columns;
  // Per-column width override, kept in lockstep with `columns`. 0 means use the
  // renderer's default column width. New workspaces seed column 0 at 1.0 so the
  // first pane starts full-bleed; later columns keep the 0 sentinel.
  std::vector<double> column_widths;
  WorkspaceLayoutMode layout_mode = WorkspaceLayoutMode::kStrip;
  PaneId focused = kInvalidId;  // the active pane within this workspace

  Workspace();
  ~Workspace();
  Workspace(Workspace&&);
  Workspace& operator=(Workspace&&);
};

// A Chromium-style shallow workspace group. Group identity and visual data are
// independent from its member workspaces, so every workspace remains a normal,
// selectable row.
struct WorkspaceGroup {
  WorkspaceGroupId id = kInvalidId;
  std::string title;
  GroupColor color = GroupColor::kGrey;
  bool collapsed = false;

  WorkspaceGroup();
  ~WorkspaceGroup();
  WorkspaceGroup(WorkspaceGroup&&);
  WorkspaceGroup& operator=(WorkspaceGroup&&);
};

// A flattened render row for one visible workspace. Group headers are rendered
// from WorkspaceGroup metadata immediately before the group's first member.
struct RailItem {
  WorkspaceId workspace = kInvalidId;
  WorkspaceGroupId group = kInvalidId;
  int depth = 0;  // 0 == ungrouped, 1 == inside a shallow group
  bool first_in_group = false;
  bool last_in_group = false;
};

// ID-based equivalent of ui::ListSelectionModel for workspace rows. Drag
// initialization captures this before mouse-down selection changes so a
// canceled drag can restore the exact selected set, active item, and anchor.
struct WorkspaceSelectionState {
  WorkspaceSelectionState();
  WorkspaceSelectionState(const WorkspaceSelectionState&);
  WorkspaceSelectionState& operator=(const WorkspaceSelectionState&);
  WorkspaceSelectionState(WorkspaceSelectionState&&);
  WorkspaceSelectionState& operator=(WorkspaceSelectionState&&);
  WorkspaceSelectionState(std::vector<WorkspaceId> selected,
                          WorkspaceId active,
                          WorkspaceId anchor);
  ~WorkspaceSelectionState();

  std::vector<WorkspaceId> selected;
  WorkspaceId active = kInvalidId;
  WorkspaceId anchor = kInvalidId;
};

// The fresh model identities created by DuplicateWorkspace(). Surface
// mappings let the renderer bind independently-cloned WebContents to the
// corresponding duplicate tabs without relying on traversal order.
struct DuplicateWorkspaceResult {
  DuplicateWorkspaceResult();
  DuplicateWorkspaceResult(const DuplicateWorkspaceResult&);
  DuplicateWorkspaceResult& operator=(const DuplicateWorkspaceResult&);
  DuplicateWorkspaceResult(DuplicateWorkspaceResult&&);
  DuplicateWorkspaceResult& operator=(DuplicateWorkspaceResult&&);
  ~DuplicateWorkspaceResult();

  WorkspaceId workspace = kInvalidId;
  std::vector<std::pair<PaneId, PaneId>> panes;
  std::vector<std::pair<SurfaceTabId, SurfaceTabId>> tabs;
};

// Shared pure-model helpers used by both state mutation and geometry. Keeping
// these in one place prevents model state and rendered layout from diverging.
double ClampSplitRatio(double ratio);
bool SubtreeHasPane(const LayoutNode& node, PaneId id);
int ColumnOf(const Workspace& ws, PaneId id);

class WindowModel {
 public:
  WindowModel();
  ~WindowModel();

  WindowModel(const WindowModel&) = delete;
  WindowModel& operator=(const WindowModel&) = delete;

  // ---- Workspaces + shallow groups -----------------------------------------
  // Adds a top-level workspace with one pane of `kind`, selects it, returns id.
  WorkspaceId AddWorkspace(SurfaceKind first_tab_kind = SurfaceKind::kWeb,
                           const std::string& title = {},
                           const std::string& registry_key = {});
  // Compatibility helper for callers that add a workspace "under" another:
  // the parent and new workspace are placed in the same shallow group. If the
  // parent is already grouped, the new workspace is appended to that group.
  WorkspaceId AddChildWorkspace(WorkspaceId parent,
                                 SurfaceKind first_tab_kind = SurfaceKind::kWeb,
                                const std::string& title = {},
                                const std::string& registry_key = {});
  // Deep-copies a workspace immediately after `source`, preserving its group,
  // title, layout tree, widths, tab stacks/selections, layout mode, and focused
  // pane. Every workspace/pane/split/tab receives a fresh process-unique id.
  // The duplicate becomes the sole selected workspace.
  DuplicateWorkspaceResult DuplicateWorkspace(
      WorkspaceId source,
      std::string registry_key = std::string());
  // Removes the workspace. If it was selected, selection moves to a neighbor.
  void CloseWorkspace(WorkspaceId id);
  // Makes `id` the sole selected and active workspace, and resets the range
  // anchor to it. Unknown ids are ignored.
  void SelectWorkspace(WorkspaceId id);
  // Replaces the selection with the inclusive rail-order range from the
  // anchor to `id`, retaining the anchor and making `id` active. If there is
  // no valid anchor, this is equivalent to SelectWorkspace(id).
  void ExtendWorkspaceSelectionTo(WorkspaceId id);
  // Adds the inclusive anchor-to-`id` range to the existing selection and
  // makes `id` active. This is Chromium's Command/Ctrl+Shift behavior.
  void AddWorkspaceSelectionFromAnchorTo(WorkspaceId id);
  // Adds or removes `id` from the selection. The sole selected workspace
  // cannot be deselected. Newly-added workspaces become active and anchor;
  // removing the active or anchor workspace repairs those roles to the first
  // selected workspace in rail order. Returns whether selection changed.
  bool ToggleWorkspaceSelection(WorkspaceId id);
  // Makes an already-selected workspace active and the range anchor without
  // changing the selected set. Used when an actual multi-drag starts.
  void ActivateWorkspaceInSelection(WorkspaceId id);
  WorkspaceSelectionState GetWorkspaceSelectionState() const;
  // Restores a previously captured state without revealing/expanding its
  // active workspace. Chromium uses this path while reverting tab drags,
  // including drags of collapsed groups.
  void RestoreWorkspaceSelectionState(const WorkspaceSelectionState& state);
  // Moves `id` into `new_parent`'s shallow group at member `index`. If the
  // parent is ungrouped, a new group containing both workspaces is created.
  // kInvalidId ungroups the workspace and moves it to `index` in flat rail
  // order. -1 appends in either destination.
  void MoveWorkspace(WorkspaceId id, WorkspaceId new_parent, int index);
  // Atomically moves an ID set in current rail order. `index` is interpreted
  // after the moved workspaces have been removed: a flat-root index when
  // `new_parent` is invalid, or a member index in `new_parent`'s existing
  // group. Unlike MoveWorkspace, this never creates a group as a side effect.
  void MoveWorkspaces(const std::vector<WorkspaceId>& ids,
                      WorkspaceId new_parent,
                      int index);
  // Explicit group commands used by the Helium-style context menu. Dragging
  // never calls CreateWorkspaceGroup; it may only join an existing group.
  WorkspaceGroupId CreateWorkspaceGroup(WorkspaceId id);
  // Atomically removes valid grouped IDs from their groups. Detached
  // workspaces retain their relative rail order while surviving group members
  // remain contiguous; unknown, duplicate, and already-ungrouped IDs are
  // ignored. Selection and active-workspace state are unchanged.
  void UngroupWorkspaces(const std::vector<WorkspaceId>& ids);
  void UngroupWorkspace(WorkspaceId id);
  // Toggles the group containing `id`; retained as the rail-facing API.
  void SetWorkspaceExpanded(WorkspaceId id, bool expanded);
  void SetWorkspaceTitle(WorkspaceId id, const std::string& title);
  // Sets the containing group's color (or remembers the preference until the
  // workspace is grouped).
  void SetWorkspaceColor(WorkspaceId id, GroupColor color);
  void SetWorkspaceGroupTitle(WorkspaceGroupId id, const std::string& title);
  void SetWorkspaceGroupCollapsed(WorkspaceGroupId id, bool collapsed);
  // Switches a workspace between the scrollable niri strip and tiled mode.
  // Unknown workspaces are ignored; the mode is runtime-only until session
  // restore is taught to carry Workspace::layout_mode.
  void SetWorkspaceLayoutMode(WorkspaceId id, WorkspaceLayoutMode mode);
  WorkspaceLayoutMode ToggleWorkspaceLayoutMode(WorkspaceId id);

  // `selected_workspace()` is the active workspace kept visible by the window.
  // Multiple workspace rows may additionally be selected for bulk commands.
  WorkspaceId selected_workspace() const { return selected_; }
  WorkspaceId workspace_selection_anchor() const {
    return selection_anchor_;
  }
  bool IsWorkspaceSelected(WorkspaceId id) const;
  // Selected ids in current rail order, independent of the order in which
  // they were selected. IDs remain selected across moves/reorders.
  std::vector<WorkspaceId> selected_workspaces() const;
  // Chromium context commands target the complete selection only when invoked
  // on a selected row; invoking them on an unselected row targets that row
  // alone without disturbing the selection.
  std::vector<WorkspaceId> WorkspacesForCommand(WorkspaceId context) const;
  size_t workspace_count() const { return workspaces_.size(); }
  const Workspace* GetWorkspace(WorkspaceId id) const;
  Workspace* GetWorkspace(WorkspaceId id);
  const Workspace* FindWorkspaceByRegistryKey(const std::string& key) const;
  Workspace* FindWorkspaceByRegistryKey(const std::string& key);
  // Apply canonical registry order while preserving contiguous shallow groups.
  void ReorderWorkspaceSiblingsByRegistryKeys(
      const std::vector<std::string>& keys);
  const std::vector<Workspace>& workspaces() const { return workspaces_; }
  // All workspaces in rail order. Group members are always contiguous.
  const std::vector<WorkspaceId>& roots() const { return roots_; }
  const std::vector<WorkspaceGroup>& workspace_groups() const {
    return workspace_groups_;
  }
  const WorkspaceGroup* GetWorkspaceGroup(WorkspaceGroupId id) const;
  WorkspaceGroup* GetWorkspaceGroup(WorkspaceGroupId id);
  std::vector<WorkspaceId> WorkspacesInGroup(WorkspaceGroupId id) const;

  // Visible workspace rows in rail order, skipping members of collapsed
  // groups. The renderer inserts one group header before first_in_group.
  std::vector<RailItem> BuildRail() const;

  // ---- Layout (within a workspace) -----------------------------------------
  // Inserts a new column at `index` among `workspace`'s columns (clamped;
  // -1 == append), holding one pane with one tab of `kind`. Focuses the new
  // pane and returns its id (kInvalidId on failure).
  PaneId AddColumn(WorkspaceId workspace, SurfaceKind kind, int index = -1);
  // Splits `pane` in two WITHIN the column containing it (at any depth), the
  // new pane carrying one tab of `new_tab_kind`. `ratio` is the existing
  // pane's fraction. Returns the new pane id (or kInvalidId on failure).
  // Focus moves to the new pane.
  PaneId SplitPane(WorkspaceId workspace,
                   PaneId pane,
                   SplitOrientation orientation,
                   double ratio,
                   SurfaceKind new_tab_kind = SurfaceKind::kWeb);
  // Closes `pane`, collapsing its parent split into the sibling. If `pane` was
  // its column's only pane, the column is removed. No-op if it is the
  // workspace's only pane (use CloseWorkspace for that). Focus falls to the
  // sibling that absorbed the space, else the nearest column's first pane.
  void ClosePane(WorkspaceId workspace, PaneId pane);
  void SetSplitRatio(WorkspaceId workspace, SplitId split, double ratio);
  void FocusPane(WorkspaceId workspace, PaneId pane);
  // Cycles the column containing `pane` through the supplied fractions in
  // their declared order. A width that is not already a mode starts at the
  // first mode. `default_fraction` is what a no-override column renders at.
  // Returns the new fraction, or 0 if the pane/workspace/modes are missing.
  double CycleColumnWidth(WorkspaceId workspace,
                          PaneId pane,
                          double default_fraction = 0.62);
  double CycleColumnWidth(WorkspaceId workspace,
                          PaneId pane,
                          const std::vector<double>& modes,
                          double default_fraction = 0.62);
  // Sets a freeform viewport fraction for mouse-driven column resizing.
  // Values are constrained to the same 10%-200% range as layout config.
  double SetColumnWidth(WorkspaceId workspace, PaneId pane, double fraction);

  // ---- Tabs (within a pane) ------------------------------------------------
  SurfaceTabId AddTab(WorkspaceId workspace,
                      PaneId pane,
                      SurfaceKind kind,
                      const std::string& title = {});
  // Closes a tab. If it was the pane's last tab, this behaves like ClosePane
  // on that pane. If that pane is the workspace's only pane, no-op so every
  // pane remains non-empty.
  void CloseTab(WorkspaceId workspace, PaneId pane, SurfaceTabId tab);
  void SelectTab(WorkspaceId workspace, PaneId pane, SurfaceTabId tab);
  bool SetTabLoading(WorkspaceId workspace,
                     PaneId pane,
                     SurfaceTabId tab,
                     bool loading);
  // Moves a tab to `to_pane` at `index` (clamped; -1 == append). Panes may be
  // in the same or different workspaces. Cross-pane moves select the moved tab
  // in the destination. Same-pane moves leave selection unchanged, so selection
  // follows the moved tab only if that tab was already selected. Refuses when
  // the move would empty the source workspace's only pane.
  void MoveTab(WorkspaceId from_workspace,
               PaneId from_pane,
               SurfaceTabId tab,
               WorkspaceId to_workspace,
               PaneId to_pane,
               int index);
  // Same-pane reorder. `to_index` is the desired final index in the pane's tab
  // vector, clamped to [0, tabs-1]. Selection remains by id.
  SurfaceTabId ReorderTab(WorkspaceId workspace,
                          PaneId pane,
                          SurfaceTabId tab,
                          int to_index);
  // Move one existing tab into a newly-created pane split against `target`.
  // The new pane is first for left/top drops (`insert_first`), second
  // otherwise. Focus moves to the new pane.
  PaneId SplitPaneWithTab(WorkspaceId workspace,
                          PaneId target,
                          SplitOrientation orientation,
                          bool insert_first,
                          PaneId from_pane,
                          SurfaceTabId tab);
  // Move one existing tab into a new column inserted at `column_index`
  // (pre-removal column coordinates; -1 appends). Focus moves to the new pane.
  PaneId MoveTabToNewColumn(WorkspaceId workspace,
                            PaneId from_pane,
                            SurfaceTabId tab,
                            int column_index);
  // Move every tab from `from_pane` into `to_pane` at `index` (-1 appends),
  // preserving order. The destination selection becomes the source's previous
  // selected tab, then the source pane is closed/collapsed.
  bool MergePaneInto(WorkspaceId workspace,
                     PaneId from_pane,
                     PaneId to_pane,
                     int index);
  // Whole-pane drag helpers used by the renderer. They preserve `pane`'s id
  // and tab stack while moving the leaf to a split edge or a new column.
  PaneId MovePaneToSplit(WorkspaceId workspace,
                         PaneId pane,
                         PaneId target,
                         SplitOrientation orientation,
                         bool insert_first);
  PaneId MovePaneToNewColumn(WorkspaceId workspace,
                             PaneId pane,
                             int column_index);

  // ---- Lookups (renderer convenience) --------------------------------------
  // Finds a pane by id anywhere in `workspace` (or in any workspace if
  // workspace == kInvalidId), searching every column. Returns nullptr if
  // absent.
  Pane* FindPane(WorkspaceId workspace, PaneId pane);
  const Pane* FindPane(WorkspaceId workspace, PaneId pane) const;
  // Collects every pane id in a workspace, flat in-order: columns left to
  // right, first-before-second within a column.
  std::vector<PaneId> PanesOf(WorkspaceId workspace) const;
  // Index of the column containing `pane`, or -1 if absent.
  int ColumnIndexOf(WorkspaceId workspace, PaneId pane) const;
  size_t ColumnCountOf(WorkspaceId workspace) const;
  // The pane focus would move to from `pane`: kLeft/kRight == the adjacent
  // column's first pane (in-order); kUp/kDown == the previous/next pane
  // in-order within the same column. kInvalidId when there is no neighbor.
  PaneId NeighborPane(WorkspaceId workspace, PaneId pane, Direction d) const;

 private:
  int64_t NextId() { return next_id_++; }
  std::unique_ptr<Pane> NewPaneWithTab(SurfaceKind kind,
                                       const std::string& title);
  LayoutNode DuplicateLayoutNode(const LayoutNode& source,
                                 DuplicateWorkspaceResult* result);
  // Index of a workspace in workspaces_, or -1.
  int IndexOfWorkspace(WorkspaceId id) const;
  int IndexInRailOrder(WorkspaceId id) const;
  WorkspaceGroupId EnsureGroupForWorkspace(WorkspaceId id);
  void RemoveWorkspaceFromGroup(WorkspaceId id);
  void RemoveEmptyGroup(WorkspaceGroupId id);
  int GroupStartIndex(WorkspaceGroupId id) const;
  int GroupEndIndex(WorkspaceGroupId id) const;
  // Returns the nearest legal flat insertion slot without splitting a
  // surviving group's contiguous run. `index` is interpreted against roots_
  // after the moving workspace(s) have been removed; equal-distance ties go
  // after the group.
  int ClampFlatInsertionIndexToGroupBoundary(int index) const;
  WorkspaceId FirstSelectedWorkspace() const;
  void RevealWorkspace(WorkspaceId id);

  std::vector<Workspace> workspaces_;
  std::vector<WorkspaceGroup> workspace_groups_;
  std::vector<WorkspaceId> roots_;  // flat rail order; grouped runs contiguous
  WorkspaceId selected_ = kInvalidId;
  WorkspaceId selection_anchor_ = kInvalidId;
  // IDs, rather than indices, make selection survive rail reorder/group moves
  // without the index-repair bookkeeping required by ListSelectionModel.
  std::set<WorkspaceId> selected_workspace_ids_;
  int64_t next_id_ = 1;
};

}  // namespace cmux

#endif  // CHROME_BROWSER_CMUX_TERM_WINDOW_MODEL_H_
