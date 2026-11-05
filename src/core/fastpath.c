/* zpaqII - ZPAQL(II) native fast paths for the PCOMPs of zpaq 7.15's own
 * methods.
 *
 * libzpaq::makeConfig() generates four families of postprocessor:
 *
 *   lazy2   LZ77 with variable-length codes  (level 1, +E8E9, rb = args[0]-4)
 *   lzpre   byte-aligned LZ77                (level 2, +E8E9, $3 = args[2])
 *   bwtrle  inverse BWT                      (level 3, +E8E9, two traversals)
 *   e8e9    E8E9 alone                       (level 0 with E8E9)
 *
 * When a block's PCOMP is byte for byte one of them, the program is run here,
 * in C, instead of in the VM.  Recognition is exact: the parameters are read
 * from the bytecode, the template is compiled again with them, and the result
 * must equal the block's program; anything else stays on the JIT/interpreter.
 * Archives are unchanged and plain zpaq 7.15 still runs the ZPAQL.
 *
 * Each function below is a line-by-line transcription of the ZPAQL program
 * (quoted in the comments): the same operations on the same 32-bit registers
 * a, b, c, d, the same R registers and the same masked M and H, so the machine
 * state after every input is the VM's (only f, which is dead at HALT in all
 * four programs, is not kept, exactly as the JIT does not keep it).
 * tests/unit/fastpath_dif.c checks this against the one-step reference.
 */
#include "zq_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { FP_NONE = 0, FP_LZ1, FP_LZ2, FP_E8E9, FP_BWT_FAST, FP_BWT_SLOW };

/* ------------------------------------------------------------------ */
/* Recognition                                                         */
/* ------------------------------------------------------------------ */

/* Compile the PCOMP of a method string into pz.  1 on success. */
static int fp_compile(const char *method, zq_zpaql *pz) {
  zq_err_frame ef;
  zq_zpaql hz;
  char *volatile cfg = NULL;
  int args[9] = {0};
  volatile int ok = 0;
  zq_zpaql_init(&hz);
  if (setjmp(ef.jb) == 0) {
    zq_err_push(&ef);
    cfg = zq_make_config(method, args);
    if (cfg) {
      zq_compile(cfg, args, &hz, pz, NULL);
      ok = pz->hend > pz->hbegin;
    }
    zq_err_pop(&ef);
  }
  free(cfg);
  zq_zpaql_free(&hz);
  return ok;
}

/* The program of z, as compiled. */
static const zq_u8 *fp_code(const zq_zpaql *z, int *len) {
  *len = z->hend - z->hbegin;
  return (const zq_u8 *)z->header.data + z->hbegin;
}

/* 1 if the PCOMP of method is exactly z's program. */
static int fp_same(const zq_zpaql *z, const char *method) {
  zq_zpaql pz;
  int n1, n2, eq = 0;
  const zq_u8 *c1 = fp_code(z, &n1);
  zq_zpaql_init(&pz);
  if (fp_compile(method, &pz)) {
    const zq_u8 *c2 = fp_code(&pz, &n2);
    eq = (n1 == n2 && memcmp(c1, c2, (size_t)n1) == 0);
  }
  zq_zpaql_free(&pz);
  return eq;
}

/* The byte of z's program where the PCOMPs of m0 and m1 first differ (the
 * place a parameter lives), or -1. */
static int fp_param(const zq_zpaql *z, const char *m0, const char *m1) {
  zq_zpaql p0, p1;
  int n0, n1, n, i, v = -1;
  zq_zpaql_init(&p0);
  zq_zpaql_init(&p1);
  if (fp_compile(m0, &p0) && fp_compile(m1, &p1)) {
    const zq_u8 *c0 = fp_code(&p0, &n0);
    const zq_u8 *c1 = fp_code(&p1, &n1);
    n = n0 < n1 ? n0 : n1;
    for (i = 0; i < n && c0[i] == c1[i]; ++i) {
    }
    if (i < n && i < z->hend - z->hbegin)
      v = ((const zq_u8 *)z->header.data)[z->hbegin + i];
  }
  zq_zpaql_free(&p0);
  zq_zpaql_free(&p1);
  return v;
}

