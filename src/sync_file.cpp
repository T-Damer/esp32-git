#include "esp32_git.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "hexutil.h"
#include "history.h"
#include "io.h"
#include "repo.h"
#include "sha1.h"

namespace {

constexpr size_t kMaxObjectBytes = 64 * 1024;

// Works for normal checkouts (<repo>/.git/...) and bare origins (<repo>/...).
std::string object_path(const char *repo, const char *sha) {
  const std::string sub = std::string(sha).substr(0, 2);
  const std::string rest = std::string(sha).substr(2);
  const std::string nested = std::string(repo) + "/.git/objects/" + sub + "/" + rest;
  if (!e32g::exists(nested)) {
    return std::string(repo) + "/objects/" + sub + "/" + rest;
  }
  return nested;
}

bool object_exists(const char *repo, const char *sha) {
  return e32g::exists(object_path(repo, sha));
}

// Copies a loose object file verbatim between repos (no decode needed).
bool copy_object(const char *src_repo, const char *dst_repo, const char *sha) {
  if (object_exists(dst_repo, sha)) return true;
  std::vector<uint8_t> data;
  if (!e32g::read_whole(object_path(src_repo, sha), data)) return false;

  const std::string sub = std::string(sha).substr(0, 2);
  const std::string rest = std::string(sha).substr(2);
  // Bare destination keeps objects directly under <repo>/objects/...
  const bool dst_is_bare =
      e32g::exists(std::string(dst_repo) + "/HEAD") &&
      !e32g::exists(std::string(dst_repo) + "/.git/HEAD");
  const std::string base =
      dst_is_bare ? std::string(dst_repo) : std::string(dst_repo) + "/.git";
  const std::string dst_dir = base + "/objects/" + sub;
  if (!e32g::make_dirs(dst_dir)) return false;
  return e32g::write_whole(dst_dir + "/" + rest, data.data(), data.size());
}

// Copies every object reachable from a commit (commit -> tree -> subtrees ->
// blobs, plus ancestors through parent links). Returns objects copied.
long copy_reachable(const char *from_repo, const char *head_sha,
                    const char *to_repo) {
  long copied = 0;
  std::vector<std::string> commits{head_sha};
  size_t visited = 0;
  char type[16];
  std::vector<char> buf(kMaxObjectBytes);

  while (!commits.empty() && visited++ < 4096) { // ponytail: bounded walk
    const std::string commit = commits.back();
    commits.pop_back();
    if (!copy_object(from_repo, to_repo, commit.c_str())) continue;
    copied++;

    // Parse this commit fully before any other read reuses the buffer.
    size_t len = 0;
    if (esp32git_object_read(from_repo, commit.c_str(), type, sizeof(type),
                             buf.data(), buf.size(), &len) != ESP32GIT_OK) {
      continue;
    }
    char root_tree[41] = "";
    sscanf(buf.data(), "tree %40s", root_tree);
    std::vector<std::string> parents;
    for (const char *line = buf.data(); line < buf.data() + len;) {
      if (strncmp(line, "parent ", 7) == 0) {
        char parent[41] = "";
        if (sscanf(line, "parent %40s", parent) == 1) parents.push_back(parent);
      }
      const char *nl = (const char *)memchr(line, '\n', (size_t)(buf.data() + len - line));
      if (!nl) break;
      line = nl + 1;
    }

    // Walk the tree graph depth-first.
    std::vector<std::string> trees{root_tree};
    while (!trees.empty()) {
      const std::string t = trees.back();
      trees.pop_back();
      if (!copy_object(from_repo, to_repo, t.c_str())) continue;
      copied++;
      size_t tlen = 0;
      if (esp32git_object_read(from_repo, t.c_str(), type, sizeof(type),
                               buf.data(), buf.size(), &tlen) != ESP32GIT_OK) {
        continue;
      }
      const char *q = buf.data();
      const char *end = buf.data() + tlen;
      while (q < end) {
        // Entry format: "<mode> <name>\0<raw 20-byte sha>".
        const bool is_dir =
            (end - q > 6) && strncmp(q, "40000 ", 6) == 0;
        const char *sp = (const char *)memchr(q, ' ', (size_t)(end - q));
        if (!sp) break;
        const char *nul = (const char *)memchr(sp, '\0', (size_t)(end - sp));
        if (!nul || end - nul < 21) break;
        char hex[41];
        esp32git_bytes_to_hex((const uint8_t *)(nul + 1), hex);
        if (is_dir) {
          trees.push_back(hex);
        } else if (strncmp(q, "100644 ", 6) == 0 &&
                   copy_object(from_repo, to_repo, hex)) {
          copied++;
        }
        q = nul + 21;
      }
    }

    for (const auto &p : parents) commits.push_back(p);
  }
  return copied;
}

void ensure_parent_dirs(const std::string &path) {
  // Parents only - the final component is the file itself.
  e32g::make_dirs(path.substr(0, path.find_last_of('/')));
}

} // namespace

