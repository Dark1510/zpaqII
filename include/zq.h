/* zpaqII - core C API.  Single public header (see docs/ARCHITECTURE.md).
 *
 * Port of libzpaq 7.15 (public domain / Unlicense, Matt Mahoney).
 * divsufsort keeps its MIT license.
 *
 * Rules: fixed-width types, no undocumented UB, same error messages as
 * upstream, no file access (everything goes through callbacks).
 */
#ifndef ZPAQII_ZQ_H
#define ZPAQII_ZQ_H 1

#include <setjmp.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Types                                                               */
/* ------------------------------------------------------------------ */

typedef uint8_t  zq_u8;
typedef uint16_t zq_u16;
typedef uint32_t zq_u32;
typedef uint64_t zq_u64;
typedef int64_t  zq_i64;

#define ZQ_NORETURN __attribute__((noreturn))

/* ------------------------------------------------------------------ */
/* Errors                                                              */
/*                                                                     */
/* Upstream throws std::runtime_error (bad_alloc when the message has   */
/* "out of memory") and main() prints it and exits with 1.  The core    */
/* cannot unwind through Pascal, so every entry point installs a frame  */
/* and returns a status; the host reads the message.                    */
/* ------------------------------------------------------------------ */

typedef struct zq_err_frame {
  struct zq_err_frame *prev;
  char  msg[256];
  jmp_buf jb;
} zq_err_frame;

/* Mandatory pattern at every core entry point:
 *
 *   zq_err_frame ef;
 *   if (setjmp(ef.jb)) return -1;      // error: the frame is already popped
 *   zq_err_push(&ef);
 *   ... core calls ...
 *   zq_err_pop(&ef);
 *   return 0;
 *
 * zq_error() copies the message, pops the frame and longjmps.  The jump
 * happens entirely inside C frames, so it never crosses Pascal.
 */
void zq_err_push(zq_err_frame *f);
void zq_err_pop(zq_err_frame *f);
const char *zq_last_error(void);   /* last error message of this thread */

/* Never returns.  With no active frame it prints "zpaq error: <msg>" and exits(1). */
void zq_error(const char *msg) ZQ_NORETURN;

/* ------------------------------------------------------------------ */
/* Memory                                                              */
/* ------------------------------------------------------------------ */

void *zq_calloc(size_t n);            /* zero-filled; error on failure */
void *zq_realloc(void *p, size_t n);

/* Equivalent to libzpaq::Array<T>: 64-byte aligned buffer, always
 * zero-filled, `n` elements of `elem` bytes each. */
typedef struct zq_arr {
  void  *data;    /* usable area, 64-byte aligned */
  size_t n;       /* number of elements */
  int    offset;  /* bytes from allocation start to data */
  int    elem;    /* element size (1..8); fits in upstream's padding so that
                     sizeof(zq_arr) == sizeof(libzpaq::Array) == 24 and the
                     predictor/component layouts match upstream's exactly */
} zq_arr;

void zq_arr_resize(zq_arr *a, size_t n, size_t elem);
void zq_arr_free(zq_arr *a);

#define ZQ_U8A(a, i)  (((zq_u8 *)(a)->data)[(i)])
#define ZQ_U32A(a, i) (((zq_u32 *)(a)->data)[(i)])

/* ------------------------------------------------------------------ */
/* Streams (Reader/Writer)                                             */
/* ------------------------------------------------------------------ */

typedef struct zq_reader {
  int  (*get)(void *self);                       /* 0..255, -1 = EOF */
  int  (*read)(void *self, char *buf, int n);    /* optional (NULL = get) */
  void  *self;
} zq_reader;

typedef struct zq_writer {
  void (*put)(void *self, int c);
  void (*write)(void *self, const char *buf, int n); /* optional (NULL) */
  void  *self;
} zq_writer;

/* Default read()/write() built on get()/put(). */
int  zq_reader_read(zq_reader *r, char *buf, int n);
void zq_writer_write(zq_writer *w, const char *buf, int n);

/* ------------------------------------------------------------------ */
/* In-memory buffer (libzpaq::StringBuffer)                            */
/* ------------------------------------------------------------------ */

typedef struct zq_buf zq_buf;

