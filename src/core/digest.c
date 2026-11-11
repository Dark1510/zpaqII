/* zpaqII - extra digests for the configurable integrity lists (phase 2.3).
 *
 * CRC-32 (IEEE, the one zlib uses) and XXH64 (seed 0 unless the caller says
 * otherwise).  Both are streamed, so a file or a fragment is hashed while it
 * is read, and both are portable C: no tables generated at run time, no
 * endianness assumptions, no unaligned reads.
 */
#include "../../include/zq.h"

#include <string.h>

/* ------------------------------------------------------------------ */
/* CRC-32                                                              */
/* ------------------------------------------------------------------ */

static zq_u32 crc_table[256];
static int crc_ready;

static void crc_init_table(void) {
  zq_u32 c;
  int i, k;
  for (i = 0; i < 256; ++i) {
    c = (zq_u32)i;
    for (k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
    crc_table[i] = c;
  }
  crc_ready = 1;
}

void zq_crc32_init(zq_crc32 *s) { s->crc = 0xFFFFFFFFu; }

void zq_crc32_write(zq_crc32 *s, const void *p, size_t n) {
  const unsigned char *b = (const unsigned char *)p;
  zq_u32 c = s->crc;
  size_t i;
  if (!crc_ready) crc_init_table(); /* benign race: same table either way */
  for (i = 0; i < n; ++i) c = crc_table[(c ^ b[i]) & 255] ^ (c >> 8);
  s->crc = c;
}

zq_u32 zq_crc32_result(const zq_crc32 *s) { return s->crc ^ 0xFFFFFFFFu; }

/* ------------------------------------------------------------------ */
/* XXH64                                                               */
/* ------------------------------------------------------------------ */

#define P1 0x9E3779B185EBCA87ull
#define P2 0xC2B2AE3D27D4EB4Full
#define P3 0x165667B19E3779F9ull
#define P4 0x85EBCA77C2B2AE63ull
#define P5 0x27D4EB2F165667C5ull

static zq_u64 rotl64(zq_u64 x, int r) { return (x << r) | (x >> (64 - r)); }

static zq_u64 rd64(const unsigned char *p) {
  return (zq_u64)p[0] | ((zq_u64)p[1] << 8) | ((zq_u64)p[2] << 16) |
         ((zq_u64)p[3] << 24) | ((zq_u64)p[4] << 32) | ((zq_u64)p[5] << 40) |
         ((zq_u64)p[6] << 48) | ((zq_u64)p[7] << 56);
}

static zq_u32 rd32(const unsigned char *p) {
  return (zq_u32)p[0] | ((zq_u32)p[1] << 8) | ((zq_u32)p[2] << 16) |
         ((zq_u32)p[3] << 24);
}

static zq_u64 round64(zq_u64 acc, zq_u64 input) {
  acc += input * P2;
  acc = rotl64(acc, 31);
  return acc * P1;
}

static zq_u64 merge_round(zq_u64 h, zq_u64 v) {
  h ^= round64(0, v);
  return h * P1 + P4;
}

void zq_xxh64_init(zq_xxh64 *s, zq_u64 seed) {
  s->seed = seed;
  s->total = 0;
  s->n = 0;
  s->started = 0;
}

static void xxh64_start(zq_xxh64 *s) {
  s->v1 = s->seed + P1 + P2;
  s->v2 = s->seed + P2;
  s->v3 = s->seed;
  s->v4 = s->seed - P1;
  s->started = 1;
}

static void xxh64_block(zq_xxh64 *s) {
  s->v1 = round64(s->v1, rd64(s->buf + 0));
  s->v2 = round64(s->v2, rd64(s->buf + 8));
  s->v3 = round64(s->v3, rd64(s->buf + 16));
  s->v4 = round64(s->v4, rd64(s->buf + 24));
}

void zq_xxh64_write(zq_xxh64 *s, const void *p, size_t n) {
  const unsigned char *b = (const unsigned char *)p;

  s->total += n;
  if (s->n > 0) { /* complete the block left over from the previous call */
    size_t take = 32 - (size_t)s->n;
    if (take > n) take = n;
    memcpy(s->buf + s->n, b, take);
    s->n += (int)take;
    b += take;
    n -= take;
    if (s->n < 32) return;
    if (!s->started) xxh64_start(s);
    xxh64_block(s);
    s->n = 0;
  }
  while (n >= 32) {
    if (!s->started) xxh64_start(s);
    s->v1 = round64(s->v1, rd64(b + 0));
    s->v2 = round64(s->v2, rd64(b + 8));
    s->v3 = round64(s->v3, rd64(b + 16));
    s->v4 = round64(s->v4, rd64(b + 24));
    b += 32;
    n -= 32;
  }
  if (n > 0) {
    memcpy(s->buf, b, n);
    s->n = (int)n;
  }
}

zq_u64 zq_xxh64_result(const zq_xxh64 *s) {
  zq_u64 h;
  const unsigned char *p = s->buf;
  size_t n = (size_t)s->n;

  if (s->total >= 32) {
    h = rotl64(s->v1, 1) + rotl64(s->v2, 7) + rotl64(s->v3, 12) +
        rotl64(s->v4, 18);
    h = merge_round(h, s->v1);
    h = merge_round(h, s->v2);
    h = merge_round(h, s->v3);
    h = merge_round(h, s->v4);
  } else {
    h = s->seed + P5;
  }
  h += s->total;

  while (n >= 8) {
    h ^= round64(0, rd64(p));
    h = rotl64(h, 27) * P1 + P4;
    p += 8;
    n -= 8;
  }
  if (n >= 4) {
    h ^= (zq_u64)rd32(p) * P1;
    h = rotl64(h, 23) * P2 + P3;
    p += 4;
    n -= 4;
  }
  while (n > 0) {
    h ^= (zq_u64)(*p) * P5;
    h = rotl64(h, 11) * P1;
    ++p;
    --n;
  }

  h ^= h >> 33;
  h *= P2;
  h ^= h >> 29;
  h *= P3;
  h ^= h >> 32;
  return h;
}
