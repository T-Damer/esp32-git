#include "esp32_git.h"
#include "hexutil.h"

#include <new>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "esp32_git.h"
#include "history.h"
#include "io.h"
#include "pack.h"
#include "pkt.h"
#include "repo.h"
#include "sync_internal.h"

namespace e32g {

namespace {

constexpr const char *AGENT = "agent=esp32-git/0.1";
constexpr size_t kMaxObjectBytes = 64 * 1024;

const esp32git_http_port *active_http = nullptr;

struct RefAd {
  std::string sha;
  std::string name;
};

// GET /info/refs?service=<svc>; strips the preamble and parses "<sha> <name>".
esp32git_status discover(const esp32git_remote &auth, const char *url,
                         const char *service, std::vector<RefAd> &refs,
                         std::string *capabilities = nullptr) {
  if (!active_http || !active_http->request) return ESP32GIT_PROTOCOL_ERROR;
  const std::string full = std::string(url) + "/info/refs?service=" + service;
  uint8_t *body = nullptr;
  size_t len = 0;
  const int status = active_http->request(full.c_str(), 0, auth.user, auth.token,
                                          nullptr, nullptr, 0, &body, &len);
  if (status == 401 || status == 403) {
    if (body) esp32git_free_buffer(body);
    return ESP32GIT_AUTH_FAILED;
  }
  if (status != 200) {
    if (body) esp32git_free_buffer(body);
    return status < 0 ? ESP32GIT_IO_ERROR : ESP32GIT_PROTOCOL_ERROR;
  }
  std::vector<std::string> lines;
  const bool framed = pkt_split(body, len, lines);
  esp32git_free_buffer(body);
  if (!framed) return ESP32GIT_PROTOCOL_ERROR;
  bool seen_service = false;
  for (auto &line : lines) {
    if (!seen_service) {
      seen_service = line.rfind("# service=", 0) == 0;
      continue;
    }
    if (line.rfind("version ", 0) == 0) continue; // v2 header we did not request
    const size_t sp = line.find(' ');
    if (sp < 40) continue;
    RefAd ad;
    ad.sha = line.substr(0, 40);
    ad.name = line.substr(sp + 1);
    const size_t nul = ad.name.find('\0');
    if (nul != std::string::npos) {
      if (capabilities && capabilities->empty()) {
        *capabilities = ad.name.substr(nul + 1);
      }
      ad.name.resize(nul);
    }
    refs.push_back(std::move(ad));
  }
  return refs.empty() ? ESP32GIT_INVALID_REF : ESP32GIT_OK;
}

esp32git_status remote_head(const esp32git_remote &auth, const char *url,
                            const char *branch, char out[41],
                            std::string *capabilities = nullptr) {
  std::vector<RefAd> refs;
  const esp32git_status st = discover(auth, url, "git-upload-pack", refs,
                                     capabilities);
  if (st != ESP32GIT_OK) return st;
  const std::string want = esp32git_branch_ref(branch);
  for (const auto &r : refs) {
    if (r.name == want) {
      memcpy(out, r.sha.c_str(), 41);
      return ESP32GIT_OK;
    }
  }
  return ESP32GIT_INVALID_REF; // unborn remote branch
}

bool has_capability(const std::string &capabilities, const char *name) {
  const std::string padded = " " + capabilities + " ";
  return padded.find(std::string(" ") + name + " ") != std::string::npos;
}

// Git's upload-pack may send shallow/ACK pkt-lines before the raw PACK bytes.
bool pack_offset(const uint8_t *data, size_t len, size_t &offset) {
  offset = 0;
  while (offset + 4 <= len && offset < 4096) {
    if (memcmp(data + offset, "PACK", 4) == 0) return true;
    char hex[5];
    memcpy(hex, data + offset, 4);
    hex[4] = '\0';
    char *end = nullptr;
    const unsigned long packet = strtoul(hex, &end, 16);
    if (end != hex + 4 || packet > len - offset ||
        (packet > 0 && packet < 4)) return false;
    if (packet == 0) {
      offset += 4;
      continue;
    }
    const char *line = reinterpret_cast<const char *>(data + offset + 4);
    const size_t line_len = packet - 4;
    if (!(line_len >= 4 && memcmp(line, "NAK\n", 4) == 0) &&
        !(line_len >= 4 && memcmp(line, "ACK ", 4) == 0) &&
        !(line_len >= 8 && memcmp(line, "shallow ", 8) == 0) &&
        !(line_len >= 10 && memcmp(line, "unshallow ", 10) == 0)) {
      return false;
    }
    offset += packet;
  }
  return false;
}

esp32git_status store_pack(const uint8_t *data, size_t len, const char *repo_path) {
  bool ok = true;
  if (!pack_read(data, len, repo_path, [&](const PackEntry &e) {
        const char *type = e.type == PACK_COMMIT    ? "commit"
                           : e.type == PACK_TREE    ? "tree"
                           : e.type == PACK_TAG     ? "tag"
                                                    : "blob";
        char got[41];
        if (esp32git_object_write(repo_path, type, e.data.data(), e.data.size(),
                                  got) != ESP32GIT_OK) {
          ok = false;
        }
      })) {
    return ESP32GIT_PROTOCOL_ERROR; // malformed pack
  }
  return ok ? ESP32GIT_OK : ESP32GIT_IO_ERROR;
}

esp32git_status store_pack_file(const std::string &path, uint64_t offset,
                                uint64_t len, const char *repo_path) {
  bool ok = true;
  if (!pack_read_file(path, offset, len, repo_path,
                      [&](const PackEntry &e) {
                        const char *type = e.type == PACK_COMMIT    ? "commit"
                                           : e.type == PACK_TREE    ? "tree"
                                           : e.type == PACK_TAG     ? "tag"
                                                                    : "blob";
                        char got[41];
                        if (esp32git_object_write(repo_path, type, e.data.data(),
                                                  e.data.size(), got) !=
                            ESP32GIT_OK) {
                          ok = false;
                        }
                      })) {
    return ESP32GIT_PROTOCOL_ERROR;
  }
  return ok ? ESP32GIT_OK : ESP32GIT_IO_ERROR;
}

struct PackFileSink {
  e32g::File *file = nullptr;
  bool ok = true;
};

int write_pack_chunk(void *context, const uint8_t *data, size_t len) {
  auto *sink = static_cast<PackFileSink *>(context);
  if (!sink || !sink->file || (len > 0 && !data) ||
      !sink->file->write(data, len)) {
    if (sink) sink->ok = false;
    return -1;
  }
  return 0;
}

struct TemporaryFile {
  explicit TemporaryFile(const std::string &path) : path(path) {}
  ~TemporaryFile() { e32g::remove_file(path); }
  std::string path;
};

esp32git_status fetch_pack_stream(const std::string &post_url,
                                  const std::string &request,
                                  const esp32git_remote &auth,
                                  const char *repo_path,
                                  const char *blob_sha = nullptr,
                                  const char *destination = nullptr) {
  const std::string pack_path =
      std::string(repo_path) + "/.git/esp32git-pack.tmp";
  TemporaryFile cleanup(pack_path);
  e32g::File output;
  if (!output.open(pack_path, true)) return ESP32GIT_IO_ERROR;

  PackFileSink sink = {&output, true};
  size_t response_len = 0;
  const int status = active_http->request_stream(
      post_url.c_str(), 1, auth.user, auth.token,
      "application/x-git-upload-pack-request",
      reinterpret_cast<const uint8_t *>(request.data()), request.size(),
      write_pack_chunk, &sink, &response_len);
  if (!output.close()) sink.ok = false;
  if (status == 401 || status == 403) return ESP32GIT_AUTH_FAILED;
  if (status < 0 || !sink.ok) return ESP32GIT_IO_ERROR;
  if (status != 200) return ESP32GIT_PROTOCOL_ERROR;

  const int64_t written = e32g::file_size(pack_path);
  if (written < 0 || (uint64_t)written != response_len) {
    return ESP32GIT_IO_ERROR;
  }

  static uint8_t prefix[4096];
  const size_t prefix_len = response_len < sizeof(prefix) ? response_len : sizeof(prefix);
  e32g::File input;
  if (!input.open(pack_path, false)) return ESP32GIT_IO_ERROR;
  size_t read_at = 0;
  while (read_at < prefix_len) {
    size_t got = 0;
    if (!input.read(prefix + read_at, prefix_len - read_at, &got) || got == 0) {
      return ESP32GIT_IO_ERROR;
    }
    read_at += got;
  }
  if (!input.close()) return ESP32GIT_IO_ERROR;
  size_t payload_offset = 0;
  if (!pack_offset(prefix, prefix_len, payload_offset) ||
      response_len < payload_offset + 32) return ESP32GIT_PROTOCOL_ERROR;
  if (blob_sha) {
    return pack_extract_blob_file(pack_path, payload_offset,
                                  response_len - payload_offset, blob_sha,
                                  destination) ? ESP32GIT_OK : ESP32GIT_PROTOCOL_ERROR;
  }
  return store_pack_file(pack_path, payload_offset,
                         response_len - payload_offset, repo_path);
}

struct TreeItem {
  std::string name;
  bool is_dir = false;
  std::string sha;
};

// Blobs committed on the device may exceed the 64 KiB tree/commit bound, so
// the buffer grows until the object fits (object_read rejects truncation).
bool read_object(const char *repo_path, const std::string &sha, const char *want_type,
                 std::vector<uint8_t> &out) {
  char object_path[576];
  if (!esp32git_object_path(repo_path, sha.c_str(), object_path, sizeof(object_path))) return false;
  for (size_t cap : {kMaxObjectBytes, (size_t)1 << 20, (size_t)8 << 20}) {
    out.resize(cap);
    char type[16];
    size_t len = 0;
    if (esp32git_object_read(repo_path, sha.c_str(), type, sizeof(type), out.data(),
                             out.size(), &len) == ESP32GIT_OK) {
      if (strcmp(type, want_type) != 0) return false;
      out.resize(len);
      return true;
    }
    if (strcmp(want_type, "blob") != 0) break;
  }
  out.clear();
  return false;
}

bool parse_tree(const std::vector<uint8_t> &data, std::vector<TreeItem> &items) {
  items.clear();
  const char *q = reinterpret_cast<const char *>(data.data());
  const char *end = q + data.size();
  while (q < end) {
    const char *sp = (const char *)memchr(q, ' ', (size_t)(end - q));
    if (!sp) return false;
    const char *nul = (const char *)memchr(sp, '\0', (size_t)(end - sp));
    if (!nul || end - nul < 21) return false;
    TreeItem item;
    item.is_dir = (sp - q == 5) && strncmp(q, "40000", 5) == 0;
    item.name.assign(sp + 1, (size_t)(nul - sp - 1));
    char hex[41];
    esp32git_bytes_to_hex((const uint8_t *)(nul + 1), hex);
    item.sha = hex;
    items.push_back(std::move(item));
    q = nul + 21;
  }
  return true;
}

void add_once(std::vector<std::string> &seen, std::vector<PackEntry> &out, int type,
              const std::string &sha, std::vector<uint8_t> &&data) {
  for (const auto &s : seen) if (s == sha) return;
  seen.push_back(sha);
  PackEntry entry;
  entry.type = type;
  entry.sha = sha;
  entry.data = std::move(data);
  out.push_back(std::move(entry));
}

// Trees and blobs of `tree` that differ from `base` ("" = no base). A blob
// that is not stored locally belongs to history the remote already has.
bool collect_tree_diff(const char *repo_path, const std::string &tree,
                       const std::string &base, std::vector<std::string> &seen,
                       std::vector<PackEntry> &out) {
  if (tree == base) return true;
  std::vector<uint8_t> data;
  if (!read_object(repo_path, tree, "tree", data)) return false;
  std::vector<TreeItem> items, base_items;
  if (!parse_tree(data, items)) return false;
  if (!base.empty()) {
    std::vector<uint8_t> base_data;
    if (read_object(repo_path, base, "tree", base_data)) parse_tree(base_data, base_items);
  }
  add_once(seen, out, PACK_TREE, tree, std::move(data));
  for (const auto &item : items) {
    std::string previous;
    for (const auto &b : base_items) {
      if (b.name == item.name && b.is_dir == item.is_dir) previous = b.sha;
    }
    if (item.sha == previous) continue;
    if (item.is_dir) {
      if (!collect_tree_diff(repo_path, item.sha, previous, seen, out)) return false;
    } else {
      std::vector<uint8_t> blob;
      if (read_object(repo_path, item.sha, "blob", blob)) {
        add_once(seen, out, PACK_BLOB, item.sha, std::move(blob));
      }
    }
  }
  return true;
}

// Commits from new_head back to base_head ("" = the whole local history), each
// with only the trees and blobs its parent does not already have.
bool collect_delta_objects(const char *repo_path, const char *new_head,
                           const char *base_head, std::vector<PackEntry> &out) {
  std::vector<std::string> commits{new_head};
  std::vector<std::string> seen;
  size_t visited = 0;
  while (!commits.empty() && visited++ < 4096) { // ponytail: bounded walk
    const std::string commit = commits.back();
    commits.pop_back();
    if (base_head[0] && commit == base_head) continue;
    std::vector<uint8_t> data;
    if (!read_object(repo_path, commit, "commit", data)) continue;
    const std::string text(data.begin(), data.end());
    char tree[41] = "";
    sscanf(text.c_str(), "tree %40s", tree);
    std::vector<std::string> parents;
    for (size_t at = text.find("\nparent "); at != std::string::npos;
         at = text.find("\nparent ", at + 1)) {
      parents.push_back(text.substr(at + 8, 40));
    }
    add_once(seen, out, PACK_COMMIT, commit, std::move(data));
    std::string parent_tree;
    if (!parents.empty()) {
      char ptree[41];
      if (esp32git_head_tree(repo_path, parents[0].c_str(), ptree) == ESP32GIT_OK) parent_tree = ptree;
    }
    if (tree[0] && !collect_tree_diff(repo_path, tree, parent_tree, seen, out)) return false;
    for (const auto &parent : parents) commits.push_back(parent);
  }
  return true;
}

bool safe_relative_path(const char *path) {
  if (!path || !*path || *path == '/') return false;
  const char *part = path;
  for (const char *p = path;; ++p) {
    if (*p == '\\') return false;
    if (*p != '/' && *p != '\0') continue;
    const size_t len = (size_t)(p - part);
    if (len == 0 || (len == 1 && part[0] == '.') ||
        (len == 2 && part[0] == '.' && part[1] == '.')) return false;
    if (*p == '\0') return true;
    part = p + 1;
  }
}

} // namespace

void http_register(const esp32git_http_port *port) { active_http = port; }


esp32git_status fetch_url(const char *remote_url, const char *branch,
                          const char *repo_path, const esp32git_remote &auth,
                          bool partial) {
  if (!active_http) return ESP32GIT_PROTOCOL_ERROR;
  char rhead[41];
  std::string capabilities;
  const esp32git_status rr = remote_head(auth, remote_url, branch, rhead,
                                         partial ? &capabilities : nullptr);
  if (rr == ESP32GIT_INVALID_REF) return ESP32GIT_UP_TO_DATE; // unborn remote
  if (rr != ESP32GIT_OK) return rr;
  if (partial && (!has_capability(capabilities, "filter") ||
                  !has_capability(capabilities, "shallow"))) {
    return ESP32GIT_PROTOCOL_ERROR;
  }
  char local_head[41];
  const esp32git_status local_status = esp32git_resolve_head(repo_path, local_head);
  if (local_status == ESP32GIT_OK && strcmp(local_head, rhead) == 0) {
    return ESP32GIT_UP_TO_DATE;
  }

  // want <rhead> / flush / done  ->  server replies [NAK pkt] + packfile.
  std::string req;
  pkt_write(req, std::string("want ") + rhead +
                 (partial ? " filter shallow ofs-delta " : " ofs-delta ") + AGENT + "\n");
  if (partial) {
    if (local_status == ESP32GIT_OK) {
      pkt_write(req, std::string("shallow ") + local_head + "\n");
    }
    pkt_write(req, "deepen 1\n");
    pkt_write(req, "filter blob:none\n");
  }
  pkt_flush(req);
  if (partial && local_status == ESP32GIT_OK) {
    pkt_write(req, std::string("have ") + local_head + "\n");
  }
  req += "0009done\n";

  const std::string post_url = std::string(remote_url) + "/git-upload-pack";
  esp32git_status st = ESP32GIT_PROTOCOL_ERROR;
  if (active_http->request_stream && e32g::has_file_io()) {
    st = fetch_pack_stream(post_url, req, auth, repo_path);
  } else {
    uint8_t *resp = nullptr;
    size_t resp_len = 0;
    const int status = active_http->request(
        post_url.c_str(), 1, auth.user, auth.token,
        "application/x-git-upload-pack-request", (const uint8_t *)req.data(),
        req.size(), &resp, &resp_len);
    if (status == 401 || status == 403) {
      if (resp) esp32git_free_buffer(resp);
      return ESP32GIT_AUTH_FAILED;
    }
    if (status != 200) {
      if (resp) esp32git_free_buffer(resp);
      return status < 0 ? ESP32GIT_IO_ERROR : ESP32GIT_PROTOCOL_ERROR;
    }
    size_t payload_offset = 0;
    if (pack_offset(resp, resp_len, payload_offset)) {
      st = store_pack(resp + payload_offset, resp_len - payload_offset, repo_path);
    }
    esp32git_free_buffer(resp);
  }
  if (st != ESP32GIT_OK) return st;

  const std::string refname = esp32git_branch_ref(branch);
  char tree[41];
  if (esp32git_head_tree(repo_path, rhead, tree) != ESP32GIT_OK) {
    return ESP32GIT_IO_ERROR;
  }
  if (partial) {
    const esp32git_status co = e32g::checkout_tree_partial_at(repo_path, tree);
    if (co != ESP32GIT_OK) return co;
    const std::string shallow = std::string(rhead) + "\n";
    if (!e32g::write_whole(std::string(repo_path) + "/.git/shallow",
                           reinterpret_cast<const uint8_t *>(shallow.data()),
                           shallow.size())) return ESP32GIT_IO_ERROR;
  } else {
    const esp32git_status co = e32g::checkout_tree_at(repo_path, tree);
    if (co != ESP32GIT_OK) return co;
  }
  return esp32git_write_ref(repo_path, refname.c_str(), rhead);
}

esp32git_status download_path_url(const char *remote_url, const char *repo_path,
                                  const char *relpath,
                                  const esp32git_remote &auth) {
  if (!active_http || !active_http->request_stream || !e32g::has_file_io()) {
    return ESP32GIT_PROTOCOL_ERROR;
  }
  if (!safe_relative_path(relpath)) return ESP32GIT_INVALID_REF;
  char head[41], tree[41], blob[41];
  esp32git_status st = esp32git_resolve_head(repo_path, head);
  if (st != ESP32GIT_OK) return st;
  st = esp32git_head_tree(repo_path, head, tree);
  if (st != ESP32GIT_OK) return st;
  st = esp32git_tree_lookup(repo_path, tree, relpath, blob);
  if (st != ESP32GIT_OK) return st;
  if (!blob[0]) return ESP32GIT_INVALID_REF;

  const std::string destination = std::string(repo_path) + "/" + relpath;
  if (e32g::exists(destination)) return ESP32GIT_UP_TO_DATE;
  const size_t slash = destination.find_last_of('/');
  if (slash == std::string::npos ||
      !e32g::make_dirs(destination.substr(0, slash))) return ESP32GIT_IO_ERROR;
  const std::string temporary = destination + ".esp32git.tmp";
  TemporaryFile cleanup(temporary);

  std::string request;
  pkt_write(request, std::string("want ") + blob + " " + AGENT + "\n");
  pkt_flush(request);
  request += "0009done\n";
  st = fetch_pack_stream(std::string(remote_url) + "/git-upload-pack",
                         request, auth, repo_path, blob, temporary.c_str());
  if (st != ESP32GIT_OK) return st;
  return e32g::rename_file(temporary, destination) ? ESP32GIT_OK
                                                    : ESP32GIT_IO_ERROR;
}

namespace {

constexpr size_t kBatchFiles = 48;
constexpr int kAttempts = 5;

// Retries transient network failures; any other result is final.
template <typename F>
esp32git_status with_retries(F &&attempt) {
  esp32git_status st = ESP32GIT_IO_ERROR;
  for (int i = 0; i < kAttempts; ++i) {
    st = attempt();
    if (st != ESP32GIT_IO_ERROR) return st;
  }
  return st;
}

struct Wanted {
  std::string sha;
  std::string path;
};

// Writes a locally stored blob to its worktree path. The loose object is kept:
// notes with identical content share it; drop_objects removes it afterwards.
bool materialize_object(const char *repo_path, const Wanted &file) {
  char object_path[576];
  if (!esp32git_object_path(repo_path, file.sha.c_str(), object_path,
                            sizeof(object_path))) return false;
  std::vector<uint8_t> data(kMaxObjectBytes);
  char type[16];
  size_t len = 0;
  if (esp32git_object_read(repo_path, file.sha.c_str(), type, sizeof(type),
                           data.data(), data.size(), &len) != ESP32GIT_OK ||
      strcmp(type, "blob") != 0) return false;
  const std::string destination = std::string(repo_path) + "/" + file.path;
  const size_t slash = destination.find_last_of('/');
  if (slash == std::string::npos || !e32g::make_dirs(destination.substr(0, slash))) return false;
  const std::string temporary = destination + ".esp32git.tmp";
  if (!e32g::write_whole(temporary, data.data(), len) ||
      !e32g::rename_file(temporary, destination)) {
    e32g::remove_file(temporary);
    return false;
  }
  return true;
}

// The worktree copies are what later operations read.
void drop_objects(const char *repo_path, const std::vector<const Wanted *> &files) {
  for (const Wanted *file : files) {
    char object_path[576];
    if (esp32git_object_path(repo_path, file->sha.c_str(), object_path, sizeof(object_path))) {
      e32g::remove_file(object_path);
    }
  }
}

bool object_is_local(const char *repo_path, const std::string &sha) {
  char object_path[576];
  return esp32git_object_path(repo_path, sha.c_str(), object_path, sizeof(object_path));
}

std::vector<Wanted> missing_files(const char *repo_path, esp32git_path_filter filter,
                                  void *filter_ctx) {
  std::vector<esp32git_index_entry> index;
  esp32git_index_load(repo_path, &index);
  std::vector<Wanted> missing;
  for (auto &entry : index) {
    if (entry.sha.empty() || !safe_relative_path(entry.path.c_str())) continue;
    if (filter && !filter(filter_ctx, entry.path.c_str())) continue;
    if (e32g::exists(std::string(repo_path) + "/" + entry.path)) continue;
    missing.push_back({entry.sha, entry.path});
  }
  return missing;
}

} // namespace

esp32git_status download_missing_url(const char *remote_url,
                                     const char *repo_path,
                                     const esp32git_remote &auth,
                                     bool notes_only) {
  if (!e32g::exists(std::string(repo_path) + "/.git/esp32git-index")) return ESP32GIT_NOT_A_REPO;
  // Everything else: one streamed request per file, since attachments are
  // usually too large to batch into a pack held in memory.
  for (const auto &file : missing_files(repo_path, notes_only ? esp32git_filter_notes : nullptr,
                                        nullptr)) {
    const esp32git_status st = with_retries([&] {
      return download_path_url(remote_url, repo_path, file.path.c_str(), auth);
    });
    if (st != ESP32GIT_OK && st != ESP32GIT_UP_TO_DATE) return st;
  }
  return ESP32GIT_OK;
}

namespace {

// Fetches a group of blobs in one request. A pack the streaming reader cannot
// finish (a blob over its limit) leaves the rest missing; the remainder is
// split in halves so only the oversized blob ends up as a single streamed
// request.
esp32git_status fetch_group(const char *remote_url, const char *repo_path,
                            const esp32git_remote &auth,
                            std::vector<const Wanted *> files) {
  std::vector<const Wanted *> pending;
  for (const Wanted *file : files) {
    if (!(object_is_local(repo_path, file->sha) && materialize_object(repo_path, *file))) {
      pending.push_back(file);
    }
  }
  if (pending.empty()) return ESP32GIT_OK;
  if (pending.size() == 1) {
    return with_retries([&] {
      const esp32git_status st =
          download_path_url(remote_url, repo_path, pending[0]->path.c_str(), auth);
      return st == ESP32GIT_UP_TO_DATE ? ESP32GIT_OK : st;
    });
  }
  std::string request;
  std::vector<std::string> requested;
  for (const Wanted *file : pending) {
    bool duplicate = false;
    for (const auto &sha : requested) duplicate |= sha == file->sha;
    if (duplicate) continue;
    requested.push_back(file->sha);
    // ofs-delta keeps every delta after its base, as the streaming reader needs.
    pkt_write(request, "want " + file->sha +
                           (requested.size() == 1 ? std::string(" no-progress ofs-delta ") + AGENT
                                                  : std::string()) + "\n");
  }
  pkt_flush(request);
  request += "0009done\n";
  const std::string post_url = std::string(remote_url) + "/git-upload-pack";
  const esp32git_status st = with_retries([&] {
    return fetch_pack_stream(post_url, request, auth, repo_path);
  });
  if (st == ESP32GIT_AUTH_FAILED || st == ESP32GIT_IO_ERROR) return st;
  std::vector<const Wanted *> left;
  for (const Wanted *file : pending) {
    if (!(object_is_local(repo_path, file->sha) && materialize_object(repo_path, *file))) {
      left.push_back(file);
    }
  }
  if (left.empty()) return ESP32GIT_OK;
  const size_t half = left.size() / 2;
  const esp32git_status first = fetch_group(
      remote_url, repo_path, auth, std::vector<const Wanted *>(left.begin(), left.begin() + half));
  if (first != ESP32GIT_OK) return first;
  return fetch_group(remote_url, repo_path, auth,
                     std::vector<const Wanted *>(left.begin() + half, left.end()));
}

} // namespace

esp32git_status download_matching_url(const char *remote_url, const char *repo_path,
                                      const esp32git_remote &auth,
                                      esp32git_path_filter filter, void *filter_ctx,
                                      esp32git_progress_fn progress, void *progress_ctx) {
  if (!active_http || !active_http->request_stream || !e32g::has_file_io()) {
    return ESP32GIT_PROTOCOL_ERROR;
  }
  if (!e32g::exists(std::string(repo_path) + "/.git/esp32git-index")) return ESP32GIT_NOT_A_REPO;
  const std::vector<Wanted> missing = missing_files(repo_path, filter, filter_ctx);
  if (progress) progress(progress_ctx, 0, missing.size(), nullptr);
  for (size_t start = 0; start < missing.size(); start += kBatchFiles) {
    const size_t end = start + kBatchFiles < missing.size() ? start + kBatchFiles : missing.size();
    std::vector<const Wanted *> batch;
    for (size_t i = start; i < end; ++i) batch.push_back(&missing[i]);
    const esp32git_status st = fetch_group(remote_url, repo_path, auth, batch);
    // Loose blobs are shared by notes with identical content; drop them only
    // once the whole batch is on disk.
    drop_objects(repo_path, batch);
    if (st != ESP32GIT_OK) return st;
    if (progress) progress(progress_ctx, end, missing.size(), missing[end - 1].path.c_str());
  }
  return ESP32GIT_OK;
}

esp32git_status push_url(const char *remote_url, const char *branch,
                         const char *repo_path, const esp32git_remote &auth) {
  if (!active_http) return ESP32GIT_PROTOCOL_ERROR;

  // Old value of the remote ref ("" when the branch does not exist yet).
  std::vector<RefAd> ads;
  const esp32git_status dd = discover(auth, remote_url, "git-receive-pack", ads);
  if (dd != ESP32GIT_OK && dd != ESP32GIT_INVALID_REF) return dd;
  char old_sha[41] = "";
  const std::string want = esp32git_branch_ref(branch);
  for (const auto &r : ads) {
    if (r.name == want) memcpy(old_sha, r.sha.c_str(), 41);
  }

  char lhead[41];
  const esp32git_status rl = esp32git_resolve_head(repo_path, lhead);
  if (rl != ESP32GIT_OK) return rl; // nothing to push
  if (old_sha[0] && !esp32git_is_ancestor(repo_path, old_sha, lhead)) {
    return ESP32GIT_REMOTE_DIVERGED; // fast-forward only, by design
  }

  // Send everything reachable from the new head that the remote cannot
  // already have: the closure of new minus the closure of old. When the old
  // head is not present locally we conservatively send the full closure.
  // The probe needs room for the whole commit object; object_read fails on
  // truncated buffers, and a false negative makes us resend shared history.
  std::vector<char> probe_buf(4096);
  size_t probe_len = 0;
  char probe_type[16] = "";
  if (old_sha[0] &&
      esp32git_object_read(repo_path, old_sha, probe_type, sizeof(probe_type),
                           probe_buf.data(), probe_buf.size(), &probe_len) !=
          ESP32GIT_OK) {
    old_sha[0] = '\0'; // old head unknown locally: send full closure
  }
  std::vector<PackEntry> entries;
  if (!collect_delta_objects(repo_path, lhead, old_sha, entries)) return ESP32GIT_IO_ERROR;
  if (entries.empty()) return ESP32GIT_UP_TO_DATE;

  const std::vector<uint8_t> pack = pack_write(entries);

  std::string body;
  // pkt payload must carry the NUL before capabilities: build manually.
  const char *old_hex = old_sha[0] ? old_sha : "0000000000000000000000000000000000000000";
  std::string payload = std::string(old_hex) + " " + lhead + " " + want;
  payload.append(1, '\0'); // ref/command NUL separator before capabilities
  payload += AGENT;
  payload += " report-status"; // ask for unpack/ok lines so failures are visible
  payload += "\n";
  pkt_write(body, payload);
  pkt_flush(body);
  body.append((const char *)pack.data(), pack.size());

  uint8_t *resp = nullptr;
  size_t resp_len = 0;
  const std::string post_url = std::string(remote_url) + "/git-receive-pack";
  const int status = active_http->request(
      post_url.c_str(), 1, auth.user, auth.token,
      "application/x-git-receive-pack-request", (const uint8_t *)body.data(),
      body.size(), &resp, &resp_len);
  if (status == 401 || status == 403) {
    if (resp) esp32git_free_buffer(resp);
    return ESP32GIT_AUTH_FAILED;
  }
  if (status != 200) {
    if (resp) esp32git_free_buffer(resp);
    return status < 0 ? ESP32GIT_IO_ERROR : ESP32GIT_PROTOCOL_ERROR;
  }
  std::vector<std::string> lines;
  const bool framed = pkt_split(resp, resp_len, lines);
  esp32git_free_buffer(resp);
  if (!framed) return ESP32GIT_PROTOCOL_ERROR;
  for (auto &line : lines) {
    if (line.rfind("unpack ", 0) == 0 && line.find("ok") == std::string::npos) {
      return ESP32GIT_PROTOCOL_ERROR; // "unpack <err>"
    }
    if (line.rfind("ng ", 0) == 0) return ESP32GIT_PROTOCOL_ERROR;
  }
  return ESP32GIT_OK;
}

} // namespace e32g

