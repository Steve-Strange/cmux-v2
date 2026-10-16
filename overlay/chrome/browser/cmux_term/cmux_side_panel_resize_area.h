// Copyright 2022 The Chromium Authors
// Copyright 2026 Manaflow, Inc.
// Portions derived from Helium; Helium contributors retain copyright.
// SPDX-License-Identifier: GPL-3.0-or-later AND GPL-3.0-only AND BSD-3-Clause
//
// Adapted for cmux's pane-local side panel from Chromium's
// side_panel_resize_area and Helium's GPL-3.0-only side-panel resize geometry:
// https://github.com/imputnet/helium/blob/dee5600297344dd25b2dddb2ba343e19be7723f7/patches/helium/ui/side-panel.patch
// See docs/source-provenance.md for the licensed regions and source pins.

#ifndef CHROME_BROWSER_CMUX_TERM_CMUX_SIDE_PANEL_RESIZE_AREA_H_
#define CHROME_BROWSER_CMUX_TERM_CMUX_SIDE_PANEL_RESIZE_AREA_H_

#include "base/memory/raw_ptr.h"
#include "ui/views/controls/image_view.h"
#include "ui/views/controls/resize_area.h"
#include "ui/views/controls/resize_area_delegate.h"

namespace cmux {

class CmuxSidePanelResizeDelegate : public views::ResizeAreaDelegate {
 public:
  ~CmuxSidePanelResizeDelegate() override = default;

  virtual bool IsSidePanelRightAligned() const = 0;
  virtual void RecordSidePanelResizeMetrics() = 0;
  virtual void SetSidePanelKeyboardResized(bool keyboard_resized) = 0;
};

// Keyboard-accessible drag handle copied from Chrome and styled with Helium's
// two-DIP handle geometry.
class CmuxSidePanelResizeHandle : public views::ImageView {
  METADATA_HEADER(CmuxSidePanelResizeHandle, views::ImageView)

 public:
  CmuxSidePanelResizeHandle();

  void OnThemeChanged() override;
};

// Chrome's complete mouse, keyboard, focus-ring, accessibility, and metrics
// resize interaction, with Helium's six-DIP margin/outline layout.
class CmuxSidePanelResizeArea : public views::ResizeArea {
  METADATA_HEADER(CmuxSidePanelResizeArea, views::ResizeArea)

 public:
  explicit CmuxSidePanelResizeArea(CmuxSidePanelResizeDelegate* delegate);

  CmuxSidePanelResizeHandle* resize_handle_for_testing() const {
    return resize_handle_;
  }

  void OnMouseReleased(const ui::MouseEvent& event) override;
  bool OnKeyPressed(const ui::KeyEvent& event) override;
  void OnMouseMoved(const ui::MouseEvent& event) override;
  void OnMouseEntered(const ui::MouseEvent& event) override;
  void OnMouseExited(const ui::MouseEvent& event) override;
  void OnFocus() override;
  void OnBlur() override;
  void Layout(PassKey) override;

 private:
  void UpdateHandleVisibility(bool visible);

  raw_ptr<CmuxSidePanelResizeDelegate> delegate_;
  raw_ptr<CmuxSidePanelResizeHandle> resize_handle_ = nullptr;
};

}  // namespace cmux

#endif  // CHROME_BROWSER_CMUX_TERM_CMUX_SIDE_PANEL_RESIZE_AREA_H_
