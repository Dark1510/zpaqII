/* zpaqII — core C API.  Cabecera publica unica (ver docs/ARCHITECTURE.md).
 *
 * Port de libzpaq 7.15 (dominio publico / Unlicense, Matt Mahoney).
 * divsufsort conserva su licencia MIT.
 *
 * Reglas: tipos con ancho fijo, sin UB no documentado, mismos mensajes de
 * error que upstream, ningun acceso a archivos (todo pasa por callbacks).
 */
#ifndef ZQ_H
#define ZQ_H 1

#include <setjmp.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Tipos                                                               */
/* ------------------------------------------------------------------ */

typedef uint8_t  zq_u8;
typedef uint16_t zq_u16;
typedef uint32_t zq_u32;
typedef uint64_t zq_u64;
typedef int64_t  zq_i64;

#define ZQ_NORETURN __attribute__((noreturn))

/* ------------------------------------------------------------------ */
/* Errores                                                             */
/*                                                                     */
/* Upstream lanza std::runtime_error (bad_alloc si el mensaje contiene  */
/* "out of memory") y main() lo imprime y sale con 1.  El core no puede */
/* desenrollar a traves de Pascal, asi que cada punto de entrada        */
/* establece un frame y devuelve estado; el host lee el mensaje.        */
/* ------------------------------------------------------------------ */

typedef struct zq_err_frame {
  struct zq_err_frame *prev;
  char  msg[256];
  jmp_buf jb;
} zq_err_frame;

/* Patron obligatorio en cada punto de entrada del core:
 *
 *   zq_err_frame ef;
 *   if (setjmp(ef.jb)) return -1;      // error: el frame ya fue desapilado
 *   zq_err_push(&ef);
 *   ... llamadas al core ...
 *   zq_err_pop(&ef);
 *   return 0;
 *
 * zq_error() copia el mensaje, desapila el frame y longjmp.  Como el salto
 * ocurre integramente dentro de frames C, no atraviesa Pascal.
 */
void zq_err_push(zq_err_frame *f);
void zq_err_pop(zq_err_frame *f);
const char *zq_last_error(void);   /* mensaje del ultimo error del hilo */

/* Nunca retorna.  Sin frame activo imprime "zpaq error: <msg>" y exit(1). */
void zq_error(const char *msg) ZQ_NORETURN;

/* ------------------------------------------------------------------ */
/* Memoria                                                             */
/* ------------------------------------------------------------------ */

void *zq_calloc(size_t n);            /* cero-inicializado; error si falla */
void *zq_realloc(void *p, size_t n);

/* Equivalente a libzpaq::Array<T>: buffer alineado a 64 bytes, siempre
 * cero-inicializado, `n` elementos de `elem` bytes. */
typedef struct zq_arr {
  void  *data;    /* zona util, alineada a 64 */
  size_t n;       /* numero de elementos */
  int    offset;  /* bytes desde la asignacion hasta data */
  size_t elem;    /* tamano de elemento */
} zq_arr;

void zq_arr_resize(zq_arr *a, size_t n, size_t elem);
void zq_arr_free(zq_arr *a);

#define ZQ_U8A(a, i)  (((zq_u8 *)(a)->data)[(i)])
#define ZQ_U32A(a, i) (((zq_u32 *)(a)->data)[(i)])

/* ------------------------------------------------------------------ */
/* Flujos (Reader/Writer)                                              */
/* ------------------------------------------------------------------ */

typedef struct zq_reader {
  int  (*get)(void *self);                       /* 0..255, -1 = EOF */
  int  (*read)(void *self, char *buf, int n);    /* opcional (NULL = get) */
  void  *self;
} zq_reader;

typedef struct zq_writer {
  void (*put)(void *self, int c);
  void (*write)(void *self, const char *buf, int n); /* opcional (NULL) */
  void  *self;
} zq_writer;

/* Implementaciones por defecto de read()/write() sobre get()/put(). */
int  zq_reader_read(zq_reader *r, char *buf, int n);
void zq_writer_write(zq_writer *w, const char *buf, int n);

/* ------------------------------------------------------------------ */
/* Buffer en memoria (libzpaq::StringBuffer)                           */
/* ------------------------------------------------------------------ */

typedef struct zq_buf zq_buf;

zq_buf *zq_buf_new(size_t init);
void    zq_buf_free(zq_buf *b);
void    zq_buf_set_limit(zq_buf *b, size_t n);
size_t  zq_buf_size(const zq_buf *b);
size_t  zq_buf_remaining(const zq_buf *b);
void    zq_buf_reset(zq_buf *b);
void    zq_buf_put(zq_buf *b, int c);
void    zq_buf_write(zq_buf *b, const char *p, int n);  /* p NULL = solo avanza */
int     zq_buf_get(zq_buf *b);
int     zq_buf_read(zq_buf *b, char *p, int n);         /* p NULL = solo avanza */
void    zq_buf_resize(zq_buf *b, size_t n);
void    zq_buf_swap(zq_buf *a, zq_buf *b);
const char *zq_buf_data(const zq_buf *b);
void    zq_buf_seek_read(zq_buf *b, size_t pos);

/* Adaptadores Reader/Writer sobre un zq_buf. */
zq_reader zq_buf_reader(zq_buf *b);
zq_writer zq_buf_writer(zq_buf *b);

/* ------------------------------------------------------------------ */
/* SHA-1 (checksums de bloque/segmento y dedup)                        */
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
/* SHA-256                                                             */
/* ------------------------------------------------------------------ */

typedef struct zq_sha256 {
  unsigned len0, len1;
  unsigned s[8];
  unsigned w[16];
  char     hbuf[32];
} zq_sha256;

void        zq_sha256_init(zq_sha256 *s);
void        zq_sha256_put(zq_sha256 *s, int c);
void        zq_sha256_write(zq_sha256 *s, const char *buf, zq_i64 n);
const char *zq_sha256_result(zq_sha256 *s);
double      zq_sha256_size(const zq_sha256 *s);
zq_u64      zq_sha256_usize(const zq_sha256 *s);

/* ------------------------------------------------------------------ */
/* AES-256 en modo CTR (clave de archivo)                              */
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

#ifdef __cplusplus
}
#endif

#endif /* ZQ_H */
