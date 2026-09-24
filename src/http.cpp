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
    return ESP32GIT_PROTOCOL_ERROR;
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

// Objects reachable from new_head but not from base_head ("" = all).
bool collect_delta_objects(const char *repo_path, const char *new_head,
                           const char *base_head, std::vector<PackEntry> &out) {
  std::vector<std::string> commits{new_head};
  std::vector<std::string> trees;
  std::vector<std::string> seen_trees;
  std::vector<std::string> seen_blobs; // shared between trees: pack each once
  size_t visited = 0;
  char type[16];
  std::vector<char> buf(kMaxObjectBytes);

  while (!commits.empty() && visited++ < 4096) { // ponytail: bounded walk
    const std::string commit = commits.back();
    commits.pop_back();
    if (base_head[0] && strcmp(commit.c_str(), base_head) == 0) continue;
    size_t len = 0;
    if (esp32git_object_read(repo_path, commit.c_str(), type, sizeof(type),
                             buf.data(), buf.size(), &len) != ESP32GIT_OK) {
      continue;
    }
    PackEntry ce;
    ce.type = PACK_COMMIT;
    ce.sha = commit;
    ce.data.assign(buf.data(), buf.data() + len);
    out.push_back(std::move(ce));

    char tree[41] = "";
    sscanf(buf.data(), "tree %40s", tree);
    if (tree[0]) trees.push_back(tree);

    for (const char *line = buf.data(); line < buf.data() + len;) {
      if (strncmp(line, "parent ", 7) == 0) {
        char parent[41] = "";
        if (sscanf(line, "parent %40s", parent) == 1) commits.push_back(parent);
      }
      const char *nl =
          (const char *)memchr(line, '\n', (size_t)(buf.data() + len - line));
      if (!nl) break;
      line = nl + 1;
    }
  }

  while (!trees.empty()) {
    const std::string t = trees.back();
    trees.pop_back();
    bool dup = false;
    for (const auto &s : seen_trees) dup |= (s == t);
    if (dup) continue;
    seen_trees.push_back(t);

    size_t len = 0;
    if (esp32git_object_read(repo_path, t.c_str(), type, sizeof(type),
                             buf.data(), buf.size(), &len) != ESP32GIT_OK) {
      continue;
    }
    PackEntry te;
    te.type = PACK_TREE;
    te.sha = t;
    te.data.assign(buf.data(), buf.data() + len);
    out.push_back(std::move(te));

    const char *q = buf.data();
    const char *end = buf.data() + len;
    while (q < end) {
      const char *sp = (const char *)memchr(q, ' ', (size_t)(end - q));
      if (!sp) break;
      const char *nul = (const char *)memchr(sp, '\0', (size_t)(end - sp));
      if (!nul || end - nul < 21) break;
      const bool is_dir = (sp - q == 5) && strncmp(q, "40000", 5) == 0;
      char hex[41];
      esp32git_bytes_to_hex((const uint8_t *)(nul + 1), hex);
      if (is_dir) {
        trees.push_back(hex);
      } else {
        bool seen = false;
        for (const auto &s : seen_blobs) seen |= (s == hex);
        if (seen) {
          q = nul + 21;
          continue;
        }
        seen_blobs.push_back(hex);
        uint8_t blob[kMaxObjectBytes];
        size_t blen = 0;
        if (esp32git_object_read(repo_path, hex, type, sizeof(type), blob,
                                 sizeof(blob), &blen) == ESP32GIT_OK) {
          PackEntry be;
          be.type = PACK_BLOB;
          be.sha = hex;
          be.data.assign(blob, blob + blen);
          out.push_back(std::move(be));
        }
      }
      q = nul + 21;
    }
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
                          bool partial = false) {
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
                 (partial ? " filter shallow " : " ") + AGENT + "\n");
  if (partial) {
    if (local_status == ESP32GIT_OK) {
      pkt_write(req, std::string("shallow ") + local_head + "\n");
    }
    pkt_write(req, "deepen 1\n");
    pkt_write(req, "filter blob:limit=60000\n");
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

esp32git_status download_missing_notes_url(const char *remote_url,
                                            const char *repo_path,
                                            const esp32git_remote &auth) {
  e32g::File index;
  if (!index.open(std::string(repo_path) + "/.git/esp32git-index", false)) {
    return ESP32GIT_NOT_A_REPO;
  }
  const auto process = [&](const std::string &line) -> esp32git_status {
    if (line.size() < 43 || line[40] != ' ') return ESP32GIT_PROTOCOL_ERROR;
    const std::string path = line.substr(41);
    const bool note = (path.size() >= 3 && path.compare(path.size() - 3, 3, ".md") == 0) ||
                      (path.size() >= 4 && path.compare(path.size() - 4, 4, ".txt") == 0);
    if (!note) return ESP32GIT_OK;
    const std::string destination = std::string(repo_path) + "/" + path;
    if (e32g::exists(destination)) return ESP32GIT_OK;
    const std::string sha = line.substr(0, 40);
    char object_path[576];
    if (esp32git_object_path(repo_path, sha.c_str(), object_path,
                             sizeof(object_path))) return ESP32GIT_OK;
    return download_path_url(remote_url, repo_path, path.c_str(), auth);
  };
  uint8_t buffer[1024];
  std::string line;
  for (;;) {
    size_t got = 0;
    if (!index.read(buffer, sizeof(buffer), &got)) return ESP32GIT_IO_ERROR;
    if (got == 0) break;
    for (size_t i = 0; i < got; ++i) {
      if (buffer[i] == '\n') {
        const esp32git_status st = process(line);
        if (st != ESP32GIT_OK) return st;
        line.clear();
      } else {
        if (line.size() >= 1024) return ESP32GIT_PROTOCOL_ERROR;
        line.push_back((char)buffer[i]);
      }
    }
  }
  return line.empty() ? ESP32GIT_OK : ESP32GIT_PROTOCOL_ERROR;
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
  collect_delta_objects(repo_path, lhead, old_sha, entries);
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
    return ESP32GIT_PROTOCOL_ERROR;
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
  return e32g::fetch_url(remote_url, branch, repo_path, anon);
}

esp32git_status esp32git_fetch_url_auth(const char *remote_url, const char *branch,
                                        const char *repo_path,
                                        const esp32git_remote *auth) {
  const esp32git_remote anon = {nullptr, nullptr, nullptr};
  return e32g::fetch_url(remote_url, branch, repo_path, auth ? *auth : anon);
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
  const esp32git_remote anon = {nullptr, nullptr, nullptr};
  st = e32g::fetch_url(remote_url, branch, workdir, auth ? *auth : anon);
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

esp32git_status esp32git_download_missing_notes_url(const char *remote_url,
                                                    const char *repo_path,
                                                    const esp32git_remote *auth) {
  const esp32git_remote anon = {nullptr, nullptr, nullptr};
  return e32g::download_missing_notes_url(remote_url, repo_path,
                                           auth ? *auth : anon);
}