void esp32git_http_register(const esp32git_http_port *port) {
  e32g::http_register(port);
}

void esp32git_free_buffer(uint8_t *body) { delete[] body; }

esp32git_status esp32git_fetch_url(const char *remote_url, const char *branch,
                                   const char *repo_path) {
  const esp32git_remote anon = {nullptr, nullptr, nullptr};
  return e32g::fetch_url(remote_url, branch, repo_path, anon, false);
}

esp32git_status esp32git_fetch_url_auth(const char *remote_url, const char *branch,
                                        const char *repo_path,
                                        const esp32git_remote *auth) {
  const esp32git_remote anon = {nullptr, nullptr, nullptr};
  return e32g::fetch_url(remote_url, branch, repo_path, auth ? *auth : anon, false);
}

esp32git_status esp32git_push_url(const char *remote_url, const char *branch,
                                  const char *repo_path) {
  const esp32git_remote anon = {nullptr, nullptr, nullptr};
  return e32g::push_url(remote_url, branch, repo_path, anon);
}

esp32git_status esp32git_push_url_auth(const char *remote_url, const char *branch,
                                       const char *repo_path,
                                       const esp32git_remote *auth) {
  const esp32git_remote anon = {nullptr, nullptr, nullptr};
  return e32g::push_url(remote_url, branch, repo_path, auth ? *auth : anon);
}

