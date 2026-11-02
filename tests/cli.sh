#!/bin/bash
# E6d: CLI option parity with upstream.
#
# Exercises the options that do not change the archive format itself:
#   -fragment, -to (single, multiple, prefix rename), -only, -not (names and
#   =type), -key (encrypted add, cross extraction, wrong password), multi-part
#   archives (* / ????), -force, -tN/-threads (accepted), and the list output
#   (banner and timing normalised).
#
# Usage: cli.sh <builddir>
set -u
export LC_ALL=C
B=${1:-build}
UP=${UPSTREAM:-/mnt/IA_LAB/agentes/zpaqII/upstream-zpaq}
Z=$UP/zpaq
W=$B/cliwork
rm -rf "$W"
mkdir -p "$W"
cd "$W" || exit 1
P=$PWD/../zpaqii
FT=$PWD/../faketime.so
[ -x "$Z" ] || { echo "missing upstream: $Z"; exit 2; }
[ -x "$P" ] || { echo "missing zpaqii"; exit 2; }
[ -f "$FT" ] || { echo "missing faketime.so"; exit 2; }

NOW=1791000000
cases=0
fails=0
failed=()
ok() { cases=$((cases + 1)); }
bad() {
  cases=$((cases + 1))
  fails=$((fails + 1))
  failed+=("$1")
  echo "FAIL $1"
}

# corpus
mkdir -p d/sub
yes "the quick brown fox jumps over the lazy dog 0123456789" | head -c 40000 > d/a.txt
head -c 20000 /dev/urandom > d/sub/b.bin
printf 'tiny\n' > d/c.txt

# normalise a list output: drop the banner and the timing line
norm() { grep -v -e '^zpaq v' -e ' seconds (' ; }

# ---- -fragment: the archive must be bit-identical ----
for f in 0 3 6 9 12; do
  rm -f fu.zpaq fo.zpaq
  FAKE_TIME=$NOW LD_PRELOAD=$FT "$Z" add fu.zpaq d -m1 -fragment "$f" -t1 >/dev/null 2>&1
  ZPAQII_NOW=$NOW "$P" add fo.zpaq d -m1 -fragment "$f" >/dev/null 2>&1
  if cmp -s fu.zpaq fo.zpaq; then ok; else bad "-fragment $f"; fi
done

# base archive for the extraction tests
FAKE_TIME=$NOW LD_PRELOAD=$FT "$Z" add base.zpaq d -m1 -t1 >/dev/null 2>&1
ZPAQII_NOW=$NOW "$P" add base.zpaq.port d -m1 >/dev/null 2>&1
if cmp -s base.zpaq base.zpaq.port; then ok; else bad "base add"; fi

# ---- -to: single target, several targets, prefix rename ----
extract_both() { # tag, args...
  local tag=$1
  shift
  rm -rf xu xo
  mkdir xu xo
  "$Z" extract base.zpaq -to xu "$@" -t1 >/dev/null 2>&1
  "$P" extract base.zpaq -to xo "$@" >/dev/null 2>&1
  if diff -r xu xo >/dev/null 2>&1; then ok; else bad "$tag"; fi
}
extract_both "-to DIR"
extract_both "-only d/sub" -only d/sub
extract_both "-not d/sub" -not d/sub
extract_both "one file" d/a.txt

# several targets replace the given prefixes
rm -rf xu xo
mkdir xu xo
"$Z" extract base.zpaq d/a.txt d/sub -to xu/A xu/S -t1 >/dev/null 2>&1
"$P" extract base.zpaq d/a.txt d/sub -to xo/A xo/S >/dev/null 2>&1
if diff -r xu xo >/dev/null 2>&1; then ok; else bad "-to rename prefixes"; fi

# -force overwrites
mkdir -p xo/d
printf 'old\n' > xo/d/a.txt
"$P" extract base.zpaq -to xo -force >/dev/null 2>&1
if cmp -s d/a.txt xo/d/a.txt; then ok; else bad "-force"; fi

# ---- list option parity ----
list_both() { # tag, args...
  local tag=$1
  shift
  "$Z" list base.zpaq "$@" -t1 2>&1 | norm > lu.txt
  "$P" list base.zpaq "$@" 2>&1 | norm > lo.txt
  if diff lu.txt lo.txt >/dev/null 2>&1; then ok; else bad "list $tag"; fi
}
list_both "plain"
list_both "-only" -only d/sub
list_both "-not" -not d/sub
list_both "-not =-" -not '=-'
list_both "-to" -to out

