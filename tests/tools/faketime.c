/* Test-only LD_PRELOAD shim.
 *
 *  - freezes the clock (FAKE_TIME, unix seconds) so archives created by the
 *    port and by upstream can be compared byte for byte;
 *  - replaces /dev/urandom with a deterministic stream (FAKE_RANDOM, a seed;
 *    default 1) so encrypted archives can be compared too.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

static time_t fake_now(void) {
  const char *s = getenv("FAKE_TIME");
  return s ? (time_t)atol(s) : 0;
}

time_t time(time_t *t) {
  time_t v = fake_now();
  if (t) *t = v;
  return v;
}

int gettimeofday(struct timeval *tv, void *tz) {
  (void)tz;
  if (tv) {
    tv->tv_sec = fake_now();
    tv->tv_usec = 0;
  }
  return 0;
}

/* Deterministic stream for /dev/urandom: a 1 MiB buffer refilled by a
 * xorshift generator seeded from FAKE_RANDOM.  Both binaries start from the
 * same seed, so they get the same salt. */
static unsigned char rndbuf[1 << 20];
static int rndinit = 0;
static unsigned int rndstate = 1;

static void fill_random(void) {
  const char *s = getenv("FAKE_RANDOM");
  size_t i;
  if (rndinit) return;
  rndinit = 1;
  rndstate = s ? (unsigned int)strtoul(s, NULL, 0) : 1u;
  if (rndstate == 0) rndstate = 1;
  for (i = 0; i < sizeof(rndbuf); i += 4) {
    unsigned int x = rndstate;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rndstate = x;
    rndbuf[i] = (unsigned char)(x & 255);
    rndbuf[i + 1] = (unsigned char)((x >> 8) & 255);
    rndbuf[i + 2] = (unsigned char)((x >> 16) & 255);
    rndbuf[i + 3] = (unsigned char)((x >> 24) & 255);
  }
}

FILE *fopen(const char *path, const char *mode) {
  static FILE *(*real_fopen)(const char *, const char *) = NULL;
  if (!real_fopen) real_fopen = (FILE * (*)(const char *, const char *))
      dlsym(RTLD_NEXT, "fopen");
  if (path && strcmp(path, "/dev/urandom") == 0) {
    fill_random();
    return fmemopen(rndbuf, sizeof(rndbuf), "rb");
  }
  return real_fopen(path, mode);
}

FILE *fopen64(const char *path, const char *mode) {
  static FILE *(*real_fopen64)(const char *, const char *) = NULL;
  if (!real_fopen64) real_fopen64 = (FILE * (*)(const char *, const char *))
      dlsym(RTLD_NEXT, "fopen64");
  if (path && strcmp(path, "/dev/urandom") == 0) {
    fill_random();
    return fmemopen(rndbuf, sizeof(rndbuf), "rb");
  }
  return real_fopen64 ? real_fopen64(path, mode) : fopen(path, mode);
}
