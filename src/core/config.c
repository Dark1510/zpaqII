/* zpaqII - method string to ZPAQL config text (libzpaq::makeConfig).
 * Port of libzpaq 7.15 (Unlicense, Matt Mahoney).
 *
 * The fixed postprocessor programs and the level-2 HCOMP prologue are stored
 * in config_tpl.c, generated from upstream by tools/gen_config.cpp.  The
 * per-component context-model code is generated here with the same rules. */
#include "zq_internal.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const char *const zq_tpl_l1[2][9];
extern const char *const zq_tpl_l2[2];
extern const char *const zq_tpl_l3[2][2];
extern const char *const zq_tpl_l0[2];
extern const char *const zq_tpl_hcomp_l2[2];

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

typedef struct {
  char *s;
  size_t n, cap;
} zq_sb;

static void zq_sb_need(zq_sb *b, size_t extra) {
  if (b->n + extra + 1 <= b->cap) return;
  {
    size_t c = b->cap ? b->cap : 256;
    while (c < b->n + extra + 1) c *= 2;
    b->s = (char *)zq_realloc(b->s, c);
    b->cap = c;
  }
}

static void zq_sb_add(zq_sb *b, const char *fmt, ...) {
  va_list ap;
  int len;
  va_start(ap, fmt);
  len = vsnprintf(NULL, 0, fmt, ap);
  va_end(ap);
  if (len < 0) zq_error("format error");
  zq_sb_need(b, (size_t)len);
  va_start(ap, fmt);
  vsnprintf(b->s + b->n, (size_t)len + 1, fmt, ap);
  va_end(ap);
  b->n += (size_t)len;
}

static void zq_sb_adds(zq_sb *b, const char *s) {
  const size_t len = strlen(s);
  zq_sb_need(b, len);
  memcpy(b->s + b->n, s, len);
  b->n += len;
  b->s[b->n] = 0;
}

static char *zq_sb_take(zq_sb *b) {
  char *s = b->s;
  b->s = NULL;
  b->n = b->cap = 0;
  return s;
}

/* non-negative decimal, at least n digits (libzpaq::itos) */
static void zq_itos(zq_sb *b, int x, int n) {
  char tmp[32];
  int i = 0;
  if (x < 0) x = 0;
  do {
    tmp[i++] = (char)('0' + x % 10);
    x /= 10;
    if (n > 0) --n;
  } while (x || n > 0);
  while (i > 0) zq_sb_need(b, 1), b->s[b->n++] = tmp[--i];
  b->s[b->n] = 0;
}

/* ------------------------------------------------------------------ */
/* makeConfig                                                          */
/* ------------------------------------------------------------------ */

