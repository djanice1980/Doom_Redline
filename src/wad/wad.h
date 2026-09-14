#pragma once
// rl::wad — a small, dependency-free reader for classic Doom / Doom II /
// Freedoom WAD files (IWAD and PWAD).
//
// Design notes
//   * The whole file is read into memory once; every lump is a span into it.
//   * All multi-byte fields are little-endian and are assembled byte-by-byte
//     (no reinterpret_cast punning, host-endian independent).
//   * Nothing throws for control flow. Malformed or truncated input yields
//     std::nullopt / an empty span / a skipped entry, never a crash.
//   * Lump names are stored uppercase, cut at the first NUL, max 8 chars, and
//     all lookups are case-insensitive. Where several lumps share a name the
//     LAST one wins (later lumps override earlier ones, as in the engine).
//   * Decoded pixels use the shared rl::Image type (RGBA8, top row first).

#include "core/image.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace rl::wad {

// ---------------------------------------------------------------------------
// Names
// ---------------------------------------------------------------------------

// Canonical lump-name form: uppercase, truncated at the first NUL, at most 8
// characters. Used for everything stored in and looked up from a Wad.
std::string normalizeName(std::string_view raw);

// Case-insensitive comparison of two names (after NUL truncation).
bool nameEquals(std::string_view a, std::string_view b);

// ---------------------------------------------------------------------------
// Container
// ---------------------------------------------------------------------------

struct Lump {
    std::string name;     // canonical (see normalizeName)
    uint32_t offset = 0;  // byte offset of the lump data in the file
    uint32_t size = 0;    // byte size (clamped so offset+size <= file size)
};

class Wad {
public:
    // Reads the whole file into memory. Accepts "IWAD" and "PWAD" headers.
    static std::optional<Wad> load(const std::filesystem::path& path);
    // Same, from a byte buffer already in memory (the Wad takes ownership).
    static std::optional<Wad> fromMemory(std::vector<uint8_t> bytes);

    // Last occurrence wins; case-insensitive; nullptr if absent.
    const Lump* find(std::string_view name) const;
    // Index into lumps() of the same lump find() would return.
    std::optional<size_t> findIndex(std::string_view name) const;

    // Bytes of a lump. Empty span if the lump is missing or lies outside the file.
    std::span<const uint8_t> data(const Lump& lump) const;
    std::span<const uint8_t> data(std::string_view name) const;

    const std::vector<Lump>& lumps() const { return lumps_; }

    // Lumps strictly between a start marker and the matching end marker, in
    // file order, e.g. between("S_START", "S_END"). If the markers occur more
    // than once every such region is included. A start marker with no end
    // marker collects everything up to the end of the directory.
    std::vector<const Lump*> between(std::string_view startMarker, std::string_view endMarker) const;

    bool isIwad() const { return iwad_; }
    size_t fileSize() const { return bytes_.size(); }

private:
    std::vector<uint8_t> bytes_;
    std::vector<Lump> lumps_;
    bool iwad_ = false;
};

// ---------------------------------------------------------------------------
// Palette
// ---------------------------------------------------------------------------

struct Palette {
    uint8_t rgb[256][3];
};

// PLAYPAL holds 14 palettes of 768 bytes; index 0 is the normal one.
// Returns nullopt if PLAYPAL is missing or too short for the requested index.
std::optional<Palette> loadPalette(const Wad& wad, int index = 0);

// ---------------------------------------------------------------------------
// Graphics
// ---------------------------------------------------------------------------

// Doom "patch" picture format (sprites, wall patches, menu/HUD graphics, fonts).
// Sets Image::offsetX/offsetY from the header's leftoffset/topoffset.
// Pixels not covered by any post are fully transparent (alpha 0).
// Also understands the "tall patch" extension (topdelta accumulation) used by
// some modern PWADs; vanilla patches are unaffected.
std::optional<rl::Image> decodePatch(std::span<const uint8_t> lump, const Palette& pal);

// Flats: raw 64x64 palette indices (the first 4096 bytes are used).
std::optional<rl::Image> decodeFlat(std::span<const uint8_t> lump, const Palette& pal);

// Any named patch-format lump: find() + decodePatch().
std::optional<rl::Image> loadPatch(const Wad& wad, const Palette& pal, std::string_view lumpName);

// Font glyphs: <prefix>NNN where NNN is the decimal ASCII code (STCFN065 = 'A').
// Missing glyphs are simply absent from the map.
std::map<char, rl::Image> loadFont(const Wad& wad, const Palette& pal, std::string_view prefix = "STCFN");

// ---------------------------------------------------------------------------
// Sprites
// ---------------------------------------------------------------------------

struct SpriteFrameView {
    const rl::Image* image = nullptr;  // owned by the SpriteSet
    bool mirrored = false;             // draw flipped horizontally
};

