// MUS parser + GENMIDI + OPL driver checks. Uses a synthetic MUS score and,
// when the Steam Doom WAD is present, decodes every real track too.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include "audio/genmidi.h"
#include "audio/mus.h"
#include "audio/opl_driver.h"

using namespace rl::audio;

static int g_fail = 0;
#define CHECK(cond)                                                                   \
    do {                                                                              \
        if (!(cond)) {                                                                \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
            ++g_fail;                                                                 \
        }                                                                             \
    } while (0)

static std::vector<uint8_t> synthMus() {
    // Header + score: program 0 on ch 0, volume 100, note 60 vel 90, delay 70 ticks, release, delay 70, end.
    std::vector<uint8_t> d = {'M', 'U', 'S', 0x1A, 0, 0, 16, 0, 1, 0, 0, 0, 1, 0, 0, 0};
    std::vector<uint8_t> score = {
        0x40, 0, 0,            // controller: change instrument 0 (ch 0)
        0x40, 3, 100,          // controller: volume 100
        0x90, 0x80 | 60, 90,   // play note 60 with volume 90, last -> delay follows
        70,                    // delay 70 ticks (0.5 s)
        0x80, 60,              // release note 60, delay follows
        70,
        0x60,                  // score end
    };
    d.insert(d.end(), score.begin(), score.end());
    d[4] = static_cast<uint8_t>(score.size());
    d[5] = 0;
    return d;
}

static std::vector<uint8_t> readLump(const std::string& wadPath, const std::string& lumpName) {
    std::ifstream f(wadPath, std::ios::binary);
    if (!f) return {};
    uint8_t hdr[12];
    f.read(reinterpret_cast<char*>(hdr), 12);
    uint32_t n = hdr[4] | (hdr[5] << 8) | (hdr[6] << 16) | (static_cast<uint32_t>(hdr[7]) << 24);
    uint32_t dir = hdr[8] | (hdr[9] << 8) | (hdr[10] << 16) | (static_cast<uint32_t>(hdr[11]) << 24);
    f.seekg(dir);
    for (uint32_t i = 0; i < n; ++i) {
        uint8_t e[16];
        f.read(reinterpret_cast<char*>(e), 16);
        uint32_t off = e[0] | (e[1] << 8) | (e[2] << 16) | (static_cast<uint32_t>(e[3]) << 24);
        uint32_t size = e[4] | (e[5] << 8) | (e[6] << 16) | (static_cast<uint32_t>(e[7]) << 24);
        std::string name(reinterpret_cast<char*>(e + 8), 8);
        name = name.c_str();
        if (name == lumpName) {
            std::vector<uint8_t> out(size);
            f.seekg(off);
            f.read(reinterpret_cast<char*>(out.data()), size);
            return out;
        }
    }
    return {};
}

