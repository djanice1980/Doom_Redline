// Yamaha YMF262 (OPL3) FM synthesizer emulator. Clean implementation written
// from the chip's documented behaviour (YM3812/YMF262 datasheets and the
// well-known description of the chip's log-sin / exp attenuation arithmetic).
// No third-party emulator code.
//
// Model summary
//   * Native rate 49716 Hz; output is linearly interpolated to the rate given
//     to reset() and ADDED into the caller's interleaved stereo float buffer.
//   * 36 operators / 18 channels, register-compatible OPL2 bank at 0x000-0x0FF
//     and the OPL3 bank at 0x100-0x1FF (0x105 bit 0 = NEW, 0x104 = 4-op pairs).
//   * Operator output is computed in the attenuation domain exactly the way
//     the chip does it: a quarter-wave log-sin table (256 entries, 1/256 octave
//     units) plus a 9-bit envelope value (0.1875 dB per step, 511 = silent)
//     scaled by 8, TL (<<2), KSL and tremolo, then an exp table lookup with
//     the sign carried separately.  Operator outputs are 13-bit signed
//     (+-4094 at full volume) and modulate the next operator's 10-bit phase
//     index directly, as on the real chip.
//   * Envelope: rate = 4*R + key-scale value (BLOCK*2 + FNUM bit 9, >>2 when
//     KSR=0), clamped to 63.  Envelope ticks every 2^(12-rate_hi) samples
//     with a 4-tick increment pattern for the low two rate bits; rate_hi >= 12
//     ticks every sample with the increment scaled up.  Decay/release add to
//     the 9-bit envelope linearly (calibrated to the datasheet decay times);
//     attack subtracts a fraction of the remaining attenuation each tick,
//     giving the chip's fast exponential rise (calibrated to the datasheet
//     attack times within ~10 %).
//
// Simplifications (deliberate, documented here)
//   1. Output scaling: kOperatorPeak (0.25) is the float peak of one operator
//      at TL 0.  The stereo sum then passes through a soft limiter that is
//      transparent below +-0.5 and asymptotes to +-1.0, so the header's
//      "[-1, 1]" promise holds even when all 18 channels peak together.
//      Change kOperatorPeak to alter overall loudness.
//   2. Resampling is linear interpolation between successive chip samples
//      (no band-limiting); fine for 44.1/48 kHz targets.
//   3. Rhythm mode: bass drum is the normal channel-6 pair; tom is a plain
//      operator; snare, hi-hat and top cymbal derive their phase index from
//      the hi-hat/cymbal phase bits and a 23-bit LFSR noise source (the same
//      idea as the chip, not a bit-exact copy of its phase formulae).  The
//      five rhythm outputs are doubled, as the chip does.  Rhythm-mode
//      operators are keyed by 0xBD bits OR the channel key-on bit.
//   4. 4-op mode implements all four algorithms (CNT1/CNT2 combinations) and
//      the second channel of a pair takes its F-Number/BLOCK/KEY-ON from the
//      first channel; feedback applies to the first operator only.  Panning
//      of a 4-op voice comes from the first channel's 0xC0 register.
//   5. Panning: in OPL3 mode 0xC0 bits 4/5 enable left/right; if both are
//      zero (a driver that never set them) both outputs are enabled rather
//      than the hardware's silence.  In OPL2 mode both are always enabled.
//   6. Timers (0x02-0x04), CSM and NOTE-SEL (0x08) are ignored; NOTE-SEL is
//      treated as 0 (KSR uses BLOCK*2 + F-Number bit 9).  Waveforms 0-3 are
//      always honoured (0x01 WSE is not gated); 4-7 require OPL3 mode.
//   7. Feedback: (out[n-1] + out[n-2]) >> (9 - FB), i.e. the datasheet's
//      maximum of 4 pi at FB = 7; FB = 0 disables feedback.
//   8. KSL and vibrato tables are generated from the documented dB/cent
//      figures rather than copied ROM dumps; differences are sub-dB.
#include "audio/opl3.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace rl::opl {

