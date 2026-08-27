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

const esp32git_http_port kCurlPort = {http_request};

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
  if (system("git init -q --bare build/fixtures/http/root/vault.git") != 0) {
    fprintf(stderr, "FAIL: bare origin\n");
    return 1;
  }
  // http-backend requires an export marker or GIT_HTTP_EXPORT_ALL (set server-side).
  system("git --git-dir=build/fixtures/http/root/vault.git config http.receivepack true");

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
  const esp32git_identity id = {"Vault User", "vault@example.com"};

  // ---- clone an EMPTY remote (must stay OK and leave an unborn branch) -----
  const esp32git_status cl = esp32git_clone_url(url, "main", work, nullptr);
  CHECK(cl == ESP32GIT_OK || cl == ESP32GIT_UP_TO_DATE, "clone of empty remote");

  // ---- commit locally and PUSH over smart HTTP ------------------------------
  FILE *f = fopen("build/fixtures/http/device/note.md", "wb");
  fputs("# pushed from esp32-git\n", f);
  fclose(f);
  CHECK(esp32git_add(work, "note.md") == ESP32GIT_OK, "add");
  char c1[41];
  CHECK(esp32git_commit(work, &id, "device commit", c1) == ESP32GIT_OK, "commit");
  const esp32git_status push = esp32git_push_url(url, "main", work);
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
  CHECK(esp32git_fetch_url(url, "main", work) == ESP32GIT_OK, "pull over smart HTTP");
  f = fopen("build/fixtures/http/device/from-pc.md", "rb");
  CHECK(f != NULL, "pull materialized the PC's file");
  if (f) fclose(f);

  if (failures == 0) {
    printf("all http checks passed\n");
    return 0;
  }
  printf("%d failure(s)\n", failures);
  return 1;
}
