#!/bin/bash
# Vendor the prebuilt LINUX libghostty static lib into the Chromium tree on the
# Linux builder, with symbol isolation so libghostty's bundled
# freetype/harfbuzz/simdutf/zig-std do NOT collide with chrome's own copies at
# the final link. ELF equivalent of the macOS apply.sh step 0 (ld -r +
# exported_symbols_list). The .a is prebuilt from the fork via:
#   cd ~/fun/ghostty && zig build -Dtarget=x86_64-linux -Dapp-runtime=none
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
EXPECTED_GHOSTTY_REVISION="$(tr -d '[:space:]' < "$ROOT/ghostty-revision.txt")"
GHOSTTY_SRC="${GHOSTTY_SRC:-$HOME/fun/ghostty-wt-cursor-visual-api}"
HOST="${CMUX_BUILDER:-cmux-linux-chrome}"
SRC="${CHROMIUM_SRC:-chromium/src}"
LIB="${GHOSTTY_LIB:-$GHOSTTY_SRC/zig-out/lib/ghostty-internal.a}"
HDR="${GHOSTTY_HEADER:-$GHOSTTY_SRC/include/ghostty.h}"

ACTUAL_GHOSTTY_REVISION="$(git -C "$GHOSTTY_SRC" rev-parse HEAD)"
if [ "$ACTUAL_GHOSTTY_REVISION" != "$EXPECTED_GHOSTTY_REVISION" ]; then
  echo "ERROR: Ghostty checkout is $ACTUAL_GHOSTTY_REVISION; browser pin is $EXPECTED_GHOSTTY_REVISION" >&2
  exit 1
fi
if [ -n "$(git -C "$GHOSTTY_SRC" status --porcelain --untracked-files=normal)" ]; then
  echo "ERROR: GHOSTTY_SRC must be clean so the revision identifies the artifact exactly" >&2
  exit 1
fi
case "$(git -C "$GHOSTTY_SRC" remote get-url origin)" in
  https://github.com/manaflow-ai/ghostty.git|git@github.com:manaflow-ai/ghostty.git) ;;
  *) echo "ERROR: GHOSTTY_SRC origin must be manaflow-ai/ghostty" >&2; exit 1 ;;
esac

# Zig's own x86_64-linux libc++/libc++abi/libunwind static archives, ABI-matched
# to how libghostty's C++ deps were compiled. chrome's libc++ is header-hidden
# (exports almost nothing), so libghostty must bundle its OWN libc++; these get
# merged + localized into the archive. Discover the exact paths with:
#   zig build-lib -dynamic -target x86_64-linux -lc++ -lc x.cpp --verbose-link
ZIG="${ZIG:-/opt/homebrew/Cellar/zig/0.15.2_1/bin/zig}"
discover_cxx() {
  local tmp; tmp=$(mktemp -d)
  printf '#include <string>\nextern "C" const char* z(){static std::string s=std::to_string(1);return s.c_str();}\n' > "$tmp/z.cpp"
  "$ZIG" build-lib -dynamic -target x86_64-linux -lc++ -lc -fPIC "$tmp/z.cpp" \
    -femit-bin="$tmp/z.so" --verbose-link 2>&1 | tr ' ' '\n' \
    | grep -E "/libc\+\+\.a$|/libc\+\+abi\.a$|/libunwind\.a$" | sort -u
}
CXX_LIBS="$(discover_cxx)"
echo "bundling Zig libc++ archives:"; echo "$CXX_LIBS"

echo "build-ghostty-linux: staging $LIB -> $HOST:$SRC/third_party/cmux_ghostty"
scp -o BatchMode=yes "$LIB" "$HOST:/tmp/ghostty-raw.a"
scp -o BatchMode=yes "$HDR" "$HOST:/tmp/ghostty.h"
scp -o BatchMode=yes "$ROOT/ghostty-revision.txt" "$HOST:/tmp/ghostty-revision.txt"
i=0
for cxx in $CXX_LIBS; do
  scp -o BatchMode=yes "$cxx" "$HOST:/tmp/ghostty-cxx-$i.a"
  i=$((i+1))
done
echo "staged $i C++ runtime archives"

ssh -o BatchMode=yes "$HOST" "set -e; cd '$SRC'
  mkdir -p third_party/cmux_ghostty/lib third_party/cmux_ghostty/include
  cp /tmp/ghostty.h third_party/cmux_ghostty/include/ghostty.h
  cp /tmp/ghostty-revision.txt third_party/cmux_ghostty/REVISION
  cd third_party/cmux_ghostty/lib
  # Shim for copy_file_range: zig std references the libc symbol, but chrome's
  # bullseye sysroot libc doesn't satisfy it at link (-z defs). Provide it via
  # the raw syscall; it gets localized into the archive like everything else.
  cat > /tmp/ghostty-shim.c <<'SHIM'
