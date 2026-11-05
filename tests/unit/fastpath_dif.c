/* zpaqII - ZPAQL(II) native fast paths (src/core/fastpath.c) against the
 * one-step reference interpreter.
 *
 * 1. Recognition: every parameter variant of the PCOMPs 7.15's methods
 *    generate is recognised, and a program that differs in one byte is not.
 * 2. Random input: native vs zq_zpaql_execute(), random bytes in batches of
 *    random length, comparing a, b, c, d, R, the output and (at the end) M
 *    and H, then the end-of-segment input (-1).  Inputs on which the
 *    reference needs more than a budget of instructions (garbage can ask the
 *    LZ77 programs for copies of up to 2^32 bytes) are skipped and counted.
 * 3. Real streams: blocks compressed by the core with 7.15's method strings,
 *    text and x86 code (for E8E9), extracted with and without the fast path;
 *    both must give back the original bytes.
 *
 * Usage: fastpath_dif [random-inputs-per-program]
 */
#define _POSIX_C_SOURCE 200112L /* setenv */
#include "zq_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails, cases, skipped;

static void check(int ok, const char *what, const char *m) {
  ++cases;
  if (!ok) {
    ++fails;
    printf("FAIL %s %s\n", what, m);
  }
}

/* ---- machines ---------------------------------------------------- */

typedef struct {
  zq_zpaql z;
  zq_writer w;
  zq_buf *out;
} machine;

static void sink_put(void *self, int c) { zq_buf_put((zq_buf *)self, c); }
static void sink_write(void *self, const char *p, int n) {
  zq_buf_write((zq_buf *)self, p, n);
}

/* Compile method's PCOMP into a ready machine (zq_zpaql_initp detects). */
static int machine_open(machine *m, const char *method, int fast) {
  int args[9] = {0};
  zq_zpaql hz;
  char *cfg = zq_make_config(method, args);
  memset(m, 0, sizeof(*m));
  if (!cfg) return 0;
  zq_zpaql_init(&hz);
  zq_zpaql_init(&m->z);
  zq_compile(cfg, args, &hz, &m->z, NULL);
  free(cfg);
  zq_zpaql_free(&hz);
  if (m->z.hend <= m->z.hbegin) return 0;
  if (!fast) setenv("ZPAQII_NOFASTPATH", "1", 1);
  zq_zpaql_initp(&m->z);
  unsetenv("ZPAQII_NOFASTPATH");
  m->out = zq_buf_new(1 << 16);
  m->w.put = sink_put;
  m->w.write = sink_write;
  m->w.self = m->out;
  m->z.output = &m->w;
  return 1;
}

static void machine_close(machine *m) {
  m->z.output = NULL;
  zq_zpaql_free(&m->z);
  if (m->out) zq_buf_free(m->out);
}

static int same(const machine *x, const machine *y, int mem) {
  if (x->z.a != y->z.a || x->z.b != y->z.b || x->z.c != y->z.c ||
      x->z.d != y->z.d)
    return 0;
  if (memcmp(x->z.r.data, y->z.r.data, x->z.r.n * 4)) return 0;
  if (mem && memcmp(x->z.m.data, y->z.m.data, x->z.m.n)) return 0;
  if (mem && memcmp(x->z.h.data, y->z.h.data, x->z.h.n * 4)) return 0;
  if (zq_buf_size(x->out) != zq_buf_size(y->out)) return 0;
  return memcmp(zq_buf_data(x->out), zq_buf_data(y->out),
                zq_buf_size(x->out)) == 0;
}

/* One input through the reference, at most budget instructions.
 * Returns 0 if the budget ran out (the machine is then unusable). */
static int ref_run(machine *m, zq_u32 input, long budget) {
  m->z.pc = m->z.hbegin;
  m->z.a = input;
  while (zq_zpaql_execute(&m->z))
    if (--budget <= 0) return 0;
  return 1;
}

/* ---- 1. recognition ---------------------------------------------- */

static const char *const families[] = {
    /* lazy2, rb = args[0] - 4 */
    "x4,1", "x5,1", "x6,1", "x7,1", "x8,1", "x9,1", "x10,1", "x11,1", "x12,1",
    "x4,5", "x5,5", "x8,5", "x12,5",
    /* lzpre, $3 = args[2] */
    "x4,2,0", "x4,2,1", "x4,2,3", "x4,2,4", "x4,2,12", "x4,2,64", "x4,2,255",
    "x4,6,0", "x4,6,4", "x4,6,255",
    /* e8e9 */
    "x4,4",
    /* bwtrle (recognised; not run natively yet) */
    "x4,3", "x5,3", "x4,7", "x5,7",
};

