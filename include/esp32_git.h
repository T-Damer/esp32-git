#pragma once

// esp32-git: minimal git client for ESP32 + SD card.
// Loose objects only; fast-forward-only push/pull; file and smart-HTTP
// transports; verified against stock git.

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
  const char *url;   // https://host/org/repo.git (HTTP transport)
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
typedef struct esp32git_file_port {
  // Opens a regular file. write=0 reads from the start; write=1 truncates and
  // opens a read/write file. The returned handle belongs to the caller.
  void *(*open)(const char *path, int write);
  // Reads up to cap bytes from the current position; sets *out_len.
  int (*read)(void *handle, uint8_t *buf, size_t cap, size_t *out_len);
  // Writes len bytes at the current position.
  int (*write)(void *handle, const uint8_t *data, size_t len);
  // Moves the current position to an absolute byte offset.
  int (*seek)(void *handle, uint64_t offset);
  // Closes and releases the handle.
  int (*close)(void *handle);
} esp32git_file_port;

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
  // Optional streaming file operations. Old ports may leave this member NULL.
  esp32git_file_port file;
  // Removes a regular file; 0 ok. Required for temporary pack cleanup.
  int (*remove)(const char *path);
  // Optional atomic replacement for verified on-demand downloads.
  int (*rename)(const char *from, const char *to);
  // Optional: calls entry(ctx, name, is_dir) for each child of dir; 0 ok.
  // Without it, esp32git_sync_url cannot notice files created on the device.
  int (*list_dir)(const char *dir,
                  void (*entry)(void *ctx, const char *name, int is_dir),
                  void *ctx);
  // Optional: size and modification time (any unit that changes on write);
  // 0 ok. Without it, esp32git_sync_url hashes every local file to find edits.
  int (*stat)(const char *path, int64_t *size, int64_t *mtime);
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

// ---- smart-HTTP transport ---------------------------------------------------
// Buffer-based; response bodies are allocated by the registered port and
// released through esp32git_free_buffer. Used for advertisements and push
// responses, and as a fallback when streaming is unavailable.
typedef int (*esp32git_http_write_callback)(void *context,
                                            const uint8_t *data, size_t len);

typedef struct esp32git_http_port {
  // Returns HTTP status (200..599) or negative transport error; 401/403 map
  // to AUTH_FAILED. is_post selects GET/POST; user/token enable basic auth.
  int (*request)(const char *url, int is_post, const char *user,
                 const char *token, const char *content_type,
                 const uint8_t *body, size_t body_len, uint8_t **out_body,
                 size_t *out_len);
  // Streams the response body to write(context, data, len). The callback
  // returns 0 to continue or nonzero to abort. out_len is bytes delivered.
  // Returns the HTTP status or negative transport/callback error.
  int (*request_stream)(const char *url, int is_post, const char *user,
                        const char *token, const char *content_type,
                        const uint8_t *body, size_t body_len,
                        esp32git_http_write_callback write,
                        void *context, size_t *out_len);
} esp32git_http_port;

void esp32git_http_register(const esp32git_http_port *port);
void esp32git_free_buffer(uint8_t *body);

esp32git_status esp32git_fetch_url(const char *remote_url, const char *branch,
                                   const char *repo_path);
esp32git_status esp32git_fetch_url_auth(const char *remote_url, const char *branch,
                                        const char *repo_path,
                                        const esp32git_remote *auth);
esp32git_status esp32git_push_url(const char *remote_url, const char *branch,
                                  const char *repo_path);
esp32git_status esp32git_push_url_auth(const char *remote_url, const char *branch,
                                       const char *repo_path,
                                       const esp32git_remote *auth);
esp32git_status esp32git_clone_url(const char *remote_url, const char *branch,
                                   const char *workdir,
                                   const esp32git_remote *auth);

