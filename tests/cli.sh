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

# ---- short command aliases: a, x, l are documented abbreviations ----
rm -rf au ao
mkdir -p au ao
cp -r d au/d
cp -r d ao/d
(cd au && FAKE_TIME=$NOW LD_PRELOAD=$FT "$Z" a al.zpaq d -m1 -t1 >/dev/null 2>&1)
ru=$?
(cd ao && ZPAQII_NOW=$NOW "$P" a al.zpaq d -m1 >/dev/null 2>&1)
rp=$?
if [ "$ru" = "$rp" ] && cmp -s au/al.zpaq ao/al.zpaq; then ok; else
  bad "alias a (rc $ru/$rp)"
fi
rm -rf xu xo
mkdir xu xo
"$Z" x au/al.zpaq -to xu -t1 >/dev/null 2>&1
ru=$?
"$P" x ao/al.zpaq -to xo >/dev/null 2>&1
rp=$?
if [ "$ru" = "$rp" ] && diff -r xu xo >/dev/null 2>&1; then ok; else
  bad "alias x (rc $ru/$rp)"
fi
if diff <(cd au && "$Z" l al.zpaq -t1 2>&1 | norm) \
        <(cd ao && "$P" l al.zpaq 2>&1 | norm) >/dev/null 2>&1; then ok; else
  bad "alias l"
fi

# ---- -turbo: the parallel fragmenter must not change a byte ----
mkdir -p tb
head -c 3000000 /dev/urandom > tb/big1.bin
head -c 1000000 /dev/urandom > tb/small1.bin
rm -f tu.zpaq tp.zpaq tn.zpaq
FAKE_TIME=$NOW LD_PRELOAD=$FT "$Z" add tu.zpaq tb -m1 -t1 >/dev/null 2>&1
ZPAQII_NOW=$NOW ZPAQII_TURBO_MIN=1 "$P" add tp.zpaq tb -m1 -turbo 4 >/dev/null 2>&1
ZPAQII_NOW=$NOW "$P" add tn.zpaq tb -m1 -noturbo >/dev/null 2>&1
if cmp -s tu.zpaq tp.zpaq; then ok; else bad "-turbo archive"; fi
if cmp -s tp.zpaq tn.zpaq; then ok; else bad "-turbo vs -noturbo"; fi
rm -rf xu xo
mkdir xu xo
"$Z" x tu.zpaq -to xu -t1 >/dev/null 2>&1
"$P" x tp.zpaq -to xo >/dev/null 2>&1
if diff -r xu xo >/dev/null 2>&1; then ok; else bad "-turbo extract"; fi
# -715 pins 7.15: it must ignore -turbo and produce upstream's bytes
rm -f t715.zpaq
ZPAQII_NOW=$NOW ZPAQII_TURBO_MIN=1 "$P" add t715.zpaq tb -m1 -715 -turbo 8 \
  >/dev/null 2>&1
if cmp -s tu.zpaq t715.zpaq; then ok; else bad "-715 bytes"; fi
rm -rf tb

# ---- -hash: the digest list travels inside the archive, readable by 7.15 ----
rm -f hz.zpaq
ZPAQII_NOW=$NOW "$P" add hz.zpaq d -m1 -hash >/dev/null 2>&1
if "$Z" list hz.zpaq -t1 2>/dev/null | grep -q 'VFILE-hash.txt'; then ok
else bad "-hash: 7.15 does not list VFILE-hash.txt"; fi
rm -rf xu xo
mkdir xu xo
"$Z" x hz.zpaq -to xu -t1 >/dev/null 2>&1
"$P" x hz.zpaq -to xo >/dev/null 2>&1
if diff -r xu xo >/dev/null 2>&1 && diff -r d xu/d >/dev/null 2>&1; then ok
else bad "-hash: extraction"; fi
# one line for the header plus one per file (3 files in the corpus)
n=$(grep -c . xu/VFILE-hash.txt 2>/dev/null)
if [ "$n" = 4 ]; then ok; else bad "-hash: list has ${n:-0} lines"; fi