# ---- -key: encrypted archives ----
rm -f ku.zpaq ko.zpaq
FAKE_TIME=$NOW FAKE_RANDOM=42 LD_PRELOAD=$FT \
  "$Z" add ku.zpaq d -m1 -key secret -t1 >/dev/null 2>&1
ZPAQII_NOW=$NOW FAKE_RANDOM=42 LD_PRELOAD=$FT \
  "$P" add ko.zpaq d -m1 -key secret >/dev/null 2>&1
if cmp -s ku.zpaq ko.zpaq; then ok; else bad "-key add"; fi

rm -rf xu xo
mkdir xu xo
if "$Z" extract ko.zpaq -to xu -key secret -t1 >/dev/null 2>&1 &&
   diff -r d xu/d >/dev/null 2>&1; then ok; else bad "-key: upstream reads port"; fi
if "$P" extract ku.zpaq -to xo -key secret >/dev/null 2>&1 &&
   diff -r d xo/d >/dev/null 2>&1; then ok; else bad "-key: port reads upstream"; fi

"$P" extract ku.zpaq -to xo -key wrong >/dev/null 2>&1
rp=$?
"$Z" extract ku.zpaq -to xu -key wrong -t1 >/dev/null 2>&1
ru=$?
if [ "$rp" = "$ru" ]; then ok; else bad "-key wrong password rc ($ru vs $rp)"; fi

# ---- dedupe: re-adding unchanged contents must not store the data again ----
rm -rf dd
mkdir dd
head -c 60000 /dev/urandom > dd/x.bin
printf 'same\n' > dd/y.txt
FAKE_TIME=$NOW LD_PRELOAD=$FT "$Z" add du.zpaq dd -m1 -t1 >/dev/null 2>&1
ZPAQII_NOW=$NOW "$P" add do.zpaq dd -m1 >/dev/null 2>&1
touch -d @$((NOW + 5000)) dd/x.bin dd/y.txt
FAKE_TIME=$((NOW + 5000)) LD_PRELOAD=$FT "$Z" add du.zpaq dd -m1 -t1 >/dev/null 2>&1
ZPAQII_NOW=$((NOW + 5000)) "$P" add do.zpaq dd -m1 >/dev/null 2>&1
if cmp -s du.zpaq do.zpaq; then ok; else bad "dedupe on re-add"; fi
if cmp -s du.zpaq do.zpaq && [ "$(stat -c%s do.zpaq)" -lt 80000 ]; then
  ok
else
  bad "dedupe did not reuse fragments"
fi

# ---- multi-part archives ----
rm -f mp*.zpaq op*.zpaq
printf 'uno\n' > t1.txt
head -c 50000 /dev/urandom > t2.bin
FAKE_TIME=$NOW LD_PRELOAD=$FT "$Z" add 'mp????.zpaq' t1.txt -m1 -t1 >/dev/null 2>&1
ZPAQII_NOW=$NOW "$P" add 'op????.zpaq' t1.txt -m1 >/dev/null 2>&1
FAKE_TIME=$((NOW + 1000)) LD_PRELOAD=$FT \
  "$Z" add 'mp????.zpaq' t2.bin -m1 -t1 >/dev/null 2>&1
ZPAQII_NOW=$((NOW + 1000)) "$P" add 'op????.zpaq' t2.bin -m1 >/dev/null 2>&1
if cmp -s mp0001.zpaq op0001.zpaq && cmp -s mp0002.zpaq op0002.zpaq; then
  ok
else
  bad "multi-part add"
fi
rm -rf xu xo
mkdir xu xo
"$Z" extract 'mp????.zpaq' -to xu -t1 >/dev/null 2>&1
"$P" extract 'op????.zpaq' -to xo >/dev/null 2>&1
if diff -r xu xo >/dev/null 2>&1; then ok; else bad "multi-part extract"; fi

# ---- -until: versions, from the end and dates ----
rm -rf uv
mkdir uv
cd uv || exit 1
for k in 1 2 3; do
  printf 'version %d\n' "$k" > f.txt
  touch -d @$((NOW + k * 1000)) f.txt
  ZPAQII_NOW=$((NOW + k * 1000)) "$P" add u.zpaq f.txt -m1 >/dev/null 2>&1
  FAKE_TIME=$((NOW + k * 1000)) LD_PRELOAD=$FT \
    "$Z" add u.zpaq.orig f.txt -m1 -t1 >/dev/null 2>&1
