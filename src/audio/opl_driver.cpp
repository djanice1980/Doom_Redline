#include "audio/opl_driver.h"

#include <algorithm>
#include <cmath>

namespace rl::audio {

namespace {
// Operator register offsets within a bank for the 9 channels' modulators;
// the carrier is +3.
constexpr uint8_t kModOffset[9] = {0x00, 0x01, 0x02, 0x08, 0x09, 0x0A, 0x10, 0x11, 0x12};
constexpr double kChipRate = 49716.0;
}  // namespace

OplDriver::OplDriver(uint32_t sampleRate) {
    opl_.reset(sampleRate);
    reset();
}

uint16_t OplDriver::opReg(int chip, int op) const {
    int bank = chip / 9, ch = chip % 9;
    return static_cast<uint16_t>((bank ? 0x100 : 0x000) + kModOffset[ch] + (op ? 3 : 0));
}

uint16_t OplDriver::chReg(int chip) const {
    int bank = chip / 9, ch = chip % 9;
    return static_cast<uint16_t>((bank ? 0x100 : 0x000) + ch);
}

void OplDriver::reset() {
    for (uint16_t r = 0; r < 0x200; ++r) opl_.writeReg(r, 0);
    opl_.writeReg(0x105, 0x01);   // OPL3 mode: 18 channels, stereo
    opl_.writeReg(0x001, 0x20);   // waveform select enable (OPL2 compatibility)
    opl_.writeReg(0x0BD, 0x00);
    for (auto& v : voices_) v = Voice{};
    for (int i = 0; i < 18; ++i) { voices_[i].chip = i; keyOnByte_[i] = 0; }
    for (auto& c : channels_) c = Channel{};
    stamp_ = 0;
}

int OplDriver::allocVoice() {
    for (int i = 0; i < 18; ++i) if (!voices_[i].active) return i;
    // Steal the oldest.
    int oldest = 0;
    for (int i = 1; i < 18; ++i) if (voices_[i].stamp < voices_[oldest].stamp) oldest = i;
    keyOff(voices_[oldest]);
    voices_[oldest].active = false;
    return oldest;
}

void OplDriver::keyOff(Voice& v) {
    keyOnByte_[v.chip] &= static_cast<uint8_t>(~0x20);
    opl_.writeReg(static_cast<uint16_t>(0xB0 + chReg(v.chip)), keyOnByte_[v.chip]);
}

void OplDriver::programVoice(Voice& v) {
    const GenMidiVoice& d = *v.data;
    uint16_t mod = opReg(v.chip, 0), car = opReg(v.chip, 1);
    opl_.writeReg(static_cast<uint16_t>(0x20 + mod), d.modulator.tremVibSusKsrMult);
    opl_.writeReg(static_cast<uint16_t>(0x60 + mod), d.modulator.attackDecay);
    opl_.writeReg(static_cast<uint16_t>(0x80 + mod), d.modulator.sustainRelease);
    opl_.writeReg(static_cast<uint16_t>(0xE0 + mod), static_cast<uint8_t>(d.modulator.waveform & 7));
    opl_.writeReg(static_cast<uint16_t>(0x20 + car), d.carrier.tremVibSusKsrMult);
    opl_.writeReg(static_cast<uint16_t>(0x60 + car), d.carrier.attackDecay);
    opl_.writeReg(static_cast<uint16_t>(0x80 + car), d.carrier.sustainRelease);
    opl_.writeReg(static_cast<uint16_t>(0xE0 + car), static_cast<uint8_t>(d.carrier.waveform & 7));
    // Feedback/connection plus stereo enables from the channel pan.
    const Channel& ch = channels_[v.channel];
    uint8_t pan = 0x30;
    if (ch.pan < 43) pan = 0x10; else if (ch.pan > 85) pan = 0x20;
    opl_.writeReg(static_cast<uint16_t>(0xC0 + chReg(v.chip)), static_cast<uint8_t>((d.feedbackConnection & 0x0F) | pan));
    updateVolume(v);
}

void OplDriver::updateVolume(const Voice& v) {
    const GenMidiVoice& d = *v.data;
    const Channel& ch = channels_[v.channel];
    // Perceptual volume -> attenuation in TL units (0.75 dB each).
    float vol = (static_cast<float>(v.velocity) / 127.f) * (static_cast<float>(ch.volume) / 127.f) * (static_cast<float>(ch.expression) / 127.f);
    vol = std::clamp(vol, 0.f, 1.f);
    float attenDb = vol > 0.0005f ? -20.f * std::log10(vol) : 96.f;
    int add = static_cast<int>(attenDb / 0.75f + 0.5f);
    auto level = [&](const GenMidiOperator& op, bool scale) {
        int tl = op.outputLevel & 0x3F;
        if (scale) tl = std::min(63, tl + add);
        return static_cast<uint8_t>((op.keyScale & 0xC0) | tl);
    };
    bool additive = (d.feedbackConnection & 1) != 0;   // both operators reach the output
    opl_.writeReg(static_cast<uint16_t>(0x40 + opReg(v.chip, 0)), level(d.modulator, additive));
    opl_.writeReg(static_cast<uint16_t>(0x40 + opReg(v.chip, 1)), level(d.carrier, true));
}

void OplDriver::updateFrequency(const Voice& v) {
    const Channel& ch = channels_[v.channel];
    float bend = (static_cast<float>(ch.bend - 8192) / 8192.f) * 2.f;   // semitones
    float note = static_cast<float>(v.note + v.noteOffset) + bend + v.detune;
    float freq = 440.f * std::pow(2.f, (note - 69.f) / 12.f);
    // F-Number / block: fnum = freq * 2^(20 - block) / chipRate, keep fnum < 1024.
    int block = 0;
    double fnum = freq * (1 << 20) / kChipRate;
    while (fnum >= 1024.0 && block < 7) { fnum /= 2.0; ++block; }
    int f = static_cast<int>(std::clamp(fnum, 0.0, 1023.0));
    uint16_t base = chReg(v.chip);
    opl_.writeReg(static_cast<uint16_t>(0xA0 + base), static_cast<uint8_t>(f & 0xFF));
    keyOnByte_[v.chip] = static_cast<uint8_t>((keyOnByte_[v.chip] & 0x20) | (block << 2) | (f >> 8));
    opl_.writeReg(static_cast<uint16_t>(0xB0 + base), keyOnByte_[v.chip]);
}

void OplDriver::noteOn(int channel, int note, int velocity) {
    if (!bank_ || channel < 0 || channel > 15) return;
    if (velocity == 0) { noteOff(channel, note); return; }
    const GenMidiInstrument* instr = (channel == 9) ? bank_->percussion(note) : bank_->melodic(channels_[channel].program);
    if (!instr) return;
    int voicesNeeded = instr->doubleVoice() ? 2 : 1;
    for (int k = 0; k < voicesNeeded; ++k) {
        int i = allocVoice();
        Voice& v = voices_[i];
        v.active = true;
        v.channel = channel;
        v.note = note;
        v.triggerNote = note;
        v.velocity = velocity;
        v.stamp = ++stamp_;
        v.data = &instr->voice[k];
        v.secondary = (k == 1);
        v.held = false;
        v.noteOffset = v.data->baseNoteOffset;
        if (instr->fixedPitch()) { v.note = instr->fixedNote; }
        v.detune = (k == 1) ? (static_cast<float>(instr->fineTune) - 128.f) / 64.f : 0.f;
        programVoice(v);
        keyOnByte_[v.chip] &= static_cast<uint8_t>(~0x20);
        updateFrequency(v);
        keyOnByte_[v.chip] |= 0x20;
        opl_.writeReg(static_cast<uint16_t>(0xB0 + chReg(v.chip)), keyOnByte_[v.chip]);
    }
}

void OplDriver::noteOff(int channel, int note) {
    for (Voice& v : voices_) {
        if (!v.active || v.channel != channel || v.triggerNote != note) continue;
        if (channels_[channel].sustain) { v.held = true; continue; }
        keyOff(v);
        v.active = false;
    }
}

void OplDriver::programChange(int channel, int program) {
    if (channel < 0 || channel > 15) return;
    channels_[channel].program = std::clamp(program, 0, 127);
}

void OplDriver::controller(int channel, int cc, int value) {
    if (channel < 0 || channel > 15) return;
    Channel& ch = channels_[channel];
    switch (cc) {
    case 7: ch.volume = value; break;
    case 11: ch.expression = value; break;
    case 10: ch.pan = value; break;
    case 64:
        ch.sustain = value >= 64;
        if (!ch.sustain)
            for (Voice& v : voices_)
                if (v.active && v.channel == channel && v.held) { keyOff(v); v.active = false; }
        return;
    case 120: case 123:
        for (Voice& v : voices_) if (v.active && v.channel == channel) { keyOff(v); v.active = false; }
        return;
    case 121:
        ch.volume = 100; ch.expression = 127; ch.pan = 64; ch.bend = 8192; ch.sustain = false;
        break;
    default:
        return;
    }
    for (Voice& v : voices_)
        if (v.active && v.channel == channel) { if (cc == 10) programVoice(v); else updateVolume(v); }
}

void OplDriver::pitchBend(int channel, int value14) {
    if (channel < 0 || channel > 15) return;
    channels_[channel].bend = std::clamp(value14, 0, 16383);
    for (Voice& v : voices_)
        if (v.active && v.channel == channel) updateFrequency(v);
}

void OplDriver::allNotesOff() {
    for (Voice& v : voices_) if (v.active) { keyOff(v); v.active = false; }
}

}  // namespace rl::audio
