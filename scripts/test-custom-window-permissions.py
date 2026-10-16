#!/usr/bin/env python3
"""Regression coverage for custom BrowserWindow permission compatibility."""

import hashlib
import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile


sys.dont_write_bytecode = True


ROOT = Path(__file__).resolve().parents[1]
PATCHER_PATH = ROOT / "patches/custom_window_permissions.py"
RESTORER_PATH = ROOT / "scripts/restore-custom-window-permissions.py"
SPEC = importlib.util.spec_from_file_location(
    "custom_window_permissions", PATCHER_PATH
)
assert SPEC and SPEC.loader
PATCHER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PATCHER)


# Every pinned-M151 upstream anchor must transform exactly once and a warm
# workspace re-apply must be byte-for-byte idempotent.
for relative_path, replacements in PATCHER.REPLACEMENTS.items():
    pristine = "\n".join(replacement.old for replacement in replacements)
    patched, changed = PATCHER.patch_source(relative_path, pristine)
    assert changed == [replacement.label for replacement in replacements]
    for replacement in replacements:
        assert replacement.new in patched, replacement.label

    reapplied, changed_again = PATCHER.patch_source(relative_path, patched)
    assert reapplied == patched, relative_path
    assert changed_again == [], relative_path

    restored, restored_labels = PATCHER.restore_source(relative_path, patched)
    assert restored == pristine, relative_path
    assert restored_labels == [
        replacement.label for replacement in reversed(replacements)
    ]

    restored_again, restored_again_labels = PATCHER.restore_source(
        relative_path, restored
    )
    assert restored_again == pristine, relative_path
    assert restored_again_labels == [], relative_path


# Global Chromium transformations must retain action-specific anchors in
# ordinary BrowserView windows and use the location bar only as cmux's custom
# BrowserWindow fallback.
replacement_new_by_label = {
    replacement.label: replacement.new
    for replacements in PATCHER.REPLACEMENTS.values()
    for replacement in replacements
}
file_system_anchor = replacement_new_by_label[
    "file system access generic bubble anchor"
]
assert "if (browser_view) {" in file_system_anchor
assert "GetBubbleAnchor(std::nullopt)" in file_system_anchor
assert (
    "cmux::GetLocationBarForWebContents(browser, web_contents)"
    in file_system_anchor
)
file_system_title = replacement_new_by_label[
    "file system access generic accessible title"
]
assert "return page_action_view->GetTooltipText();" in file_system_title
assert "actions::ActionManager::Get().FindAction(" in file_system_title
media_anchor = replacement_new_by_label[
    "media remoting generic bubble anchor"
]
assert "GetPinnedToolbarActions()" in media_anchor
assert "GetBubbleAnchor(kActionRouteMedia)" in media_anchor
assert (
    "cmux::GetLocationBarForWebContents(browser, web_contents_)"
    in media_anchor
)
for location_label in (
    "permission factory fullscreen location bar",
    "permission factory location bar",
    "bubble anchor generic location bar",
    "permission prompt requesting WebContents anchor",
    "permission decision generic chip controller",
):
    assert (
        "cmux::GetLocationBarForWebContents"
        in replacement_new_by_label[location_label]
    ), location_label
chooser_anchor = replacement_new_by_label[
    "chooser requesting WebContents anchor helper"
]
assert "GetPageInfoAnchorConfiguration(" in chooser_anchor
assert "contents);" in chooser_anchor
for chooser_gate_label in (
    "extension chooser visible WebContents gate",
    "chooser visible WebContents gate",
):
    assert "cmux::IsWebContentsActiveOrVisible(browser, contents)" in (
        replacement_new_by_label[chooser_gate_label]
    )


# Ambiguous warm state must fail instead of accepting duplicate stale edits.
first_path, first_replacements = next(iter(PATCHER.REPLACEMENTS.items()))
first_pristine = "\n".join(
    replacement.old for replacement in first_replacements
)
first_patched, _ = PATCHER.patch_source(first_path, first_pristine)
try:
    PATCHER.patch_source(
        first_path, first_patched + first_replacements[0].new
    )
except AssertionError as error:
    assert "expected exactly one patched or upstream anchor" in str(error)
else:
    raise AssertionError("duplicate permission transformation was accepted")