namespace {

// ---------------------------------------------------------------------------
// Immutable tables, built once at first use.
// ---------------------------------------------------------------------------

constexpr uint32_t kNativeRate = 49716;
constexpr int kNumOps = 36;
constexpr int kNumChannels = 18;
constexpr int kEnvSilent = 511;      // 9-bit envelope, 511 = -96 dB
constexpr uint16_t kWaveSilent = 0x0FFF;  // attenuation that always yields 0
constexpr uint16_t kWaveNeg = 0x8000;     // sign flag in the waveform tables

// Peak float amplitude of a single operator at TL 0 (see header comment).
constexpr float kOperatorPeak = 0.25f;
constexpr float kOutputScale = kOperatorPeak / 4096.0f;

struct Tables {
    std::array<uint16_t, 256> logsin{};   // -log2(sin) * 256 over a quarter wave
    std::array<uint16_t, 256> exp{};      // 2^(i/256) * 1024 - 1024
    std::array<std::array<uint16_t, 1024>, 8> wave{};  // per waveform: att | sign
    std::array<uint8_t, 16> ksl{};        // KSL base attenuation, 0.75 dB units
    std::array<uint8_t, 16> mult2{};      // MULT * 2

    Tables() {
        const double pi = 3.14159265358979323846;
        for (int i = 0; i < 256; ++i) {
            const double s = std::sin((i + 0.5) * pi / 512.0);
            logsin[static_cast<size_t>(i)] =
                static_cast<uint16_t>(std::lround(-std::log2(s) * 256.0));
            exp[static_cast<size_t>(i)] =
                static_cast<uint16_t>(std::lround(std::pow(2.0, i / 256.0) * 1024.0) - 1024);
        }

        auto sine = [this](uint32_t p) -> uint16_t {  // full-cycle sine, p in 0..1023
            uint32_t q = p & 255;
            if (p & 256) q = 255 - q;
            uint16_t v = logsin[q];
            return (p & 512) ? static_cast<uint16_t>(v | kWaveNeg) : v;
        };

        for (uint32_t p = 0; p < 1024; ++p) {
            const uint16_t s = sine(p);
            const uint16_t sAbs = static_cast<uint16_t>(s & ~kWaveNeg);
            const uint16_t s2 = sine((p * 2) & 1023);
            const bool secondHalf = (p & 512) != 0;
            wave[0][p] = s;                                          // sine
            wave[1][p] = secondHalf ? kWaveSilent : s;               // half sine
            wave[2][p] = sAbs;                                       // abs sine
            wave[3][p] = (p & 256) ? kWaveSilent : sAbs;             // pulse sine
            wave[4][p] = secondHalf ? kWaveSilent : s2;              // double-freq sine
            wave[5][p] = secondHalf ? kWaveSilent : static_cast<uint16_t>(s2 & ~kWaveNeg);
            wave[6][p] = secondHalf ? kWaveNeg : 0;                  // square
            // Exponential "sawtooth": attenuation ramps up over the first half
            // (positive), mirrored and negative over the second half.
            const uint16_t ramp = static_cast<uint16_t>((secondHalf ? (1023 - p) : p) << 3);
            wave[7][p] = secondHalf ? static_cast<uint16_t>(ramp | kWaveNeg) : ramp;
        }

        // KSL: 6 dB per octave relative to the top of BLOCK 7, in 0.75 dB
        // units, indexed by the top four F-Number bits.  Index 0 is clamped
        // to 0 (below the lowest usable pitch).
        ksl[0] = 0;
        for (int n = 1; n < 16; ++n) {
            const double v = 64.0 + 8.0 * std::log2(n / 16.0);
            ksl[static_cast<size_t>(n)] = static_cast<uint8_t>(std::max(0L, std::lround(v)));
        }

        const uint8_t m2[16] = {1, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 20, 24, 24, 30, 30};
        for (int i = 0; i < 16; ++i) mult2[static_cast<size_t>(i)] = m2[i];
    }
};

const Tables& tables() {
    static const Tables t;
    return t;
}

// Increment pattern for the two low rate bits: averages 1, 1.25, 1.5, 1.75.
constexpr uint8_t kRatePattern[4][4] = {
    {1, 1, 1, 1}, {1, 1, 1, 2}, {1, 2, 1, 2}, {1, 2, 2, 2}};

enum class EgState : uint8_t { Off, Attack, Decay, Sustain, Release };

struct Operator {
    // Raw register fields.
    bool am = false, vib = false, egType = false, ksr = false;
    uint8_t mult = 0, kslBits = 0, tl = 0, ar = 0, dr = 0, sl = 0, rr = 0, ws = 0;
    // Derived from the owning (source) channel.
    uint16_t fnum = 0;
    uint8_t block = 0;
    uint8_t ksv = 0;        // key scale value for rates
    uint16_t kslEnv = 0;    // KSL attenuation in envelope units
    uint16_t tlEnv = 0;     // TL << 2
    uint16_t slLevel = 0;   // sustain level in envelope units
    // Runtime state.
    EgState state = EgState::Off;
    int32_t eg = kEnvSilent;
    uint32_t phase = 0;     // 20-bit phase accumulator (1024 index steps/cycle)
    uint32_t pidx = 0;      // 10-bit phase index for the current sample
    int32_t prev1 = 0, prev2 = 0;  // last two outputs (feedback)
    bool keyed = false;
};

struct Channel {
    uint16_t fnum = 0;
    uint8_t block = 0;
    bool keyOn = false;
    uint8_t fb = 0;
    bool cnt = false;
    uint8_t pan = 0;  // bits 4-5 of 0xC0 shifted down: bit0 = left, bit1 = right
};

}  // namespace