zq_buf *zq_buf_new(size_t init);
void    zq_buf_free(zq_buf *b);
void    zq_buf_set_limit(zq_buf *b, size_t n);
size_t  zq_buf_size(const zq_buf *b);
size_t  zq_buf_remaining(const zq_buf *b);
void    zq_buf_reset(zq_buf *b);
void    zq_buf_put(zq_buf *b, int c);
void    zq_buf_write(zq_buf *b, const char *p, int n);  /* p NULL = advance only */
int     zq_buf_get(zq_buf *b);
int     zq_buf_read(zq_buf *b, char *p, int n);         /* p NULL = advance only */
void    zq_buf_resize(zq_buf *b, size_t n);
void    zq_buf_swap(zq_buf *a, zq_buf *b);
const char *zq_buf_data(const zq_buf *b);
void    zq_buf_seek_read(zq_buf *b, size_t pos);

/* Reader/Writer adapters over a zq_buf. */
zq_reader zq_buf_reader(zq_buf *b);
zq_writer zq_buf_writer(zq_buf *b);

/* ------------------------------------------------------------------ */
/* SHA-1 (block/segment checksums and dedup)                           */
/* ------------------------------------------------------------------ */

typedef struct zq_sha1 {
  zq_u64 len;
  zq_u32 h[5];
  zq_u32 w[16];
  char   hbuf[20];
} zq_sha1;

void        zq_sha1_init(zq_sha1 *s);
void        zq_sha1_put(zq_sha1 *s, int c);
void        zq_sha1_write(zq_sha1 *s, const char *buf, zq_i64 n);
const char *zq_sha1_result(zq_sha1 *s);
double      zq_sha1_size(const zq_sha1 *s);
zq_u64      zq_sha1_usize(const zq_sha1 *s);

/* ------------------------------------------------------------------ */
/* Fragment scanner (the content-defined cut loop of add)              */
/* ------------------------------------------------------------------ */

/* The state of one fragment while it is being cut.  The host reads sz,
 * hits and o1 when a fragment ends; h is internal. */
typedef struct zq_fragscan {
  zq_i64 sz;      /* bytes in the fragment so far */
  zq_u32 h;       /* rolling hash of the fragment */
  zq_u32 hits;    /* bytes that matched the order-1 prediction */
  int    c1;      /* previous byte (order-1 context) */
  zq_u8  o1[256]; /* order-1 model: context -> predicted byte */
} zq_fragscan;

/* ------------------------------------------------------------------ */
/* Fragment chains (the parallel fragmenter)                           */
/* ------------------------------------------------------------------ */

/* One complete fragment of a scanned chain.  Layout chosen so the host can
 * read it as a record without marshalling; keep in sync with zqcore.pas. */
typedef struct zq_fragcut {
  zq_i64 start;   /* offset of the fragment in the buffer */
  zq_i64 len;     /* its length */
  zq_u32 hits;    /* order-1 prediction hits */
  zq_u8  o1[256]; /* order-1 table at its end */
  zq_u8  sha1[20];/* its SHA-1, already computed */
} zq_fragcut;

/* Scan a chain of fragments starting fresh at `from`.
 *
 * Writes up to `cap` complete fragments to `out` and stops when the scan
 * reaches `fine` (*done = 1) or when `dopo` fragments that end at or after
 * `stop_at` have been produced (*done = 0): the caller uses that overlap to
 * stitch the chain of one range into the chain of the previous one.
 *
 * Returns the number of fragments written.  The fragment still open when the
 * scan stops at `fine` is NOT written unless `open` is not NULL, in which case
 * its start, length, hits, order-1 table and SHA-1 are stored there. */
int zq_frag_chain(const zq_u8 *base, zq_i64 from, zq_i64 fine, zq_i64 stop_at,
                  int dopo, zq_i64 maxfrag, zq_i64 minfrag, int fragment,
                  zq_fragcut *out, int cap, int *done, zq_fragcut *open);

void  zq_fragscan_init(zq_fragscan *st);
/* Consume up to n bytes of buf.  Returns the number consumed; *cut is set to 1
 * when the fragment ended there.  The caller resets with zq_fragscan_init for
 * the next fragment. */
zq_i64 zq_frag_scan(const zq_u8 *buf, zq_i64 n, zq_fragscan *st,
                    zq_i64 maxfrag, zq_i64 minfrag, int fragment, int *cut);

