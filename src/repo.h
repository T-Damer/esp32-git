#pragma once

// Repo layout helpers: refs, HEAD, staging index, and worktree access.
//
// The staging index is a sidecar text file (.git/esp32git-index), one
// "<sha40> <path>\n" per line sorted by path. Stock git ignores unknown files
// inside .git/, so a PC can keep working on the same repository; upgrading to
// git's native binary index format later only touches this module.
// ponytail: sidecar index until a user actually asks for native-index parity.

#include "esp32_git.h"

#include <string>
#include <vector>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  std::string path;
  std::string sha;
} esp32git_index_entry;

// Creates .git/ skeleton (objects, refs/heads, HEAD -> refs/heads/main,
// empty index). Returns NOT_A_REPO when workdir cannot be created.
esp32git_status esp32git_repo_init(const char *workdir);

// Branch helpers. Default branch is "main".
std::string esp32git_branch_ref(const char *branch); // "refs/heads/main"
esp32git_status esp32git_read_ref(const char *repo_path, const char *refname,
                                  char out_sha[41]); // INVALID_REF if absent
esp32git_status esp32git_write_ref(const char *repo_path, const char *refname,
                                   const char *sha);
// Reads symbolic HEAD ("ref: ...") and resolves it through refs/heads/.
esp32git_status esp32git_resolve_head(const char *repo_path, char out_sha[41]);

// ---- staging index --------------------------------------------------------

esp32git_status esp32git_index_load(const char *repo_path,
                                    std::vector<esp32git_index_entry> *out);
esp32git_status esp32git_index_save(const char *repo_path,
                                    const std::vector<esp32git_index_entry> &entries);
esp32git_status esp32git_add(const char *repo_path, const char *relpath);

// Classifies one path against the staging index and its worktree contents:
// 'S' staged change, 'U' unstaged change, '?' untracked, '=' clean,
// 'D' staged deletion.
esp32git_status esp32git_status_file(const char *repo_path, const char *relpath,
                                     char *out_state);

#ifdef __cplusplus
}
#endif
