# Splits + per-pane tabs (bonsplit parity), cross-platform

Goal: mirror cmux/bonsplit inside the niri strip, platform-agnostically.

- The window is still a niri strip of **columns** per workspace.
- Each column is a **bonsplit tree**: panes split side-by-side (Cmd+D) or
  stacked (Cmd+Shift+D), nested arbitrarily, all *within* the column (the
  strip metaphor survives; columns keep their width).
- Each **pane** holds a stack of **tabs** (web page or terminal), one selected,
  rendered by the cross-platform CmuxTabStrip.

## Model (window_model.h) — owned by the model agent

`Workspace.layout` (single tree) becomes `std::vector<LayoutNode> columns`.

API (signatures frozen; the renderer is coded against these):

```cpp
// Workspace gains: std::vector<LayoutNode> columns;  (replaces layout)
// A workspace always has >= 1 column; a column always has >= 1 pane.

// New column at `index` (-1 = append) with one pane holding one tab of `kind`.
// Focuses the new pane. Returns its PaneId (kInvalidId on failure).
PaneId AddColumn(WorkspaceId ws, SurfaceKind kind, int index = -1);

// Unchanged signature: splits WITHIN the column containing `pane` (any depth).
// New pane's first tab is `new_tab_kind`. Focus moves to the new pane.
PaneId SplitPane(WorkspaceId ws, PaneId pane, SplitOrientation o, double ratio,
                 SurfaceKind new_tab_kind);

// Collapses the parent split into the sibling. If `pane` was its column's only
// pane, the column is removed. Refuses (no-op) if it is the workspace's only
// pane. Focus falls to the sibling that absorbed the space, else the nearest
// column's first pane.
void ClosePane(WorkspaceId ws, PaneId pane);

// Flat in-order: columns left->right, first-before-second within a column.
std::vector<PaneId> PanesOf(WorkspaceId ws) const;   // unchanged signature

int ColumnIndexOf(WorkspaceId ws, PaneId pane) const;  // -1 if absent
size_t ColumnCountOf(WorkspaceId ws) const;

enum class Direction { kLeft, kRight, kUp, kDown };
// kLeft/kRight: first pane (in-order) of the adjacent column.
// kUp/kDown: previous/next pane in-order WITHIN the same column.
// kInvalidId when there is no neighbor.
PaneId NeighborPane(WorkspaceId ws, PaneId pane, Direction d) const;
```

Tab APIs (AddTab/CloseTab/SelectTab/MoveTab) keep their semantics; CloseTab of
a pane's last tab still closes the pane (which may now remove a column).

## Layout (window_layout.h) — owned by the model agent

`ComputeStripLayout(ws, vw, vh, scroll_x, focus_pane, metrics)` (signature
unchanged): columns are fixed-width strips (same width + scroll-into-view math
as today, keyed by the column CONTAINING focus_pane, `gap` breathing room on
both edges); INSIDE each column the tree is tiled recursively (TileNode) with
`metrics.gap` between children. Every pane comes out as a PaneBox; in-order
matches PanesOf.

`ComputeTiledLayout` should be refactored to take a `const LayoutNode&` root
(used per-column internally, and by tests).

Host tests (window_model_test.cc, window_layout_test.cc) updated + green via
plain `c++ -std=c++17 -I overlay ...`. Also mechanically update the pure-views
users: cmux_strip_view.cc (iterates ws->layout today) and
ui/views/examples/cmux_demo_example.cc (builds a demo model).

## Renderer (cmux_views.cc + new cmux_pane_view) — owned by the main session

- New cross-platform `CmuxPaneView` (cmux_pane_view.{h,cc}): implements
  CmuxPane; children = CmuxTabStrip (top) + surface stack (fill). Maps
  SurfaceTabId -> CmuxSurface (below). Selecting a tab shows its surface and
  hides the rest (surfaces stay alive across switches).
- New `CmuxSurface` interface: the per-tab content (AsView/Focus/kind +
  web-only ops: omnibox, back/forward/reload, DevTools). CmuxBrowserPane
  becomes the web surface; the terminal panes become terminal surfaces.
- Surface creation behind a factory seam so CmuxPaneView stays platform- and
  chrome-free.
- FocusController: pane focus (borders, scroll, keyboard) + tab selection.

## Keyboard map (mac = Cmd via NSEvent monitor; non-mac = Ctrl accelerators)

- T: new web tab in the focused pane. Cmd+Ctrl+T on macOS (Ctrl+Alt+T
  elsewhere): new terminal tab. Shift+T: restore the most recently closed
  browser tab.
- D: split focused pane side-by-side.  Shift+D: split stacked.
  (New pane's tab mirrors the focused tab's kind — cmux behavior.)
- W: close focused tab (pane when last tab, column when last pane).
- Shift+[ / Shift+]: previous/next tab in the focused pane.
- Ctrl+H/L (mac Cmd+Ctrl) and Opt+Left/Right: focus column left/right.
- Ctrl+J/K (mac Cmd+Ctrl) and Opt+Up/Down: focus pane down/up within column.
- N: new web column.  L: focus omnibox.  R: reload.  [ / ]: history.

## Drag and drop

Tab drags use an in-window, threshold-gated Views drag, not the OS drag loop.
Dropping on a pane tab strip reorders/inserts by pill midpoints, including the
end slot. Dropping in pane content uses bonsplit zones: center appends the tab
to that pane; left/right/top/bottom edge bands split the target pane and put
the dragged tab in the new pane. Left/top drops insert the new pane first.
The content-only hit geometry matches Bonsplit: each edge is 25% of the
content dimension with an 80-point minimum, left/right win at corners, and
the boundary itself belongs to the next zone. The placeholder keeps Bonsplit's
4-point inset, 8-point corner radius, 2-point border, and 250 ms / 0.15-bounce
spring. Like Bonsplit, edge feedback is a stable half of the pane currently
under the pointer rather than a prediction of the post-drop tree. cmux applies
that placeholder formula to the whole pane instead of only its content so the
blue area also draws over the target's tab strip. On commit, the new pane
starts at the selected edge and the split expands to 50% with Bonsplit's
160 ms exponential divider curve.

Niri extensions: dropping in the strip gap before, between, or after columns
creates a new column at that index. Dragging from a pane strip's trailing empty
area drags the whole pane; center/tab-strip drops merge all its tabs into the
target pane, while edge/gap drops move the pane itself.

The renderer reparents existing surface views on tab moves. WebContents and
terminal NativeViewHost surfaces are moved between pane containers; they are
not destroyed/recreated for drag commits.

## Extensions

Every web tab's WebContents gets the full cmux tab-helper set (session tab id,
extensions::TabHelper, etc.) via one shared attach helper so content scripts +
blocking (uBlock) work on every tab. Extension action popups / installer UI
are researched separately (see docs/extensions-findings.md).
