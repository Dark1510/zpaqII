/* zpaqII - core internal header.  Not used by the Pascal host.
 *
 * Structures and functions equivalent to libzpaq's private classes:
 * ZPAQL, StateTable, Component, Predictor, Decoder/Encoder, PostProcessor,
 * Decompresser, Compressor, Compiler.  Shared only between .c modules.
 */
#ifndef ZPAQII_ZQ_INTERNAL_H
#define ZPAQII_ZQ_INTERNAL_H 1

#include "../../include/zq.h"

/* ------------------------------------------------------------------ */
/* Target capabilities                                                 */
/* ------------------------------------------------------------------ */

/* The JIT emits System V x86-64 machine code and allocates executable memory
 * with mmap, so it is only built for x86-64 outside Windows and macOS (whose
 * x86-64 ABI and page protection differ).  Everywhere else the core uses the
 * ZPAQL interpreter and the portable predictor, which produce identical
 * output: the JIT is a speed feature, never a correctness one.  A build can
 * still force the interpreter with -DZQ_NOJIT. */
#if !defined(ZQ_NOJIT)
#if !defined(__x86_64__) || defined(_WIN32) || defined(__APPLE__)
#define ZQ_NOJIT 1
#endif
#endif

/* ------------------------------------------------------------------ */
/* CPU features (cpu.c)                                                */
/* ------------------------------------------------------------------ */

#define ZQ_CPU_SHA1 1u /* x86 SHA extensions / ARMv8 SHA-1 */
#define ZQ_CPU_SHA2 2u /* ARMv8 SHA-256 */

unsigned zq_cpu_features(void);

/* ------------------------------------------------------------------ */
/* Component types (libzpaq::CompType) */
enum {
  ZQ_NONE = 0, ZQ_CONS = 1, ZQ_CM = 2, ZQ_ICM = 3, ZQ_MATCH = 4,
  ZQ_AVG = 5, ZQ_MIX2 = 6, ZQ_MIX = 7, ZQ_ISSE = 8, ZQ_SSE = 9
};

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
  zq_u8 *rcode;         /* JIT code (NULL when interpreting) */
  int rcode_size;
  const zq_u8 *bin;     /* zq_zpaql_run_buf: inputs still to run ... */
  int bn;               /* ... and how many (0 outside a batch) */
  int fast;             /* native fast path for this PCOMP (fastpath.c), 0 = none */
  int fparam, fe8;      /* its parameter and E8E9 flag */
} zq_zpaql;

/* Exact equivalents of Array::operator() (masked index) and
 * Array::operator[] (direct index). */
#define ZQ_HDR(z, i)   (((zq_u8 *)(z)->header.data)[(i)])
#define ZQ_U16A(a, i)  (((zq_u16 *)(a)->data)[(i)])
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
/* Same as zq_zpaql_run() on buf[0], buf[1], ... buf[n-1] in order. */
void zq_zpaql_run_buf(zq_zpaql *z, const zq_u8 *buf, int n);
/* fastpath.c: recognise a PCOMP of 7.15's own methods (sets z->fast), and run
 * it natively on buf[0..n-1], or on input when buf is NULL.  zq_fast_run
 * returns 0 when z has no native path. */
void zq_fast_detect(zq_zpaql *z);
int  zq_fast_run(zq_zpaql *z, const zq_u8 *buf, int n, zq_u32 input);
extern _Thread_local int zq_fast_detected; /* recognitions, for the tests */
int zq_zpaql_read(zq_zpaql *z, zq_reader *in);
int zq_zpaql_write(zq_zpaql *z, zq_writer *out, int pp);
void zq_zpaql_flush(zq_zpaql *z);
void zq_zpaql_outc(zq_zpaql *z, int ch);
int zq_zpaql_execute(zq_zpaql *z);
void zq_zpaql_run_interp(zq_zpaql *z, zq_u32 input);

/* JIT: executable memory and code generation (see src/core/jit.c). */
void zq_allocx(zq_u8 **p, int *n, int newsize);
void zq_allocx_seal(zq_u8 *p, int n);  /* RW -> RX (W^X) */
int  zq_zpaql_assemble(zq_zpaql *z);   /* one instruction; 0 after HALT */

/* ------------------------------------------------------------------ */
/* State table and model components                                    */
/* ------------------------------------------------------------------ */

typedef struct zq_state_table {
  zq_u8 ns[1024];   /* state*4 -> next state if 0, if 1, n0, n1 */
} zq_state_table;

void zq_st_init(zq_state_table *st);
static inline int zq_st_next(const zq_state_table *st, int state, int y) {
  return st->ns[state * 4 + y];
}
int zq_st_cminit(const zq_state_table *st, int state);

typedef struct zq_component {
  size_t limit;
  size_t cxt;
  size_t a, b, c;
  zq_arr cm;    /* U32 */
  zq_arr ht;    /* U8 */
  zq_arr a16;   /* U16 */
} zq_component;