// Shallow partial clone/fetch: current commit and trees, without file blobs.
// All files stay in the index and can be downloaded by path later.
// Returns PROTOCOL_ERROR if the server does not advertise shallow + filter.
esp32git_status esp32git_clone_url_partial(const char *remote_url,
                                           const char *branch,
                                           const char *workdir,
                                           const esp32git_remote *auth);
esp32git_status esp32git_fetch_url_partial(const char *remote_url,
                                           const char *branch,
                                           const char *repo_path,
                                           const esp32git_remote *auth);

// Download one omitted blob from HEAD by its Git path into the worktree.
// Refuses to overwrite an existing local file. The server must allow wants
// for reachable object IDs (as Git partial-clone servers do).
esp32git_status esp32git_download_path_url(const char *remote_url,
                                           const char *repo_path,
                                           const char *relpath,
                                           const esp32git_remote *auth);

// Complete omitted Markdown/TXT notes after a partial clone without fetching
// large attachments or books. Can be retried after an interrupted download.
esp32git_status esp32git_download_missing_notes_url(const char *remote_url,
                                                    const char *repo_path,
                                                    const esp32git_remote *auth);

// One-time full worktree completion, including large attachments. This can
// transfer hundreds of megabytes; callers should expose it as a manual action.
esp32git_status esp32git_download_missing_files_url(const char *remote_url,
                                                    const char *repo_path,
                                                    const esp32git_remote *auth);

// ---- batched downloads and two-way sync --------------------------------------

// Returns nonzero for worktree paths that should be downloaded.
typedef int (*esp32git_path_filter)(void *ctx, const char *relpath);
// Reports progress; `done` of `total` files, `path` the latest one (may be NULL).
typedef void (*esp32git_progress_fn)(void *ctx, size_t done, size_t total,
                                     const char *path);

// Downloads every omitted file whose path passes `filter` (NULL = all),
// several per request, retrying transient network failures. Files larger than
// a pack entry allows fall back to one streamed request each. Resumable: an
// interrupted call can simply be repeated.
esp32git_status esp32git_download_missing_matching_url(
    const char *remote_url, const char *repo_path, const esp32git_remote *auth,
    esp32git_path_filter filter, void *filter_ctx,
    esp32git_progress_fn progress, void *progress_ctx);

// Markdown and text files: the filter esp32git_download_missing_notes_url uses.
int esp32git_filter_notes(void *ctx, const char *relpath);

typedef struct {
  const char *branch;                  // NULL = "main"
  const esp32git_identity *identity;   // author of commits made on the device
  const char *message;                 // commit message for local changes
  // Names conflict copies: "<name> (conflict <tag> <time>).<ext>"; NULL = "device".
  const char *conflict_tag;
  // Files downloaded after each fetch (NULL = esp32git_filter_notes).
  esp32git_path_filter download_filter;
  void *download_filter_ctx;
  esp32git_progress_fn progress;
  void *progress_ctx;
} esp32git_sync_options;

typedef struct {
  unsigned pushed;      // files added, changed or deleted by this device
  unsigned conflicts;   // local edits saved as conflict copies
  unsigned kept_remote; // local deletions skipped because the remote changed the file
  char last_conflict[256]; // worktree path of the latest conflict copy
} esp32git_sync_report;

// Two-way sync of a shallow partial clone (created by the first call):
//  1. finds files created, edited or deleted on the device since the last sync
//     (no caller bookkeeping needed);
//  2. sets them aside, fast-forwards to the remote and downloads new files;
//  3. reapplies the local changes: a file the remote left untouched takes the
//     local version, a file changed on both sides keeps the remote version and
//     gets the local one as a conflict copy, so nothing is lost;
//  4. commits and pushes, retrying when the remote moves in between.
// Local changes survive any failure and are retried by the next call.
esp32git_status esp32git_sync_url(const char *remote_url, const char *repo_path,
                                  const esp32git_remote *auth,
                                  const esp32git_sync_options *options,
                                  esp32git_sync_report *report);

#ifdef __cplusplus
}
#endif
