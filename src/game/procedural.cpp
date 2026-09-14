#include "game/procedural.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace rl::proc {

namespace {

constexpr float kPi = 3.14159265f;

// Small deterministic hash noise.
float hash2(int x, int y, int seed = 0) {
    uint32_t h = static_cast<uint32_t>(x) * 374761393u + static_cast<uint32_t>(y) * 668265263u + static_cast<uint32_t>(seed) * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return static_cast<float>(h & 0xFFFF) / 65535.f;
}

float smoothNoise(float x, float y, int seed) {
    int xi = static_cast<int>(std::floor(x)), yi = static_cast<int>(std::floor(y));
    float fx = x - xi, fy = y - yi;
    fx = fx * fx * (3 - 2 * fx);
    fy = fy * fy * (3 - 2 * fy);
    float a = hash2(xi, yi, seed), b = hash2(xi + 1, yi, seed), c = hash2(xi, yi + 1, seed), d = hash2(xi + 1, yi + 1, seed);
    return (a * (1 - fx) + b * fx) * (1 - fy) + (c * (1 - fx) + d * fx) * fy;
}

float fbm(float x, float y, int seed, int octaves = 4) {
    float v = 0, amp = 0.5f, f = 1.f;
    for (int i = 0; i < octaves; ++i) {
        v += smoothNoise(x * f, y * f, seed + i) * amp;
        amp *= 0.5f;
        f *= 2.f;
    }
    return v;
}

uint8_t clampByte(float v) { return static_cast<uint8_t>(std::clamp(v, 0.f, 255.f)); }

void circle(Image& img, float cx, float cy, float r, uint8_t R, uint8_t G, uint8_t B, uint8_t A = 255) {
    for (int y = 0; y < img.height; ++y)
        for (int x = 0; x < img.width; ++x) {
            float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
            if (dx * dx + dy * dy <= r * r) img.set(x, y, R, G, B, A);
        }
}

void rect(Image& img, int x0, int y0, int x1, int y1, uint8_t R, uint8_t G, uint8_t B, uint8_t A = 255) {
    for (int y = std::max(0, y0); y < std::min(img.height, y1); ++y)
        for (int x = std::max(0, x0); x < std::min(img.width, x1); ++x) img.set(x, y, R, G, B, A);
}

}  // namespace

// ---------------------------------------------------------------------------
Image blockTexture(int size) {
    Image img(size, size);
    int bevel = std::max(2, size / 8);
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            int edge = std::min({x, y, size - 1 - x, size - 1 - y});
            float shade = 0.82f;
            if (edge < bevel) {
                bool top = (y < bevel && y <= x && y <= size - 1 - x);
                bool left = (x < bevel && x < y && x < size - 1 - y);
                bool bottom = (size - 1 - y < bevel && size - 1 - y < x && size - 1 - y <= size - 1 - x);
                if (top) shade = 1.0f;
                else if (left) shade = 0.92f;
                else if (bottom) shade = 0.45f;
                else shade = 0.55f;
            } else {
                shade += 0.06f * (fbm(x * 0.25f, y * 0.25f, 3) - 0.5f);
            }
            uint8_t v = clampByte(255.f * shade);
            img.set(x, y, v, v, v);
        }
    return img;
}

Image redBlockTexture(int size) {
    Image img = blockTexture(size);
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            uint8_t* p = img.px(x, y);
            float n = fbm(x * 0.35f, y * 0.35f, 11, 5);
            float crack = std::fabs(n - 0.5f) < 0.025f ? 1.f : 0.f;   // thin bright veins
            float base = p[0] / 255.f;
            float r = base * 0.95f + crack * 0.6f;
            float g = base * 0.15f + crack * 0.7f;
            float b = base * 0.10f + crack * 0.2f;
            p[0] = clampByte(r * 255); p[1] = clampByte(g * 255); p[2] = clampByte(b * 255);
        }
    return img;
}

Image wallTexture(int size) {
    Image img(size, size);
    int panel = size / 2;
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            int px = x % panel, py = y % panel;
            float n = fbm(x * 0.12f, y * 0.12f, 21, 3);
            float v = 0.30f + 0.25f * n;
            int edge = std::min({px, py, panel - 1 - px, panel - 1 - py});
            if (edge == 0) v *= 0.45f;
            else if (edge == 1) v *= 1.35f;
            if (((x / 4) % 8 == 0) && ((y / 4) % 8 == 3)) v *= 1.6f;   // rivets
            uint8_t g = clampByte(v * 255);
            img.set(x, y, clampByte(g * 0.95f), clampByte(g * 0.85f), clampByte(g * 0.75f));
        }
    return img;
}

