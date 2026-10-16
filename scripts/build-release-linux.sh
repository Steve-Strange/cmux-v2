#!/usr/bin/env bash
# Build and package the Linux x64 auto-update root on a warm build disk.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VERSION="${CMUX_RELEASE_VERSION:?CMUX_RELEASE_VERSION is required}"
CHANNEL="${CMUX_RELEASE_CHANNEL:?CMUX_RELEASE_CHANNEL is required}"
SRC="${CHROMIUM_SRC:-/opt/cmux/chromium/src}"
DEPOT_TOOLS="${CMUX_DEPOT_TOOLS:-$(dirname "$SRC")/depot_tools}"
OUT="${CMUX_RELEASE_OUTPUT:-$ROOT/release}"
SOURCE_SHA="${CMUX_RELEASE_SOURCE_SHA:-$(git -C "$ROOT" rev-parse HEAD)}"
PHASE="${CMUX_RELEASE_PHASE:-all}"
BUILD_ROOT="${CMUX_LINUX_BUILD_ROOT:-$(dirname "$(dirname "$SRC")")}"
COMPILE_RECEIPT="${CMUX_LINUX_COMPILE_RECEIPT:-$BUILD_ROOT/.cmux-linux-release-compile}"
COMPILE_KEY="linux-release-compile-v1:$SOURCE_SHA:$VERSION:$CHANNEL"
CMUX_TUI_REVISION="$(
  sed -n '/kPinnedCmuxTuiBuildCommit/{n;s/^    "\([0-9a-f]*\)";$/\1/p;}' \
    "$ROOT/overlay/chrome/browser/cmux_term/cmux_tui_revision.h"
)"
CMUX_TUI_STAGE="${CMUX_TUI_STAGE:-$BUILD_ROOT/cmux-tui-stage/$CMUX_TUI_REVISION}"

# Blacksmith skips sticky-disk commits after a failed workflow step. CI runs
# compile and package as separate jobs so a late packaging failure cannot
# discard an otherwise successful multi-hour Chromium compile.
case "$CHANNEL" in stable|nightly) ;; *) echo "invalid channel: $CHANNEL" >&2; exit 2 ;; esac
case "$PHASE" in
  all|compile|package) ;;
  *) echo "invalid Linux release phase: $PHASE" >&2; exit 2 ;;
esac
[ -d "$SRC/.git" ] || { echo "warm Chromium checkout missing: $SRC" >&2; exit 2; }
[ -x "$DEPOT_TOOLS/autoninja" ] || { echo "depot_tools missing: $DEPOT_TOOLS" >&2; exit 2; }

export CMUX_BUILDER=local
export CMUX_TAILSCALE_SSH="$ROOT/scripts/local-builder-transport.sh"
export RSYNC_RSH="$CMUX_TAILSCALE_SSH"
export CMUX_CHROMIUM_BASE="$(dirname "$SRC")"
export CMUX_DEPOT_TOOLS="$DEPOT_TOOLS"
export CHROMIUM_SRC="$SRC"
export PATH="$DEPOT_TOOLS:$PATH"
export IGNORE_DEPS_CHANGES=1

configure_release_tree() {
  # A runner is single-tenant while this job is active. Reset only Chromium's
  # tracked files; sync.sh owns cmux_term and preserves the build output cache.
  git -C "$SRC" restore --worktree -- .
  "$ROOT/scripts/sync.sh"
  CMUX_TARGET=chrome_linux "$ROOT/scripts/apply.sh"
  python3 "$ROOT/scripts/stamp-release-build.py" --chromium-src "$SRC"
  python3 "$ROOT/scripts/release_version.py" \
    --chromium-src "$SRC" --version "$VERSION"
}

assert_gn_arg() {
  local name="$1" expected="$2" actual
  actual="$(cd "$SRC" && gn args out/Release "--list=$name" --short)"
  [[ "$actual" == *"$name = $expected"* ]] || {
    echo "out/Release requires $name = $expected; got: $actual" >&2
    exit 2
  }
}
assert_build_config() {
  assert_gn_arg is_component_build false
  assert_gn_arg is_debug false
  assert_gn_arg is_official_build false
  assert_gn_arg symbol_level 1
  assert_gn_arg use_static_angle true
  assert_gn_arg use_debug_fission false
  assert_gn_arg target_cpu '"x64"'
}

credits="$SRC/out/Release/gen/components/resources/about_credits.html"
read_chromium_version() {
  awk -F= '
    $1 == "MAJOR" { major=$2 }
    $1 == "MINOR" { minor=$2 }
    $1 == "BUILD" { build=$2 }
    $1 == "PATCH" { patch=$2 }
    END { print major "." minor "." build "." patch }
  ' "$1"
}
verify_compiled_release() {
  local actual_version
  actual_version="$(read_chromium_version "$SRC/chrome/VERSION")"
  [ "$actual_version" = "$VERSION" ] || {
    echo "compiled Chromium version is $actual_version; expected $VERSION" >&2
    exit 1
  }
  test -x "$SRC/out/Release/chrome" || {
    echo "compiled Chromium executable is missing" >&2
    exit 1
  }
  test -s "$credits" || {
    echo "Chromium generated credits were not produced" >&2
    exit 1
  }
  if grep -Fq 'To get correct credits page' "$credits"; then
    echo "Chromium release was built with sample credits" >&2
    exit 1
  fi
}

