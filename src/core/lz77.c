/* zpaqII - LZ77 / BWT preprocessor (libzpaq::LZBuffer).
 * Port of libzpaq 7.15 (Unlicense, Matt Mahoney). */
#include "zq_internal.h"

#include <stdlib.h>
#include <string.h>

#define ZQ_BUFSIZE (1 << 14)

typedef struct zq_lzbuf {
  zq_arr ht;                   /* U32: hash table, or SA/ISA, or BWT SA */
  const unsigned char *in;     /* input pointer */
  int checkbits;               /* hash confirmation size or lg(ISA size) */
  int level;                   /* 1=var length LZ77, 2=byte aligned, 3=BWT */
  unsigned htsize;
  unsigned n;
  unsigned i;
  unsigned minMatch, minMatch2;
  unsigned maxMatch, maxLiteral;
  unsigned lookahead;
  unsigned h1, h2;
  unsigned bucket;
  unsigned shift1, shift2;
  int minMatchBoth;
  unsigned rb;
  unsigned bits, nbits;
  unsigned rpos, wpos;
  unsigned idx;
  const unsigned *sa;
  unsigned *isa;
  unsigned char buf[ZQ_BUFSIZE];
} zq_lzbuf;

int zq_lg(unsigned x);
int zq_popcount(unsigned x);
void zq_e8e9(unsigned char *buf, int n);
int divsufsort(const unsigned char *T, int *SA, int n);

/* Write k bits of x */
static void zq_lz_putb(zq_lzbuf *z, unsigned x, int k) {
  x &= (1u << k) - 1;
  z->bits |= x << z->nbits;
  z->nbits += (unsigned)k;
  while (z->nbits > 7) {
    z->buf[z->wpos++] = (unsigned char)z->bits;
    z->bits >>= 8;
    z->nbits -= 8;
  }
}

static void zq_lz_flush_bits(zq_lzbuf *z) {
  if (z->nbits > 0) z->buf[z->wpos++] = (unsigned char)z->bits;
  z->bits = z->nbits = 0;
}

static void zq_lz_put(zq_lzbuf *z, int c) { z->buf[z->wpos++] = (unsigned char)c; }

/* floor(log2(x)) + 1 (0..32) */
int zq_lg(unsigned x) {
  unsigned r = 0;
  static const unsigned char t[16] = {0, 1, 2, 2, 3, 3, 3, 3,
                                      4, 4, 4, 4, 4, 4, 4, 4};
  if (x >= 65536) r = 16, x >>= 16;
  if (x >= 256) r += 8, x >>= 8;
  if (x >= 16) r += 4, x >>= 4;
  return t[x] + (int)r;
}

/* number of 1 bits */
int zq_popcount(unsigned x) {
  int r;
  for (r = 0; x; x >>= 1) r += x & 1;
  return r;
}

/* E8E9 transform of buf[0..n-1]: patterns (E8|E9 xx xx xx 00|FF) at offset i
 * get the 3 middle bytes replaced with i + x mod 2^24, LSB first, backwards. */
void zq_e8e9(unsigned char *buf, int n) {
  int i;
  for (i = n - 5; i >= 0; --i) {
    if (((buf[i] & 254) == 0xe8) && ((buf[i + 4] + 1) & 254) == 0) {
      unsigned a = (unsigned)(buf[i + 1] | buf[i + 2] << 8 | buf[i + 3] << 16) +
                   (unsigned)i;
      buf[i + 1] = (unsigned char)a;
      buf[i + 2] = (unsigned char)(a >> 8);
      buf[i + 3] = (unsigned char)(a >> 16);
    }
  }
}

static void zq_lz_write_literal(zq_lzbuf *z, unsigned i, unsigned *lit);
static void zq_lz_write_match(zq_lzbuf *z, unsigned len, unsigned off);
static void zq_lz_fill(zq_lzbuf *z);

int zq_lz_get(zq_lzbuf *z) {
  int c = -1;
  if (z->rpos == z->wpos) zq_lz_fill(z);
  if (z->rpos < z->wpos) c = z->buf[z->rpos++];
  if (z->rpos == z->wpos) z->rpos = z->wpos = 0;
  return c;
}

int zq_lz_read(zq_lzbuf *z, char *p, int n) {
  int nr = n;
  if (z->rpos == z->wpos) zq_lz_fill(z);
  if (nr > (int)(z->wpos - z->rpos)) nr = (int)(z->wpos - z->rpos);
  if (nr) memcpy(p, z->buf + z->rpos, (size_t)nr);
  z->rpos += (unsigned)nr;
  if (z->rpos == z->wpos) z->rpos = z->wpos = 0;
  return nr;
}

