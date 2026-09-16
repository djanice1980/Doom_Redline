#include "core/inflate.h"

#include <cstring>

namespace rl {

namespace {

constexpr int kMaxBits = 15;
constexpr int kMaxLCodes = 286, kMaxDCodes = 30, kMaxCodes = kMaxLCodes + kMaxDCodes, kFixLCodes = 288;

struct Huffman {
    short count[kMaxBits + 1];
    short symbol[kFixLCodes];
};

struct State {
    const uint8_t* in;
    size_t inLen, inPos = 0;
    std::vector<uint8_t>& out;
    uint32_t bitBuf = 0;
    int bitCnt = 0;
    bool bad = false;

    int bits(int need) {
        uint32_t val = bitBuf;
        while (bitCnt < need) {
            if (inPos >= inLen) { bad = true; return 0; }
            val |= static_cast<uint32_t>(in[inPos++]) << bitCnt;
            bitCnt += 8;
        }
        bitBuf = val >> need;
        bitCnt -= need;
        return static_cast<int>(val & ((1u << need) - 1));
    }

    int decode(const Huffman& h) {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len <= kMaxBits; ++len) {
            code |= bits(1);
            if (bad) return -1;
            int count = h.count[len];
            if (code - count < first) return h.symbol[index + (code - first)];
            index += count;
            first += count;
            first <<= 1;
            code <<= 1;
        }
        return -1;
    }
};

// Builds a canonical Huffman table; returns 0 for a complete code, >0 incomplete, <0 over-subscribed.
int construct(Huffman& h, const short* length, int n) {
    for (int len = 0; len <= kMaxBits; ++len) h.count[len] = 0;
    for (int sym = 0; sym < n; ++sym) h.count[length[sym]]++;
    if (h.count[0] == n) return 0;
    int left = 1;
    for (int len = 1; len <= kMaxBits; ++len) {
        left <<= 1;
        left -= h.count[len];
        if (left < 0) return left;
    }
    short offs[kMaxBits + 1];
    offs[1] = 0;
    for (int len = 1; len < kMaxBits; ++len) offs[len + 1] = static_cast<short>(offs[len] + h.count[len]);
    for (int sym = 0; sym < n; ++sym)
        if (length[sym] != 0) h.symbol[offs[length[sym]]++] = static_cast<short>(sym);
    return left;
}

const short kLenBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const short kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const short kDistBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
const short kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

bool codes(State& s, const Huffman& lencode, const Huffman& distcode) {
    for (;;) {
        int symbol = s.decode(lencode);
        if (symbol < 0) return false;
        if (symbol < 256) { s.out.push_back(static_cast<uint8_t>(symbol)); continue; }
        if (symbol == 256) return true;
        symbol -= 257;
        if (symbol >= 29) return false;
        int len = kLenBase[symbol] + s.bits(kLenExtra[symbol]);
        int dsym = s.decode(distcode);
        if (dsym < 0 || dsym >= 30) return false;
        size_t dist = static_cast<size_t>(kDistBase[dsym]) + static_cast<size_t>(s.bits(kDistExtra[dsym]));
        if (s.bad || dist > s.out.size()) return false;
        size_t from = s.out.size() - dist;
        for (int i = 0; i < len; ++i) s.out.push_back(s.out[from + static_cast<size_t>(i)]);
    }
}

bool stored(State& s) {
    s.bitBuf = 0;
    s.bitCnt = 0;
    if (s.inPos + 4 > s.inLen) return false;
    unsigned len = s.in[s.inPos] | (s.in[s.inPos + 1] << 8);
    unsigned nlen = s.in[s.inPos + 2] | (s.in[s.inPos + 3] << 8);
    s.inPos += 4;
    if (len != (~nlen & 0xFFFFu)) return false;
    if (s.inPos + len > s.inLen) return false;
    s.out.insert(s.out.end(), s.in + s.inPos, s.in + s.inPos + len);
    s.inPos += len;
    return true;
}

bool fixed(State& s) {
    static Huffman lencode, distcode;
    static bool built = false;
    if (!built) {
        short lengths[kFixLCodes];
        int sym = 0;
        for (; sym < 144; ++sym) lengths[sym] = 8;
        for (; sym < 256; ++sym) lengths[sym] = 9;
        for (; sym < 280; ++sym) lengths[sym] = 7;
        for (; sym < kFixLCodes; ++sym) lengths[sym] = 8;
        construct(lencode, lengths, kFixLCodes);
        for (sym = 0; sym < kMaxDCodes; ++sym) lengths[sym] = 5;
        construct(distcode, lengths, kMaxDCodes);
        built = true;
    }
    return codes(s, lencode, distcode);
}

bool dynamic(State& s) {
    static const short order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    int nlen = s.bits(5) + 257, ndist = s.bits(5) + 1, ncode = s.bits(4) + 4;
    if (s.bad || nlen > kMaxLCodes || ndist > kMaxDCodes) return false;
    short lengths[kMaxCodes];
    int index = 0;
    for (; index < ncode; ++index) lengths[order[index]] = static_cast<short>(s.bits(3));
    for (; index < 19; ++index) lengths[order[index]] = 0;
    Huffman lencode, distcode;
    if (construct(lencode, lengths, 19) != 0) return false;
    index = 0;
    while (index < nlen + ndist) {
        int symbol = s.decode(lencode);
        if (symbol < 0) return false;
        if (symbol < 16) { lengths[index++] = static_cast<short>(symbol); continue; }
        int len = 0, rep;
        if (symbol == 16) {
            if (index == 0) return false;
            len = lengths[index - 1];
            rep = 3 + s.bits(2);
        } else if (symbol == 17) rep = 3 + s.bits(3);
        else rep = 11 + s.bits(7);
        if (index + rep > nlen + ndist) return false;
        while (rep--) lengths[index++] = static_cast<short>(len);
    }
    if (lengths[256] == 0) return false;
    int err = construct(lencode, lengths, nlen);
    if (err < 0 || (err > 0 && nlen - lencode.count[0] != 1)) return false;
    err = construct(distcode, lengths + nlen, ndist);
    if (err < 0 || (err > 0 && ndist - distcode.count[0] != 1)) return false;
    return codes(s, lencode, distcode);
}

}  // namespace

bool inflateZlib(const uint8_t* data, size_t len, std::vector<uint8_t>& out, std::string* err, bool raw) {
    auto fail = [&](const char* m) { if (err) *err = m; return false; };
    size_t start = 0;
    if (!raw) {
        if (len < 2) return fail("short zlib stream");
        if ((data[0] & 0x0F) != 8 || ((data[0] << 8) | data[1]) % 31 != 0) return fail("not a zlib stream");
        if (data[1] & 0x20) return fail("preset dictionary");
        start = 2;
    }
    State s{data + start, len - start, 0, out};
    int last;
    do {
        last = s.bits(1);
        int type = s.bits(2);
        if (s.bad) return fail("truncated");
        bool ok = type == 0 ? stored(s) : type == 1 ? fixed(s) : type == 2 ? dynamic(s) : false;
        if (!ok || s.bad) return fail("corrupt deflate data");
    } while (!last);
    return true;
}

}  // namespace rl
