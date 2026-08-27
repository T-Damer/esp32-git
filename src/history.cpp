#include "history.h"

#include <algorithm>
#include <ctime>
#include <map>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "esp32_git.h"
#include "hexutil.h"

namespace {

constexpr size_t kMaxObjectBytes = 64 * 1024; // trees/commits stay far below this

struct tree_item {
  std::string name;
  bool is_dir = false;
  std::string sha; // blob or subtree id
};

// Git sorts tree entries byte-wise, comparing directory names as if they
// ended in '/'.
bool git_name_less(const tree_item &a, const tree_item &b) {
  const std::string sa = a.name + (a.is_dir ? "/" : "");
  const std::string sb = b.name + (b.is_dir ? "/" : "");
  return sa < sb;
}

esp32git_status build_tree(const char *repo, const std::string &prefix,
                           const std::vector<esp32git_index_entry> &all,
                           char out_sha[41]) {
  std::vector<tree_item> items;
  for (const auto &e : all) {
    if (e.sha.empty()) continue; // staged deletion: not part of the tree
    if (e.path.compare(0, prefix.size(), prefix) != 0) continue;
    const std::string rest = e.path.substr(prefix.size());
    const size_t slash = rest.find('/');
    if (slash == std::string::npos) {
      items.push_back({rest, false, e.sha});
      continue;
    }
    const std::string dir = rest.substr(0, slash);
    bool known = false;
    for (auto &i : items) {
      if (i.is_dir && i.name == dir) known = true;
    }
    if (!known) items.push_back({dir, true, ""});
  }

  for (auto &item : items) {
    if (!item.is_dir) continue;
    char sub[41];
    const esp32git_status st =
        build_tree(repo, prefix + item.name + "/", all, sub);
    if (st != ESP32GIT_OK) return st;
    item.sha = sub;
  }
  if (items.empty()) return ESP32GIT_INVALID_REF;

  std::sort(items.begin(), items.end(), git_name_less);

  std::string payload;
  payload.reserve(items.size() * 40 + 32);
  for (const auto &item : items) {
    // UTF-8 names can exceed any fixed buffer; build the entry at its own size.
    std::string entry;
    entry.reserve(item.name.size() + 28);
    entry += item.is_dir ? "40000 " : "100644 ";
    entry += item.name;
    entry.push_back('\0');
    payload.append(entry);
    uint8_t raw[20];
    esp32git_hex_to_bytes(item.sha.c_str(), raw);
    payload.append((const char *)raw, 20);
  }
  return esp32git_object_write(repo, "tree", payload.data(), payload.size(), out_sha);
}

} // namespace

esp32git_status esp32git_write_tree(const char *repo_path,
                                    const std::vector<esp32git_index_entry> &entries,
                                    char out_sha[41]) {
  return build_tree(repo_path, "", entries, out_sha);
}

esp32git_status esp32git_tree_lookup(const char *repo_path, const char *tree_sha,
                                     const char *relpath, char out_blob[41]) {
  out_blob[0] = '\0';
  char cur[41];
  snprintf(cur, sizeof(cur), "%s", tree_sha);
  const char *p = relpath;
  while (*p) {
    char type[16];
    static std::vector<char> buf; // ponytail: reused scratch; single-threaded API
    buf.resize(kMaxObjectBytes);
    size_t len = 0;
    const esp32git_status st =
        esp32git_object_read(repo_path, cur, type, sizeof(type), buf.data(),
                             buf.size(), &len);
    if (st != ESP32GIT_OK) return st;
    const bool want_dir = strchr(p, '/') != nullptr;
    size_t comp_len = strcspn(p, "/");

    const char *q = buf.data();
    const char *end = buf.data() + len;
    bool matched = false;
    while (q < end) {
      const char *sp = (const char *)memchr(q, ' ', (size_t)(end - q));
      if (!sp) break;
      const char *nul = (const char *)memchr(sp, '\0', (size_t)(end - sp));
      if (!nul || end - nul < 21) return ESP32GIT_PROTOCOL_ERROR;
      const bool entry_is_dir = (sp - q == 5) && strncmp(q, "40000", 5) == 0;
      const size_t name_len = (size_t)(nul - sp - 1);
      if (name_len == comp_len && strncmp(sp + 1, p, comp_len) == 0 &&
          entry_is_dir == want_dir) {
        esp32git_bytes_to_hex((const uint8_t *)(nul + 1), cur);
        matched = true;
        q = nul + 21;
        break;
      }
      q = nul + 21;
    }
    if (!matched) return ESP32GIT_OK; // absent: empty string, OK per contract
    p += comp_len + (want_dir ? 1 : 0);
  }
  snprintf(out_blob, 41, "%s", cur);
  return ESP32GIT_OK;
}

