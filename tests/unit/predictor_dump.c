/* E3 check: predictor trace, encoder output and decoder round trip using the
 * port's C core, against the manifest produced by tools/oracle/e3_dump.cpp.
 *
 * Usage: predictor_dump <manifest> <results>
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

static unsigned lcg = 1;
static unsigned nextrand(void) {
  lcg = lcg * 1103515245u + 12345u;
  return lcg >> 8;
}

static void make_data(unsigned char *b, int n) {
  int i;
  for (i = 0; i < n; ++i) {
    if (i >= 64 && (i % 64) >= 32) b[i] = b[i - 32];
    else b[i] = (unsigned char)(nextrand() & 255);
  }
}

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
  char *config;
  int caseid = -1;
  int args[9];
  static unsigned char data[512];
  char hexbuf[20000];

  if (argc < 3) {
    fprintf(stderr, "usage: predictor_dump <manifest> <results>\n");
    return 2;
  }
  mf = fopen(argv[1], "r");
  rf = fopen(argv[2], "w");
  if (!mf || !rf) {
    fprintf(stderr, "cannot open files\n");
    return 2;
  }
  config = (char *)malloc(70000);

  lcg = 12345u;
  make_data(data, (int)sizeof(data));

  while (fgets(line, sizeof(line), mf)) {
    size_t len = strlen(line);
    while (len && (line[len - 1] == '\n' || line[len - 1] == '\r'))
      line[--len] = 0;
    if (!strncmp(line, "CASE ", 5)) {
      sscanf(line + 5, "%d", &caseid);
      continue;
    }
    if (!strncmp(line, "CONFIG ", 7)) {
      unescape(line + 7, config);
      continue;
    }
    if (!strncmp(line, "ARGS", 4)) {
      const char *p = line + 4;
      int i;
      zq_zpaql hz, pz;
      zq_buf *pcmd = zq_buf_new(0), *encbuf = zq_buf_new(0);
      zq_writer wcmd, wenc;
      sink scmd, senc;
      for (i = 0; i < 9; ++i) args[i] = (int)strtol(p, (char **)&p, 10);

      zq_zpaql_init(&hz);
      zq_zpaql_init(&pz);
      scmd.b = pcmd; wcmd.put = sink_put; wcmd.write = sink_write; wcmd.self = &scmd;
      senc.b = encbuf; wenc.put = sink_put; wenc.write = sink_write; wenc.self = &senc;

      zq_compile(config, args, &hz, &pz, &wcmd);
      fprintf(rf, "RESULT %d\n", caseid);

      /* Predictor trace (only when the config has model components) */
      if (ZQ_HDR(&hz, 6) > 0) {
        zq_predictor pr;
        int k, j;
        zq_pred_open(&pr, &hz);
        zq_pred_init(&pr);
        for (k = 0; k < (int)sizeof(data); ++k) {
          const int c = data[k];
          fprintf(rf, "P %d", k);
          for (j = 0; j < 8; ++j) fprintf(rf, " %d", zq_pred_predict(&pr));
          fprintf(rf, "\n");
          for (j = 7; j >= 0; --j) zq_pred_update(&pr, (c >> j) & 1);
        }
        zq_pred_free(&pr);
      }

      /* Encoder */
      {
        zq_encoder e;
        int k;
        zq_enc_open(&e, &hz);
        e.out = &wenc;
        zq_enc_init_block(&e);
        for (k = 0; k < (int)sizeof(data); ++k) zq_enc_compress(&e, data[k]);
        zq_enc_compress(&e, -1);
        for (k = 0; k < 4; ++k) zq_buf_put(encbuf, 0);
        hex((const unsigned char *)zq_buf_data(encbuf), zq_buf_size(encbuf),
            hexbuf);
        fprintf(rf, "ENC %zu %s\n", zq_buf_size(encbuf), hexbuf);
        zq_enc_close(&e);
      }

      /* Decoder round trip */
      {
        zq_decoder d;
        zq_reader r;
        zq_buf *out = zq_buf_new(0);
        int c, count = 0;
        zq_buf_seek_read(encbuf, 0);
        r = zq_buf_reader(encbuf);
        zq_dec_open(&d, &hz);
        d.in = &r;
        zq_dec_init_block(&d);
        while ((c = zq_dec_decompress(&d)) >= 0) {
          zq_buf_put(out, c);
          ++count;
        }
        sha1hex(zq_buf_data(out), zq_buf_size(out), hexbuf);
        fprintf(rf, "DEC %d %s\n", count, hexbuf);
        zq_dec_close(&d);
        zq_buf_free(out);
      }

      zq_zpaql_free(&hz);
      zq_zpaql_free(&pz);
      zq_buf_free(pcmd);
      zq_buf_free(encbuf);
      continue;
    }
  }

  fclose(mf);
  fclose(rf);
  free(config);
  return 0;
}
