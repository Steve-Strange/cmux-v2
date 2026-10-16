#!/bin/bash
# Rewrite Chromium's localized product-facing strings after packaging.
set -euo pipefail

RESOURCES_DIR="${1:-}"
if [ -z "$RESOURCES_DIR" ] || [ ! -d "$RESOURCES_DIR" ]; then
  echo "usage: $0 <app Resources directory>" >&2
  exit 2
fi

replace_product_token() {
  local value="$1"
  local localized_product="$2"
  # Keep the helper source out of stdin. Bash 5.3 on macOS can deadlock while
  # preparing a heredoc inside the surrounding command substitution.
  python3 -c '
import re
import sys
import unicodedata

value, product = sys.argv[1:]
if "cmux" in value:
    sys.stdout.write(value)
    raise SystemExit(0)

# Chromium locale data is not consistent about which upstream brand form it
# embeds. Most values use the Chromium token from the About string. Some
# shortcuts use Chrome/Google Chrome, and Hindi privacy strings transliterate
# Chromium even though the Hindi About string retains ASCII "Chromium".
aliases = [
    alias
    for alias in (product, "Google Chrome", "Chromium", "Chrome", "क्रोमियम")
    if alias
]
aliases = sorted(set(aliases), key=len, reverse=True)
for alias in aliases:
    if alias in value:
        sys.stdout.write(value.replace(alias, "cmux", 1))
        raise SystemExit(0)


def folded_token(text):
    decomposed = unicodedata.normalize("NFKD", text).casefold()
    return "".join(
        character
        for character in decomposed
        if character.isalnum() and not unicodedata.combining(character)
    )


targets = {folded_token(alias) for alias in aliases}
targets.discard("")
if targets:
    for match in re.finditer(r"\S+", value):
        if folded_token(match.group(0)) in targets:
            sys.stdout.write(
                value[: match.start()] + "cmux" + value[match.end() :]
            )
            raise SystemExit(0)

print(
    f"cannot locate localized or Unicode-equivalent product {product!r}",
    file=sys.stderr,
)
raise SystemExit(1)
' "$value" "$localized_product"
}

rewrite_strings_file() {
  local strings_file="$1"
  local key value rewritten
  local localized_product=""
  local converted=0
  for key in \
      CFBundleGetInfoString \
      "Chromium Shortcut" \
      NSAudioCaptureUsageDescription \
      NSBluetoothAlwaysUsageDescription \
      NSBluetoothPeripheralUsageDescription \
      NSCameraUsageDescription \
      NSLocationUsageDescription \
      NSMicrophoneUsageDescription \
      NSWebBrowserPublicKeyCredentialUsageDescription; do
    value="$(plutil -extract "$key" raw -o - "$strings_file" 2>/dev/null || true)"
    [ -n "$value" ] || continue

    case "$key" in
      CFBundleGetInfoString)
        # The value also credits "The Chromium Authors". Replace only its
        # leading localized product. The dotted Chromium version is a
        # language-independent boundary and gives us the exact localized token
        # to replace in the capability-specific descriptions below.
        if [[ "$value" =~ ^(.+)[[:space:]]([0-9]+\.[0-9].*)$ ]]; then
          localized_product="${BASH_REMATCH[1]}"
          rewritten="cmux ${BASH_REMATCH[2]}"
        else
          echo "error: cannot locate the product/version boundary in $strings_file:$key" >&2
          exit 1
        fi
        ;;
      "Chromium Shortcut")
        # Info.plist references this lookup key, so retain the key and replace
        # only the displayed product token. Preserve the localized shortcut
        # suffix rather than replacing the complete value with English.
        if [ -z "$localized_product" ]; then
          echo "error: cannot brand $strings_file:$key without a localized product token" >&2
          exit 1
        fi
        if ! rewritten="$(
          replace_product_token "$value" "$localized_product"
        )"; then
          echo "error: cannot replace the product in $strings_file:$key" >&2
          exit 1
        fi
        ;;
      *)
        # Most translations receive PRODUCT_FULLNAME=cmux at build time. Some
        # bake in a localized/transliterated product. Replace that exact token
        # or its pseudo-localized Unicode equivalent while preserving the
        # language and capability-specific explanation.
        if [ -z "$localized_product" ]; then
          echo "error: cannot brand $strings_file:$key without a localized product token" >&2
          exit 1
        fi
        if ! rewritten="$(
          replace_product_token "$value" "$localized_product"
        )"; then
          echo "error: cannot replace the product in $strings_file:$key" >&2
          exit 1
        fi
        ;;
    esac

    if [ "$rewritten" != "$value" ]; then
      if [ "$converted" = "0" ]; then
        # plutil reads Chromium's OpenStep-format strings but cannot edit that
        # representation in place. Binary plists are native InfoPlist.strings.
        plutil -convert binary1 "$strings_file"
        converted=1
      fi
      plutil -replace "$key" -string "$rewritten" "$strings_file"
    fi
  done
}

list_strings_files() {
  if command -v rg >/dev/null 2>&1; then
    rg --files "$RESOURCES_DIR" -g 'InfoPlist.strings'
  else
    find "$RESOURCES_DIR" -path '*/InfoPlist.strings' -type f -print
  fi
}

while IFS= read -r strings_file; do
  rewrite_strings_file "$strings_file"
done < <(list_strings_files)
