#include "pack.h"

#include <functional>
#include <memory>
#include <map>
#include <new>
#include <utility>

#include "esp32_git.h"
#include "hexutil.h"
#include "io.h"
#include "sha1.h"
#include "uzlib.h"
#include "zlib.h"

namespace e32g {

namespace {

constexpr size_t kMaxObjectBytes = 64 * 1024;
constexpr size_t kObjectReadBufferBytes = kMaxObjectBytes + 64;

// Inflates one zlib-framed pack entry payload.
// Returns false on error; consumed = bytes of input used.
bool inflate_entry(const uint8_t *in, size_t in_len, uint64_t expect,
                   std::vector<uint8_t> &out, size_t &consumed) {
  if (expect > kMaxObjectBytes) return false;
  auto d = std::unique_ptr<uzlib_uncomp>(new (std::nothrow) uzlib_uncomp{});
  if (!d) return false;
  uzlib_uncompress_init(d.get(), nullptr, 0);
  d->source = in;
  d->source_limit = in + in_len;
  // Over-allocate so the stream runs to TINF_DONE (final block + Adler-32
  // trailer consumed); sizing dest to exactly expect would stop at TINF_OK
  // with the trailer unread and the next entry's offset unknown.
  out.resize((size_t)expect + 8);
  d->dest_start = out.data();
  d->dest = out.data();
  d->dest_limit = out.data() + out.size();
  int rc = zlib_parse_header(d.get());
  if (rc != TINF_OK) return false;
  rc = uzlib_uncompress_chksum(d.get());
  if (rc != TINF_DONE ||
      (size_t)(d->dest - d->dest_start) != (size_t)expect) {
    return false;
  }
  out.resize((size_t)expect);
  consumed = (size_t)(d->source - in);
  return true;
}

bool read_delta_size(const uint8_t *data, size_t len, size_t &at,
                     uint64_t &size) {
  size = 0;
  int shift = 0;
  while (at < len) {
    const uint8_t c = data[at++];
    const uint64_t part = c & 0x7f;
    if (shift >= 64 || part > (UINT64_MAX >> shift)) return false;
    size |= part << shift;
    if (!(c & 0x80)) return true;
    shift += 7;
  }
  return false;
}

bool read_ofs(const uint8_t *data, size_t len, size_t &at,
              uint64_t &distance) {
  if (at >= len) return false;
  uint8_t c = data[at++];
  uint64_t off = c & 0x7f;
  while (c & 0x80) {
    if (at >= len) return false;
    c = data[at++];
    const uint64_t part = c & 0x7f;
    if (off > (UINT64_MAX - part - 128) / 128) return false;
    off = ((off + 1) << 7) | part;
  }
  distance = off;
  return true;
}

void entry_sha(const char *type, const uint8_t *payload, size_t len, char out[41]) {
  char header[32];
  const int hl = snprintf(header, sizeof(header), "%s %zu", type, len);
  esp32git_sha1 ctx;
  esp32git_sha1_init(&ctx);
  esp32git_sha1_update(&ctx, header, (size_t)hl + 1);
  const uint8_t *payload_bytes = payload ? payload
                                         : reinterpret_cast<const uint8_t *>("");
  esp32git_sha1_update(&ctx, payload_bytes, len);
  uint8_t digest[20];
  esp32git_sha1_final(&ctx, digest);
  esp32git_bytes_to_hex(digest, out);
}

} // namespace

bool delta_apply(const uint8_t *base, size_t base_len, const uint8_t *delta,
                 size_t delta_len, std::vector<uint8_t> &out) {
  size_t at = 0;
  uint64_t src_size = 0, dst_size = 0;
  if (!read_delta_size(delta, delta_len, at, src_size) ||
      !read_delta_size(delta, delta_len, at, dst_size) ||
      src_size != base_len || dst_size > kMaxObjectBytes) {
    return false;
  }

  out.clear();
  out.reserve((size_t)dst_size);
  while (at < delta_len) {
    const uint8_t op = delta[at++];
    if (op & 0x80) { // copy from base
      uint64_t offset = 0, size = 0;
      if ((op & 0x01) && at >= delta_len) return false;
      if ((op & 0x02) && at >= delta_len) return false;
      if ((op & 0x04) && at >= delta_len) return false;
      if ((op & 0x08) && at >= delta_len) return false;
      if ((op & 0x10) && at >= delta_len) return false;
      if ((op & 0x20) && at >= delta_len) return false;
      if ((op & 0x40) && at >= delta_len) return false;
      if (op & 0x01) offset |= (uint64_t)delta[at++];
      if (op & 0x02) offset |= (uint64_t)delta[at++] << 8;
      if (op & 0x04) offset |= (uint64_t)delta[at++] << 16;
      if (op & 0x08) offset |= (uint64_t)delta[at++] << 24;
      if (op & 0x10) size |= (uint64_t)delta[at++];
      if (op & 0x20) size |= (uint64_t)delta[at++] << 8;
      if (op & 0x40) size |= (uint64_t)delta[at++] << 16;
      if (size == 0) size = 0x10000;
      if (offset > base_len || size > base_len - offset ||
          out.size() > dst_size || size > dst_size - out.size()) {
        return false;
      }
      out.insert(out.end(), base + offset, base + offset + size);
    } else if (op) { // insert literal run
      if (op > delta_len - at || out.size() > dst_size ||
          op > dst_size - out.size()) {
        return false;
      }
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
  if ((uint64_t)count > (len - 32) / 2) return false;

  struct Raw {
    size_t offset = 0;
    int type = 0;
    uint64_t expect = 0;
    size_t payload_at = 0;
    std::vector<uint8_t> payload; // inflated (delta payload for deltas)
    char base_sha[41] = {};       // REF_DELTA base id
    size_t base_offset = 0;       // OFS_DELTA base position
  };
  std::vector<Raw> raws(count);
  std::map<size_t, size_t> by_offset;

  size_t at = 12;
  for (uint32_t i = 0; i < count; i++) {
    if (at >= len - 20) return false;
    Raw &r = raws[i];
    r.offset = at;
    uint8_t c = data[at++];
    r.type = (c >> 4) & 7;
    r.expect = c & 0x0f;
    int shift = 4;
    while (c & 0x80) {
      if (at >= len - 20 || shift >= 64) return false;
      c = data[at++];
      if ((uint64_t)(c & 0x7f) > (UINT64_MAX >> shift)) return false;
      r.expect |= (uint64_t)(c & 0x7f) << shift;
      shift += 7;
    }
    if (r.type == PACK_OFS_DELTA) {
      uint64_t distance = 0;
      if (!read_ofs(data, len - 20, at, distance) || distance > r.offset) return false;
      r.base_offset = r.offset - (size_t)distance;
    } else if (r.type == PACK_REF_DELTA) {
      if (len - at < 20) return false;
      esp32git_bytes_to_hex(data + at, r.base_sha);
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
        if (esp32git_object_read(scratch_repo.c_str(), r.base_sha, type,
                                 sizeof(type), loose.data(), loose.size(),
                                 &loose_len) != ESP32GIT_OK) {
          return false; // thin pack against objects we do not have
        }
        base.assign(loose.data(), loose.data() + loose_len);
        ref_base_type = type; // resolved object keeps the base's type
      }
      size_t dat = 0;
      uint64_t src_size = 0;
      if (!read_delta_size(r.payload.data(), r.payload.size(), dat, src_size) ||
          src_size != base.size()) {
        return false;
      }
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
    if (!type_name) return false;
    entry_sha(type_name, e.data.data(), e.data.size(), sha);
    e.sha = sha;
    on_entry(e);
  }
  return true;
}

namespace {

constexpr size_t kPackSourceBufferBytes = 1024;
constexpr size_t kPackIndexRecordBytes = 49; // uint64 offset + type + SHA-1 hex

bool read_exact(File &file, uint8_t *buffer, size_t length) {
  size_t at = 0;
  while (at < length) {
    size_t got = 0;
    if (!file.read(buffer + at, length - at, &got) || got == 0 ||
        got > length - at) {
      return false;
    }
    at += got;
  }
  return true;
}

bool read_file_byte(File &file, uint64_t &at, uint64_t end, uint8_t &out) {
  if (at >= end) return false;
  size_t got = 0;
  if (!file.read(&out, 1, &got) || got != 1) return false;
  ++at;
  return true;
}

class PackIndex {
 public:
  explicit PackIndex(std::string path) : path_(std::move(path)) {}
  ~PackIndex() {
    file_.close();
    remove_file(path_);
  }

  bool open() {
    if (exists(path_)) remove_file(path_);
    return file_.open(path_, true);
  }

  bool append(uint64_t offset, int type, const char sha[41]) {
    uint8_t record[kPackIndexRecordBytes];
    memcpy(record, &offset, sizeof(offset));
    record[8] = (uint8_t)type;
    memcpy(record + 9, sha, 40);
    if (!file_.seek(write_offset_) || !file_.write(record, sizeof(record))) {
      return false;
    }
    write_offset_ += sizeof(record);
    return true;
  }

  bool find(uint64_t offset, int &type, char sha[41]) {
    uint8_t record[kPackIndexRecordBytes];
    // ponytail: linear SD scan keeps RAM flat; replace with a persistent
    // indexed lookup only if large delta packs make this measurable.
    for (uint64_t at = 0; at < write_offset_; at += sizeof(record)) {
      if (!file_.seek(at) || !read_exact(file_, record, sizeof(record))) {
        return false;
      }
      uint64_t stored_offset = 0;
      memcpy(&stored_offset, record, sizeof(stored_offset));
      if (stored_offset != offset) continue;
      type = record[8];
      memcpy(sha, record + 9, 40);
      sha[40] = '\0';
      return true;
    }
    return false;
  }

 private:
  std::string path_;
  File file_;
  uint64_t write_offset_ = 0;
};

struct FileInflateSource {
  File *file = nullptr;
  uint8_t *buffer = nullptr;
  size_t buffer_size = 0;
  uint64_t remaining = 0;
  uint64_t loaded = 0;
};

int read_file_source(uzlib_uncomp *uncomp) {
  auto *source = static_cast<FileInflateSource *>(uncomp->source_read_context);
  if (!source || !source->file || !source->buffer || source->buffer_size == 0 ||
      source->remaining == 0) {
    return -1;
  }
  const size_t want = source->remaining < source->buffer_size
                          ? (size_t)source->remaining
                          : source->buffer_size;
  size_t got = 0;
  if (!source->file->read(source->buffer, want, &got) || got == 0 ||
      got > want) {
    return -1;
  }
  source->loaded += got;
  source->remaining -= got;
  uncomp->source = source->buffer;
  uncomp->source_limit = source->buffer + got;
  return *uncomp->source++;
}

bool inflate_file_entry(File &file, uint64_t payload_at, uint64_t available,
                        uint64_t expect, uint8_t *source_buffer,
                        std::vector<uint8_t> &out, uint64_t &consumed) {
  if (expect > kMaxObjectBytes || available == 0 ||
      !file.seek(payload_at)) {
    return false;
  }

  FileInflateSource source = {&file, source_buffer, kPackSourceBufferBytes,
                              available, 0};
  auto d = std::unique_ptr<uzlib_uncomp>(new (std::nothrow) uzlib_uncomp{});
  if (!d) return false;
  uzlib_uncompress_init(d.get(), nullptr, 0);
  d->source = source_buffer;
  d->source_limit = source_buffer;
  d->source_read_cb = read_file_source;
  d->source_read_context = &source;
  out.resize((size_t)expect + 8);
  d->dest_start = out.data();
  d->dest = out.data();
  d->dest_limit = out.data() + out.size();
  if (zlib_parse_header(d.get()) != TINF_OK ||
      uzlib_uncompress_chksum(d.get()) != TINF_DONE ||
      (size_t)(d->dest - d->dest_start) != (size_t)expect) {
    return false;
  }

  const size_t unread = (size_t)(d->source_limit - d->source);
  if (unread > source.loaded) return false;
  consumed = source.loaded - unread;
  if (consumed == 0 || consumed > available ||
      !file.seek(payload_at + consumed)) {
    return false;
  }
  out.resize((size_t)expect);
  return true;
}

const char *pack_type_name(const int type) {
  switch (type) {
    case PACK_COMMIT: return "commit";
    case PACK_TREE: return "tree";
    case PACK_BLOB: return "blob";
    case PACK_TAG: return "tag";
    default: return nullptr;
  }
}

int pack_type_from_name(const char *name) {
  if (strcmp(name, "commit") == 0) return PACK_COMMIT;
  if (strcmp(name, "tree") == 0) return PACK_TREE;
  if (strcmp(name, "blob") == 0) return PACK_BLOB;
  if (strcmp(name, "tag") == 0) return PACK_TAG;
  return 0;
}

} // namespace

bool pack_read_file(const std::string &path, uint64_t offset, uint64_t len,
                    const std::string &scratch_repo,
                    const std::function<void(const PackEntry &)> &on_entry) {
  const int64_t file_size = e32g::file_size(path);
  if (file_size < 0 || offset > (uint64_t)file_size ||
      len > (uint64_t)file_size - offset || len < 32) {
    return false;
  }

  File pack;
  if (!pack.open(path, false) || !pack.seek(offset)) return false;

  uint8_t header[12];
  if (!read_exact(pack, header, sizeof(header)) ||
      memcmp(header, "PACK", 4) != 0) {
    return false;
  }
  const uint32_t version = ((uint32_t)header[4] << 24) |
                           ((uint32_t)header[5] << 16) |
                           ((uint32_t)header[6] << 8) | header[7];
  if (version != 2 && version != 3) return false;
  const uint32_t count = ((uint32_t)header[8] << 24) |
                         ((uint32_t)header[9] << 16) |
                         ((uint32_t)header[10] << 8) | header[11];
  if ((uint64_t)count > (len - 32) / 2) return false;

  PackIndex index(path + ".idx");
  if (!index.open()) return false;
  std::unique_ptr<uint8_t[]> source_buffer(
      new (std::nothrow) uint8_t[kPackSourceBufferBytes]);
  if (!source_buffer) return false;

  const uint64_t data_end = len - 20;
  uint64_t at = 12;
  std::vector<uint8_t> inflated;
  std::vector<uint8_t> base;
  std::vector<uint8_t> resolved;
  for (uint32_t i = 0; i < count; ++i) {
    if (at >= data_end) return false;
    const uint64_t entry_offset = at;
    uint8_t c = 0;
    if (!read_file_byte(pack, at, data_end, c)) return false;
    const int packed_type = (c >> 4) & 7;
    uint64_t expect = c & 0x0f;
    int shift = 4;
    while (c & 0x80) {
      if (!read_file_byte(pack, at, data_end, c) || shift >= 64 ||
          (uint64_t)(c & 0x7f) > (UINT64_MAX >> shift)) {
        return false;
      }
      expect |= (uint64_t)(c & 0x7f) << shift;
      shift += 7;
    }

    uint64_t base_offset = 0;
    char base_sha[41] = {};
    if (packed_type == PACK_OFS_DELTA) {
      uint64_t distance = 0;
      // OFS_DELTA offsets are relative to the pack start, not the file.
      if (!read_file_byte(pack, at, data_end, c)) return false;
      distance = c & 0x7f;
      while (c & 0x80) {
        if (!read_file_byte(pack, at, data_end, c) ||
            distance > (UINT64_MAX - (c & 0x7f) - 128) / 128) {
          return false;
        }
        distance = ((distance + 1) << 7) | (c & 0x7f);
      }
      if (distance > entry_offset) return false;
      base_offset = entry_offset - distance;
    } else if (packed_type == PACK_REF_DELTA) {
      uint8_t raw_sha[20];
      if (at + sizeof(raw_sha) > data_end ||
          !read_exact(pack, raw_sha, sizeof(raw_sha))) {
        return false;
      }
      esp32git_bytes_to_hex(raw_sha, base_sha);
      at += sizeof(raw_sha);
    }

    const uint64_t payload_at = at;
    if (!inflate_file_entry(pack, offset + payload_at, data_end - payload_at,
                            expect, source_buffer.get(), inflated, at)) {
      return false;
    }
    at = payload_at + at;

    PackEntry entry;
    if (packed_type == PACK_COMMIT || packed_type == PACK_TREE ||
        packed_type == PACK_BLOB || packed_type == PACK_TAG) {
      entry.type = packed_type;
      entry.data.swap(inflated);
    } else if (packed_type == PACK_OFS_DELTA ||
               packed_type == PACK_REF_DELTA) {
      int base_type = 0;
      if (packed_type == PACK_OFS_DELTA) {
        if (!index.find(base_offset, base_type, base_sha)) return false;
      }
      char base_type_name[16] = {};
      if (packed_type == PACK_REF_DELTA) {
        base.resize(kObjectReadBufferBytes);
        size_t base_len = 0;
        if (esp32git_object_read(scratch_repo.c_str(), base_sha,
                                 base_type_name, sizeof(base_type_name),
                                 base.data(), base.size(), &base_len) !=
                ESP32GIT_OK) {
          return false;
        }
        base.resize(base_len);
        base_type = pack_type_from_name(base_type_name);
      } else {
        const char *name = pack_type_name(base_type);
        if (!name) return false;
        snprintf(base_type_name, sizeof(base_type_name), "%s", name);
        base.resize(kObjectReadBufferBytes);
        size_t base_len = 0;
        if (esp32git_object_read(scratch_repo.c_str(), base_sha,
                                 base_type_name, sizeof(base_type_name),
                                 base.data(), base.size(), &base_len) !=
                ESP32GIT_OK) {
          return false;
        }
        base.resize(base_len);
      }
      if (base_type == 0) base_type = pack_type_from_name(base_type_name);
      if (base_type == 0) return false;

      size_t delta_at = 0;
      uint64_t source_size = 0;
      uint64_t destination_size = 0;
      if (!read_delta_size(inflated.data(), inflated.size(), delta_at,
                           source_size) ||
          source_size != base.size() ||
          !read_delta_size(inflated.data(), inflated.size(), delta_at,
                           destination_size) ||
          destination_size > kMaxObjectBytes) {
        return false;
      }
      if (!delta_apply(base.data(), base.size(),
                       inflated.data() + delta_at,
                       inflated.size() - delta_at, resolved)) {
        return false;
      }
      entry.type = base_type;
      entry.data.swap(resolved);
      // Keep object_write's raw and compressed allocations from overlapping
      // the input/base working buffers on the constrained target.
      std::vector<uint8_t>().swap(inflated);
      std::vector<uint8_t>().swap(base);
    } else {
      return false;
    }

    const char *type_name = pack_type_name(entry.type);
    if (!type_name) return false;
    char sha[41];
    entry_sha(type_name, entry.data.data(), entry.data.size(), sha);
    entry.sha = sha;
    on_entry(entry);
    if (packed_type == PACK_OFS_DELTA || packed_type == PACK_REF_DELTA) {
      resolved.swap(entry.data);
    } else {
      inflated.swap(entry.data);
    }
    if (!index.append(entry_offset, entry.type, sha)) return false;
  }
  return at == data_end;
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