# The refresh helper must restore the definition already present on a warm
# builder. This legacy fixture intentionally lacks restore_tree(), exercising
# the migration path from the first version of the patcher. Once restored, a
# changed replacement can apply without seeing the obsolete v1 output.
with tempfile.TemporaryDirectory() as temporary_directory:
    warm_root = Path(temporary_directory)
    (warm_root / "chrome").mkdir()
    version_file = warm_root / "chrome/VERSION"
    version_file.write_text("MAJOR=151\n")
    previous_patch_dir = warm_root / ".cmux-patches"
    previous_patch_dir.mkdir()
    previous_patcher = previous_patch_dir / "custom_window_permissions.py"
    previous_patcher.write_text(
        """from collections import namedtuple
Replacement = namedtuple("Replacement", "label old new")
REPLACEMENTS = {
    "upstream.cc": (
        Replacement(
            "legacy permission anchor",
            "upstream-anchor\\n",
            "upstream-anchor\\ncmux-v1\\n",
        ),
    ),
}
"""
    )
    warm_source = warm_root / "upstream.cc"
    warm_source.write_text("upstream-anchor\ncmux-v1\n")

    subprocess.run(
        [sys.executable, "-B", str(RESTORER_PATH), str(warm_root)],
        check=True,
        capture_output=True,
        text=True,
    )
    assert warm_source.read_text() == "upstream-anchor\n"

    subprocess.run(
        [sys.executable, "-B", str(RESTORER_PATH), str(warm_root)],
        check=True,
        capture_output=True,
        text=True,
    )
    assert warm_source.read_text() == "upstream-anchor\n"

    version_file.write_text("MAJOR=150\n")
    warm_source.write_text("Chromium-150-layout\n")
    subprocess.run(
        [sys.executable, "-B", str(RESTORER_PATH), str(warm_root)],
        check=True,
        capture_output=True,
        text=True,
    )
    assert warm_source.read_text() == "Chromium-150-layout\n"

    version_file.write_text("MAJOR=151\n")
    warm_source.write_text("upstream-anchor\n")
    warm_source.write_text(
        warm_source.read_text().replace(
            "upstream-anchor\n", "upstream-anchor\ncmux-v2\n"
        )
    )
    assert warm_source.read_text() == "upstream-anchor\ncmux-v2\n"


OVERLAY = ROOT / "overlay/chrome/browser/cmux_term"
SURFACE_H = (OVERLAY / "cmux_surface.h").read_text()
SURFACE_CC = (OVERLAY / "cmux_surface.cc").read_text()
WINDOW_H = (OVERLAY / "cmux_browser_window.h").read_text()
WINDOW_CC = (OVERLAY / "cmux_browser_window.cc").read_text()
PANE = (OVERLAY / "cmux_browser_pane.cc").read_text()
VIEWS = (OVERLAY / "cmux_views.cc").read_text()
APPLY = (ROOT / "scripts/apply.sh").read_text()
WINDOWS_APPLY = (ROOT / "scripts/apply_win_chrome.py").read_text()
SYNC = (ROOT / "scripts/sync.sh").read_text()
UIDEMO = (ROOT / "scripts/uidemo.sh").read_text()
WINDOWS_RELEASE = (ROOT / "scripts/build-release-windows.ps1").read_text()
ROLLBACK_MIRROR = (
    ROOT / "patches/helium-settings-chromium-151.patch"
).read_text()

