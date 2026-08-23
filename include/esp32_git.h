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

// ---- storage port -----------------------------------------------------------
// Register on-device to back all library file access with an SD-card HAL
// (e.g. HalStorage). Leave NULL members or skip registration to use the
// built-in host stdio backend. Call before any other API; single-threaded.
typedef struct esp32git_fs_port {
  // Byte size of a regular file, -1 when missing/unreadable.
  int64_t (*size)(const char *path);
  // Reads up to cap bytes into buf; sets *out_len; 0 ok, nonzero error.
  int (*read)(const char *path, uint8_t *buf, size_t cap, size_t *out_len);
  // Overwrites path with len bytes; 0 ok.
  int (*write)(const char *path, const uint8_t *data, size_t len);
  // 1 when path is an existing regular file.
  int (*exists)(const char *path);
  // Creates a directory chain itself (like mkdir -p); 0 ok.
  int (*make_dirs)(const char *dir_chain);
} esp32git_fs_port;

void esp32git_fs_register(const esp32git_fs_port *port);

// ---- object model ---------------------------------------------------------

// Loose-object file path for sha, accepting both layouts:
// <repo>/.git/objects/xx/yyyy (checkout) and <repo>/objects/xx/yyyy (bare).
// Returns false when the file exists at neither.
int esp32git_object_path(const char *repo_path, const char *sha, char *out,
                         size_t cap);

esp32git_status esp32git_object_write(const char *repo_path, const char *type,
                                      const void *payload, size_t len,
                                      char out_sha[41]);

esp32git_status esp32git_object_read(const char *repo_path, const char *sha,
                                     char *out_type, size_t type_cap,
                                     void *out, size_t out_cap, size_t *out_len);

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

// Fetches branch objects into repo_path and fast-forwards the local ref,
// materializing the worktree after a fast-forward. A diverged remote fails
// with REMOTE_DIVERGED; an unchanged remote returns UP_TO_DATE.
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