void zq_lz_open(zq_lzbuf *z, zq_buf *inbuf, int *args, const unsigned *sap) {
  const unsigned n = (unsigned)zq_buf_size(inbuf);
  size_t htn;
  memset(z, 0, sizeof(*z));
  if ((args[1] & 3) == 3) htn = ((size_t)n + 1) * (sap ? 0 : 1);
  else if (args[5] - args[0] < 21) htn = (size_t)1 << args[5];
  else htn = (size_t)n * (sap ? 0 : 1) + ((size_t)1 << 17 << args[0]);
  zq_arr_resize(&z->ht, htn, 4);

  z->in = (const unsigned char *)zq_buf_data(inbuf);
  z->checkbits = args[5] - args[0] < 21 ? 12 - args[0] : 17 + args[0];
  z->level = args[1] & 3;
  z->htsize = (unsigned)z->ht.n;
  z->n = n;
  z->i = 0;
  z->minMatch = (unsigned)args[2];
  z->minMatch2 = (unsigned)args[3];
  z->maxMatch = ZQ_BUFSIZE * 3;
  z->maxLiteral = ZQ_BUFSIZE / 4;
  z->lookahead = (unsigned)args[6];
  z->h1 = z->h2 = 0;
  z->bucket = (1u << args[4]) - 1;
  z->shift1 = z->minMatch > 0 ? (unsigned)((args[5] - 1) / (int)z->minMatch + 1) : 1;
  z->shift2 = z->minMatch2 > 0 ? (unsigned)((args[5] - 1) / (int)z->minMatch2 + 1) : 0;
  z->minMatchBoth = (int)((z->minMatch > z->minMatch2 + z->lookahead
                               ? z->minMatch
                               : z->minMatch2 + z->lookahead) + 4);
  z->rb = args[0] > 4 ? (unsigned)(args[0] - 4) : 0;
  z->bits = z->nbits = z->rpos = z->wpos = 0;
  z->idx = 0;
  z->sa = NULL;
  z->isa = NULL;

  if ((z->minMatch < 4 && z->level == 1) || (z->minMatch < 1 && z->level == 2))
    zq_error("match length $3 too small");

  /* e8e9 transform */
  if (args[1] > 4 && !sap) zq_e8e9((unsigned char *)zq_buf_data(inbuf), (int)n);

  /* build suffix array if not supplied */
  if (args[5] - args[0] >= 21 || z->level == 3) { /* LZ77-SA or BWT */
    if (sap) {
      z->sa = sap;
    } else {
      z->sa = &ZQ_U32A(&z->ht, 0);
      if (n > 0) divsufsort((const unsigned char *)z->in, (int *)z->sa, (int)n);
    }
    if (z->level < 3) z->isa = &ZQ_U32A(&z->ht, (size_t)n * (sap ? 0 : 1));
  }
}

void zq_lz_close(zq_lzbuf *z) { zq_arr_free(&z->ht); }

zq_lzbuf *zq_lz_new(zq_buf *inbuf, int *args, const unsigned *sap) {
  zq_lzbuf *z = (zq_lzbuf *)zq_calloc(sizeof(*z));
  zq_lz_open(z, inbuf, args, sap);
  return z;
}

void zq_lz_free(zq_lzbuf *z) {
  if (!z) return;
  zq_lz_close(z);
  free(z);
}

