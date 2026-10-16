#!/bin/bash
# Apply edits to EXISTING chromium files on the builder: register the
# cmux_term extra parts and add its source + dep to chrome/browser/BUILD.gn.
# Idempotent.
set -euo pipefail
. "$(dirname "$0")/builder-transport.sh"
HOST="${CMUX_BUILDER:-ec2-user@aws-m4pro-1}"
SRC="${CHROMIUM_SRC:-/Volumes/recpass123/cmux-ci/cmux-browser-benchmark/src}"

# CMUX_TARGET selects how much to patch:
#   full           (default) -- the whole macOS app: libghostty vendoring, the
#                  startup hook + chrome/browser/BUILD.gn cmux sources (mac-only
#                  .mm + cross-platform .cc), omnibox seam, frameworks, etc.
#                  PLUS the cross-platform views_examples demo (step 9).
#   chrome_linux   -- the cmux app on Linux: the cross-platform cmux sources
#                  (.cc) + startup hook (is_mac||is_linux||is_win) + omnibox
#                  seam, but NO mac-only .mm or frameworks. Linux/Windows
#                  terminal panes require their separately staged Ghostty
#                  artifacts.
#   views_examples -- ONLY the cross-platform demo prerequisites + wiring: the
#                  standalone `views_examples` target (rail + strip + tab
#                  strip) on ANY platform incl. Linux, with no
#                  Ghostty/AppKit/mac tools.
MODE="${CMUX_TARGET:-full}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
EXPECTED_GHOSTTY_REVISION="$(tr -d '[:space:]' < "$ROOT/ghostty-revision.txt")"
echo "apply.sh: CMUX_TARGET=$MODE host=$HOST src=$SRC"

MAC_BUNDLE_ID="${CMUX_MAC_BUNDLE_ID:-com.cmux.app}"
case "$MAC_BUNDLE_ID" in
  com.cmux.app|com.cmux.app.*) ;;
  *)
    echo "error: CMUX_MAC_BUNDLE_ID must be com.cmux.app or a com.cmux.app.* channel ID" >&2
    exit 2
    ;;
esac
printf -v REMOTE_MAC_BUNDLE_ID '%q' "$MAC_BUNDLE_ID"

# Version-gate source-layout patches before applying them. Chromium 151 made
# BubbleDialogDelegateView's legacy constructor private; the supported 148-150
# bases retain the public constructor and do not have its 151-only friend-list
# anchor.
CHROMIUM_MAJOR="$(ssh -o BatchMode=yes "$HOST" "awk -F= '\$1 == \"MAJOR\" { print \$2 }' '$SRC/chrome/VERSION'")"
HELIUM_NTP_PATCH=".cmux-patches/helium-new-tab.patch"
HELIUM_MEDIA_TOOLBAR_PATCH=".cmux-patches/helium-media-toolbar.patch"
case "$CHROMIUM_MAJOR" in
  148|149)
    HELIUM_OMNIBAR_PATCH=".cmux-patches/helium-omnibar-chromium-149.patch"
    HELIUM_SETTINGS_PATCH=".cmux-patches/helium-settings-chromium-149.patch"
    CMUX_PINNED_TOOLBAR_PATCH=".cmux-patches/cmux-pinned-toolbar-actions-chromium-149.patch"
    ;;
  150)
    HELIUM_OMNIBAR_PATCH=".cmux-patches/helium-omnibar-chromium-149.patch"
    HELIUM_SETTINGS_PATCH=".cmux-patches/helium-settings-chromium-149.patch"
    CMUX_PINNED_TOOLBAR_PATCH=".cmux-patches/cmux-pinned-toolbar-actions-chromium-150.patch"
    ;;
  151)
    HELIUM_OMNIBAR_PATCH=".cmux-patches/helium-omnibar-chromium-151.patch"
    HELIUM_SETTINGS_PATCH=".cmux-patches/helium-settings-chromium-151.patch"
    CMUX_PINNED_TOOLBAR_PATCH=".cmux-patches/cmux-pinned-toolbar-actions-chromium-151.patch"
    ;;
  *)
    echo "helium-ui: unsupported Chromium major $CHROMIUM_MAJOR" >&2
    exit 1
    ;;
esac
echo "helium-ui: Chromium $CHROMIUM_MAJOR using $HELIUM_OMNIBAR_PATCH, $HELIUM_SETTINGS_PATCH, $HELIUM_NTP_PATCH, $HELIUM_MEDIA_TOOLBAR_PATCH, and $CMUX_PINNED_TOOLBAR_PATCH"

# Apply the product identity before any platform build. This keeps the macOS
# bundle/team constants used by WebAuthn, Windows shell registration, and Linux
# desktop/package metadata aligned with one cmux identity.
ssh -o BatchMode=yes "$HOST" \
  "cd '$SRC' && CMUX_MAC_BUNDLE_ID=$REMOTE_MAC_BUNDLE_ID python3 .cmux-patches/platform_metadata.py"

# Chromium 151 makes BubbleDialogDelegateView's legacy constructor private and
# grandfathers existing hover-card subclasses. Both the shipped app and the
# views_examples rail instantiate cmux's BubbleSlideAnimator-compatible hover
# card, so keep this prerequisite outside the app-only patch block, but never
# apply its 151-only friend-list transformation to the 148-150 source layout.
if [ "$CHROMIUM_MAJOR" -ge 151 ]; then
  ssh -o BatchMode=yes "$HOST" "cd '$SRC' && python3 - <<'PYEOF'
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
open(p, 'w').write(s)
PYEOF"
else
  echo "cmux-hover-card-friend: not required on Chromium $CHROMIUM_MAJOR"
fi

if [ "$MODE" = "full" ]; then
  MAC_BUNDLE_ID="${CMUX_MAC_BUNDLE_ID:-com.cmux.app}"
  case "$MAC_BUNDLE_ID" in
    com.cmux.app|com.cmux.app.*) ;;
    *)
      echo "error: CMUX_MAC_BUNDLE_ID must be com.cmux.app or a com.cmux.app.* channel ID" >&2
      exit 2
      ;;
  esac
  printf -v REMOTE_MAC_BUNDLE_ID '%q' "$MAC_BUNDLE_ID"

  # Apply the compiled macOS identity before generating build files. This owns
  # the executable/framework/helper names, bundle ID, URL scheme, and WebAuthn
  # keychain groups; deploy.sh must validate this identity, never rewrite it.
  ssh -o BatchMode=yes "$HOST" \
    "cd '$SRC' && CMUX_MAC_BUNDLE_ID=$REMOTE_MAC_BUNDLE_ID python3 .cmux-patches/macos_product_identity.py"
fi

if [ "$MODE" = "full" ] || [ "$MODE" = "chrome_linux" ] || [ "$MODE" = "chrome_win" ]; then
  # Unbranded Chromium otherwise gives Crashpad an empty upload URL. Keep the
  # macOS branch unchanged (native cmux owns its own Sentry integration), while
  # wiring Windows/Linux to the dedicated cmux-browser Native project.
  ssh -o BatchMode=yes "$HOST" \
    "cd '$SRC' && python3 .cmux-patches/sentry_crashpad.py --chromium-src ."
fi

