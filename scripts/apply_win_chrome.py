import os
import runpy
import subprocess
from pathlib import Path

chromium_src = Path(os.environ.get("CHROMIUM_SRC", r"C:\cr\src")).resolve()
if not (chromium_src / "chrome" / "VERSION").is_file():
    raise RuntimeError(f"Chromium checkout not found: {chromium_src}")


def find_mac_app_sources_anchor(source):
    """Return the leading chrome/browser macOS source-block anchor."""
    anchors = [
        ('  if (is_mac) {\n    sources += [\n'
         '      "accessibility/caption_settings_dialog.h",'),
        # Chromium 151 moved caption settings out of :core, leaving the app
        # controller as the first source in the same macOS source block.
        ('  if (is_mac) {\n    sources += [\n'
         '      "app_controller_mac.mm",'),
    ]
    matches = [anchor for anchor in anchors if anchor in source]
    assert len(matches) == 1, 'is_mac app sources anchor not found or ambiguous'
    return matches[0]


script_dir = os.path.dirname(os.path.abspath(__file__))
metadata_candidates = [
    os.path.join(os.path.dirname(script_dir), 'patches', 'platform_metadata.py'),
    r'C:\cr\cmux-browser\patches\platform_metadata.py',
    str(chromium_src / '.cmux-patches' / 'platform_metadata.py'),
]
metadata_script = next((path for path in metadata_candidates
                        if os.path.isfile(path)), None)
if metadata_script is None:
    raise RuntimeError(
        'platform_metadata.py is required; run this script from the cmux-browser '
        'checkout or sync patches/ to C:\\cr\\src\\.cmux-patches')
sentry_candidates = [
    os.path.join(os.path.dirname(script_dir), 'patches', 'sentry_crashpad.py'),
    r'C:\cr\cmux-browser\patches\sentry_crashpad.py',
    str(chromium_src / '.cmux-patches' / 'sentry_crashpad.py'),
]
sentry_script = next((path for path in sentry_candidates
                      if os.path.isfile(path)), None)
if sentry_script is None:
    raise RuntimeError(
        'sentry_crashpad.py is required; run this script from the cmux-browser '
        'checkout or sync patches/ to C:\\cr\\src\\.cmux-patches')

os.chdir(chromium_src)

# Keep the shipped Windows Settings UI aligned with the macOS/Linux patcher.
# Patches are copied into this cmux-owned directory by build-release-windows.ps1.
version = {}
for line in (chromium_src / "chrome" / "VERSION").read_text().splitlines():
    key, separator, value = line.partition("=")
    if separator:
        version[key] = value
chromium_major = version.get("MAJOR")
patch_dir = Path(os.environ.get("CMUX_PATCH_DIR", ".cmux-patches"))
if chromium_major in {"148", "149"}:
    helium_settings_patch = patch_dir / "helium-settings-chromium-149.patch"
    cmux_pinned_toolbar_patch = (
        patch_dir / "cmux-pinned-toolbar-actions-chromium-149.patch"
    )
elif chromium_major == "150":
    helium_settings_patch = patch_dir / "helium-settings-chromium-149.patch"
    cmux_pinned_toolbar_patch = (
        patch_dir / "cmux-pinned-toolbar-actions-chromium-150.patch"
    )
elif chromium_major == "151":
    helium_settings_patch = patch_dir / "helium-settings-chromium-151.patch"
    cmux_pinned_toolbar_patch = (
        patch_dir / "cmux-pinned-toolbar-actions-chromium-151.patch"
    )
else:
    raise RuntimeError(f"Unsupported Chromium major for Helium settings: {chromium_major}")
helium_ntp_patch = patch_dir / "helium-new-tab.patch"
helium_media_toolbar_patch = patch_dir / "helium-media-toolbar.patch"

def git_apply_check(*args):
    return subprocess.run(
        ["git", "apply", *args, str(helium_patch)],
        cwd=chromium_src,
        check=False,
        capture_output=True,
        text=True,
    )

for helium_label, helium_patch in [
    ("helium-settings", helium_settings_patch),
    ("helium-new-tab", helium_ntp_patch),
    ("helium-media-toolbar", helium_media_toolbar_patch),
    ("cmux-pinned-toolbar-actions", cmux_pinned_toolbar_patch),
]:
    if not helium_patch.is_file():
        raise RuntimeError(f"{helium_label} patch not found: {helium_patch}")
    if git_apply_check("--reverse", "--check").returncode == 0:
        print(f"{helium_label}: already patched ({helium_patch})")
        continue
    check_result = git_apply_check("--check")
    if check_result.returncode != 0:
        raise RuntimeError(
            f"{helium_label} patch does not match Chromium "
            f"{chromium_major}:\n{check_result.stderr}"
        )
    apply_result = git_apply_check()
    if apply_result.returncode != 0:
        raise RuntimeError(
            f"Failed to apply {helium_patch}:\n{apply_result.stderr}"
        )
    print(f"{helium_label}: patched ({helium_patch})")

chromium_major = int(chromium_major)
metadata = runpy.run_path(metadata_script,
                          run_name='__cmux_platform_metadata__')
metadata['patch_branding']()
metadata['patch_windows']()
print('platform-metadata: cmux Windows identity applied')
sentry = runpy.run_path(sentry_script, run_name='__cmux_sentry_crashpad__')
sentry['patch_chromium'](chromium_src)
print('sentry-crashpad: cmux-browser endpoint applied')

# Keep the custom BrowserWindow permission/chooser/modal anchor compatibility
# identical across the macOS/Linux shell patcher and the native Windows build.
if chromium_major >= 151:
    permission_patcher_candidates = [
        os.path.join(
            os.path.dirname(script_dir),
            'patches',
            'custom_window_permissions.py',
        ),
        r'C:\cr\cmux-browser\patches\custom_window_permissions.py',
        str(chromium_src / '.cmux-patches' /
            'custom_window_permissions.py'),
    ]
    permission_patcher_script = next(
        (
            path
            for path in permission_patcher_candidates
            if os.path.isfile(path)
        ),
        None,
    )
    if permission_patcher_script is None:
        raise RuntimeError(
            'custom_window_permissions.py is required for Chromium 151+'
        )
    permission_patcher = runpy.run_path(
        permission_patcher_script,
        run_name='__cmux_custom_window_permissions__',
    )
    permission_patcher['patch_tree'](chromium_src)
else:
    print(
        'custom-window-permissions: not required before Chromium 151'
    )

# The Windows Ghostty artifact used to be staged manually, which allowed its
# header/import library to drift from the browser's pinned manaflow fork. Keep
# this literal synchronized with ghostty-revision.txt; the host validation
# script checks the duplicate before this file is sent to the Windows builder.
EXPECTED_GHOSTTY_REVISION = '50ad1963d9c73ee957932ccb4d26bf6d15575ee7'
ghostty_root = Path('third_party/cmux_ghostty')
required_ghostty_artifacts = [
    ghostty_root / 'include/ghostty.h',
    ghostty_root / 'lib/ghostty.lib',
    ghostty_root / 'REVISION',
]
missing_ghostty_artifacts = [
    str(path) for path in required_ghostty_artifacts if not path.is_file()
]
if missing_ghostty_artifacts:
    raise RuntimeError(
        'missing pinned Windows Ghostty artifacts: '
        + ', '.join(missing_ghostty_artifacts)
    )
actual_ghostty_revision = (ghostty_root / 'REVISION').read_text().strip()
if actual_ghostty_revision != EXPECTED_GHOSTTY_REVISION:
    raise RuntimeError(
        'Windows Ghostty artifact revision '
        f'{actual_ghostty_revision!r} does not match browser pin '
        f'{EXPECTED_GHOSTTY_REVISION}'
    )

# 1. chrome_browser_main.cc: include + AddParts (mac||linux||win)
p='chrome/browser/chrome_browser_main.cc'; s=open(p).read()
inc='#include "chrome/browser/cmux_term/cmux_term.h"'
if inc not in s:
    a='#include "chrome/browser/chrome_browser_main.h"'
    s=s.replace(a, a+'\n'+inc, 1)
if 'ChromeBrowserMainExtraPartsCmuxTerm' not in s:
    mac=('#if BUILDFLAG(IS_MAC)\n'
         '  main_parts->AddParts(std::make_unique<ChromeBrowserMainExtraPartsMac>());\n'
         '#endif')
    s=s.replace(mac, mac+'\n\n#if BUILDFLAG(IS_MAC) || BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_WIN)\n'
                '  main_parts->AddParts(\n'
                '      std::make_unique<ChromeBrowserMainExtraPartsCmuxTerm>());\n'
                '#endif', 1)
open(p,'w',newline='\n').write(s)
print('main.cc:', 'ChromeBrowserMainExtraPartsCmuxTerm' in open(p).read())

# Chromium 151 restricts BubbleDialogDelegateView's legacy constructor to a
# fixed friend list. Chromium 149's public-constructor layout has no such
# anchor, so do not apply this source transformation there.
if chromium_major >= 151:
    p = 'ui/views/bubble/bubble_dialog_delegate_view.h'
    s = open(p).read()
    cmux_hover_forward = ('namespace cmux {\n'
                          'class CmuxRailHoverCardBubble;\n'
                          '}\n')
    if cmux_hover_forward not in s:
        forward_anchor = 'class WebUIBubbleDialogView;\n'
        assert s.count(forward_anchor) == 1, 'cmux hover-card forward anchor changed'
        s = s.replace(forward_anchor,
                      forward_anchor + cmux_hover_forward, 1)
    cmux_hover_friend = '  friend class ::cmux::CmuxRailHoverCardBubble;\n'
    if cmux_hover_friend not in s:
        friend_anchor = '  friend class ::TabHoverCardBubbleView;\n'
        assert s.count(friend_anchor) == 1, 'cmux hover-card friend anchor changed'
        s = s.replace(friend_anchor,
                      friend_anchor + cmux_hover_friend, 1)
    open(p, 'w', newline='\n').write(s)