/* Encode from in to buf until end of input or buf is not empty */
static void zq_lz_fill(zq_lzbuf *z) {
  /* BWT */
  if (z->level == 3) {
    for (; z->wpos < ZQ_BUFSIZE && z->i < z->n + 5; ++z->i) {
      if (z->i == 0) zq_lz_put(z, z->n > 0 ? z->in[z->n - 1] : 255);
      else if (z->i > z->n) {
        zq_lz_put(z, (int)(z->idx & 255));
        z->idx >>= 8;
      } else if (z->sa[z->i - 1] == 0) {
        z->idx = z->i;
        zq_lz_put(z, 255);
      } else {
        zq_lz_put(z, z->in[z->sa[z->i - 1] - 1]);
      }
    }
    return;
  }

  /* LZ77: scan the input */
  {
    unsigned lit = 0; /* number of pending output literals */
    const unsigned mask = (1u << z->checkbits) - 1;
    while (z->i < z->n && z->wpos * 2 < ZQ_BUFSIZE) {
      unsigned blen = z->minMatch - 1; /* best match length */
      unsigned bp = 0;                 /* pointer to best match */
      unsigned blit = 0;               /* literals before best match */
      int bscore = 0;

      if (z->isa) {
        /* Look up contexts in the suffix array */
        if (z->sa[z->isa[z->i & mask]] != z->i) { /* rebuild ISA */
          unsigned j;
          for (j = 0; j < z->n; ++j)
            if ((z->sa[j] & ~mask) == (z->i & ~mask))
              z->isa[z->sa[j] & mask] = j;
        }
        {
          unsigned h;
          for (h = 0; h <= z->lookahead; ++h) {
            const unsigned q = z->isa[(h + z->i) & mask];
            if (z->sa[q] != h + z->i) continue;
            {
              int j;
              for (j = -1; j <= 1; j += 2) {
                unsigned k;
                for (k = 1; k <= z->bucket; ++k) {
                  unsigned p;
                  if (q + (unsigned)j * k < z->n &&
                      (p = z->sa[q + (unsigned)j * k] - h) < z->i) {
                    unsigned l, l1;
                    for (l = h; z->i + l < z->n && l < z->maxMatch &&
                                z->in[p + l] == z->in[z->i + l];
                         ++l)
                      ;
                    for (l1 = h; l1 > 0 && z->in[p + l1 - 1] == z->in[z->i + l1 - 1];
                         --l1)
                      ;
                    {
                      int score = (int)(l - l1) * 8 - zq_lg(z->i - p) -
                                  4 * (lit == 0 && l1 > 0) - 11;
                      unsigned a;
                      for (a = 0; a < h; ++a) score = score * 5 / 8;
                      if (score > bscore) {
                        blen = l;
                        bp = p;
                        blit = l1;
                        bscore = score;
                      }
                    }
                    if (l < blen || l < z->minMatch || l > 255) break;
                  }
                }
              }
            }
            if (bscore <= 0 || blen < z->minMatch) break;
          }
        }
      } else if (z->level == 1 || z->minMatch <= 64) {
        /* Look up contexts in a hash table (longest context order first) */
        if (z->minMatch2 > 0) {
          unsigned k;
          for (k = 0; k <= z->bucket; ++k) {
            unsigned p = ZQ_U32A(&z->ht, z->h2 ^ k);
            if (p && (p & mask) == (z->in[z->i + 3] & mask)) {
              p >>= z->checkbits;
              if (p < z->i && z->i + blen <= z->n &&
                  z->in[p + blen - 1] == z->in[z->i + blen - 1]) {
                unsigned l;
                for (l = z->lookahead; z->i + l < z->n && l < z->maxMatch &&
                                        z->in[p + l] == z->in[z->i + l];
                     ++l)
                  ;
                if (l >= z->minMatch2 + z->lookahead) {
                  int l1;
                  for (l1 = (int)z->lookahead;
                       l1 > 0 && z->in[p + l1 - 1] == z->in[z->i + l1 - 1];
                       --l1)
                    ;
                  {
                    int score = (int)(l - (unsigned)l1) * 8 -
                                zq_lg(z->i - p) - 8 * (lit == 0 && l1 > 0) - 11;
                    if (score > bscore) {
                      blen = l;
                      bp = p;
                      blit = (unsigned)l1;
                      bscore = score;
                    }
                  }
                }
              }
            }
            if (blen >= 128) break;
          }
        }

        /* Search the lower order context */
        if (!z->minMatch2 || blen < z->minMatch2) {
          unsigned k;
          for (k = 0; k <= z->bucket; ++k) {
            unsigned p = ZQ_U32A(&z->ht, z->h1 ^ k);
            if (p && z->i + 3 < z->n && (p & mask) == (z->in[z->i + 3] & mask)) {
              p >>= z->checkbits;
              if (p < z->i && z->i + blen <= z->n &&
                  z->in[p + blen - 1] == z->in[z->i + blen - 1]) {
                unsigned l;
                for (l = 0; z->i + l < z->n && l < z->maxMatch &&
                            z->in[p + l] == z->in[z->i + l];
                     ++l)
                  ;
                {
                  int score = (int)l * 8 - zq_lg(z->i - p) - 2 * (lit > 0) - 11;
                  if (score > bscore) {
                    blen = l;
                    bp = p;
                    blit = 0;
                    bscore = score;
                  }
                }
              }
            }
            if (blen >= 128) break;
          }
        }
      }

      /* If the match is long enough, output pending literals then the match */
      {
        const unsigned off = z->i - bp;
        if (off > 0 && bscore > 0 &&
            blen - blit >=
                z->minMatch + (z->level == 2) *
                                  ((off >= (1u << 16)) + (off >= (1u << 24)))) {
          lit += blit;
          zq_lz_write_literal(z, z->i + blit, &lit);
          zq_lz_write_match(z, blen - blit, off);
        } else {
          blen = 1;
          ++lit;
        }
      }

      /* Update index, advance blen bytes */
      if (z->isa) {
        z->i += blen;
      } else {
        while (blen--) {
          if (z->i + (unsigned)z->minMatchBoth < z->n) {
            const unsigned ih = ((z->i * 1234547) >> 19) & z->bucket;
            const unsigned p = (z->i << z->checkbits) | (z->in[z->i + 3] & mask);
            if (z->minMatch2) {
              ZQ_U32A(&z->ht, z->h2 ^ ih) = p;
              z->h2 = (((z->h2 * 9) << z->shift2) +
                       (z->in[z->i + z->minMatch2 + z->lookahead] + 1) * 23456789u) &
                      (z->htsize - 1);
            }
            ZQ_U32A(&z->ht, z->h1 ^ ih) = p;
            z->h1 = (((z->h1 * 5) << z->shift1) +
                     (z->in[z->i + z->minMatch] + 1) * 123456791u) &
                    (z->htsize - 1);
          }
          ++z->i;
        }
      }

      if (lit >= z->maxLiteral) zq_lz_write_literal(z, z->i, &lit);
    }

    if (z->i == z->n) {
      zq_lz_write_literal(z, z->n, &lit);
      zq_lz_flush_bits(z);
    }
  }
}

