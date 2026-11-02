/* zpaqII - ZPAQL VM: lifecycle, headers, interpreted execution.
 * Port of libzpaq 7.15 (Unlicense, Matt Mahoney). */
#include "zq_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

void zq_zpaql_clear(zq_zpaql *z) {
  z->cend = z->hbegin = z->hend = 0;
  z->a = z->b = z->c = z->d = 0;
  z->f = z->pc = 0;
  zq_arr_resize(&z->header, 0, 1);
  zq_arr_resize(&z->h, 0, 4);
  zq_arr_resize(&z->m, 0, 1);
  zq_arr_resize(&z->r, 0, 4);
  if (z->rcode) {
    free(z->rcode);
    z->rcode = NULL;
    z->rcode_size = 0;
  }
}

void zq_zpaql_init(zq_zpaql *z) {
  memset(z, 0, sizeof(*z));
  zq_zpaql_clear(z);
  zq_arr_resize(&z->outbuf, 1 << 14, 1);
  z->bufptr = 0;
}

void zq_zpaql_free(zq_zpaql *z) {
  zq_arr_free(&z->header);
  zq_arr_free(&z->h);
  zq_arr_free(&z->m);
  zq_arr_free(&z->r);
  zq_arr_free(&z->outbuf);
  if (z->rcode) free(z->rcode);
  z->rcode = NULL;
  z->rcode_size = 0;
}

/* ------------------------------------------------------------------ */
/* Header                                                              */
/* ------------------------------------------------------------------ */

int zq_zpaql_write(zq_zpaql *z, zq_writer *out2, int pp) {
  int i;
  if ((int)z->header.n <= 6) return 0;
  if (!pp) {
    for (i = 0; i < z->cend; ++i) out2->put(out2->self, ZQ_HDR(z, i));
  } else {
    out2->put(out2->self, (z->hend - z->hbegin) & 255);
    out2->put(out2->self, (z->hend - z->hbegin) >> 8);
  }
  for (i = z->hbegin; i < z->hend; ++i) out2->put(out2->self, ZQ_HDR(z, i));
  return 1;
}

int zq_zpaql_read(zq_zpaql *z, zq_reader *in2) {
  int hsize, n, i, j;
  hsize = in2->get(in2->self);
  hsize += in2->get(in2->self) * 256;
  zq_arr_resize(&z->header, (size_t)hsize + 300, 1);
  z->cend = z->hbegin = z->hend = 0;
  ZQ_HDR(z, z->cend++) = (zq_u8)(hsize & 255);
  ZQ_HDR(z, z->cend++) = (zq_u8)(hsize >> 8);
  while (z->cend < 7) ZQ_HDR(z, z->cend++) = (zq_u8)in2->get(in2->self);

  n = ZQ_HDR(z, z->cend - 1);
  for (i = 0; i < n; ++i) {
    int type = in2->get(in2->self);
    int size;
    if (type < 0 || type > 255) zq_error("unexpected end of file");
    ZQ_HDR(z, z->cend++) = (zq_u8)type;
    size = zq_compsize[type];
    if (size < 1) zq_error("Invalid component type");
    if (z->cend + size > hsize) zq_error("COMP overflows header");
    for (j = 1; j < size; ++j)
      ZQ_HDR(z, z->cend++) = (zq_u8)in2->get(in2->self);
  }
  ZQ_HDR(z, z->cend) = (zq_u8)in2->get(in2->self);
  if (ZQ_HDR(z, z->cend++) != 0) zq_error("missing COMP END");

  z->hbegin = z->hend = z->cend + 128;
  if (z->hend > hsize + 129) zq_error("missing HCOMP");
  while (z->hend < hsize + 129) {
    int op = in2->get(in2->self);
    if (op == -1) zq_error("unexpected end of file");
    ZQ_HDR(z, z->hend++) = (zq_u8)op;
  }
  ZQ_HDR(z, z->hend) = (zq_u8)in2->get(in2->self);
  if (ZQ_HDR(z, z->hend++) != 0) zq_error("missing HCOMP END");
  return z->cend + z->hend - z->hbegin;
}

/* ------------------------------------------------------------------ */
/* Execution                                                           */
/* ------------------------------------------------------------------ */

static void zq_zpaql_err(void) { zq_error("ZPAQL execution error"); }

void zq_zpaql_inith(zq_zpaql *z) {
  zq_zpaql_init_sizes(z, ZQ_HDR(z, 2), ZQ_HDR(z, 3));
}

