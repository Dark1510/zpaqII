/* zpaqII - block/segment container reader.
 * Port of libzpaq 7.15 (Unlicense, Matt Mahoney). */
#include "zq_internal.h"

#include <stdlib.h>
#include <string.h>

/* Adapter: expose the decoder's buffered byte stream as a zq_reader
 * (upstream's Decoder derives from Reader). */
static int zq_dec_as_get(void *self) { return zq_dec_get((zq_decoder *)self); }

static zq_reader zq_dec_as_reader(zq_decoder *d) {
  zq_reader r;
  r.get = zq_dec_as_get;
  r.read = NULL;
  r.self = d;
  return r;
}

zq_decompresser *zq_d_new(void) {
  zq_decompresser *d = (zq_decompresser *)zq_calloc(sizeof(*d));
  zq_zpaql_init(&d->z);
  zq_dec_open(&d->dec, &d->z);
  zq_pp_open(&d->pp, 0, 0);
  d->state = ZQ_BLOCK;
  d->decode_state = ZQ_FIRSTSEG;
  return d;
}

void zq_d_free(zq_decompresser *d) {
  if (!d) return;
  zq_pp_free(&d->pp);
  zq_dec_close(&d->dec);
  zq_zpaql_free(&d->z);
  free(d);
}

void zq_d_set_input(zq_decompresser *d, zq_reader *in) { d->dec.in = in; }
void zq_d_set_output(zq_decompresser *d, zq_writer *out) {
  zq_pp_set_output(&d->pp, out);
}
void zq_d_set_sha1(zq_decompresser *d, zq_sha1 *sha1) {
  zq_pp_set_sha1(&d->pp, sha1);
}
int zq_d_buffered(const zq_decompresser *d) { return zq_dec_buffered(&d->dec); }

void zq_d_hcomp(zq_decompresser *d, zq_writer *out) {
  zq_zpaql_write(&d->z, out, 0);
}

int zq_d_pcomp(zq_decompresser *d, zq_writer *out) {
  return zq_zpaql_write(&d->pp.z, out, 1);
}

static int zq_d_find_block_impl(zq_decompresser *d, double *memptr) {
  zq_u32 h1 = 0x3D49B113u, h2 = 0x29EB7F93u, h3 = 0x2614BE13u,
         h4 = 0x3828EB13u;
  int c;
  if (d->state != ZQ_BLOCK) zq_error("findBlock in wrong state");

  /* Rolling hashes of the 16-byte tag, initialized to the hash of the first
   * 13 bytes. */
  while ((c = zq_dec_get(&d->dec)) != -1) {
    h1 = h1 * 12 + (zq_u32)c;
    h2 = h2 * 20 + (zq_u32)c;
    h3 = h3 * 28 + (zq_u32)c;
    h4 = h4 * 44 + (zq_u32)c;
    if (h1 == 0xB16B88F1u && h2 == 0xFF5376F1u && h3 == 0x72AC5BF1u &&
        h4 == 0x2F909AF1u)
      break;
  }
  if (c == -1) return 0;

  c = zq_dec_get(&d->dec);
  if (c != 1 && c != 2) zq_error("unsupported ZPAQ level");
  if (zq_dec_get(&d->dec) != 1) zq_error("unsupported ZPAQL type");
  {
    zq_reader r = zq_dec_as_reader(&d->dec);
    zq_zpaql_read(&d->z, &r);
  }
  if (c == 1 && d->z.header.n > 6 && ZQ_HDR(&d->z, 6) == 0)
    zq_error("ZPAQ level 1 requires at least 1 component");
  if (memptr) *memptr = zq_zpaql_memory(&d->z);
  d->state = ZQ_FILENAME;
  d->decode_state = ZQ_FIRSTSEG;
  return 1;
}

/* Read the start of a segment (1) or the end of block code (255).
 * Returns 1 if a segment was found (writing its filename), else 0. */
static int zq_d_find_filename_impl(zq_decompresser *d, zq_writer *filename) {
  int c = zq_dec_get(&d->dec);
  if (c == 1) {
    for (;;) {
      c = zq_dec_get(&d->dec);
      if (c == -1) zq_error("unexpected EOF");
      if (c == 0) {
        d->state = ZQ_COMMENT;
        return 1;
      }
      if (filename) filename->put(filename->self, c);
    }
  } else if (c == 255) {
    d->state = ZQ_BLOCK;
    return 0;
  } else {
    zq_error("missing segment or end of block");
  }
  return 0;
}

static int zq_d_read_comment_impl(zq_decompresser *d, zq_writer *comment) {
  d->state = ZQ_DATA;
  for (;;) {
    int c = zq_dec_get(&d->dec);
    if (c == -1) zq_error("unexpected EOF");
    if (c == 0) break;
    if (comment) comment->put(comment->self, c);
  }
  if (zq_dec_get(&d->dec) != 0) zq_error("missing reserved byte");
  return 1;
}

/* Decode up to lim bytes into buf, stopping at the first c < 0 (end of
 * segment), which is returned; *k is the number of bytes stored.  The PCOMP
 * then sees the same bytes in the same order as with one call per byte.  If
 * decoding fails part-way, the bytes decoded before the failure still reach
 * the postprocessor first and the same error is raised again, so whether an
 * error is reported, and which one, is exactly as before. */
