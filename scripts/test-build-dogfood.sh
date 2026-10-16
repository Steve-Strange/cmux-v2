#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
script="$root/scripts/build-dogfood.sh"

grep -Fq 'CMUX_DEPOT_TOOLS="${CMUX_DEPOT_TOOLS:-$REMOTE_ROOT/depot_tools}"' "$script"

clean_line="$(grep -n 'git restore --worktree -- .' "$script" | cut -d: -f1)"
sync_line="$(grep -n '"$ROOT/scripts/sync.sh"' "$script" | cut -d: -f1)"

test -n "$clean_line"
test -n "$sync_line"
test "$clean_line" -lt "$sync_line"
grep -Fq 'rm -rf "$tmp/.cmux-build-lock"' "$script"

# Tagged builds must be independently launchable. Compile a distinct bundle ID
# and use CrProductDirName for ProcessSingleton/profile isolation; a forwarding
# executable would make macOS attribute Local Network access to the wrong name.
grep -Fq 'DOGFOOD_BUNDLE_ID="${CMUX_DOGFOOD_BUNDLE_ID:-com.cmux.app.dogfood.build-$BUNDLE_TAG}"' "$script"
grep -Fq 'DOGFOOD_PRODUCT_DIR="${CMUX_DOGFOOD_PRODUCT_DIR_NAME:-cmux-browser-dogfood-$TAG}"' "$script"
grep -Fq 'export CMUX_MAC_BUNDLE_ID="$DOGFOOD_BUNDLE_ID"' "$script"
grep -Fq 'CMUX_EXPECTED_BUNDLE_ID="$DOGFOOD_BUNDLE_ID"' "$script"
grep -Fq 'CMUX_PRODUCT_DIR_NAME="$DOGFOOD_PRODUCT_DIR"' "$script"
if grep -Fq 'CMUX_PROFILE=' "$script" ||
    grep -Fq 'CMUX_BUNDLE_ID=' "$script"; then
  echo "dogfood builds must not use deploy-time identity or a profile launcher" >&2
  exit 1
fi

# Every build must suppress Chromium's bundled field-trial testing config so
# normal Google requests do not carry Chrome experiment IDs from cmux.
build_script="$root/scripts/build.sh"
args_script="$root/scripts/set-alloc-args.sh"
apply_script="$root/scripts/apply.sh"
grep -Fq '"$CMUX_SCRIPTS_DIR/set-alloc-args.sh"' "$build_script"
grep -Fq 'disable_fieldtrial_testing_config = true' "$args_script"

# Exercise the real remote rewrite through a local fake transport. Stale,
# malformed, duplicate, commented, and partial matches must not weaken any of
# the four required GN product arguments.
fieldtrial_test_dir="$(mktemp -d "${TMPDIR:-/tmp}/cmux-fieldtrial-args-test.XXXXXX")"
trap 'rm -rf "$fieldtrial_test_dir"' EXIT
mkdir -p "$fieldtrial_test_dir/src/out/Release"
printf '%s\n' \
  'is_debug = false' \
  '# use_partition_alloc_as_malloc = false' \
  'not_use_partition_alloc_as_malloc = false' \
  'use_partition_alloc_as_malloc = true' \
  '  use_partition_alloc_as_malloc = "invalid"' \
  '# use_allocator_shim = false' \
  'use_allocator_shim_extra = false' \
  'use_allocator_shim = true' \
  ' use_allocator_shim = "invalid"' \
  '# enable_backup_ref_ptr_support = false' \
  'enable_backup_ref_ptr_support_extra = false' \
  'enable_backup_ref_ptr_support = true' \
  ' enable_backup_ref_ptr_support = "invalid"' \
  '# disable_fieldtrial_testing_config = true' \
  'disable_fieldtrial_testing_config_extra = true' \
  'disable_fieldtrial_testing_config = false' \
  '  disable_fieldtrial_testing_config = "invalid"' \
  >"$fieldtrial_test_dir/src/out/Release/args.gn"
printf '%s\n' \
  '#!/usr/bin/env bash' \
  'set -euo pipefail' \
  '/bin/bash -c "${!#}"' \
  >"$fieldtrial_test_dir/fake-tailscale-ssh"
chmod +x "$fieldtrial_test_dir/fake-tailscale-ssh"
CMUX_TAILSCALE_SSH="$fieldtrial_test_dir/fake-tailscale-ssh" \
  CMUX_BUILDER='test-builder' \
  CHROMIUM_SRC="$fieldtrial_test_dir/src" \
  "$args_script" >/dev/null
args_fixture="$fieldtrial_test_dir/src/out/Release/args.gn"
for required_arg in \
  'use_partition_alloc_as_malloc = false' \
  'use_allocator_shim = false' \
  'enable_backup_ref_ptr_support = false' \
  'disable_fieldtrial_testing_config = true'; do
  grep -Fxq "$required_arg" "$args_fixture"
done
for required_key in \
  use_partition_alloc_as_malloc \
  use_allocator_shim \
  enable_backup_ref_ptr_support \
  disable_fieldtrial_testing_config; do
  test "$(grep -Ec "^[[:space:]]*$required_key[[:space:]]*=" "$args_fixture")" -eq 1
done
rm -rf "$fieldtrial_test_dir"
trap - EXIT

# Both platform patchers retain Chromium's native UA client hints and suppress
# X-Client-Data / future variations seed fetches.
grep -Fq 'removed legacy Google Chrome spoof' "$apply_script"
grep -Fq 'cmux: this is not a Google Chrome release channel' "$apply_script"
grep -Fq 'cmux: do not enroll this unofficial product' "$apply_script"
grep -Fq 'google-search-template: plain' "$apply_script"
grep -Fq 'https://www.google.com/search?q={searchTerms}' "$apply_script"
grep -Fq 'cmux: a pane-local LocationBarView has no BrowserView-owned zoom' "$apply_script"
grep -Fq 'CHROMIUM_MAJOR="$(ssh -o BatchMode=yes "$HOST"' "$apply_script"
grep -Fq 'if [ "$CHROMIUM_MAJOR" -ge 151 ]; then' "$apply_script"
grep -Fq 'HELIUM_SETTINGS_PATCH=".cmux-patches/helium-settings-chromium-151.patch"' "$apply_script"
grep -Fq 'HELIUM_SETTINGS_PATCH=".cmux-patches/helium-settings-chromium-149.patch"' "$apply_script"
grep -Fq 'helium-settings: patched (GPL-3.0-only; see THIRD_PARTY_NOTICES.md)' "$apply_script"
windows_apply="$root/scripts/apply_win_chrome.py"
grep -Fq 'removed legacy Google Chrome spoof' "$windows_apply"
grep -Fq 'cmux: this is not a Google Chrome release channel' "$windows_apply"
grep -Fq 'cmux: do not enroll this unofficial product' "$windows_apply"
grep -Fq 'google-search-template: plain' "$windows_apply"
grep -Fq 'https://www.google.com/search?q={searchTerms}' "$windows_apply"
grep -Fq 'cmux: a custom BrowserWindow has no BrowserView-owned zoom' "$windows_apply"
grep -Fq 'if chromium_major >= 151:' "$windows_apply"
grep -Fq 'patch_dir / "helium-settings-chromium-149.patch"' "$windows_apply"
grep -Fq 'patch_dir / "helium-settings-chromium-151.patch"' "$windows_apply"
grep -Fq 'git_apply_check("--reverse", "--check")' "$windows_apply"
grep -Fq "'cmux_term/cmux_rail_group_editor_bubble.h'" "$windows_apply"
grep -Fq "'cmux_term/cmux_rail_group_editor_bubble.cc'" "$windows_apply"
grep -Fq "'cmux_term/cmux_rail_hover_card.h'" "$windows_apply"
grep -Fq "'cmux_term/cmux_rail_hover_card.cc'" "$windows_apply"

# The Windows patcher must recognize the chrome/browser source-block layout
# used by Chromium 149/150 as well as Chromium 151's caption-settings move.
# Execute the patcher's selector itself without running its top-level Windows
# mutations, and pin both source shapes plus its fail-closed behavior.
python3 - "$windows_apply" <<'PY'
import ast
import sys

script_path = sys.argv[1]
source = open(script_path, encoding='utf-8').read()
tree = ast.parse(source, filename=script_path)
selector_node = next(
    node for node in tree.body
    if isinstance(node, ast.FunctionDef) and
    node.name == 'find_mac_app_sources_anchor'
)
namespace = {}
exec(ast.get_source_segment(source, selector_node), namespace)
find_anchor = namespace['find_mac_app_sources_anchor']

caption_anchor = (
    '  if (is_mac) {\n    sources += [\n'
    '      "accessibility/caption_settings_dialog.h",'
)
controller_anchor = (
    '  if (is_mac) {\n    sources += [\n'
    '      "app_controller_mac.mm",'
)
for chromium_major in (149, 150):
    fixture = (
        f'# Chromium {chromium_major}\n'
        + caption_anchor
        + '\n      "app_controller_mac.mm",\n'
    )
    assert find_anchor(fixture) == caption_anchor

chromium_151_fixture = (
    '# Chromium 151\n'
    + controller_anchor
    + '\n      "browser_finder_mac.mm",\n'
)
assert find_anchor(chromium_151_fixture) == controller_anchor

try:
    find_anchor('  if (is_mac) {\n    sources += [\n      "unknown.mm",')
