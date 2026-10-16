#!/usr/bin/env bash
# Isolate and stage a locally built Linux libghostty archive for Chromium.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
EXPECTED_REVISION="$(tr -d '[:space:]' < "$ROOT/ghostty-revision.txt")"
GHOSTTY_SRC="${GHOSTTY_SRC:?GHOSTTY_SRC is required}"
ZIG_GLOBAL_CACHE_DIR="${ZIG_GLOBAL_CACHE_DIR:?ZIG_GLOBAL_CACHE_DIR is required}"
SRC="${CHROMIUM_SRC:?CHROMIUM_SRC is required}"
ZIG="${ZIG:-zig}"
LIB="${GHOSTTY_LIB:-$GHOSTTY_SRC/zig-out/lib/ghostty-internal.a}"
HEADER="${GHOSTTY_HEADER:-$GHOSTTY_SRC/include/ghostty.h}"
DEST="$SRC/third_party/cmux_ghostty"

actual_revision="$(git -C "$GHOSTTY_SRC" rev-parse HEAD)"
if [ "$actual_revision" != "$EXPECTED_REVISION" ]; then
  echo "error: Ghostty checkout is $actual_revision; expected $EXPECTED_REVISION" >&2
  exit 1
fi
case "$(git -C "$GHOSTTY_SRC" remote get-url origin)" in
  https://github.com/manaflow-ai/ghostty.git|git@github.com:manaflow-ai/ghostty.git) ;;
  *) echo "error: Ghostty origin must be manaflow-ai/ghostty" >&2; exit 1 ;;
esac
test -s "$LIB"
test -s "$HEADER"
case "$DEST" in
  "$SRC"/third_party/cmux_ghostty) ;;
  *) echo "error: unsafe Ghostty staging destination: $DEST" >&2; exit 2 ;;
esac

work="$(mktemp -d "${TMPDIR:-/tmp}/cmux-ghostty-linux.XXXXXX")"
stage="$SRC/third_party/.cmux_ghostty.$$.new"
cleanup() {
  rm -rf "$work" "$stage"
}
trap cleanup EXIT

discover_cxx() {
  local source="$work/discover-cxx.cpp"
  printf '%s\n' \
    '#include <string>' \
    'extern "C" const char* cmux_zig_cxx_probe() {' \
    '  static std::string value = std::to_string(1);' \
    '  return value.c_str();' \
    '}' > "$source"
  "$ZIG" build-lib -dynamic -target x86_64-linux -lc++ -lc -fPIC \
    "$source" -femit-bin="$work/discover-cxx.so" --verbose-link 2>&1 |
    tr ' ' '\n' |
    grep -E '/libc\+\+\.a$|/libc\+\+abi\.a$|/libunwind\.a$' |
    sort -u
}

mapfile -t cxx_libraries < <(discover_cxx)
if [ "${#cxx_libraries[@]}" -lt 3 ]; then
  echo "error: could not resolve Zig libc++, libc++abi, and libunwind archives" >&2
  exit 1
fi
for library in "${cxx_libraries[@]}"; do
  test -s "$library"
done

cp "$LIB" "$work/ghostty-raw.a"
cat > "$work/ghostty-shim.c" <<'SHIM'
#define _GNU_SOURCE
#include <unistd.h>
#include <sys/syscall.h>
#include <stddef.h>
#include <sys/types.h>
ssize_t copy_file_range(int fd_in, off_t *off_in, int fd_out, off_t *off_out,
                        size_t len, unsigned int flags) {
  return syscall(SYS_copy_file_range, fd_in, off_in, fd_out, off_out, len,
                 flags);
}
SHIM
cc -fPIC -c "$work/ghostty-shim.c" -o "$work/ghostty-shim.o"

ld -r --whole-archive "$work/ghostty-raw.a" "${cxx_libraries[@]}" \
  --no-whole-archive "$work/ghostty-shim.o" -z noexecstack \
  -o "$work/ghostty-merged.o"

# Give every bundled symbol a private namespace, then restore only unresolved
# externals and the ghostty_* C API. Merely localizing bundled definitions is
# unsafe for weak COMDAT templates: when Chromium links another HarfBuzz or
# FreeType instantiation with the same group signature first, lld can discard
# the localized group and strand its local callers at ELF address zero.
symbol_prefix=cmux_ghostty_
nm -g --undefined-only -P "$work/ghostty-merged.o" |
  LC_ALL=C sort -u > "$work/ghostty-undefined.before"
nm -g --defined-only -P "$work/ghostty-merged.o" |
  awk '$1 ~ /^ghostty_/ { print $1 }' |
  LC_ALL=C sort -u > "$work/ghostty-public.before"
