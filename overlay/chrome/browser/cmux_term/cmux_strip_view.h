// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef CHROME_BROWSER_CMUX_TERM_CMUX_STRIP_VIEW_H_
#define CHROME_BROWSER_CMUX_TERM_CMUX_STRIP_VIEW_H_

#include <map>
#include <memory>

#include "base/memory/raw_ptr.h"
#include "chrome/browser/cmux_term/window_model.h"
#include "third_party/skia/include/core/SkColor.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/views/view.h"

// Cross-platform niri scrollable strip. A pure views::View that renders ONE
// workspace's panes as a horizontal strip of fixed-width columns, using the
// pure cmux WindowModel + ComputeStripLayout (window_layout.h) for geometry —
// the SAME engine the real renderer (cmux_views.cc CmuxWindowView) drives, so
// the column geometry/scroll is identical by construction. It depends ONLY on
// //ui/views + the pure model, NOT on //chrome/browser, AppKit, IOSurface, or
// any platform surface, so it builds and looks identical on macOS / Linux /
// Windows.
//
// Pane CONTENT is supplied by a delegate (a StripPaneDelegate): the demo
// returns colored placeholder views; the real renderer can later return a
// native pane's view through the same seam. The strip owns the per-pane chrome
// (focus border) and the niri scroll/keep-focus-in-view behavior; it does not
// know or care what is inside a pane.

namespace cmux {

inline constexpr SkColor kDefaultStripViewBg = SkColorSetRGB(0x10, 0x12, 0x17);
inline constexpr SkColor kDefaultStripViewFocusBorder =
    SkColorSetRGB(0x4d, 0x9a, 0xff);
inline constexpr SkColor kDefaultStripViewIdleBorder =
    SkColorSetRGB(0x2a, 0x2e, 0x38);

struct StripViewThemeColors {
  SkColor bg = kDefaultStripViewBg;
  SkColor focus_border = kDefaultStripViewFocusBorder;
  SkColor idle_border = kDefaultStripViewIdleBorder;
};

// Supplies the content view for a pane. Called once per newly-seen PaneId.
class StripPaneDelegate {
 public:
  virtual ~StripPaneDelegate() = default;

  // Build the content for `pane` (id == pane.id). The demo returns a colored
  // placeholder; a renderer returns a native pane view. The strip wraps the
  // returned view in a focus-bordered container it owns.
  virtual std::unique_ptr<views::View> CreateStripPane(PaneId id,
                                                       const Pane& pane) = 0;
};

class CmuxStripView : public views::View {
  METADATA_HEADER(CmuxStripView, views::View)

 public:
  explicit CmuxStripView(StripPaneDelegate* delegate);
  CmuxStripView(const CmuxStripView&) = delete;
  CmuxStripView& operator=(const CmuxStripView&) = delete;
  ~CmuxStripView() override;

  // Render `workspace`'s panes as a strip, keeping `focused` scrolled into
  // view. Re-call after any model change. The workspace is resolved from
  // `model` at use-time so WindowModel vector reallocations cannot dangle us.
  void SetWorkspace(const WindowModel* model,
                    WorkspaceId workspace,
                    PaneId focused);
  void SetThemeColors(const StripViewThemeColors& colors);

  PaneId focused_pane() const { return focused_; }

  // views::View:
  void Layout(PassKey) override;
  bool OnMouseWheel(const ui::MouseWheelEvent& event) override;
  bool OnMousePressed(const ui::MouseEvent& event) override;
  void OnPaintBackground(gfx::Canvas* canvas) override;

 private:
  // Position the pane wrappers from ComputeStripLayout; persists clamped
  // scroll.
  void ApplyLayout();
  // Repaint focus borders to match focused_.
  void UpdateFocusBorders();
  const Workspace* workspace() const;

  raw_ptr<StripPaneDelegate> delegate_;
  raw_ptr<const WindowModel> model_ = nullptr;  // not owned
  WorkspaceId workspace_id_ = kInvalidId;
  PaneId focused_ = kInvalidId;
  double scroll_x_ = 0;
  StripViewThemeColors theme_colors_;

  // PaneId -> the focus-bordered wrapper view (a child of this strip) that
  // holds the delegate's pane content.
  std::map<PaneId, raw_ptr<views::View>> panes_;
};

}  // namespace cmux

#endif  // CHROME_BROWSER_CMUX_TERM_CMUX_STRIP_VIEW_H_
