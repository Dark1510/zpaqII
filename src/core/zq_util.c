/* zpaqII - core base utilities: errors, aligned memory, streams.
 * Literal port of libzpaq 7.15 (Unlicense, Matt Mahoney). */
#include "../../include/zq.h"

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Errors                                                              */
/* ------------------------------------------------------------------ */

static _Thread_local zq_err_frame *zq_top;
static _Thread_local char zq_msg[256];

void zq_err_push(zq_err_frame *f) {
  f->prev = zq_top;
  f->msg[0] = 0;
  zq_top = f;
}

void zq_err_pop(zq_err_frame *f) {
  /* Only the top frame is popped; a mismatch is a core bug. */
  if (zq_top == f) zq_top = f->prev;
  else {
    zq_err_frame *p = zq_top;
    while (p && p->prev != f) p = p->prev;
    if (p) p->prev = f->prev;
    else if (zq_top == f) zq_top = f->prev;
  }
}

const char *zq_last_error(void) { return zq_msg; }

void zq_error(const char *msg) {
  zq_err_frame *f = zq_top;
  snprintf(zq_msg, sizeof(zq_msg), "%s", msg ? msg : "");
  if (f) {
    snprintf(f->msg, sizeof(f->msg), "%s", msg ? msg : "");
    zq_top = f->prev;
    longjmp(f->jb, 1);
  }
  /* No active frame: same observable behavior as the libzpaq.h example
   * (message to stderr and exit 1). */
  fprintf(stderr, "zpaq error: %s\n", msg ? msg : "");
  exit(1);
}

/* ------------------------------------------------------------------ */
/* Memory                                                              */
/* ------------------------------------------------------------------ */

void *zq_calloc(size_t n) {
  void *p = calloc(n ? n : 1, 1);
  if (!p) zq_error("Out of memory");
  return p;
}

void *zq_realloc(void *p, size_t n) {
  void *q = realloc(p, n ? n : 1);
  if (!q) zq_error("Out of memory");
  return q;
}

/* Literal equivalent of libzpaq::Array<T>::resize: free, allocate
 * 128 + n*elem with calloc and align the usable area to 64 bytes. */
void zq_arr_resize(zq_arr *a, size_t n, size_t elem) {
  if (!a) zq_error("null Array");
  if (a->data) {
    free((char *)a->data - a->offset);
    a->data = NULL;
    a->n = 0;
    a->offset = 0;
  }
  a->elem = (int)elem;
  if (n == 0) return;
  {
    const size_t nb = 128 + n * elem; /* upstream overflow check */
    char *p;
    if (nb <= 128 || (nb - 128) / elem != n) zq_error("Array too big");
    p = (char *)calloc(nb, 1);
    if (!p) zq_error("Out of memory");
    a->offset = 64 - (int)((size_t)p & 63);
    a->data = p + a->offset;
    a->n = n;
  }
}

void zq_arr_free(zq_arr *a) {
  if (!a) return;
  if (a->data) free((char *)a->data - a->offset);
  a->data = NULL;
  a->n = 0;
  a->offset = 0;
}

/* ------------------------------------------------------------------ */
/* Default streams                                                     */
/* ------------------------------------------------------------------ */

int zq_reader_read(zq_reader *r, char *buf, int n) {
  int i = 0, c;
  if (!r || !r->get) return 0;
  while (i < n && (c = r->get(r->self)) >= 0) buf[i++] = (char)c;
  return i;
}

void zq_writer_write(zq_writer *w, const char *buf, int n) {
  int i;
  if (!w) return;
  /* Prefer a bulk write: the byte loop below is only a fallback (it costs an
   * indirect call per byte, which shows up on stored blocks). */
  if (w->write) {
    w->write(w->self, buf, n);
    return;
  }
  if (!w->put) return;
  for (i = 0; i < n; ++i) w->put(w->self, (zq_u8)buf[i]);
}

/* ------------------------------------------------------------------ */
/* Heap wrappers used by the Pascal host                               */
/* ------------------------------------------------------------------ */

zq_sha1 *zq_sha1_new(void) {
  zq_sha1 *s = (zq_sha1 *)zq_calloc(sizeof(zq_sha1));
  zq_sha1_init(s);
  return s;
}

void zq_sha1_free(zq_sha1 *s) { free(s); }

zq_aes_ctr *zq_aes_new(const char *key, int keylen, const char *iv) {
  zq_aes_ctr *a = (zq_aes_ctr *)zq_calloc(sizeof(zq_aes_ctr));
  zq_aes_init(a, key, keylen, iv);
  return a;
}

void zq_aes_free(zq_aes_ctr *a) { free(a); }
