// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "chrome/browser/cmux_term/cmux_surface.h"

namespace cmux {

CmuxSurface::~CmuxSurface() = default;

void CmuxSurface::ActivateToolbarHost() {}

void CmuxSurface::SetActivationCallback(base::RepeatingClosure callback) {}

void CmuxSurface::SetInteractionCallback(base::RepeatingClosure callback) {}

void CmuxSurface::SetTitleChangedCallback(
    base::RepeatingCallback<void(const std::u16string&)> callback) {}

void CmuxSurface::SetFaviconChangedCallback(
    base::RepeatingCallback<void(const gfx::ImageSkia&)> callback) {}

void CmuxSurface::SetLoadingChangedCallback(
    base::RepeatingCallback<void(bool)> callback) {}

void CmuxSurface::SetCloseRequestedCallback(base::RepeatingClosure callback) {}

void CmuxSurface::FireCloseRequestedForTesting() {}

bool CmuxSurface::ShowPageInfoForTesting() {
  return false;
}

void CmuxSurface::SetRoundedFrame(const RoundedFrameGeometry& geometry) {}

void CmuxSurface::FocusOmnibar() {}
void CmuxSurface::GoBack() {}
void CmuxSurface::GoForward() {}
void CmuxSurface::Reload() {}

bool CmuxSurface::HandleOmniboxEscape() {
  return false;
}

void CmuxSurface::CloseOmniboxPopupUnlessFocused(views::View* focused) {}

void CmuxSurface::ToggleDevTools() {}
void CmuxSurface::UndockDevTools() {}

content::WebContents* CmuxSurface::GetInspectableWebContents() {
  return nullptr;
}

LocationBar* CmuxSurface::GetLocationBar() {
  return nullptr;
}

web_modal::WebContentsModalDialogHost*
CmuxSurface::GetWebContentsModalDialogHost() {
  return nullptr;
}

std::u16string CmuxSurface::E2EOmniboxText() {
  return std::u16string();
}

}  // namespace cmux
