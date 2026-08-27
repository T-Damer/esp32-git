#pragma once

// Internal cross-module helper surface.

#include "esp32_git.h"

namespace e32g {

// Materializes worktree files + staging index at a tree id (sync_file.cpp).
esp32git_status checkout_tree_at(const char *repo_path, const char *tree_sha);

} // namespace e32g