// ---------------------------------------------------------------------------
// Impl
// ---------------------------------------------------------------------------

struct Opl3::Impl {
    std::array<Operator, kNumOps> ops{};
    std::array<Channel, kNumChannels> chans{};
    std::array<uint8_t, 512> regs{};

    bool opl3 = false;
    uint8_t fourOp = 0;
    bool rhythm = false;
    uint8_t rhythmKeys = 0;
    bool tremDeep = false;
    bool vibDeep = false;

    uint32_t counter = 0;   // sample counter driving EG ticks and the LFOs
    uint32_t tremPos = 0;   // 0..209, advances every 64 samples
    int32_t tremValue = 0;  // current tremolo attenuation (envelope units)
    uint32_t noise = 1;     // 23-bit LFSR
    uint32_t noiseBit = 0;

    // Resampler state.
    double step = 1.0;
    double frac = 0.0;
    float prevL = 0, prevR = 0, curL = 0, curR = 0;

    // ---- register decoding helpers -------------------------------------

    static int slotFromOffset(uint8_t off) {  // 0x00-0x15 -> 0..17 or -1
        if (off > 0x15 || (off & 7) > 5) return -1;
        return static_cast<int>((off >> 3) * 6 + (off & 7));
    }
    static int channelOfSlot(int slot) { return (slot / 6) * 3 + (slot % 6) % 3; }
    static int modOp(int gch) {  // global channel -> global modulator op index
        const int bank = gch / 9, c = gch % 9;
        return bank * 18 + (c / 3) * 6 + (c % 3);
    }
    static int carOp(int gch) { return modOp(gch) + 3; }

    bool pairEnabled(int gch) const {  // is this channel part of an active 4-op pair
        if (!opl3) return false;
        const int bank = gch / 9, c = gch % 9;
        if (c >= 6) return false;
        return ((fourOp >> (bank * 3 + (c % 3))) & 1) != 0;
    }
    bool isPrimary(int gch) const { return pairEnabled(gch) && (gch % 9) < 3; }
    bool isSecondary(int gch) const { return pairEnabled(gch) && (gch % 9) >= 3; }
    int sourceChannel(int gch) const { return isSecondary(gch) ? gch - 3 : gch; }

    // ---- derived-state refresh ------------------------------------------