if [ "$MODE" = "full" ]; then
# 0. Vendor the prebuilt libghostty (static lib + headers) into the tree so
# gn references in-tree paths, not absolute home paths.
# Revision-qualified staging prevents a previous runner's GhosttyKit from being
# silently reused after the browser pin advances. Builders may override this
# path explicitly, but the receipt below remains authoritative either way.
GHOSTTY_KIT="${GHOSTTY_KIT:-\$HOME/cmux-ghostty-kits/$EXPECTED_GHOSTTY_REVISION-macos-arm64}"
ssh -o BatchMode=yes "$HOST" "set -e; cd '$SRC'
  # Idempotent: skip re-vendoring if the static lib + header are already in the
  # tree. The source xcframework is a transient runner artifact whose path moves
  # (and can vanish) between builds; once vendored, the in-tree copy is all that
  # the build needs, so a missing source must NOT abort the later patch steps.
  if [ -f third_party/cmux_ghostty/lib/ghostty-internal.a ] \
     && [ -f third_party/cmux_ghostty/include/ghostty.h ] \
     && [ -f third_party/cmux_ghostty/REVISION ] \
     && [ \"\$(tr -d '[:space:]' < third_party/cmux_ghostty/REVISION)\" = \"$EXPECTED_GHOSTTY_REVISION\" ]; then
    echo 'ghostty already vendored; skipping re-vendor'; exit 0
  fi
  if [ ! -f \"$GHOSTTY_KIT/REVISION\" ] \
     || [ \"\$(tr -d '[:space:]' < \"$GHOSTTY_KIT/REVISION\")\" != \"$EXPECTED_GHOSTTY_REVISION\" ]; then
    echo 'ERROR: staged GhosttyKit revision does not match browser pin' >&2
    exit 1
  fi
  mkdir -p third_party/cmux_ghostty/lib third_party/cmux_ghostty/include
  # The receipt is the skip authority. Remove it (and the old archive) before
  # replacing anything, then publish it only after every transform succeeds;
  # an interrupted ld/vtool/libtool run must never bless stale code as new.
  rm -f third_party/cmux_ghostty/REVISION \
    third_party/cmux_ghostty/lib/ghostty-internal.a
  cp \"$GHOSTTY_KIT/Headers/ghostty.h\" \
    third_party/cmux_ghostty/include/ghostty.h.next
  mv third_party/cmux_ghostty/include/ghostty.h.next \
    third_party/cmux_ghostty/include/ghostty.h
  cd third_party/cmux_ghostty/lib
  # GhosttyKit ships a fat archive whose objects target macOS 13; chrome
  # links at 12, and lld errors on the newer minos. Thin to arm64, merge all
  # objects (ld64 only warns) into one relocatable, rewrite its build version
  # to 12.0 with vtool, repack. Deterministic and dup-name safe (the merge
  # collapses the one duplicate member).
  cp \"$GHOSTTY_KIT/libghostty-internal-fat.a\" fat.a
  lipo fat.a -thin arm64 -output arm64.a 2>/dev/null || cp fat.a arm64.a
  # Export only the ghostty_* C API; everything else libghostty bundles
  # (simdutf, freetype, harfbuzz, zig std, ...) becomes a local/hidden symbol
  # so it cannot collide with chrome's own copies at the final link.
  printf '_ghostty_*\n' > ghostty_exports.txt
  xcrun ld -r -arch arm64 -platform_version macos 12.0 12.0 -all_load arm64.a \
    -exported_symbols_list ghostty_exports.txt -o merged.o 2>/dev/null
  vtool -set-build-version macos 12.0 12.0 -replace -output merged.o merged.o
  libtool -static -o ghostty-internal.a.next merged.o
  mv ghostty-internal.a.next ghostty-internal.a
  rm -f fat.a arm64.a merged.o ghostty_exports.txt
  cp \"$GHOSTTY_KIT/REVISION\" ../REVISION.next
  mv ../REVISION.next ../REVISION
  echo vendored libghostty minos: \$(otool -l ghostty-internal.a 2>/dev/null | grep -m1 minos || echo via-merged)"
fi  # end MODE=full (libghostty vendor)

if [ "$MODE" = "full" ]; then
# Mark GhosttyKit builds that carry cmux's optional offscreen extension. This
# keeps the regular AppKit terminal build-compatible with older cached kits on
# fallback builders while enabling the compositor experiment on patched kits.
ssh -o BatchMode=yes "$HOST" "cd '$SRC' && python3 - <<'PYEOF'
p = 'third_party/cmux_ghostty/include/ghostty.h'
s = open(p).read()
marker = '#define CMUX_GHOSTTY_HAS_OFFSCREEN 1'
if 'ghostty_surface_set_frame_callback' in s and marker not in s:
    s = marker + '\n' + s
    open(p, 'w').write(s)
PYEOF"

fi

if [ "$MODE" = "full" ] || [ "$MODE" = "chrome_linux" ] || [ "$MODE" = "chrome_win" ]; then
ssh -o BatchMode=yes "$HOST" "cd '$SRC' && CMUX_MODE='$MODE' python3 - <<'PYEOF'
import json
import os
MODE = os.environ.get('CMUX_MODE', 'full')

# 1. chrome_browser_main.cc: include + AddParts. The cmux niri window is the app
#    on macOS, Linux, and Windows, so gate the hook on all three.
p = 'chrome/browser/chrome_browser_main.cc'
s = open(p).read()
inc = '#include \"chrome/browser/cmux_term/cmux_term.h\"'
if inc not in s:
    anchor = '#include \"chrome/browser/chrome_browser_main.h\"'
    s = s.replace(anchor, anchor + '\n' + inc, 1)
reg = 'std::make_unique<ChromeBrowserMainExtraPartsCmuxTerm>()'
if reg not in s:
    mac = ('#if BUILDFLAG(IS_MAC)\n'
           '  main_parts->AddParts(std::make_unique<ChromeBrowserMainExtraPartsMac>());\n'
           '#endif')
    s = s.replace(mac, mac + '\n\n#if BUILDFLAG(IS_MAC) || BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_WIN)\n'
                  '  main_parts->AddParts(\n'
                  '      std::make_unique<ChromeBrowserMainExtraPartsCmuxTerm>());\n'
                  '#endif', 1)
open(p, 'w').write(s)

# 1b. chrome_main_delegate.cc: choose the profile before Chrome resolves
#     chrome::DIR_USER_DATA and acquires the process singleton. Tagged macOS
#     bundles carry CrProductDirName, so honor that isolated Application
#     Support directory. Untagged builds retain ~/.cmux-profile; an explicit
#     --user-data-dir remains authoritative.
p = 'chrome/app/chrome_main_delegate.cc'
s = open(p).read()
profile_marker = 'cmux: tagged macOS bundles honor CrProductDirName'
legacy_profile_marker = \"cmux: this fork's default profile is ~/.cmux-profile\"
profile_anchor = ('  // Initialize the user data dir for any process type that needs it.\n'
                  '  if (chrome::ProcessNeedsProfileDir(process_type)) {\n'
                  '    InitializeUserDataDir(base::CommandLine::ForCurrentProcess());\n'
                  '  }\n')
profile_add = ('  // Initialize the user data dir for any process type that needs it.\n'
               '  if (chrome::ProcessNeedsProfileDir(process_type)) {\n'
               '    // cmux: tagged macOS bundles honor CrProductDirName so separate\n'
               '    // dogfood apps can run concurrently. Untagged builds retain the\n'
               '    // persistent ~/.cmux-profile default used by scripts/run.sh. An\n'
               '    // explicit --user-data-dir always wins. Child processes copy this\n'
               '    // switch from the browser command line.\n'
               '    {\n'
               '      base::CommandLine* cmux_cl = base::CommandLine::ForCurrentProcess();\n'
               '      if (!cmux_cl->HasSwitch(switches::kUserDataDir)) {\n'
               '#if BUILDFLAG(IS_MAC)\n'
               '        base::FilePath cmux_default_profile;\n'
               '        if (chrome::GetDefaultUserDataDirectory(&cmux_default_profile) &&\n'
               '            cmux_default_profile.BaseName().value() !=\n'
               '                FILE_PATH_LITERAL(\"Chromium\") &&\n'
               '            cmux_default_profile.BaseName().value() !=\n'
               '                FILE_PATH_LITERAL(\"cmux\")) {\n'
               '          cmux_cl->AppendSwitchPath(switches::kUserDataDir,\n'
               '                                    cmux_default_profile);\n'
               '        } else\n'
               '#endif\n'
               '        {\n'
               '          base::FilePath cmux_home;\n'
               '          if (base::PathService::Get(base::DIR_HOME, &cmux_home) &&\n'
               '              !cmux_home.empty()) {\n'
               '            cmux_cl->AppendSwitchPath(\n'
               '                switches::kUserDataDir,\n'
               '                cmux_home.Append(FILE_PATH_LITERAL(\".cmux-profile\")));\n'
               '          }\n'
               '        }\n'
               '      }\n'
               '    }\n'
               '    InitializeUserDataDir(base::CommandLine::ForCurrentProcess());\n'
               '  }\n')
if profile_marker in s:
    stable_guard = ('            cmux_default_profile.BaseName().value() !=\n'
                    '                FILE_PATH_LITERAL(\"cmux\")')
    if stable_guard not in s:
        legacy_guard = ('            cmux_default_profile.BaseName().value() !=\n'
                        '                FILE_PATH_LITERAL(\"Chromium\")) {')
        replacement_guard = ('            cmux_default_profile.BaseName().value() !=\n'
                             '                FILE_PATH_LITERAL(\"Chromium\") &&\n'
                             + stable_guard + ') {')
        assert s.count(legacy_guard) == 1, 'legacy cmux profile guard not unique'
        s = s.replace(legacy_guard, replacement_guard, 1)
        open(p, 'w').write(s)
        print('apply: cmux-default-profile stable guard updated')
    else:
        print('apply: cmux-default-profile already patched')
elif legacy_profile_marker in s:
    marker_offset = s.index(legacy_profile_marker)
    start = s.rfind(
        '  // Initialize the user data dir for any process type that needs it.\n',
        0, marker_offset)
    end_marker = ('    InitializeUserDataDir('
                  'base::CommandLine::ForCurrentProcess());\n  }\n')
    end = s.index(end_marker, marker_offset) + len(end_marker)
    assert start >= 0, 'legacy cmux default profile start not found'
    s = s[:start] + profile_add + s[end:]
    open(p, 'w').write(s)
    print('apply: migrated cmux-default-profile to tagged bundle isolation')
else:
    assert s.count(profile_anchor) == 1, 'cmux default profile anchor not unique'
    s = s.replace(profile_anchor, profile_add, 1)
    open(p, 'w').write(s)
    print('apply: cmux-default-profile patched')

# 2. chrome/browser/BUILD.gn: register cmux sources. Cross-platform .cc go in an
#    is_mac||is_linux||is_win block. Platform startup glue and the Ghostty theme
#    implementation have exactly one owner per platform: chrome/browser on
#    Linux/Windows, and cmux_ghostty on macOS. mac-only .mm + the cmux_ghostty
#    dep stay in is_mac (full mode only). Idempotent: skips when the cmux block is
#    already present (this was the double-insert trap — 'Duplicate object file
#    cmux_term.o'). NOTE the skip does NOT refresh a stale source list: after
#    editing xplat/maconly, reset the file first
#    (git checkout -- chrome/browser/BUILD.gn).
p = 'chrome/browser/BUILD.gn'
s = open(p).read()

# Older cmux overlays may already have the marker while carrying a stale source
# list. Reconcile the TUI backend entries in place instead of asking callers to
# reset Chromium's BUILD.gn (which can discard unrelated, newer overlay work).
if '# cmux cross-platform' in s:
    # sync.sh owns chrome/browser/cmux_term with --delete. Reconcile both
    # retired files and newly added backend/UI/updater files so warm incremental
    # builders cannot retain a stale BUILD.gn source list.
    import os
    import re
    quote = chr(34)
    modified = False
    stale = []
    source_pattern = quote + r'(cmux_term/[^' + quote + r' ]+)' + quote
    for candidate in sorted(set(re.findall(source_pattern, s))):
        if not os.path.exists(os.path.join('chrome/browser', candidate)):
            stale.append(candidate)
    if stale:
        for candidate in stale:
            line_pattern = (r'^\s*' + re.escape(quote + candidate + quote) +
                            r',\n')
            s = re.sub(line_pattern, '', s, flags=re.MULTILINE)
        modified = True
        print('apply: BUILD.gn removed retired cmux sources: ' +
              ', '.join(stale))

    xplat_anchor = f'      {quote}cmux_term/cmux_layout_config.cc{quote},\n'
    xplat_missing = [
        'cmux_term/cmux_ghostty_opengl_host.h',
        'cmux_term/cmux_terminal_backend.h',
        'cmux_term/cmux_terminal_backend.cc',
        'cmux_term/cmux_terminal_lifecycle.h',
        'cmux_term/cmux_terminal_placement.h',
        'cmux_term/cmux_terminal_placement.cc',
        'cmux_term/cmux_terminal_recovery.h',
        'cmux_term/cmux_terminal_recovery.cc',
        'cmux_term/cmux_tui_client.h',
        'cmux_term/cmux_tui_client.cc',
        'cmux_term/cmux_terminal_host_connection.h',
        'cmux_term/cmux_terminal_host_connection.cc',
        'cmux_term/cmux_tui_protocol.h',
        'cmux_term/cmux_tui_protocol.cc',
        'cmux_term/cmux_workspace_projection.h',
        'cmux_term/cmux_workspace_projection.cc',
        'cmux_term/cmux_tui_revision.h',
        'cmux_term/cmux_pane_extension_pins.h',
        'cmux_term/cmux_pane_extension_pins.cc',
        'cmux_term/cmux_side_panel.h',
        'cmux_term/cmux_side_panel.cc',
        'cmux_term/cmux_side_panel_resize_area.h',
        'cmux_term/cmux_side_panel_resize_area.cc',
        'cmux_term/cmux_browser_finder.h',
        'cmux_term/cmux_rail_animating_layout_manager.h',
        'cmux_term/cmux_rail_animating_layout_manager.cc',
        'cmux_term/cmux_rail_group_editor_bubble.h',
        'cmux_term/cmux_rail_group_editor_bubble.cc',
        'cmux_term/cmux_rail_hover_card.h',
        'cmux_term/cmux_rail_hover_card.cc',
        'cmux_term/cmux_update_installer.h',
        'cmux_term/cmux_update_installer.cc',
        'cmux_term/cmux_update_model.h',
        'cmux_term/cmux_update_model.cc',
        'cmux_term/cmux_update_network.h',
        'cmux_term/cmux_update_script.h',
        'cmux_term/cmux_update_script.cc',
        'cmux_term/cmux_update_service.h',
        'cmux_term/cmux_update_service.cc',
        'cmux_term/cmux_release_build.h',
        'cmux_term/cmux_telemetry.h',
        'cmux_term/cmux_telemetry.cc',
        'cmux_term/cmux_telemetry_model.h',
        'cmux_term/cmux_telemetry_model.cc',
    ]
    additions = [f for f in xplat_missing if quote + f + quote not in s]
    if additions:
        assert s.count(xplat_anchor) == 1, 'cmux source refresh anchor changed'
        s = s.replace(
            xplat_anchor,
            xplat_anchor +
            ''.join(f'      {quote}{f}{quote},\n' for f in additions),
            1)
        modified = True

    platform_sources = [
        ('cmux_term/cmux_app_icon.h',
         f'      {quote}cmux_term/cmux_compositor_test_pane.mm{quote},\n'),
        ('cmux_term/cmux_app_icon_mac.mm',
         f'      {quote}cmux_term/cmux_compositor_test_pane.mm{quote},\n'),
        ('cmux_term/cmux_update_network_mac.mm',
         f'      {quote}cmux_term/cmux_compositor_test_pane.mm{quote},\n'),
        ('cmux_term/cmux_ghostty_opengl_host.cc',
         f'      {quote}cmux_term/cmux_terminal_pane_linux.cc{quote},\n'),
        ('cmux_term/cmux_update_network_linux.cc',
         f'      {quote}cmux_term/cmux_terminal_pane_linux.cc{quote},\n'),
    ]
    for source, anchor in platform_sources:
        source_scope = s
        if source == 'cmux_term/cmux_ghostty_opengl_host.cc':
            linux_start = s.index(
                'if (is_linux) {  # cmux linux terminal')
            linux_end = s.index('\n  }', linux_start)
            source_scope = s[linux_start:linux_end]
        if quote + source + quote not in source_scope:
            assert anchor in s, f'{source} refresh anchor changed'
            s = s.replace(
                anchor, anchor + f'      {quote}{source}{quote},\n', 1)
            modified = True

    # The OpenGL host is shared by Linux and Windows. Restrict this refresh to
    # the Windows block so seeing the Linux entry cannot hide a missing Windows
    # owner on a warm builder.
    win_sources = [
        'cmux_term/cmux_ghostty_opengl_host.cc',
        'cmux_term/cmux_update_network_win.cc',
    ]
    win_start = s.index('if (is_win) {  # cmux windows terminal')
    win_end = s.index('\n  }', win_start)
    win_block = s[win_start:win_end]
    win_missing = [
        source for source in win_sources
        if quote + source + quote not in win_block
    ]
    if win_missing:
        win_anchor = (
            f'      {quote}cmux_term/cmux_terminal_pane_linux.cc{quote},\n')
        if win_anchor in win_block:
            win_block = win_block.replace(
                win_anchor,
                win_anchor + ''.join(
                    f'      {quote}{source}{quote},\n'
                    for source in win_missing),
                1)
        else:
            compact = (
                f'    sources += [ '
                f'{quote}cmux_term/cmux_terminal_pane_linux.cc{quote} ]')
            expanded = (
                '    sources += [\n'
                f'      {quote}cmux_term/cmux_terminal_pane_linux.cc{quote},\n'
                + ''.join(
                    f'      {quote}{source}{quote},\n'
                    for source in win_missing)
                + '    ]')
            assert compact in win_block, 'Windows source refresh anchor changed'
            win_block = win_block.replace(compact, expanded, 1)
        s = s[:win_start] + win_block + s[win_end:]
        modified = True

    if modified:
        open(p, 'w').write(s)
    print('apply: BUILD.gn cmux sources refreshed')
else:
    xplat = ['cmux_term/cmux_term.h',
             'cmux_term/cmux_views.h', 'cmux_term/cmux_views.cc',
             'cmux_term/cmux_browser_window.h',
             'cmux_term/cmux_browser_window.cc',
             'cmux_term/cmux_browser_finder.h',
             'cmux_term/cmux_browser_pane.h', 'cmux_term/cmux_browser_pane.cc',
             'cmux_term/cmux_toolbar_menus.h', 'cmux_term/cmux_toolbar_menus.cc',
             'cmux_term/cmux_configure_page.h', 'cmux_term/cmux_configure_page.cc',
             'cmux_term/cmux_demo_page.h', 'cmux_term/cmux_demo_page.cc',
             'cmux_term/cmux_easing.h', 'cmux_term/cmux_easing.cc',
             'cmux_term/cmux_keymap.h', 'cmux_term/cmux_keymap.cc',
             'cmux_term/cmux_native_window_registry.h',
             'cmux_term/cmux_native_window_registry.cc',
             'cmux_term/cmux_native_frame_material.h',
             'cmux_term/cmux_native_frame_material.cc',
             'cmux_term/cmux_new_tab_page.h',
             'cmux_term/cmux_extensions.h', 'cmux_term/cmux_extensions.cc',
             'cmux_term/cmux_extensions_container.h',
             'cmux_term/cmux_extensions_container.cc',
             'cmux_term/cmux_extension_strip.h', 'cmux_term/cmux_extension_strip.cc',
             'cmux_term/cmux_pane_extension_pins.h',
             'cmux_term/cmux_pane_extension_pins.cc',
             'cmux_term/cmux_side_panel.h', 'cmux_term/cmux_side_panel.cc',
             'cmux_term/cmux_side_panel_resize_area.h',
             'cmux_term/cmux_side_panel_resize_area.cc',
             'cmux_term/cmux_focus.h', 'cmux_term/cmux_focus.cc',
             'cmux_term/cmux_pane.h', 'cmux_term/cmux_pane.cc',
             'cmux_term/cmux_pane_view.h', 'cmux_term/cmux_pane_view.cc',
             'cmux_term/cmux_surface.h', 'cmux_term/cmux_surface.cc',
             'cmux_term/cmux_tab_drag.h', 'cmux_term/cmux_tab_drag.cc',
             'cmux_term/cmux_tab_strip.h', 'cmux_term/cmux_tab_strip.cc',
             'cmux_term/cmux_ghostty_opengl_host.h',
             'cmux_term/cmux_terminal_backend.h',
             'cmux_term/cmux_terminal_backend.cc',
             'cmux_term/cmux_terminal_lifecycle.h',
             'cmux_term/cmux_terminal_placement.h',
             'cmux_term/cmux_terminal_placement.cc',
             'cmux_term/cmux_terminal_recovery.h',
             'cmux_term/cmux_terminal_recovery.cc',
             'cmux_term/cmux_tui_client.h', 'cmux_term/cmux_tui_client.cc',
             'cmux_term/cmux_terminal_host_connection.h',
             'cmux_term/cmux_terminal_host_connection.cc',
             'cmux_term/cmux_tui_protocol.h', 'cmux_term/cmux_tui_protocol.cc',
             'cmux_term/cmux_workspace_projection.h',
             'cmux_term/cmux_workspace_projection.cc',
             'cmux_term/cmux_tui_revision.h',
             'cmux_term/cmux_theme.h', 'cmux_term/cmux_theme.cc',
             'cmux_term/cmux_layout_config.h', 'cmux_term/cmux_layout_config.cc',
             'cmux_term/cmux_release_build.h',
             'cmux_term/cmux_telemetry.h', 'cmux_term/cmux_telemetry.cc',
             'cmux_term/cmux_telemetry_model.h',
             'cmux_term/cmux_telemetry_model.cc',
             'cmux_term/cmux_update_installer.h', 'cmux_term/cmux_update_installer.cc',
             'cmux_term/cmux_update_model.h', 'cmux_term/cmux_update_model.cc',
             'cmux_term/cmux_update_network.h',
             'cmux_term/cmux_update_script.h', 'cmux_term/cmux_update_script.cc',
             'cmux_term/cmux_update_service.h', 'cmux_term/cmux_update_service.cc',
             'cmux_term/cmux_rail.h', 'cmux_term/cmux_rail.cc',
             'cmux_term/cmux_rail_animating_layout_manager.h',
             'cmux_term/cmux_rail_animating_layout_manager.cc',
             'cmux_term/cmux_rail_group_editor_bubble.h',
             'cmux_term/cmux_rail_group_editor_bubble.cc',
             'cmux_term/cmux_rail_hover_card.h',
             'cmux_term/cmux_rail_hover_card.cc',
             'cmux_term/cmux_rail_config.h', 'cmux_term/cmux_rail_config.cc',
             'cmux_term/cmux_sidebar_metrics.h',
             'cmux_term/cmux_strip_controller.h',
             'cmux_term/cmux_input.h',
             'cmux_term/window_layout.h', 'cmux_term/window_layout.cc',
             'cmux_term/window_model.h', 'cmux_term/window_model.cc']
    maconly = ['cmux_term/cmux_app_icon.h',
               'cmux_term/cmux_app_icon_mac.mm',
               'cmux_term/cmux_term_mac.mm',
               'cmux_term/cmux_views_mac.mm',
               'cmux_term/cmux_native_frame_material_mac.mm',
               'cmux_term/cmux_input.mm',
               'cmux_term/cmux_terminal_pane.h', 'cmux_term/cmux_terminal_pane.mm',
               'cmux_term/cmux_compositor_test_pane.h',
               'cmux_term/cmux_compositor_test_pane.mm',
               'cmux_term/cmux_update_network_mac.mm']

    # Cross-platform block, inserted just before the browser target's macOS
    # sources. Chromium 150 modularized a number of browser sources and removed
    # caption_settings_dialog_mac.cc plus most headers from this list. Keep an
    # explicit anchor for each supported layout instead of tying this patch to
    # a source that is unrelated to cmux.
    block = ('  if (is_mac || is_linux || is_win) {  # cmux cross-platform\n'
             '    sources += [\n'
             + ''.join('      \"%s\",\n' % f for f in xplat)
             + '    ]\n'
             '    deps += [\n'
             '      \"//chrome/services/cmux_terminal_renderer/public/cpp:'
             'terminal_host_protocol\",\n'
             '    ]\n'
             '  }\n')
    mac_anchors = (
        # Chromium 149 and earlier.
        ('  if (is_mac) {\n    sources += [\n'
         '      \"accessibility/caption_settings_dialog.h\",'),
        # Chromium 150 (as of 150.0.7871.125).
        ('  if (is_mac) {\n    sources += [\n'
         '      \"app_controller_mac.mm\",'),
    )
    matching_mac_anchors = [anchor for anchor in mac_anchors if anchor in s]
    assert len(matching_mac_anchors) == 1, (
        'browser target macOS sources anchor not uniquely identified; '
        'supported layouts: Chromium 149 through Chromium 151')
    mac_anchor = matching_mac_anchors[0]
    s = s.replace(mac_anchor, block + mac_anchor, 1)

    # cmux_term_mac.mm supplies the startup-extra-parts definitions on macOS,
    # while cmux_term.cc supplies them on Linux/Windows. Likewise, macOS gets
    # cmux_theme_ghostty.cc through :cmux_ghostty; non-mac platforms compile it
    # directly into chrome/browser alongside their vendored Ghostty library.
    # Keeping these out of the cross-platform list prevents duplicate symbols
    # when the browser target absorbs the cmux_ghostty source_set on macOS.
    nonmac_block = ('  if (is_linux || is_win) {  # cmux non-mac implementations\n'
                    '    sources += [\n'
                    '      \"cmux_term/cmux_term.cc\",\n'
                    '      \"cmux_term/cmux_theme_ghostty.h\",\n'
                    '      \"cmux_term/cmux_theme_ghostty.cc\",\n'
                    '    ]\n'
                    '    deps += [\n'
                    '      \"//chrome/services/cmux_terminal_renderer/public/cpp:'
                    'ghostty_resources\",\n'
                    '    ]\n'
                    '  }\n')
    s = s.replace(mac_anchor, nonmac_block + mac_anchor, 1)

    # Linux-only: host-owned EGL/OpenGL terminal pane. chrome/browser is a
    # source_set, so the final chrome executable owns libghostty and its system
    # link dependencies below.
    linux_block = ('  if (is_linux) {  # cmux linux terminal\n'
                   '    sources += [\n'
                   '      \"cmux_term/cmux_ghostty_opengl_host.cc\",\n'
                   '      \"cmux_term/cmux_terminal_pane_linux.cc\",\n'
                   '      \"cmux_term/cmux_update_network_linux.cc\",\n'
                   '    ]\n  }\n')
    s = s.replace(mac_anchor, linux_block + mac_anchor, 1)

    # Windows-only: the same host-owned OpenGL frontend, backed by a hidden WGL
    # drawable, plus the vendored ghostty.dll import library.
    win_block = ('  if (is_win) {  # cmux windows terminal\n'
                 '    sources += [\n'
                 '      \"cmux_term/cmux_ghostty_opengl_host.cc\",\n'
                 '      \"cmux_term/cmux_terminal_pane_linux.cc\",\n'
                 '      \"cmux_term/cmux_update_network_win.cc\",\n'
                 '    ]\n'
                 '    libs += [\n'
                 '      rebase_path(\"//third_party/cmux_ghostty/lib/ghostty.lib\"),\n'
                 '      \"gdi32.lib\",\n'
                 '      \"user32.lib\",\n'
                 '    ]\n  }\n')
    s = s.replace(mac_anchor, win_block + mac_anchor, 1)

    if MODE == 'full':
        # Add mac-only sources and the Ghostty source_set dependency to this
        # same browser-target is_mac block. Do not search from the start of the
        # file: Chromium 150 has earlier is_mac source_sets with their own deps.
        mac_idx = s.index(mac_anchor)
        mac_block_end = s.find('\n  }', mac_idx)
        assert mac_block_end != -1, 'browser target macOS block end not found'
        mac_source_anchor = '      \"chrome_browser_main_mac.mm\",'
        assert s.count(mac_source_anchor, mac_idx, mac_block_end) == 1, (
            'chrome_browser_main_mac.mm not unique in browser macOS block')
        source_idx = s.index(mac_source_anchor, mac_idx, mac_block_end)
        source_idx += len(mac_source_anchor)
        mac_source_lines = ''.join('\n      \"%s\",' % f for f in maconly)
        s = s[:source_idx] + mac_source_lines + s[source_idx:]

        # Recompute the block end after growing the source list.
        mac_block_end = s.find('\n  }', mac_idx)
        dep = '\"//chrome/browser/cmux_term:cmux_ghostty\",'
        deps_anchor = '    deps += ['
        deps_idx = s.find(deps_anchor, mac_idx, mac_block_end)
        assert deps_idx != -1, 'browser target macOS deps list not found'
        deps_insert = deps_idx + len(deps_anchor)
        s = s[:deps_insert] + '\n      ' + dep + s[deps_insert:]
    open(p, 'w').write(s)
    print('apply: hook', reg in open('chrome/browser/chrome_browser_main.cc').read(),
          '| xplat', 'cmux_term/cmux_views.cc' in open('chrome/browser/BUILD.gn').read(),
          '| mode', MODE)

# The terminal-host codec lives at a neutral browser/utility boundary. Upgrade
# previously patched Chromium trees whose cmux source block predates that GN
# dependency.
terminal_host_dep = ('\"//chrome/services/cmux_terminal_renderer/public/cpp:'
                     'terminal_host_protocol\"')
if '# cmux cross-platform' in s and terminal_host_dep not in s:
    marker = s.index('# cmux cross-platform')
    end = s.index('    ]\n  }\n', marker)
    insert = ('    ]\n'
              '    deps += [\n'
              '      ' + terminal_host_dep + ',\n'
              '    ]\n'
              '  }\n')
    s = s[:end] + s[end:].replace('    ]\n  }\n', insert, 1)
    open(p, 'w').write(s)
    print('apply: BUILD.gn added neutral terminal-host protocol dep')

# Linux and Windows compile the Ghostty theme callers directly into the
# browser target. Ensure warm, previously patched checkouts also pull the
# source_set containing EnsureGhosttyResourcesDirectory() into the final link.
ghostty_resources_dep = (
    '\"//chrome/services/cmux_terminal_renderer/public/cpp:ghostty_resources\"')
if '# cmux non-mac implementations' in s:
    marker = s.index('# cmux non-mac implementations')
    block_end = s.index('\n  }', marker)
    if ghostty_resources_dep not in s[marker:block_end]:
        sources_end = s.index('    ]\n', marker) + len('    ]\n')
        insert = ('    deps += [\n'
                  '      ' + ghostty_resources_dep + ',\n'
                  '    ]\n')
        s = s[:sources_end] + insert + s[sources_end:]
        open(p, 'w').write(s)
        print('apply: BUILD.gn added non-mac Ghostty resources dep')

# 2b. A cmux workspace is a real Browser with a production custom
# BrowserWindow. Give the desktop factory a narrow first-chance hook; normal
# Chrome Browsers still fall straight through to BrowserView byte-for-byte.
p = 'chrome/browser/ui/views/frame/browser_window_factory.cc'
s = open(p).read()
include_anchor = '#include \"build/build_config.h\"\n'
include_line = '#include \"chrome/browser/cmux_term/cmux_browser_window.h\"\n'
if include_line not in s:
    assert include_anchor in s, 'browser window factory include anchor missing'
    s = s.replace(include_anchor, include_anchor + include_line, 1)
factory_anchor = ('BrowserWindow::CreateBrowserWindow(Browser* browser,\n'
                  '                                   bool user_gesture,\n'
                  '                                   bool in_tab_dragging) {\n')
if 'MaybeCreateCmuxBrowserWindow(browser)' not in s:
    assert factory_anchor in s, 'browser window factory function anchor missing'
    factory_hook = factory_anchor + (
        '  if (auto cmux_window = cmux::MaybeCreateCmuxBrowserWindow(browser)) {\n'
        '    return cmux_window;\n'
        '  }\n\n')
    s = s.replace(factory_anchor, factory_hook, 1)
open(p, 'w').write(s)
print('apply: BrowserWindow cmux workspace factory hook patched')

# 2c. The AWS scratch mount cannot traverse generated ../../.. paths back into
# /Applications even though direct access succeeds. HQ provisions explicitly
# named system_xcode/system_sdk links under build/mac_files. Keep Chromium in
# supported system-Xcode mode, but make its tool binary path checkout-relative
# when (and only when) that reviewed SDK path is selected.
p = 'build/config/mac/mac_sdk.gni'
s = open(p).read()
tool_marker = '# cmux: checkout-relative system Xcode tools'
if tool_marker not in s:
    tool_anchor = ('  mac_bin_path = rebase_path(mac_bin_path, root_build_dir)\n'
                   '}\n')
    assert s.count(tool_anchor) == 1, 'mac_bin_path rebase anchor not unique'
    tool_override = tool_anchor + (
        '\n# cmux: checkout-relative system Xcode tools\n'
        'if (use_system_xcode &&\n'
        '    mac_sdk_path == \"//build/mac_files/system_sdk\") {\n'
        '  mac_bin_path = rebase_path(\n'
        '      \"//build/mac_files/system_xcode/Contents/Developer/Toolchains/'
        'XcodeDefault.xctoolchain/usr/bin/\")\n'
        '}\n')
    s = s.replace(tool_anchor, tool_override, 1)
    open(p, 'w').write(s)
print('apply: checkout-relative system Xcode tools', tool_marker in open(p).read())

# 3b. chrome/BUILD.gn: attach the libghostty archive and platform libraries to
#     the final link target. On macOS, a component build puts chrome/browser in
#     libchrome_dll.dylib and a static build folds it into chrome_framework. On
#     Linux, chrome/browser is a source_set and chrome_initial is the executable
#     that owns its link inputs.
if MODE == 'chrome_linux':
    # cmux's host-owned EGL context gives chrome a direct libegl1 dependency.
    # Chromium's distro audit intentionally rejects packages absent from its
    # reviewed version map, so register the versions shipped by each supported
    # distro rather than disabling that audit.
    p = 'chrome/installer/linux/debian/dist_package_versions.json'
    distro_versions = json.load(open(p))
    libegl1_versions = {
        'Debian 11 (Bullseye)': '1.3.2-1',
        'Debian 12 (Bookworm)': '1.6.0-1',
        'Ubuntu 18.04 (Bionic)': '1.0.0-2ubuntu2.3',
        'Ubuntu 20.04 (Focal)': '1.3.2-1~ubuntu0.20.04.2',
        'Ubuntu 22.04 (Jammy)': '1.4.0-1',
    }
    assert set(libegl1_versions).issubset(distro_versions), (
        'Chromium supported Linux distro set changed')
    for distro, version in libegl1_versions.items():
        distro_versions[distro]['libegl1'] = version
    with open(p, 'w') as output:
        json.dump(distro_versions, output, indent=4, sort_keys=True)
        output.write('\n')

p = 'chrome/BUILD.gn'
s = open(p).read()
component_anchor = ('    if (is_component_build) {\n'
                    '      frameworks = [ \"Carbon.framework\" ]\n'
                    '    }\n\n'
                    '    ldflags = [ \"-ObjC\" ]')
component_repl = ('    ldflags = [ \"-ObjC\" ]\n\n'
                  '    if (is_component_build) {  # cmux Ghostty component link\n'
                  '      frameworks = [\n'
                  '        \"Carbon.framework\",\n'
                  '        \"Metal.framework\",\n'
                  '        \"MetalKit.framework\",\n'
                  '        \"QuartzCore.framework\",\n'
                  '        \"CoreText.framework\",\n'
                  '        \"CoreGraphics.framework\",\n'
                  '        \"GameController.framework\",\n'
                  '        \"IOSurface.framework\",\n'
                  '        \"AppKit.framework\",\n'
                  '      ]\n'
                  '      ldflags += [\n'
                  '        rebase_path(\"//third_party/cmux_ghostty/lib/ghostty-internal.a\",\n'
                  '                    root_build_dir),\n'
                  '        \"-lc++\",\n'
                  '      ]\n'
                  '    }')
component_marker = '# cmux Ghostty component link'
legacy_component_marker = '# cmux ghostty chrome_dll link'
if (MODE == 'full' and component_marker not in s and
        legacy_component_marker in s):
    legacy_idx = s.index(legacy_component_marker)
    legacy_start = s.rfind('    if (is_component_build) {', 0, legacy_idx)
    legacy_end = s.find('    configs += [', legacy_idx)
    assert legacy_start != -1 and legacy_end != -1, (
        'legacy chrome_dll component link bounds not found')
    legacy_section = s[legacy_start:legacy_end]
    assert 'frameworks = [ \"Carbon.framework\" ]' in legacy_section
    assert 'cmux_ghostty/lib/ghostty-internal.a' in legacy_section
    assert 'ldflags = [ \"-ObjC\", \"-lc++\" ]' in legacy_section
    s = s[:legacy_start] + component_repl + '\n\n' + s[legacy_end:]
    print('apply: migrated legacy chrome_dll Ghostty link')
if MODE == 'full' and component_marker not in s:
    assert component_anchor in s, 'chrome_dll component link anchor not found'
    s = s.replace(component_anchor, component_repl, 1)
anchor = ('ldflags = [\n'
          '      \"-compatibility_version\",\n'
          '      chrome_dylib_version,\n'
          '      \"-current_version\",\n'
          '      chrome_dylib_version,\n'
          '    ]')
if MODE == 'full' and anchor in s:
    repl = ('ldflags = [\n'
            '      \"-compatibility_version\",\n'
            '      chrome_dylib_version,\n'
            '      \"-current_version\",\n'
            '      chrome_dylib_version,\n'
            '      rebase_path(\"//third_party/cmux_ghostty/lib/ghostty-internal.a\",\n'
            '                  root_build_dir),\n'
            '      \"-lc++\",\n'
            '    ]\n'
            '    frameworks = [\n'
            '      \"Metal.framework\",\n'
            '      \"MetalKit.framework\",\n'
            '      \"QuartzCore.framework\",\n'
            '      \"CoreText.framework\",\n'
            '      \"CoreGraphics.framework\",\n'
            '      \"GameController.framework\",\n'
            '      \"IOSurface.framework\",\n'
            '      \"AppKit.framework\",\n'
            '    ]')
    s = s.replace(anchor, repl, 1)
# Upgrade trees patched by an earlier version of this script.
component_idx = s.find(component_marker)
component_end = s.find('    configs += [', component_idx)
if (MODE == 'full' and component_idx != -1 and component_end != -1 and
        'GameController.framework' not in s[component_idx:component_end]):
    section = s[component_idx:component_end]
    section = section.replace(
        '        \"CoreGraphics.framework\",\n',
        '        \"CoreGraphics.framework\",\n'
        '        \"GameController.framework\",\n',
        1)
    s = s[:component_idx] + section + s[component_end:]
framework_idx = s.find('mac_framework_bundle(\"chrome_framework\")')
framework_end = s.find('    if (!is_component_build) {', framework_idx)
if (MODE == 'full' and framework_idx != -1 and framework_end != -1 and
        'GameController.framework' not in s[framework_idx:framework_end]):
    section = s[framework_idx:framework_end]
    section = section.replace(
        '      \"CoreGraphics.framework\",\n',
        '      \"CoreGraphics.framework\",\n'
        '      \"GameController.framework\",\n',
        1)
    s = s[:framework_idx] + section + s[framework_end:]

linux_marker = '# cmux Linux Ghostty executable link'
linux_anchor = ('    if (is_linux) {\n'
                '      sources += [\n'
                '        \"app/chrome_main_linux.cc\",')
if MODE == 'chrome_linux' and linux_marker not in s:
    assert s.count(linux_anchor) == 1, (
        'chrome_initial Linux source anchor not unique')
    linux_link = (
        '    if (is_linux) {  # cmux Linux Ghostty executable link\n'
        '      libs = [\n'
        '        \"//third_party/cmux_ghostty/lib/ghostty-internal.a\",\n'
        '        \"EGL\",\n'
        '        \"GL\",\n'
        '        \"dl\",\n'
        '        \"fontconfig\",\n'
        '        \"util\",\n'
        '      ]\n'
        '    }\n\n')
    s = s.replace(linux_anchor, linux_link + linux_anchor, 1)
open(p, 'w').write(s)
print('apply: ghostty-link', '# cmux Ghostty component link' in open(p).read(),
      'cmux_ghostty/lib/ghostty-internal.a' in open(p).read())

# 3c. Register the one-terminal-per-process Ghostty renderer as a Chrome
# utility service. The browser launches one service receiver per terminal;
# ServiceProcessHost gives every launch its own utility OS process.
if MODE == 'full':
    p = 'chrome/utility/services.cc'
    s = open(p).read()
    include_anchor = ('#if BUILDFLAG(IS_MAC)\n'
                      '#include \"chrome/services/mac_notifications/'
                      'mac_notification_provider_impl.h\"\n')
    include_add = ('#if BUILDFLAG(IS_MAC)\n'
                   '#include \"chrome/services/cmux_terminal_renderer/'
                   'cmux_terminal_renderer_service.h\"\n'
                   '#include \"chrome/services/cmux_terminal_renderer/public/'
                   'mojom/cmux_terminal_renderer.mojom.h\"\n'
                   '#include \"chrome/services/mac_notifications/'
                   'mac_notification_provider_impl.h\"\n')
    if 'cmux_terminal_renderer_service.h' not in s:
        assert include_anchor in s, 'cmux renderer service include anchor missing'
        s = s.replace(include_anchor, include_add, 1)

    # Keep the renderer factory structurally inside Chromium's existing macOS
    # block. The ScreenAI/non-Android block has changed whitespace and members
    # across supported Chromium revisions, while this factory is the stable
    # platform-local neighbor of our service.
    run_anchor = ('#if BUILDFLAG(IS_MAC)\n'
                  'auto RunMacNotificationService(')
    run_add = ('#if BUILDFLAG(IS_MAC)\n'
               'auto RunCmuxTerminalRenderer(\n'
               '    mojo::PendingReceiver<cmux::mojom::CmuxTerminalRenderer> '
               'receiver) {\n'
               '  return std::make_unique<cmux::CmuxTerminalRendererService>(\n'
               '      std::move(receiver));\n'
               '}\n\n'
               'auto RunMacNotificationService(')
    if 'RunCmuxTerminalRenderer(' not in s:
        assert run_anchor in s, 'cmux renderer service function anchor missing'
        s = s.replace(run_anchor, run_add, 1)

    registration_anchor = ('#if BUILDFLAG(IS_MAC)\n'
                           '  services.Add(RunMacNotificationService);\n'
                           '#endif  // BUILDFLAG(IS_MAC)')
    registration_add = ('#if BUILDFLAG(IS_MAC)\n'
                        '  services.Add(RunCmuxTerminalRenderer);\n'
                        '  services.Add(RunMacNotificationService);\n'
                        '#endif  // BUILDFLAG(IS_MAC)')
    if 'services.Add(RunCmuxTerminalRenderer);' not in s:
        assert registration_anchor in s, (
            'cmux renderer service registration anchor missing')
        s = s.replace(registration_anchor, registration_add, 1)
    open(p, 'w').write(s)

    p = 'chrome/utility/BUILD.gn'
    s = open(p).read()
    utility_anchor = ('  if (is_mac) {\n'
                      '    deps += [\n'
                      '      \"//chrome/services/mac_notifications\",')
    utility_add = ('  if (is_mac) {\n'
                   '    deps += [\n'
                   '      \"//chrome/services/cmux_terminal_renderer\",\n'
                   '      \"//chrome/services/mac_notifications\",')
    if '//chrome/services/cmux_terminal_renderer\"' not in s:
        assert utility_anchor in s, 'cmux renderer utility dep anchor missing'
        s = s.replace(utility_anchor, utility_add, 1)
    open(p, 'w').write(s)
    print('apply: cmux terminal renderer utility service registered')

# 3. chrome/BUILD.gn: the libghostty link config propagates its system
#    frameworks to the thin chrome_app executable too (gn all_dependent_configs
#    crosses the shared_library boundary). The objects dead-strip there; only
#    the framework load commands remain. Allowlist them in the executable's
#    verify_dynamic_libraries action. TODO: scope the link config to the
#    Chromium Framework only so the executable stays libSystem-only.
p = 'chrome/BUILD.gn'
s = open(p).read()
fw = ['/System/Library/Frameworks/Metal.framework/Versions/A/Metal',
      '/System/Library/Frameworks/MetalKit.framework/Versions/A/MetalKit',
      '/System/Library/Frameworks/QuartzCore.framework/Versions/A/QuartzCore',
      '/System/Library/Frameworks/CoreText.framework/Versions/A/CoreText',
      '/System/Library/Frameworks/CoreGraphics.framework/Versions/A/CoreGraphics',
      '/System/Library/Frameworks/GameController.framework/Versions/A/GameController',
      '/System/Library/Frameworks/IOSurface.framework/Versions/A/IOSurface',
      '/System/Library/Frameworks/AppKit.framework/Versions/C/AppKit']
marker = 'action(\"verify_libraries_chrome_app\")'
idx = s.find(marker)
if MODE == 'full' and idx != -1 and '/System/Library/Frameworks/Metal.framework' not in s[idx:idx+1200]:
    anchor = '\"--allow\",\n        \"/usr/lib/libSystem.B.dylib\",'
    add = anchor + ''.join('\n        \"--allow\",\n        \"%s\",' % f for f in fw)
    s = s[:idx] + s[idx:].replace(anchor, add, 1)
if (MODE == 'full' and idx != -1 and
        '/System/Library/Frameworks/GameController.framework' not in
        s[idx:idx+1800]):
    section = s[idx:]
    section = section.replace(
        '\"--allow\",\n'
        '        \"/System/Library/Frameworks/CoreGraphics.framework/Versions/A/CoreGraphics\",',
        '\"--allow\",\n'
        '        \"/System/Library/Frameworks/CoreGraphics.framework/Versions/A/CoreGraphics\",\n'
        '        \"--allow\",\n'
        '        \"/System/Library/Frameworks/GameController.framework/Versions/A/GameController\",',
        1)
    s = s[:idx] + section
open(p, 'w').write(s)
print('apply: verify-allow', '/System/Library/Frameworks/Metal.framework' in open(p).read()[s.find(marker):s.find(marker)+1500] if marker in s else False)
PYEOF"

# 5. Match Helium's desktop omnibar geometry and autocomplete density on each
# supported Chromium base. The patch is kept separate from the cmux color
# overlay: Helium owns the fixed metrics (28 px controls, 14 px type, 8 px bar
# radius, 12 px popup radius), while the final color mixer adapts its state
# colors to the resolved Ghostty theme. Apply before the cmux popup safety
# guards below so this remains a clean upstream-derived patch.
ssh -o BatchMode=yes "$HOST" "cd '$SRC' && set -e
  test -f '$HELIUM_OMNIBAR_PATCH'
  if git apply --reverse --check '$HELIUM_OMNIBAR_PATCH' >/dev/null 2>&1; then
    echo 'helium-omnibar: already patched'
  elif git apply --check '$HELIUM_OMNIBAR_PATCH'; then
    git apply '$HELIUM_OMNIBAR_PATCH'
    echo 'helium-omnibar: patched'
  else
    echo 'helium-omnibar: patch does not match Chromium $CHROMIUM_MAJOR' >&2
    exit 1
  fi"

# 5b. Apply the stable M151 source patch. It carries the GPL-3.0-only Helium
# settings appearance port plus a BSD-style mirror of the custom-window
# permission hunks. The latter gives older branches an existing patch path to
# reverse when rolling a shared warm checkout back. The checked-in header
# records provenance and licensing for both sections.
ssh -o BatchMode=yes "$HOST" "cd '$SRC' && set -e
  test -f '$HELIUM_SETTINGS_PATCH'
  if git apply --reverse --check '$HELIUM_SETTINGS_PATCH' >/dev/null 2>&1; then
    echo 'helium-settings: already patched'
  elif git apply --check '$HELIUM_SETTINGS_PATCH'; then
    git apply '$HELIUM_SETTINGS_PATCH'
    echo 'helium-settings: patched (GPL-3.0-only; see THIRD_PARTY_NOTICES.md)'
  else
    echo 'helium-settings: patch does not match Chromium $CHROMIUM_MAJOR' >&2
    exit 1
  fi"

# 5c. Keep new tabs local and apply Helium's shortcut-first New Tab geometry,
# higher-resolution rounded tiles, and Customize side-panel availability. The
# adaptation retains Chromium's live customization DOM so background and
# shortcut preferences remain native.
ssh -o BatchMode=yes "$HOST" "cd '$SRC' && set -e
  test -f '$HELIUM_NTP_PATCH'
  if git apply --reverse --check '$HELIUM_NTP_PATCH' >/dev/null 2>&1; then
    echo 'helium-new-tab: already patched'
  elif git apply --check '$HELIUM_NTP_PATCH'; then
    git apply '$HELIUM_NTP_PATCH'
    echo 'helium-new-tab: patched (GPL-3.0-only; see THIRD_PARTY_NOTICES.md)'
  else
    echo 'helium-new-tab: patch does not match Chromium $CHROMIUM_MAJOR' >&2
    exit 1
  fi"

# 5d. Keep Chromium's native global-media button in cmux's pane-local toolbar.
# Helium's controller gate makes the button user-hideable, while the cmux
# constructor seam hosts that native control from a Browser without requiring
# Chromium's mutually exclusive BrowserView.
ssh -o BatchMode=yes "$HOST" "cd '$SRC' && set -e
  test -f '$HELIUM_MEDIA_TOOLBAR_PATCH'
  if git apply --reverse --check '$HELIUM_MEDIA_TOOLBAR_PATCH' >/dev/null 2>&1; then
    echo 'helium-media-toolbar: already patched'
  elif git apply --check '$HELIUM_MEDIA_TOOLBAR_PATCH'; then
    git apply '$HELIUM_MEDIA_TOOLBAR_PATCH'
    echo 'helium-media-toolbar: patched (GPL-3.0-only and BSD-3-Clause; see THIRD_PARTY_NOTICES.md)'
  else
    echo 'helium-media-toolbar: patch does not match Chromium $CHROMIUM_MAJOR' >&2
    exit 1
  fi"

# 5e. Let cmux pane toolbars host Chromium's native pinned-action container.
# The constructor seam accepts cmux's real Browser directly while keeping the
# BrowserView constructor and normal Chromium windows behaviorally unchanged.
ssh -o BatchMode=yes "$HOST" "cd '$SRC' && set -e
  test -f '$CMUX_PINNED_TOOLBAR_PATCH'
  if git apply --reverse --check '$CMUX_PINNED_TOOLBAR_PATCH' >/dev/null 2>&1; then
    echo 'cmux-pinned-toolbar-actions: already patched'
  elif git apply --check '$CMUX_PINNED_TOOLBAR_PATCH'; then
    git apply '$CMUX_PINNED_TOOLBAR_PATCH'
    echo 'cmux-pinned-toolbar-actions: patched (BSD-3-Clause; see THIRD_PARTY_NOTICES.md)'
  else
    echo 'cmux-pinned-toolbar-actions: patch does not match Chromium $CHROMIUM_MAJOR' >&2
    exit 1
  fi"

# 6. Keep Chromium's native UA client-hint brands. Older cmux builds forced an
#    official "Google Chrome" brand into Chromium builds. That claim is not
#    internally consistent with the unbranded binary (and is not needed for
#    normal-browser behavior), so remove the exact legacy patch if present.
ssh -o BatchMode=yes "$HOST" "cd '$SRC' && python3 - <<'PYEOF'
p = 'components/embedder_support/user_agent_utils.cc'
s = open(p).read()
anchor = ('  std::optional<std::string> brand;\n'
          '#if !BUILDFLAG(CHROMIUM_BRANDING)\n'
          '  brand = version_info::GetProductName();\n'
          '#endif')
legacy = anchor + ('\n  // cmux: present as \"Google Chrome\" so Sec-CH-UA /'
                   ' navigator.userAgentData\n'
                   '  // brands match real Chrome (our build is Chromium-branded'
                   ' otherwise).\n'
                   '  brand = \"Google Chrome\";')
legacy_win = anchor + ('\n  // cmux: present as Google Chrome brand.\n'
                       '  brand = \"Google Chrome\";')
if legacy in s:
    s = s.replace(legacy, anchor, 1)
    open(p, 'w').write(s)
    print('ua-brand: removed legacy Google Chrome spoof')
elif legacy_win in s:
    s = s.replace(legacy_win, anchor, 1)
    open(p, 'w').write(s)
    print('ua-brand: removed legacy Windows Google Chrome spoof')
else:
    print('ua-brand: native Chromium behavior')
assert 'cmux: present as' not in s, 'unrecognized legacy cmux UA-brand spoof'
PYEOF"

# 6b. Do not attach Google's X-Client-Data experiment header. cmux is not a
#     Google Chrome release channel, so sending Chrome field-trial IDs from an
#     unofficial Chromium product creates an inconsistent network identity.
#     Helium disables this header as part of its ungoogled Chromium patch set.
ssh -o BatchMode=yes "$HOST" "cd '$SRC' && python3 - <<'PYEOF'
p = 'components/variations/net/variations_http_headers.cc'
s = open(p).read()
old = '''bool ShouldAppendVariationsHeader(const GURL& url, InIncognito incognito) {
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
new = '''bool ShouldAppendVariationsHeader(const GURL& url, InIncognito incognito) {
  // cmux: this is not a Google Chrome release channel. Do not send Google's
  // experiment identifiers as X-Client-Data from an unofficial product.
  return false;
}'''
if 'cmux: this is not a Google Chrome release channel' in s:
    print('variations-header: already disabled')
elif old in s:
    s = s.replace(old, new, 1)
    open(p, 'w').write(s)
    print('variations-header: disabled')
else:
    raise AssertionError('variations header anchor not found')
PYEOF"

# 6c. Never fetch a Google variations seed. Unofficial Chromium builds already
#     avoid this in the normal configuration, but make the product invariant
#     explicit so a future branding/build-flag change cannot re-enable it.
ssh -o BatchMode=yes "$HOST" "cd '$SRC' && python3 - <<'PYEOF'
p = 'components/variations/service/variations_service.cc'
s = open(p).read()
start = s.index('bool IsFetchingEnabled() {')
end_marker = '\n}\n\n// Returns the already downloaded first run seed'
end = s.index(end_marker, start) + 2
body = s[start:end]
new = '''bool IsFetchingEnabled() {
  // cmux: do not enroll this unofficial product in Google field trials.
  return false;
}'''
if 'cmux: do not enroll this unofficial product' in body:
    print('variations-fetch: already disabled')
else:
    assert 'kDisableVariationsSeedFetch' in body, 'variations fetch anchor changed'
    s = s[:start] + new + s[end:]
    open(p, 'w').write(s)
    print('variations-fetch: disabled')
PYEOF"

# 6d. Use Helium's ordinary, non-Chrome-channel Google search template. The
# stock Chromium template adds Chrome release-channel attribution parameters
# (sourceid, assisted-query stats, field-trial data, etc.). Those parameters
# are inconsistent once this unofficial product correctly suppresses Google's
# X-Client-Data/variations enrollment. A plain q= navigation is also what
# Helium emits from its omnibox. Bump the prepopulate data version so existing
# cmux profiles receive the corrected template, not only newly-created ones.
ssh -o BatchMode=yes "$HOST" "cd '$SRC' && python3 - <<'PYEOF'
import re

p = 'third_party/search_engines_data/resources/definitions/prepopulated_engines.json'
s = open(p).read()
google_start = s.index('    \"google\": {')
google_end = s.index('\n    },', google_start) + len('\n    },')
google = s[google_start:google_end]
plain_search = '      \"search_url\": \"https://www.google.com/search?q={searchTerms}\",'
plain_suggest = '      \"suggest_url\": \"https://www.google.com/complete/search?client=chrome&q={searchTerms}\",'
if plain_search in google and '\"type\": \"SEARCH_ENGINE_OTHER\"' in google:
    print('google-search-template: already plain')
else:
    google, search_count = re.subn(
        r'^      \"search_url\": .+$', plain_search, google,
        count=1, flags=re.MULTILINE)
    google, suggest_count = re.subn(
        r'^      \"suggest_url\": .+$', plain_suggest, google,
        count=1, flags=re.MULTILINE)
    google, type_count = re.subn(
        r'^      \"type\": \"SEARCH_ENGINE_GOOGLE\",$',
        '      \"type\": \"SEARCH_ENGINE_OTHER\",', google,
        count=1, flags=re.MULTILINE)
    assert search_count == 1, 'Google search URL anchor changed'
    assert suggest_count == 1, 'Google suggest URL anchor changed'
    assert type_count == 1, 'Google engine type anchor changed'
    s = s[:google_start] + google + s[google_end:]
    version_match = re.search(r'(\"kCurrentDataVersion\": )(\d+)', s)
    assert version_match, 'prepopulated engine data version missing'
    old_version = int(version_match.group(2))
    s = (s[:version_match.start(2)] + str(old_version + 1) +
         s[version_match.end(2):])
    open(p, 'w').write(s)
    print('google-search-template: plain; data version', old_version + 1)

p = 'components/search_engines/search_engine_utils.cc'
s = open(p).read()
old = '    return TemplateURLPrepopulateData::google.type;'
new = ('    // cmux: the built-in Google entry uses the same ordinary search URL\n'
       '    // as Helium, not Chrome release-channel attribution.\n'
       '    return SEARCH_ENGINE_OTHER;')
if 'cmux: the built-in Google entry uses the same ordinary search URL' in s:
    print('google-search-type: already ordinary')
else:
    assert s.count(old) == 1, 'Google search type anchor changed'
    s = s.replace(old, new, 1)
    open(p, 'w').write(s)
    print('google-search-type: ordinary')
PYEOF"

# 7. Guard the omnibox autocomplete-popup frame against an orphaned widget. In
#    a pane-local omnibox the floating results widget can be torn down while
#    a queued mouse event is still in flight; GetParentWidgetAndEvent then
#    dereferences a null this_view->GetWidget() and crashes
#    (RoundedOmniboxResultsFrame::OnMouseEvent). Add the same null guard the code
#    already has for parent_widget. Idempotent.
ssh -o BatchMode=yes "$HOST" "cd '$SRC' && python3 - <<'PYEOF'
p = 'chrome/browser/ui/views/omnibox/rounded_omnibox_results_frame.cc'
s = open(p).read()
anchor = ('  views::Widget* this_widget = this_view->GetWidget();\n'
          '  views::Widget* parent_widget = this_widget->parent();')
add = ('  views::Widget* this_widget = this_view->GetWidget();\n'
       '  // cmux: the floating results widget can be orphaned (no widget) yet\n'
       '  // still receive a queued mouse event during teardown, null-derefing\n'
       '  // below. Guard it the same way parent_widget is guarded.\n'
       '  if (!this_widget) {\n'
       '    return {nullptr, std::unique_ptr<ui::MouseEvent>(\n'
       '                         static_cast<ui::MouseEvent*>(\n'
       '                             this_event->Clone().release()))};\n'
       '  }\n'
       '  views::Widget* parent_widget = this_widget->parent();')
if 'cmux: the floating results widget can be orphaned' in s:
    print('omnibox-popup-guard: already patched')
elif anchor in s:
    open(p, 'w').write(s.replace(anchor, add, 1))
    print('omnibox-popup-guard: patched')
else:
    print('omnibox-popup-guard: ANCHOR NOT FOUND')
PYEOF"

# 8. Guard the omnibox results frame's immersive-fullscreen helper against a null
#    BrowserView. The shared cmux widget hosts real Browsers without being a
#    BrowserView itself, so
#    GetBrowserViewForNativeWindow() returns null and browser_view->overlay_widget()
#    null-derefs when a queued mouse event hits an orphaned autocomplete popup.
#    Idempotent.
ssh -o BatchMode=yes "$HOST" "cd '$SRC' && python3 - <<'PYEOF'
p = 'chrome/browser/ui/views/omnibox/rounded_omnibox_results_frame.cc'
s = open(p).read()
anchor = ('  BrowserView* browser_view = BrowserView::GetBrowserViewForNativeWindow(\n'
          '      parent_widget->GetNativeWindow());\n')
add = anchor + ('  // cmux: the shared workspace widget is not a BrowserView -> not in\n'
                '  // immersive fullscreen; bail before dereferencing it.\n'
                '  if (!browser_view) {\n'
                '    return nullptr;\n'
                '  }\n')
if 'cmux: the shared workspace widget is not a BrowserView' in s:
    print('omnibox-browserview-guard: already patched')
elif anchor in s:
    open(p, 'w').write(s.replace(anchor, add, 1))
    print('omnibox-browserview-guard: patched')
else:
    print('omnibox-browserview-guard: ANCHOR NOT FOUND')
PYEOF"

# 8a. A pane-local LocationBarView is hosted by CmuxBrowserWindow rather than
#     BrowserView, so BrowserWindowFeatures has no ZoomBubbleCoordinator. The
#     stock ZoomView updates during LocationBarView::Init and currently CHECKs
#     that optional BrowserView-only coordinator before it even decides whether
#     the icon is visible. Hide/ignore the zoom page action when the coordinator
#     is absent. Idempotent.
ssh -o BatchMode=yes "$HOST" "cd '$SRC' && python3 - <<'PYEOF'
p = 'chrome/browser/ui/views/page_action/zoom_view.cc'
s = open(p).read()
old_update = '''  auto* zoom_bubble_coordinator = GetZoomBubbleCoordinator(web_contents);
  CHECK(zoom_bubble_coordinator);

  if (ShouldBeVisible(can_show_bubble)) {'''
new_update = '''  auto* zoom_bubble_coordinator = GetZoomBubbleCoordinator(web_contents);
  // cmux: a pane-local LocationBarView has no BrowserView-owned zoom bubble
  // coordinator. Keep the unsupported page action hidden instead of crashing
  // while LocationBarView initializes.
  if (!zoom_bubble_coordinator) {
    SetVisible(false);
    return;
  }

  if (ShouldBeVisible(can_show_bubble)) {'''
old_execute = '''  auto* zoom_bubble_coordinator = GetZoomBubbleCoordinator(GetWebContents());
  CHECK(zoom_bubble_coordinator);

  zoom_bubble_coordinator->Show(GetWebContents(), ZoomBubbleView::USER_GESTURE);'''
new_execute = '''  auto* zoom_bubble_coordinator = GetZoomBubbleCoordinator(GetWebContents());
  if (!zoom_bubble_coordinator) {
    return;
  }

  zoom_bubble_coordinator->Show(GetWebContents(), ZoomBubbleView::USER_GESTURE);'''
changed = False
if old_update in s:
    s = s.replace(old_update, new_update, 1)
    changed = True
elif new_update not in s:
    raise AssertionError('zoom bubble update guard anchor not found')

if old_execute in s:
    s = s.replace(old_execute, new_execute, 1)
    changed = True
elif new_execute not in s:
    raise AssertionError('zoom bubble execute guard anchor not found')

if changed:
    open(p, 'w').write(s)
    print('zoom-bubble-coordinator-guard: patched')
else:
    print('zoom-bubble-coordinator-guard: already patched')
PYEOF"

# 10. Classify "cmux" as a handled scheme so the omnibox treats typed
#     cmux:// input as a URL (navigation), not a Google search. The navigation
#     itself is rewritten cmux:// -> chrome:// by the BrowserURLHandler cmux
#     registers in cmux_views.cc (so cmux://extensions == chrome://extensions,
#     displayed as cmux://). Idempotent.
ssh -o BatchMode=yes "$HOST" "cd '$SRC' && python3 - <<'PYEOF'
p = 'chrome/browser/profiles/profile_io_data.cc'
s = open(p).read()
anchor = '  return kProtocolList.contains(scheme);'
add = ('  // cmux: cmux:// URLs rewrite to chrome:// (BrowserURLHandler registered\n'
       '  // in cmux_views.cc); mark the scheme handled so the omnibox classifies\n'
       '  // typed cmux:// input as a URL, not a search.\n'
       '  if (scheme == \"cmux\") {\n'
       '    return true;\n'
       '  }\n'
       + anchor)
if 'cmux: cmux:// URLs rewrite to chrome://' in s:
    print('cmux-scheme-handled: already patched')
elif anchor in s:
    open(p, 'w').write(s.replace(anchor, add, 1))
    print('cmux-scheme-handled: patched')
else:
    print('cmux-scheme-handled: ANCHOR NOT FOUND')
PYEOF"

# 9. A real Browser hosted by CmuxBrowserWindow has no BrowserView-only
#    ImmersiveModeController. LocationBarView correctly carries the Browser,
#    so make the optional feature lookup null-safe before focusing the omnibox.
ssh -o BatchMode=yes "$HOST" "cd '$SRC' && python3 - <<'PYEOF'
p = 'chrome/browser/ui/views/omnibox/omnibox_view_views.cc'
s = open(p).read()
quote = chr(34)
browser_include = f'#include {quote}chrome/browser/ui/browser.h{quote}\n'
browser_window_include = (
    f'#include {quote}chrome/browser/ui/browser_window.h{quote}\n')
if browser_window_include not in s:
    if browser_include not in s:
        raise AssertionError('omnibox BrowserWindow include anchor not found')
    s = s.replace(browser_include,
                  browser_include + browser_window_include, 1)
    open(p, 'w').write(s)
old = ('  if (location_bar_view_ && location_bar_view_->browser()) {\n'
       '    focus_reveal_lock =\n'
       '        ImmersiveModeController::From(location_bar_view_->browser())\n'
       '            ->GetRevealedLock(ImmersiveModeController::ANIMATE_REVEAL_YES);\n'
       '  }')
old_cmux = ('  if (location_bar_view_ && location_bar_view_->browser()) {\n'
            '    // cmux: a Browser hosted by the shared workspace widget has no\n'
            '    // BrowserView-only ImmersiveModeController.\n'
            '    if (auto* immersive =\n'
            '            ImmersiveModeController::From(location_bar_view_->browser())) {\n'
            '      focus_reveal_lock = immersive->GetRevealedLock(\n'
            '          ImmersiveModeController::ANIMATE_REVEAL_YES);\n'
            '    }\n'
            '  }')
new = ('  if (location_bar_view_ && location_bar_view_->browser()) {\n'
       '    // cmux: a Browser hosted by the shared workspace widget has no\n'
       '    // BrowserView-only ImmersiveModeController. Test the concrete\n'
       '    // window boundary first: the user-data lookup is specified for\n'
       '    // BrowserView hosts and may be optimized as non-null.\n'
       '    Browser* browser = location_bar_view_->browser();\n'
       '    if (browser->window() && browser->window()->AsBrowserView()) {\n'
       '      if (auto* immersive = ImmersiveModeController::From(browser)) {\n'
       '        focus_reveal_lock = immersive->GetRevealedLock(\n'
       '            ImmersiveModeController::ANIMATE_REVEAL_YES);\n'
       '      }\n'
       '    }\n'
       '  }')
if new in s:
    print('omnibox-immersive-guard: already patched')
elif old_cmux in s:
    open(p, 'w').write(s.replace(old_cmux, new, 1))
    print('omnibox-immersive-guard: strengthened')
elif old in s:
    open(p, 'w').write(s.replace(old, new, 1))
    print('omnibox-immersive-guard: patched')
else:
    print('omnibox-immersive-guard: ANCHOR NOT FOUND')
PYEOF"

# 11. LocationBarView creates its page actions for a real Browser, but a Browser
#     hosted by CmuxBrowserWindow has no BrowserView-only ZoomBubbleCoordinator.
#     Keep the unsupported action hidden instead of crashing during its first
#     active-tab zoom notification or a queued activation. Idempotent.
ssh -o BatchMode=yes "$HOST" "cd '$SRC' && python3 - <<'PYEOF'
p = 'chrome/browser/ui/views/page_action/zoom_view.cc'
s = open(p).read()
changed = False
old_changed = '''  auto* zoom_bubble_coordinator = GetZoomBubbleCoordinator(web_contents);
  CHECK(zoom_bubble_coordinator);

  if (ShouldBeVisible(can_show_bubble)) {
'''
new_changed = '''  auto* zoom_bubble_coordinator = GetZoomBubbleCoordinator(web_contents);
  // cmux: a Browser hosted by a custom BrowserWindow has no BrowserView-only
  // ZoomBubbleCoordinator. Keep its unsupported page action hidden.
  if (!zoom_bubble_coordinator) {
    SetVisible(false);
    return;
  }

  if (ShouldBeVisible(can_show_bubble)) {
'''
if old_changed in s:
    s = s.replace(old_changed, new_changed, 1)
    changed = True

old_execute = '''  auto* zoom_bubble_coordinator = GetZoomBubbleCoordinator(GetWebContents());
  CHECK(zoom_bubble_coordinator);

  zoom_bubble_coordinator->Show(GetWebContents(), ZoomBubbleView::USER_GESTURE);
'''
new_execute = '''  auto* zoom_bubble_coordinator = GetZoomBubbleCoordinator(GetWebContents());
  // cmux: queued activation may outlive the optional BrowserView feature.
  if (!zoom_bubble_coordinator) {
    return;
  }

  zoom_bubble_coordinator->Show(GetWebContents(), ZoomBubbleView::USER_GESTURE);
'''
if old_execute in s:
    s = s.replace(old_execute, new_execute, 1)
    changed = True

marker_changed = 'cmux: a Browser hosted by a custom BrowserWindow has no BrowserView-only'
marker_execute = 'cmux: queued activation may outlive the optional BrowserView feature.'
if marker_changed in s and marker_execute in s:
    if changed:
        open(p, 'w').write(s)
        print('zoom-custom-window-guard: patched')
    else:
        print('zoom-custom-window-guard: already patched')
else:
    print('zoom-custom-window-guard: ANCHOR NOT FOUND')
PYEOF"

# 11b. Chromium's permission Views still have a few BrowserView-only seams
#      even though BrowserWindow exposes the production LocationBar contract.
#      cmux is a real Browser hosted by a custom BrowserWindow, so use those
#      generic anchors and make chooser/permission-element fallbacks safe.
#      The overlay supplies the matching pane-local tab-modal dialog host.
if [ "$CHROMIUM_MAJOR" -ge 151 ]; then
  ssh -o BatchMode=yes "$HOST" \
    "cd '$SRC' && python3 .cmux-patches/custom_window_permissions.py"
else
  echo "custom-window-permissions: not required before Chromium 151"
fi

# 11. A custom BrowserWindow may intentionally omit BrowserView's side panel
#     feature bundle. ReadAnything currently CHECKs that this only happens in a
#     test (with a one-off exemption for WebUIBrowser), which crashes while a
#     real cmux Browser closes its tabs. Treat any non-BrowserView window as a
#     supported embedder boundary here.
ssh -o BatchMode=yes "$HOST" "cd '$SRC' && python3 - <<'PYEOF'
p = 'chrome/browser/ui/read_anything/read_anything_side_panel_controller.cc'
s = open(p).read()
quote = chr(34)
browser_include = f'#include {quote}chrome/browser/ui/browser.h{quote}\n'
if browser_include not in s:
    browser_finder_include = (
        f'#include {quote}chrome/browser/ui/browser_finder.h{quote}\n')
    browser_features_include = (
        f'#include {quote}chrome/browser/ui/browser_window/public/'
        f'browser_window_features.h{quote}\n')
    if browser_finder_include in s:
        s = s.replace(browser_finder_include,
                      browser_include + browser_finder_include, 1)
    elif browser_features_include in s:
        # M151 no longer includes browser_finder.h here. Keep Browser complete
        # for the custom-window guard without depending on that retired anchor.
        s = s.replace(browser_features_include,
                      browser_include + browser_features_include, 1)
    else:
        raise AssertionError('read anything Browser include anchor not found')
    # Persist the include independently from the body mutation: on an
    # incremental re-apply the guard can already exist while an older patcher
    # silently missed this M151-only include anchor.
    open(p, 'w').write(s)
browser_window_include = (
    f'#include {quote}chrome/browser/ui/browser_window.h{quote}\n')
if browser_window_include not in s:
    if browser_include not in s:
        raise AssertionError('read anything BrowserWindow include anchor not found')
    s = s.replace(browser_include,
                  browser_include + browser_window_include, 1)
    open(p, 'w').write(s)
old = '''  if (!side_panel_ui) {
    // TODO(webium): create a SidePanelCoordinator for WebUIBrowser.
    // This is a temporary solution to avoid a crash.
    if (!webui_browser::IsWebUIBrowserEnabled()) {
      CHECK_IS_TEST();
    }
    return;  // IN-TEST
  }
'''
new = '''  if (!side_panel_ui) {
    // A Browser hosted by a custom BrowserWindow (including cmux's shared
    // workspace widget) has no BrowserView side-panel coordinator.
    Browser* browser = browser_window_interface->GetBrowserForMigrationOnly();
    if (browser && browser->window() &&
        !browser->window()->AsBrowserView()) {
      return;
    }
    // TODO(webium): create a SidePanelCoordinator for WebUIBrowser.
    // This is a temporary solution to avoid a crash.
    if (!webui_browser::IsWebUIBrowserEnabled()) {
      CHECK_IS_TEST();
    }
    return;  // IN-TEST
  }
'''
if 'including cmux\'s shared' in s:
    print('read-anything-custom-window-guard: already patched')
elif old in s:
    open(p, 'w').write(s.replace(old, new, 1))
    print('read-anything-custom-window-guard: patched')
else:
    print('read-anything-custom-window-guard: ANCHOR NOT FOUND')
PYEOF"

# cmux presents one real Browser per workspace without constructing a
# BrowserView. Install its SidePanelUI into BrowserWindowFeatures and create the
# normal window-scoped ExtensionSidePanelManager so upstream side-panel entries,
# options, ExtensionViewHost creation, and events remain Chrome-owned.
ssh -o BatchMode=yes "$HOST" "cd '$SRC' && python3 - <<'PYEOF'
p = 'chrome/browser/ui/browser_window/public/browser_window_features.h'
s = open(p).read()
method = '''  // Installs a SidePanelUI for a production custom BrowserWindow that does
  // not construct BrowserView. The caller owns side_panel_ui; passing null
  // tears down the window-scoped extension manager before the custom UI dies.
  void SetSidePanelUIForCustomBrowserWindow(SidePanelUI* side_panel_ui);

'''
anchor = '  SidePanelUI* side_panel_ui();\n'
if 'SetSidePanelUIForCustomBrowserWindow' not in s:
    assert s.count(anchor) == 1, 'custom side-panel public anchor changed'
    s = s.replace(anchor, method + anchor, 1)
member = '  raw_ptr<SidePanelUI> custom_side_panel_ui_ = nullptr;\n\n'
member_anchor = '  std::unique_ptr<SidePanelCoordinator> side_panel_coordinator_;\n'
if 'custom_side_panel_ui_' not in s:
    assert s.count(member_anchor) == 1, 'custom side-panel member anchor changed'
    s = s.replace(member_anchor, member_anchor + member, 1)
open(p, 'w').write(s)

p = 'chrome/browser/ui/browser_window/internal/browser_window_features.cc'
s = open(p).read()
accessor_old = '''SidePanelUI* BrowserWindowFeatures::side_panel_ui() {
  if (webui_browser::IsWebUIBrowserEnabled() && webui_browser_side_panel_ui_) {
    return webui_browser_side_panel_ui_.get();
  }

  return side_panel_coordinator_.get();
}
'''
tick = chr(96)
accessor_m151 = f'''SidePanelUI* BrowserWindowFeatures::side_panel_ui() {{
  // TODO(crbug.com/428946261): Remove this and replace all clients with
  // {tick}SidePanelUI::From(){tick}.
  return browser_ ? SidePanelUI::From(browser_) : nullptr;
}}
'''
setter = '''void BrowserWindowFeatures::SetSidePanelUIForCustomBrowserWindow(
    SidePanelUI* side_panel_ui) {
  CHECK(!side_panel_coordinator_);
  if (!side_panel_ui) {
    extension_side_panel_manager_.reset();
    custom_side_panel_ui_ = nullptr;
    return;
  }
  CHECK(!custom_side_panel_ui_);
  custom_side_panel_ui_ = side_panel_ui;
  extension_side_panel_manager_ =
      std::make_unique<extensions::ExtensionSidePanelManager>(
          browser_->GetBrowserForMigrationOnly(), side_panel_registry_.get());
}

'''
accessor_new = setter + '''SidePanelUI* BrowserWindowFeatures::side_panel_ui() {
  if (custom_side_panel_ui_) {
    return custom_side_panel_ui_;
  }
  if (webui_browser::IsWebUIBrowserEnabled() && webui_browser_side_panel_ui_) {
    return webui_browser_side_panel_ui_.get();
  }

  return side_panel_coordinator_.get();
}
'''
accessor_m151_new = setter + f'''SidePanelUI* BrowserWindowFeatures::side_panel_ui() {{
  if (custom_side_panel_ui_) {{
    return custom_side_panel_ui_;
  }}
  // TODO(crbug.com/428946261): Remove this and replace all clients with
  // {tick}SidePanelUI::From(){tick}.
  return browser_ ? SidePanelUI::From(browser_) : nullptr;
}}
'''
if 'SetSidePanelUIForCustomBrowserWindow' not in s:
    if s.count(accessor_old) == 1:
        s = s.replace(accessor_old, accessor_new, 1)
    else:
        assert s.count(accessor_m151) == 1, 'custom side-panel accessor anchor changed'
        s = s.replace(accessor_m151, accessor_m151_new, 1)
open(p, 'w').write(s)
print('custom-side-panel-ui: patched')
PYEOF"

# Upstream intentionally defers an inactive tab's contextual panel until that
# tab becomes the Browser's single active tab. cmux can display selected tabs
# from several panes simultaneously, so route explicit tab-scoped extension
# open/close calls to that tab's in-surface host when this is a cmux window.
ssh -o BatchMode=yes "$HOST" "cd '$SRC' && python3 - <<'PYEOF'
p = 'chrome/browser/ui/views/side_panel/extensions/extension_side_panel_utils.cc'
s = open(p).read()
include = '#include \"chrome/browser/cmux_term/cmux_side_panel.h\"\n'
include_anchor = '#include \"chrome/browser/ui/extensions/extension_side_panel_utils.h\"\n'
if include not in s:
    assert s.count(include_anchor) == 1, 'cmux side-panel utils include anchor changed'
    s = s.replace(include_anchor, include_anchor + '\n' + include, 1)
open_anchor = '''  SidePanelEntry::Key extension_key =
      SidePanelEntry::Key(SidePanelEntry::Id::kExtension, extension_id);

  if (browser_window.GetActiveTabInterface()->GetContents() == &web_contents) {
'''
open_replacement = '''  SidePanelEntry::Key extension_key =
      SidePanelEntry::Key(SidePanelEntry::Id::kExtension, extension_id);

  if (cmux::OpenCmuxContextualSidePanel(browser_window, web_contents,
                                        extension_id)) {
    return;
  }

  if (browser_window.GetActiveTabInterface()->GetContents() == &web_contents) {
'''
if 'OpenCmuxContextualSidePanel(browser_window' not in s:
    assert s.count(open_anchor) == 1, 'cmux contextual open anchor changed'
    s = s.replace(open_anchor, open_replacement, 1)
close_anchor = '''  const SidePanelEntry::Key extension_key(SidePanelEntry::Id::kExtension,
                                          extension_id);

  // Get the registry for the specific tab (whether active or inactive).
'''
close_replacement = '''  const SidePanelEntry::Key extension_key(SidePanelEntry::Id::kExtension,
                                          extension_id);

  if (cmux::CloseCmuxContextualSidePanel(browser_window, web_contents,
                                         extension_id)) {
    return;
  }

  // Get the registry for the specific tab (whether active or inactive).
'''
if 'CloseCmuxContextualSidePanel(browser_window' not in s:
    assert s.count(close_anchor) == 1, 'cmux contextual close anchor changed'
    s = s.replace(close_anchor, close_replacement, 1)
open(p, 'w').write(s)
print('cmux-contextual-side-panel-routing: patched')
PYEOF"

# Claude Code writes its native host manifest to Google Chrome's standard
# per-user directory. cmux keeps an isolated profile/product directory, so use
# Chrome's directory as a read-only fallback when cmux's own host registration
# is absent. Manifest origin checks still enforce the installed extension ID.
ssh -o BatchMode=yes "$HOST" "cd '$SRC' && python3 - <<'PYEOF'
p = 'chrome/browser/extensions/api/messaging/launch_context_posix.cc'
s = open(p).read()
include = '#include \"base/base_paths.h\"\n'
include_anchor = '#include \"base/command_line.h\"\n'
if include not in s:
    assert s.count(include_anchor) == 1, 'native host base-path include anchor changed'
    s = s.replace(include_anchor, include + include_anchor, 1)
old = '''  if (allow_user_level_hosts) {
    result = FindManifestInDir(chrome::DIR_USER_NATIVE_MESSAGING, host_name);
  }
  if (result.empty()) {
    result = FindManifestInDir(chrome::DIR_NATIVE_MESSAGING, host_name);
  }
'''
new = '''  if (allow_user_level_hosts) {
    result = FindManifestInDir(chrome::DIR_USER_NATIVE_MESSAGING, host_name);
  }

  // cmux uses its own product/profile directory, while tools such as Claude
  // Code install native host manifests for Google Chrome. Consult that
  // standard directory only as a fallback; NativeMessagingHostManifest still
  // validates allowed_origins before the host is launched.
  if (result.empty() && allow_user_level_hosts) {
    base::FilePath home;
    if (base::PathService::Get(base::DIR_HOME, &home)) {
#if BUILDFLAG(IS_MAC)
      base::FilePath chrome_hosts =
          home.Append(FILE_PATH_LITERAL(\"Library\"))
              .Append(FILE_PATH_LITERAL(\"Application Support\"))
              .Append(FILE_PATH_LITERAL(\"Google\"))
              .Append(FILE_PATH_LITERAL(\"Chrome\"))
              .Append(FILE_PATH_LITERAL(\"NativeMessagingHosts\"));
#elif BUILDFLAG(IS_LINUX)
      base::FilePath chrome_hosts =
          home.Append(FILE_PATH_LITERAL(\".config\"))
              .Append(FILE_PATH_LITERAL(\"google-chrome\"))
              .Append(FILE_PATH_LITERAL(\"NativeMessagingHosts\"));
#endif
#if BUILDFLAG(IS_MAC) || BUILDFLAG(IS_LINUX)
      base::FilePath candidate = chrome_hosts.Append(host_name + \".json\");
      if (base::PathExists(candidate)) {
        result = std::move(candidate);
      }
#endif
    }
  }
  if (result.empty()) {
    result = FindManifestInDir(chrome::DIR_NATIVE_MESSAGING, host_name);
  }
'''
if 'tools such as Claude Code install native host manifests' not in s:
    assert s.count(old) == 1, 'native host fallback anchor changed'
    s = s.replace(old, new, 1)
open(p, 'w').write(s)
print('cmux-native-messaging-chrome-fallback: patched')
PYEOF"

# Browser shutdown may outlive a custom BrowserWindow's presentation. The
# regular BrowserView always has a LocationBar, but custom production windows
# can legitimately return null once their host UI begins teardown.
ssh -o BatchMode=yes "$HOST" "cd '$SRC' && python3 - <<'PYEOF'
p = 'chrome/browser/ui/browser.cc'
s = open(p).read()
old = '''  // Save what the user's currently typing, so it can be restored when we
  // switch back to this tab.
  window_->GetLocationBar()->SaveStateToContents(contents);
'''
new = '''  // Save what the user's currently typing, so it can be restored when we
  // switch back to this tab. A custom BrowserWindow's presentation may have
  // already begun teardown while asynchronous unload processing continues.
  if (LocationBar* location_bar = window_->GetLocationBar()) {
    location_bar->SaveStateToContents(contents);
  }
'''
if 'while asynchronous unload processing continues' in s:
    print('browser-locationbar-teardown-guard: already patched')
elif old in s:
    open(p, 'w').write(s.replace(old, new, 1))
    print('browser-locationbar-teardown-guard: patched')
else:
    print('browser-locationbar-teardown-guard: ANCHOR NOT FOUND')
PYEOF"

# 12. Post-install success dialog: never create/show a tabbed Browser just to
#     parent the success UI. Route extension installs through a live cmux pane
#     callback, prefer the cmux puzzle-button bubble host, and skip app
#     notifications that would navigate/show Browser UI. Idempotent.
ssh -o BatchMode=yes "$HOST" "cd '$SRC' && python3 - <<'PYEOF'
p = 'chrome/browser/ui/extensions/extension_install_ui_desktop.cc'
s = open(p).read()
def replace_once(text, old, new, label):
    assert text.count(old) == 1, label + ' anchor not unique/found'
    return text.replace(old, new, 1)

if '#include \"base/logging.h\"' not in s:
    s = replace_once(s, '#include \"base/functional/bind.h\"\n',
                     '#include \"base/functional/bind.h\"\n'
                     '#include \"base/logging.h\"\n',
                     'postinstall logging include')
if '#include \"chrome/browser/cmux_term/cmux_extensions_container.h\"' not in s:
    s = replace_once(s, '#include \"build/build_config.h\"\n',
                     '#include \"build/build_config.h\"\n'
                     '#include \"chrome/browser/cmux_term/cmux_extensions_container.h\"\n'
                     '#include \"chrome/browser/cmux_term/cmux_views.h\"\n',
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
                    << \"cmux: skipping post-install dialog; no active pane\";
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
    open(p, 'w').write(s)
    print('postinstall-cmux-dialog: patched old guard')
elif old_pristine in s:
    s = s.replace(old_pristine, new_block, 1)
    open(p, 'w').write(s)
    print('postinstall-cmux-dialog: patched pristine')
else:
    raise AssertionError('postinstall-cmux-dialog anchor not found')
PYEOF"

# 12b. Export the real post-install DialogModel builder so cmux can host the
#      stock content at its own puzzle-button anchor. Idempotent.
ssh -o BatchMode=yes "$HOST" "cd '$SRC' && python3 - <<'PYEOF'
def replace_once(text, old, new, label):
    assert text.count(old) == 1, label + ' anchor not unique/found'
    return text.replace(old, new, 1)

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
    open(p, 'w').write(s)
    print('postinstall-dialog-builder-h: patched')

p = 'chrome/browser/ui/extensions/extension_post_install_dialog.cc'
s = open(p).read()
old_configure_call = '''  ConfigurePostInstallDialogModel(dialog_model_builder, weak_delegate->model(),
                                  manage_shortcuts_callback);
'''
new_configure_call = '''  ConfigurePostInstallDialogModel(
      profile, dialog_model_builder, weak_delegate->model(),
      manage_shortcuts_callback);
'''
configure_takes_profile = '''void ConfigurePostInstallDialogModel(
    Profile* profile,
''' in s
if configure_takes_profile and old_configure_call in s:
    s = s.replace(old_configure_call, new_configure_call, 1)
    open(p, 'w').write(s)
    print('postinstall-dialog-builder-cc: updated Chromium 151 signature')

configure_call = (new_configure_call if configure_takes_profile
                  else old_configure_call)
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
''' + configure_call + '''

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
    open(p, 'w').write(s)
    print('postinstall-dialog-builder-cc: patched')
PYEOF"

# 14. Full uBlock Origin is Manifest V2. Mirror Helium/ungoogled's MV2 policy
#     stance: MV2 remains an allowed manifest version, MV2 extensions are exempt
#     from deprecation policy, and the experiment manager never disables legacy
#     extensions. Idempotent.
ssh -o BatchMode=yes "$HOST" "cd '$SRC' && python3 - <<'PYEOF'
p = 'chrome/browser/extensions/extension_management.cc'
from pathlib import Path

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
PYEOF"

ssh -o BatchMode=yes "$HOST" "cd '$SRC' && python3 - <<'PYEOF'
from pathlib import Path

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
PYEOF"

# 14b. Extensions menu coordinator reuse seams. These are deliberately narrow:
#     flip the new-menu chain from Browser* to BrowserWindowInterface*, keep
#     both upstream menu implementations buildable, and add cmux embedder
#     switches for row activation and row context menus. Idempotent and
#     marker-guarded.
if ssh -o BatchMode=yes "$HOST" "cd '$SRC' &&
  grep -q 'SetEmbedderActionActivationOverride' chrome/browser/ui/views/extensions/extensions_menu_delegate_desktop.h &&
  grep -q 'SetContextMenusEnabledForEmbedder' chrome/browser/ui/views/extensions/extensions_menu_entry_view.h"; then
  echo 'extensions-menu-reuse-seams: already patched'
else
  {
perl -pe 's/\\{2}n/\\n/g' <<'PYEOF'
from pathlib import Path

def read(p):
    return Path(p).read_text()

def write(p, s):
    Path(p).write_text(s)

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
s = read(p)
s = replace_once(s, '#include \"base/memory/raw_ref.h\"\\n',
                 '#include \"base/memory/raw_ref.h\"\\n'
                 '#include \"chrome/browser/ui/browser_window/public/browser_window_interface.h\"\\n',
                 'coordinator include')
s = replace_all_present(s, 'class Browser;\\n', 'class BrowserWindowInterface;\\n')
s = replace_all_present(s, 'ExtensionsMenuCoordinator(Browser* browser,',
                        'ExtensionsMenuCoordinator(BrowserWindowInterface* browser,')
s = replace_all_present(s, 'const raw_ptr<Browser> browser_;',
                        'const raw_ptr<BrowserWindowInterface> browser_;')
write(p, s)

p = 'chrome/browser/ui/views/extensions/extensions_menu_coordinator.cc'
s = read(p)
s = replace_all_present(
    s, '#include \"chrome/browser/ui/browser.h\"',
    '#include \"chrome/browser/ui/browser_window/public/browser_window_interface.h\"')
s = replace_all_present(s, 'Browser* browser,\\n    ExtensionsContainer* extensions_container)',
                        'BrowserWindowInterface* browser,\\n    ExtensionsContainer* extensions_container)')
write(p, s)

# extensions_menu_delegate_desktop.{h,cc}: BWI flip + row activation override.
p = 'chrome/browser/ui/views/extensions/extensions_menu_delegate_desktop.h'
s = read(p)
s = replace_once(s, '#include \"base/memory/raw_ptr.h\"\\n',
                 '#include <string>\\n\\n'
                 '#include \"base/functional/callback_forward.h\"\\n'
                 '#include \"base/memory/raw_ptr.h\"\\n',
                 'delegate h includes')
s = replace_all_present(s, 'class Browser;\\n', 'class BrowserWindowInterface;\\n')
s = replace_all_present(s, 'Browser* browser,\\n      ExtensionsContainer* extensions_container,',
                        'BrowserWindowInterface* browser,\\n      ExtensionsContainer* extensions_container,')
decl = ('  // cmux: lets a BrowserWindowInterface embedder handle menu-row\\n'
        '  // activation before ExtensionActionViewModel creates a Browser-only\\n'
        '  // popup host. Unset keeps Chrome behavior byte-identical.\\n'
        '  using ActionActivationOverride =\\n'
        '      base::RepeatingCallback<bool(const std::string& action_id)>;\\n'
        '  static void SetEmbedderActionActivationOverride(\\n'
        '      ActionActivationOverride cb);\\n\\n')
if 'SetEmbedderActionActivationOverride' not in s:
    s = s.replace('  ~ExtensionsMenuDelegateDesktop() override;\\n\\n',
                  '  ~ExtensionsMenuDelegateDesktop() override;\\n\\n' + decl, 1)
s = replace_all_present(s, 'const raw_ptr<Browser> browser_;',
                        'const raw_ptr<BrowserWindowInterface> browser_;')
write(p, s)

p = 'chrome/browser/ui/views/extensions/extensions_menu_delegate_desktop.cc'
s = read(p)
s = replace_once(s, '#include <algorithm>\\n',
                 '#include <algorithm>\\n#include <utility>\\n',
                 'delegate cc utility')
s = replace_once(s, '#include \"base/check_deref.h\"\\n',
                 '#include \"base/check_deref.h\"\\n#include \"base/no_destructor.h\"\\n',
                 'delegate cc no_destructor')
s = replace_all_present(
    s, '#include \"chrome/browser/ui/browser.h\"',
    '#include \"chrome/browser/ui/browser_window/public/browser_window_interface.h\"')
if '#include \"components/tabs/public/tab_interface.h\"' not in s:
    s = s.replace('#include \"chrome/grit/generated_resources.h\"\\n',
                  '#include \"chrome/grit/generated_resources.h\"\\n'
                  '#include \"components/tabs/public/tab_interface.h\"\\n', 1)
if ('->GetTabStripModel()' in s and
        '#include \"chrome/browser/ui/tabs/tab_strip_model.h\"' not in s):
    s = s.replace(
        '#include \"chrome/browser/ui/browser_window/public/browser_window_interface.h\"\\n',
        '#include \"chrome/browser/ui/browser_window/public/browser_window_interface.h\"\\n'
        '#include \"chrome/browser/ui/tabs/tab_strip_model.h\"\\n', 1)
helper = ('ExtensionsMenuDelegateDesktop::ActionActivationOverride&\\n'
          'GetEmbedderActionActivationOverride() {\\n'
          '  static base::NoDestructor<\\n'
          '      ExtensionsMenuDelegateDesktop::ActionActivationOverride>\\n'
          '      activation_override;\\n'
          '  return *activation_override;\\n'
          '}\\n\\n')
if 'GetEmbedderActionActivationOverride()' not in s:
    s = s.replace('}  // namespace\\n\\nExtensionsMenuDelegateDesktop::ExtensionsMenuDelegateDesktop(',
                  helper + '}  // namespace\\n\\n'
                  'ExtensionsMenuDelegateDesktop::ExtensionsMenuDelegateDesktop(', 1)
s = replace_all_present(s, 'Browser* browser,\\n    ExtensionsContainer* extensions_container,',
                        'BrowserWindowInterface* browser,\\n    ExtensionsContainer* extensions_container,')
s = replace_all_present(s, 'ToolbarActionsModel::Get(browser_->profile())',
                        'ToolbarActionsModel::Get(browser_->GetProfile())')
method = ('void ExtensionsMenuDelegateDesktop::SetEmbedderActionActivationOverride(\\n'
          '    ActionActivationOverride cb) {\\n'
          '  GetEmbedderActionActivationOverride() = std::move(cb);\\n'
          '}\\n\\n')
if 'ExtensionsMenuDelegateDesktop::SetEmbedderActionActivationOverride' not in s:
    s = s.replace('ExtensionsMenuDelegateDesktop::~ExtensionsMenuDelegateDesktop() = default;\\n\\n',
                  'ExtensionsMenuDelegateDesktop::~ExtensionsMenuDelegateDesktop() = default;\\n\\n' + method, 1)
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
m150 = '''    DCHECK_NE(PermissionsManager::Get(browser_->GetProfile())
                  ->GetUserSiteSetting(browser_->GetTabStripModel()
                                           ->GetActiveWebContents()
                                           ->GetPrimaryMainFrame()
                                           ->GetLastCommittedOrigin()),
              PermissionsManager::UserSiteSetting::kCustomizeByExtension);
'''
if m150 not in s:
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
write(p, s)

# extensions_menu_main_page_view.{h,cc}: BWI flip.
p = 'chrome/browser/ui/views/extensions/extensions_menu_main_page_view.h'
s = read(p)
s = replace_all_present(s, 'class Browser;\\n', 'class BrowserWindowInterface;\\n')
s = replace_all_present(s, 'explicit ExtensionsMenuMainPageView(Browser* browser,',
                        'explicit ExtensionsMenuMainPageView(BrowserWindowInterface* browser,')
s = replace_all_present(s, 'const raw_ptr<Browser> browser_;',
                        'const raw_ptr<BrowserWindowInterface> browser_;')
write(p, s)

p = 'chrome/browser/ui/views/extensions/extensions_menu_main_page_view.cc'
s = read(p)
s = replace_all_present(
    s, '#include \"chrome/browser/ui/browser.h\"',
    '#include \"chrome/browser/ui/browser_window/public/browser_window_interface.h\"')
s = replace_all_present(s, 'ExtensionsMenuMainPageView::ExtensionsMenuMainPageView(\\n    Browser* browser,',
                        'ExtensionsMenuMainPageView::ExtensionsMenuMainPageView(\\n    BrowserWindowInterface* browser,')
s = replace_all_present(s, '[](Browser* browser) {',
                        '[](BrowserWindowInterface* browser) {')
write(p, s)

# extensions_menu_site_permissions_page_view.{h,cc}: BWI flip.
p = 'chrome/browser/ui/views/extensions/extensions_menu_site_permissions_page_view.h'
s = read(p)
s = replace_all_present(s, 'class Browser;\\n', 'class BrowserWindowInterface;\\n')
s = replace_all_present(s, 'Browser* browser,\\n      extensions::ExtensionId extension_id,',
                        'BrowserWindowInterface* browser,\\n      extensions::ExtensionId extension_id,')
s = replace_all_present(s, 'const raw_ptr<Browser> browser_;',
                        'const raw_ptr<BrowserWindowInterface> browser_;')
write(p, s)

p = 'chrome/browser/ui/views/extensions/extensions_menu_site_permissions_page_view.cc'
s = read(p)
s = replace_all_present(
    s, '#include \"chrome/browser/ui/browser.h\"',
    '#include \"chrome/browser/ui/browser_window/public/browser_window_interface.h\"')
s = replace_all_present(s, 'Browser* browser,\\n    extensions::ExtensionId extension_id,',
                        'BrowserWindowInterface* browser,\\n    extensions::ExtensionId extension_id,')
old = '''[](Browser* browser,
                                 extensions::ExtensionId extension_id) {'''
new = '''[](BrowserWindowInterface* browser,
                                 extensions::ExtensionId extension_id) {'''
s = replace_once(s, old, new, 'site permissions settings lambda BWI')
write(p, s)

# extensions_menu_entry_view.{h,cc}: unused param flip + context-menu switch.
p = 'chrome/browser/ui/views/extensions/extensions_menu_entry_view.h'
s = read(p)
s = replace_all_present(s, 'class Browser;\\n', 'class BrowserWindowInterface;\\n')
s = replace_all_present(s, 'Browser* browser,\\n      bool is_enterprise,',
                        'BrowserWindowInterface* browser,\\n      bool is_enterprise,')
decl = ('  // cmux: BrowserWindowInterface embedders can disable the in-menu\\n'
        '  // per-row context menu; the toolbar strip context menu remains.\\n'
        '  static void SetContextMenusEnabledForEmbedder(bool enabled);\\n\\n')
if 'SetContextMenusEnabledForEmbedder' not in s:
    s = s.replace('  ~ExtensionsMenuEntryView() override;\\n\\n',
                  '  ~ExtensionsMenuEntryView() override;\\n\\n' + decl, 1)
write(p, s)

p = 'chrome/browser/ui/views/extensions/extensions_menu_entry_view.cc'
s = read(p)
s = replace_all_present(
    s, '#include \"chrome/browser/ui/browser.h\"',
    '#include \"chrome/browser/ui/browser_window/public/browser_window_interface.h\"')
switch = ('bool& ContextMenusEnabledForEmbedder() {\\n'
          '  static bool enabled = true;\\n'
          '  return enabled;\\n'
          '}\\n\\n')
if 'ContextMenusEnabledForEmbedder()' not in s:
    s = s.replace('namespace {\\n\\n', 'namespace {\\n\\n' + switch, 1)
s = replace_all_present(s, 'ExtensionsMenuEntryView::ExtensionsMenuEntryView(\\n    Browser* browser,',
                        'ExtensionsMenuEntryView::ExtensionsMenuEntryView(\\n    BrowserWindowInterface* browser,')
method = ('void ExtensionsMenuEntryView::SetContextMenusEnabledForEmbedder(\\n'
          '    bool enabled) {\\n'
          '  ContextMenusEnabledForEmbedder() = enabled;\\n'
          '}\\n\\n')
if 'ExtensionsMenuEntryView::SetContextMenusEnabledForEmbedder' not in s:
    s = s.replace('ExtensionsMenuEntryView::~ExtensionsMenuEntryView() = default;\\n\\n',
                  'ExtensionsMenuEntryView::~ExtensionsMenuEntryView() = default;\\n\\n' + method, 1)
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
      \"Extensions.Toolbar.MoreActionsButtonPressedFromMenu\"));
'''
new = '''void ExtensionsMenuEntryView::OnContextMenuPressed() {
  if (!context_menu_controller_) {
    return;
  }
  base::RecordAction(base::UserMetricsAction(
      \"Extensions.Toolbar.MoreActionsButtonPressedFromMenu\"));
'''
s = replace_once(s, old, new, 'entry context menu pressed guard')
write(p, s)

# The reference Chrome UI uses the compact menu (Access requested, pin and
# overflow controls) and ToolbarIconContainerView's hover outline. Both are the
# disabled state of kExtensionsMenuAccessControl in this Chromium revision.
p = 'extensions/common/extension_features.cc'
s = read(p)
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
write(p, s)

print('extensions-menu-reuse-seams: patched')
PYEOF
  } | ssh -o BatchMode=yes "$HOST" "cd '$SRC' && python3 -"
fi

# 15. Hide MV2 deprecation UI/warnings for the full-MV2 build. This matches the
#     user-visible half of Helium's MV2 patch: chrome://extensions should not
#     report MV2 extensions as deprecated, and unpacked MV2 loads should not get
#     a deprecation install warning. Idempotent.
ssh -o BatchMode=yes "$HOST" "cd '$SRC' && python3 - <<'PYEOF'
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
PYEOF"

ssh -o BatchMode=yes "$HOST" "cd '$SRC' && python3 - <<'PYEOF'
p = 'chrome/browser/ui/webui/extensions/extensions_ui.cc'
s = open(p).read()
anchor = '''  // MV2 deprecation.
  auto* mv2_experiment_manager = ManifestV2ExperimentManager::Get(profile);
  MV2ExperimentStage experiment_stage =
      mv2_experiment_manager->GetCurrentExperimentStage();
  source->AddInteger(\"MV2ExperimentStage\", static_cast<int>(experiment_stage));
  source->AddBoolean(
      \"MV2DeprecationNoticeDismissed\",
      mv2_experiment_manager->DidUserAcknowledgeNoticeGlobally());
'''
add = '''  // cmux: full MV2 extensions remain supported; keep the extensions page in
  // its warning-era state with the notice treated as dismissed.
  source->AddInteger(\"MV2ExperimentStage\",
                     static_cast<int>(MV2ExperimentStage::kWarning));
  source->AddBoolean(\"MV2DeprecationNoticeDismissed\", true);
'''
anchor_150 = '''  // MV2 deprecation.
  auto* mv2_experiment_manager = ManifestV2ExperimentManager::Get(profile);
  source->AddBoolean(
      \"MV2DeprecationNoticeDismissed\",
      mv2_experiment_manager->DidUserAcknowledgeNoticeGlobally());
'''
anchor_151 = '''  // MV2 deprecation.
  auto* mv2_handler = ManifestV2Handler::Get(profile);
  source->AddBoolean(\"MV2DeprecationNoticeDismissed\",
                     mv2_handler->DidUserAcknowledgeNoticeGlobally());
'''
add_150 = '''  // cmux: full MV2 extensions remain supported; the M150+ extensions page
  // no longer consumes an experiment-stage value, so only dismiss its notice.
  source->AddBoolean(\"MV2DeprecationNoticeDismissed\", true);
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
PYEOF"

ssh -o BatchMode=yes "$HOST" "cd '$SRC' && python3 - <<'PYEOF'
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
PYEOF"
fi  # end MODE in {full, chrome_linux}

# 9. Wire the cross-platform cmux UI demo into views_examples so the rail +
#    niri strip + tab Views iterate in a ~30s standalone-app build loop (no full
#    chrome) and prove cross-platform parity. Adds the CmuxDemoExample to the
#    example registry and makes views_examples_lib depend on the
#    //chrome/browser/cmux_term:cmux_chrome_ui source_set. Idempotent.
ssh -o BatchMode=yes "$HOST" "cd '$SRC' && python3 - <<'PYEOF'
# create_examples.cc: include + register the example.
p = 'ui/views/examples/create_examples.cc'
s = open(p).read()
inc = '#include \"ui/views/examples/cmux_demo_example.h\"\n'
if 'cmux_demo_example.h' not in s:
    anchor = '#include \"ui/views/examples/create_examples.h\"\n'
    s = s.replace(anchor, anchor + inc, 1)
reg = ('  examples.push_back(std::make_unique<CmuxDemoExample>(1, \"Cmux Rail - Polished\"));\n'
       '  examples.push_back(std::make_unique<CmuxDemoExample>(0, \"Cmux Rail - Minimal\"));\n'
       '  examples.push_back(std::make_unique<CmuxDemoExample>(2, \"Cmux Rail - Arc\"));\n')
old = '  examples.push_back(std::make_unique<CmuxDemoExample>());\n'
if old in s:
    s = s.replace(old, reg, 1)  # upgrade the earlier single registration
elif 'CmuxDemoExample' not in s.split('CreateExamples(ExampleVector')[1] if 'CreateExamples(ExampleVector' in s else True:
    anchor = '  ExampleVector examples = std::move(extra_examples);\n'
    if reg not in s:
        s = s.replace(anchor, anchor + reg, 1)
open(p, 'w').write(s)
print('cmux-example-register:', 'CmuxDemoExample' in open(p).read())

# cmux_chrome_ui deliberately does not link Chrome's browser color-mixer
# graph, but its rail/group-editor/hover-card Views consume a small set of
# Chrome ColorIds. Install the demo-owned definitions after the generic Views
# mixer so the standalone harness never renders placeholder colors.
p = 'ui/views/examples/examples_main_proc.cc'
s = open(p).read()
color_include = '#include \"ui/views/examples/cmux_demo_example.h\"\n'
if color_include not in s:
    include_anchor = '#include \"ui/views/examples/example_base.h\"\n'
    assert s.count(include_anchor) == 1, 'cmux demo color include anchor changed'
    s = s.replace(include_anchor, color_include + include_anchor, 1)
color_initializer = '''  ui::ColorProviderManager::Get().AppendColorProviderInitializer(
      base::BindRepeating(&AddCmuxDemoColorMixers));
'''
if color_initializer not in s:
    initializer_anchor = '''  ui::ColorProviderManager::Get().AppendColorProviderInitializer(
      base::BindRepeating(&AddExamplesColorMixers));
'''
    assert s.count(initializer_anchor) == 1, 'examples color initializer anchor changed'
    s = s.replace(initializer_anchor,
                  initializer_anchor + color_initializer, 1)
open(p, 'w').write(s)
print('cmux-example-color-mixer:', color_initializer in open(p).read())

# Chromium 151's standalone Views harness creates viz GPU consumers without
# installing the MemoryConsumerRegistry that Chrome's content main loop owns.
# Keep the demo honest by installing the existing test registry for the full
# ExamplesMainProc lifetime; otherwise it CHECKs a few seconds after launch.
# The registry did not exist in Chromium 150, whose harness does not need it.
import os
registry_header = 'base/memory_coordinator/test_memory_consumer_registry.h'
if os.path.exists(registry_header):
    p = 'ui/views/examples/examples_main.cc'
    s = open(p).read()
    registry_include = '#include \"base/memory_coordinator/test_memory_consumer_registry.h\"\n'
    if registry_include not in s:
        include_anchor = '#include \"base/command_line.h\"\n'
        assert s.count(include_anchor) == 1, 'views examples registry include anchor changed'
        s = s.replace(include_anchor, include_anchor + registry_include, 1)
    registry_decl = '  base::TestMemoryConsumerRegistry memory_consumer_registry;\n'
    if registry_decl not in s:
        registry_anchor = '  base::AtExitManager at_exit;\n'
        assert s.count(registry_anchor) == 1, 'views examples registry lifetime anchor changed'
        s = s.replace(registry_anchor, registry_anchor + '\n' + registry_decl, 1)
    open(p, 'w').write(s)
    print('cmux-example-memory-registry:', registry_decl in open(p).read())
else:
    print('cmux-example-memory-registry: not required on this revision')

# ui/views/examples/BUILD.gn: add sources + the cmux_chrome_ui dep to the lib.
p = 'ui/views/examples/BUILD.gn'
s = open(p).read()
src_anchor = '    \"create_examples.cc\",\n    \"create_examples.h\",\n'
src_add = ('    \"cmux_demo_example.cc\",\n    \"cmux_demo_example.h\",\n')
if 'cmux_demo_example.cc' not in s:
    s = s.replace(src_anchor, src_anchor + src_add, 1)
dep = '\"//chrome/browser/cmux_term:cmux_chrome_ui\"'
if dep not in s:
    idx = s.find('(\"views_examples_lib\") {')
    dstart = s.find('deps = [', idx)
    assert idx != -1 and dstart != -1, 'views_examples_lib deps anchor not found'
    s = s[:dstart + len('deps = [')] + '\n    ' + dep + ',' + s[dstart + len('deps = ['):]
open(p, 'w').write(s)
print('cmux-example-build:',
      'cmux_demo_example.cc' in open(p).read(),
      '| dep', dep in open(p).read())
PYEOF"

# Apply changes tracked GN inputs. Regenerate explicitly instead of relying on
# Ninja's automatic regeneration: during a toolchain path migration, the old
# graph may be unable to stat its old Xcode tools and cannot reach the regen
# edge at all.
ssh -o BatchMode=yes "$HOST" "set -e; export PATH='$CMUX_DEPOT_TOOLS':\"\$PATH\"; cd '$SRC'; gn gen out/Release"
