// Two-way sync of a shallow partial clone: local changes are found by the
// library itself, set aside, reapplied on top of the fetched remote, then
// committed and pushed. See esp32git_sync_url in esp32_git.h.

#include <cstdio>
#include <cstring>
#include <ctime>
#include <map>
#include <string>
#include <vector>

#include "esp32_git.h"
#include "history.h"
#include "io.h"
#include "repo.h"
#include "sync_internal.h"

namespace {

constexpr int kAttempts = 5;

// What the device last had in its worktree: "<sha> <size> <mtime> <path>".
// A file listed here with the index's blob id and now missing was deleted on
// the device; a file never listed was simply never downloaded.
constexpr const char *kSnapshot = "/.git/esp32git-present";
constexpr const char *kStashDir = "/.git/esp32git-stash";
// "<M|D> <base sha or -> <path>" per set-aside change; stash/<n> holds line n's content.
constexpr const char *kManifest = "/.git/esp32git-stash/manifest";

struct Snapshot {
  std::string sha;
  int64_t size = -1;
  int64_t mtime = -1;
};
using SnapshotMap = std::map<std::string, Snapshot>;

struct Change {
  char kind = 'M'; // M: created or edited on the device, D: deleted
  std::string base; // blob id before the local change ("" for a new file)
  std::string path;
};

template <typename F>
esp32git_status with_retries(F &&attempt) {
  esp32git_status st = ESP32GIT_IO_ERROR;
  for (int i = 0; i < kAttempts; ++i) {
    st = attempt();
    if (st != ESP32GIT_IO_ERROR) return st;
  }
  return st;
}

std::string at(const char *repo, const std::string &rel) { return std::string(repo) + "/" + rel; }

bool read_lines(const std::string &path, std::vector<std::string> &lines) {
  lines.clear();
  std::vector<uint8_t> data;
  if (!e32g::read_whole(path, data)) return false;
  std::string line;
  for (uint8_t byte : data) {
    if (byte == '\n') {
      if (!line.empty()) lines.push_back(line);
      line.clear();
    } else {
      line.push_back((char)byte);
    }
  }
  if (!line.empty()) lines.push_back(line);
  return true;
}

bool write_text(const std::string &path, const std::string &text) {
  const std::string temporary = path + ".tmp";
  if (!e32g::write_whole(temporary, (const uint8_t *)text.data(), text.size())) return false;
  return e32g::rename_file(temporary, path);
}

bool load_snapshot(const char *repo, SnapshotMap &out) {
  out.clear();
  std::vector<std::string> lines;
  if (!read_lines(std::string(repo) + kSnapshot, lines)) return false;
  for (const auto &line : lines) {
    char sha[41];
    long long size = 0, mtime = 0;
    int consumed = 0;
    if (sscanf(line.c_str(), "%40s %lld %lld %n", sha, &size, &mtime, &consumed) != 3 || consumed <= 0) continue;
    out[line.substr((size_t)consumed)] = {sha, size, mtime};
  }
  return true;
}

bool save_snapshot(const char *repo, const SnapshotMap &snapshot) {
  std::string text;
  char head[80];
  for (const auto &[path, item] : snapshot) {
    snprintf(head, sizeof(head), "%s %lld %lld ", item.sha.c_str(), (long long)item.size,
             (long long)item.mtime);
    text += head;
    text += path;
    text += '\n';
  }
  return write_text(std::string(repo) + kSnapshot, text);
}

std::map<std::string, std::string> index_map(const char *repo) {
  std::vector<esp32git_index_entry> entries;
  esp32git_index_load(repo, &entries);
  std::map<std::string, std::string> map;
  for (auto &entry : entries) {
    if (!entry.sha.empty()) map[entry.path] = entry.sha;
  }
  return map;
}

bool ignored_name(const std::string &name) {
  static const char kTemp[] = ".esp32git.tmp";
  const size_t n = sizeof(kTemp) - 1;
  return name.empty() || name[0] == '.' ||
         (name.size() > n && name.compare(name.size() - n, n, kTemp) == 0);
}

void walk(const char *repo, const std::string &rel, std::vector<std::string> &files) {
  std::vector<e32g::DirEntry> entries;
  if (!e32g::list_dir(rel.empty() ? std::string(repo) : at(repo, rel), entries)) return;
  for (const auto &entry : entries) {
    if (ignored_name(entry.name)) continue;
    const std::string child = rel.empty() ? entry.name : rel + "/" + entry.name;
    if (entry.is_dir) walk(repo, child, files);
    else files.push_back(child);
  }
}

// Local changes since the last sync, and the unchanged files' current stamps.
bool find_changes(const char *repo, const std::map<std::string, std::string> &index,
                  const SnapshotMap &snapshot, std::vector<Change> &changes,
                  SnapshotMap &unchanged) {
  for (const auto &[path, sha] : index) {
    const std::string full = at(repo, path);
    int64_t size = -1, mtime = -1;
    const bool stamped = e32g::stat_file(full, &size, &mtime);
    if (!stamped && !e32g::exists(full)) {
      const auto seen = snapshot.find(path);
      if (seen != snapshot.end() && seen->second.sha == sha) changes.push_back({'D', sha, path});
      continue;
    }
    const auto seen = snapshot.find(path);
    if (stamped && seen != snapshot.end() && seen->second.sha == sha &&
        seen->second.size == size && seen->second.mtime == mtime) {
      unchanged[path] = seen->second;
      continue;
    }
    char current[41];
    if (!e32g::hash_worktree_file(full, current)) return false;
    if (sha == current) {
      unchanged[path] = {sha, size, mtime};
    } else {
      changes.push_back({'M', sha, path});
    }
  }
  if (e32g::can_list_dirs()) {
    std::vector<std::string> files;
    walk(repo, "", files);
    for (const auto &path : files) {
      if (index.find(path) == index.end()) changes.push_back({'M', "", path});
    }
  }
  return true;
}

bool copy_file(const std::string &from, const std::string &to) {
  e32g::File in, out;
  const size_t slash = to.find_last_of('/');
  if (slash != std::string::npos && !e32g::make_dirs(to.substr(0, slash))) return false;
  const std::string temporary = to + ".esp32git.tmp";
  if (!in.open(from, false) || !out.open(temporary, true)) return false;
  std::vector<uint8_t> chunk(4096);
  for (;;) {
    size_t got = 0;
    if (!in.read(chunk.data(), chunk.size(), &got)) return false;
    if (got == 0) break;
    if (!out.write(chunk.data(), got)) return false;
  }
  if (!in.close() || !out.close()) return false;
  return e32g::rename_file(temporary, to);
}

std::string stash_file(const char *repo, size_t n) {
  return std::string(repo) + kStashDir + "/" + std::to_string(n);
}

// Copies local changes aside, records them, then clears them from the
// worktree so the fetch sees a clean checkout.
bool stash_changes(const char *repo, const std::vector<Change> &changes) {
  if (!e32g::make_dirs(std::string(repo) + kStashDir)) return false;
  std::string manifest;
  for (size_t i = 0; i < changes.size(); ++i) {
    const Change &change = changes[i];
    if (change.kind == 'M' && !copy_file(at(repo, change.path), stash_file(repo, i))) return false;
    manifest += change.kind;
    manifest += ' ';
    manifest += change.base.empty() ? "-" : change.base;
    manifest += ' ';
    manifest += change.path;
    manifest += '\n';
  }
  if (!write_text(std::string(repo) + kManifest, manifest)) return false;
  for (const auto &change : changes) {
    if (change.kind == 'M' && !e32g::remove_file(at(repo, change.path))) return false;
  }
  return true;
}

bool load_stash(const char *repo, std::vector<Change> &changes) {
  changes.clear();
  std::vector<std::string> lines;
  if (!read_lines(std::string(repo) + kManifest, lines)) return false;
  for (const auto &line : lines) {
    const size_t first = line.find(' ');
    const size_t second = first == std::string::npos ? first : line.find(' ', first + 1);
    if (second == std::string::npos) return false;
    Change change;
    change.kind = line[0];
    change.base = line.substr(first + 1, second - first - 1);
    if (change.base == "-") change.base.clear();
    change.path = line.substr(second + 1);
    changes.push_back(change);
  }
  return true;
}

void drop_stash(const char *repo, size_t count) {
  for (size_t i = 0; i < count; ++i) e32g::remove_file(stash_file(repo, i));
  e32g::remove_file(std::string(repo) + kManifest);
}

std::string conflict_path(const char *repo, const std::string &path, const char *tag,
                          const std::string &local_sha) {
  char stamp[32];
  const time_t now = time(nullptr);
  if (now > 1600000000) {
    struct tm parts;
    gmtime_r(&now, &parts);
    strftime(stamp, sizeof(stamp), "%Y-%m-%d %H%M", &parts);
  } else {
    snprintf(stamp, sizeof(stamp), "%.7s", local_sha.c_str()); // clock not set yet
  }
  const size_t slash = path.find_last_of('/');
  const size_t dot = path.find_last_of('.');
  const bool has_ext = dot != std::string::npos && (slash == std::string::npos || dot > slash + 1);
  const std::string stem = has_ext ? path.substr(0, dot) : path;
  const std::string ext = has_ext ? path.substr(dot) : "";
  for (int n = 1;; ++n) {
    std::string candidate = stem + " (conflict " + (tag ? tag : "device") + " " + stamp +
                            (n > 1 ? " " + std::to_string(n) : "") + ")" + ext;
    if (!e32g::exists(at(repo, candidate))) return candidate;
  }
}

// Puts set-aside changes back on top of the current checkout; returns the
// paths to commit.
bool reapply(const char *repo, const std::vector<Change> &changes, const char *tag,
             std::vector<std::string> &staged, esp32git_sync_report &report) {
  const auto index = index_map(repo);
  for (size_t i = 0; i < changes.size(); ++i) {
    const Change &change = changes[i];
    const auto found = index.find(change.path);
    const std::string remote = found == index.end() ? "" : found->second;
    if (change.kind == 'D') {
      if (remote == change.base) {
        e32g::remove_file(at(repo, change.path));
        staged.push_back(change.path);
      } else if (!remote.empty()) {
        ++report.kept_remote; // edited elsewhere: the edit wins over the deletion
      }
      continue;
    }
    const std::string stashed = stash_file(repo, i);
    char local[41];
    if (!e32g::hash_worktree_file(stashed, local)) return false;
    if (remote == local) {
      if (!copy_file(stashed, at(repo, change.path))) return false;
    } else if (remote == change.base || remote.empty()) {
      // Untouched remotely, or deleted remotely while edited here: keep ours.
      if (!copy_file(stashed, at(repo, change.path))) return false;
      staged.push_back(change.path);
    } else {
      const std::string copy = conflict_path(repo, change.path, tag, local);
      if (!copy_file(stashed, at(repo, copy))) return false;
      staged.push_back(copy);
      ++report.conflicts;
      snprintf(report.last_conflict, sizeof(report.last_conflict), "%s", copy.c_str());
    }
  }
  return true;
}

// Records every indexed file currently in the worktree, reusing stamps of
// files known to be unchanged.
bool refresh_snapshot(const char *repo, const SnapshotMap &unchanged) {
  SnapshotMap snapshot;
  for (const auto &[path, sha] : index_map(repo)) {
    const std::string full = at(repo, path);
    int64_t size = -1, mtime = -1;
    const bool stamped = e32g::stat_file(full, &size, &mtime);
    if (!stamped && !e32g::exists(full)) continue;
    const auto known = unchanged.find(path);
    if (stamped && known != unchanged.end() && known->second.sha == sha &&
        known->second.size == size && known->second.mtime == mtime) {
      snapshot[path] = known->second;
      continue;
    }
    char current[41];
    if (!e32g::hash_worktree_file(full, current)) return false;
    if (sha == current) snapshot[path] = {sha, size, mtime};
  }
  return save_snapshot(repo, snapshot);
}

esp32git_status commit_staged(const char *repo, const char *branch,
                              const std::vector<std::string> &staged,
                              const esp32git_sync_options &options, char out[41]) {
  for (const auto &path : staged) {
    const esp32git_status st = esp32git_add(repo, path.c_str());
    if (st != ESP32GIT_OK) return st;
  }
  std::vector<esp32git_index_entry> index;
  esp32git_index_load(repo, &index);
  char tree[41], parent[41] = "";
  esp32git_status st = esp32git_write_tree(repo, index, tree);
  if (st != ESP32GIT_OK) return st;
  esp32git_resolve_head(repo, parent);
  char parent_tree[41];
  if (parent[0] && esp32git_head_tree(repo, parent, parent_tree) == ESP32GIT_OK &&
      strcmp(parent_tree, tree) == 0) {
    return ESP32GIT_UP_TO_DATE;
  }
  const esp32git_identity fallback = {"esp32-git", "esp32-git@localhost"};
  st = esp32git_commit_create(repo, options.identity ? *options.identity : fallback,
                              options.message ? options.message : "Sync from device", tree,
                              parent, out);
  if (st != ESP32GIT_OK) return st;
  return esp32git_write_ref(repo, esp32git_branch_ref(branch).c_str(), out);
}

// Forgets an unpushed local commit; its changes stay in the worktree and are
// found again by the next pass.
esp32git_status reset_to(const char *repo, const char *branch, const char *head) {
  char tree[41];
  esp32git_status st = esp32git_head_tree(repo, head, tree);
  if (st != ESP32GIT_OK) return st;
  st = esp32git_write_ref(repo, esp32git_branch_ref(branch).c_str(), head);
  if (st != ESP32GIT_OK) return st;
  return e32g::index_from_tree(repo, tree);
}

} // namespace