# ---- check: verifies the digest list without writing anything ----
rm -f ck.zpaq ckbad.zpaq
ZPAQII_NOW=$NOW "$P" add ck.zpaq d -m1 -hash >/dev/null 2>&1
"$P" check ck.zpaq >/dev/null 2>&1
r=$?
if [ "$r" = 0 ]; then ok; else bad "check healthy (rc $r)"; fi
cp ck.zpaq ckbad.zpaq
sz=$(stat -c%s ckbad.zpaq)
printf '\377' | dd of=ckbad.zpaq bs=1 seek=$((sz / 3)) conv=notrunc status=none
"$P" check ckbad.zpaq >/dev/null 2>&1
r=$?
if [ "$r" != 0 ]; then ok; else bad "check corrupted archive (rc 0)"; fi
rm -f nolist.zpaq
ZPAQII_NOW=$NOW "$P" add nolist.zpaq d/a.txt -m1 >/dev/null 2>&1
"$P" check nolist.zpaq >/dev/null 2>&1
r=$?
if [ "$r" = 2 ]; then ok; else bad "check without a list (rc $r)"; fi

# ---- -savelist / -checklist: export and import the digest list ----
rm -f hl.txt hlb.txt
"$P" check ck.zpaq -savelist hl.txt >/dev/null 2>&1
n=$(grep -c . hl.txt 2>/dev/null)
if [ "$n" = 4 ]; then ok; else bad "savelist has ${n:-0} lines"; fi
"$P" check ck.zpaq -checklist hl.txt >/dev/null 2>&1
r=$?
if [ "$r" = 0 ]; then ok; else bad "checklist (rc $r)"; fi
sed 's/^[0-9a-f]/f/' hl.txt > hlb.txt
"$P" check ck.zpaq -checklist hlb.txt >/dev/null 2>&1
r=$?
if [ "$r" = 1 ]; then ok; else bad "checklist tampered (rc $r)"; fi

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
listcmp "compare dir -force" sub -force
listcmp "compare one dir" sub
listcmp "compare -only" -only '*.txt'
listcmp "compare -not" -not '='
printf 'alpha changed\n' > a.txt
listcmp "changed date" .
listcmp "changed -force" . -force
printf 'alpha\n' > a.txt
touch -d @$((NOW + 100)) a.txt
listcmp "changed time" .
listcmp "-s2" -s2
listcmp "-summary 1" -summary 1
listcmp "-s-1" -s-1

# ---- -noattributes: attributes are not stored ----
rm -f na1.zpaq na2.zpaq
printf 'noattr\n' > na.txt
FAKE_TIME=$NOW LD_PRELOAD=$FT "$Z" add na1.zpaq na.txt -m1 -noattributes -t1 >/dev/null 2>&1
ZPAQII_NOW=$NOW "$P" add na2.zpaq na.txt -m1 -noattributes >/dev/null 2>&1
if cmp -s na1.zpaq na2.zpaq; then ok; else bad "-noattributes: add"; fi
# (list -noattributes is checked manually; see docs/PLAN.md)

# ---- -test: verify without writing ----
rm -rf tst tu tp
mkdir tst tu tp
"$P" extract l.zpaq -to tp -test >/dev/null 2>&1
rp=$?
"$Z" extract l.zpaq -to tu -test -t1 >/dev/null 2>&1
ru=$?
if [ "$rp" = "$ru" ] && [ -z "$(ls -A tp)" ]; then ok; else bad "-test: rc or files"; fi
if [ -z "$(ls -A tu)" ]; then ok; else bad "-test: upstream wrote files?"; fi
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
if diff <("$Z" list iu.zpaq -t1 2>&1 | norm | sed 's/iu\.zpaq/I/') \
        <("$P" list io.zpaq 2>&1 | norm | sed 's/io\.zpaq/I/') \
        >/dev/null 2>&1; then
  ok
else
  bad "list index archive"
fi
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


# ---- a missing archive is an error, like upstream's InputArchive::open ----
"$Z" list noexiste.zpaq >uu.txt 2>uu.err
ru=$?
"$P" list noexiste.zpaq >pp.txt 2>pp.err
rp=$?
if [ "$ru" = "$rp" ] &&
   diff <(norm <uu.txt) <(norm <pp.txt) >/dev/null 2>&1 &&
   diff <(norm <uu.err) <(norm <pp.err) >/dev/null 2>&1; then
  ok
else
  bad "missing archive (rc $ru/$rp)"
