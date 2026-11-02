/* Verificación E1: mismas líneas que tools/oracle/e1_dump.cpp pero usando el
 * core C del port.  `make test-e1` compara ambas salidas byte a byte. */
#include "zq.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void gen(unsigned char *p, int n, unsigned seed) {
  unsigned x = seed;
  int i;
  for (i = 0; i < n; ++i) {
    x = x * 1103515245u + 12345u;
    p[i] = (unsigned char)(x >> 16);
  }
}

static void hex(const char *p, int n) {
  int i;
  for (i = 0; i < n; ++i) printf("%02x", (unsigned char)p[i]);
}

static void dump_sha1(const char *label, const char *p, int n, int chunk) {
  zq_sha1 s;
  zq_sha1_init(&s);
  if (chunk <= 0) zq_sha1_write(&s, p, n);
  else {
    int i;
    for (i = 0; i < n; i += chunk) {
      int m = n - i < chunk ? n - i : chunk;
      zq_sha1_write(&s, p + i, m);
    }
  }
  printf("sha1 %s %d %d ", label, n, chunk);
  hex(zq_sha1_result(&s), 20);
  printf("\n");
}

static void dump_sha256(const char *label, const char *p, int n, int chunk) {
  zq_sha256 s;
  zq_sha256_init(&s);
  if (chunk <= 0) zq_sha256_write(&s, p, n);
  else {
    int i;
    for (i = 0; i < n; i += chunk) {
      int m = n - i < chunk ? n - i : chunk;
      zq_sha256_write(&s, p + i, m);
    }
  }
  printf("sha256 %s %d %d ", label, n, chunk);
  hex(zq_sha256_result(&s), 32);
  printf("\n");
}

static void dump_aes(int keylen, int n, unsigned offset) {
  char key[32], iv[8], buf[256];
  zq_aes_ctr a;
  gen((unsigned char *)key, keylen, 1000u + keylen);
  gen((unsigned char *)iv, 8, 2000u + keylen);
  gen((unsigned char *)buf, n, 3000u + n);
  zq_aes_init(&a, key, keylen, iv);
  zq_aes_encrypt(&a, buf, n, offset);
  printf("aes %d %d %u ", keylen, n, offset);
  hex(buf, n);
  printf("\n");
}

static void dump_scrypt(int n, int r, int p) {
  char pw[40], salt[40], out[192];
  int pwlen = 32, saltlen = 32, buflen = p * 32;
  gen((unsigned char *)pw, pwlen, 4000u + n);
  gen((unsigned char *)salt, saltlen, 5000u + n);
  zq_scrypt(pw, pwlen, salt, saltlen, n, r, p, out, buflen);
  printf("scrypt %d %d %d ", n, r, p);
  hex(out, buflen);
  printf("\n");
}

static void dump_stretch(unsigned seed) {
  char in[32], salt[32], out[32];
  gen((unsigned char *)in, 32, 6000u + seed);
  gen((unsigned char *)salt, 32, 7000u + seed);
  zq_stretch_key(out, in, salt);
  printf("stretch %u ", seed);
  hex(out, 32);
  printf("\n");
}

/* Vectores conocidos (FIPS 180).  Falla rápido si el core se desvía. */
static void sanity(void) {
  static const char abc_sha1[] = "a9993e364706816aba3e25717850c26c9cd0d89d";
  static const char abc_sha256[] =
      "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
  static const char empty_sha256[] =
      "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
  char h[80];
  int i;
  zq_sha1 s1;
  zq_sha256 s2;
  zq_sha1_init(&s1);
  zq_sha1_write(&s1, "abc", 3);
  {
    const char *r = zq_sha1_result(&s1);
    for (i = 0; i < 20; ++i)
      snprintf(h + 2 * i, 3, "%02x", (unsigned char)r[i]);
  }
  if (strcmp(h, abc_sha1)) {
    fprintf(stderr, "sanity: SHA1(abc) != %s (%s)\n", abc_sha1, h);
    exit(2);
  }
  zq_sha256_init(&s2);
  zq_sha256_write(&s2, "abc", 3);
  {
    const char *r = zq_sha256_result(&s2);
    for (i = 0; i < 32; ++i)
      snprintf(h + 2 * i, 3, "%02x", (unsigned char)r[i]);
  }
  if (strcmp(h, abc_sha256)) {
    fprintf(stderr, "sanity: SHA256(abc) != %s (%s)\n", abc_sha256, h);
    exit(2);
  }
  zq_sha256_init(&s2);
  {
    const char *r = zq_sha256_result(&s2);
    for (i = 0; i < 32; ++i)
      snprintf(h + 2 * i, 3, "%02x", (unsigned char)r[i]);
  }
  if (strcmp(h, empty_sha256)) {
    fprintf(stderr, "sanity: SHA256() != %s (%s)\n", empty_sha256, h);
    exit(2);
  }
}

int main(void) {
  static char buf[200000];
  int n, c, keylen;
  sanity();
  gen((unsigned char *)buf, sizeof(buf), 42u);

  for (n = 0; n <= 200; n += (n < 70 ? 1 : 37)) {
    for (c = 0; c < 2; ++c) {
      const int chunk = c ? 7 : 0;
      dump_sha1("seq", buf, n, chunk);
      dump_sha256("seq", buf, n, chunk);
    }
  }
  for (n = 1000; n <= 200000; n *= 10) {
    dump_sha1("big", buf, n, 0);
    dump_sha1("big", buf, n, 63);
    dump_sha1("big", buf, n, 64);
    dump_sha1("big", buf, n, 65);
    dump_sha256("big", buf, n, 0);
    dump_sha256("big", buf, n, 1);
    dump_sha256("big", buf, n, 64);
  }
  dump_sha1("abc", "abc", 3, 0);
  dump_sha256("abc", "abc", 3, 0);
  dump_sha1("empty", "", 0, 0);
  dump_sha256("empty", "", 0, 0);

  for (keylen = 16; keylen <= 32; keylen += 8) {
    for (n = 1; n <= 100; n = n * 3 + 2) dump_aes(keylen, n, 0);
    dump_aes(keylen, 16, 0);
    dump_aes(keylen, 16, 1);
    dump_aes(keylen, 16, 5);
    dump_aes(keylen, 31, 17);
    dump_aes(keylen, 64, 32);
    dump_aes(keylen, 200, 33);
  }

  dump_scrypt(16, 1, 1);
  dump_scrypt(16, 1, 3);
  dump_scrypt(1024, 8, 1);
  dump_scrypt(256, 4, 2);

  dump_stretch(0);
  dump_stretch(1);

  return 0;
}
