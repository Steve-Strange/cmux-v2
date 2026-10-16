# Third-party notices

cmux Browser is a Chromium-derived application that also incorporates or
ships material from the projects below. The repository's root license applies
only to material for which Manaflow has the necessary rights; it does not
replace any third-party license or copyright notice.

This file records source-level provenance. By itself it is not a complete
binary notice bundle. A distributable application must generate target-specific
Chromium notices and the complete Ghostty, Rust, extension, font, shell, and
resource notice set described below.

## Chromium

- License: BSD-3-Clause
- Current Browser base: `782af9cb30a53f54487e5d2e44738645a8ec457c`
- Compatibility bases: `daf5ae401e05179ef94450c3f340df0dedc46dfe`
  (Chromium 149) and `81891e5ca708047763816c778216799ef14c66cb`
  (Chromium 150)
- License text: [`third_party/chromium/LICENSE`](third_party/chromium/LICENSE)

The overlay includes modified Chromium files and smaller adapted portions.
Those paths retain Chromium's copyright and BSD terms. Release builds must run
Chromium's license tooling against the exact shipped target and include its
generated credits/notices.

`patches/cmux-pinned-toolbar-actions-chromium-{149,150,151}.patch` adapts
Chromium's native pinned-action container and BrowserWindow feature
registration so cmux's pane-local toolbar can host the same model without a
BrowserView. The files remain BSD-3-Clause and are pinned to the compatibility
revisions listed above.

## Bonsplit

- Project: https://github.com/almonk/bonsplit
- License: MIT
- Reviewed source revision:
  `10563e2fda6fc18c47adf1864d55e0e25087a864`
- License text: [`third_party/bonsplit/LICENSE`](third_party/bonsplit/LICENSE)

`cmux_easing.{h,cc}` and its test adapt Bonsplit animation behavior.
`window_model.{h,cc}`, `window_layout.{h,cc}`, and `cmux_tab_drag.cc` contain
cross-language translations of Bonsplit model, layout, and drag behavior and
retain its MIT notice. Exact origin commits and regions are recorded in
[`docs/source-provenance.md`](docs/source-provenance.md).

## Ghostty

- Project: https://github.com/manaflow-ai/ghostty
- License: MIT for Ghostty itself; bundled dependencies and resources retain
  their own licenses
- Browser pin: `50ad1963d9c73ee957932ccb4d26bf6d15575ee7`
- Upstream base: `bb30526cdab8f5fb08ae43e404e3aacc40d3ffc3`
- License text: [`third_party/ghostty/LICENSE`](third_party/ghostty/LICENSE)

The current static archive and CLI include permissive dependencies,
MPL-covered z2d code, JetBrains Mono 2.304 under OFL-1.1, Nerd Fonts Symbols
Only 3.4.0 under MIT, and static GNU gettext/libintl 0.24 under
LGPL-2.1-or-later. Three Kitty-derived Bash/Zsh shell-integration files are
GPL-3.0-or-later. A release must ship their complete notices and corresponding
source. The current Darwin dependency graph links libintl unconditionally even
when Ghostty is built with `-Di18n=false`; that option disables catalogs but
does not prove the LGPL library is absent. Release checks must inspect the
actual archive. A proprietary build must additionally remove or provide a
compliant relinking path for static libintl. It must distribute the GPL shell
material as separate GPL components with corresponding source, or remove or
separately license that material. Modified or rebuilt OFL fonts must also
respect any applicable Reserved Font Name conditions.

The current 574-theme bundle comes from `mbadolato/iTerm2-Color-Schemes` tag
`release-20260629-161812-8c97c3c`, commit
`8c97c3c684da58a19149d1080f3d18ff5e161c01`. Its collection license explicitly
leaves each theme under its individual author's terms, while the converted
files contain no per-theme license metadata. Do not distribute the full theme
set until each selected theme has verified redistribution provenance.

## uBlock Origin