fi
"$Z" list 'no????.zpaq' >uu.txt 2>uu.err
ru=$?
"$P" list 'no????.zpaq' >pp.txt 2>pp.err
rp=$?
if [ "$ru" = "$rp" ] &&
   diff <(norm <uu.txt) <(norm <pp.txt) >/dev/null 2>&1 &&
   diff <(norm <uu.err) <(norm <pp.err) >/dev/null 2>&1; then
  ok
else
  bad "missing multi-part archive (rc $ru/$rp)"
fi

# ---- usage: unknown options and commands match upstream ----
usagecmp() {  # tag, args...
  local tag=$1
  shift
  "$Z" "$@" >uu.txt 2>uu.err
  ru=$?
  "$P" "$@" >pp.txt 2>pp.err
  rp=$?
  if [ "$ru" = "$rp" ] &&
     diff <(norm <uu.txt | sed -E 's/-until [0-9-]+ [0-9:]+/-until DATE/') \
          <(norm <pp.txt | sed -E 's/-until [0-9-]+ [0-9:]+/-until DATE/') \
          >/dev/null 2>&1 &&
     diff <(norm <uu.err) <(norm <pp.err) >/dev/null 2>&1; then
    ok
  else
    bad "usage $tag (rc $ru/$rp)"
  fi
}
usagecmp "no args"
usagecmp "-h" -h
usagecmp "help" help
usagecmp "unknown option" list base.zpaq -zzz
usagecmp "unknown command" foo bar
usagecmp "-m with a separate value" add mx.zpaq d/a.txt -m 5

# ---- a bare -m keeps the default method, like 7.15 ----
rm -f ma.zpaq mb.zpaq
FAKE_TIME=$NOW LD_PRELOAD=$FT "$Z" add ma.zpaq d/a.txt -m -t1 >/dev/null 2>&1
ZPAQII_NOW=$NOW "$P" add mb.zpaq d/a.txt -m >/dev/null 2>&1
if cmp -s ma.zpaq mb.zpaq; then ok; else bad "bare -m"; fi

# ---- convert: upstream parses it but has no command, so usage + exit 1 ----
"$Z" convert base.zpaq >/dev/null 2>&1
ru=$?
"$P" convert base.zpaq >/dev/null 2>&1
rp=$?
if [ "$ru" = "$rp" ]; then ok; else bad "convert rc ($ru vs $rp)"; fi

# ---- -strict: command-line errors exit 2 (our extension) ----
strict_rc() {  # tag, expected, args...
  local tag=$1 exp=$2
  shift 2
  "$P" "$@" >/dev/null 2>&1
  local r=$?
  if [ "$r" = "$exp" ]; then ok; else bad "strict $tag (rc $r != $exp)"; fi
}
strict_rc "misplaced argument" 2 add s1.zpaq d/a.txt -m5 basura -strict
strict_rc "unknown option" 2 list base.zpaq -zzz -strict
strict_rc "unknown command" 2 foo bar -strict
strict_rc "no archive" 2 list -strict
strict_rc "bad value" 2 add s2.zpaq d/a.txt -fragment abc -strict
rm -f s1.zpaq s2.zpaq

# ---- -715 wins over -strict: the line behaves like 7.15 again ----
"$P" list base.zpaq -zzz -715 -strict >/dev/null 2>&1
rp=$?
"$Z" list base.zpaq -zzz -t1 >/dev/null 2>&1
ru=$?
if [ "$rp" = "$ru" ]; then ok; else bad "-715 over -strict (rc $rp vs $ru)"; fi

# ---- a bad -fragment value is atoi(0), like upstream ----
rm -f fb1.zpaq fb2.zpaq
FAKE_TIME=$NOW LD_PRELOAD=$FT "$Z" add fb1.zpaq d/a.txt -m1 -fragment abc -t1 \
  >/dev/null 2>&1
ZPAQII_NOW=$NOW "$P" add fb2.zpaq d/a.txt -m1 -fragment abc >/dev/null 2>&1
if cmp -s fb1.zpaq fb2.zpaq; then ok; else bad "-fragment abc bytes"; fi

