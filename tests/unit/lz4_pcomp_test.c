/* zpaqII - the LZ4 block decoder written in ZPAQL, run by the real
 * postprocessor.  Each case: LZ4-compress a buffer with our encoder, feed the
 * block through the PCOMP (the same PROG framing a zpaq block carries), and
 * check the output is the original bytes.
 */
#include "zq_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  zq_buf *b;
} sink;

static void sink_put(void *self, int c) { zq_buf_put(((sink *)self)->b, c); }
static void sink_write(void *self, const char *p, int n) {
  zq_buf_write(((sink *)self)->b, p, n);
}

/* Compile the decoder once: returns the PCOMP bytes in pcomp (caller frees). */
static unsigned char *build_pcomp(int *len) {
  char *cfg;
  zq_zpaql hz, pz;
  zq_writer w;
  int args[9] = {0};
  unsigned char *out;

  const char *prog = zq_lz4_pcomp_program();
  cfg = (char *)malloc(strlen(prog) + 64);
  strcpy(cfg, "comp 0 0 0 16 0\nhcomp\nhalt\n");
  strcat(cfg, prog);

  zq_zpaql_init(&hz);
  zq_zpaql_init(&pz);
  zq_compile(cfg, args, &hz, &pz, NULL);

  *len = pz.hend - pz.hbegin;
  if (getenv("LZ4_DUMP")) {
    FILE *f = fopen(getenv("LZ4_DUMP"), "wb");
    if (f) {
      fwrite(pz.header.data + pz.hbegin, 1, (size_t)*len, f);
      fclose(f);
    }
  }
  out = (unsigned char *)malloc(*len > 0 ? *len : 1);
  memcpy(out, pz.header.data + pz.hbegin, *len);
  zq_zpaql_free(&hz);
  zq_zpaql_free(&pz);
  free(cfg);
  return out;
}

/* Runs one LZ4 block through the PCOMP.  Returns 1 if the output matches. */
static int roundtrip(const unsigned char *pcomp, int plen,
                     const unsigned char *in, size_t n, const char *what) {
  unsigned char *blk = (unsigned char *)malloc(n * 2 + 64);
  int bn = zq_lz4_compress(in, n, blk, n * 2 + 64);
  zq_buf *got = zq_buf_new(0);
  zq_postproc pp;
  zq_writer w;
  sink s;
  size_t i;
  int ok = 1;

  if (bn < 0) {
    printf("FAIL %s: encoder\n", what);
    free(blk);
    return 0;
  }

  memset(&pp, 0, sizeof(pp));
  zq_zpaql_init(&pp.z);
  zq_pp_open(&pp, 0, 16);
  s.b = got;
  w.put = sink_put;
  w.write = sink_write;
  w.self = &s;
  zq_pp_set_output(&pp, &w);

  zq_pp_write(&pp, 1); /* PROG */
  zq_pp_write(&pp, plen & 255);
  zq_pp_write(&pp, plen >> 8);
  for (i = 0; i < (size_t)plen; ++i) zq_pp_write(&pp, pcomp[i]);
  for (i = 0; i < (size_t)bn; ++i) zq_pp_write(&pp, blk[i]);
  zq_pp_write(&pp, -1);

  if (zq_buf_size(got) != n || memcmp(zq_buf_data(got), in, n)) {
    printf("FAIL %s: %zu bytes in, %zu out (block %d)\n", what, n,
           zq_buf_size(got), bn);
    ok = 0;
  }
  zq_zpaql_free(&pp.z);
  zq_buf_free(got);
  free(blk);
  return ok;
}

int main(void) {
  unsigned char *pcomp;
  int plen = 0, fails = 0, cases = 0;
  static unsigned char text[1 << 22];
  static unsigned char rnd[1 << 22];
  size_t i;
  unsigned lcg = 42;

  pcomp = build_pcomp(&plen);
  printf("pcomp: %d bytes\n", plen);

  for (i = 0; i < sizeof(rnd); ++i) {
    lcg = lcg * 1103515245u + 12345u;
    rnd[i] = (unsigned char)(lcg >> 16);
  }
  for (i = 0; i < sizeof(text); ++i) text[i] = "abcabcabd "[i % 10];

  {
    static const size_t sizes[] = {1, 4, 11, 12, 13, 15, 16, 17, 255, 300,
                                   270, 4096, 65535, 65536, 65836,
                                   1 << 20, (1 << 20) + 70000};
    size_t k;
    for (k = 0; k < sizeof(sizes) / sizeof(sizes[0]); ++k) {
      char name[64];
      snprintf(name, sizeof(name), "text %zu", sizes[k]);
      fails += !roundtrip(pcomp, plen, text, sizes[k], name);
      snprintf(name, sizeof(name), "random %zu", sizes[k]);
      fails += !roundtrip(pcomp, plen, rnd, sizes[k], name);
      cases += 2;
    }
  }

  printf("lz4 pcomp: %d cases, %d failures\n", cases, fails);
  free(pcomp);
  return fails != 0;
}