    void refreshOp(Operator& op) {
        const Tables& t = tables();
        int ksv = op.block * 2 + (op.fnum >> 9);
        if (!op.ksr) ksv >>= 2;
        op.ksv = static_cast<uint8_t>(ksv);

        int base = static_cast<int>(t.ksl[op.fnum >> 6]) - 8 * (7 - op.block);
        if (base < 0) base = 0;
        base <<= 2;  // 0.75 dB units -> envelope units
        switch (op.kslBits) {
            case 0: op.kslEnv = 0; break;
            case 1: op.kslEnv = static_cast<uint16_t>(base >> 1); break;  // 3 dB/oct
            case 2: op.kslEnv = static_cast<uint16_t>(base >> 2); break;  // 1.5 dB/oct
            default: op.kslEnv = static_cast<uint16_t>(base); break;     // 6 dB/oct
        }
        op.tlEnv = static_cast<uint16_t>(op.tl << 2);
        op.slLevel = static_cast<uint16_t>((op.sl == 15 ? 31 : op.sl) << 4);
    }

    void refreshChannel(int gch) {
        const Channel& src = chans[static_cast<size_t>(sourceChannel(gch))];
        for (int idx : {modOp(gch), carOp(gch)}) {
            Operator& op = ops[static_cast<size_t>(idx)];
            op.fnum = src.fnum;
            op.block = src.block;
            refreshOp(op);
        }
    }

    void refreshAll() {
        for (int c = 0; c < kNumChannels; ++c) refreshChannel(c);
        updateKeys();
    }

    // ---- key handling ------------------------------------------------------

    void keyOn(Operator& op) {
        op.phase = 0;
        if (op.state == EgState::Attack) return;
        op.state = EgState::Attack;
        // Rate 15 with full key scaling is an instant attack.
        const int rate = std::min(63, op.ar * 4 + op.ksv);
        if (op.ar != 0 && (rate >> 2) == 15) {
            op.eg = 0;
            op.state = EgState::Decay;
        }
    }

    static void keyOff(Operator& op) {
        if (op.state != EgState::Off) op.state = EgState::Release;
    }

    void updateKeys() {
        for (int gch = 0; gch < kNumChannels; ++gch) {
            const bool chanKey = chans[static_cast<size_t>(sourceChannel(gch))].keyOn;
            bool modKey = chanKey, carKey = chanKey;
            if (rhythm && gch >= 6 && gch <= 8) {
                switch (gch) {
                    case 6:  // bass drum
                        modKey |= (rhythmKeys & 0x10) != 0;
                        carKey |= (rhythmKeys & 0x10) != 0;
                        break;
                    case 7:  // hi-hat / snare
                        modKey |= (rhythmKeys & 0x01) != 0;
                        carKey |= (rhythmKeys & 0x08) != 0;
                        break;
                    default:  // tom / top cymbal
                        modKey |= (rhythmKeys & 0x04) != 0;
                        carKey |= (rhythmKeys & 0x02) != 0;
                        break;
                }
            }
            applyKey(ops[static_cast<size_t>(modOp(gch))], modKey);
            applyKey(ops[static_cast<size_t>(carOp(gch))], carKey);
        }
    }

    void applyKey(Operator& op, bool keyed) {
        if (keyed && !op.keyed) keyOn(op);
        else if (!keyed && op.keyed) keyOff(op);
        op.keyed = keyed;
    }

    // ---- register writes ---------------------------------------------------