# ---- -all N: one directory per version ----
rm -rf av
mkdir av
cd av || exit 1
printf 'v1\n' > f.txt
touch -d @$((NOW + 1000)) f.txt
ZPAQII_NOW=$((NOW + 1000)) "$P" add v.zpaq f.txt -m1 >/dev/null 2>&1
printf 'v2 v2\n' > f.txt
touch -d @$((NOW + 2000)) f.txt
ZPAQII_NOW=$((NOW + 2000)) "$P" add v.zpaq f.txt -m1 >/dev/null 2>&1
for n in 1 2 4; do
  if diff <("$Z" list v.zpaq -all "$n" -t1 2>&1 | norm) \
          <("$P" list v.zpaq -all "$n" 2>&1 | norm) >/dev/null 2>&1; then
    ok
  else
    bad "-all $n list"
  fi
done
rm -rf ay az
mkdir ay az
"$Z" extract v.zpaq -to ay -all 2 -t1 >/dev/null 2>&1
"$P" extract v.zpaq -to az -all 2 >/dev/null 2>&1
if diff -r ay az >/dev/null 2>&1; then ok; else bad "-all 2 extract"; fi
cd ..

# ---- argument order: files are read only between the archive and the options ----
rm -rf ag1 ag2
mkdir ag1 ag2
(cd ag1 && cp ../d/a.txt . && "$Z" add a.zpaq -m1 a.txt >log 2>&1); ru=$?
(cd ag2 && cp ../d/a.txt . && "$P" add a.zpaq -m1 a.txt >log 2>&1); rp=$?
if [ "$ru" = "$rp" ] && [ ! -f ag1/a.zpaq ] && [ ! -f ag2/a.zpaq ]; then
  ok
else
  bad "argument order: file after an option (rc $ru/$rp)"
fi
(cd ag1 && "$Z" add "" a.txt >log 2>&1); ru=$?
(cd ag2 && "$P" add "" a.txt >log 2>&1); rp=$?
if [ "$ru" = "$rp" ] && [ ! -f ag1/.zpaq ] && [ ! -f ag2/.zpaq ]; then
  ok
else
  bad "empty archive name (dry run)"
fi

# ---- add/extract console output (progress, +/#/-, >/?/=) ----
# Normalise what cannot match: banner, timing, and the ETA prefix of the
# -summary progress lines.
norm2() {
  grep -v -e '^zpaq v' -e ' seconds (' |
    sed -E 's/^ *[0-9]+\.[0-9]+% [0-9]+:[0-9][0-9]:[0-9][0-9] //'
}
rm -rf au ap
mkdir au ap
printf 'uno\n' > au/s1.txt
head -c 8000 /dev/urandom > au/s2.bin
touch -d @$NOW au/s1.txt au/s2.bin
cp -p au/s1.txt au/s2.bin ap/
(cd au && FAKE_TIME=$NOW LD_PRELOAD=$FT \
  "$Z" add a.zpaq s1.txt s2.bin -m1 -t1 >log 2>err)
ru=$?
(cd ap && ZPAQII_NOW=$NOW "$P" add a.zpaq s1.txt s2.bin -m1 -t1 >log 2>err)
rp=$?
if [ "$ru" = "$rp" ] && cmp -s au/a.zpaq ap/a.zpaq &&
   diff <(norm2 <au/log) <(norm2 <ap/log) >/dev/null 2>&1 &&
   diff <(norm2 <au/err) <(norm2 <ap/err) >/dev/null 2>&1; then
  ok
else
  bad "add output (rc $ru/$rp)"
fi

# update: one changed file (#) and one removed (-)
printf 'dos\n' > au/s2.bin
printf 'dos\n' > ap/s2.bin
touch -d @$((NOW + 100)) au/s2.bin ap/s2.bin
rm -f au/s1.txt ap/s1.txt
(cd au && FAKE_TIME=$((NOW + 100)) LD_PRELOAD=$FT \
  "$Z" add a.zpaq s2.bin -m1 -t1 >log 2>err)
ru=$?
(cd ap && ZPAQII_NOW=$((NOW + 100)) \
  "$P" add a.zpaq s2.bin -m1 -t1 >log 2>err)
rp=$?
if [ "$ru" = "$rp" ] && cmp -s au/a.zpaq ap/a.zpaq &&
   diff <(norm2 <au/log) <(norm2 <ap/log) >/dev/null 2>&1 &&
   diff <(norm2 <au/err) <(norm2 <ap/err) >/dev/null 2>&1; then
  ok
