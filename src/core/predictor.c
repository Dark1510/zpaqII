/* zpaqII - context-mixing predictor.
 * Port of libzpaq 7.15 (Unlicense, Matt Mahoney).
 *
 * Bit-exactness notes: upstream relies on 32-bit two's-complement wrap-around
 * in a few places (model weight updates and the `train` rule).  In C that
 * would be signed overflow (UB), so those operations go through zq_wmul /
 * zq_wadd and the results are cast back to int32_t, which reproduces the
 * machine behavior of the reference build.
 */
#include "zq_internal.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Wrapping 32-bit arithmetic (matches the reference build)            */
/* ------------------------------------------------------------------ */

static inline int32_t zq_wmul(int32_t a, int32_t b) {
  return (int32_t)((zq_u32)a * (zq_u32)b);
}

static inline int32_t zq_wadd(int32_t a, int32_t b) {
  return (int32_t)((zq_u32)a + (zq_u32)b);
}

/* Masked accesses, matching Array::operator() */
#define CMV(cr, i) ZQ_U32A(&(cr)->cm, (size_t)(i) & ((cr)->cm.n - 1))
#define HTV(cr, i) ZQ_U8A(&(cr)->ht, (size_t)(i) & ((cr)->ht.n - 1))

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

void zq_pred_open(zq_predictor *pr, zq_zpaql *z) {
  memset(pr, 0, sizeof(*pr));
  pr->c8 = 1;
  pr->hmap4 = 1;
  pr->z = z;
  zq_st_init(&pr->st);
}

void zq_pred_free(zq_predictor *pr) {
  int i;
  for (i = 0; i < 256; ++i) {
    zq_arr_free(&pr->comp[i].cm);
    zq_arr_free(&pr->comp[i].ht);
    zq_arr_free(&pr->comp[i].a16);
  }
  if (pr->pcode) {
    free(pr->pcode);
    pr->pcode = NULL;
    pr->pcode_size = 0;
  }
}

int zq_pred_is_modeled(const zq_predictor *pr) {
  return pr->z->header.n > 6 && ZQ_HDR(pr->z, 6) != 0;
}

/* ------------------------------------------------------------------ */
/* Lookup helpers                                                      */
/* ------------------------------------------------------------------ */

static int zq_squash(zq_predictor *pr, int x) { return pr->squasht[x + 2048]; }

static int zq_stretch(zq_predictor *pr, int x) { return pr->stretcht[x]; }

static int zq_clamp2k(int x) {
  if (x < -2048) return -2048;
  if (x > 2047) return 2047;
  return x;
}

static int32_t zq_clamp512k(int32_t x) {
  if (x < -(1 << 19)) return -(1 << 19);
  if (x >= (1 << 19)) return (1 << 19) - 1;
  return x;
}

/* Reduce prediction error in cr.cm */
static void zq_train(zq_predictor *pr, zq_component *cr, int y) {
  zq_u32 *pn = &ZQ_U32A(&cr->cm, cr->cxt & (cr->cm.n - 1));
  const zq_u32 count = *pn & 0x3ff;
  const int32_t error = (int32_t)(y * 32767 - (int32_t)(*pn >> 17));
  int32_t delta = zq_wmul(error, (int32_t)pr->dt[count]);
  delta = (int32_t)((zq_u32)delta & 0xFFFFFC00u); /* & -1024 */
  *pn = (zq_u32)zq_wadd((int32_t)*pn, delta) + (zq_u32)(count < cr->limit);
}

