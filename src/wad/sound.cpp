// DMX sound lumps ("DS*").
#include "wad/wad.h"

#include "wad/bytes.h"

#include <algorithm>

namespace rl::wad {

using detail::inRange;
using detail::rdU16;
using detail::rdU32;

// Format:  uint16 format (3 = DMX digital), uint16 sampleRate,
//          uint32 numSamples, uint8 pcm[numSamples] (unsigned, 128 = silence)
// Vanilla DMX lumps surround the real samples with 16 bytes of padding on each
// side (copies of the first / last sample, used by DMX for interpolation) and
// numSamples counts those padding bytes. Lumps written by later tools may be
// unpadded, so the padding is only stripped when its signature is present.

static bool looksPadded(std::span<const uint8_t> pcm) {
    if (pcm.size() <= 32) return false;
    uint8_t first = pcm[16];
    uint8_t last = pcm[pcm.size() - 17];
    for (size_t i = 0; i < 16; ++i) {
        if (pcm[i] != first) return false;
        if (pcm[pcm.size() - 1 - i] != last) return false;
    }
    return true;
}

std::optional<Sound> loadSound(const Wad& wad, std::string_view lumpName) {
    std::span<const uint8_t> d = wad.data(lumpName);
    if (!inRange(d, 0, 8)) return std::nullopt;
    if (rdU16(d, 0) != 3) return std::nullopt;  // 0 = PC speaker (DP*), anything else is not audio
    uint16_t rate = rdU16(d, 2);
    if (rate == 0) return std::nullopt;

    // Clamp the declared count to what the lump physically contains.
    size_t declared = rdU32(d, 4);
    size_t count = std::min(declared, d.size() - 8);
    std::span<const uint8_t> pcm = d.subspan(8, count);
    if (looksPadded(pcm)) pcm = pcm.subspan(16, pcm.size() - 32);

    Sound s;
    s.sampleRate = rate;
    s.samples.reserve(pcm.size());
    for (uint8_t v : pcm) s.samples.push_back((static_cast<float>(v) - 128.0f) / 128.0f);
    return s;
}

}  // namespace rl::wad