else
  bad "add update output (rc $ru/$rp)"
fi

# -summary 1 keeps only the progress line
(cd au && FAKE_TIME=$NOW LD_PRELOAD=$FT \
  "$Z" add s.zpaq s2.bin -m1 -summary 1 -t1 >log 2>&1)
ru=$?
(cd ap && ZPAQII_NOW=$NOW "$P" add s.zpaq s2.bin -m1 -summary 1 -t1 >log 2>&1)
rp=$?
if [ "$ru" = "$rp" ] && cmp -s au/s.zpaq ap/s.zpaq &&
   diff <(norm2 <au/log) <(norm2 <ap/log) >/dev/null 2>&1; then
  ok
else
  bad "add -summary output"
fi

# extract: "Extracting ...", block lines and "> file"
rm -rf eu ep
mkdir eu ep
"$Z" extract base.zpaq -to eu -t1 >eu.log 2>&1
ru=$?
"$P" extract base.zpaq -to ep -t1 >ep.log 2>&1
rp=$?
if [ "$ru" = "$rp" ] && diff -r eu ep >/dev/null 2>&1 &&
   diff <(norm2 <eu.log | sed 's/eu\//ep\//g') <(norm2 <ep.log) >/dev/null 2>&1
then
  ok
else
  bad "extract output (rc $ru/$rp)"
fi

# extracting again: "? file" for existing files, then 0 files
"$Z" extract base.zpaq -to eu -t1 >eu.log 2>&1
ru=$?
"$P" extract base.zpaq -to ep -t1 >ep.log 2>&1
rp=$?
if [ "$ru" = "$rp" ] &&
   diff <(norm2 <eu.log | sed 's/eu\//ep\//g') <(norm2 <ep.log) >/dev/null 2>&1
then
  ok
else
  bad "extract existing output (rc $ru/$rp)"
fi

# -force over identical files: "= file"
"$Z" extract base.zpaq -to eu -force -t1 >eu.log 2>&1
ru=$?
"$P" extract base.zpaq -to ep -force -t1 >ep.log 2>&1
rp=$?
if [ "$ru" = "$rp" ] &&
   diff <(norm2 <eu.log | sed 's/eu\//ep\//g') <(norm2 <ep.log) >/dev/null 2>&1
then
  ok
else
  bad "extract -force identical output (rc $ru/$rp)"
fi

# -force compares fragments by SHA-1, so it also works for preprocessed blocks
# (E8E9/LZ77): an archive of binary data is "=" without re-extracting.
rm -rf fx1 fx2
mkdir fx1 fx2
FAKE_TIME=$NOW LD_PRELOAD=$FT "$Z" add fbase.zpaq d/sub/b.bin d/a.txt -m1 -t1 \
  >/dev/null 2>&1
"$Z" extract fbase.zpaq -to fx1 -t1 >/dev/null 2>&1
"$P" extract fbase.zpaq -to fx2 -t1 >/dev/null 2>&1
"$Z" extract fbase.zpaq -to fx1 -force -t1 >eu.log 2>&1
ru=$?
"$P" extract fbase.zpaq -to fx2 -force -t1 >ep.log 2>&1
rp=$?
if [ "$ru" = "$rp" ] &&
   diff <(norm2 <eu.log | sed 's/fx1/fxX/g') \
        <(norm2 <ep.log | sed 's/fx2/fxX/g') >/dev/null 2>&1; then
  ok
else
  bad "extract -force with preprocessed blocks"
fi

# extract -only / -not: selection and output
rm -rf eo1 eo2
mkdir eo1 eo2
"$Z" extract base.zpaq -to eo1 -only 'd/a.txt' -t1 >eu.log 2>&1
ru=$?
"$P" extract base.zpaq -to eo2 -only 'd/a.txt' -t1 >ep.log 2>&1
rp=$?
if [ "$ru" = "$rp" ] && diff -r eo1 eo2 >/dev/null 2>&1 &&
   diff <(norm2 <eu.log | sed 's/eo1/eoX/g') \
        <(norm2 <ep.log | sed 's/eo2/eoX/g') >/dev/null 2>&1; then
  ok
else
  bad "extract -only output"
