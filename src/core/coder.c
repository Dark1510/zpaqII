/* zpaqII - arithmetic coder (Decoder and Encoder).
 * Port of libzpaq 7.15 (Unlicense, Matt Mahoney). */
#include "zq_internal.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Decoder                                                             */
/* ------------------------------------------------------------------ */

void zq_dec_open(zq_decoder *d, zq_zpaql *z) {
  memset(d, 0, sizeof(*d));
  d->low = 1;
  d->high = 0xFFFFFFFFu;
  zq_pred_open(&d->pr, z);
  d->bufn = 1u << 16;
  d->buf = (zq_u8 *)zq_calloc(d->bufn);
}

void zq_dec_close(zq_decoder *d) {
  zq_pred_free(&d->pr);
  free(d->buf);
  d->buf = NULL;
}

void zq_dec_init_block(zq_decoder *d) {
  zq_pred_init(&d->pr);
  if (zq_pred_is_modeled(&d->pr)) {
    d->low = 1;
    d->high = 0xFFFFFFFFu;
    d->curr = 0;
  } else {
    d->low = d->high = d->curr = 0;
  }
}

int zq_dec_buffered(const zq_decoder *d) { return (int)(d->wpos - d->rpos); }

int zq_dec_get(zq_decoder *d) {
  if (d->rpos == d->wpos) {
    d->rpos = 0;
    if (!d->in)
      d->wpos = 0;
    else if (d->in->read)
      d->wpos = (zq_u32)d->in->read(d->in->self, (char *)d->buf, (int)d->bufn);
    else
      d->wpos = (zq_u32)zq_reader_read(d->in, (char *)d->buf, (int)d->bufn);
  }
  return d->rpos < d->wpos ? (int)d->buf[d->rpos++] : -1;
}

int zq_dec_decode(zq_decoder *d, int p) {
  zq_u32 mid;
  int y;
  if (d->curr < d->low || d->curr > d->high) zq_error("archive corrupted");
  mid = d->low + (zq_u32)(((zq_u64)(d->high - d->low) * (zq_u64)(zq_u32)p) >> 16);
  if (d->curr <= mid) {
    y = 1;
    d->high = mid;
  } else {
    y = 0;
    d->low = mid + 1;
  }
  while ((d->high ^ d->low) < 0x1000000u) {
    int c;
    d->high = d->high << 8 | 255;
    d->low = d->low << 8;
    d->low += (d->low == 0);
    c = zq_dec_get(d);
    if (c < 0) zq_error("unexpected end of file");
    d->curr = d->curr << 8 | (zq_u32)c;
  }
  return y;
}

int zq_dec_decompress(zq_decoder *d) {
  if (zq_pred_is_modeled(&d->pr)) {
    if (d->curr == 0) { /* segment initialization */
      int i;
      for (i = 0; i < 4; ++i)
        d->curr = d->curr << 8 | (zq_u32)zq_dec_get(d);
    }
    if (zq_dec_decode(d, 0)) {
      if (d->curr != 0) zq_error("decoding end of stream");
      return -1;
    } else {
      int c = 1;
      while (c < 256) {
        const int p = zq_pred_predict(&d->pr) * 2 + 1;
        c += c + zq_dec_decode(d, p);
        zq_pred_update(&d->pr, c & 1);
      }
      return c - 256;
    }
  } else {
    if (d->curr == 0) {
      int i;
      for (i = 0; i < 4; ++i)
        d->curr = d->curr << 8 | (zq_u32)zq_dec_get(d);
      if (d->curr == 0) return -1;
    }
    --d->curr;
    return zq_dec_get(d);
  }
}

