#!/usr/bin/env python3
"""Regression checks for the nested extension-strip highlight border."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
OVERLAY = ROOT / "overlay/chrome/browser/cmux_term"
STRIP_HEADER = (OVERLAY / "cmux_extension_strip.h").read_text()
STRIP = (OVERLAY / "cmux_extension_strip.cc").read_text()
CONTAINER_HEADER = (OVERLAY / "cmux_extensions_container.h").read_text()
CONTAINER = (OVERLAY / "cmux_extensions_container.cc").read_text()
EXTENSIONS = (OVERLAY / "cmux_extensions.cc").read_text()

# Chromium computes the inherited border in widget coordinates. cmux embeds
# that view below a pane compositor layer, so the subclass must correct it
# after the base callback and use the sibling view layer's parent-relative
# bounds.
assert "void OnBoundsChanged(const gfx::Rect& previous_bounds) override;" in STRIP_HEADER
override = STRIP.split("void CmuxExtensionStrip::OnBoundsChanged(", 1)[1].split(
    "\n}\n", 1
)[0]
assert "ToolbarIconContainerView::OnBoundsChanged(previous_bounds);" in override
assert "GetLayersInOrder(views::ViewLayer::kExclude)" in override
assert "associated_layers.front()->SetBounds(layer()->bounds());" in override
assert "ConvertRectToWidget" not in override

# The shipped extension self-test compares the actual border and strip layers
# after the real nested pane has been laid out.
assert "IsActionContainerHighlightAlignedForTesting" in CONTAINER_HEADER
assert "IsActionContainerHighlightAlignedForTesting" in CONTAINER
assert "associated_layers.front()->bounds() == container->layer()->bounds()" in CONTAINER
assert "PASS extension highlight border aligned" in EXTENSIONS
assert "FAIL extension highlight border misaligned" in EXTENSIONS

print("extension highlight border alignment: PASS")
