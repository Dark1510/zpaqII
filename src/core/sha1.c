/* zpaqII — SHA-1.  Port de libzpaq 7.15 (Unlicense, Matt Mahoney). */
#include "zq.h"

#include <string.h>

static zq_u32 rol32(zq_u32 x, unsigned n) {
  return (x << n) | (x >> (32 - n));
}

void zq_sha1_init(zq_sha1 *s) {
  s->len = 0;
  s->h[0] = 0x67452301u;
  s->h[1] = 0xEFCDAB89u;
  s->h[2] = 0x98BADCFEu;
  s->h[3] = 0x10325476u;
  s->h[4] = 0xC3D2E1F0u;
  memset(s->w, 0, sizeof(s->w));
}

/* Procesa un bloque de 64 bytes ya cargado en w[]. */
static void sha1_process(zq_sha1 *s) {
  zq_u32 a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4];
  static const zq_u32 k[4] = {0x5A827999u, 0x6ED9EBA1u, 0x8F1BBCDCu,
                              0xCA62C1D6u};
  int i;
  for (i = 0; i < 80; ++i) {
    zq_u32 f, t;
    if (i >= 16) {
      zq_u32 x = s->w[i & 15] ^ s->w[(i - 3) & 15] ^ s->w[(i - 8) & 15] ^
                 s->w[(i - 14) & 15];
      s->w[i & 15] = rol32(x, 1);
    }
    if (i % 40 >= 20) f = b ^ c ^ d;
    else if (i >= 40) f = (b & c) | (d & (b | c));
    else f = d ^ (b & (c ^ d));
    t = rol32(a, 5) + f + e + k[i / 20] + s->w[i & 15];
    e = d;
    d = c;
    c = rol32(b, 30);
    b = a;
    a = t;
  }
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
