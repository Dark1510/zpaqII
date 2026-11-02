/* zpaqII - core internal header.  Not used by the Pascal host.
 *
 * Structures and functions equivalent to libzpaq's private classes:
 * ZPAQL, StateTable, Component, Predictor, Decoder/Encoder, PostProcessor,
 * Decompresser, Compressor, Compiler.  Shared only between .c modules.
 */
#ifndef ZQ_INTERNAL_H
#define ZQ_INTERNAL_H 1

#include "zq.h"

/* ------------------------------------------------------------------ */
/* ZPAQL                                                               */
/* ------------------------------------------------------------------ */

typedef struct zq_zpaql {
  zq_writer *output;    /* OUT destination, or NULL */
  zq_sha1   *sha1;      /* output checksum, or NULL */
  zq_arr header;        /* U8: hsize[2] hh hm ph pm n COMP (guard) HCOMP (guard) */
  int cend;             /* COMP at header[7..cend-1] */
  int hbegin, hend;     /* HCOMP/PCOMP at header[hbegin..hend-1] */
  zq_arr m;             /* U8: memory M */
  zq_arr h;             /* U32: hash H */
  zq_arr r;             /* U32: 256 registers */
  zq_arr outbuf;        /* U8: output buffer */
  int bufptr;
  zq_u32 a, b, c, d;
  int f, pc;
  zq_u8 *rcode;         /* codigo JIT (NULL si interprete) */
  int rcode_size;
} zq_zpaql;

/* Exact equivalents of Array::operator() (masked index) and
 * Array::operator[] (direct index). */
#define ZQ_HDR(z, i)  (((zq_u8 *)(z)->header.data)[(i)])
#define ZQ_M(z, i)    (((zq_u8 *)(z)->m.data)[(i) & ((int)(z)->m.n - 1)])
#define ZQ_H(z, i)    (((zq_u32 *)(z)->h.data)[(i) & ((int)(z)->h.n - 1)])
#define ZQ_R(z, i)    (((zq_u32 *)(z)->r.data)[(i)])

void zq_zpaql_init(zq_zpaql *z);
void zq_zpaql_clear(zq_zpaql *z);
void zq_zpaql_free(zq_zpaql *z);
void zq_zpaql_inith(zq_zpaql *z);
void zq_zpaql_initp(zq_zpaql *z);
void zq_zpaql_init_sizes(zq_zpaql *z, int hbits, int mbits);
double zq_zpaql_memory(zq_zpaql *z);
void zq_zpaql_run(zq_zpaql *z, zq_u32 input);
int zq_zpaql_read(zq_zpaql *z, zq_reader *in);
int zq_zpaql_write(zq_zpaql *z, zq_writer *out, int pp);
void zq_zpaql_flush(zq_zpaql *z);
void zq_zpaql_outc(zq_zpaql *z, int ch);
int zq_zpaql_execute(zq_zpaql *z);   /* one instruction; 0 after HALT */

/* ------------------------------------------------------------------ */
/* Components and compressor                                           */
/* ------------------------------------------------------------------ */

/* Size in bytes of each component by type (libzpaq::compsize). */
extern const int zq_compsize[256];

/* Component names (compname) and opcodes (opcodelist) for the Compiler. */
extern const char *const zq_compname[256];
extern const char *const zq_opcodelist[272];

/* ZPAQL compiler (text -> bytecode).  Equivalent to libzpaq::Compiler. */
void zq_compile(const char *in, int *args, zq_zpaql *hz, zq_zpaql *pz,
                zq_writer *out2);

#endif