- Project: https://github.com/gorhill/uBlock
- License: GPL-3.0-only
- Version/tag: `1.72.2`
- Source commit: `3d370ea1603988bf678d7c5f621f161925fc36d9`
- Chrome Web Store CRX SHA-256:
  `6e02d8e6dce569eec721b531f9c7d28403e5426442417d5a855a17dd288b56f8`

The exact CRX digest and source commit must be enforced by packaging. Every
binary release must provide the extension's GPL notice and corresponding
source using a GPL-compliant distribution method appropriate to that release.
The CRX also contains separately licensed material, including the CC0 URLhaus
filter, Inter and Font Awesome fonts under OFL terms, Font Awesome CSS under
MIT (and any bundled Font Awesome documentation under CC BY 3.0), and
MIT-licensed CodeMirror, css-tree, HSLuv, and js-beautify code. Preserve the
exact embedded license files and include those items in the generated extension
notice inventory; the uBlock GPL notice does not replace their terms.

## Helium

Project: [imputnet/helium](https://github.com/imputnet/helium)

License: GNU General Public License version 3 only (GPL-3.0-only). Helium's
upstream README uses the legacy `GPL-3.0` identifier, which SPDX defines as
GPL-3.0-only. The “or later” text in the license's application example does not
change that project-level notice. A verbatim copy is in
[`third_party/helium/LICENSE`](third_party/helium/LICENSE).

The cmux tab strip, omnibar styling, workspace rail, Settings surface, New Tab
page, and toolbar customization contain modified adaptations of Helium
patches. Helium is copyright its contributors. cmux's workspace model differs
from Chromium tabs, so copied behavior and constants are marked as modified
where they are translated to workspaces.

Primary pinned source revisions:

- `8030a8a3050151a141c06cc5a85f95cbbdc42a25` (Helium `main` reviewed for
  this port, Chromium 150.0.7871.128, revision 7)
- `dee5600297344dd25b2dddb2ba343e19be7723f7` (Helium revision 8,
  Chromium 150.0.7871.181, reviewed for pane-local side-panel resize geometry)
- `3d042e6224222e965e967e3038ef8a10380c89e2` (Helium revision 6,
  Chromium 150 series)
- `c362740d53158863a8c253028679033addf085e2` (settings/WebUI patch base,
  Chromium 149.0.7827.53)
- `bee47818ed1f6a73790d46db74bf7324fee88799` (Helium merge commit,
  “merge: update to chromium 150.0.7871.128 (#2143)”)
- `81891e5ca708047763816c778216799ef14c66cb` (Chromium
  150.0.7871.128)
- `782af9cb30a53f54487e5d2e44738645a8ec457c` (Chromium
  151.0.7922.34)

Relevant Helium-authored history retained as provenance:

- `d868911b556c94e5a167cb43210625d884b99daf` — “helium/ui: initial vertical tabs layout,” wukko
  `<me@wukko.me>`
- `b9c270aa6be65d568de4394acf000cb30586e062` — “helium/ui/vertical:
  fix ntb height, tab insets and collapsed state,” wukko `<me@wukko.me>`
- `e93aece7f5f786c1eb8f6d0542f3d8f301dd26a1` — “helium/ui/vertical:
  fix tab group appearance, prevent line overlap,” wukko `<me@wukko.me>`
- `3de6ec1c457ffb1e293f4f41f4227b9ceff01625` — “helium/ui/layout/vertical:
  use same tween & duration in flyover,” wukko `<me@wukko.me>`
- `c99531d55d6157f11c5c3d82a62a585f62272f94` — “helium/core: add an
  option to copy URLs from tab context menu,” wukko `<me@wukko.me>`
- `dd2e11f3610046f158f4024a4c55a9e8556391fb` — “helium/core: add a
  context menu option to hibernate tabs,” wukko `<me@wukko.me>`
- `eb6711f4cedd4293331fb90bca64da7a48876203` — “helium/core/hibernate:
  add an option to hibernate other tabs,” wukko `<me@wukko.me>`
- `75b548a5bb78b803b53e473aabdc6ac06a36beae` — “helium/core: add ‘close
  tabs to left’ context menu action,” jj `<log@riseup.net>`
- `830b7bcc4b6816d7bfefa89a230ae2f79cded413` — “helium/ui: disable tab
  group editor footer,” wukko `<me@wukko.me>`
- `eba585e718c6d52f0d1ccf12eea2b541cbe8309b` — “helium/ui/vertical:
  fix new tab button alignment and icon size,” wukko `<me@wukko.me>`
- `f34f4e8bd28fb46d6dc688bf5d3f41943ae94d5c` — “helium/settings:
  remove about page easter egg,” wukko `<me@wukko.me>`
- `af5539abf7da5640a46d81d2dd6ba615d3615861` — “helium/ui/settings:
  clean up product info footer on about page,” wukko `<me@wukko.me>`
- `d2021bb3ef0bd47f55490e8a4c84acc09d851f38` — “helium/settings/about-page:
  remove learn more links,” wukko `<me@wukko.me>`
- `89ee3b3b1444e637234e2e6378cabad407a155a8` — “helium/settings: fix
  section separators,” wukko `<me@wukko.me>`
- `ec8480c3f530cb589dff30c3e3c62f695ad37872` — “helium/settings: add
  behavior section to the appearance page,” wukko `<me@wukko.me>`
- `f8269d25877daaad87e24c753bc3fcd1d899bbe6` — “helium/ui: helium color
  scheme in skia ui & webui,” wukko `<me@wukko.me>`
- `04e5e237a63144dde7d195a7930d40fb41b7a0a3` — “helium/ui/color-scheme:
  update with new brand color,” wukko `<me@wukko.me>`

Source mapping:

| cmux area | Helium source |
| --- | --- |
| Workspace rail metrics, nested group insets, group header/line geometry, colors and collapse icon, and whole-sidebar expand/collapse timing and easing | `patches/helium/ui/layout/vertical.patch`, including commit `3de6ec1c` |
| Workspace rail bottom action geometry, responsive label layout, shaped hit target, focus/ink-drop behavior, and frame-active colors | Helium `vertical_new_tab_button.{h,cc}` and `vertical_tab_strip_bottom_container.{h,cc}`, plus Helium's patch to Chromium `shared/tab_strip_flat_edge_button.{h,cc}`, at commit `8030a8a3`; adapted from new-tab dispatch to new-workspace dispatch |
| Horizontal tab and new-tab-button geometry | `patches/helium/ui/tabs.patch`, `patches/helium/ui/layout-constants.patch` |
| Omnibar geometry and styling | Helium UI/layout patches preserved in `patches/helium-omnibar-chromium-{149,151}.patch` |
| Default-on rounded terminal/browser content frame: preference key/default, 3-DIP toolbar-surface inset, attached top/side chrome edge treatment, 8-DIP ordinary corners, native-window-concentric bottom corners (9 DIPs before macOS 26, 14 DIPs on macOS 26, and 5 DIPs on other platforms), 1-DIP separator outline, and fullscreen suppression | Helium's `patches/helium/ui/rounded-frame-corners.patch`, `patches/helium/ui/multi-contents-view.patch`, `patches/helium/ui/frame-radius-helper.patch`, and `patches/helium/ui/fix-layout-separators.patch` at `bee47818`; the preference registration is preserved in `patches/helium-settings-chromium-{149,151}.patch`, and the custom-surface adaptation is in `cmux_surface.h`, `cmux_browser_pane.cc`, `cmux_terminal_pane.{h,mm}`, `cmux_terminal_pane_linux.cc`, and `cmux_views.cc` |
| Workspace context-menu ordering, Copy URL(s), Hibernate/Other, and Close Above entries | Helium's core context-menu patches at commits `c99531d5`, `dd2e11f3`, `eb6711f4`, and `75b548a5`, plus Chromium's `tab_menu_model.cc` as patched by Helium |
| Workspace-group header editor/context behavior | Chromium's vertical group editor as patched by Helium commit `830b7bcc` |
| Workspace Command/Shift selection, selected-set context targeting, selected-set drag behavior, and application-wide Escape drag cancellation | Chromium `vertical_tab_view.cc`, `vertical_tab_drag_handler.cc`, `dragging/tab_drag_controller.cc` (`EventTracker`), `tab_strip_model.cc`, and `tab_menu_model.cc` at tag `150.0.7871.128` (`81891e5ca708047763816c778216799ef14c66cb`), adapted from tab indices to stable workspace IDs |
| Vertical drag-start consolidation, dragged-block insertion thresholds, and edge auto-scroll | Chromium `vertical_dragged_tabs_container.cc` and `tab_drag_scroll_handler.{h,cc}` at tag `150.0.7871.128` (`81891e5ca708047763816c778216799ef14c66cb`), adapted to cmux's workspace collection hierarchy while retaining the rich 200 ms `EASE_IN_OUT` animation and 5-DIP/20-ms scroll cadence |
| Layout switching and tabs-on-right menu concepts | `patches/helium/ui/layout/context-menu.patch` |
| Settings navigation order, icons, compact controls, flat cards, about-page cleanup, section separators, and WebUI color palette | Helium's `patches/helium/settings/{about-page-tweaks,fix-section-separators,reorder-settings-menu,settings-page-icons}.patch`, `patches/helium/ui/update-cr-components.patch`, and the WebUI portion of `patches/helium/ui/helium-color-scheme.patch`, rebased and modified in `patches/helium-settings-chromium-{149,151}.patch` |
| Shortcut-first local New Tab page, rounded high-resolution shortcut tiles, and Customize side-panel availability | Helium's `patches/helium/ui/{always-use-better-ntp,clean-new-tab-page,fix-customize-side-panel,restyle-ntp-tiles,square-ntp-monograms}.patch`, rebased and modified in `patches/helium-new-tab.patch` |
| User-hideable native global-media toolbar control | Helium's `patches/helium/ui/toolbar-button-prefs.patch`, with a cmux-specific Browser constructor seam, in `patches/helium-media-toolbar.patch` |
| Pane-local side-panel resize geometry | Helium's `patches/helium/ui/side-panel.patch` at `dee5600297344dd25b2dddb2ba343e19be7723f7`, adapted with Chromium's `side_panel_resize_area.{h,cc}` in `cmux_side_panel_resize_area.{h,cc}` |

The main cmux adaptation points are
`overlay/chrome/browser/cmux_term/cmux_rail.cc`,
`overlay/chrome/browser/cmux_term/cmux_rail_config.h`,
`overlay/chrome/browser/cmux_term/cmux_tab_strip.cc`, and
`patches/helium-omnibar-chromium-{149,151}.patch`. Rounded-frame adaptation
also lives in the surface files listed in the source-mapping table above. The
settings adaptation points are
`patches/helium-settings-chromium-{149,151}.patch`; their mail-style
headers carry the complete upstream source list, authors, revisions, cmux
modification date, and GPL-3.0-only notice alongside each diff. The New Tab and
media-toolbar adaptation points are `patches/helium-new-tab.patch` and
`patches/helium-media-toolbar.patch`. Chromium 151 is the current cmux target;
the 149 and 150 variants remain compatibility fallbacks only.

## Chromium vertical-tab collection animation

The workspace collection animation is copied from Chromium's
`chrome/browser/ui/views/tabs/vertical/tab_collection_animating_layout_manager.{h,cc}`
at tag `150.0.7871.128` (`81891e5ca708047763816c778216799ef14c66cb`),
mechanically renamed to
`CmuxRailAnimatingLayoutManager`, and adapted with the nested structure from
`vertical_tab_group_view.{h,cc}` and
`vertical_unpinned_tab_container_view.{h,cc}`. The copied files retain
Chromium's BSD-style license and copyright notice. Helium uses and patches
that Chromium implementation; the animation machinery itself is attributed
to Chromium, while Helium-specific dimensions and behavior changes are
attributed above.

The workspace rail bottom action's browser-neutral ink-drop setup copies the
final mode, opacity, layer, base-color, flood-fill ripple, and hover-highlight
callback behavior from Chromium's
`chrome/browser/ui/views/toolbar/toolbar_ink_drop_util.{h,cc}` at tag
`151.0.7922.34` (`782af9cb30a53f54487e5d2e44738645a8ec457c`). The
in-product-help promo-color override is omitted because cmux's workspace
button is not registered as a user-education anchor. Its hover, ripple, and
inactive-frame colors follow Helium's patch to
`shared/tab_strip_flat_edge_button.cc` at
`8030a8a3050151a141c06cc5a85f95cbbdc42a25`.

The workspace rail's layer-backed scrolling and interactive rounded overlay
scrollbar adapt Chromium 150's
`vertical_tab_strip_view.cc`,
`vertical_tab_strip_scroll_bar.{h,cc}`, and
`shared/rounded_scroll_bar.{h,cc}` at the same pinned Chromium revision. The
workspace row's hover timing, close-button interaction, focus path, and input
ordering adapt `vertical_tab_view.{h,cc}`,
`tab/glow_hover_controller.{h,cc}`, and `tab/tab_close_button.{h,cc}` at that
revision. Names and model callbacks are modified only at cmux's workspace
boundary.

The workspace hover-card adapter in `cmux_rail_hover_card.{h,cc}` copies the
target observation, delayed-show calculation, 300 ms immediate-reshow window,
event dismissal, non-activating bubble ownership, 200 ms anchor slide/text
crossfade, widget fade behavior, two-layer preview-image crossfade, thumbnail
capture-readiness delay, and cancellable subscription lifetime from Chromium
150's
`tabs/hovercard/tab_hover_card_controller.{h,cc}`,
`tab_hover_card_bubble_view.{h,cc}`,
`tab_hover_card_thumbnail_observer.{h,cc}`, and `fade_view.h`. The tab data
provider is replaced by a browser-neutral workspace title/domain/preview
boundary so the same presentation code can remain in cmux's cross-platform
Views target; the browser delegate bridges that boundary to Chromium's
`ThumbnailImage` and `ThumbnailTabHelper`. Helium enables Chromium's hover-card
implementation and preview-image feature in the pinned
`8030a8a3050151a141c06cc5a85f95cbbdc42a25` snapshot; that enabling and the
surrounding Helium vertical-tab integration remain attributed to the Helium
contributors under GPL-3.0-only as described above.

Compatibility guards retain Chromium 149's `AnchorView`, parameterless widget
fade, and legacy preview-icon APIs; Chromium 150 and later use the copied
`BubbleAnchor`, directional-fade, `CancelSlide`, and rounded-icon APIs.

The workspace-group editor in
`cmux_rail_group_editor_bubble.{h,cc}` adapts Chromium 150's
`tabs/groups/tab_group_editor_bubble_view.{h,cc}` and
`tabs/groups/color_picker_view.{h,cc}`, plus the accelerator presentation from
`bubble_menu_item_factory.{h,cc}` and the interaction behavior from
`controls/hover_button.{h,cc}` and
`controls/hover_button_controller.{h,cc}`, at that revision. Its tab actions
are translated to workspace-group actions, while the 240-DIP bubble, control
metrics, refreshed nine-color ordering, focus behavior, release-triggered
mouse and keyboard handling, right-click activation, accelerator layout, and
native bubble animation remain Chromium-derived. The saved-group footer is
deliberately absent, matching Helium commit
`830b7bcc4b6816d7bfefa89a230ae2f79cded413` by wukko `<me@wukko.me>`.

The workspace Gemini context submenu in `cmux_views.cc` adapts Chromium 150's
`chrome/browser/ui/tabs/glic_tab_sub_menu_model.{h,cc}` at
`81891e5ca708047763816c778216799ef14c66cb`. It retains Chromium's recent
conversation commands, labels, metrics, and live weak-tab validation while
replacing the single-`TabStripModel` selection lookup with cmux's equivalent
set of selected workspace Browser graphs.