else:
    print(f'cmux-hover-card-friend: not required on Chromium {chromium_major}')

# 1b. chrome_main_delegate.cc: make the fork default to ~/.cmux-profile before
#     Chrome resolves chrome::DIR_USER_DATA and acquires the process singleton.
#     An explicit --user-data-dir remains authoritative.
p = 'chrome/app/chrome_main_delegate.cc'
s = open(p).read()
profile_markers = (
    "cmux: this fork's default profile is ~/.cmux-profile",
    "cmux: tagged macOS bundles honor CrProductDirName",
)
profile_anchor = ('  // Initialize the user data dir for any process type that needs it.\n'
                  '  if (chrome::ProcessNeedsProfileDir(process_type)) {\n'
                  '    InitializeUserDataDir(base::CommandLine::ForCurrentProcess());\n'
                  '  }\n')
profile_add = ('  // Initialize the user data dir for any process type that needs it.\n'
               '  if (chrome::ProcessNeedsProfileDir(process_type)) {\n'
               "    // cmux: this fork's default profile is ~/.cmux-profile (persistent,\n"
               '    // carries the preinstalled extensions) so a plain Finder/Explorer\n'
               '    // launch matches scripts/run.sh. An explicit --user-data-dir always\n'
               '    // wins. Child processes copy this switch from the browser command\n'
               '    // line; if a helper reaches this path without it, appending the same\n'
               '    // fork default before InitializeUserDataDir() is harmless.\n'
               '    {\n'
               '      base::CommandLine* cmux_cl = base::CommandLine::ForCurrentProcess();\n'
               '      if (!cmux_cl->HasSwitch(switches::kUserDataDir)) {\n'
               '        base::FilePath cmux_home;\n'
               '        if (base::PathService::Get(base::DIR_HOME, &cmux_home) &&\n'
               '            !cmux_home.empty()) {\n'
               '          cmux_cl->AppendSwitchPath(\n'
               '              switches::kUserDataDir,\n'
               '              cmux_home.Append(FILE_PATH_LITERAL(".cmux-profile")));\n'
               '        }\n'
               '      }\n'
               '    }\n'
               '    InitializeUserDataDir(base::CommandLine::ForCurrentProcess());\n'
               '  }\n')
if any(marker in s for marker in profile_markers):
    print('cmux-default-profile: already patched')
else:
    assert s.count(profile_anchor) == 1, 'cmux default profile anchor not unique'
    s = s.replace(profile_anchor, profile_add, 1)
    open(p, 'w', newline='\n').write(s)
    print('cmux-default-profile: patched')

# 2. chrome/browser/BUILD.gn: xplat block + win terminal block
p='chrome/browser/BUILD.gn'; s=open(p).read()
# The protocol implementation moved to the neutral browser/utility target.
# Remove source entries left by an older overlay before adding its dependency.
for retired in [
    'cmux_term/cmux_terminal_host_protocol.h',
    'cmux_term/cmux_terminal_host_protocol.cc',
]:
    s=s.replace('      "'+retired+'",\n','')
xplat=['cmux_term/cmux_term.h','cmux_term/cmux_term.cc','cmux_term/cmux_views.h','cmux_term/cmux_views.cc',
       'cmux_term/cmux_browser_window.h','cmux_term/cmux_browser_window.cc',
       'cmux_term/cmux_browser_pane.h','cmux_term/cmux_browser_pane.cc',
       'cmux_term/cmux_toolbar_menus.h','cmux_term/cmux_toolbar_menus.cc',
       'cmux_term/cmux_configure_page.h','cmux_term/cmux_configure_page.cc',
       'cmux_term/cmux_extensions_container.h','cmux_term/cmux_extensions_container.cc',
       'cmux_term/cmux_extension_strip.h','cmux_term/cmux_extension_strip.cc',
       'cmux_term/cmux_pane_extension_pins.h','cmux_term/cmux_pane_extension_pins.cc',
       'cmux_term/cmux_side_panel.h','cmux_term/cmux_side_panel.cc',
       'cmux_term/cmux_side_panel_resize_area.h','cmux_term/cmux_side_panel_resize_area.cc',
       'cmux_term/cmux_demo_page.h','cmux_term/cmux_demo_page.cc',
       'cmux_term/cmux_focus.h','cmux_term/cmux_focus.cc','cmux_term/cmux_pane.h','cmux_term/cmux_pane.cc',
       'cmux_term/cmux_pane_view.h','cmux_term/cmux_pane_view.cc',
       'cmux_term/cmux_surface.h','cmux_term/cmux_surface.cc',
       'cmux_term/cmux_tab_strip.h','cmux_term/cmux_tab_strip.cc',
       'cmux_term/cmux_ghostty_opengl_host.h',
       'cmux_term/cmux_terminal_backend.h','cmux_term/cmux_terminal_backend.cc',
       'cmux_term/cmux_terminal_lifecycle.h',
       'cmux_term/cmux_terminal_placement.h','cmux_term/cmux_terminal_placement.cc',
       'cmux_term/cmux_terminal_recovery.h','cmux_term/cmux_terminal_recovery.cc',
       'cmux_term/cmux_tui_client.h','cmux_term/cmux_tui_client.cc',
       'cmux_term/cmux_terminal_host_connection.h','cmux_term/cmux_terminal_host_connection.cc',
       'cmux_term/cmux_tui_protocol.h','cmux_term/cmux_tui_protocol.cc',
       'cmux_term/cmux_workspace_projection.h','cmux_term/cmux_workspace_projection.cc',
       'cmux_term/cmux_tui_revision.h',
       'cmux_term/cmux_rail.h','cmux_term/cmux_rail.cc',
       'cmux_term/cmux_rail_animating_layout_manager.h',
       'cmux_term/cmux_rail_animating_layout_manager.cc',
       'cmux_term/cmux_rail_group_editor_bubble.h',
       'cmux_term/cmux_rail_group_editor_bubble.cc',
       'cmux_term/cmux_rail_hover_card.h',
       'cmux_term/cmux_rail_hover_card.cc',
       'cmux_term/cmux_rail_config.h','cmux_term/cmux_rail_config.cc','cmux_term/cmux_strip_controller.h','cmux_term/cmux_input.h',
       'cmux_term/cmux_keymap.h','cmux_term/cmux_keymap.cc',
       'cmux_term/cmux_native_window_registry.h','cmux_term/cmux_native_window_registry.cc',
       'cmux_term/cmux_new_tab_page.h',
       'cmux_term/window_layout.h','cmux_term/window_layout.cc','cmux_term/window_model.h','cmux_term/window_model.cc',
       'cmux_term/cmux_extensions.h','cmux_term/cmux_extensions.cc',
       'cmux_term/cmux_tab_drag.h','cmux_term/cmux_tab_drag.cc',
       'cmux_term/cmux_layout_config.h','cmux_term/cmux_layout_config.cc',
       'cmux_term/cmux_release_build.h',
       'cmux_term/cmux_telemetry.h','cmux_term/cmux_telemetry.cc',
       'cmux_term/cmux_telemetry_model.h','cmux_term/cmux_telemetry_model.cc',
       'cmux_term/cmux_update_installer.h','cmux_term/cmux_update_installer.cc',
       'cmux_term/cmux_update_model.h','cmux_term/cmux_update_model.cc',
       'cmux_term/cmux_update_network.h',
       'cmux_term/cmux_update_script.h','cmux_term/cmux_update_script.cc',
       'cmux_term/cmux_update_service.h','cmux_term/cmux_update_service.cc',
       'cmux_term/cmux_theme.h','cmux_term/cmux_theme.cc',
       'cmux_term/cmux_easing.h','cmux_term/cmux_easing.cc']
mac_anchor = find_mac_app_sources_anchor(s)
if 'cmux_term/cmux_views.cc' not in s:
    block=('  if (is_mac || is_linux || is_win) {  # cmux cross-platform\n'
           '    sources += [\n'+''.join('      "%s",\n'%f for f in xplat)+
           '    ]\n'
           '    deps += [\n'
           '      "//chrome/services/cmux_terminal_renderer/public/cpp:'
           'terminal_host_protocol",\n'
           '    ]\n'
           '  }\n')
    s=s.replace(mac_anchor, block+mac_anchor,1)
else:
    # A warm Windows builder keeps the previously patched BUILD.gn. Refresh
    # every cross-platform entry so TUI and updater additions cannot compile
    # locally yet disappear from the cached Windows link.
    xplat_refresh = [*xplat]
    xplat_missing = [f for f in xplat_refresh if f'"{f}"' not in s]
    if xplat_missing:
        anchor = '      "cmux_term/cmux_layout_config.cc",\n'
        assert s.count(anchor) == 1, 'cmux source refresh anchor changed'
        s = s.replace(
            anchor,
            anchor + ''.join(
                f'      "{source}",\n' for source in xplat_missing
            ),
            1,
        )
terminal_host_dep='"//chrome/services/cmux_terminal_renderer/public/cpp:terminal_host_protocol"'
if terminal_host_dep not in s:
    marker=s.index('# cmux cross-platform')
    end=s.index('    ]\n  }\n', marker)
    insert=('    ]\n'
            '    deps += [\n'
            '      '+terminal_host_dep+',\n'
            '    ]\n'
            '  }\n')
    s=s[:end]+s[end:].replace('    ]\n  }\n',insert,1)