except AssertionError as error:
    assert 'not found or ambiguous' in str(error)
else:
    raise AssertionError('unknown Chromium layout must fail closed')
PY

# The fast Views demo sync must include every rail companion source, not just
# the rail itself. It must also refresh the authoritative target manifest and
# patch inputs, run the existing Views-only patch path, and never mask a failed
# compile or startup behind stale output.
uidemo_script="$root/scripts/uidemo.sh"
grep -Fq 'set -euo pipefail' "$uidemo_script"
grep -Fq 'RSYNC_ARGS=(-az --ignore-times --no-times)' "$uidemo_script"
grep -Fq 'overlay/chrome/browser/cmux_term/BUILD.gn' "$uidemo_script"
grep -Fq 'overlay/chrome/browser/cmux_term/cmux_rail_animating_layout_manager.h' "$uidemo_script"
grep -Fq 'overlay/chrome/browser/cmux_term/cmux_rail_animating_layout_manager.cc' "$uidemo_script"
grep -Fq 'overlay/chrome/browser/cmux_term/cmux_rail_group_editor_bubble.h' "$uidemo_script"
grep -Fq 'overlay/chrome/browser/cmux_term/cmux_rail_group_editor_bubble.cc' "$uidemo_script"
grep -Fq 'overlay/chrome/browser/cmux_term/cmux_rail_hover_card.h' "$uidemo_script"
grep -Fq 'overlay/chrome/browser/cmux_term/cmux_rail_hover_card.cc' "$uidemo_script"
grep -Fq -- '--delete patches/ "$HOST:$SRC/.cmux-patches/"' "$uidemo_script"
uidemo_reverse_line="$(grep -n \
  "git apply --reverse '\$PREVIOUS_HELIUM_SETTINGS_PATCH'" \
  "$uidemo_script" | cut -d: -f1)"
uidemo_patch_sync_line="$(grep -n \
  'patches/ "$HOST:$SRC/.cmux-patches/"' \
  "$uidemo_script" | cut -d: -f1)"
test -n "$uidemo_reverse_line"
test -n "$uidemo_patch_sync_line"
test "$uidemo_reverse_line" -lt "$uidemo_patch_sync_line"
grep -Fq 'warm tree matches neither applied nor pristine state' \
  "$uidemo_script"
grep -Fq 'CMUX_TARGET=views_examples "$CMUX_SCRIPTS_DIR/apply.sh"' "$uidemo_script"
grep -Fq 'autoninja -C out/Release views_examples > \"\$log\" 2>&1' "$uidemo_script"
grep -Fq 'build_rc=\$?' "$uidemo_script"
grep -Fq 'exit \"\$build_rc\"' "$uidemo_script"
grep -Fq 'remote_build_rc=$?' "$uidemo_script"
grep -Fq 'exit "$remote_build_rc"' "$uidemo_script"
if grep -Eq 'autoninja[^|]*\|' "$uidemo_script"; then
  echo "uidemo must not pipe autoninja output" >&2
  exit 1
fi
grep -Fq 'rsync -az --delete "$HOST:$SRC/out/Release/cmux_ui_demo.app/"' "$uidemo_script"
grep -Fq '/usr/bin/open -na "$demo_app"' "$uidemo_script"
grep -Fq 'sleep 8' "$uidemo_script"
grep -Fq 'demo_pids | grep -Fxq "$demo_pid"' "$uidemo_script"
grep -Fq 'base::TestMemoryConsumerRegistry memory_consumer_registry;' "$apply_script"
demo_source="$root/overlay/ui/views/examples/cmux_demo_example.cc"
demo_header="$root/overlay/ui/views/examples/cmux_demo_example.h"
grep -Fq 'VIEWS_EXAMPLES_EXPORT void AddCmuxDemoColorMixers(' "$demo_header"
grep -Fq 'void AddCmuxDemoColorMixers(ui::ColorProvider* color_provider,' "$demo_source"
grep -Fq 'mixer[kColorTabGroupDialogIconEnabled] = {on_surface};' "$demo_source"
grep -Fq 'mixer[kColorTabHoverCardBackground] = {surface};' "$demo_source"
grep -Fq 'base::BindRepeating(&AddCmuxDemoColorMixers));' "$apply_script"
if grep -Eq '"//chrome/browser/ui/color:mixers"' "$apply_script"; then
  echo "views_examples must use its cmux-only mixer, not the browser graph" >&2
  exit 1
fi

# The hover-card constructor patch must run for views_examples as well as the
# full browser build; keeping it before the app-only mode block prevents the
# fast loop from compiling against an unpatched Chromium header.
hover_friend_line="$(grep -n "cmux_hover_friend = '  friend class ::cmux::CmuxRailHoverCardBubble;" "$apply_script" | cut -d: -f1)"
app_mode_line="$(grep -n '^if \[ "\$MODE" = "full" \]; then$' "$apply_script" | head -1 | cut -d: -f1)"
test -n "$hover_friend_line"
test -n "$app_mode_line"
test "$hover_friend_line" -lt "$app_mode_line"

# Keep the workspace group header mechanically aligned with Helium's patched
# Chromium 150 vertical group header. These assertions cover interaction
# contracts that otherwise regress without pure-model test visibility.
rail_source="$root/overlay/chrome/browser/cmux_term/cmux_rail.cc"
rail_header="$root/overlay/chrome/browser/cmux_term/cmux_rail.h"
rail_build="$root/overlay/chrome/browser/cmux_term/BUILD.gn"
group_bubble_header="$root/overlay/chrome/browser/cmux_term/cmux_rail_group_editor_bubble.h"
group_bubble_source="$root/overlay/chrome/browser/cmux_term/cmux_rail_group_editor_bubble.cc"
hover_card_source="$root/overlay/chrome/browser/cmux_term/cmux_rail_hover_card.cc"
cmux_views_source="$root/overlay/chrome/browser/cmux_term/cmux_views.cc"
toolbar_menus_source="$root/overlay/chrome/browser/cmux_term/cmux_toolbar_menus.cc"
tab_strip_source="$root/overlay/chrome/browser/cmux_term/cmux_tab_strip.cc"
grep -Fq 'constexpr int kWorkspaceHorizontalInset = 7;' "$rail_source"
grep -Fq 'views::InkDrop::Get(button)->SetHighlightOpacity(0.2f);' "$rail_source"
grep -Fq 'views::InkDrop::Get(button)->SetVisibleOpacity(0.08f);' "$rail_source"
grep -Fq 'views::ButtonController::NotifyAction::kOnPress' "$rail_source"
grep -Fq 'GetGroupReorderCommandForKeyboardEvent(event)' "$rail_source"
grep -Fq 'case ui::EventType::kGestureLongTap:' "$rail_source"
grep -Fq 'case ui::EventType::kGestureScrollUpdate:' "$rail_source"
grep -Fq 'set_context_menu_controller(this);' "$rail_source"
grep -Fq 'close_button_observation_.Observe(close_);' "$rail_source"
grep -Fq 'void CmuxRail::ShiftWorkspace(WorkspaceId id,' "$rail_source"
grep -Fq 'rail_->ShowGroupEditorBubble(group_, editor_bubble_button_);' "$rail_source"
grep -Fq 'ui::GestureEvent converted_event(*event' "$rail_source"
grep -Fq 'drag_time_elapsed >= drag_start_animation_duration' "$rail_source"
grep -Fq 'drag_start_animation_->Start(drag_start_time);' "$rail_source"
grep -Fq 'gfx::Tween::CalculateValue(' "$rail_source"
grep -Fq 'static constexpr int kExistingGroupCommandBase = 100000;' "$rail_source"
grep -Fq 'rail_->ActivateRowInSelection(id_);' "$rail_source"
grep -Fq 'class PendingFocusedWebTabCloseObserver' "$cmux_views_source"
grep -Fq 'void BeforeUnloadFired(bool proceed) override' "$cmux_views_source"
grep -Fq 'void BeforeUnloadDialogCancelled() override' "$cmux_views_source"
grep -Fq '&CmuxWindowView::CompleteFocusedWebTabClose' \
  "$cmux_views_source"
gesture_block="$(sed -n \
  '/class CmuxRailRow/,/BEGIN_METADATA(CmuxRailRow)/p' "$rail_source" | \
  sed -n \
    '/void OnGestureEvent(ui::GestureEvent\* event) override {/,/void OnFocus() override {/p')"
printf '%s\n' "$gesture_block" | grep -Fq 'else if (!active_) {'
printf '%s\n' "$gesture_block" | grep -Fq \
  'rail_->ActivateRowInSelection(id_);'

# The adapted workspace menu keeps Chromium/Helium's item ordering. Cmux-only
# convenience commands belong elsewhere because even disabled/extra rows alter
# the native menu's geometry and interaction feel.
workspace_menu_block="$(sed -n \
  '/class CmuxRailWorkspaceContextMenu/,/---- Bottom new-workspace action/p' \
  "$rail_source")"
for unexpected_label in 'u"Copy URL"' 'u"Copy URLs"' \
    'u"Hibernate"' 'u"Hibernate Other Workspaces"' \
    'u"Close Workspaces Above"'; do
  if printf '%s\n' "$workspace_menu_block" | grep -Fq "$unexpected_label"; then
    echo "workspace context menu contains non-Helium item: $unexpected_label" >&2
    exit 1
  fi