done
if cmp -s u.zpaq u.zpaq.orig; then ok; else bad "-until: base archive"; fi
for v in 0 1 2 3 4 -1 -2 -3; do
  rm -rf au ao
  mkdir au ao
  "$Z" extract u.zpaq -to au -until "$v" -t1 >/dev/null 2>&1
  "$P" extract u.zpaq -to ao -until "$v" >/dev/null 2>&1
  if diff -r au ao >/dev/null 2>&1; then ok; else bad "-until $v"; fi
done
cd ..

# ---- encrypted streaming archives ----
rm -f su.zpaq so.zpaq
FAKE_TIME=$NOW FAKE_RANDOM=5 LD_PRELOAD=$FT \
  "$Z" add su.zpaq d/a.txt -method s4.1.5.0.3.22 -key pw -t1 >/dev/null 2>&1
ZPAQII_NOW=$NOW FAKE_RANDOM=5 LD_PRELOAD=$FT \
  "$P" add so.zpaq d/a.txt -method s4.1.5.0.3.22 -key pw >/dev/null 2>&1
if cmp -s su.zpaq so.zpaq; then ok; else bad "streaming -key"; fi
rm -rf xu xo
mkdir xu xo
"$Z" extract su.zpaq -to xu -key pw -t1 >/dev/null 2>&1 &&
  "$P" extract so.zpaq -to xo -key pw >/dev/null 2>&1 &&
  diff -r xu xo >/dev/null 2>&1 && ok || bad "streaming -key extract"

# ---- extraction threads: same files for any thread count ----
rm -rf dt
mkdir dt
cd dt || exit 1
head -c 6000000 /dev/urandom > r.bin
yes "extraction threads 0123456789" | head -c 6000000 > t.txt
ZPAQII_NOW=$NOW "$P" add e.zpaq r.bin t.txt -method x1.1.5.0.3.22 -t1 >/dev/null 2>&1
for t in 1 2 4 8; do
  rm -rf "o$t"
  mkdir "o$t"
  "$P" extract e.zpaq -to "o$t" -t"$t" >/dev/null 2>&1
done
if diff -r o1 o2 >/dev/null 2>&1 && diff -r o1 o4 >/dev/null 2>&1 &&
   diff -r o1 o8 >/dev/null 2>&1 && cmp -s r.bin o1/r.bin; then
  ok
else
  bad "extraction: -t1/-t2/-t4/-t8 differ"
fi
cd ..
cd ..

# ---- list comparison against the file system ----
rm -rf lc
mkdir -p lc/sub
cd lc || exit 1
printf 'alpha\n' > a.txt
printf 'beta\n' > sub/b.txt
head -c 5000 /dev/urandom > c.bin
ZPAQII_NOW=$NOW "$P" add l.zpaq a.txt sub c.bin -m1 >/dev/null 2>&1
norm() { grep -v -e '^zpaq v' -e ' seconds (' ; }
listcmp() { # tag, args...
  local tag=$1
  shift
  "$Z" list l.zpaq "$@" -t1 2>&1 | norm > ../lu.txt
  "$P" list l.zpaq "$@" 2>&1 | norm > ../lo.txt
  if diff ../lu.txt ../lo.txt >/dev/null 2>&1; then ok; else bad "list $tag"; fi
}
listcmp "plain"
listcmp "compare same" .
listcmp "compare -force" . -force
listcmp "compare one dir" sub
listcmp "compare -only" -only '*.txt'
listcmp "compare -not" -not '='
printf 'alpha changed\n' > a.txt
listcmp "changed date" .
listcmp "changed -force" . -force
printf 'alpha\n' > a.txt
touch -d @$((NOW + 100)) a.txt
listcmp "changed time" .
cd ..

# ---- -index: create an index from an archive (extract -index F) ----
rm -rf ix
mkdir ix
cd ix || exit 1
printf 'uno\n' > a.txt
head -c 25000 /dev/urandom > b.bin
FAKE_TIME=$NOW LD_PRELOAD=$FT "$Z" add arc.zpaq a.txt b.bin -m1 -t1 >/dev/null 2>&1
printf 'dos dos\n' > a.txt
FAKE_TIME=$((NOW + 1000)) LD_PRELOAD=$FT "$Z" add arc.zpaq a.txt -m1 -t1 >/dev/null 2>&1
"$Z" extract arc.zpaq -index iu.zpaq -t1 >/dev/null 2>&1
"$P" extract arc.zpaq -index io.zpaq >/dev/null 2>&1
if cmp -s iu.zpaq io.zpaq; then ok; else bad "-index: create index"; fi
"$P" extract arc.zpaq -index io.zpaq >/dev/null 2>&1
rp=$?
"$Z" extract arc.zpaq -index iu.zpaq -t1 >/dev/null 2>&1
ru=$?
if [ "$rp" = "$ru" ]; then ok; else bad "-index: existing index rc"; fi
"$P" extract arc.zpaq -index io.zpaq -force >/dev/null 2>&1 && ok || bad "-index: -force"