Image floorTexture(int size) {
    Image img(size, size);
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            float n = fbm(x * 0.08f, y * 0.08f, 31, 4);
            float v = 0.22f + 0.30f * n;
            if ((x % 16 == 0) || (y % 16 == 0)) v *= 0.55f;
            uint8_t g = clampByte(v * 255);
            img.set(x, y, clampByte(g * 0.8f), clampByte(g * 0.72f), clampByte(g * 0.7f));
        }
    return img;
}

Image skyGradient(int w, int h) {
    Image img(w, h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float t = static_cast<float>(y) / h;
            float n = fbm(x * 0.05f, y * 0.1f, 41, 3);
            img.set(x, y, clampByte(90 + 60 * t + 30 * n), clampByte(20 + 20 * t), clampByte(20 + 10 * t));
        }
    return img;
}

// ---------------------------------------------------------------------------
Image enemyFrame(int frame, int size) {
    Image img(size, size);
    float c = size * 0.5f;
    float r = size * 0.36f;
    // Death frames: collapse into a puddle.
    if (frame >= 3) {
        float t = static_cast<float>(frame - 3) / 3.f;   // 0..1
        float squash = 1.f - 0.8f * t;
        for (int y = 0; y < size; ++y)
            for (int x = 0; x < size; ++x) {
                float dx = (x + 0.5f - c) / (r * (1.f + 0.6f * t)), dy = (y + 0.5f - (size - r * squash)) / (r * squash);
                if (dx * dx + dy * dy <= 1.f) img.set(x, y, clampByte(160 - 80 * t), 20, 20);
            }
        img.offsetX = size / 2;
        img.offsetY = size;
        return img;
    }
    // Body: a dark red blob with a jagged outline.
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            float dx = x + 0.5f - c, dy = y + 0.5f - c;
            float ang = std::atan2(dy, dx);
            float spikes = 1.f + 0.12f * std::sin(ang * 9.f + frame) ;
            float d = std::sqrt(dx * dx + dy * dy) / (r * spikes);
            if (d <= 1.f) {
                float shade = 1.f - 0.5f * d;
                float n = fbm(x * 0.2f, y * 0.2f, 51 + frame, 3);
                img.set(x, y, clampByte((150 + 80 * n) * shade), clampByte(25 * shade), clampByte(25 * shade));
            }
        }
    // Eyes
    float ey = c - r * 0.25f;
    float eyeR = r * 0.16f;
    for (int s = -1; s <= 1; s += 2) {
        circle(img, c + s * r * 0.35f, ey, eyeR * 1.4f, 20, 5, 5);
        circle(img, c + s * r * 0.35f, ey, eyeR, 255, 230, 60);
        circle(img, c + s * r * 0.35f + (frame == 1 ? 0 : s), ey, eyeR * 0.4f, 0, 0, 0);
    }
    // Mouth
    if (frame == 1) {   // attack: open, glowing
        circle(img, c, c + r * 0.35f, r * 0.32f, 20, 5, 5);
        circle(img, c, c + r * 0.35f, r * 0.24f, 255, 140, 30);
        for (int i = -2; i <= 2; ++i) rect(img, static_cast<int>(c + i * r * 0.16f - 1), static_cast<int>(c + r * 0.05f), static_cast<int>(c + i * r * 0.16f + 1), static_cast<int>(c + r * 0.2f), 240, 240, 220);
    } else if (frame == 2) {   // pain: squint
        rect(img, static_cast<int>(c - r * 0.4f), static_cast<int>(c + r * 0.35f), static_cast<int>(c + r * 0.4f), static_cast<int>(c + r * 0.45f), 20, 5, 5);
    } else {
        for (int i = -3; i <= 3; ++i) {
            int tx = static_cast<int>(c + i * r * 0.14f);
            rect(img, tx - 1, static_cast<int>(c + r * 0.3f), tx + 1, static_cast<int>(c + r * 0.45f + (i % 2 ? 2 : 0)), 240, 240, 220);
        }
    }
    img.offsetX = size / 2;
    img.offsetY = size;   // origin at bottom centre like Doom monster sprites
    return img;
}

