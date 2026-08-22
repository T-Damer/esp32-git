#pragma once

// esp32-git: minimal git client for ESP32 + SD card.
// Loose objects only; fast-forward-only push/pull; smart-HTTP transport.
//
// Pure C API so it links into ESP-IDF/Arduino builds unchanged; the
// PlatformIO registration maps file I/O onto HalStorage later.

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  ESP32GIT_OK = 0,
  ESP32GIT_IO_ERROR,
  ESP32GIT_NOT_A_REPO,
  ESP32GIT_REMOTE_DIVERGED, // would require merge/rebase - unsupported by design
  ESP32GIT_UP_TO_DATE,
  ESP32GIT_AUTH_FAILED,
  ESP32GIT_PROTOCOL_ERROR,
  ESP32GIT_OUT_OF_MEMORY,
  ESP32GIT_INVALID_REF,
} esp32git_status;

typedef struct {
  const char *url;   // https://host/org/repo.git
  const char *user;  // basic auth user (often a token name)
  const char *token; // basic auth password
} esp32git_remote;

typedef struct {
  const char *name;
  const char *email;
} esp32git_identity;

// ---- object model ---------------------------------------------------------

// Hash and write a loose object under <repo_path>/.git/objects/xx/yyyy...
// type is "blob", "tree", or "commit"; payload may be any bytes.
// out_sha receives the 40-char lowercase hex object id.
esp32git_status esp32git_object_write(const char *repo_path, const char *type,
                                      const void *payload, size_t len,
                                      char out_sha[41]);

// Read a loose object by hex sha. out_type receives the stored type string.
// Returns OK; INVALID_REF when the object does not exist; IO_ERROR on storage
// failure; PROTOCOL_ERROR when the Adler-32 trailer fails validation.
esp32git_status esp32git_object_read(const char *repo_path, const char *sha,
                                     char *out_type, size_t type_cap,
                                     void *out, size_t out_cap, size_t *out_len);

#ifdef __cplusplus
}
#endif
