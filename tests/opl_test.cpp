// Standalone test for rl::opl::Opl3. Plain asserts, no framework; returns
// non-zero on failure. Exercises pitch accuracy, envelope release, FM
// modulation, LFOs, rhythm mode, register fuzzing, output scaling and
// throughput.
#include "audio/opl3.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

using rl::opl::Opl3;

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        ++g_checks;                                                     \
        if (!(cond)) {                                                  \
            ++g_failures;                                               \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                               \
    } while (0)

namespace {

constexpr uint32_t kRate = 48000;

// Operator register offsets for channel c (0-8) of a bank.
uint16_t modOff(int c) { return static_cast<uint16_t>((c / 3) * 8 + (c % 3)); }
uint16_t carOff(int c) { return static_cast<uint16_t>(modOff(c) + 3); }

struct Voice {
    uint8_t modTl, carTl, modMult, carMult, cnt, fb, modWs, carWs;
    uint8_t modFlags, carFlags;  // AM/VIB/EG/KSR bits (0x20 register high nibble)
};

// Programs a channel (bank 0 or 1) with a percussive-free sustained voice:
// AR 15, DR 0, SL 0, RR 15, EG type on.
void program(Opl3& chip, int bank, int c, const Voice& v) {
    const uint16_t base = static_cast<uint16_t>(bank << 8);
    const uint16_t m = base + modOff(c), k = base + carOff(c);
    chip.writeReg(m + 0x20, static_cast<uint8_t>(v.modFlags | v.modMult));
    chip.writeReg(k + 0x20, static_cast<uint8_t>(v.carFlags | v.carMult));
    chip.writeReg(m + 0x40, v.modTl);
    chip.writeReg(k + 0x40, v.carTl);
    chip.writeReg(m + 0x60, 0xF0);
    chip.writeReg(k + 0x60, 0xF0);
    chip.writeReg(m + 0x80, 0x0F);
    chip.writeReg(k + 0x80, 0x0F);
    chip.writeReg(m + 0xE0, v.modWs);
    chip.writeReg(k + 0xE0, v.carWs);
    chip.writeReg(static_cast<uint16_t>(base + 0xC0 + c),
                  static_cast<uint8_t>(0x30 | (v.fb << 1) | v.cnt));
}

void keyOn(Opl3& chip, int bank, int c, double hz, int block) {
    const int fnum = static_cast<int>(std::lround(hz * std::pow(2.0, 20 - block) / 49716.0));
    const uint16_t base = static_cast<uint16_t>(bank << 8);
    chip.writeReg(static_cast<uint16_t>(base + 0xA0 + c), static_cast<uint8_t>(fnum & 0xFF));
    chip.writeReg(static_cast<uint16_t>(base + 0xB0 + c),
                  static_cast<uint8_t>(0x20 | (block << 2) | (fnum >> 8)));
}

void keyOff(Opl3& chip, int bank, int c) {
    const uint16_t base = static_cast<uint16_t>(bank << 8);
    chip.writeReg(static_cast<uint16_t>(base + 0xB0 + c), 0);
}

std::vector<float> render(Opl3& chip, int frames) {
    std::vector<float> buf(static_cast<size_t>(frames) * 2, 0.0f);
    chip.generate(buf.data(), frames);
    return buf;
}

float peakAbs(const std::vector<float>& b, size_t from = 0) {
    float p = 0;
    for (size_t i = from; i < b.size(); ++i) p = std::max(p, std::fabs(b[i]));
    return p;
}

bool allFinite(const std::vector<float>& b) {
    for (float v : b)
        if (!std::isfinite(v)) return false;
    return true;
}

// Fundamental from zero crossings of the left channel.
double zeroCrossFreq(const std::vector<float>& b, uint32_t rate) {
    int crossings = 0;
    bool pos = b[0] >= 0;
    for (size_t i = 2; i < b.size(); i += 2) {
        const bool p = b[i] >= 0;
        if (p != pos) ++crossings;
        pos = p;
    }
    const double seconds = static_cast<double>(b.size() / 2) / rate;
    return crossings / 2.0 / seconds;
}

int zeroCrossings(const std::vector<float>& b) {
    int crossings = 0;
    bool pos = b[0] >= 0;
    for (size_t i = 2; i < b.size(); i += 2) {
        const bool p = b[i] >= 0;
        if (p != pos) ++crossings;
        pos = p;
    }
    return crossings;
}

double derivRms(const std::vector<float>& b) {
    double acc = 0;
    size_t n = 0;
    for (size_t i = 2; i < b.size(); i += 2) {
        const double d = b[i] - b[i - 2];
        acc += d * d;
        ++n;
    }
    return std::sqrt(acc / static_cast<double>(n));
}

const Voice kSineCarrier{63, 0, 1, 1, 1, 0, 0, 0, 0x20, 0x20};

// -------------------------------------------------------------------------

void testPitchAndRelease() {
    Opl3 chip;
    chip.reset(kRate);
    chip.writeReg(0x105, 1);
    program(chip, 0, 0, kSineCarrier);
    keyOn(chip, 0, 0, 440.0, 4);

    auto buf = render(chip, static_cast<int>(kRate / 2));
    CHECK(allFinite(buf));
    const float peak = peakAbs(buf);
    std::printf("sine 440 Hz: peak %.4f\n", peak);
    CHECK(peak > 0.05f);
    CHECK(peak < 0.5f);
    const double f = zeroCrossFreq(buf, kRate);
    std::printf("sine 440 Hz: measured %.2f Hz\n", f);
    CHECK(std::fabs(f - 440.0) < 440.0 * 0.02);

    // Left and right both enabled (0xC0 bits 4-5 set): identical channels.
    CHECK(buf[100] == buf[101]);

    keyOff(chip, 0, 0);
    auto rel = render(chip, static_cast<int>(kRate / 10));  // 100 ms
    const float tailPeak = peakAbs(rel, rel.size() - 2000);
    std::printf("release tail peak after 100 ms: %.6f\n", tailPeak);
    CHECK(tailPeak < 0.001f);
}

void testFmModulation() {
    Opl3 pure;
    pure.reset(kRate);
    pure.writeReg(0x105, 1);
    program(pure, 0, 0, kSineCarrier);
    keyOn(pure, 0, 0, 440.0, 4);
    auto a = render(pure, static_cast<int>(kRate / 4));

    Opl3 fm;
    fm.reset(kRate);
    fm.writeReg(0x105, 1);
    Voice v = kSineCarrier;
    v.modTl = 0;
    v.modMult = 2;
    v.cnt = 0;
    program(fm, 0, 0, v);
    keyOn(fm, 0, 0, 440.0, 4);
    auto b = render(fm, static_cast<int>(kRate / 4));

    CHECK(allFinite(b));
    CHECK(peakAbs(b) > 0.05f);
    const int zcA = zeroCrossings(a), zcB = zeroCrossings(b);
    const double dA = derivRms(a), dB = derivRms(b);
    std::printf("FM: zero crossings %d vs %d, derivative RMS %.5f vs %.5f\n", zcA, zcB, dA, dB);
    CHECK(zcB > zcA * 3 / 2 || dB > dA * 1.5);
}

void testLfos() {
    auto renderWith = [](uint8_t flags, uint8_t bd) {
        Opl3 chip;
        chip.reset(kRate);
        chip.writeReg(0x105, 1);
        chip.writeReg(0xBD, bd);
        Voice v = kSineCarrier;
        v.carFlags = static_cast<uint8_t>(0x20 | flags);
        program(chip, 0, 0, v);
        keyOn(chip, 0, 0, 440.0, 4);
        return render(chip, kRate);
    };
    auto plain = renderWith(0x00, 0x00);
    auto trem = renderWith(0x80, 0x80);
    auto vib = renderWith(0x40, 0x40);
    CHECK(allFinite(trem));
    CHECK(allFinite(vib));
    int diffT = 0, diffV = 0;
    for (size_t i = 0; i < plain.size(); ++i) {
        if (std::fabs(plain[i] - trem[i]) > 1e-4f) ++diffT;
        if (std::fabs(plain[i] - vib[i]) > 1e-4f) ++diffV;
    }
    std::printf("tremolo differs in %d samples, vibrato in %d\n", diffT, diffV);
    CHECK(diffT > 1000);
    CHECK(diffV > 1000);
    // Tremolo must not raise the peak level.
    CHECK(peakAbs(trem) <= peakAbs(plain) + 1e-4f);
}

void testRhythm() {
    Opl3 chip;
    chip.reset(kRate);
    chip.writeReg(0x105, 1);
    Voice v{0, 0, 1, 1, 0, 0, 0, 0, 0x20, 0x20};
    for (int c = 6; c <= 8; ++c) {
        program(chip, 0, c, v);
        keyOn(chip, 0, c, 220.0 + 50 * c, 3);
        keyOff(chip, 0, c);
    }
    chip.writeReg(0xBD, 0x20);  // rhythm mode, nothing keyed
    auto silent = render(chip, 2000);
    CHECK(allFinite(silent));
    chip.writeReg(0xBD, 0x3F);  // all five keyed
    auto buf = render(chip, static_cast<int>(kRate / 4));
    CHECK(allFinite(buf));
    const float peak = peakAbs(buf);
    std::printf("rhythm mode peak: %.4f\n", peak);
    CHECK(peak > 0.01f);
    CHECK(peak <= 1.0f);
    // Key each one individually, then off, then leave rhythm mode.
    for (int bit = 0; bit < 5; ++bit) {
        chip.writeReg(0xBD, static_cast<uint8_t>(0x20 | (1 << bit)));
        auto b = render(chip, 4000);
        CHECK(allFinite(b));
        chip.writeReg(0xBD, 0x20);
        b = render(chip, 4000);
        CHECK(allFinite(b));
    }
    chip.writeReg(0xBD, 0x00);
    CHECK(allFinite(render(chip, 4000)));
}

void testRegisterFuzz() {
    for (uint8_t val : {static_cast<uint8_t>(0x00), static_cast<uint8_t>(0x55),
                        static_cast<uint8_t>(0xFF)}) {
        Opl3 chip;
        chip.reset(kRate);
        for (uint16_t r = 0; r < 0x200; ++r) chip.writeReg(r, val);
        auto buf = render(chip, static_cast<int>(kRate / 5));
        CHECK(allFinite(buf));
        const float peak = peakAbs(buf);
        std::printf("fuzz 0x%02X: peak %.4f\n", val, peak);
        CHECK(peak <= 1.5f);
        // Toggle OPL3 mode/4-op while notes are keyed: must stay finite.
        chip.writeReg(0x105, 0);
        chip.writeReg(0x104, 0x3F);
        chip.writeReg(0x105, 1);
        buf = render(chip, 4000);
        CHECK(allFinite(buf));
        CHECK(peakAbs(buf) <= 1.5f);
    }
    // Also fuzz with 4-op and rhythm both on, and OPL2 mode with bank-1 writes.
    Opl3 chip;
    chip.reset(44100);
    for (uint16_t r = 0; r < 0x200; ++r) chip.writeReg(r, static_cast<uint8_t>(r * 37));
    chip.writeReg(0x104, 0x3F);
    chip.writeReg(0xBD, 0xFF);
    auto buf = render(chip, 20000);
    CHECK(allFinite(buf));
    CHECK(peakAbs(buf) <= 1.5f);
}

void testAllChannelsFullVolume() {
    Opl3 chip;
    chip.reset(kRate);
    chip.writeReg(0x105, 1);
    for (int ch = 0; ch < 18; ++ch) {
        program(chip, ch / 9, ch % 9, kSineCarrier);
        keyOn(chip, ch / 9, ch % 9, 220.0 * std::pow(2.0, ch / 12.0), 4);
    }
    auto buf = render(chip, kRate);
    CHECK(allFinite(buf));
    const float peak = peakAbs(buf);
    std::printf("18 channels full volume: peak %.4f\n", peak);
    CHECK(peak <= 1.0f);
    CHECK(peak > 0.5f);
}

void testThroughput() {
    Opl3 chip;
    chip.reset(kRate);
    chip.writeReg(0x105, 1);
    chip.writeReg(0xBD, 0xC0);
    Voice v{20, 0, 2, 1, 0, 5, 0, 0, 0xE0, 0xE0};
    for (int ch = 0; ch < 18; ++ch) {
        program(chip, ch / 9, ch % 9, v);
        keyOn(chip, ch / 9, ch % 9, 110.0 * std::pow(2.0, ch / 7.0), 4);
    }
    std::vector<float> buf(static_cast<size_t>(kRate) * 2 * 5, 0.0f);
    const auto t0 = std::chrono::steady_clock::now();
    constexpr int kBlock = 512;
    const int total = static_cast<int>(kRate) * 5;
    for (int f = 0; f < total; f += kBlock) {
        chip.generate(buf.data() + static_cast<size_t>(f) * 2, std::min(kBlock, total - f));
    }
    const auto t1 = std::chrono::steady_clock::now();
    const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    std::printf("throughput: 5 s of 18 voices rendered in %.1f ms (%.1f%% of real time)\n",
                ms, ms / 50.0);
    CHECK(allFinite(buf));
    CHECK(ms < 1000.0);
}

}  // namespace

int main() {
    testPitchAndRelease();
    testFmModulation();
    testLfos();
    testRhythm();
    testRegisterFuzz();
    testAllChannelsFullVolume();
    testThroughput();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
