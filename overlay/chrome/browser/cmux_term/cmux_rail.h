// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef CHROME_BROWSER_CMUX_TERM_CMUX_RAIL_H_
#define CHROME_BROWSER_CMUX_TERM_CMUX_RAIL_H_

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "base/callback_list.h"
#include "base/functional/callback_forward.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/time/time.h"
#include "chrome/browser/cmux_term/cmux_rail_config.h"
#include "chrome/browser/cmux_term/window_model.h"
#include "ui/base/mojom/menu_source_type.mojom.h"
#include "ui/menus/simple_menu_model.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/gfx/geometry/point.h"
#include "ui/gfx/image/image_skia.h"
#include "ui/views/view.h"

namespace gfx {
class Rect;
}

namespace ui {
class Accelerator;
class MouseEvent;
}

namespace views {
class ScrollView;
}

namespace cmux {

class CmuxRailRow;        // one workspace row; defined in cmux_rail.cc
class CmuxRailScrollBar;  // auto-hiding overlay scrollbar (cmux_rail.cc)
class CmuxRailHeader;     // optional Arc-style header (cmux_rail.cc)
class CmuxRailNewWorkspaceButton;
class CmuxRailGroupHeader;
class CmuxRailGroupLine;
class CmuxRailCollectionView;
class CmuxRailGroupView;
class CmuxRailWorkspaceContextMenu;
class CmuxRailGroupEditorBubble;
class CmuxRailDragEventTracker;
class CmuxRailDragScrollHandler;
class CmuxRailDragStartAnimation;
class CmuxRailHoverCardController;

// Browser-neutral data boundary for Chromium's tab-hover-card interaction.
// The browser-backed rail delegate maps the focused surface in a workspace to
// the title/domain/thumbnail metadata that a single Chromium tab would supply.
enum class WorkspaceHoverCardPreviewReadiness {
  kNotReady,
  kReadyForInitialCapture,
  kReadyForFinalCapture,
};

struct WorkspaceHoverCardData {
  WorkspaceHoverCardData();
  WorkspaceHoverCardData(const WorkspaceHoverCardData&);
  WorkspaceHoverCardData& operator=(const WorkspaceHoverCardData&);
  WorkspaceHoverCardData(WorkspaceHoverCardData&&);
  WorkspaceHoverCardData& operator=(WorkspaceHoverCardData&&);
  ~WorkspaceHoverCardData();