if 'cmux windows terminal' not in s:
    win_block=('  if (is_win) {  # cmux windows terminal\n'
               '    sources += [\n'
               '      "cmux_term/cmux_ghostty_opengl_host.cc",\n'
               '      "cmux_term/cmux_theme_ghostty.h",\n'
               '      "cmux_term/cmux_theme_ghostty.cc",\n'
               '      "cmux_term/cmux_terminal_pane_linux.cc",\n'
               '      "cmux_term/cmux_update_network_win.cc",\n'
               '    ]\n'
               '    deps += [\n'
               '      "//chrome/services/cmux_terminal_renderer/public/cpp:'
               'ghostty_resources",\n'
               '    ]\n'
               '    libs += [\n'
               '      rebase_path("//third_party/cmux_ghostty/lib/ghostty.lib"),\n'
               '      "gdi32.lib",\n'
               '      "user32.lib",\n'
               '    ]\n  }\n')
    s=s.replace(mac_anchor, win_block+mac_anchor,1)
else:
    win_start = s.index('if (is_win) {  # cmux windows terminal')
    win_end = s.index('\n  }', win_start)
    win_block = s[win_start:win_end]
    win_sources = [
        'cmux_term/cmux_ghostty_opengl_host.cc',
        'cmux_term/cmux_theme_ghostty.h',
        'cmux_term/cmux_theme_ghostty.cc',
        'cmux_term/cmux_terminal_pane_linux.cc',
        'cmux_term/cmux_update_network_win.cc',
    ]
    win_missing = [
        source for source in win_sources if f'"{source}"' not in win_block
    ]
    if win_missing:
        sources_start = win_block.index('    sources += [')
        sources_end = win_block.index('    ]\n', sources_start)
        win_block = (
            win_block[:sources_end]
            + ''.join(f'      "{source}",\n' for source in win_missing)
            + win_block[sources_end:]
        )
    ghostty_resources_dep = (
        '"//chrome/services/cmux_terminal_renderer/public/cpp:'
        'ghostty_resources"'
    )
    if ghostty_resources_dep not in win_block:
        sources_start = win_block.index('    sources += [')
        sources_end = win_block.index('    ]\n', sources_start) + len('    ]\n')
        win_block = (
            win_block[:sources_end]
            + '    deps += [\n'
            + f'      {ghostty_resources_dep},\n'
            + '    ]\n'
            + win_block[sources_end:]
        )
    required_libs = ['gdi32.lib', 'user32.lib']
    missing_libs = [lib for lib in required_libs if f'"{lib}"' not in win_block]
    if missing_libs:
        libs_start = win_block.index('    libs += [')
        if '    ]\n' in win_block[libs_start:]:
            libs_end = win_block.index('    ]\n', libs_start)
            win_block = (
                win_block[:libs_end]
                + ''.join(f'      "{lib}",\n' for lib in missing_libs)
                + win_block[libs_end:]
            )
        else:
            compact = (
                '    libs += [ '
                'rebase_path("//third_party/cmux_ghostty/lib/ghostty.lib") ]'
            )
            expanded = (
                '    libs += [\n'
                '      rebase_path("//third_party/cmux_ghostty/lib/ghostty.lib"),\n'
                + ''.join(f'      "{lib}",\n' for lib in missing_libs)
                + '    ]'
            )
            assert compact in win_block, 'Windows library refresh anchor changed'
            win_block = win_block.replace(compact, expanded, 1)
    s = s[:win_start] + win_block + s[win_end:]
open(p,'w',newline='\n').write(s)
print('BUILD.gn xplat:', 'cmux_term/cmux_views.cc' in open(p).read(), 'win:', 'cmux windows terminal' in open(p).read())

# A cmux workspace is a real Browser with a custom production BrowserWindow.
p='chrome/browser/ui/views/frame/browser_window_factory.cc'; s=open(p).read()
include_anchor='#include "build/build_config.h"\n'
include_line='#include "chrome/browser/cmux_term/cmux_browser_window.h"\n'
if include_line not in s:
    assert include_anchor in s, 'browser window factory include anchor missing'
    s=s.replace(include_anchor,include_anchor+include_line,1)
factory_anchor=('BrowserWindow::CreateBrowserWindow(Browser* browser,\n'
                '                                   bool user_gesture,\n'
                '                                   bool in_tab_dragging) {\n')
if 'MaybeCreateCmuxBrowserWindow(browser)' not in s:
    assert factory_anchor in s, 'browser window factory function anchor missing'
    s=s.replace(factory_anchor,factory_anchor+
                '  if (auto cmux_window = cmux::MaybeCreateCmuxBrowserWindow(browser)) {\n'
                '    return cmux_window;\n'
                '  }\n\n',1)
open(p,'w',newline='\n').write(s)

# 3. Extensions menu coordinator reuse seams.
def replace_once(s, old, new, label):
    if new in s:
        return s
    if old in s:
        return s.replace(old, new, 1)
    raise AssertionError(label + ' anchor not found')

def replace_all_present(s, old, new):
    return s.replace(old, new) if old in s else s

# extensions_menu_coordinator.{h,cc}: Browser* -> BrowserWindowInterface*.
p = 'chrome/browser/ui/views/extensions/extensions_menu_coordinator.h'
s = open(p).read()
s = replace_once(s, '#include "base/memory/raw_ref.h"\n',
                 '#include "base/memory/raw_ref.h"\n'
                 '#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"\n',
                 'coordinator include')
s = replace_all_present(s, 'class Browser;\n', 'class BrowserWindowInterface;\n')
s = replace_all_present(s, 'ExtensionsMenuCoordinator(Browser* browser,',
                        'ExtensionsMenuCoordinator(BrowserWindowInterface* browser,')
s = replace_all_present(s, 'const raw_ptr<Browser> browser_;',
                        'const raw_ptr<BrowserWindowInterface> browser_;')
open(p, 'w', newline='\n').write(s)

p = 'chrome/browser/ui/views/extensions/extensions_menu_coordinator.cc'
s = open(p).read()
s = replace_all_present(
    s, '#include "chrome/browser/ui/browser.h"',
    '#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"')
s = replace_all_present(s, 'Browser* browser,\n    ExtensionsContainer* extensions_container)',
                        'BrowserWindowInterface* browser,\n    ExtensionsContainer* extensions_container)')
open(p, 'w', newline='\n').write(s)

# extensions_menu_delegate_desktop.{h,cc}: BWI flip + row activation override.
p = 'chrome/browser/ui/views/extensions/extensions_menu_delegate_desktop.h'
s = open(p).read()
s = replace_once(s, '#include "base/memory/raw_ptr.h"\n',
                 '#include <string>\n\n'
                 '#include "base/functional/callback_forward.h"\n'
                 '#include "base/memory/raw_ptr.h"\n',
                 'delegate h includes')
s = replace_all_present(s, 'class Browser;\n', 'class BrowserWindowInterface;\n')
s = replace_all_present(s, 'Browser* browser,\n      ExtensionsContainer* extensions_container,',
                        'BrowserWindowInterface* browser,\n      ExtensionsContainer* extensions_container,')
decl = ('  // cmux: lets a BrowserWindowInterface embedder handle menu-row\n'
        '  // activation before ExtensionActionViewModel creates a Browser-only\n'
        '  // popup host. Unset keeps Chrome behavior byte-identical.\n'
        '  using ActionActivationOverride =\n'
        '      base::RepeatingCallback<bool(const std::string& action_id)>;\n'
        '  static void SetEmbedderActionActivationOverride(\n'
        '      ActionActivationOverride cb);\n\n')
if 'SetEmbedderActionActivationOverride' not in s:
    s = s.replace('  ~ExtensionsMenuDelegateDesktop() override;\n\n',
                  '  ~ExtensionsMenuDelegateDesktop() override;\n\n' + decl, 1)
s = replace_all_present(s, 'const raw_ptr<Browser> browser_;',
                        'const raw_ptr<BrowserWindowInterface> browser_;')
open(p, 'w', newline='\n').write(s)

p = 'chrome/browser/ui/views/extensions/extensions_menu_delegate_desktop.cc'
s = open(p).read()
s = replace_once(s, '#include <algorithm>\n',
                 '#include <algorithm>\n#include <utility>\n',
                 'delegate cc utility')
s = replace_once(s, '#include "base/check_deref.h"\n',
                 '#include "base/check_deref.h"\n#include "base/no_destructor.h"\n',
                 'delegate cc no_destructor')
s = replace_all_present(
    s, '#include "chrome/browser/ui/browser.h"',
    '#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"')
if '#include "components/tabs/public/tab_interface.h"' not in s:
    s = s.replace('#include "chrome/grit/generated_resources.h"\n',
                  '#include "chrome/grit/generated_resources.h"\n'
                  '#include "components/tabs/public/tab_interface.h"\n', 1)
if ('->GetTabStripModel()' in s and
        '#include "chrome/browser/ui/tabs/tab_strip_model.h"' not in s):
    s = s.replace(
        '#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"\n',
        '#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"\n'
        '#include "chrome/browser/ui/tabs/tab_strip_model.h"\n', 1)
helper = ('ExtensionsMenuDelegateDesktop::ActionActivationOverride&\n'
          'GetEmbedderActionActivationOverride() {\n'
          '  static base::NoDestructor<\n'
          '      ExtensionsMenuDelegateDesktop::ActionActivationOverride>\n'
          '      activation_override;\n'
          '  return *activation_override;\n'
          '}\n\n')
if 'GetEmbedderActionActivationOverride()' not in s:
    s = s.replace('}  // namespace\n\nExtensionsMenuDelegateDesktop::ExtensionsMenuDelegateDesktop(',
                  helper + '}  // namespace\n\n'
                  'ExtensionsMenuDelegateDesktop::ExtensionsMenuDelegateDesktop(', 1)