static void test_recognition(void) {
  size_t i;
  for (i = 0; i < sizeof(families) / sizeof(families[0]); ++i) {
    machine m;
    char what[64];
    snprintf(what, sizeof(what), "recognise %s", families[i]);
    if (!machine_open(&m, families[i], 1)) {
      check(0, what, "(no PCOMP)");
      continue;
    }
    check(m.z.fast != 0, what, "");
    /* one changed byte in the middle: no longer recognised */
    {
      zq_u8 *p = (zq_u8 *)m.z.header.data + m.z.hbegin +
                 (m.z.hend - m.z.hbegin) / 2;
      *p ^= 0x40;
      zq_fast_detect(&m.z);
      snprintf(what, sizeof(what), "reject altered %s", families[i]);
      check(m.z.fast == 0, what, "");
    }
    machine_close(&m);
  }
}

/* ---- 2. random input --------------------------------------------- */

static unsigned lcg = 2463534242u;
static unsigned rnd(void) {
  lcg = lcg * 1103515245u + 12345u;
  return lcg >> 8;
}

static void test_random(const char *method, int n) {
  machine ref, fast;
  zq_u8 *in = (zq_u8 *)malloc((size_t)n);
  int k = 0, i, bad = 0, skip = 0;
  const long budget = 50000000;
  char what[64];
  snprintf(what, sizeof(what), "random %s", method);
  if (!machine_open(&ref, method, 0) || !machine_open(&fast, method, 1)) {
    check(0, what, "(open)");
    free(in);
    return;
  }
  if (!fast.z.fast) {
    check(0, what, "(not recognised)");
    goto done;
  }
  for (i = 0; i < n; ++i) in[i] = (zq_u8)rnd();
  while (k < n && !bad && !skip) {
    int len = 1 + (int)(rnd() % 700), j;
    if (len > n - k) len = n - k;
    for (j = 0; j < len && !skip; ++j) skip = !ref_run(&ref, in[k + j], budget);
    if (skip) break;
    zq_zpaql_run_buf(&fast.z, in + k, len);
    k += len;
    zq_zpaql_flush(&ref.z);
    zq_zpaql_flush(&fast.z);
    if (!same(&ref, &fast, k == n)) bad = 1;
  }
  if (!bad && !skip) { /* end of segment */
    if (ref_run(&ref, (zq_u32)-1, budget)) {
      zq_zpaql_run(&fast.z, (zq_u32)-1);
      zq_zpaql_flush(&ref.z);
      zq_zpaql_flush(&fast.z);
      if (!same(&ref, &fast, 1)) bad = 2;
    } else {
      skip = 1;
    }
  }
  if (skip) {
    ++skipped;
  } else {
    char where[64];
    snprintf(where, sizeof(where), "%s at input %d", bad == 2 ? "EOF" : "", k);
    check(!bad, what, where);
  }
done:
  machine_close(&ref);
  machine_close(&fast);
  free(in);
}

/* ---- 3. real streams --------------------------------------------- */

static zq_buf *extract(zq_buf *arc, int fast) {
  zq_decompresser *d = zq_d_new();
  zq_buf *out = zq_buf_new(1 << 20);
  zq_reader rd;
  zq_writer wr = zq_buf_writer(out);
  zq_buf_seek_read(arc, 0);
  rd = zq_buf_reader(arc);
  if (!fast) setenv("ZPAQII_NOFASTPATH", "1", 1);
  zq_d_set_input(d, &rd);
  zq_d_set_output(d, &wr);
  for (;;) {
    int r = zq_d_find_block(d, NULL);
    if (r <= 0) break;
    while ((r = zq_d_find_filename(d, NULL)) > 0) {
      if (zq_d_read_comment(d, NULL) < 0 || zq_d_decompress(d, -1) < 0 ||
          zq_d_read_segment_end(d, NULL) < 0) {
        printf("  extract error: %s\n", zq_last_error());
        zq_buf_free(out);
        out = NULL;
        break;
      }
    }
    if (!out || r < 0) break;
  }
  unsetenv("ZPAQII_NOFASTPATH");
  zq_d_free(d);
  return out;
}