#define _GNU_SOURCE
#include <unistd.h>
#include <sys/syscall.h>
#include <stddef.h>
#include <sys/types.h>
ssize_t copy_file_range(int fd_in, off_t *off_in, int fd_out, off_t *off_out,
                        size_t len, unsigned int flags) {
  return syscall(SYS_copy_file_range, fd_in, off_in, fd_out, off_out, len, flags);
}
SHIM
  cc -fPIC -c /tmp/ghostty-shim.c -o /tmp/ghostty-shim.o
  # Merge all archive members (libghostty + its bundled Zig libc++/abi/unwind +
  # the shim) into one relocatable object. -z noexecstack marks the result as NOT
  # requiring an executable stack (Zig objects often omit the .note.GNU-stack
  # note, which makes chrome's -z noexecstack link reject it).
  ld -r --whole-archive /tmp/ghostty-raw.a /tmp/ghostty-cxx-*.a --no-whole-archive \
    /tmp/ghostty-shim.o -z noexecstack -o /tmp/ghostty-merged.o
  # Namespace every bundled symbol, then restore unresolved externals and the
  # ghostty_* API. Local weak COMDAT groups can collide with Chromium's copies
  # and leave bundled callers targeting ELF address zero.
  symbol_prefix=cmux_ghostty_
  nm -g --undefined-only -P /tmp/ghostty-merged.o |
    LC_ALL=C sort -u > /tmp/ghostty-undefined.before
  nm -g --defined-only -P /tmp/ghostty-merged.o |
    awk '\$1 ~ /^ghostty_/ { print \$1 }' |
    LC_ALL=C sort -u > /tmp/ghostty-public.before
  if [ ! -s /tmp/ghostty-undefined.before ] ||
     [ ! -s /tmp/ghostty-public.before ]; then
    echo 'ERROR: Ghostty symbol-isolation inputs are unexpectedly empty' >&2
    exit 1
  fi
  {
    awk '{ print \$1 }' /tmp/ghostty-undefined.before
    cat /tmp/ghostty-public.before
  } | LC_ALL=C sort -u > /tmp/ghostty-restore-names.txt
  unsafe_restore_names=\"\$(
    LC_ALL=C grep -Ev '^[[:alnum:]_.\$@?]+$' \
      /tmp/ghostty-restore-names.txt || true
  )\"
  if [ -n \"\$unsafe_restore_names\" ]; then
    echo 'ERROR: Ghostty symbols cannot be encoded safely for objcopy' >&2
    printf '%s\n' \"\$unsafe_restore_names\" >&2
    exit 1
  fi
  awk -v prefix=\"\$symbol_prefix\" '{ print prefix \$1, \$1 }' \
    /tmp/ghostty-restore-names.txt > /tmp/ghostty-restore-symbols.txt
  objcopy --prefix-symbols=\"\$symbol_prefix\" \
    /tmp/ghostty-merged.o /tmp/ghostty-prefixed.o
  objcopy --redefine-syms=/tmp/ghostty-restore-symbols.txt \
    /tmp/ghostty-prefixed.o /tmp/ghostty-local.o
  nm -g --undefined-only -P /tmp/ghostty-local.o |
    LC_ALL=C sort -u > /tmp/ghostty-undefined.after
  if ! cmp -s /tmp/ghostty-undefined.before /tmp/ghostty-undefined.after; then
    echo 'ERROR: Ghostty localization changed unresolved external symbols' >&2
    diff -u /tmp/ghostty-undefined.before /tmp/ghostty-undefined.after >&2 || true
    exit 1
  fi
  nm -g --defined-only -P /tmp/ghostty-local.o |
    awk '\$1 ~ /^ghostty_/ { print \$1 }' |
    LC_ALL=C sort -u > /tmp/ghostty-public.after
  if ! cmp -s /tmp/ghostty-public.before /tmp/ghostty-public.after; then
    echo 'ERROR: Ghostty isolation changed its public C API' >&2
    diff -u /tmp/ghostty-public.before /tmp/ghostty-public.after >&2 || true
    exit 1
  fi
  unisolated_definitions=\"\$(
    nm -g --defined-only -P /tmp/ghostty-local.o |
      awk '\$1 !~ /^ghostty_/ && \$1 !~ /^cmux_ghostty_/ { print \$1 }'
  )\"
  if [ -n \"\$unisolated_definitions\" ]; then
    echo 'ERROR: Ghostty archive exposes unprefixed private definitions' >&2
    printf '%s\n' \"\$unisolated_definitions\" >&2
    exit 1
  fi
  private_symbols=\"\$(
    nm -g --defined-only -P /tmp/ghostty-local.o |
      awk '\$1 ~ /^cmux_ghostty_/ { count++ } END { print count + 0 }'
  )\"
  if [ \"\$private_symbols\" -lt 1 ]; then
    echo 'ERROR: Ghostty archive has no namespaced private definitions' >&2
    exit 1
  fi
  # Belt-and-suspenders: ensure a non-exec GNU-stack note is present.
  objcopy --add-section .note.GNU-stack=/dev/null \
    --set-section-flags .note.GNU-stack=readonly /tmp/ghostty-local.o \
    /tmp/ghostty-local.o 2>/dev/null || true
  rm -f ghostty-internal.a
  ar rcs ghostty-internal.a /tmp/ghostty-local.o
  rm -f /tmp/ghostty-merged.o /tmp/ghostty-prefixed.o /tmp/ghostty-local.o \
    /tmp/ghostty-undefined.before /tmp/ghostty-undefined.after \
    /tmp/ghostty-public.before /tmp/ghostty-public.after \
    /tmp/ghostty-restore-names.txt /tmp/ghostty-restore-symbols.txt
  echo \"vendored libghostty-linux: global ghostty_ syms = \$(nm -g ghostty-internal.a 2>/dev/null | grep -c ' T ghostty_' || true); size = \$(du -h ghostty-internal.a | cut -f1)\""
