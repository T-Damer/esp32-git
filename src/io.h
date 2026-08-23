#pragma once

// Internal file-I/O seam: every storage access in the library goes through
// these helpers. They dispatch to a registered firmware port (HalStorage on
// device) or fall back to host stdio.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace e32g {

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

} // namespace e32g
