// Two-way sync against a real `git http-backend`; the "pc" side is stock git.

#include "esp32_git.h"

#include <curl/curl.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>

namespace {

size_t write_cb(uint8_t *ptr, size_t size, size_t nmemb, void *userp) {
  static_cast<std::string *>(userp)->append((const char *)ptr, size * nmemb);
  return size * nmemb;
}

void configure(CURL *curl, const char *url, int is_post, const char *user, const char *token,
               const char *content_type, const uint8_t *body, size_t body_len, curl_slist **headers) {
  curl_easy_setopt(curl, CURLOPT_URL, url);
  if (is_post) {
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body_len);
    if (content_type) {
      *headers = curl_slist_append(nullptr, (std::string("Content-Type: ") + content_type).c_str());
      curl_easy_setopt(curl, CURLOPT_HTTPHEADER, *headers);
    }
  }
  if (user && token) {
    curl_easy_setopt(curl, CURLOPT_USERNAME, user);
    curl_easy_setopt(curl, CURLOPT_PASSWORD, token);
  }
}

int requests = 0;
// When set, runs once just before the device's next push handshake, so the
// remote moves between the device's fetch and its push.
const char *race_command = nullptr;

int http_request(const char *url, int is_post, const char *user, const char *token,
                 const char *content_type, const uint8_t *body, size_t body_len,
                 uint8_t **out_body, size_t *out_len) {
  ++requests;
  if (race_command && strstr(url, "service=git-receive-pack")) {
    const char *command = race_command;
    race_command = nullptr;
    if (system(command) != 0) fprintf(stderr, "race command failed\n");
  }
  CURL *curl = curl_easy_init();
  std::string response;
  curl_slist *headers = nullptr;
  configure(curl, url, is_post, user, token, content_type, body, body_len, &headers);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
  const CURLcode rc = curl_easy_perform(curl);
  long status = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
  if (headers) curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
  if (rc != CURLE_OK) return -1;
  *out_len = response.size();
  *out_body = new uint8_t[*out_len + 1];
  memcpy(*out_body, response.data(), *out_len);
  return (int)status;
}

struct Sink {
  esp32git_http_write_callback write;
  void *context;
  size_t total = 0;
  bool failed = false;
};

size_t stream_cb(uint8_t *ptr, size_t size, size_t nmemb, void *userp) {
  auto *sink = static_cast<Sink *>(userp);
  if (sink->write(sink->context, ptr, size * nmemb) != 0) {
    sink->failed = true;
    return 0;
  }
  sink->total += size * nmemb;
  return size * nmemb;
}

int http_stream(const char *url, int is_post, const char *user, const char *token,
                const char *content_type, const uint8_t *body, size_t body_len,
                esp32git_http_write_callback write, void *context, size_t *out_len) {
  ++requests;
  CURL *curl = curl_easy_init();
  curl_slist *headers = nullptr;
  configure(curl, url, is_post, user, token, content_type, body, body_len, &headers);
  Sink sink = {write, context};
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, stream_cb);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sink);
  const CURLcode rc = curl_easy_perform(curl);
  long status = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
  if (headers) curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
  *out_len = sink.total;
  if (rc != CURLE_OK || sink.failed) return -1;
  return (int)status;
}

const esp32git_http_port kCurlPort = {http_request, http_stream};

int failures = 0;
#define CHECK(cond, msg)                                                       \
  do {                                                                         \
    if (!(cond)) {                                                             \
      fprintf(stderr, "FAIL: %s\n", msg);                                      \
      failures++;                                                              \
    }                                                                          \
  } while (0)

const char *kRoot = "build/fixtures/sync";
const char *kPc = "build/fixtures/sync/pc";
const char *kDevice = "build/fixtures/sync/device";

void sh(const std::string &cmd) {
  if (system(cmd.c_str()) != 0) fprintf(stderr, "command failed: %s\n", cmd.c_str());
}

void put(const std::string &path, const std::string &text) {
  FILE *f = fopen(path.c_str(), "wb");
  fputs(text.c_str(), f);
  fclose(f);
}

std::string get(const std::string &path) {
  FILE *f = fopen(path.c_str(), "rb");
  if (!f) return "<missing>";
  std::string out;
  char buf[4096];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
  fclose(f);
  return out;
}

bool present(const std::string &path) { return access(path.c_str(), F_OK) == 0; }

std::string pc(const std::string &rel) { return std::string(kPc) + "/" + rel; }
std::string device(const std::string &rel) { return std::string(kDevice) + "/" + rel; }

