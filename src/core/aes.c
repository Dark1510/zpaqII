/* zpaqII - AES-256-CTR, scrypt/stretchKey and random.
 * Port of libzpaq 7.15 (Unlicense, Matt Mahoney).  AES derives from
 * libtomcrypt 1.17 (public domain); scrypt/Salsa20/8 from cr.yp.to. */
#include "../../include/zq.h"

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* AES                                                                 */
/* ------------------------------------------------------------------ */

#define BYTE(x, n) (((x) >> (8 * (n))) & 255u)

void zq_aes_init(zq_aes_ctr *a, const char *key, int keylen, const char *iv) {
  int i;
  static const zq_u32 rcon[10] = {0x01000000u, 0x02000000u, 0x04000000u,
                                  0x08000000u, 0x10000000u, 0x20000000u,
                                  0x40000000u, 0x80000000u, 0x1B000000u,
                                  0x36000000u};

  /* IV (default 0) */
  a->iv0 = a->iv1 = 0;
  if (iv) {
    const unsigned char *u = (const unsigned char *)iv;
    a->iv0 = ((zq_u32)u[0] << 24) | ((zq_u32)u[1] << 16) |
             ((zq_u32)u[2] << 8) | (zq_u32)u[3];
    a->iv1 = ((zq_u32)u[4] << 24) | ((zq_u32)u[5] << 16) |
             ((zq_u32)u[6] << 8) | (zq_u32)u[7];
  }

  /* encryption tables */
  for (i = 0; i < 256; ++i) {
    const unsigned s1 =
        "\x63\x7c\x77\x7b\xf2\x6b\x6f\xc5\x30\x01\x67\x2b\xfe\xd7\xab\x76"
        "\xca\x82\xc9\x7d\xfa\x59\x47\xf0\xad\xd4\xa2\xaf\x9c\xa4\x72\xc0"
        "\xb7\xfd\x93\x26\x36\x3f\xf7\xcc\x34\xa5\xe5\xf1\x71\xd8\x31\x15"
        "\x04\xc7\x23\xc3\x18\x96\x05\x9a\x07\x12\x80\xe2\xeb\x27\xb2\x75"
        "\x09\x83\x2c\x1a\x1b\x6e\x5a\xa0\x52\x3b\xd6\xb3\x29\xe3\x2f\x84"
        "\x53\xd1\x00\xed\x20\xfc\xb1\x5b\x6a\xcb\xbe\x39\x4a\x4c\x58\xcf"
        "\xd0\xef\xaa\xfb\x43\x4d\x33\x85\x45\xf9\x02\x7f\x50\x3c\x9f\xa8"
        "\x51\xa3\x40\x8f\x92\x9d\x38\xf5\xbc\xb6\xda\x21\x10\xff\xf3\xd2"
        "\xcd\x0c\x13\xec\x5f\x97\x44\x17\xc4\xa7\x7e\x3d\x64\x5d\x19\x73"
        "\x60\x81\x4f\xdc\x22\x2a\x90\x88\x46\xee\xb8\x14\xde\x5e\x0b\xdb"
        "\xe0\x32\x3a\x0a\x49\x06\x24\x5c\xc2\xd3\xac\x62\x91\x95\xe4\x79"
        "\xe7\xc8\x37\x6d\x8d\xd5\x4e\xa9\x6c\x56\xf4\xea\x65\x7a\xae\x08"
        "\xba\x78\x25\x2e\x1c\xa6\xb4\xc6\xe8\xdd\x74\x1f\x4b\xbd\x8b\x8a"
        "\x70\x3e\xb5\x66\x48\x03\xf6\x0e\x61\x35\x57\xb9\x86\xc1\x1d\x9e"
        "\xe1\xf8\x98\x11\x69\xd9\x8e\x94\x9b\x1e\x87\xe9\xce\x55\x28\xdf"
        "\x8c\xa1\x89\x0d\xbf\xe6\x42\x68\x41\x99\x2d\x0f\xb0\x54\xbb\x16"
        [i] & 255u;
    unsigned s2 = s1 << 1;
    unsigned s3;
    if (s2 >= 0x100) s2 ^= 0x11b;
    s3 = s1 ^ s2;
    a->Te0[i] = (zq_u32)s2 << 24 | (zq_u32)s1 << 16 | (zq_u32)s1 << 8 | s3;
    a->Te1[i] = (zq_u32)s3 << 24 | (zq_u32)s2 << 16 | (zq_u32)s1 << 8 | s1;
    a->Te2[i] = (zq_u32)s1 << 24 | (zq_u32)s3 << 16 | (zq_u32)s2 << 8 | s1;
    a->Te3[i] = (zq_u32)s1 << 24 | (zq_u32)s1 << 16 | (zq_u32)s3 << 8 | s2;
    a->Te4[i] = (zq_u32)s1 << 24 | (zq_u32)s1 << 16 | (zq_u32)s1 << 8 | s1;
  }

  /* key schedule */
  a->Nr = 10 + ((keylen / 8) - 2) * 2; /* 10, 12 or 14 rounds */
  {
    int idx = 0;
    zq_u32 *rk = &a->ek[0];
    zq_u32 temp;
    const unsigned char *ku = (const unsigned char *)key;
#define ZQ_LOAD32H(x, y)                                                     \
  do {                                                                       \
    x = ((zq_u32)(y)[0] << 24) | ((zq_u32)(y)[1] << 16) |                    \
        ((zq_u32)(y)[2] << 8) | (zq_u32)(y)[3];                              \
  } while (0)
#define ZQ_SETUP_MIX(t)                                                      \
  ((a->Te4[BYTE(t, 2)] & 0xFF000000u) ^                                      \
   (a->Te4[BYTE(t, 1)] & 0x00FF0000u) ^                                      \
   (a->Te4[BYTE(t, 0)] & 0x0000FF00u) ^                                      \
   (a->Te4[BYTE(t, 3)] & 0x000000FFu))

    ZQ_LOAD32H(rk[0], ku + 0);
    ZQ_LOAD32H(rk[1], ku + 4);
    ZQ_LOAD32H(rk[2], ku + 8);
    ZQ_LOAD32H(rk[3], ku + 12);
    if (keylen == 16) {
      for (;;) {
        temp = rk[3];
        rk[4] = rk[0] ^ ZQ_SETUP_MIX(temp) ^ rcon[idx];
        rk[5] = rk[1] ^ rk[4];
        rk[6] = rk[2] ^ rk[5];
        rk[7] = rk[3] ^ rk[6];
        if (++idx == 10) break;
        rk += 4;
      }
    } else if (keylen == 24) {
      ZQ_LOAD32H(rk[4], ku + 16);
      ZQ_LOAD32H(rk[5], ku + 20);
      for (;;) {
        temp = rk[5];
        rk[6] = rk[0] ^ ZQ_SETUP_MIX(temp) ^ rcon[idx];
        rk[7] = rk[1] ^ rk[6];
        rk[8] = rk[2] ^ rk[7];
        rk[9] = rk[3] ^ rk[8];
        if (++idx == 8) break;
        rk[10] = rk[4] ^ rk[9];
        rk[11] = rk[5] ^ rk[10];
        rk += 6;
      }
    } else if (keylen == 32) {
      ZQ_LOAD32H(rk[4], ku + 16);
      ZQ_LOAD32H(rk[5], ku + 20);
      ZQ_LOAD32H(rk[6], ku + 24);
      ZQ_LOAD32H(rk[7], ku + 28);
      for (;;) {
        temp = rk[7];
        rk[8] = rk[0] ^ ZQ_SETUP_MIX(temp) ^ rcon[idx];
        rk[9] = rk[1] ^ rk[8];
        rk[10] = rk[2] ^ rk[9];
        rk[11] = rk[3] ^ rk[10];
        if (++idx == 7) break;
        temp = rk[11];
        rk[12] = rk[4] ^ ZQ_SETUP_MIX((temp << 24) | (temp >> 8));
        rk[13] = rk[5] ^ rk[12];
        rk[14] = rk[6] ^ rk[13];
        rk[15] = rk[7] ^ rk[14];
        rk += 8;
      }
    }
#undef ZQ_SETUP_MIX
#undef ZQ_LOAD32H
  }
}