esp32git_status esp32git_clone_url(const char *remote_url, const char *branch,
                                   const char *workdir,
                                   const esp32git_remote *auth) {
  esp32git_status st = esp32git_repo_init(workdir);
  if (st != ESP32GIT_OK) return st;
  const std::string head_ref = "ref: " + esp32git_branch_ref(branch) + "\n";
  if (!e32g::write_whole(std::string(workdir) + "/.git/HEAD",
                         reinterpret_cast<const uint8_t *>(head_ref.data()),
                         head_ref.size())) return ESP32GIT_IO_ERROR;
  const esp32git_remote anon = {nullptr, nullptr, nullptr};
  st = e32g::fetch_url(remote_url, branch, workdir, auth ? *auth : anon, false);
  if (st != ESP32GIT_OK) return st;
  char head[41];
  st = esp32git_resolve_head(workdir, head);
  if (st != ESP32GIT_OK) return st;
  char tree[41];
  st = esp32git_head_tree(workdir, head, tree);
  if (st != ESP32GIT_OK) return st;
  return e32g::checkout_tree_at(workdir, tree);
}

esp32git_status esp32git_fetch_url_partial(const char *remote_url,
                                           const char *branch,
                                           const char *repo_path,
                                           const esp32git_remote *auth) {
  const esp32git_remote anon = {nullptr, nullptr, nullptr};
  return e32g::fetch_url(remote_url, branch, repo_path, auth ? *auth : anon,
                         true);
}

