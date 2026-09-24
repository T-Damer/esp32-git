// End-to-end smart-HTTP test against a real `git http-backend` server.
// The HTTP port is a thin libcurl wrapper; the server side is stock git.

#include "esp32_git.h"

#include <curl/curl.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>

#include <stdio.h>




namespace {

size_t write_cb(uint8_t *ptr, size_t size, size_t nmemb, void *userp) {
  auto *out = (std::string *)userp;
  out->append((const char *)ptr, size * nmemb);
  return size * nmemb;
}

int http_request(const char *url, int is_post, const char *user,
                 const char *token, const char *content_type,
                 const uint8_t *body, size_t body_len, uint8_t **out_body,
                 size_t *out_len) {
  CURL *curl = curl_easy_init();
  if (!curl) return -1;
  std::string response;
  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_slist *headers = nullptr;
  if (is_post) {
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body_len);
    if (content_type) {
      headers = curl_slist_append(nullptr, (std::string("Content-Type: ") + content_type).c_str());
      curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    }
  }
  if (user && token) {
    curl_easy_setopt(curl, CURLOPT_USERNAME, user);
    curl_easy_setopt(curl, CURLOPT_PASSWORD, token);
  }
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
  const CURLcode rc = curl_easy_perform(curl);
  long status = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
  if (headers) curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
  if (rc != CURLE_OK) {
    return -1;
  }
  *out_len = response.size();
  *out_body = new uint8_t[*out_len + 1];
  memcpy(*out_body, response.data(), *out_len);
  return (int)status;
}

struct StreamSink {
  esp32git_http_write_callback write;
  void *context;
  size_t total = 0;
  bool failed = false;
};

size_t stream_write_cb(uint8_t *ptr, size_t size, size_t nmemb, void *userp) {
  auto *sink = (StreamSink *)userp;
  const size_t len = size * nmemb;
  if (!sink || sink->write(sink->context, ptr, len) != 0) {
    if (sink) sink->failed = true;
    return 0;
  }
  sink->total += len;
  return len;
}

int http_request_stream(const char *url, int is_post, const char *user,
                        const char *token, const char *content_type,
                        const uint8_t *body, size_t body_len,
                        esp32git_http_write_callback write, void *context,
                        size_t *out_len) {
  if (!write || !out_len) return -1;
  CURL *curl = curl_easy_init();
  if (!curl) return -1;
  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_slist *headers = nullptr;
  if (is_post) {
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body_len);
    if (content_type) {
      headers = curl_slist_append(nullptr, (std::string("Content-Type: ") + content_type).c_str());
      curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    }
  }
  if (user && token) {
    curl_easy_setopt(curl, CURLOPT_USERNAME, user);
    curl_easy_setopt(curl, CURLOPT_PASSWORD, token);
  }
  StreamSink sink = {write, context};
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, stream_write_cb);
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

const esp32git_http_port kCurlPort = {http_request, http_request_stream};

void run(const char *cmd, char *out, size_t cap) {
  FILE *f = popen(cmd, "r");
  const size_t n = fread(out, 1, cap - 1, f);
  out[n] = '\0';
  pclose(f);
}

void trim(char *s) {
  const size_t n = strlen(s);
  if (n && s[n - 1] == '\n') s[n - 1] = '\0';
}

int failures = 0;

#define CHECK(cond, msg)                                                       \
  do {                                                                         \
    if (!(cond)) {                                                             \
      fprintf(stderr, "FAIL: %s\n", msg);                                      \
      failures++;                                                              \
    }                                                                          \
  } while (0)

} // namespace