static int fp_disabled(void) {
  const char *e = getenv("ZPAQII_NOFASTPATH");
  return e && *e && *e != '0';
}

static void fp_detect(zq_zpaql *z);

_Thread_local int zq_fast_detected;

void zq_fast_detect(zq_zpaql *z) {
  fp_detect(z);
  if (z->fast) ++zq_fast_detected;
  if (getenv("ZPAQII_FASTPATH_DEBUG"))
    fprintf(stderr, "fastpath: %d param %d e8 %d (len %d)\n", z->fast,
            z->fparam, z->fe8, z->hend - z->hbegin);
}

static void fp_detect(zq_zpaql *z) {
  char m[64];
  int d, v;
  z->fast = FP_NONE;
  z->fparam = 0;
  z->fe8 = 0;
  if (z->hend - z->hbegin < 8 || fp_disabled()) return;

  for (d = 0; d < 2; ++d) {
    const int t = d ? 4 : 0; /* args[1] = level + 4 with E8E9 */

    /* lazy2: rb = args[0] - 4 when args[0] > 4, else no rb section */
    snprintf(m, sizeof(m), "x4,%d", 1 + t);
    if (fp_same(z, m)) {
      z->fast = FP_LZ1, z->fparam = 0, z->fe8 = d;
      return;
    }
    {
      char m0[32], m1[32];
      snprintf(m0, sizeof(m0), "x5,%d", 1 + t);
      snprintf(m1, sizeof(m1), "x6,%d", 1 + t);
      v = fp_param(z, m0, m1); /* the operand of "a> rb-1" */
      if (v >= 0 && v + 1 <= 8) {
        snprintf(m, sizeof(m), "x%d,%d", v + 1 + 4, 1 + t);
        if (fp_same(z, m)) {
          z->fast = FP_LZ1, z->fparam = v + 1, z->fe8 = d;
          return;
        }
      }
    }

    /* lzpre: "a+= $3", $3 = args[2] */
    {
      char m0[32], m1[32];
      snprintf(m0, sizeof(m0), "x4,%d,0", 2 + t);
      snprintf(m1, sizeof(m1), "x4,%d,1", 2 + t);
      v = fp_param(z, m0, m1);
      if (v >= 0) {
        snprintf(m, sizeof(m), "x4,%d,%d", 2 + t, v);
        if (fp_same(z, m)) {
          z->fast = FP_LZ2, z->fparam = v, z->fe8 = d;
          return;
        }
      }
    }

    /* bwtrle: args[0] <= 4 selects the faster traversal */
    snprintf(m, sizeof(m), "x4,%d", 3 + t);
    if (fp_same(z, m)) {
      z->fast = FP_BWT_FAST, z->fe8 = d;
      return;
    }
    snprintf(m, sizeof(m), "x5,%d", 3 + t);
    if (fp_same(z, m)) {
      z->fast = FP_BWT_SLOW, z->fe8 = d;
      return;
    }
  }

  if (fp_same(z, "x4,4")) z->fast = FP_E8E9, z->fe8 = 1;
}

/* ------------------------------------------------------------------ */
/* Machine access                                                      */
/* ------------------------------------------------------------------ */

typedef struct {
  zq_zpaql *z;
  zq_u8 *m;
  zq_u32 mmask;
  zq_u32 *h;
  zq_u32 hmask;
  zq_u32 *r;
  zq_u8 *ob;    /* z->outbuf, with z->bufptr cached in bp */
  int bp, on;
} fp_vm;

#define M(i) (vm->m[(i) & vm->mmask])
#define H(i) (vm->h[(i) & vm->hmask])
#define R(i) (vm->r[(i)])

