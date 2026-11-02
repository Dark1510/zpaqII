#!/bin/bash
# E6b: full verification matrix.
#
# For each corpus file and each method:
#   1. upstream add vs port add (frozen clock)  -> archives bit-identical
#   2. port extracts the upstream archive       -> files identical
#   3. upstream extracts the port archive       -> files identical
# Plus multi-version updates, -until rollback, directory trees and deletion.
#
# Anything not covered yet (multi-part, -key, -index, -fragment, threads) is
# listed at the end so the gap is explicit.
set -u
B=${1:-build}
UP=${UPSTREAM:-/mnt/IA_LAB/agentes/zpaqII/upstream-zpaq}
Z=$UP/zpaq
W=$B/matrixwork
mkdir -p "$W"
cd "$W" || exit 1
P=../zpaqii
FT=$PWD/../faketime.so
[ -x "$Z" ] || { echo "missing upstream: $Z"; exit 2; }
[ -x "$P" ] || { echo "missing zpaqii"; exit 2; }
[ -f "$FT" ] || { echo "missing faketime.so"; exit 2; }

# fresh state on every run (the corpus is reused)
rm -rf o-*.zpaq o-*.xu o-*.xo tree tree.u.zpaq tree.o.zpaq tree.out \
       mv mv1 mv.u.zpaq mv.o.zpaq mv.out mv1.out

mkdir -p corpus
[ -f corpus/text.txt ] || yes "the quick brown fox jumps over the lazy dog 0123456789" | head -c 120000 > corpus/text.txt
[ -f corpus/bin.dat ] || head -c 120000 /dev/urandom > corpus/bin.dat
[ -f corpus/exe.bin ] || head -c 60000 /bin/ls > corpus/exe.bin
[ -f corpus/empty.bin ] || : > corpus/empty.bin

cases=0
fails=0
declare -a failed

ok() { cases=$((cases+1)); }
bad() { cases=$((cases+1)); fails=$((fails+1)); failed+=("$1"); echo "FAIL $1"; }

# bit-identical add + cross extraction for one file/method pair
one() { # tag method file
  local tag=$1 m=$2 f=$3
  local base="o-$tag"
  rm -rf "$base".u.zpaq "$base".o.zpaq "$base".xu "$base".xo
  if ! FAKE_TIME=1791000000 LD_PRELOAD=$FT "$Z" add "$base".u.zpaq "$f" -method "$m" -t1 >/dev/null 2>&1; then
    echo "SKIP $tag ($m): upstream add failed"; return
  fi
  if ! ZPAQII_NOW=1791000000 "$P" add "$base".o.zpaq "$f" -method "$m" >/dev/null 2>&1; then
    bad "$tag ($m) add-error"; return
  fi
  if cmp -s "$base".u.zpaq "$base".o.zpaq; then ok; else bad "$tag ($m) add-bytes"; return; fi
  mkdir -p "$base".xo "$base".xu
  if ! "$P" extract "$base".u.zpaq -to "$base".xo >/dev/null 2>&1 || ! cmp -s "$f" "$base".xo/"$f"; then
    bad "$tag ($m) port-extract"; return
  fi
  if ! "$Z" extract "$base".o.zpaq -to "$base".xu -t1 >/dev/null 2>&1 || ! cmp -s "$f" "$base".xu/"$f"; then
    bad "$tag ($m) upstream-extract"; return
  fi
  ok
}

for m in 0 1 2 3 4 5; do
  for f in text.txt bin.dat exe.bin empty.bin; do
    one "$f-m$m" "$m" "corpus/$f"
  done
done

# explicit methods (levels 0..3 with LZ77 / SA / BWT / E8E9 and model types)
for m in x4.0 x4.1.5.0.3.22 x4.2.12.0.7.25.1 x4.3ci1 x4.0ci1 \
         x4.0c256ci1,1,1,1,1,1,2a x4.0a24 x4.0w2c0,1010,255i1 x4.4 x4.7; do
  one "text-$m" "$m" corpus/text.txt
  one "exe-$m" "$m" corpus/exe.bin
done

# streaming methods
for m in s4.0 s4.1.5.0.3.22 s4.3ci1 s0.1.5.0.3.22; do
  one "stream-$m" "$m" corpus/text.txt
done

# directory tree
rm -rf tree
mkdir -p tree/sub
cp corpus/text.txt tree/a.txt
cp corpus/bin.dat tree/sub/b.dat
cp corpus/empty.bin tree/sub/e
if FAKE_TIME=1791000000 LD_PRELOAD=$FT "$Z" add tree.u.zpaq tree -m1 -t1 >/dev/null 2>&1 &&
   ZPAQII_NOW=1791000000 "$P" add tree.o.zpaq tree -m1 >/dev/null 2>&1 &&
   cmp -s tree.u.zpaq tree.o.zpaq; then ok; else bad "tree add"; fi
rm -rf tree.out
if "$P" extract tree.u.zpaq -to tree.out >/dev/null 2>&1 &&
   diff -r tree tree.out/tree >/dev/null 2>&1; then ok; else bad "tree extract"; fi

# multi-version update + -until rollback
rm -rf mv mv1
mkdir -p mv mv1
printf 'first version\n' > mv/f.txt
FAKE_TIME=1791100000 LD_PRELOAD=$FT "$Z" add mv.u.zpaq mv/f.txt -m1 -t1 >/dev/null 2>&1
ZPAQII_NOW=1791100000 "$P" add mv.o.zpaq mv/f.txt -m1 >/dev/null 2>&1
printf 'second version, longer\n' > mv/f.txt
FAKE_TIME=1791200000 LD_PRELOAD=$FT "$Z" add mv.u.zpaq mv/f.txt -m1 -t1 >/dev/null 2>&1
ZPAQII_NOW=1791200000 "$P" add mv.o.zpaq mv/f.txt -m1 >/dev/null 2>&1
if cmp -s mv.u.zpaq mv.o.zpaq; then ok; else bad "multi-version add"; fi
rm -rf mv.out mv1.out
if "$P" extract mv.u.zpaq -to mv.out >/dev/null 2>&1 &&
   cmp -s mv/f.txt mv.out/mv/f.txt; then ok; else bad "multi-version latest"; fi
if "$P" extract mv.u.zpaq -to mv1.out -until 1 >/dev/null 2>&1 &&
   [ "$(cat mv1.out/mv/f.txt)" = "first version" ]; then ok; else bad "-until 1"; fi

# deletion
rm -f mv/f.txt
FAKE_TIME=1791300000 LD_PRELOAD=$FT "$Z" add mv.u.zpaq mv -m1 -t1 >/dev/null 2>&1
ZPAQII_NOW=1791300000 "$P" add mv.o.zpaq mv -m1 >/dev/null 2>&1
if cmp -s mv.u.zpaq mv.o.zpaq; then ok; else bad "deletion add"; fi

echo
echo "matrix: $cases checks, $fails failures"
if [ $fails -ne 0 ]; then
  printf '  failed: %s\n' "${failed[@]}"
  exit 1
fi
cat <<'EOF'
not covered yet (known gaps):
  - multi-part archives, -key (encryption in add), -index, -fragment, -tN>1
EOF