Image gunFrame(int frame, int w, int h) {
    Image img(w, h);
    int cx = w / 2;
    int recoil = frame == 2 ? 6 : (frame == 1 ? 3 : 0);
    // Barrel
    rect(img, cx - 6, 8 + recoil, cx + 6, h, 70, 70, 80);
    rect(img, cx - 4, 8 + recoil, cx - 2, h, 110, 110, 120);
    rect(img, cx - 10, 4 + recoil, cx + 10, 12 + recoil, 50, 50, 60);
    // Body / grip
    rect(img, cx - 18, h / 2 + recoil, cx + 18, h, 60, 55, 55);
    rect(img, cx - 14, h / 2 + 2 + recoil, cx - 10, h, 90, 85, 80);
    rect(img, cx + 8, h / 2 + 6 + recoil, cx + 16, h, 45, 40, 40);
    // Hand
    rect(img, cx - 30, h - 24 + recoil, cx - 14, h, 200, 150, 120);
    rect(img, cx + 14, h - 20 + recoil, cx + 30, h, 200, 150, 120);
    img.offsetX = cx;
    img.offsetY = h;
    return img;
}

Image muzzleFlash(int size) {
    Image img(size, size);
    float c = size * 0.5f;
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            float dx = x + 0.5f - c, dy = y + 0.5f - c;
            float ang = std::atan2(dy, dx);
            float rr = size * 0.45f * (0.6f + 0.4f * std::fabs(std::sin(ang * 4.f)));
            float d = std::sqrt(dx * dx + dy * dy) / rr;
            if (d <= 1.f) img.set(x, y, 255, clampByte(255 - 120 * d), clampByte(120 - 120 * d), clampByte(255 * (1 - d * 0.5f)));
        }
    img.offsetX = size / 2;
    img.offsetY = size / 2;
    return img;
}

Image explosionFrame(int frame, int size) {
    Image img(size, size);
    float c = size * 0.5f;
    float t = static_cast<float>(frame) / 4.f;
    float rr = size * (0.15f + 0.35f * t);
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            float dx = x + 0.5f - c, dy = y + 0.5f - c;
            float n = fbm(x * 0.15f + frame * 3.f, y * 0.15f, 61, 3);
            float d = std::sqrt(dx * dx + dy * dy) / (rr * (0.7f + 0.6f * n));
            if (d <= 1.f) {
                float heat = (1.f - d) * (1.f - t * 0.7f);
                img.set(x, y, clampByte(255 * std::min(1.f, heat * 2.f + 0.3f)), clampByte(255 * heat * 1.3f), clampByte(255 * heat * 0.3f),
                        clampByte(255 * std::min(1.f, (1.f - d) * 3.f)));
            }
        }
    img.offsetX = size / 2;
    img.offsetY = size / 2;
    return img;
}

Image fireball(int size) {
    Image img(size, size);
    float c = size * 0.5f;
    circle(img, c, c, c * 0.95f, 255, 90, 20, 200);
    circle(img, c, c, c * 0.6f, 255, 200, 60);
    circle(img, c, c, c * 0.3f, 255, 255, 200);
    img.offsetX = size / 2;
    img.offsetY = size / 2;
    return img;
}

Image crosshair(int size) {
    Image img(size, size);
    int c = size / 2;
    for (int i = 0; i < size; ++i) {
        if (std::abs(i - c) > 2) { img.set(i, c, 255, 255, 255); img.set(c, i, 255, 255, 255); }
    }
    img.offsetX = c;
    img.offsetY = c;
    return img;
}

Image solid(int w, int h, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    Image img(w, h);
    rect(img, 0, 0, w, h, r, g, b, a);
    return img;
}