int main(void) {
  system("rm -rf build/fixtures/http && mkdir -p build/fixtures/http/root");
  if (system("git init -q --bare build/fixtures/http/root/vault.git && "
             "git --git-dir=build/fixtures/http/root/vault.git "
             "symbolic-ref HEAD refs/heads/main") != 0) {
    fprintf(stderr, "FAIL: bare origin\n");
    return 1;
  }
  // http-backend requires an export marker or GIT_HTTP_EXPORT_ALL (set server-side).
  system("git --git-dir=build/fixtures/http/root/vault.git config http.receivepack true");
  system("git --git-dir=build/fixtures/http/root/vault.git config uploadpack.allowFilter true");
  system("git --git-dir=build/fixtures/http/root/vault.git config uploadpack.allowReachableSHA1InWant true");

  // Start the CGI server.
  char start[256];
  snprintf(start, sizeof(start),
           "python3 test/http_backend.py 8931 build/fixtures/http/root "
           "> build/fixtures/http/server.log 2>&1 & echo $! > build/fixtures/http/server.pid");
  FILE *pf = popen(start, "r");
  char pid[16] = "";
  fread(pid, 1, sizeof(pid) - 1, pf);
  pclose(pf);
  atexit([] { system("kill $(cat build/fixtures/http/server.pid 2>/dev/null) 2>/dev/null"); });
  sleep(1); // let it bind

  esp32git_http_register(&kCurlPort);
  const char *url = "http://127.0.0.1:8931/vault.git";
  const char *work = "build/fixtures/http/device";
  const esp32git_remote auth = {url, "x-access-token", "local-test-token"};
  const esp32git_identity id = {"Vault User", "vault@example.com"};

  CHECK(esp32git_clone_url(url, "main", "build/fixtures/http/denied", nullptr) ==
            ESP32GIT_AUTH_FAILED,
        "private remote rejects anonymous clone");

  // ---- clone an EMPTY remote (must stay OK and leave an unborn branch) -----
  const esp32git_status cl = esp32git_clone_url(url, "main", work, &auth);
  CHECK(cl == ESP32GIT_OK || cl == ESP32GIT_UP_TO_DATE, "clone of empty remote");

  // ---- commit locally and PUSH over smart HTTP ------------------------------
  FILE *f = fopen("build/fixtures/http/device/note.md", "wb");
  fputs("# pushed from esp32-git\n", f);
  fclose(f);
  CHECK(esp32git_add(work, "note.md") == ESP32GIT_OK, "add");
  char c1[41];
  CHECK(esp32git_commit(work, &id, "device commit", c1) == ESP32GIT_OK, "commit");
  const esp32git_status push = esp32git_push_url_auth(url, "main", work, &auth);
  CHECK(push == ESP32GIT_OK, "push over smart HTTP");

  // Stock git verifies the server side.
  char out[512];
  run("git --git-dir=build/fixtures/http/root/vault.git log --format=%s main", out,
      sizeof(out));
  trim(out);
  CHECK(strcmp(out, "device commit") == 0, "server history readable by real git");

  // ---- a second clone (real git this time) sees the pushed content ---------
  run("rm -rf build/fixtures/http/pc && git clone -q build/fixtures/http/root/vault.git "
      "build/fixtures/http/pc 2>&1",
      out, sizeof(out));
  f = fopen("build/fixtures/http/pc/note.md", "rb");
  CHECK(f != NULL, "real git clone has the pushed note");
  if (f) fclose(f);

  // ---- PC pushes; device pulls over HTTP ------------------------------------
  f = fopen("build/fixtures/http/pc/from-pc.md", "wb");
  fputs("written on the pc\n", f);
  fclose(f);
  system("git -C build/fixtures/http/pc add from-pc.md && git -C build/fixtures/http/pc "
         "-c user.name=pc -c user.email=pc@x commit -qm 'pc commit'");
  system("git -C build/fixtures/http/pc push -q origin main 2>&1");
  CHECK(esp32git_fetch_url_auth(url, "main", work, &auth) == ESP32GIT_OK, "pull over private smart HTTP");
  f = fopen("build/fixtures/http/device/from-pc.md", "rb");
  CHECK(f != NULL, "pull materialized the PC's file");
  if (f) fclose(f);
  CHECK(access("build/fixtures/http/device/.git/esp32git-pack.tmp", F_OK) != 0,
        "streamed pack temporary file was cleaned up");

  // Shallow partial clone keeps notes + catalog but leaves a large book
  // available for an explicit fetch by its path in HEAD's tree.
  system("mkdir -p build/fixtures/http/pc/Books");
  f = fopen("build/fixtures/http/pc/Books/book.epub", "wb");
  uint32_t random = 0x12345678;
  for (int i = 0; i < 512 * 1024; ++i) {
    random ^= random << 13;
    random ^= random >> 17;
    random ^= random << 5;
    fputc(random & 0xff, f);
  }
  fclose(f);
  f = fopen("build/fixtures/http/pc/Books/catalog.json", "wb");
  fputs("{\"books\":[{\"path\":\"Books/book.epub\"}]}\n", f);
  fclose(f);
  f = fopen("build/fixtures/http/pc/large.md", "wb");
  for (int i = 0; i < 80000; ++i) fputc('a' + (i % 26), f);
  fclose(f);
  system("git -C build/fixtures/http/pc add Books large.md && git -C build/fixtures/http/pc "
         "-c user.name=pc -c user.email=pc@x commit -qm 'add book' && "
         "git -C build/fixtures/http/pc push -q origin main");
  const char *partial = "build/fixtures/http/partial";
  CHECK(esp32git_clone_url_partial(url, "main", partial, &auth) == ESP32GIT_OK,
        "partial clone over private smart HTTP");
  CHECK(access("build/fixtures/http/partial/from-pc.md", F_OK) == 0,
        "partial clone materialized note");
  CHECK(access("build/fixtures/http/partial/Books/catalog.json", F_OK) == 0,
        "partial clone materialized catalog");
  CHECK(access("build/fixtures/http/partial/Books/book.epub", F_OK) != 0,
        "partial clone omitted large book");
  CHECK(access("build/fixtures/http/partial/large.md", F_OK) != 0,
        "partial clone omitted oversized note");
  CHECK(access("build/fixtures/http/partial/.git/shallow", F_OK) == 0,
        "partial clone records shallow boundary");
  CHECK(esp32git_download_missing_notes_url(url, partial, &auth) == ESP32GIT_OK,
        "complete omitted notes");
  CHECK(access("build/fixtures/http/partial/large.md", F_OK) == 0,
        "oversized note is available locally");
  CHECK(access("build/fixtures/http/partial/Books/book.epub", F_OK) != 0,
        "completing notes leaves book on demand");
  CHECK(esp32git_download_path_url(url, partial, "Books/book.epub", &auth) ==
            ESP32GIT_OK, "download omitted book by Git path");
  CHECK(esp32git_download_path_url(url, partial, "Books/book.epub", &auth) ==
            ESP32GIT_UP_TO_DATE, "download preserves existing local book");
  CHECK(esp32git_download_path_url(url, partial, "../outside", &auth) ==
            ESP32GIT_INVALID_REF, "download rejects path traversal");
  char source_sha[128], fetched_sha[128];
  run("git hash-object build/fixtures/http/pc/Books/book.epub", source_sha,
      sizeof(source_sha));
  run("git hash-object build/fixtures/http/partial/Books/book.epub", fetched_sha,
      sizeof(fetched_sha));
  CHECK(strcmp(source_sha, fetched_sha) == 0,
        "downloaded book matches source Git blob");
  CHECK(access("build/fixtures/http/partial/Books/book.epub.esp32git.tmp", F_OK) != 0,
        "download temporary file was cleaned up");

  f = fopen("build/fixtures/http/pc/after-clone.md", "wb");
  fputs("# New note\n", f);
  fclose(f);
  system("git -C build/fixtures/http/pc add after-clone.md && "
         "git -C build/fixtures/http/pc -c user.name=pc -c user.email=pc@x "
         "commit -qm 'new note' && git -C build/fixtures/http/pc push -q origin main");
  CHECK(esp32git_fetch_url_partial(url, "main", partial, &auth) == ESP32GIT_OK,
        "partial fetch updates a clean vault");
  CHECK(access("build/fixtures/http/partial/after-clone.md", F_OK) == 0,
        "partial fetch materializes new note");

  f = fopen("build/fixtures/http/partial/after-clone.md", "ab");
  fputs("local edit\n", f);
  fclose(f);
  f = fopen("build/fixtures/http/pc/after-clone.md", "ab");
  fputs("remote edit\n", f);
  fclose(f);
  system("git -C build/fixtures/http/pc add after-clone.md && "
         "git -C build/fixtures/http/pc -c user.name=pc -c user.email=pc@x "
         "commit -qm 'remote edit' && git -C build/fixtures/http/pc push -q origin main");
  CHECK(esp32git_fetch_url_partial(url, "main", partial, &auth) ==
            ESP32GIT_REMOTE_DIVERGED, "partial fetch protects local edits");
  f = fopen("build/fixtures/http/partial/after-clone.md", "rb");
  char note[100] = {};
  if (f) { fread(note, 1, sizeof(note) - 1, f); fclose(f); }
  CHECK(strstr(note, "local edit") != nullptr &&
        strstr(note, "remote edit") == nullptr,
        "conflict leaves local note unchanged");

  if (failures == 0) {
    printf("all http checks passed\n");
    return 0;
  }
  printf("%d failure(s)\n", failures);
  return 1;
}