void pc_commit(const std::string &message) {
  sh(std::string("git -C ") + kPc + " add -A && git -C " + kPc +
     " -c user.name=pc -c user.email=pc@x commit -qm '" + message + "' && git -C " + kPc +
     " push -q origin main");
}

void pc_pull() { sh(std::string("git -C ") + kPc + " pull -q --no-rebase origin main"); }

size_t last_done = 0, last_total = 0;
void on_progress(void *, size_t done, size_t total, const char *) {
  last_done = done;
  last_total = total;
}

} // namespace

int main(void) {
  sh(std::string("rm -rf ") + kRoot + " && mkdir -p " + kRoot + "/root");
  sh(std::string("git init -q --bare -b main ") + kRoot + "/root/vault.git");
  const std::string bare = std::string(kRoot) + "/root/vault.git";
  sh("git --git-dir=" + bare + " config http.receivepack true");
  sh("git --git-dir=" + bare + " config uploadpack.allowFilter true");
  sh("git --git-dir=" + bare + " config uploadpack.allowReachableSHA1InWant true");
  sh(std::string("git clone -q ") + bare + " " + kPc + " 2>/dev/null");
  sh(std::string("mkdir -p ") + kPc + "/Notes/deep " + kPc + "/Files");
  for (int i = 0; i < 120; ++i) put(pc("Notes/deep/n" + std::to_string(i) + ".md"), "note " + std::to_string(i) + "\n");
  put(pc("Notes/a.md"), "alpha\n");
  put(pc("Notes/b.md"), "bravo\n");
  put(pc("Notes/c.md"), "charlie\n");
  put(pc("Notes/d.md"), "delta\n");
  put(pc("Notes/e.md"), "echo\n");
  put(pc("Files/picture.png"), std::string(200000, 'p')); // stays on demand
  pc_commit("seed");

  sh(std::string("python3 test/http_backend.py 8932 ") + kRoot + "/root > " + kRoot +
     "/server.log 2>&1 & echo $! > " + kRoot + "/server.pid");
  atexit([] { system("kill $(cat build/fixtures/sync/server.pid 2>/dev/null) 2>/dev/null"); });
  sleep(1);

  esp32git_http_register(&kCurlPort);
  const char *url = "http://127.0.0.1:8932/vault.git";
  const esp32git_remote auth = {url, "x-access-token", "local-test-token"};
  const esp32git_identity id = {"Xteink", "xteink@device"};
  esp32git_sync_options options = {};
  options.identity = &id;
  options.message = "Sync from Xteink";
  options.conflict_tag = "xteink";
  options.progress = on_progress;
  esp32git_sync_report report;

  // ---- first sync: partial clone, notes in batches, attachment on demand ----
  requests = 0;
  CHECK(esp32git_sync_url(url, kDevice, &auth, &options, &report) == ESP32GIT_OK, "first sync clones");
  CHECK(get(device("Notes/deep/n119.md")) == "note 119\n", "notes downloaded");
  CHECK(!present(device("Files/picture.png")), "attachment stays on demand");
  CHECK(last_done == 125 && last_total == 125, "progress reports every note");
  CHECK(requests < 20, "125 notes arrive in a handful of requests, not one each");
  fprintf(stderr, "first sync: %d HTTP requests for 125 notes\n", requests);

  // ---- nothing changed: no commit ----
  CHECK(esp32git_sync_url(url, kDevice, &auth, &options, &report) == ESP32GIT_OK, "idle sync");
  CHECK(report.pushed == 0, "idle sync pushes nothing");

  // ---- edit, create in a new folder, delete; the attachment must survive ----
  put(device("Notes/a.md"), "alpha edited on device\n");
  sh(std::string("mkdir -p ") + kDevice + "/Inbox");
  put(device("Inbox/new.md"), "created on device\n");
  sh("rm " + device("Notes/d.md"));
  CHECK(esp32git_sync_url(url, kDevice, &auth, &options, &report) == ESP32GIT_OK, "sync local changes");
  CHECK(report.pushed == 3, "edit, creation and deletion pushed");
  pc_pull();
  CHECK(get(pc("Notes/a.md")) == "alpha edited on device\n", "pc sees the edit");
  CHECK(get(pc("Inbox/new.md")) == "created on device\n", "pc sees the new note");
  CHECK(!present(pc("Notes/d.md")), "pc sees the deletion");
  CHECK(present(pc("Files/picture.png")), "never-downloaded attachment was not deleted");
  CHECK(get(pc("Notes/deep/n5.md")) == "note 5\n", "untouched notes stay");

  // ---- both sides edit b.md; device also edits c.md; pc deletes e.md ----
  put(pc("Notes/b.md"), "bravo from pc\n");
  sh("rm " + pc("Notes/e.md"));
  pc_commit("pc edits");
  put(device("Notes/b.md"), "bravo from device\n");
  put(device("Notes/c.md"), "charlie from device\n");
  CHECK(esp32git_sync_url(url, kDevice, &auth, &options, &report) == ESP32GIT_OK, "sync with a conflict");
  CHECK(report.conflicts == 1, "one conflict");
  CHECK(get(device("Notes/b.md")) == "bravo from pc\n", "remote version kept at the path");
  CHECK(get(device(report.last_conflict)) == "bravo from device\n", "device version saved as a conflict copy");
  CHECK(strstr(report.last_conflict, "Notes/b (conflict xteink ") == report.last_conflict,
        "conflict copy named after the note");
  CHECK(!present(device("Notes/e.md")), "remote deletion applied on device");
  pc_pull();
  CHECK(get(pc("Notes/c.md")) == "charlie from device\n", "non-conflicting edit pushed");
  CHECK(get(pc(report.last_conflict)) == "bravo from device\n", "pc receives the conflict copy");

  // ---- device deletes what the pc edited: the edit wins ----
  put(pc("Notes/c.md"), "charlie edited on pc\n");
  pc_commit("pc edits c");
  sh("rm " + device("Notes/c.md"));
  CHECK(esp32git_sync_url(url, kDevice, &auth, &options, &report) == ESP32GIT_OK, "delete vs edit");
  CHECK(report.kept_remote == 1, "local deletion skipped");
  CHECK(get(device("Notes/c.md")) == "charlie edited on pc\n", "edited note restored on device");

  // ---- network failure mid-sync keeps local work; the next sync pushes it ----
  put(device("Notes/a.md"), "alpha offline edit\n");
  const esp32git_remote dead = {"http://127.0.0.1:1/vault.git", "x-access-token", "local-test-token"};
  CHECK(esp32git_sync_url("http://127.0.0.1:1/vault.git", kDevice, &dead, &options, &report) ==
            ESP32GIT_IO_ERROR, "offline sync fails");
  CHECK(get(device("Notes/a.md")) == "alpha offline edit\n", "offline edit left in place");
  CHECK(!present(device(".git/esp32git-stash/manifest")), "no stash left behind");
  CHECK(esp32git_sync_url(url, kDevice, &auth, &options, &report) == ESP32GIT_OK, "sync after reconnect");
  pc_pull();
  CHECK(get(pc("Notes/a.md")) == "alpha offline edit\n", "offline edit pushed later");

  // ---- the remote moves between the device's fetch and push ----
  put(device("Notes/a.md"), "alpha during a race\n");
  put(pc("Notes/b.md"), "bravo pushed mid-sync\n");
  static const std::string race = std::string("git -C ") + kPc + " add -A && git -C " + kPc +
                                  " -c user.name=pc -c user.email=pc@x commit -qm race && git -C " +
                                  kPc + " push -q origin main";
  race_command = race.c_str();
  CHECK(esp32git_sync_url(url, kDevice, &auth, &options, &report) == ESP32GIT_OK, "sync survives a race");
  CHECK(race_command == nullptr, "race was injected");
  CHECK(report.pushed == 1 && report.conflicts == 0, "retry pushes the edit without a conflict");
  CHECK(get(device("Notes/b.md")) == "bravo pushed mid-sync\n", "device took the racing commit");
  pc_pull();
  CHECK(get(pc("Notes/a.md")) == "alpha during a race\n", "edit reached the remote after the retry");

  // ---- a note over 64 KiB written on the device ----
  put(device("Notes/long.md"), std::string(150000, 'l'));
  CHECK(esp32git_sync_url(url, kDevice, &auth, &options, &report) == ESP32GIT_OK, "large note sync");
  pc_pull();
  CHECK(get(pc("Notes/long.md")).size() == 150000, "large note pushed");

  // ---- a large note written elsewhere downloads through the fallback path ----
  put(pc("Notes/big-remote.md"), std::string(90000, 'b'));
  pc_commit("big remote note");
  CHECK(esp32git_sync_url(url, kDevice, &auth, &options, &report) == ESP32GIT_OK, "large remote note");
  CHECK(get(device("Notes/big-remote.md")).size() == 90000, "large remote note downloaded");

  char fsck[64];
  FILE *p = popen(("git --git-dir=" + bare + " fsck --strict >/dev/null 2>&1; echo $?").c_str(), "r");
  fgets(fsck, sizeof(fsck), p);
  pclose(p);
  CHECK(atoi(fsck) == 0, "server repository passes git fsck");

  if (failures == 0) fprintf(stderr, "sync: all checks passed\n");
  return failures == 0 ? 0 : 1;
}