/* Find cxt row in hash table ht (rows of 16). */
size_t zq_pred_find(zq_predictor *pr, zq_arr *ht, int sizebits, zq_u32 cxt) {
  const int chk = (int)(cxt >> sizebits) & 255;
  const size_t h0 = ((size_t)cxt * 16) & (ht->n - 16);
  const size_t h1 = h0 ^ 16;
  const size_t h2 = h0 ^ 32;
  (void)pr;
  if (ZQ_U8A(ht, h0) == chk) return h0;
  if (ZQ_U8A(ht, h1) == chk) return h1;
  if (ZQ_U8A(ht, h2) == chk) return h2;
  if (ZQ_U8A(ht, h0 + 1) <= ZQ_U8A(ht, h1 + 1) &&
      ZQ_U8A(ht, h0 + 1) <= ZQ_U8A(ht, h2 + 1)) {
    memset(&ZQ_U8A(ht, h0), 0, 16);
    ZQ_U8A(ht, h0) = (zq_u8)chk;
    return h0;
  } else if (ZQ_U8A(ht, h1 + 1) < ZQ_U8A(ht, h2 + 1)) {
    memset(&ZQ_U8A(ht, h1), 0, 16);
    ZQ_U8A(ht, h1) = (zq_u8)chk;
    return h1;
  } else {
    memset(&ZQ_U8A(ht, h2), 0, 16);
    ZQ_U8A(ht, h2) = (zq_u8)chk;
    return h2;
  }
}

/* ------------------------------------------------------------------ */
/* init(): build the model from the COMP list                          */
/* ------------------------------------------------------------------ */

void zq_pred_init(zq_predictor *pr) {
  zq_zpaql *z = pr->z;
  int i, n, cp;

  /* Drop any old JIT code (no JIT yet: pcode is always NULL). */
  if (pr->pcode) {
    free(pr->pcode);
    pr->pcode = NULL;
    pr->pcode_size = 0;
  }

  /* Context hash function */
  zq_zpaql_inith(z);

  /* Model-independent tables */
  if (!pr->initTables && zq_pred_is_modeled(pr)) {
    pr->initTables = 1;
    memcpy(pr->dt2k, zq_sdt2k, sizeof(pr->dt2k));
    memcpy(pr->dt, zq_sdt, sizeof(pr->dt));

    memset(pr->squasht, 0, 1376 * 2);
    memcpy(pr->squasht + 1376, zq_ssquasht, 1344 * 2);
    for (i = 2720; i < 4096; ++i) pr->squasht[i] = 32767;

    /* stretcht[i] = round(64*log((i+0.5)/(32767.5-i))) */
    {
      int k = 16384;
      for (i = 0; i < 712; ++i)
        for (int j = zq_stdt[i]; j > 0; --j) pr->stretcht[k++] = (short)i;
      for (i = 0; i < 16384; ++i) pr->stretcht[i] = (short)-pr->stretcht[32767 - i];
    }
  }

  /* Predictions */
  for (i = 0; i < 256; ++i) {
    pr->h[i] = 0;
    pr->p[i] = 0;
  }

  /* Components */
  for (i = 0; i < 256; ++i) zq_component_init(&pr->comp[i]);

  n = ZQ_HDR(z, 6);
  cp = 7;
  for (i = 0; i < n; ++i) {
    zq_component *cr = &pr->comp[i];
    const zq_u8 *c = &ZQ_HDR(z, cp);
    switch (c[0]) {
      case ZQ_CONS: /* c */
        pr->p[i] = (c[1] - 128) * 4;
        break;
      case ZQ_CM: /* sizebits limit */
        if (c[1] > 32) zq_error("max size for CM is 32");
        zq_arr_resize(&cr->cm, (size_t)1 << c[1], 4);
        cr->limit = (size_t)c[2] * 4;
        for (zq_u32 j = 0; j < cr->cm.n; ++j) ZQ_U32A(&cr->cm, j) = 0x80000000u;
        break;
      case ZQ_ICM: /* sizebits */
        if (c[1] > 26) zq_error("max size for ICM is 26");
        cr->limit = 1023;
        zq_arr_resize(&cr->cm, 256, 4);
        zq_arr_resize(&cr->ht, (size_t)64 << c[1], 1);
        for (zq_u32 j = 0; j < cr->cm.n; ++j)
          ZQ_U32A(&cr->cm, j) = (zq_u32)zq_st_cminit(&pr->st, (int)j);
        break;
      case ZQ_MATCH: /* sizebits bufbits */
        if (c[1] > 32 || c[2] > 32) zq_error("max size for MATCH is 32 32");
        zq_arr_resize(&cr->cm, (size_t)1 << c[1], 4);
        zq_arr_resize(&cr->ht, (size_t)1 << c[2], 1);
        ZQ_U8A(&cr->ht, 0) = 1;
        break;
      case ZQ_AVG: /* j k wt */
        if (c[1] >= i) zq_error("AVG j >= i");
        if (c[2] >= i) zq_error("AVG k >= i");
        break;
      case ZQ_MIX2: /* sizebits j k rate mask */
        if (c[1] > 32) zq_error("max size for MIX2 is 32");
        if (c[3] >= i) zq_error("MIX2 k >= i");
        if (c[2] >= i) zq_error("MIX2 j >= i");
        cr->c = (size_t)1 << c[1];
        zq_arr_resize(&cr->a16, (size_t)1 << c[1], 2);
        for (zq_u32 j = 0; j < cr->a16.n; ++j) ZQ_U16A(&cr->a16, j) = 32768;
        break;
      case ZQ_MIX: { /* sizebits j m rate mask */
        int m = c[3];
        if (c[1] > 32) zq_error("max size for MIX is 32");
        if (c[2] >= i) zq_error("MIX j >= i");
        if (c[3] < 1 || c[3] > i - c[2]) zq_error("MIX m not in 1..i-j");
        cr->c = (size_t)1 << c[1];
        zq_arr_resize(&cr->cm, (size_t)m << c[1], 4);
        for (zq_u32 j = 0; j < cr->cm.n; ++j)
          ZQ_U32A(&cr->cm, j) = (zq_u32)(65536 / m);
        break;
      }
      case ZQ_ISSE: /* sizebits j */
        if (c[1] > 32) zq_error("max size for ISSE is 32");
        if (c[2] >= i) zq_error("ISSE j >= i");
        zq_arr_resize(&cr->ht, (size_t)64 << c[1], 1);
        zq_arr_resize(&cr->cm, 512, 4);
        for (int j = 0; j < 256; ++j) {
          ZQ_U32A(&cr->cm, j * 2) = 1u << 15;
          ZQ_U32A(&cr->cm, j * 2 + 1) =
              (zq_u32)zq_clamp512k((int32_t)zq_stretch(
                  pr, zq_st_cminit(&pr->st, j) >> 8) * 1024);
        }
        break;
      case ZQ_SSE: /* sizebits j start limit */
        if (c[1] > 32) zq_error("max size for SSE is 32");
        if (c[2] >= i) zq_error("SSE j >= i");
        if (c[3] > c[4] * 4) zq_error("SSE start > limit*4");
        zq_arr_resize(&cr->cm, (size_t)32 << c[1], 4);
        cr->limit = (size_t)c[4] * 4;
        for (zq_u32 j = 0; j < cr->cm.n; ++j)
          ZQ_U32A(&cr->cm, j) =
              ((zq_u32)zq_squash(pr, (int)(j & 31) * 64 - 992) << 17) |
              (zq_u32)c[3];
        break;
      default:
        zq_error("unknown component type");
    }
    cp += zq_compsize[c[0]];
  }
}