assert "GetWebContentsModalDialogHost();" in SURFACE_H
assert "CmuxSurface::GetWebContentsModalDialogHost()" in SURFACE_CC
assert "GetWorkspaceWebContentsModalDialogHost(" in WINDOW_H
assert "host_->GetWorkspaceWebContentsModalDialogHost(" in WINDOW_CC
assert "GetWorkspaceWebContentsModalDialogHost(" in VIEWS
assert "GetWorkspaceLocationBar(" in WINDOW_H
assert "GetLocationBarForWebContents(" in WINDOW_H
assert "IsWorkspaceWebContentsVisible(" in WINDOW_H
assert "IsWebContentsActiveOrVisible(" in WINDOW_H
assert "BrowserWindows()" in WINDOW_CC
assert "host_->GetWorkspaceLocationBar(workspace_, web_contents)" in WINDOW_CC
assert "host_->IsWorkspaceWebContentsVisible(workspace_, web_contents)" in (
    WINDOW_CC
)
assert "GetWorkspaceLocationBar(" in VIEWS
assert "IsWorkspaceWebContentsVisible(" in VIEWS
assert "!surface_view->GetVisibleBounds().IsEmpty()" in VIEWS
assert "CMUX_PERMISSION_SELFTEST PASS" in VIEWS
assert "modal_host->GetMaximumDialogSize()" in VIEWS
permission_selftest_start = VIEWS.index("void RunPermissionHostSelfTest(")
permission_selftest_end = VIEWS.index("\n private:", permission_selftest_start)
permission_selftest = VIEWS[permission_selftest_start:permission_selftest_end]
assert "for (PaneId candidate_pane : model_.PanesOf(ws_))" in permission_selftest
assert "NewTabInPane(focused, SurfaceKind::kWeb);" in permission_selftest
assert "SplitOrientation::kVertical" in permission_selftest
assert "checked_visible_non_active_location_bar" in permission_selftest
assert "candidate_model ? candidate_model->SelectedTab() : nullptr" in (
    permission_selftest
)
assert "!selected_surface_view->GetVisibleBounds().IsEmpty()" in (
    permission_selftest
)
assert "!candidate_surface_view->GetVisibleBounds().IsEmpty()" in (
    permission_selftest
)
assert "candidate_model->FindTab(candidate_model->selected)" not in (
    permission_selftest
)
assert "GetLocationBarForWebContents(browser, candidate_contents)" in (
    permission_selftest
)
assert "IsWebContentsActiveOrVisible(browser, candidate_contents)" in (
    permission_selftest
)
assert "FocusColumn(0)" not in permission_selftest
assert "class ModalPositionObserver final" in permission_selftest
assert "pane_view->SetBoundsRect(clipped_pane_bounds);" in permission_selftest
assert "clipped_maximum_size.width()" in permission_selftest
assert "modal host ignored ancestor clipping" in permission_selftest
assert "modal host ignored visible movement" in permission_selftest
assert "original_pane_bounds.bottom() <" in permission_selftest
assert "visible_content_bounds.bottom()" in permission_selftest
assert "translated_pane_bounds.Offset(translation_x, translation_y);" in (
    permission_selftest
)
assert "const gfx::Size translated_maximum_size =" in permission_selftest
assert (
    "CHECK_LE(translated_maximum_size.width(), maximum_size.width() + 1);"
    in permission_selftest
)
assert (
    "CHECK_LE(translated_maximum_size.height(), maximum_size.height() + 1);"
    in permission_selftest
)
assert "fully clipped pane remained visible" in permission_selftest
assert "fully clipped modal host remained active" in permission_selftest
assert "!GetVisibleContentBoundsInWidget().IsEmpty()" in PANE

assert "class CmuxWebContentsModalDialogHost final" in PANE
assert "OnHostDestroying" in PANE
assert "OnPositionRequiresUpdate" in PANE
modal_host_start = PANE.index("class CmuxWebContentsModalDialogHost final")
modal_host_end = PANE.index(
    "\n// ---- Chrome pane:", modal_host_start
)
modal_host = PANE[modal_host_start:modal_host_end]
modal_host_destructor_start = modal_host.index(
    "~CmuxWebContentsModalDialogHost() override"
)
modal_host_destructor_end = modal_host.index(
    "\n  // web_modal::ModalDialogHost:", modal_host_destructor_start
)
modal_host_destructor = modal_host[
    modal_host_destructor_start:modal_host_destructor_end
]
assert "visible_bounds_observation_.reset();" in modal_host_destructor
assert "content_view_observation_.Reset();" in modal_host_destructor
assert "ancestor_view_observations_.RemoveAllObservations();" in (
    modal_host_destructor
)
assert "widget_observation_.Reset();" in modal_host_destructor
assert modal_host_destructor.index(
    "visible_bounds_observation_.reset();"
) < modal_host_destructor.index("observer_list_.Notify(")
assert "ancestor_view_observations_" in PANE
assert "void OnViewBoundsChanged(views::View*) override" in PANE
assert "void OnViewLayerTransformed(views::View*) override" in PANE
assert "RefreshAncestorObservations();" in PANE
assert "RefreshWidgetObservation();" in PANE
assert "if (observed_view == content_view_) {" in PANE
assert "widget_observation_.IsObservingSource(widget)" in PANE
assert "ScopedNotifyObserversOnVisibleBoundsChanged" in PANE
assert "OnViewVisibleBoundsChanged" in PANE
assert "GetVisibleContentBoundsInWidget()" in PANE
assert "content_view_->GetVisibleBounds()" in PANE
assert "GetMaximumDialogSize() override" in PANE
assert "modal_dialog_host_.reset();" in PANE
assert (
    "browser_->GetFeatures().content_setting_bubble_model_delegate()" in PANE
)
assert "StubBubbleModelDelegate" not in PANE
assert "location_bar->Update(nullptr);" in WINDOW_CC
assert "location_bar->HasSecurityStateChanged()" in WINDOW_CC
assert "location_bar->ResetTabState(contents);" in WINDOW_CC
page_action_update_start = WINDOW_CC.index(
    "void CmuxBrowserWindow::UpdatePageActionIcon("
)
page_action_update_end = WINDOW_CC.index(
    "\nautofill::AutofillBubbleHandler*",
    page_action_update_start,
)
page_action_update = WINDOW_CC[
    page_action_update_start:page_action_update_end
]
assert "for (int index = 0; index < tabs->count(); ++index)" in (
    page_action_update
)
assert "IsWebContentsVisible(contents)" in page_action_update
assert "GetLocationBarForWebContents(contents)" in page_action_update
assert "location_bar->Update(contents);" in page_action_update