static void fp_open(fp_vm *vm, zq_zpaql *z) {
  vm->z = z;
  vm->m = (zq_u8 *)z->m.data;
  vm->mmask = (zq_u32)z->m.n - 1;
  vm->h = (zq_u32 *)z->h.data;
  vm->hmask = (zq_u32)z->h.n - 1;
  vm->r = (zq_u32 *)z->r.data;
  vm->ob = (zq_u8 *)z->outbuf.data;
  vm->bp = z->bufptr;
  vm->on = (int)z->outbuf.n;
}

static void fp_flush(fp_vm *vm) {
  vm->z->bufptr = vm->bp;
  zq_zpaql_flush(vm->z);
  vm->bp = vm->z->bufptr;
}

/* out: same as the VM's (zq_zpaql_outc with a >= 0) */
static inline void fp_out(fp_vm *vm, zq_u32 a) {
  vm->ob[vm->bp] = (zq_u8)a;
  if (++vm->bp == vm->on) fp_flush(vm);
}

/* n times: a=*c *b=a c++ b++ [out].  Byte for byte what the loop does,
 * including an overlapping source (c < b within the run, an LZ77 repeat),
 * done in runs that wrap neither index nor fill the output buffer. */
static void fp_copy(fp_vm *vm, zq_u32 *pb, zq_u32 *pc, zq_u64 n, int out) {
  zq_u32 b = *pb, c = *pc;
  const zq_u64 size = (zq_u64)vm->mmask + 1;
  zq_u8 *m = vm->m;
  while (n > 0) {
    const zq_u32 bi = b & vm->mmask, ci = c & vm->mmask;
    zq_u64 k = n;
    if (k > size - bi) k = size - bi;
    if (k > size - ci) k = size - ci;
    if (out && k > (zq_u64)(vm->on - vm->bp)) k = (zq_u64)(vm->on - vm->bp);
    if (ci < bi && (zq_u64)(bi - ci) < k) {
      zq_u64 i; /* overlap: the run repeats itself, byte order matters */
      for (i = 0; i < k; ++i) m[bi + i] = m[ci + i];
    } else {
      memmove(m + bi, m + ci, (size_t)k); /* same as forward bytes here */
    }
    if (out) {
      memcpy(vm->ob + vm->bp, m + bi, (size_t)k);
      vm->bp += (int)k;
      if (vm->bp == vm->on) fp_flush(vm);
    }
    b += (zq_u32)k;
    c += (zq_u32)k;
    n -= k;
  }
  *pb = b;
  *pc = c;
}

/* The E8E9 decoding pass shared by lazy2, lzpre and the fast bwtrle:
 *
 *   do (for b=0..d-1, d = end of buf)
 *     a=b a==d ifnot
 *       a+= 4 a<d if
 *         a=*b a&= 254 a== 232 if (e8 or e9?)
 *           c=b b++ b++ b++ b++ a=*b a++ a&= 254 a== 0 if (00 or ff)
 *             b-- a=*b
 *             b-- a<<= 8 a+=*b
 *             b-- a<<= 8 a+=*b
 *             a-=b a++
 *             *b=a a>>= 8 b++
 *             *b=a a>>= 8 b++
 *             *b=a b++
 *           endif
 *           b=c
 *         endif
 *       endif
 *       a=*b out b++
 *     forever
 *   endif
 */