/* ------------------------------------------------------------------ */
/* predict0 / update0                                                  */
/* ------------------------------------------------------------------ */

static int zq_pred_predict0(zq_predictor *pr) {
  zq_zpaql *z = pr->z;
  const int n = ZQ_HDR(z, 6);
  int cp = 7, i;

  for (i = 0; i < n; ++i) {
    zq_component *cr = &pr->comp[i];
    const zq_u8 *c = &ZQ_HDR(z, cp);
    switch (c[0]) {
      case ZQ_CONS:
        break;
      case ZQ_CM:
        cr->cxt = (size_t)(pr->h[i] ^ (zq_u32)pr->hmap4);
        pr->p[i] = zq_stretch(pr, (int)(CMV(cr, cr->cxt) >> 17));
        break;
      case ZQ_ICM:
        if (pr->c8 == 1 || (pr->c8 & 0xf0) == 16)
          cr->c = zq_pred_find(pr, &cr->ht, c[1] + 2,
                               pr->h[i] + 16 * (zq_u32)pr->c8);
        cr->cxt = ZQ_U8A(&cr->ht, cr->c + (size_t)(pr->hmap4 & 15));
        pr->p[i] = zq_stretch(pr, (int)(CMV(cr, cr->cxt) >> 8));
        break;
      case ZQ_MATCH:
        if (cr->a == 0) {
          pr->p[i] = 0;
        } else {
          cr->c = (HTV(cr, cr->limit - cr->b) >> (7 - cr->cxt)) & 1;
          pr->p[i] = zq_stretch(
              pr, (pr->dt2k[cr->a] * (1 - 2 * (int)cr->c)) & 32767);
        }
        break;
      case ZQ_AVG:
        pr->p[i] = (pr->p[c[1]] * c[3] + pr->p[c[2]] * (256 - c[3])) >> 8;
        break;
      case ZQ_MIX2: {
        int w;
        cr->cxt = (size_t)((pr->h[i] + (zq_u32)(pr->c8 & c[5])) & (cr->c - 1));
        w = ZQ_U16A(&cr->a16, cr->cxt);
        pr->p[i] = (w * pr->p[c[2]] + (65536 - w) * pr->p[c[3]]) >> 16;
        break;
      }
      case ZQ_MIX: {
        const int m = c[3];
        const zq_u32 *wt;
        int32_t s = 0;
        int j;
        cr->cxt = (size_t)(pr->h[i] + (zq_u32)(pr->c8 & c[5]));
        cr->cxt = (cr->cxt & (cr->c - 1)) * (size_t)m;
        wt = &ZQ_U32A(&cr->cm, cr->cxt);
        for (j = 0; j < m; ++j)
          s = zq_wadd(s, zq_wmul(((int32_t)wt[j]) >> 8, pr->p[c[2] + j]));
        pr->p[i] = zq_clamp2k(s >> 8);
        break;
      }
      case ZQ_ISSE: {
        const zq_u32 *wt;
        int32_t v;
        if (pr->c8 == 1 || (pr->c8 & 0xf0) == 16)
          cr->c = zq_pred_find(pr, &cr->ht, c[1] + 2,
                               pr->h[i] + 16 * (zq_u32)pr->c8);
        cr->cxt = ZQ_U8A(&cr->ht, cr->c + (size_t)(pr->hmap4 & 15));
        wt = &ZQ_U32A(&cr->cm, cr->cxt * 2);
        v = zq_wadd(zq_wmul((int32_t)wt[0], pr->p[c[2]]),
                    zq_wmul((int32_t)wt[1], 64));
        pr->p[i] = zq_clamp2k(v >> 16);
        break;
      }
      case ZQ_SSE: {
        int pq = pr->p[c[2]] + 992;
        int wt;
        zq_u32 a1, a2;
        if (pq < 0) pq = 0;
        if (pq > 1983) pq = 1983;
        wt = pq & 63;
        pq >>= 6;
        cr->cxt = (size_t)((pr->h[i] + (zq_u32)pr->c8) * 32 + (zq_u32)pq);
        a1 = CMV(cr, cr->cxt) >> 10;
        a2 = CMV(cr, cr->cxt + 1) >> 10;
        pr->p[i] = zq_stretch(
            pr, (int)((a1 * (zq_u32)(64 - wt) + a2 * (zq_u32)wt) >> 13));
        cr->cxt += (size_t)(wt >> 5);
        break;
      }
      default:
        zq_error("component predict not implemented");
    }
    cp += zq_compsize[c[0]];
  }
  return zq_squash(pr, pr->p[n - 1]);
}

