#include "io.h"

#include <sys/stat.h>

#include <cerrno>
#include <climits>
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

void *stdio_file_open(const char *path, int write) {
  return fopen(path, write ? "w+b" : "rb");
}

int stdio_file_read(void *handle, uint8_t *buf, size_t cap, size_t *out_len) {
  if (!handle || !buf || !out_len) return -1;
  FILE *file = static_cast<FILE *>(handle);
  *out_len = fread(buf, 1, cap, file);
  return ferror(file) ? -1 : 0;
}

int stdio_file_write(void *handle, const uint8_t *data, size_t len) {
  if (!handle || (len > 0 && !data)) return -1;
  FILE *file = static_cast<FILE *>(handle);
  return fwrite(data, 1, len, file) == len ? 0 : -1;
}

int stdio_file_seek(void *handle, uint64_t offset) {
  if (!handle || offset > static_cast<uint64_t>(LONG_MAX)) return -1;
  return fseek(static_cast<FILE *>(handle), static_cast<long>(offset), SEEK_SET);
}

int stdio_file_close(void *handle) {
  return handle ? fclose(static_cast<FILE *>(handle)) : 0;
}

int stdio_remove(const char *path) {
  return std::remove(path) == 0 || errno == ENOENT ? 0 : -1;
}

const esp32git_fs_port kStdioPort = {
    stdio_size,
    stdio_read,
    stdio_write,
    stdio_exists,
    stdio_make_dirs,
    {stdio_file_open, stdio_file_read, stdio_file_write, stdio_file_seek,
     stdio_file_close},
    stdio_remove};

const esp32git_fs_port &p() {
  return active_port ? *active_port : kStdioPort;
}

} // namespace

void fs_register(const ::esp32git_fs_port *new_port) { active_port = new_port; }

File::~File() { close(); }

File::File(File &&other) noexcept
    : port_(other.port_), handle_(other.handle_) {
  other.port_ = nullptr;
  other.handle_ = nullptr;
}

File &File::operator=(File &&other) noexcept {
  if (this == &other) return *this;
  close();
  port_ = other.port_;
  handle_ = other.handle_;
  other.port_ = nullptr;
  other.handle_ = nullptr;
  return *this;
}

bool File::open(const std::string &path, bool write) {
  close();
  const esp32git_fs_port &fs = p();
  if (!fs.file.open) return false;
  void *handle = fs.file.open(path.c_str(), write ? 1 : 0);
  if (!handle) return false;
  port_ = &fs.file;
  handle_ = handle;
  return true;
}

bool File::read(uint8_t *buf, size_t cap, size_t *out_len) {
  if (!port_ || !port_->read || !out_len) return false;
  if (cap == 0) {
    *out_len = 0;
    return true;
  }
  if (!buf) return false;
  return port_->read(handle_, buf, cap, out_len) == 0;
}

bool File::write(const uint8_t *data, size_t len) {
  if (!port_ || !port_->write) return false;
  if (len == 0) return true;
  return data && port_->write(handle_, data, len) == 0;
}

bool File::seek(uint64_t offset) {
  return port_ && port_->seek && port_->seek(handle_, offset) == 0;
}

bool File::close() {
  if (!handle_) return true;
  const int result = port_ && port_->close ? port_->close(handle_) : -1;
  port_ = nullptr;
  handle_ = nullptr;
  return result == 0;
}

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

bool has_file_io() {
  const esp32git_fs_port &fs = p();
  return fs.file.open && fs.file.read && fs.file.write && fs.file.seek &&
         fs.file.close && fs.remove;
}

bool remove_file(const std::string &path) {
  const esp32git_fs_port &fs = p();
  if (!fs.remove) return false;
  if (fs.exists && fs.exists(path.c_str()) == 0) return true;
  return fs.remove(path.c_str()) == 0;
}

} // namespace e32g

void esp32git_fs_register(const esp32git_fs_port *new_port) {
  e32g::fs_register(new_port);
}
