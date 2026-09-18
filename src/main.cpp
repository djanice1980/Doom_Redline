// REDLINE — falling blocks that refuse to die.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <exception>
#include <string>

#include "game/app.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>   // WinMain shim on Windows; a no-op elsewhere

namespace {
void usage() {
    std::printf(
        "redline [options]\n"
        "  --wad <file>         Doom IWAD to load art/sounds from (auto-detected from Steam if omitted)\n"
        "  --extras <file>      Rerelease extras.wad for the Modern / SC-55 soundtracks (default: next to the IWAD)\n"
        "  --no-wad             Force procedural art\n"
        "  --size WxH           Window size (default 1600x900)\n"
        "  --fullscreen\n"
        "  --igpu               Prefer the integrated GPU (or set REDLINE_GPU=<index>)\n"
        "  --seed <n>           Deterministic piece sequence\n"
        "  --scenario <name>    title | blocks | redline | fps | corrupt | prize\n"
        "  --screenshot <png>   Save a screenshot after --frames frames and exit\n"
        "  --frames <n>         Frame count for --screenshot (fixed 60 Hz step)\n"
        "  --record <file.rgba> Append every 2nd frame (--record-every N) as raw RGBA; ffmpeg -f rawvideo -pix_fmt rgba -s WxH -r 30 -i file\n"
        "  --bot                Auto-aim and fire in FPS mode (smoke testing)\n"
        "  --level <n>          Starting level for --scenario fps (enemy health scaling)\n"
        "  --absorb <sec>       Seconds before a monster may absorb blocks and grow (default 20)\n"
        "  --god                Player takes no damage (testing)\n"
        "  --arsenal N          Start a fight holding weapon N with every weapon owned (testing)\n"
        "  --keys <letters>@<frame>[x<hold>]  Hold letters from a frame for <hold> frames (default 150), e.g. --keys BFG@30x150\n"
        "  --click <x>,<y>@<frame>  Testing: move the mouse there and left-click on that frame (repeatable)\n"
        "  --stack <rows>       Pre-fill that many holey rows (testing)\n"
        "  --mute               No sound at all\n"
        "  --no-music           Sound effects only\n"
        "  --music-volume <0-1> Music level (default 0.45)\n"
        "  --profile <name>     Player profile to use (created if new)\n"
        "  --reload-wad <file>  Testing: start with placeholder art, then switch to this WAD after 30 frames\n"
        "  --voxels-dir <dir>   Folder of Voxel Doom .kvx files (default: $REDLINE_VOXELS, ./voxels, ~/Downloads/doom-voxel-models/...)\n"
        "  --voxels | --sprites Force voxel models on or off for this run (default: saved option)\n"
        "  --rt 0|1|2|3         Ray tracing: 0 off (shadow map), 1 sun shadows, 2 sun and all lights, 3 plus floor reflections\n"
        "  --msaa 0|1           Multisample anti-aliasing off/on (default: saved setting, on)\n"
        "  --bloom 0|1          Bloom off/on (default: saved setting, on)\n"
        "  --brutal 0|1         Brutal mode (blood, gibs, casings, bullet holes) off/on (default: saved setting, on)\n"
        "  --music <set>        classic (OPL) | sc55 (original score recordings) | modern (Andrew Hulshult); remembered\n"
        "Environment: REDLINE_WAD, REDLINE_GPU, REDLINE_VALIDATION=1, REDLINE_NOVSYNC=1, REDLINE_SOUNDFONT=<file.sf2>\n");
}
}  // namespace

int main(int argc, char** argv) {
    rl::game::Options o;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> const char* { return (i + 1 < argc) ? argv[++i] : ""; };
        if (a == "--wad") o.wad = next();
        else if (a == "--extras") o.extras = next();
        else if (a == "--no-wad") o.noWad = true;
        else if (a == "--size") { std::string v = next(); if (std::sscanf(v.c_str(), "%dx%d", &o.width, &o.height) != 2) { usage(); return 2; } }
        else if (a == "--fullscreen") o.fullscreen = true;
        else if (a == "--igpu") o.preferIntegrated = true;
        else if (a == "--seed") o.seed = static_cast<uint32_t>(std::atoi(next()));
        else if (a == "--scenario") o.scenario = next();
        else if (a == "--screenshot") o.screenshot = next();
        else if (a == "--record") o.record = next();
        else if (a == "--record-every") o.recordEvery = std::max(1, std::atoi(next()));
        else if (a == "--frames") o.frames = std::atoi(next());
        else if (a == "--bot") o.bot = true;
        else if (a == "--level") o.level = std::atoi(next());
        else if (a == "--absorb") o.absorbPeriod = static_cast<float>(std::atof(next()));
        else if (a == "--god") o.god = true;
        else if (a == "--arsenal") o.arsenal = std::atoi(next());
        else if (a == "--click") { int x = 0, y = 0, f = 0; if (std::sscanf(next(), "%d,%d@%d", &x, &y, &f) == 3) o.clicks.push_back({x, y, f}); }
        else if (a == "--keys") {
            std::string v = next();
            size_t at = v.find('@');
            o.keys = v.substr(0, at);
            if (at != std::string::npos) {
                o.keysFrame = std::atoi(v.c_str() + at + 1);
                size_t x = v.find('x', at);
                if (x != std::string::npos) o.keysHoldFrames = std::atoi(v.c_str() + x + 1);
            }
        }
        else if (a == "--stack") o.stackRows = std::atoi(next());
        else if (a == "--mute") o.mute = true;
        else if (a == "--no-music") o.noMusic = true;
        else if (a == "--music-volume") o.musicVolume = static_cast<float>(std::atof(next()));
        else if (a == "--music") o.musicSet = next();
        else if (a == "--profile") o.profile = next();
        else if (a == "--reload-wad") o.reloadWad = next();
        else if (a == "--voxels-dir") o.voxelDir = next();
        else if (a == "--voxels") o.voxels = 1;
        else if (a == "--sprites") o.voxels = 0;
        else if (a == "--rt") o.rtShadows = std::clamp(std::atoi(next()), 0, 3);
        else if (a == "--msaa") o.msaa = std::atoi(next()) != 0;
        else if (a == "--bloom") o.bloom = std::atoi(next()) != 0;
        else if (a == "--brutal") o.brutal = std::atoi(next()) != 0;
        else if (a == "-h" || a == "--help") { usage(); return 0; }
        else { std::fprintf(stderr, "unknown option %s\n", a.c_str()); usage(); return 2; }
    }
#ifdef _WIN32
    // No console under the GUI subsystem: keep the log next to the save data.
    if (char* pref = SDL_GetPrefPath("redline", "redline")) {
        std::string log = std::string(pref) + "redline.log";
        SDL_free(pref);
        std::freopen(log.c_str(), "w", stderr);
        std::setvbuf(stderr, nullptr, _IONBF, 0);
    }
#endif
    try {
        rl::game::App app(o);
        return app.run();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "fatal: %s\n", e.what());
        return 1;
    }
}
