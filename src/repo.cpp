#include "repo.h"

#include <sys/stat.h>
#include <cstdio>
#include <cstring>
#include <algorithm>

#include "history.h"
#include "sha1.h"

namespace {

bool ensure_dir(const std::string &path) {
  // mkdir -p
  std::string cur;
  for (size_t i = 0; i <= path.size(); i++) {
    if (i == path.size() || path[i] == '/') {
      cur = path.substr(0, i);
      if (!cur.empty()) mkdir(cur.c_str(), 0777);
    }
  }
  struct stat st;
  return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

std::string git_dir(const char *repo_path) {
  return std::string(repo_path) + "/.git";
}

int read_text(const std::string &path, char *out, size_t cap) {
  FILE *f = fopen(path.c_str(), "rb");
  if (!f) return -1;
  const size_t n = fread(out, 1, cap - 1, f);
  fclose(f);
  out[n] = '\0';
  return 0;
}

} // namespace

bool esp32git_is_repo_dir(const char *repo_path) {
  struct stat st;
  const std::string g = git_dir(repo_path);
  return stat((g + "/objects").c_str(), &st) == 0 && stat((g + "/HEAD").c_str(), &st) == 0;
}

esp32git_status esp32git_repo_init(const char *workdir) {
  if (!workdir) return ESP32GIT_IO_ERROR;
  if (!ensure_dir(workdir)) return ESP32GIT_IO_ERROR;
  const std::string g = git_dir(workdir);
  if (!ensure_dir(g + "/objects") || !ensure_dir(g + "/refs/heads")) {
    return ESP32GIT_IO_ERROR;
  }
  FILE *f = fopen((g + "/HEAD").c_str(), "wb");
  if (!f) return ESP32GIT_IO_ERROR;
  fputs("ref: refs/heads/main\n", f);
  fclose(f);
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
  struct stat st;
  // Bare origin: write under <repo>/refs/... instead.
  if (stat((std::string(repo_path) + "/refs").c_str(), &st) == 0 &&
      stat((std::string(repo_path) + "/HEAD").c_str(), &st) == 0) {
    FILE *probe = fopen(git_dir(repo_path).c_str(), "rb");
    const bool not_a_dir = !probe;
    if (probe) fclose(probe);
    if (not_a_dir) full = std::string(repo_path) + "/" + refname;
  }
  const size_t slash = full.find_last_of('/');
  if (slash == std::string::npos) return ESP32GIT_IO_ERROR;
  if (!ensure_dir(full.substr(0, slash))) return ESP32GIT_IO_ERROR;
  FILE *f = fopen(full.c_str(), "wb");
  if (!f) return ESP32GIT_IO_ERROR;
  fprintf(f, "%s\n", sha);
  fclose(f);
  return ESP32GIT_OK;
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
  FILE *f = fopen((git_dir(repo_path) + "/esp32git-index").c_str(), "rb");
  if (!f) { // no index yet = empty staging area
    out->clear();
    return ESP32GIT_OK;
  }
  char line[512];
  out->clear();
  while (fgets(line, sizeof(line), f)) {
    char sha[41];
    if (sscanf(line, "%40s", sha) != 1) continue;
    const char *path = line + 40;
    while (*path == ' ') path++;
    const size_t n = strlen(path);
    if (n && path[n - 1] == '\n') ((char *)path)[n - 1] = '\0'; // const_cast ok: our buffer
    if (*path) out->push_back({path, sha});
  }
  fclose(f);
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
  FILE *f = fopen((git_dir(repo_path) + "/esp32git-index").c_str(), "wb");
  if (!f) return ESP32GIT_IO_ERROR;
  for (const auto &e : sorted) {
    fprintf(f, "%s %s\n", e.sha.c_str(), e.path.c_str());
  }
  fclose(f);
  return ESP32GIT_OK;
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
  std::vector<char> content;
  {
    const std::string fp = std::string(repo_path) + "/" + relpath;
    FILE *f = fopen(fp.c_str(), "rb");
    if (!f) { // deletion staged by adding a missing path
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
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    content.resize(size > 0 ? (size_t)size : 1);
    fread(content.data(), 1, content.size(), f);
    fclose(f);
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
    const std::string fp = std::string(repo_path) + "/" + relpath;
    FILE *f = fopen(fp.c_str(), "rb");
    if (f) {
      fseek(f, 0, SEEK_END);
      const long size = ftell(f);
      fseek(f, 0, SEEK_SET);
      std::vector<char> data(size > 0 ? (size_t)size : 1);
      fread(data.data(), 1, data.size(), f);
      fclose(f);
      hash_only(data.data(), data.size(), wt_sha);
    } // else: absent from worktree -> wt_sha stays ""
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
