/* zpaqII - LZ4 block format (phase 2.4).
 *
 * The block format is the one from lz4.c by Yann Collet (BSD-2), not the frame
 * format: a sequence of tokens, each with a literal run and a match, and a
 * final literal-only sequence.  It is small enough to be decoded by a ZPAQL
 * program inside a zpaq block, which is the point: an archive that uses it is
 * still readable by plain zpaq 7.15, which runs the ZPAQL.
 *
 * This file is the native side: our own encoder and decoder, no dependency on
 * liblz4 at build time (the tests use it, if present, to cross-check).
 */
#include "zq.h"

#include <stdlib.h>
#include <string.h>

#define LZ4_MIN_MATCH 4
#define LZ4_MAX_OFFSET 65535
#define LZ4_HASH_BITS 16
#define LZ4_HASH_SIZE (1 << LZ4_HASH_BITS)

static zq_u32 lz4_read32(const unsigned char *p) {
  return (zq_u32)p[0] | ((zq_u32)p[1] << 8) | ((zq_u32)p[2] << 16) |
         ((zq_u32)p[3] << 24);
}

static zq_u32 lz4_hash(zq_u32 v) {
  return (v * 2654435761u) >> (32 - LZ4_HASH_BITS);
}

/* Count bytes equal from p and q, at most `limit`. */
static size_t lz4_match_len(const unsigned char *p, const unsigned char *q,
                            size_t limit) {
  size_t n = 0;
  while (n < limit && p[n] == q[n]) ++n;
  return n;
}

/* Compress src[0..n) into dst[0..cap).  Returns the size, or -1 if it does not
 * fit (or n == 0 with cap == 0).  The output is a valid LZ4 block: any LZ4
 * decoder reads it. */
int zq_lz4_compress(const void *srcv, size_t n, void *dstv, size_t cap) {
  const unsigned char *src = (const unsigned char *)srcv;
  unsigned char *dst = (unsigned char *)dstv;
  zq_u32 *table;
  size_t op = 0;      /* output position */
  size_t anchor = 0;  /* first byte not yet emitted as a literal */
  size_t ip = 0;

  if (n == 0) { /* the reference emits one empty token */
    if (cap < 1) return -1;
    dst[0] = 0;
    return 1;
  }
  if (cap < 1) return -1;

  table = (zq_u32 *)malloc(sizeof(zq_u32) * LZ4_HASH_SIZE);
  if (!table) return -1;
  memset(table, 0xFF, sizeof(zq_u32) * LZ4_HASH_SIZE); /* "no entry" */

  /* The LZ4 spec keeps the last 5 bytes as literals and does not let a match
     start within the last 12: LZ4_decompress_safe rejects blocks that break
     that, so the encoder must respect it. */
  while (n >= 12 && ip + 12 <= n) {
    const zq_u32 h = lz4_hash(lz4_read32(src + ip));
    const zq_u32 cand = table[h];
    table[h] = (zq_u32)ip;

    if (cand != 0xFFFFFFFFu && ip > (size_t)cand &&
        ip - (size_t)cand <= LZ4_MAX_OFFSET &&
        lz4_read32(src + cand) == lz4_read32(src + ip)) {
      /* a match: emit the pending literals plus this sequence */
      size_t lit = ip - anchor;
      size_t match = lz4_match_len(src + cand, src + ip, n - ip);
      if (match > n - 5 - ip) match = n - 5 - ip;   /* keep 5 literals */
      if (match < LZ4_MIN_MATCH) { ++ip; continue; }
      size_t need = lit + 1 + 2 + 8 + 1; /* worst case extension bytes */
      size_t litlen, mlen;

      if (op + need > cap) goto fail;
      litlen = lit;
      mlen = match - LZ4_MIN_MATCH;
      dst[op] = (unsigned char)((litlen >= 15 ? 15 : litlen) << 4);
      dst[op] |= (unsigned char)(mlen >= 15 ? 15 : mlen);
      ++op;
      if (litlen >= 15) {
        size_t r = litlen - 15;
        while (r >= 255) {
          dst[op++] = 255;
          r -= 255;
        }
        dst[op++] = (unsigned char)r;
      }
      memcpy(dst + op, src + anchor, lit);
      op += lit;
      {
        const size_t off = ip - (size_t)cand;
        dst[op++] = (unsigned char)(off & 255);
        dst[op++] = (unsigned char)(off >> 8);
      }
      if (mlen >= 15) {
        size_t r = mlen - 15;
        while (r >= 255) {
          dst[op++] = 255;
          r -= 255;
        }
        dst[op++] = (unsigned char)r;
      }
      ip += match;
      anchor = ip;
      continue;
    }
    ++ip;
  }

  /* the last sequence is literals only */
  {
    const size_t lit = n - anchor;
    const size_t need = lit + 1 + (lit / 255) + 1;
    if (op + need > cap) goto fail;
    dst[op] = (unsigned char)((lit >= 15 ? 15 : lit) << 4);
    ++op;
    if (lit >= 15) {
      size_t r = lit - 15;
      while (r >= 255) {
        dst[op++] = 255;
        r -= 255;
      }
      dst[op++] = (unsigned char)r;
    }
    memcpy(dst + op, src + anchor, lit);
    op += lit;
  }

  free(table);
  return (int)op;

fail:
  free(table);
  return -1;
}

/* Decompress a block into dst[0..cap).  Returns the size, or -1 when the input
 * is malformed or the output does not fit. */
int zq_lz4_decompress(const void *srcv, size_t n, void *dstv, size_t cap) {
  const unsigned char *src = (const unsigned char *)srcv;
  unsigned char *dst = (unsigned char *)dstv;
  size_t ip = 0, op = 0;

  if (n == 0) return 0;
  for (;;) {
    unsigned token;
    size_t lit, mlen;

    if (ip >= n) return -1;
    token = src[ip++];
    lit = token >> 4;
    if (lit == 15) {
      unsigned b;
      do {
        if (ip >= n) return -1;
        b = src[ip++];
        lit += b;
      } while (b == 255);
    }
    if (lit > n - ip || lit > cap - op) return -1;
    memcpy(dst + op, src + ip, lit);
    ip += lit;
    op += lit;
    if (ip == n) break; /* the last sequence has no match */

    if (ip + 2 > n) return -1;
    {
      const size_t off = (size_t)src[ip] | ((size_t)src[ip + 1] << 8);
      ip += 2;
      if (off == 0 || off > op) return -1;
      mlen = token & 15;
      if (mlen == 15) {
        unsigned b;
        do {
          if (ip >= n) return -1;
          b = src[ip++];
          mlen += b;
        } while (b == 255);
      }
      mlen += LZ4_MIN_MATCH;
      if (mlen > cap - op) return -1;
      {
        /* the copy may overlap: byte by byte is the definition */
        size_t k;
        const size_t from = op - off;
        for (k = 0; k < mlen; ++k) dst[op + k] = dst[from + k];
        op += mlen;
      }
    }
  }
  return (int)op;
}
