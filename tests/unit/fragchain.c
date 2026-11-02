/* zpaqII - fragment chains: the invariant the parallel fragmenter relies on.
 *
 * Scanning from a point that is a real fragment boundary must reproduce the
 * real chain from there, fragment by fragment, with the same hits and the same
 * order-1 table.  That is what lets a worker scan a range blind and have its
 * chain adopted the moment it meets the true one.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zq.h"

static unsigned rnd = 987654321u;
static int nextbyte(void) {
  rnd = rnd * 1103515245u + 12345u;
  return (int)((rnd >> 16) & 255u);
}

#define CAP 4096

int main(void) {
  const zq_i64 fine = 4 << 20;
  const zq_i64 maxfrag = 8128 << 6, minfrag = 64 << 6;
  const int fragment = 6;
  zq_u8 *buf = (zq_u8 *)malloc((size_t)fine);
  zq_fragcut *chain = (zq_fragcut *)malloc(sizeof(zq_fragcut) * CAP);
  zq_fragcut *single = (zq_fragcut *)malloc(sizeof(zq_fragcut) * CAP);
  zq_i64 *cutpos = (zq_i64 *)malloc(sizeof(zq_i64) * CAP);
  zq_i64 pos = 0, i;
  int ncuts = 0, done = 0, n, k, cases = 0, fails = 0;

  if (!buf || !chain || !single || !cutpos) return 2;

  /* A mixture of noise and text so the cuts are not all at maxfrag. */
  for (i = 0; i < fine; ++i) {
    if ((i >> 12) % 3 == 0)
      buf[i] = (zq_u8)("the quick brown fox jumps over the lazy dog ")[i % 45];
    else
      buf[i] = (zq_u8)nextbyte();
  }

  /* The true chain: one fragment at a time, like add() does it. */
  while (pos < fine && ncuts < CAP) {
    zq_fragscan st;
    const zq_i64 start = pos;
    int cut = 0;
    zq_fragscan_init(&st);
    while (pos < fine) {
      pos += zq_frag_scan(buf + pos, fine - pos, &st, maxfrag, minfrag,
                          fragment, &cut);
      if (cut) break;
    }
    if (!cut) break;
    cutpos[ncuts++] = start;
  }

  /* Every real cut is a valid entry point for a blind scan. */
  {
    const int step = ncuts > 40 ? ncuts / 40 : 1;
    for (k = 0; k < ncuts; k += step) {
      zq_fragscan st;
      zq_i64 p = cutpos[k];
      int cut = 0;

      memset(&st, 0, sizeof(st));
      zq_fragscan_init(&st);
      pos = p;
      while (pos < fine) {
        pos += zq_frag_scan(buf + pos, fine - pos, &st, maxfrag, minfrag,
                            fragment, &cut);
        if (cut) break;
      }
      single[0].start = p;
      single[0].len = pos - p;
      single[0].hits = st.hits;
      memcpy(single[0].o1, st.o1, 256);

      n = zq_frag_chain(buf, p, fine, fine, 0, maxfrag, minfrag, fragment,
                        chain, CAP, &done, NULL);
      ++cases;
      if (n < 1 || chain[0].start != single[0].start ||
          chain[0].len != single[0].len ||
          chain[0].hits != single[0].hits ||
          memcmp(chain[0].o1, single[0].o1, 256)) {
        printf("FAIL chain at %lld: got start=%lld len=%lld hits=%u\n",
               (long long)p, (long long)chain[0].start,
               (long long)chain[0].len, chain[0].hits);
        ++fails;
        continue;
      }
      /* And it continues with the same fragments as the true chain. */
      for (i = k + 1; i < ncuts && i < k + 8; ++i) {
        const int j = (int)(i - k);
        if (j >= n) break;
        if (chain[j].start != cutpos[i]) {
          printf("FAIL continuation at %lld frag %d: %lld != %lld\n",
                 (long long)p, j, (long long)chain[j].start,
                 (long long)cutpos[i]);
          ++fails;
          break;
        }
      }
    }
  }

  /* dopo: a chain asked to stop after 3 cuts past a point must do so. */
  {
    const zq_i64 stop = fine / 2;
    n = zq_frag_chain(buf, 0, fine, stop, 3, maxfrag, minfrag, fragment, chain,
                      CAP, &done, NULL);
    cases++;
    if (n < 1 || done || chain[n - 1].start + chain[n - 1].len < stop) {
      printf("FAIL dopo=3 near %lld: n=%d done=%d\n", (long long)stop, n, done);
      ++fails;
    }
    n = zq_frag_chain(buf, 0, fine, fine, 0, maxfrag, minfrag, fragment, chain,
                      CAP, &done, NULL);
    cases++;
    if (!done || n != ncuts) {
      printf("FAIL full chain: n=%d ncuts=%d done=%d\n", n, ncuts, done);
      ++fails;
    }
  }

  printf("fragchain: %d cases, %d failures\n", cases, fails);
  free(buf);
  free(chain);
  free(single);
  free(cutpos);
  return fails != 0;
}