    void write(uint16_t reg, uint8_t val) {
        reg &= 0x1FF;
        regs[reg] = val;
        const int bank = reg >> 8;
        const uint8_t r = static_cast<uint8_t>(reg & 0xFF);

        if (bank == 1 && r == 0x04) {
            fourOp = val & 0x3F;
            refreshAll();
            return;
        }
        if (bank == 1 && r == 0x05) {
            opl3 = (val & 1) != 0;
            refreshAll();
            return;
        }
        if (bank == 0 && r == 0xBD) {
            tremDeep = (val & 0x80) != 0;
            vibDeep = (val & 0x40) != 0;
            rhythm = (val & 0x20) != 0;
            rhythmKeys = val & 0x1F;
            updateKeys();
            return;
        }
        if (r < 0x20) return;  // test, timers, CSM/NOTE-SEL: ignored

        const uint8_t group = r & 0xE0;
        if (group == 0xA0 || group == 0xC0) {
            const uint8_t c = r & 0x0F;
            if (c > 8) return;
            const int gch = bank * 9 + c;
            Channel& ch = chans[static_cast<size_t>(gch)];
            if (group == 0xA0 && r < 0xB0) {
                ch.fnum = static_cast<uint16_t>((ch.fnum & 0x300) | val);
            } else if (group == 0xA0) {
                ch.fnum = static_cast<uint16_t>((ch.fnum & 0xFF) | ((val & 3) << 8));
                ch.block = (val >> 2) & 7;
                ch.keyOn = (val & 0x20) != 0;
            } else {
                ch.cnt = (val & 1) != 0;
                ch.fb = (val >> 1) & 7;
                ch.pan = (val >> 4) & 3;
                return;  // nothing derived to refresh
            }
            refreshChannel(gch);
            if (isPrimary(gch)) refreshChannel(gch + 3);
            updateKeys();
            return;
        }

        const int slot = slotFromOffset(r & 0x1F);
        if (slot < 0) return;
        Operator& op = ops[static_cast<size_t>(bank * 18 + slot)];
        switch (group) {
            case 0x20:
                op.am = (val & 0x80) != 0;
                op.vib = (val & 0x40) != 0;
                op.egType = (val & 0x20) != 0;
                op.ksr = (val & 0x10) != 0;
                op.mult = val & 0x0F;
                break;
            case 0x40:
                op.kslBits = (val >> 6) & 3;
                op.tl = val & 0x3F;
                break;
            case 0x60:
                op.ar = (val >> 4) & 0x0F;
                op.dr = val & 0x0F;
                break;
            case 0x80:
                op.sl = (val >> 4) & 0x0F;
                op.rr = val & 0x0F;
                break;
            case 0xE0:
                op.ws = val & 7;
                break;
            default:
                return;
        }
        refreshOp(op);
    }

    // ---- per-sample synthesis ---------------------------------------------

    void egStep(Operator& op) {
        uint8_t r;
        switch (op.state) {
            case EgState::Attack: r = op.ar; break;
            case EgState::Decay:
                if (op.eg >= op.slLevel) {
                    op.eg = op.slLevel;
                    op.state = op.egType ? EgState::Sustain : EgState::Release;
                    return;
                }
                r = op.dr;
                break;
            case EgState::Release: r = op.rr; break;
            default: return;  // Off / Sustain: hold
        }
        if (r == 0) return;  // rate 0 = never

        const int rate = std::min(63, r * 4 + op.ksv);
        const int hi = rate >> 2, lo = rate & 3;
        int stepAmt;
        if (hi >= 12) {
            stepAmt = kRatePattern[lo][counter & 3] << (hi - 12);
        } else {
            const uint32_t shift = static_cast<uint32_t>(12 - hi);
            if ((counter & ((1u << shift) - 1)) != 0) return;
            stepAmt = kRatePattern[lo][(counter >> shift) & 3];
        }

        if (op.state == EgState::Attack) {
            // Exponential approach to 0: remove ~9.4 % of the remaining
            // attenuation per tick (per unit step), at least 1 unit.
            const int dec = ((op.eg + 1) * stepAmt * 3 + 31) >> 5;
            op.eg -= dec;
            if (op.eg <= 0) {
                op.eg = 0;
                op.state = EgState::Decay;
            }
        } else {
            op.eg += stepAmt;
            if (op.state == EgState::Decay) {
                if (op.eg >= op.slLevel) {
                    op.eg = op.slLevel;
                    op.state = op.egType ? EgState::Sustain : EgState::Release;
                }
            } else if (op.eg >= kEnvSilent) {
                op.eg = kEnvSilent;
                op.state = EgState::Off;
            }
        }
    }

    int32_t vibratoDelta(uint16_t fnum) const {
        const uint32_t pos = (counter >> 10) & 7;
        int32_t full = fnum >> 7;
        if (!vibDeep) full >>= 1;
        const int32_t half = full >> 1;
        switch (pos) {
            case 1: case 3: return half;
            case 2: return full;
            case 5: case 7: return -half;
            case 6: return -full;
            default: return 0;
        }
    }