int zq_dec_skip(zq_decoder *d) {
  int c = -1;
  if (zq_pred_is_modeled(&d->pr)) {
    while (d->curr == 0) d->curr = (zq_u32)zq_dec_get(d);
    while (d->curr && (c = zq_dec_get(d)) >= 0) d->curr = d->curr << 8 | (zq_u32)c;
    while ((c = zq_dec_get(d)) == 0) {
    }
    return c;
  } else {
    if (d->curr == 0) {
      int i;
      for (i = 0; i < 4 && (c = zq_dec_get(d)) >= 0; ++i)
        d->curr = d->curr << 8 | (zq_u32)c;
    }
    while (d->curr > 0) {
      while (d->curr > 0) {
        --d->curr;
        if (zq_dec_get(d) < 0) {
          zq_error("skipped to EOF");
          return -1;
        }
      }
      {
        int i;
        for (i = 0; i < 4 && (c = zq_dec_get(d)) >= 0; ++i)
          d->curr = d->curr << 8 | (zq_u32)c;
      }
    }
    if (c >= 0) c = zq_dec_get(d);
    return c;
  }
}

/* ------------------------------------------------------------------ */
/* Encoder                                                             */
/* ------------------------------------------------------------------ */

void zq_enc_open(zq_encoder *e, zq_zpaql *z) {
  memset(e, 0, sizeof(*e));
  e->low = 1;
  e->high = 0xFFFFFFFFu;
  zq_pred_open(&e->pr, z);
}

void zq_enc_close(zq_encoder *e) {
  zq_pred_free(&e->pr);
  free(e->buf);
  e->buf = NULL;
}

void zq_enc_init_block(zq_encoder *e) {
  e->low = 1;
  e->high = 0xFFFFFFFFu;
  zq_pred_init(&e->pr);
  if (!zq_pred_is_modeled(&e->pr)) {
    e->low = 0;
    if (!e->buf) {
      e->bufn = 1u << 16;
      e->buf = (zq_u8 *)zq_calloc(e->bufn);
    }
  }
}

void zq_enc_encode(zq_encoder *e, int y, int p) {
  zq_u32 mid =
      e->low + (zq_u32)(((zq_u64)(e->high - e->low) * (zq_u64)(zq_u32)p) >> 16);
  if (y) e->high = mid;
  else e->low = mid + 1;
  while ((e->high ^ e->low) < 0x1000000u) {
    e->out->put(e->out->self, (int)(e->high >> 24));
    e->high = e->high << 8 | 255;
    e->low = e->low << 8;
    e->low += (e->low == 0);
  }
}

/* Flush the stored (unmodelled) buffer: 4 byte length then the bytes. */
static void zq_enc_flush_store(zq_encoder *e) {
  if (!e->low) return;
  e->out->put(e->out->self, (int)((e->low >> 24) & 255));
  e->out->put(e->out->self, (int)((e->low >> 16) & 255));
  e->out->put(e->out->self, (int)((e->low >> 8) & 255));
  e->out->put(e->out->self, (int)(e->low & 255));
  zq_writer_write(e->out, (const char *)e->buf, (int)e->low);
  e->low = 0;
}

/* Compress n bytes.  For unmodelled blocks they are stored, so the whole
 * chunk is copied instead of going through the per-byte function (which
 * costs an indirect call per byte on a stored block). */
void zq_enc_compress_buf(zq_encoder *e, const char *p, int n) {
  if (zq_pred_is_modeled(&e->pr)) {
    while (n-- > 0) zq_enc_compress(e, (zq_u8)*p++);
    return;
  }
  while (n > 0) {
    zq_u32 space = e->bufn - e->low;
    int k = (zq_u32)n < space ? n : (int)space;
    memcpy(e->buf + e->low, p, (size_t)k);
    e->low += (zq_u32)k;
    p += k;
    n -= k;
    if (e->low == e->bufn) zq_enc_flush_store(e);
  }
}

void zq_enc_compress(zq_encoder *e, int c) {
  if (zq_pred_is_modeled(&e->pr)) {
    if (c == -1) {
      zq_enc_encode(e, 1, 0);
    } else {
      int i;
      zq_enc_encode(e, 0, 0);
      for (i = 7; i >= 0; --i) {
        const int p = zq_pred_predict(&e->pr) * 2 + 1;
        const int y = (c >> i) & 1;
        zq_enc_encode(e, y, p);
        zq_pred_update(&e->pr, y);
      }
    }
  } else {
    if (e->low && (c < 0 || e->low == e->bufn)) zq_enc_flush_store(e);
    if (c >= 0) e->buf[e->low++] = (zq_u8)c;
  }
}