done
printf '%s\n' "$workspace_menu_block" | grep -Fq 'u"Close"'
printf '%s\n' "$workspace_menu_block" | grep -Fq 'u"Close Other Workspaces"'
printf '%s\n' "$workspace_menu_block" | grep -Fq 'u"Close Workspaces Below"'

# Keep the workspace row's P0 visual behavior pinned to Helium's material-tab
# mixer and patched Chromium 150 close/group controls.
grep -Fq 'constexpr SkAlpha kTabInactiveHoverAlpha = 0.45 * SK_AlphaOPAQUE;' "$rail_source"
grep -Fq 'const int button_size = WorkspaceCloseIconSize() + 12;' "$rail_source"
grep -Fq 'kCloseTabChromeRefreshOldIcon' "$rail_source"
grep -Fq 'gfx::Insets::VH(1, 0), 4' "$rail_source"
grep -Fq 'void AddLayerToRegion(ui::Layer* new_layer,' "$rail_source"
grep -Fq 'void RemoveLayerFromRegions(ui::Layer* old_layer) override' "$rail_source"
grep -Fq 'const gfx::Size close_size = close_->GetPreferredSize();' "$rail_source"
grep -Fq '&CmuxRailRow::OnCloseButtonMouseEvent' "$rail_source"
close_button_block="$(sed -n \
  '/class CmuxRailCloseButton/,/BEGIN_METADATA(CmuxRailCloseButton)/p' \
  "$rail_source")"
if printf '%s\n' "$close_button_block" | grep -Fq 'PaintButtonContents'; then
  echo "workspace close button must use Chromium vector-icon geometry" >&2
  exit 1
fi
grep -Fq 'ResolveProviderColor(this, kColorLocationBarBackground)' "$rail_source"
grep -Fq 'apparently_active ? ui::kColorSysOnSurface' "$rail_source"
grep -Fq ': ui::kColorSysOnSurfaceSecondary)' "$rail_source"
grep -Fq 'focus_ring->SetColorId(ui::kColorSysStateFocusRing);' "$rail_source"
grep -Fq 'gfx::Animation::RichAnimationDuration(' "$rail_source"
grep -Fq 'kWorkspaceGlowHoverAnimationDuration' "$rail_source"
grep -Fq 'return GetHoverOpacity() > 0.5f;' "$rail_source"
grep -Fq 'if (active_ || selected_) {' "$rail_source"
# views_examples has a generic, non-null ColorProvider but deliberately does
# not link Chrome's heavyweight browser mixer graph. Chrome-only IDs must be
# accepted only when they resolve to a real color, then fall back to the rail
# config or generic system palette without changing the shipped browser path.
grep -Fq 'std::optional<SkColor> ResolveProviderColor(' "$rail_source"
grep -Fq 'color == gfx::kPlaceholderColor ? std::nullopt' "$rail_source"
grep -Fq 'ResolveProviderColor(this, kColorLocationBarBackground)' "$rail_source"
grep -Fq 'ResolveProviderColor(this, kColorToolbar)' "$rail_source"
grep -Fq 'ResolveProviderColor(view, ui::kColorSysSurface)' "$rail_source"
grep -Fq 'ResolveFooterColor(kColorToolbarButtonIconDisabled)' "$rail_source"
grep -Fq 'FooterColorCallback color_callback' "$rail_source"
if grep -Eq \
    'GetColor\((kColorLocationBarBackground|kColorToolbar|kColorTabBackground|kColorTabGroupContextMenu|kColorToolbarButtonIconDisabled|kColorToolbarInkDrop)' \
    "$rail_source"; then
  echo "Chrome-only rail colors must use placeholder-aware resolution" >&2
  exit 1
fi
if grep -Eq \
    '^[[:space:]]*"//chrome/browser/ui/color:mixers"' \
    "$rail_build"; then
  echo "views_examples rail target must not link Chrome browser color mixers" >&2
  exit 1
fi
hover_state_block="$(sed -n \
  '/void UpdateHoverState()/,/void OnFrameActiveStateChanged()/p' \
  "$rail_source")"
if printf '%s\n' "$hover_state_block" | grep -Eq \
    'cfg_->animations|cfg_->animation_ms'; then
  echo "workspace hover animation must be independent of rail animation config" >&2
  exit 1
fi
row_block="$(sed -n \
  '/class CmuxRailRow/,/BEGIN_METADATA(CmuxRailRow)/p' \
  "$rail_source")"
printf '%s\n' "$row_block" | \
  grep -Fq 'SetFocusBehavior(FocusBehavior::ALWAYS);'
if printf '%s\n' "$row_block" | grep -Fq 'kColorTabBackground'; then
  echo "workspace rows must use Helium material palette, not stock tab tokens" >&2
  exit 1
fi
group_header_block="$(sed -n \
  '/class CmuxRailGroupHeader/,/BEGIN_METADATA(CmuxRailGroupHeader)/p' \
  "$rail_source")"
printf '%s\n' "$group_header_block" | \
  grep -Fq 'SetFocusBehavior(FocusBehavior::ALWAYS);'
grep -Fq 'gfx::kTabGroupLimeDarkMode' "$rail_source"
grep -Fq 'gfx::kTabGroupMagentaLightMode' "$rail_source"
grep -Fq 'kBrowserToolsChromeRefreshOldIcon' "$rail_source"
grep -Fq 'kKeyboardArrowDownChromeRefreshOldIcon' "$rail_source"
grep -Fq 'GroupCollapseIcon(collapsed_)' "$rail_source"

grep -Fq 'METADATA_HEADER(CmuxRailGroupEditorBubble, views::View)' "$group_bubble_header"
grep -Fq 'BEGIN_METADATA(CmuxRailGroupEditorBubble)' "$group_bubble_source"
grep -Fq 'CreateBubbleMenuItem(' "$group_bubble_source"
grep -Fq 'ui::EF_SHIFT_DOWN | ui::EF_ALT_DOWN' "$group_bubble_source"
grep -Fq 'return ui::Accelerator(ui::VKEY_C' "$group_bubble_source"
grep -Fq 'return ui::Accelerator(ui::VKEY_W' "$group_bubble_source"
grep -Fq 'bool CmuxRailGroupEditorBubble::AcceleratorPressed(' "$group_bubble_source"
grep -Fq 'std::move(closed_callback_).Run();' "$group_bubble_source"
# Bubble menu rows must retain Chromium 150 HoverButton's interaction
# controller, including right-click activation and release-triggered ink drop.
grep -Fq 'using WorkspaceButtonControllerBase = views::ButtonController;' "$group_bubble_source"
grep -Fq 'class WorkspaceHoverButtonController : public WorkspaceButtonControllerBase' "$group_bubble_source"
grep -Fq 'class WorkspaceHoverButton : public views::LabelButton' "$group_bubble_source"
grep -Fq 'class WorkspaceBubbleMenuItemButton : public WorkspaceHoverButton' "$group_bubble_source"
grep -Fq 'views::InkDrop::UseInkDropForFloodFillRipple(' "$group_bubble_source"
grep -Fq 'kColorHoverButtonBackgroundHovered' "$group_bubble_source"
grep -Fq 'SetTriggerableEventFlags(ui::EF_LEFT_MOUSE_BUTTON |' "$group_bubble_source"
grep -Fq 'ui::EF_RIGHT_MOUSE_BUTTON);' "$group_bubble_source"
grep -Fq 'views::ButtonController::NotifyAction::kOnRelease' "$group_bubble_source"
grep -Fq 'button->SetBorder(views::CreateEmptyBorder(gfx::Insets(12)));' "$group_bubble_source"
grep -Fq '`controls/hover_button.{h,cc}`' "$root/THIRD_PARTY_NOTICES.md"
grep -Fq '`controls/hover_button_controller.{h,cc}`' "$root/THIRD_PARTY_NOTICES.md"
# Chromium 151 carries both icon families. Helium's default non-rounded branch
# must remain selected by the feature at runtime instead of by major version.
grep -Fq 'features::IsRoundedIconsEnabled()' "$group_bubble_source"
grep -Fq 'kNewTabInGroupRefreshOldIcon' "$group_bubble_source"
grep -Fq 'kMoveGroupToNewWindowRefreshOldIcon' "$group_bubble_source"
grep -Fq 'kUngroupRefreshOldIcon' "$group_bubble_source"
grep -Fq 'kCloseGroupRefreshOldIcon' "$group_bubble_source"
# Disabled action icons inherit the disabled row text color after attachment,
# exactly as TabGroupEditorBubbleView::AddedToWidget() does.
grep -Fq 'void AddedToWidget() override;' "$group_bubble_header"
grep -Fq 'simple_menu_items_' "$group_bubble_header"
grep -Fq 'void CmuxRailGroupEditorBubble::AddedToWidget()' "$group_bubble_source"
grep -Fq 'menu_item->GetCurrentTextColor()' "$group_bubble_source"
grep -Fq 'kColorTabGroupDialogIconEnabled' "$group_bubble_source"
if grep -Fq 'ui::kColorMenuIconDisabled' "$group_bubble_source"; then
  echo "group editor must derive disabled icon color from disabled row text" >&2
  exit 1
fi
# Chromium 151 renamed the ownership-transferring bubble factory, while the
# supported 148-150 branches still expose CreateBubble(). Keep both sides of
# the version gate next to this Helium-derived editor implementation.
grep -Fq '#include "chrome/common/chrome_version.h"' "$group_bubble_source"
group_bubble_factory_block="$(sed -n \
  '/views::BubbleDialogDelegate\* raw_delegate = bubble_delegate.get();/,/raw_delegate->GetBubbleFrameView()/p' \
  "$group_bubble_source")"
