#!/bin/bash
# zpaqII - cross-build for Windows from Linux.
#
#   tools/win.sh win64     core as zqcore.dll + the Pascal host as zpaqii.exe
#   tools/win.sh win32     core + the unit tests as console executables
#
# Needs mingw-w64 (x86_64-w64-mingw32-gcc / i686-w64-mingw32-gcc).  The 64-bit
# host also needs FPC with the win64 units (Debian: fp-units-win-base).  Debian
# ships no i386-win32 units, so the 32-bit host has to be built on Windows
# itself; the 32-bit core is fully built and tested here.
set -eu
cd "$(dirname "$0")/.."

ARCH=${1:-win64}
OUT=build/$ARCH

case "$ARCH" in
  win64)
    CC=x86_64-w64-mingw32-gcc
    FPCARGS="-Twin64 -Px86_64"
    ;;
  win32)
    CC=i686-w64-mingw32-gcc
    FPCARGS=""
    ;;
  *)
    echo "usage: $0 [win64|win32]" >&2
    exit 2
    ;;
esac

command -v "$CC" >/dev/null 2>&1 || {
  echo "missing $CC (apt install gcc-mingw-w64-x86-64)" >&2
  exit 2
}

rm -rf "$OUT"
mkdir -p "$OUT/obj"

echo "== core for $ARCH"
for f in src/core/*.c; do
  "$CC" -std=c11 -O2 -Wall -Wextra -Iinclude -Isrc/core \
    -c "$f" -o "$OUT/obj/$(basename "${f%.c}").o"
done

"$CC" -shared -o "$OUT/zqcore.dll" "$OUT"/obj/*.o \
  -Wl,--out-implib,"$OUT/libzqcore.dll.a" -static-libgcc
# FPC's {$LinkLib zqcore} looks for libzqcore.a; an import library is fine
cp "$OUT/libzqcore.dll.a" "$OUT/libzqcore.a"

echo "== unit tests for $ARCH"
for t in vectors_e1 fragchain; do
  "$CC" -std=c11 -O2 -static -Iinclude -Isrc/core \
    "tests/unit/$t.c" src/core/*.c -o "$OUT/$t.exe"
done

if [ "$ARCH" = win64 ]; then
  command -v fpc >/dev/null 2>&1 || {
    echo "missing fpc: install fp-units-win-base to cross-build the host" >&2
    exit 2
  }
  echo "== host for $ARCH"
  mkdir -p "$OUT/fpc"
  # shellcheck disable=SC2086
  fpc $FPCARGS -Mobjfpc -O2 -FU"$OUT/fpc" -Fuhost -Fl"$OUT" \
    host/zpaqii.lpr -o"$OUT/zpaqii.exe"
  echo
  echo "built $OUT/zpaqii.exe; copy it with $OUT/zqcore.dll"
else
  echo
  echo "built the core and the tests for $ARCH."
  echo "the 32-bit host needs FPC on Windows (no i386-win32 units in Debian)."
fi