void zq_zpaql_initp(zq_zpaql *z) {
  zq_zpaql_init_sizes(z, ZQ_HDR(z, 4), ZQ_HDR(z, 5));
}

void zq_zpaql_init_sizes(zq_zpaql *z, int hbits, int mbits) {
  if (hbits > 32) zq_error("H too big");
  if (mbits > 32) zq_error("M too big");
  zq_arr_resize(&z->h, (size_t)1 << hbits, 4);
  zq_arr_resize(&z->m, (size_t)1 << mbits, 1);
  zq_arr_resize(&z->r, 256, 4);
  z->a = z->b = z->c = z->d = 0;
  z->pc = z->f = 0;
}

static double pow2i(int x) {
  double r = 1;
  for (; x > 0; --x) r += r;
  return r;
}

double zq_zpaql_memory(zq_zpaql *z) {
  double mem = pow2i(ZQ_HDR(z, 2) + 2) + pow2i(ZQ_HDR(z, 3)) +
               pow2i(ZQ_HDR(z, 4) + 2) + pow2i(ZQ_HDR(z, 5)) +
               (double)z->header.n;
  int cp = 7;
  int i;
  for (i = 0; i < ZQ_HDR(z, 6); ++i) {
    const double size = pow2i(ZQ_HDR(z, cp + 1));
    switch (ZQ_HDR(z, cp)) {
      case 2 /*CM*/: mem += 4 * size; break;
      case 3 /*ICM*/: mem += 64 * size + 1024; break;
      case 4 /*MATCH*/: mem += 4 * size + pow2i(ZQ_HDR(z, cp + 2)); break;
      case 6 /*MIX2*/: mem += 2 * size; break;
      case 7 /*MIX*/: mem += 4 * size * ZQ_HDR(z, cp + 3); break;
      case 8 /*ISSE*/: mem += 64 * size + 2048; break;
      case 9 /*SSE*/: mem += 128 * size; break;
      default: break;
    }
    cp += zq_compsize[ZQ_HDR(z, cp)];
  }
  return mem;
}

void zq_zpaql_flush(zq_zpaql *z) {
  if (z->output && z->bufptr)
    z->output->write(z->output->self, (const char *)z->outbuf.data, z->bufptr);
  if (z->sha1 && z->bufptr)
    zq_sha1_write(z->sha1, (const char *)z->outbuf.data, z->bufptr);
  z->bufptr = 0;
}

void zq_zpaql_outc(zq_zpaql *z, int ch) {
  if (ch < 0 || ((ZQ_U8A(&z->outbuf, z->bufptr) = (zq_u8)ch),
                 ++z->bufptr == (int)z->outbuf.n))
    zq_zpaql_flush(z);
}

void zq_zpaql_run(zq_zpaql *z, zq_u32 input) {
  z->pc = z->hbegin;
  z->a = input;
  while (zq_zpaql_execute(z)) {
  }
}

static void zq_div(zq_u32 *a, zq_u32 x) { *a = x ? *a / x : 0; }
static void zq_mod(zq_u32 *a, zq_u32 x) { *a = x ? *a % x : 0; }

