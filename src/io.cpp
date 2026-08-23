#include "io.h"

#include <sys/stat.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "esp32_git.h"

namespace e32g {

namespace {

const esp32git_fs_port *active_port = nullptr; // NULL -> stdio backend

int64_t stdio_size(const char *path) {
  struct stat st;
  if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) return -1;
  return (int64_t)st.st_size;
}

int stdio_read(const char *path, uint8_t *buf, size_t cap, size_t *out_len) {
  FILE *f = fopen(path, "rb");
  if (!f) return -1;
  const size_t got = fread(buf, 1, cap, f);
  const bool at_eof = fgetc(f) == EOF;
  fclose(f);
  if (!at_eof) return -1;
  *out_len = got;
  return 0;
}

int stdio_write(const char *path, const uint8_t *data, size_t len) {
  FILE *f = fopen(path, "wb");
  if (!f) return -1;
  const size_t wrote = fwrite(data, 1, len, f);
  fclose(f);
  return wrote == len ? 0 : -1;
}

int stdio_exists(const char *path) {
  struct stat st;
  return (stat(path, &st) == 0 && S_ISREG(st.st_mode)) ? 1 : 0;
}

int stdio_make_dirs(const char *dir_chain) {
  const size_t n = strlen(dir_chain);
  for (size_t i = 1; i <= n; i++) {
    if (i == n || dir_chain[i] == '/') mkdir(std::string(dir_chain, i).c_str(), 0777);
  }
  struct stat st;
  return (stat(dir_chain, &st) == 0 && S_ISDIR(st.st_mode)) ? 0 : -1;
}

const esp32git_fs_port kStdioPort = {stdio_size,   stdio_read, stdio_write,
                                     stdio_exists, stdio_make_dirs};

const esp32git_fs_port &p() {
  return active_port ? *active_port : kStdioPort;
}

} // namespace

void fs_register(const ::esp32git_fs_port *new_port) { active_port = new_port; }

int64_t file_size(const std::string &path) {
  return p().size ? p().size(path.c_str()) : -1;
}

bool read_whole(const std::string &path, std::vector<uint8_t> &out) {
  const int64_t sz = file_size(path);
  if (sz < 0) return false;
  out.resize((size_t)sz);
  if (sz == 0) return true;
  size_t got = 0;
  if (!p().read || p().read(path.c_str(), out.data(), out.size(), &got) != 0 ||
      got != out.size()) {
    return false;
  }
  return true;
}

bool write_whole(const std::string &path, const uint8_t *data, size_t len) {
  return !p().write || p().write(path.c_str(), data, len) == 0;
}

bool exists(const std::string &path) {
  return p().exists ? p().exists(path.c_str()) == 1 : false;
}

bool make_dirs(const std::string &dir_chain) {
  return p().make_dirs ? p().make_dirs(dir_chain.c_str()) == 0 : false;
}

} // namespace e32g

void esp32git_fs_register(const esp32git_fs_port *new_port) {
  e32g::fs_register(new_port);
}
