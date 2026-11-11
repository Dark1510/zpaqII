/* zpaqII - SHA-256.  Port of libzpaq 7.15 (Unlicense, Matt Mahoney). */
#include "../../include/zq.h"

#include <stdlib.h>
#include <string.h>

static unsigned ror32(unsigned a, unsigned b) {
  return (a >> b) | (a << (32 - b));
}

zq_sha256 *zq_sha256_new(void) {
  zq_sha256 *s = (zq_sha256 *)zq_calloc(sizeof(zq_sha256));
  zq_sha256_init(s);
  return s;
}

void zq_sha256_free(zq_sha256 *s) { free(s); }

void zq_sha256_init(zq_sha256 *s) {
  s->len0 = s->len1 = 0;
  s->s[0] = 0x6a09e667u;
  s->s[1] = 0xbb67ae85u;
  s->s[2] = 0x3c6ef372u;
  s->s[3] = 0xa54ff53au;
  s->s[4] = 0x510e527fu;
  s->s[5] = 0x9b05688cu;
  s->s[6] = 0x1f83d9abu;
  s->s[7] = 0x5be0cd19u;
  memset(s->w, 0, sizeof(s->w));
}

static void sha256_process(zq_sha256 *s) {
  static const unsigned k[64] = {
      0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu,
      0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u,
      0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u,
      0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
      0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u,
      0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
      0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
      0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
      0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u,
      0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u, 0x1e376c08u,
      0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu,
      0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
      0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};
  unsigned w[64];
  unsigned a = s->s[0], b = s->s[1], c = s->s[2], d = s->s[3];
  unsigned e = s->s[4], f = s->s[5], g = s->s[6], h = s->s[7];
  int i;
  for (i = 0; i < 16; ++i) w[i] = s->w[i];
  for (i = 16; i < 64; ++i) {
    const unsigned s0 = ror32(w[i - 15], 7) ^ ror32(w[i - 15], 18) ^
                        (w[i - 15] >> 3);
    const unsigned s1 = ror32(w[i - 2], 17) ^ ror32(w[i - 2], 19) ^
                        (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  for (i = 0; i < 64; ++i) {
    const unsigned S1 = ror32(e, 6) ^ ror32(e, 11) ^ ror32(e, 25);
    const unsigned ch = (e & f) ^ (~e & g);
    const unsigned t1 = h + S1 + ch + k[i] + w[i];
    const unsigned S0 = ror32(a, 2) ^ ror32(a, 13) ^ ror32(a, 22);
    const unsigned maj = (a & b) ^ (c & (a ^ b));
    const unsigned t2 = S0 + maj;
    h = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b;
    b = a;
    a = t1 + t2;
  }
  s->s[0] += a;
  s->s[1] += b;
  s->s[2] += c;
  s->s[3] += d;
  s->s[4] += e;
  s->s[5] += f;
  s->s[6] += g;
  s->s[7] += h;
}

void zq_sha256_put(zq_sha256 *s, int c) {
  unsigned *r = &s->w[s->len0 >> 5 & 15];
  *r = (*r << 8) | ((unsigned)c & 255u);
  if (!(s->len0 += 8)) ++s->len1;
  if ((s->len0 & 511u) == 0) sha256_process(s);
}

void zq_sha256_write(zq_sha256 *s, const char *buf, zq_i64 n) {
  const unsigned char *p = (const unsigned char *)buf;
  for (; n > 0 && (s->len0 & 511u) != 0; --n) zq_sha256_put(s, *p++);
  for (; n >= 64; n -= 64) {
    int i;
    for (i = 0; i < 16; ++i) {
      s->w[i] = ((unsigned)p[0] << 24) | ((unsigned)p[1] << 16) |
                ((unsigned)p[2] << 8) | (unsigned)p[3];
      p += 4;
    }
    s->len0 += 512;
    if (s->len0 < 512) ++s->len1;
    sha256_process(s);
  }
  for (; n > 0; --n) zq_sha256_put(s, *p++);
}

const char *zq_sha256_result(zq_sha256 *s) {
  const unsigned s1 = s->len1, s0 = s->len0;
  int i;
  zq_sha256_put(s, 0x80);
  while ((s->len0 & 511u) != 448u) zq_sha256_put(s, 0);
  zq_sha256_put(s, (int)(s1 >> 24));
  zq_sha256_put(s, (int)(s1 >> 16));
  zq_sha256_put(s, (int)(s1 >> 8));
  zq_sha256_put(s, (int)s1);
  zq_sha256_put(s, (int)(s0 >> 24));
  zq_sha256_put(s, (int)(s0 >> 16));
  zq_sha256_put(s, (int)(s0 >> 8));
  zq_sha256_put(s, (int)s0);
  for (i = 0; i < 8; ++i) {
    s->hbuf[4 * i + 0] = (char)(s->s[i] >> 24);
    s->hbuf[4 * i + 1] = (char)(s->s[i] >> 16);
    s->hbuf[4 * i + 2] = (char)(s->s[i] >> 8);
    s->hbuf[4 * i + 3] = (char)(s->s[i]);
  }
  zq_sha256_init(s);
  return s->hbuf;
}

double zq_sha256_size(const zq_sha256 *s) {
  return (double)(s->len0 / 8) + (double)s->len1 * 536870912.0;
}
zq_u64 zq_sha256_usize(const zq_sha256 *s) {
  return (zq_u64)(s->len0 / 8) + ((zq_u64)s->len1 << 29);
}
