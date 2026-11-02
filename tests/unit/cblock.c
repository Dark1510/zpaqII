/* E4b check: compress one block with the port's compressor.
 * Usage: cblock <in> <out> <method>
 */
#include "zq.h"

#include <stdio.h>
#include <stdlib.h>

static int src_get(void *self) { return getc((FILE *)self); }
static int src_read(void *self, char *buf, int n) {
  return (int)fread(buf, 1, (size_t)n, (FILE *)self);
}
static void dst_put(void *self, int c) { fputc(c, (FILE *)self); }
static void dst_write(void *self, const char *p, int n) {
  fwrite(p, 1, (size_t)n, (FILE *)self);
}

int main(int argc, char **argv) {
  FILE *in, *out;
  zq_reader r;
  zq_writer w;
  zq_buf *b;
  zq_err_frame ef;
  int rc = 0;

  if (argc < 4) {
    fprintf(stderr, "usage: cblock <in> <out> <method>\n");
    return 2;
  }
  in = fopen(argv[1], "rb");
  out = fopen(argv[2], "wb");
  if (!in || !out) {
    fprintf(stderr, "cannot open files\n");
    return 2;
  }
  b = zq_buf_new(1 << 16);
  r.get = src_get;
  r.read = src_read;
  r.self = in;
  for (;;) {
    char tmp[1 << 16];
    int nr = zq_reader_read(&r, tmp, (int)sizeof(tmp));
    if (nr <= 0) break;
    zq_buf_write(b, tmp, nr);
  }
  fclose(in);

  w.put = dst_put;
  w.write = dst_write;
  w.self = out;

  if (setjmp(ef.jb)) {
    fprintf(stderr, "error: %s\n", zq_last_error());
    rc = 1;
  } else {
    zq_err_push(&ef);
    zq_compress_block(b, &w, argv[3], "data.bin", NULL, 1);
    zq_err_pop(&ef);
  }
  fclose(out);
  zq_buf_free(b);
  return rc;
}