if [ ! -s "$work/ghostty-undefined.before" ] ||
   [ ! -s "$work/ghostty-public.before" ]; then
  echo "error: Ghostty symbol-isolation inputs are unexpectedly empty" >&2
  exit 1
fi
{
  awk '{ print $1 }' "$work/ghostty-undefined.before"
  cat "$work/ghostty-public.before"
} | LC_ALL=C sort -u > "$work/ghostty-restore-names.txt"
unsafe_restore_names="$(
  LC_ALL=C grep -Ev '^[[:alnum:]_.$@?]+$' \
    "$work/ghostty-restore-names.txt" || true
)"
if [ -n "$unsafe_restore_names" ]; then
  echo "error: Ghostty symbols cannot be encoded safely for objcopy" >&2
  printf '%s\n' "$unsafe_restore_names" >&2
  exit 1
fi
awk -v prefix="$symbol_prefix" '{ print prefix $1, $1 }' \
  "$work/ghostty-restore-names.txt" > "$work/ghostty-restore-symbols.txt"
objcopy --prefix-symbols="$symbol_prefix" \
  "$work/ghostty-merged.o" "$work/ghostty-prefixed.o"
objcopy --redefine-syms="$work/ghostty-restore-symbols.txt" \
  "$work/ghostty-prefixed.o" "$work/ghostty-local.o"
nm -g --undefined-only -P "$work/ghostty-local.o" |
  LC_ALL=C sort -u > "$work/ghostty-undefined.after"
if ! cmp -s "$work/ghostty-undefined.before" \
             "$work/ghostty-undefined.after"; then
  echo "error: Ghostty localization changed unresolved external symbols" >&2
  diff -u "$work/ghostty-undefined.before" \
          "$work/ghostty-undefined.after" >&2 || true
  exit 1
fi
nm -g --defined-only -P "$work/ghostty-local.o" |
  awk '$1 ~ /^ghostty_/ { print $1 }' |
  LC_ALL=C sort -u > "$work/ghostty-public.after"
if ! cmp -s "$work/ghostty-public.before" "$work/ghostty-public.after"; then
  echo "error: Ghostty localization changed its public C API" >&2
  diff -u "$work/ghostty-public.before" \
          "$work/ghostty-public.after" >&2 || true
  exit 1
fi
unisolated_definitions="$(
  nm -g --defined-only -P "$work/ghostty-local.o" |
    awk '$1 !~ /^ghostty_/ && $1 !~ /^cmux_ghostty_/ { print $1 }'
)"
if [ -n "$unisolated_definitions" ]; then
  echo "error: Ghostty archive still exposes unprefixed private definitions" >&2
  printf '%s\n' "$unisolated_definitions" >&2
  exit 1
fi
private_symbols="$(
  nm -g --defined-only -P "$work/ghostty-local.o" |
    awk '$1 ~ /^cmux_ghostty_/ { count++ } END { print count + 0 }'
)"
if [ "$private_symbols" -lt 1 ]; then
  echo "error: Ghostty archive has no namespaced private definitions" >&2
  exit 1
fi
objcopy --add-section .note.GNU-stack=/dev/null \
  --set-section-flags .note.GNU-stack=readonly \
  "$work/ghostty-local.o" "$work/ghostty-local.o" 2>/dev/null || true
ar rcs "$work/ghostty-internal.a" "$work/ghostty-local.o"

public_symbols="$(
  nm -g --defined-only "$work/ghostty-internal.a" 2>/dev/null |
    grep -Ec ' [A-Z] ghostty_' || true
)"
if [ "$public_symbols" -lt 1 ]; then
  echo "error: localized Ghostty archive has no public ghostty_* symbols" >&2
  exit 1
fi

mkdir -p "$stage/lib" "$stage/include"
cp "$work/ghostty-internal.a" "$stage/lib/"
cp "$HEADER" "$stage/include/ghostty.h"
printf '%s\n' "$EXPECTED_REVISION" > "$stage/REVISION"
printf '%s\n' "$("$ZIG" version)" > "$stage/ZIG_VERSION"
python3 "$ROOT/scripts/collect-ghostty-licenses.py" \
  --ghostty-source "$GHOSTTY_SRC" \
  --zig-cache "$ZIG_GLOBAL_CACHE_DIR" \
  --output "$stage/licenses" \
  --revision "$EXPECTED_REVISION"

rm -rf "$DEST"
mv "$stage" "$DEST"
echo "vendored Linux libghostty: revision=$EXPECTED_REVISION symbols=$public_symbols"