static void fp_e8e9_pass(fp_vm *vm, zq_u32 *pa, zq_u32 *pb, zq_u32 *pc,
                         zq_u32 d) {
  zq_u32 a = *pa, b = *pb, c = *pc;
  for (;;) {
    a = b;
    if (a == d) break;
    a += 4;
    if (a < d) {
      a = M(b);
      a &= 254;
      if (a == 232) {
        c = b;
        b += 4;
        a = M(b);
        a++;
        a &= 254;
        if (a == 0) {
          b--;
          a = M(b);
          b--;
          a <<= 8;
          a += M(b);
          b--;
          a <<= 8;
          a += M(b);
          a -= b;
          a++;
          M(b) = (zq_u8)a;
          a >>= 8;
          b++;
          M(b) = (zq_u8)a;
          a >>= 8;
          b++;
          M(b) = (zq_u8)a;
          b++;
        }
        b = c;
      }
    }
    a = M(b);
    fp_out(vm, a);
    b++;
  }
  *pa = a;
  *pb = b;
  *pc = c;
}

/* ------------------------------------------------------------------ */
/* lazy2 (level 1)                                                     */
/* ------------------------------------------------------------------ */

/* r1 = state, r2 = len, r3 = m (offset bits), r4 = ptr, r5 = r (low bits of
 * the offset), c = bit buffer, d = number of bits in c. */
static void fp_lz1(fp_vm *vm, zq_u32 a, int rb, int e8) {
  zq_zpaql *z = vm->z;
  zq_u32 b = z->b, c = z->c, d = z->d;

  /* a> 255 if ... (reset state) halt endif */
  if (a > 255) {
    if (e8) {
      b = 0;
      d = R(4);
      fp_e8e9_pass(vm, &a, &b, &c, d);
    }
    a = 0, b = 0, c = 0, d = 0;
    R(1) = a, R(2) = a, R(3) = a, R(4) = a;
    goto halt;
  }

  /* a<<=d a+=c c=a (bits+=a<<n); a= 8 a+=d d=a (n+=8) */
  a <<= (d & 31);
  a += c;
  c = a;
  a = 8;
  a += d;
  d = a;

  /* a=r 1 a== 0 if (match code mm,mmm) */
  a = R(1);
  if (a == 0) {
    a = 1;
    R(2) = a;
    a = c;
    a &= 3;
    if (a > 0) {
      a--;
      a <<= 3;
      R(3) = a;
      a = c;
      a >>= 2;
      c = a;
      b = R(3);
      a &= 7;
      a += b;
      R(3) = a;
      a = c;
      a >>= 3;
      c = a;
      a = d;
      a -= 5;
      d = a;
      a = 1;
      R(1) = a;
    } else {
      a = c;
      a >>= 2;
      c = a;
      d--;
      d--;
      a = 3;
      R(1) = a;
    }
  }

  /* do a=r 1 a== 1 if a=d a> 2 if ... forever endif endif */
  for (;;) {
    a = R(1);
    if (!(a == 1)) break;
    a = d;
    if (!(a > 2)) break;
    a = c;
    a &= 1;
    if (a == 1) {
      a = c;
      a >>= 1;
      c = a;
      b = R(2);
      a = c;
      a &= 1;
      a += b;
      a += b;
      R(2) = a;
      a = c;
      a >>= 1;
      c = a;
      d--;
      d--;
    } else {
      a = c;
      a >>= 1;
      c = a;
      a = R(2);
      a <<= 2;
      b = a;
      a = c;
      a &= 3;
      a += b;
      R(2) = a;
      a = c;
      a >>= 2;
      c = a;
      d -= 3;
      a = rb ? 5 : 2;
      R(1) = a;
    }
  }

  /* (if state==5 && n>=8) a=r 1 a== 5 if a=d a> rb-1 if ... endif endif */
  if (rb) {
    a = R(1);
    if (a == 5) {
      a = d;
      if (a > (zq_u32)(rb - 1)) {
        a = c;
        a &= (zq_u32)((1 << rb) - 1);
        R(5) = a;
        a = c;
        a >>= rb;
        c = a;
        a = d;
        a -= (zq_u32)rb;
        d = a;
        a = 2;
        R(1) = a;
      }
    }
  }

  /* (if state==2 && n>=m) a=r 1 a== 2 if a=r 3 a>d ifnot */
  a = R(1);
  if (a == 2) {
    a = R(3);
    if (!(a > d)) {
      a = c;
      R(6) = a;
      a = d;
      R(7) = a;
      b = R(3);
      a = 1;
      a <<= (b & 31);
      d = a;
      a--;
      a &= c;
      a += d;
      if (rb) {
        a <<= rb;
        d = R(5);
        a += d;
        a -= (zq_u32)((1 << rb) - 1);
      }
      d = a;
      b = R(4);
      a = b;
      a -= d;
      c = a;

      /* d=r 2 do a=d a> 0 if d-- a=*c *b=a c++ b++ [out] forever endif
       * (ends with d = 0 and a = d) */
      d = R(2);
      fp_copy(vm, &b, &c, d, !e8);
      d = 0;
      a = d;
      a = b;
      R(4) = a;

      a = R(6);
      b = R(3);
      a >>= (b & 31);
      c = a;
      a = R(7);
      a -= b;
      d = a;
      a = 0;
      R(1) = a;
    }
  }

  /* do a=r 1 a== 3 if a=d a> 1 if ... forever endif endif */
  for (;;) {
    a = R(1);
    if (!(a == 3)) break;
    a = d;
    if (!(a > 1)) break;
    a = c;
    a &= 1;
    if (a == 1) {
      a = c;
      a >>= 1;
      c = a;
      b = R(2);
      a &= 1;
      a += b;
      a += b;
      R(2) = a;
      a = c;
      a >>= 1;
      c = a;
      d--;
      d--;
    } else {
      a = c;
      a >>= 1;
      c = a;
      d--;
      a = 4;
      R(1) = a;
    }
  }

  /* (if state==4 && n>=8) a=r 1 a== 4 if a=d a> 7 if */
  a = R(1);
  if (a == 4) {
    a = d;
    if (a > 7) {
      b = R(4);
      a = c;
      M(b) = (zq_u8)a;
      if (!e8) fp_out(vm, a);
      b++;
      a = b;
      R(4) = a;
      a = c;
      a >>= 8;
      c = a;
      a = d;
      a -= 8;
      d = a;
      a = R(2);
      a--;
      R(2) = a;
      if (a == 0) {
        a = 0;
        R(1) = a;
      }
    }
  }

halt:
  z->a = a, z->b = b, z->c = c, z->d = d;
}