  std::u16string title;
  std::u16string domain;
  // Stable cmux model identity for the selected surface represented by this
  // card. `preview_source_id` is an opaque, process-local identity for the
  // browser-owned ThumbnailImage and is never dereferenced by the rail.
  SurfaceTabId source_surface = kInvalidId;
  uintptr_t preview_source_id = 0;
  bool show_preview = false;
  bool has_preview_source = false;
  bool has_preview_data = false;
  bool is_discarded = false;
  bool is_crashed = false;
  WorkspaceHoverCardPreviewReadiness preview_readiness =
      WorkspaceHoverCardPreviewReadiness::kNotReady;
};

// Keeps Chromium's ThumbnailImage subscription alive without exposing the
// browser-only thumbnail implementation across cmux_chrome_ui's //ui boundary.
class WorkspaceHoverCardPreviewRequest {
 public:
  virtual ~WorkspaceHoverCardPreviewRequest() = default;
};

// Workspace adaptations of Helium's vertical-tab context-menu commands. The
// ordering and separators mirror Helium; commands without a workspace-level
// equivalent are exposed disabled by the delegate.
enum class WorkspaceContextAction {
  kNewWorkspaceBelow = 1,
  kNewSplitSideBySide,
  kNewSplitStacked,
  kAddToNewGroup,
  kRemoveFromGroup,
  kMoveToNewWindow,
  kReload,
  kDuplicate,
  kTogglePinned,
  kToggleSiteMuted,
  kAddToReadLater,
  kClose,
  kCloseOthers,
  kCloseBelow,
};

enum class WorkspaceGroupContextAction {
  kNewWorkspaceInGroup = 1,
  kMoveGroupToNewWindow,
  kCloseGroup,
  kUngroup,
};

enum class NewWorkspaceContextAction {
  kNewWorkspace = 1,
  kNewWorkspaceInRecentGroup,
  kNewWorkspaceGroup,
  kNewSplitView,
};

struct RecentWorkspaceGroup {
  WorkspaceGroupId id = kInvalidId;
  std::string title;
  int member_count = 0;
};

// What the rail asks its owner (the strip renderer) to do. Each mutates the
// pure WindowModel and then calls RefreshRail() to push the new state back.
class RailDelegate {
 public:
  virtual ~RailDelegate() = default;
  virtual void OnSelectWorkspace(WorkspaceId id) = 0;
  // Chromium vertical tabs mutate multi-selection on mouse-down so a drag can
  // start with the resulting set. The defaults retain single-selection for
  // lightweight embedders; the browser-backed delegate implements the full
  // WindowModel selection semantics.
  virtual void OnExtendWorkspaceSelection(WorkspaceId id);
  virtual void OnAddWorkspaceSelectionFromAnchorTo(WorkspaceId id);
  virtual void OnToggleWorkspaceSelection(WorkspaceId id);
  virtual void OnActivateWorkspaceInSelection(WorkspaceId id);
  virtual bool IsWorkspaceSelected(WorkspaceId id) const;
  virtual WorkspaceSelectionState GetWorkspaceSelectionState() const;
  virtual void OnRestoreWorkspaceSelectionState(
      const WorkspaceSelectionState& state);
  virtual void OnNewWorkspace() = 0;
  virtual std::optional<RecentWorkspaceGroup>
  GetMostRecentWorkspaceGroupForContextMenu() const;
  virtual bool IsNewWorkspaceContextActionEnabled(
      NewWorkspaceContextAction action) const;
  virtual void OnNewWorkspaceContextAction(
      NewWorkspaceContextAction action,
      WorkspaceGroupId recent_group);
  virtual bool GetNewWorkspaceContextAccelerator(
      NewWorkspaceContextAction action,
      ui::Accelerator* accelerator) const;
  // Toggle the shallow group containing `id`.
  virtual void OnToggleExpanded(WorkspaceId id) = 0;
  // Rename one canonical workspace. Default no-op keeps lightweight rail
  // embedders source-compatible while the browser and demo opt in.
  virtual void OnRenameWorkspace(WorkspaceId id, const std::string& name) {}
  // Move `id` beside `new_parent`, joining (or creating) its shallow group.
  // kInvalidId ungroups and moves it to a top-level position.
  virtual void OnMoveWorkspace(WorkspaceId id,
                               WorkspaceId new_parent,
                               int index) = 0;
  // Chromium applies a drag or group-menu move to the complete selection when
  // the context row is selected, otherwise to that row alone.
  virtual void OnMoveWorkspaceSelection(WorkspaceId context,
                                        WorkspaceId new_parent,
                                        int index);
  // Moves an existing shallow group as one atomic top-level collection. This
  // is the workspace equivalent of dragging Chromium's tab-group header and
  // intentionally does not infer the moved members from row selection.
  virtual void OnMoveWorkspaceGroup(WorkspaceGroupId id, int index);
  // Begin a native window drag from the rail header. Default no-op keeps the
  // cross-platform demo and native-frame platforms independent of window seams.
  virtual void OnBeginWindowDrag(const ui::MouseEvent& event) {}
  // The hover "X" was clicked. Default no-op so existing delegates compile.
  virtual void OnCloseWorkspace(WorkspaceId id) {}
  // The rail's persisted config was loaded and applied. Default no-op keeps
  // non-window delegates independent of layout/theme derivation.
  virtual void OnRailConfigLoaded() {}
  // Visible only after a complete package has passed manifest signature,
  // archive hash, extraction, and executable validation.
  virtual void OnApplyUpdate() {}
  virtual void OnWorkspaceContextAction(WorkspaceId id,
                                        WorkspaceContextAction action) {}
  virtual bool IsWorkspaceContextActionEnabled(
      WorkspaceId id,
      WorkspaceContextAction action) const;
  virtual bool IsWorkspaceContextActionToggled(
      WorkspaceId id,
      WorkspaceContextAction action) const;
  // Browser feature state is supplied at the delegate boundary so the rail's
  // //ui-only target does not link chrome/browser/ui/ui_features.cc.
  virtual bool IsWorkspaceContextMenuSimplificationEnabled() const;
  virtual int WorkspaceContextUrlCount(WorkspaceId id) const;
  virtual int WorkspaceContextTargetCount(WorkspaceId id) const;
  virtual WorkspaceHoverCardData GetWorkspaceHoverCardData(
      WorkspaceId id) const;
  virtual std::unique_ptr<WorkspaceHoverCardPreviewRequest>
  RequestWorkspaceHoverCardPreview(
      WorkspaceId id,
      SurfaceTabId source_surface,
      uintptr_t preview_source_id,
      base::RepeatingCallback<void(gfx::ImageSkia)> callback);
  // Chromium inserts account/profile-dependent tab actions (Reading List,
  // Glic, Send to your devices, then extension-provided TAB items) between
  // Mute and Hibernate. The browser-backed delegate owns the supporting
  // submenu models while this cross-platform rail owns the top-level model.
  virtual void AppendOptionalWorkspaceContextMenuItems(
      WorkspaceId id,
      ui::SimpleMenuModel* model,
      ui::SimpleMenuModel::Delegate* menu_delegate) {}
  virtual bool IsOptionalWorkspaceContextMenuCommandEnabled(
      WorkspaceId id,
      int command_id) const;
  virtual bool IsOptionalWorkspaceContextMenuCommandChecked(
      WorkspaceId id,
      int command_id) const;
  virtual bool IsOptionalWorkspaceContextMenuCommandVisible(
      WorkspaceId id,
      int command_id) const;
  virtual void ExecuteOptionalWorkspaceContextMenuCommand(
      WorkspaceId id,
      int command_id,
      int event_flags) {}
  // Chromium's group editor applies title and color changes live while the
  // bubble remains open.
  virtual void OnSetWorkspaceGroupTitle(WorkspaceGroupId id,
                                        const std::string& title) {}
  virtual void OnSetWorkspaceGroupColor(WorkspaceGroupId id,
                                        GroupColor color) {}
  virtual void OnWorkspaceGroupContextAction(
      WorkspaceGroupId id,
      WorkspaceGroupContextAction action) {}
  virtual bool IsWorkspaceGroupContextActionEnabled(
      WorkspaceGroupId id,
      WorkspaceGroupContextAction action) const;
};

// A Helium-style vertical rail: shallow workspace groups have their own header
// and color line, while each visible workspace has one compact row. The rail is
// cross-platform and depends only on //ui/views + //ui/base + //base.
class CmuxRail : public views::View {
  METADATA_HEADER(CmuxRail, views::View)