s = replace_all_present(s, 'Browser* browser,\n    ExtensionsContainer* extensions_container,',
                        'BrowserWindowInterface* browser,\n    ExtensionsContainer* extensions_container,')
s = replace_all_present(s, 'ToolbarActionsModel::Get(browser_->profile())',
                        'ToolbarActionsModel::Get(browser_->GetProfile())')
method = ('void ExtensionsMenuDelegateDesktop::SetEmbedderActionActivationOverride(\n'
          '    ActionActivationOverride cb) {\n'
          '  GetEmbedderActionActivationOverride() = std::move(cb);\n'
          '}\n\n')
if 'ExtensionsMenuDelegateDesktop::SetEmbedderActionActivationOverride' not in s:
    s = s.replace('ExtensionsMenuDelegateDesktop::~ExtensionsMenuDelegateDesktop() = default;\n\n',
                  'ExtensionsMenuDelegateDesktop::~ExtensionsMenuDelegateDesktop() = default;\n\n' + method, 1)
old = '''    DCHECK_NE(PermissionsManager::Get(browser_->profile())
                  ->GetUserSiteSetting(browser_->tab_strip_model()
                                           ->GetActiveWebContents()
                                           ->GetPrimaryMainFrame()
                                           ->GetLastCommittedOrigin()),
              PermissionsManager::UserSiteSetting::kCustomizeByExtension);
'''
new = '''    DCHECK_NE(PermissionsManager::Get(browser_->GetProfile())
                  ->GetUserSiteSetting(browser_->GetActiveTabInterface()
                                           ->GetContents()
                                           ->GetPrimaryMainFrame()
                                           ->GetLastCommittedOrigin()),
              PermissionsManager::UserSiteSetting::kCustomizeByExtension);
'''
s = replace_once(s, old, new, 'delegate DCHECK BWI')
old = '''void ExtensionsMenuDelegateDesktop::OnActionButtonClicked(
    const extensions::ExtensionId& extension_id) {
  menu_model_->ExecuteAction(extension_id);
}
'''
new = '''void ExtensionsMenuDelegateDesktop::OnActionButtonClicked(
    const extensions::ExtensionId& extension_id) {
  const ActionActivationOverride& activation_override =
      GetEmbedderActionActivationOverride();
  if (activation_override && activation_override.Run(extension_id)) {
    CloseBubble();
    return;
  }

  menu_model_->ExecuteAction(extension_id);
}
'''
s = replace_once(s, old, new, 'delegate action override')
open(p, 'w', newline='\n').write(s)

# extensions_menu_main_page_view.{h,cc}: BWI flip.
p = 'chrome/browser/ui/views/extensions/extensions_menu_main_page_view.h'
s = open(p).read()
s = replace_all_present(s, 'class Browser;\n', 'class BrowserWindowInterface;\n')
s = replace_all_present(s, 'explicit ExtensionsMenuMainPageView(Browser* browser,',
                        'explicit ExtensionsMenuMainPageView(BrowserWindowInterface* browser,')
s = replace_all_present(s, 'const raw_ptr<Browser> browser_;',
                        'const raw_ptr<BrowserWindowInterface> browser_;')
open(p, 'w', newline='\n').write(s)

p = 'chrome/browser/ui/views/extensions/extensions_menu_main_page_view.cc'
s = open(p).read()
s = replace_all_present(
    s, '#include "chrome/browser/ui/browser.h"',
    '#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"')
s = replace_all_present(s, 'ExtensionsMenuMainPageView::ExtensionsMenuMainPageView(\n    Browser* browser,',
                        'ExtensionsMenuMainPageView::ExtensionsMenuMainPageView(\n    BrowserWindowInterface* browser,')
s = replace_all_present(s, '[](Browser* browser) {',
                        '[](BrowserWindowInterface* browser) {')
open(p, 'w', newline='\n').write(s)

# extensions_menu_site_permissions_page_view.{h,cc}: BWI flip.
p = 'chrome/browser/ui/views/extensions/extensions_menu_site_permissions_page_view.h'
s = open(p).read()
s = replace_all_present(s, 'class Browser;\n', 'class BrowserWindowInterface;\n')
s = replace_all_present(s, 'Browser* browser,\n      extensions::ExtensionId extension_id,',
                        'BrowserWindowInterface* browser,\n      extensions::ExtensionId extension_id,')
s = replace_all_present(s, 'const raw_ptr<Browser> browser_;',
                        'const raw_ptr<BrowserWindowInterface> browser_;')
open(p, 'w', newline='\n').write(s)

p = 'chrome/browser/ui/views/extensions/extensions_menu_site_permissions_page_view.cc'
s = open(p).read()
s = replace_all_present(
    s, '#include "chrome/browser/ui/browser.h"',
    '#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"')
s = replace_all_present(s, 'Browser* browser,\n    extensions::ExtensionId extension_id,',
                        'BrowserWindowInterface* browser,\n    extensions::ExtensionId extension_id,')
old = '''[](Browser* browser,
                                 extensions::ExtensionId extension_id) {'''
new = '''[](BrowserWindowInterface* browser,
                                 extensions::ExtensionId extension_id) {'''
s = replace_once(s, old, new, 'site permissions settings lambda BWI')
open(p, 'w', newline='\n').write(s)

# extensions_menu_entry_view.{h,cc}: unused param flip + context-menu switch.
p = 'chrome/browser/ui/views/extensions/extensions_menu_entry_view.h'
s = open(p).read()
s = replace_all_present(s, 'class Browser;\n', 'class BrowserWindowInterface;\n')
s = replace_all_present(s, 'Browser* browser,\n      bool is_enterprise,',
                        'BrowserWindowInterface* browser,\n      bool is_enterprise,')
decl = ('  // cmux: BrowserWindowInterface embedders can disable the in-menu\n'
        '  // per-row context menu; the toolbar strip context menu remains.\n'
        '  static void SetContextMenusEnabledForEmbedder(bool enabled);\n\n')
if 'SetContextMenusEnabledForEmbedder' not in s:
    s = s.replace('  ~ExtensionsMenuEntryView() override;\n\n',
                  '  ~ExtensionsMenuEntryView() override;\n\n' + decl, 1)
open(p, 'w', newline='\n').write(s)

p = 'chrome/browser/ui/views/extensions/extensions_menu_entry_view.cc'
s = open(p).read()
s = replace_all_present(
    s, '#include "chrome/browser/ui/browser.h"',
    '#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"')
switch = ('bool& ContextMenusEnabledForEmbedder() {\n'
          '  static bool enabled = true;\n'
          '  return enabled;\n'
          '}\n\n')
if 'ContextMenusEnabledForEmbedder()' not in s:
    s = s.replace('namespace {\n\n', 'namespace {\n\n' + switch, 1)
s = replace_all_present(s, 'ExtensionsMenuEntryView::ExtensionsMenuEntryView(\n    Browser* browser,',
                        'ExtensionsMenuEntryView::ExtensionsMenuEntryView(\n    BrowserWindowInterface* browser,')
method = ('void ExtensionsMenuEntryView::SetContextMenusEnabledForEmbedder(\n'
          '    bool enabled) {\n'
          '  ContextMenusEnabledForEmbedder() = enabled;\n'
          '}\n\n')
if 'ExtensionsMenuEntryView::SetContextMenusEnabledForEmbedder' not in s:
    s = s.replace('ExtensionsMenuEntryView::~ExtensionsMenuEntryView() = default;\n\n',
                  'ExtensionsMenuEntryView::~ExtensionsMenuEntryView() = default;\n\n' + method, 1)
old = '''void ExtensionsMenuEntryView::SetupContextMenuButton(
    ToolbarActionViewModel* view_model) {
  // Add a controller to the context menu
  context_menu_controller_ = std::make_unique<ExtensionContextMenuController>(
      view_model, this,
      extensions::ExtensionContextMenuModel::ContextMenuSource::kMenuItem);
'''
new = '''void ExtensionsMenuEntryView::SetupContextMenuButton(
    ToolbarActionViewModel* view_model) {
  if (!ContextMenusEnabledForEmbedder()) {
    return;
  }

  // Add a controller to the context menu
  context_menu_controller_ = std::make_unique<ExtensionContextMenuController>(
      view_model, this,
      extensions::ExtensionContextMenuModel::ContextMenuSource::kMenuItem);
'''
s = replace_once(s, old, new, 'entry context menu switch')
old = '''bool ExtensionsMenuEntryView::IsContextMenuRunningForTesting() const {
  return context_menu_controller_->IsMenuRunning();
}
'''
new = '''bool ExtensionsMenuEntryView::IsContextMenuRunningForTesting() const {
  return context_menu_controller_ && context_menu_controller_->IsMenuRunning();
}
'''
s = replace_once(s, old, new, 'entry context menu test guard')
old = '''void ExtensionsMenuEntryView::OnContextMenuPressed() {
  base::RecordAction(base::UserMetricsAction(
      "Extensions.Toolbar.MoreActionsButtonPressedFromMenu"));
'''
new = '''void ExtensionsMenuEntryView::OnContextMenuPressed() {
  if (!context_menu_controller_) {
    return;
  }
  base::RecordAction(base::UserMetricsAction(
      "Extensions.Toolbar.MoreActionsButtonPressedFromMenu"));
'''
s = replace_once(s, old, new, 'entry context menu pressed guard')
open(p, 'w', newline='\n').write(s)

