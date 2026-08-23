#include "repo.h"

#include <sys/stat.h>
#include <cstdio>
#include <cstring>
#include <algorithm>

#include "history.h"
#include "io.h"
#include "sha1.h"

namespace {

std::string git_dir(const char *repo_path) {
  return std::string(repo_path) + "/.git";
}

int read_text(const std::string &path, char *out, size_t cap) {
  std::vector<uint8_t> data;
  if (!e32g::read_whole(path, data)) return -1;
  const size_t n = data.size() < cap - 1 ? data.size() : cap - 1;
  memcpy(out, data.data(), n);
  out[n] = '\0';
  return 0;
}

} // namespace

bool esp32git_is_repo_dir(const char *repo_path) {
  const std::string g = git_dir(repo_path);
  return e32g::exists(g + "/HEAD");
}

esp32git_status esp32git_repo_init(const char *workdir) {
  if (!workdir) return ESP32GIT_IO_ERROR;
  if (!e32g::make_dirs(workdir)) return ESP32GIT_IO_ERROR;
  const std::string g = git_dir(workdir);
  if (!e32g::make_dirs(g + "/objects") || !e32g::make_dirs(g + "/refs/heads")) {
    return ESP32GIT_IO_ERROR;
  }
  const char head_content[] = "ref: refs/heads/main\n";
  if (!e32g::write_whole(g + "/HEAD", (const uint8_t *)head_content,
                         sizeof(head_content) - 1)) {
    return ESP32GIT_IO_ERROR;
  }
  return esp32git_index_save(workdir, {});
}

std::string esp32git_branch_ref(const char *branch) {
  return std::string("refs/heads/") + (branch ? branch : "main");
}

esp32git_status esp32git_read_ref(const char *repo_path, const char *refname,
                                  char out_sha[41]) {
  char buf[256];
  std::string path = git_dir(repo_path) + "/" + refname;
  if (read_text(path, buf, sizeof(buf)) != 0) {
    // Bare repositories keep refs directly under <repo>/refs/...
    if (read_text(std::string(repo_path) + "/" + refname, buf, sizeof(buf)) != 0) {
      return ESP32GIT_INVALID_REF;
    }
  }
  char hex[41];
  if (sscanf(buf, "%40s", hex) != 1) return ESP32GIT_INVALID_REF;
  if (strlen(hex) != 40) return ESP32GIT_INVALID_REF;
  memcpy(out_sha, hex, 41);
  return ESP32GIT_OK;
}

esp32git_status esp32git_write_ref(const char *repo_path, const char *refname,
                                   const char *sha) {
  std::string full = git_dir(repo_path) + "/" + refname;
  // Bare origin: write under <repo>/refs/... instead.
  if (!e32g::exists(git_dir(repo_path) + "/HEAD") &&
      e32g::exists(std::string(repo_path) + "/HEAD")) {
    full = std::string(repo_path) + "/" + refname;
  }
  const size_t slash = full.find_last_of('/');
  if (slash == std::string::npos) return ESP32GIT_IO_ERROR;
  if (!e32g::make_dirs(full.substr(0, slash))) return ESP32GIT_IO_ERROR;
  const std::string content = std::string(sha) + "\n";
  return e32g::write_whole(full, (const uint8_t *)content.data(), content.size())
             ? ESP32GIT_OK
             : ESP32GIT_IO_ERROR;
}

esp32git_status esp32git_resolve_head(const char *repo_path, char out_sha[41]) {
  char head[128];
  if (read_text(git_dir(repo_path) + "/HEAD", head, sizeof(head)) != 0) {
    return ESP32GIT_NOT_A_REPO;
  }
  char refname[96];
  if (sscanf(head, "ref: %95s", refname) != 1) return ESP32GIT_INVALID_REF;
  return esp32git_read_ref(repo_path, refname, out_sha); // INVALID_REF = unborn branch
}

// ---- staging index --------------------------------------------------------

esp32git_status esp32git_index_load(const char *repo_path,
                                    std::vector<esp32git_index_entry> *out) {
  out->clear();
  std::vector<uint8_t> data;
  if (!e32g::read_whole(git_dir(repo_path) + "/esp32git-index", data)) {
    return ESP32GIT_OK; // no index yet = empty staging area
  }
  std::string text(data.begin(), data.end());
  size_t pos = 0;
  while (pos < text.size()) {
    size_t end = text.find('\n', pos);
    if (end == std::string::npos) end = text.size();
    const std::string line = text.substr(pos, end - pos);
    pos = end + 1;
    if (line.size() < 42 || line[40] != ' ') continue;
    out->push_back({line.substr(41), line.substr(0, 40)});
  }
  std::sort(out->begin(), out->end(),
            [](const esp32git_index_entry &a, const esp32git_index_entry &b) {
              return a.path < b.path;
            });
  return ESP32GIT_OK;
}

