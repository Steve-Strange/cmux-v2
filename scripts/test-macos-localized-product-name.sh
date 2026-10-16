#!/bin/bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
REWRITER="$ROOT/scripts/rewrite-macos-localized-product-name.sh"
FIXTURE_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/cmux-localized-name-test.XXXXXX")"
trap 'rm -rf "$FIXTURE_ROOT"' EXIT

STRINGS="$FIXTURE_ROOT/Resources/en.lproj/InfoPlist.strings"
mkdir -p "$(dirname "$STRINGS")"
plutil -create xml1 "$STRINGS"
plutil -insert CFBundleGetInfoString -string \
  "Chromium 151.0, Copyright 2026 The Chromium Authors. All rights reserved." \
  "$STRINGS"
plutil -insert NSCameraUsageDescription -string \
  "Chromium would like to use your camera." "$STRINGS"
plutil -insert NSMicrophoneUsageDescription -string \
  "Chromium needs your microphone for browser calls." "$STRINGS"
plutil -insert "Chromium Shortcut" -string "Chromium Shortcut" "$STRINGS"
plutil -insert NSLocalNetworkUsageDescription -string \
  "This will allow you to select from available devices and display content on them." \
  "$STRINGS"

HI_STRINGS="$FIXTURE_ROOT/Resources/hi.lproj/InfoPlist.strings"
mkdir -p "$(dirname "$HI_STRINGS")"
plutil -create xml1 "$HI_STRINGS"
plutil -insert CFBundleGetInfoString -string \
  "Chromium 151.0, कॉपीराइट 2026 The Chromium Authors. सर्वाधिकार सुरक्षित." \
  "$HI_STRINGS"
plutil -insert "Chromium Shortcut" -string \
  "Chromium का शॉर्टकट" "$HI_STRINGS"
plutil -insert NSCameraUsageDescription -string \
  "क्रोमियम को ऐक्सेस मिलने के बाद, वेबसाइटें आपसे ऐक्सेस मांग सकेंगी." \
  "$HI_STRINGS"
plutil -insert NSLocalNetworkUsageDescription -string \
  "इससे, आपको उपलब्ध डिवाइसों पर कॉन्टेंट दिखाने का विकल्प मिलेगा." \
  "$HI_STRINGS"

PSEUDO_STRINGS="$FIXTURE_ROOT/Resources/en_XA.lproj/InfoPlist.strings"
mkdir -p "$(dirname "$PSEUDO_STRINGS")"
plutil -create xml1 "$PSEUDO_STRINGS"
plutil -insert CFBundleGetInfoString -string \
  "Chromium 151.0, Copyright 2026 The Chromium Authors." \
  "$PSEUDO_STRINGS"
plutil -insert "Chromium Shortcut" -string \
  "Çĥrömîûm Šĥörţçûţ" "$PSEUDO_STRINGS"
plutil -insert NSCameraUsageDescription -string \
  "Öñçé Çĥrömîûm ĥåš åççéšš." "$PSEUDO_STRINGS"

HR_STRINGS="$FIXTURE_ROOT/Resources/hr.lproj/InfoPlist.strings"
mkdir -p "$(dirname "$HR_STRINGS")"
plutil -create xml1 "$HR_STRINGS"
plutil -insert CFBundleGetInfoString -string \
  "Chromium 151.0, Autorska prava 2026. Autori Chromiuma." \
  "$HR_STRINGS"
plutil -insert "Chromium Shortcut" -string \
  "Prečac za Chrome" "$HR_STRINGS"
plutil -insert NSCameraUsageDescription -string \
  "Kada Chromium dobije pristup, web-lokacije će vas moći tražiti pristup." \
  "$HR_STRINGS"
plutil -insert NSLocalNetworkUsageDescription -string \
  "To će vam omogućiti odabir između dostupnih uređaja i prikaz sadržaja na njima." \
  "$HR_STRINGS"

bash "$REWRITER" "$FIXTURE_ROOT/Resources"

[ "$(
  plutil -extract CFBundleGetInfoString raw -o - "$STRINGS"
)" = "cmux 151.0, Copyright 2026 The Chromium Authors. All rights reserved." ]
[ "$(
  plutil -extract NSCameraUsageDescription raw -o - "$STRINGS"
)" = "cmux would like to use your camera." ]
[ "$(
  plutil -extract NSMicrophoneUsageDescription raw -o - "$STRINGS"
)" = "cmux needs your microphone for browser calls." ]
[ "$(
  plutil -extract "Chromium Shortcut" raw -o - "$STRINGS"
)" = "cmux Shortcut" ]
[ "$(
  plutil -extract NSLocalNetworkUsageDescription raw -o - "$STRINGS"
)" = "This will allow you to select from available devices and display content on them." ]

for localized_strings in "$HI_STRINGS" "$PSEUDO_STRINGS"; do
  [[ "$(
    plutil -extract CFBundleGetInfoString raw -o - "$localized_strings"
  )" == "cmux 151.0,"* ]]
done
[ "$(
  plutil -extract "Chromium Shortcut" raw -o - "$HI_STRINGS"
)" = "cmux का शॉर्टकट" ]
[ "$(
  plutil -extract "Chromium Shortcut" raw -o - "$PSEUDO_STRINGS"
)" = "cmux Šĥörţçûţ" ]
[ "$(
  plutil -extract NSCameraUsageDescription raw -o - "$HI_STRINGS"
)" = "cmux को ऐक्सेस मिलने के बाद, वेबसाइटें आपसे ऐक्सेस मांग सकेंगी." ]
[ "$(
  plutil -extract NSCameraUsageDescription raw -o - "$PSEUDO_STRINGS"
)" = "Öñçé cmux ĥåš åççéšš." ]
[ "$(
  plutil -extract NSLocalNetworkUsageDescription raw -o - "$HI_STRINGS"
)" = "इससे, आपको उपलब्ध डिवाइसों पर कॉन्टेंट दिखाने का विकल्प मिलेगा." ]
[ "$(
  plutil -extract "Chromium Shortcut" raw -o - "$HR_STRINGS"
)" = "Prečac za cmux" ]
[ "$(
  plutil -extract NSCameraUsageDescription raw -o - "$HR_STRINGS"
)" = "Kada cmux dobije pristup, web-lokacije će vas moći tražiti pristup." ]
[ "$(
  plutil -extract NSLocalNetworkUsageDescription raw -o - "$HR_STRINGS"
)" = "To će vam omogućiti odabir između dostupnih uređaja i prikaz sadržaja na njima." ]

# A second pass must be a byte-for-byte no-op. This catches accidental changes
# to Chromium's authorship attribution or to Cast's localized reason text.
BEFORE_HASH="$(
  find "$FIXTURE_ROOT/Resources" -name InfoPlist.strings -type f \
    -exec shasum -a 256 {} + | sort | shasum -a 256 | awk '{print $1}'
)"
bash "$REWRITER" "$FIXTURE_ROOT/Resources"
AFTER_HASH="$(
  find "$FIXTURE_ROOT/Resources" -name InfoPlist.strings -type f \
    -exec shasum -a 256 {} + | sort | shasum -a 256 | awk '{print $1}'
)"
[ "$BEFORE_HASH" = "$AFTER_HASH" ]

echo "macOS localized product-name tests passed"
