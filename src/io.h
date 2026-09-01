#pragma once

// Internal file-I/O seam: every storage access in the library goes through
// these helpers. They dispatch to a registered firmware port (HalStorage on
// device) or fall back to host stdio.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "esp32_git.h"

namespace e32g {

class File {
 public:
  File() = default;
  ~File();
  File(const File &) = delete;
  File &operator=(const File &) = delete;
  File(File &&other) noexcept;
  File &operator=(File &&other) noexcept;

  bool open(const std::string &path, bool write);
  bool read(uint8_t *buf, size_t cap, size_t *out_len);
  bool write(const uint8_t *data, size_t len);
  bool seek(uint64_t offset);
  bool close();
  bool is_open() const { return handle_ != nullptr; }

 private:
  const esp32git_file_port *port_ = nullptr;
  void *handle_ = nullptr;
};

// Byte size of a regular file; -1 when missing or unreadable.
int64_t file_size(const std::string &path);

// Reads a whole file into out (resized); false when missing/truncated.
bool read_whole(const std::string &path, std::vector<uint8_t> &out);

// Overwrite-write len bytes; false on failure.
bool write_whole(const std::string &path, const uint8_t *data, size_t len);

// True when path is an existing regular file.
bool exists(const std::string &path);

// mkdir -p over the whole chain.
bool make_dirs(const std::string &dir_chain);

// True when the registered backend supports the handle operations required by
// streamed pack ingestion.
bool has_file_io();

// Removes a temporary or regular file. Missing files are treated as success.
bool remove_file(const std::string &path);

} // namespace e32g
