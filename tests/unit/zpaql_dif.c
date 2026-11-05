/* zpaqII - ZPAQL(II): the new interpreter loop against the one-step reference.
 *
 * zq_zpaql_execute() steps one instruction at a time with the state in the
 * struct; zq_zpaql_run_interp() runs a whole input byte with the state in
 * locals.  Both must be indistinguishable: registers, flags, pc, H, M, R and
 * the output stream are compared after every input byte.
 *
 * It also checks zq_zpaql_run_buf() (the batched entry, one call into the JIT
 * for many inputs) against the reference, at the end of every batch, on the
 * HCOMP and on the PCOMP of each config, with batches of random length.
 *
 * Usage: zpaql_dif [e2_manifest] [inputs-per-config]
 */
#include "zq_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  zq_zpaql z;
  zq_writer w;
  zq_buf *out;
} machine;

static void sink_put(void *self, int c) { zq_buf_put((zq_buf *)self, c); }
static void sink_write(void *self, const char *p, int n) {
  zq_buf_write((zq_buf *)self, p, n);
}

static void machine_open2(machine *m, const zq_zpaql *src, int pcomp) {
  memset(m, 0, sizeof(*m));
  zq_zpaql_init(&m->z);
  zq_arr_resize(&m->z.header, src->header.n, 1);
  memcpy(m->z.header.data, src->header.data, src->header.n);
  m->z.cend = src->cend;
  m->z.hbegin = src->hbegin;
  m->z.hend = src->hend;
  if (pcomp)
    zq_zpaql_init_sizes(&m->z, ZQ_HDR(&m->z, 4), ZQ_HDR(&m->z, 5));
  else
    zq_zpaql_init_sizes(&m->z, ZQ_HDR(&m->z, 2), ZQ_HDR(&m->z, 3));
  m->out = zq_buf_new(1 << 16);
  m->w.put = sink_put;
  m->w.write = sink_write;
  m->w.self = m->out;
  m->z.output = &m->w;
}

static void machine_open(machine *m, const zq_zpaql *src) {
  machine_open2(m, src, 0);
}

static void machine_close(machine *m) {
  m->z.output = NULL;
  zq_zpaql_free(&m->z);
  zq_buf_free(m->out);
}

static int same_mem = 1; /* compare H and M too (they can be 64 MB) */
static int same(const machine *x, const machine *y) {
  if (x->z.a != y->z.a || x->z.b != y->z.b || x->z.c != y->z.c ||
      x->z.d != y->z.d || x->z.f != y->z.f || x->z.pc != y->z.pc)
    return 0;
  if (x->z.h.n != y->z.h.n || x->z.m.n != y->z.m.n) return 0;
  if (same_mem && x->z.h.n && memcmp(x->z.h.data, y->z.h.data, x->z.h.n * 4)) return 0;
  if (same_mem && x->z.m.n && memcmp(x->z.m.data, y->z.m.data, x->z.m.n)) return 0;
  if (x->z.r.n && memcmp(x->z.r.data, y->z.r.data, x->z.r.n * 4)) return 0;
  if (zq_buf_size(x->out) != zq_buf_size(y->out)) return 0;
  if (zq_buf_size(x->out) &&
      memcmp(zq_buf_data(x->out), zq_buf_data(y->out), zq_buf_size(x->out)))
    return 0;
  return 1;
}

/* Returns 1 on a mismatch. */
static int compare(const char *what, int idx, const zq_zpaql *src,
                   const zq_u32 *in, int n) {
  machine ref, fast;
  int k, bad = 0;

  machine_open(&ref, src);
  machine_open(&fast, src);

  for (k = 0; k < n; ++k) {
    ref.z.pc = ref.z.hbegin;
    ref.z.a = in[k];
    while (zq_zpaql_execute(&ref.z)) {
    }
    zq_zpaql_run_interp(&fast.z, in[k]);

    zq_zpaql_flush(&ref.z);
    zq_zpaql_flush(&fast.z);
    if (!same(&ref, &fast)) {
      printf("MISMATCH %s %d at input %d\n", what, idx, k);
      printf("  ref  a=%u b=%u c=%u d=%u f=%d pc=%d\n", ref.z.a, ref.z.b,
             ref.z.c, ref.z.d, ref.z.f, ref.z.pc);
      printf("  fast a=%u b=%u c=%u d=%u f=%d pc=%d\n", fast.z.a, fast.z.b,
             fast.z.c, fast.z.d, fast.z.f, fast.z.pc);
      bad = 1;
      break;
    }
  }
  machine_close(&ref);
  machine_close(&fast);
  return bad;
}

/* Batched runs: the reference steps every byte, the batch machine gets the same
 * bytes through zq_zpaql_run_buf() in chunks of 1..300. */