# The reference Chrome UI uses the compact menu and toolbar hover pill. Both
# are the disabled state of this feature in the matching Chromium revision.
p = 'extensions/common/extension_features.cc'
s = open(p).read()
upstream = '''BASE_FEATURE(kExtensionsMenuAccessControl,
#if BUILDFLAG(IS_ANDROID)
             base::FEATURE_ENABLED_BY_DEFAULT
#else
             base::FEATURE_DISABLED_BY_DEFAULT
#endif
);
'''
previous_cmux = '''BASE_FEATURE(kExtensionsMenuAccessControl,
#if BUILDFLAG(IS_ANDROID)
             base::FEATURE_ENABLED_BY_DEFAULT
#else
             // cmux: runs the shipping-Chrome Finch state.
             base::FEATURE_ENABLED_BY_DEFAULT
#endif
);
'''
compact = '''BASE_FEATURE(kExtensionsMenuAccessControl,
#if BUILDFLAG(IS_ANDROID)
             base::FEATURE_ENABLED_BY_DEFAULT
#else
             // cmux: match Chrome's compact extensions menu and toolbar pill.
             base::FEATURE_DISABLED_BY_DEFAULT
#endif
);
'''
if compact not in s:
    if previous_cmux in s:
        s = s.replace(previous_cmux, compact, 1)
    elif upstream in s:
        s = s.replace(upstream, compact, 1)
    else:
        raise AssertionError('extensions menu feature default anchor not found')
open(p, 'w', newline='\n').write(s)
print('extensions-menu-reuse-seams: patched')


# 6. Keep Chromium's native UA client-hint brands. cmux is not an official
# Google Chrome build, so remove the exact legacy spoof if it is present.
p='components/embedder_support/user_agent_utils.cc'; s=open(p).read()
anchor='  std::optional<std::string> brand;\n#if !BUILDFLAG(CHROMIUM_BRANDING)\n  brand = version_info::GetProductName();\n#endif'
brand_line='  brand = "Google Chrome";'
legacy_bare = anchor + '\n' + brand_line
legacy_win = (anchor + '\n  // cmux: present as Google Chrome brand.\n' +
              brand_line)
legacy_full = (
    anchor +
    '\n  // cmux: present as "Google Chrome" so Sec-CH-UA / navigator.userAgentData\n'
    '  // brands match real Chrome (our build is Chromium-branded otherwise).\n' +
    brand_line)
legacy_spoofs = (
    (legacy_bare, 'ua-brand: removed legacy Google Chrome spoof'),
    (legacy_win, 'ua-brand: removed legacy Windows Google Chrome spoof'),
    (legacy_full, 'ua-brand: removed legacy full Google Chrome spoof'),
)
removed_legacy_spoof = False
for legacy, message in legacy_spoofs:
    if legacy in s:
        s=s.replace(legacy,anchor,1)
        open(p,'w',newline='\n').write(s)
        print(message)
        removed_legacy_spoof = True
        break
if not removed_legacy_spoof:
    print('ua-brand: native Chromium behavior')
assert all(legacy not in s for legacy, _ in legacy_spoofs), \
       'legacy Chrome compatibility brand still present'
assert 'cmux: present as' not in s, 'unrecognized legacy cmux UA-brand spoof'

# 6b. Do not attach Google field-trial IDs to requests from an unofficial
# product. This matches Helium's network identity.
p='components/variations/net/variations_http_headers.cc'; s=open(p).read()
old='''bool ShouldAppendVariationsHeader(const GURL& url, InIncognito incognito) {
  // Note the criteria for attaching client experiment headers:
  // 1. We only transmit to Google owned domains which can evaluate
  // experiments.
  //    1a. These include hosts which have a standard postfix such as:
  //         *.doubleclick.net or *.googlesyndication.com or
  //         exactly www.googleadservices.com or
  //         international TLD domains *.google.<TLD> or *.youtube.<TLD>.
  // 2. Only transmit for non-Incognito profiles.
  return incognito == InIncognito::kNo &&
         GetUrlValidationResult(url) == URLValidationResult::kShouldAppend;
}'''
new='''bool ShouldAppendVariationsHeader(const GURL& url, InIncognito incognito) {
  // cmux: this is not a Google Chrome release channel. Do not send Google's
  // experiment identifiers as X-Client-Data from an unofficial product.
  return false;
}'''
if 'cmux: this is not a Google Chrome release channel' in s:
    print('variations-header: already disabled')
elif old in s:
    s=s.replace(old,new,1)
    open(p,'w',newline='\n').write(s)
    print('variations-header: disabled')
else:
    raise AssertionError('variations header anchor not found')

# 6c. Do not fetch Google variations seeds, even if future build flags make
# this executable look like an official channel.
p='components/variations/service/variations_service.cc'; s=open(p).read()
start=s.index('bool IsFetchingEnabled() {')
end_marker='\n}\n\n// Returns the already downloaded first run seed'
end=s.index(end_marker,start)+2
body=s[start:end]
new='''bool IsFetchingEnabled() {
  // cmux: do not enroll this unofficial product in Google field trials.
  return false;
}'''
if 'cmux: do not enroll this unofficial product' in body:
    print('variations-fetch: already disabled')
else:
    assert 'kDisableVariationsSeedFetch' in body, 'variations fetch anchor changed'
    s=s[:start]+new+s[end:]
    open(p,'w',newline='\n').write(s)
    print('variations-fetch: disabled')

# 6d. Match Helium's plain Google omnibox template. Do not claim Chrome
# release-channel attribution while suppressing Chrome variations identity.
p='third_party/search_engines_data/resources/definitions/prepopulated_engines.json'; s=open(p).read()
google_start=s.index('    "google": {')
google_end=s.index('\n    },',google_start)+len('\n    },')
google=s[google_start:google_end]
plain_search='      "search_url": "https://www.google.com/search?q={searchTerms}",'
plain_suggest='      "suggest_url": "https://www.google.com/complete/search?client=chrome&q={searchTerms}",'
if plain_search in google and '"type": "SEARCH_ENGINE_OTHER"' in google:
    print('google-search-template: already plain')
else:
    import re
    google,search_count=re.subn(r'^      "search_url": .+$',plain_search,google,count=1,flags=re.MULTILINE)
    google,suggest_count=re.subn(r'^      "suggest_url": .+$',plain_suggest,google,count=1,flags=re.MULTILINE)
    google,type_count=re.subn(r'^      "type": "SEARCH_ENGINE_GOOGLE",$','      "type": "SEARCH_ENGINE_OTHER",',google,count=1,flags=re.MULTILINE)
    assert search_count == 1, 'Google search URL anchor changed'
    assert suggest_count == 1, 'Google suggest URL anchor changed'
    assert type_count == 1, 'Google engine type anchor changed'
    s=s[:google_start]+google+s[google_end:]
    version_match=re.search(r'("kCurrentDataVersion": )(\d+)',s)
    assert version_match, 'prepopulated engine data version missing'
    old_version=int(version_match.group(2))
    s=s[:version_match.start(2)]+str(old_version+1)+s[version_match.end(2):]
    open(p,'w',newline='\n').write(s)
    print('google-search-template: plain; data version',old_version+1)

p='components/search_engines/search_engine_utils.cc'; s=open(p).read()
old='    return TemplateURLPrepopulateData::google.type;'
new=('    // cmux: the built-in Google entry uses the same ordinary search URL\n'
     '    // as Helium, not Chrome release-channel attribution.\n'
     '    return SEARCH_ENGINE_OTHER;')
if 'cmux: the built-in Google entry uses the same ordinary search URL' in s:
    print('google-search-type: already ordinary')
else:
    assert s.count(old) == 1, 'Google search type anchor changed'
    s=s.replace(old,new,1)
    open(p,'w',newline='\n').write(s)
    print('google-search-type: ordinary')

# 6e. A cmux Browser is hosted by a custom BrowserWindow, so the omnibox zoom
# page action has no BrowserView-owned ZoomBubbleCoordinator.
p='chrome/browser/ui/views/page_action/zoom_view.cc'; s=open(p).read()
old_changed='''  auto* zoom_bubble_coordinator = GetZoomBubbleCoordinator(web_contents);
  CHECK(zoom_bubble_coordinator);

  if (ShouldBeVisible(can_show_bubble)) {'''
new_changed='''  auto* zoom_bubble_coordinator = GetZoomBubbleCoordinator(web_contents);
  // cmux: a custom BrowserWindow has no BrowserView-owned zoom coordinator.
  if (!zoom_bubble_coordinator) {
    return;
  }

  if (ShouldBeVisible(can_show_bubble)) {'''
old_execute='''  auto* zoom_bubble_coordinator = GetZoomBubbleCoordinator(GetWebContents());
  CHECK(zoom_bubble_coordinator);

  zoom_bubble_coordinator->Show(GetWebContents(), ZoomBubbleView::USER_GESTURE);'''
new_execute='''  auto* zoom_bubble_coordinator = GetZoomBubbleCoordinator(GetWebContents());
  // cmux: a custom BrowserWindow has no BrowserView-owned zoom coordinator.
  if (!zoom_bubble_coordinator) {
    return;
  }

  zoom_bubble_coordinator->Show(GetWebContents(), ZoomBubbleView::USER_GESTURE);'''
if ('CHECK(zoom_bubble_coordinator);' not in s and
        s.count('if (!zoom_bubble_coordinator) {') >= 3):
    print('zoom-coordinator-guard: equivalent guard already present')
else:
    assert old_changed in s, 'zoom changed coordinator anchor not found'
    assert old_execute in s, 'zoom execute coordinator anchor not found'
    s=s.replace(old_changed,new_changed,1)
    s=s.replace(old_execute,new_execute,1)
    open(p,'w',newline='\n').write(s)
    print('zoom-coordinator-guard: patched')