void zq_aes_encrypt_block(zq_aes_ctr *a, zq_u32 s0, zq_u32 s1, zq_u32 s2,
                          zq_u32 s3, unsigned char *ct) {
  int r = a->Nr >> 1;
  zq_u32 *rk = &a->ek[0];
  zq_u32 t0 = 0, t1 = 0, t2 = 0, t3 = 0;
  s0 ^= rk[0];
  s1 ^= rk[1];
  s2 ^= rk[2];
  s3 ^= rk[3];
  for (;;) {
    t0 = a->Te0[BYTE(s0, 3)] ^ a->Te1[BYTE(s1, 2)] ^ a->Te2[BYTE(s2, 1)] ^
         a->Te3[BYTE(s3, 0)] ^ rk[4];
    t1 = a->Te0[BYTE(s1, 3)] ^ a->Te1[BYTE(s2, 2)] ^ a->Te2[BYTE(s3, 1)] ^
         a->Te3[BYTE(s0, 0)] ^ rk[5];
    t2 = a->Te0[BYTE(s2, 3)] ^ a->Te1[BYTE(s3, 2)] ^ a->Te2[BYTE(s0, 1)] ^
         a->Te3[BYTE(s1, 0)] ^ rk[6];
    t3 = a->Te0[BYTE(s3, 3)] ^ a->Te1[BYTE(s0, 2)] ^ a->Te2[BYTE(s1, 1)] ^
         a->Te3[BYTE(s2, 0)] ^ rk[7];

    rk += 8;
    if (--r == 0) break;

    s0 = a->Te0[BYTE(t0, 3)] ^ a->Te1[BYTE(t1, 2)] ^ a->Te2[BYTE(t2, 1)] ^
         a->Te3[BYTE(t3, 0)] ^ rk[0];
    s1 = a->Te0[BYTE(t1, 3)] ^ a->Te1[BYTE(t2, 2)] ^ a->Te2[BYTE(t3, 1)] ^
         a->Te3[BYTE(t0, 0)] ^ rk[1];
    s2 = a->Te0[BYTE(t2, 3)] ^ a->Te1[BYTE(t3, 2)] ^ a->Te2[BYTE(t0, 1)] ^
         a->Te3[BYTE(t1, 0)] ^ rk[2];
    s3 = a->Te0[BYTE(t3, 3)] ^ a->Te1[BYTE(t0, 2)] ^ a->Te2[BYTE(t1, 1)] ^
         a->Te3[BYTE(t2, 0)] ^ rk[3];
  }

  s0 = (a->Te4[BYTE(t0, 3)] & 0xFF000000u) ^
       (a->Te4[BYTE(t1, 2)] & 0x00FF0000u) ^
       (a->Te4[BYTE(t2, 1)] & 0x0000FF00u) ^
       (a->Te4[BYTE(t3, 0)] & 0x000000FFu) ^ rk[0];
  ct[0] = (unsigned char)(s0 >> 24);
  ct[1] = (unsigned char)(s0 >> 16);
  ct[2] = (unsigned char)(s0 >> 8);
  ct[3] = (unsigned char)(s0);
  s1 = (a->Te4[BYTE(t1, 3)] & 0xFF000000u) ^
       (a->Te4[BYTE(t2, 2)] & 0x00FF0000u) ^
       (a->Te4[BYTE(t3, 1)] & 0x0000FF00u) ^
       (a->Te4[BYTE(t0, 0)] & 0x000000FFu) ^ rk[1];
  ct[4] = (unsigned char)(s1 >> 24);
  ct[5] = (unsigned char)(s1 >> 16);
  ct[6] = (unsigned char)(s1 >> 8);
  ct[7] = (unsigned char)(s1);
  s2 = (a->Te4[BYTE(t2, 3)] & 0xFF000000u) ^
       (a->Te4[BYTE(t3, 2)] & 0x00FF0000u) ^
       (a->Te4[BYTE(t0, 1)] & 0x0000FF00u) ^
       (a->Te4[BYTE(t1, 0)] & 0x000000FFu) ^ rk[2];
  ct[8] = (unsigned char)(s2 >> 24);
  ct[9] = (unsigned char)(s2 >> 16);
  ct[10] = (unsigned char)(s2 >> 8);
  ct[11] = (unsigned char)(s2);
  s3 = (a->Te4[BYTE(t3, 3)] & 0xFF000000u) ^
       (a->Te4[BYTE(t0, 2)] & 0x00FF0000u) ^
       (a->Te4[BYTE(t1, 1)] & 0x0000FF00u) ^
       (a->Te4[BYTE(t2, 0)] & 0x000000FFu) ^ rk[3];
  ct[12] = (unsigned char)(s3 >> 24);
  ct[13] = (unsigned char)(s3 >> 16);
  ct[14] = (unsigned char)(s3 >> 8);
  ct[15] = (unsigned char)(s3);
}