esp32git_status esp32git_clone_url_partial(const char *remote_url,
                                           const char *branch,
                                           const char *workdir,
                                           const esp32git_remote *auth) {
  const esp32git_status st = esp32git_repo_init(workdir);
  if (st != ESP32GIT_OK) return st;
  const std::string head_ref = "ref: " + esp32git_branch_ref(branch) + "\n";
  if (!e32g::write_whole(std::string(workdir) + "/.git/HEAD",
                         reinterpret_cast<const uint8_t *>(head_ref.data()),
                         head_ref.size())) return ESP32GIT_IO_ERROR;
  return esp32git_fetch_url_partial(remote_url, branch, workdir, auth);
}

esp32git_status esp32git_download_path_url(const char *remote_url,
                                           const char *repo_path,
                                           const char *relpath,
                                           const esp32git_remote *auth) {
  const esp32git_remote anon = {nullptr, nullptr, nullptr};
  return e32g::download_path_url(remote_url, repo_path, relpath,
                                  auth ? *auth : anon);
}

int esp32git_filter_notes(void *, const char *relpath) {
  const size_t n = strlen(relpath);
  return (n >= 3 && strcmp(relpath + n - 3, ".md") == 0) ||
         (n >= 4 && strcmp(relpath + n - 4, ".txt") == 0);
}

esp32git_status esp32git_download_missing_notes_url(const char *remote_url,
                                                    const char *repo_path,
                                                    const esp32git_remote *auth) {
  const esp32git_remote anon = {nullptr, nullptr, nullptr};
  return e32g::download_matching_url(remote_url, repo_path, auth ? *auth : anon,
                                     esp32git_filter_notes, nullptr, nullptr, nullptr);
}

esp32git_status esp32git_download_missing_matching_url(
    const char *remote_url, const char *repo_path, const esp32git_remote *auth,
    esp32git_path_filter filter, void *filter_ctx, esp32git_progress_fn progress,
    void *progress_ctx) {
  const esp32git_remote anon = {nullptr, nullptr, nullptr};
  return e32g::download_matching_url(remote_url, repo_path, auth ? *auth : anon, filter,
                                     filter_ctx, progress, progress_ctx);
}

esp32git_status esp32git_download_missing_files_url(const char *remote_url,
                                                    const char *repo_path,
                                                    const esp32git_remote *auth) {
  const esp32git_remote anon = {nullptr, nullptr, nullptr};
  return e32g::download_missing_url(remote_url, repo_path,
                                    auth ? *auth : anon, false);
}
