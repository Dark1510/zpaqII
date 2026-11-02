/* E4 extraction check: walks a .zpaq archive with the port's Decompresser and
 * writes all segment data to one output file, printing one line per segment
 * (filename, bytes, SHA-1 status).
 *
 * Usage: extract <archive.zpaq> <output>
 */
#include "zq.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  FILE *f;
} file_src;

static int src_get(void *self) { return getc(((file_src *)self)->f); }
static int src_read(void *self, char *buf, int n) {
  return (int)fread(buf, 1, (size_t)n, ((file_src *)self)->f);
}

typedef struct {
  FILE *f;
  long long total;
} file_dst;

static void dst_put(void *self, int c) {
  fputc(c, ((file_dst *)self)->f);
  ((file_dst *)self)->total++;
}
static void dst_write(void *self, const char *p, int n) {
  fwrite(p, 1, (size_t)n, ((file_dst *)self)->f);
  ((file_dst *)self)->total += n;
}

typedef struct {
  char s[4096];
  int n;
} namebuf;

static void name_put(void *self, int c) {
  namebuf *b = (namebuf *)self;
  if (b->n < (int)sizeof(b->s) - 1) b->s[b->n++] = (char)c;
  b->s[b->n] = 0;
}

int main(int argc, char **argv) {
  FILE *in, *out;
  file_src src;
  file_dst dst;
  zq_reader rd;
  zq_writer wr;
  zq_decompresser *d;
  zq_sha1 sha;
  int blocks = 0, segs = 0, badsha = 0;

  if (argc < 3) {
    fprintf(stderr, "usage: extract <archive.zpaq> <output>\n");
    return 2;
  }
  in = fopen(argv[1], "rb");
  out = fopen(argv[2], "wb");
  if (!in || !out) {
    fprintf(stderr, "cannot open files\n");
    return 2;
  }

  src.f = in;
  dst.f = out;
  dst.total = 0;
  rd.get = src_get;
  rd.read = src_read;
  rd.self = &src;
  wr.put = dst_put;
  wr.write = dst_write;
  wr.self = &dst;

  d = zq_d_new();
  zq_d_set_input(d, &rd);
  zq_d_set_output(d, &wr);
  zq_sha1_init(&sha);
  zq_d_set_sha1(d, &sha);

  while (zq_d_find_block(d, NULL)) {
    ++blocks;
    for (;;) {
      namebuf nb;
      char sha1str[21];
      long long before = dst.total;
      nb.s[0] = 0;
      nb.n = 0;
      if (!zq_d_find_filename(d, &(zq_writer){name_put, NULL, &nb})) break;
      zq_d_read_comment(d, NULL);
      zq_sha1_init(&sha);
      zq_d_decompress(d, -1);
      zq_d_read_segment_end(d, sha1str);
      ++segs;
      if (sha1str[0] == 1) {
        char h[20];
        memcpy(h, zq_sha1_result(&sha), 20);
        if (memcmp(h, sha1str + 1, 20) != 0) {
          fprintf(stderr, "SHA-1 mismatch in %s\n", nb.s);
          ++badsha;
        }
      }
      printf("seg %d %s %lld sha=%d\n", segs, nb.s, dst.total - before,
             sha1str[0] == 1);
    }
  }

  fclose(in);
  fclose(out);
  zq_d_free(d);
  printf("blocks %d segments %d bytes %lld badsha %d\n", blocks, segs,
         dst.total, badsha);
  return badsha ? 1 : 0;
}
