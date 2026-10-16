#!/usr/bin/env python3
"""Regression test for Chromium 151's download bubble mode API."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (
    ROOT / "overlay/chrome/browser/cmux_term/cmux_toolbar_menus.cc"
).read_text()

assert '#include "chrome/common/chrome_version.h"' in SOURCE
assert "#if CHROME_VERSION_MAJOR >= 151" in SOURCE

call_start = SOURCE.index(
    "auto contents = std::make_unique<DownloadBubbleContentsView>("
)
call_end = SOURCE.index("delegate.get());", call_start)
call = SOURCE[call_start:call_end]

# M151 replaced the bool constructor parameter with DownloadBubbleMode.  A
# toolbar-button click requests the complete view and supplies GetMainView()
# models, matching Chromium's DownloadToolbarUIController::InvokeUI path.
assert "DownloadBubbleMode::kComplete" in call
assert "bubble_controller_->GetMainView()" in SOURCE[:call_start]

# Preserve the M149/M150 constructor call for rollback builds.
assert "#else" in call
assert "/*primary_view_is_partial_view=*/false" in call
assert "#endif" in call

print("download bubble M149/M150/M151 compatibility: PASS")