 public:
  // `forced_style` overrides the CMUX_RAIL_STYLE env selection when >= 0
  // (0=minimal, 1=polished, 2=arc); -1 reads the env (default minimal). A
  // persisted ~/.cmux_rail.json, if present, overrides the preset.
  explicit CmuxRail(RailDelegate* delegate, int forced_style = -1);
  CmuxRail(const CmuxRail&) = delete;
  CmuxRail& operator=(const CmuxRail&) = delete;
  ~CmuxRail() override;

  // Sync the rows to the model: reuses rows by WorkspaceId, creates/destroys as
  // the visible set changes, updates content + selection, and animates rows to
  // their slots.
  void Update(const WindowModel& model);
  void Update(const WindowModel& model,
              const std::map<WorkspaceId, gfx::ImageSkia>& icons);

  // Swap in a new visual config and relayout/repaint. Does NOT persist.
  void SetConfig(const RailConfig& config);
  const RailConfig& config() const { return config_; }
  void SetDisplayMode(sidebar_metrics::SidebarMode mode);
  sidebar_metrics::SidebarMode display_mode() const { return display_mode_; }
  void SetUpdateReady(bool ready, const std::string& version);
  gfx::Rect FirstRowBoundsForTesting() const;
  gfx::Rect NewWorkspaceButtonBoundsForTesting() const;
  void SetNewWorkspaceButtonHoveredForTesting();
  bool NewWorkspaceButtonIsHoveredForTesting() const;
  // Refresh an already-targeted hover card without rebuilding rail rows.
  void NotifyWorkspaceDataChanged(WorkspaceId id);

  // views::View:
  void Layout(PassKey) override;
  bool OnMouseWheel(const ui::MouseWheelEvent& event) override;
  void OnMouseExited(const ui::MouseEvent& event) override;
  void AddedToWidget() override;
  void RemovedFromWidget() override;