// ---------------------------------------------------------------------------
// 5x7 font. Each glyph is 7 rows of 5 characters; '#' = pixel.
namespace {
struct Glyph { char ch; const char* rows[7]; };
const Glyph kGlyphs[] = {
    {' ', {".....", ".....", ".....", ".....", ".....", ".....", "....."}},
    {'0', {".###.", "#...#", "#..##", "#.#.#", "##..#", "#...#", ".###."}},
    {'1', {"..#..", ".##..", "..#..", "..#..", "..#..", "..#..", ".###."}},
    {'2', {".###.", "#...#", "....#", "...#.", "..#..", ".#...", "#####"}},
    {'3', {"#####", "...#.", "..#..", "...#.", "....#", "#...#", ".###."}},
    {'4', {"...#.", "..##.", ".#.#.", "#..#.", "#####", "...#.", "...#."}},
    {'5', {"#####", "#....", "####.", "....#", "....#", "#...#", ".###."}},
    {'6', {"..##.", ".#...", "#....", "####.", "#...#", "#...#", ".###."}},
    {'7', {"#####", "....#", "...#.", "..#..", ".#...", ".#...", ".#..."}},
    {'8', {".###.", "#...#", "#...#", ".###.", "#...#", "#...#", ".###."}},
    {'9', {".###.", "#...#", "#...#", ".####", "....#", "...#.", ".##.."}},
    {'A', {".###.", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"}},
    {'B', {"####.", "#...#", "#...#", "####.", "#...#", "#...#", "####."}},
    {'C', {".###.", "#...#", "#....", "#....", "#....", "#...#", ".###."}},
    {'D', {"###..", "#..#.", "#...#", "#...#", "#...#", "#..#.", "###.."}},
    {'E', {"#####", "#....", "#....", "####.", "#....", "#....", "#####"}},
    {'F', {"#####", "#....", "#....", "####.", "#....", "#....", "#...."}},
    {'G', {".###.", "#...#", "#....", "#.###", "#...#", "#...#", ".####"}},
    {'H', {"#...#", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"}},
    {'I', {".###.", "..#..", "..#..", "..#..", "..#..", "..#..", ".###."}},
    {'J', {"..###", "...#.", "...#.", "...#.", "...#.", "#..#.", ".##.."}},
    {'K', {"#...#", "#..#.", "#.#..", "##...", "#.#..", "#..#.", "#...#"}},
    {'L', {"#....", "#....", "#....", "#....", "#....", "#....", "#####"}},
    {'M', {"#...#", "##.##", "#.#.#", "#.#.#", "#...#", "#...#", "#...#"}},
    {'N', {"#...#", "#...#", "##..#", "#.#.#", "#..##", "#...#", "#...#"}},
    {'O', {".###.", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."}},
    {'P', {"####.", "#...#", "#...#", "####.", "#....", "#....", "#...."}},
    {'Q', {".###.", "#...#", "#...#", "#...#", "#.#.#", "#..#.", ".##.#"}},
    {'R', {"####.", "#...#", "#...#", "####.", "#.#..", "#..#.", "#...#"}},
    {'S', {".####", "#....", "#....", ".###.", "....#", "....#", "####."}},
    {'T', {"#####", "..#..", "..#..", "..#..", "..#..", "..#..", "..#.."}},
    {'U', {"#...#", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."}},
    {'V', {"#...#", "#...#", "#...#", "#...#", "#...#", ".#.#.", "..#.."}},
    {'W', {"#...#", "#...#", "#...#", "#.#.#", "#.#.#", "#.#.#", ".#.#."}},
    {'X', {"#...#", "#...#", ".#.#.", "..#..", ".#.#.", "#...#", "#...#"}},
    {'Y', {"#...#", "#...#", "#...#", ".#.#.", "..#..", "..#..", "..#.."}},
    {'Z', {"#####", "....#", "...#.", "..#..", ".#...", "#....", "#####"}},
    {'.', {".....", ".....", ".....", ".....", ".....", ".##..", ".##.."}},
    {',', {".....", ".....", ".....", ".....", ".##..", "..#..", ".#..."}},
    {':', {".....", ".##..", ".##..", ".....", ".##..", ".##..", "....."}},
    {'-', {".....", ".....", ".....", "#####", ".....", ".....", "....."}},
    {'!', {"..#..", "..#..", "..#..", "..#..", "..#..", ".....", "..#.."}},
    {'?', {".###.", "#...#", "....#", "...#.", "..#..", ".....", "..#.."}},
    {'/', {".....", "....#", "...#.", "..#..", ".#...", "#....", "....."}},
    {'\'', {".##..", "..#..", ".#...", ".....", ".....", ".....", "....."}},
    {'+', {".....", "..#..", "..#..", "#####", "..#..", "..#..", "....."}},
    {'%', {"##..#", "##.#.", "...#.", "..#..", ".#...", "#.##.", "#..##"}},
    {'(', {"..#..", ".#...", "#....", "#....", "#....", ".#...", "..#.."}},
    {')', {"..#..", "...#.", "....#", "....#", "....#", "...#.", "..#.."}},
    {'<', {"...#.", "..#..", ".#...", "#....", ".#...", "..#..", "...#."}},
    {'>', {".#...", "..#..", "...#.", "....#", "...#.", "..#..", ".#..."}},
    {'=', {".....", ".....", "#####", ".....", "#####", ".....", "....."}},
    {'_', {".....", ".....", ".....", ".....", ".....", ".....", "#####"}},
    {'[', {".###.", ".#...", ".#...", ".#...", ".#...", ".#...", ".###."}},
    {']', {".###.", "...#.", "...#.", "...#.", "...#.", "...#.", ".###."}},
    {'*', {".....", ".#.#.", "..#..", "#####", "..#..", ".#.#.", "....."}},
};
}  // namespace

std::map<char, Image> font(int scale) {
    std::map<char, Image> out;
    for (const Glyph& g : kGlyphs) {
        Image img(5 * scale, 7 * scale);
        for (int y = 0; y < 7; ++y)
            for (int x = 0; x < 5; ++x)
                if (g.rows[y][x] == '#')
                    for (int sy = 0; sy < scale; ++sy)
                        for (int sx = 0; sx < scale; ++sx) img.set(x * scale + sx, y * scale + sy, 255, 255, 255);
        out[g.ch] = img;
        if (g.ch >= 'A' && g.ch <= 'Z') out[static_cast<char>(g.ch + 32)] = img;
    }
    return out;
}

// ---------------------------------------------------------------------------
// Sounds
namespace {
Sound make(float seconds, int rate = 22050) {
    Sound s;
    s.rate = rate;
    s.samples.assign(static_cast<size_t>(seconds * rate), 0.f);
    return s;
}
float noise(uint32_t& state) {
    state = state * 1664525u + 1013904223u;
    return (static_cast<float>(state >> 8) / 8388607.5f) - 1.f;
}
void normalise(Sound& s, float peak = 0.8f) {
    float m = 0.f;
    for (float v : s.samples) m = std::max(m, std::fabs(v));
    if (m > 0.f) for (float& v : s.samples) v = v / m * peak;
}
}  // namespace

Sound sndShoot() {
    Sound s = make(0.35f);
    uint32_t st = 1;
    for (size_t i = 0; i < s.samples.size(); ++i) {
        float t = static_cast<float>(i) / s.rate;
        float env = std::exp(-t * 14.f);
        float thump = std::sin(2 * kPi * (120.f - 200.f * t) * t) * std::exp(-t * 30.f);
        s.samples[i] = env * noise(st) * 0.7f + thump;
    }
    normalise(s);
    return s;
}

Sound sndExplode() {
    Sound s = make(1.2f);
    uint32_t st = 7;
    float lp = 0.f;
    for (size_t i = 0; i < s.samples.size(); ++i) {
        float t = static_cast<float>(i) / s.rate;
        float env = std::exp(-t * 3.5f);
        lp += (noise(st) - lp) * 0.12f;   // low-pass rumble
        float boom = std::sin(2 * kPi * (60.f - 40.f * t) * t) * std::exp(-t * 6.f);
        s.samples[i] = env * lp * 1.5f + boom;
    }
    normalise(s);
    return s;
}

Sound sndHit() {
    Sound s = make(0.12f);
    for (size_t i = 0; i < s.samples.size(); ++i) {
        float t = static_cast<float>(i) / s.rate;
        s.samples[i] = (std::sin(2 * kPi * 900.f * t) > 0 ? 1.f : -1.f) * std::exp(-t * 40.f);
    }
    normalise(s, 0.5f);
    return s;
}

Sound sndMove() {
    Sound s = make(0.05f);
    for (size_t i = 0; i < s.samples.size(); ++i) {
        float t = static_cast<float>(i) / s.rate;
        s.samples[i] = std::sin(2 * kPi * 440.f * t) * std::exp(-t * 60.f);
    }
    normalise(s, 0.35f);
    return s;
}

Sound sndRotate() {
    Sound s = make(0.07f);
    for (size_t i = 0; i < s.samples.size(); ++i) {
        float t = static_cast<float>(i) / s.rate;
        s.samples[i] = std::sin(2 * kPi * (600.f + 800.f * t) * t) * std::exp(-t * 40.f);
    }
    normalise(s, 0.35f);
    return s;
}

Sound sndLock() {
    Sound s = make(0.15f);
    for (size_t i = 0; i < s.samples.size(); ++i) {
        float t = static_cast<float>(i) / s.rate;
        s.samples[i] = std::sin(2 * kPi * 180.f * t) * std::exp(-t * 25.f);
    }
    normalise(s, 0.5f);
    return s;
}

Sound sndClear() {
    Sound s = make(0.5f);
    for (size_t i = 0; i < s.samples.size(); ++i) {
        float t = static_cast<float>(i) / s.rate;
        int step = static_cast<int>(t * 12.f);
        float f = 440.f * std::pow(2.f, step / 12.f * 2.f);
        s.samples[i] = std::sin(2 * kPi * f * t) * std::exp(-t * 4.f);
    }
    normalise(s, 0.5f);
    return s;
}

Sound sndRedLine() {
    Sound s = make(2.0f);
    for (size_t i = 0; i < s.samples.size(); ++i) {
        float t = static_cast<float>(i) / s.rate;
        float f = 300.f + 250.f * (0.5f + 0.5f * std::sin(2 * kPi * 2.f * t));
        float v = std::sin(2 * kPi * f * t);
        v = v > 0 ? 0.8f : -0.8f;
        s.samples[i] = v * (0.6f + 0.4f * std::sin(2 * kPi * 8.f * t)) * std::min(1.f, (2.f - t));
    }
    normalise(s, 0.6f);
    return s;
}

Sound sndPain() {
    Sound s = make(0.3f);
    uint32_t st = 3;
    for (size_t i = 0; i < s.samples.size(); ++i) {
        float t = static_cast<float>(i) / s.rate;
        s.samples[i] = (std::sin(2 * kPi * (220.f - 150.f * t) * t) + 0.3f * noise(st)) * std::exp(-t * 8.f);
    }
    normalise(s, 0.6f);
    return s;
}

Sound sndEnemyDie() {
    Sound s = make(0.6f);
    uint32_t st = 5;
    for (size_t i = 0; i < s.samples.size(); ++i) {
        float t = static_cast<float>(i) / s.rate;
        float f = 160.f - 120.f * t;
        s.samples[i] = (std::sin(2 * kPi * f * t) * 0.7f + 0.5f * noise(st) * std::exp(-t * 6.f)) * std::exp(-t * 4.f);
    }
    normalise(s, 0.7f);
    return s;
}

Sound sndLevelUp() {
    Sound s = make(0.6f);
    const float notes[4] = {523.f, 659.f, 784.f, 1047.f};
    for (size_t i = 0; i < s.samples.size(); ++i) {
        float t = static_cast<float>(i) / s.rate;
        int n = std::min(3, static_cast<int>(t * 8.f));
        s.samples[i] = std::sin(2 * kPi * notes[n] * t) * std::exp(-(t - n * 0.125f) * 10.f);
    }
    normalise(s, 0.5f);
    return s;
}

Sound sndFireball() {
    Sound s = make(0.3f);
    uint32_t st = 9;
    float lp = 0.f;
    for (size_t i = 0; i < s.samples.size(); ++i) {
        float t = static_cast<float>(i) / s.rate;
        lp += (noise(st) - lp) * 0.3f;
        s.samples[i] = lp * std::exp(-t * 10.f) + 0.3f * std::sin(2 * kPi * 90.f * t) * std::exp(-t * 12.f);
    }
    normalise(s, 0.5f);
    return s;
}

Sound sndGameOver() {
    Sound s = make(1.5f);
    for (size_t i = 0; i < s.samples.size(); ++i) {
        float t = static_cast<float>(i) / s.rate;
        float f = 200.f * std::pow(0.5f, t);
        s.samples[i] = (std::sin(2 * kPi * f * t) > 0 ? 0.6f : -0.6f) * std::exp(-t * 1.5f);
    }
    normalise(s, 0.5f);
    return s;
}

}  // namespace rl::proc
