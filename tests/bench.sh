#!/bin/bash
# E6c baseline: throughput of the port vs upstream (single threaded).
set -u
export LC_ALL=C
B=${1:-build}
UP=${UPSTREAM:-/mnt/IA_LAB/agentes/zpaqII/upstream-zpaq}
Z=$UP/zpaq
W=$B/benchwork
mkdir -p "$W"; cd "$W" || exit 1
P=../zpaqii; FT=../faketime.so
[ -x "$P" ] && [ -x "$Z" ] || { echo "missing binaries"; exit 2; }

mkdir -p corpus
[ -f corpus/text.txt ] || yes "the quick brown fox jumps over the lazy dog 0123456789" | head -c 4000000 > corpus/text.txt
[ -f corpus/bin.dat ] || head -c 4000000 /dev/urandom > corpus/bin.dat

now() { date +%s.%N; }
el() { awk -v a="$1" -v b="$2" 'BEGIN{printf "%.2f", b-a}'; }
row() { awk -v op="$1" -v f="$2" -v m="$3" -v u="$4" -v p="$5" \
  'BEGIN{printf "%-7s %-9s %-3s up %6.2fs  port %6.2fs  %5.1f%%\n", op, f, m, u, p, 100*u/p}'; }

bench() { # file method
  local f=$1 m=$2 t0 t1 tu tp
  rm -f b_u.zpaq b_o.zpaq
  t0=$(now); FAKE_TIME=1791000000 LD_PRELOAD=$FT "$Z" add b_u.zpaq "$f" -method "$m" -t1 >/dev/null 2>&1; t1=$(now); tu=$(el "$t0" "$t1")
  t0=$(now); ZPAQII_NOW=1791000000 "$P" add b_o.zpaq "$f" -method "$m" >/dev/null 2>&1; t1=$(now); tp=$(el "$t0" "$t1")
  row add "$(basename "$f")" "$m" "$tu" "$tp"
  rm -rf xu xp; mkdir xu xp
  t0=$(now); "$Z" extract b_u.zpaq -to xu -t1 >/dev/null 2>&1; t1=$(now); tu=$(el "$t0" "$t1")
  t0=$(now); "$P" extract b_u.zpaq -to xp >/dev/null 2>&1; t1=$(now); tp=$(el "$t0" "$t1")
  row extract "$(basename "$f")" "$m" "$tu" "$tp"
}

for m in ${METHODS:-1 2}; do
  bench corpus/text.txt "$m"
  bench corpus/bin.dat "$m"
done
