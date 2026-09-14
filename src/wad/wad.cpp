// WAD container: header/directory parsing, lump lookup, palette, and the
// inventory helpers used by describe() and the wadinfo tool.
#include "wad/wad.h"

#include "wad/bytes.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <sstream>

namespace rl::wad {

using detail::inRange;
using detail::rdI32;

// ---------------------------------------------------------------------------
// Names
// ---------------------------------------------------------------------------

static char upperAscii(char c) {
    return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
}

std::string normalizeName(std::string_view raw) {
    std::string out;
    out.reserve(8);
    for (size_t i = 0; i < raw.size() && i < 8; ++i) {
        if (raw[i] == '\0') break;
        out.push_back(upperAscii(raw[i]));
    }
    return out;
}

bool nameEquals(std::string_view a, std::string_view b) {
    // Cut both at the first NUL, then compare case-insensitively.
    a = a.substr(0, a.find('\0'));
    b = b.substr(0, b.find('\0'));
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (upperAscii(a[i]) != upperAscii(b[i])) return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Wad
// ---------------------------------------------------------------------------

std::optional<Wad> Wad::load(const std::filesystem::path& path) {
    // Plain stdio keeps this free of iostream locale overhead and works for
    // paths with spaces. Read the whole file in one go.
    std::FILE* f = std::fopen(path.string().c_str(), "rb");
    if (!f) return std::nullopt;
    std::vector<uint8_t> bytes;
    uint8_t chunk[1 << 16];
    for (;;) {
        size_t n = std::fread(chunk, 1, sizeof chunk, f);
        if (n == 0) break;
        bytes.insert(bytes.end(), chunk, chunk + n);
    }
    bool readError = std::ferror(f) != 0;
    std::fclose(f);
    if (readError) return std::nullopt;
    return fromMemory(std::move(bytes));
}

std::optional<Wad> Wad::fromMemory(std::vector<uint8_t> bytes) {
    // Header: char id[4] ("IWAD"/"PWAD"), int32 numlumps, int32 infotableofs.
    // Directory entry: int32 filepos, int32 size, char name[8].
    if (bytes.size() < 12) return std::nullopt;
    std::span<const uint8_t> b(bytes);

    Wad wad;
    if (b[0] == 'I' && b[1] == 'W' && b[2] == 'A' && b[3] == 'D') {
        wad.iwad_ = true;
    } else if (b[0] == 'P' && b[1] == 'W' && b[2] == 'A' && b[3] == 'D') {
        wad.iwad_ = false;
    } else {
        return std::nullopt;
    }

    int32_t numLumps = rdI32(b, 4);
    int32_t dirOffset = rdI32(b, 8);
    if (numLumps < 0 || dirOffset < 0) return std::nullopt;
    if (static_cast<size_t>(dirOffset) > b.size()) return std::nullopt;

    // A directory that claims more entries than fit is truncated to what fits
    // rather than rejected; some tools write slightly inconsistent counts.
    size_t maxEntries = (b.size() - static_cast<size_t>(dirOffset)) / 16;
    size_t count = std::min(static_cast<size_t>(numLumps), maxEntries);

    wad.lumps_.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        size_t pos = static_cast<size_t>(dirOffset) + i * 16;
        int32_t filePos = rdI32(b, pos);
        int32_t size = rdI32(b, pos + 4);
        Lump lump;
        char rawName[8];
        for (size_t k = 0; k < 8; ++k) rawName[k] = static_cast<char>(b[pos + 8 + k]);
        lump.name = normalizeName(std::string_view(rawName, 8));
        if (filePos < 0 || size < 0 || static_cast<size_t>(filePos) > b.size()) {
            // Nonsense entry: keep the name (it may be a marker) but no data.
            lump.offset = 0;
            lump.size = 0;
        } else {
            lump.offset = static_cast<uint32_t>(filePos);
            size_t avail = b.size() - static_cast<size_t>(filePos);
            lump.size = static_cast<uint32_t>(std::min(static_cast<size_t>(size), avail));
        }
        wad.lumps_.push_back(std::move(lump));
    }

    wad.bytes_ = std::move(bytes);
    return wad;
}

std::optional<size_t> Wad::findIndex(std::string_view name) const {
    // A request longer than 8 characters can never name a lump; refuse it
    // rather than silently truncating it to a name that does exist.
    if (name.substr(0, name.find('\0')).size() > 8) return std::nullopt;
    std::string key = normalizeName(name);
    for (size_t i = lumps_.size(); i-- > 0;) {
        if (lumps_[i].name == key) return i;
    }
    return std::nullopt;
}

const Lump* Wad::find(std::string_view name) const {
    auto idx = findIndex(name);
    return idx ? &lumps_[*idx] : nullptr;
}

std::span<const uint8_t> Wad::data(const Lump& lump) const {
    std::span<const uint8_t> b(bytes_);
    if (!inRange(b, lump.offset, lump.size)) return {};
    return b.subspan(lump.offset, lump.size);
}

std::span<const uint8_t> Wad::data(std::string_view name) const {
    const Lump* l = find(name);
    return l ? data(*l) : std::span<const uint8_t>{};
}

std::vector<const Lump*> Wad::between(std::string_view startMarker, std::string_view endMarker) const {
    std::vector<const Lump*> out;
    bool inside = false;
    for (const Lump& l : lumps_) {
        if (!inside) {
            if (nameEquals(l.name, startMarker)) inside = true;
        } else if (nameEquals(l.name, endMarker)) {
            inside = false;
        } else {
            out.push_back(&l);
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Palette
// ---------------------------------------------------------------------------

std::optional<Palette> loadPalette(const Wad& wad, int index) {
    if (index < 0) return std::nullopt;
    std::span<const uint8_t> d = wad.data("PLAYPAL");
    size_t start = static_cast<size_t>(index) * 768;
    if (!inRange(d, start, 768)) return std::nullopt;
    Palette pal;
    for (int i = 0; i < 256; ++i) {
        pal.rgb[i][0] = d[start + static_cast<size_t>(i) * 3 + 0];
        pal.rgb[i][1] = d[start + static_cast<size_t>(i) * 3 + 1];
        pal.rgb[i][2] = d[start + static_cast<size_t>(i) * 3 + 2];
    }
    return pal;
}

// ---------------------------------------------------------------------------
// Inventory helpers
// ---------------------------------------------------------------------------

std::vector<std::string> spriteNames(const Wad& wad) {
    std::vector<std::string> out;
    auto scan = [&](const std::vector<const Lump*>& lumps) {
        for (const Lump* l : lumps) {
            // NNNNFR or NNNNFRFR with a plausible frame letter and rotation digit.
            const std::string& n = l->name;
            if (n.size() != 6 && n.size() != 8) continue;
            if (n[4] < 'A' || n[4] > 'Z' || n[5] < '0' || n[5] > '8') continue;
            if (l->size == 0) continue;
            std::string base = n.substr(0, 4);
            if (std::find(out.begin(), out.end(), base) == out.end()) out.push_back(base);
        }
    };
    scan(wad.between("S_START", "S_END"));
    scan(wad.between("SS_START", "SS_END"));
    return out;
}

std::vector<const Lump*> flatLumps(const Wad& wad) {
    std::vector<const Lump*> out;
    auto scan = [&](std::string_view start, std::string_view end) {
        for (const Lump* l : wad.between(start, end)) {
            if (l->size >= 4096) out.push_back(l);
        }
    };
    scan("F_START", "F_END");
    scan("FF_START", "FF_END");
    return out;
}

std::vector<const Lump*> soundLumps(const Wad& wad) {
    std::vector<const Lump*> out;
    for (const Lump& l : wad.lumps()) {
        if (l.name.size() > 2 && l.name[0] == 'D' && l.name[1] == 'S' && l.size >= 8) out.push_back(&l);
    }
    return out;
}

std::vector<const Lump*> fontLumps(const Wad& wad, std::string_view prefix) {
    std::string p = normalizeName(prefix);
    std::vector<const Lump*> out;
    for (const Lump& l : wad.lumps()) {
        if (l.name.size() != p.size() + 3 || l.name.compare(0, p.size(), p) != 0) continue;
        bool digits = true;
        for (size_t i = p.size(); i < l.name.size(); ++i) {
            if (l.name[i] < '0' || l.name[i] > '9') digits = false;
        }
        if (digits && l.size > 0) out.push_back(&l);
    }
    return out;
}

std::string describe(const Wad& wad) {
    std::ostringstream ss;
    ss << (wad.isIwad() ? "IWAD" : "PWAD") << ", " << wad.lumps().size() << " lumps, "
       << spriteNames(wad).size() << " sprites, " << listTextures(wad).size() << " textures, "
       << flatLumps(wad).size() << " flats, " << fontLumps(wad).size() << " font glyphs, "
       << soundLumps(wad).size() << " sounds";
    return ss.str();
}

}  // namespace rl::wad
