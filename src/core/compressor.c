/* zpaqII - block writer (libzpaq::Compressor).
 * Port of libzpaq 7.15 (Unlicense, Matt Mahoney). */
#include "zq_internal.h"

#include <stdlib.h>
#include <string.h>

/* Read 16-bit little-endian from a byte pointer (libzpaq::toU16). */
static int zq_to_u16(const zq_u8 *p) { return p[0] + 256 * p[1]; }

/* Byte reader over a memory string (libzpaq::MemoryReader). */
typedef struct {
  const zq_u8 *p;
} zq_memr;

static int zq_memr_get(void *self) {
  zq_memr *m = (zq_memr *)self;
  return *m->p++;
}

void zq_c_open(zq_compressor *c) {
  memset(c, 0, sizeof(*c));
  zq_zpaql_init(&c->z);
  zq_zpaql_init(&c->pz);
  zq_enc_open(&c->enc, &c->z);
  zq_sha1_init(&c->sha1);
  c->state = ZQ_C_INIT;
  c->verify = 0;
}

void zq_c_close(zq_compressor *c) {
  zq_enc_close(&c->enc);
  zq_zpaql_free(&c->z);
  zq_zpaql_free(&c->pz);
}

void zq_c_set_output(zq_compressor *c, zq_writer *out) { c->enc.out = out; }
void zq_c_set_input(zq_compressor *c, zq_reader *in) { c->in = in; }
void zq_c_set_verify(zq_compressor *c, int v) { c->verify = v; }
void zq_c_hcomp(zq_compressor *c, zq_writer *out) {
  zq_zpaql_write(&c->z, out, 0);
}
int zq_c_pcomp(zq_compressor *c, zq_writer *out) {
  return zq_zpaql_write(&c->pz, out, 1);
}
zq_i64 zq_c_size(zq_compressor *c) { return (zq_i64)zq_sha1_usize(&c->sha1); }
const char *zq_c_checksum(zq_compressor *c) { return zq_sha1_result(&c->sha1); }

void zq_c_write_tag(zq_compressor *c) {
  zq_writer *out = c->enc.out;
  if (c->state != ZQ_C_INIT) zq_error("writeTag in wrong state");
  out->put(out->self, 0x37);
  out->put(out->self, 0x6b);
  out->put(out->self, 0x53);
  out->put(out->self, 0x74);
  out->put(out->self, 0xa0);
  out->put(out->self, 0x31);
  out->put(out->self, 0x83);
  out->put(out->self, 0xd3);
  out->put(out->self, 0x8c);
  out->put(out->self, 0xb2);
  out->put(out->self, 0x28);
  out->put(out->self, 0xb0);
  out->put(out->self, 0xd3);
}

void zq_c_start_block_bytes(zq_compressor *c, const zq_u8 *hcomp) {
  zq_reader r;
  zq_memr m;
  zq_writer *out = c->enc.out;
  m.p = hcomp;
  r.get = zq_memr_get;
  r.read = NULL;
  r.self = &m;
  if (c->state != ZQ_C_INIT) zq_error("startBlock in wrong state");
  zq_zpaql_read(&c->z, &r);
  c->pz.sha1 = &c->sha1;
  out->put(out->self, 'z');
  out->put(out->self, 'P');
  out->put(out->self, 'Q');
  out->put(out->self, 1 + (ZQ_HDR(&c->z, 6) == 0)); /* level 1 or 2 */
  out->put(out->self, 1);
  zq_zpaql_write(&c->z, out, 0);
  c->state = ZQ_C_BLOCK1;
}

void zq_c_start_block_level(zq_compressor *c, int level) {
  const zq_u8 *p = zq_models;
  int i;
  if (level < 1) zq_error("compression level must be at least 1");
  for (i = 1; i < level && zq_to_u16(p); ++i) p += zq_to_u16(p) + 2;
  if (zq_to_u16(p) < 1) zq_error("compression level too high");
  zq_c_start_block_bytes(c, p);
}

void zq_c_start_block(zq_compressor *c, const char *config, int *args,
                      zq_writer *pcomp_cmd) {
  zq_writer *out = c->enc.out;
  if (c->state != ZQ_C_INIT) zq_error("startBlock in wrong state");
  zq_compile(config, args, &c->z, &c->pz, pcomp_cmd);
  c->pz.sha1 = &c->sha1;
  out->put(out->self, 'z');
  out->put(out->self, 'P');
  out->put(out->self, 'Q');
  out->put(out->self, 1 + (ZQ_HDR(&c->z, 6) == 0)); /* level 1 or 2 */
  out->put(out->self, 1);
  zq_zpaql_write(&c->z, out, 0);
  c->state = ZQ_C_BLOCK1;
}

void zq_c_start_segment(zq_compressor *c, const char *filename,
                        const char *comment) {
  zq_writer *out = c->enc.out;
  if (c->state != ZQ_C_BLOCK1 && c->state != ZQ_C_BLOCK2)
    zq_error("startSegment in wrong state");
  out->put(out->self, 1);
  while (filename && *filename) out->put(out->self, (zq_u8)*filename++);
  out->put(out->self, 0);
  while (comment && *comment) out->put(out->self, (zq_u8)*comment++);
  out->put(out->self, 0);
  out->put(out->self, 0);
  if (c->state == ZQ_C_BLOCK1) c->state = ZQ_C_SEG1;
  if (c->state == ZQ_C_BLOCK2) c->state = ZQ_C_SEG2;
}

