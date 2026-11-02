/* Test-only LD_PRELOAD shim: freezes the clock so archives created by the
 * port and by upstream can be compared byte for byte.  FAKE_TIME holds the
 * unix time in seconds. */
#define _GNU_SOURCE
#include <stdlib.h>
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