fi
rm -rf eo1 eo2
mkdir eo1 eo2
"$Z" extract base.zpaq -to eo1 -not d/sub -t1 >eu.log 2>&1
ru=$?
"$P" extract base.zpaq -to eo2 -not d/sub -t1 >ep.log 2>&1
rp=$?
if [ "$ru" = "$rp" ] && diff -r eo1 eo2 >/dev/null 2>&1 &&
   diff <(norm2 <eu.log | sed 's/eo1/eoX/g') \
        <(norm2 <ep.log | sed 's/eo2/eoX/g') >/dev/null 2>&1; then
  ok
else
  bad "extract -not output"
fi

# extract into a read-only directory: one perror line per file, keep going, 1
rm -rf ro1 ro2
mkdir ro1 ro2
mkdir -p ro1/d ro2/d
chmod 555 ro1/d ro2/d
"$Z" extract base.zpaq -to ro1 -t1 >eu.log 2>eu.err
ru=$?
"$P" extract base.zpaq -to ro2 -t1 >ep.log 2>ep.err
rp=$?
chmod 755 ro1/d ro2/d
if [ "$ru" = "$rp" ] &&
   diff <(norm2 <eu.log | sed 's/ro1/roX/g') \
        <(norm2 <ep.log | sed 's/ro2/roX/g') >/dev/null 2>&1 &&
   diff <(norm2 <eu.err | sed 's/ro1/roX/g') \
        <(norm2 <ep.err | sed 's/ro2/roX/g') >/dev/null 2>&1; then
  ok
else
  bad "extract permission errors (rc $ru/$rp)"
fi

# list -s2: duplicates marked "^" and counted
if diff <("$Z" list base.zpaq -s2 -t1 2>&1 | norm) \
        <("$P" list base.zpaq -s2 2>&1 | norm) >/dev/null 2>&1; then
  ok
else
  bad "list -s2 duplicates"
fi

# ---- add/extract error paths (same message and exit code) ----
rm -f st.zpaq j.zpaq
FAKE_TIME=$NOW LD_PRELOAD=$FT "$Z" add st.zpaq d/a.txt -method s1.0 -t1 >/dev/null 2>&1
FAKE_TIME=$NOW LD_PRELOAD=$FT "$Z" add j.zpaq d/a.txt -m1 -t1 >/dev/null 2>&1
errcmp() {  # tag, args...
  local tag=$1
  shift
  rm -rf e1 e2
  mkdir e1 e2
  (cd e1 && cp -p ../d/a.txt . 2>/dev/null
   FAKE_TIME=$NOW LD_PRELOAD=$FT "$Z" "$@" >log 2>err)
  ru=$?
  (cd e2 && cp -p ../d/a.txt . 2>/dev/null
   ZPAQII_NOW=$NOW "$P" "$@" >log 2>err)
  rp=$?
  if [ "$ru" = "$rp" ] &&
     diff <(norm2 <e1/log) <(norm2 <e2/log) >/dev/null 2>&1 &&
     diff <(norm2 <e1/err) <(norm2 <e2/err) >/dev/null 2>&1; then
    ok
  else
    bad "error path $tag (rc $ru/$rp)"
  fi
}
errcmp "-index with -method s" add a.zpaq a.txt -method s1.0 -index x.idx
errcmp "journaling add on a streaming archive" add st.zpaq a.txt -m1
errcmp "streaming add on a journaling archive" add j.zpaq a.txt -method s1.0
errcmp "add with -until N" add base.zpaq a.txt -until 1
errcmp "-until with an index" add base.zpaq a.txt -until 1 -index x.idx

# a symbolic link is not a regular file: upstream skips it silently
rm -rf sl1 sl2
mkdir sl1 sl2
printf 'target\n' > sl1/t.txt
printf 'target\n' > sl2/t.txt
ln -s t.txt sl1/l.txt
ln -s t.txt sl2/l.txt
touch -h -d @$NOW sl1/t.txt sl1/l.txt sl2/t.txt sl2/l.txt 2>/dev/null
(cd sl1 && FAKE_TIME=$NOW LD_PRELOAD=$FT "$Z" add a.zpaq t.txt l.txt -m1 -t1 >log 2>&1)
ru=$?
(cd sl2 && ZPAQII_NOW=$NOW "$P" add a.zpaq t.txt l.txt -m1 -t1 >log 2>&1)
rp=$?
if [ "$ru" = "$rp" ] && cmp -s sl1/a.zpaq sl2/a.zpaq &&
   diff <(norm2 <sl1/log) <(norm2 <sl2/log) >/dev/null 2>&1; then
  ok
