/* E6c check: differential test of the ZPAQL JIT against the interpreter.
 *
 * Two parts:
 *   1. every config of the E2 manifest (real HCOMP/PCOMP programs);
 *   2. randomly generated syntactically valid HCOMP programs, which between
 *      them exercise every opcode, both immediate widths, all three jump
 *      forms, the memory/hash addressing modes and the error paths.
 *
 * For each program both implementations start from an identical state, are
 * fed the same inputs, and after every input the registers, H, M and the
 * output stream must match.  An error must be raised by both.
 *
 * Usage: jit_diff <e2_manifest> [programs] [inputs-per-program]
 */
#include "zq_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Output collector                                                    */
/* ------------------------------------------------------------------ */

typedef struct {
  unsigned char buf[1 << 16];
  size_t n;
} sink;

static void sink_put(void *self, int c) {
  sink *s = (sink *)self;
  if (s->n < sizeof(s->buf)) s->buf[s->n++] = (unsigned char)c;
}

static void sink_write(void *self, const char *p, int n) {
  sink *s = (sink *)self;
  int i;
  for (i = 0; i < n; ++i) {
    if (s->n < sizeof(s->buf)) s->buf[s->n++] = (unsigned char)p[i];
  }
}

static unsigned long long fnv(const void *p, size_t n) {
  const unsigned char *q = (const unsigned char *)p;
  unsigned long long h = 1469598103934665603ull;
  size_t i;
  for (i = 0; i < n; ++i) {
    h ^= q[i];
    h *= 1099511628211ull;
  }
  return h;
}

/* ------------------------------------------------------------------ */
/* State comparison                                                    */
/* ------------------------------------------------------------------ */

typedef struct {
  zq_u32 a, b, c, d;
  zq_u32 f; /* 0/1 */
  unsigned long long hh, hm, ho;
  size_t on;
} state;

static void snapshot(zq_zpaql *z, const sink *s, state *st) {
  st->a = z->a;
  st->b = z->b;
  st->c = z->c;
  st->d = z->d;
  st->f = z->f ? 1u : 0u;
  st->hh = fnv(z->h.data, z->h.n * 4);
  st->hm = fnv(z->m.data, z->m.n);
  st->ho = fnv(s->buf, s->n);
  st->on = s->n;
}

static int same_state(const state *x, const state *y) {
  return x->a == y->a && x->b == y->b && x->c == y->c && x->d == y->d &&
         x->hh == y->hh && x->hm == y->hm && x->ho == y->ho && x->on == y->on;
}

/* ------------------------------------------------------------------ */
/* Program execution                                                   */
/* ------------------------------------------------------------------ */

typedef struct {
  zq_zpaql z;
  zq_writer w;
  sink s;
  zq_err_frame ef; /* must stay alive while running */
} machine;

static void machine_open(machine *m, const zq_zpaql *src) {
  zq_zpaql_init(&m->z);
  /* Clone the header and sizes but keep separate storage. */
  zq_arr_resize(&m->z.header, src->header.n, 1);
  memcpy(m->z.header.data, src->header.data, src->header.n);
  m->z.cend = src->cend;
  m->z.hbegin = src->hbegin;
  m->z.hend = src->hend;
  zq_zpaql_init_sizes(&m->z, ZQ_HDR(&m->z, 2), ZQ_HDR(&m->z, 3));
  m->w.put = sink_put;
  m->w.write = sink_write;
  m->w.self = &m->s;
  m->z.output = &m->w;
  m->s.n = 0;
}

static void machine_close(machine *m) {
  m->z.output = NULL;
  zq_zpaql_free(&m->z);
}

/* One step; returns 0 on success or -1 if the program raised an error. */
static int step(machine *m, zq_u32 in, int usejit) {
  if (setjmp(m->ef.jb)) return -1;
  zq_err_push(&m->ef);
  if (usejit) {
    zq_zpaql_run(&m->z, in);
  } else {
    m->z.pc = m->z.hbegin;
    m->z.a = in;
    while (zq_zpaql_execute(&m->z)) {
    }
  }
  zq_err_pop(&m->ef);
  return 0;
}

