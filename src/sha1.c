#include "sha1.h"

#include <string.h>

static uint32_t rol(uint32_t v, int bits) { return (v << bits) | (v >> (32 - bits)); }

static void sha1_block(esp32git_sha1 *ctx, const uint8_t block[64]) {
  uint32_t w[80];
  for (int i = 0; i < 16; i++) {
    w[i] = ((uint32_t)block[i * 4] << 24) | ((uint32_t)block[i * 4 + 1] << 16) |
           ((uint32_t)block[i * 4 + 2] << 8) | (uint32_t)block[i * 4 + 3];
  }
  for (int i = 16; i < 80; i++) {
    w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
  }

  uint32_t a = ctx->state[0], b = ctx->state[1], c = ctx->state[2], d = ctx->state[3],
           e = ctx->state[4];
  for (int i = 0; i < 80; i++) {
    uint32_t f, k;
    if (i < 20) {
      f = (b & c) | ((~b) & d);
      k = 0x5A827999;
    } else if (i < 40) {
      f = b ^ c ^ d;
      k = 0x6ED9EBA1;
    } else if (i < 60) {
      f = (b & c) | (b & d) | (c & d);
      k = 0x8F1BBCDC;
    } else {
      f = b ^ c ^ d;
      k = 0xCA62C1D6;
    }
    const uint32_t temp = rol(a, 5) + f + e + k + w[i];
    e = d;
    d = c;
    c = rol(b, 30);
    b = a;
    a = temp;
  }
  ctx->state[0] += a;
  ctx->state[1] += b;
  ctx->state[2] += c;
  ctx->state[3] += d;
  ctx->state[4] += e;
}

void esp32git_sha1_init(esp32git_sha1 *ctx) {
  ctx->state[0] = 0x67452301;
  ctx->state[1] = 0xEFCDAB89;
  ctx->state[2] = 0x98BADCFE;
  ctx->state[3] = 0x10325476;
  ctx->state[4] = 0xC3D2E1F0;
  ctx->total_bits = 0;
  ctx->buffered = 0;
}

void esp32git_sha1_update(esp32git_sha1 *ctx, const void *data, size_t len) {
  const uint8_t *p = data;
  ctx->total_bits += (uint64_t)len * 8;
  if (ctx->buffered > 0) {
    const size_t take = (64 - ctx->buffered < len) ? 64 - ctx->buffered : len;
    memcpy(ctx->buffer + ctx->buffered, p, take);
    ctx->buffered += take;
    p += take;
    len -= take;
    if (ctx->buffered == 64) {
      sha1_block(ctx, ctx->buffer);
      ctx->buffered = 0;
    }
  }
  while (len >= 64) {
    sha1_block(ctx, p);
    p += 64;
    len -= 64;
  }
  if (len > 0) {
    memcpy(ctx->buffer, p, len);
    ctx->buffered = len;
  }
}

void esp32git_sha1_final(esp32git_sha1 *ctx, uint8_t digest[20]) {
  const uint64_t total_bits = ctx->total_bits;
  const uint8_t pad = 0x80;
  esp32git_sha1_update(ctx, &pad, 1);
  const uint8_t zero = 0;
  while (ctx->buffered != 56) {
    esp32git_sha1_update(ctx, &zero, 1);
  }
  const uint8_t tail[8] = {
      (uint8_t)(total_bits >> 56), (uint8_t)(total_bits >> 48), (uint8_t)(total_bits >> 40),
      (uint8_t)(total_bits >> 32), (uint8_t)(total_bits >> 24), (uint8_t)(total_bits >> 16),
      (uint8_t)(total_bits >> 8),  (uint8_t)total_bits,
  };
  esp32git_sha1_update(ctx, tail, 8);
  for (int i = 0; i < 5; i++) {
    digest[i * 4] = (uint8_t)(ctx->state[i] >> 24);
    digest[i * 4 + 1] = (uint8_t)(ctx->state[i] >> 16);
    digest[i * 4 + 2] = (uint8_t)(ctx->state[i] >> 8);
    digest[i * 4 + 3] = (uint8_t)ctx->state[i];
  }
}
