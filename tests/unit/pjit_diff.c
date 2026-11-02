/* E18: predictor JIT vs interpreter differential test.
 *
 * Builds random component lists, compiles them into a ZPAQL model and feeds
 * the same bits to two predictors: one uses zq_pred_predict/update (the JIT
 * when available) and the other zq_pred_*_ref (the interpreter).  After every
 * bit the prediction must match, and at the end the complete model state
 * (p, h, component counters and the cm/ht/a16 tables) must be identical.
 *
 * Usage: pjit_diff [cases] [bytes]
 */
#include "zq_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned rnd_state = 1;

static unsigned rnd(unsigned n) {
  rnd_state = rnd_state * 1103515245u + 12345u;
  return n ? (rnd_state >> 8) % n : 0;
}

static void nullput(void *s, int c) { (void)s; (void)c; }
static void nullwrite(void *s, const char *p, int n) { (void)s; (void)p; (void)n; }

/* An HCOMP that writes distinct hashes into h[0..9]. */
static const char *HCOMP =
    "hcomp\n"
    "c-- *c=a a+= 255 d=a *d=c\n"
    "d= 0 *d=0\n"
    "d= 1 *d=0\n"
    "d= 1 b=c a=*d d++\n"
    "hash b++ *d=a d++\n"
    "hash b++ *d=a d++\n"
    "hash b++ *d=a d++\n"
    "hash b++ *d=a d++\n"
    "hash b++ *d=a d++\n"
    "hash b++ *d=a d++\n"
    "hash b++ hash *d=a\n"
    "d= 9 a=*d a*= 24 a+=*c a++ *d=a\n"
    "halt\n"
    "end\n";

/* Append one random, valid component to the config text. */
static int gen_component(char *buf, int i, int *nbytes) {
  char line[128];
  int t = (int)rnd(8);
  if (i == 0 && t > 2) t = 1; /* these need an earlier component */
  switch (t) {
    case 0: /* CM */
      snprintf(line, sizeof line, "%d cm %u %u\n", i, rnd(10), rnd(256));
      break;
    case 1: /* ICM */
      snprintf(line, sizeof line, "%d icm %u\n", i, rnd(10));
      break;
    case 2: /* MATCH */
      snprintf(line, sizeof line, "%d match %u %u\n", i, 4 + rnd(8), 8 + rnd(7));
      break;
    case 3: /* MIX2 */
      snprintf(line, sizeof line, "%d mix2 %u %u %u %u %u\n", i, rnd(8),
               (unsigned)rnd(i), (unsigned)rnd(i), rnd(256), rnd(256));
      break;
    case 4: /* AVG */
      snprintf(line, sizeof line, "%d avg %u %u %u\n", i, (unsigned)rnd(i),
               (unsigned)rnd(i), rnd(256));
      break;
    case 5: { /* MIX */
      unsigned j = (unsigned)rnd(i);
      unsigned m = 1 + rnd(i - (int)j);
      snprintf(line, sizeof line, "%d mix %u %u %u %u %u\n", i, rnd(6), j, m,
               rnd(256), rnd(256));
      break;
    }
    case 6: /* ISSE */
      snprintf(line, sizeof line, "%d isse %u %u\n", i, rnd(10), (unsigned)rnd(i));
      break;
    default: { /* SSE */
      unsigned j = (unsigned)rnd(i);
      unsigned limit = rnd(64);
      unsigned start = limit ? rnd(limit * 4) : 0;
      snprintf(line, sizeof line, "%d sse %u %u %u %u\n", i, rnd(8), j, start,
               limit);
      break;
    }
  }
  strcpy(buf + *nbytes, line);
  *nbytes += (int)strlen(line);
  return 0;
}

