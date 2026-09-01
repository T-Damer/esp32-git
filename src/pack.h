#pragma once

// Minimal packfile support: read v2 packs (full objects + OFS_DELTA/REF_DELTA
// with standard git delta application) and write simple non-delta packs.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace e32g {

enum PackType : int {
  PACK_COMMIT = 1,
  PACK_TREE = 2,
  PACK_BLOB = 3,
  PACK_TAG = 4,
  PACK_OFS_DELTA = 6,
  PACK_REF_DELTA = 7,
};

struct PackEntry {
  int type = 0; // PackType
  std::string sha;
  std::vector<uint8_t> data;
};

// Inflates and resolves every entry (applying deltas in dependency order).
// on_entry is called once per resolved object. Returns false on malformed
// input. Entries may arrive in any order; bases always precede deltas in
// valid packs except REF_DELTA against objects the reader supplies via
// lookup (loose objects already fetched).
bool pack_read(const uint8_t *data, size_t len,
               const std::string &scratch_repo, // loose objects for REF_DELTA
               const std::function<void(const PackEntry &)> &on_entry);

// Reads and resolves a pack stored in a random-access file. The pack itself,
// compressed input windows, and the OFS_DELTA lookup index stay off-heap; only
// the current object's bounded working set is resident.
bool pack_read_file(const std::string &path, uint64_t offset, uint64_t len,
                    const std::string &scratch_repo,
                    const std::function<void(const PackEntry &)> &on_entry);

// Writes count objects as a non-delta v2 pack; returns the full pack bytes
// including the trailing SHA-1 over the pack contents.
std::vector<uint8_t> pack_write(const std::vector<PackEntry> &entries);

// Applies a git delta buffer to base; returns false on malformed delta.
bool delta_apply(const uint8_t *base, size_t base_len, const uint8_t *delta,
                 size_t delta_len, std::vector<uint8_t> &out);

} // namespace e32g