# encrypted index
rm -f ka.zpaq kiU.zpaq kiO.zpaq
FAKE_TIME=$NOW FAKE_RANDOM=7 LD_PRELOAD=$FT \
  "$Z" add ka.zpaq a.txt b.bin -m1 -key pw -t1 >/dev/null 2>&1
"$Z" extract ka.zpaq -index kiU.zpaq -key pw -t1 >/dev/null 2>&1
"$P" extract ka.zpaq -index kiO.zpaq -key pw >/dev/null 2>&1
if cmp -s kiU.zpaq kiO.zpaq; then ok; else bad "-index: encrypted"; fi

# ---- -index: add to an indexed archive (one part per update) ----
rm -rf ia
mkdir ia
cd ia || exit 1
printf 'uno\n' > a.txt
head -c 25000 /dev/urandom > b.bin
FAKE_TIME=$NOW LD_PRELOAD=$FT "$Z" add 'u????.zpaq' a.txt b.bin -m1 -index uidx.zpaq -t1 >/dev/null 2>&1
ZPAQII_NOW=$NOW "$P" add 'o????.zpaq' a.txt b.bin -m1 -index oidx.zpaq >/dev/null 2>&1
if cmp -s u0001.zpaq o0001.zpaq && cmp -s uidx.zpaq oidx.zpaq; then
  ok
else
  bad "-index add: first part"
fi
printf 'dos dos dos\n' > a.txt
FAKE_TIME=$((NOW + 1000)) LD_PRELOAD=$FT \
  "$Z" add 'u????.zpaq' a.txt -m1 -index uidx.zpaq -t1 >/dev/null 2>&1
ZPAQII_NOW=$((NOW + 1000)) \
  "$P" add 'o????.zpaq' a.txt -m1 -index oidx.zpaq >/dev/null 2>&1
if cmp -s u0002.zpaq o0002.zpaq && cmp -s uidx.zpaq oidx.zpaq; then
  ok
else
  bad "-index add: second part"
fi
rm -rf xu xo
mkdir xu xo
"$Z" extract 'u????.zpaq' -to xu -t1 >/dev/null 2>&1
"$P" extract 'o????.zpaq' -to xo >/dev/null 2>&1
if diff -r xu xo >/dev/null 2>&1; then ok; else bad "-index multiply extract"; fi
cd ..
cd ..

# ---- threads: parallel compression must not change the archive ----
# 8 MB with 2 MB blocks -> several blocks, so the pool is really used.
rm -rf tp
mkdir tp
cd tp || exit 1
head -c 8000000 /dev/urandom > r.bin
yes "parallel compression test 0123456789" | head -c 8000000 > t.txt
for f in r.bin t.txt; do
  for t in 1 2 4 8; do
    rm -f "p$t.zpaq"
    ZPAQII_NOW=$NOW "$P" add "p$t.zpaq" "$f" -method x1.1.5.0.3.22 -t"$t" >/dev/null 2>&1
  done
  rm -f up.zpaq
  FAKE_TIME=$NOW LD_PRELOAD=$FT "$Z" add up.zpaq "$f" -method x1.1.5.0.3.22 -t4 >/dev/null 2>&1
  if cmp -s p1.zpaq p2.zpaq && cmp -s p1.zpaq p4.zpaq && cmp -s p1.zpaq p8.zpaq; then
    ok
  else
    bad "threads: $f differs across -t1/-t2/-t4/-t8"
  fi
  if cmp -s p1.zpaq up.zpaq; then ok; else bad "threads: $f differs from upstream -t4"; fi
done
rm -f th.zpaq
if ZPAQII_NOW=$NOW "$P" add th.zpaq t.txt -m1 -t4 >/dev/null 2>&1 &&
   ZPAQII_NOW=$NOW "$P" add th.zpaq r.bin -m1 -threads 2 >/dev/null 2>&1 &&
   "$P" extract th.zpaq -to xo -t3 >/dev/null 2>&1; then
  ok
else
  bad "-tN / -threads"
fi
cd ..


echo
echo "cli: $cases checks, $fails failures"
if [ $fails -ne 0 ]; then
  printf '  failed: %s\n' "${failed[@]}"
  exit 1
fi
echo "not covered yet (known gaps):"
echo "  -summary, -test, -repack, -all, -noattributes, convert"