static int same_state(zq_predictor *a, zq_predictor *b, int n, const char *cfg) {
  int i;
  if (a->c8 != b->c8 || a->hmap4 != b->hmap4) {
    printf("FAIL c8/hmap4: %d/%d %d/%d\n", a->c8, b->c8, a->hmap4, b->hmap4);
    printf("config:\n%s", cfg);
    return 0;
  }
  for (i = 0; i < n; ++i) {
    if (a->p[i] != b->p[i] || a->h[i] != b->h[i]) {
      printf("FAIL p/h[%d]\n", i);
      printf("config:\n%s", cfg);
      return 0;
    }
    /* SSE keeps the context masked in the JIT and unmasked in the
     * interpreter (upstream does the same); only the masked form is used. */
    if (((a->comp[i].cxt ^ b->comp[i].cxt) &
         (a->comp[i].cm.n ? a->comp[i].cm.n - 1 : ~(size_t)0)) != 0 ||
        (zq_u32)a->comp[i].a != (zq_u32)b->comp[i].a ||
        (zq_u32)a->comp[i].b != (zq_u32)b->comp[i].b ||
        (zq_u32)a->comp[i].c != (zq_u32)b->comp[i].c ||
        (zq_u32)a->comp[i].limit != (zq_u32)b->comp[i].limit) {
      printf("FAIL comp[%d] counters: cxt %zu/%zu a %zu/%zu b %zu/%zu c %zu/%zu lim %zu/%zu\n",
             i, a->comp[i].cxt, b->comp[i].cxt, a->comp[i].a, b->comp[i].a,
             a->comp[i].b, b->comp[i].b, a->comp[i].c, b->comp[i].c,
             a->comp[i].limit, b->comp[i].limit);
      printf("config:\n%s", cfg);
      return 0;
    }
    if (a->comp[i].cm.n != b->comp[i].cm.n ||
        memcmp(a->comp[i].cm.data, b->comp[i].cm.data,
               a->comp[i].cm.n * a->comp[i].cm.elem) ||
        a->comp[i].ht.n != b->comp[i].ht.n ||
        memcmp(a->comp[i].ht.data, b->comp[i].ht.data,
               a->comp[i].ht.n * a->comp[i].ht.elem) ||
        a->comp[i].a16.n != b->comp[i].a16.n ||
        memcmp(a->comp[i].a16.data, b->comp[i].a16.data,
               a->comp[i].a16.n * a->comp[i].a16.elem)) {
      printf("FAIL comp[%d] tables\n", i);
      printf("config:\n%s", cfg);
      return 0;
    }
  }
  return 1;
}

int main(int argc, char **argv) {
  int cases = argc > 1 ? atoi(argv[1]) : 200;
  int nbytes = argc > 2 ? atoi(argv[2]) : 512;
  int c;

  for (c = 0; c < cases; ++c) {
    char cfg[4096];
    int len = 0, args[9], i, j, nb;
    unsigned seed;
    zq_zpaql hz, pz, hz2, pz2;
    zq_writer w;
    zq_predictor pa, pb;

    rnd_state = (unsigned)(c * 2654435761u + 12345u);
    nb = (int)rnd(8) + 1; /* 1..8 components */
    len = snprintf(cfg, sizeof cfg, "comp 9 16 0 0 %d\n", nb);
    for (i = 0; i < nb; ++i) gen_component(cfg, i, &len);
    strcpy(cfg + len, HCOMP);

    memset(args, 0, sizeof args);
    w.put = nullput;
    w.write = nullwrite;
    w.self = NULL;
    /* Each predictor needs its own VM: the byte-boundary HCOMP run mutates
     * the VM state. */
    zq_zpaql_init(&hz);
    zq_zpaql_init(&pz);
    zq_zpaql_init(&hz2);
    zq_zpaql_init(&pz2);
    zq_compile(cfg, args, &hz, &pz, &w);
    zq_compile(cfg, args, &hz2, &pz2, &w);

    zq_pred_open(&pa, &hz);
    zq_pred_open(&pb, &hz2);
    zq_pred_init(&pa);
    zq_pred_init(&pb);

    seed = (unsigned)(c * 7919u + 17u);
    for (j = 0; j < nbytes; ++j) {
      int byte;
      seed = seed * 1103515245u + 12345u;
      byte = (int)((seed >> 16) & 255);
      for (i = 7; i >= 0; --i) {
        int y = (byte >> i) & 1;
        int x = zq_pred_predict(&pa);
        int z = zq_pred_predict_ref(&pb);
        if (x != z) {
          printf("FAIL case %d predict: %d/%d at byte %d\n", c, x, z, j);
          printf("config:\n%s", cfg);
          return 1;
        }
        zq_pred_update(&pa, y);
        zq_pred_update_ref(&pb, y);
      }
    }
    if (!same_state(&pa, &pb, nb, cfg)) return 1;

    zq_pred_free(&pa);
    zq_pred_free(&pb);
    zq_zpaql_free(&hz);
    zq_zpaql_free(&pz);
    zq_zpaql_free(&hz2);
    zq_zpaql_free(&pz2);
  }
  printf("pjit: %d configs, %d bytes each: identical\n", cases, nbytes);
  return 0;
}