// All frames/rotations of one 4-character sprite (e.g. "TROO").
// Lump names are NNNNFR or NNNNFRFR: frame letter A-Z, rotation digit 0-8.
// Rotation 0 = one image for all angles. The optional second FR pair reuses
// the same image mirrored (TROOA2A8: rotation 2 as-is, rotation 8 mirrored).
// Rotations 1..8, 1 = facing the viewer, increasing clockwise.
class SpriteSet {
public:
    // Loads every matching lump between S_START/S_END and SS_START/SS_END.
    // nullopt if no frame of that name exists.
    static std::optional<SpriteSet> load(const Wad& wad, const Palette& pal, std::string_view spriteName);

    // frame 'A'..'Z' (case-insensitive), rotation 1..8 (0 is treated as 1).
    // Returns the rotation-0 image when the frame has one; otherwise the exact
    // rotation; otherwise rotation 1; otherwise any rotation that exists.
    // nullopt if the frame does not exist. The returned pointer stays valid
    // for the lifetime of this SpriteSet (moves included).
    std::optional<SpriteFrameView> get(char frame, int rotation = 1) const;

    std::vector<char> frames() const;  // sorted frame letters present
    const std::string& name() const { return name_; }
    size_t imageCount() const { return images_.size(); }

private:
    struct Slot {
        int image = -1;  // index into images_, -1 = not present
        bool mirrored = false;
    };
    struct Frame {
        Slot rot[9];  // rot[0] = all angles, rot[1..8] = specific
    };

    void install(char frame, int rotation, int imageIndex, bool mirrored);

    std::string name_;
    std::vector<rl::Image> images_;
    std::map<char, Frame> frames_;
};

// Unique 4-character sprite names between S_START/S_END and SS_START/SS_END,
// in first-seen order.
std::vector<std::string> spriteNames(const Wad& wad);

// ---------------------------------------------------------------------------
// Wall textures
// ---------------------------------------------------------------------------

struct TextureInfo {
    std::string name;
    int width = 0;
    int height = 0;
};

class TextureSet {
public:
    // Parses PNAMES, TEXTURE1 and TEXTURE2 (if present) and decodes every
    // patch PNAMES refers to (patches are looked up by name, case-insensitive;
    // missing patches are silently skipped). nullopt if PNAMES or TEXTURE1 is
    // missing/unparseable. Later definitions override earlier ones by name.
    static std::optional<TextureSet> load(const Wad& wad, const Palette& pal);

    // Composes the texture on demand: each patch is drawn at its origin with
    // clipping; later patches overwrite earlier ones where they are opaque.
    std::optional<rl::Image> get(std::string_view textureName) const;
    std::optional<TextureInfo> info(std::string_view textureName) const;

    std::vector<std::string> names() const;  // definition order
    size_t size() const { return defs_.size(); }

private:
    struct PatchRef {
        int originX = 0;
        int originY = 0;
        int patch = -1;  // index into PNAMES
    };
    struct Def {
        TextureInfo info;
        std::vector<PatchRef> patches;
    };
    friend std::vector<TextureInfo> listTextures(const Wad&);
    static void parseTextureLump(std::span<const uint8_t> lump, std::vector<Def>& out);

    std::vector<Def> defs_;
    std::map<std::string, size_t> index_;  // name -> defs_ index (last wins)
    std::vector<std::optional<rl::Image>> patches_;  // decoded, indexed like PNAMES
};

// Texture names/sizes from TEXTURE1/TEXTURE2 without decoding any patches.
std::vector<TextureInfo> listTextures(const Wad& wad);

// ---------------------------------------------------------------------------
// Sound
// ---------------------------------------------------------------------------

struct Sound {
    int sampleRate = 11025;
    std::vector<float> samples;  // mono, [-1, 1]
};

// DMX format ("DS*" lumps): uint16 format (=3), uint16 sampleRate,
// uint32 numSamples, then unsigned 8-bit PCM. Vanilla lumps carry 16 padding
// bytes before and after the samples (copies of the first/last sample) that
// are counted in numSamples; those are detected and stripped. The sample
// count is clamped to what the lump actually contains.
std::optional<Sound> loadSound(const Wad& wad, std::string_view lumpName);

// ---------------------------------------------------------------------------
// Inventory helpers / diagnostics
// ---------------------------------------------------------------------------

// Flat lumps between F_START/F_END and FF_START/FF_END that are at least 4096 bytes.
std::vector<const Lump*> flatLumps(const Wad& wad);
// Lumps whose name starts with "DS" and that are large enough to hold a DMX header.
std::vector<const Lump*> soundLumps(const Wad& wad);
// Lumps named <prefix>NNN.
std::vector<const Lump*> fontLumps(const Wad& wad, std::string_view prefix = "STCFN");

// One-line summary, e.g.
// "IWAD, 2306 lumps, 138 sprites, 428 textures, 107 flats, 63 font glyphs, 108 sounds"
std::string describe(const Wad& wad);

}  // namespace rl::wad