void zq_component_init(zq_component *cr);

/* ------------------------------------------------------------------ */
/* Predictor                                                           */
/* ------------------------------------------------------------------ */

typedef struct zq_predictor {
  int c8;                 /* last 0..7 bits */
  int hmap4;              /* c8 split into nibbles */
  int p[256];             /* predictions */
  zq_u32 h[256];          /* unrolled copy of z->h */
  zq_zpaql *z;            /* VM that computes context hashes (H, n) */
  zq_component comp[256]; /* the model */
  int initTables;         /* are the lookup tables initialized? */
  int dt2k[256];          /* division table for MATCH */
  int dt[1024];           /* division table for CM */
  zq_u16 squasht[4096];   /* squash() lookup table */
  short stretcht[32768];  /* stretch() lookup table */
  zq_state_table st;
  zq_u8 *pcode;           /* JIT code for predict()/update() */
  int pcode_size;
} zq_predictor;

extern const zq_u8  zq_sns[1024];
extern const int    zq_sdt2k[256];
extern const int    zq_sdt[1024];
extern const zq_u16 zq_ssquasht[1344];
extern const zq_u8  zq_stdt[712];

void zq_pred_open(zq_predictor *pr, zq_zpaql *z);
void zq_pred_init(zq_predictor *pr);
void zq_pred_free(zq_predictor *pr);
/* Inline: it is called once per byte by the coder. */
static inline int zq_pred_is_modeled(const zq_predictor *pr) {
  return pr->z->header.n > 6 && ZQ_HDR(pr->z, 6) != 0;
}
int  zq_pred_predict_ref(zq_predictor *pr);  /* interpreter, for tests */
void zq_pred_update_ref(zq_predictor *pr, int y);
#ifndef ZQ_NOJIT
int  zq_pred_assemble(zq_predictor *pr);     /* predictor JIT, bytes needed */
int  zq_pred_jit_build(zq_predictor *pr);    /* assemble once; 1 on success */
#endif

/* c8/hmap4 bookkeeping and the HCOMP run at byte boundaries.  The JIT only
 * updates the components, so this stays in C (like upstream).  It is inline
 * because the coder calls it once per bit. */