namespace e32g {
bool hash_worktree_file(const std::string &path, char out[41]) {
  const int64_t size = e32g::file_size(path);
  if (size < 0) return false;
  e32g::File file;
  if (!file.open(path, false)) return false;
  char header[48];
  const int n = snprintf(header, sizeof(header), "blob %llu",
                         (unsigned long long)size);
  if (n <= 0 || n >= (int)sizeof(header)) return false;
  esp32git_sha1 sha;
  esp32git_sha1_init(&sha);
  esp32git_sha1_update(&sha, header, (size_t)n + 1);
  uint8_t chunk[4096];
  int64_t remaining = size;
  while (remaining > 0) {
    size_t got = 0;
    const size_t want = remaining < (int64_t)sizeof(chunk)
                            ? (size_t)remaining : sizeof(chunk);
    if (!file.read(chunk, want, &got) || got == 0) return false;
    esp32git_sha1_update(&sha, chunk, got);
    remaining -= (int64_t)got;
  }
  if (!file.close()) return false;
  uint8_t digest[20];
  esp32git_sha1_final(&sha, digest);
  esp32git_bytes_to_hex(digest, out);
  return true;
}
} // namespace e32g

namespace {
using e32g::hash_worktree_file;

bool old_index_sha(const char *repo_path, const std::string &relpath,
                   char out[41]) {
  out[0] = '\0';
  e32g::File index;
  if (!index.open(std::string(repo_path) + "/.git/esp32git-index", false)) {
    return true; // initial clone has no previous index
  }
  std::string line;
  uint8_t chunk[1024];
  for (;;) {
    size_t got = 0;
    if (!index.read(chunk, sizeof(chunk), &got)) return false;
    if (got == 0) return true;
    for (size_t i = 0; i < got; ++i) {
      if (chunk[i] == '\n') {
        if (line.size() >= 42 && line[40] == ' ' &&
            line.compare(41, std::string::npos, relpath) == 0) {
          memcpy(out, line.data(), 40);
          out[40] = '\0';
          return true;
        }
        line.clear();
      } else {
        if (line.size() >= 1024) return false;
        line.push_back((char)chunk[i]);
      }
    }
  }
}

// Materializes worktree files and the staging index from a tree id.
esp32git_status checkout_tree(const char *repo_path, const char *tree_sha,
                              bool allow_missing = false,
                              bool preflight = false, bool index_only = false) {
  if (!preflight && !index_only) {
    const esp32git_status safe = checkout_tree(repo_path, tree_sha,
                                               allow_missing, true);
    if (safe != ESP32GIT_OK) return safe;
  }
  std::vector<std::pair<std::string, std::string>> pending{{"", tree_sha}};
  // Files of the previous checkout that the new tree no longer has are removed
  // afterwards, unless they were edited locally.
  std::vector<esp32git_index_entry> previous;
  if (!index_only) esp32git_index_load(repo_path, &previous);
  std::vector<std::string> current_paths;
  const std::string index_path = std::string(repo_path) + "/.git/esp32git-index";
  const std::string index_temp = index_path + ".tmp";
  struct TempCleanup {
    const std::string &path;
    ~TempCleanup() { e32g::remove_file(path); }
  } cleanup{index_temp};
  e32g::File index_file;
  if (!preflight && !index_file.open(index_temp, true)) return ESP32GIT_IO_ERROR;
  char type[16];
  static std::vector<char> buf;
  buf.resize(kMaxObjectBytes);
  static std::vector<uint8_t> content;
  content.resize(kMaxObjectBytes);

  while (!pending.empty()) {
    const auto [prefix, tree] = pending.back();
    pending.pop_back();
    size_t len = 0;
    if (esp32git_object_read(repo_path, tree.c_str(), type, sizeof(type),
                             buf.data(), buf.size(), &len) != ESP32GIT_OK) {
      return ESP32GIT_IO_ERROR;
    }
    const char *q = buf.data();
    const char *end = buf.data() + len;
    while (q + 6 < end) {
      const char *sp = (const char *)memchr(q, ' ', (size_t)(end - q));
      if (!sp) break;
      const char *nul = (const char *)memchr(sp, '\0', (size_t)(end - sp));
      if (!nul || end - nul < 21) break;
      const bool is_dir = (sp - q == 5) && strncmp(q, "40000", 5) == 0;
      const std::string name(sp + 1, (size_t)(nul - sp - 1));
      char hex[41];
      esp32git_bytes_to_hex((const uint8_t *)(nul + 1), hex);
      const std::string relpath = prefix.empty() ? name : prefix + "/" + name;
      if (is_dir) {
        pending.push_back({relpath, hex});
      } else {
        if (!index_only) current_paths.push_back(relpath);
        const std::string fp = std::string(repo_path) + "/" + relpath;
        if (preflight) {
          if (e32g::exists(fp)) {
            char current[41], previous[41];
            if (!hash_worktree_file(fp, current)) return ESP32GIT_IO_ERROR;
            if (strcmp(current, hex) != 0) {
              if (!old_index_sha(repo_path, relpath, previous)) return ESP32GIT_IO_ERROR;
              if (strcmp(current, previous) != 0) return ESP32GIT_REMOTE_DIVERGED;
            }
          }
          q = nul + 21;
          continue;
        }
        const std::string record = std::string(hex) + " " + relpath + "\n";
        if (!index_file.write(reinterpret_cast<const uint8_t *>(record.data()),
                              record.size())) return ESP32GIT_IO_ERROR;
        if (index_only) {
          q = nul + 21;
          continue;
        }
        char object_path[576];
        if (allow_missing && !esp32git_object_path(repo_path, hex, object_path,
                                                   sizeof(object_path))) {
          if (e32g::exists(fp)) {
            char current[41];
            if (!hash_worktree_file(fp, current)) return ESP32GIT_IO_ERROR;
            if (strcmp(current, hex) != 0 && !e32g::remove_file(fp)) {
              return ESP32GIT_IO_ERROR;
            }
          }
          q = nul + 21;
          continue;
        }
        if (e32g::exists(fp)) {
          char current[41];
          if (!hash_worktree_file(fp, current)) return ESP32GIT_IO_ERROR;
          if (strcmp(current, hex) == 0) {
            q = nul + 21;
            continue;
          }
        }
        size_t blen = 0;
        if (esp32git_object_read(repo_path, hex, type, sizeof(type),
                                 content.data(), content.size(), &blen) != ESP32GIT_OK) {
          return ESP32GIT_IO_ERROR;
        }
        ensure_parent_dirs(fp);
        if (!e32g::write_whole(fp, content.data(), blen)) return ESP32GIT_IO_ERROR;
      }
      q = nul + 21;
    }
  }
  std::sort(current_paths.begin(), current_paths.end());
  for (const auto &old : previous) {
    if (old.sha.empty() || std::binary_search(current_paths.begin(), current_paths.end(), old.path)) continue;
    const std::string fp = std::string(repo_path) + "/" + old.path;
    if (!e32g::exists(fp)) continue;
    char current[41];
    if (!hash_worktree_file(fp, current)) return ESP32GIT_IO_ERROR;
    const bool edited = old.sha != current;
    if (preflight) {
      if (edited) return ESP32GIT_REMOTE_DIVERGED;
    } else if (!edited && !e32g::remove_file(fp)) {
      return ESP32GIT_IO_ERROR;
    }
  }
  if (preflight) return ESP32GIT_OK;
  if (!index_file.close() || !e32g::rename_file(index_temp, index_path)) {
    return ESP32GIT_IO_ERROR;
  }
  return ESP32GIT_OK;
}

// Refreshes the worktree + staging index at a commit id. Shared with the
// HTTP transport (see sync_internal.h).
esp32git_status materialize_head(const char *repo_path, const char *head_sha) {
  char tree[41];
  if (esp32git_head_tree(repo_path, head_sha, tree) != ESP32GIT_OK) {
    return ESP32GIT_IO_ERROR;
  }
  return checkout_tree(repo_path, tree);
}

} // namespace