else
  bad "add skips symbolic links (rc $ru/$rp)"
fi

# ---- add: selection and -to rename ----
rm -rf su sp
mkdir -p su/d1 su/d2 sp/d1 sp/d2
printf 'a\n' > su/d1/a.txt
printf 'b\n' > su/d1/b.txt
printf 'c\n' > su/d2/c.txt
touch -d @$NOW su/d1/a.txt su/d1/b.txt su/d2/c.txt
cp -p su/d1/a.txt su/d1/b.txt sp/d1/
cp -p su/d2/c.txt sp/d2/
addsel() {  # tag, args...
  local tag=$1; shift
  rm -f su/x.zpaq sp/x.zpaq
  (cd su && FAKE_TIME=$NOW LD_PRELOAD=$FT "$Z" add x.zpaq "$@" -m1 -t1 >log 2>err)
  ru=$?
  (cd sp && ZPAQII_NOW=$NOW "$P" add x.zpaq "$@" -m1 -t1 >log 2>err)
  rp=$?
  if [ "$ru" = "$rp" ] && cmp -s su/x.zpaq sp/x.zpaq &&
     diff <(norm2 <su/log) <(norm2 <sp/log) >/dev/null 2>&1 &&
     diff <(norm2 <su/err) <(norm2 <sp/err) >/dev/null 2>&1; then
    ok
  else
    bad "add $tag (rc $ru/$rp)"
  fi
}
addsel "-only" d1 -only '*.txt'
addsel "-not" d1 d2 -not d2
addsel "-to rename" d1 -to out
addsel "-to several" d1/a.txt d2/c.txt -to x.a y.c

# add: an unreadable input file is reported and skipped (exit code 1)
rm -rf uu up
mkdir uu up
printf 'ok\n' > uu/f.txt
printf 'no\n' > uu/noread.txt
touch -d @$NOW uu/f.txt uu/noread.txt
cp -p uu/f.txt uu/noread.txt up/
chmod 000 uu/noread.txt up/noread.txt
(cd uu && FAKE_TIME=$NOW LD_PRELOAD=$FT "$Z" add a.zpaq f.txt noread.txt -m1 -t1 >log 2>err)
ru=$?
(cd up && ZPAQII_NOW=$NOW "$P" add a.zpaq f.txt noread.txt -m1 -t1 >log 2>err)
rp=$?
chmod 644 uu/noread.txt up/noread.txt
if [ "$ru" = "$rp" ] && cmp -s uu/a.zpaq up/a.zpaq &&
   diff <(norm2 <uu/log) <(norm2 <up/log) >/dev/null 2>&1 &&
   diff <(norm2 <uu/err) <(norm2 <up/err) >/dev/null 2>&1; then
  ok
else
  bad "add unreadable file (rc $ru/$rp)"
fi

# ---- streaming add and extract output ----
rm -rf stu stp
mkdir stu stp
printf 'uno dos tres\n' > stu/f.txt
head -c 5000 /dev/urandom > stu/g.bin
touch -d @$NOW stu/f.txt stu/g.bin
cp -p stu/f.txt stu/g.bin stp/
(cd stu && FAKE_TIME=$NOW LD_PRELOAD=$FT \
  "$Z" add s.zpaq f.txt g.bin -method s1.0 -t1 >log 2>err)
ru=$?
(cd stp && ZPAQII_NOW=$NOW "$P" add s.zpaq f.txt g.bin -method s1.0 -t1 >log 2>err)
rp=$?
if [ "$ru" = "$rp" ] && cmp -s stu/s.zpaq stp/s.zpaq &&
   diff <(norm2 <stu/log) <(norm2 <stp/log) >/dev/null 2>&1 &&
   diff <(norm2 <stu/err) <(norm2 <stp/err) >/dev/null 2>&1; then
  ok
else
  bad "streaming add output (rc $ru/$rp)"
