// Renders a MUS lump from a WAD through the OPL3 driver to a 16-bit WAV.
// Usage: musrender <wad> <lump> <out.wav> [seconds]
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include "audio/genmidi.h"
#include "audio/mus.h"
#include "audio/opl_driver.h"

using namespace rl::audio;

static std::vector<uint8_t> readLump(const std::string& wadPath, const std::string& lumpName) {
    std::ifstream f(wadPath, std::ios::binary);
    if (!f) return {};
    uint8_t hdr[12];
    f.read(reinterpret_cast<char*>(hdr), 12);
    uint32_t n = hdr[4] | (hdr[5] << 8) | (hdr[6] << 16) | (static_cast<uint32_t>(hdr[7]) << 24);
    uint32_t dir = hdr[8] | (hdr[9] << 8) | (hdr[10] << 16) | (static_cast<uint32_t>(hdr[11]) << 24);
    f.seekg(dir);
    std::vector<uint8_t> found;
    for (uint32_t i = 0; i < n; ++i) {
        uint8_t e[16];
        f.read(reinterpret_cast<char*>(e), 16);
        uint32_t off = e[0] | (e[1] << 8) | (e[2] << 16) | (static_cast<uint32_t>(e[3]) << 24);
        uint32_t size = e[4] | (e[5] << 8) | (e[6] << 16) | (static_cast<uint32_t>(e[7]) << 24);
        std::string name(reinterpret_cast<char*>(e + 8), 8);
        name = name.c_str();
        if (name == lumpName) {   // last wins
            found.resize(size);
            auto pos = f.tellg();
            f.seekg(off);
            f.read(reinterpret_cast<char*>(found.data()), size);
            f.seekg(pos);
        }
    }
    return found;
}

int main(int argc, char** argv) {
    if (argc < 4) { std::fprintf(stderr, "usage: musrender <wad> <lump> <out.wav> [seconds]\n"); return 2; }
    std::string wad = argv[1], lump = argv[2], out = argv[3];
    double seconds = argc > 4 ? std::atof(argv[4]) : 30.0;
    const uint32_t rate = 48000;
    std::vector<uint8_t> mus = readLump(wad, lump);
    if (mus.empty()) { std::fprintf(stderr, "lump %s not found\n", lump.c_str()); return 1; }
    MusTrack track = parseMus(mus);
    if (!track.valid()) { std::fprintf(stderr, "not a MUS lump\n"); return 1; }
    GenMidiBank bank;
    std::vector<uint8_t> gm = readLump(wad, "GENMIDI");
    if (auto b = parseGenMidi(gm)) bank = std::move(*b); else bank = builtinGenMidi();

    OplDriver drv(rate);
    drv.setBank(&bank);
    const size_t total = static_cast<size_t>(seconds * rate);
    std::vector<float> buf(total * 2, 0.f);
    size_t rendered = 0, ei = 0;
    double acc = 0.0;
    const double samplesPerTick = rate / 140.0;
    while (rendered < total) {
        while (acc <= 0.0) {
            const MusEvent& ev = track.events[ei];
            if (ev.type == MusEvent::Type::ScoreEnd) { drv.allNotesOff(); ei = 0; continue; }
            switch (ev.type) {
            case MusEvent::Type::NoteOn: drv.noteOn(ev.channel, ev.a, ev.b); break;
            case MusEvent::Type::NoteOff: drv.noteOff(ev.channel, ev.a); break;
            case MusEvent::Type::PitchBend: drv.pitchBend(ev.channel, ev.a * 64); break;
            case MusEvent::Type::Controller:
                switch (ev.a) {
                case 0: drv.programChange(ev.channel, ev.b); break;
                case 3: drv.controller(ev.channel, 7, ev.b); break;
                case 4: drv.controller(ev.channel, 10, ev.b); break;
                case 5: drv.controller(ev.channel, 11, ev.b); break;
                case 8: drv.controller(ev.channel, 64, ev.b); break;
                default: break;
                }
                break;
            case MusEvent::Type::System:
                if (ev.a == 10 || ev.a == 11) drv.controller(ev.channel, 123, 0);
                break;
            default: break;
            }
            acc += ev.delayTicks * samplesPerTick;
            ++ei;
        }
        int chunk = static_cast<int>(std::min<double>(static_cast<double>(total - rendered), std::ceil(acc)));
        if (chunk <= 0) chunk = 1;
        drv.render(buf.data() + rendered * 2, chunk);
        rendered += static_cast<size_t>(chunk);
        acc -= chunk;
    }
    float peak = 0.f;
    for (float v : buf) peak = std::max(peak, std::fabs(v));
    float gain = peak > 0.f ? std::min(1.f, 0.9f / peak) : 1.f;

    std::ofstream w(out, std::ios::binary);
    auto put32 = [&](uint32_t v) { w.put(static_cast<char>(v)); w.put(static_cast<char>(v >> 8)); w.put(static_cast<char>(v >> 16)); w.put(static_cast<char>(v >> 24)); };
    auto put16 = [&](uint16_t v) { w.put(static_cast<char>(v)); w.put(static_cast<char>(v >> 8)); };
    uint32_t dataBytes = static_cast<uint32_t>(total * 2 * 2);
    w.write("RIFF", 4); put32(36 + dataBytes); w.write("WAVE", 4);
    w.write("fmt ", 4); put32(16); put16(1); put16(2); put32(rate); put32(rate * 4); put16(4); put16(16);
    w.write("data", 4); put32(dataBytes);
    for (float v : buf) put16(static_cast<uint16_t>(static_cast<int16_t>(std::lround(std::max(-1.f, std::min(1.f, v * gain)) * 32767.f))));
    std::printf("%s: %zu events, %.1f s long, rendered %.1f s, peak %.3f, gain %.2f -> %s\n", lump.c_str(), track.events.size(), track.totalTicks / 140.0, seconds, peak, gain, out.c_str());
    return 0;
}
