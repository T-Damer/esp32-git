#pragma once

// zlib framing over uzlib: stored-blocks writer (valid zlib, no compression)
// and a checksummed inflate reader.

#include <stddef.h>
#include <stdint.h>

#include "uzlib.h"

#ifdef __cplusplus
extern "C" {
#endif

// Upper bound of the stored-blocks encoding for in_len bytes.
size_t esp32git_zlib_stored_bound(size_t in_len);

// Writes a valid zlib stream containing in_len bytes uncompressed.
// Returns the encoded length, or -1 if out_cap is too small.
int esp32git_zlib_deflate_stored(const uint8_t *in, size_t in_len, uint8_t *out,
                                 size_t out_cap);

// Parses a zlib header (CMF/FLG), arming the checksum accumulator. Provided
// because the vendored tinflate.c omits it.
int zlib_parse_header(struct uzlib_uncomp *d);

// Inflates a zlib stream. Returns the output length (>= 0) or a negative
// TINF_* error. The stream's Adler-32 checksum is verified.
long esp32git_zlib_inflate(const uint8_t *in, size_t in_len, uint8_t *out,
                           size_t out_cap);

#ifdef __cplusplus
}
#endif