printf '%s\n' "$group_bubble_factory_block" | grep -Fq '#if CHROME_VERSION_MAJOR >= 151'
printf '%s\n' "$group_bubble_factory_block" | grep -Fq 'views::BubbleDialogDelegate::CreateBubbleDeprecated('
printf '%s\n' "$group_bubble_factory_block" | grep -Fq 'views::BubbleDialogDelegate::CreateBubble(std::move(bubble_delegate));'
printf '%s\n' "$group_bubble_factory_block" | grep -Fq '#endif'
# The M151 profile menu keeps the same native-Widget ownership as older
# Chromium while using the renamed explicit compatibility factory.
profile_bubble_factory_block="$(sed -n \
  '/ProfileMenuViewBase\* bubble_ptr = bubble.get();/,/if (widget) {/p' \
  "$toolbar_menus_source")"
printf '%s\n' "$profile_bubble_factory_block" | \
  grep -Fq '#if CHROME_VERSION_MAJOR >= 151'
printf '%s\n' "$profile_bubble_factory_block" | \
  grep -Fq 'views::BubbleDialogDelegate::CreateBubbleDeprecated('
printf '%s\n' "$profile_bubble_factory_block" | \
  grep -Fq 'views::Widget::InitParams::NATIVE_WIDGET_OWNS_WIDGET'
printf '%s\n' "$profile_bubble_factory_block" | \
  grep -Fq 'views::BubbleDialogDelegateView::CreateBubble(std::move(bubble));'
# Drag insertion still reserves and animates Chromium's layout gap, but Helium
# does not paint an accent-colored insertion bar inside that gap.
insertion_spacer_block="$(sed -n \
  '/class InsertionSpacer : public views::View {/,/BEGIN_METADATA(InsertionSpacer)/p' \
  "$tab_strip_source")"
printf '%s\n' "$insertion_spacer_block" | grep -Fq 'showing ? kInsertionGap : 0'
if printf '%s\n' "$insertion_spacer_block" | grep -Eq 'OnPaint|accent_color|DrawRoundRect'; then
  echo "tab insertion spacer must be an unpainted animated layout gap" >&2
  exit 1
fi
grep -Fq 'constexpr base::TimeDelta kHoverCardSlideDuration = base::Milliseconds(200);' "$hover_card_source"
grep -Fq 'constexpr base::TimeDelta kShowWithoutDelayTimeBuffer' "$hover_card_source"
grep -Fq 'std::make_unique<views::BubbleSlideAnimator>' "$hover_card_source"
grep -Fq 'std::make_unique<views::WidgetFadeAnimator>' "$hover_card_source"
grep -Fq 'views::BubbleBorder::Arrow::LEFT_TOP' "$hover_card_source"
grep -Fq 'SetAccessibleWindowRole(ax::mojom::Role::kNone);' "$hover_card_source"
# Keep Chromium's FadeView maximum-size interpolation and the exact flex
# priority ordering: preview shrinks before the title when height is limited.
crossfade_label_block="$(sed -n \
  '/class CrossfadeLabel : public views::View {/,/BEGIN_METADATA(CrossfadeLabel)/p' \
  "$hover_card_source")"
printf '%s\n' "$crossfade_label_block" | \
  grep -Fq 'percent_ = std::clamp(percent, 0.0, 1.0);'
printf '%s\n' "$crossfade_label_block" | \
  grep -Fq 'return gfx::Tween::SizeValueBetween(percent_,'
printf '%s\n' "$crossfade_label_block" | \
  grep -Fq 'outgoing_->GetPreferredSize(),'
printf '%s\n' "$crossfade_label_block" | \
  grep -Fq 'current_->GetPreferredSize());'
printf '%s\n' "$crossfade_label_block" | \
  grep -Fq 'double percent_ = 1.0;'
hover_card_bubble_ctor_block="$(sed -n \
  '/  CmuxRailHoverCardBubble(views::View\* anchor,/,/    UpdateContent(data);/p' \
  "$hover_card_source")"
title_flex_block="$(printf '%s\n' "$hover_card_bubble_ctor_block" | sed -n \
  '/    title_->SetProperty(/,/            \.WithOrder(2));/p')"
printf '%s\n' "$title_flex_block" | grep -Fq 'views::kFlexBehaviorKey'
printf '%s\n' "$title_flex_block" | \
  grep -Fq 'views::MinimumFlexSizeRule::kScaleToMinimum'
printf '%s\n' "$title_flex_block" | \
  grep -Fq 'views::MaximumFlexSizeRule::kScaleToMaximum'
printf '%s\n' "$title_flex_block" | grep -Fq '.WithOrder(2)'
thumbnail_flex_block="$(printf '%s\n' "$hover_card_bubble_ctor_block" | sed -n \
  '/    thumbnail_->SetProperty(/,/            \.WithOrder(1));/p')"
printf '%s\n' "$thumbnail_flex_block" | grep -Fq 'views::kFlexBehaviorKey'
printf '%s\n' "$thumbnail_flex_block" | \
  grep -Fq 'views::MinimumFlexSizeRule::kScaleToMinimum'
printf '%s\n' "$thumbnail_flex_block" | \
  grep -Fq 'views::MaximumFlexSizeRule::kScaleToMaximum'
printf '%s\n' "$thumbnail_flex_block" | grep -Fq '.WithOrder(1)'
# Helium enables Chromium's preview path by default. Keep the workspace adapter
# connected to the real ThumbnailImage source, including cancellation,
# readiness-based capture delay, exact 252x141 rail geometry, and the native
# two-layer 200 ms image crossfade.
grep -Fq 'constexpr int kHoverCardPreviewHeight = 141;' "$hover_card_source"
grep -Fq 'class HoverCardThumbnailView : public views::View,' "$hover_card_source"
grep -Fq 'target_image_ = AddChildView(CreateImageView());' "$hover_card_source"
grep -Fq 'image_fading_out_ = AddChildView(CreateImageView());' "$hover_card_source"
grep -Fq 'gfx::LinearAnimation image_transition_animation_;' "$hover_card_source"
grep -Fq 'MaybeStartPreviewRequest(is_initial);' "$hover_card_source"
show_hover_card_block="$(sed -n \
  '/  void ShowHoverCard(bool is_initial,/,/  void UpdateCardContent() {/p' \
  "$hover_card_source")"
show_bounds_line="$(printf '%s\n' "$show_hover_card_block" | \
  grep -n 'slide_animator_->UpdateTargetBounds' | head -1 | cut -d: -f1)"
show_preview_line="$(printf '%s\n' "$show_hover_card_block" | \
  grep -n 'MaybeStartPreviewRequest(is_initial);' | cut -d: -f1)"
test -n "$show_bounds_line"
test -n "$show_preview_line"
test "$show_bounds_line" -lt "$show_preview_line"
grep -Fq 'GetPreviewImageCaptureDelay(data_.preview_readiness)' "$hover_card_source"
grep -Fq 'preview_request_workspace_ != intended_workspace' "$hover_card_source"
grep -Fq 'value >= kPreviewImageCrossfadeStart' "$hover_card_source"
preview_capture_delay_block="$(sed -n \
  '/if (delayed_show_timer_\.IsRunning()) {/,/delayed_show_timer_\.Start(/p' \
  "$hover_card_source")"
printf '%s\n' "$preview_capture_delay_block" | \
  grep -Fq 'hover_card_->SetPlaceholderImage();'
printf '%s\n' "$preview_capture_delay_block" | \
  grep -Fq 'PreviewWaitState::kWaitingWithPlaceholder'
preview_thumbnail_block="$(sed -n \
  '/class HoverCardThumbnailView : public views::View,/,/BEGIN_METADATA(HoverCardThumbnailView)/p' \
  "$hover_card_source")"
preview_icon_block="$(printf '%s\n' "$preview_thumbnail_block" | sed -n \
  '/void SetPlaceholderImage() {/,/void ClearImage() {/p')"
printf '%s\n' "$preview_icon_block" | \
  grep -Fq '#if CHROME_VERSION_MAJOR >= 150'
printf '%s\n' "$preview_icon_block" | \
  grep -Fq 'features::IsRoundedIconsEnabled() ? kGlobeIcon'
printf '%s\n' "$preview_icon_block" | \
  grep -Fq 'SetImageFromIcon(ImageType::kPlaceholder, kGlobeIcon);'
printf '%s\n' "$preview_icon_block" | \
  grep -Fq 'SetImageFromIcon(ImageType::kCrashed, kCrashedTabIcon);'
# Chromium 150 is the shared API boundary for BubbleAnchor, directional widget
# fades, CancelSlide(), and rounded/old icon selection. Chromium 149 must retain
# the exact AnchorView, parameterless fade, and legacy crashed-icon APIs.
test "$(grep -Fc '#if CHROME_VERSION_MAJOR >= 150' \
  "$hover_card_source")" -eq 12
if grep -Fq '#if CHROME_VERSION_MAJOR >= 151' "$hover_card_source"; then
  echo "hover-card Chromium API boundary must be version 150" >&2
  exit 1
fi
grep -Fq 'slide_animator_->SnapToAnchorView(target_.get());' \
  "$hover_card_source"
grep -Fq 'slide_animator_->AnimateToAnchorView(target_.get());' \
  "$hover_card_source"
