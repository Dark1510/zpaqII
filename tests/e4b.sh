#!/bin/bash
# E4b: bit-exact block compression.  Both implementations compress the same
# input with the same method into one block; the outputs must be identical.
set -u
B=${1:-build}
W=$B/e4bwork
mkdir -p "$W"
cd "$W" || exit 1
X=../cblock
O=../e4_oracle
[ -x "$X" ] || { echo "missing cblock"; exit 1; }
[ -x "$O" ] || { echo "missing e4_oracle"; exit 1; }

mkdir -p corpus
[ -f corpus/text.txt ] || yes "the quick brown fox jumps over the lazy dog 0123456789" | head -c 100000 > corpus/text.txt
[ -f corpus/bin.dat ] || head -c 100000 /dev/urandom > corpus/bin.dat
[ -f corpus/exe.bin ] || head -c 60000 /bin/ls > corpus/exe.bin
[ -f corpus/small.txt ] || printf 'abcabcabcabcabcabcabc\n' > corpus/small.txt
[ -f corpus/empty.bin ] || : > corpus/empty.bin

fail=0; n=0
run() { # file method
  local f=$1 m=$2
  n=$((n+1))
  rm -f a.out b.out
  if ! "$O" "$f" a.out "$m" >/dev/null 2>&1; then echo "SKIP $f $m (oracle)"; return; fi
  if ! "$X" "$f" b.out "$m" >/dev/null 2>&1; then echo "FAIL $f $m (port error)"; fail=1; return; fi
  if cmp -s a.out b.out; then
    echo "ok   $(basename "$f") $m ($(stat -c%s a.out) bytes)"
  else
    echo "FAIL $(basename "$f") $m ($(stat -c%s a.out) vs $(stat -c%s b.out))"
    fail=1
  fi
}

for m in 0 1 2 3 4 5 6 7; do
  for f in corpus/text.txt corpus/bin.dat corpus/small.txt corpus/empty.bin; do
    run "$f" "$m"
  done
done
for m in x4.0 x4.1.5.0.3.22 x4.2.12.0.7.25.1 x4.3ci1 x4.0ci1 x4.0c256ci1,1,1,1,1,1,2a x4.0a24 x4.0w2c0,1010,255i1 x4.0t0 x4.0s8 x4.4 x4.7 s4.3ci1 s4.1.5.0.3.22; do
  run corpus/text.txt "$m"
done
for m in 4 5 x4.4 x4.7 x4.3ci1; do run corpus/exe.bin "$m"; done

if [ $fail -ne 0 ]; then echo "E4b: FAILURES"; exit 1; fi
echo "E4b: $n blocks identical"