/* XOR buf[0..n-1] at the given offset with AES(i) (i big-endian per block). */
void zq_aes_encrypt(zq_aes_ctr *a, char *buf, int n, zq_u64 offset) {
  zq_u64 i;
  for (i = offset / 16; i <= (offset + (zq_u64)n) / 16; ++i) {
    unsigned char ct[16];
    int j;
    zq_aes_encrypt_block(a, a->iv0, a->iv1, (zq_u32)(i >> 32), (zq_u32)i, ct);
    for (j = 0; j < 16; ++j) {
      const zq_i64 k = (zq_i64)(i * 16) - (zq_i64)offset + j;
      if (k >= 0 && k < n) buf[k] ^= (char)ct[j];
    }
  }
}

/* ------------------------------------------------------------------ */
/* scrypt (PBKDF2-SHA256 with c=1 + SMix)                              */
/* ------------------------------------------------------------------ */

static void pbkdf2(const char *pw, int pwLen, const char *salt, int saltLen,
                   int c, char *buf, int dkLen) {
  zq_sha256 sha;
  char b[32];
  int i, j;
  (void)c;
  zq_sha256_init(&sha);
  for (i = 1; i * 32 <= dkLen; ++i) {
    for (j = 0; j < pwLen; ++j) zq_sha256_put(&sha, pw[j] ^ 0x36);
    for (j = pwLen; j < 64; ++j) zq_sha256_put(&sha, 0x36);
    for (j = 0; j < saltLen; ++j) zq_sha256_put(&sha, salt[j]);
    for (j = 24; j >= 0; j -= 8) zq_sha256_put(&sha, (int)((unsigned)i >> j));
    memcpy(b, zq_sha256_result(&sha), 32);
    for (j = 0; j < pwLen; ++j) zq_sha256_put(&sha, pw[j] ^ 0x5c);
    for (j = pwLen; j < 64; ++j) zq_sha256_put(&sha, 0x5c);
    for (j = 0; j < 32; ++j) zq_sha256_put(&sha, b[j]);
    memcpy(buf + i * 32 - 32, zq_sha256_result(&sha), 32);
  }
}

