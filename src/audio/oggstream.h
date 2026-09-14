#pragma once
// Streams an Ogg Vorbis file (or a byte range inside a larger file, such as
// a lump in extras.wad) through libvorbisfile, resampled to the output rate.
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace rl::audio {

class OggStream {
public:
    ~OggStream();
    // Opens bytes [offset, offset+size) of `path`. size 0 = to end of file.
    static std::unique_ptr<OggStream> open(const std::string& path, uint64_t offset, uint64_t size, uint32_t outputRate);

    // Fills `frames` stereo frames (interleaved) at the output rate, ADDING
    // into `out` scaled by `gain`. Returns the number of frames produced;
    // fewer than requested means the end was reached.
    int read(float* out, int frames, float gain);
    void rewind();
    bool atEnd() const { return eof_; }
    uint32_t sourceRate() const { return srcRate_; }
    double lengthSeconds() const { return lengthSeconds_; }

private:
    OggStream() = default;
    bool fillSource();   // decode the next block into src_

    struct Impl;
    Impl* impl_ = nullptr;
    std::FILE* file_ = nullptr;
    uint64_t begin_ = 0, end_ = 0;
    uint32_t srcRate_ = 44100, outRate_ = 48000;
    int channels_ = 2;
    double lengthSeconds_ = 0.0;
    bool eof_ = false;
    // Decoded source frames (stereo, interleaved) with a fractional read cursor for resampling.
    std::vector<float> src_;
    size_t srcFrames_ = 0;
    double srcPos_ = 0.0;
    float last_[2] = {0.f, 0.f};
};

}  // namespace rl::audio
