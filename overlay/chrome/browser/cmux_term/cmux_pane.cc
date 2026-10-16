// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "chrome/browser/cmux_term/cmux_pane.h"

namespace cmux {

CmuxPane::~CmuxPane() = default;

void CmuxPane::FocusOmnibar() {}

void CmuxPane::SetActivationCallback(base::RepeatingClosure callback) {}

std::u16string CmuxPane::E2EOmniboxText() {
  return std::u16string();
}

void CmuxPane::CloseOmniboxPopupUnlessFocused(views::View* focused) {}

content::WebContents* CmuxPane::GetInspectableWebContents() {
  return nullptr;
}

void CmuxPane::ToggleDevTools() {}

void CmuxPane::UndockDevTools() {}

bool CmuxPane::HandleOmniboxEscape() {
  return false;
}

void CmuxPane::GoBack() {}
void CmuxPane::GoForward() {}
void CmuxPane::Reload() {}

}  // namespace cmux