/* Salsa20/8 over b[0..15] (32-bit words). */
static void salsa8(zq_u32 *b) {
  unsigned x[16];
  int i;
  memset(x, 0, sizeof(x));
  memcpy(x, b, 64);
  for (i = 0; i < 4; ++i) {
#define R(a, b_) (((a) << (b_)) + ((a) >> (32 - (b_))))
    x[4] ^= R(x[0] + x[12], 7);
    x[8] ^= R(x[4] + x[0], 9);
    x[12] ^= R(x[8] + x[4], 13);
    x[0] ^= R(x[12] + x[8], 18);
    x[9] ^= R(x[5] + x[1], 7);
    x[13] ^= R(x[9] + x[5], 9);
    x[1] ^= R(x[13] + x[9], 13);
    x[5] ^= R(x[1] + x[13], 18);
    x[14] ^= R(x[10] + x[6], 7);
    x[2] ^= R(x[14] + x[10], 9);
    x[6] ^= R(x[2] + x[14], 13);
    x[10] ^= R(x[6] + x[2], 18);
    x[3] ^= R(x[15] + x[11], 7);
    x[7] ^= R(x[3] + x[15], 9);
    x[11] ^= R(x[7] + x[3], 13);
    x[15] ^= R(x[11] + x[7], 18);
    x[1] ^= R(x[0] + x[3], 7);
    x[2] ^= R(x[1] + x[0], 9);
    x[3] ^= R(x[2] + x[1], 13);
    x[0] ^= R(x[3] + x[2], 18);
    x[6] ^= R(x[5] + x[4], 7);
    x[7] ^= R(x[6] + x[5], 9);
    x[4] ^= R(x[7] + x[6], 13);
    x[5] ^= R(x[4] + x[7], 18);
    x[11] ^= R(x[10] + x[9], 7);
    x[8] ^= R(x[11] + x[10], 9);
    x[9] ^= R(x[8] + x[11], 13);
    x[10] ^= R(x[9] + x[8], 18);
    x[12] ^= R(x[15] + x[14], 7);
    x[13] ^= R(x[12] + x[15], 9);
    x[14] ^= R(x[13] + x[12], 13);
    x[15] ^= R(x[14] + x[13], 18);
#undef R
  }
  for (i = 0; i < 16; ++i) b[i] += x[i];
}