char *zq_make_config(const char *method_in, int args[9]) {
  const char *method = method_in;
  const char type = *method;
  zq_sb hdr, pcomp, comp, hcomp, out;
  const char *pcomp_txt = NULL;
  const char *hdr_txt = NULL;
  int level, doe8, ncomp = 0, membits, sb, i, j;
  memset(&hdr, 0, sizeof(hdr));
  memset(&pcomp, 0, sizeof(pcomp));
  memset(&comp, 0, sizeof(comp));
  memset(&hcomp, 0, sizeof(hcomp));
  memset(&out, 0, sizeof(out));

  if (type != 'x' && type != 's' && type != '0' && type != 'i')
    zq_error("Unsupported method");

  /* Read "{x|s|i|0}N1,N2...N9" into args[0..8] ($1..$9) */
  for (i = 0; i < 9; ++i) args[i] = 0;
  ++method;
  for (i = 0; i < 9 && (isdigit((unsigned char)*method) || *method == ',' ||
                        *method == '.');) {
    if (isdigit((unsigned char)*method))
      args[i] = args[i] * 10 + *method - '0';
    else if (++i < 9)
      args[i] = 0;
    ++method;
  }

  /* "0..." = no compression */
  if (type == '0') {
    char *r = (char *)zq_calloc(25);
    memcpy(r, "comp 0 0 0 0 0 hcomp end\n", 25);
    return r;
  }

  level = args[1] & 3;
  doe8 = args[1] >= 4 && args[1] <= 7;

  switch (level) {
    case 0:
      hdr_txt = "comp 9 16 0 0 ";
      pcomp_txt = zq_tpl_l0[doe8];
      break;
    case 1: {
      const int rb = args[0] > 4 ? args[0] - 4 : 0;
      /* 7.15 generates the text for any rb but its compiler rejects rb >= 9
       * ("a&= 511": number too high); the templates cover rb 0..8. */
      if (rb > 8) zq_error("number too high");
      hdr_txt = "comp 9 16 0 $1+20 ";
      pcomp_txt = zq_tpl_l1[doe8][rb];
      break;
    }
    case 2:
      hdr_txt = "comp 9 16 0 $1+20 ";
      pcomp_txt = zq_tpl_l2[doe8];
      break;
    case 3:
      hdr_txt = "comp 9 16 $1+20 $1+20 ";
      pcomp_txt = zq_tpl_l3[doe8][args[0] <= 4];
      break;
    default:
      zq_error("Unsupported method");
  }
  zq_sb_adds(&hdr, hdr_txt);

  /* Build the context model assuming:
   *   H[0..254] = contexts
   *   H[255..511] = location of last byte i-255
   *   M = last 64K bytes, filling backward
   *   C = pointer to most recent byte
   *   R1 = level 2 lz77 1+bytes expected until next code, 0=init
   *   R2 = level 2 lz77 first byte of code */
  membits = args[0] + 20;
  sb = 5;
  zq_sb_adds(&hcomp, "hcomp\nc-- *c=a a+= 255 d=a *d=c\n");
  if (level == 2) zq_sb_adds(&hcomp, zq_tpl_hcomp_l2[doe8]);

  while (*method && ncomp < 254) {
    int v[16];
    size_t vn = 0;

    /* parse C[N1[,N2]...] into v = {C, N1, N2...} */
    memset(v, 0, sizeof(v));
    v[vn++] = *method++;
    if (isdigit((unsigned char)*method)) {
      v[vn++] = *method++ - '0';
      while (isdigit((unsigned char)*method) || *method == ',' ||
             *method == '.') {
        if (isdigit((unsigned char)*method))
          v[vn - 1] = v[vn - 1] * 10 + *method++ - '0';
        else {
          if (vn >= 16) zq_error("method too long");
          v[vn++] = 0;
          ++method;
        }
      }
    }

    /* c: context model */
    if (v[0] == 'c') {
      while (vn < 3) v[vn++] = 0;
      zq_sb_add(&comp, "%d ", ncomp);
      sb = 11;
      if (v[2] < 256) sb += zq_lg((unsigned)v[2]);
      else sb += 6;
      for (i = 3; i < (int)vn; ++i)
        if (v[i] < 512) sb += zq_popcount((unsigned)v[i]) * 3 / 4;
      if (sb > membits) sb = membits;
      if (v[1] % 1000 == 0)
        zq_sb_add(&comp, "icm %d\n", sb - 6 - v[1] / 1000);
      else
        zq_sb_add(&comp, "cm %d %d\n", sb - 2 - v[1] / 1000, v[1] % 1000 - 1);

      /* special contexts */
      zq_sb_add(&hcomp, "d= %d *d=0\n", ncomp);
      if (v[2] > 1 && v[2] <= 255) {
        if (zq_lg((unsigned)v[2]) != zq_lg((unsigned)(v[2] - 1)))
          zq_sb_add(&hcomp, "a=c a&= %d hashd\n", v[2] - 1);
        else
          zq_sb_add(&hcomp, "a=c a%%= %d hashd\n", v[2]);
      } else if (v[2] >= 1000 && v[2] <= 1255) {
        zq_sb_add(&hcomp,
                  "a= 255 a+= %d d=a a=*d a-=c a> 255 if a= 255 endif d= %d "
                  "hashd\n",
                  v[2] - 1000, ncomp);
      }

      /* masked context */
      for (i = 3; i < (int)vn; ++i) {
        if (i == 3) zq_sb_adds(&hcomp, "b=c ");
        if (v[i] == 255) {
          zq_sb_adds(&hcomp, "a=*b hashd\n");
        } else if (v[i] > 0 && v[i] < 255) {
          zq_sb_add(&hcomp, "a=*b a&= %d hashd\n", v[i]);
        } else if (v[i] >= 256 && v[i] < 512) {
          zq_sb_adds(&hcomp, "a=r 1 a> 1 if\n  a=r 2 a< 64 if\n    a=*b ");
          if (v[i] < 511) zq_sb_add(&hcomp, "a&= %d", v[i] - 256);
          zq_sb_adds(&hcomp,
                     " hashd\n  else\n    a>>= 6 hashd a=r 1 hashd\n  endif\n"
                     "else\n  a= 255 hashd a=r 2 hashd\nendif\n");
        } else if (v[i] >= 1256) {
          zq_sb_add(&hcomp, "a= %d a<<= 8 a+= %d a+=b b=a\n",
                    ((v[i] - 1000) >> 8) & 255, (v[i] - 1000) & 255);
        } else if (v[i] > 1000) {
          zq_sb_add(&hcomp, "a= %d a+=b b=a\n", v[i] - 1000);
        }
        if (v[i] < 512 && i < (int)vn - 1) zq_sb_adds(&hcomp, "b++ ");
      }
      ++ncomp;
    }

    /* m,8,24: MIX; t,8,24: MIX2; s,8,32,255: SSE */
    if (strchr("mts", v[0]) && ncomp > (v[0] == 't')) {
      if (vn <= 1) v[vn++] = 8;
      if (vn <= 2) v[vn++] = 24 + 8 * (v[0] == 's');
      if (v[0] == 's' && vn <= 3) v[vn++] = 255;
      zq_sb_add(&comp, "%d", ncomp);
      sb = 5 + v[1] * 3 / 4;
      if (v[0] == 'm')
        zq_sb_add(&comp, " mix %d 0 %d %d 255\n", v[1], ncomp, v[2]);
      else if (v[0] == 't')
        zq_sb_add(&comp, " mix2 %d %d %d %d 255\n", v[1], ncomp - 1,
                  ncomp - 2, v[2]);
      else
        zq_sb_add(&comp, " sse %d %d %d %d\n", v[1], ncomp - 1, v[2], v[3]);
      if (v[1] > 8) {
        zq_sb_add(&hcomp, "d= %d *d=0 b=c a=0\n", ncomp);
        for (; v[1] >= 16; v[1] -= 8) {
          zq_sb_adds(&hcomp, "a<<= 8 a+=*b");
          if (v[1] > 16) zq_sb_adds(&hcomp, " b++");
          zq_sb_adds(&hcomp, "\n");
        }
        if (v[1] > 8)
          zq_sb_add(&hcomp, "a<<= 8 a+=*b a>>= %d\n", 16 - v[1]);
        zq_sb_adds(&hcomp, "a<<= 8 *d=a\n");
      }
      ++ncomp;
    }

    /* i: ISSE chain with order increasing by N1, N2... */
    if (v[0] == 'i' && ncomp > 0) {
      zq_sb_add(&hcomp, "d= %d b=c a=*d d++\n", ncomp - 1);
      for (i = 1; i < (int)vn && ncomp < 254; ++i) {
        for (j = 0; j < v[i] % 10; ++j) {
          zq_sb_adds(&hcomp, "hash ");
          if (i < (int)vn - 1 || j < v[i] % 10 - 1) zq_sb_adds(&hcomp, "b++ ");
          sb += 6;
        }
        zq_sb_adds(&hcomp, "*d=a");
        if (i < (int)vn - 1) zq_sb_adds(&hcomp, " d++");
        zq_sb_adds(&hcomp, "\n");
        if (sb > membits) sb = membits;
        zq_sb_add(&comp, "%d isse %d %d\n", ncomp, sb - 6 - v[i] / 10,
                  ncomp - 1);
        ++ncomp;
      }
    }

    /* a24,0,0: MATCH. N1=hash multiplier, N2,N3=halve buf, table */
    if (v[0] == 'a') {
      if (vn <= 1) v[vn++] = 24;
      while (vn < 4) v[vn++] = 0;
      zq_sb_add(&comp, "%d match %d %d\n", ncomp, membits - v[3] - 2,
                membits - v[2]);
      zq_sb_add(&hcomp, "d= %d a=*d a*= %d a+=*c a++ *d=a\n", ncomp, v[1]);
      sb = 5 + (membits - v[2]) * 3 / 4;
      ++ncomp;
    }

    /* w1,65,26,223,20,0: ICM-ISSE chain with word contexts */
    if (v[0] == 'w') {
      if (vn <= 1) v[vn++] = 1;
      if (vn <= 2) v[vn++] = 65;
      if (vn <= 3) v[vn++] = 26;
      if (vn <= 4) v[vn++] = 223;
      if (vn <= 5) v[vn++] = 20;
      if (vn <= 6) v[vn++] = 0;
      zq_sb_add(&comp, "%d icm %d\n", ncomp, membits - 6 - v[6]);
      for (i = 1; i < v[1]; ++i)
        zq_sb_add(&comp, "%d isse %d %d\n", ncomp + i, membits - 6 - v[6],
                  ncomp + i - 1);
      zq_sb_add(&hcomp, "a=*c a&= %d a-= %d a&= 255 a< %d if\n", v[4], v[2],
                v[3]);
      for (i = 0; i < v[1]; ++i) {
        if (i == 0) zq_sb_add(&hcomp, "  d= %d", ncomp);
        else zq_sb_adds(&hcomp, "  d++");
        zq_sb_add(&hcomp, " a=*d a*= %d a+=*c a++ *d=a\n", v[5]);
      }
      zq_sb_adds(&hcomp, "else\n");
      for (i = v[1] - 1; i > 0; --i)
        zq_sb_add(&hcomp, "  d= %d a=*d d++ *d=a\n", ncomp + i - 1);
      zq_sb_add(&hcomp, "  d= %d *d=0\nendif\n", ncomp);
      ncomp += v[1] - 1;
      sb = membits - v[6];
      ++ncomp;
    }
  }

  /* hdr + itos(ncomp) + "\n" + comp + hcomp + "halt\n" + pcomp */
  zq_sb_adds(&out, hdr.s ? hdr.s : "");
  zq_itos(&out, ncomp, 1);
  zq_sb_adds(&out, "\n");
  if (comp.s) zq_sb_adds(&out, comp.s);
  if (hcomp.s) zq_sb_adds(&out, hcomp.s);
  zq_sb_adds(&out, "halt\n");
  if (pcomp_txt) zq_sb_adds(&out, pcomp_txt);
  else if (pcomp.s) zq_sb_adds(&out, pcomp.s);

  free(hdr.s);
  free(comp.s);
  free(hcomp.s);
  free(pcomp.s);
  return zq_sb_take(&out);
}
