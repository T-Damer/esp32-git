// End-to-end history test: esp32-git writes a repository that STOCK GIT
// reads, logs, checks, and reports clean status on.

#include "esp32_git.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond, msg)                                                       \
  do {                                                                         \
    if (!(cond)) {                                                             \
      fprintf(stderr, "FAIL: %s\n", msg);                                      \
      failures++;                                                              \
    }                                                                          \
  } while (0)

static void run(const char *cmd, char *out, size_t cap) {
  FILE *f = popen(cmd, "r");
  const size_t n = fread(out, 1, cap - 1, f);
  out[n] = '\0';
  pclose(f);
}

static void trim(char *s) {
  const size_t n = strlen(s);
  if (n && s[n - 1] == '\n') s[n - 1] = '\0';
}

static void put_file(const char *path, const char *text) {
  FILE *f = fopen(path, "wb");
  fputs(text, f);
  fclose(f);
}

int main(void) {
  system("rm -rf build/fixtures/work && mkdir -p build/fixtures/work");
  const char *repo = "build/fixtures/work";
  const char *prefix = "build/fixtures/work/";

  CHECK(esp32git_init(repo) == ESP32GIT_OK, "init");
  const esp32git_identity id = {"Vault User", "vault@example.com"};

  // ---- commit 1: flat + nested file ---------------------------------------
  char p[128];
  snprintf(p, sizeof(p), "%sa.txt", prefix);
  put_file(p, "alpha\n");
  system("mkdir -p build/fixtures/work/notes");
  snprintf(p, sizeof(p), "%snotes/b.txt", prefix);
  put_file(p, "beta\n");
  CHECK(esp32git_add(repo, "a.txt") == ESP32GIT_OK, "add a.txt");
  CHECK(esp32git_add(repo, "notes/b.txt") == ESP32GIT_OK, "add notes/b.txt");

  char c1[41];
  CHECK(esp32git_commit(repo, &id, "first note commit\n", c1) == ESP32GIT_OK,
        "commit 1");

  // Real git must read our log, our trees, and pass fsck.
  char out[512];
  run("git -C build/fixtures/work log --format=%s", out, sizeof(out));
  trim(out);
  CHECK(strcmp(out, "first note commit") == 0, "git log shows our message");
  run("git -C build/fixtures/work ls-tree -r --name-only HEAD", out, sizeof(out));
  CHECK(strstr(out, "a.txt") && strstr(out, "notes/b.txt"),
        "nested tree lists both paths");
  run("git -C build/fixtures/work fsck --strict 2>&1", out, sizeof(out));
  CHECK(strlen(out) == 0, "fsck --strict is silent");

  // ---- status: clean after commit -----------------------------------------
  char st = 0;
  CHECK(esp32git_status_file(repo, "a.txt", &st) == ESP32GIT_OK, "status a.txt");
  CHECK(st == '=', "a.txt is clean ('=') after commit");

  // ---- status: unstaged modification --------------------------------------
  snprintf(p, sizeof(p), "%sa.txt", prefix);
  put_file(p, "alpha changed\n");
  esp32git_status_file(repo, "a.txt", &st);
  CHECK(st == 'U', "modified file is unstaged ('U')");
  CHECK(esp32git_add(repo, "a.txt") == ESP32GIT_OK, "re-add a.txt");
  esp32git_status_file(repo, "a.txt", &st);
  CHECK(st == 'S', "staged change shows 'S'");

  // ---- untracked -----------------------------------------------------------
  snprintf(p, sizeof(p), "%snew.txt", prefix);
  put_file(p, "?\n");
  esp32git_status_file(repo, "new.txt", &st);
  CHECK(st == '?', "untracked shows '?'");

  // ---- commit 2 and history depth ------------------------------------------
  char c2[41];
  CHECK(esp32git_commit(repo, &id, "second note commit\n", c2) == ESP32GIT_OK,
        "commit 2");
  run("git -C build/fixtures/work rev-list --count HEAD", out, sizeof(out));
  trim(out);
  CHECK(strcmp(out, "2") == 0, "two commits visible in git log");
  run("git -C build/fixtures/work show --stat --format=%s HEAD", out, sizeof(out));
  CHECK(strstr(out, "second note commit"), "HEAD message readable by git");
  run("git -C build/fixtures/work status --porcelain", out, sizeof(out));
  CHECK(strlen(out) > 0, "porcelain lists the staged/untracked leftovers");

  if (failures == 0) {
    printf("all history checks passed\n");
    return 0;
  }
  printf("%d failure(s)\n", failures);
  return 1;
}