# 7 + 8. rounded_omnibox_results_frame guards
p='chrome/browser/ui/views/omnibox/rounded_omnibox_results_frame.cc'; s=open(p).read()
a7=('  views::Widget* this_widget = this_view->GetWidget();\n  views::Widget* parent_widget = this_widget->parent();')
add7=('  views::Widget* this_widget = this_view->GetWidget();\n'
      '  if (!this_widget) {\n    return {nullptr, std::unique_ptr<ui::MouseEvent>(\n'
      '                         static_cast<ui::MouseEvent*>(\n                             this_event->Clone().release()))};\n  }\n'
      '  views::Widget* parent_widget = this_widget->parent();')
if 'if (!this_widget) {' not in s and a7 in s: s=s.replace(a7,add7,1)
a8=('  BrowserView* browser_view = BrowserView::GetBrowserViewForNativeWindow(\n      parent_widget->GetNativeWindow());\n')
add8=a8+'  if (!browser_view) {\n    return nullptr;\n  }\n'
if 'if (!browser_view) {' not in s and a8 in s: s=s.replace(a8,add8,1)
open(p,'w',newline='\n').write(s)
print('omnibox-guards done')

# A CmuxBrowserWindow has a real Browser but no BrowserView-only immersive
# controller. Keep LocationBarView's native Browser path and guard the optional
# feature during focus.
p='chrome/browser/ui/views/omnibox/omnibox_view_views.cc'; s=open(p).read()
browser_window_include='#include "chrome/browser/ui/browser_window.h"\n'
if browser_window_include not in s:
    include_anchor='#include "chrome/browser/ui/browser.h"\n'
    if include_anchor not in s:
        raise SystemExit('omnibox-immersive-guard: browser include anchor not found')
    s=s.replace(include_anchor,include_anchor+browser_window_include,1)
    open(p,'w',newline='\n').write(s)
old=('  if (location_bar_view_ && location_bar_view_->browser()) {\n'
     '    focus_reveal_lock =\n'
     '        ImmersiveModeController::From(location_bar_view_->browser())\n'
     '            ->GetRevealedLock(ImmersiveModeController::ANIMATE_REVEAL_YES);\n'
     '  }')
new=('  if (location_bar_view_ && location_bar_view_->browser()) {\n'
     '    // cmux: a Browser hosted by the shared workspace widget has no\n'
     '    // BrowserView-only ImmersiveModeController.\n'
     '    if (location_bar_view_->browser()->window()->AsBrowserView()) {\n'
     '      if (auto* immersive =\n'
     '              ImmersiveModeController::From(location_bar_view_->browser())) {\n'
     '        focus_reveal_lock = immersive->GetRevealedLock(\n'
     '            ImmersiveModeController::ANIMATE_REVEAL_YES);\n'
     '      }\n'
     '    }\n'
     '  }')
previous=('  if (location_bar_view_ && location_bar_view_->browser()) {\n'
          '    // cmux: a Browser hosted by the shared workspace widget has no\n'
          '    // BrowserView-only ImmersiveModeController.\n'
          '    if (auto* immersive =\n'
          '            ImmersiveModeController::From(location_bar_view_->browser())) {\n'
          '      focus_reveal_lock = immersive->GetRevealedLock(\n'
          '          ImmersiveModeController::ANIMATE_REVEAL_YES);\n'
          '    }\n'
          '  }')
if new not in s:
    if previous in s:
        s=s.replace(previous,new,1)
    elif old in s:
        s=s.replace(old,new,1)
    open(p,'w',newline='\n').write(s)
print('omnibox-immersive-guard:', 'cmux: a Browser hosted by the shared workspace widget' in open(p).read())
print('APPLY_WIN_CHROME OK')

# --- cmux MV2 + install-dialog patches (ported from scripts/apply.sh) ---
p = 'chrome/browser/ui/extensions/extension_install_ui_desktop.cc'
s = open(p).read()
if '#include "base/logging.h"' not in s:
    s = replace_once(s, '#include "base/functional/bind.h"\n',
                     '#include "base/functional/bind.h"\n'
                     '#include "base/logging.h"\n',
                     'postinstall logging include')
if '#include "chrome/browser/cmux_term/cmux_extensions_container.h"' not in s:
    s = replace_once(s, '#include "build/build_config.h"\n',
                     '#include "build/build_config.h"\n'
                     '#include "chrome/browser/cmux_term/cmux_extensions_container.h"\n'
                     '#include "chrome/browser/cmux_term/cmux_views.h"\n',
                     'postinstall cmux includes')
if '[[maybe_unused]] void ShowAppInstalledNotification' not in s:
    s = replace_once(s, 'void ShowAppInstalledNotification(\n',
                     '[[maybe_unused]] void ShowAppInstalledNotification(\n',
                     'postinstall app notification maybe_unused')
old_pristine = '''  // Extensions aren't enabled by default in incognito so we confirm
  // the install in a normal window.
  Profile* current_profile = profile()->GetOriginalProfile();
  BrowserWindowInterface* browser_window =
      FindOrCreateVisibleBrowser(current_profile);
  CHECK(browser_window);

  if (!extension->is_app()) {
    SkBitmap icon_to_use = icon ? *icon : SkBitmap();
    extensions::TriggerPostInstallDialog(
        profile(), extension, icon_to_use,
        base::BindOnce(
            [](BrowserWindowInterface* bwi) {
              return bwi->GetActiveTabInterface()->GetContents();
            },
            browser_window));
    return;
  }

  if (!use_app_installed_bubble()) {
    ShowAppInstalledNotification(extension, profile());
    return;
  }
'''
old_guard = '''  // Extensions aren't enabled by default in incognito so we confirm
  // the install in a normal window.
  Profile* current_profile = profile()->GetOriginalProfile();
  // cmux: no tabbed Browser windows exist; FindOrCreateVisibleBrowser would
  // spawn a stray tabbed window just to parent the post-install toast (cmux
  // panes are not Browser tabs). Skip the toast; the install succeeded.
  if (!chrome::FindTabbedBrowser(current_profile,
                                 /*match_original_profiles=*/false)) {
    return;
  }
  BrowserWindowInterface* browser_window =
      FindOrCreateVisibleBrowser(current_profile);
  CHECK(browser_window);

  if (!extension->is_app()) {
    SkBitmap icon_to_use = icon ? *icon : SkBitmap();
    extensions::TriggerPostInstallDialog(
        profile(), extension, icon_to_use,
        base::BindOnce(
            [](BrowserWindowInterface* bwi) {
              return bwi->GetActiveTabInterface()->GetContents();
            },
            browser_window));
    return;
  }

  if (!use_app_installed_bubble()) {
    ShowAppInstalledNotification(extension, profile());
    return;
  }
'''
new_block = '''  if (!extension->is_app()) {
    SkBitmap icon_to_use = icon ? *icon : SkBitmap();
    extensions::TriggerPostInstallDialog(
        profile(), extension, icon_to_use,
        base::BindOnce(
            [](Profile* dialog_profile,
               scoped_refptr<const extensions::Extension> dialog_extension,
               const SkBitmap& dialog_icon) -> content::WebContents* {
              // cmux: post-install dialogs are hosted by the cmux extension
              // strip when the puzzle button exists. Returning nullptr tells
              // TriggerPostInstallDialog that the dialog was already shown.
              if (cmux::ShowExtensionPostInstallDialogAtPuzzle(
                      dialog_profile, dialog_extension, dialog_icon)) {
                return nullptr;
              }
              content::WebContents* web_contents =
                  cmux::GetActiveCmuxWebContents();
              if (!web_contents) {
                LOG(WARNING)
                    << "cmux: skipping post-install dialog; no active pane";
              }
              return web_contents;
            },
            profile(), extension, icon_to_use));
    return;
  }

  if (!use_app_installed_bubble()) {
    // cmux: app install notifications navigate a visible tabbed Browser; cmux
    // panes do not surface Chrome apps, so avoid creating/showing Browser UI.
    return;
  }
'''
if 'cmux: post-install dialogs are hosted by the cmux extension' in s:
    print('postinstall-cmux-dialog: already patched')
elif old_guard in s:
    s = s.replace(old_guard, new_block, 1)
    open(p, 'w', newline='\n').write(s)
    print('postinstall-cmux-dialog: patched old guard')
elif old_pristine in s:
    s = s.replace(old_pristine, new_block, 1)
    open(p, 'w', newline='\n').write(s)
    print('postinstall-cmux-dialog: patched pristine')
else:
    raise AssertionError('postinstall-cmux-dialog anchor not found')

print("--- next patch ---")

p = 'chrome/browser/ui/extensions/extension_post_install_dialog.h'
s = open(p).read()
if '#include <memory>' not in s:
    s = replace_once(s,
                     '#define CHROME_BROWSER_UI_EXTENSIONS_EXTENSION_POST_INSTALL_DIALOG_H_\n\n',
                     '#define CHROME_BROWSER_UI_EXTENSIONS_EXTENSION_POST_INSTALL_DIALOG_H_\n\n'
                     '#include <memory>\n\n',
                     'postinstall builder h memory include')
if 'namespace ui {\nclass DialogModel;\n}' not in s:
    s = replace_once(s, 'namespace content {\nclass WebContents;\n}\n',
                     'namespace content {\nclass WebContents;\n}\n\n'
                     'namespace ui {\nclass DialogModel;\n}\n',
                     'postinstall builder h ui forward')
decl = '''// Builds the post-install dialog model without showing it. Embedders can host
// the returned stock Chrome dialog at their own anchor.
std::unique_ptr<ui::DialogModel> BuildExtensionPostInstallDialogModel(
    Profile* profile,
    content::WebContents* web_contents,
    scoped_refptr<const extensions::Extension> extension,
    const SkBitmap& icon);

'''
if 'BuildExtensionPostInstallDialogModel' in s:
    print('postinstall-dialog-builder-h: already patched')