esp32git_status e32g_checkout_tree_at(const char *repo_path, const char *tree_sha) {
  return checkout_tree(repo_path, tree_sha);
}

namespace e32g {
esp32git_status checkout_tree_at(const char *repo_path, const char *tree_sha) {
  return checkout_tree(repo_path, tree_sha);
}
esp32git_status checkout_tree_partial_at(const char *repo_path,
                                         const char *tree_sha) {
  return checkout_tree(repo_path, tree_sha, true);
}
esp32git_status index_from_tree(const char *repo_path, const char *tree_sha) {
  return checkout_tree(repo_path, tree_sha, true, false, true);
}
} // namespace e32g

esp32git_status esp32git_init(const char *workdir) { return esp32git_repo_init(workdir); }

esp32git_status esp32git_commit(const char *repo_path, const esp32git_identity *id,
                                const char *message, char out_sha[41]) {
  std::vector<esp32git_index_entry> idx;
  esp32git_index_load(repo_path, &idx);
  if (idx.empty()) return ESP32GIT_INVALID_REF; // nothing to commit

  char tree[41];
  esp32git_status st = esp32git_write_tree(repo_path, idx, tree);
  if (st != ESP32GIT_OK) return st;

  char parent[41] = "";
  esp32git_resolve_head(repo_path, parent); // unborn branch: parent stays ""
  if (parent[0]) {
    // Like git: a commit whose tree equals its parent's is a no-op.
    char parent_tree[41];
    if (esp32git_head_tree(repo_path, parent, parent_tree) == ESP32GIT_OK &&
        strcmp(parent_tree, tree) == 0) {
      return ESP32GIT_UP_TO_DATE;
    }
  }

  st = esp32git_commit_create(repo_path, *id, message, tree, parent, out_sha);
  if (st != ESP32GIT_OK) return st;

  const std::string refname = esp32git_branch_ref("main");
  return esp32git_write_ref(repo_path, refname.c_str(), out_sha);
}

