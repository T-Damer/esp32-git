#pragma once

#include <cstdint>
#include <cstring>

inline void esp32git_hex_to_bytes(const char *hex, uint8_t out[20]) {
  static const char *kHex = "0123456789abcdef";
  auto val = [&](char c) -> int {
    const char *p = strchr(kHex, c >= 'A' && c <= 'F' ? c - 'A' + 'a' : c);
    return p ? int(p - kHex) : 0;
  };
  for (int i = 0; i < 20; i++) {
    out[i] = (uint8_t)((val(hex[i * 2]) << 4) | val(hex[i * 2 + 1]));
  }
}

inline void esp32git_bytes_to_hex(const uint8_t digest[20], char out[41]) {
  static const char kHex[] = "0123456789abcdef";
  for (int i = 0; i < 20; i++) {
    out[i * 2] = kHex[digest[i] >> 4];
    out[i * 2 + 1] = kHex[digest[i] & 0xf];
  }
  out[40] = '\0';
}
