# Browser source provenance

This inventory classifies the Browser source at private candidate
`b3ea02c2c27ab517bbb5beb6a58c87035bc4325c`. It is a publication gate, not a
claim that storing compatible code together changes any file's license.

## Reviewed source revisions

- Chromium 149: `daf5ae401e05179ef94450c3f340df0dedc46dfe`
- Chromium 150: `81891e5ca708047763816c778216799ef14c66cb`
- Chromium 151: `782af9cb30a53f54487e5d2e44738645a8ec457c`
- Helium: `3d042e6224222e965e967e3038ef8a10380c89e2`,
  `bee47818ed1f6a73790d46db74bf7324fee88799`, and
  `8030a8a3050151a141c06cc5a85f95cbbdc42a25`
- Bonsplit: `10563e2fda6fc18c47adf1864d55e0e25087a864`; drop-zone
  behavior rechecked against `77b9ccebf1c6e6533c3df1030b5efa9a3db2f351`

## Classification

The audited set contains 152 overlay files and six Helium patches.

| Class | Count | Treatment |
| --- | ---: | --- |
| Manaflow rights-controlled only | 129 | Manaflow copyright; GPL-3.0-or-later |
| Chromium-derived only | 4 | Retain Chromium BSD-3-Clause notice |
| Helium-derived patch | 6 | GPL-3.0-only and exact Helium provenance |
| Bonsplit-derived only | 3 | Retain Bonsplit MIT notice; identify Manaflow modifications |
| Manaflow and Chromium | 1 | `GPL-3.0-or-later AND BSD-3-Clause` |
| Manaflow and Bonsplit | 1 | `GPL-3.0-or-later AND MIT` |
| Manaflow, Bonsplit, and Chromium | 4 | `GPL-3.0-or-later AND MIT AND BSD-3-Clause` |
| Manaflow and Helium | 4 | `GPL-3.0-or-later AND GPL-3.0-only` |
| Manaflow, Helium, and Chromium | 6 | `GPL-3.0-or-later AND GPL-3.0-only AND BSD-3-Clause` |

The first 119 Manaflow-only files are:

- all `overlay/chrome/services/cmux_terminal_renderer/**` files;
- both `overlay/ui/views/examples/**` files; and
- the 92 `overlay/chrome/browser/cmux_term/**` files outside the original
  22-file manual-review set and the explicit third-party-only sets below.

The region-level review resolved ten additional Manaflow-only files. Their
compatibility values, colors, standard formulas, and local tests do not copy
upstream expression:

- `cmux_layout_config.h`
- `cmux_rail_config.h`
- `cmux_sidebar_metrics_standalone.cc`
- `cmux_tab_drag.h`
- `cmux_theme.{h,cc}` and `cmux_theme_test.cc`
- `window_layout_test.cc`
- `window_model_test.cc`
- `cmux_chrome_surface_colors.h`

Chromium-derived BSD files:

- `overlay/chrome/browser/cmux_term/cmux_browser_window.cc`
- `overlay/chrome/browser/cmux_term/cmux_browser_window.h`
- `overlay/chrome/browser/cmux_term/cmux_extensions_container.cc`
- `overlay/chrome/browser/cmux_term/cmux_extensions_container.h`

Helium-derived GPL patches:

- `patches/helium-omnibar-chromium-149.patch`
- `patches/helium-omnibar-chromium-151.patch`
- `patches/helium-settings-chromium-149.patch`
- `patches/helium-settings-chromium-151.patch`
- `patches/helium-new-tab.patch`
- `patches/helium-media-toolbar.patch`

Bonsplit-derived MIT files:

- `overlay/chrome/browser/cmux_term/cmux_easing.cc`
- `overlay/chrome/browser/cmux_term/cmux_easing.h`
- `overlay/chrome/browser/cmux_term/cmux_easing_test.cc`

Manaflow and Chromium mixed files:

- `overlay/chrome/browser/cmux_term/cmux_tab_strip.h`

Manaflow and Bonsplit mixed file:

- `overlay/chrome/browser/cmux_term/window_model.cc`

Manaflow, Bonsplit, and Chromium mixed files:

- `overlay/chrome/browser/cmux_term/cmux_tab_drag.cc`
- `overlay/chrome/browser/cmux_term/window_layout.cc`
- `overlay/chrome/browser/cmux_term/window_layout.h`
- `overlay/chrome/browser/cmux_term/window_model.h`

Manaflow and Helium mixed files:

- `overlay/chrome/browser/cmux_term/cmux_surface.h`
- `overlay/chrome/browser/cmux_term/cmux_terminal_pane.h`
- `overlay/chrome/browser/cmux_term/cmux_terminal_pane.mm`
- `overlay/chrome/browser/cmux_term/cmux_terminal_pane_linux.cc`

Manaflow, Helium, and Chromium mixed files:

- `overlay/chrome/browser/cmux_term/cmux_browser_pane.cc`
- `overlay/chrome/browser/cmux_term/cmux_rail.cc`
- `overlay/chrome/browser/cmux_term/cmux_sidebar_metrics.h`
- `overlay/chrome/browser/cmux_term/cmux_tab_strip.cc`
- `overlay/chrome/browser/cmux_term/cmux_views.cc`
- `overlay/chrome/browser/ui/color/chrome_color_mixers.cc`

The Helium-derived and Helium-mixed files make GPL-3.0-only the compatible
license for the current combined browser work. Independently authored cmux
material remains available under GPL-3.0-or-later. The Helium-mixed files are
also blockers for a future commercial composition: they require separate
permission or an independent rewrite, and Manaflow's commercial offer cannot
cover the Helium-derived regions.

Two pane-local side-panel resize files were added after the frozen candidate
classified above. `cmux_side_panel_resize_area.{h,cc}` combine Manaflow's
GPL-3.0-or-later adaptation, Helium's GPL-3.0-only resize geometry at
`dee5600297344dd25b2dddb2ba343e19be7723f7`, and Chromium's BSD-3-Clause
implementation. Their file headers record all three grants; they do not change
the historical counts in the frozen-candidate table.

## Region evidence

- `cmux_browser_window.*` derives closely from Chromium's
  `chrome/browser/ui/webui_browser/webui_browser_window.*`.
- `cmux_extensions_container.*` derives from Chromium's
  `chrome/browser/ui/webui/webui_toolbar/webui_toolbar_extensions_container.*`.
- `cmux_easing.*` ports Bonsplit easing; relevant origin commits include
  `0523729`, `76fc5744`, and `5dc3c82e`.

The mixed-file line references below refer to the audited `d4f47b16` source
body before provenance headers were expanded. The changes through the frozen
`b3ea02c2` candidate affect only the Manaflow-only `cmux_ghostty.mm` body and do
not change those mixed-file references or classifications.

- `cmux_browser_pane.cc` lines 119-137 and 844-888 retain Chromium's
  `StubBubbleModelDelegate` architecture; its constructor adapts
  `chrome/browser/ui/views/simple_web_view_dialog.cc` (2014).
- `cmux_surface.h`, `cmux_browser_pane.cc`, `cmux_terminal_pane.{h,mm}`,
  `cmux_terminal_pane_linux.cc`, and `cmux_views.cc` adapt Helium's default-on
  rounded-frame preference and its 3-DIP inset, attached-chrome edge treatment,
  8-DIP generic radius, native-window-concentric bottom radii, 1-DIP separator
  outline, and fullscreen suppression from `rounded-frame-corners.patch`,
  `multi-contents-view.patch`, `frame-radius-helper.patch`, and
  `fix-layout-separators.patch` at `bee47818`.
- `cmux_tab_strip.h` lines 123-126 and 200-280 structurally adapt Chromium
  `TabContainerImpl` live-reorder, ideal-bounds, animation-observer, and
  closing-view state.
- `cmux_views.cc` lines 687-714 adapt Chromium `bounds_animator.cc`
  `CreateAnimation`; lines 7203-7212 adapt
  `PresentationReceiverWindowFrame::GetThemeProvider`.
- `window_model.cc` translates Bonsplit `PaneState.swift`, `SplitNode.swift`,
  `SplitState.swift`, and `SplitViewController.swift`, including tree helpers,
  split/close/ratio/focus behavior, and tab operations.
- `cmux_tab_drag.cc` adapts Bonsplit `TabDragPreview.swift` and
  `PaneContainerView.swift`, including the current placeholder geometry and
  spring (with a cmux extension across the target tab strip); its translucent
  drag widget also adapts Chromium
  `ui/views/button_drag_utils.cc` (2012).
- `window_layout.{h,cc}` translates Bonsplit split bounds, neighbor selection,
  and drop-zone behavior. Its detached-tab insertion candidate and distance
  logic adapts Chromium's pinned `tab_strip.cc` implementation.
- `window_model.h` structurally adapts Bonsplit's split/tab model and retains
  Chromium `tab_group_color.h` names and ordering (2019).
- `cmux_rail.cc` adapts Helium's GPL-added `VerticalNewTabButton` and Chromium
  `TabContainerImpl` insertion/reflow animation.
- `cmux_sidebar_metrics.h` follows Helium vertical-strip/footer metrics and
  snap behavior while adapting Chromium close-button visibility policy.
- `cmux_tab_strip.cc` adapts Helium layout/control/color behavior and Chromium
  close-icon coordinates, tab behavior, remove delegates, bounds/insertion
  animations, live-reorder hysteresis, and width solving.
- `chrome_color_mixers.cc` retains Chromium's 2021 mixer file; the cmux surface
  graph translates Helium's `helium-color-mixers.patch`.

The original generic `Copyright The Chromium Authors` headers were not
classification evidence. The publication-prep change replaces them with
Manaflow-only or combined notices while retaining genuine Chromium, Bonsplit,
and Helium rights.

The standalone Helium patch files may carry comment-prefixed provenance
before their first `diff --git` line. This form remains parseable by
`git apply`; publication checks must verify both patch parsing and application
against the pinned Chromium bases.
