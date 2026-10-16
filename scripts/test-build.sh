#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
script="$root/scripts/build.sh"

grep -Fq 'out/Release/cmux_build.log' "$script"
if grep -Fq '/tmp/cmux_build_log.txt' "$script"; then
  echo "build log must stay in the isolated Chromium workspace" >&2
  exit 1
fi
if grep -Fq '\"' "$script"; then
  echo "heredoc remote commands must not preserve literal quote characters" >&2
  exit 1
fi
grep -Fq '> "$log" 2>&1 &' "$script"
grep -Fq 'wait "$build_pid"' "$script"
grep -Fq 'SIGABRT: abort' "$script"
grep -Fq 'retrying once' "$script"
grep -Fq 'gn gen out/Release' "$root/scripts/apply.sh"
grep -Fq 'window()->AsBrowserView()' "$root/scripts/apply.sh"
grep -Fq 'window()->AsBrowserView()' "$root/scripts/apply_win_chrome.py"
grep -Fq 'browser_window.h{quote}' "$root/scripts/apply.sh"
grep -Fq 'read anything Browser include anchor not found' \
  "$root/scripts/apply.sh"
grep -Fq 'browser_window_features.h{quote}' \
  "$root/scripts/apply.sh"
grep -Fq '#include "chrome/browser/ui/browser_window.h"' "$root/scripts/apply_win_chrome.py"
grep -Fq 'bool IsFocusOnExtensionAction() const override;' \
  "$root/overlay/chrome/browser/cmux_term/cmux_extension_strip.h"
grep -Fq 'focus_manager->GetFocusedView()' \
  "$root/overlay/chrome/browser/cmux_term/cmux_extension_strip.cc"
grep -Fq 'struct PinnedIdsForSlots {' \
  "$root/overlay/chrome/browser/cmux_term/cmux_extension_strip.cc"
grep -Fq '#include "extensions/browser/extension_action_manager.h"' \
  "$root/overlay/chrome/browser/cmux_term/cmux_extension_strip.cc"
grep -Fq 'PinnedIdsForSlots GetPinnedIdsForSlots(' \
  "$root/overlay/chrome/browser/cmux_term/cmux_extension_strip.cc"
grep -Fq '#include "chrome/common/chrome_version.h"' \
  "$root/overlay/chrome/browser/cmux_term/cmux_extension_strip.cc"
grep -Fq '#include "components/vector_icons/vector_icons.h"' \
  "$root/overlay/chrome/browser/cmux_term/cmux_extension_strip.cc"
grep -Fq '#include "ui/views/accessibility/view_accessibility.h"' \
  "$root/overlay/chrome/browser/cmux_term/cmux_extension_strip.cc"
if [[ "$(grep -Fc 'bool IsFocusOnExtensionAction() const override;' \
    "$root/overlay/chrome/browser/cmux_term/cmux_extension_strip.h")" -ne 1 ]] ||
   [[ "$(grep -Fc 'bool CmuxExtensionStrip::IsFocusOnExtensionAction() const {' \
    "$root/overlay/chrome/browser/cmux_term/cmux_extension_strip.cc")" -ne 1 ]]; then
  echo "extension focus delegate must have exactly one declaration and definition" >&2
  exit 1
fi
python3 - \
  "$root/overlay/chrome/browser/cmux_term/cmux_extension_strip.h" \
  "$root/overlay/chrome/browser/cmux_term/cmux_extension_strip.cc" \
  "$root/overlay/chrome/browser/cmux_term/cmux_extensions_container.cc" <<'PY'
import pathlib
import re
import sys

for filename in sys.argv[1:]:
    stack = []
    for line_number, line in enumerate(
        pathlib.Path(filename).read_text().splitlines(), start=1
    ):
        directive = re.match(r"^\s*#\s*(if|ifdef|ifndef|endif)\b", line)
        if not directive:
            continue
        if directive.group(1) == "endif":
            if not stack:
                raise SystemExit(f"{filename}:{line_number}: unmatched #endif")
            stack.pop()
        else:
            stack.append(line_number)
    if stack:
        raise SystemExit(f"{filename}:{stack[-1]}: unterminated conditional")
PY
if grep -Eq 'cmux-profile-launcher|Chromium\.real|MacOS/Chromium' \
    "$root/scripts/deploy.sh"; then
  echo "deploy must preserve the compiled cmux executable identity" >&2
  exit 1
fi
grep -Fq '"cmux_theme_ghostty.cc",' \
  "$root/overlay/chrome/browser/cmux_term/BUILD.gn"
grep -Fq 'cmux_term/cmux_theme_ghostty.cc' "$root/scripts/apply.sh"
grep -Fq 'CMUX_DOGFOOD_BUNDLE_ID' "$root/scripts/build-dogfood.sh"
grep -Fq 'export CMUX_MAC_BUNDLE_ID="$DOGFOOD_BUNDLE_ID"' \
  "$root/scripts/build-dogfood.sh"
grep -Fq 'CMUX_EXPECTED_BUNDLE_ID="$DOGFOOD_BUNDLE_ID"' \
  "$root/scripts/build-dogfood.sh"