esp32git_status esp32git_head_tree(const char *repo_path, const char *commit_sha,
                                   char out_tree[41]) {
  char type[16];
  static std::vector<char> buf;
  buf.resize(kMaxObjectBytes);
  size_t len = 0;
  const esp32git_status st = esp32git_object_read(repo_path, commit_sha, type,
                                                  sizeof(type), buf.data(),
                                                  buf.size(), &len);
  if (st != ESP32GIT_OK) return st;
  if (sscanf(buf.data(), "tree %40s", out_tree) != 1) return ESP32GIT_PROTOCOL_ERROR;
  return ESP32GIT_OK;
}

esp32git_status esp32git_commit_create(const char *repo_path,
                                       const esp32git_identity &id,
                                       const char *message, const char *tree_sha,
                                       const char *parent_sha, char out_commit[41]) {
  std::string payload;
  char line[256];
  int n = snprintf(line, sizeof(line), "tree %s\n", tree_sha);
  payload.append(line, (size_t)n);
  if (parent_sha && parent_sha[0]) {
    n = snprintf(line, sizeof(line), "parent %s\n", parent_sha);
    payload.append(line, (size_t)n);
  }
  const long ts = (long)time(nullptr);
  n = snprintf(line, sizeof(line), "author %s <%s> %ld +0000\n", id.name, id.email, ts);
  payload.append(line, (size_t)n);
  n = snprintf(line, sizeof(line), "committer %s <%s> %ld +0000\n", id.name, id.email, ts);
  payload.append(line, (size_t)n);
  payload += "\n";
  payload += message;
  if (payload.empty() || payload.back() != '\n') payload += "\n";
  return esp32git_object_write(repo_path, "commit", payload.data(), payload.size(),
                               out_commit);
}

bool esp32git_is_ancestor(const char *repo_path, const char *ancestor,
                          const char *descendant) {
  if (strcmp(ancestor, descendant) == 0) return true;
  std::vector<std::string> queue{descendant};
  size_t visited = 0;
  char type[16];
  static std::vector<char> buf;
  buf.resize(kMaxObjectBytes);
  while (!queue.empty() && visited++ < 4096) { // ponytail: bound walk on corrupt data
    const std::string cur = queue.back();
    queue.pop_back();
    size_t len = 0;
    if (esp32git_object_read(repo_path, cur.c_str(), type, sizeof(type),
                             buf.data(), buf.size(), &len) != ESP32GIT_OK) {
      continue; // shallow history on one side: treat as not reachable
    }
    for (const char *line = buf.data(); line && *line;) {
      if (strncmp(line, "parent ", 7) == 0) {
        char parent[41] = "";
        if (sscanf(line, "parent %40s", parent) == 1) {
          if (strcmp(parent, ancestor) == 0) return true;
          queue.push_back(parent);
        }
      }
      const char *nl = (const char *)memchr(line, '\n', kMaxObjectBytes - (line - buf.data()));
      if (!nl) break;
      line = nl + 1;
    }
  }
  return false;
}
