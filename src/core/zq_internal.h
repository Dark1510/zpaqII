/* zpaqII — cabecera interna del core.  No la usa el host Pascal.
 *
 * Estructuras y funciones equivalentes a las clases privadas de libzpaq:
 * ZPAQL, StateTable, Component, Predictor, Decoder/Encoder, PostProcessor,
 * Decompresser, Compressor, Compiler.  Se exponen solo entre modulos .c.
 */
#ifndef ZQ_INTERNAL_H
#define ZQ_INTERNAL_H 1

#include "zq.h"

/* ------------------------------------------------------------------ */
/* ZPAQL                                                               */
/* ------------------------------------------------------------------ */

typedef struct zq_zpaql {
  zq_writer *output;    /* destino de OUT, o NULL */
  zq_sha1   *sha1;      /* checksum de la salida, o NULL */
  zq_arr header;        /* U8: hsize[2] hh hm ph pm n COMP (guard) HCOMP (guard) */
  int cend;             /* COMP en header[7..cend-1] */
  int hbegin, hend;     /* HCOMP/PCOMP en header[hbegin..hend-1] */
  zq_arr m;             /* U8: memoria M */
  zq_arr h;             /* U32: hash H */
  zq_arr r;             /* U32: 256 registros */
  zq_arr outbuf;        /* U8: buffer de salida */
  int bufptr;
  zq_u32 a, b, c, d;
  int f, pc;
  zq_u8 *rcode;         /* codigo JIT (NULL si interprete) */
  int rcode_size;
} zq_zpaql;

/* Equivalencias exactas con Array::operator() (indice enmascarado) y
 * Array::operator[] (indice directo). */
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
int zq_zpaql_execute(zq_zpaql *z);   /* 1 instruccion; 0 tras HALT */

/* ------------------------------------------------------------------ */
/* Componentes y compresor                                             */
/* ------------------------------------------------------------------ */

/* Tamano en bytes de cada componente segun su tipo (libzpaq::compsize). */
extern const int zq_compsize[256];

/* Nombres de componentes (compname) y opcodes (opcodelist) del Compiler. */
extern const char *const zq_compname[256];
extern const char *const zq_opcodelist[272];

/* Compilador ZPAQL (texto -> bytecode).  Equivale a libzpaq::Compiler. */
void zq_compile(const char *in, int *args, zq_zpaql *hz, zq_zpaql *pz,
                zq_writer *out2);

#endif