int main() {
    // MUS parsing
    MusTrack t = parseMus(synthMus());
    CHECK(t.valid());
    CHECK(t.events.size() == 5);
    CHECK(t.events[0].type == MusEvent::Type::Controller && t.events[0].a == 0);
    CHECK(t.events[2].type == MusEvent::Type::NoteOn && t.events[2].a == 60 && t.events[2].b == 90 && t.events[2].delayTicks == 70);
    CHECK(t.events[3].type == MusEvent::Type::NoteOff && t.events[3].delayTicks == 70);
    CHECK(t.totalTicks == 140);
    CHECK(!parseMus(std::vector<uint8_t>{1, 2, 3}).valid());
    std::vector<uint8_t> truncated = synthMus();
    truncated.resize(20);
    (void)parseMus(truncated);   // must not crash

    // Built-in bank drives the OPL: a note produces sound, release silences it.
    GenMidiBank bank = builtinGenMidi();
    CHECK(bank.instruments.size() == 175);
    OplDriver drv(48000);
    drv.setBank(&bank);
    drv.programChange(0, 0);
    drv.controller(0, 7, 100);
    drv.noteOn(0, 60, 100);
    std::vector<float> buf(48000 * 2, 0.f);
    drv.render(buf.data(), 24000);
    float peak = 0.f;
    for (int i = 0; i < 24000 * 2; ++i) peak = std::max(peak, std::fabs(buf[static_cast<size_t>(i)]));
    CHECK(peak > 0.02f && peak < 1.f);
    drv.noteOff(0, 60);
    std::fill(buf.begin(), buf.end(), 0.f);
    drv.render(buf.data(), 48000);
    float tail = 0.f;
    for (int i = 40000 * 2; i < 48000 * 2; ++i) tail = std::max(tail, std::fabs(buf[static_cast<size_t>(i)]));
    CHECK(tail < 0.02f);
    // Percussion and many voices: no NaNs, bounded.
    for (int n = 35; n <= 81; ++n) drv.noteOn(9, n, 110);
    for (int n = 48; n < 72; ++n) drv.noteOn(1, n, 120);
    std::fill(buf.begin(), buf.end(), 0.f);
    drv.render(buf.data(), 48000);
    bool finite = true;
    float p2 = 0.f;
    for (float v : buf) { finite = finite && std::isfinite(v); p2 = std::max(p2, std::fabs(v)); }
    CHECK(finite);
    CHECK(p2 <= 1.5f);
    drv.allNotesOff();

    // Real WAD, if present: GENMIDI parses and every D_* track parses and plays without NaNs.
    const char* home = std::getenv("HOME");
    std::string wad = home ? std::string(home) + "/.steam/steam/steamapps/common/Ultimate Doom/rerelease/doom.wad" : "";
    std::vector<uint8_t> gm = readLump(wad, "GENMIDI");
    if (!gm.empty()) {
        auto real = parseGenMidi(gm);
        CHECK(real.has_value());
        CHECK(real && real->instruments.size() == 175);
        CHECK(real && real->instruments[0].name.find("Piano") != std::string::npos);
        CHECK(real && real->instruments[128 + 36 - 35].fixedPitch());   // bass drum
        const char* names[] = {"D_E1M1", "D_E1M8", "D_INTRO", "D_INTER", "D_E2M8", "D_BUNNY"};
        for (const char* n : names) {
            std::vector<uint8_t> mus = readLump(wad, n);
            CHECK(!mus.empty());
            MusTrack tr = parseMus(mus);
            CHECK(tr.valid() && tr.events.size() > 100);
            // Render the first second through the driver.
            OplDriver d2(48000);
            d2.setBank(&*real);
            double acc = 0.0;
            size_t ei = 0;
            std::vector<float> out(48000 * 2, 0.f);
            int rendered = 0;
            while (rendered < 48000 && ei < tr.events.size()) {
                const MusEvent& ev = tr.events[ei++];
                switch (ev.type) {
                case MusEvent::Type::NoteOn: d2.noteOn(ev.channel, ev.a, ev.b); break;
                case MusEvent::Type::NoteOff: d2.noteOff(ev.channel, ev.a); break;
                case MusEvent::Type::PitchBend: d2.pitchBend(ev.channel, ev.a * 64); break;
                case MusEvent::Type::Controller:
                    if (ev.a == 0) d2.programChange(ev.channel, ev.b);
                    else if (ev.a == 3) d2.controller(ev.channel, 7, ev.b);
                    else if (ev.a == 4) d2.controller(ev.channel, 10, ev.b);
                    break;
                default: break;
                }
                acc += ev.delayTicks * (48000.0 / 140.0);
                int chunk = std::min(48000 - rendered, static_cast<int>(acc));
                if (chunk > 0) { d2.render(out.data() + static_cast<size_t>(rendered) * 2, chunk); rendered += chunk; acc -= chunk; }
            }
            float pk = 0.f;
            bool ok = true;
            for (float v : out) { ok = ok && std::isfinite(v); pk = std::max(pk, std::fabs(v)); }
            CHECK(ok);
            CHECK(pk > 0.01f);   // the first second of every one of these tracks has audible notes
            std::printf("%s: %zu events, %.1f s, first-second peak %.3f\n", n, tr.events.size(), tr.totalTicks / 140.0, pk);
        }
    } else {
        std::printf("(no doom.wad found; real-track checks skipped)\n");
    }

    if (g_fail) { std::fprintf(stderr, "%d check(s) failed\n", g_fail); return 1; }
    std::printf("music_test: all checks passed\n");
    return 0;
}
