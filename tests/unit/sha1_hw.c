/* zpaqII - SHA-1: hardware path against the portable one.
 *
 * The x86 SHA extensions are only used when the CPU reports them, so on most
 * machines this test has nothing to compare and says so.  Run it under
 * `qemu-x86_64 -cpu max` (make test-sha1hw does) to exercise the hardware
 * path on a machine without SHA-NI.
 */
#include <stdio.h>
#include <string.h>

#include "zq.h"

/* internal hook, src/core/sha1.c */
void zq_sha1_force_portable(int on);
unsigned zq_cpu_features(void);
#define ZQ_CPU_SHA1 1u

static unsigned rnd = 12345u;
static int nextbyte(void) {
  rnd = rnd * 1103515245u + 12345u;
  return (int)((rnd >> 16) & 255u);
}

static void digest(const unsigned char *b, long n, char out[20], int portable) {
  zq_sha1 *s = zq_sha1_new();
  zq_sha1_force_portable(portable);
  zq_sha1_init(s);
  zq_sha1_write(s, (const char *)b, n);
  memcpy(out, zq_sha1_result(s), 20);
  zq_sha1_force_portable(0);
  zq_sha1_free(s);
}

int main(void) {
  static unsigned char buf[1 << 20];
  char a[20], b[20];
  long n, len;
  int i, cases = 0, fails = 0;

  if ((zq_cpu_features() & ZQ_CPU_SHA1) == 0) {
    printf("sha1 hw: not available on this CPU (skipped)\n");
    return 0;
  }

  /* every length up to a few blocks, then random larger ones */
  for (n = 0; n <= 300; ++n) {
    for (i = 0; i < n; ++i) buf[i] = (unsigned char)nextbyte();
    digest(buf, n, a, 0);
    digest(buf, n, b, 1);
    ++cases;
    if (memcmp(a, b, 20)) {
      printf("FAIL length %ld\n", n);
      ++fails;
    }
  }
  for (i = 0; i < 60; ++i) {
    len = (long)((unsigned)nextbyte() << 16 | (unsigned)nextbyte() << 8 |
                 (unsigned)nextbyte()) % (1 << 20);
    for (n = 0; n < len; ++n) buf[n] = (unsigned char)nextbyte();
    digest(buf, len, a, 0);
    digest(buf, len, b, 1);
    ++cases;
    if (memcmp(a, b, 20)) {
      printf("FAIL length %ld\n", len);
      ++fails;
    }
  }

  printf("sha1 hw: %d cases, %d failures\n", cases, fails);
  return fails != 0;
}