    void advanceOperator(Operator& op) {
        op.pidx = (op.phase >> 10) & 1023;
        int32_t fn = op.fnum;
        if (op.vib) fn += vibratoDelta(op.fnum);
        if (fn < 0) fn = 0;
        const uint32_t inc = ((static_cast<uint32_t>(fn) << op.block) *
                              tables().mult2[op.mult]) >> 1;
        op.phase = (op.phase + inc) & 0xFFFFF;
    }

    // Operator output for the given 10-bit phase index: 13-bit signed.
    int32_t opOutput(const Operator& op, uint32_t pidx) const {
        if (op.state == EgState::Off) return 0;
        int32_t env = op.eg + op.tlEnv + op.kslEnv + (op.am ? tremValue : 0);
        if (env > kEnvSilent) env = kEnvSilent;
        const Tables& t = tables();
        const uint16_t w = t.wave[op.ws][pidx & 1023];
        const uint32_t att = static_cast<uint32_t>(w & 0x7FFF) +
                             (static_cast<uint32_t>(env) << 3);
        const uint32_t mant = (static_cast<uint32_t>(t.exp[(att & 255) ^ 255]) | 1024u) << 1;
        const int32_t v = static_cast<int32_t>(mant >> (att >> 8));
        return (w & kWaveNeg) ? -v : v;
    }

    // Operator with self-feedback (the first operator of a channel/pair).
    int32_t fbOutput(Operator& op, uint8_t fb) {
        const int32_t mod = fb ? ((op.prev1 + op.prev2) >> (9 - fb)) : 0;
        const int32_t out = opOutput(op, op.pidx + static_cast<uint32_t>(mod));
        op.prev2 = op.prev1;
        op.prev1 = out;
        return out;
    }

    int32_t modulated(const Operator& op, int32_t mod) const {
        return opOutput(op, op.pidx + static_cast<uint32_t>(mod));
    }

    int32_t render2op(int gch) {
        const Channel& ch = chans[static_cast<size_t>(gch)];
        Operator& m = ops[static_cast<size_t>(modOp(gch))];
        const Operator& c = ops[static_cast<size_t>(carOp(gch))];
        const int32_t mOut = fbOutput(m, ch.fb);
        if (ch.cnt) return mOut + modulated(c, 0);
        return modulated(c, mOut);
    }

    int32_t render4op(int gch) {
        const Channel& ch1 = chans[static_cast<size_t>(gch)];
        const Channel& ch2 = chans[static_cast<size_t>(gch + 3)];
        Operator& a = ops[static_cast<size_t>(modOp(gch))];
        const Operator& b = ops[static_cast<size_t>(carOp(gch))];
        const Operator& c = ops[static_cast<size_t>(modOp(gch + 3))];
        const Operator& d = ops[static_cast<size_t>(carOp(gch + 3))];
        const int32_t aOut = fbOutput(a, ch1.fb);
        if (!ch1.cnt && !ch2.cnt) {          // A -> B -> C -> D
            return modulated(d, modulated(c, modulated(b, aOut)));
        }
        if (!ch1.cnt && ch2.cnt) {           // (A -> B) + (C -> D)
            return modulated(b, aOut) + modulated(d, modulated(c, 0));
        }
        if (ch1.cnt && !ch2.cnt) {           // A + (B -> C -> D)
            return aOut + modulated(d, modulated(c, modulated(b, 0)));
        }
        return aOut + modulated(c, modulated(b, 0)) + modulated(d, 0);  // A + (B->C) + D
    }

