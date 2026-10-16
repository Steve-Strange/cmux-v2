// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef UI_VIEWS_EXAMPLES_CMUX_DEMO_EXAMPLE_H_
#define UI_VIEWS_EXAMPLES_CMUX_DEMO_EXAMPLE_H_

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "chrome/browser/cmux_term/cmux_layout_config.h"
#include "chrome/browser/cmux_term/cmux_rail.h"
#include "chrome/browser/cmux_term/cmux_strip_view.h"
#include "chrome/browser/cmux_term/cmux_tab_strip.h"
#include "chrome/browser/cmux_term/window_model.h"
#include "third_party/skia/include/core/SkColor.h"
#include "ui/color/color_provider.h"
#include "ui/color/color_provider_key.h"
#include "ui/gfx/geometry/point.h"
#include "ui/views/controls/slider.h"
#include "ui/views/controls/textfield/textfield_controller.h"
#include "ui/views/examples/example_base.h"
#include "ui/views/examples/views_examples_export.h"

namespace views {
class Checkbox;
class Label;
class Textfield;
}  // namespace views

namespace views::examples {

class CmuxDemoStripView;

// Installs the small Chrome-token surface used only by the standalone Views
// harness. The shipped browser supplies these IDs through Chrome's real color
// mixers; linking that browser graph into views_examples would defeat the
// cross-platform, fast-build boundary this demo exists to test.
VIEWS_EXAMPLES_EXPORT void AddCmuxDemoColorMixers(
    ui::ColorProvider* color_provider,
    const ui::ColorProviderKey& key);

// A views_examples panel that hosts the cross-platform cmux chrome UI with mock
// data, plus a live controls column that tweaks RailConfig and LayoutConfig
// knobs and persists them to disk.
class CmuxDemoExample : public ExampleBase,
                        public cmux::RailDelegate,
                        public cmux::TabStripDelegate,
                        public cmux::StripPaneDelegate,
                        public views::SliderListener,
                        public views::TextfieldController {
 public:
  // `rail_style`: -1 reads CMUX_RAIL_STYLE; 0=minimal, 1=polished, 2=arc.
  explicit CmuxDemoExample(int rail_style = -1,
                           const char* name = "Cmux Workspaces");
  CmuxDemoExample(const CmuxDemoExample&) = delete;
  CmuxDemoExample& operator=(const CmuxDemoExample&) = delete;
  ~CmuxDemoExample() override;

  // ExampleBase:
  void CreateExampleView(View* parent) override;

  // cmux::RailDelegate:
  void OnSelectWorkspace(cmux::WorkspaceId id) override;
  void OnExtendWorkspaceSelection(cmux::WorkspaceId id) override;
  void OnAddWorkspaceSelectionFromAnchorTo(cmux::WorkspaceId id) override;
  void OnToggleWorkspaceSelection(cmux::WorkspaceId id) override;
  void OnActivateWorkspaceInSelection(cmux::WorkspaceId id) override;
  bool IsWorkspaceSelected(cmux::WorkspaceId id) const override;
  cmux::WorkspaceSelectionState GetWorkspaceSelectionState() const override;
  void OnRestoreWorkspaceSelectionState(
      const cmux::WorkspaceSelectionState& state) override;
  void OnNewWorkspace() override;
  void OnToggleExpanded(cmux::WorkspaceId id) override;
  void OnRenameWorkspace(cmux::WorkspaceId id,
                         const std::string& name) override;
  void OnMoveWorkspace(cmux::WorkspaceId id,
                       cmux::WorkspaceId new_parent,
                       int index) override;
  void OnMoveWorkspaceGroup(cmux::WorkspaceGroupId id, int index) override;
  void OnCloseWorkspace(cmux::WorkspaceId id) override;

  // cmux::TabStripDelegate:
  void OnSelectTab(cmux::SurfaceTabId id) override;
  void OnCloseTab(cmux::SurfaceTabId id) override;
  void OnCloseOtherTabs(cmux::SurfaceTabId id) override;
  void OnNewTab(cmux::SurfaceKind kind) override;
  void OnNewTabRight(cmux::SurfaceTabId id) override;
  void OnMoveTabToNewColumn(cmux::SurfaceTabId id) override;
  void OnSplitRightWithTab(cmux::SurfaceTabId id) override;
  void OnSplitDownWithTab(cmux::SurfaceTabId id) override;
  bool IsTabContextActionEnabled(cmux::SurfaceTabId id,
                                 cmux::TabContextAction action) const override;
  bool IsTabContextActionToggled(cmux::SurfaceTabId id,
                                 cmux::TabContextAction action) const override;
  void OnTabContextAction(cmux::SurfaceTabId id,
                          cmux::TabContextAction action) override;
  void OnTabDragStarted(cmux::SurfaceTabId id,
                        const gfx::Point& screen_pt,
                        const gfx::Point& grab_offset) override;
  void OnTabDragUpdated(const gfx::Point& screen_pt) override;
  void OnTabDragEnded(bool commit) override;
  bool OnLiveReorderTab(cmux::SurfaceTabId id, int final_index) override;
  void OnPaneDragStarted(const gfx::Point& screen_pt,
                         const gfx::Point& grab_offset) override;
  void OnPaneDragUpdated(const gfx::Point& screen_pt) override;
  void OnPaneDragEnded(bool commit) override;

  // cmux::StripPaneDelegate:
  std::unique_ptr<View> CreateStripPane(cmux::PaneId id,
                                        const cmux::Pane& pane) override;

  // views::SliderListener:
  void SliderValueChanged(views::Slider* sender,
                          float value,
                          float old_value,
                          views::SliderChangeReason reason) override;

  // views::TextfieldController:
  void ContentsChanged(views::Textfield* sender,
                       const std::u16string& new_contents) override;

 private:
  void RefreshRail();
  void RefreshTabs();
  void RefreshStrip();

  // ---- Controls panel ----
  void BuildControls(View* parent);
  void ApplyAndSave();            // push cfg_ to the rail + persist to disk
  void ApplyLayoutAndSave();      // push layout_cfg_ live + persist to disk
  void SyncControlsFromConfig();  // reflect cfg_ back into the widgets
  void AddSliderRow(View* parent,
                    const char* label,
                    int* field,
                    int lo,
                    int hi,
                    bool layout = false);
  void AddDoubleSliderRow(View* parent,
                          const char* label,
                          double* field,
                          double lo,
                          double hi,
                          bool layout = false);
  void AddCheckRow(View* parent,
                   const char* label,
                   bool* field,
                   bool layout = false);
  void AddColorRow(View* parent, const char* label, SkColor* field);
  void AddLayoutColorRow(View* parent, const char* label, std::string* field);
  SkColor LayoutColorForField(const std::string* field) const;
  cmux::RailConfig RailConfigForDemo() const;

  // Mock workspace groups driving the rail.
  int rail_style_ = -1;
  cmux::WindowModel model_;
  cmux::RailConfig cfg_;
  cmux::LayoutConfig layout_cfg_;
  raw_ptr<cmux::CmuxRail> rail_ = nullptr;

  // Control bindings.
  struct SliderBind {
    raw_ptr<views::Slider> slider;
    raw_ptr<int> field;
    int lo;
    int hi;
    raw_ptr<views::Label> value_label;
    bool layout = false;
  };
  std::vector<SliderBind> slider_binds_;
  struct DoubleSliderBind {
    raw_ptr<views::Slider> slider;
    raw_ptr<double> field;
    double lo;
    double hi;
    raw_ptr<views::Label> value_label;
    bool layout = false;
  };
  std::vector<DoubleSliderBind> double_slider_binds_;
  std::map<views::Textfield*, raw_ptr<SkColor>> color_binds_;
  std::map<views::Textfield*, raw_ptr<std::string>> layout_color_binds_;
  std::map<views::Textfield*, raw_ptr<views::View>> swatches_;  // color preview
  struct CheckBind {
    raw_ptr<views::Checkbox> checkbox;
    raw_ptr<bool> field;
    bool layout = false;
  };
  std::vector<CheckBind> checks_;
  raw_ptr<views::Textfield> family_field_ = nullptr;
  raw_ptr<views::Textfield> icon_field_ = nullptr;
  bool syncing_ = false;  // guard control->cfg writes during programmatic sync

  // Mock per-pane tab strip.
  std::vector<cmux::SurfaceTab> tabs_;
  cmux::SurfaceTabId selected_tab_ = cmux::kInvalidId;
  int64_t next_tab_id_ = 1;
  raw_ptr<cmux::CmuxTabStrip> strip_ = nullptr;

  // The niri strip rendering the selected workspace's panes as columns.
  raw_ptr<CmuxDemoStripView> strip_view_ = nullptr;

  base::WeakPtrFactory<CmuxDemoExample> weak_factory_{this};
};

}  // namespace views::examples

#endif  // UI_VIEWS_EXAMPLES_CMUX_DEMO_EXAMPLE_H_