fi
rm -rf stu.out stp.out
mkdir stu.out stp.out
(cd stu && "$Z" extract s.zpaq -to ../stu.out -t1 >log 2>&1)
ru=$?
(cd stp && "$P" extract s.zpaq -to ../stp.out -t1 >log 2>&1)
rp=$?
if [ "$ru" = "$rp" ] && diff -r stu.out stp.out >/dev/null 2>&1 &&
   diff <(norm2 <stu/log | sed 's/stu\.out/OUT/g') \
        <(norm2 <stp/log | sed 's/stp\.out/OUT/g') >/dev/null 2>&1; then
  ok
else
  bad "streaming extract output (rc $ru/$rp)"
fi

# ---- add stamps the transaction with UTC, like upstream ----
rm -rf tzd
mkdir tzd
cd tzd || exit 1
printf 'tz\n' > f.txt
TZ=US/Alaska "$P" add t.zpaq f.txt -m1 >/dev/null 2>&1
ts=$(head -c 160 t.zpaq | tr -c '[:print:]' '\n' |
     sed -n 's/^jDC\([0-9][0-9]*\)c.*/\1/p' | head -1)
iso=$(printf '%s' "$ts" |
      sed -E 's/^(....)(..)(..)(..)(..)(..)$/\1-\2-\3 \4:\5:\6/')
delta=$(( $(date -u -d "$iso" +%s) - $(date +%s) ))
if [ -n "$ts" ] && [ "$delta" -ge -5 ] && [ "$delta" -le 5 ]; then
  ok
else
  bad "add date is not UTC (ts=$ts delta=$delta)"
fi
cd ..

# ---- -repack F [X]: rewrite the archive into a new file ----
repack_both() {  # name, new key (may be empty), args...
  local n=$1 nk=$2
  shift 2
  rm -rf rp1 rp2
  mkdir rp1 rp2
  (cd rp1 && "$Z" extract "$@" -repack out.zpaq $nk -t1 >log 2>&1)
  ru=$?
  (cd rp2 && "$P" extract "$@" -repack out.zpaq $nk >log 2>&1)
  rp=$?
  if [ "$ru" = "$rp" ] && cmp -s rp1/out.zpaq rp2/out.zpaq &&
     diff <(norm <rp1/log) <(norm <rp2/log) >/dev/null 2>&1; then
    ok
  else
    bad "$n (rc $ru/$rp)"
  fi
}
repack_both "-repack plain" "" ../base.zpaq
repack_both "-repack -all" "" ../base.zpaq -all
FAKE_RANDOM=31 LD_PRELOAD=$FT repack_both "-repack new key" nueva ../base.zpaq
FAKE_RANDOM=32 LD_PRELOAD=$FT repack_both "-repack keeps the key" secret ../ku.zpaq -key secret
rm -rf rpx
mkdir rpx
if "$P" extract rp1/out.zpaq -to rpx -key secret >/dev/null 2>&1 &&
   diff -r d rpx/d >/dev/null 2>&1; then ok; else bad "-repack: round trip"; fi

rm -rf rp1 rp2
mkdir rp1 rp2
(cd rp1 && "$Z" extract ../base.zpaq -all -only d/a.txt -repack out.zpaq -t1 >log 2>&1)
ru=$?
(cd rp2 && "$P" extract ../base.zpaq -all -only d/a.txt -repack out.zpaq >log 2>&1)
rp=$?
if [ "$ru" = "$rp" ] && diff <(norm <rp1/log) <(norm <rp2/log) >/dev/null 2>&1; then
  ok
else
  bad "-repack -all partial copy"
fi

printf 'x' > rp1/exists.zpaq
printf 'x' > rp2/exists.zpaq
(cd rp1 && "$Z" extract ../base.zpaq -repack exists.zpaq -t1 >log 2>&1); ru=$?
(cd rp2 && "$P" extract ../base.zpaq -repack exists.zpaq >log 2>&1); rp=$?
if [ "$ru" = "$rp" ] && diff <(norm <rp1/log) <(norm <rp2/log) >/dev/null 2>&1; then
  ok
else
  bad "-repack existing output (rc $ru/$rp)"
fi

echo
echo "cli: $cases checks, $fails failures"
if [ $fails -ne 0 ]; then
  printf '  failed: %s\n' "${failed[@]}"
  exit 1
fi
echo "not covered yet (known gaps):"
echo "  (none)"