/* ------------------------------------------------------------------ */
/* lzpre (level 2)                                                     */
/* ------------------------------------------------------------------ */

/* d = state, M = output buffer, b = size, r1 = length, r2 = offset */
static void fp_lz2(fp_vm *vm, zq_u32 a, zq_u32 minmatch, int e8) {
  zq_zpaql *z = vm->z;
  zq_u32 b = z->b, c = z->c, d = z->d;

  /* a> 255 if [e8e9: d=b b=0 do ...] b=0 c=0 d=0 a=0 r=a 1 r=a 2 halt endif */
  if (a > 255) {
    if (e8) {
      d = b;
      b = 0;
      fp_e8e9_pass(vm, &a, &b, &c, d);
    }
    b = 0, c = 0, d = 0, a = 0;
    R(1) = a, R(2) = a;
    goto halt;
  }

  /* c=a a=d a== 0 if */
  c = a;
  a = d;
  if (a == 0) {
    /* a=c a>>= 6 a++ d=a a== 1 if (literal?) */
    a = c;
    a >>= 6;
    a++;
    d = a;
    if (a == 1) {
      a += c;
      R(1) = a;
      a = 0;
      R(2) = a;
    } else { /* 3 to 5 byte match */
      d++;
      a = c;
      a &= 63;
      a += minmatch;
      R(1) = a;
      a = 0;
      R(2) = a;
    }
  } else {
    if (a == 1) { /* writing literal */
      a = c;
      M(b) = (zq_u8)a;
      b++;
      if (!e8) fp_out(vm, a);
      /* a=r 1 a-- a== 0 if d=0 endif r=a 1 */
      a = R(1);
      a--;
      if (a == 0) d = 0;
      R(1) = a;
    } else {
      if (a > 2) { /* reading offset */
        a = R(2);
        a <<= 8;
        a |= c;
        R(2) = a;
        d--;
      } else { /* state==2, write match */
        a = R(2);
        a <<= 8;
        a |= c;
        c = a;
        a = b;
        a -= c;
        a--;
        c = a;
        d = R(1);
        /* do a=*c *b=a c++ b++ [out] d-- a=d a> 0 while
         * (runs at least once: d = 0 means 2^32 bytes; ends with a = d = 0) */
        fp_copy(vm, &b, &c, d ? (zq_u64)d : ((zq_u64)1 << 32), !e8);
        d = 0;
        a = d;
      }
    }
  }

halt:
  z->a = a, z->b = b, z->c = c, z->d = d;
}