/* BlockMix_{Salsa20/8, r} over b[0..128*r-1]. */
static void blockmix(zq_u32 *b, int r) {
  zq_u32 x[16];
  zq_u32 y[256];
  int i, j;
  memcpy(x, b + 32 * r - 16, 64);
  for (i = 0; i < 2 * r; ++i) {
    for (j = 0; j < 16; ++j) x[j] ^= b[i * 16 + j];
    salsa8(x);
    memcpy(&y[i * 16], x, 64);
  }
  for (i = 0; i < r; ++i) memcpy(b + i * 16, &y[i * 32], 64);
  for (i = 0; i < r; ++i) memcpy(b + (i + r) * 16, &y[i * 32 + 16], 64);
}

/* SMix over b[0..128*r-1]. */
static void smix(char *b, int r, int n) {
  zq_arr xarr = {0}, varr = {0};
  zq_u32 *x;
  zq_u32 *v;
  int i, k;
  zq_arr_resize(&xarr, (size_t)32 * r, sizeof(zq_u32));
  zq_arr_resize(&varr, (size_t)32 * r * n, sizeof(zq_u32));
  x = (zq_u32 *)xarr.data;
  v = (zq_u32 *)varr.data;
  for (i = 0; i < r * 128; ++i)
    x[i / 4] += (zq_u32)(unsigned char)b[i] << (i % 4 * 8);
  for (i = 0; i < n; ++i) {
    memcpy(&v[(size_t)i * r * 32], &x[0], (size_t)r * 128);
    blockmix(&x[0], r);
  }
  for (i = 0; i < n; ++i) {
    const zq_u32 j = x[(2 * r - 1) * 16] & (zq_u32)(n - 1);
    for (k = 0; k < r * 32; ++k) x[k] ^= v[(size_t)j * r * 32 + k];
    blockmix(&x[0], r);
  }
  for (i = 0; i < r * 128; ++i)
    b[i] = (char)(x[i / 4] >> (i % 4 * 8));
  zq_arr_free(&xarr);
  zq_arr_free(&varr);
}

void zq_scrypt(const char *pw, int pwlen, const char *salt, int saltlen,
               int n, int r, int p, char *buf, int buflen) {
  zq_arr barr = {0};
  char *b;
  int i;
  zq_arr_resize(&barr, (size_t)p * r * 128, 1);
  b = (char *)barr.data;
  pbkdf2(pw, pwlen, salt, saltlen, 1, &b[0], p * r * 128);
  for (i = 0; i < p; ++i) smix(&b[(size_t)i * r * 128], r, n);
  pbkdf2(pw, pwlen, &b[0], p * r * 128, 1, buf, buflen);
  zq_arr_free(&barr);
}

void zq_stretch_key(char *out, const char *in, const char *salt) {
  zq_scrypt(in, 32, salt, 32, 1 << 14, 8, 1, out, 32);
}

/* ------------------------------------------------------------------ */
/* random                                                              */
/* ------------------------------------------------------------------ */

void zq_random(char *buf, int n) {
  FILE *in = fopen("/dev/urandom", "rb");
  if (in && (int)fread(buf, 1, (size_t)n, in) == n) {
    fclose(in);
  } else {
    if (in) fclose(in);
    zq_error("key generation failed");
  }
  if (n >= 1 && (buf[0] == 'z' || buf[0] == '7')) buf[0] ^= (char)0x80;
}
