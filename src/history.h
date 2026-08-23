#pragma once

#include "esp32_git.h"

#include "repo.h"

#ifdef __cplusplus
extern "C" {
#endif

// Builds all necessary subtree objects for the staged paths and writes the
// root tree; out receives its 40-char id.
esp32git_status esp32git_write_tree(const char *repo_path,
                                    const std::vector<esp32git_index_entry> &entries,
                                    char out_sha[41]);

// Looks up the blob id of relpath inside tree_sha ("", if absent).
esp32git_status esp32git_tree_lookup(const char *repo_path,
                                     const char *tree_sha, const char *relpath,
                                     char out_blob[41]);

// Root tree id of a commit object.
esp32git_status esp32git_head_tree(const char *repo_path, const char *commit_sha,
                                   char out_tree[41]);

// Writes a commit object for tree_sha on top of parent_sha ("" for none).
esp32git_status esp32git_commit_create(const char *repo_path,
                                       const esp32git_identity &id,
                                       const char *message, const char *tree_sha,
                                       const char *parent_sha,
                                       char out_commit[41]);

// True when ancestor is reachable from descendant through parent links.
bool esp32git_is_ancestor(const char *repo_path, const char *ancestor,
                          const char *descendant);

#ifdef __cplusplus
}
#endif