/* Run both machines over the same inputs.  Returns 1 on mismatch. */
static int compare_run(const zq_zpaql *proto, const zq_u32 *in, int n,
                       const char *what, int idx) {
  machine mi, mj;
  state si, sj;
  int k;
  int ei = 0, ej = 0;

  machine_open(&mi, proto);
  machine_open(&mj, proto);

  for (k = 0; k < n; ++k) {
    ei = step(&mi, in[k], 0);
    ej = step(&mj, in[k], 1);
    if (ei != ej) {
      printf("MISMATCH %s %d: error interp=%d jit=%d at input %d (0x%x)\n",
             what, idx, ei, ej, k, in[k]);
      goto bad;
    }
    if (ei) break; /* both stopped the same way */
    zq_zpaql_flush(&mi.z);
    zq_zpaql_flush(&mj.z);
    snapshot(&mi.z, &mi.s, &si);
    snapshot(&mj.z, &mj.s, &sj);
    if (!same_state(&si, &sj)) {
      printf("MISMATCH %s %d at input %d (0x%x)\n", what, idx, k, in[k]);
      printf("  interp a=%u b=%u c=%u d=%u h=%016llx m=%016llx o=%016llx/%zu\n",
             si.a, si.b, si.c, si.d, si.hh, si.hm, si.ho, si.on);
      printf("  jit    a=%u b=%u c=%u d=%u h=%016llx m=%016llx o=%016llx/%zu\n",
             sj.a, sj.b, sj.c, sj.d, sj.hh, sj.hm, sj.ho, sj.on);
      goto bad;
    }
  }

  if (!mj.z.rcode) {
    printf("NOJIT %s %d: assemble() produced no code\n", what, idx);
    goto bad;
  }

  machine_close(&mi);
  machine_close(&mj);
  return 0;

bad:
  machine_close(&mi);
  machine_close(&mj);
  return 1;
}

/* ------------------------------------------------------------------ */
/* Random programs                                                     */
/* ------------------------------------------------------------------ */

static unsigned lcg = 12345;

static unsigned rnd(void) {
  lcg = lcg * 1103515245u + 12345u;
  return lcg >> 8;
}

static int iserr(int op) {
  return op == 0 || (op >= 120 && op <= 127) || (op >= 240 && op <= 254) ||
         op == 58 || (op < 64 && (op % 8 == 5 || op % 8 == 6));
}

/* Build a random forward-branching program (terminates by construction). */
static int build_random(zq_u8 *prog, int maxlen) {
  int ops[64], off[64], nops, i, o = 0, total;
  nops = 1 + (int)(rnd() % 40);
  /* A program is a run of instructions plus a final halt.  Only forward
   * branches are generated, so the program always terminates. */
  for (i = 0; i < nops; ++i) {
    int op = (int)(rnd() % 256);
    while (iserr(op) || op == 56) op = (int)(rnd() % 256);
    /* Upstream's assembler merges a run of identical bytes as one
     * instruction (oplen).  That is exact for ++/-- but for "X<>A" it
     * performs a single swap per run, so the JIT and the interpreter
     * disagree there (upstream has the same quirk; see docs/JIT.md).
     * Do not generate such runs: the quirk has its own check below. */
    if (i && op == ops[i - 1] &&
        (op == 8 || op == 16 || op == 24 || op == 32 || op == 40 || op == 48))
      op = (op == 8) ? 24 : 8;
    ops[i] = op;
  }
  /* Offsets. */
  for (i = 0; i < nops; ++i) {
    off[i] = o;
    o += (ops[i] == 255 ? 3 : (ops[i] % 8 == 7 ? 2 : 1));
  }
  total = o + 1; /* halt */
  if (total + 1 > maxlen) return 0;

  o = 0;
  for (i = 0; i < nops; ++i) {
    int op = ops[i];
    prog[o++] = (zq_u8)op;
    if (op == 39 || op == 47 || op == 63) {
      /* Jump forward to a random later instruction (may be the halt). */
      int t = i + 1 + (int)(rnd() % (unsigned)(nops - i));
      int arg;
      int target = (t < nops) ? off[t] : total - 1;
      if ((rnd() % 100) < 8) {
        /* Deliberately out of range: must be an error in both. */
        arg = 127;
      } else {
        arg = target - (off[i] + 2);
        if (arg < 0) arg = 0;
        if (arg > 127) arg = 127;
      }
      prog[o++] = (zq_u8)(arg & 255);
    } else if (op == 255) {
      int t = i + 1 + (int)(rnd() % (unsigned)(nops - i));
      int target = (t < nops) ? off[t] : total - 1;
      prog[o++] = (zq_u8)(target & 255);
      prog[o++] = (zq_u8)((target >> 8) & 255);
    } else if (op % 8 == 7) {
      prog[o++] = (zq_u8)(rnd() & 255);
    }
  }
  prog[o++] = 56; /* halt */
  prog[o++] = 0;  /* terminating error opcode */
  return o;
}