/* Write literal sequence in[i-lit..i-1], set lit=0 */
static void zq_lz_write_literal(zq_lzbuf *z, unsigned i, unsigned *lit) {
  if (z->level == 1) {
    if (*lit < 1) return;
    {
      int ll = zq_lg(*lit);
      zq_lz_putb(z, 0, 2);
      --ll;
      while (--ll >= 0) {
        zq_lz_putb(z, 1, 1);
        zq_lz_putb(z, (*lit >> ll) & 1, 1);
      }
      zq_lz_putb(z, 0, 1);
      while (*lit) {
        zq_lz_putb(z, z->in[i - *lit], 8);
        --*lit;
      }
    }
  } else {
    while (*lit > 0) {
      unsigned lit1 = *lit;
      unsigned j;
      if (lit1 > 64) lit1 = 64;
      zq_lz_put(z, (int)(lit1 - 1));
      for (j = i - *lit; j < i - *lit + lit1; ++j) zq_lz_put(z, z->in[j]);
      *lit -= lit1;
    }
  }
}

/* Write match sequence of the given length and offset */
static void zq_lz_write_match(zq_lzbuf *z, unsigned len, unsigned off) {
  if (z->level == 1) {
    int ll = zq_lg(len) - 1;
    int lo;
    off += (1u << z->rb) - 1;
    lo = zq_lg(off) - 1 - (int)z->rb;
    zq_lz_putb(z, (unsigned)((lo + 8) >> 3), 2);
    zq_lz_putb(z, (unsigned)(lo & 7), 3);
    while (--ll >= 2) {
      zq_lz_putb(z, 1, 1);
      zq_lz_putb(z, (len >> ll) & 1, 1);
    }
    zq_lz_putb(z, 0, 1);
    zq_lz_putb(z, len & 3, 2);
    zq_lz_putb(z, off, (int)z->rb);
    zq_lz_putb(z, off >> z->rb, lo);
  } else {
    --off;
    while (len > 0) {
      const unsigned len1 = len > z->minMatch * 2 + 63
                                ? z->minMatch + 63
                                : len > z->minMatch + 63 ? len - z->minMatch : len;
      if (off < (1u << 16)) {
        zq_lz_put(z, (int)(64 + len1 - z->minMatch));
        zq_lz_put(z, (int)(off >> 8));
        zq_lz_put(z, (int)off);
      } else if (off < (1u << 24)) {
        zq_lz_put(z, (int)(128 + len1 - z->minMatch));
        zq_lz_put(z, (int)(off >> 16));
        zq_lz_put(z, (int)(off >> 8));
        zq_lz_put(z, (int)off);
      } else {
        zq_lz_put(z, (int)(192 + len1 - z->minMatch));
        zq_lz_put(z, (int)(off >> 24));
        zq_lz_put(z, (int)(off >> 16));
        zq_lz_put(z, (int)(off >> 8));
        zq_lz_put(z, (int)off);
      }
      len -= len1;
    }
  }
}
