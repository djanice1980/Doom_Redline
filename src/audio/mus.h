#pragma once
// Doom MUS music format: a compact MIDI-like event stream at 140 ticks/s.
// Parses a lump into a flat event list the sequencer walks at playback time.
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace rl::audio {

struct MusEvent {
    enum class Type : uint8_t { NoteOff, NoteOn, PitchBend, System, Controller, ScoreEnd };
    Type type;
    uint8_t channel;    // MIDI channel 0-15 (MUS channel 15 remapped to 9 = percussion)
    uint8_t a = 0;      // note / controller number / bend value / system event
    uint8_t b = 0;      // velocity / controller value
    uint32_t delayTicks = 0;   // ticks to wait AFTER this event (140 Hz)
};

struct MusTrack {
    std::vector<MusEvent> events;
    uint32_t totalTicks = 0;
    bool valid() const { return !events.empty(); }
};

// Returns an empty track on malformed input. Never throws.
MusTrack parseMus(std::span<const uint8_t> data);

}  // namespace rl::audio
