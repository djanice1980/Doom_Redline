#include "audio/oggstream.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#ifdef REDLINE_HAVE_VORBIS
#include <vorbis/vorbisfile.h>
#endif

namespace rl::audio {

struct OggStream::Impl {
#ifdef REDLINE_HAVE_VORBIS
    OggVorbis_File vf{};
    bool opened = false;
#endif
};

#ifdef REDLINE_HAVE_VORBIS
// libvorbisfile callbacks restricted to a byte range of a FILE*. The
// callbacks need the range and the FILE*; keep them in a plain struct whose
// address is the datasource.
struct RangeSource {
    std::FILE* f;
    uint64_t begin, end, pos;
};
static size_t rsRead(void* ptr, size_t size, size_t nmemb, void* ds) {
    auto* r = static_cast<RangeSource*>(ds);
    uint64_t want = static_cast<uint64_t>(size) * nmemb;
    uint64_t avail = r->pos < r->end ? r->end - r->pos : 0;
    size_t n = static_cast<size_t>(std::min<uint64_t>(want, avail));
    if (n == 0) return 0;
    if (std::fseek(r->f, static_cast<long>(r->pos), SEEK_SET) != 0) return 0;
    size_t got = std::fread(ptr, 1, n, r->f);
    r->pos += got;
    return size ? got / size : 0;
}
static int rsSeek(void* ds, ogg_int64_t offset, int whence) {
    auto* r = static_cast<RangeSource*>(ds);
    int64_t target;
    if (whence == SEEK_SET) target = static_cast<int64_t>(r->begin) + offset;
    else if (whence == SEEK_CUR) target = static_cast<int64_t>(r->pos) + offset;
    else target = static_cast<int64_t>(r->end) + offset;
    if (target < static_cast<int64_t>(r->begin) || target > static_cast<int64_t>(r->end)) return -1;
    r->pos = static_cast<uint64_t>(target);
    return 0;
}
static int rsClose(void*) { return 0; }
static long rsTell(void* ds) {
    auto* r = static_cast<RangeSource*>(ds);
    return static_cast<long>(r->pos - r->begin);
}
#endif

OggStream::~OggStream() {
#ifdef REDLINE_HAVE_VORBIS
    if (impl_) {
        if (impl_->opened) ov_clear(&impl_->vf);
        delete static_cast<RangeSource*>(impl_->vf.datasource);
    }
#endif
    delete impl_;
    if (file_) std::fclose(file_);
}

std::unique_ptr<OggStream> OggStream::open(const std::string& path, uint64_t offset, uint64_t size, uint32_t outputRate) {
#ifdef REDLINE_HAVE_VORBIS
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return nullptr;
    if (size == 0) {
        std::fseek(f, 0, SEEK_END);
        long len = std::ftell(f);
        size = len > 0 ? static_cast<uint64_t>(len) - offset : 0;
    }
    auto s = std::unique_ptr<OggStream>(new OggStream());
    s->file_ = f;
    s->begin_ = offset;
    s->end_ = offset + size;
    s->outRate_ = outputRate;
    s->impl_ = new Impl();
    auto* range = new RangeSource{f, offset, offset + size, offset};
    ov_callbacks cb{rsRead, rsSeek, rsClose, rsTell};
    if (ov_open_callbacks(range, &s->impl_->vf, nullptr, 0, cb) < 0) {
        delete range;
        return nullptr;
    }
    s->impl_->opened = true;
    vorbis_info* info = ov_info(&s->impl_->vf, -1);
    if (!info) return nullptr;
    s->srcRate_ = static_cast<uint32_t>(info->rate);
    s->channels_ = info->channels;
    s->lengthSeconds_ = ov_time_total(&s->impl_->vf, -1);
    return s;
#else
    (void)path; (void)offset; (void)size; (void)outputRate;
    return nullptr;
#endif
}

bool OggStream::fillSource() {
#ifdef REDLINE_HAVE_VORBIS
    float** pcm = nullptr;
    int bitstream = 0;
    long got = ov_read_float(&impl_->vf, &pcm, 2048, &bitstream);
    if (got <= 0) { eof_ = true; return false; }
    // Keep the last frame for interpolation continuity.
    src_.assign(static_cast<size_t>(got + 1) * 2, 0.f);
    src_[0] = last_[0];
    src_[1] = last_[1];
    for (long i = 0; i < got; ++i) {
        float l = pcm[0][i];
        float r = channels_ > 1 ? pcm[1][i] : l;
        src_[static_cast<size_t>(i + 1) * 2] = l;
        src_[static_cast<size_t>(i + 1) * 2 + 1] = r;
    }
    last_[0] = src_[static_cast<size_t>(got) * 2];
    last_[1] = src_[static_cast<size_t>(got) * 2 + 1];
    srcFrames_ = static_cast<size_t>(got + 1);
    return true;
#else
    return false;
#endif
}

int OggStream::read(float* out, int frames, float gain) {
    if (!impl_ || eof_) return 0;
    const double step = static_cast<double>(srcRate_) / static_cast<double>(outRate_);
    int produced = 0;
    while (produced < frames) {
        // Need the two source frames around srcPos_; refill when the cursor
        // runs past the block. Index 0 of a new block is the previous block's
        // last frame, so the cursor carries over as (pos - (frames - 1)).
        while (srcFrames_ < 2 || srcPos_ + 1.0 >= static_cast<double>(srcFrames_)) {
            double carry = srcFrames_ >= 1 ? srcPos_ - static_cast<double>(srcFrames_ - 1) : 0.0;
            if (!fillSource()) return produced;
            srcPos_ = std::max(0.0, carry);
        }
        size_t i = static_cast<size_t>(srcPos_);
        float t = static_cast<float>(srcPos_ - static_cast<double>(i));
        const float* a = &src_[i * 2];
        const float* b = &src_[(i + 1) * 2];
        out[static_cast<size_t>(produced) * 2] += (a[0] + (b[0] - a[0]) * t) * gain;
        out[static_cast<size_t>(produced) * 2 + 1] += (a[1] + (b[1] - a[1]) * t) * gain;
        srcPos_ += step;
        ++produced;
    }
    return produced;
}

void OggStream::rewind() {
#ifdef REDLINE_HAVE_VORBIS
    if (impl_ && impl_->opened) ov_pcm_seek(&impl_->vf, 0);
#endif
    eof_ = false;
    src_.clear();
    srcFrames_ = 0;
    srcPos_ = 0.0;
    last_[0] = last_[1] = 0.f;
}

}  // namespace rl::audio
