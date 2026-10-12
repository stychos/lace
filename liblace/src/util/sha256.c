/*
 * Lace
 * SHA-256 (FIPS 180-4) and HMAC-SHA256 (RFC 2104)
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "../../include/util/sha256.h"
#include <string.h>

typedef struct {
  uint32_t h[8];
  uint8_t buf[64];
  size_t buf_len;
  uint64_t total;
} Sha256;

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_block(Sha256 *s, const uint8_t *p) {
  uint32_t w[64];
  for (int i = 0; i < 16; i++) {
    w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 |
           (uint32_t)p[4 * i + 2] << 8 | (uint32_t)p[4 * i + 3];
  }
  for (int i = 16; i < 64; i++) {
    uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
    uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  uint32_t a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3];
  uint32_t e = s->h[4], f = s->h[5], g = s->h[6], h = s->h[7];
  for (int i = 0; i < 64; i++) {
    uint32_t t1 = h + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) +
                  ((e & f) ^ (~e & g)) + K[i] + w[i];
    uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) +
                  ((a & b) ^ (a & c) ^ (b & c));
    h = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b;
    b = a;
    a = t1 + t2;
  }
  s->h[0] += a;
  s->h[1] += b;
  s->h[2] += c;
  s->h[3] += d;
  s->h[4] += e;
  s->h[5] += f;
  s->h[6] += g;
  s->h[7] += h;
}

static void sha256_init(Sha256 *s) {
  static const uint32_t iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372,
                                 0xa54ff53a, 0x510e527f, 0x9b05688c,
                                 0x1f83d9ab, 0x5be0cd19};
  memcpy(s->h, iv, sizeof(iv));
  s->buf_len = 0;
  s->total = 0;
}

static void sha256_update(Sha256 *s, const void *data, size_t len) {
  const uint8_t *p = data;
  s->total += len;
  while (len > 0) {
    size_t n = 64 - s->buf_len;
    if (n > len) {
      n = len;
    }
    memcpy(s->buf + s->buf_len, p, n);
    s->buf_len += n;
    p += n;
    len -= n;
    if (s->buf_len == 64) {
      sha256_block(s, s->buf);
      s->buf_len = 0;
    }
  }
}

static void sha256_final(Sha256 *s, uint8_t out[LACE_SHA256_LEN]) {
  uint64_t bits = s->total * 8;
  uint8_t pad = 0x80;
  sha256_update(s, &pad, 1);
  pad = 0;
  while (s->buf_len != 56) {
    sha256_update(s, &pad, 1);
  }
  uint8_t len_be[8];
  for (int i = 0; i < 8; i++) {
    len_be[i] = (uint8_t)(bits >> (56 - 8 * i));
  }
  sha256_update(s, len_be, 8);
  for (int i = 0; i < 8; i++) {
    out[4 * i] = (uint8_t)(s->h[i] >> 24);
    out[4 * i + 1] = (uint8_t)(s->h[i] >> 16);
    out[4 * i + 2] = (uint8_t)(s->h[i] >> 8);
    out[4 * i + 3] = (uint8_t)s->h[i];
  }
  memset(s, 0, sizeof(*s));
}

void lace_sha256(const void *data, size_t len, uint8_t out[LACE_SHA256_LEN]) {
  Sha256 s;
  sha256_init(&s);
  sha256_update(&s, data, len);
  sha256_final(&s, out);
}

void lace_hmac_sha256(const void *key, size_t key_len, const void *msg,
                      size_t msg_len, uint8_t out[LACE_SHA256_LEN]) {
  uint8_t k[64] = {0};
  if (key_len > 64) {
    lace_sha256(key, key_len, k);
  } else if (key_len > 0) {
    memcpy(k, key, key_len);
  }
  uint8_t pad[64];
  uint8_t inner[LACE_SHA256_LEN];
  Sha256 s;

  for (int i = 0; i < 64; i++) {
    pad[i] = k[i] ^ 0x36;
  }
  sha256_init(&s);
  sha256_update(&s, pad, 64);
  sha256_update(&s, msg, msg_len);
  sha256_final(&s, inner);

  for (int i = 0; i < 64; i++) {
    pad[i] = k[i] ^ 0x5c;
  }
  sha256_init(&s);
  sha256_update(&s, pad, 64);
  sha256_update(&s, inner, sizeof(inner));
  sha256_final(&s, out);

  memset(k, 0, sizeof(k));
  memset(pad, 0, sizeof(pad));
  memset(inner, 0, sizeof(inner));
}

void lace_auth_proof(const char *token, const char *label,
                     const char *client_nonce, const char *daemon_nonce,
                     char out[2 * LACE_SHA256_LEN + 1]) {
  Sha256 s;
  uint8_t k[64] = {0};
  size_t key_len = token ? strlen(token) : 0;
  if (key_len > 64) {
    lace_sha256(token, key_len, k);
  } else if (key_len > 0) {
    memcpy(k, token, key_len);
  }
  /* HMAC over label ":" Nc ":" Nd, streamed (no message buffer to size) */
  uint8_t pad[64], inner[LACE_SHA256_LEN], mac[LACE_SHA256_LEN];
  for (int i = 0; i < 64; i++) {
    pad[i] = k[i] ^ 0x36;
  }
  sha256_init(&s);
  sha256_update(&s, pad, 64);
  sha256_update(&s, label, strlen(label));
  sha256_update(&s, ":", 1);
  sha256_update(&s, client_nonce, strlen(client_nonce));
  sha256_update(&s, ":", 1);
  sha256_update(&s, daemon_nonce, strlen(daemon_nonce));
  sha256_final(&s, inner);
  for (int i = 0; i < 64; i++) {
    pad[i] = k[i] ^ 0x5c;
  }
  sha256_init(&s);
  sha256_update(&s, pad, 64);
  sha256_update(&s, inner, sizeof(inner));
  sha256_final(&s, mac);

  static const char hex[] = "0123456789abcdef";
  for (int i = 0; i < LACE_SHA256_LEN; i++) {
    out[2 * i] = hex[mac[i] >> 4];
    out[2 * i + 1] = hex[mac[i] & 15];
  }
  out[2 * LACE_SHA256_LEN] = '\0';
  memset(k, 0, sizeof(k));
  memset(pad, 0, sizeof(pad));
  memset(inner, 0, sizeof(inner));
  memset(mac, 0, sizeof(mac));
}

bool lace_secure_equal(const char *a, const char *b) {
  size_t la = strlen(a), lb = strlen(b);
  unsigned char diff = (unsigned char)(la != lb);
  for (size_t i = 0; i < la && i < lb; i++) {
    diff |= (unsigned char)(a[i] ^ b[i]);
  }
  return diff == 0;
}
