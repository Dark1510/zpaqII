/* zpaqII - high-level block compression (libzpaq::compressBlock / compress).
 * Port of libzpaq 7.15 (Unlicense, Matt Mahoney). */
#include "zq_internal.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Small string builder (method strings and comments)                  */
/* ------------------------------------------------------------------ */

typedef struct {
  char *s;
  size_t n, cap;
} zq_sb;

static void zq_sb_add(zq_sb *b, const char *fmt, ...) {
  va_list ap;
  int len;
  va_start(ap, fmt);
  len = vsnprintf(NULL, 0, fmt, ap);
  va_end(ap);
  if (len < 0) zq_error("format error");
  if (b->n + (size_t)len + 1 > b->cap) {
    size_t c = b->cap ? b->cap : 64;
    while (c < b->n + (size_t)len + 1) c *= 2;
    b->s = (char *)zq_realloc(b->s, c);
    b->cap = c;
  }
  va_start(ap, fmt);
  vsnprintf(b->s + b->n, (size_t)len + 1, fmt, ap);
  va_end(ap);
  b->n += (size_t)len;
}

static void zq_sb_adds(zq_sb *b, const char *s) { zq_sb_add(b, "%s", s); }

#define ZQ_MAX(a, b) ((a) > (b) ? (a) : (b))

/* Reader callbacks over a zq_lzbuf */
static int zq_lz_get_cb(void *self) { return zq_lz_get((zq_lzbuf *)self); }
static int zq_lz_read_cb(void *self, char *p, int n) {
  return zq_lz_read((zq_lzbuf *)self, p, n);
}

/* ------------------------------------------------------------------ */
/* compressBlock                                                       */
/* ------------------------------------------------------------------ */