int zq_zpaql_execute(zq_zpaql *z) {
  const int pc0 = z->pc;
  const int op = ZQ_HDR(z, pc0);
  z->pc = pc0 + 1;
  switch (op) {
    case 0: zq_zpaql_err(); break;
    case 1: ++z->a; break;
    case 2: --z->a; break;
    case 3: z->a = ~z->a; break;
    case 4: z->a = 0; break;
    case 7: z->a = ZQ_R(z, ZQ_HDR(z, z->pc++)); break;
    case 8: { zq_u32 t = z->b; z->b = z->a; z->a = t; } break;
    case 9: ++z->b; break;
    case 10: --z->b; break;
    case 11: z->b = ~z->b; break;
    case 12: z->b = 0; break;
    case 15: z->b = ZQ_R(z, ZQ_HDR(z, z->pc++)); break;
    case 16: { zq_u32 t = z->c; z->c = z->a; z->a = t; } break;
    case 17: ++z->c; break;
    case 18: --z->c; break;
    case 19: z->c = ~z->c; break;
    case 20: z->c = 0; break;
    case 23: z->c = ZQ_R(z, ZQ_HDR(z, z->pc++)); break;
    case 24: { zq_u32 t = z->d; z->d = z->a; z->a = t; } break;
    case 25: ++z->d; break;
    case 26: --z->d; break;
    case 27: z->d = ~z->d; break;
    case 28: z->d = 0; break;
    case 31: z->d = ZQ_R(z, ZQ_HDR(z, z->pc++)); break;
    case 32: { zq_u8 t = ZQ_M(z, z->b); ZQ_M(z, z->b) = (zq_u8)z->a;
               z->a = t; } break;
    case 33: ZQ_M(z, z->b) = (zq_u8)(ZQ_M(z, z->b) + 1); break;
    case 34: ZQ_M(z, z->b) = (zq_u8)(ZQ_M(z, z->b) - 1); break;
    case 35: ZQ_M(z, z->b) = (zq_u8)~ZQ_M(z, z->b); break;
    case 36: ZQ_M(z, z->b) = 0; break;
    case 39:
      if (z->f) z->pc += ((ZQ_HDR(z, z->pc) + 128) & 255) - 127;
      else ++z->pc;
      break;
    case 40: { zq_u8 t = ZQ_M(z, z->c); ZQ_M(z, z->c) = (zq_u8)z->a;
               z->a = t; } break;
    case 41: ZQ_M(z, z->c) = (zq_u8)(ZQ_M(z, z->c) + 1); break;
    case 42: ZQ_M(z, z->c) = (zq_u8)(ZQ_M(z, z->c) - 1); break;
    case 43: ZQ_M(z, z->c) = (zq_u8)~ZQ_M(z, z->c); break;
    case 44: ZQ_M(z, z->c) = 0; break;
    case 47:
      if (!z->f) z->pc += ((ZQ_HDR(z, z->pc) + 128) & 255) - 127;
      else ++z->pc;
      break;
    case 48: { zq_u32 t = ZQ_H(z, z->d); ZQ_H(z, z->d) = z->a;
               z->a = t; } break;
    case 49: ++ZQ_H(z, z->d); break;
    case 50: --ZQ_H(z, z->d); break;
    case 51: ZQ_H(z, z->d) = ~ZQ_H(z, z->d); break;
    case 52: ZQ_H(z, z->d) = 0; break;
    case 55: ZQ_R(z, ZQ_HDR(z, z->pc++)) = z->a; break;
    case 56: return 0;
    case 57: zq_zpaql_outc(z, (int)(z->a & 255)); break;
    case 59: z->a = (z->a + ZQ_M(z, z->b) + 512) * 773; break;
    case 60: ZQ_H(z, z->d) = (ZQ_H(z, z->d) + z->a + 512) * 773; break;
    case 63: z->pc += ((ZQ_HDR(z, z->pc) + 128) & 255) - 127; break;
    case 64: break;
    case 65: z->a = z->b; break;
    case 66: z->a = z->c; break;
    case 67: z->a = z->d; break;
    case 68: z->a = ZQ_M(z, z->b); break;
    case 69: z->a = ZQ_M(z, z->c); break;
    case 70: z->a = ZQ_H(z, z->d); break;
    case 71: z->a = ZQ_HDR(z, z->pc++); break;
    case 72: z->b = z->a; break;
    case 73: break;
    case 74: z->b = z->c; break;
    case 75: z->b = z->d; break;
    case 76: z->b = ZQ_M(z, z->b); break;
    case 77: z->b = ZQ_M(z, z->c); break;
    case 78: z->b = ZQ_H(z, z->d); break;
    case 79: z->b = ZQ_HDR(z, z->pc++); break;
    case 80: z->c = z->a; break;
    case 81: z->c = z->b; break;
    case 82: break;
    case 83: z->c = z->d; break;
    case 84: z->c = ZQ_M(z, z->b); break;
    case 85: z->c = ZQ_M(z, z->c); break;
    case 86: z->c = ZQ_H(z, z->d); break;
    case 87: z->c = ZQ_HDR(z, z->pc++); break;
    case 88: z->d = z->a; break;
    case 89: z->d = z->b; break;
    case 90: z->d = z->c; break;
    case 91: break;
    case 92: z->d = ZQ_M(z, z->b); break;
    case 93: z->d = ZQ_M(z, z->c); break;
    case 94: z->d = ZQ_H(z, z->d); break;
    case 95: z->d = ZQ_HDR(z, z->pc++); break;
    case 96: ZQ_M(z, z->b) = (zq_u8)z->a; break;
    case 97: ZQ_M(z, z->b) = (zq_u8)z->b; break;
    case 98: ZQ_M(z, z->b) = (zq_u8)z->c; break;
    case 99: ZQ_M(z, z->b) = (zq_u8)z->d; break;
    case 100: break;
    case 101: ZQ_M(z, z->b) = ZQ_M(z, z->c); break;
    case 102: ZQ_M(z, z->b) = (zq_u8)ZQ_H(z, z->d); break;
    case 103: ZQ_M(z, z->b) = ZQ_HDR(z, z->pc++); break;
    case 104: ZQ_M(z, z->c) = (zq_u8)z->a; break;
    case 105: ZQ_M(z, z->c) = (zq_u8)z->b; break;
    case 106: ZQ_M(z, z->c) = (zq_u8)z->c; break;
    case 107: ZQ_M(z, z->c) = (zq_u8)z->d; break;
    case 108: ZQ_M(z, z->c) = ZQ_M(z, z->b); break;
    case 109: break;
    case 110: ZQ_M(z, z->c) = (zq_u8)ZQ_H(z, z->d); break;
    case 111: ZQ_M(z, z->c) = ZQ_HDR(z, z->pc++); break;
    case 112: ZQ_H(z, z->d) = z->a; break;
    case 113: ZQ_H(z, z->d) = z->b; break;
    case 114: ZQ_H(z, z->d) = z->c; break;
    case 115: ZQ_H(z, z->d) = z->d; break;
    case 116: ZQ_H(z, z->d) = ZQ_M(z, z->b); break;
    case 117: ZQ_H(z, z->d) = ZQ_M(z, z->c); break;
    case 118: break;
    case 119: ZQ_H(z, z->d) = ZQ_HDR(z, z->pc++); break;
    case 128: z->a += z->a; break;
    case 129: z->a += z->b; break;
    case 130: z->a += z->c; break;
    case 131: z->a += z->d; break;
    case 132: z->a += ZQ_M(z, z->b); break;
    case 133: z->a += ZQ_M(z, z->c); break;
    case 134: z->a += ZQ_H(z, z->d); break;
    case 135: z->a += ZQ_HDR(z, z->pc++); break;
    case 136: z->a -= z->a; break;
    case 137: z->a -= z->b; break;
    case 138: z->a -= z->c; break;
    case 139: z->a -= z->d; break;
    case 140: z->a -= ZQ_M(z, z->b); break;
    case 141: z->a -= ZQ_M(z, z->c); break;
    case 142: z->a -= ZQ_H(z, z->d); break;
    case 143: z->a -= ZQ_HDR(z, z->pc++); break;
    case 144: z->a *= z->a; break;
    case 145: z->a *= z->b; break;
    case 146: z->a *= z->c; break;
    case 147: z->a *= z->d; break;
    case 148: z->a *= ZQ_M(z, z->b); break;
    case 149: z->a *= ZQ_M(z, z->c); break;
    case 150: z->a *= ZQ_H(z, z->d); break;
    case 151: z->a *= ZQ_HDR(z, z->pc++); break;
    case 152: zq_div(&z->a, z->a); break;
    case 153: zq_div(&z->a, z->b); break;
    case 154: zq_div(&z->a, z->c); break;
    case 155: zq_div(&z->a, z->d); break;
    case 156: zq_div(&z->a, ZQ_M(z, z->b)); break;
    case 157: zq_div(&z->a, ZQ_M(z, z->c)); break;
    case 158: zq_div(&z->a, ZQ_H(z, z->d)); break;
    case 159: zq_div(&z->a, ZQ_HDR(z, z->pc++)); break;
    case 160: zq_mod(&z->a, z->a); break;
    case 161: zq_mod(&z->a, z->b); break;
    case 162: zq_mod(&z->a, z->c); break;
    case 163: zq_mod(&z->a, z->d); break;
    case 164: zq_mod(&z->a, ZQ_M(z, z->b)); break;
    case 165: zq_mod(&z->a, ZQ_M(z, z->c)); break;
    case 166: zq_mod(&z->a, ZQ_H(z, z->d)); break;
    case 167: zq_mod(&z->a, ZQ_HDR(z, z->pc++)); break;
    case 168: z->a &= z->a; break;
    case 169: z->a &= z->b; break;
    case 170: z->a &= z->c; break;
    case 171: z->a &= z->d; break;
    case 172: z->a &= ZQ_M(z, z->b); break;
    case 173: z->a &= ZQ_M(z, z->c); break;
    case 174: z->a &= ZQ_H(z, z->d); break;
    case 175: z->a &= ZQ_HDR(z, z->pc++); break;
    case 176: z->a &= ~z->a; break;
    case 177: z->a &= ~z->b; break;
    case 178: z->a &= ~z->c; break;
    case 179: z->a &= ~z->d; break;
    case 180: z->a &= ~(zq_u32)ZQ_M(z, z->b); break;
    case 181: z->a &= ~(zq_u32)ZQ_M(z, z->c); break;
    case 182: z->a &= ~ZQ_H(z, z->d); break;
    case 183: z->a &= ~(zq_u32)ZQ_HDR(z, z->pc++); break;
    case 184: z->a |= z->a; break;
    case 185: z->a |= z->b; break;
    case 186: z->a |= z->c; break;
    case 187: z->a |= z->d; break;
    case 188: z->a |= ZQ_M(z, z->b); break;
    case 189: z->a |= ZQ_M(z, z->c); break;
    case 190: z->a |= ZQ_H(z, z->d); break;
    case 191: z->a |= ZQ_HDR(z, z->pc++); break;
    case 192: z->a ^= z->a; break;
    case 193: z->a ^= z->b; break;
    case 194: z->a ^= z->c; break;
    case 195: z->a ^= z->d; break;
    case 196: z->a ^= ZQ_M(z, z->b); break;
    case 197: z->a ^= ZQ_M(z, z->c); break;
    case 198: z->a ^= ZQ_H(z, z->d); break;
    case 199: z->a ^= ZQ_HDR(z, z->pc++); break;
    case 200: z->a <<= (z->a & 31); break;
    case 201: z->a <<= (z->b & 31); break;
    case 202: z->a <<= (z->c & 31); break;
    case 203: z->a <<= (z->d & 31); break;
    case 204: z->a <<= (ZQ_M(z, z->b) & 31); break;
    case 205: z->a <<= (ZQ_M(z, z->c) & 31); break;
    case 206: z->a <<= (ZQ_H(z, z->d) & 31); break;
    case 207: z->a <<= (ZQ_HDR(z, z->pc++) & 31); break;
    case 208: z->a >>= (z->a & 31); break;
    case 209: z->a >>= (z->b & 31); break;
    case 210: z->a >>= (z->c & 31); break;
    case 211: z->a >>= (z->d & 31); break;
    case 212: z->a >>= (ZQ_M(z, z->b) & 31); break;
    case 213: z->a >>= (ZQ_M(z, z->c) & 31); break;
    case 214: z->a >>= (ZQ_H(z, z->d) & 31); break;
    case 215: z->a >>= (ZQ_HDR(z, z->pc++) & 31); break;
    case 216: z->f = 1; break;
    case 217: z->f = (z->a == z->b); break;
    case 218: z->f = (z->a == z->c); break;
    case 219: z->f = (z->a == z->d); break;
    case 220: z->f = (z->a == (zq_u32)ZQ_M(z, z->b)); break;
    case 221: z->f = (z->a == (zq_u32)ZQ_M(z, z->c)); break;
    case 222: z->f = (z->a == ZQ_H(z, z->d)); break;
    case 223: z->f = (z->a == (zq_u32)ZQ_HDR(z, z->pc++)); break;
    case 224: z->f = 0; break;
    case 225: z->f = (z->a < z->b); break;
    case 226: z->f = (z->a < z->c); break;
    case 227: z->f = (z->a < z->d); break;
    case 228: z->f = (z->a < (zq_u32)ZQ_M(z, z->b)); break;
    case 229: z->f = (z->a < (zq_u32)ZQ_M(z, z->c)); break;
    case 230: z->f = (z->a < ZQ_H(z, z->d)); break;
    case 231: z->f = (z->a < (zq_u32)ZQ_HDR(z, z->pc++)); break;
    case 232: z->f = 0; break;
    case 233: z->f = (z->a > z->b); break;
    case 234: z->f = (z->a > z->c); break;
    case 235: z->f = (z->a > z->d); break;
    case 236: z->f = (z->a > (zq_u32)ZQ_M(z, z->b)); break;
    case 237: z->f = (z->a > (zq_u32)ZQ_M(z, z->c)); break;
    case 238: z->f = (z->a > ZQ_H(z, z->d)); break;
    case 239: z->f = (z->a > (zq_u32)ZQ_HDR(z, z->pc++)); break;
    case 255:
      z->pc = z->hbegin + ZQ_HDR(z, z->pc) + 256 * ZQ_HDR(z, z->pc + 1);
      if (z->pc >= z->hend) zq_zpaql_err();
      break;
    default: zq_zpaql_err();
  }
  return 1;
}
