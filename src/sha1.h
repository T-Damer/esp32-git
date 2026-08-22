#pragma once

// Minimal streaming SHA-1 (FIPS 180-1). Self-contained; no allocation.

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  uint32_t state[5];
  uint64_t total_bits;
  uint8_t buffer[64];
  size_t buffered;
} esp32git_sha1;

void esp32git_sha1_init(esp32git_sha1 *ctx);
void esp32git_sha1_update(esp32git_sha1 *ctx, const void *data, size_t len);
// Writes 20 raw bytes; ctx is reset and may be reused.
void esp32git_sha1_final(esp32git_sha1 *ctx, uint8_t digest[20]);

#ifdef __cplusplus
}
#endif
