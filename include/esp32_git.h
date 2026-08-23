#pragma once

// esp32-git: minimal git client for ESP32 + SD card.
// Loose objects only; fast-forward-only push/pull; file transport now,
// smart-HTTP transport planned; verified against stock git.

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
  const char *url;   // https://host/org/repo.git (HTTP transport, planned)
  const char *user;  // basic auth user (often a token name)
  const char *token; // basic auth password
} esp32git_remote;

typedef struct {
  const char *name;
  const char *email;
} esp32git_identity;

// ---- object model ---------------------------------------------------------

esp32git_status esp32git_object_write(const char *repo_path, const char *type,
                                      const void *payload, size_t len,
                                      char out_sha[41]);

esp32git_status esp32git_object_read(const char *repo_path, const char *sha,
                                     char *out_type, size_t type_cap,
                                     void *out, size_t out_cap, size_t *out_len);

// Loose-object file path for sha, accepting both layouts:
// <repo>/.git/objects/xx/yyyy (checkout) and <repo>/objects/xx/yyyy (bare).
// Returns false when the file exists at neither.
int esp32git_object_path(const char *repo_path, const char *sha, char *out,
                         size_t cap);

// ---- repo lifecycle -------------------------------------------------------

// Creates .git/ skeleton under workdir (objects, refs/heads, HEAD -> main).
esp32git_status esp32git_init(const char *workdir);

// Stages one worktree path (missing path stages its deletion).
esp32git_status esp32git_add(const char *repo_path, const char *relpath);

// Classifies a path: 'S' staged change, 'U' unstaged change,
// '?' untracked, 'D' staged deletion, '=' clean.
esp32git_status esp32git_status_file(const char *repo_path, const char *relpath,
                                     char *out_state);

// Writes tree + commit objects from the staged index and advances HEAD.
esp32git_status esp32git_commit(const char *repo_path, const esp32git_identity *id,
                                const char *message, char out_sha[41]);

// ---- file-transport sync (remote is a plain directory with .git/) ---------

// Fetches branch objects into repo_path and fast-forwards the local ref.
// A diverged remote fails with REMOTE_DIVERGED; up-to-date returns UP_TO_DATE.
esp32git_status esp32git_fetch(const char *remote_dir, const char *branch,
                               const char *repo_path);

// Pushes local HEAD's objects to remote branch; fast-forward only.
esp32git_status esp32git_push(const char *remote_dir, const char *branch,
                              const char *repo_path);

// init + fetch + materialize the worktree and staging index at remote HEAD.
esp32git_status esp32git_clone(const char *remote_dir, const char *branch,
                               const char *workdir);

#ifdef __cplusplus
}
#endif