static inline void zq_pred_update_c8(zq_predictor *pr, int y) {
  zq_zpaql *z = pr->z;
  const int n = ZQ_HDR(z, 6);
  int i;

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

/* Called once per bit by the coder: keep the JIT dispatch here so there is no
 * extra call/prologue per bit, and fall back to the interpreter only when no
 * code could be assembled. */
static inline int zq_pred_predict(zq_predictor *pr) {
#ifndef ZQ_NOJIT
  if (!pr->pcode && !zq_pred_jit_build(pr)) return zq_pred_predict_ref(pr);
  return ((int (*)(zq_predictor *))&pr->pcode[10])(pr);
#else
  return zq_pred_predict_ref(pr);
#endif
}

static inline void zq_pred_update(zq_predictor *pr, int y) {
#ifndef ZQ_NOJIT
  if (!pr->pcode && !zq_pred_jit_build(pr)) {
    zq_pred_update_ref(pr, y);
    return;
  }
  ((void (*)(zq_predictor *, int))&pr->pcode[5])(pr, y);
  zq_pred_update_c8(pr, y);
#else
  zq_pred_update_ref(pr, y);
#endif
}
size_t zq_pred_find(zq_predictor *pr, zq_arr *ht, int sizebits, zq_u32 cxt);

/* ------------------------------------------------------------------ */
/* Coder                                                               */
/* ------------------------------------------------------------------ */

typedef struct zq_decoder {
  zq_reader *in;
  zq_u32 low, high, curr;
  zq_u32 rpos, wpos;
  zq_predictor pr;
  zq_u8 *buf;      /* input buffer, 1<<16 bytes */
  zq_u32 bufn;
} zq_decoder;

void zq_dec_open(zq_decoder *d, zq_zpaql *z);
void zq_dec_close(zq_decoder *d);
void zq_dec_init_block(zq_decoder *d);
int  zq_dec_get(zq_decoder *d);
int  zq_dec_decode(zq_decoder *d, int p);
int  zq_dec_decompress(zq_decoder *d);
int  zq_dec_skip(zq_decoder *d);
int  zq_dec_buffered(const zq_decoder *d);

typedef struct zq_encoder {
  zq_writer *out;
  zq_u32 low, high;
  zq_predictor pr;
  zq_u8 *buf;      /* unmodeled input, 1<<16 bytes */
  zq_u32 bufn;
} zq_encoder;

void zq_enc_open(zq_encoder *e, zq_zpaql *z);
void zq_enc_close(zq_encoder *e);
void zq_enc_init_block(zq_encoder *e);
void zq_enc_encode(zq_encoder *e, int y, int p);
void zq_enc_compress(zq_encoder *e, int c);
void zq_enc_compress_buf(zq_encoder *e, const char *p, int n);   /* c is 0..255 or -1 (EOS) */

/* ------------------------------------------------------------------ */
/* Postprocessor, Decompresser, Compressor                             */
/* ------------------------------------------------------------------ */

typedef struct zq_postproc {
  int state;   /* 0=INIT, 1=PASS, 2..4=loading, 5=POST */
  int hsize;
  int ph, pm;
  zq_zpaql z;
} zq_postproc;

void zq_pp_open(zq_postproc *pp, int h, int m);
void zq_pp_free(zq_postproc *pp);
void zq_pp_set_output(zq_postproc *pp, zq_writer *out);
void zq_pp_set_sha1(zq_postproc *pp, zq_sha1 *sha1);
int  zq_pp_write(zq_postproc *pp, int c);
void zq_pp_write_buf(zq_postproc *pp, const zq_u8 *buf, int n);
int  zq_pp_state(const zq_postproc *pp);

enum { ZQ_BLOCK, ZQ_FILENAME, ZQ_COMMENT, ZQ_DATA, ZQ_SEGEND };
enum { ZQ_FIRSTSEG, ZQ_SEG, ZQ_SKIP };

typedef struct zq_decompresser zq_decompresser;

struct zq_decompresser {
  zq_zpaql z;
  zq_decoder dec;
  zq_postproc pp;
  int state;
  int decode_state;
};

enum { ZQ_C_INIT, ZQ_C_BLOCK1, ZQ_C_SEG1, ZQ_C_BLOCK2, ZQ_C_SEG2 };

typedef struct zq_compressor zq_compressor;

struct zq_compressor {
  zq_zpaql z, pz;
  zq_encoder enc;
  zq_reader *in;
  zq_sha1 sha1;
  char sha1result[20];
  int state;
  int verify;
};

void zq_c_open(zq_compressor *c);
void zq_c_close(zq_compressor *c);
void zq_c_set_output(zq_compressor *c, zq_writer *out);
void zq_c_set_input(zq_compressor *c, zq_reader *in);
void zq_c_set_verify(zq_compressor *c, int v);
void zq_c_write_tag(zq_compressor *c);
void zq_c_start_block_bytes(zq_compressor *c, const zq_u8 *hcomp);
void zq_c_start_block_level(zq_compressor *c, int level);
void zq_c_start_block(zq_compressor *c, const char *config, int *args,
                      zq_writer *pcomp_cmd);
void zq_c_start_segment(zq_compressor *c, const char *filename,
                        const char *comment);
void zq_c_post_process(zq_compressor *c, const char *pcomp, int len);
int  zq_c_compress(zq_compressor *c, int n);
void zq_c_end_segment(zq_compressor *c, const char *sha1string);
char *zq_c_end_segment_checksum(zq_compressor *c, zq_i64 *size, int dosha1);
void zq_c_end_block(zq_compressor *c);
void zq_c_hcomp(zq_compressor *c, zq_writer *out);
int  zq_c_pcomp(zq_compressor *c, zq_writer *out);
zq_i64 zq_c_size(zq_compressor *c);
const char *zq_c_checksum(zq_compressor *c);

/* Built-in block models for startBlock(level) (generated). */
extern const zq_u8 zq_models[299];

/* ------------------------------------------------------------------ */
/* LZ77/BWT preprocessor (libzpaq::LZBuffer) and e8e9 filter           */
/* ------------------------------------------------------------------ */

typedef struct zq_lzbuf zq_lzbuf;
void zq_lz_open(zq_lzbuf *z, zq_buf *inbuf, int *args, const unsigned *sap);
void zq_lz_close(zq_lzbuf *z);
int  zq_lz_get(zq_lzbuf *z);
int  zq_lz_read(zq_lzbuf *z, char *p, int n);
zq_lzbuf *zq_lz_new(zq_buf *inbuf, int *args, const unsigned *sap);
void zq_lz_free(zq_lzbuf *z);

int  zq_lg(unsigned x);
int  zq_popcount(unsigned x);
void zq_e8e9(unsigned char *buf, int n);
int  divsufsort(const unsigned char *T, int *SA, int n);
int  divbwt(const unsigned char *T, unsigned char *U, int *A, int n);

/* ------------------------------------------------------------------ */
/* Compiler                                                            */
/* ------------------------------------------------------------------ */

/* Size in bytes of each component by type (libzpaq::compsize). */
extern const int zq_compsize[256];

/* Component names (compname) and opcodes (opcodelist) for the Compiler. */
extern const char *const zq_compname[256];
extern const char *const zq_opcodelist[272];

/* Method string -> config text (libzpaq::makeConfig).  Caller frees. */
char *zq_make_config(const char *method, int args[9]);

/* ZPAQL compiler (text -> bytecode).  Equivalent to libzpaq::Compiler. */
void zq_compile(const char *in, int *args, zq_zpaql *hz, zq_zpaql *pz,
                zq_writer *out2);

#endif /* ZPAQII_ZQ_INTERNAL_H */