deb="$SRC/out/Release/cmux-browser-stable_${VERSION}-1_amd64.deb"
build_debian_package() {
  local build_timestamp installer_version sysroot
  sysroot="$SRC/build/linux/debian_bullseye_amd64-sysroot"

  # Siso has repeatedly reported a successful Chromium Debian action without
  # materializing its declared .deb output on Blacksmith. Build every declared
  # prerequisite through Ninja, then run the same pinned Chromium builder
  # directly so its diagnostics and exact output stay local to this job.
  autoninja -C "$SRC/out/Release" \
    chrome/installer/linux:installer_deps \
    chrome/installer/linux:merge_deb_dependencies

  installer_version="$(
    read_chromium_version "$SRC/out/Release/installer/version.txt"
  )"
  [ "$installer_version" = "$VERSION" ] || {
    echo "Linux installer version is $installer_version; expected $VERSION" >&2
    exit 1
  }
  build_timestamp="$(
    python3 "$SRC/build/compute_build_timestamp.py" default
  )"
  [[ "$build_timestamp" =~ ^[0-9]+$ ]] || {
    echo "invalid Chromium build timestamp: $build_timestamp" >&2
    exit 1
  }
  [ -d "$sysroot" ] || {
    echo "Chromium Debian sysroot is missing: $sysroot" >&2
    exit 1
  }

  rm -f -- "$deb"
  (
    cd "$SRC/out/Release"
    VERBOSE=1 python3 ../../chrome/installer/linux/debian/build.py \
      -a amd64 \
      -b "$build_timestamp" \
      -c stable \
      -d chromium \
      -o . \
      -s "$sysroot" \
      -t linux \
      --xz-nthreads 0 \
      --use-static-angle=true
  )
  test -s "$deb" || {
    echo "cmux Browser stable Debian package was not produced: $deb" >&2
    exit 1
  }
}

case "$PHASE" in
  compile)
    configure_release_tree
    assert_build_config
    autoninja -C "$SRC/out/Release" chrome
    verify_compiled_release
    printf '%s\n' "$COMPILE_KEY" > "$COMPILE_RECEIPT"
    echo "Linux release compile cache ready: $VERSION"
    exit 0
    ;;
  package)
    [ "$(cat "$COMPILE_RECEIPT" 2>/dev/null || true)" = "$COMPILE_KEY" ] || {
      echo "Linux release compile receipt is missing or stale" >&2
      exit 1
    }
    assert_build_config
    verify_compiled_release
    build_debian_package
    ;;
  all)
    configure_release_tree
    assert_build_config
    autoninja -C "$SRC/out/Release" chrome
    verify_compiled_release
    build_debian_package
    ;;
esac

work="$(mktemp -d "${TMPDIR:-/tmp}/cmux-linux-release.XXXXXX")"
trap 'rm -rf "$work"' EXIT
dpkg-deb --raw-extract "$deb" "$work/extracted"
test -s "$work/extracted/DEBIAN/control" || {
  echo "packaged Debian control metadata was not extracted" >&2
  exit 1
}
binary="$(find "$work/extracted" -type f \( -name chrome -o -name chromium \) -perm -0100 -print | sort | tail -1)"
[ -n "$binary" ] || { echo "packaged Chromium executable not found" >&2; exit 1; }
package_root="$(dirname "$binary")"

# Install the exact cmux-tui helper and reviewed Ghostty terminfo beside the
# browser before deriving any of the three release formats. A fresh install
# cannot materialize its canonical workspace without this sibling helper.
CMUX_TUI_STAGE="$CMUX_TUI_STAGE" \
CMUX_TUI_TARGET=x86_64-unknown-linux-gnu \
CMUX_TUI_MANIFEST_DIR="$package_root" \
  "$ROOT/scripts/install-cmux-tui-artifact.sh" "$package_root"

# Carry the exact target notice set in both the Debian package and standalone
# update archive. Ghostty's license tree was collected from its isolated Zig
# dependency cache by bootstrap-release-linux.sh.
notice_root="$package_root/cmux-licenses"
license_input="$work/license-input"
mkdir -p "$notice_root/third_party/helium" \
  "$notice_root/third_party/bonsplit" \
  "$notice_root/third_party/chromium" \
  "$notice_root/third_party/ghostty" \
  "$license_input/ghostty"
cp "$ROOT/LICENSE" "$ROOT/THIRD_PARTY_NOTICES.md" "$notice_root/"
cp "$ROOT/LICENSE" "$license_input/cmux-GPL-3.0-or-later.txt"
cp "$ROOT/THIRD_PARTY_NOTICES.md" "$license_input/source-provenance.md"
cp "$ROOT/third_party/bonsplit/LICENSE" \
  "$notice_root/third_party/bonsplit/"
cp "$ROOT/third_party/chromium/LICENSE" \
  "$notice_root/third_party/chromium/"