static int zq_d_fill(zq_decompresser *d, zq_u8 *buf, int lim, int *kout) {
  volatile int k = 0;
  int c = 0;
  zq_err_frame ef;
  if (setjmp(ef.jb)) {
    char msg[sizeof(ef.msg)];
    memcpy(msg, ef.msg, sizeof(msg));
    zq_pp_write_buf(&d->pp, buf, k);
    zq_error(msg);
  }
  zq_err_push(&ef);
  while (k < lim) {
    c = zq_dec_decompress(&d->dec);
    if (c < 0) break;
    buf[k] = (zq_u8)c;
    k = k + 1;
  }
  zq_err_pop(&ef);
  *kout = k;
  return c;
}

/* Decompress n bytes, or all when n < 0.  Returns 0 when the segment ends. */
static int zq_d_decompress_impl(zq_decompresser *d, int n) {
  if (d->decode_state == ZQ_SKIP) zq_error("decompression after skipped segment");
  if (d->state != ZQ_DATA) zq_error("decompress in wrong state");

  /* Initialize models at the start of the block */
  if (d->decode_state == ZQ_FIRSTSEG) {
    zq_dec_init_block(&d->dec);
    zq_pp_open(&d->pp, ZQ_HDR(&d->z, 4), ZQ_HDR(&d->z, 5));
    d->decode_state = ZQ_SEG;
  }

  /* Decompress and load PCOMP into the postprocessor */
  while ((zq_pp_state(&d->pp) & 3) != 1) zq_pp_write(&d->pp, zq_dec_decompress(&d->dec));

  /* ZPAQL(II): decode a run of bytes, then hand the whole run to the
   * postprocessor (one call into the PCOMP for all of them); see
   * zq_d_fill(). */
  while (n) {
    zq_u8 tmp[4096];
    int k;
    const int lim = (n > 0 && n < (int)sizeof(tmp)) ? n : (int)sizeof(tmp);
    const int c = zq_d_fill(d, tmp, lim, &k);
    zq_pp_write_buf(&d->pp, tmp, k);
    if (n > 0) n -= k;
    if (k < lim) { /* stopped at c < 0 */
      zq_pp_write(&d->pp, c);
      if (c == -1) {
        d->state = ZQ_SEGEND;
        return 0;
      }
      if (n > 0) --n;
    }
  }
  return 1;
}

/* Read the end of a segment.  sha1string[0] is 0 (no checksum) or 1 followed
 * by the 20 checksum bytes.  May be NULL to discard. */
static int zq_d_read_segment_end_impl(zq_decompresser *d, char *sha1string) {
  int c = 0, i;
  if (d->state == ZQ_DATA) {
    c = zq_dec_skip(&d->dec);
    d->decode_state = ZQ_SKIP;
  } else if (d->state == ZQ_SEGEND) {
    c = zq_dec_get(&d->dec);
  } else {
    zq_error("readSegmentEnd in wrong state");
  }
  d->state = ZQ_FILENAME;

  if (c == 254) {
    if (sha1string) sha1string[0] = 0;
  } else if (c == 253) {
    if (sha1string) sha1string[0] = 1;
    for (i = 1; i <= 20; ++i) {
      c = zq_dec_get(&d->dec);
      if (sha1string) sha1string[i] = (char)c;
    }
  } else {
    zq_error("missing end of segment marker");
  }
  return 1;
}

/* ------------------------------------------------------------------ */
/* Public entry points: errors are caught here and reported as -1      */
/* ------------------------------------------------------------------ */

int zq_d_find_block(zq_decompresser *d, double *memptr) {
  zq_err_frame ef;
  int rc;
  if (setjmp(ef.jb)) return -1;
  zq_err_push(&ef);
  rc = zq_d_find_block_impl(d, memptr);
  zq_err_pop(&ef);
  return rc;
}

int zq_d_find_filename(zq_decompresser *d, zq_writer *filename) {
  zq_err_frame ef;
  int rc;
  if (setjmp(ef.jb)) return -1;
  zq_err_push(&ef);
  rc = zq_d_find_filename_impl(d, filename);
  zq_err_pop(&ef);
  return rc;
}

int zq_d_read_comment(zq_decompresser *d, zq_writer *comment) {
  zq_err_frame ef;
  int rc;
  if (setjmp(ef.jb)) return -1;
  zq_err_push(&ef);
  rc = zq_d_read_comment_impl(d, comment);
  zq_err_pop(&ef);
  return rc;
}

int zq_d_decompress(zq_decompresser *d, int n) {
  zq_err_frame ef;
  int rc;
  if (setjmp(ef.jb)) return -1;
  zq_err_push(&ef);
  rc = zq_d_decompress_impl(d, n);
  zq_err_pop(&ef);
  return rc;
}

int zq_d_read_segment_end(zq_decompresser *d, char *sha1string) {
  zq_err_frame ef;
  int rc;
  if (setjmp(ef.jb)) return -1;
  zq_err_push(&ef);
  rc = zq_d_read_segment_end_impl(d, sha1string);
  zq_err_pop(&ef);
  return rc;
}
