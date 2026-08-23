// Sync test: push/pull/clone against a BARE origin created by real git,
// with stock git verifying both ends. Divergence must be refused.

#include "esp32_git.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

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
  system("rm -rf build/fixtures/origin.git build/fixtures/A build/fixtures/B "
         "build/fixtures/C && mkdir -p build/fixtures");
  if (system("git init -q --bare build/fixtures/origin.git") != 0) {
    fprintf(stderr, "FAIL: bare origin\n");
    return 1;
  }
  const char *origin = "build/fixtures/origin.git";
  const char *A = "build/fixtures/A";
  const char *B = "build/fixtures/B";
  const esp32git_identity id = {"Vault User", "vault@example.com"};

  // ---- A: two commits, then push to the bare origin ------------------------
  CHECK(esp32git_init(A) == ESP32GIT_OK, "init A");
  put_file("build/fixtures/A/note.md", "# hello from A\n");
  CHECK(esp32git_add(A, "note.md") == ESP32GIT_OK, "A add");
  char c1[41];
  CHECK(esp32git_commit(A, &id, "seed vault", c1) == ESP32GIT_OK, "A commit 1");
  put_file("build/fixtures/A/second.md", "more\n");
  CHECK(esp32git_add(A, "second.md") == ESP32GIT_OK, "A add 2");
  char c2[41];
  CHECK(esp32git_commit(A, &id, "second entry", c2) == ESP32GIT_OK, "A commit 2");

  CHECK(esp32git_push(origin, "main", A) == ESP32GIT_OK, "A push");

  // Stock git verifies the origin's history.
  char out[512];
  run("git --git-dir=build/fixtures/origin.git log --format=%s main", out, sizeof(out));
  trim(out);
  CHECK(strncmp(out, "second entry\n", 13) == 0, "origin HEAD message via real git");
  run("git --git-dir=build/fixtures/origin.git fsck --strict 2>&1", out, sizeof(out));
  CHECK(strlen(out) == 0, "origin passes fsck --strict");

  // ---- clone into B; files materialize -------------------------------------
  CHECK(esp32git_clone(origin, "main", B) == ESP32GIT_OK, "clone into B");
  FILE *f = fopen("build/fixtures/B/note.md", "rb");
  CHECK(f != NULL, "cloned worktree has note.md");
  char buf2[128] = "";
  if (f) {
    fread(buf2, 1, sizeof(buf2) - 1, f);
    fclose(f);
  }
  CHECK(strcmp(buf2, "# hello from A\n") == 0, "cloned content matches");

  // ---- B commits and pushes; A pulls the update -----------------------------
  put_file("build/fixtures/B/from-b.md", "written on device B\n");
  CHECK(esp32git_add(B, "from-b.md") == ESP32GIT_OK, "B add");
  char c3[41];
  CHECK(esp32git_commit(B, &id, "entry from b", c3) == ESP32GIT_OK, "B commit");
  CHECK(esp32git_push(origin, "main", B) == ESP32GIT_OK, "B push (ff)");

  CHECK(esp32git_fetch(origin, "main", A) == ESP32GIT_OK, "A pull");
  f = fopen("build/fixtures/A/from-b.md", "rb");
  CHECK(f != NULL, "pull materialized from-b.md in A");
  fclose(f);

  // ---- up-to-date fetch is reported ------------------------------------------
  CHECK(esp32git_fetch(origin, "main", A) == ESP32GIT_UP_TO_DATE, "fetch idempotent");

  // ---- divergence is refused -------------------------------------------------
  // A commits locally while C advances the origin independently.
  put_file("build/fixtures/A/divergent.md", "A-only\n");
  CHECK(esp32git_add(A, "divergent.md") == ESP32GIT_OK, "A diverge add");
  CHECK(esp32git_commit(A, &id, "divergent a", c3) == ESP32GIT_OK, "A diverge commit");

  CHECK(esp32git_clone(origin, "main", "build/fixtures/C") == ESP32GIT_OK,
        "clone C");
  put_file("build/fixtures/C/c-side.md", "C-only\n");
  CHECK(esp32git_add("build/fixtures/C", "c-side.md") == ESP32GIT_OK, "C add");
  CHECK(esp32git_commit("build/fixtures/C", &id, "divergent c", c3) == ESP32GIT_OK,
        "C commit");
  CHECK(esp32git_push(origin, "main", "build/fixtures/C") == ESP32GIT_OK, "C push");

  CHECK(esp32git_push(origin, "main", A) == ESP32GIT_REMOTE_DIVERGED,
        "push refuses divergence");
  CHECK(esp32git_fetch(origin, "main", A) == ESP32GIT_REMOTE_DIVERGED,
        "pull refuses divergence");

  if (failures == 0) {
    printf("all sync checks passed\n");
    return 0;
  }
  printf("%d failure(s)\n", failures);
  return 1;
}