assert "python3 .cmux-patches/custom_window_permissions.py" in APPLY
assert 'if [ "$CHROMIUM_MAJOR" -ge 151 ]; then' in APPLY
assert "'custom_window_permissions.py'," in WINDOWS_APPLY
assert "permission_patcher['patch_tree'](chromium_src)" in WINDOWS_APPLY
assert "if chromium_major >= 151:" in WINDOWS_APPLY
for relative_path in PATCHER.REPLACEMENTS:
    mirror_header = (
        f"diff --git a/{relative_path} b/{relative_path}"
    )
    assert ROLLBACK_MIRROR.count(mirror_header) == 1, relative_path
assert "allowing rollback to branches that predate the permission patcher" in (
    ROLLBACK_MIRROR
)
definition_hash = hashlib.sha256()
for relative_path, replacements in PATCHER.REPLACEMENTS.items():
    definition_hash.update(relative_path.encode())
    definition_hash.update(b"\0")
    for replacement in replacements:
        for value in (replacement.label, replacement.old, replacement.new):
            definition_hash.update(value.encode())
            definition_hash.update(b"\0")
definition_digest = definition_hash.hexdigest()
assert f"permission definition SHA-256:\n    {definition_digest}" in (
    ROLLBACK_MIRROR
)
first_mirror_path = next(iter(PATCHER.REPLACEMENTS))
first_mirror_header = (
    f"diff --git a/{first_mirror_path} b/{first_mirror_path}\n"
)
permission_mirror = (
    first_mirror_header
    + ROLLBACK_MIRROR.split(first_mirror_header, 1)[1]
)
mirror_digest = hashlib.sha256(permission_mirror.encode()).hexdigest()
assert f"permission mirror SHA-256:\n    {mirror_digest}" in ROLLBACK_MIRROR
assert SYNC.index(
    "git apply --reverse '$PREVIOUS_HELIUM_SETTINGS_PATCH'"
) < SYNC.index(
    '< "$CMUX_SCRIPTS_DIR/restore-custom-window-permissions.py"'
)
assert SYNC.index("restore-custom-window-permissions.py") < SYNC.index(
    '--delete patches/ "$HOST:$SRC/.cmux-patches/"'
)
assert UIDEMO.index("restore-custom-window-permissions.py") < UIDEMO.index(
    '--delete patches/ "$HOST:$SRC/.cmux-patches/"'
)
assert UIDEMO.index(
    "git apply --reverse '$PREVIOUS_HELIUM_SETTINGS_PATCH'"
) < UIDEMO.index(
    '< "$CMUX_SCRIPTS_DIR/restore-custom-window-permissions.py"'
)
assert WINDOWS_RELEASE.index(
    "restore-custom-window-permissions.py"
) < WINDOWS_RELEASE.index(
    "Remove-Item -LiteralPath $patches -Recurse -Force"
)

print("custom BrowserWindow permission compatibility: PASS")
