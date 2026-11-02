#!/bin/bash
# zpaqII - cross-target check of the C core.
#
# Compiles src/core for every target this machine has a compiler for, and runs
# the unit tests on the targets it can execute (natively or under qemu-user).
# Targets without a compiler are reported as skipped, not as passed.
#
#   tools/cross.sh            # default: all
#   OUT=build/cross tools/cross.sh
#
# The Windows binaries are built with mingw and are meant to be run on a real
# Windows VM (tools/cross.sh only builds them, since there is no wine here).
set -u
cd "$(dirname "$0")/.." || exit 1

OUT=${OUT:-build/cross}
REF=${REF:-build/e1_upstream.txt}
CFLAGS=${CFLAGS:--std=c11 -O2 -Wall -Wextra -Wshadow}
mkdir -p "$OUT"

pass=0
fail=0
skip=0
note() { printf '  %-26s %s\n' "$1" "$2"; }
ok() { pass=$((pass + 1)); }
bad() {
  fail=$((fail + 1))
  note "$1" "FAIL"
  [ -s "$2" ] && sed -n '1,6p' "$2"
}

# Build every core object; a target that compiles is a target that builds.
build_target() { # tag, cc, extra flags...
  local tag=$1 cc=$2
  shift 2
  local d="$OUT/$tag"
  rm -rf "$d"
  mkdir -p "$d"
  local f
  for f in src/core/*.c; do
    # shellcheck disable=SC2086
    "$cc" $CFLAGS -Iinclude -Isrc/core "$@" -c "$f" \
      -o "$d/$(basename "${f%.c}").o" 2>>"$d/err.txt" ||
      { bad "$tag" "$d/err.txt"; return; }
  done
  note "$tag" "compiles ($(ls "$d"/*.o | wc -l) objects)"
  ok
}

# Build and run one unit test.
run_test() { # tag, cc, runner, source
  local tag=$1 cc=$2 runner=$3 src=$4
  local d="$OUT/$tag"
  local bin="$d/$(basename "${src%.c}")"
  if ! "$cc" $CFLAGS -static -Iinclude -Isrc/core "$src" src/core/*.c \
       -o "$bin" 2>>"$d/err.txt"; then
    bad "$tag $(basename "$src")" "$d/err.txt"
    return
  fi
  if $runner "$bin" >"$d/$(basename "${src%.c}").out" 2>&1; then
    note "$tag $(basename "$src")" \
      "$(tail -1 "$d/$(basename "${src%.c}").out")"
    ok
  else
    bad "$tag $(basename "$src")" "$d/$(basename "${src%.c}").out"
  fi
}

# The reference output comes from the oracle binary built by `make build/..."
if [ ! -s "$REF" ]; then
  echo "missing $REF: run 'make build/e1_upstream.txt' first" >&2
  exit 2
fi

echo "native x86-64"
build_target native gcc
run_test native gcc "" tests/unit/vectors_e1.c
run_test native gcc "" tests/unit/fragchain.c
run_test native gcc "" tests/unit/sha1_hw.c
if cmp -s "$OUT/native/vectors_e1.out" "$REF"; then
  note "native E1 vs oracle" "identical"
  ok
else
  bad "native E1 vs oracle" "$OUT/native/vectors_e1.out"
fi

echo "native x86 (gcc -m32)"
if gcc -m32 -x c - -o "$OUT/m32probe" >/dev/null 2>&1 <<<'int main(void){return 0;}'; then
  build_target i386-linux gcc -m32
  if gcc -m32 $CFLAGS -static -Iinclude -Isrc/core tests/unit/vectors_e1.c \
       src/core/*.c -o "$OUT/i386-linux/e1" 2>>"$OUT/i386-linux/err.txt"; then
    if "$OUT/i386-linux/e1" >"$OUT/i386-linux/e1.out" 2>&1 &&
       cmp -s "$OUT/i386-linux/e1.out" "$REF"; then
      note "i386-linux vectors_e1" "identical to oracle"
      ok
    else
      bad "i386-linux vectors_e1" "$OUT/i386-linux/e1.out"
    fi
  else
    bad "i386-linux link" "$OUT/i386-linux/err.txt"
  fi
else
  note "i386-linux" "skipped (no 32-bit libc)"
  skip=$((skip + 1))
fi

echo "linux arm64 (qemu-aarch64)"
if command -v aarch64-linux-gnu-gcc >/dev/null 2>&1; then
  build_target aarch64-linux aarch64-linux-gnu-gcc
  if command -v qemu-aarch64 >/dev/null 2>&1; then
    run_test aarch64-linux aarch64-linux-gnu-gcc qemu-aarch64 \
      tests/unit/vectors_e1.c
    run_test aarch64-linux aarch64-linux-gnu-gcc qemu-aarch64 \
      tests/unit/fragchain.c
    if cmp -s "$OUT/aarch64-linux/vectors_e1.out" "$REF"; then
      note "aarch64 E1 vs oracle" "identical"
      ok
    else
      bad "aarch64 E1 vs oracle" "$OUT/aarch64-linux/vectors_e1.out"
    fi
  else
    note "aarch64 run" "skipped (no qemu-aarch64)"
    skip=$((skip + 1))
  fi
else
  note "aarch64-linux" "skipped (no cross compiler)"
  skip=$((skip + 1))
fi

echo "windows x86-64 and x86 (mingw, build only)"
for t in x86_64-w64-mingw32 i686-w64-mingw32; do
  if command -v "$t-gcc" >/dev/null 2>&1; then
    build_target "$t" "$t-gcc" -static
    # link a static test exe for a real Windows run
    if "$t-gcc" $CFLAGS -static -Iinclude -Isrc/core tests/unit/vectors_e1.c \
         src/core/*.c -o "$OUT/$t/vectors_e1.exe" 2>>"$OUT/$t/err.txt"; then
      note "$t exe" "built"
      ok
    else
      bad "$t exe" "$OUT/$t/err.txt"
    fi
  else
    note "$t" "skipped (no cross compiler)"
    skip=$((skip + 1))
  fi
done

echo
echo "cross: $pass passed, $fail failed, $skip skipped"
[ "$fail" = 0 ]
