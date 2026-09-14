#include "audio/genmidi.h"

#include <cstring>

namespace rl::audio {

namespace {
GenMidiOperator readOp(const uint8_t* p) {
    return {p[0], p[1], p[2], p[3], p[4], p[5]};
}
}  // namespace

std::optional<GenMidiBank> parseGenMidi(std::span<const uint8_t> d) {
    constexpr size_t kHeader = 8, kInstr = 36, kCount = 175, kName = 32;
    if (d.size() < kHeader + kInstr * kCount) return std::nullopt;
    if (std::memcmp(d.data(), "#OPL_II#", 8) != 0) return std::nullopt;
    GenMidiBank bank;
    bank.instruments.resize(kCount);
    for (size_t i = 0; i < kCount; ++i) {
        const uint8_t* p = d.data() + kHeader + i * kInstr;
        GenMidiInstrument& in = bank.instruments[i];
        in.flags = static_cast<uint16_t>(p[0] | (p[1] << 8));
        in.fineTune = p[2];
        in.fixedNote = p[3];
        for (int v = 0; v < 2; ++v) {
            const uint8_t* q = p + 4 + v * 16;
            GenMidiVoice& voice = in.voice[v];
            voice.modulator = readOp(q);
            voice.feedbackConnection = q[6];
            voice.carrier = readOp(q + 7);
            // q[13] unused, q[14..15] base note offset (int16 LE)
            voice.baseNoteOffset = static_cast<int16_t>(q[14] | (q[15] << 8));
        }
    }
    size_t namesAt = kHeader + kInstr * kCount;
    if (d.size() >= namesAt + kName * kCount) {
        for (size_t i = 0; i < kCount; ++i) {
            const char* n = reinterpret_cast<const char*>(d.data() + namesAt + i * kName);
            size_t len = 0;
            while (len < kName && n[len] != '\0') ++len;
            bank.instruments[i].name.assign(n, len);
        }
    }
    return bank;
}

// ---------------------------------------------------------------------------
GenMidiBank builtinGenMidi() {
    GenMidiBank bank;
    bank.instruments.resize(175);
    // A few hand-made OPL2 patches (register bytes: char, AD, SR, wave, KSL, TL).
    auto voice = [](GenMidiOperator mod, uint8_t fb, GenMidiOperator car, int16_t off) {
        GenMidiVoice v;
        v.modulator = mod;
        v.feedbackConnection = fb;
        v.carrier = car;
        v.baseNoteOffset = off;
        return v;
    };
    const GenMidiVoice piano = voice({0x01, 0xF2, 0x53, 0x00, 0x40, 0x1A}, 0x06, {0x01, 0xF2, 0x74, 0x00, 0x00, 0x00}, 0);
    const GenMidiVoice organ = voice({0x21, 0xF0, 0x0F, 0x00, 0x00, 0x18}, 0x0A, {0x21, 0xF0, 0x0F, 0x00, 0x00, 0x00}, 0);
    const GenMidiVoice bass = voice({0x21, 0xF5, 0x24, 0x00, 0x00, 0x10}, 0x0A, {0x21, 0xF6, 0x34, 0x00, 0x00, 0x00}, 0);
    const GenMidiVoice lead = voice({0x31, 0xF1, 0x28, 0x01, 0x40, 0x24}, 0x0E, {0x21, 0xF0, 0x18, 0x00, 0x00, 0x00}, 0);
    const GenMidiVoice strings = voice({0x61, 0x51, 0x1F, 0x00, 0x00, 0x1C}, 0x0C, {0x61, 0x41, 0x1F, 0x00, 0x00, 0x00}, 0);
    const GenMidiVoice brass = voice({0x21, 0x83, 0x27, 0x00, 0x00, 0x1A}, 0x0C, {0x21, 0x84, 0x36, 0x00, 0x00, 0x00}, 0);
    const GenMidiVoice guitar = voice({0x11, 0xF2, 0x23, 0x00, 0x40, 0x22}, 0x0A, {0x11, 0xF2, 0x43, 0x00, 0x00, 0x00}, 0);
    for (int p = 0; p < 128; ++p) {
        GenMidiInstrument& in = bank.instruments[static_cast<size_t>(p)];
        const GenMidiVoice* v = &piano;
        if (p >= 16 && p < 24) v = &organ;
        else if (p >= 24 && p < 32) v = &guitar;
        else if (p >= 32 && p < 40) v = &bass;
        else if (p >= 40 && p < 56) v = &strings;
        else if (p >= 56 && p < 72) v = &brass;
        else if (p >= 80 && p < 104) v = &lead;
        in.voice[0] = *v;
        in.voice[1] = *v;
        in.name = "builtin";
    }
    // Percussion: fixed-pitch noisy patches.
    const GenMidiVoice kick = voice({0x00, 0xF8, 0x66, 0x00, 0x00, 0x08}, 0x00, {0x00, 0xF6, 0xA6, 0x00, 0x00, 0x00}, 0);
    const GenMidiVoice snare = voice({0x0C, 0xF8, 0xB5, 0x00, 0x00, 0x00}, 0x0E, {0x00, 0xF6, 0x94, 0x03, 0x00, 0x00}, 0);
    const GenMidiVoice hat = voice({0x0C, 0xF9, 0xF5, 0x03, 0x00, 0x00}, 0x0E, {0x00, 0xF8, 0xF8, 0x03, 0x00, 0x10}, 0);
    for (int n = 35; n <= 81; ++n) {
        GenMidiInstrument& in = bank.instruments[static_cast<size_t>(128 + n - 35)];
        in.flags = GenMidiInstrument::FixedPitch;
        bool k = (n == 35 || n == 36 || n == 41 || n == 43 || n == 45 || n == 47);
        bool s = (n == 38 || n == 40 || n == 37 || n == 39);
        in.voice[0] = k ? kick : (s ? snare : hat);
        in.voice[1] = in.voice[0];
        in.fixedNote = static_cast<uint8_t>(k ? 36 : (s ? 60 : 84));
        in.name = "builtin drum";
    }
    return bank;
}

}  // namespace rl::audio