  // Row -> rail callbacks (public so the row impl in the .cc can reach them).
  void SelectRow(WorkspaceId id);
  void ActivateRowInSelection(WorkspaceId id);
  void ExtendSelectionToRow(WorkspaceId id);
  void AddSelectionFromAnchorToRow(WorkspaceId id);
  void ToggleRowSelection(WorkspaceId id);
  bool IsRowSelected(WorkspaceId id) const;
  WorkspaceSelectionState GetWorkspaceSelectionState() const;
  void ToggleRow(WorkspaceId id);
  void CloseRow(WorkspaceId id);
  void RenameRow(WorkspaceId id, const std::string& name);
  void ShowContextMenuForRow(
      WorkspaceId id,
      const gfx::Point& screen_pt,
      ui::mojom::MenuSourceType source_type =
          ui::mojom::MenuSourceType::kMouse);
  void ShowGroupEditorBubble(WorkspaceGroupId id);
  void ShowGroupEditorBubble(WorkspaceGroupId id, views::View* anchor_view);
  void ShiftWorkspace(WorkspaceId id,
                      bool toward_start,
                      bool move_to_end);
  void ShiftWorkspaceGroup(WorkspaceGroupId id, bool toward_start);

  // Manual drag-and-drop, driven by a CmuxRailRow's mouse events. Coordinates
  // are in screen space (stable while the dragged row moves).
  void StartRowDrag(CmuxRailRow* row,
                    const gfx::Point& press_screen_pt,
                    const gfx::Point& current_screen_pt,
                    const WorkspaceSelectionState& original_selection);
  void StartGroupDrag(WorkspaceGroupId id,
                      const gfx::Point& press_screen_pt,
                      const gfx::Point& current_screen_pt,
                      const WorkspaceSelectionState& original_selection);
  void UpdateRowDrag(const gfx::Point& screen_pt);
  void EndRowDrag(bool commit);

 private:
  friend class CmuxRailCollectionView;
  friend class CmuxRailDragStartAnimation;
  friend class CmuxRailGroupView;
  friend class CmuxRailRow;
  friend class CmuxRailWorkspaceContextMenu;
  // (Re)build the header/scrollbar children to match config_.
  void ApplyChrome();
  // Position the nested Chromium-style collection inside the clipped viewport.
  void ReflowRows(bool animated);
  // The outer collection's preferred height animates frame-by-frame. Mirror
  // each frame into the plain ScrollView contents wrapper after layout returns.
  void ScheduleContentGeometrySync();
  void RunScheduledContentGeometrySync();
  // Move the drag gap to a top-level logical slot.
  void OpenGapAt(int gap_slot);
  void ClearDragGaps();
  void RestoreDraggedRow();
  bool InDragGroup(const views::View* row) const;
  bool IsDraggedFullGroup(const views::View* view) const;
  bool ShouldSnapDragViewToTarget(const views::View& view) const;
  void ClearSnapDragViewsForParent(const views::View& parent);
  void ReparentDraggedFullGroupsToCollection();
  void PositionDraggedViews(int visual_block_top, bool nested_in_group);
  void OnDragStartAnimationProgressed();
  void OnDragStartAnimationEnded();
  void OnDragContentsScrolled();
  bool ActiveDragStructureMatches(const WindowModel& model) const;
  void RefreshActiveDragModelFields(
      const WindowModel& model,
      const std::map<WorkspaceId, gfx::ImageSkia>& icons);
  void StartDrag(CmuxRailRow* row,
                 const gfx::Point& press_screen_pt,
                 const gfx::Point& current_screen_pt,
                 WorkspaceGroupId header_group,
                 const WorkspaceSelectionState& original_selection);
  void OnGroupEditorBubbleClosed(WorkspaceGroupId id);
  void OnWorkspaceRowMouseEntered(CmuxRailRow* row);
  void OnWorkspaceRowFocused(CmuxRailRow* row);
  void OnWorkspaceRowBlurred();
  void OnWorkspaceRowEvent();
  void OnWorkspaceRowDataChanged(CmuxRailRow* row);
  void CloseRowNow(WorkspaceId id);
  void BeginRenameRow(WorkspaceId id);
  bool AnimationsEnabled() const;
  base::TimeDelta AnimationDuration(int base_ms) const;
  void ApplyAnimationConfig();
  // The max vertical scroll so the last row stays reachable below the header.
  int ContentHeight() const;
  int MaxScroll() const;
  void ClampScroll();
  void UpdateScrollbar(bool flash);