static void test_stream(const char *method, const char *name, const zq_u8 *data,
                        int n) {
  zq_buf *in = zq_buf_new((size_t)n + 1);
  zq_buf *arc = zq_buf_new(1 << 20);
  zq_writer aw = zq_buf_writer(arc);
  zq_buf *o1, *o2;
  int before;
  char what[96];
  snprintf(what, sizeof(what), "stream %s %s", method, name);
  zq_buf_write(in, (const char *)data, n);
  if (zq_compress_block(in, &aw, method, "f", NULL, 1) != 0) {
    check(0, what, zq_last_error());
    zq_buf_free(in);
    zq_buf_free(arc);
    return;
  }
  o1 = extract(arc, 0);
  before = zq_fast_detected;
  o2 = extract(arc, 1);
  check(o1 && (int)zq_buf_size(o1) == n &&
            memcmp(zq_buf_data(o1), data, (size_t)n) == 0,
        what, "(VM)");
  check(o2 && (int)zq_buf_size(o2) == n &&
            memcmp(zq_buf_data(o2), data, (size_t)n) == 0,
        what, "(native)");
  /* the stored-block levels may have no PCOMP; the others must be native */
  if (strncmp(method, "x4,0", 4) != 0)
    check(zq_fast_detected > before, what, "(fast path not used)");
  if (o1) zq_buf_free(o1);
  if (o2) zq_buf_free(o2);
  zq_buf_free(in);
  zq_buf_free(arc);
}

static zq_u8 *load(const char *path, int *n) {
  FILE *f = fopen(path, "rb");
  zq_u8 *p;
  long len;
  if (!f) return NULL;
  fseek(f, 0, SEEK_END);
  len = ftell(f);
  if (len > (4 << 20)) len = 4 << 20;
  fseek(f, 0, SEEK_SET);
  p = (zq_u8 *)malloc((size_t)len + 1);
  *n = (int)fread(p, 1, (size_t)len, f);
  fclose(f);
  return p;
}

int main(int argc, char **argv) {
  const int nrand = (argc > 1) ? atoi(argv[1]) : 3000;
  static const char *const rand_methods[] = {
      "x4,1", "x6,1", "x9,1", "x12,1", "x4,5", "x8,5",
      "x4,2,1", "x4,2,4", "x4,2,64", "x4,6,4", "x4,6,1", "x4,4",
  };
  static const char *const stream_methods[] = {
      "x4,1,4,0,1,15", "x4,1,4,0,2,16", "x4,1,4,0,3,24", "x4,1,5,0,3,24",
      "x6,1,6,0,3,25", "x8,1,4,0,3,27", "x4,5,4,0,3,24", "x6,5,4,0,3,25",
      "x4,2,4,0,3,24", "x4,2,4,0,7,25,1", "x4,6,4,0,3,24", "x4,6,4,0,7,25,1",
      "x4,2,12,0,7,25,1c0,0,511i2", "x4,4",
  };
  zq_u8 *text, *code;
  int ntext, ncode = 0;
  size_t i;
  int r;

  test_recognition();

  for (r = 0; r < 3; ++r)
    for (i = 0; i < sizeof(rand_methods) / sizeof(rand_methods[0]); ++i)
      test_random(rand_methods[i], nrand);

  /* text: words with repeats; code: this executable (E8/E9 calls) */
  ntext = 1 << 20;
  text = (zq_u8 *)malloc((size_t)ntext);
  {
    static const char *const w[] = {"the ", "quick ", "brown ", "fox ",
                                    "jumps ", "over ", "lazy ", "dog\n",
                                    "zpaq ", "ZPAQL ", "0123 ", "\xe8\x00"};
    int p = 0;
    while (p < ntext) {
      const char *s = w[rnd() % 12];
      while (*s && p < ntext) text[p++] = (zq_u8)*s++;
    }
  }
  code = load("/proc/self/exe", &ncode);
  for (i = 0; i < sizeof(stream_methods) / sizeof(stream_methods[0]); ++i) {
    test_stream(stream_methods[i], "text", text, ntext);
    if (code) test_stream(stream_methods[i], "x86", code, ncode);
  }
  free(text);
  free(code);

  printf("fastpath: %d cases, %d failures, %d random runs skipped (budget)\n",
         cases, fails, skipped);
  return fails != 0;
}