grep -Fq 'slide_animator_->UpdateTargetBounds();' "$hover_card_source"
grep -Fq 'fade_animator_->FadeIn();' "$hover_card_source"
grep -Fq 'fade_animator_->FadeOut();' "$hover_card_source"
grep -Fq 'fade_animator_->CancelSlide(false);' "$hover_card_source"
grep -Fq 'class CmuxWorkspaceHoverCardPreviewRequest final' "$cmux_views_source"
grep -Fq 'subscription_ = thumbnail_->Subscribe();' "$cmux_views_source"
grep -Fq 'subscription_->SetSizeHint(kWorkspaceHoverCardPreviewSize);' "$cmux_views_source"
grep -Fq 'thumbnail_->RequestThumbnailImage();' "$cmux_views_source"
grep -Fq 'data.show_preview = id != ws_;' "$cmux_views_source"
grep -Fq 'data.preview_readiness =' "$cmux_views_source"
grep -Fq 'RequestWorkspaceHoverCardPreview(' "$cmux_views_source"
# A retained hover card is a live view of the focused pane's selected surface,
# not merely of the workspace row/favicon. Same-favicon surface switches and
# TabData kAll changes must update it without rebuilding the whole rail, and
# queued ThumbnailImage callbacks must carry enough identity to reject an old
# surface or a replaced thumbnail.
grep -Fq 'SurfaceTabId source_surface = kInvalidId;' "$rail_header"
grep -Fq 'uintptr_t preview_source_id = 0;' "$rail_header"
grep -Fq 'void NotifyWorkspaceDataChanged(WorkspaceId id);' "$rail_header"
grep -Fq 'void CmuxRail::NotifyWorkspaceDataChanged(WorkspaceId id)' \
  "$rail_source"
grep -Fq 'data.source_surface = surface ? surface->id : kInvalidId;' \
  "$cmux_views_source"
grep -Fq 'data.title = tab_data.title;' "$cmux_views_source"
grep -Fq 'const GURL& visible_url = tab_data.visible_url;' "$cmux_views_source"
grep -Fq 'reinterpret_cast<uintptr_t>(tab_data.thumbnail.get())' \
  "$cmux_views_source"
grep -Fq 'void OnTabChangedAt(tabs::TabInterface* tab,' "$cmux_views_source"
grep -Fq 'change_type != TabChangeType::kAll' "$cmux_views_source"
grep -Fq 'void NotifyWorkspaceHoverCardDataChanged(WorkspaceId workspace,' \
  "$cmux_views_source"
grep -Fq 'rail_->NotifyWorkspaceDataChanged(workspace);' "$cmux_views_source"
refresh_pane_tabs_block="$(sed -n \
  '/  void RefreshPaneTabs(PaneId pane_id) {/,/  void FlushDeferredPaneTabs() {/p' \
  "$cmux_views_source")"
printf '%s\n' "$refresh_pane_tabs_block" | \
  grep -Fq 'NotifyWorkspaceHoverCardDataChanged(workspace, pane_id, pane->selected);'
printf '%s\n' "$refresh_pane_tabs_block" | \
  grep -Fq 'RefreshRailIfFaviconChanged(workspace, pane_id, *view);'
hover_notify_line="$(printf '%s\n' "$refresh_pane_tabs_block" | \
  grep -n 'NotifyWorkspaceHoverCardDataChanged' | cut -d: -f1)"
favicon_gate_line="$(printf '%s\n' "$refresh_pane_tabs_block" | \
  grep -n 'RefreshRailIfFaviconChanged' | cut -d: -f1)"
test -n "$hover_notify_line"
test -n "$favicon_gate_line"
test "$hover_notify_line" -lt "$favicon_gate_line"
grep -Fq 'reinterpret_cast<uintptr_t>(thumbnail.get()) != preview_source_id' \
  "$cmux_views_source"
grep -Fq 'data_.preview_source_id != next_data.preview_source_id' \
  "$hover_card_source"
grep -Fq 'if (!content_changed && !preview_changed)' "$hover_card_source"
grep -Fq 'preview_request_source_surface_ == data_.source_surface' \
  "$hover_card_source"
grep -Fq 'preview_request_source_id_ == data_.preview_source_id' \
  "$hover_card_source"
grep -Fq 'data_.source_surface != intended_source_surface' \
  "$hover_card_source"
grep -Fq 'data_.preview_source_id != intended_source_id' "$hover_card_source"
grep -Fq 'preview_request_source_surface_ = kInvalidId;' "$hover_card_source"
grep -Fq 'preview_request_source_id_ = 0;' "$hover_card_source"
grep -Fq 'rail_->OnWorkspaceRowMouseEntered(this);' "$rail_source"
row_mouse_exit_block="$(sed -n \
  '/void OnMouseExited(const ui::MouseEvent&) override {/,/^  }/p' \
  "$rail_source" | head -20)"
printf '%s\n' "$row_mouse_exit_block" | grep -Fq 'SetHovered(false);'
if printf '%s\n' "$row_mouse_exit_block" | grep -Fq 'hover_card_controller_'; then
  echo "workspace-row exit must leave hover-card lifetime to the rail" >&2
  exit 1
fi
rail_mouse_exit_block="$(sed -n \
  '/void CmuxRail::OnMouseExited(const ui::MouseEvent&) {/,/^}/p' \
  "$rail_source")"
printf '%s\n' "$rail_mouse_exit_block" | \
  grep -Fq 'CmuxRailHoverCardController::UpdateType::kHover'

# The bottom action is a local, workspace-dispatching copy of Helium's
# VerticalNewTabButton plus the Chromium shared button behavior it inherits.
grep -Fq 'class CmuxRailNewWorkspaceButton : public views::LabelButton' "$rail_source"
grep -Fq 'constexpr int kFooterVerticalPadding = 4;' "$rail_source"
grep -Fq 'constexpr int kFooterHorizontalPadding = 7;' "$rail_source"
grep -Fq 'constexpr int kFooterImageLabelGap = 7;' "$rail_source"
grep -Fq 'constexpr int kFooterCornerRadius = 8;' "$rail_source"
grep -Fq 'SetPreferredSize(gfx::Size(-1, kFooterHeight));' "$rail_source"
grep -Fq 'UpdateLabel(width() > CalculatePreferredSize({}).width());' "$rail_source"
grep -Fq 'SetImageModel(views::Button::STATE_DISABLED, image_model);' "$rail_source"
grep -Fq 'views::LabelButton::OnMouseEvent(event);' "$rail_source"
grep -Fq '*mask = SkPath::RRect(GetButtonShape());' "$rail_source"
grep -Fq 'views::MenuAnchorPosition::kTopLeft, source_type' "$rail_source"
grep -Fq 'void ConfigureFooterInkDrop(' "$rail_source"
grep -Fq 'constexpr float kFooterInkDropVisibleOpacity = 0.06f;' "$rail_source"
grep -Fq 'constexpr SkAlpha kFooterInkDropHighlightVisibleAlpha = 0x14;' "$rail_source"
grep -Fq 'std::make_unique<views::FloodFillInkDropRipple>' "$rail_source"
grep -Fq 'ink_drop->SetLayerRegion(views::LayerRegion::kAbove);' "$rail_source"
grep -Fq 'kColorTabBackgroundInactiveHoverFrameActive,' "$rail_source"
grep -Fq 'kColorTabBackgroundActiveFrameActive);' "$rail_source"
grep -Fq 'The new-workspace action is visually part of the rail at rest.' \
  "$rail_source"
if grep -Fq '? kColorTabBackgroundInactiveFrameActive' "$rail_source" ||
   grep -Fq ': kColorTabBackgroundInactiveFrameInactive;' "$rail_source"; then
  echo "new-workspace button must not paint a separate idle background" >&2
  exit 1
fi
if grep -Fq \
    '#include "chrome/browser/ui/views/toolbar/toolbar_ink_drop_util.h"' \
    "$rail_source"; then
  echo "cmux_chrome_ui must not depend on the browser toolbar ink-drop target" >&2
  exit 1
fi
if grep -Eq \
    '^[[:space:]]+(ConfigureInkDrop|ConfigureToolbarInkdropForRefresh2023)\(' \
    "$rail_source"; then
  echo "rail footer must use its browser-neutral ink-drop copy" >&2
  exit 1
fi
grep -Fq \
  '`chrome/browser/ui/views/toolbar/toolbar_ink_drop_util.{h,cc}` at tag' \
  "$root/THIRD_PARTY_NOTICES.md"
grep -Fq \
  '`shared/tab_strip_flat_edge_button.cc` at' \
  "$root/THIRD_PARTY_NOTICES.md"

# Workspace-row and footer menus retain Chromium 150's conditional sections,
# ordering, dynamic extension state, and input source. These source contracts
# complement the pure WindowModel tests; the shipped build exercises Chromium's
# real account/profile services and MenuRunner.
views_source="$root/overlay/chrome/browser/cmux_term/cmux_views.cc"
grep -Fq 'AppendOptionalWorkspaceContextMenuItems(' "$rail_source"
grep -Fq 'WorkspaceContextAction::kAddToReadLater' "$views_source"
grep -Fq 'ui::mojom::MenuSourceType::kTouch' "$rail_source"
grep -Fq 'send_tab_to_self::GetEntryPointDisplayReason(' "$views_source"
# Chromium 148-150 does not define the rounded/old send-to-self icon API used
# by Chromium 151. Keep every such reference inside the major-version branch,
# including both enhanced submenu paths and the ordinary menu item path.
send_to_self_block="$(sed -n \
  '/  void AppendSendTabToSelfItem(/,/  base::WeakPtr<Browser> browser_/p' \
  "$views_source")"
