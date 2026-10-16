#!/bin/bash
# Exercise the GNU ELF + lld weak-COMDAT collision that Ghostty isolation must
# prevent. The old localization policy intentionally produces a broken control
# executable; the namespaced policy must link and run in either object order.
set -euo pipefail

if [ "$(uname -s)" != Linux ]; then
  echo "Ghostty Linux symbol-isolation test skipped on non-Linux host"
  exit 0
fi

for tool in c++ ld objcopy nm readelf; do
  command -v "$tool" >/dev/null || {
    echo "error: $tool is required" >&2
    exit 1
  }
done
objcopy --version | head -1 | grep -Fq 'GNU objcopy'

work="$(mktemp -d "${TMPDIR:-/tmp}/cmux-ghostty-symbol-test.XXXXXX")"
cleanup() {
  rm -rf "$work"
}
trap cleanup EXIT
ulimit -c 0

cat > "$work/shared.h" <<'EOF'
template <class T>
__attribute__((noinline)) int shared_template(T value) {
  return value + 1;
}
EOF
cat > "$work/ghostty.cc" <<'EOF'
#include "shared.h"

extern "C" int external_provider(int);
extern "C" int weak_provider(int) __attribute__((weak));

int private_data = 7;
thread_local int private_tls = 1;

extern "C" int ghostty_fixture(int value) {
  const int weak = weak_provider ? weak_provider(value) : 0;
  return shared_template(value) + external_provider(value) + private_data +
         private_tls + weak;
}
EOF
cat > "$work/chromium.cc" <<'EOF'
#include "shared.h"

extern "C" int chromium_fixture(int value) {
  return shared_template(value);
}
EOF
cat > "$work/provider.cc" <<'EOF'
extern "C" int external_provider(int) {
  return 2;
}
extern "C" int weak_provider(int) {
  return 3;
}
EOF
cat > "$work/main.cc" <<'EOF'
extern "C" int chromium_fixture(int);
extern "C" int ghostty_fixture(int);

int main() {
  return ghostty_fixture(1) + chromium_fixture(1) == 17 ? 0 : 2;
}
EOF

cxx="${CXX:-c++}"
common_flags=(-std=c++17 -O2 -ffunction-sections -fdata-sections)
"$cxx" "${common_flags[@]}" -c "$work/ghostty.cc" -o "$work/ghostty.o"
"$cxx" "${common_flags[@]}" -c "$work/chromium.cc" -o "$work/chromium.o"
"$cxx" "${common_flags[@]}" -c "$work/provider.cc" -o "$work/provider.o"
"$cxx" "${common_flags[@]}" -c "$work/main.cc" -o "$work/main.o"
ld -r "$work/ghostty.o" -o "$work/ghostty-merged.o"

# Prove the fixture is sensitive to the production bug: localizing a duplicate
# weak COMDAT group makes one of the two final-link callers target address zero.
# Build the list from defined symbols so unresolved providers remain global;
# this isolates the COMDAT failure from any undefined-symbol transformation.
nm -g --defined-only -P "$work/ghostty-merged.o" |
  awk '$1 !~ /^ghostty_/ { print $1 }' |
  LC_ALL=C sort -u > "$work/old-localize-symbols.txt"
objcopy --localize-symbols="$work/old-localize-symbols.txt" \
  "$work/ghostty-merged.o" "$work/ghostty-old.o"
nm -g --undefined-only -P "$work/ghostty-old.o" |
  LC_ALL=C sort -u > "$work/old-undefined.after"
nm -g --undefined-only -P "$work/ghostty-merged.o" |
  LC_ALL=C sort -u > "$work/old-undefined.before"
cmp "$work/old-undefined.before" "$work/old-undefined.after"
readelf --section-groups "$work/ghostty-old.o" |
  grep -Eq '\[.*shared_template'
"$cxx" -fuse-ld=lld -no-pie \
  "$work/main.o" "$work/ghostty-old.o" "$work/chromium.o" "$work/provider.o" \
  -o "$work/old-app"
old_status=0
( "$work/old-app" ) >/dev/null 2>&1 || old_status=$?
if [ "$old_status" -ne 139 ]; then
  echo "error: weak-COMDAT control returned $old_status instead of SIGSEGV" >&2
  exit 1
fi

symbol_prefix=cmux_ghostty_
nm -g --undefined-only -P "$work/ghostty-merged.o" |
  LC_ALL=C sort -u > "$work/undefined.before"
nm -g --defined-only -P "$work/ghostty-merged.o" |
  awk '$1 ~ /^ghostty_/ { print $1 }' |
  LC_ALL=C sort -u > "$work/public.before"
{
  awk '{ print $1 }' "$work/undefined.before"
  cat "$work/public.before"
} | LC_ALL=C sort -u > "$work/restore-names.txt"
awk -v prefix="$symbol_prefix" '{ print prefix $1, $1 }' \
  "$work/restore-names.txt" > "$work/restore-symbols.txt"
objcopy --prefix-symbols="$symbol_prefix" \
  "$work/ghostty-merged.o" "$work/ghostty-prefixed.o"
objcopy --redefine-syms="$work/restore-symbols.txt" \
  "$work/ghostty-prefixed.o" "$work/ghostty-isolated.o"

nm -g --undefined-only -P "$work/ghostty-isolated.o" |
  LC_ALL=C sort -u > "$work/undefined.after"
cmp "$work/undefined.before" "$work/undefined.after"
nm -g --defined-only -P "$work/ghostty-isolated.o" |
  awk '$1 ~ /^ghostty_/ { print $1 }' |
  LC_ALL=C sort -u > "$work/public.after"
cmp "$work/public.before" "$work/public.after"
nm -g --defined-only -P "$work/ghostty-isolated.o" |
  grep -Eq '^cmux_ghostty_'
readelf --section-groups "$work/ghostty-isolated.o" |
  grep -Eq '\[cmux_ghostty_.*shared_template'

for order in chromium-first ghostty-first; do
  if [ "$order" = chromium-first ]; then
    objects=("$work/chromium.o" "$work/ghostty-isolated.o")
  else
    objects=("$work/ghostty-isolated.o" "$work/chromium.o")
  fi
  "$cxx" -fuse-ld=lld -no-pie \
    "$work/main.o" "${objects[@]}" "$work/provider.o" \
    -o "$work/$order"
  "$work/$order"
done

echo "Ghostty Linux symbol-isolation tests passed"