grep -Fq 'CMUX_PRODUCT_DIR_NAME="$DOGFOOD_PRODUCT_DIR"' \
  "$root/scripts/build-dogfood.sh"
grep -Fq '"$ROOT/scripts/verify-build-bundle-identity.sh"' \
  "$root/scripts/deploy.sh"
grep -Fq '!browser->window()->AsBrowserView()' \
  "$root/overlay/chrome/browser/cmux_term/cmux_views.cc"
grep -Fq 'browser->window() && browser->window()->AsBrowserView()' \
  "$root/overlay/chrome/browser/cmux_term/cmux_views.cc"
grep -Fq 'CMUX_BUILD_PROGRESS percent=' "$root/scripts/build.sh"
grep -Fq 'TARGET="${CMUX_BUILD_TARGET:-chrome}"' "$root/scripts/build.sh"
grep -Fq 'scripts/verify-cmux-runtime-pins.sh "$CMUX_RUNTIME_IMAGE"' \
  "$root/scripts/deploy.sh"
grep -Fq 'scripts/resolve-chromium-runtime-image.sh "$DEST"' \
  "$root/scripts/deploy.sh"
grep -Fq 'CMUX_TUI_SKIP_RUN_VERIFY=0' "$root/scripts/deploy.sh"
grep -Fq '#include "ui/views/accessibility/view_accessibility.h"' \
  "$root/overlay/chrome/browser/cmux_term/cmux_rail.cc"
grep -Fq 'GetViewAccessibility().SetName(is_update_ ? u"Update now"' \
  "$root/overlay/chrome/browser/cmux_term/cmux_rail.cc"
grep -Fq ': u"New workspace");' \
  "$root/overlay/chrome/browser/cmux_term/cmux_rail.cc"
if grep -Fq 'ApplyWindowBackgrounds();' \
  "$root/overlay/chrome/browser/cmux_term/cmux_views.cc"; then
  echo 'theme reload must apply the rail tint through ApplyRailLayoutConfig' >&2
  exit 1
fi
grep -Fq 'ApplyWindowBackgrounds(rail_tint);' \
  "$root/overlay/chrome/browser/cmux_term/cmux_views.cc"
grep -Fq '148|149)' "$root/scripts/apply.sh"
grep -Fq '  150)' "$root/scripts/apply.sh"
grep -Fq 'helium-omnibar-chromium-151.patch' "$root/scripts/apply.sh"
grep -Fq 'unsupported Chromium major' "$root/scripts/apply.sh"
grep -Fq 'standard_icon_label_bubble_layout_strategy.cc' \
  "$root/patches/helium-omnibar-chromium-151.patch"
grep -Fq '#if CHROME_VERSION_MAJOR >= 151' \
  "$root/overlay/chrome/browser/cmux_term/cmux_tab_strip.cc"
grep -Fq 'vector_icons::kAddOldIcon' \
  "$root/overlay/chrome/browser/cmux_term/cmux_tab_strip.cc"
views_source="$root/overlay/chrome/browser/cmux_term/cmux_views.cc"
grep -Fq 'const bool is_cmux_configure =' "$views_source"
grep -Fq '(url->host() == "settings" && (path.empty() || path == "/"))' \
  "$views_source"
if grep -Fq 'url->host() == "settings" || url->host() == "configure"' \
    "$views_source"; then
  echo "cmux settings subroutes must remain native chrome://settings pages" >&2
  exit 1
fi
for route in payments addresses clearBrowserData search importData defaultBrowser; do
  grep -Fq "cmux://settings/$route" \
    "$root/overlay/chrome/browser/cmux_term/cmux_toolbar_menus.cc" \
    "$root/overlay/chrome/browser/cmux_term/cmux_configure_page.cc"
done
toolbar_menus_source="$root/overlay/chrome/browser/cmux_term/cmux_toolbar_menus.cc"
grep -Fq 'if (bubble_widget_) {' "$toolbar_menus_source"
grep -Fq 'Do not replace its delegate until the matching' "$toolbar_menus_source"
grep -Fq 'if (!bubble_widget_) {' "$toolbar_menus_source"

python3 "$root/scripts/test-apply-build-gn.py"
python3 "$root/scripts/test-apply-ghostty-link.py"
python3 "$root/scripts/test-apply-mac-toolchain.py"
python3 "$root/scripts/test-apply-mv2-compat.py"
python3 "$root/scripts/test-apply-mv2-ui-compat.py"
python3 "$root/scripts/test-apply-postinstall-compat.py"
python3 "$root/scripts/test-apply-extension-menu-compat.py"
python3 "$root/scripts/test-extension-popup-toggle.py"
python3 "$root/scripts/test-extension-highlight-alignment.py"
python3 "$root/scripts/test-m151-browser-finder-compat.py"
python3 "$root/scripts/test-m151-command-updater-compat.py"
python3 "$root/scripts/test-custom-window-permissions.py"
python3 "$root/scripts/test-macos-product-identity.py"
python3 "$root/scripts/test-cmux-startup-mode.py"
bash "$root/scripts/test-build-dogfood.sh"

echo "build tests passed"