test "$(printf '%s\n' "$send_to_self_block" |
  grep -Fc '#if CHROME_VERSION_MAJOR >= 151')" -eq 4
enhanced_send_to_self_block="$(printf '%s\n' "$send_to_self_block" |
  sed -n \
    '/send_tab_to_self::kSendTabToSelfEnhancedDesktopUI) &&/,/model->SetIsNewFeatureAt(/p')"
test "$(printf '%s\n' "$enhanced_send_to_self_block" |
  grep -Fc '#if CHROME_VERSION_MAJOR >= 151')" -eq 3
test "$(printf '%s\n' "$enhanced_send_to_self_block" |
  grep -Fc 'kDevicesOldIcon')" -eq 2
test "$(printf '%s\n' "$enhanced_send_to_self_block" |
  grep -Ec '^[[:space:]]+kDevicesIcon,$')" -eq 2
if ! printf '%s\n' "$send_to_self_block" | awk '
  /^#if CHROME_VERSION_MAJOR >= 151$/ {
    branch[++depth] = "m151"
    next
  }
  /^#if / {
    branch[++depth] = "other"
    next
  }
  /^#else$/ {
    if (branch[depth] == "m151") {
      branch[depth] = "not_m151"
    } else if (branch[depth] == "not_m151") {
      branch[depth] = "m151"
    }
    next
  }
  /^#endif$/ {
    delete branch[depth--]
    next
  }
  /features::IsRoundedIconsEnabled\(\)|kDevicesOldIcon/ {
    guarded = 0
    for (i = 1; i <= depth; ++i) {
      if (branch[i] == "m151") {
        guarded = 1
      }
    }
    if (!guarded) {
      exit 1
    }
  }
  END {
    if (depth != 0) {
      exit 1
    }
  }
'; then
  echo "Chromium 151-only send-to-self icons escaped their version guard" >&2
  exit 1
fi
grep -Fq 'glic::GlicEnabling::IsReadyForProfile(' "$views_source"
grep -Fq 'IDS_TAB_CXMENU_GLIC_START_SHARE' "$views_source"
grep -Fq 'CreateNewConversationForTabs(tabs)' "$views_source"
grep -Fq 'for (WorkspaceId target : model_.WorkspacesForCommand(id))' "$views_source"
grep -Fq 'extensions_features::kExtensionTabContextMenu' "$views_source"
grep -Fq 'extensions::MenuItem::TAB' "$views_source"
grep -Fq 'WorkspaceContextWebTabTargets(WorkspaceId context) const' "$views_source"
grep -Fq 'return ::IsSiteMuted(tabs, index);' "$views_source"
grep -Fq 'const bool mute =' "$views_source"
grep -Fq 'return !CmuxIsSiteMuted(*tabs, browser_index);' "$views_source"
grep -Fq 'CmuxIsSiteMuted(*tabs, browser_index) == mute' "$views_source"
if grep -Eq 'WorkspaceContextAction::k(CopyUrls|Hibernate|CloseAbove)' \
    "$views_source" "$rail_header"; then
  echo "non-Helium workspace commands must not leave unreachable handlers" >&2
  exit 1
fi
grep -Fq 'New Workspace in Group' "$rail_source"
grep -Fq 'New Workspace Group' "$rail_source"
grep -Fq 'New Split View with Current Workspace' "$rail_source"

# An ordinary title/icon/selection refresh during a drag must not reparent the
# content_-owned overlay visuals back into their logical collections. Geometry
# changes cancel first; compatible refreshes update safe fields and return
# before rows_by_id ownership reconciliation begins.
grep -Fq 'bool CmuxRail::ActiveDragStructureMatches(' "$rail_source"
grep -Fq 'view->parent() == content_' "$rail_source"
grep -Fq 'void CmuxRail::RefreshActiveDragModelFields(' "$rail_source"
grep -Fq 'drag_views_detached_ = true;' "$rail_source"
active_drag_refresh_block="$(sed -n \
  '/if (drag_views_detached_) {/,/std::map<WorkspaceId, raw_ptr<CmuxRailRow>> rows_by_id;/p' \
  "$rail_source")"
printf '%s\n' "$active_drag_refresh_block" |
  grep -Fq 'RefreshActiveDragModelFields(model, icons);'
printf '%s\n' "$active_drag_refresh_block" |
  grep -Fq 'EndRowDrag(/*commit=*/false);'

# Preserve Chromium 150's drag geometry and completion contracts. The logical
# dragged box has one trailing 2-DIP pad, uses the original press grab offset,
# and compares against target layouts while the painted rows are clamped.
grep -Fq 'drag_grab_dy_ = press_screen_pt.y() - previous_bounds.y();' \
  "$rail_source"
test "$(grep -Fc \
  '(screen - press_screen_pt_).LengthSquared() <=' "$rail_source")" -eq 4
grep -Fq \
  'const int drag_hit_test_height = block_height + kTabVerticalPadding;' \
  "$rail_source"
grep -Fq 'std::max(height, dragged_view_bottom)' "$rail_source"
grep -Fq 'collection_->target_layout().GetLayoutFor(drag_view);' \
  "$rail_source"
update_row_drag_block="$(sed -n \
  '/void CmuxRail::UpdateRowDrag(/,/void CmuxRail::EndRowDrag(/p' \
  "$rail_source")"
printf '%s\n' "$update_row_drag_block" |
  grep -Fq 'outer_target_layout.GetLayoutFor(group);'
if printf '%s\n' "$update_row_drag_block" |
    grep -Eq '(current|group)->GetBoundsInScreen\(\)'; then
  echo "group drag hit testing must use target-layout bounds" >&2
  exit 1
fi

# Successful drops snap only the dragged views into their final collection;
# surrounding rows retain the copied animating-layout-manager transition.
grep -Fq 'snap_drag_views_to_target_.insert(drag_views_.begin(),' \
  "$rail_source"
grep -Fq 'snap_drag_views_to_target_.insert(drag_group_.begin(),' \
  "$rail_source"
test "$(grep -Fc 'bool ShouldSnapToTarget(' "$rail_source")" -eq 2
grep -Fq 'rail_->ShouldSnapDragViewToTarget(child_view);' "$rail_source"

# The ScrollView owns a plain contents wrapper around the outer animating
# collection. Mirror every animation frame's preferred height into that wrapper
# so open/close motion is not clipped or delayed until animation completion.
grep -Fq 'void CmuxRail::ScheduleContentGeometrySync()' "$rail_source"
collection_layout_block="$(sed -n \
  '/class CmuxRailCollectionView/,/class CmuxRailGroupView/p' \
  "$rail_source")"
printf '%s\n' "$collection_layout_block" |
  grep -Fq 'rail_->ScheduleContentGeometrySync();'
printf '%s\n' "$collection_layout_block" |
  grep -Fq 'layout_manager_->is_animating()'

# Keep the New Tab, Ghostty appearance, and toolbar integration contract close
# to the Helium provenance checks that govern its source patches.
python3 -B "$root/scripts/test-customization-parity.py"

# Helium-derived patches must keep their source-level attribution. The project
# license has a Manaflow scope preamble, while Helium's GPLv3 copy is preserved
# separately and validated with the release-license checks below.
for settings_patch in \
  "$root/patches/helium-settings-chromium-149.patch" \
  "$root/patches/helium-settings-chromium-151.patch"; do
  grep -Fq 'From: Helium contributors' "$settings_patch"
  grep -Fq 'Modified for cmux on 2026-07-21' "$settings_patch"
  grep -Fq 'GPL-3.0-only' "$settings_patch"
  grep -Fq 'kHeliumRoundedFrame[] = "helium.browser.rounded_frame"' \
    "$settings_patch"
  grep -Fq 'RegisterBooleanPref(prefs::kHeliumRoundedFrame, true)' \
    "$settings_patch"
done
# The repository license adds Manaflow's scope/preamble, so verify the vendored
# Helium GPL text by its pinned digest instead of requiring byte equality.
helium_license_sha="$(
  shasum -a 256 "$root/third_party/helium/LICENSE" | awk '{print $1}'
)"
test "$helium_license_sha" = \
  "7952925b3b9f6577548f235839d53694179d519cdf2af3b6fe996273cb239b2e"

# The optional content frame is default-on and deliberately shares Helium's
# geometry on browser and terminal surfaces. Keep the pref, live propagation,
# attached-edge/window-corner rules, fullscreen suppression, and custom
# settings bridge together.
rounded_surface="$root/overlay/chrome/browser/cmux_term/cmux_surface.h"
rounded_browser="$root/overlay/chrome/browser/cmux_term/cmux_browser_pane.cc"
rounded_terminal="$root/overlay/chrome/browser/cmux_term/cmux_terminal_pane.mm"
rounded_terminal_cross="$root/overlay/chrome/browser/cmux_term/cmux_terminal_pane_linux.cc"
rounded_window="$root/overlay/chrome/browser/cmux_term/cmux_views.cc"
rounded_settings="$root/overlay/chrome/browser/cmux_term/cmux_configure_page.cc"
rounded_browser_setter="$(sed -n \
  '/^  void SetRoundedFrame(const RoundedFrameGeometry& geometry) override {$/,/^  void OnWebViewFocused/p' \
  "$rounded_browser")"