esp32git_status esp32git_index_save(const char *repo_path,
                                    const std::vector<esp32git_index_entry> &entries) {
  std::vector<esp32git_index_entry> sorted(entries);
  std::sort(sorted.begin(), sorted.end(),
            [](const esp32git_index_entry &a, const esp32git_index_entry &b) {
              return a.path < b.path;
            });
  std::string blob;
  for (const auto &e : sorted) {
    blob += e.sha;
    blob += ' ';
    blob += e.path;
    blob += '\n';
  }
  return e32g::write_whole(git_dir(repo_path) + "/esp32git-index",
                           (const uint8_t *)blob.data(), blob.size())
             ? ESP32GIT_OK
             : ESP32GIT_IO_ERROR;
}

namespace {

// SHA-1 id of the given bytes as a blob, without writing anything.
void hash_only(const void *data, size_t len, char out[41]) {
  char header[32];
  const int hl = snprintf(header, sizeof(header), "blob %zu", len);
  esp32git_sha1 ctx;
  esp32git_sha1_init(&ctx);
  esp32git_sha1_update(&ctx, header, (size_t)hl + 1);
  esp32git_sha1_update(&ctx, data, len);
  uint8_t digest[20];
  esp32git_sha1_final(&ctx, digest);
  static const char kHex[] = "0123456789abcdef";
  for (int i = 0; i < 20; i++) {
    out[i * 2] = kHex[digest[i] >> 4];
    out[i * 2 + 1] = kHex[digest[i] & 0xf];
  }
  out[40] = '\0';
}

} // namespace

esp32git_status esp32git_add(const char *repo_path, const char *relpath) {
  std::vector<uint8_t> content;
  if (!e32g::read_whole(std::string(repo_path) + "/" + relpath, content)) {
    // deletion staged by adding a missing path
    std::vector<esp32git_index_entry> idx;
    esp32git_index_load(repo_path, &idx);
    bool found = false;
    for (auto &e : idx) {
      if (e.path == relpath) {
        e.sha = ""; // tombstone = staged deletion
        found = true;
      }
    }
    if (!found) return ESP32GIT_IO_ERROR;
    return esp32git_index_save(repo_path, idx);
  }

  char sha[41];
  esp32git_status st =
      esp32git_object_write(repo_path, "blob", content.data(), content.size(), sha);
  if (st != ESP32GIT_OK) return st;

  std::vector<esp32git_index_entry> idx;
  esp32git_index_load(repo_path, &idx);
  bool found = false;
  for (auto &e : idx) {
    if (e.path == relpath) {
      e.sha = sha;
      found = true;
    }
  }
  if (!found) idx.push_back({relpath, sha});
  return esp32git_index_save(repo_path, idx);
}

esp32git_status esp32git_status_file(const char *repo_path, const char *relpath,
                                     char *out_state) {
  std::vector<esp32git_index_entry> idx;
  esp32git_index_load(repo_path, &idx);

  const esp32git_index_entry *entry = nullptr;
  for (const auto &e : idx) {
    if (e.path == relpath) entry = &e;
  }

  // Worktree state.
  char wt_sha[41] = "";
  {
    std::vector<uint8_t> data;
    if (e32g::read_whole(std::string(repo_path) + "/" + relpath, data)) {
      hash_only(data.data(), data.size(), wt_sha);
    } // absent from worktree -> wt_sha stays ""
  }

  // HEAD state.
  char head_tree[41];
  bool have_head = false;
  {
    std::vector<esp32git_index_entry> ignore;
    (void)ignore;
    char head[41];
    if (esp32git_resolve_head(repo_path, head) == ESP32GIT_OK) {
      have_head = esp32git_head_tree(repo_path, head, head_tree) == ESP32GIT_OK;
    }
  }
  char head_blob[41] = "";
  if (have_head) {
    (void)esp32git_tree_lookup(repo_path, head_tree, relpath, head_blob);
  }

  if (!entry) {
    *out_state = wt_sha[0] ? '?' : '=';
    return ESP32GIT_OK;
  }
  if (entry->sha.empty()) {
    // Staged deletion: clean only once the worktree copy is gone too.
    *out_state = wt_sha[0] ? 'U' : 'S';
    return ESP32GIT_OK;
  }
  if (wt_sha[0] && strcmp(wt_sha, entry->sha.c_str()) != 0) {
    *out_state = 'U';
    return ESP32GIT_OK;
  }
  *out_state = (head_blob[0] && strcmp(entry->sha.c_str(), head_blob) == 0) ? '=' : 'S';
  return ESP32GIT_OK;
}
