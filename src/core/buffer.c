/* zpaqII - in-memory buffer (libzpaq::StringBuffer) and adapters.
 * Port of libzpaq 7.15 (Unlicense, Matt Mahoney). */
#include "../../include/zq.h"

#include <stdlib.h>
#include <string.h>

struct zq_buf {
  unsigned char *p;   /* allocated memory, may be NULL */
  size_t al;          /* bytes allocated, 0 iff p==NULL */
  size_t wpos;        /* next byte to write, <= al */
  size_t rpos;        /* next byte to read, <= wpos */
  size_t limit;       /* maximum size */
  size_t init;        /* initial growth size */
};

static void zq_buf_reserve(zq_buf *b, size_t a) {
  if (a <= b->al) return;
  {
    unsigned char *q = (unsigned char *)(b->p ? realloc(b->p, a) : malloc(a));
    if (!q) zq_error("Out of memory");
    b->p = q;
    b->al = a;
  }
}

static void zq_buf_lengthen(zq_buf *b, size_t n) {
  size_t a;
  if (b->wpos + n > b->limit || b->wpos + n < b->wpos)
    zq_error("StringBuffer overflow");
  if (b->wpos + n <= b->al) return;
  a = b->al;
  while (b->wpos + n >= a) a = a * 2 + b->init;
  zq_buf_reserve(b, a);
}

zq_buf *zq_buf_new(size_t init) {
  zq_buf *b = (zq_buf *)zq_calloc(sizeof(zq_buf));
  b->init = init > 128 ? init : 128;
  b->limit = (size_t)-1;
  return b;
}

void zq_buf_free(zq_buf *b) {
  if (!b) return;
  free(b->p);
  free(b);
}

void zq_buf_set_limit(zq_buf *b, size_t n) { b->limit = n; }

size_t zq_buf_size(const zq_buf *b) { return b->wpos; }
size_t zq_buf_remaining(const zq_buf *b) { return b->wpos - b->rpos; }

void zq_buf_reset(zq_buf *b) {
  free(b->p);
  b->p = NULL;
  b->al = b->rpos = b->wpos = 0;
}

void zq_buf_put(zq_buf *b, int c) {
  zq_buf_lengthen(b, 1);
  b->p[b->wpos++] = (unsigned char)c;
}

void zq_buf_write(zq_buf *b, const char *p, int n) {
  if (n < 1) return;
  zq_buf_lengthen(b, (size_t)n);
  if (p) memcpy(b->p + b->wpos, p, (size_t)n);
  b->wpos += (size_t)n;
}

int zq_buf_get(zq_buf *b) {
  return b->rpos < b->wpos ? b->p[b->rpos++] : -1;
}

int zq_buf_read(zq_buf *b, char *p, int n) {
  if (b->rpos + (size_t)n > b->wpos) n = (int)(b->wpos - b->rpos);
  if (n > 0 && p) memcpy(p, b->p + b->rpos, (size_t)n);
  b->rpos += (size_t)n;
  return n;
}

void zq_buf_resize(zq_buf *b, size_t n) {
  b->wpos = n;
  if (b->rpos > b->wpos) b->rpos = b->wpos;
}

void zq_buf_swap(zq_buf *a, zq_buf *b) {
  unsigned char *p = a->p;
  size_t al = a->al, wp = a->wpos, rp = a->rpos, lim = a->limit;
  a->p = b->p;
  a->al = b->al;
  a->wpos = b->wpos;
  a->rpos = b->rpos;
  a->limit = b->limit;
  b->p = p;
  b->al = al;
  b->wpos = wp;
  b->rpos = rp;
  b->limit = lim;
}

const char *zq_buf_data(const zq_buf *b) { return (const char *)b->p; }

void zq_buf_seek_read(zq_buf *b, size_t pos) {
  if (pos > b->wpos) pos = b->wpos;
  b->rpos = pos;
}

/* ------------------------------------------------------------------ */
/* Reader/Writer adapters                                              */
/* ------------------------------------------------------------------ */

static int zq_buf_rget(void *self) { return zq_buf_get((zq_buf *)self); }
static int zq_buf_rread(void *self, char *p, int n) {
  return zq_buf_read((zq_buf *)self, p, n);
}
static void zq_buf_wput(void *self, int c) { zq_buf_put((zq_buf *)self, c); }
static void zq_buf_wwrite(void *self, const char *p, int n) {
  zq_buf_write((zq_buf *)self, p, n);
}

zq_reader zq_buf_reader(zq_buf *b) {
  zq_reader r;
  r.get = zq_buf_rget;
  r.read = zq_buf_rread;
  r.self = b;
  return r;
}

zq_writer zq_buf_writer(zq_buf *b) {
  zq_writer w;
  w.put = zq_buf_wput;
  w.write = zq_buf_wwrite;
  w.self = b;
  return w;
}
