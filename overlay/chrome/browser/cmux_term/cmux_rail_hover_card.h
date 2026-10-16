// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_CMUX_TERM_CMUX_RAIL_HOVER_CARD_H_
#define CHROME_BROWSER_CMUX_TERM_CMUX_RAIL_HOVER_CARD_H_

#include <memory>

#include "chrome/browser/cmux_term/window_model.h"

namespace views {
class View;
}  // namespace views

namespace cmux {

class RailDelegate;

// Workspace adaptation of Chromium's TabHoverCardController. The controller
// intentionally retains Chromium's delay, immediate-reshow, fade, slide,
// event-dismissal, target-observation, and bubble-ownership behavior. Only the
// target's data source changes from TabInterface to RailDelegate.
class CmuxRailHoverCardController {
 public:
  enum class UpdateType {
    kHover,
    kFocus,
    kEvent,
    kDataChanged,
    kAnimating,
  };

  CmuxRailHoverCardController(views::View* rail, RailDelegate* delegate);
  CmuxRailHoverCardController(const CmuxRailHoverCardController&) = delete;
  CmuxRailHoverCardController& operator=(
      const CmuxRailHoverCardController&) = delete;
  ~CmuxRailHoverCardController();

  void Update(views::View* target,
              WorkspaceId workspace,
              UpdateType update_type);
  void AddedToWidget();
  void RemovedFromWidget();

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace cmux

#endif  // CHROME_BROWSER_CMUX_TERM_CMUX_RAIL_HOVER_CARD_H_