rounded_terminal_setter="$(sed -n \
  '/^void CmuxTerminalSurface::SetRoundedFrame($/,/^void CmuxTerminalSurface::OnCmuxTerminalReplay/p' \
  "$rounded_terminal")"
rounded_terminal_cross_setter="$(sed -n \
  '/^  void SetRoundedFrame(const RoundedFrameGeometry& geometry) override {$/,/^  void OnCmuxTerminalReplay(/p' \
  "$rounded_terminal_cross")"
for rounded_frame_setter in \
  "$rounded_browser_setter" \
  "$rounded_terminal_setter" \
  "$rounded_terminal_cross_setter"; do
  grep -Fq 'geometry.enabled' <<<"$rounded_frame_setter"
  grep -Fq ': nullptr);' <<<"$rounded_frame_setter"
done
frame_geometry_block="$(sed -n \
  '/^  RoundedFrameGeometry FrameGeometryForPane(/,/^  int StripWidthForLayout()/p' \
  "$rounded_window")"
grep -Fq 'sidebar_metrics::IsVisible(layout_config_.sidebar_mode)' \
  <<<"$frame_geometry_block"
grep -Fq 'RailWidthForMode(layout_config_.sidebar_mode) > 0' \
  <<<"$frame_geometry_block"
if grep -Eq 'sidebar_collapsed|RailWidthForLayout' \
    <<<"$frame_geometry_block"; then
  echo "rounded frame geometry must use the current sidebar mode API" >&2
  exit 1
fi
rail_resize_block="$(sed -n \
  '/^class RailResizeHandle : public views::View {$/,/^class PendingFocusedWebTabCloseObserver/p' \
  "$rounded_window")"
grep -Fq 'sidebar_metrics::kExpandedResizeAreaWidth' <<<"$rail_resize_block"
grep -Fq 'ui::mojom::CursorType::kColumnResize' <<<"$rail_resize_block"
if grep -Eq 'OnPaint|SetPaintToLayer|SetDividerColor' \
    <<<"$rail_resize_block"; then
  echo "sidebar resize edge must remain an invisible input target" >&2
  exit 1
fi
grep -Fq 'kRoundedFrameInset = 3' "$rounded_surface"
grep -Fq 'kRoundedFrameCornerRadius = 8' "$rounded_surface"
grep -Fq 'kRoundedFrameOutlineThickness = 1' "$rounded_surface"
grep -Fq 'bottom_left_radius = kRoundedFrameCornerRadius' "$rounded_surface"
grep -Fq 'bottom_right_radius = kRoundedFrameCornerRadius' "$rounded_surface"
grep -Fq 'kColorToolbarContentAreaSeparator' "$rounded_browser"
grep -Fq 'gfx::Insets::TLBR(' "$rounded_browser"
grep -Fq 'geometry.bottom_right_radius' "$rounded_browser"
grep -Fq 'geometry.bottom_left_radius' "$rounded_browser"
grep -Fq 'rounded_view_.layer.cornerRadius' "$rounded_terminal"
grep -Fq 'rounded_mask_layer_.path = mask_path' "$rounded_terminal"
grep -Fq 'rounded_outline_layer_.path = outline_path' "$rounded_terminal"
grep -Fq 'SkPath::RRect' "$rounded_terminal_cross"
grep -Fq 'geometry.bottom_right_radius' "$rounded_terminal_cross"
grep -Fq 'geometry.bottom_left_radius' "$rounded_terminal_cross"
grep -Fq 'base::mac::MacOSMajorVersion() >= 26 ? 17 : 12' "$rounded_window"
grep -Fq 'system_window_radius - kRoundedFrameInset' "$rounded_window"
grep -Fq 'geometry.left_inset = left_chrome_attached ? 0' "$rounded_window"
grep -Fq 'geometry.right_inset = right_chrome_attached ? 0' "$rounded_window"
grep -Fq '/*top_chrome_attached=*/true' "$rounded_window"
grep -Fq 'touches_bottom && touches_left && !rail_on_left' "$rounded_window"
grep -Fq 'touches_bottom && touches_right && !rail_on_right' "$rounded_window"
grep -Fq '!widget || !widget->IsFullscreen()' "$rounded_window"
grep -Fq 'PrefChangeRegistrar rounded_frame_pref_change_registrar_' \
  "$rounded_window"
grep -Fq 'setRoundedFrameEnabled' "$rounded_settings"
grep -Fq 'Show rounded content frame' "$rounded_settings"
grep -Fq 'Off: no inset, corner mask, or outline.' "$rounded_settings"

# Stable Finder/Dock launches retain the existing ~/.cmux-profile, while a
# tagged dogfood bundle honors its non-stable CrProductDirName. This keeps the
# stable app on one ProcessSingleton/profile without merging independent builds.
apply_script="$root/scripts/apply.sh"
grep -Fq "run_anchor = ('#if BUILDFLAG(IS_MAC)" "$apply_script"
grep -Fq "'auto RunMacNotificationService('" "$apply_script"
grep -Fq 's = s.replace(run_anchor, run_add, 1)' "$apply_script"
! grep -Fq "run_anchor = 'auto RunScreenAIServiceFactory('" "$apply_script"
grep -Fq 'chrome::GetDefaultUserDataDirectory(&cmux_default_profile)' "$apply_script"
grep -Fq 'cmux_default_profile.BaseName().value() !=' "$apply_script"
grep -Fq 'FILE_PATH_LITERAL(\"cmux\")' "$apply_script"
grep -Fq 'cmux_cl->AppendSwitchPath(switches::kUserDataDir,' "$apply_script"
grep -Fq 'PROFILE="${CMUX_PROFILE:-$HOME/.cmux-profile}"' "$root/scripts/run.sh"

# A real Browser hosted by the custom cmux window has LocationBarView page
# actions but no BrowserView-owned ZoomBubbleCoordinator. The source patch must
# guard both the active-tab notification and queued activation paths.
grep -Fq "p = 'chrome/browser/ui/views/page_action/zoom_view.cc'" "$apply_script"
grep -Fq 'cmux: a Browser hosted by a custom BrowserWindow has no BrowserView-only' "$apply_script"
grep -Fq 'cmux: queued activation may outlive the optional BrowserView feature.' "$apply_script"

# A flipped NSView reverses Core Animation's visual top/bottom gravity. Keep
# retained resize frames and mouse hit-testing anchored to the same top-left.
ghostty_view="$root/overlay/chrome/browser/cmux_term/cmux_ghostty.mm"
grep -Fq '[self.layer contentsAreFlipped]' "$ghostty_view"
grep -Fq '? kCAGravityBottomLeft' "$ghostty_view"
grep -Fq ': kCAGravityTopLeft' "$ghostty_view"

# The resize trace is test-only, but the path arrives through the launched
# app's environment. Resolve and hold its harness-owned directory before
# creating the exact log file, and record the actual layer mutations so the UI
# test cannot miss a clear or scale-first presentation between size callbacks.
grep -Fq 'realpath(resizeTraceParent.fileSystemRepresentation,' "$ghostty_view"
grep -Fq 'NSString* const resizeTraceBasename = @"terminal-resize-trace.log";' \
  "$ghostty_view"
grep -Fq 'isEqualToString:resizeTraceBasename' "$ghostty_view"
grep -Fq 'isEqualToString:@"/private/tmp"' "$ghostty_view"
grep -Fq 'hasPrefix:resizeTraceSessionPrefix' "$ghostty_view"
grep -Fq 'O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW' "$ghostty_view"
grep -Fq 'resizeTraceParentStat.st_uid == geteuid()' "$ghostty_view"
grep -Fq 'openat(' "$ghostty_view"
grep -Fq 'O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW |' \
  "$ghostty_view"
grep -Fq 'O_NONBLOCK' "$ghostty_view"
grep -Fq 'S_ISREG(resizeTraceStat.st_mode)' "$ghostty_view"
grep -Fq 'resizeTraceStat.st_uid == geteuid()' "$ghostty_view"
grep -Fq '[self appendResizeTraceEvent:@"frame_presented"]' "$ghostty_view"
grep -Fq '[self appendResizeTraceEvent:@"contents_cleared"]' "$ghostty_view"
grep -Fq '[self appendResizeTraceEvent:@"resize_settled"]' "$ghostty_view"
test "$(grep -Fc 'self.layer.contents = nil;' "$ghostty_view")" -eq \
  "$(grep -Fc '[self appendResizeTraceEvent:@"contents_cleared"]' "$ghostty_view")"

# Theme extraction replies run after blocking Ghostty/config work. The custom
# window can remain alive while its Profile's keyed services shut down, so
# object WeakPtrs alone do not make ThemeService access safe.
cmux_views="$root/overlay/chrome/browser/cmux_term/cmux_views.cc"
tab_drag_source="$root/overlay/chrome/browser/cmux_term/cmux_tab_drag.cc"
grep -Fq 'params.child = true;' "$tab_drag_source"
grep -Fq 'params.parent = host_widget->GetNativeView();' "$tab_drag_source"
if grep -Fq 'params.z_order =' "$tab_drag_source"; then
  echo "drag overlay must not use an application-independent always-on-top level" >&2
  exit 1
