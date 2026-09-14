#pragma once
// Internal little-endian readers shared by the rl::wad parsers.
// Every reader takes an explicit offset and the caller is responsible for
// checking inRange() first; the readers themselves never touch memory outside
// the span because the callers only pass offsets that passed inRange().

#include <cstddef>
#include <cstdint>
#include <span>

namespace rl::wad::detail {

// True if [off, off+n) lies inside the buffer (overflow-safe).
inline bool inRange(std::span<const uint8_t> b, size_t off, size_t n) {
    return off <= b.size() && n <= b.size() - off;
}

inline uint8_t rdU8(std::span<const uint8_t> b, size_t off) { return b[off]; }

inline uint16_t rdU16(std::span<const uint8_t> b, size_t off) {
    return static_cast<uint16_t>(static_cast<uint16_t>(b[off]) | (static_cast<uint16_t>(b[off + 1]) << 8));
}

inline int16_t rdI16(std::span<const uint8_t> b, size_t off) { return static_cast<int16_t>(rdU16(b, off)); }

inline uint32_t rdU32(std::span<const uint8_t> b, size_t off) {
    return static_cast<uint32_t>(b[off]) | (static_cast<uint32_t>(b[off + 1]) << 8) |
           (static_cast<uint32_t>(b[off + 2]) << 16) | (static_cast<uint32_t>(b[off + 3]) << 24);
}

inline int32_t rdI32(std::span<const uint8_t> b, size_t off) { return static_cast<int32_t>(rdU32(b, off)); }

// Largest picture dimension we are willing to decode. Vanilla graphics are
// far smaller; this only bounds allocations when a lump is garbage.
inline constexpr int kMaxImageDim = 8192;
inline constexpr size_t kMaxImagePixels = size_t{16} * 1024 * 1024;

inline bool plausibleImageSize(int w, int h) {
    return w > 0 && h > 0 && w <= kMaxImageDim && h <= kMaxImageDim &&
           static_cast<size_t>(w) * static_cast<size_t>(h) <= kMaxImagePixels;
}

}  // namespace rl::wad::detail
