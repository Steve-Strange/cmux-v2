// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef CHROME_BROWSER_CMUX_TERM_CMUX_CONFIGURE_PAGE_H_
#define CHROME_BROWSER_CMUX_TERM_CMUX_CONFIGURE_PAGE_H_

#include <string>
#include <vector>

#include "base/functional/callback_forward.h"
#include "chrome/browser/cmux_term/cmux_theme_ghostty.h"
#include "content/public/browser/web_ui_controller.h"

namespace content {
class WebUI;
}

namespace cmux {

inline constexpr char kCmuxConfigureChromeHost[] = "cmux-configure";

// Native controller for cmux://settings. cmux://configure remains a backwards-
// compatible alias. The visible cmux URL is rewritten to the private
// chrome://cmux-configure WebUI host at navigation-entry creation.
class CmuxConfigureUI : public content::WebUIController {
 public:
  explicit CmuxConfigureUI(content::WebUI* web_ui);
  CmuxConfigureUI(const CmuxConfigureUI&) = delete;
  CmuxConfigureUI& operator=(const CmuxConfigureUI&) = delete;
  ~CmuxConfigureUI() override;

  WEB_UI_CONTROLLER_TYPE_DECL();
};

// Registers the private WebUI host. Safe to call more than once, and called
// before profile initialization on every supported desktop platform.
void RegisterCmuxConfigureWebUI();

// Loads the complete pinned Ghostty theme catalog away from the UI thread,
// together with the currently published cmux selection.
using CmuxThemePickerStateCallback =
    base::OnceCallback<void(std::vector<GhosttyThemePreview>, std::string)>;
void LoadCmuxThemePickerState(CmuxThemePickerStateCallback callback);

// Persists a browser-owned theme picker selection and refreshes every cmux
// theme consumer. An empty name follows the user's Ghostty configuration.
bool ApplyCmuxGhosttyTheme(const std::string& theme);

}  // namespace cmux

#endif  // CHROME_BROWSER_CMUX_TERM_CMUX_CONFIGURE_PAGE_H_
