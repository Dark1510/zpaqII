/* zpaqII - SHA-1.  Port of libzpaq 7.15 (Unlicense, Matt Mahoney). */
#include "zq.h"

#include <string.h>

void zq_sha1_init(zq_sha1 *s) {
  s->len = 0;
  s->h[0] = 0x67452301u;
  s->h[1] = 0xEFCDAB89u;
  s->h[2] = 0x98BADCFEu;
  s->h[3] = 0x10325476u;
  s->h[4] = 0xC3D2E1F0u;
  memset(s->w, 0, sizeof(s->w));
}

/* Processes one 64-byte block already loaded into w[].  The 80 rounds are
 * unrolled exactly like upstream (macros with constant indices), which is what
 * lets the compiler keep a..e in registers and emit plain rotate/add code. */
static void sha1_process(zq_sha1 *s) {
  zq_u32 a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4];
  static const zq_u32 k[4] = {0x5A827999u, 0x6ED9EBA1u, 0x8F1BBCDCu,
                              0xCA62C1D6u};
  zq_u32 *w = s->w;
#define ZQ_SHA1_F(a, b, c, d, e, i)                                           \
  if ((i) >= 16) {                                                            \
    w[(i) & 15] ^= w[((i) - 3) & 15] ^ w[((i) - 8) & 15] ^ w[((i) - 14) & 15]; \
    w[(i) & 15] = w[(i) & 15] << 1 | w[(i) & 15] >> 31;                        \
  }                                                                           \
  e += ((a) << 5 | (a) >> 27) + k[(i) / 20] + w[(i) & 15] +                    \
       (((i) % 40 >= 20) ? (b) ^ (c) ^ (d)                                    \
                         : ((i) >= 40 ? ((b) & (c)) | ((d) & ((b) | (c)))     \
                                      : (d) ^ ((b) & ((c) ^ (d)))));          \
  b = (b) << 30 | (b) >> 2;
#define ZQ_SHA1_R(i)                                                          \
  ZQ_SHA1_F(a, b, c, d, e, i) ZQ_SHA1_F(e, a, b, c, d, (i) + 1)               \
  ZQ_SHA1_F(d, e, a, b, c, (i) + 2) ZQ_SHA1_F(c, d, e, a, b, (i) + 3)         \
  ZQ_SHA1_F(b, c, d, e, a, (i) + 4)
  ZQ_SHA1_R(0) ZQ_SHA1_R(5) ZQ_SHA1_R(10) ZQ_SHA1_R(15) ZQ_SHA1_R(20)
  ZQ_SHA1_R(25) ZQ_SHA1_R(30) ZQ_SHA1_R(35) ZQ_SHA1_R(40) ZQ_SHA1_R(45)
  ZQ_SHA1_R(50) ZQ_SHA1_R(55) ZQ_SHA1_R(60) ZQ_SHA1_R(65) ZQ_SHA1_R(70)
  ZQ_SHA1_R(75)
#undef ZQ_SHA1_R
#undef ZQ_SHA1_F
  s->h[0] += a;
  s->h[1] += b;
  s->h[2] += c;
  s->h[3] += d;
  s->h[4] += e;
}

void zq_sha1_put(zq_sha1 *s, int c) {
  zq_u32 *r = &s->w[(zq_u32)s->len >> 5 & 15];
  *r = (*r << 8) | ((zq_u32)c & 255u);
  s->len += 8;
  if (((zq_u32)s->len & 511u) == 0) sha1_process(s);
}

void zq_sha1_write(zq_sha1 *s, const char *buf, zq_i64 n) {
  const unsigned char *p = (const unsigned char *)buf;
  for (; n > 0 && ((zq_u32)s->len & 511u) != 0; --n) zq_sha1_put(s, *p++);
  for (; n >= 64; n -= 64) {
    int i;
    for (i = 0; i < 16; ++i) {
      s->w[i] = ((zq_u32)p[0] << 24) | ((zq_u32)p[1] << 16) |
                ((zq_u32)p[2] << 8) | (zq_u32)p[3];
      p += 4;
    }
    s->len += 512;
    sha1_process(s);
  }
  for (; n > 0; --n) zq_sha1_put(s, *p++);
}

const char *zq_sha1_result(zq_sha1 *s) {
  const zq_u64 len = s->len;
  int i;
  zq_sha1_put(s, 0x80);
  while ((s->len & 511u) != 448u) zq_sha1_put(s, 0);
  zq_sha1_put(s, (int)((len >> 56) & 255u));
  zq_sha1_put(s, (int)((len >> 48) & 255u));
  zq_sha1_put(s, (int)((len >> 40) & 255u));
  zq_sha1_put(s, (int)((len >> 32) & 255u));
  zq_sha1_put(s, (int)((len >> 24) & 255u));
  zq_sha1_put(s, (int)((len >> 16) & 255u));
  zq_sha1_put(s, (int)((len >> 8) & 255u));
  zq_sha1_put(s, (int)(len & 255u));
  for (i = 0; i < 5; ++i) {
    s->hbuf[4 * i + 0] = (char)(s->h[i] >> 24);
    s->hbuf[4 * i + 1] = (char)(s->h[i] >> 16);
    s->hbuf[4 * i + 2] = (char)(s->h[i] >> 8);
    s->hbuf[4 * i + 3] = (char)(s->h[i]);
  }
  zq_sha1_init(s);
  return s->hbuf;
}

double zq_sha1_size(const zq_sha1 *s) { return (double)(s->len / 8); }
zq_u64 zq_sha1_usize(const zq_sha1 *s) { return s->len / 8; }
