#pragma once
// zlib/deflate decompressor (RFC 1950/1951), after Mark Adler's "puff": small
// and dependency-free, fast enough for sprite-sized PNGs.
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace rl {

// Inflates a zlib stream (2-byte header + deflate + adler32) or, when `raw` is
// set, a bare deflate stream. Appends to `out`. Returns false with `err` set on
// malformed input.
bool inflateZlib(const uint8_t* data, size_t len, std::vector<uint8_t>& out, std::string* err = nullptr, bool raw = false);

}  // namespace rl