/* ------------------------------------------------------------------ */
/* e8e9 (level 0)                                                      */
/* ------------------------------------------------------------------ */

static void fp_e8e9(fp_vm *vm, zq_u32 a) {
  zq_zpaql *z = vm->z;
  zq_u32 b = z->b, c = z->c, d = z->d;

  if (a > 255) {
    /* a=c a> 4 if c= 4 else a! a+= 5 a<<= 3 d=a a=b a>>=d b=a endif */
    a = c;
    if (a > 4) {
      c = 4;
    } else {
      a = ~a;
      a += 5;
      a <<= 3;
      d = a;
      a = b;
      a >>= (d & 31);
      b = a;
    }
    /* do a=c a> 0 if a=b out a>>= 8 b=a c-- forever endif */
    for (;;) {
      a = c;
      if (!(a > 0)) break;
      a = b;
      fp_out(vm, a);
      a >>= 8;
      b = a;
      c--;
    }
  } else {
    /* *b=b a<<= 24 d=a a=b a>>= 8 a+=d b=a c++ */
    M(b) = (zq_u8)b;
    a <<= 24;
    d = a;
    a = b;
    a >>= 8;
    a += d;
    b = a;
    c++;
    /* a=c a> 4 if a=*b out a&= 254 a== 232 if ... endif endif */
    a = c;
    if (a > 4) {
      a = M(b);
      fp_out(vm, a);
      a &= 254;
      if (a == 232) {
        a = b;
        a >>= 24;
        a++;
        a &= 254;
        if (a == 0) {
          a = b;
          a >>= 24;
          a <<= 24;
          d = a;
          a = b;
          a -= c;
          a += 5;
          a <<= 8;
          a >>= 8;
          a |= d;
          b = a;
        }
      }
    }
  }
  z->a = a, z->b = b, z->c = c, z->d = d;
}

/* ------------------------------------------------------------------ */
/* Dispatch                                                            */
/* ------------------------------------------------------------------ */

static void fp_one(fp_vm *vm, zq_u32 a) {
  zq_zpaql *z = vm->z;
  switch (z->fast) {
    case FP_LZ1: fp_lz1(vm, a, z->fparam, z->fe8); break;
    case FP_LZ2: fp_lz2(vm, a, (zq_u32)z->fparam, z->fe8); break;
    case FP_E8E9: fp_e8e9(vm, a); break;
  }
}

int zq_fast_run(zq_zpaql *z, const zq_u8 *buf, int n, zq_u32 input) {
  fp_vm vm;
  if (z->fast != FP_LZ1 && z->fast != FP_LZ2 && z->fast != FP_E8E9) return 0;
  fp_open(&vm, z);
  if (!buf) {
    fp_one(&vm, input);
  } else {
    int i;
    for (i = 0; i < n; ++i) fp_one(&vm, buf[i]);
  }
  z->bufptr = vm.bp;
  return 1;
}
