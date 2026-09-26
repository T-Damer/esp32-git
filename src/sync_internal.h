#pragma once

// Internal cross-module helper surface.

#include "esp32_git.h"

#include <string>

namespace e32g {

// Materializes worktree files + staging index at a tree id (sync_file.cpp).
esp32git_status checkout_tree_at(const char *repo_path, const char *tree_sha);
esp32git_status checkout_tree_partial_at(const char *repo_path,
                                         const char *tree_sha);
// Rewrites the staging index from a tree without touching worktree files.
esp32git_status index_from_tree(const char *repo_path, const char *tree_sha);
// Git blob id of a worktree file, streamed.
bool hash_worktree_file(const std::string &path, char out[41]);

// Smart-HTTP operations shared with the sync driver (http.cpp).
esp32git_status fetch_url(const char *remote_url, const char *branch,
                          const char *repo_path, const esp32git_remote &auth,
                          bool partial);
esp32git_status push_url(const char *remote_url, const char *branch,
                         const char *repo_path, const esp32git_remote &auth);
esp32git_status download_matching_url(const char *remote_url, const char *repo_path,
                                      const esp32git_remote &auth,
                                      esp32git_path_filter filter, void *filter_ctx,
                                      esp32git_progress_fn progress, void *progress_ctx);

} // namespace e32g
