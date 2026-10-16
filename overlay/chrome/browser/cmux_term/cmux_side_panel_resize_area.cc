// Copyright 2022 The Chromium Authors
// Copyright 2026 Manaflow, Inc.
// Portions derived from Helium; Helium contributors retain copyright.
// SPDX-License-Identifier: GPL-3.0-or-later AND GPL-3.0-only AND BSD-3-Clause
//
// Adapted for cmux's pane-local side panel from Chromium's
// side_panel_resize_area and Helium's GPL-3.0-only side-panel resize geometry:
// https://github.com/imputnet/helium/blob/dee5600297344dd25b2dddb2ba343e19be7723f7/patches/helium/ui/side-panel.patch
// See docs/source-provenance.md for the licensed regions and source pins.

#include "chrome/browser/cmux_term/cmux_side_panel_resize_area.h"

#include "base/check.h"
#include "base/functional/bind.h"
#include "base/i18n/rtl.h"
#include "chrome/browser/ui/browser_element_identifiers.h"
#include "chrome/browser/ui/color/chrome_color_id.h"
#include "chrome/grit/generated_resources.h"
#include "ui/accessibility/mojom/ax_node_data.mojom.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/color/color_provider.h"
#include "ui/compositor/layer.h"
#include "ui/gfx/geometry/insets.h"
#include "ui/gfx/geometry/size.h"
#include "ui/views/accessibility/view_accessibility.h"
#include "ui/views/background.h"
#include "ui/views/controls/focus_ring.h"
#include "ui/views/layout/flex_layout.h"
#include "ui/views/view_class_properties.h"

namespace cmux {

CmuxSidePanelResizeHandle::CmuxSidePanelResizeHandle() {
  SetProperty(views::kElementIdentifierKey, kSidePanelResizeHandleElementId);
  SetVisible(false);
  SetPreferredSize(gfx::Size(2, 24));
  SetCanProcessEventsWithinSubtree(false);

  const int resize_handle_left_margin = 2;
  SetProperty(views::kMarginsKey,
              gfx::Insets().set_left(resize_handle_left_margin));
}

void CmuxSidePanelResizeHandle::OnThemeChanged() {
  views::ImageView::OnThemeChanged();

  const SkColor resize_handle_color =
      GetColorProvider()->GetColor(kColorSidePanelHoverResizeAreaHandle);
  SetBackground(views::CreateRoundedRectBackground(resize_handle_color, 2));
}

BEGIN_METADATA(CmuxSidePanelResizeHandle)
END_METADATA

CmuxSidePanelResizeArea::CmuxSidePanelResizeArea(
    CmuxSidePanelResizeDelegate* delegate)
    : views::ResizeArea(delegate), delegate_(delegate) {
  CHECK(delegate_);
  SetProperty(views::kElementIdentifierKey, kSidePanelResizeAreaElementId);
  SetPaintToLayer();
  layer()->SetFillsBoundsOpaquely(false);

  SetFocusBehavior(FocusBehavior::ALWAYS);

  auto* layout_manager =
      SetLayoutManager(std::make_unique<views::FlexLayout>());
  layout_manager->SetOrientation(views::LayoutOrientation::kVertical)
      .SetMainAxisAlignment(views::LayoutAlignment::kCenter)
      .SetCrossAxisAlignment(views::LayoutAlignment::kStart);

  GetViewAccessibility().SetRole(ax::mojom::Role::kSlider);
  GetViewAccessibility().SetName(
      l10n_util::GetStringUTF16(IDS_ACCNAME_SIDE_PANEL_RESIZE));

  resize_handle_ =
      AddChildView(std::make_unique<CmuxSidePanelResizeHandle>());

  views::FocusRing::Install(resize_handle_);
  views::FocusRing::Get(resize_handle_)
      ->SetHasFocusPredicate(base::BindRepeating([](const views::View* view) {
        return view->parent() && view->parent()->HasFocus();
      }));
}

void CmuxSidePanelResizeArea::OnMouseReleased(
    const ui::MouseEvent& event) {
  views::ResizeArea::OnMouseReleased(event);
  delegate_->RecordSidePanelResizeMetrics();
}

bool CmuxSidePanelResizeArea::OnKeyPressed(const ui::KeyEvent& event) {
  const int resize_increment = 50;
  if (event.key_code() == ui::VKEY_LEFT) {
    delegate_->OnResize(
        base::i18n::IsRTL() ? resize_increment : -resize_increment, true);
    delegate_->SetSidePanelKeyboardResized(true);
    return true;
  }
  if (event.key_code() == ui::VKEY_RIGHT) {
    delegate_->OnResize(
        base::i18n::IsRTL() ? -resize_increment : resize_increment, true);
    delegate_->SetSidePanelKeyboardResized(true);
    return true;
  }
  return false;
}

void CmuxSidePanelResizeArea::OnMouseMoved(const ui::MouseEvent& event) {
  UpdateHandleVisibility(true);
}

void CmuxSidePanelResizeArea::OnMouseEntered(const ui::MouseEvent& event) {
  UpdateHandleVisibility(true);
}

void CmuxSidePanelResizeArea::OnMouseExited(const ui::MouseEvent& event) {
  UpdateHandleVisibility(HasFocus());
}

void CmuxSidePanelResizeArea::OnFocus() {
  UpdateHandleVisibility(true);
  if (auto* focus_ring = views::FocusRing::Get(resize_handle_)) {
    focus_ring->SchedulePaint();
  }
  views::View::OnFocus();
}

void CmuxSidePanelResizeArea::OnBlur() {
  UpdateHandleVisibility(IsMouseHovered());
  if (auto* focus_ring = views::FocusRing::Get(resize_handle_)) {
    focus_ring->SchedulePaint();
  }
  views::View::OnBlur();
  delegate_->RecordSidePanelResizeMetrics();
  delegate_->SetSidePanelKeyboardResized(false);
}

void CmuxSidePanelResizeArea::Layout(PassKey) {
  LayoutSuperclass<views::ResizeArea>(this);
}

void CmuxSidePanelResizeArea::UpdateHandleVisibility(bool visible) {
  resize_handle_->SetVisible(visible);
}

BEGIN_METADATA(CmuxSidePanelResizeArea)
END_METADATA

}  // namespace cmux
