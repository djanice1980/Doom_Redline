// REDLINE — falling blocks that refuse to die.
#include <cstdio>
#include <cstring>
#include <exception>
#include <string>

#include "game/app.h"

namespace {
void usage() {
    std::printf(
        "redline [options]\n"
        "  --wad <file>         Doom IWAD to load art/sounds from (auto-detected from Steam if omitted)\n"
        "  --no-wad             Force procedural art\n"
        "  --size WxH           Window size (default 1600x900)\n"
        "  --fullscreen\n"
        "  --igpu               Prefer the integrated GPU (or set REDLINE_GPU=<index>)\n"
        "  --seed <n>           Deterministic piece sequence\n"
        "  --scenario <name>    title | blocks | redline | fps\n"
        "  --screenshot <png>   Save a screenshot after --frames frames and exit\n"
        "  --frames <n>         Frame count for --screenshot (fixed 60 Hz step)\n"
        "  --bot                Auto-aim and fire in FPS mode (smoke testing)\n"
        "  --level <n>          Starting level for --scenario fps (enemy health scaling)\n"
        "  --absorb <sec>       Seconds before a monster may absorb blocks and grow (default 20)\n"
        "  --god                Player takes no damage (testing)\n"
        "  --mute\n"
        "Environment: REDLINE_WAD, REDLINE_GPU, REDLINE_VALIDATION=1, REDLINE_NOVSYNC=1\n");
}
}  // namespace

int main(int argc, char** argv) {
    rl::game::Options o;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> const char* { return (i + 1 < argc) ? argv[++i] : ""; };
        if (a == "--wad") o.wad = next();
        else if (a == "--no-wad") o.noWad = true;
        else if (a == "--size") { std::string v = next(); if (std::sscanf(v.c_str(), "%dx%d", &o.width, &o.height) != 2) { usage(); return 2; } }
        else if (a == "--fullscreen") o.fullscreen = true;
        else if (a == "--igpu") o.preferIntegrated = true;
        else if (a == "--seed") o.seed = static_cast<uint32_t>(std::atoi(next()));
        else if (a == "--scenario") o.scenario = next();
        else if (a == "--screenshot") o.screenshot = next();
        else if (a == "--frames") o.frames = std::atoi(next());
        else if (a == "--bot") o.bot = true;
        else if (a == "--level") o.level = std::atoi(next());
        else if (a == "--absorb") o.absorbPeriod = static_cast<float>(std::atof(next()));
        else if (a == "--god") o.god = true;
        else if (a == "--mute") o.mute = true;
        else if (a == "-h" || a == "--help") { usage(); return 0; }
        else { std::fprintf(stderr, "unknown option %s\n", a.c_str()); usage(); return 2; }
    }
    try {
        rl::game::App app(o);
        return app.run();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "fatal: %s\n", e.what());
        return 1;
    }
}
