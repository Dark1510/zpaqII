/* zpaqII - fragment scanner: the hot loop of add().
 *
 * zpaq cuts a file into fragments with a content-defined boundary.  While
 * reading a file it keeps, per fragment, a rolling hash (updated with one of
 * two multipliers depending on whether the byte equals its order-1
 * prediction), the count of correct predictions and the 256-byte order-1
 * table, and closes the fragment when the hash falls below 2^(22-fragment)
 * (once the fragment is at least minfrag long) or when it reaches maxfrag.
 *
 * The host used to do this byte by byte in Pascal; the same state machine in C
 * produces exactly the same cuts (and the same `hits` and `o1` table that the
 * block heuristics use) several times faster.
 */
#include "zq.h"

#include <string.h>

/* Start a fragment: all state at its initial value, like the beginning of the
 * loop in zpaq.cpp:add(). */
void zq_fragscan_init(zq_fragscan *st) {
  st->sz = 0;
  st->h = 0;
  st->hits = 0;
  st->c1 = 0;
  memset(st->o1, 0, sizeof(st->o1));
}

/* Scan up to n bytes for the end of the current fragment.
 *
 * Returns the number of bytes consumed.  On return st holds the state after
 * those bytes; if *cut is 1 the fragment ended there (its size is st->sz and
 * the caller must reset the state with zq_fragscan_init after using
 * st->hits/st->o1), otherwise the fragment is still open and the caller keeps
 * scanning with the next buffer. */
zq_i64 zq_frag_scan(const zq_u8 *buf, zq_i64 n, zq_fragscan *st,
                    zq_i64 maxfrag, zq_i64 minfrag, int fragment, int *cut) {
  const zq_u32 threshold =
      fragment <= 22 ? ((zq_u32)1 << (22 - fragment)) : 0;
  const int bycontent = fragment <= 22;
  zq_u32 h = st->h;
  zq_u32 hits = st->hits;
  int c1 = st->c1;
  zq_i64 sz = st->sz;
  zq_i64 i;

  *cut = 0;
  for (i = 0; i < n; ++i) {
    const zq_u32 c = buf[i];
    if (c == st->o1[c1]) {
      h = (h + c + 1) * 314159265u;
      ++hits;
    } else {
      h = (h + c + 1) * 271828182u;
    }
    st->o1[c1] = (zq_u8)c;
    c1 = (int)c;
    if (++sz >= maxfrag ||
        (bycontent && h < threshold && sz >= minfrag)) {
      st->h = h;
      st->hits = hits;
      st->c1 = c1;
      st->sz = sz;
      *cut = 1;
      return i + 1;
    }
  }
  st->h = h;
  st->hits = hits;
  st->c1 = c1;
  st->sz = sz;
  return n;
}
