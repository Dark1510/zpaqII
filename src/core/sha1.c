/* zpaqII - SHA-1.  Port of libzpaq 7.15 (Unlicense, Matt Mahoney). */
#include "zq_internal.h"

#include <string.h>

#if (defined(__x86_64__) || defined(__i386__)) && \
    (defined(__GNUC__) || defined(__clang__))
#include <immintrin.h>
#define ZQ_HAVE_SHANI 1
#endif

/* Test hook: force the portable path so a test can compare both
 * implementations on a CPU that has the extensions.  Never used in the
 * archiver itself. */
static int zq_sha1_portable;
void zq_sha1_force_portable(int on) { zq_sha1_portable = on; }

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

#if defined(ZQ_HAVE_SHANI)
/* Four rounds at a time with the x86 SHA extensions, using the sequence from
 * Intel's "Intel SHA Extensions" note: sha1msg1/sha1msg2 expand the message
 * schedule while the rounds run, so an 80-round block never leaves registers.
 *
 * Only reached when zq_cpu_features() reports ZQ_CPU_SHA1.  The portable
 * code above is always compiled: it is the reference the tests compare
 * against and the fallback on every CPU without the extensions. */
__attribute__((target("sha,sse4.1,ssse3"))) static void sha1_blocks_shani(
    zq_u32 *h, const unsigned char *data, size_t blocks) {
  __m128i abcd, abcd_save, e0, e0_save, e1;
  __m128i msg0, msg1, msg2, msg3;
  const __m128i mask =
      _mm_set_epi64x(0x0001020304050607LL, 0x08090a0b0c0d0e0fLL);

  abcd = _mm_loadu_si128((const __m128i *)h);
  e0 = _mm_set_epi32((int)h[4], 0, 0, 0);
  abcd = _mm_shuffle_epi32(abcd, 0x1B);

  while (blocks--) {
    abcd_save = abcd;
    e0_save = e0;

    /* rounds 0-3 */
    msg0 = _mm_shuffle_epi8(_mm_loadu_si128((const __m128i *)(data + 0)), mask);
    e0 = _mm_add_epi32(e0, msg0);
    e1 = abcd;
    abcd = _mm_sha1rnds4_epu32(abcd, e0, 0);

    /* rounds 4-7 */
    msg1 = _mm_shuffle_epi8(_mm_loadu_si128((const __m128i *)(data + 16)), mask);
    e1 = _mm_sha1nexte_epu32(e1, msg1);
    e0 = abcd;
    abcd = _mm_sha1rnds4_epu32(abcd, e1, 0);
    msg0 = _mm_sha1msg1_epu32(msg0, msg1);

    /* rounds 8-11 */
    msg2 = _mm_shuffle_epi8(_mm_loadu_si128((const __m128i *)(data + 32)), mask);
    e0 = _mm_sha1nexte_epu32(e0, msg2);
    e1 = abcd;
    abcd = _mm_sha1rnds4_epu32(abcd, e0, 0);
    msg1 = _mm_sha1msg1_epu32(msg1, msg2);
    msg0 = _mm_xor_si128(msg0, msg2);

    /* rounds 12-15 */
    msg3 = _mm_shuffle_epi8(_mm_loadu_si128((const __m128i *)(data + 48)), mask);
    e1 = _mm_sha1nexte_epu32(e1, msg3);
    e0 = abcd;
    msg0 = _mm_sha1msg2_epu32(msg0, msg3);
    abcd = _mm_sha1rnds4_epu32(abcd, e1, 0);
    msg2 = _mm_sha1msg1_epu32(msg2, msg3);
    msg1 = _mm_xor_si128(msg1, msg3);

    /* rounds 16-19 */
    e0 = _mm_sha1nexte_epu32(e0, msg0);
    e1 = abcd;
    msg1 = _mm_sha1msg2_epu32(msg1, msg0);
    abcd = _mm_sha1rnds4_epu32(abcd, e0, 0);
    msg3 = _mm_sha1msg1_epu32(msg3, msg0);
    msg2 = _mm_xor_si128(msg2, msg0);

    /* rounds 20-23 */
    e1 = _mm_sha1nexte_epu32(e1, msg1);
    e0 = abcd;
    msg2 = _mm_sha1msg2_epu32(msg2, msg1);
    abcd = _mm_sha1rnds4_epu32(abcd, e1, 1);
    msg0 = _mm_sha1msg1_epu32(msg0, msg1);
    msg3 = _mm_xor_si128(msg3, msg1);

    /* rounds 24-27 */
    e0 = _mm_sha1nexte_epu32(e0, msg2);
    e1 = abcd;
    msg3 = _mm_sha1msg2_epu32(msg3, msg2);
    abcd = _mm_sha1rnds4_epu32(abcd, e0, 1);
    msg1 = _mm_sha1msg1_epu32(msg1, msg2);
    msg0 = _mm_xor_si128(msg0, msg2);

    /* rounds 28-31 */
    e1 = _mm_sha1nexte_epu32(e1, msg3);
    e0 = abcd;
    msg0 = _mm_sha1msg2_epu32(msg0, msg3);
    abcd = _mm_sha1rnds4_epu32(abcd, e1, 1);
    msg2 = _mm_sha1msg1_epu32(msg2, msg3);
    msg1 = _mm_xor_si128(msg1, msg3);

    /* rounds 32-35 */
    e0 = _mm_sha1nexte_epu32(e0, msg0);
    e1 = abcd;
    msg1 = _mm_sha1msg2_epu32(msg1, msg0);
    abcd = _mm_sha1rnds4_epu32(abcd, e0, 1);
    msg3 = _mm_sha1msg1_epu32(msg3, msg0);
    msg2 = _mm_xor_si128(msg2, msg0);

    /* rounds 36-39 */
    e1 = _mm_sha1nexte_epu32(e1, msg1);
    e0 = abcd;
    msg2 = _mm_sha1msg2_epu32(msg2, msg1);
    abcd = _mm_sha1rnds4_epu32(abcd, e1, 1);
    msg0 = _mm_sha1msg1_epu32(msg0, msg1);
    msg3 = _mm_xor_si128(msg3, msg1);

    /* rounds 40-43 */
    e0 = _mm_sha1nexte_epu32(e0, msg2);
    e1 = abcd;
    msg3 = _mm_sha1msg2_epu32(msg3, msg2);
    abcd = _mm_sha1rnds4_epu32(abcd, e0, 2);
    msg1 = _mm_sha1msg1_epu32(msg1, msg2);
    msg0 = _mm_xor_si128(msg0, msg2);

    /* rounds 44-47 */
    e1 = _mm_sha1nexte_epu32(e1, msg3);
    e0 = abcd;
    msg0 = _mm_sha1msg2_epu32(msg0, msg3);
    abcd = _mm_sha1rnds4_epu32(abcd, e1, 2);
    msg2 = _mm_sha1msg1_epu32(msg2, msg3);
    msg1 = _mm_xor_si128(msg1, msg3);

    /* rounds 48-51 */
    e0 = _mm_sha1nexte_epu32(e0, msg0);
    e1 = abcd;
    msg1 = _mm_sha1msg2_epu32(msg1, msg0);
    abcd = _mm_sha1rnds4_epu32(abcd, e0, 2);
    msg3 = _mm_sha1msg1_epu32(msg3, msg0);
    msg2 = _mm_xor_si128(msg2, msg0);

    /* rounds 52-55 */
    e1 = _mm_sha1nexte_epu32(e1, msg1);
    e0 = abcd;
    msg2 = _mm_sha1msg2_epu32(msg2, msg1);
    abcd = _mm_sha1rnds4_epu32(abcd, e1, 2);
    msg0 = _mm_sha1msg1_epu32(msg0, msg1);
    msg3 = _mm_xor_si128(msg3, msg1);

    /* rounds 56-59 */
    e0 = _mm_sha1nexte_epu32(e0, msg2);
    e1 = abcd;
    msg3 = _mm_sha1msg2_epu32(msg3, msg2);
    abcd = _mm_sha1rnds4_epu32(abcd, e0, 2);
    msg1 = _mm_sha1msg1_epu32(msg1, msg2);
    msg0 = _mm_xor_si128(msg0, msg2);

    /* rounds 60-63 */
    e1 = _mm_sha1nexte_epu32(e1, msg3);
    e0 = abcd;
    msg0 = _mm_sha1msg2_epu32(msg0, msg3);
    abcd = _mm_sha1rnds4_epu32(abcd, e1, 3);
    msg2 = _mm_sha1msg1_epu32(msg2, msg3);
    msg1 = _mm_xor_si128(msg1, msg3);

    /* rounds 64-67 */
    e0 = _mm_sha1nexte_epu32(e0, msg0);
    e1 = abcd;
    msg1 = _mm_sha1msg2_epu32(msg1, msg0);
    abcd = _mm_sha1rnds4_epu32(abcd, e0, 3);
    msg3 = _mm_sha1msg1_epu32(msg3, msg0);
    msg2 = _mm_xor_si128(msg2, msg0);

    /* rounds 68-71 */
    e1 = _mm_sha1nexte_epu32(e1, msg1);
    e0 = abcd;
    msg2 = _mm_sha1msg2_epu32(msg2, msg1);
    abcd = _mm_sha1rnds4_epu32(abcd, e1, 3);
    msg3 = _mm_xor_si128(msg3, msg1);

    /* rounds 72-75 */
    e0 = _mm_sha1nexte_epu32(e0, msg2);
    e1 = abcd;
    msg3 = _mm_sha1msg2_epu32(msg3, msg2);
    abcd = _mm_sha1rnds4_epu32(abcd, e0, 3);

    /* rounds 76-79 */
    e1 = _mm_sha1nexte_epu32(e1, msg3);
    e0 = abcd;
    abcd = _mm_sha1rnds4_epu32(abcd, e1, 3);

    /* feed the rotated E and add the previous state back */
    e0 = _mm_sha1nexte_epu32(e0, e0_save);
    abcd = _mm_add_epi32(abcd, abcd_save);

    data += 64;
  }

  abcd = _mm_shuffle_epi32(abcd, 0x1B);
  _mm_storeu_si128((__m128i *)h, abcd);
  h[4] = (zq_u32)_mm_extract_epi32(e0, 3);
}
#endif /* ZQ_HAVE_SHANI */

void zq_sha1_put(zq_sha1 *s, int c) {
  zq_u32 *r = &s->w[(zq_u32)s->len >> 5 & 15];
  *r = (*r << 8) | ((zq_u32)c & 255u);
  s->len += 8;
  if (((zq_u32)s->len & 511u) == 0) sha1_process(s);
}

void zq_sha1_write(zq_sha1 *s, const char *buf, zq_i64 n) {
  const unsigned char *p = (const unsigned char *)buf;
  for (; n > 0 && ((zq_u32)s->len & 511u) != 0; --n) zq_sha1_put(s, *p++);
#if defined(ZQ_HAVE_SHANI)
  if (n >= 64 && !zq_sha1_portable && (zq_cpu_features() & ZQ_CPU_SHA1) != 0) {
    const zq_i64 blocks = n / 64;
    sha1_blocks_shani(s->h, p, (size_t)blocks);
    s->len += (zq_u64)blocks * 512;
    p += blocks * 64;
    n -= blocks * 64;
  }
#endif
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
