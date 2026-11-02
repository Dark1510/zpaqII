/* zpaqII - LZ4 block codec: roundtrip and interoperability.
 *
 * Our encoder's output must be readable by the reference decoder and the
 * reference encoder's output by ours (the block format is the standard one).
 * The reference library is only used by this test; the core has no dependency.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zq.h"

#ifdef HAVE_LZ4
#include <lz4.h>
#endif

static unsigned rnd = 13572468u;
static int nextbyte(void) {
  rnd = rnd * 1103515245u + 12345u;
  return (int)((rnd >> 16) & 255u);
}

static int fails;

static void check(int ok, const char *what, size_t n) {
  if (!ok) {
    printf("FAIL %s (n=%zu)\n", what, n);
    ++fails;
  }
}

int main(void) {
  const size_t maxn = 300000;
  unsigned char *src = (unsigned char *)malloc(maxn);
  unsigned char *comp = (unsigned char *)malloc(maxn * 2 + 64);
  unsigned char *back = (unsigned char *)malloc(maxn);
  size_t n;
  int cases = 0;

  if (!src || !comp || !back) return 2;

  for (n = 0; n <= maxn; n = n < 64 ? n + 1 : (size_t)(n * 3 / 2 + 7)) {
    size_t i;
    size_t mode = n % 3;
    int c, d;

    for (i = 0; i < n; ++i) {
      if (mode == 0) src[i] = (unsigned char)nextbyte();
      else if (mode == 1) src[i] = (unsigned char)("abcabcabcabcabcd"[i % 16]);
      else src[i] = (unsigned char)(i & 255);
    }

    c = zq_lz4_compress(src, n, comp, maxn * 2 + 64);
    check(c >= 0, "compress", n);
    if (c < 0) continue;
    d = zq_lz4_decompress(comp, (size_t)c, back, maxn);
    check(d == (int)n, "roundtrip size", n);
    check(d == (int)n && memcmp(src, back, n) == 0, "roundtrip data", n);
    ++cases;

#ifdef HAVE_LZ4
    {
      /* the reference must read ours ... */
      char *ref = (char *)malloc(maxn + 16);
      int r;
      if (!ref) return 2;
      r = LZ4_decompress_safe((const char *)comp, ref, c, (int)maxn);
      check(r == (int)n && memcmp(src, ref, n) == 0, "liblz4 reads ours", n);
      /* ... and we must read the reference's */
      r = LZ4_compress_default((const char *)src, ref, (int)n, (int)maxn + 16);
      if (r > 0) {
        d = zq_lz4_decompress(ref, (size_t)r, back, maxn);
        check(d == (int)n && memcmp(src, back, n) == 0,
              "we read liblz4's", n);
      }
      free(ref);
    }
#endif
  }

  /* truncated and corrupted input must be rejected, not crash */
  {
    size_t i;
    for (i = 0; i < 1000; ++i) src[i] = (unsigned char)("mississippi"[i % 11]);
    {
      int c = zq_lz4_compress(src, 1000, comp, maxn * 2 + 64);
      int d;
      check(c > 0, "corrupt: compress", 1000);
      d = zq_lz4_decompress(comp, (size_t)c - 1, back, maxn);
      (void)d; /* may or may not fail, but must not crash */
      comp[0] = (char)0xFF;
      comp[1] = (char)0xFF;
      d = zq_lz4_decompress(comp, (size_t)c, back, maxn);
      check(d < 0, "corrupt: rejected", 1000);
      ++cases;
    }
  }

  printf("lz4: %d cases, %d failures\n", cases, fails);
  free(src);
  free(comp);
  free(back);
  return fails != 0;
}
