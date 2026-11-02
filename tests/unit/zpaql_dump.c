/* E2 check: reads the manifest produced by the oracle (ZPAQL configs +
 * args), compiles with the C core, runs the VM and emits the same results.
 *
 * Usage: zpaql_dump <manifest> <results>
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

static void hex(const unsigned char *p, size_t n, char *out) {
  static const char *d = "0123456789abcdef";
  size_t i;
  for (i = 0; i < n; ++i) {
    out[2 * i] = d[p[i] >> 4];
    out[2 * i + 1] = d[p[i] & 15];
  }
  out[2 * n] = 0;
}

static void sha1hex(const void *p, size_t n, char *out) {
  zq_sha1 s;
  zq_sha1_init(&s);
  zq_sha1_write(&s, (const char *)p, (zq_i64)n);
  hex((const unsigned char *)zq_sha1_result(&s), 20, out);
}

static unsigned lcg;
static unsigned nextrand(void) {
  lcg = lcg * 1103515245u + 12345u;
  return lcg >> 8;
}

/* Undoes the manifest escaping: \\ -> \ and \n -> newline. */
static void unescape(const char *in, char *out) {
  while (*in) {
    if (*in == '\\' && in[1] == 'n') {
      *out++ = '\n';
      in += 2;
    } else if (*in == '\\' && in[1] == '\\') {
      *out++ = '\\';
      in += 2;
    } else {
      *out++ = *in++;
    }
  }
  *out = 0;
}

int main(int argc, char **argv) {
  FILE *mf, *rf;
  char line[70000];
  char *config = NULL;
  unsigned char *cfgtext = NULL;
  size_t cfgcap = 0;
  int caseid = -1, hp = 0;
  int args[9];
  FILE *out;

  if (argc < 3) {
    fprintf(stderr, "uso: zpaql_dump <manifest> <resultados>\n");
    return 2;
  }
  mf = fopen(argv[1], "r");
  rf = fopen(argv[2], "w");
  if (!mf || !rf) {
    fprintf(stderr, "cannot open files\n");
    return 2;
  }
  cfgcap = 70000;
  cfgtext = (unsigned char *)malloc(cfgcap);
  config = (char *)malloc(cfgcap);

  while (fgets(line, sizeof(line), mf)) {
    size_t len = strlen(line);
    while (len && (line[len - 1] == '\n' || line[len - 1] == '\r'))
      line[--len] = 0;
    if (!strncmp(line, "CASE ", 5)) {
      sscanf(line + 5, "%d %d", &caseid, &hp);
      continue;
    }
    if (!strncmp(line, "CONFIG ", 7)) {
      unescape(line + 7, config);
      continue;
    }
    if (!strncmp(line, "ARGS", 4)) {
      const char *p = line + 4;
      int i;
      for (i = 0; i < 9; ++i) args[i] = (int)strtol(p, (char **)&p, 10);
      /* full case: compile and run */
      {
        zq_zpaql hz, pz;
        zq_buf *bch = zq_buf_new(0), *bcp = zq_buf_new(0),
               *pcmd = zq_buf_new(0), *pout = zq_buf_new(0);
        zq_writer w1, w2, w3, w4;
        sink s1, s2, s3, s4;
        int has;

        zq_zpaql_init(&hz);
        zq_zpaql_init(&pz);

        s1.b = bch; w1.put = sink_put; w1.write = sink_write; w1.self = &s1;
        s2.b = bcp; w2.put = sink_put; w2.write = sink_write; w2.self = &s2;
        s3.b = pcmd; w3.put = sink_put; w3.write = sink_write; w3.self = &s3;
        s4.b = pout; w4.put = sink_put; w4.write = sink_write; w4.self = &s4;

        zq_compile(config, args, &hz, &pz, &w3);

        fprintf(rf, "RESULT %d\n", caseid);

        has = zq_zpaql_write(&hz, &w1, 0);
        fprintf(rf, "BCH %d ", has ? 1 : 0);
        {
          char *h = (char *)malloc(2 * zq_buf_size(bch) + 1);
          hex((const unsigned char *)zq_buf_data(bch), zq_buf_size(bch), h);
          fprintf(rf, "%s\n", h);
          free(h);
        }
        fprintf(rf, "BCHSIZE %d %d %d %zu\n", hz.cend, hz.hbegin, hz.hend,
                hz.header.n);

        has = zq_zpaql_write(&pz, &w2, 1);
        fprintf(rf, "BCP %d ", has ? 1 : 0);
        {
          char *h = (char *)malloc(2 * zq_buf_size(bcp) + 1);
          hex((const unsigned char *)zq_buf_data(bcp), zq_buf_size(bcp), h);
          fprintf(rf, "%s\n", h);
          free(h);
        }
        {
          char *h = (char *)malloc(2 * zq_buf_size(pcmd) + 1);
          hex((const unsigned char *)zq_buf_data(pcmd), zq_buf_size(pcmd), h);
          fprintf(rf, "PCMD %s\n", h);
          free(h);
        }

        lcg = 123456789u;
        if (hz.hend > hz.hbegin) {
          int k;
          zq_zpaql_inith(&hz);
          for (k = 0; k < 200; ++k) {
            unsigned v = nextrand();
            zq_zpaql_run(&hz, v);
            if (k < 8 || k % 50 == 49) {
              zq_sha1 s;
              int nb = 1 << ZQ_HDR(&hz, 2);
              int j;
              char hx[64];
              if (nb > 256) nb = 256;
              zq_sha1_init(&s);
              for (j = 0; j < nb; ++j) {
                unsigned x = ZQ_H(&hz, j);
                char b[4];
                b[0] = (char)(x >> 24);
                b[1] = (char)(x >> 16);
                b[2] = (char)(x >> 8);
                b[3] = (char)x;
                zq_sha1_write(&s, b, 4);
              }
              hex((const unsigned char *)zq_sha1_result(&s), 20, hx);
              fprintf(rf, "H %d %s\n", k + 1, hx);
            }
          }
        }

        lcg = 987654321u;
        if ((hp & 2) && pz.hend > pz.hbegin) {
          int k;
          char hx[64];
          pz.output = &w4;
          zq_zpaql_initp(&pz);
          for (k = 0; k < 400; ++k)
            zq_zpaql_run(&pz, (zq_u32)(nextrand() & 255));
          zq_zpaql_run(&pz, (zq_u32)-1);
          zq_zpaql_flush(&pz);
          sha1hex(zq_buf_data(pout), zq_buf_size(pout), hx);
          fprintf(rf, "POUT %zu %s\n", zq_buf_size(pout), hx);
        }

        zq_zpaql_free(&hz);
        zq_zpaql_free(&pz);
        zq_buf_free(bch);
        zq_buf_free(bcp);
        zq_buf_free(pcmd);
        zq_buf_free(pout);
      }
      continue;
    }
  }
  out = rf;
  fclose(mf);
  if (out) fclose(out);
  free(config);
  free(cfgtext);
  return 0;
}
