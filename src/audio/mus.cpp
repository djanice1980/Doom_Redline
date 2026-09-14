#include "audio/mus.h"

namespace rl::audio {

namespace {
uint16_t rd16(std::span<const uint8_t> d, size_t at) {
    return static_cast<uint16_t>(d[at] | (d[at + 1] << 8));
}
// MUS channels 0-14 -> MIDI 0-8, 10-15; MUS 15 -> MIDI 9 (percussion).
uint8_t midiChannel(uint8_t musChannel) {
    if (musChannel == 15) return 9;
    return musChannel < 9 ? musChannel : static_cast<uint8_t>(musChannel + 1);
}
}  // namespace

MusTrack parseMus(std::span<const uint8_t> d) {
    MusTrack track;
    if (d.size() < 16 || d[0] != 'M' || d[1] != 'U' || d[2] != 'S' || d[3] != 0x1A) return track;
    size_t scoreLen = rd16(d, 4);
    size_t scoreStart = rd16(d, 6);
    if (scoreStart >= d.size()) return track;
    size_t end = std::min(d.size(), scoreStart + scoreLen);
    size_t p = scoreStart;
    uint8_t lastVelocity[16] = {};
    for (auto& v : lastVelocity) v = 100;

    while (p < end) {
        uint8_t desc = d[p++];
        bool last = (desc & 0x80) != 0;
        uint8_t type = (desc >> 4) & 7;
        uint8_t ch = midiChannel(desc & 15);
        MusEvent ev{MusEvent::Type::NoteOff, ch};
        bool emit = true;
        switch (type) {
        case 0: {   // release note
            if (p >= end) return track;
            ev.type = MusEvent::Type::NoteOff;
            ev.a = d[p++] & 0x7F;
            break;
        }
        case 1: {   // play note
            if (p >= end) return track;
            uint8_t n = d[p++];
            ev.type = MusEvent::Type::NoteOn;
            ev.a = n & 0x7F;
            if (n & 0x80) {
                if (p >= end) return track;
                lastVelocity[ch] = d[p++] & 0x7F;
            }
            ev.b = lastVelocity[ch];
            break;
        }
        case 2: {   // pitch bend: 0..255, 128 = centre, +-1 semitone... MUS range is +-2 semitones over 0..255? (DMX: 128 = none, full range = 2 semitones)
            if (p >= end) return track;
            ev.type = MusEvent::Type::PitchBend;
            ev.a = d[p++];
            break;
        }
        case 3: {   // system event
            if (p >= end) return track;
            ev.type = MusEvent::Type::System;
            ev.a = d[p++] & 0x7F;
            break;
        }
        case 4: {   // controller
            if (p + 1 >= end) return track;
            ev.type = MusEvent::Type::Controller;
            ev.a = d[p++] & 0x7F;
            ev.b = d[p++] & 0x7F;
            break;
        }
        case 5:     // end of measure: no payload, no effect
            emit = false;
            break;
        case 6:     // score end
            ev.type = MusEvent::Type::ScoreEnd;
            break;
        default:    // 7: unused, one byte payload
            if (p >= end) return track;
            ++p;
            emit = false;
            break;
        }
        uint32_t delay = 0;
        if (last) {
            uint8_t b;
            do {
                if (p >= end) { b = 0; break; }
                b = d[p++];
                delay = (delay << 7) | (b & 0x7F);
            } while (b & 0x80);
        }
        if (emit) {
            ev.delayTicks = delay;
            track.events.push_back(ev);
            track.totalTicks += delay;
            if (ev.type == MusEvent::Type::ScoreEnd) break;
        } else if (delay > 0 && !track.events.empty()) {
            track.events.back().delayTicks += delay;
            track.totalTicks += delay;
        }
    }
    if (track.events.empty() || track.events.back().type != MusEvent::Type::ScoreEnd) track.events.push_back({MusEvent::Type::ScoreEnd, 0});
    return track;
}

}  // namespace rl::audio