fi
grep -Fq 'void CmuxTabDragController::CancelImmediately()' "$tab_drag_source"
grep -Fq 'CancelActiveDragForHostState();' "$cmux_views"
grep -Fq 'host deactivation destroys drag overlay' "$cmux_views"
grep -Fq 'public ProfileObserver {' "$cmux_views"
grep -Fq 'profile_observation_.Observe(profile_);' "$cmux_views"
grep -Fq 'void OnProfileWillBeDestroyed(Profile* profile) override {' "$cmux_views"
grep -Fq 'profile_services_available_ = false;' "$cmux_views"
grep -Fq 'weak_factory_.InvalidateWeakPtrs();' "$cmux_views"
grep -Fq 'if (!profile_services_available_) {' "$cmux_views"
grep -Fq 'if (observing_tui_client_) {' "$cmux_views"
grep -Fq 'observing_tui_client_ = false;' "$cmux_views"
grep -Fq 'TabStripModelObserver::StopObservingAll(this);' "$cmux_views"
grep -Fq 'profile_ = nullptr;' "$cmux_views"
grep -Fq 'if (!profile_services_available_ || !profile_) {' "$cmux_views"
grep -Fq 'void CompleteViewsStartupAfterWorkspaceBrowserCreated() {' "$cmux_views"
grep -Fq 'CompleteViewsStartupAfterWorkspaceBrowserCreated();' "$cmux_views"
test "$(grep -Fc 'CompleteViewsStartupAfterWorkspaceBrowserCreated();' "$cmux_views")" -eq 2
grep -Fq 'if (!id || *id == 0 || !key || !IsCmuxUuid(*key) || !name) {' \
  "$root/overlay/chrome/browser/cmux_term/cmux_tui_client.cc"
grep -Fq 'const std::string durable_workspace_token =' "$cmux_views"
grep -Fq 'if (durable_workspace_token.empty()) {' "$cmux_views"
grep -Fq 'FinishStartupBrowserSuppression();' "$cmux_views"
startup_window_prefix="$(sed -n \
  '/void ShowViewsWebWindowInternal(bool force_new)/,/cmux-views: niri strip up/p' \
  "$cmux_views")"
! grep -Fq 'FinishStartupBrowserSuppression();' <<<"$startup_window_prefix"
! grep -Fq 'ReleaseViewsKeepAlive();' <<<"$startup_window_prefix"

# Cross-branch APFS clones can carry objects newer than a rolled-back header.
# Content comparison plus fresh destination mtimes prevents Ninja from reusing
# those ABI-incompatible objects.
sync_script="$root/scripts/sync.sh"
grep -Fq 'rsync -az --ignore-times --no-times --delete' "$sync_script"
grep -Fq 'rsync -az --ignore-times --no-times overlay/' "$sync_script"
grep -Fq 'patches/ "$HOST:$SRC/.cmux-patches/"' "$sync_script"

python3 -B "$root/scripts/test-benchmark-cmux-browser-scaling.py"
python3 -B "$root/scripts/test-apply-utility-services-compat.py"

# Helium-derived UI patches are selected from the builder's actual Chromium
# major. Chromium 151 is cmux's current target; 149/150 remain compatibility
# bases for older builders.
grep -Fq 'CHROMIUM_MAJOR="$(ssh -o BatchMode=yes "$HOST"' "$apply_script"
grep -Fq 'HELIUM_SETTINGS_PATCH=".cmux-patches/helium-settings-chromium-149.patch"' "$apply_script"
grep -Fq 'HELIUM_SETTINGS_PATCH=".cmux-patches/helium-settings-chromium-151.patch"' "$apply_script"
grep -Fq "echo 'helium-settings: patched (GPL-3.0-only; see THIRD_PARTY_NOTICES.md)'" "$apply_script"

settings_149="$root/patches/helium-settings-chromium-149.patch"
settings_151="$root/patches/helium-settings-chromium-151.patch"
for settings_patch in "$settings_149" "$settings_151"; do
  test -s "$settings_patch"
  grep -Fq 'From: Helium contributors <https://github.com/imputnet/helium>' "$settings_patch"
  grep -Fq 'Primary upstream authors:' "$settings_patch"
  grep -Fq 'Helium-derived modifications are licensed under GPL-3.0-only.' "$settings_patch"
  grep -Fq '         <template is="dom-if" if="[[enableLiveCaption_]]">' "$settings_patch"
  grep -Fq '+  margin-inline-start: 96px;' "$settings_patch"
  grep -Fq '+    --cr-fallback-color-on-secondary-container: var(--google-blue-100);' \
    "$settings_patch"
  if grep -Fq -- '-        <template is="dom-if" if="[[enableLiveCaption_]]">' \
      "$settings_patch"; then
    echo "Helium settings patch must preserve the Live Caption control: $settings_patch" >&2
    exit 1
  fi
  if grep -Fq '+  margin: 0 0 0 96px;' "$settings_patch"; then
    echo "Helium settings hue offset must use logical margins: $settings_patch" >&2
    exit 1
  fi
done

win_apply="$root/scripts/apply_win_chrome.py"
grep -Fq 'chromium_major = version.get("MAJOR")' "$win_apply"
grep -Fq 'patch_dir = Path(os.environ.get("CMUX_PATCH_DIR", ".cmux-patches"))' "$win_apply"
grep -Fq 'patch_dir / "helium-settings-chromium-149.patch"' "$win_apply"
grep -Fq 'patch_dir / "helium-settings-chromium-151.patch"' "$win_apply"
grep -Fq 'git_apply_check("--reverse", "--check")' "$win_apply"

test -s "$root/LICENSE"
test -s "$root/third_party/helium/LICENSE"
# The project license carries Manaflow's scope and dual-licensing preamble.
# Preserve Helium's own GPLv3 copy separately, including its upstream
# application-example attribution.
grep -Fq 'GNU GENERAL PUBLIC LICENSE' "$root/LICENSE"
grep -Fq 'Version 3, 29 June 2007' "$root/LICENSE"
grep -Fq 'GNU GENERAL PUBLIC LICENSE' "$root/third_party/helium/LICENSE"
grep -Fq 'Copyright (C) 2025 The Helium Authors' \
  "$root/third_party/helium/LICENSE"
grep -Fq 'modification date, and GPL-3.0-only notice alongside each diff.' \
  "$root/THIRD_PARTY_NOTICES.md"
grep -Fq 'Chromium 151 is the current cmux target;' \
  "$root/THIRD_PARTY_NOTICES.md"
grep -Fq 'media-toolbar adaptation points are `patches/helium-new-tab.patch` and' \
  "$root/THIRD_PARTY_NOTICES.md"
grep -Fq 'Revision: 8030a8a3050151a141c06cc5a85f95cbbdc42a25' \
  "$root/third_party/helium/README.chromium"

# License metadata must be present in Chromium's generated credits inputs and
# in every distributed app/update payload, not only in the source repository.
sync_script="$root/scripts/sync.sh"
grep -Fq 'third_party/helium/ "$HOST:$SRC/third_party/helium/"' "$sync_script"
reverse_line="$(grep -n "git apply --reverse '\$PREVIOUS_HELIUM_SETTINGS_PATCH'" \
  "$sync_script" | cut -d: -f1)"
patch_sync_line="$(grep -n 'patches/ "$HOST:$SRC/.cmux-patches/"' \
  "$sync_script" | cut -d: -f1)"
test -n "$reverse_line"
test -n "$patch_sync_line"
test "$reverse_line" -lt "$patch_sync_line"
grep -Fq 'warm tree matches neither applied nor pristine state' "$sync_script"

deploy_script="$root/scripts/deploy.sh"
grep -Fq 'NOTICE_RES="$DEST/Contents/Resources/cmux-licenses"' "$deploy_script"
grep -Fq 'cp LICENSE THIRD_PARTY_NOTICES.md "$NOTICE_RES/"' "$deploy_script"
grep -Fq 'LAST_USED_MARKER="$FINAL_DEST.cmux-last-used"' "$deploy_script"
grep -Fq 'SIGN_WORK="$STAGING_ROOT/signing"' "$deploy_script"
grep -Fq 'rm -rf -- "$SIGN_WORK"' "$deploy_script"
marker_line="$(grep -n 'LAST_USED_MARKER=' "$deploy_script" | cut -d: -f1)"
verify_line="$(grep -n 'codesign --verify --deep --strict' "$deploy_script" | cut -d: -f1)"
promote_line="$(grep -n '^promote_staged_app$' "$deploy_script" | cut -d: -f1)"
test "$marker_line" -gt "$verify_line"
test "$marker_line" -gt "$promote_line"
(
  unset DEST
  CMUX_SIGN_DRY_RUN=1 bash "$deploy_script" >/dev/null
)

linux_release="$root/scripts/build-release-linux.sh"
grep -Fq 'notice_root="$package_root/cmux-licenses"' "$linux_release"
grep -Fq 'cmux-licenses/third_party/helium/LICENSE' "$linux_release"

windows_release="$root/scripts/build-release-windows.ps1"
grep -Fq "'cmux-licenses\\THIRD_PARTY_NOTICES.md: %(ChromeDir)s\\cmux-licenses\\'," \
  "$windows_release"
grep -Fq "'cmux-licenses\\third_party\\helium\\LICENSE: %(ChromeDir)s\\cmux-licenses\\third_party\\helium\\'," \
  "$windows_release"
grep -Fq "staged Windows root is missing the Helium license" "$windows_release"

bash "$root/scripts/test-deploy-bundle-identity.sh"

echo "build dogfood tests passed"
