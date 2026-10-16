// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Workspace adaptation of Chromium 150's TabGroupEditorBubbleView and
// ColorPickerView. Helium's footer suppression is retained. Exact revisions
// and contributor attribution are recorded in THIRD_PARTY_NOTICES.md.

#ifndef CHROME_BROWSER_CMUX_TERM_CMUX_RAIL_GROUP_EDITOR_BUBBLE_H_
#define CHROME_BROWSER_CMUX_TERM_CMUX_RAIL_GROUP_EDITOR_BUBBLE_H_

#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "chrome/browser/cmux_term/window_model.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/views/controls/textfield/textfield_controller.h"
#include "ui/views/view.h"

namespace ui {
class Accelerator;
class KeyEvent;
}

namespace views {
class LabelButton;
class Textfield;
class Widget;
}

namespace cmux {

class RailDelegate;
enum class WorkspaceGroupContextAction;

// The native workspace-group editor shown from a vertical group header. Its
// view hierarchy and metrics follow Chromium's unsaved tab-group editor; the
// model callbacks are the only workspace-specific boundary.
class CmuxRailGroupEditorBubble : public views::View,
                                  public views::TextfieldController {
  METADATA_HEADER(CmuxRailGroupEditorBubble, views::View)

 public:
  static base::WeakPtr<CmuxRailGroupEditorBubble> Show(
      RailDelegate* delegate,
      WorkspaceGroupId group,
      const std::string& title,
      GroupColor color,
      views::View* anchor_view,
      base::OnceClosure closed_callback);

  CmuxRailGroupEditorBubble(const CmuxRailGroupEditorBubble&) = delete;
  CmuxRailGroupEditorBubble& operator=(const CmuxRailGroupEditorBubble&) =
      delete;
  ~CmuxRailGroupEditorBubble() override;

  WorkspaceGroupId group_id() const { return group_; }

  // views::View:
  void AddedToWidget() override;
  gfx::Size CalculatePreferredSize(
      const views::SizeBounds& available_size) const override;
  bool AcceleratorPressed(const ui::Accelerator& accelerator) override;

  // views::TextfieldController:
  void ContentsChanged(views::Textfield* sender,
                       const std::u16string& new_contents) override;
  bool HandleKeyEvent(views::Textfield* sender,
                      const ui::KeyEvent& key_event) override;

 private:
  CmuxRailGroupEditorBubble(RailDelegate* delegate,
                            WorkspaceGroupId group,
                            const std::string& title,
                            GroupColor color,
                            base::OnceClosure closed_callback);

  void AddMenuItem(WorkspaceGroupContextAction action,
                   const std::u16string& label);
  void RunAction(WorkspaceGroupContextAction action);
  void OnColorSelected(GroupColor color);

  raw_ptr<RailDelegate> delegate_ = nullptr;
  const WorkspaceGroupId group_ = kInvalidId;
  raw_ptr<views::Textfield> title_field_ = nullptr;
  std::vector<raw_ptr<views::LabelButton>> simple_menu_items_;
  base::OnceClosure closed_callback_;
  base::WeakPtrFactory<CmuxRailGroupEditorBubble> weak_factory_{this};
};

}  // namespace cmux

#endif  // CHROME_BROWSER_CMUX_TERM_CMUX_RAIL_GROUP_EDITOR_BUBBLE_H_
