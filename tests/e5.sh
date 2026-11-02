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

# ---- journaling archives: read, list and extract (add is not implemented) ----
JFAIL=0
jrun() { # name method file
  local name=$1 m=$2 f=$3
  rm -rf "$name".zpaq "$name".out "$name".up
  mkdir -p "$name".out "$name".up
  if ! "$Z" add "$name".zpaq "$f" -m"$m" -t1 >/dev/null 2>&1; then
    echo "SKIP $name (upstream add)"; return
  fi
  if ! "$P" extract "$name".zpaq -to "$name".out >/dev/null 2>&1; then
    echo "FAIL $name (m$m): our extract"; JFAIL=1; return
  fi
  if ! cmp -s "$f" "$name".out/"$f"; then
    echo "FAIL $name (m$m): data differs"; JFAIL=1; return
  fi
  if ! "$Z" extract "$name".zpaq -to "$name".up -t1 >/dev/null 2>&1; then
    echo "FAIL $name (m$m): upstream extract"; JFAIL=1; return
  fi
  if ! diff -r "$name".out "$name".up >/dev/null 2>&1; then
    echo "FAIL $name (m$m): differs from upstream extract"; JFAIL=1; return
  fi
  echo "ok   $name (journaling m$m)"
}
for m in 0 1 2 3 4 5; do
  jrun "j-text-m$m" "$m" corpus/text.txt
  jrun "j-bin-m$m" "$m" corpus/bin.dat
done
jrun j-two 1 corpus/text.txt

# multi-version with -until
rm -rf multi.zpaq multi.out1 multi.out2
printf 'one\n' > v1.txt
"$Z" add multi.zpaq v1.txt -m1 -t1 >/dev/null 2>&1
printf 'two, longer\n' > v1.txt
sleep 1
"$Z" add multi.zpaq v1.txt -m1 -t1 >/dev/null 2>&1
mkdir -p multi.out1 multi.out2
if "$P" extract multi.zpaq -to multi.out2 >/dev/null 2>&1 && \
   cmp -s v1.txt multi.out2/v1.txt; then echo "ok   latest version"; else echo "FAIL latest version"; JFAIL=1; fi
if "$P" extract multi.zpaq -to multi.out1 -until 1 >/dev/null 2>&1 && \
   [ "$(cat multi.out1/v1.txt)" = "one" ]; then echo "ok   -until 1"; else echo "FAIL -until 1"; JFAIL=1; fi

if [ $JFAIL -ne 0 ]; then echo "E5b: FAILURES"; exit 1; fi
echo "E5b: journaling archives listed and extracted identically"

# ---- journaling add: bit-identical to upstream with a frozen clock ----
AF=0
run_add() { # name method files...
  local name=$1 m=$2; shift 2
  local files=("$@")
  rm -f "$name".u.zpaq "$name".o.zpaq
  if ! FAKE_TIME=1790000000 LD_PRELOAD=../faketime.so "$Z" add "$name".u.zpaq "${files[@]}" -method "$m" -t1 >/dev/null 2>&1; then
    echo "SKIP add $name ($m)"; return
  fi
  if ! ZPAQII_NOW=1790000000 "$P" add "$name".o.zpaq "${files[@]}" -method "$m" >/dev/null 2>&1; then
    echo "FAIL add $name ($m): error"; AF=1; return
  fi
  if cmp -s "$name".u.zpaq "$name".o.zpaq; then
    echo "ok   add $name ($m)"
  else
    echo "FAIL add $name ($m): archives differ"; AF=1
  fi
}
for m in 0 1 2 3 4 5 x4.1.5.0.3.22 x4.3ci1; do
  run_add "a-text-$m" "$m" corpus/text.txt
  run_add "a-two-$m" "$m" corpus/text.txt corpus/bin.dat
done
run_add a-dir 1 corpus/text.txt corpus/bin.dat corpus/small.txt

# two updates (dedup across versions) and a deletion
rm -rf up1 up2 upd.u.zpaq upd.o.zpaq
mkdir -p up1
cp corpus/text.txt up1/a.txt
cp corpus/bin.dat up1/b.txt
FAKE_TIME=1790010000 LD_PRELOAD=../faketime.so "$Z" add upd.u.zpaq up1 -m1 -t1 >/dev/null 2>&1
ZPAQII_NOW=1790010000 "$P" add upd.o.zpaq up1 -m1 >/dev/null 2>&1
printf 'more data\n' >> up1/a.txt
rm -f up1/b.txt
FAKE_TIME=1790020000 LD_PRELOAD=../faketime.so "$Z" add upd.u.zpaq up1 -m1 -t1 >/dev/null 2>&1
ZPAQII_NOW=1790020000 "$P" add upd.o.zpaq up1 -m1 >/dev/null 2>&1
if cmp -s upd.u.zpaq upd.o.zpaq; then
  echo "ok   update+deletion"
else
  echo "FAIL update+deletion"; AF=1
fi
mkdir -p upd.out
if "$P" extract upd.o.zpaq -to upd.out >/dev/null 2>&1 && cmp -s up1/a.txt upd.out/up1/a.txt && [ ! -e upd.out/up1/b.txt ]; then
  echo "ok   extract after update"
else
  echo "FAIL extract after update"; AF=1
fi

if [ $AF -ne 0 ]; then echo "E5c: FAILURES"; exit 1; fi
echo "E5c: journaling add is bit-identical (levels 0-5, updates, deletion)"
