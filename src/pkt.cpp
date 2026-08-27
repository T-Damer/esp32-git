#include "pkt.h"

#include <cstdio>

namespace e32g {

namespace {
int hex_val(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}
} // namespace

void pkt_write(std::string &out, const std::string &payload) {
  if (payload.empty()) {
    out += "0000";
    return;
  }
  char len[5];
  snprintf(len, sizeof(len), "%04zx", payload.size() + 4);
  out += len;
  out += payload;
}

void pkt_flush(std::string &out) { out += "0000"; }

bool pkt_split(const uint8_t *data, size_t len, std::vector<std::string> &out) {
  out.clear();
  size_t at = 0;
  while (at + 4 <= len) {
    const int hi = hex_val((char)data[at]), lo = hex_val((char)data[at + 1]);
    const int h2 = hex_val((char)data[at + 2]), l2 = hex_val((char)data[at + 3]);
    if (hi < 0 || lo < 0 || h2 < 0 || l2 < 0) return false;
    const int pkt_len = (hi << 12) | (lo << 8) | (h2 << 4) | l2;
    if (pkt_len == 0) { // flush-pkt: boundary marker, keep scanning
      at += 4;
      continue;
    }
    if (pkt_len < 4 || at + (size_t)pkt_len > len) return false;
    out.emplace_back((const char *)data + at + 4, (size_t)pkt_len - 4);
    // pkt-line payloads end with LF; drop it so ref names compare cleanly.
    std::string &line = out.back();
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
    at += (size_t)pkt_len;
  }
  return true;
}

} // namespace e32g
