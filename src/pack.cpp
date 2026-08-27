#include "pack.h"

#include <functional>
#include <map>

#include "esp32_git.h"
#include "hexutil.h"
#include "sha1.h"
#include "uzlib.h"
#include "zlib.h"

namespace e32g {

namespace {

constexpr size_t kMaxObjectBytes = 64 * 1024;

// Inflates one zlib-framed pack entry payload.
// Returns false on error; consumed = bytes of input used.
bool inflate_entry(const uint8_t *in, size_t in_len, uint64_t expect,
                   std::vector<uint8_t> &out, size_t &consumed) {
  struct uzlib_uncomp d;
  uzlib_uncompress_init(&d, nullptr, 0);
  d.source = in;
  d.source_limit = in + in_len;
  // Over-allocate so the stream runs to TINF_DONE (final block + Adler-32
  // trailer consumed); sizing dest to exactly expect would stop at TINF_OK
  // with the trailer unread and the next entry's offset unknown.
  out.resize((size_t)expect + 8);
  d.dest_start = out.data();
  d.dest = out.data();
  d.dest_limit = out.data() + out.size();
  int rc = zlib_parse_header(&d);
  if (rc != TINF_OK) return false;
  rc = uzlib_uncompress_chksum(&d);
  if (rc != TINF_DONE) return false;
  out.resize((size_t)expect);
  consumed = (size_t)(d.source - in);
  return true;
}

uint64_t read_delta_size(const uint8_t *data, size_t len, size_t &at) {
  uint64_t size = 0;
  int shift = 0;
  while (at < len) {
    const uint8_t c = data[at++];
    size |= (uint64_t)(c & 0x7f) << shift;
    shift += 7;
    if (!(c & 0x80)) break;
  }
  return size;
}

uint64_t read_ofs(const uint8_t *data, size_t len, size_t &at) {
  uint64_t off = 0;
  uint8_t c = data[at++];
  while (c & 0x80) {
    off = ((off + 1) << 7) | (c & 0x7f);
    c = data[at++];
  }
  return off + c;
}

void entry_sha(const char *type, const uint8_t *payload, size_t len, char out[41]) {
  char header[32];
  const int hl = snprintf(header, sizeof(header), "%s %zu", type, len);
  esp32git_sha1 ctx;
  esp32git_sha1_init(&ctx);
  esp32git_sha1_update(&ctx, header, (size_t)hl + 1);
  esp32git_sha1_update(&ctx, payload, len);
  uint8_t digest[20];
  esp32git_sha1_final(&ctx, digest);
  esp32git_bytes_to_hex(digest, out);
}

} // namespace

bool delta_apply(const uint8_t *base, size_t base_len, const uint8_t *delta,
                 size_t delta_len, std::vector<uint8_t> &out) {
  size_t at = 0;
  uint64_t src_size = 0, dst_size = 0;
  int shift = 0;
  while (at < delta_len) {
    const uint8_t c = delta[at++];
    src_size |= (uint64_t)(c & 0x7f) << shift;
    shift += 7;
    if (!(c & 0x80)) break;
  }
  if (src_size != base_len) return false;
  shift = 0;
  while (at < delta_len) {
    const uint8_t c = delta[at++];
    dst_size |= (uint64_t)(c & 0x7f) << shift;
    shift += 7;
    if (!(c & 0x80)) break;
  }

  out.clear();
  out.reserve((size_t)dst_size);
  while (at < delta_len) {
    const uint8_t op = delta[at++];
    if (op & 0x80) { // copy from base
      uint64_t offset = 0, size = 0;
      if (op & 0x01) offset |= (uint64_t)delta[at++];
      if (op & 0x02) offset |= (uint64_t)delta[at++] << 8;
      if (op & 0x04) offset |= (uint64_t)delta[at++] << 16;
      if (op & 0x08) offset |= (uint64_t)delta[at++] << 24;
      if (op & 0x10) size |= (uint64_t)delta[at++];
      if (op & 0x20) size |= (uint64_t)delta[at++] << 8;
      if (op & 0x40) size |= (uint64_t)delta[at++] << 16;
      if (size == 0) size = 0x10000;
      if (offset > base_len || size > base_len - offset) return false;
      out.insert(out.end(), base + offset, base + offset + size);
    } else if (op) { // insert literal run
      if (op > delta_len - at) return false;
      out.insert(out.end(), delta + at, delta + at + op);
      at += op;
    } else {
      return false; // reserved opcode 0
    }
  }
  return out.size() == dst_size;
}

bool pack_read(const uint8_t *data, size_t len, const std::string &scratch_repo,
               const std::function<void(const PackEntry &)> &on_entry) {
  if (len < 32 || memcmp(data, "PACK", 4) != 0) return false;
  const uint32_t version =
      ((uint32_t)data[4] << 24) | (data[5] << 16) | (data[6] << 8) | data[7];
  if (version != 2 && version != 3) return false;
  const uint32_t count =
      ((uint32_t)data[8] << 24) | (data[9] << 16) | (data[10] << 8) | data[11];

  struct Raw {
    size_t offset = 0;
    int type = 0;
    uint64_t expect = 0;
    size_t payload_at = 0;
    std::vector<uint8_t> payload; // inflated (delta payload for deltas)
    std::string base_sha;         // REF_DELTA base id
    size_t base_offset = 0;       // OFS_DELTA base position
  };
  std::vector<Raw> raws(count);
  std::map<size_t, size_t> by_offset;

  size_t at = 12;
  for (uint32_t i = 0; i < count && at < len - 20; i++) {
    Raw &r = raws[i];
    r.offset = at;
    uint8_t c = data[at++];
    r.type = (c >> 4) & 7;
    r.expect = c & 0x0f;
    int shift = 4;
    while (c & 0x80) {
      c = data[at++];
      r.expect |= (uint64_t)(c & 0x7f) << shift;
      shift += 7;
    }
    if (r.type == PACK_OFS_DELTA) {
      r.base_offset = r.offset - read_ofs(data, len, at);
    } else if (r.type == PACK_REF_DELTA) {
      esp32git_bytes_to_hex(data + at, r.base_sha.data());
      at += 20;
    }
    r.payload_at = at;
    size_t consumed = 0;
    if (!inflate_entry(data + at, len - at, r.expect, r.payload, consumed)) {
      return false;
    }
    at += consumed;
    by_offset[r.offset] = i;
  }

  // Resolve in order: valid packs place bases before their deltas.
  for (size_t i = 0; i < raws.size(); i++) {
    const Raw &r = raws[i];
    PackEntry e;
    e.data.clear();
    const char *type_name = nullptr;
    switch (r.type) {
      case PACK_COMMIT: type_name = "commit"; break;
      case PACK_TREE: type_name = "tree"; break;
      case PACK_BLOB: type_name = "blob"; break;
      case PACK_TAG: type_name = "tag"; break;
      case PACK_OFS_DELTA:
      case PACK_REF_DELTA: type_name = "delta"; break;
    }

    if (r.type != PACK_OFS_DELTA && r.type != PACK_REF_DELTA) {
      e.type = r.type;
      e.data = r.payload;
    } else {
      std::vector<uint8_t> base;
      const char *ref_base_type = "blob";
      if (r.type == PACK_OFS_DELTA) {
        auto it = by_offset.find(r.base_offset);
        if (it == by_offset.end()) return false;
        base = raws[it->second].payload; // deltas-of-deltas: raws payload is
                                         // resolved lazily below only for
                                         // non-delta; chains unsupported here
        // ponytail: delta-on-delta chains resolve via recursion below
        if (raws[it->second].type == PACK_OFS_DELTA ||
            raws[it->second].type == PACK_REF_DELTA) {
          return false; // unsupported nested delta
        }
      } else {
        char type[16];
        std::vector<uint8_t> loose(kMaxObjectBytes);
        size_t loose_len = 0;
        if (esp32git_object_read(scratch_repo.c_str(), r.base_sha.c_str(), type,
                                 sizeof(type), loose.data(), loose.size(),
                                 &loose_len) != ESP32GIT_OK) {
          return false; // thin pack against objects we do not have
        }
        base.assign(loose.data(), loose.data() + loose_len);
        ref_base_type = type; // resolved object keeps the base's type
      }
      size_t dat = 0;
      const uint64_t src_size = read_delta_size(r.payload.data(), r.payload.size(), dat);
      if (src_size != base.size()) return false;
      if (!delta_apply(base.data(), base.size(), r.payload.data() + dat,
                       r.payload.size() - dat, e.data)) {
        return false;
      }
      // The resolved object keeps its base's type.
      if (r.type == PACK_OFS_DELTA) {
        auto it = by_offset.find(r.base_offset);
        const int base_type = raws[it->second].type;
        type_name = base_type == PACK_COMMIT    ? "commit"
                    : base_type == PACK_TREE    ? "tree"
                    : base_type == PACK_BLOB    ? "blob"
                                                : "tag";
      } else {
        type_name = ref_base_type;
      }
    }

    e.sha = "";
    char sha[41];
    entry_sha(type_name, e.data.data(), e.data.size(), sha);
    e.sha = sha;
    on_entry(e);
  }
  return true;
}

std::vector<uint8_t> pack_write(const std::vector<PackEntry> &entries) {
  std::vector<uint8_t> out;
  const uint8_t hdr[12] = {'P', 'A', 'C', 'K', 0, 0, 0, 2,
                           (uint8_t)(entries.size() >> 24),
                           (uint8_t)(entries.size() >> 16),
                           (uint8_t)(entries.size() >> 8),
                           (uint8_t)entries.size()};
  out.insert(out.end(), hdr, hdr + 12);
  for (const auto &e : entries) {
    uint8_t head[16];
    size_t hn = 0;
    uint8_t c = (uint8_t)((e.type << 4) | (e.data.size() & 0x0f));
    uint64_t rem = e.data.size() >> 4;
    if (rem) c |= 0x80;
    head[hn++] = c;
    while (rem) {
      c = (uint8_t)(rem & 0x7f);
      rem >>= 7;
      if (rem) c |= 0x80;
      head[hn++] = c;
    }
    out.insert(out.end(), head, head + hn);
    std::vector<uint8_t> comp(esp32git_zlib_stored_bound(e.data.size()));
    const int clen = esp32git_zlib_deflate_stored(e.data.data(), e.data.size(),
                                                  comp.data(), comp.size());
    out.insert(out.end(), comp.begin(), comp.begin() + clen);
  }
  esp32git_sha1 ctx;
  esp32git_sha1_init(&ctx);
  esp32git_sha1_update(&ctx, out.data(), out.size());
  uint8_t digest[20];
  esp32git_sha1_final(&ctx, digest);
  out.insert(out.end(), digest, digest + 20);
  return out;
}

} // namespace e32g