esp32git_status esp32git_sync_url(const char *remote_url, const char *repo_path,
                                  const esp32git_remote *auth,
                                  const esp32git_sync_options *options,
                                  esp32git_sync_report *report) {
  const esp32git_remote anon = {nullptr, nullptr, nullptr};
  const esp32git_remote &remote = auth ? *auth : anon;
  const esp32git_sync_options defaults = {};
  const esp32git_sync_options &opt = options ? *options : defaults;
  esp32git_sync_report scratch;
  esp32git_sync_report &out = report ? *report : scratch;
  memset(&out, 0, sizeof(out));
  const char *branch = opt.branch ? opt.branch : "main";
  const esp32git_path_filter filter = opt.download_filter ? opt.download_filter : esp32git_filter_notes;

  if (!e32g::exists(std::string(repo_path) + "/.git/HEAD")) {
    esp32git_status st = with_retries([&] {
      return esp32git_clone_url_partial(remote_url, branch, repo_path, &remote);
    });
    if (st != ESP32GIT_OK && st != ESP32GIT_UP_TO_DATE) return st;
    st = e32g::download_matching_url(remote_url, repo_path, remote, filter,
                                     opt.download_filter_ctx, opt.progress, opt.progress_ctx);
    if (st != ESP32GIT_OK) return st;
    return refresh_snapshot(repo_path, {}) ? ESP32GIT_OK : ESP32GIT_IO_ERROR;
  }

  for (int attempt = 0; attempt < kAttempts; ++attempt) {
    SnapshotMap snapshot, unchanged;
    load_snapshot(repo_path, snapshot);
    std::vector<Change> changes;
    // A manifest left by an interrupted sync is finished before anything new.
    if (!load_stash(repo_path, changes)) {
      if (!find_changes(repo_path, index_map(repo_path), snapshot, changes, unchanged)) {
        return ESP32GIT_IO_ERROR;
      }
      if (!changes.empty() && !stash_changes(repo_path, changes)) return ESP32GIT_IO_ERROR;
    }

    esp32git_status st = with_retries([&] {
      return e32g::fetch_url(remote_url, branch, repo_path, remote, true);
    });
    if (st == ESP32GIT_OK || st == ESP32GIT_UP_TO_DATE) {
      st = e32g::download_matching_url(remote_url, repo_path, remote, filter,
                                       opt.download_filter_ctx, opt.progress, opt.progress_ctx);
    }
    std::vector<std::string> staged;
    // Reapplying against an unchanged index simply restores every local change.
    if (!reapply(repo_path, changes, opt.conflict_tag, staged, out)) return ESP32GIT_IO_ERROR;
    drop_stash(repo_path, changes.size());
    if (st != ESP32GIT_OK && st != ESP32GIT_UP_TO_DATE) return st;

    if (staged.empty()) {
      return refresh_snapshot(repo_path, unchanged) ? ESP32GIT_OK : ESP32GIT_IO_ERROR;
    }
    char fetched[41];
    st = esp32git_resolve_head(repo_path, fetched);
    if (st != ESP32GIT_OK) return st;
    char commit[41];
    st = commit_staged(repo_path, branch, staged, opt, commit);
    if (st == ESP32GIT_UP_TO_DATE) {
      return refresh_snapshot(repo_path, unchanged) ? ESP32GIT_OK : ESP32GIT_IO_ERROR;
    }
    if (st != ESP32GIT_OK) return st;
    st = with_retries([&] { return e32g::push_url(remote_url, branch, repo_path, remote); });
    if (st == ESP32GIT_OK || st == ESP32GIT_UP_TO_DATE) {
      out.pushed = (unsigned)staged.size();
      return refresh_snapshot(repo_path, unchanged) ? ESP32GIT_OK : ESP32GIT_IO_ERROR;
    }
    const esp32git_status reset = reset_to(repo_path, branch, fetched);
    if (reset != ESP32GIT_OK) return reset;
    if (st != ESP32GIT_REMOTE_DIVERGED) return st;
    memset(&out, 0, sizeof(out)); // the next pass reports from scratch
  }
  return ESP32GIT_REMOTE_DIVERGED;
}