    // Rhythm-mode channels 7 and 8 (hi-hat/snare, tom/cymbal).
    int32_t renderRhythm78(int gch) {
        const Operator& hh = ops[13];
        const Operator& sd = ops[16];
        const Operator& tom = ops[14];
        const Operator& cym = ops[17];
        const uint32_t hp = hh.pidx, cp = cym.pidx;
        const uint32_t metal = (((hp >> 2) ^ (hp >> 7)) | ((cp >> 3) ^ (cp >> 5))) & 1;
        if (gch == 7) {
            uint32_t hhIdx;
            if (metal) hhIdx = noiseBit ? 0x2D0 : 0x234;
            else       hhIdx = noiseBit ? 0x034 : 0x0D0;
            const uint32_t sdIdx = (0x100u << ((hp >> 8) & 1)) ^ (noiseBit << 8);
            return (opOutput(hh, hhIdx) + opOutput(sd, sdIdx)) * 2;
        }
        const uint32_t cymIdx = metal ? 0x300 : 0x100;
        return (opOutput(tom, tom.pidx) + opOutput(cym, cymIdx)) * 2;
    }

    void renderChipSample(float& outL, float& outR) {
        ++counter;
        if ((counter & 63) == 0) {
            if (++tremPos >= 210) tremPos = 0;
        }
        const int32_t tri = static_cast<int32_t>(tremPos < 105 ? tremPos : 210 - tremPos);
        tremValue = tremDeep ? (tri >> 2) : (tri >> 4);

        noiseBit = noise & 1;
        noise >>= 1;
        if (noiseBit) noise ^= 0x420000u;  // x^23 + x^18 + 1

        for (Operator& op : ops) {
            egStep(op);
            advanceOperator(op);
        }

        int32_t sumL = 0, sumR = 0;
        const int limit = opl3 ? kNumChannels : 9;
        for (int gch = 0; gch < limit; ++gch) {
            if (isSecondary(gch)) continue;
            int32_t out;
            if (isPrimary(gch)) {
                out = render4op(gch);
            } else if (rhythm && gch >= 6 && gch <= 8) {
                out = (gch == 6) ? render2op(6) * 2 : renderRhythm78(gch);
            } else {
                out = render2op(gch);
            }
            const uint8_t pan = chans[static_cast<size_t>(gch)].pan;
            if (!opl3 || pan == 0) {
                sumL += out;
                sumR += out;
            } else {
                if (pan & 1) sumL += out;
                if (pan & 2) sumR += out;
            }
        }
        outL = softLimit(static_cast<float>(sumL) * kOutputScale);
        outR = softLimit(static_cast<float>(sumR) * kOutputScale);
    }

    static float softLimit(float x) {
        const float a = std::fabs(x);
        if (a <= 0.5f) return x;
        const float y = 0.5f + 0.5f * std::tanh((a - 0.5f) * 2.0f);
        return std::copysign(y, x);
    }

    // ---- public-facing operations -----------------------------------------

    void reset(uint32_t sampleRate) {
        ops.fill(Operator{});
        chans.fill(Channel{});
        regs.fill(0);
        opl3 = false;
        fourOp = 0;
        rhythm = false;
        rhythmKeys = 0;
        tremDeep = vibDeep = false;
        counter = 0;
        tremPos = 0;
        tremValue = 0;
        noise = 1;
        noiseBit = 0;
        if (sampleRate == 0) sampleRate = kNativeRate;
        step = static_cast<double>(kNativeRate) / static_cast<double>(sampleRate);
        frac = 0.0;
        prevL = prevR = curL = curR = 0.0f;
    }

    void generate(float* out, int frames) {
        for (int i = 0; i < frames; ++i) {
            frac += step;
            while (frac >= 1.0) {
                prevL = curL;
                prevR = curR;
                renderChipSample(curL, curR);
                frac -= 1.0;
            }
            const float f = static_cast<float>(frac);
            out[2 * i] += prevL + (curL - prevL) * f;
            out[2 * i + 1] += prevR + (curR - prevR) * f;
        }
    }
};

// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------

Opl3::Opl3() : impl_(new Impl) {
    (void)tables();
    impl_->reset(kNativeRate);
}

Opl3::~Opl3() { delete impl_; }

void Opl3::reset(uint32_t sampleRate) { impl_->reset(sampleRate); }

void Opl3::writeReg(uint16_t reg, uint8_t val) { impl_->write(reg, val); }

void Opl3::generate(float* out, int frames) {
    if (out == nullptr || frames <= 0) return;
    impl_->generate(out, frames);
}

}  // namespace rl::opl
