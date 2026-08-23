// Proves the storage-port seam: a registered port is actually used, and a
// NULL member falls back cleanly to the stdio backend.

#include "esp32_git.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int g_write_calls = 0;

static int64_t port_size(const char *path) {
  struct stat st;
  if (stat(path, &st) != 0) return -1;
  return st.st_size;
}
static int port_read(const char *path, uint8_t *buf, size_t cap, size_t *out_len) {
  FILE *f = fopen(path, "rb");
  if (!f) return -1;
  *out_len = fread(buf, 1, cap, f);
  fclose(f);
  return 0;
}
static int port_write(const char *path, const uint8_t *data, size_t len) {
  g_write_calls++;
  FILE *f = fopen(path, "wb");
  if (!f) return -1;
  fwrite(data, 1, len, f);
  fclose(f);
  return 0;
}
static int port_exists(const char *path) {
  struct stat st;
  return stat(path, &st) == 0 ? 1 : 0;
}
static int port_make_dirs(const char *chain) {
  char cmd[1024];
  snprintf(cmd, sizeof(cmd), "mkdir -p '%s'", chain);
  return system(cmd) == 0 ? 0 : -1;
}

int main(void) {
  system("rm -rf build/fixtures/port && mkdir -p build/fixtures/port");
  const char *repo = "build/fixtures/port";

  // size member left NULL: library must still function via fallbacks where
  // possible; write/exists/make_dirs route through the port.
  const esp32git_fs_port port = {NULL, port_read, port_write, port_exists,
                                 port_make_dirs};
  esp32git_fs_register(&port);

  const char *payload = "port seam check\n";
  char sha[41];
  const esp32git_status st = esp32git_object_write(repo, "blob", payload,
                                                   strlen(payload), sha);
  const int writes_after_object = g_write_calls;

  esp32git_fs_register(NULL); // restore stdio backend

  int failures = 0;
  if (st != ESP32GIT_OK) {
    fprintf(stderr, "FAIL: object_write via port\n");
    failures++;
  }
  if (writes_after_object == 0) {
    fprintf(stderr, "FAIL: port write was never called\n");
    failures++;
  }
  // The object must exist on disk (port actually wrote it) and read back.
  char path[256];
  if (!esp32git_object_path(repo, sha, path, sizeof(path))) {
    fprintf(stderr, "FAIL: port-written object missing on disk\n");
    failures++;
  } else {
    char type[16];
    uint8_t buf[64];
    size_t len = 0;
    if (esp32git_object_read(repo, sha, type, sizeof(type), buf, sizeof(buf), &len) !=
            ESP32GIT_OK ||
        len != strlen(payload) || memcmp(buf, payload, len) != 0) {
      fprintf(stderr, "FAIL: port-written content mismatch\n");
      failures++;
    }
  }

  printf(failures ? "%d failure(s)\n" : "fs-port seam verified\n", failures);
  return failures ? 1 : 0;
}