static void zq_pred_update0(zq_predictor *pr, int y) {
  zq_zpaql *z = pr->z;
  const int n = ZQ_HDR(z, 6);
  int cp = 7, i;

  for (i = 0; i < n; ++i) {
    zq_component *cr = &pr->comp[i];
    const zq_u8 *c = &ZQ_HDR(z, cp);
    switch (c[0]) {
      case ZQ_CONS:
        break;
      case ZQ_CM:
        zq_train(pr, cr, y);
        break;
      case ZQ_ICM: {
        zq_u8 *hp = &ZQ_U8A(&cr->ht, cr->c + (size_t)(pr->hmap4 & 15));
        zq_u32 *pn = &CMV(cr, cr->cxt);
        int32_t d;
        *hp = (zq_u8)zq_st_next(&pr->st, *hp, y);
        d = (int32_t)((zq_u32)(y * 32767) - (*pn >> 8)) >> 2;
        *pn = (zq_u32)zq_wadd((int32_t)*pn, d);
        break;
      }
      case ZQ_MATCH:
        if ((int)cr->c != y) cr->a = 0;
        {
          zq_u8 *hp = &HTV(cr, cr->limit);
          *hp = (zq_u8)(*hp + *hp + y);
        }
        if (++cr->cxt == 8) {
          cr->cxt = 0;
          ++cr->limit;
          cr->limit &= ((size_t)1 << c[2]) - 1;
          if (cr->a == 0) {
            cr->b = cr->limit - CMV(cr, pr->h[i]);
            if (cr->b & (cr->ht.n - 1)) {
              while (cr->a < 255 &&
                     HTV(cr, cr->limit - cr->a - 1) ==
                         HTV(cr, cr->limit - cr->a - cr->b - 1))
                ++cr->a;
            }
          } else {
            cr->a += cr->a < 255;
          }
          CMV(cr, pr->h[i]) = (zq_u32)cr->limit;
        }
        break;
      case ZQ_AVG:
        break;
      case ZQ_MIX2: {
        const int32_t err =
            ((y * 32767 - zq_squash(pr, pr->p[i])) * c[4]) >> 5;
        int32_t w = ZQ_U16A(&cr->a16, cr->cxt);
        w = zq_wadd(w, zq_wadd(zq_wmul(err, pr->p[c[2]] - pr->p[c[3]]),
                               1 << 12) >> 13);
        if (w < 0) w = 0;
        if (w > 65535) w = 65535;
        ZQ_U16A(&cr->a16, cr->cxt) = (zq_u16)w;
        break;
      }
      case ZQ_MIX: {
        const int m = c[3];
        const int32_t err =
            ((y * 32767 - zq_squash(pr, pr->p[i])) * c[4]) >> 4;
        zq_u32 *wt = &ZQ_U32A(&cr->cm, cr->cxt);
        int j;
        for (j = 0; j < m; ++j) {
          int32_t v = zq_wadd(
              (int32_t)wt[j],
              zq_wadd(zq_wmul(err, pr->p[c[2] + j]), 1 << 12) >> 13);
          wt[j] = (zq_u32)zq_clamp512k(v);
        }
        break;
      }
      case ZQ_ISSE: {
        const int32_t err = y * 32767 - zq_squash(pr, pr->p[i]);
        zq_u32 *wt = &ZQ_U32A(&cr->cm, cr->cxt * 2);
        wt[0] = (zq_u32)zq_clamp512k(zq_wadd(
            (int32_t)wt[0],
            zq_wadd(zq_wmul(err, pr->p[c[2]]), 1 << 12) >> 13));
        wt[1] = (zq_u32)zq_clamp512k(zq_wadd((int32_t)wt[1], (err + 16) >> 5));
        ZQ_U8A(&cr->ht, cr->c + (size_t)(pr->hmap4 & 15)) =
            (zq_u8)zq_st_next(&pr->st, (int)cr->cxt, y);
        break;
      }
      case ZQ_SSE:
        zq_train(pr, cr, y);
        break;
      default:
        zq_error("component update not implemented");
    }
    cp += zq_compsize[c[0]];
  }

  /* Save bit y in c8, hmap4 */
  pr->c8 += pr->c8 + y;
  if (pr->c8 >= 256) {
    zq_zpaql_run(z, (zq_u32)(pr->c8 - 256));
    pr->hmap4 = 1;
    pr->c8 = 1;
    for (i = 0; i < n; ++i) pr->h[i] = ZQ_H(z, i);
  } else if (pr->c8 >= 16 && pr->c8 < 32) {
    pr->hmap4 = ((pr->hmap4 & 0xf) << 5) | (y << 4) | 1;
  } else {
    pr->hmap4 = (pr->hmap4 & 0x1f0) | (((pr->hmap4 & 0xf) * 2 + y) & 0xf);
  }
}

int zq_pred_predict(zq_predictor *pr) { return zq_pred_predict0(pr); }

void zq_pred_update(zq_predictor *pr, int y) { zq_pred_update0(pr, y); }