else:
    s = replace_once(s, '// Triggers the post-install dialog for an extension.\n',
                     decl + '// Triggers the post-install dialog for an extension.\n',
                     'postinstall builder h decl')
    open(p, 'w', newline='\n').write(s)
    print('postinstall-dialog-builder-h: patched')

p = 'chrome/browser/ui/extensions/extension_post_install_dialog.cc'
s = open(p).read()
builder = '''std::unique_ptr<ui::DialogModel> BuildExtensionPostInstallDialogModel(
    Profile* profile,
    content::WebContents* web_contents,
    scoped_refptr<const extensions::Extension> extension,
    const SkBitmap& icon) {
  if (!web_contents || !extension) {
    return nullptr;
  }

  auto delegate = std::make_unique<ExtensionPostInstallDialog>(
      web_contents, std::make_unique<ExtensionPostInstallDialogModel>(
                        profile, extension.get(), icon));
  auto* weak_delegate = delegate.get();

  ui::DialogModel::Builder dialog_model_builder(std::move(delegate));
  auto manage_shortcuts_callback =
      base::BindRepeating(&ExtensionPostInstallDialog::LinkClicked,
                          base::Unretained(weak_delegate));
  ConfigurePostInstallDialogModel(dialog_model_builder, weak_delegate->model(),
                                  manage_shortcuts_callback);

#if !BUILDFLAG(IS_CHROMEOS) && !BUILDFLAG(IS_ANDROID)
  // Add a sync or sign in promo in the footer if it should be shown.
  extensions::ExtensionRegistry* registry =
      extensions::ExtensionRegistry::Get(profile);
  const extensions::Extension* enabled_extension =
      registry->enabled_extensions().GetByID(
          weak_delegate->model()->extension_id());

  if (enabled_extension) {
    extensions::MaybeAddSigninPromoFootnoteView(
        profile, web_contents, *enabled_extension, dialog_model_builder);
  }
#endif  // !BUILDFLAG(IS_CHROMEOS) && !BUILDFLAG(IS_ANDROID)

  return dialog_model_builder.Build();
}

'''
if 'void ConfigurePostInstallDialogModel(\n    Profile* profile,' in s:
    builder = builder.replace(
        'ConfigurePostInstallDialogModel(dialog_model_builder,',
        'ConfigurePostInstallDialogModel(profile, dialog_model_builder,', 1)
if 'BuildExtensionPostInstallDialogModel' in s:
    print('postinstall-dialog-builder-cc: already patched')
else:
    s = replace_once(s, '}  // namespace\n\nvoid TriggerPostInstallDialog(',
                     '}  // namespace\n\n' + builder +
                     'void TriggerPostInstallDialog(',
                     'postinstall builder cc insertion')
    open(p, 'w', newline='\n').write(s)
    print('postinstall-dialog-builder-cc: patched')

print("--- next patch ---")


p = 'chrome/browser/extensions/extension_management.cc'
s = open(p).read()
allowed_anchor = '''bool ExtensionManagement::IsAllowedManifestVersion(
    int manifest_version,
    const std::string& extension_id,
    Manifest::Type manifest_type) {
  bool enabled_by_default =
      !base::FeatureList::IsEnabled(
          extensions_features::kExtensionsManifestV3Only) ||
      manifest_version >= 3;

  // Manifest version policy only supports normal extensions and Chrome OS login
  // screen extension.
  if (manifest_type != Manifest::Type::TYPE_EXTENSION &&
      manifest_type != Manifest::Type::TYPE_LOGIN_SCREEN_EXTENSION) {
    return enabled_by_default;
  }
  switch (global_settings_->manifest_v2_setting) {
    case internal::GlobalSettings::ManifestV2Setting::kDefault:
      return enabled_by_default;
    case internal::GlobalSettings::ManifestV2Setting::kDisabled:
      return manifest_version >= 3;
    case internal::GlobalSettings::ManifestV2Setting::kEnabled:
      return true;
    case internal::GlobalSettings::ManifestV2Setting::kEnabledForForceInstalled:
      auto installation_mode =
          GetInstallationMode(extension_id, /*update_url=*/std::string());
      return manifest_version >= 3 ||
             installation_mode == ManagedInstallationMode::kForced ||
             installation_mode == ManagedInstallationMode::kRecommended;
  }
}
'''
allowed_add = '''bool ExtensionManagement::IsAllowedManifestVersion(
    int manifest_version,
    const std::string& extension_id,
    Manifest::Type manifest_type) {
  // cmux: mirror Helium/ungoogled full-MV2 support. This keeps Manifest V2
  // extensions loadable even when upstream MV2 deprecation features/policies
  // would otherwise reject them.
  return true;
}
'''
exempt_anchor = '''bool ExtensionManagement::IsExemptFromMV2DeprecationByPolicy(
    int manifest_version,
    const std::string& extension_id,
    Manifest::Type manifest_type) {
  // This policy only affects MV2 extensions.
  if (manifest_version != 2) {
    return false;
  }
  if (manifest_type != Manifest::Type::TYPE_EXTENSION &&
      manifest_type != Manifest::Type::TYPE_LOGIN_SCREEN_EXTENSION) {
    return false;
  }

  switch (global_settings_->manifest_v2_setting) {
    case internal::GlobalSettings::ManifestV2Setting::kDefault:
      // Default browser behavior. Not exempt.
      return false;
    case internal::GlobalSettings::ManifestV2Setting::kDisabled:
      // All MV2 extensions are disallowed. Not exempt.
      return false;
    case internal::GlobalSettings::ManifestV2Setting::kEnabled:
      // All MV2 extensions are allowed. Exempt.
      return true;
    case internal::GlobalSettings::ManifestV2Setting::kEnabledForForceInstalled:
      // Force-installed MV2 extensions are allowed. Exempt if it's a force-
      // installed extension only.
      auto installation_mode =
          GetInstallationMode(extension_id, /*update_url=*/std::string());
      return installation_mode == ManagedInstallationMode::kForced ||
             installation_mode == ManagedInstallationMode::kRecommended;
  }

  return false;
}
'''
exempt_add = '''bool ExtensionManagement::IsExemptFromMV2DeprecationByPolicy(
    int manifest_version,
    const std::string& extension_id,
    Manifest::Type manifest_type) {
  // This policy only affects MV2 extensions.
  if (manifest_version != 2) {
    return false;
  }
  if (manifest_type != Manifest::Type::TYPE_EXTENSION &&
      manifest_type != Manifest::Type::TYPE_LOGIN_SCREEN_EXTENSION) {
    return false;
  }

  // cmux: mirror Helium/ungoogled full-MV2 support. MV2 extensions that pass
  // the basic type/version checks are exempt from upstream deprecation policy.
  return true;
}
'''
changed = False
if 'cmux: mirror Helium/ungoogled full-MV2 support. This keeps Manifest V2' in s:
    print('mv2-extension-management-allowed: already patched')
elif allowed_anchor in s:
    s = s.replace(allowed_anchor, allowed_add, 1)
    changed = True
else:
    # M150 changed Manifest::Type spellings and added user scripts to this
    # policy. Replace the complete overload by its stable neighboring overload
    # instead of duplicating every upstream branch in another exact anchor.
    start = s.find('bool ExtensionManagement::IsAllowedManifestVersion(\n'
                   '    int manifest_version,')
    if start == -1:
        # M151 removed these ExtensionManagement policy hooks. Installation
        # and enable blocking moved to ManifestV2Handler below.
        handler = Path('extensions/browser/manifest_v2_handler.cc')
        assert handler.is_file() and \
            'ShouldBlockExtensionInstallation' in handler.read_text(), (
                'mv2 extension-management replacement handler not found')
        print('mv2-extension-management-allowed: moved upstream')
    else:
        end = s.find(
            'bool ExtensionManagement::IsAllowedManifestVersion(const Extension* extension)',
            start)
        assert end != -1, (
            'mv2 extension-management allowed function end not found')
        s = s[:start] + allowed_add + '\n' + s[end:]
        changed = True
if 'cmux: mirror Helium/ungoogled full-MV2 support. MV2 extensions that pass' in s:
    print('mv2-extension-management-exempt: already patched')
elif exempt_anchor in s:
    s = s.replace(exempt_anchor, exempt_add, 1)
    changed = True
else:
    start = s.find('bool ExtensionManagement::IsExemptFromMV2DeprecationByPolicy(')
    if start == -1:
        handler = Path('extensions/browser/manifest_v2_handler.cc')
        assert handler.is_file() and \
            'ShouldBlockExtensionEnable' in handler.read_text(), (
                'mv2 extension-management exemption replacement handler not found')
        print('mv2-extension-management-exempt: moved upstream')
    else:
        end = s.find(
            'bool ExtensionManagement::IsAllowedByUnpublishedAvailabilityPolicy(',
            start)
        assert end != -1, (
            'mv2 extension-management exemption function end not found')
        old_function = s[start:end]
        if 'Manifest::Type::kExtension' in old_function:
            # M150 enum spellings and its newly-supported user-script type.
            replacement = '''bool ExtensionManagement::IsExemptFromMV2DeprecationByPolicy(
    int manifest_version,
    const std::string& extension_id,
    Manifest::Type manifest_type) {
  // This policy only affects MV2 extensions.
  if (manifest_version != 2) {
    return false;
  }
  if (manifest_type != Manifest::Type::kExtension &&
      manifest_type != Manifest::Type::kLoginScreenExtension &&
      manifest_type != Manifest::Type::kUserScript) {
    return false;
  }

  // cmux: mirror Helium/ungoogled full-MV2 support. MV2 extensions that pass
  // the basic type/version checks are exempt from upstream deprecation policy.
  return true;
}
'''
        else:
            replacement = exempt_add
        s = s[:start] + replacement + '\n' + s[end:]
        changed = True
