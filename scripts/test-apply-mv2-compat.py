#!/usr/bin/env python3
"""Regression test for M149/M150/M151 MV2 manager patching on every host."""

import os
from pathlib import Path
import tempfile


ROOT = Path(__file__).resolve().parent.parent
APPLY = (ROOT / "scripts/apply.sh").read_text()
manager = APPLY.index("p_candidates = (\n    # Chromium 149: the manager")
START = APPLY.rindex("from pathlib import Path", 0, manager)
END = APPLY.index("\nPYEOF\"", START)
WIN_APPLY = (ROOT / "scripts/apply_win_chrome.py").read_text()
WIN_START = WIN_APPLY.index("p_candidates = (\n    # Chromium 149: the manager")
WIN_END = WIN_APPLY.index('\nprint("--- next patch ---")', WIN_START)
PATCHES = (
    ("apply.sh", APPLY[START:END]),
    ("apply_win_chrome.py", "from pathlib import Path\n" + WIN_APPLY[WIN_START:WIN_END]),
)

BODY_149 = '''bool ShouldDisableLegacyExtensions(MV2ExperimentStage stage) {
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

BODY_150 = '''bool ShouldDisableLegacyExtensions(MV2ExperimentStage stage) {
  if (g_allow_mv2_for_testing) {
    // We allow legacy MV2 extensions for testing purposes.
    return false;
  }

  return true;
}
'''

BODY_151 = '''bool ShouldDisableLegacyExtensions() {
  if (g_allow_mv2_for_testing) {
    // We allow legacy MV2 extensions for testing purposes.
    return false;
  }

  return true;
}
'''


def validate(patcher: str, patch: str, name: str, relative_path: str, body: str) -> None:
    with tempfile.TemporaryDirectory() as temp:
        checkout = Path(temp)
        path = checkout / relative_path
        path.parent.mkdir(parents=True)
        path.write_text(body)
        old_cwd = Path.cwd()
        try:
            os.chdir(checkout)
            exec(compile(patch, f"{patcher} MV2 patch", "exec"), {"os": os})
            exec(compile(patch, f"{patcher} MV2 patch rerun", "exec"), {"os": os})
        finally:
            os.chdir(old_cwd)
        patched = path.read_text()
        assert "Never let upstream MV2" in patched
        assert "return true;" not in patched
        print(f"PASS {patcher}: {name}")


for patcher, patch in PATCHES:
    validate(
        patcher,
        patch,
        "Chromium 149 MV2 manager",
        "chrome/browser/extensions/manifest_v2_experiment_manager.cc",
        BODY_149,
    )
    validate(
        patcher,
        patch,
        "Chromium 150 MV2 manager",
        "extensions/browser/manifest_v2_experiment_manager.cc",
        BODY_150,
    )
    validate(
        patcher,
        patch,
        "Chromium 151 MV2 handler",
        "extensions/browser/manifest_v2_handler.cc",
        BODY_151,
    )
