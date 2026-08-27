#include "esp32_git.h"
#include "hexutil.h"

#include <new>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "esp32_git.h"
#include "history.h"
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
                         const char *service, std::vector<RefAd> &refs) {
  if (!active_http) return ESP32GIT_PROTOCOL_ERROR;
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
    if (nul != std::string::npos) ad.name.resize(nul); // drop capability block
    refs.push_back(std::move(ad));
  }
  return refs.empty() ? ESP32GIT_INVALID_REF : ESP32GIT_OK;
}

esp32git_status remote_head(const esp32git_remote &auth, const char *url,
                            const char *branch, char out[41]) {
  std::vector<RefAd> refs;
  const esp32git_status st = discover(auth, url, "git-upload-pack", refs);
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

} // namespace

void http_register(const esp32git_http_port *port) { active_http = port; }


esp32git_status fetch_url(const char *remote_url, const char *branch,
                          const char *repo_path, const esp32git_remote &auth) {
  if (!active_http) return ESP32GIT_PROTOCOL_ERROR;
  char rhead[41];
  const esp32git_status rr = remote_head(auth, remote_url, branch, rhead);
  if (rr == ESP32GIT_INVALID_REF) return ESP32GIT_UP_TO_DATE; // unborn remote
  if (rr != ESP32GIT_OK) return rr;

  // want <rhead> / flush / done  ->  server replies [NAK pkt] + packfile.
  std::string req;
  pkt_write(req, std::string("want ") + rhead + " " + AGENT + "\n");
  pkt_flush(req);
  req += "0009done\n";

  uint8_t *resp = nullptr;
  size_t resp_len = 0;
  const std::string post_url = std::string(remote_url) + "/git-upload-pack";
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
    return ESP32GIT_PROTOCOL_ERROR;
  }
  const uint8_t *pack = resp;
  size_t pack_len = resp_len;
  // Without side-band the server prefixes the pack with a "NAK\n" pkt.
  if (pack_len >= 8 && memcmp(pack, "0008", 4) == 0) {
    pack += 8;
    pack_len -= 8;
  }
  const esp32git_status st = store_pack(pack, pack_len, repo_path);
  {
    char ptype[16] = "";
    uint8_t probe[8];
    size_t plen = 0;
    const esp32git_status pr = esp32git_object_read(repo_path, rhead, ptype, sizeof(ptype), probe, sizeof(probe), &plen);
  }
  esp32git_free_buffer(resp);
  if (st != ESP32GIT_OK) return st;

  const std::string refname = esp32git_branch_ref(branch);
  char lhead[41];
  const esp32git_status rl = esp32git_resolve_head(repo_path, lhead);
  if (rl == ESP32GIT_INVALID_REF) {
    const esp32git_status w = esp32git_write_ref(repo_path, refname.c_str(), rhead);
    char tree[41];
    if (esp32git_head_tree(repo_path, rhead, tree) != ESP32GIT_OK) {
      return ESP32GIT_IO_ERROR;
    }
    const esp32git_status co = e32g::checkout_tree_at(repo_path, tree);
    return co;
  }
  if (strcmp(lhead, rhead) == 0) return ESP32GIT_UP_TO_DATE;
  const esp32git_status w = esp32git_write_ref(repo_path, refname.c_str(), rhead);
  char tree[41];
  if (esp32git_head_tree(repo_path, rhead, tree) != ESP32GIT_OK) {
    return ESP32GIT_IO_ERROR;
  }
  const esp32git_status co = e32g::checkout_tree_at(repo_path, tree);
  return co;
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
