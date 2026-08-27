// ponytail: stored-only deflate; swap in tdefl_static.c from upstream uzlib
// if note payloads ever make compression worth its RAM.
#include "zlib.h"

#include <string.h>

#include "uzlib.h"

size_t esp32git_zlib_stored_bound(size_t in_len) {
  return 2 + in_len + (in_len / 65535) + 5 + 4;
}

static void put16(uint8_t *out, size_t *at, uint16_t v) {
  out[(*at)++] = (uint8_t)(v & 0xff);
  out[(*at)++] = (uint8_t)(v >> 8);
}

int esp32git_zlib_deflate_stored(const uint8_t *in, size_t in_len, uint8_t *out,
                                 size_t out_cap) {
  if (out_cap < esp32git_zlib_stored_bound(in_len)) return -1;
  // Adler-32 of the uncompressed data, big-endian trailer (RFC 1950).
  uint32_t a = 1, b = 0;
  for (size_t i = 0; i < in_len; i++) {
    a = (a + in[i]) % 65521;
    b = (b + a) % 65521;
  }
  const uint32_t adler = (b << 16) | a;

  size_t at = 0;
  // CMF=0x78 (32K window), FLG=0x01 (fastest, no dict).
  out[at++] = 0x78;
  out[at++] = 0x01;
  const uint8_t *p = in;
  size_t remaining = in_len;
  do {
    const size_t chunk = (remaining > 65535) ? 65535 : remaining;
    out[at++] = (chunk == remaining) ? 1 : 0;
    put16(out, &at, (uint16_t)chunk);
    put16(out, &at, (uint16_t)(~(uint16_t)chunk));
    memcpy(out + at, p, chunk);
    at += chunk;
    p += chunk;
    remaining -= chunk;
  } while (remaining > 0);
  out[at++] = (uint8_t)(adler >> 24);
  out[at++] = (uint8_t)(adler >> 16);
  out[at++] = (uint8_t)(adler >> 8);
  out[at++] = (uint8_t)adler;
  return (int)at;
}

long esp32git_zlib_inflate(const uint8_t *in, size_t in_len, uint8_t *out,
                           size_t out_cap) {
  struct uzlib_uncomp d;
  uzlib_uncompress_init(&d, NULL, 0);
  d.source = in;
  d.source_limit = in + in_len;
  d.dest_start = out;
  d.dest = out;
  d.dest_limit = out + out_cap;

  int rc = zlib_parse_header(&d);
  if (rc != TINF_OK) return rc;
  rc = uzlib_uncompress_chksum(&d); // validates the Adler-32 trailer
  if (rc != TINF_DONE) return (rc == TINF_OK) ? TINF_DATA_ERROR : rc;
  return (long)(d.dest - out);
}

// The vendored tinflate.c omits the RFC 1950 preamble parser; a minimal one:
// validate CMF/FLG, reject dictionaries, arm the Adler-32 accumulator.
int zlib_parse_header(struct uzlib_uncomp *d) {
  const unsigned char cmf = (unsigned char)uzlib_get_byte(d);
  const unsigned char flg = (unsigned char)uzlib_get_byte(d);
  if ((cmf & 0x0f) != 8) return TINF_DATA_ERROR;            // only method 8
  if (((cmf << 8) + flg) % 31 != 0) return TINF_DATA_ERROR; // header checksum
  if (flg & 0x20) return TINF_DATA_ERROR;                   // FDICT unsupported
  d->checksum_type = TINF_CHKSUM_ADLER;
  d->checksum = 1;
  return TINF_OK;
}

// The vendored tinflate.c references uzlib_crc32 for the gzip checksum path
// but ships without it; provide the standard table-less implementation.
__attribute__((weak)) uint32_t uzlib_crc32(const void *data, unsigned int length, uint32_t crc) {
  const unsigned char *p = data;
  while (length--) {
    crc ^= *p++;
    for (int i = 0; i < 8; i++) {
      crc = (crc >> 1) ^ (0xedb88320u & (-(int)(crc & 1)));
    }
  }
  return crc;
}

// Also stripped from the vendored tinflate.c: the Adler-32 accumulator.
__attribute__((weak)) uint32_t uzlib_adler32(const void *data, unsigned int length, uint32_t checksum) {
  const unsigned char *p = data;
  uint32_t s1 = checksum & 0xffff;
  uint32_t s2 = checksum >> 16;
  while (length--) {
    s1 = (s1 + *p++) % 65521;
    s2 = (s2 + s1) % 65521;
  }
  return (s2 << 16) | s1;
}
