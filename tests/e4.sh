#!/bin/bash
# E4: extraction differential.  Upstream creates streaming archives (one
# segment per file, no JIDAC metadata), the port extracts them and the result
# is compared byte for byte with the original file.
#
# Journaling archives (.zpaq -m0..-m5) carry fragment metadata and an index;
# reassembling those is host work (stage E5), so they are not part of this test.
set -u
B=${1:-build}
UP=${UPSTREAM:-/mnt/IA_LAB/agentes/zpaqII/upstream-zpaq}
Z=$UP/zpaq
W=$B/e4work
mkdir -p "$W"
cd "$W" || exit 1

if [ ! -x "$Z" ]; then echo "missing oracle: $Z"; exit 1; fi
X=../extract
if [ ! -x "$X" ]; then echo "missing extractor: $X"; exit 1; fi

mkdir -p corpus
[ -f corpus/text.txt ] || yes "the quick brown fox jumps over the lazy dog 0123456789" | head -c 200000 > corpus/text.txt
[ -f corpus/bin.dat ] || head -c 200000 /dev/urandom > corpus/bin.dat
[ -f corpus/big.bin ] || head -c 3000000 /dev/urandom > corpus/big.bin
[ -f corpus/exe.bin ] || cp /bin/ls corpus/exe.bin
[ -f corpus/empty.bin ] || : > corpus/empty.bin

fail=0
n=0
run() { # name file method
  local name=$1 f=$2 m=$3
  n=$((n+1))
  rm -f "$name.zpaq" "$name.out"
  if ! "$Z" add "$name.zpaq" "$f" -method "$m" -t1 >/dev/null 2>&1; then
    echo "FAIL $name ($m): add failed"; fail=1; return
  fi
  if ! "$X" "$name.zpaq" "$name.out" >/dev/null 2>&1; then
    echo "FAIL $name ($m): extract error"; fail=1; return
  fi
  if cmp -s "$f" "$name.out"; then
    echo "ok   $name ($m)"
  else
    echo "FAIL $name ($m): data differs"; fail=1
  fi
}

METHODS="s4.0 s4.1.5.0.3.22 s4.2.12.0.7.25.1 s4.3ci1 s4.0ci1 s4.0c256ci1,1,1,1,1,1,2a s4.0a24 s4.0w2c0,1010,255i1 s4.0t0 s4.0s8 s4.4 s4.7 s4.0cm16 s4.0icm16"

for m in $METHODS; do
  run "text-$m" corpus/text.txt "$m"
  run "bin-$m" corpus/bin.dat "$m"
done
for m in s4.0 s4.1.5.0.3.22 s4.3ci1 s4.4 s4.7; do
  run "exe-$m" corpus/exe.bin "$m"
done
# multi-block (1 MiB blocks over a 3 MB input) and empty input
run "big-s0.0" corpus/big.bin s0.0
run "big-s0.1.5.0.3.22" corpus/big.bin s0.1.5.0.3.22
run "big-s0.3ci1" corpus/big.bin s0.3ci1
run "empty-s4.1.5.0.3.22" corpus/empty.bin s4.1.5.0.3.22
run "empty-s4.0" corpus/empty.bin s4.0

if [ $fail -ne 0 ]; then echo "E4: FAILURES"; exit 1; fi
echo "E4: $n archives extracted identically"