static unsigned blcg = 13579u;
static int compare_batch(const char *what, int idx, const zq_zpaql *src,
                         int pcomp, const zq_u32 *in, int n) {
  machine ref, fast;
  zq_u8 bytes[512];
  int k = 0, bad = 0;

  if (src->hend <= src->hbegin) return 0;
  for (k = 0; k < n; ++k) bytes[k] = (zq_u8)in[k];
  machine_open2(&ref, src, pcomp);
  machine_open2(&fast, src, pcomp);
  k = 0;
  /* No EOF (-1) here: on random input the E8E9 pass at EOF can walk billions
   * of positions.  EOF is checked on real streams (fastpath tests). */
  while (k < n && !bad) {
    int len, j;
    blcg = blcg * 1103515245u + 12345u;
    len = 1 + (int)((blcg >> 8) % 300);
    if (getenv("ZPAQL_DIF_LEN") && atoi(getenv("ZPAQL_DIF_LEN")) > 0)
      len = atoi(getenv("ZPAQL_DIF_LEN"));
    if (len > n - k) len = n - k;
    for (j = 0; j < len; ++j) {
      ref.z.pc = ref.z.hbegin;
      ref.z.a = bytes[k + j];
      while (zq_zpaql_execute(&ref.z)) {
      }
    }
    zq_zpaql_run_buf(&fast.z, bytes + k, len);
    k += len;
    zq_zpaql_flush(&ref.z);
    zq_zpaql_flush(&fast.z);
    /* The JIT does not keep pc, and like 7.15's it does not store f when a
     * comparison only feeds the jt/jf right after it (f is dead there). */
    fast.z.pc = ref.z.pc;
    fast.z.f = ref.z.f;
    same_mem = (k >= n); /* H and M once, at the end */
    if (!same(&ref, &fast)) {
      printf("MISMATCH batch %s %s %d before input %d\n", what,
             pcomp ? "pcomp" : "hcomp", idx, k);
      bad = 1;
    }
  }
  same_mem = 1;
  machine_close(&ref);
  machine_close(&fast);
  return bad;
}

/* Compile a config text and compare both interpreters on it. */
static int run_config(int idx, const char *config, int *args, const zq_u32 *in,
                      int n, int pcomp_batch) {
  zq_zpaql hz, pz;
  int bad;

  zq_zpaql_init(&hz);
  zq_zpaql_init(&pz);
  zq_compile(config, args, &hz, &pz, NULL);
  bad = compare("config", idx, &hz, in, n);
  bad |= compare_batch("config", idx, &hz, 0, in, n);
  /* The inverse BWT follows a chain of pointers at EOF; on random input the
   * chain can be a cycle and the program never halts (in 7.15 as well), so
   * that one is checked on real streams instead (fastpath tests).  Some
   * manifest PCOMPs run ~10^10 instructions on random bytes (legitimately),
   * so only the PCOMPs of 7.15's own levels get random batches. */
  if (pcomp_batch && !strstr(config, "pcomp bwt"))
    bad |= compare_batch("config", idx, &pz, 1, in, n);
  zq_zpaql_free(&hz);
  zq_zpaql_free(&pz);
  return bad;
}

int main(int argc, char **argv) {
  zq_u32 in[512];
  int n = (argc > 2) ? atoi(argv[2]) : 200;
  int fails = 0, cases = 0, i, k;
  unsigned lcg = 987654321u;
  static const char *const methods[] = {
      "x1", "x2", "x3", "x4", "x5", "x6", "x7",
      "s4.1.5.0.3.22", "s0", "s4.3ci1",
      /* the postprocessors 7.15's own levels generate (PCOMP) */
      "x4,1,4,0,1,15", "x4,1,4,0,2,16", "x4,1,5,0,3,24", "x6,1,6,0,3,25",
      "x4,2,4,0,3,24", "x4,2,4,0,7,25,1", "x4,5,4,0,3,24", "x4,6,4,0,7,25,1",
      "x4,3ci1", "x4,7ci1", "x4,4",
  };

  if (n > (int)(sizeof(in) / sizeof(in[0]))) n = (int)(sizeof(in) / sizeof(in[0]));
  for (i = 0; i < n; ++i) {
    lcg = lcg * 1103515245u + 12345u;
    in[i] = (i < 128) ? (zq_u32)(i * 2) : (lcg >> 8);
  }

  for (i = 0; i < (int)(sizeof(methods) / sizeof(methods[0])); ++i) {
    int args[9] = {0};
    char *cfg = zq_make_config(methods[i], args);
    if (!cfg) continue;
    if (getenv("ZPAQL_DIF_VERBOSE")) {
      printf("%s\n", methods[i]);
      fflush(stdout);
    }
    fails += run_config(i, cfg, args, in, n, 1);
    free(cfg);
    ++cases;
  }

  if (argc > 1) {
    FILE *mf = fopen(argv[1], "r");
    if (mf) {
      char line[65536], config[65536];
      int args[9];
      config[0] = 0;
      while (fgets(line, (int)sizeof(line), mf)) {
        if (!strncmp(line, "CONFIG ", 7)) {
          const char *p = line + 7;
          char *q = config;
          while (*p && *p != '\n') {
            if (*p == '\\' && p[1] == 'n') {
              *q++ = '\n';
              p += 2;
            } else {
              *q++ = *p++;
            }
          }
          *q = 0;
        } else if (!strncmp(line, "ARGS", 4)) {
          const char *p = line + 4;
          for (k = 0; k < 9; ++k) args[k] = (int)strtol(p, (char **)&p, 10);
          if (getenv("ZPAQL_DIF_VERBOSE")) {
            printf("manifest %d\n", cases);
            fflush(stdout);
          }
          fails += run_config(cases, config, args, in, n, 0);
          ++cases;
        }
      }
      fclose(mf);
    }
  }

  printf("zpaql dif: %d configs, %d failures\n", cases, fails);
  return fails != 0;
}