cp "$ROOT/third_party/ghostty/LICENSE" \
  "$notice_root/third_party/ghostty/"
cp "$ROOT/third_party/helium/LICENSE" \
  "$ROOT/third_party/helium/README.chromium" \
  "$notice_root/third_party/helium/"
cp "$ROOT/third_party/bonsplit/LICENSE" "$license_input/Bonsplit-LICENSE.txt"
cp "$ROOT/third_party/chromium/LICENSE" "$license_input/Chromium-LICENSE.txt"
cp "$ROOT/third_party/ghostty/LICENSE" "$license_input/Ghostty-LICENSE.txt"
cp "$ROOT/third_party/helium/LICENSE" \
  "$license_input/Helium-GPL-3.0-only.txt"
cp "$credits" "$license_input/Chromium-about-credits.html"
cp -a "$SRC/third_party/cmux_ghostty/licenses/." "$license_input/ghostty/"

python3 "$ROOT/scripts/generate-linux-release-compliance.py" \
  --version "$VERSION" \
  --source-sha "$SOURCE_SHA" \
  --license-root "$license_input" \
  --output-html "$OUT/THIRD_PARTY_NOTICES.html" \
  --output-spdx "$OUT/third-party-notices.spdx.json"
cp "$credits" "$notice_root/Chromium-about-credits.html"
cp "$OUT/THIRD_PARTY_NOTICES.html" \
  "$OUT/third-party-notices.spdx.json" \
  "$notice_root/"
cp -a "$SRC/third_party/cmux_ghostty/licenses" \
  "$notice_root/third_party/ghostty/dependency-licenses"

rm -rf "$OUT/cmux-browser"
mkdir -p "$OUT"
cp -a "$package_root" "$OUT/cmux-browser"
if [ ! -e "$OUT/cmux-browser/chrome" ]; then
  ln -s "$(basename "$binary")" "$OUT/cmux-browser/chrome"
fi
if [ "$CHANNEL" = nightly ]; then
  printf '%s\n' \
    'https://github.com/manaflow-ai/cmux-v2/releases/download/nightly/update.json' \
    > "$OUT/cmux-browser/cmux-update-feed-url"
else
  rm -f "$OUT/cmux-browser/cmux-update-feed-url"
fi

# Preserve the selected channel in the Debian package as well. Rebuilding the
# package avoids publishing Chromium's pre-overlay archive for nightlies.
if [ "$CHANNEL" = nightly ]; then
  cp "$OUT/cmux-browser/cmux-update-feed-url" "$package_root/cmux-update-feed-url"
else
  rm -f "$package_root/cmux-update-feed-url"
fi
rm -f "$work/extracted/DEBIAN/md5sums"
dpkg-deb --build --root-owner-group "$work/extracted" "$OUT/cmux-linux-x64.deb" >/dev/null

python3 "$ROOT/scripts/build-update-archive.py" \
  --input "$OUT/cmux-browser" --output "$OUT/cmux-linux-x64.zip"
"$ROOT/scripts/build-linux-user-installer.sh"
test -x "$OUT/cmux-browser/chrome"
test -x "$OUT/cmux-browser/cmux-tui"
test -s "$OUT/cmux-browser/cmux-tui.REVISION"
test -s "$OUT/cmux-browser/cmux-tui.GHOSTTY_REVISION"
test -s "$OUT/cmux-browser/cmux-tui.GHOSTTY_RESOURCES_MANIFEST.sha256"
test -d "$OUT/cmux-browser/ghostty/themes"
test -d "$OUT/cmux-browser/ghostty/shell-integration"
UNEXPECTED_UNLICENSED_RESOURCE="$(
  find "$OUT/cmux-browser/ghostty/themes" \
       "$OUT/cmux-browser/ghostty/shell-integration" \
    -mindepth 1 -print -quit
)"
if [ -n "$UNEXPECTED_UNLICENSED_RESOURCE" ]; then
  echo "Linux release contains an unverified Ghostty theme or shell-integration resource: $UNEXPECTED_UNLICENSED_RESOURCE" >&2
  exit 1
fi
test -e "$OUT/cmux-browser/terminfo/x/xterm-ghostty"
test -f "$OUT/cmux-browser/cmux-licenses/THIRD_PARTY_NOTICES.md"
test -f "$OUT/cmux-browser/cmux-licenses/THIRD_PARTY_NOTICES.html"
test -f "$OUT/cmux-browser/cmux-licenses/third-party-notices.spdx.json"
test -f "$OUT/cmux-browser/cmux-licenses/Chromium-about-credits.html"
test -f "$OUT/cmux-browser/cmux-licenses/third_party/helium/LICENSE"
test -f "$OUT/THIRD_PARTY_NOTICES.html"
test -f "$OUT/third-party-notices.spdx.json"
test -x "$OUT/cmux-linux-x64-installer.run"
echo "Linux release artifacts:"
echo "  per-user installer: $OUT/cmux-linux-x64-installer.run"
echo "  managed package: $OUT/cmux-linux-x64.deb"
echo "  in-app update: $OUT/cmux-linux-x64.zip"
