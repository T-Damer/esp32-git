#pragma once

// pkt-line framing (protocol-common.txt). Max payload 65516 bytes.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace e32g {

// Appends the pkt-line encoding of payload to out ("0000" flush = empty).
void pkt_write(std::string &out, const std::string &payload);
void pkt_flush(std::string &out);

// Splits a pkt-line stream into payloads; delim-only lines are skipped.
// Returns false on malformed framing.
bool pkt_split(const uint8_t *data, size_t len, std::vector<std::string> &out);

} // namespace e32g
