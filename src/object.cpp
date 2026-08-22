#include "esp32_git.h"

#include <sys/stat.h>

#include <cstdio>
#include <cstring>
#include <new>

#include "sha1.h"
#include "zlib.h"

namespace {

// ponytail: stdio + POSIX mkdir on host for now; the ESP32 build swaps this
// file layer for HalStorage-backed calls behind the same signatures.
int write_file(const char *path, const uint8_t *data, size_t len) {
  FILE *f = fopen(path, "wb");
  if (!f) return -1;
  const size_t wrote = fwrite(data, 1, len, f);
  fclose(f);
  return wrote == len ? 0 : -1;
}

// Reads the whole file; returns 0 and sets *len, -1 if missing or larger than cap.
int read_file(const char *path, uint8_t *data, size_t cap, size_t *len) {
  FILE *f = fopen(path, "rb");
  if (!f) return -1;
  const size_t got = fread(data, 1, cap, f);
  const bool at_eof = (fgetc(f) == EOF);
  fclose(f);
  if (!at_eof) return -1;
  *len = got;
  return 0;
}

void hex_digest(const uint8_t digest[20], char out[41]) {
  static const char kHex[] = "0123456789abcdef";
  for (int i = 0; i < 20; i++) {
    out[i * 2] = kHex[digest[i] >> 4];
    out[i * 2 + 1] = kHex[digest[i] & 0xf];
  }
  out[40] = '\0';
}

} // namespace

esp32git_status esp32git_object_write(const char *repo_path, const char *type,
                                      const void *payload, size_t len,
                                      char out_sha[41]) {
  if (!repo_path || !type || !out_sha || !payload) return ESP32GIT_IO_ERROR;

  // Object = "<type> <size>\0" + payload.
  char header[32];
  const int header_len = snprintf(header, sizeof(header), "%s %zu", type, len);
  if (header_len <= 0 || (size_t)header_len >= sizeof(header)) return ESP32GIT_IO_ERROR;

  esp32git_sha1 sha;
  esp32git_sha1_init(&sha);
  esp32git_sha1_update(&sha, header, (size_t)header_len + 1); // includes NUL
  esp32git_sha1_update(&sha, payload, len);
  uint8_t digest[20];
  esp32git_sha1_final(&sha, digest);
  hex_digest(digest, out_sha);

  const size_t raw_len = (size_t)header_len + 1 + len;
  uint8_t raw_header[64];
  memcpy(raw_header, header, (size_t)header_len + 1);

  // Compress header and payload in one stream: assemble them contiguously.
  uint8_t *raw = new (std::nothrow) uint8_t[raw_len];
  if (!raw) return ESP32GIT_OUT_OF_MEMORY;
  memcpy(raw, header, (size_t)header_len + 1);
  memcpy(raw + header_len + 1, payload, len);

  const size_t bound = esp32git_zlib_stored_bound(raw_len);
  uint8_t *compressed = new (std::nothrow) uint8_t[bound];
  if (!compressed) {
    delete[] raw;
    return ESP32GIT_OUT_OF_MEMORY;
  }
  const int compressed_len = esp32git_zlib_deflate_stored(raw, raw_len, compressed, bound);
  delete[] raw;
  if (compressed_len <= 0) {
    delete[] compressed;
    return ESP32GIT_OUT_OF_MEMORY;
  }

  char dir[512];
  snprintf(dir, sizeof(dir), "%s/.git/objects/%c%c", repo_path, out_sha[0], out_sha[1]);
  mkdir(dir, 0777); // exists_ok

  char path[576];
  snprintf(path, sizeof(path), "%s/%s", dir, out_sha + 2);
  if (write_file(path, compressed, (size_t)compressed_len) != 0) {
    delete[] compressed;
    return ESP32GIT_IO_ERROR;
  }
  delete[] compressed;
  return ESP32GIT_OK;
}

esp32git_status esp32git_object_read(const char *repo_path, const char *sha,
                                     char *out_type, size_t type_cap,
                                     void *out, size_t out_cap, size_t *out_len) {
  if (!repo_path || !sha || strlen(sha) != 40) return ESP32GIT_INVALID_REF;

  char path[576];
  snprintf(path, sizeof(path), "%s/.git/objects/%c%c/%s", repo_path, sha[0], sha[1],
           sha + 2);

  // Size the file first so one read suffices.
  FILE *f = fopen(path, "rb");
  if (!f) return ESP32GIT_INVALID_REF; // object does not exist
  fseek(f, 0, SEEK_END);
  const long file_size = ftell(f);
  fclose(f);
  if (file_size <= 0) return ESP32GIT_IO_ERROR;

  uint8_t *compressed = new (std::nothrow) uint8_t[(size_t)file_size];
  if (!compressed) return ESP32GIT_OUT_OF_MEMORY;
  size_t stored_len = 0;
  if (read_file(path, compressed, (size_t)file_size, &stored_len) != 0) {
    delete[] compressed;
    return ESP32GIT_IO_ERROR;
  }

  // Inflate into caller's buffer; require room for the header too.
  const long raw_len = esp32git_zlib_inflate(compressed, stored_len, (uint8_t *)out, out_cap);
  delete[] compressed;
  if (raw_len < 0) return ESP32GIT_PROTOCOL_ERROR;

  // Parse "<type> <size>\0".
  const char *raw = (const char *)out;
  const char *nul = (const char *)memchr(raw, '\0', (size_t)raw_len);
  if (!nul) return ESP32GIT_PROTOCOL_ERROR;
  const size_t header_len = (size_t)(nul - raw);
  unsigned long long declared_len = 0;
  char parsed_type[16];
  if (sscanf(raw, "%15s %llu", parsed_type, &declared_len) != 2) return ESP32GIT_PROTOCOL_ERROR;
  if ((size_t)header_len + 1 + declared_len != (size_t)raw_len) return ESP32GIT_PROTOCOL_ERROR;

  // Verify the SHA-1 id against the content (catches any corruption Adler misses).
  esp32git_sha1 sha_ctx;
  esp32git_sha1_init(&sha_ctx);
  esp32git_sha1_update(&sha_ctx, raw, header_len + 1);
  esp32git_sha1_update(&sha_ctx, nul + 1, (size_t)declared_len);
  uint8_t digest[20];
  esp32git_sha1_final(&sha_ctx, digest);
  char check[41];
  hex_digest(digest, check);
  if (memcmp(check, sha, 40) != 0) return ESP32GIT_PROTOCOL_ERROR;

  // Move the payload over the header inside the caller's buffer.
  memmove(out, nul + 1, (size_t)declared_len);
  if (out_type) snprintf(out_type, type_cap, "%s", parsed_type);
  if (out_len) *out_len = (size_t)declared_len;
  return ESP32GIT_OK;
}