if changed:
    open(p, 'w').write(s)
    print('mv2-extension-management: patched')

print("--- next patch ---")

p_candidates = (
    # Chromium 149: the manager was still Chrome-layer implementation code.
    'chrome/browser/extensions/manifest_v2_experiment_manager.cc',
    # Chromium 150: upstream moved the implementation into extensions/browser.
    'extensions/browser/manifest_v2_experiment_manager.cc',
    # Chromium 151: the experiment manager became the final MV2 handler.
    'extensions/browser/manifest_v2_handler.cc',
)
existing_paths = [candidate for candidate in p_candidates if Path(candidate).is_file()]
assert len(existing_paths) == 1, (
    'MV2 experiment manager implementation not uniquely identified')
p = existing_paths[0]
s = open(p).read()
anchor_149 = '''bool ShouldDisableLegacyExtensions(MV2ExperimentStage stage) {
  if (g_allow_mv2_for_testing) {
    // We allow legacy MV2 extensions for testing purposes.
    return false;
  }

  switch (stage) {
    case MV2ExperimentStage::kWarning:
      return false;
    case MV2ExperimentStage::kDisableWithReEnable:
    case MV2ExperimentStage::kUnsupported:
      return true;
  }
}
'''
anchor_150 = '''bool ShouldDisableLegacyExtensions(MV2ExperimentStage stage) {
  if (g_allow_mv2_for_testing) {
    // We allow legacy MV2 extensions for testing purposes.
    return false;
  }

  return true;
}
'''
anchor_151 = '''bool ShouldDisableLegacyExtensions() {
  if (g_allow_mv2_for_testing) {
    // We allow legacy MV2 extensions for testing purposes.
    return false;
  }

  return true;
}
'''
add = '''bool ShouldDisableLegacyExtensions(MV2ExperimentStage stage) {
  // cmux: mirror Helium/ungoogled full-MV2 support. Never let upstream MV2
  // experiment stages disable legacy extensions.
  return false;
}
'''
add_151 = '''bool ShouldDisableLegacyExtensions() {
  // cmux: mirror Helium/ungoogled full-MV2 support. Never let upstream MV2
  // handling disable or block legacy extensions.
  return false;
}
'''
if 'cmux: mirror Helium/ungoogled full-MV2 support. Never let upstream MV2' in s:
    print('mv2-experiment-disable: already patched')
elif anchor_149 in s:
    open(p, 'w').write(s.replace(anchor_149, add, 1))
    print('mv2-experiment-disable: patched')
elif anchor_150 in s:
    open(p, 'w').write(s.replace(anchor_150, add, 1))
    print('mv2-experiment-disable: patched')
elif anchor_151 in s:
    open(p, 'w').write(s.replace(anchor_151, add_151, 1))
    print('mv2-experiment-disable: patched')
else:
    raise AssertionError('MV2 disable policy anchor not found')

print("--- next patch ---")

p = 'chrome/browser/extensions/api/developer_private/extension_info_generator.cc'
s = open(p).read()
anchor = '''  // MV2 deprecation.
  ManifestV2ExperimentManager* mv2_experiment_manager =
      ManifestV2ExperimentManager::Get(profile);
  CHECK(mv2_experiment_manager);
  info.is_affected_by_mv2_deprecation =
      mv2_experiment_manager->IsExtensionAffected(extension);
  info.did_acknowledge_mv2_deprecation_notice =
      mv2_experiment_manager->DidUserAcknowledgeNotice(extension.id());
  if (info.web_store_url.length() > 0) {
    info.recommendations_url =
        extension_urls::GetNewWebstoreItemRecommendationsUrl(extension.id())
            .spec();
  }
'''
add = '''  // cmux: full MV2 extensions remain supported, so do not surface MV2
  // deprecation state or recommendations in chrome://extensions.
  info.is_affected_by_mv2_deprecation = false;
  info.did_acknowledge_mv2_deprecation_notice = true;
'''
anchor_150 = '''  // MV2 deprecation.
  ManifestV2ExperimentManager* mv2_experiment_manager =
      ManifestV2ExperimentManager::Get(profile);
  CHECK(mv2_experiment_manager);
  info.is_affected_by_mv2_deprecation =
      mv2_experiment_manager->IsExtensionAffected(extension);
  if (info.web_store_url.length() > 0) {
    info.recommendations_url =
        extension_urls::GetNewWebstoreItemRecommendationsUrl(extension.id())
            .spec();
  }
'''
anchor_151 = '''  // MV2 deprecation.
  ManifestV2Handler* mv2_handler = ManifestV2Handler::Get(profile);
  CHECK(mv2_handler);
  info.is_affected_by_mv2_deprecation =
      mv2_handler->IsExtensionAffected(extension);
  if (info.web_store_url.length() > 0) {
    info.recommendations_url =
        extension_urls::GetNewWebstoreItemRecommendationsUrl(extension.id())
            .spec();
  }
'''
add_150 = '''  // cmux: full MV2 extensions remain supported, so do not surface MV2
  // deprecation state or recommendations in chrome://extensions.
  info.is_affected_by_mv2_deprecation = false;
'''
if 'cmux: full MV2 extensions remain supported' in s:
    print('mv2-developer-private-ui: already patched')
elif anchor in s:
    open(p, 'w').write(s.replace(anchor, add, 1))
    print('mv2-developer-private-ui: patched')
elif anchor_150 in s:
    open(p, 'w').write(s.replace(anchor_150, add_150, 1))
    print('mv2-developer-private-ui: patched')
elif anchor_151 in s:
    open(p, 'w').write(s.replace(anchor_151, add_150, 1))
    print('mv2-developer-private-ui: patched')
else:
    raise AssertionError('MV2 developer-private UI anchor not found')

print("--- next patch ---")

p = 'chrome/browser/ui/webui/extensions/extensions_ui.cc'
s = open(p).read()
anchor = '''  // MV2 deprecation.
  auto* mv2_experiment_manager = ManifestV2ExperimentManager::Get(profile);
  MV2ExperimentStage experiment_stage =
      mv2_experiment_manager->GetCurrentExperimentStage();
  source->AddInteger("MV2ExperimentStage", static_cast<int>(experiment_stage));
  source->AddBoolean(
      "MV2DeprecationNoticeDismissed",
      mv2_experiment_manager->DidUserAcknowledgeNoticeGlobally());
'''
add = '''  // cmux: full MV2 extensions remain supported; keep the extensions page in
  // its warning-era state with the notice treated as dismissed.
  source->AddInteger("MV2ExperimentStage",
                     static_cast<int>(MV2ExperimentStage::kWarning));
  source->AddBoolean("MV2DeprecationNoticeDismissed", true);
'''
anchor_150 = '''  // MV2 deprecation.
  auto* mv2_experiment_manager = ManifestV2ExperimentManager::Get(profile);
  source->AddBoolean(
      "MV2DeprecationNoticeDismissed",
      mv2_experiment_manager->DidUserAcknowledgeNoticeGlobally());
'''
anchor_151 = '''  // MV2 deprecation.
  auto* mv2_handler = ManifestV2Handler::Get(profile);
  source->AddBoolean("MV2DeprecationNoticeDismissed",
                     mv2_handler->DidUserAcknowledgeNoticeGlobally());
'''
add_150 = '''  // cmux: full MV2 extensions remain supported; the M150+ extensions page
  // no longer consumes an experiment-stage value, so only dismiss its notice.
  source->AddBoolean("MV2DeprecationNoticeDismissed", true);
'''
if 'cmux: full MV2 extensions remain supported; keep the extensions page' in s:
    print('mv2-extensions-webui: already patched')
elif 'cmux: full MV2 extensions remain supported; the M150+ extensions page' in s:
    print('mv2-extensions-webui: already patched')
elif anchor in s:
    open(p, 'w').write(s.replace(anchor, add, 1))
    print('mv2-extensions-webui: patched')
elif anchor_150 in s:
    open(p, 'w').write(s.replace(anchor_150, add_150, 1))
    print('mv2-extensions-webui: patched')
elif anchor_151 in s:
    open(p, 'w').write(s.replace(anchor_151, add_150, 1))
    print('mv2-extensions-webui: patched')
else:
    raise AssertionError('MV2 extensions WebUI anchor not found')

print("--- next patch ---")

p = 'extensions/common/extension.cc'
s = open(p).read()
anchor = '''    // Emit a warning for unpacked extensions on Manifest V2 warning that
    // MV2 is deprecated.
    if (type == Manifest::Type::kExtension && manifest_version == 2 &&
        Manifest::IsUnpackedLocation(location) &&
        !g_silence_deprecated_manifest_version_warnings) {
      *warning = errors::kManifestV2IsDeprecatedWarning;
    }
'''
anchor_151 = '''    // Emit a warning for unpacked extensions on Manifest V2 warning that
    // MV2 is deprecated.
    if (type == Manifest::Type::kExtension && manifest_version == 2 &&
        Manifest::IsUnpackedLocation(location)) {
      *warning = errors::kManifestV2IsDeprecatedWarning;
    }
'''
add = '''    // cmux: full MV2 extensions remain supported, so unpacked MV2 loads should
    // not get a deprecation warning.
'''
if 'cmux: full MV2 extensions remain supported, so unpacked MV2 loads' in s:
    print('mv2-unpacked-warning: already patched')
elif anchor in s:
    open(p, 'w').write(s.replace(anchor, add, 1))
    print('mv2-unpacked-warning: patched')
elif anchor_151 in s:
    open(p, 'w').write(s.replace(anchor_151, add, 1))
    print('mv2-unpacked-warning: patched')
else:
    print('mv2-unpacked-warning: ANCHOR NOT FOUND')
print("APPLY_WIN_MV2 OK")
