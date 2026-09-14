#pragma once
// Yamaha YMF262 (OPL3) FM synthesizer emulator, register-compatible with the
// YM3812 (OPL2) that Doom's DMX driver programmed through GENMIDI. Written
// from the chip's documented behaviour; no third-party code.
//
// Usage: reset(rate) -> writeReg(...) as a driver would -> generate() from
// the audio callback. Registers are the raw chip map: 0x000-0x0FF is the
// primary (OPL2-compatible) bank, 0x100-0x1FF the OPL3 second bank
// (0x105 bit 0 enables OPL3 mode: 18 channels, stereo panning via 0xC0
// bits 4-5, 4-op pairs via 0x104).
#include <cstdint>

namespace rl::opl {

class Opl3 {
public:
    Opl3();
    ~Opl3();
    Opl3(const Opl3&) = delete;
    Opl3& operator=(const Opl3&) = delete;

    // Silences everything and sets the output sample rate. The chip's native
    // rate is 49716 Hz; output is resampled to `sampleRate`.
    void reset(uint32_t sampleRate);

    // Register write: reg 0x000-0x1FF, val 0-255.
    void writeReg(uint16_t reg, uint8_t val);

    // Renders `frames` stereo frames of interleaved float samples in [-1, 1]
    // into `out` (2 * frames floats). Additive: the caller clears the buffer.
    void generate(float* out, int frames);

private:
    struct Impl;
    Impl* impl_;
};

}  // namespace rl::opl