  raw_ptr<RailDelegate> delegate_;
  RailConfig config_;
  int header_h_ = 0;  // == config_.header_height or 0
  sidebar_metrics::SidebarMode display_mode_ =
      sidebar_metrics::SidebarMode::kExpanded;
  raw_ptr<CmuxRailHeader> header_ = nullptr;
  raw_ptr<CmuxRailNewWorkspaceButton> footer_ = nullptr;
  raw_ptr<CmuxRailNewWorkspaceButton> update_footer_ = nullptr;
  bool update_ready_ = false;
  std::string update_version_;
  raw_ptr<views::ScrollView> scroll_view_ = nullptr;
  raw_ptr<views::View> content_ = nullptr;  // layer-backed scroll contents
  raw_ptr<CmuxRailCollectionView> collection_ = nullptr;
  bool content_geometry_sync_pending_ = false;
  raw_ptr<CmuxRailScrollBar> scrollbar_ = nullptr;
  std::unique_ptr<CmuxRailWorkspaceContextMenu> context_menu_;
  std::unique_ptr<CmuxRailHoverCardController> hover_card_controller_;
  base::CallbackListSubscription hover_card_scroll_subscription_;
  base::WeakPtr<CmuxRailGroupEditorBubble> group_editor_bubble_;
  // Every workspace row remains alive across group collapse. Rows are owned by
  // `collection_`, a nested group view, or temporarily by `content_` while
  // being dragged.
  std::vector<raw_ptr<CmuxRailRow>> rows_;
  std::map<WorkspaceGroupId, raw_ptr<CmuxRailGroupView>> group_views_;
  struct GroupMenuEntry {
    WorkspaceGroupId id = kInvalidId;
    WorkspaceId representative = kInvalidId;
    std::string title;
    GroupColor color = GroupColor::kGrey;
  };
  std::vector<GroupMenuEntry> group_menu_entries_;
  // ---- Drag state (null/-1 when not dragging) ----
  raw_ptr<CmuxRailRow> dragging_ = nullptr;
  // Selected workspace rows dragged as one ordered block. `dragging_` is the
  // source under the pointer and may appear anywhere in this rail-order list.
  std::vector<raw_ptr<CmuxRailRow>> drag_group_;
  // Chromium keeps a completely selected group atomic: its actual outer group
  // view (header, color line, and rows) travels in the drag overlay. A partial
  // group contributes only its selected rows.
  std::set<WorkspaceGroupId> drag_full_groups_;
  std::vector<raw_ptr<views::View>> drag_views_;
  // True only after every drag visual has been detached from its logical
  // collection and reparented to `content_`'s overlay coordinate space.
  bool drag_views_detached_ = false;
  std::vector<int> drag_view_heights_;
  // Offsets from pre-drag positions to Chromium's contiguous vertical stack.
  // The rich drag-start animation interpolates each value back to zero.
  std::vector<int> drag_start_offset_ys_;
  std::map<WorkspaceId, bool> drag_original_visibility_;
  int drag_primary_offset_y_ = 0;
  int drag_visual_height_ = 0;
  // Chromium's logical drag box includes one trailing inter-row padding after
  // the final visible row. Its bottom also expands the scroll contents while
  // the pointer is dragged beyond the current collection.
  int drag_unclamped_bottom_ = 0;
  int drag_grab_dy_ = 0;
  gfx::Point drag_last_point_in_screen_;
  int drag_last_visual_block_top_ = 0;
  bool drag_last_nested_in_group_ = false;
  std::unique_ptr<CmuxRailDragStartAnimation> drag_start_animation_;
  std::unique_ptr<CmuxRailDragScrollHandler> drag_scroll_handler_;
  int drag_slot_ = -1;
  WorkspaceGroupId drag_origin_group_ = kInvalidId;
  WorkspaceGroupId dragging_group_header_ = kInvalidId;
  WorkspaceId dragging_group_active_workspace_ = kInvalidId;
  std::optional<WorkspaceSelectionState> drag_original_selection_;
  // Chromium's TabDragController installs an application event monitor for
  // the lifetime of a drag so Escape can restore the pre-drag selection.
  std::unique_ptr<CmuxRailDragEventTracker> drag_event_tracker_;
  int drag_origin_index_ = -1;
  WorkspaceGroupId drop_group_ = kInvalidId;
  WorkspaceId drop_parent_ = kInvalidId;
  int drop_index_ = -1;
  // Dragged views snap to their destination on a successful drop. Other views
  // remain animated by their collection layout managers.
  std::set<const views::View*> snap_drag_views_to_target_;

  base::WeakPtrFactory<CmuxRail> weak_factory_{this};
};

}  // namespace cmux

#endif  // CHROME_BROWSER_CMUX_TERM_CMUX_RAIL_H_
