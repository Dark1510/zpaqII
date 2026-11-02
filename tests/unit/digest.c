/* zpaqII - CRC-32 and XXH64 against known vectors.
 *
 * Vectors generated with zlib.crc32 and the reference xxhash package
 * (seed 0), plus streaming splits that must give the same answer as one call.
 */
#include <stdio.h>
#include <string.h>

#include "zq.h"

static unsigned rnd = 24681357u;
static int nextbyte(void) {
  rnd = rnd * 1103515245u + 12345u;
  return (int)((rnd >> 16) & 255u);
}

struct vec {
  const char *s;
  unsigned crc;
  unsigned long long xxh;
};

static const struct vec vecs[] = {
    {"", 0x00000000u, 0xef46db3751d8e999ull},
    {"a", 0xe8b7be43u, 0xd24ec4f1a98c6e5bull},
    {"abc", 0x352441c2u, 0x44bc2cf5ad770999ull},
    {"0123456789", 0xa684c7c6u, 0x3f5fc178a81867e7ull},
    {"message digest", 0x20159d7fu, 0x066ed728fceeb3beull},
};

int main(void) {
  int i, cases = 0, fails = 0;
  zq_u8 big[1000];
  zq_crc32 c;
  zq_xxh64 x;

  for (i = 0; i < (int)(sizeof(vecs) / sizeof(vecs[0])); ++i) {
    const size_t n = strlen(vecs[i].s);
    zq_crc32_init(&c);
    zq_crc32_write(&c, vecs[i].s, n);
    zq_xxh64_init(&x, 0);
    zq_xxh64_write(&x, vecs[i].s, n);
    ++cases;
    if (zq_crc32_result(&c) != vecs[i].crc) {
      printf("FAIL crc32 \"%s\"\n", vecs[i].s);
      ++fails;
    }
    if (zq_xxh64_result(&x) != vecs[i].xxh) {
      printf("FAIL xxh64 \"%s\"\n", vecs[i].s);
      ++fails;
    }
  }

  /* Streaming in arbitrary pieces must equal one call. */
  for (i = 0; i < 1000; ++i) big[i] = (zq_u8)nextbyte();
  {
    int split;
    for (split = 0; split <= 64; ++split) {
      zq_u64 whole, piecewise;
      zq_u32 cw, cp;
      size_t n = 1000;
      zq_xxh64_init(&x, 0);
      zq_xxh64_write(&x, big, n);
      whole = zq_xxh64_result(&x);
      zq_crc32_init(&c);
      zq_crc32_write(&c, big, n);
      cw = zq_crc32_result(&c);

      zq_xxh64_init(&x, 0);
      zq_crc32_init(&c);
      {
        size_t off = 0;
        while (off < n) {
          size_t k = (size_t)split;
          if (k == 0) k = 1;
          if (off + k > n) k = n - off;
          zq_xxh64_write(&x, big + off, k);
          zq_crc32_write(&c, big + off, k);
          off += k;
        }
      }
      piecewise = zq_xxh64_result(&x);
      cp = zq_crc32_result(&c);
      ++cases;
      if (whole != piecewise || cw != cp) {
        printf("FAIL streaming split %d\n", split);
        ++fails;
      }
    }
  }

  /* Seeds other than 0 must change the answer. */
  {
    zq_u64 a, b;
    zq_xxh64_init(&x, 0);
    zq_xxh64_write(&x, big, 100);
    a = zq_xxh64_result(&x);
    zq_xxh64_init(&x, 1);
    zq_xxh64_write(&x, big, 100);
    b = zq_xxh64_result(&x);
    ++cases;
    if (a == b) {
      printf("FAIL seed has no effect\n");
      ++fails;
    }
  }

  printf("digest: %d cases, %d failures\n", cases, fails);
  return fails != 0;
}