/* Write the PCOMP program into the first segment.  If pcomp is NULL it is
 * taken from pz.header; if len is 0 the length is read from pcomp[0..1]. */
void zq_c_post_process(zq_compressor *c, const char *pcomp, int len) {
  int i;
  if (c->state == ZQ_C_SEG2) return;
  if (c->state != ZQ_C_SEG1) zq_error("postProcess in wrong state");
  zq_enc_init_block(&c->enc);
  if (!pcomp) {
    len = c->pz.hend - c->pz.hbegin;
    if (len > 0) pcomp = (const char *)&ZQ_HDR(&c->pz, c->pz.hbegin);
  } else if (len == 0) {
    len = zq_to_u16((const zq_u8 *)pcomp);
    pcomp += 2;
  }
  if (len > 0) {
    zq_enc_compress(&c->enc, 1);
    zq_enc_compress(&c->enc, len & 255);
    zq_enc_compress(&c->enc, (len >> 8) & 255);
    for (i = 0; i < len; ++i) zq_enc_compress(&c->enc, pcomp[i] & 255);
    if (c->verify) zq_zpaql_initp(&c->pz);
  } else {
    zq_enc_compress(&c->enc, 0);
  }
  c->state = ZQ_C_SEG2;
}

/* Compress n bytes, or to EOF when n < 0.  Returns 0 at EOF. */
int zq_c_compress(zq_compressor *c, int n) {
  const int BUFSIZE = 1 << 14;
  char buf[1 << 14];
  if (c->state == ZQ_C_SEG1) zq_c_post_process(c, NULL, 0);
  if (c->state != ZQ_C_SEG2) zq_error("compress in wrong state");

  while (n) {
    int nbuf = BUFSIZE;
    int nr, i;
    if (n >= 0 && n < nbuf) nbuf = n;
    nr = c->in->read ? c->in->read(c->in->self, buf, nbuf)
                     : zq_reader_read(c->in, buf, nbuf);
    if (nr < 0 || nr > BUFSIZE || nr > nbuf) zq_error("invalid read size");
    if (nr <= 0) return 0;
    if (n >= 0) n -= nr;
    if (!zq_pred_is_modeled(&c->enc.pr)) {
      /* stored block: bulk copy, and SHA-1 in bulk when there is no PCOMP */
      if (c->verify) {
        if (c->pz.hend) {
          for (i = 0; i < nr; ++i)
            zq_zpaql_run(&c->pz, (zq_u32)(zq_u8)buf[i]);
        } else {
          zq_sha1_write(&c->sha1, buf, nr);
        }
      }
      zq_enc_compress_buf(&c->enc, buf, nr);
    } else {
      for (i = 0; i < nr; ++i) {
        const int ch = (zq_u8)buf[i];
        zq_enc_compress(&c->enc, ch);
        if (c->verify) {
          if (c->pz.hend) zq_zpaql_run(&c->pz, (zq_u32)ch);
          else zq_sha1_put(&c->sha1, ch);
        }
      }
    }
  }
  return 1;
}

static void zq_c_end_common(zq_compressor *c) {
  if (c->state == ZQ_C_SEG1) zq_c_post_process(c, NULL, 0);
  if (c->state != ZQ_C_SEG2) zq_error("endSegment in wrong state");
  zq_enc_compress(&c->enc, -1);
  if (c->verify && c->pz.hend) {
    zq_zpaql_run(&c->pz, (zq_u32)-1);
    zq_zpaql_flush(&c->pz);
  }
  {
    zq_writer *out = c->enc.out;
    int i;
    for (i = 0; i < 4; ++i) out->put(out->self, 0);
  }
}

void zq_c_end_segment(zq_compressor *c, const char *sha1string) {
  zq_writer *out = c->enc.out;
  int i;
  zq_c_end_common(c);
  if (sha1string) {
    out->put(out->self, 253);
    for (i = 0; i < 20; ++i) out->put(out->self, (zq_u8)sha1string[i]);
  } else {
    out->put(out->self, 254);
  }
  c->state = ZQ_C_BLOCK2;
}

char *zq_c_end_segment_checksum(zq_compressor *c, zq_i64 *size, int dosha1) {
  zq_writer *out = c->enc.out;
  int i;
  zq_c_end_common(c);
  if (c->verify) {
    if (size) *size = (zq_i64)zq_sha1_usize(&c->sha1);
    memcpy(c->sha1result, zq_sha1_result(&c->sha1), 20);
  }
  if (c->verify && dosha1) {
    out->put(out->self, 253);
    for (i = 0; i < 20; ++i) out->put(out->self, (zq_u8)c->sha1result[i]);
  } else {
    out->put(out->self, 254);
  }
  c->state = ZQ_C_BLOCK2;
  return c->verify ? c->sha1result : NULL;
}

void zq_c_end_block(zq_compressor *c) {
  if (c->state != ZQ_C_BLOCK2) zq_error("endBlock in wrong state");
  c->enc.out->put(c->enc.out->self, 255);
  c->state = ZQ_C_INIT;
}
