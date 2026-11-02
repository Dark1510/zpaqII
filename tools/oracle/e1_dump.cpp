// E1 oracle: dumps SHA-1, SHA-256, AES-CTR, scrypt and stretchKey results
// using libzpaq 7.15 (upstream) in a stable format.  The port's C test
// produces exactly the same lines and they are compared with diff.  Not
// part of the binary: verification tooling only.
#include "libzpaq.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void libzpaq::error(const char *msg) {
  fprintf(stderr, "oracle error: %s\n", msg);
  exit(1);
}

static void hex(const char *p, int n) {
  for (int i = 0; i < n; ++i) printf("%02x", (unsigned char)p[i]);
}

static void gen(unsigned char *p, int n, unsigned seed) {
  unsigned x = seed;
  for (int i = 0; i < n; ++i) {
    x = x * 1103515245u + 12345u;
    p[i] = (unsigned char)(x >> 16);
  }
}

static void dump_sha1(const char *label, const char *p, int n, int chunk) {
  libzpaq::SHA1 s;
  if (chunk <= 0) s.write(p, n);
  else
    for (int i = 0; i < n; i += chunk) s.write(p + i, n - i < chunk ? n - i : chunk);
  printf("sha1 %s %d %d ", label, n, chunk);
  hex(s.result(), 20);
  printf("\n");
}

static void dump_sha256(const char *label, const char *p, int n, int chunk) {
  libzpaq::SHA256 s;
  for (int i = 0; i < n; ++i) s.put((unsigned char)p[i]);
  printf("sha256 %s %d %d ", label, n, chunk);
  hex(s.result(), 32);
  printf("\n");
}

static void dump_aes(int keylen, int n, unsigned offset) {
  char key[32], iv[8], buf[256];
  gen((unsigned char *)key, keylen, 1000u + keylen);
  gen((unsigned char *)iv, 8, 2000u + keylen);
  gen((unsigned char *)buf, n, 3000u + n);
  libzpaq::AES_CTR a(key, keylen, iv);
  a.encrypt(buf, n, offset);
  printf("aes %d %d %u ", keylen, n, offset);
  hex(buf, n);
  printf("\n");
}

static void dump_scrypt(int n, int r, int p) {
  char pw[40], salt[40], out[192];
  int pwlen = 32, saltlen = 32, buflen = p * 32;
  gen((unsigned char *)pw, pwlen, 4000u + n);
  gen((unsigned char *)salt, saltlen, 5000u + n);
  libzpaq::scrypt(pw, pwlen, salt, saltlen, n, r, p, out, buflen);
  printf("scrypt %d %d %d ", n, r, p);
  hex(out, buflen);
  printf("\n");
}

static void dump_stretch(unsigned seed) {
  char in[32], salt[32], out[32];
  gen((unsigned char *)in, 32, 6000u + seed);
  gen((unsigned char *)salt, 32, 7000u + seed);
  libzpaq::stretchKey(out, in, salt);
  printf("stretch %u ", seed);
  hex(out, 32);
  printf("\n");
}

int main() {
  static char buf[200000];
  gen((unsigned char *)buf, sizeof(buf), 42u);

  for (int n = 0; n <= 200; n += (n < 70 ? 1 : 37)) {
    for (int c = 0; c < 2; ++c) {
      const int chunk = c ? 7 : 0;
      dump_sha1("seq", buf, n, chunk);
      dump_sha256("seq", buf, n, chunk);
    }
  }
  for (int n = 1000; n <= 200000; n *= 10) {
    dump_sha1("big", buf, n, 0);
    dump_sha1("big", buf, n, 63);
    dump_sha1("big", buf, n, 64);
    dump_sha1("big", buf, n, 65);
    dump_sha256("big", buf, n, 0);
    dump_sha256("big", buf, n, 1);
    dump_sha256("big", buf, n, 64);
  }
  /* known vectors */
  dump_sha1("abc", "abc", 3, 0);
  dump_sha256("abc", "abc", 3, 0);
  dump_sha1("empty", "", 0, 0);
  dump_sha256("empty", "", 0, 0);

  for (int keylen = 16; keylen <= 32; keylen += 8) {
    for (int n = 1; n <= 100; n = n * 3 + 2) dump_aes(keylen, n, 0);
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