void zq_compress_block(zq_buf *in, zq_writer *out, const char *method_,
                       const char *filename, const char *comment, int dosha1) {
  const unsigned n = (unsigned)zq_buf_size(in);
  const int arg0 = ZQ_MAX(zq_lg(n + 4095) - 20, 0);
  zq_sb method;
  unsigned type = 0;
  zq_sha1 sha1;
  const char *sha1ptr = NULL;
  int args[9];
  char *config;
  zq_compressor co;
  zq_buf *pcmd = zq_buf_new(0);

  memset(&method, 0, sizeof(method));
  zq_sb_adds(&method, method_);

  /* Type from method "LB,R,t": L = level 0..5, B = block size 0..11,
   * R = redundancy 0..255, t = 0..3 (binary, text, exe, both). */
  if (isdigit((unsigned char)method.s[0])) {
    int commas = 0, arg[4] = {0, 0, 0, 0};
    int i;
    for (i = 1; i < (int)method.n && commas < 4; ++i) {
      if (method.s[i] == ',' || method.s[i] == '.')
        ++commas;
      else if (isdigit((unsigned char)method.s[i]))
        arg[commas] = arg[commas] * 10 + method.s[i] - '0';
    }
    type = commas == 0 ? 512u : (unsigned)(arg[1] * 4 + arg[2]);
  }

  /* Hash of the input (before any preprocessing) */
  zq_sha1_init(&sha1);
  if (dosha1) {
    zq_sha1_write(&sha1, zq_buf_data(in), (zq_i64)n);
    sha1ptr = zq_sha1_result(&sha1);
  }

  /* Expand default methods */
  if (isdigit((unsigned char)method.s[0])) {
    const int level = method.s[0] - '0';
    const int doe8 = (int)(type & 2) * 2;
    char htsz[32], sasz[32];

    method.n = 0;
    method.s[0] = 0;
    snprintf(htsz, sizeof(htsz), ",%d", 19 + arg0 + (arg0 <= 6));
    snprintf(sasz, sizeof(sasz), ",%d", 21 + arg0);

    if (level == 0) {
      zq_sb_add(&method, "0%d,0", arg0);
    } else if (level >= 5) {
      /* Slow CM with lots of models (levels 5..9) */
      const int NR = 1 << 12;
      int pt[256], r[4096];
      const unsigned char *p = (const unsigned char *)zq_buf_data(in);
      int n1, i, j;
      zq_sb_add(&method, "x%d,%d", arg0, doe8);
      if (type & 1) zq_sb_adds(&method, "w2c0,1010,255i1");
      else zq_sb_adds(&method, "w1i1");
      zq_sb_adds(&method, "c256ci1,1,1,1,1,1,2a");

      memset(pt, 0, sizeof(pt));
      memset(r, 0, sizeof(r));
      for (i = 0; i < (int)n; ++i) {
        const int k = i - pt[p[i]];
        if (k > 0 && k < NR) ++r[k];
        pt[p[i]] = i;
      }
      n1 = (int)n - r[1] - r[2] - r[3];
      for (i = 0; i < 2; ++i) {
        int period = 0, t = 0;
        double score = 0;
        for (j = 5; j < NR && t < n1; ++j) {
          const double s = r[j] / (256.0 + n1 - t);
          if (s > score) score = s, period = j;
          t += r[j];
        }
        if (period > 4 && score > 0.1) {
          zq_sb_add(&method, "c0,0,%d,255i1", 999 + period);
          if (period <= 255) zq_sb_add(&method, "c0,%di1", period);
          n1 -= r[period];
          r[period] = 0;
        } else {
          break;
        }
      }
      zq_sb_adds(&method,
                 "c0,2,0,255i1c0,3,0,0,255i1c0,4,0,0,0,255i1mm16ts19t0");
    } else {
      zq_sb_add(&method, "x%d", arg0);
      if (level == 1) {
        if (type < 40) {
          zq_sb_adds(&method, ",0");
        } else {
          zq_sb_add(&method, ",%d,", 1 + doe8);
          if (type < 80) zq_sb_adds(&method, "4,0,1,15");
          else if (type < 128) zq_sb_adds(&method, "4,0,2,16");
          else if (type < 256) zq_sb_add(&method, "4,0,2%s", htsz);
          else if (type < 960) zq_sb_add(&method, "5,0,3%s", htsz);
          else zq_sb_add(&method, "6,0,3%s", htsz);
        }
      } else if (level == 2) {
        if (type < 32) {
          zq_sb_adds(&method, ",0");
        } else {
          zq_sb_add(&method, ",%d,", 1 + doe8);
          if (type < 64) zq_sb_add(&method, "4,0,3%s", htsz);
          else zq_sb_add(&method, "4,0,7%s,1", sasz);
        }
      } else if (level == 3) {
        if (type < 20) zq_sb_adds(&method, ",0");
        else if (type < 48) zq_sb_add(&method, ",%d,4,0,3%s", 1 + doe8, htsz);
        else if (type >= 640 || (type & 1))
          zq_sb_add(&method, ",%dci1", 3 + doe8);
        else
          zq_sb_add(&method, ",%d,12,0,7%s,1c0,0,511i2", 2 + doe8, sasz);
      } else { /* level == 4 */
        if (type < 12) zq_sb_adds(&method, ",0");
        else if (type < 24) zq_sb_add(&method, ",%d,4,0,3%s", 1 + doe8, htsz);
        else if (type < 48)
          zq_sb_add(&method, ",%d,5,0,7%s1c0,0,511", 2 + doe8, sasz);
        else if (type < 900) {
          zq_sb_add(&method, ",%dci1,1,1,1,2a", doe8);
          if (type & 1) zq_sb_adds(&method, "w");
          zq_sb_adds(&method, "m");
        } else {
          zq_sb_add(&method, ",%dci1", 3 + doe8);
        }
      }
    }
  }

  config = zq_make_config(method.s, args);
  zq_c_open(&co);
  zq_c_set_output(&co, out);
  {
    zq_writer cw = zq_buf_writer(pcmd);
    zq_c_write_tag(&co);
    zq_c_start_block(&co, config, args, &cw);
  }
  {
    zq_sb cs;
    memset(&cs, 0, sizeof(cs));
    zq_sb_add(&cs, "%u", n);
    if (comment) zq_sb_add(&cs, " %s", comment);
    zq_c_start_segment(&co, filename, cs.s);
    free(cs.s);
  }

  if (args[1] >= 1 && args[1] <= 7 && args[1] != 4) {
    zq_lzbuf *lz = zq_lz_new(in, args, NULL);
    zq_reader r;
    r.get = zq_lz_get_cb;
    r.read = zq_lz_read_cb;
    r.self = lz;
    zq_c_set_input(&co, &r);
    zq_c_compress(&co, -1);
    zq_lz_free(lz);
  } else {
    zq_reader r;
    if (args[1] >= 4 && args[1] <= 7)
      zq_e8e9((unsigned char *)zq_buf_data(in), (int)n);
    r = zq_buf_reader(in);
    zq_c_set_input(&co, &r);
    zq_c_compress(&co, -1);
  }

  zq_c_end_segment(&co, sha1ptr);
  zq_c_end_block(&co);
  zq_c_close(&co);
  free(config);
  free(method.s);
  zq_buf_free(pcmd);
}

/* ------------------------------------------------------------------ */
/* compress(): multiple blocks                                         */
/* ------------------------------------------------------------------ */

void zq_compress(zq_reader *in, zq_writer *out, const char *method,
                 const char *filename, const char *comment, int dosha1) {
  int bs = 4;
  zq_buf *sb;
  char *scratch;

  if (method && method[0] && method[1] >= '0' && method[1] <= '9') {
    bs = method[1] - '0';
    if (method[2] >= '0' && method[2] <= '9') bs = bs * 10 + method[2] - '0';
    if (bs > 11) bs = 11;
  }
  bs = (0x100000 << bs) - 4096;

  sb = zq_buf_new((size_t)bs);
  zq_buf_write(sb, NULL, bs); /* reserve the block buffer */
  scratch = (char *)zq_buf_data(sb);

  for (;;) {
    int nr = zq_reader_read(in, scratch, bs);
    if (nr <= 0) break;
    zq_buf_resize(sb, (size_t)nr);
    zq_compress_block(sb, out, method, filename, comment, dosha1);
    filename = NULL;
    comment = NULL;
    zq_buf_resize(sb, 0);
  }
  zq_buf_free(sb);
}