esp32git_status esp32git_fetch(const char *remote_dir, const char *branch,
                               const char *repo_path) {
  char rhead[41];
  const esp32git_status rr = esp32git_read_ref(
      remote_dir, esp32git_branch_ref(branch).c_str(), rhead);
  if (rr == ESP32GIT_INVALID_REF) return ESP32GIT_UP_TO_DATE; // remote has nothing
  if (rr != ESP32GIT_OK) return rr;

  copy_reachable(remote_dir, rhead, repo_path);

  const std::string refname = esp32git_branch_ref(branch);
  char lhead[41];
  const esp32git_status rl = esp32git_resolve_head(repo_path, lhead);
  if (rl == ESP32GIT_INVALID_REF) { // unborn local branch: fast-forward trivially
    const esp32git_status st =
        esp32git_write_ref(repo_path, refname.c_str(), rhead);
    if (st != ESP32GIT_OK) return st;
    return materialize_head(repo_path, rhead);
  }
  if (rl != ESP32GIT_OK) return rl;
  if (strcmp(lhead, rhead) == 0) return ESP32GIT_UP_TO_DATE;
  if (!esp32git_is_ancestor(repo_path, lhead, rhead)) return ESP32GIT_REMOTE_DIVERGED;
  const esp32git_status st = esp32git_write_ref(repo_path, refname.c_str(), rhead);
  if (st != ESP32GIT_OK) return st;
  // Fast-forwarded: refresh the worktree and staging index to the new head.
  // ponytail: overwrites tracked files and leaves removed-file stragglers;
  // device-vault model until someone needs git-clean semantics.
  return materialize_head(repo_path, rhead);
}

esp32git_status esp32git_push(const char *remote_dir, const char *branch,
                              const char *repo_path) {
  char lhead[41];
  const esp32git_status rl = esp32git_resolve_head(repo_path, lhead);
  if (rl != ESP32GIT_OK) return rl; // nothing to push

  char rhead[41] = "";
  const esp32git_status rr = esp32git_read_ref(
      remote_dir, esp32git_branch_ref(branch).c_str(), rhead);
  if (rr == ESP32GIT_OK && !esp32git_is_ancestor(repo_path, rhead, lhead)) {
    return ESP32GIT_REMOTE_DIVERGED; // would need merge/rebase: refuse by design
  }
  copy_reachable(repo_path, lhead, remote_dir);
  return esp32git_write_ref(remote_dir, esp32git_branch_ref(branch).c_str(), lhead);
}

esp32git_status esp32git_clone(const char *remote_dir, const char *branch,
                               const char *workdir) {
  esp32git_status st = esp32git_repo_init(workdir);
  if (st != ESP32GIT_OK) return st;
  st = esp32git_fetch(remote_dir, branch, workdir);
  if (st == ESP32GIT_UP_TO_DATE) return ESP32GIT_OK; // empty origin clones fine
  if (st != ESP32GIT_OK) return st;
  char head[41];
  st = esp32git_resolve_head(workdir, head);
  if (st != ESP32GIT_OK) return st;
  return materialize_head(workdir, head);
}