/* ------------------------------------------------------------------ */
/* SHA-256                                                             */
/* ------------------------------------------------------------------ */

typedef struct zq_sha256 {
  unsigned len0, len1;
  unsigned s[8];
  unsigned w[16];
  char     hbuf[32];
} zq_sha256;

zq_sha256  *zq_sha256_new(void);
void        zq_sha256_free(zq_sha256 *s);
void        zq_sha256_init(zq_sha256 *s);
void        zq_sha256_put(zq_sha256 *s, int c);
void        zq_sha256_write(zq_sha256 *s, const char *buf, zq_i64 n);
const char *zq_sha256_result(zq_sha256 *s);
double      zq_sha256_size(const zq_sha256 *s);
zq_u64      zq_sha256_usize(const zq_sha256 *s);

/* ------------------------------------------------------------------ */
/* AES-256 in CTR mode (archive key)                                   */
/* ------------------------------------------------------------------ */

typedef struct zq_aes_ctr {
  zq_u32 Te0[256], Te1[256], Te2[256], Te3[256], Te4[256];
  zq_u32 ek[60];
  int    Nr;
  zq_u32 iv0, iv1;
} zq_aes_ctr;

void zq_aes_init(zq_aes_ctr *a, const char *key, int keylen, const char *iv);
void zq_aes_encrypt_block(zq_aes_ctr *a, zq_u32 s0, zq_u32 s1, zq_u32 s2,
                          zq_u32 s3, unsigned char *ct);
void zq_aes_encrypt(zq_aes_ctr *a, char *buf, int n, zq_u64 offset);

/* ------------------------------------------------------------------ */
/* scrypt / stretchKey / random                                        */
/* ------------------------------------------------------------------ */

void zq_scrypt(const char *pw, int pwlen, const char *salt, int saltlen,
               int n, int r, int p, char *buf, int buflen);
void zq_stretch_key(char *out, const char *in, const char *salt);
void zq_random(char *buf, int n);

/* ------------------------------------------------------------------ */
/* Block container (libzpaq Decompresser / Compressor)                 */
/* ------------------------------------------------------------------ */

typedef struct zq_decompresser zq_decompresser;

zq_decompresser *zq_d_new(void);
void zq_d_free(zq_decompresser *d);

void zq_d_set_input(zq_decompresser *d, zq_reader *in);
void zq_d_set_output(zq_decompresser *d, zq_writer *out);
void zq_d_set_sha1(zq_decompresser *d, zq_sha1 *sha1);
int  zq_d_buffered(const zq_decompresser *d);

/* 1 = block header found, 0 = end of input, -1 = error */
int  zq_d_find_block(zq_decompresser *d, double *memptr);
/* 1 = segment found (filename written if not NULL), 0 = end of block, -1 = error */
int  zq_d_find_filename(zq_decompresser *d, zq_writer *filename);
int  zq_d_read_comment(zq_decompresser *d, zq_writer *comment);
/* n bytes, or the whole segment when n < 0.  Returns 0 at segment end. */
int  zq_d_decompress(zq_decompresser *d, int n);
/* sha1string21: [0] = 0 (no checksum) or 1, then the 20-byte SHA-1. */
int  zq_d_read_segment_end(zq_decompresser *d, char *sha1string21);
void zq_d_hcomp(zq_decompresser *d, zq_writer *out);
int  zq_d_pcomp(zq_decompresser *d, zq_writer *out);

/* Heap wrappers so the host never needs the struct layout. */
zq_sha1   *zq_sha1_new(void);
void       zq_sha1_free(zq_sha1 *s);
zq_aes_ctr *zq_aes_new(const char *key, int keylen, const char *iv);
void       zq_aes_free(zq_aes_ctr *a);

/* Compress one block (single segment) with a zpaq method string.
 * Returns 0 on success, -1 on error (see zq_last_error()). */
int zq_compress_block(zq_buf *in, zq_writer *out, const char *method,
                      const char *filename, const char *comment, int dosha1);

/* Multi-block compression (libzpaq::compress).  Returns 0 or -1. */
int zq_compress(zq_reader *in, zq_writer *out, const char *method,
                const char *filename, const char *comment, int dosha1);

#ifdef __cplusplus
}
#endif

#endif /* ZPAQII_ZQ_H */