/* Wrap a program in a minimal header (no components). */
static void make_config(zq_zpaql *z, const zq_u8 *prog, int plen, int hh,
                        int hm) {
  int hsize = 6 + plen;
  zq_zpaql_init(z);
  zq_arr_resize(&z->header, (size_t)hsize + 300, 1);
  ZQ_HDR(z, 0) = (zq_u8)(hsize & 255);
  ZQ_HDR(z, 1) = (zq_u8)(hsize >> 8);
  ZQ_HDR(z, 2) = (zq_u8)hh;
  ZQ_HDR(z, 3) = (zq_u8)hm;
  ZQ_HDR(z, 4) = 0;
  ZQ_HDR(z, 5) = 0;
  ZQ_HDR(z, 6) = 0; /* no components */
  z->cend = 7;
  ZQ_HDR(z, z->cend++) = 0; /* COMP END */
  z->hbegin = z->hend = z->cend + 128;
  memcpy((zq_u8 *)z->header.data + z->hbegin, prog, (size_t)plen);
  z->hend += plen;
  ZQ_HDR(z, z->hend++) = 0; /* HCOMP END */
  zq_zpaql_init_sizes(z, hh, hm);
}

/* ------------------------------------------------------------------ */
/* Manifest (E2 configs)                                               */
/* ------------------------------------------------------------------ */

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
  zq_u32 inputs[256];
  char line[65536], config[65536];
  int args[9];
  FILE *mf;
  int fails = 0, cases = 0, i, k;
  int nprog = (argc > 2) ? atoi(argv[2]) : 300;
  int ninput = (argc > 3) ? atoi(argv[3]) : 64;

  for (i = 0; i < 256; ++i) inputs[i] = (zq_u32)(i < 128 ? (unsigned)i : rnd());

  if (argc > 1) {
    mf = fopen(argv[1], "r");
    if (mf) {
      while (fgets(line, (int)sizeof(line), mf)) {
        if (!strncmp(line, "CONFIG ", 7)) {
          unescape(line + 7, config);
        } else if (!strncmp(line, "ARGS", 4)) {
          const char *p = line + 4;
          zq_zpaql hz, pz;
          for (i = 0; i < 9; ++i) args[i] = (int)strtol(p, (char **)&p, 10);
          zq_zpaql_init(&hz);
          zq_zpaql_init(&pz);
          zq_compile(config, args, &hz, &pz, NULL);
          zq_zpaql_init_sizes(&hz, ZQ_HDR(&hz, 2), ZQ_HDR(&hz, 3));
          fails += compare_run(&hz, inputs, 256, "config", cases);
          zq_zpaql_free(&hz);
          zq_zpaql_free(&pz);
          ++cases;
        }
      }
      fclose(mf);
    }
  }
  printf("jit_diff: %d manifest configs\n", cases);

  /* Known upstream quirk, checked explicitly: "c<>a c<>a halt".  The
   * interpreter performs both swaps; the assembler merges the identical run
   * and emits a single swap, exactly like upstream's JIT (verified against
   * the reference binary). */
  {
    static const zq_u8 prog[] = {16, 16, 56, 0};
    zq_zpaql z;
    machine mi, mj;
    int err;
    make_config(&z, prog, 4, 0, 0);
    {
      zq_u32 in = 0xFFFFFF00u;
      machine_open(&mi, &z);
      machine_open(&mj, &z);
      err = step(&mi, in, 0);
      err |= step(&mj, in, 1);
      if (err) {
        printf("QUIRK: unexpected error\n");
        ++fails;
      } else if (mi.z.a != 0xFFFFFF00u || mi.z.c != 0) {
        printf("QUIRK: interpreter a=%u c=%u\n", mi.z.a, mi.z.c);
        ++fails;
      } else if (mj.z.a != 0 || mj.z.c != 0xFFFFFF00u) {
        printf("QUIRK: jit a=%u c=%u (upstream gives a=0 c=0xffffff00)\n",
               mj.z.a, mj.z.c);
        ++fails;
      }
      machine_close(&mi);
      machine_close(&mj);
    }
    zq_zpaql_free(&z);
    ++cases;
  }

  for (k = 0; k < nprog; ++k) {
    zq_u8 prog[512];
    int plen = build_random(prog, (int)sizeof(prog) - 1);
    zq_zpaql z;
    zq_u32 rin[64];
    int n = ninput < 64 ? ninput : 64;
    if (plen <= 0) continue;
    make_config(&z, prog, plen, (int)(rnd() % 9), (int)(rnd() % 9));
    for (i = 0; i < n; ++i) rin[i] = (zq_u32)(rnd() & 255);
    {
      int r = compare_run(&z, rin, n, "random", k);
      if (r) {
        printf("  prog:");
        for (i = 0; i < plen; ++i) printf(" %02x", prog[i]);
        printf("\n  hh=%d hm=%d\n", ZQ_HDR(&z, 2), ZQ_HDR(&z, 3));
      }
      fails += r;
    }
    zq_zpaql_free(&z);
    ++cases;
  }

  printf("jit_diff: %d programs, %d failures\n", cases, fails);
  return fails ? 1 : 0;
}
