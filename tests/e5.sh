#!/bin/bash
# E5: Pascal host.  Streaming archives must be bit-identical to upstream and
# mutually extractable.
set -u
B=${1:-build}
UP=${UPSTREAM:-/mnt/IA_LAB/agentes/zpaqII/upstream-zpaq}
Z=$UP/zpaq
W=$B/e5work
mkdir -p "$W"
cd "$W" || exit 1
P=../zpaqii
[ -x "$Z" ] || { echo "missing oracle"; exit 1; }
[ -x "$P" ] || { echo "missing zpaqii"; exit 1; }

mkdir -p corpus
[ -f corpus/text.txt ] || yes "the quick brown fox jumps over the lazy dog 0123456789" | head -c 200000 > corpus/text.txt
[ -f corpus/bin.dat ] || head -c 200000 /dev/urandom > corpus/bin.dat
[ -f corpus/big.bin ] || head -c 3000000 /dev/urandom > corpus/big.bin
[ -f corpus/exe.bin ] || cp /bin/ls corpus/exe.bin
[ -f corpus/empty.bin ] || : > corpus/empty.bin

fail=0; n=0
run() { # name method files...
  local name=$1 m=$2; shift 2
  local files=("$@")
  n=$((n+1))
  rm -rf "$name".*.zpaq "$name".out
  mkdir -p "$name".out "$name".up
  if ! "$P" add "$name".ours.zpaq "${files[@]}" -method "$m" >/dev/null 2>&1; then
    echo "FAIL $name ($m): our add"; fail=1; return
  fi
  if ! "$Z" add "$name".up.zpaq "${files[@]}" -method "$m" -t1 >/dev/null 2>&1; then
    echo "FAIL $name ($m): upstream add"; fail=1; return
  fi
  if ! cmp -s "$name".ours.zpaq "$name".up.zpaq; then
    echo "FAIL $name ($m): archives differ"; fail=1; return
  fi
  # upstream archive -> our extract
  if ! "$P" extract "$name".up.zpaq -to "$name".out >/dev/null 2>&1; then
    echo "FAIL $name ($m): our extract"; fail=1; return
  fi
  for f in "${files[@]}"; do
    if ! cmp -s "$f" "$name".out/"$f"; then
      echo "FAIL $name ($m): extract differs ($f)"; fail=1; return
    fi
  done
  # our archive -> upstream extract
  if ! "$Z" extract "$name".ours.zpaq -to "$name".up -t1 >/dev/null 2>&1; then
    echo "FAIL $name ($m): upstream extract"; fail=1; return
  fi
  for f in "${files[@]}"; do
    if ! cmp -s "$f" "$name".up/"$f"; then
      echo "FAIL $name ($m): upstream extract differs ($f)"; fail=1; return
    fi
  done
  echo "ok   $name ($m, $(stat -c%s "$name".ours.zpaq) bytes)"
}

METHODS="s4.0 s4.1.5.0.3.22 s4.2.12.0.7.25.1 s4.3ci1 s4.0ci1 s4.4 s4.7 s4.0c256ci1,1,1,1,1,1,2a s4.0a24"
for m in $METHODS; do
  run "one-$m" "$m" corpus/text.txt
  run "two-$m" "$m" corpus/text.txt corpus/bin.dat
done
for m in s0.0 s0.1.5.0.3.22 s0.3ci1; do
  run "big-$m" "$m" corpus/big.bin corpus/text.txt
done
run empty s4.1.5.0.3.22 corpus/empty.bin
run exe s4.7 corpus/exe.bin

if [ $fail -ne 0 ]; then echo "E5: FAILURES"; exit 1; fi
echo "E5: $n streaming archives bit-identical and cross-extractable"
