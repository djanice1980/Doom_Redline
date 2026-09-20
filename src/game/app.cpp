#include "game/app.h"

#include <cstring>

#include "version.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <stdexcept>

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace rl::game {

namespace {
constexpr float kPi = 3.14159265f;
constexpr float kAlertTime = 1.4f;
constexpr float kFlyInTime = 2.2f;
constexpr float kFlyOutTime = 1.6f;
constexpr float kCountdownTime = 3.0f;
constexpr float kLevelCardTime = 4.0f;
constexpr int kGateHeight = 4;   // rows of the back wall that fall to open the dungeon

const glm::vec3 kPieceColors[7] = {
    {0.25f, 0.85f, 0.95f},   // I cyan
    {0.95f, 0.85f, 0.25f},   // O yellow
    {0.70f, 0.35f, 0.90f},   // T purple
    {0.35f, 0.85f, 0.35f},   // S green
    {0.95f, 0.45f, 0.75f},   // Z pink (red is reserved)
    {0.35f, 0.45f, 0.95f},   // J blue
    {0.95f, 0.60f, 0.25f},   // L orange
};

float smoothstep(float t) { t = std::clamp(t, 0.f, 1.f); return t * t * (3.f - 2.f * t); }
}  // namespace

// ---------------------------------------------------------------------------
App::App(Options opts) : opts_(std::move(opts)) {
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMEPAD)) throw std::runtime_error(std::string("SDL_Init: ") + SDL_GetError());
    // High pixel density: on a scaled desktop (Wayland/KDE at 150%, macOS Retina) the swapchain gets
    // the physical pixels, so the RESOLUTION setting means pixels; window sizes are converted to points.
    Uint32 flags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    loadDisplaySettings();
    if (opts_.fullscreen) displayMode_ = 1;
    if (opts_.width != 1600 || opts_.height != 900) { resW_ = opts_.width; resH_ = opts_.height; }
    window_ = SDL_CreateWindow("REDLINE", resW_, resH_, flags);
    if (!window_) throw std::runtime_error(std::string("SDL_CreateWindow: ") + SDL_GetError());
    auto logWindow = [&](const char* when) {
        int pw = 0, ph = 0, lw = 0, lh = 0;
        SDL_GetWindowSizeInPixels(window_, &pw, &ph);
        SDL_GetWindowSize(window_, &lw, &lh);
        const SDL_DisplayID disp = SDL_GetDisplayForWindow(window_);
        std::fprintf(stderr, "[display] %s: %dx%d points, %dx%d pixels, pixel density %.3f, display scale %.3f, content scale %.3f (display %u)\n", when, lw, lh, pw, ph,
                     SDL_GetWindowPixelDensity(window_), SDL_GetWindowDisplayScale(window_), disp ? SDL_GetDisplayContentScale(disp) : 0.f, disp);
    };
    logWindow("created");
    if (pixelDensity() != 1.f) {   // the window was requested in points; make it resW_ x resH_ physical pixels
        SDL_SetWindowSize(window_, static_cast<int>(std::lround(resW_ / pixelDensity())), static_cast<int>(std::lround(resH_ / pixelDensity())));
        SDL_SyncWindow(window_);
        logWindow("resized to pixels");
    }
    buildResolutionList();
    if (displayMode_ != 0) applyDisplay();
    if (opts_.rtShadows >= 0) rtShadows_ = opts_.rtShadows;
    if (opts_.msaa >= 0) msaa_ = opts_.msaa != 0;
    if (opts_.brutal >= 0) brutal_ = opts_.brutal != 0;
    fps_.setBrutal(brutalActive());
    if (opts_.bloom >= 0) bloom_ = opts_.bloom != 0;
    ctx_ = std::make_unique<render::VkContext>(window_, opts_.preferIntegrated);
    renderer_ = std::make_unique<render::Renderer>(*ctx_);

    if (!opts_.mute) audio_.init();
    if (char* p = SDL_GetPrefPath("redline", "redline")) { prefDir_ = p; SDL_free(p); }
    if (const char* b = SDL_GetBasePath()) baseDir_ = b;
    {
        // One random install id per save folder: the key for "which machine was this played on".
        std::string idFile = prefDir_ + "install.txt";
        if (std::FILE* f = std::fopen(idFile.c_str(), "r")) { char buf[64] = {}; if (std::fgets(buf, sizeof buf, f)) machine_.installId = buf; std::fclose(f); }
        while (!machine_.installId.empty() && (machine_.installId.back() == '\n' || machine_.installId.back() == '\r')) machine_.installId.pop_back();
        if (machine_.installId.size() != 36) {
            machine_.installId = newUuid();
            if (std::FILE* f = std::fopen(idFile.c_str(), "w")) { std::fprintf(f, "%s\n", machine_.installId.c_str()); std::fclose(f); }
        }
        machine_.platform = SDL_GetPlatform();
        machine_.os = machine_.platform;
        if (std::FILE* f = std::fopen("/etc/os-release", "r")) {   // distro name on Linux; harmlessly absent elsewhere
            char buf[256];
            while (std::fgets(buf, sizeof buf, f)) {
                std::string line = buf;
                if (line.rfind("PRETTY_NAME=", 0) == 0) {
                    line = line.substr(12);
                    while (!line.empty() && (line.back() == '\n' || line.back() == '"')) line.pop_back();
                    if (!line.empty() && line.front() == '"') line.erase(0, 1);
                    machine_.os = line;
                }
            }
            std::fclose(f);
        }
        machine_.gpu = ctx_->gpuName();
        machine_.cpuCores = SDL_GetNumLogicalCPUCores();
        machine_.ramMb = SDL_GetSystemRAM();
    }
    std::optional<std::filesystem::path> wad;
    if (!opts_.noWad && opts_.reloadWad.empty()) wad = Assets::findWad(opts_.wad, prefDir_, baseDir_);
    if (!wad && !opts_.noWad) {
        wadMissing_ = true;
        std::fprintf(stderr, "[assets] no Doom WAD found; using procedural art (the game will ask for one; or pass --wad <file> / set REDLINE_WAD)\n");
    }
    assets_.brutalPackDir = Assets::findBrutalPack(baseDir_);
    assets_.fontPackDir = Assets::findFontPack(baseDir_);
    if (!assets_.load(wad, audio_, extrasHint())) throw std::runtime_error("asset build failed");
    for (int k = 0; k < kEnemyKinds; ++k) fps_.setKindAvailable(k, assets_.enemies[k].available);
    if (wad && assets_.usingWad()) wadPath_ = wad->string();
    renderer_->setAtlas(assets_.atlas().image());
    renderer_->setMsaa(msaa_);
    if (!renderer_->rayTracingAvailable() && rtShadows_ != 0) { rtShadows_ = 0; saveDisplaySettings(); }   // ray tracing is on by default only on GPUs that have it
    loadMaterials();
    buildEnvironment();
    buildProps();
    {
        std::string pref;
        if (char* p = SDL_GetPrefPath("redline", "redline")) { pref = p; SDL_free(p); }
        std::string base;
        if (const char* b = SDL_GetBasePath()) base = b;
        if (auto dir = VoxelModels::findPack(opts_.voxelDir, pref, base)) voxels_.init(*renderer_, *dir);
        else std::fprintf(stderr, "[voxels] no Voxel Doom pack found (set REDLINE_VOXELS or --voxels-dir); sprites only\n");
        if (assets_.brutalPackDir) {   // voxel gibs ride along with the monster pack
            int n = voxels_.addPack(*assets_.brutalPackDir / "voxels");
            if (n) std::fprintf(stderr, "[voxels] %d gore voxels from the brutal pack\n", n);
        }
    }

    initMusic();
    // Profiles: pick up the last player, or ask for a name on first launch.
    {
        std::string last;
        std::string base = prefDir_;
        if (std::FILE* f = std::fopen((base + "profile.txt").c_str(), "r")) {
            char buf[64] = {};
            if (std::fgets(buf, sizeof buf, f)) { last = buf; while (!last.empty() && (last.back() == '\n' || last.back() == '\r')) last.pop_back(); }
            std::fclose(f);
        }
        std::vector<std::string> profiles = listProfiles();
        if (opts_.scenario != "title" || opts_.bot) last = "PLAYER";   // scripted runs and the bot never touch a real player's scores or play time
        if (!opts_.profile.empty()) last = opts_.profile;
        if (last.empty() && !profiles.empty()) last = profiles.front();
        if (!last.empty()) switchProfile(last);
        else nameRequired_ = true;
        loadOnlineConfig();
        // Several people may share this machine: ask who is playing on every launch.
        if (!nameRequired_ && opts_.profile.empty() && opts_.scenario == "title" && (opts_.frames == 0 || std::getenv("REDLINE_ASK_PROFILE"))) profileRequired_ = true;   // scripted captures skip it unless asked
    }
    if (!opts_.musicSet.empty()) {
        if (opts_.musicSet == "classic") musicSet_ = MusicSet::Classic;
        else if (opts_.musicSet == "sc55") musicSet_ = MusicSet::Sc55;
        else if (opts_.musicSet == "modern") musicSet_ = MusicSet::Modern;
        saveSettings();
    }
    std::fprintf(stderr, "[music] set: %s\n", musicSetName());
    std::fprintf(stderr, "[app] high scores: %zu entries at %s\n", highScores_.entries().size(), highScores_.path().c_str());

    newGame();
    applyScenario();
    // First run without Doom data: offer to find it before anything else.
    if (wadMissing_ && opts_.scenario == "title") openScreen(kScreenWadSetup);
    if (doomArtOff_ && assets_.usingWad()) setDoomArt(false);   // the profile prefers the placeholder look
}

// Music: MUS tracks from the WAD through the OPL3 emulator with GENMIDI, or
// FluidSynth when a soundfont is available; the rerelease Ogg sets stream from
// extras.wad. Safe to call again after the WAD changes.
void App::initMusic() {
    music_.shutdown();
    music_.clearTracks();
    if (!opts_.mute && !opts_.noMusic) {
        audio::GenMidiBank bank;
        if (auto parsed = audio::parseGenMidi(assets_.genmidi)) bank = std::move(*parsed);
        else bank = audio::builtinGenMidi();
        std::string soundfont;
        if (const char* e = std::getenv("REDLINE_SOUNDFONT")) soundfont = e;
        else {
            const char* candidates[] = {"/usr/share/soundfonts/FluidR3_GM.sf2", "/usr/share/soundfonts/default.sf2", "/usr/share/sounds/sf2/FluidR3_GM.sf2",
                                        "/usr/share/soundfonts/FluidR3_GM2-2.sf2", "/usr/share/sounds/sf2/default-GM.sf2"};
            std::error_code ec;
            for (const char* c : candidates) if (std::filesystem::is_regular_file(c, ec)) { soundfont = c; break; }
        }
        bool ok = music_.init(audio_.deviceId(), audio::Audio::kRate, bank, soundfont);
        for (auto& [name, data] : assets_.music) music_.addTrack(name, data);
        // The recorded soundtracks are mastered hot; bring them level with the synth.
        if (music_.oggAvailable())
            for (const Assets::OggLump& l : assets_.oggMusic) music_.addOggTrack(l.name, l.path, l.offset, l.size, l.name[0] == 'H' ? 0.55f : 0.8f);
        music_.setVolume(std::max(music_.volume(), 0.f) > 0.f ? music_.volume() : opts_.musicVolume);
        std::fprintf(stderr, "[music] %s, %zu tracks, backend %s\n", ok ? "ready" : "unavailable", music_.trackNames().size(), music_.backendName());
    }
}

// Swaps every Doom-derived asset for the ones in `wad`: atlas, sounds, music.
// Game state is untouched (it only refers to art by logical name).
bool App::reloadAssets(const std::filesystem::path& wad, const std::string& extras) {
    Assets fresh;
    fresh.brutalPackDir = assets_.brutalPackDir;
    fresh.fontPackDir = assets_.fontPackDir;
    if (!fresh.load(wad, audio_, extras.empty() ? extrasHint() : extras) || !fresh.usingWad()) {
        std::fprintf(stderr, "[assets] %s is not a usable Doom WAD\n", wad.string().c_str());
        return false;
    }
    applyAssets(std::move(fresh));
    doomArtOff_ = false;
    wadMissing_ = false;
    wadStatus_.clear();
    wadPath_ = wad.string();
    if (screen_ == kScreenWadSetup || screen_ == kScreenWadPath) closeScreen();
    saveWadChoice(wad.string());
    if (!extras.empty()) saveExtrasChoice(extras);
    announce("DOOM DATA: " + assets_.wadName() + (assets_.oggMusic.empty() ? "" : "  +  SOUNDTRACKS"), glm::vec4(0.8f, 1.f, 0.8f, 1.f), 1.1f);
    std::fprintf(stderr, "[assets] switched to %s\n", wad.string().c_str());
    return true;
}

void App::applyAssets(Assets&& fresh) {
    assets_ = std::move(fresh);
    for (int k = 0; k < kEnemyKinds; ++k) fps_.setKindAvailable(k, assets_.enemies[k].available);
    fps_.setBrutal(brutalActive());
    renderer_->setAtlas(assets_.atlas().image());
    loadMaterials();
    buildEnvironment();
    buildProps();
    initMusic();
    music_.stop(0.f);   // updateMusic() restarts the right track from the new set
}

// The material maps (assets/materials, see CREDITS.txt there) are normal and
// roughness maps for three Doom textures. They only make sense on top of the real
// art, so they are loaded when a WAD is in use and dropped with the placeholder set.
void App::loadMaterials() {
    namespace fs = std::filesystem;
    materialLumps_.clear();
    std::vector<render::MaterialMaps> maps;
    if (assets_.usingWad() && !std::getenv("REDLINE_NO_MATERIALS")) {   // the env switch is for A/B screenshots
        std::vector<fs::path> candidates;
        if (!baseDir_.empty()) {
            fs::path base(baseDir_);
            candidates.push_back(base / "materials");                                    // Windows zip / installer
            candidates.push_back(base / ".." / "share" / "redline" / "materials");   // Linux: bin/../share
            candidates.push_back(base / "assets" / "materials");
            candidates.push_back(base / ".." / "assets" / "materials");              // build/ next to the checkout
        }
        candidates.emplace_back("assets/materials");
        candidates.emplace_back("/usr/share/redline/materials");
        candidates.emplace_back("/usr/local/share/redline/materials");
        std::error_code ec;
        fs::path dir;
        for (const fs::path& c : candidates) if (fs::is_directory(c, ec)) { dir = c; break; }
        auto readFile = [&](const fs::path& p, render::Ktx2Image& out) {
            std::vector<uint8_t> bytes;
            if (FILE* f = std::fopen(p.string().c_str(), "rb")) {
                uint8_t buf[65536];
                size_t n;
                while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) bytes.insert(bytes.end(), buf, buf + n);
                std::fclose(f);
            }
            if (bytes.empty()) return false;
            std::string err;
            if (!render::loadKtx2(bytes, out, &err)) { std::fprintf(stderr, "[materials] %s: %s\n", p.string().c_str(), err.c_str()); return false; }
            return true;
        };
        if (!dir.empty()) {
            for (const std::string& lump : {assets_.wallLump, assets_.floorLump, assets_.ceilingLump}) {
                if (lump.empty()) continue;
                render::MaterialMaps m;
                bool n = readFile(dir / (lump + "_remix_normal.ktx2"), m.normal);
                bool r = readFile(dir / (lump + "_remix_roughness.ktx2"), m.roughness);
                if (!n && !r) continue;
                materialLumps_.push_back(lump);
                maps.push_back(std::move(m));
            }
        }
        if (maps.empty()) std::fprintf(stderr, "[materials] no material maps found for %s/%s (looked for a materials folder next to the game)\n", assets_.wallLump.c_str(), assets_.floorLump.c_str());
        else std::fprintf(stderr, "[materials] %zu material map sets from %s\n", maps.size(), dir.string().c_str());
    }
    renderer_->setMaterialMaps(maps);
}

int App::materialSlot(const std::string& lump) const {
    for (size_t i = 0; i < materialLumps_.size(); ++i) if (materialLumps_[i] == lump) return static_cast<int>(i) + 1;
    return 0;
}

// The "just because" switch: OFF swaps in the procedural placeholder set while the
// WAD stays known; ON reloads it. The choice is saved with the other settings.
void App::setDoomArt(bool on) {
    if (on) {
        doomArtOff_ = false;
        if (assets_.usingWad()) return;
        if (!wadPath_.empty()) { reloadAssets(wadPath_); return; }
        if (auto w = Assets::findWad(opts_.wad, prefDir_, baseDir_)) { reloadAssets(*w); return; }
        wadMissing_ = true;   // nothing to reload: back to the chooser
        openScreen(kScreenWadSetup);
        return;
    }
    doomArtOff_ = true;
    if (!assets_.usingWad()) return;
    Assets fresh;
    fresh.brutalPackDir = assets_.brutalPackDir;
    fresh.fontPackDir = assets_.fontPackDir;
    fresh.load(std::nullopt, audio_);
    applyAssets(std::move(fresh));
    announce("PLACEHOLDER ART", glm::vec4(0.8f, 0.9f, 1.f, 1.f), 1.1f);
    std::fprintf(stderr, "[assets] Doom art switched off (placeholder set)\n");
}

void App::saveWadChoice(const std::string& path) const {
    if (prefDir_.empty()) return;
    if (std::FILE* f = std::fopen((prefDir_ + "wad.txt").c_str(), "w")) { std::fprintf(f, "%s\n", path.c_str()); std::fclose(f); }
}

void App::saveExtrasChoice(const std::string& path) const {
    if (prefDir_.empty()) return;
    if (std::FILE* f = std::fopen((prefDir_ + "extras.txt").c_str(), "w")) { std::fprintf(f, "%s\n", path.c_str()); std::fclose(f); }
}

std::string App::extrasHint() const {
    if (!opts_.extras.empty()) return opts_.extras;
    if (std::string s = Assets::savedExtrasPath(prefDir_); !s.empty()) return s;
    return Assets::configExtrasPath(baseDir_);
}

void App::dialogCallback(void* userdata, const char* const* files, int) {
    App* app = static_cast<App*>(userdata);
    std::lock_guard<std::mutex> lock(app->dialogMutex_);
    app->dialogFiles_.clear();
    if (files) for (const char* const* f = files; *f; ++f) app->dialogFiles_.emplace_back(*f);
    else std::fprintf(stderr, "[app] file dialog: %s\n", SDL_GetError());
    app->dialogDone_ = true;
}

// Native "open file" dialog (SDL3: Windows common dialog, GTK/portal/kdialog on
// Linux). The result arrives through dialogCallback and is applied in update().
void App::browseForWad(bool extras) {
    if (dialogOpen_) return;
    dialogOpen_ = true;
    dialogForExtras_ = extras;
    wadStatus_ = extras ? "CHOOSE EXTRAS.WAD IN THE FILE WINDOW" : "CHOOSE DOOM.WAD IN THE FILE WINDOW";
    static const SDL_DialogFileFilter filters[] = {{"Doom WAD files", "wad;WAD"}, {"All files", "*"}};
    std::string start;
    if (!wadPath_.empty()) start = std::filesystem::path(wadPath_).parent_path().string();
    SDL_ShowOpenFileDialog(&App::dialogCallback, this, window_, filters, 2, start.empty() ? nullptr : start.c_str(), false);
}

App::~App() {
    stats_.save();
    // Order matters: the music stream, then the audio device and effect
    // streams, must go before SDL_Quit (they are members, so they would
    // otherwise be destroyed after it).
    music_.shutdown();
    audio_.shutdown();
    renderer_.reset();
    ctx_.reset();
    if (window_) SDL_DestroyWindow(window_);
    SDL_Quit();
}

void App::play(const char* name, float gain, float pitch, int minIntervalMs) { audio_.play(name, gain, pitch, minIntervalMs); }

void App::newGame(bool enter) {
    uint32_t seed = opts_.seed ? opts_.seed : static_cast<uint32_t>(std::chrono::steady_clock::now().time_since_epoch().count() & 0xFFFFFFFFu);
    game_ = std::make_unique<core::Game>(seed);
    fps_ = FpsMode();
    fps_.setBrutal(brutalActive());
    fps_.setDungeonEnabled(dungeon_ && !std::getenv("REDLINE_NO_DUNGEON"));
    for (int k = 0; k < kEnemyKinds; ++k) fps_.setKindAvailable(k, assets_.enemies[k].available);
    ambient_.reset(seed, 1);
    brawlDecals_.clear();
    redLinesSurvived_ = 0;
    diedInFps_ = false;
    fightStats_ = {};
    gameBlocks_ = gamePickups_ = 0;
    levelCardT_ = 99.f;
    bfgUsed_ = false;
    bfgFlash_ = 0.f;
    bfgHoldT_ = 0.f;
    keyB_ = keyF_ = keyG_ = false;
    keys_ = {};
    fpsIn_ = {};
    run_ = {};
    run_.startedAt = isoNowUtc();
    run_.kb0 = stats_.lifetime().keyboardActions; run_.mouse0 = stats_.lifetime().mouseActions; run_.pad0 = stats_.lifetime().padActions;
    runRecorded_ = false;
    onlineStatus_.clear();
    onlineRank_ = 0;
    if (enter) enterMode(opts_.scenario == "title" ? Mode::Title : Mode::Blocks);
}

void App::applyScenario() {
    if (opts_.scenario == "blocks" && opts_.level > 1) { game_->setLevel(opts_.level); ambient_.reset(static_cast<uint32_t>(opts_.seed ? opts_.seed : 7u), opts_.level); }
    if (opts_.stackRows > 0) {
        for (int r = std::max(0, core::kBoardH - opts_.stackRows); r < core::kBoardH; ++r)
            for (int c = 0; c < core::kBoardW; ++c)
                if ((c * 7 + r * 3) % 5 != 0) game_->setCell(c, r, core::Cell{core::CellKind::Normal, static_cast<uint8_t>((c + r) % 7)});
    }
    if (opts_.scenario == "corrupt") {
        // A tall, holey stack so corruption pressure is high from the start,
        // with a few blocks already mid-flicker.
        for (int r = 7; r < core::kBoardH; ++r)
            for (int c = 0; c < core::kBoardW; ++c)
                if ((c * 7 + r * 3) % 5 != 0) game_->setCell(c, r, core::Cell{core::CellKind::Normal, static_cast<uint8_t>((c + r) % 7)});
        game_->setLevel(std::max(opts_.level, 4));
        // A hole boxed in by red at the bottom (red left, right and above; floor below): evil is already filling it.
        for (int c = 0; c < core::kBoardW; ++c) if (c != 5) game_->setCell(c, core::kBoardH - 1, core::Cell{core::CellKind::Red, 0});
        game_->setCell(5, core::kBoardH - 2, core::Cell{core::CellKind::Red, 0});
        { core::Cell hole; hole.corrupt = 0.9f; game_->setCell(5, core::kBoardH - 1, hole); }
        const int pre[4][2] = {{2, 12}, {6, 15}, {8, 9}, {4, 18}};
        for (auto& pc : pre) {
            core::Cell cell = game_->at(pc[0], pc[1]);
            if (cell.kind == core::CellKind::Normal) { cell.corrupt = 0.4f + 0.5f * static_cast<float>(pc[0] % 3); game_->setCell(pc[0], pc[1], cell); }
        }
    }
    if (opts_.scenario == "prize") {
        // A tetris straight into a red line: rows 15-18 full except column 0,
        // row 19 all red. The I piece fills column 0, clears four lines, and
        // the red row underneath triggers the fight with the prizes banked.
        const int H = core::kBoardH;
        for (int r = H - 5; r < H - 1; ++r)
            for (int c = 1; c < core::kBoardW; ++c) game_->setCell(c, r, core::Cell{core::CellKind::Normal, static_cast<uint8_t>((c + r) % 7)});
        for (int c = 0; c < core::kBoardW; ++c) game_->setCell(c, H - 1, core::Cell{core::CellKind::Red, 0});
        game_->forcePiece(core::Shape::I, {});
        game_->spawnNow();
        game_->rotateCW();
        for (int i = 0; i < 6; ++i) game_->moveLeft();
        game_->hardDrop();
    }
    if (opts_.scenario == "redline" || opts_.scenario == "fps" || opts_.scenario == "dungeon") {
        // A nearly complete red floor row plus assorted red regions of
        // different sizes (one enemy of each class) and normal blocks around
        // them so the explosions have something to destroy.
        const int H = core::kBoardH;
        auto red = [&](int c, int r) { game_->setCell(c, r, core::Cell{core::CellKind::Red, 0}); };
        auto normal = [&](int c, int r) { game_->setCell(c, r, core::Cell{core::CellKind::Normal, static_cast<uint8_t>((c * 3 + r) % 7)}); };
        for (int c = 1; c < core::kBoardW; ++c) red(c, H - 1);            // floor row (10 with the dropped piece)
        for (int c = 2; c < 8; ++c) normal(c, H - 2);
        for (int c = 3; c < 7; ++c) normal(c, H - 3);
        red(1, H - 5); red(2, H - 5); red(1, H - 6);                        // 3-region -> imp
        normal(3, H - 5); normal(3, H - 6);
        red(8, H - 8);                                                      // single -> zombie
        normal(7, H - 8); normal(9, H - 8); normal(8, H - 9);
        red(4, H - 10); red(5, H - 10); red(4, H - 11); red(5, H - 11); red(6, H - 11);   // 5-region -> demon
        normal(3, H - 10); normal(6, H - 10); normal(3, H - 11); normal(7, H - 11);
        if ((opts_.scenario == "fps" || opts_.scenario == "dungeon") && opts_.level >= 8) {
            // Big fights: a 20-cell red slab -> cyberdemon, plus cover to eat.
            for (int r = H - 16; r < H - 12; ++r) for (int c = 2; c < 7; ++c) red(c, r);
            for (int c = 0; c < 2; ++c) normal(c, H - 13);
            for (int c = 7; c < 10; ++c) normal(c, H - 14);
        }
        game_->forcePiece(core::Shape::I, {true, true, true, true});
        game_->spawnNow();
        game_->rotateCW();
        for (int i = 0; i < 6; ++i) game_->moveLeft();
        game_->hardDrop();
        for (int i = 0; i < 200 && game_->phase() != core::Phase::RedLine; ++i) game_->tick(0.05f);
        if (opts_.scenario == "fps" || opts_.scenario == "dungeon") {
            game_->setLevel(opts_.level);
            fps_.setAbsorbPeriod(opts_.absorbPeriod);
            fps_.setGodMode(opts_.god);
            fps_.begin(*game_, opts_.level);
            if (opts_.arsenal >= 0) fps_.giveArsenal(opts_.arsenal);
            if (opts_.scenario == "dungeon") fps_.debugClearArena();   // straight to the wall coming down
            enterMode(Mode::Fps);
        }
    }
}

void App::enterMode(Mode m) {
    static const char* names[] = {"Title", "Blocks", "Alert", "FlyIn", "Countdown", "Fps", "FlyOut", "GameOver", "Paused"};
    std::fprintf(stderr, "[app] mode %s -> %s (frame %d, score %d, level %d)\n", names[static_cast<int>(mode_)], names[static_cast<int>(m)], frameCount_,
                 game_ ? game_->score() : 0, game_ ? game_->level() : 0);
    modeT_ = 0.f;
    Mode prev = mode_;
    mode_ = m;
    bool wantMouse = (m == Mode::Fps || m == Mode::Countdown);
    if (wantMouse != mouseCaptured_) {
        SDL_SetWindowRelativeMouseMode(window_, wantMouse);
        mouseCaptured_ = wantMouse;
    }
    menu_ = {};
    switch (m) {
    case Mode::Title:
        menu_.items = {"START", "PLAYER: " + (profileName_.empty() ? std::string("NONE") : profileName_), "OPTIONS", "TROPHIES", "LEADERBOARD", updateAvailable_ ? "WHAT'S NEW: VERSION " + latestVersion_ : "WHAT'S NEW", "CREDITS", "QUIT"};
        refreshAllScores();
        if (nameRequired_ && profileName_.empty() && screen_ == kScreenNone) openScreen(kScreenNameEntry);
        else if (profileRequired_ && screen_ == kScreenNone) openScreen(kScreenProfiles);
        else if (screen_ == kScreenNone && OnlineClient::available() && !onlineAsked_ && !profileName_.empty() && !stats_.emailVerified() && stats_.pendingPoll().empty() && (opts_.frames == 0 || std::getenv("REDLINE_ASK_ONLINE"))) { std::fprintf(stderr, "[online] asking whether to post scores online\n"); openScreen(kScreenOnlineAsk); }   // scripted captures skip it unless asked
        break;
    case Mode::Alert:
        play("redline", 1.f);
        break;
    case Mode::FlyIn:
        ++run_.fights;
        ambient_.clear();   // the brawlers leave when the real fight starts
        flyFrom_ = blocksCamera();
        fps_.setAbsorbPeriod(opts_.absorbPeriod);
        fps_.setGodMode(opts_.god);
        {
            core::Prizes p = game_->takePrizes();
            lastInvulnChance_ = p.invulnChance;
            fps_.begin(*game_, game_->level(), p);
            std::fprintf(stderr, "[fps] prizes: +%d health, %d armour, %d%% invuln -> %s\n", static_cast<int>(p.bonusHealth), static_cast<int>(p.shield),
                         static_cast<int>(p.invulnChance * 100.f), fps_.startedInvulnerable() ? "INVULNERABLE" : "no");
        }
        flyTo_ = fpsCamera();
        break;
    case Mode::Countdown:
        fpsIn_ = {};
        countdownLast_ = -1;
        break;
    case Mode::Fps:
        fpsIn_ = {};
        break;
    case Mode::FlyOut:
        flyFrom_ = fpsCamera();
        flyTo_ = blocksCamera();
        ++redLinesSurvived_;
        trophy("red_line");
        if (fps_.damageTaken() <= 0.f) trophy("untouchable");
        if (redLinesSurvived_ >= 5) trophy("survivor");
        break;
    case Mode::Blocks:
        if (prev == Mode::FlyOut) { game_->resumeAfterRedLine(); beginLevelCard(); ambient_.reset(static_cast<uint32_t>(frameCount_), game_->level()); }
        break;
    case Mode::GameOver:
        play("gameover", 1.f);
        gameOverT_ = 0.f;
        highScore_ = std::max(highScore_, game_->score());
        lastRank_ = highScores_.add({game_->score(), game_->level(), redLinesSurvived_, game_->lines(), ""});
        if (lastRank_ > 0) std::fprintf(stderr, "[app] new high score rank %d: %d\n", lastRank_, game_->score());
        if (lastRank_ == 1) trophy("doom_slayer");
        recordRun();
        menu_.items = {"RESTART", "MAIN MENU", "QUIT"};
        if (mouseCaptured_) { SDL_SetWindowRelativeMouseMode(window_, false); mouseCaptured_ = false; }
        break;
    case Mode::Paused:
        menu_.items = {"RESUME", "TROPHIES", "OPTIONS", "RESTART", "MAIN MENU", "QUIT"};
        break;
    default:
        break;
    }
    updateMusic();
}

// Settings: the chosen music set persists next to the high scores.
void App::loadSettings() {
    if (settingsPath_.empty()) {
        char* pref = SDL_GetPrefPath("redline", "redline");
        settingsPath_ = pref ? std::string(pref) + "settings.txt" : "settings.txt";
        if (pref) SDL_free(pref);
    }
    // Default: the modern soundtrack when the rerelease extras are present.
    bool haveModern = false;
    for (const Assets::OggLump& l : assets_.oggMusic) if (l.name[0] == 'H') haveModern = true;
    musicSet_ = haveModern && music_.oggAvailable() ? MusicSet::Modern : MusicSet::Classic;
    padSens_ = 1.f;
    padInvertY_ = false;
    padRumble_ = true;
    useVoxels_ = voxels_.available();   // default on when the pack is present
    doomArtOff_ = false;
    onlineOn_ = true;
    onlineAsked_ = false;
    music_.setEnabled(true);
    music_.setVolume(opts_.musicVolume);
    bool sawSfx = false;
    if (std::FILE* f = std::fopen(settingsPath_.c_str(), "r")) {
        char key[64], val[64];
        while (std::fscanf(f, "%63[^=]=%63s\n", key, val) == 2) {
            std::string k = key, v = val;
            if (k == "music_set") musicSet_ = v == "classic" ? MusicSet::Classic : v == "sc55" ? MusicSet::Sc55 : MusicSet::Modern;
            else if (k == "music_on") music_.setEnabled(v != "0");
            else if (k == "music_volume") music_.setVolume(static_cast<float>(std::atof(v.c_str())));
            else if (k == "sfx_volume") { sfxVolume_ = std::clamp(static_cast<float>(std::atof(v.c_str())), 0.f, 1.f); sawSfx = true; }
            else if (k == "pad_sens") padSens_ = std::clamp(static_cast<float>(std::atof(v.c_str())), 0.25f, 3.f);
            else if (k == "pad_invert") padInvertY_ = v != "0";
            else if (k == "pad_rumble") padRumble_ = v != "0";
            else if (k == "voxels") useVoxels_ = v != "0";
            else if (k == "doom_art") doomArtOff_ = v == "0";
            else if (k == "brutal") brutal_ = v != "0";
            else if (k == "dungeon") dungeon_ = v != "0";
            else if (k == "online") onlineOn_ = v != "0";
            else if (k == "online_asked") onlineAsked_ = v != "0";
        }
        std::fclose(f);
    }
    if (opts_.voxels >= 0) useVoxels_ = opts_.voxels == 1;   // --voxels / --sprites override the saved choice
    if (!sawSfx) sfxVolume_ = music_.volume();   // first run with the slider: start level with the music
    audio_.setMasterGain(sfxVolume_);
}

std::string App::profilesRoot() const {
    char* pref = SDL_GetPrefPath("redline", "redline");
    std::string base = pref ? pref : "";
    if (pref) SDL_free(pref);
    return base + "profiles/";
}

std::vector<std::string> App::listProfiles() const {
    std::vector<std::string> out;
    std::error_code ec;
    for (auto& entry : std::filesystem::directory_iterator(profilesRoot(), ec))
        if (entry.is_directory(ec)) out.push_back(entry.path().filename().string());
    std::sort(out.begin(), out.end());
    return out;
}

void App::refreshAllScores() {
    allProfileScores_.clear();
    for (const std::string& p : listProfiles()) {
        HighScores hs;
        hs.load(profilesRoot() + p + "/highscores.txt");
        for (const HighScore& h : hs.entries()) allProfileScores_.push_back({h, p});
    }
    std::stable_sort(allProfileScores_.begin(), allProfileScores_.end(), [](const NamedScore& a, const NamedScore& b) { return a.score.score > b.score.score; });
    if (allProfileScores_.size() > 8) allProfileScores_.resize(8);
}

void App::loadProfileList() {
    profileList_ = listProfiles();
    profileInfo_.clear();
    for (const std::string& p : profileList_) {
        const std::string dir = profilesRoot() + p + "/";
        ProfileInfo info;
        HighScores hs; hs.load(dir + "highscores.txt"); info.best = hs.best();
        Trophies t; t.load(dir + "trophies.txt"); info.trophies = t.unlockedCount();
        PlayerStats ps; ps.load(dir, machine_); info.played = ps.lifetime().total(); info.email = !ps.email().empty();
        profileInfo_.push_back(info);
    }
    profileConfirmDelete_ = -1;
    screenIndex_ = 0;
    for (size_t i = 0; i < profileList_.size(); ++i) if (profileList_[i] == profileName_) screenIndex_ = static_cast<int>(i);
}

bool App::renameProfile(const std::string& from, const std::string& to) {
    std::error_code ec;
    const std::string src = profilesRoot() + from, dst = profilesRoot() + to;
    if (from == to) return true;
    if (std::filesystem::exists(dst, ec)) { wadStatus_ = "THERE IS ALREADY A PLAYER CALLED " + to; return false; }
    const bool current = from == profileName_;
    if (current) { stats_.save(); stats_ = PlayerStats(); }   // detach before the folder moves
    std::filesystem::rename(src, dst, ec);
    if (ec) { wadStatus_ = "COULD NOT RENAME: " + ec.message(); if (current) stats_.load(src + "/", machine_); return false; }
    if (current) switchProfile(to);
    refreshAllScores();
    return true;
}

void App::deleteProfile(const std::string& name) {
    std::error_code ec;
    const bool current = name == profileName_;
    if (current) {
        // Move to another profile first (switchProfile saves the old one), or to none.
        std::string other;
        for (const std::string& p : listProfiles()) if (p != name) { other = p; break; }
        if (!other.empty()) switchProfile(other);
        else {
            stats_ = PlayerStats();
            highScores_ = HighScores();
            trophies_ = Trophies();
            profileName_.clear();
            std::filesystem::remove(prefDir_ + "profile.txt", ec);
            if (mode_ == Mode::Title && menu_.items.size() > 1) menu_.items[1] = "PLAYER: NONE";
        }
    }
    std::filesystem::remove_all(profilesRoot() + name, ec);
    std::fprintf(stderr, "[app] profile %s deleted%s\n", name.c_str(), ec ? " (with errors)" : "");
    refreshAllScores();
    if (profileName_.empty()) { nameRequired_ = true; profileRequired_ = false; openScreen(kScreenNameEntry); }
    else { loadProfileList(); if (mode_ == Mode::Title && menu_.items.size() > 1) menu_.items[1] = "PLAYER: " + profileName_; }
}

void App::profileAction(int action) {
    if (screen_ != kScreenProfiles || screenIndex_ >= static_cast<int>(profileList_.size())) return;
    const std::string sel = profileList_[static_cast<size_t>(screenIndex_)];
    if (action == 0) {   // rename on the name screen
        openScreen(kScreenNameEntry);   // resets the name state, so the rename flags come after
        nameRename_ = true;
        renameFrom_ = sel;
        nameEntry_ = sel;
    } else if (action == 1) {   // the email belongs to the loaded profile, so load it first
        if (sel != profileName_) switchProfile(sel);
        profileRequired_ = false;
        if (mode_ == Mode::Title && menu_.items.size() > 1) menu_.items[1] = "PLAYER: " + profileName_;
        openScreen(kScreenEmailEntry);
    } else if (action == 2) {
        if (profileConfirmDelete_ == screenIndex_) {
            play("menu_select", 0.7f);
            // A registered profile has a record on the service. Removing it is a
            // separate, irreversible choice, so ask, with the numbers.
            PlayerStats victim;
            if (sel == profileName_) victim = stats_;
            else victim.peek(profilesRoot() + sel + "/");
            if (OnlineClient::available() && onlineOn_ && victim.emailVerified() && !victim.token().empty()) {
                profileConfirmDelete_ = -1;
                deleteProfileName_ = sel;
                deletePlayerId_ = victim.playerId();
                deleteToken_ = victim.token();
                deleteOnlineInfo_ = {};
                deleteInfoLoaded_ = false;
                online_.player(deletePlayerId_);   // fills in what the record holds
                screenIndex_ = 0;
                openScreen(kScreenDeleteOnline);
            } else {
                deleteProfile(sel);
            }
        }
        else { profileConfirmDelete_ = screenIndex_; play("menu", 0.6f); }
    }
}

void App::switchProfile(const std::string& name) {
    std::string dir = profilesRoot() + name + "/";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    // First profile ever: adopt the files from the flat layout, if any.
    char* pref = SDL_GetPrefPath("redline", "redline");
    std::string base = pref ? pref : "";
    if (pref) SDL_free(pref);
    for (const char* f : {"settings.txt", "highscores.txt", "trophies.txt"})
        if (std::filesystem::is_regular_file(base + f, ec) && !std::filesystem::exists(dir + f, ec)) std::filesystem::rename(base + f, dir + f, ec);
    profileName_ = name;
    settingsPath_ = dir + "settings.txt";
    highScores_.load(dir + "highscores.txt");
    trophies_.load(dir + "trophies.txt");
    loadSettings();
    stats_.save();   // the previous profile's counters
    stats_.load(dir, machine_);
    if (pad_) stats_.setPadModel(padName_);
    if (std::FILE* f = std::fopen((base + "profile.txt").c_str(), "w")) { std::fputs(name.c_str(), f); std::fclose(f); }
    std::fprintf(stderr, "[app] profile %s: %zu scores, %d/%d trophies, music %s, pad sens %.2f\n", name.c_str(), highScores_.entries().size(),
                 trophies_.unlockedCount(), trophies_.total(), musicSetName(), padSens_);
    updateMusic();
}

// --- online ------------------------------------------------------------------
std::string App::runsDir() const { return profileName_.empty() ? std::string() : profilesRoot() + profileName_ + "/runs"; }

std::string App::machineLabel() const {
    std::string gpu = machine_.gpu;
    for (const char* strip : {"AMD ", "NVIDIA ", "Intel(R) ", "Radeon ", "GeForce "}) { const size_t p = gpu.find(strip); if (p == 0) gpu.erase(0, std::strlen(strip)); }
    std::string label = machine_.platform;
    for (char& c : label) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    for (char& c : gpu) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (gpu.size() > 28) gpu.resize(28);
    return gpu.empty() ? label : label + " / " + gpu;
}

// The service URL: --online-server, REDLINE_ONLINE_URL, <pref>/online.txt "server=", redline.cfg "server=" next to the exe, or the default.
void App::loadOnlineConfig() {
    std::string url = "https://doom-redline.vercel.app";
    auto readServer = [&](const std::string& path) {
        if (std::FILE* f = std::fopen(path.c_str(), "r")) {
            char line[512];
            while (std::fgets(line, sizeof line, f)) {
                std::string l = line;
                while (!l.empty() && (l.back() == '\n' || l.back() == '\r' || l.back() == ' ')) l.pop_back();
                if (l.rfind("server=", 0) == 0 && l.size() > 7) url = l.substr(7);
            }
            std::fclose(f);
        }
    };
    if (!baseDir_.empty()) readServer(baseDir_ + "/redline.cfg");
    readServer(prefDir_ + "online.txt");
    if (const char* e = std::getenv("REDLINE_ONLINE_URL")) if (*e) url = e;
    if (!opts_.onlineServer.empty()) url = opts_.onlineServer;
    online_.setServer(url);
    onlineServer_ = online_.server();
    std::fprintf(stderr, "[online] %s (%s, %s)\n", onlineServer_.c_str(), OnlineClient::available() ? "http available" : "no http in this build", onlineOn_ ? "on" : "off");
    if (onlineOn_ && !stats_.token().empty()) submitPendingRuns();
    sendPendingDeletes();              // profiles deleted while offline
    applyVersionInfo(Json(), false);   // the cached copy, if any
    checkVersion();
    pollT_ = 1.f;
}

void App::startRegistration() {
    if (!OnlineClient::available() || stats_.email().empty()) return;
    registerEmail_ = stats_.email();
    registerBusy_ = false;
    openScreen(kScreenRegister);
}

void App::openLeaderboard(int tab) {
    static const char* boards[] = {"global", "week", "fights", "level", "kills"};
    leaderboardTab_ = std::clamp(tab, 0, 4);
    if (screen_ != kScreenLeaderboard) openScreen(kScreenLeaderboard);
    leaderboard_ = Json();
    leaderboardError_.clear();
    leaderboardFetched_.clear();
    // The cached copy first, so the screen is never empty offline.
    if (std::FILE* f = std::fopen((prefDir_ + "leaderboard-" + boards[leaderboardTab_] + ".json").c_str(), "rb")) {
        std::string text; char buf[4096]; size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
        std::fclose(f);
        Json cached;
        if (Json::parse(text, cached)) { leaderboard_ = cached["reply"]; leaderboardFetched_ = "EARLIER (" + cached["fetched"].asString("?") + ")"; }
    }
    if (OnlineClient::available() && onlineOn_) { leaderboardLoading_ = true; online_.leaderboard(boards[leaderboardTab_], stats_.playerId()); }
}

// Every finished game becomes a run record on disk; with online on and a registered
// player it is uploaded from runs/pending and moved to runs/ once the server has it.
void App::recordRun() {
    if (runRecorded_ || !game_ || profileName_.empty()) return;
    runRecorded_ = true;
    RunRecord r;
    r.runId = newUuid();
    r.playerId = stats_.playerId();
    r.installId = machine_.installId;
    r.displayName = profileName_;
    r.version = REDLINE_VERSION;
    r.platform = machine_.platform;
    r.startedAt = run_.startedAt;
    r.endedAt = isoNowUtc();
    r.durationS = run_.duration;
    r.score = game_->score(); r.level = game_->level(); r.lines = game_->lines(); r.redLines = redLinesSurvived_;
    r.fights = run_.fights; r.pieces = run_.pieces; r.tetrises = run_.tetrises; r.bestChain = run_.bestChain;
    for (int k = 0; k < kRunKinds; ++k) { r.killsByKind[k] = run_.killsByKind[k]; r.kills += run_.killsByKind[k]; if (run_.killsByKind[k] > 0) r.highestKind = std::max(r.highestKind, k); }
    r.blocksDestroyed = gameBlocks_; r.pickups = gamePickups_;
    for (int w = 0; w < kRunWeapons; ++w) { r.weaponsOwned[w] = fps_.weapon(w).owned; r.shots[w] = run_.shots[w]; }
    r.damageTaken = run_.damage + (diedInFps_ ? fps_.damageTaken() : 0.0);
    r.bfgUsed = bfgUsed_;
    r.deathCause = diedInFps_ ? "killed" : "stack";
    r.killedBy = diedInFps_ ? run_.killedBy : -1;
    r.seed = game_->seed();
    r.voxels = useVoxels_ && voxels_.available();
    r.brutal = brutalActive();
    r.keyboardActions = static_cast<int>(stats_.lifetime().keyboardActions - run_.kb0);
    r.mouseActions = static_cast<int>(stats_.lifetime().mouseActions - run_.mouse0);
    r.padActions = static_cast<int>(stats_.lifetime().padActions - run_.pad0);
    r.trophies = run_.trophies;
    r.machineOs = machine_.os; r.machineGpu = machine_.gpu; r.machineCores = machine_.cpuCores; r.machineRamMb = machine_.ramMb; r.padModel = pad_ ? padName_ : "";
    // Scripted and cheat runs (scenarios, --bot, --god, --arsenal, --level, --stack, --seed, --keys) stay local.
    const bool scripted = opts_.scenario != "title" || opts_.bot || opts_.god || opts_.arsenal >= 0 || opts_.level > 1 || opts_.stackRows > 0 || opts_.seed != 0 || !opts_.keys.empty() || opts_.frames > 0;
    const bool allowScripted = std::getenv("REDLINE_ONLINE_ALLOW_SCRIPTED") != nullptr;   // for testing the upload against a local server
    const bool upload = OnlineClient::available() && onlineOn_ && !stats_.token().empty() && (!scripted || allowScripted);
    const std::string path = r.write(runsDir() + (upload ? "/pending" : ""));
    std::fprintf(stderr, "[online] run record %s (%s)\n", path.c_str(), upload ? "queued for upload" : "kept locally");
    if (upload && !path.empty()) {
        onlineStatus_ = "POSTING YOUR SCORE...";
        online_.submitRun(r.toJson().dump(), stats_.token(), path);
    } else if (scripted && OnlineClient::available() && onlineOn_) {
        onlineStatus_ = "TEST RUN: NOT POSTED";
    } else if (OnlineClient::available() && !onlineOn_) {
        onlineStatus_ = "TURN ON ONLINE UNDER OPTIONS TO POST SCORES";
    } else if (OnlineClient::available() && stats_.token().empty()) {
        onlineStatus_ = "REGISTER YOUR EMAIL UNDER OPTIONS TO POST SCORES";
    }
}

void App::submitPendingRuns() {
    if (!OnlineClient::available() || !onlineOn_ || stats_.token().empty() || runsDir().empty()) return;
    std::error_code ec;
    int n = 0;
    for (const auto& e : std::filesystem::directory_iterator(runsDir() + "/pending", ec)) {
        if (e.path().extension() != ".json") continue;
        std::string text;
        if (std::FILE* f = std::fopen(e.path().string().c_str(), "rb")) { char buf[4096]; size_t k; while ((k = std::fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, k); std::fclose(f); }
        if (text.empty()) continue;
        online_.submitRun(text, stats_.token(), e.path().string());
        ++n;
    }
    if (n) std::fprintf(stderr, "[online] %d pending run(s) queued for upload\n", n);
}

// The account's other players, as the confirm reply (or the link poll) listed
// them. With any, the game asks whether this profile is one of them before it
// starts a second history under the same address.
void App::applyAccountPlayers(const Json& existing) {
    adoptChoices_.clear();
    for (size_t i = 0; i < existing.size(); ++i) {
        const Json& e = existing[i];
        AccountPlayer a;
        a.id = e["player_id"].asString();
        a.name = e["name"].asString();
        a.since = e["since"].asString().substr(0, 10);
        a.runs = e["runs"].asInt();
        a.bestScore = e["best_score"].asInt();
        a.machines = e["machines"].asInt();
        a.sameName = e["same_name"].asBool();
        if (!a.id.empty()) adoptChoices_.push_back(a);
    }
    // The likeliest match first: same name, then the one with the most behind it.
    std::sort(adoptChoices_.begin(), adoptChoices_.end(), [](const AccountPlayer& x, const AccountPlayer& y) {
        if (x.sameName != y.sameName) return x.sameName;
        return x.runs > y.runs;
    });
    if (adoptChoices_.size() > 6) adoptChoices_.resize(6);
}

// A deletion that has to outlive the profile folder: the token lives inside the
// folder being removed, so it is written here first and sent at the next launch
// if the machine is offline right now.
void App::queuePlayerDelete(const std::string& playerId, const std::string& token) {
    if (playerId.empty() || token.empty()) return;
    if (std::FILE* f = std::fopen((prefDir_ + "pending-deletes.txt").c_str(), "a")) {
        std::fprintf(f, "%s %s\n", playerId.c_str(), token.c_str());
        std::fclose(f);
    }
}

void App::sendPendingDeletes() {
    if (!OnlineClient::available()) return;
    std::vector<std::pair<std::string, std::string>> queued;
    if (std::FILE* f = std::fopen((prefDir_ + "pending-deletes.txt").c_str(), "rb")) {
        char line[512];
        while (std::fgets(line, sizeof line, f)) {
            std::string l(line);
            while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
            const size_t sp = l.find(' ');
            if (sp != std::string::npos && sp + 1 < l.size()) queued.push_back({l.substr(0, sp), l.substr(sp + 1)});
        }
        std::fclose(f);
    }
    for (const auto& [id, token] : queued) online_.deletePlayer(token, id);
    if (!queued.empty()) std::fprintf(stderr, "[online] %zu queued online deletion(s) being sent\n", queued.size());
}

// The test bot walks straight at what it wants, which wedges it on corners and in
// doorways. If it has stopped making ground while trying to move, sidestep for a
// while and flip the direction now and then until it comes free.
void App::botUnstick(FpsInput& in, float dt) {
    const glm::vec3 p = fps_.playerPos();
    const bool trying = std::fabs(in.moveZ) > 0.01f || std::fabs(in.moveX) > 0.01f;
    if (trying && glm::length(p - botLastPos_) < 0.02f) botStuckT_ += dt;
    else botStuckT_ = 0.f;
    botLastPos_ = p;
    if (botStuckT_ < 0.6f) return;
    if (std::fmod(botStuckT_, 1.4f) < dt) botSide_ = -botSide_;
    // Strafe, but leave the aim alone: nudging the look here made the bot spin
    // instead of shooting, so it never killed what it was stuck on.
    in.moveX = botSide_;
    in.moveZ = 0.35f;
}

void App::pollOnline() {
    for (const OnlineClient::Result& res : online_.poll()) {
        switch (res.kind) {
        case OnlineClient::Kind::Register:
            registerBusy_ = false;
            if (res.ok) { stats_.setPendingPoll(res.body["poll_secret"].asString()); pollT_ = 4.f; wadStatus_.clear(); openScreen(kScreenCode); play("menu_select", 0.7f); }
            else { wadStatus_ = "COULD NOT SEND: " + (res.error.empty() ? std::string("NO REPLY") : res.error); play("menu", 0.6f); }
            for (char& c : wadStatus_) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            break;
        case OnlineClient::Kind::Confirm:
            registerBusy_ = false;
            if (res.ok && !res.body["token"].asString().empty()) {
                stats_.setVerified(res.body["token"].asString());
                announce("REGISTERED: " + registerEmail_, glm::vec4(0.8f, 1.f, 0.8f, 1.f), 1.1f);
                play("pickup_weapon", 1.f);
                applyAccountPlayers(res.body["existing"]);
                closeScreen();
                // This address already has players: ask before starting a second history.
                if (!adoptChoices_.empty()) { screenIndex_ = 0; openScreen(kScreenAdopt); }
                else submitPendingRuns();
            } else {
                wadStatus_ = res.error.empty() ? "WRONG CODE" : res.error;
                for (char& c : wadStatus_) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                codeEntry_.clear();
                play("menu", 0.6f);
            }
            break;
        case OnlineClient::Kind::SubmitRun: {
            std::error_code ec;
            if (res.ok) {
                // Uploaded: the record moves out of pending.
                const std::filesystem::path p(res.tag);
                std::filesystem::rename(p, p.parent_path().parent_path() / p.filename(), ec);
                onlineRank_ = res.body["rank"].asInt();
                onlineTotal_ = res.body["total_players"].asInt();
                onlineTier_ = res.body["tier"].asString();
                if (onlineRank_ > 0) onlineStatus_ = "WORLD RANK #" + std::to_string(onlineRank_) + (onlineTotal_ > 0 ? " OF " + std::to_string(onlineTotal_) : "") + (onlineTier_.empty() ? "" : "   " + onlineTier_);
                else onlineStatus_ = "SCORE POSTED";
                if (res.body["best_rank"].asInt() > 0 && res.body["best_rank"].asInt() < onlineRank_) onlineStatus_ += "   (YOUR BEST: #" + std::to_string(res.body["best_rank"].asInt()) + ")";
            } else if (res.status == 401 || res.status == 403) {
                onlineStatus_ = "THE SERVER DOES NOT KNOW THIS PLAYER: REGISTER AGAIN UNDER OPTIONS";
                stats_.setVerified("");   // the token is stale
            } else if (res.status >= 400 && res.status < 500 && res.status != 429) {
                // Refused for good (an implausible record): out of the queue, kept for inspection.
                const std::filesystem::path p(res.tag);
                std::filesystem::create_directories(p.parent_path().parent_path() / "rejected", ec);
                std::filesystem::rename(p, p.parent_path().parent_path() / "rejected" / p.filename(), ec);
                onlineStatus_ = "NOT POSTED: " + (res.error.empty() ? std::string("REFUSED") : res.error);
                for (char& c : onlineStatus_) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            } else {
                onlineStatus_ = "COULD NOT POST (" + (res.error.empty() ? std::string("NO REPLY") : res.error) + "), WILL RETRY NEXT LAUNCH";
                for (char& c : onlineStatus_) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            }
            std::fprintf(stderr, "[online] submit %s: %s\n", res.tag.c_str(), onlineStatus_.c_str());
            break;
        }
        case OnlineClient::Kind::Leaderboard: {
            leaderboardLoading_ = false;
            static const char* boards[] = {"global", "week", "fights", "level", "kills"};
            if (res.tag != boards[leaderboardTab_]) break;   // a stale reply for another tab
            if (res.ok) {
                leaderboard_ = res.body;
                leaderboardError_.clear();
                leaderboardFetched_ = "JUST NOW";
                Json cached = Json::object();
                cached.set("fetched", isoNowUtc()).set("reply", res.body);
                if (std::FILE* f = std::fopen((prefDir_ + "leaderboard-" + res.tag + ".json").c_str(), "wb")) { const std::string t = cached.dump(); std::fwrite(t.data(), 1, t.size(), f); std::fclose(f); }
            } else {
                leaderboardError_ = res.error.empty() ? "NO REPLY" : res.error;
                for (char& c : leaderboardError_) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            }
            break;
        }
        case OnlineClient::Kind::Player:
            // Only asked for when a registered profile is about to be deleted, so the
            // question can say what the online record actually holds.
            if (res.ok && res.tag == deletePlayerId_) {
                deleteOnlineInfo_.name = res.body["name"].asString();
                deleteOnlineInfo_.since = res.body["since"].asString().substr(0, 10);
                deleteOnlineInfo_.runs = res.body["totals"]["runs"].asInt();
                deleteOnlineInfo_.bestScore = res.body["best_score"].asInt();
                deleteOnlineInfo_.machines = res.body["totals"]["machines"].asInt();
                deleteInfoLoaded_ = true;
            }
            break;
        case OnlineClient::Kind::Adopt:
            adoptBusy_ = false;
            if (res.ok && !res.body["player_id"].asString().empty()) {
                // The token the game holds now belongs to the older player.
                stats_.setPlayerId(res.body["player_id"].asString());
                const int moved = res.body["runs_moved"].asInt();
                std::fprintf(stderr, "[online] adopted player %s (%d run(s) moved)\n", res.body["player_id"].asString().c_str(), moved);
                announce("CARRYING ON AS YOUR EXISTING PLAYER", glm::vec4(0.8f, 1.f, 0.8f, 1.f), 1.1f);
                play("pickup_weapon", 1.f);
                adoptChoices_.clear();
                closeScreen();
                submitPendingRuns();
            } else {
                wadStatus_ = "COULD NOT MERGE: " + (res.error.empty() ? std::string("NO REPLY") : res.error);
                for (char& c : wadStatus_) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                play("menu", 0.6f);
            }
            break;
        case OnlineClient::Kind::DeletePlayer: {
            // 401 means the server has already forgotten it: either way the queue entry is done.
            const bool done = res.ok || res.status == 401 || res.status == 404;
            if (done) {
                std::vector<std::string> keep;
                if (std::FILE* f = std::fopen((prefDir_ + "pending-deletes.txt").c_str(), "rb")) {
                    char line[512];
                    while (std::fgets(line, sizeof line, f)) {
                        std::string l(line);
                        while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
                        if (!l.empty() && l.rfind(res.tag, 0) != 0) keep.push_back(l);
                    }
                    std::fclose(f);
                }
                std::error_code ec;
                if (keep.empty()) std::filesystem::remove(prefDir_ + "pending-deletes.txt", ec);
                else if (std::FILE* f = std::fopen((prefDir_ + "pending-deletes.txt").c_str(), "wb")) {
                    for (const std::string& l : keep) std::fprintf(f, "%s\n", l.c_str());
                    std::fclose(f);
                }
                std::fprintf(stderr, "[online] online record %s removed (%d run(s))\n", res.tag.c_str(), res.body["runs_deleted"].asInt());
            } else {
                std::fprintf(stderr, "[online] could not remove %s (%s), will retry next launch\n", res.tag.c_str(), res.error.empty() ? "no reply" : res.error.c_str());
            }
            break;
        }
        case OnlineClient::Kind::Poll: {
            const std::string status = res.body["status"].asString();
            if (res.ok && status == "confirmed" && !res.body["token"].asString().empty()) {
                stats_.setVerified(res.body["token"].asString());
                std::fprintf(stderr, "[online] registration approved from the email\n");
                announce("REGISTERED: " + (registerEmail_.empty() ? stats_.email() : registerEmail_), glm::vec4(0.8f, 1.f, 0.8f, 1.f), 1.1f);
                play("pickup_weapon", 1.f);
                applyAccountPlayers(res.body["existing"]);
                if (screen_ == kScreenCode) closeScreen();
                if (!adoptChoices_.empty() && mode_ == Mode::Title) { screenIndex_ = 0; openScreen(kScreenAdopt); }
                else submitPendingRuns();
            } else if (res.ok && (status == "declined" || status == "expired")) {
                stats_.setPendingPoll("");
                if (screen_ == kScreenCode) wadStatus_ = status == "declined" ? "THE REGISTRATION WAS DECLINED FROM THE EMAIL" : "THE LINK AND CODE HAVE EXPIRED, PRESS R FOR NEW ONES";
                else announce(status == "declined" ? "ONLINE REGISTRATION DECLINED" : "ONLINE REGISTRATION EXPIRED", glm::vec4(1.f, 0.7f, 0.5f, 1.f), 1.f);
            } else if (res.status == 404) {
                stats_.setPendingPoll("");   // the server no longer knows it
            }
            break;
        }
        case OnlineClient::Kind::Version:
            if (res.ok) {
                applyVersionInfo(res.body, true);
                if (std::FILE* f = std::fopen((prefDir_ + "version.json").c_str(), "wb")) { const std::string t = res.body.dump(); std::fwrite(t.data(), 1, t.size(), f); std::fclose(f); }
            }
            break;
        }
    }
}

// --- updates ---------------------------------------------------------------------
bool App::versionNewer(const std::string& a, const std::string& b) {
    int av[3] = {0, 0, 0}, bv[3] = {0, 0, 0};
    std::sscanf(a.c_str(), "%d.%d.%d", &av[0], &av[1], &av[2]);
    std::sscanf(b.c_str(), "%d.%d.%d", &bv[0], &bv[1], &bv[2]);
    for (int i = 0; i < 3; ++i) { if (av[i] != bv[i]) return av[i] > bv[i]; }
    return false;
}

void App::checkVersion() {
    if (!OnlineClient::available() || !onlineOn_) return;
    online_.version();
}

// A version reply (from the network or the cached file) becomes the title's notice and the WHAT'S NEW list.
void App::applyVersionInfo(const Json& info, bool fromNetwork) {
    Json j = info;
    if (!fromNetwork) {
        std::string text;
        if (std::FILE* f = std::fopen((prefDir_ + "version.json").c_str(), "rb")) { char buf[4096]; size_t n; while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n); std::fclose(f); }
        if (text.empty() || !Json::parse(text, j)) return;
    }
    if (const char* demo = std::getenv("REDLINE_UPDATE_DEMO")) j.set("latest", std::string(demo));   // test knob: pretend this version is out
    latestVersion_ = j["latest"].asString();
    latestUrl_ = j["url"].asString("https://github.com/djanice1980/Doom_Redline/releases/latest");
    changelog_ = j["changelog"];
    const bool was = updateAvailable_;
    updateAvailable_ = !latestVersion_.empty() && versionNewer(latestVersion_, REDLINE_VERSION);
    if (mode_ == Mode::Title && menu_.items.size() > 5) menu_.items[5] = updateAvailable_ ? "WHAT'S NEW: VERSION " + latestVersion_ : "WHAT'S NEW";
    if (updateAvailable_ && !was && fromNetwork) { announce("VERSION " + latestVersion_ + " IS OUT: SEE WHAT'S NEW ON THE TITLE SCREEN", glm::vec4(0.8f, 1.f, 0.8f, 1.f), 1.f); play("pickup_item", 0.8f); }
}

void App::trophy(const char* id) {
    if (!trophies_.unlock(id)) return;
    if (mode_ != Mode::Title) run_.trophies.push_back(id);
    for (const TrophyDef& d : trophyCatalogue())
        if (std::string(id) == d.id) { showTrophy(d, false); std::fprintf(stderr, "[app] trophy unlocked: %s\n", d.name); }
    // The ultimate trophy: everything else in the cabinet.
    if (std::string(id) != "rip_and_tear" && !trophies_.unlocked("rip_and_tear") && trophies_.unlockedCount() == trophies_.total() - 1) {
        trophies_.unlock("rip_and_tear");
        for (const TrophyDef& d : trophyCatalogue()) if (std::string("rip_and_tear") == d.id) showTrophy(d, true);
        std::fprintf(stderr, "[app] trophy unlocked: RIP AND TEAR!!!\n");
        startCelebration();
    }
}

// The console-style toast: queued, one card at a time, with its jingle when it appears.
void App::showTrophy(const TrophyDef& d, bool ultimate) {
    Toast t;
    t.name = d.name;
    t.desc = d.description;
    t.count = trophies_.unlockedCount();
    t.total = trophies_.total();
    t.ultimate = ultimate;
    toasts_.push_back(t);
    if (toasts_.size() == 1) { play("pickup_weapon", 1.f, 1.3f); rumble(0.3f, 0.6f, 200); }
}

// RIP AND TEAR: the cabinet is full. Fanfare, a shake, fireworks and confetti over
// whatever is happening, for eight seconds, without taking the controls away.
void App::startCelebration() {
    celebrateT_ = 0.f;
    celebrateNext_ = 0.f;
    play("levelup", 1.f, 0.8f);
    play("bfg", 0.8f);
    shakeT_ = 0.8f;
    rumble(1.f, 1.f, 900);
}

// --- gamepad -----------------------------------------------------------------
void App::openGamepad(uint32_t which) {
    if (pad_) return;
    pad_ = SDL_OpenGamepad(which);
    if (!pad_) return;
    const char* n = SDL_GetGamepadName(pad_);
    padName_ = n ? n : "GAMEPAD";
    stats_.setPadModel(padName_);
    std::fprintf(stderr, "[pad] connected: %s\n", padName_.c_str());
    announce("GAMEPAD: " + padName_, glm::vec4(0.8f, 0.9f, 1.f, 1.f), 0.9f);
}

void App::rumble(float low, float high, int ms) {
    if (pad_ && padRumble_) SDL_RumbleGamepad(pad_, static_cast<Uint16>(std::clamp(low, 0.f, 1.f) * 65535.f), static_cast<Uint16>(std::clamp(high, 0.f, 1.f) * 65535.f), static_cast<Uint32>(ms));
}

void App::padButton(int button, bool down) {
    if (down) stats_.addInput(InputDevice::Gamepad);
    auto key = [&](SDL_Keycode k) { screenKey(k, true); };
    if (screen_ != kScreenNone) {
        if (!down) return;
        switch (button) {
        case SDL_GAMEPAD_BUTTON_DPAD_UP: key(SDLK_UP); break;
        case SDL_GAMEPAD_BUTTON_DPAD_DOWN: key(SDLK_DOWN); break;
        case SDL_GAMEPAD_BUTTON_DPAD_LEFT: key(SDLK_LEFT); break;
        case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: key(SDLK_RIGHT); break;
        case SDL_GAMEPAD_BUTTON_SOUTH: key(SDLK_RETURN); break;
        case SDL_GAMEPAD_BUTTON_EAST: key(SDLK_BACKSPACE); break;
        case SDL_GAMEPAD_BUTTON_START: key(SDLK_TAB); break;      // name entry: confirm
        case SDL_GAMEPAD_BUTTON_BACK: key(SDLK_ESCAPE); break;
        case SDL_GAMEPAD_BUTTON_WEST: if (screen_ == kScreenProfiles) key(SDLK_R); break;        // X: rename
        case SDL_GAMEPAD_BUTTON_NORTH: if (screen_ == kScreenProfiles) key(SDLK_DELETE); break;  // Y: delete
        default: break;
        }
        return;
    }
    if (!menu_.items.empty()) {
        if (!down) return;
        switch (button) {
        case SDL_GAMEPAD_BUTTON_DPAD_UP: menuKey(SDLK_UP); break;
        case SDL_GAMEPAD_BUTTON_DPAD_DOWN: menuKey(SDLK_DOWN); break;
        case SDL_GAMEPAD_BUTTON_SOUTH: menuKey(SDLK_RETURN); break;
        case SDL_GAMEPAD_BUTTON_EAST: case SDL_GAMEPAD_BUTTON_START:
            if (mode_ == Mode::Paused) enterMode(pausedFrom_);
            break;
        default: break;
        }
        return;
    }
    if (button == SDL_GAMEPAD_BUTTON_START && down) {
        if (mode_ == Mode::Blocks || mode_ == Mode::Fps || mode_ == Mode::Countdown) { pausedFrom_ = mode_; enterMode(Mode::Paused); }
        return;
    }
    if (mode_ == Mode::Blocks) {
        switch (button) {
        case SDL_GAMEPAD_BUTTON_DPAD_LEFT:
            if (down) { keys_.left = true; keys_.dasDir = -1; keys_.dasT = 0.f; keys_.dasActive = false; game_->moveLeft(); }
            else { keys_.left = false; if (keys_.dasDir == -1) keys_.dasDir = keys_.right ? 1 : 0; }
            break;
        case SDL_GAMEPAD_BUTTON_DPAD_RIGHT:
            if (down) { keys_.right = true; keys_.dasDir = 1; keys_.dasT = 0.f; keys_.dasActive = false; game_->moveRight(); }
            else { keys_.right = false; if (keys_.dasDir == 1) keys_.dasDir = keys_.left ? -1 : 0; }
            break;
        case SDL_GAMEPAD_BUTTON_DPAD_DOWN: keys_.down = down; game_->setSoftDrop(down || padHeld_.down); break;
        case SDL_GAMEPAD_BUTTON_SOUTH: case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: if (down) game_->rotateCW(); break;
        case SDL_GAMEPAD_BUTTON_EAST: case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: if (down) game_->rotateCCW(); break;
        case SDL_GAMEPAD_BUTTON_WEST: case SDL_GAMEPAD_BUTTON_DPAD_UP: if (down) game_->hardDrop(); break;
        default: break;
        }
    } else if (mode_ == Mode::Fps) {
        switch (button) {
        case SDL_GAMEPAD_BUTTON_SOUTH: fpsIn_.fire = down; break;
        case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: case SDL_GAMEPAD_BUTTON_NORTH: if (down) fpsIn_.wheel += 1; break;
        case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: case SDL_GAMEPAD_BUTTON_WEST: if (down) fpsIn_.wheel -= 1; break;
        case SDL_GAMEPAD_BUTTON_LEFT_STICK: fpsIn_.run = down; break;
        default: break;
        }
    }
}

void App::pollGamepad(float dt) {
    if (!pad_) return;
    auto axis = [&](SDL_GamepadAxis a) {
        float v = static_cast<float>(SDL_GetGamepadAxis(pad_, a)) / 32767.f;
        const float dz = 0.18f;
        if (std::fabs(v) < dz) return 0.f;
        float m = (std::fabs(v) - dz) / (1.f - dz);
        return std::copysign(std::min(1.f, m), v);
    };
    float lx = axis(SDL_GAMEPAD_AXIS_LEFTX), ly = axis(SDL_GAMEPAD_AXIS_LEFTY);
    float rx = axis(SDL_GAMEPAD_AXIS_RIGHTX), ry = axis(SDL_GAMEPAD_AXIS_RIGHTY);
    float lt = static_cast<float>(SDL_GetGamepadAxis(pad_, SDL_GAMEPAD_AXIS_LEFT_TRIGGER)) / 32767.f;
    float rt = static_cast<float>(SDL_GetGamepadAxis(pad_, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)) / 32767.f;
    if (mode_ == Mode::Blocks && screen_ == kScreenNone) {
        // Left stick acts like the d-pad with hysteresis so DAS behaves.
        bool left = padHeld_.left ? lx < -0.35f : lx < -0.55f;
        bool right = padHeld_.right ? lx > 0.35f : lx > 0.55f;
        if (left != padHeld_.left) { padHeld_.left = left; padButton(SDL_GAMEPAD_BUTTON_DPAD_LEFT, left); }
        if (right != padHeld_.right) { padHeld_.right = right; padButton(SDL_GAMEPAD_BUTTON_DPAD_RIGHT, right); }
        bool downNow = padHeld_.down ? ly > 0.35f : ly > 0.55f;
        if (downNow != padHeld_.down) { padHeld_.down = downNow; game_->setSoftDrop(downNow || keys_.down); }
    } else if (mode_ == Mode::Fps || mode_ == Mode::Countdown) {
        // Dual analogue: left stick moves, right stick looks (squared response), triggers fire and sprint.
        fpsIn_.padMoveX = lx;
        fpsIn_.padMoveZ = -ly;
        float cx = rx * std::fabs(rx), cy = ry * std::fabs(ry);
        const float yawRate = 3.4f * padSens_, pitchRate = 2.2f * padSens_;   // rad/s at full deflection
        fpsIn_.dx += cx * yawRate / 0.0022f * dt;
        fpsIn_.dy += (padInvertY_ ? -cy : cy) * pitchRate / 0.0022f * dt;
        bool fireNow = rt > 0.5f;
        if (fireNow != padHeld_.fire) { padHeld_.fire = fireNow; fpsIn_.padFire = fireNow; }
        fpsIn_.padRun = lt > 0.5f;
    }
}

// --- overlay screens ---------------------------------------------------------
void App::openScreen(int screen) {
    screen_ = screen;
    screenIndex_ = 0;
    optionsScroll_ = 0;
    optionsFollow_ = true;
    if (screen == kScreenProfiles) loadProfileList();
    if (screen == kScreenNameEntry) { nameEntry_.clear(); nameChar_ = 0; nameRename_ = false; wadStatus_.clear(); SDL_StartTextInput(window_); }
    if (screen == kScreenWadPath) { wadEntry_ = Assets::savedWadPath(prefDir_); wadStatus_.clear(); SDL_StartTextInput(window_); }
    if (screen == kScreenEmailEntry) { emailEntry_ = stats_.email(); wadStatus_.clear(); SDL_StartTextInput(window_); }
    if (screen == kScreenCode) { codeEntry_.clear(); codeChar_ = 0; wadStatus_.clear(); SDL_StartTextInput(window_); }
    if (screen == kScreenRegister) wadStatus_.clear();
    if (screen == kScreenWadSetup) wadStatus_.clear();
}

void App::closeScreen() {
    if (screen_ == kScreenNameEntry || screen_ == kScreenWadPath || screen_ == kScreenEmailEntry || screen_ == kScreenCode) SDL_StopTextInput(window_);
    if (nameRequired_ && profileName_.empty()) { openScreen(kScreenNameEntry); return; }   // no dodging the name
    if (profileRequired_) { openScreen(kScreenProfiles); return; }                        // nor the choice of player
    screen_ = kScreenNone;
}

void App::adjustOption(int dir) {
    switch (screenIndex_) {
    case 0: {   // music set
        for (int i = 0; i < 3; ++i) { cycleMusicSet(); if (dir > 0) break; }   // cycling backwards = two forward steps
        break;
    }
    case 1: music_.setVolume(std::clamp(music_.volume() + 0.05f * static_cast<float>(dir), 0.f, 1.f)); break;
    case 2: sfxVolume_ = std::clamp(sfxVolume_ + 0.05f * static_cast<float>(dir), 0.f, 1.f); audio_.setMasterGain(sfxVolume_); break;
    case 3: music_.setEnabled(!music_.enabled()); break;
    case 4: padSens_ = std::clamp(padSens_ + 0.1f * static_cast<float>(dir), 0.3f, 3.f); break;
    case 5: padInvertY_ = !padInvertY_; break;
    case 6: padRumble_ = !padRumble_; if (padRumble_) rumble(0.5f, 0.5f, 150); break;
    case 7: displayMode_ = (displayMode_ + 3 + dir) % 3; applyDisplay(); break;
    case 9: if (voxels_.available()) useVoxels_ = !useVoxels_; break;
    case 10: if (dir > 0) browseForWad(); break;
    case 11: if (dir > 0) browseForWad(true); break;
    case 12: if (dir > 0) openScreen(kScreenEmailEntry); break;
    case 13:   // ONLINE: opt in per profile; turning it on with an unverified address starts the registration
        if (!OnlineClient::available()) break;
        onlineOn_ = !onlineOn_;
        if (onlineOn_ && !stats_.email().empty() && !stats_.emailVerified()) startRegistration();
        else if (onlineOn_ && stats_.email().empty()) openScreen(kScreenEmailEntry);
        else if (onlineOn_) submitPendingRuns();
        break;
    case 14: setDoomArt(doomArtOff_); break;   // toggles
    case 15: if (assets_.usingWad()) { brutal_ = !brutal_; fps_.setBrutal(brutalActive()); saveSettings(); } break;   // sub-option of DOOM ART
    case 16: dungeon_ = !dungeon_; fps_.setDungeonEnabled(dungeon_); saveSettings(); break;   // takes effect from the next fight
    case 17: if (renderer_->rayTracingAvailable()) { rtShadows_ = (rtShadows_ + 4 + dir) % 4; saveDisplaySettings(); } break;
    case 18: if (renderer_->msaaAvailable()) { msaa_ = !msaa_; renderer_->setMsaa(msaa_); saveDisplaySettings(); } break;
    case 19: bloom_ = !bloom_; saveDisplaySettings(); break;
    case 8: {
        if (resolutions_.empty()) break;
        int idx = 0;
        for (size_t i = 0; i < resolutions_.size(); ++i) if (resolutions_[i].first == resW_ && resolutions_[i].second == resH_) idx = static_cast<int>(i);
        int n = static_cast<int>(resolutions_.size());
        idx = (idx + n - dir) % n;   // list is largest-first, so "right" steps to a larger size
        resW_ = resolutions_[static_cast<size_t>(idx)].first;
        resH_ = resolutions_[static_cast<size_t>(idx)].second;
        if (displayMode_ != 1) applyDisplay(); else saveDisplaySettings();
        break;
    }
    default: break;
    }
    saveSettings();
    play("menu", 0.6f);
}

void App::screenKey(int key, bool fromPad) {
    static const std::string kLetters = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 ";
    switch (screen_) {
    case kScreenOptions: {
        const int n = 21;   // 20 options + BACK (the last index)
        const int back = n - 1;
        if (key == SDLK_UP) { screenIndex_ = (screenIndex_ + n - 1) % n; optionsFollow_ = true; play("menu", 0.6f); }
        else if (key == SDLK_DOWN) { screenIndex_ = (screenIndex_ + 1) % n; optionsFollow_ = true; play("menu", 0.6f); }
        else if (key == SDLK_LEFT) { if (screenIndex_ < back) adjustOption(-1); }
        else if (key == SDLK_RIGHT) { if (screenIndex_ < back) adjustOption(1); }
        else if (key == SDLK_RETURN || key == SDLK_SPACE) { if (screenIndex_ == back) closeScreen(); else adjustOption(1); }
        else if (key == SDLK_ESCAPE || key == SDLK_BACKSPACE) closeScreen();
        break;
    }
    case kScreenTrophies:
    case kScreenCredits:
        if (key == SDLK_ESCAPE || key == SDLK_RETURN || key == SDLK_BACKSPACE || key == SDLK_SPACE) closeScreen();
        break;
    case kScreenWadSetup: {
        const int n = 4;   // browse, type, placeholder, quit
        if (key == SDLK_UP) { screenIndex_ = (screenIndex_ + n - 1) % n; play("menu", 0.6f); }
        else if (key == SDLK_DOWN) { screenIndex_ = (screenIndex_ + 1) % n; play("menu", 0.6f); }
        else if (key == SDLK_RETURN || key == SDLK_SPACE) {
            play("menu_select", 0.7f);
            if (screenIndex_ == 0) browseForWad();
            else if (screenIndex_ == 1) openScreen(kScreenWadPath);
            else if (screenIndex_ == 2) closeScreen();
            else running_ = false;
        }
        else if (key == SDLK_ESCAPE) closeScreen();
        break;
    }
    case kScreenEmailEntry: {
        if (key == SDLK_RETURN || key == SDLK_KP_ENTER) {
            std::string e = emailEntry_;
            size_t at = e.find('@');
            bool ok = e.empty() || (at != std::string::npos && at > 0 && e.find('.', at) != std::string::npos && e.find('.', at) < e.size() - 1);
            if (!ok) { wadStatus_ = "THAT DOES NOT LOOK LIKE AN EMAIL ADDRESS"; play("menu", 0.6f); return; }
            stats_.setEmail(e);
            play("menu_select", 0.7f);
            closeScreen();
            if (onlineOn_ && !e.empty() && !stats_.emailVerified()) startRegistration();
        }
        else if (key == SDLK_BACKSPACE) { if (!emailEntry_.empty()) emailEntry_.pop_back(); }
        else if (key == SDLK_ESCAPE) closeScreen();
        break;
    }
    case kScreenRegister: {
        if (registerBusy_) break;
        if (key == SDLK_RETURN || key == SDLK_KP_ENTER || key == SDLK_SPACE) {
            registerBusy_ = true;
            wadStatus_ = "SENDING THE CODE...";
            online_.registerEmail(stats_.playerId(), machine_.installId, registerEmail_, profileName_, machineLabel(), REDLINE_VERSION);
            play("menu_select", 0.7f);
        }
        else if (key == SDLK_ESCAPE || key == SDLK_BACKSPACE) closeScreen();
        break;
    }
    case kScreenCode: {
        if (registerBusy_) break;
        if (fromPad) {   // digit picker, like the name screen
            if (key == SDLK_UP || key == SDLK_RIGHT) { codeChar_ = (codeChar_ + 1) % 10; return; }
            if (key == SDLK_DOWN || key == SDLK_LEFT) { codeChar_ = (codeChar_ + 9) % 10; return; }
            if (key == SDLK_RETURN) { if (codeEntry_.size() < 6) codeEntry_.push_back(static_cast<char>('0' + codeChar_)); play("menu", 0.6f); return; }
            if (key == SDLK_BACKSPACE) { if (!codeEntry_.empty()) codeEntry_.pop_back(); return; }
            if (key == SDLK_TAB) key = SDLK_KP_ENTER;
            else if (key == SDLK_ESCAPE) { closeScreen(); return; }
        }
        if (key == SDLK_KP_ENTER || (!fromPad && key == SDLK_RETURN)) {
            if (codeEntry_.size() != 6) { wadStatus_ = "THE CODE HAS SIX DIGITS"; play("menu", 0.6f); return; }
            registerBusy_ = true;
            wadStatus_ = "CHECKING...";
            online_.confirm(stats_.playerId(), codeEntry_);
        }
        else if (!fromPad && key == SDLK_BACKSPACE) { if (!codeEntry_.empty()) codeEntry_.pop_back(); }
        else if (key == SDLK_R) { registerBusy_ = true; wadStatus_ = "SENDING A NEW CODE..."; online_.registerEmail(stats_.playerId(), machine_.installId, registerEmail_, profileName_, machineLabel(), REDLINE_VERSION); }
        else if (!fromPad && key == SDLK_ESCAPE) closeScreen();
        break;
    }
    case kScreenOnlineAsk: {
        const int n = 3;
        if (key == SDLK_UP) { screenIndex_ = (screenIndex_ + n - 1) % n; play("menu", 0.6f); }
        else if (key == SDLK_DOWN) { screenIndex_ = (screenIndex_ + 1) % n; play("menu", 0.6f); }
        else if (key == SDLK_RETURN || key == SDLK_SPACE || key == SDLK_KP_ENTER) {
            play("menu_select", 0.7f);
            onlineAsked_ = true;
            if (screenIndex_ == 0) { onlineOn_ = true; saveSettings(); screen_ = kScreenNone; openScreen(kScreenEmailEntry); }
            else if (screenIndex_ == 1) { saveSettings(); closeScreen(); }
            else { onlineOn_ = false; saveSettings(); closeScreen(); }
        }
        else if (key == SDLK_ESCAPE || key == SDLK_BACKSPACE) { onlineAsked_ = true; saveSettings(); closeScreen(); }
        break;
    }
    case kScreenAdopt: {
        // The account already has players: carry on as one, or start fresh.
        const int n = static_cast<int>(adoptChoices_.size()) + 1;   // + START FRESH
        if (adoptBusy_) break;
        if (key == SDLK_UP) { screenIndex_ = (screenIndex_ + n - 1) % n; play("menu", 0.6f); }
        else if (key == SDLK_DOWN) { screenIndex_ = (screenIndex_ + 1) % n; play("menu", 0.6f); }
        else if (key == SDLK_RETURN || key == SDLK_SPACE || key == SDLK_KP_ENTER) {
            play("menu_select", 0.7f);
            if (screenIndex_ < static_cast<int>(adoptChoices_.size())) {
                adoptBusy_ = true;
                wadStatus_.clear();
                online_.adopt(stats_.token(), adoptChoices_[static_cast<size_t>(screenIndex_)].id);
            } else {   // a different person on the same address: leave the old player alone
                adoptChoices_.clear();
                closeScreen();
                submitPendingRuns();
            }
        }
        else if (key == SDLK_ESCAPE || key == SDLK_BACKSPACE) { adoptChoices_.clear(); closeScreen(); submitPendingRuns(); }
        break;
    }
    case kScreenDeleteOnline: {
        const int n = 3;
        if (key == SDLK_UP) { screenIndex_ = (screenIndex_ + n - 1) % n; play("menu", 0.6f); }
        else if (key == SDLK_DOWN) { screenIndex_ = (screenIndex_ + 1) % n; play("menu", 0.6f); }
        else if (key == SDLK_RETURN || key == SDLK_SPACE || key == SDLK_KP_ENTER) {
            play("menu_select", 0.7f);
            const std::string name = deleteProfileName_;
            if (screenIndex_ == 1) {
                // The token lives in the folder about to go: write the deletion down first.
                queuePlayerDelete(deletePlayerId_, deleteToken_);
            }
            deleteProfileName_.clear(); deletePlayerId_.clear(); deleteToken_.clear();
            if (screenIndex_ == 2) { closeScreen(); break; }
            closeScreen();
            deleteProfile(name);
            sendPendingDeletes();
        }
        else if (key == SDLK_ESCAPE || key == SDLK_BACKSPACE) { deleteProfileName_.clear(); deletePlayerId_.clear(); deleteToken_.clear(); closeScreen(); }
        break;
    }
    case kScreenWhatsNew: {
        if (key == SDLK_UP) whatsNewScroll_ = std::max(0, whatsNewScroll_ - 1);
        else if (key == SDLK_DOWN) ++whatsNewScroll_;   // clamped when drawn
        else if (key == SDLK_RETURN || key == SDLK_SPACE || key == SDLK_KP_ENTER) { SDL_OpenURL(latestUrl_.c_str()); play("menu_select", 0.7f); }
        else if (key == SDLK_ESCAPE || key == SDLK_BACKSPACE) closeScreen();
        break;
    }
    case kScreenLeaderboard: {
        static const char* boards[] = {"global", "week", "fights", "level", "kills"};
        if (key == SDLK_LEFT) openLeaderboard((leaderboardTab_ + 4) % 5);
        else if (key == SDLK_RIGHT) openLeaderboard((leaderboardTab_ + 1) % 5);
        else if (key == SDLK_R || key == SDLK_RETURN || key == SDLK_SPACE) openLeaderboard(leaderboardTab_);
        else if (key == SDLK_ESCAPE || key == SDLK_BACKSPACE) closeScreen();
        (void)boards;
        break;
    }
    case kScreenWadPath: {
        if (key == SDLK_RETURN || key == SDLK_KP_ENTER) {
            std::string p = wadEntry_;
            while (!p.empty() && p.back() == ' ') p.pop_back();
            if (!p.empty() && p.front() == '"' && p.back() == '"' && p.size() > 1) p = p.substr(1, p.size() - 2);
            std::error_code ec;
            if (p.empty()) return;
            if (!std::filesystem::is_regular_file(p, ec)) { wadStatus_ = "FILE NOT FOUND"; play("menu", 0.6f); return; }
            if (!reloadAssets(p)) wadStatus_ = "NOT A DOOM WAD";
        }
        else if (key == SDLK_BACKSPACE) { if (!wadEntry_.empty()) wadEntry_.pop_back(); }
        else if (key == SDLK_ESCAPE) { SDL_StopTextInput(window_); if (wadMissing_) openScreen(kScreenWadSetup); else closeScreen(); }
        break;
    }
    case kScreenProfiles: {
        int n = static_cast<int>(profileList_.size()) + 1;   // + NEW PLAYER
        const bool onProfile = screenIndex_ < static_cast<int>(profileList_.size());
        if (profileConfirmDelete_ >= 0 && key != SDLK_RETURN && key != SDLK_DELETE && key != SDLK_SPACE) profileConfirmDelete_ = -1;   // anything else cancels
        if (key == SDLK_UP) { screenIndex_ = (screenIndex_ + n - 1) % n; play("menu", 0.6f); }
        else if (key == SDLK_DOWN) { screenIndex_ = (screenIndex_ + 1) % n; play("menu", 0.6f); }
        else if (key == SDLK_RETURN || key == SDLK_SPACE) {
            if (profileConfirmDelete_ == screenIndex_ && onProfile) { profileAction(2); break; }   // second ENTER confirms the delete
            play("menu_select", 0.7f);
            if (onProfile) {
                switchProfile(profileList_[static_cast<size_t>(screenIndex_)]);
                nameRequired_ = false;
                profileRequired_ = false;
                if (mode_ == Mode::Title && menu_.items.size() > 1) menu_.items[1] = "PLAYER: " + profileName_;
                closeScreen();
                if (screen_ == kScreenNone && OnlineClient::available() && !onlineAsked_ && !stats_.emailVerified() && stats_.pendingPoll().empty()) openScreen(kScreenOnlineAsk);
            }
            else openScreen(kScreenNameEntry);
        }
        else if (key == SDLK_R && onProfile) profileAction(0);
        else if (key == SDLK_E && onProfile) profileAction(1);
        else if (key == SDLK_DELETE && onProfile) profileAction(2);
        else if (key == SDLK_ESCAPE || key == SDLK_BACKSPACE) { if (!profileRequired_) closeScreen(); else play("menu", 0.4f); }
        break;
    }
    case kScreenNameEntry: {
        if (fromPad) {
            // Letter picker: up/down scrolls the letter, A adds it, B deletes, Start confirms.
            int n = static_cast<int>(kLetters.size());
            if (key == SDLK_UP || key == SDLK_RIGHT) { nameChar_ = (nameChar_ + 1) % n; return; }
            if (key == SDLK_DOWN || key == SDLK_LEFT) { nameChar_ = (nameChar_ + n - 1) % n; return; }
            if (key == SDLK_RETURN) { if (nameEntry_.size() < 12) nameEntry_.push_back(kLetters[static_cast<size_t>(nameChar_)]); play("menu", 0.6f); return; }
            if (key == SDLK_BACKSPACE) { if (!nameEntry_.empty()) nameEntry_.pop_back(); return; }
            if (key == SDLK_TAB) key = SDLK_KP_ENTER;   // confirm
            else if (key == SDLK_ESCAPE) { if (!nameRequired_ || !profileName_.empty()) closeScreen(); return; }
        }
        if (key == SDLK_KP_ENTER || (!fromPad && key == SDLK_RETURN)) {
            std::string name = nameEntry_;
            while (!name.empty() && name.back() == ' ') name.pop_back();
            if (name.empty()) return;
            for (char& c : name) if (c == ' ') c = '_';
            if (nameRename_) {   // renaming an existing player: back to the list either way
                if (renameProfile(renameFrom_, name)) { play("menu_select", 0.8f); nameRename_ = false; SDL_StopTextInput(window_); openScreen(kScreenProfiles); }
                else play("menu", 0.6f);
                return;
            }
            SDL_StopTextInput(window_);
            screen_ = kScreenNone;
            nameRequired_ = false;
            profileRequired_ = false;
            switchProfile(name);
            play("menu_select", 0.8f);
            announce("WELCOME, " + name, glm::vec4(0.8f, 0.9f, 1.f, 1.f), 1.2f);
            if (mode_ == Mode::Title) menu_.items[1] = "PLAYER: " + name;
            onlineAsked_ = true;
            saveSettings();
            openScreen(kScreenEmailEntry);   // the leaderboard question: an address here starts the registration
        } else if (!fromPad && key == SDLK_BACKSPACE) { if (!nameEntry_.empty()) nameEntry_.pop_back(); }
        else if (!fromPad && key == SDLK_ESCAPE) { if (!nameRequired_ || !profileName_.empty()) closeScreen(); }
        break;
    }
    default:
        break;
    }
}

void App::beginLevelCard() {
    run_.damage += fps_.damageTaken();
    levelCardT_ = 0.f;
    levelCard_.level = game_->level();
    levelCard_.kills = fightStats_.kills;
    levelCard_.blocks = fightStats_.blocks;
    levelCard_.seconds = fps_.elapsed();
    levelCard_.damage = fps_.damageTaken();
    levelCard_.score = game_->score();
    play("levelup", 1.f);
    play("pickup_weapon", 0.8f, 0.9f);
    rumble(0.4f, 0.8f, 400);
    shakeT_ = 0.3f;
}

// --- display -----------------------------------------------------------------
void App::loadDisplaySettings() {
    char* pref = SDL_GetPrefPath("redline", "redline");
    std::string base = pref ? pref : "";
    if (pref) SDL_free(pref);
    if (std::FILE* f = std::fopen((base + "display.txt").c_str(), "r")) {
        char key[64], val[64];
        while (std::fscanf(f, "%63[^=]=%63s\n", key, val) == 2) {
            std::string k = key, v = val;
            if (k == "mode") displayMode_ = std::clamp(std::atoi(v.c_str()), 0, 2);
            else if (k == "width") resW_ = std::max(640, std::atoi(v.c_str()));
            else if (k == "rt_shadows") rtShadows_ = std::clamp(std::atoi(v.c_str()), 0, 3);
            else if (k == "msaa") msaa_ = std::atoi(v.c_str()) != 0;
            else if (k == "bloom") bloom_ = std::atoi(v.c_str()) != 0;
            else if (k == "height") resH_ = std::max(360, std::atoi(v.c_str()));
        }
        std::fclose(f);
    }
}

void App::saveDisplaySettings() const {
    char* pref = SDL_GetPrefPath("redline", "redline");
    std::string base = pref ? pref : "";
    if (pref) SDL_free(pref);
    if (std::FILE* f = std::fopen((base + "display.txt").c_str(), "w")) {
        std::fprintf(f, "mode=%d\nwidth=%d\nheight=%d\nrt_shadows=%d\nmsaa=%d\nbloom=%d\n", displayMode_, resW_, resH_, rtShadows_, msaa_ ? 1 : 0, bloom_ ? 1 : 0);
        std::fclose(f);
    }
}

const char* App::displayModeName() const {
    switch (displayMode_) {
    case 1: return "FULLSCREEN (BORDERLESS)";
    case 2: return "FULLSCREEN (EXCLUSIVE)";
    default: return "WINDOWED";
    }
}

// Distinct sizes the display can do, largest first, plus common windowed
// sizes that fit on the desktop.
void App::buildResolutionList() {
    resolutions_.clear();
    SDL_DisplayID display = SDL_GetDisplayForWindow(window_);
    if (!display) display = SDL_GetPrimaryDisplay();
    int count = 0;
    SDL_DisplayMode** modes = SDL_GetFullscreenDisplayModes(display, &count);
    if (modes) {
        for (int i = 0; i < count; ++i) {
            std::pair<int, int> r{modes[i]->w, modes[i]->h};
            if (r.first < 640 || r.second < 360) continue;
            bool dup = false;
            for (auto& e : resolutions_) dup = dup || e == r;
            if (!dup) resolutions_.push_back(r);
        }
        SDL_free(modes);
    }
    const SDL_DisplayMode* desktop = SDL_GetDesktopDisplayMode(display);
    const std::pair<int, int> common[] = {{1280, 720}, {1600, 900}, {1920, 1080}, {2560, 1440}, {3440, 1440}, {3840, 2160}};
    for (auto r : common) {
        if (desktop && (r.first > desktop->w || r.second > desktop->h)) continue;
        bool dup = false;
        for (auto& e : resolutions_) dup = dup || e == r;
        if (!dup) resolutions_.push_back(r);
    }
    std::sort(resolutions_.begin(), resolutions_.end(), [](auto& a, auto& b) { return a.first != b.first ? a.first > b.first : a.second > b.second; });
    bool have = false;
    for (auto& e : resolutions_) have = have || (e.first == resW_ && e.second == resH_);
    if (!have) resolutions_.insert(resolutions_.begin(), {resW_, resH_});
}

void App::applyDisplay() {
    SDL_DisplayID display = SDL_GetDisplayForWindow(window_);
    if (!display) display = SDL_GetPrimaryDisplay();
    if (displayMode_ == 0) {
        SDL_SetWindowFullscreen(window_, false);
        const float dens = pixelDensity();
        SDL_SetWindowSize(window_, static_cast<int>(std::lround(resW_ / dens)), static_cast<int>(std::lround(resH_ / dens)));
        SDL_SetWindowPosition(window_, SDL_WINDOWPOS_CENTERED_DISPLAY(display), SDL_WINDOWPOS_CENTERED_DISPLAY(display));
    } else if (displayMode_ == 1) {
        SDL_SetWindowFullscreenMode(window_, nullptr);   // borderless at the desktop resolution
        SDL_SetWindowFullscreen(window_, true);
    } else {
        // Exclusive: the display mode matching the chosen size, highest refresh rate.
        SDL_DisplayMode chosen{};
        bool found = false;
        int count = 0;
        SDL_DisplayMode** modes = SDL_GetFullscreenDisplayModes(display, &count);
        if (modes) {
            for (int i = 0; i < count; ++i)
                if (modes[i]->w == resW_ && modes[i]->h == resH_ && (!found || modes[i]->refresh_rate > chosen.refresh_rate)) { chosen = *modes[i]; found = true; }
            SDL_free(modes);
        }
        if (found) SDL_SetWindowFullscreenMode(window_, &chosen);
        else SDL_SetWindowFullscreenMode(window_, nullptr);
        SDL_SetWindowFullscreen(window_, true);
        if (!found) std::fprintf(stderr, "[display] no exclusive mode at %dx%d; using borderless\n", resW_, resH_);
    }
    SDL_SyncWindow(window_);
    if (ctx_) ctx_->requestResize();   // at startup the context is created after this and sizes itself from the window
    int pw = 0, ph = 0, lw = 0, lh2 = 0;
    SDL_GetWindowSizeInPixels(window_, &pw, &ph);
    SDL_GetWindowSize(window_, &lw, &lh2);
    std::fprintf(stderr, "[display] %s %dx%d: window %dx%d points, %dx%d pixels (desktop scale %.2f)\n", displayModeName(), resW_, resH_, lw, lh2, pw, ph, pixelDensity());
    saveDisplaySettings();
}

float App::pixelDensity() const {
    if (!window_) return 1.f;
    const float d = SDL_GetWindowPixelDensity(window_);
    return d > 0.f ? d : 1.f;
}

void App::saveSettings() const {
    if (std::FILE* f = std::fopen(settingsPath_.c_str(), "w")) {
        std::fprintf(f, "music_set=%s\nmusic_on=%d\nmusic_volume=%.2f\nsfx_volume=%.2f\npad_sens=%.2f\npad_invert=%d\npad_rumble=%d\nvoxels=%d\ndoom_art=%d\nbrutal=%d\ndungeon=%d\nonline=%d\nonline_asked=%d\n",
                     musicSet_ == MusicSet::Classic ? "classic" : musicSet_ == MusicSet::Sc55 ? "sc55" : "modern", music_.enabled() ? 1 : 0,
                     music_.volume(), sfxVolume_, padSens_, padInvertY_ ? 1 : 0, padRumble_ ? 1 : 0, useVoxels_ ? 1 : 0, doomArtOff_ ? 0 : 1, brutal_ ? 1 : 0, dungeon_ ? 1 : 0, onlineOn_ ? 1 : 0, onlineAsked_ ? 1 : 0);
        std::fclose(f);
    }
}

const char* App::musicSetName() const {
    switch (musicSet_) {
    case MusicSet::Classic: return "CLASSIC";
    case MusicSet::Sc55: return "SC-55";
    default: return "MODERN";
    }
}

void App::cycleMusicSet() {
    bool haveModern = false, haveSc55 = false;
    for (const Assets::OggLump& l : assets_.oggMusic) { if (l.name[0] == 'H') haveModern = true; if (l.name[0] == 'O') haveSc55 = true; }
    if (!music_.oggAvailable()) haveModern = haveSc55 = false;
    for (int i = 0; i < 3; ++i) {
        musicSet_ = static_cast<MusicSet>((static_cast<int>(musicSet_) + 1) % 3);
        if (musicSet_ == MusicSet::Classic) break;
        if (musicSet_ == MusicSet::Sc55 && haveSc55) break;
        if (musicSet_ == MusicSet::Modern && haveModern) break;
    }
    saveSettings();
    announce(std::string("MUSIC: ") + musicSetName(), glm::vec4(0.8f, 0.8f, 1.f, 1.f), 0.9f);
    music_.stop(0.2f);
    updateMusic();
}

// Classic (D_) name -> the same piece in the chosen set, falling back to the
// classic MUS when the set lacks it.
std::string App::resolveTrack(const std::string& classicName) const {
    if (classicName.size() < 3) return classicName;
    std::string suffix = classicName.substr(2);
    if (musicSet_ == MusicSet::Modern && music_.hasTrack("H_" + suffix)) return "H_" + suffix;
    if (musicSet_ == MusicSet::Sc55 && music_.hasTrack("O_" + suffix)) return "O_" + suffix;
    return classicName;
}

// Music selection: title music on the menu, a stacking track per level, a
// fight track per red line, the ending music over the game-over screen.
std::string App::pickTrack(const std::vector<const char*>& prefs, int index) const {
    std::vector<std::string> avail;
    for (const char* p : prefs) {
        std::string r = resolveTrack(p);
        if (music_.hasTrack(r)) avail.push_back(r);
    }
    if (avail.empty()) return "";
    return avail[static_cast<size_t>(((index % static_cast<int>(avail.size())) + static_cast<int>(avail.size())) % static_cast<int>(avail.size()))];
}

void App::updateMusic() {
    static const std::vector<const char*> kTitleIntro = {"D_INTRO", "D_DM2TTL"};
    static const std::vector<const char*> kTitleLoop = {"D_INTER", "D_DM2INT", "D_VICTOR", "D_READ_M"};
    static const std::vector<const char*> kBlocks = {"D_E1M1", "D_E1M2", "D_E1M3", "D_E1M5", "D_E1M7", "D_E2M1", "D_E2M2", "D_E2M4", "D_E3M2", "D_E3M3", "D_E1M4", "D_E2M5",
                                                     "D_RUNNIN", "D_STALKS", "D_COUNTD", "D_BETWEE", "D_DOOM", "D_THE_DA", "D_SHAWN", "D_DDTBLU", "D_IN_CIT", "D_DEAD"};
    static const std::vector<const char*> kFight = {"D_E1M8", "D_E2M8", "D_E3M8", "D_E1M6", "D_E2M6", "D_E3M4", "D_E1M9", "D_E3M1",
                                                    "D_ROMERO", "D_ADRIAN", "D_MESSAG", "D_TENSE", "D_SHAWN2", "D_OPENIN"};
    static const std::vector<const char*> kGameOver = {"D_BUNNY", "D_DM2INT", "D_INTER"};
    if (!music_.enabled() && music_.trackNames().empty()) return;
    std::string want;
    bool loop = true;
    std::string next;
    switch (mode_) {
    case Mode::Title: {
        std::string intro = pickTrack(kTitleIntro, 0), loopTrack = pickTrack(kTitleLoop, 0);
        if (!intro.empty()) { want = intro; loop = false; next = loopTrack; } else want = loopTrack;
        // Already on the title sequence? Leave it alone.
        if (music_.current() == intro || music_.current() == loopTrack) return;
        break;
    }
    case Mode::Blocks:
        want = pickTrack(kBlocks, game_ ? game_->level() - 1 : 0);
        break;
    case Mode::Alert: case Mode::FlyIn: case Mode::Countdown: case Mode::Fps: case Mode::FlyOut: {
        want = pickTrack(kFight, game_ ? game_->redLineCount() - 1 : 0);
        if (want.empty()) return;
        for (const char* f : kFight) if (music_.current() == resolveTrack(f)) return;   // keep the fight track across the transitions
        break;
    }
    case Mode::GameOver:
        want = pickTrack(kGameOver, 0);
        loop = false;
        break;
    case Mode::Paused:
        return;
    }
    if (want.empty()) { music_.stop(); return; }
    if (music_.current() == want) return;
    music_.play(want, loop, next);
}

// ---------------------------------------------------------------------------
int App::run() {
    auto last = std::chrono::steady_clock::now();
    while (running_) {
        auto now = std::chrono::steady_clock::now();
        float dt = std::chrono::duration<float>(now - last).count();
        last = now;
        dt = std::min(dt, 0.05f);
        if (opts_.frames > 0) dt = 1.f / 60.f;   // deterministic when scripted
        time_ += dt;

        if (!opts_.keys.empty() && (frameCount_ == opts_.keysFrame || frameCount_ == opts_.keysFrame + opts_.keysHoldFrames)) {
            // Scripted chord: press the letters at one frame, release them later.
            bool down = frameCount_ == opts_.keysFrame;
            for (char ch : opts_.keys) {
                SDL_Event ev{};
                ev.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
                // Letters as themselves; '_' down, '^' up, '<' left, '>' right, '~' return, '`' escape, ' ' space.
                SDL_Keycode k = SDLK_UNKNOWN;
                if (std::isalpha(static_cast<unsigned char>(ch))) k = static_cast<SDL_Keycode>(SDLK_A + (std::toupper(static_cast<unsigned char>(ch)) - 'A'));
                else if (ch == '_') k = SDLK_DOWN; else if (ch == '^') k = SDLK_UP; else if (ch == '<') k = SDLK_LEFT; else if (ch == '>') k = SDLK_RIGHT;
                else if (ch == '#') k = SDLK_DELETE;
                else if (ch == '~') k = SDLK_RETURN; else if (ch == '`') k = SDLK_ESCAPE; else if (ch == ' ') k = SDLK_SPACE;
                if (k == SDLK_UNKNOWN) continue;
                ev.key.key = k;
                SDL_PushEvent(&ev);
            }
        }
        for (const Options::Click& c : opts_.clicks) {
            if (c.frame != frameCount_) continue;
            const float px = static_cast<float>(c.x) / pixelDensity(), py = static_cast<float>(c.y) / pixelDensity();   // --click is in pixels, events are in points
            SDL_Event mv{}; mv.type = SDL_EVENT_MOUSE_MOTION; mv.motion.x = px; mv.motion.y = py; SDL_PushEvent(&mv);
            SDL_Event dn{}; dn.type = SDL_EVENT_MOUSE_BUTTON_DOWN; dn.button.button = SDL_BUTTON_LEFT; dn.button.x = px; dn.button.y = py; SDL_PushEvent(&dn);
            SDL_Event up = dn; up.type = SDL_EVENT_MOUSE_BUTTON_UP; SDL_PushEvent(&up);
        }
        handleEvents();
        update(dt);
        if (stats_.loaded()) {
            bool blocks = mode_ == Mode::Blocks || mode_ == Mode::Alert || mode_ == Mode::FlyOut;
            bool fight = mode_ == Mode::FlyIn || mode_ == Mode::Countdown || mode_ == Mode::Fps;
            stats_.addTime(blocks ? PlayPhase::Blocks : fight ? PlayPhase::Fps : PlayPhase::Menu, dt);
            statsSaveT_ += dt;
            if (statsSaveT_ >= 30.f) { statsSaveT_ = 0.f; stats_.save(); }
        }
        audio_.update();
        buildScene();
        cubeRanges_ = envRanges_;
        if (cubes_.size() > envCubes_.size())
            cubeRanges_.push_back({static_cast<uint32_t>(envCubes_.size()), static_cast<uint32_t>(cubes_.size() - envCubes_.size()), 0});
        renderer_->render(frame_, cubes_, worldQuads_, screenQuads_, meshes_, cubeRanges_);
        ++frameCount_;
        if (!opts_.record.empty() && opts_.frames > 0 && frameCount_ % std::max(1, opts_.recordEvery) == 0) {   // the video recorder
            static std::FILE* rec = nullptr;
            if (!rec) rec = std::fopen(opts_.record.c_str(), "wb");
            std::vector<uint8_t> px; int w = 0, h = 0;
            if (rec && renderer_->readbackFrame(px, w, h)) {
                std::fwrite(px.data(), 1, px.size(), rec);
                if (frameCount_ == std::max(1, opts_.recordEvery)) std::fprintf(stderr, "[app] recording %dx%d raw RGBA frames to %s\n", w, h, opts_.record.c_str());
            }
            if (frameCount_ >= opts_.frames && rec) { std::fclose(rec); rec = nullptr; }
        }

        if (opts_.frames > 0 && frameCount_ >= opts_.frames) {
            if (!opts_.screenshot.empty()) {
                bool ok = renderer_->screenshot(opts_.screenshot);
                std::fprintf(stderr, "[app] screenshot %s: %s\n", opts_.screenshot.c_str(), ok ? "written" : "FAILED");
            }
            running_ = false;
        }
    }
    return 0;
}

void App::menuKey(int key) {
    int n = static_cast<int>(menu_.items.size());
    if (n == 0) return;
    if (key == SDLK_UP || key == SDLK_W) { menu_.index = (menu_.index + n - 1) % n; play("menu", 0.6f); }
    else if (key == SDLK_DOWN || key == SDLK_S) { menu_.index = (menu_.index + 1) % n; play("menu", 0.6f); }
    else if (key == SDLK_RETURN || key == SDLK_SPACE || key == SDLK_KP_ENTER) { play("menu_select", 0.7f); menuSelect(); }
}

void App::menuSelect() {
    const std::string& item = menu_.items[static_cast<size_t>(menu_.index)];
    if (item == "QUIT") running_ = false;
    else if (item == "OPTIONS") openScreen(kScreenOptions);
    else if (item == "TROPHIES") openScreen(kScreenTrophies);
    else if (item == "LEADERBOARD") openLeaderboard(leaderboardTab_);
    else if (item.rfind("WHAT'S NEW", 0) == 0) {
        whatsNewScroll_ = 0;
        if (versionAgeT_ > 60.f) { versionAgeT_ = 0.f; checkVersion(); }   // a release published since launch shows up here
        openScreen(kScreenWhatsNew);
    }
    else if (item == "CREDITS") openScreen(kScreenCredits);
    else if (item.rfind("PLAYER: ", 0) == 0) openScreen(kScreenProfiles);
    else if (item == "START") { if (profileName_.empty()) openScreen(kScreenNameEntry); else enterMode(Mode::Blocks); }
    else if (item == "RESUME") enterMode(pausedFrom_);
    else if (item == "RESTART") { newGame(); enterMode(Mode::Blocks); }
    else if (item == "MAIN MENU") { newGame(false); enterMode(Mode::Title); }
}

void App::handleEvents() {
    SDL_Event e;
    fpsIn_.dx = fpsIn_.dy = 0.f;
    fpsIn_.select = -1;
    fpsIn_.wheel = 0;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_EVENT_QUIT:
            running_ = false;
            break;
        case SDL_EVENT_WINDOW_RESIZED:
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            if (std::getenv("REDLINE_LOG_DISPLAY")) {
                int pw = 0, ph = 0, lw = 0, lh = 0;
                SDL_GetWindowSizeInPixels(window_, &pw, &ph);
                SDL_GetWindowSize(window_, &lw, &lh);
                std::fprintf(stderr, "[display] size event %s: %dx%d points, %dx%d pixels, density %.3f\n", e.type == SDL_EVENT_WINDOW_RESIZED ? "resized" : "pixel size", lw, lh, pw, ph, SDL_GetWindowPixelDensity(window_));
            }
            ctx_->requestResize();
            break;
        case SDL_EVENT_MOUSE_MOTION:
            if ((mode_ == Mode::Fps || mode_ == Mode::Countdown) && screen_ == kScreenNone) { fpsIn_.dx += e.motion.xrel; fpsIn_.dy += e.motion.yrel; }
            else if (const Hotspot* h = hotspotAt(e.motion.x, e.motion.y)) {
                // Hovering highlights the item the way the arrow keys would.
                if (h->kind == kHotMenu && menu_.index != h->index) { menu_.index = h->index; play("menu", 0.4f); }
                else if ((h->kind == kHotScreenItem || h->kind == kHotOptionRow) && screenIndex_ != h->index) { screenIndex_ = h->index; play("menu", 0.4f); }
            }
            break;
        case SDL_EVENT_GAMEPAD_ADDED:
            openGamepad(e.gdevice.which);
            break;
        case SDL_EVENT_GAMEPAD_REMOVED:
            if (pad_ && SDL_GetGamepadID(pad_) == e.gdevice.which) { SDL_CloseGamepad(pad_); pad_ = nullptr; padHeld_ = {}; fpsIn_.padFire = fpsIn_.padRun = false; fpsIn_.padMoveX = fpsIn_.padMoveZ = 0.f; std::fprintf(stderr, "[pad] disconnected\n"); }
            break;
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
            padButton(e.gbutton.button, true);
            break;
        case SDL_EVENT_GAMEPAD_BUTTON_UP:
            padButton(e.gbutton.button, false);
            break;
        case SDL_EVENT_TEXT_INPUT:
            if (screen_ == kScreenCode && e.text.text) {
                for (const char* c = e.text.text; *c; ++c) if (*c >= '0' && *c <= '9' && codeEntry_.size() < 6) codeEntry_.push_back(*c);
                break;
            }
            if (screen_ == kScreenNameEntry && e.text.text) {
                for (const char* c = e.text.text; *c; ++c) {
                    char ch = static_cast<char>(std::toupper(static_cast<unsigned char>(*c)));
                    if (((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == ' ') && nameEntry_.size() < 12) nameEntry_.push_back(ch);
                }
            } else if (screen_ == kScreenWadPath && e.text.text) {
                for (const char* c = e.text.text; *c; ++c)
                    if (static_cast<unsigned char>(*c) >= 32 && wadEntry_.size() < 400) wadEntry_.push_back(*c);
            } else if (screen_ == kScreenEmailEntry && e.text.text) {
                for (const char* c = e.text.text; *c; ++c) {
                    char ch = static_cast<char>(std::tolower(static_cast<unsigned char>(*c)));
                    if ((std::isalnum(static_cast<unsigned char>(ch)) || ch == '@' || ch == '.' || ch == '_' || ch == '-' || ch == '+') && emailEntry_.size() < 80) emailEntry_.push_back(ch);
                }
            }
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            stats_.addInput(InputDevice::Mouse);
            if (mode_ == Mode::Fps && screen_ == kScreenNone) { if (e.button.button == SDL_BUTTON_LEFT) fpsIn_.fire = true; break; }
            if (e.button.button == SDL_BUTTON_LEFT || e.button.button == SDL_BUTTON_RIGHT) {
                const Hotspot* h = hotspotAt(e.button.x, e.button.y);
                bool left = e.button.button == SDL_BUTTON_LEFT;
                if (!opts_.clicks.empty()) {
                    std::fprintf(stderr, "[ui] click %.0f,%.0f -> %s (hotspots %zu)\n", e.button.x, e.button.y, h ? (std::to_string(h->kind) + "/" + std::to_string(h->index)).c_str() : "none", hotspots_.size());
                    if (!h) for (const Hotspot& hs : hotspots_) std::fprintf(stderr, "[ui]   hotspot kind %d index %d at %.0f,%.0f size %.0fx%.0f\n", hs.kind, hs.index, hs.x, hs.y, hs.w, hs.h);
                }
                if (h && h->kind == kHotMenu && left) { menu_.index = h->index; play("menu_select", 0.7f); menuSelect(); }
                else if (h && h->kind == kHotScreenItem && left) {
                    if (screen_ == kScreenLeaderboard) openLeaderboard(h->index);
                    else if (screen_ == kScreenWhatsNew) screenKey(SDLK_RETURN, false);
                    else { screenIndex_ = h->index; screenKey(SDLK_RETURN, false); }
                }
                else if (h && h->kind == kHotProfileAction && left) profileAction(h->index);
                else if (h && h->kind == kHotOptionRow) { screenIndex_ = h->index; adjustOption(left ? 1 : -1); }   // right-click steps back
                else if (h && h->kind == kHotBack && left) { play("menu", 0.6f); screenKey(SDLK_ESCAPE, false); }
                else if (!h && left && mode_ == Mode::GameOver && gameOverT_ > 1.2f && screen_ == kScreenNone) { menu_.index = 0; play("menu_select", 0.7f); menuSelect(); }   // "press any key"
            }
            break;
        case SDL_EVENT_MOUSE_BUTTON_UP:
            if (e.button.button == SDL_BUTTON_LEFT) fpsIn_.fire = false;
            break;
        case SDL_EVENT_MOUSE_WHEEL:
            if (screen_ == kScreenOptions) { optionsScroll_ -= (e.wheel.y > 0.f ? 3 : e.wheel.y < 0.f ? -3 : 0); optionsFollow_ = false; }   // clamped when drawn
            else if (mode_ == Mode::Fps) fpsIn_.wheel += (e.wheel.y > 0.f) ? 1 : (e.wheel.y < 0.f ? -1 : 0);
            break;
        case SDL_EVENT_KEY_DOWN: {
            SDL_Keycode k = e.key.key;
            if (!e.key.repeat) stats_.addInput(InputDevice::Keyboard);
            if (k == SDLK_F12) {
                // Numbered, so holding F12 through a fight keeps every shot instead of
                // overwriting the one file each time.
                std::error_code ec;
                std::string name;
                for (int n = 1; n < 10000; ++n) {
                    name = "redline-screenshot-" + std::string(n < 10 ? "00" : n < 100 ? "0" : "") + std::to_string(n) + ".png";
                    if (!std::filesystem::exists(name, ec)) break;
                }
                renderer_->screenshot(name);
                std::fprintf(stderr, "[app] screenshot %s\n", name.c_str());
                break;
            }
            if (k == SDLK_RETURN && (e.key.mod & SDL_KMOD_ALT) && !e.key.repeat) {
                displayMode_ = displayMode_ == 0 ? 1 : 0;
                applyDisplay();
                announce(displayModeName(), glm::vec4(0.8f, 0.9f, 1.f, 1.f), 0.9f);
                break;
            }
            if (screen_ != kScreenNone) {
                if (!e.key.repeat || k == SDLK_BACKSPACE) screenKey(k, false);
                break;
            }
            if (k == SDLK_M && !e.key.repeat) {
                music_.setEnabled(!music_.enabled());
                saveSettings();
                announce(music_.enabled() ? "MUSIC ON" : "MUSIC OFF", glm::vec4(0.8f, 0.8f, 1.f, 1.f), 0.9f);
                break;
            }
            if (k == SDLK_N && !e.key.repeat) {
                cycleMusicSet();
                for (auto& item : menu_.items) if (item.rfind("MUSIC: ", 0) == 0) item = std::string("MUSIC: ") + musicSetName();
                break;
            }
            if (k == SDLK_ESCAPE) {
                if (mode_ == Mode::Blocks || mode_ == Mode::Fps || mode_ == Mode::Countdown) { pausedFrom_ = mode_; enterMode(Mode::Paused); }
                else if (mode_ == Mode::Paused) enterMode(pausedFrom_);
                else if (mode_ == Mode::Title) running_ = false;
                break;
            }
            if (k == SDLK_T && mode_ == Mode::Title && !e.key.repeat) { openScreen(kScreenTrophies); break; }
            if (mode_ == Mode::GameOver && gameOverT_ > 1.2f && !e.key.repeat && k != SDLK_UP && k != SDLK_DOWN && k != SDLK_W && k != SDLK_S && k != SDLK_RETURN && k != SDLK_SPACE && k != SDLK_KP_ENTER) {
                // "Press any key to try again": everything but the menu keys restarts.
                menu_.index = 0;
                play("menu_select", 0.7f);
                menuSelect();
                break;
            }
            if (!menu_.items.empty() && !e.key.repeat) { menuKey(k); break; }
            if (k == SDLK_B) keyB_ = true;
            if (k == SDLK_F) keyF_ = true;
            if (k == SDLK_G) keyG_ = true;
            if (mode_ == Mode::Blocks && !e.key.repeat) {
                switch (k) {
                case SDLK_LEFT: case SDLK_A: keys_.left = true; keys_.dasDir = -1; keys_.dasT = 0.f; keys_.dasActive = false; game_->moveLeft(); break;
                case SDLK_RIGHT: case SDLK_D: keys_.right = true; keys_.dasDir = 1; keys_.dasT = 0.f; keys_.dasActive = false; game_->moveRight(); break;
                case SDLK_DOWN: case SDLK_S: keys_.down = true; game_->setSoftDrop(true); break;
                case SDLK_UP: case SDLK_X: case SDLK_W: game_->rotateCW(); break;
                case SDLK_Z: case SDLK_LCTRL: game_->rotateCCW(); break;
                case SDLK_SPACE: game_->hardDrop(); break;
                default: break;
                }
            }
            if (mode_ == Mode::Fps && !e.key.repeat) {
                switch (k) {
                case SDLK_W: case SDLK_UP: fpsIn_.fwd = true; break;
                case SDLK_S: case SDLK_DOWN: fpsIn_.back = true; break;
                case SDLK_A: case SDLK_LEFT: fpsIn_.left = true; break;
                case SDLK_D: case SDLK_RIGHT: fpsIn_.right = true; break;
                case SDLK_SPACE: case SDLK_LCTRL: fpsIn_.fire = true; break;
                case SDLK_LSHIFT: case SDLK_RSHIFT: fpsIn_.run = true; break;
                case SDLK_1: fpsIn_.select = 0; break;
                case SDLK_2: fpsIn_.select = 1; break;
                case SDLK_3: fpsIn_.select = 2; break;
                case SDLK_4: fpsIn_.select = 3; break;
                case SDLK_Q: fpsIn_.wheel -= 1; break;
                case SDLK_E: fpsIn_.wheel += 1; break;
                default: break;
                }
            }
            break;
        }
        case SDL_EVENT_KEY_UP: {
            SDL_Keycode k = e.key.key;
            switch (k) {
            case SDLK_LEFT: case SDLK_A: keys_.left = false; fpsIn_.left = false; if (keys_.dasDir == -1) keys_.dasDir = keys_.right ? 1 : 0; break;
            case SDLK_RIGHT: case SDLK_D: keys_.right = false; fpsIn_.right = false; if (keys_.dasDir == 1) keys_.dasDir = keys_.left ? -1 : 0; break;
            case SDLK_DOWN: case SDLK_S: keys_.down = false; fpsIn_.back = false; game_->setSoftDrop(false); break;
            case SDLK_UP: case SDLK_W: fpsIn_.fwd = false; break;
            case SDLK_SPACE: case SDLK_LCTRL: fpsIn_.fire = false; break;
            case SDLK_LSHIFT: case SDLK_RSHIFT: fpsIn_.run = false; break;
            case SDLK_B: keyB_ = false; break;
            case SDLK_F: keyF_ = false; break;
            case SDLK_G: keyG_ = false; break;
            default: break;
            }
            break;
        }
        default:
            break;
        }
    }
}

void App::updateBlocksInput(float dt) {
    if (keys_.dasDir == 0) { keys_.dasT = 0.f; keys_.dasActive = false; return; }
    keys_.dasT += dt;
    const float dasDelay = 0.17f, dasRepeat = 0.045f;
    if (!keys_.dasActive) {
        if (keys_.dasT >= dasDelay) { keys_.dasActive = true; keys_.dasT = 0.f; }
        return;
    }
    while (keys_.dasT >= dasRepeat) {
        keys_.dasT -= dasRepeat;
        if (keys_.dasDir < 0) game_->moveLeft(); else game_->moveRight();
    }
}

void App::handleGameEvents() {
    for (const core::Event& ev : game_->drainEvents()) {
        switch (ev.type) {
        case core::EventType::PieceMoved: play("move", 0.5f, 1.f, 70); break;
        case core::EventType::PieceRotated: play("rotate", 0.6f, 1.f, 80); break;
        case core::EventType::PieceLocked:
            ++run_.pieces;
            play("lock", 0.8f);
            if (std::getenv("REDLINE_LOG_FX")) std::fprintf(stderr, "[fx] piece locked: %zu remembered cells, smoke frames %zu\n", lastPieceCells_.size(), assets_.brSmoke.frames.size());
            // Dust where the piece lands: a puff under each cell that has nothing of the piece below it.
            if (!assets_.brSmoke.empty())
                for (const auto& [cx, cy] : lastPieceCells_) {
                    bool lowest = true;
                    for (const auto& [ox, oy] : lastPieceCells_) if (ox == cx && oy > cy) lowest = false;
                    if (lowest && cy >= 0) bursts_.push_back({boardPos(static_cast<float>(cx), static_cast<float>(cy)) + glm::vec3(0.f, -0.45f, 0.7f), 0.f, 0.55f, 0.009f, &assets_.brSmoke, glm::vec4(0.9f, 0.85f, 0.8f, 0.9f)});
                }
            break;
        case core::EventType::HardDrop:
            shakeT_ = 0.15f;
            for (auto& cell : lastPieceCells_) cell.second += ev.a;   // the drop and the lock land in one tick: move the remembered cells down
            break;
        case core::EventType::LinesCleared: {
            if (ev.a >= 4) ++run_.tetrises;
            run_.bestChain = std::max(run_.bestChain, game_->chain());
            play("clear", 0.9f, ev.a >= 4 ? 1.3f : 1.f);
            // Flash, a shockwave ring per row, dust along it and a few sparks.
            clearFlash_ = std::min(1.f, 0.5f + 0.15f * static_cast<float>(ev.a));
            shakeT_ = std::max(shakeT_, 0.08f + 0.04f * static_cast<float>(ev.a));
            for (int row : game_->clearingRows()) {
                const glm::vec3 centre = boardPos(core::kBoardW * 0.5f - 0.5f, static_cast<float>(row));
                rings_.push_back({centre + glm::vec3(0.f, 0.f, 0.55f), 0.f});
                for (int c = 0; c < core::kBoardW; c += 2) {
                    const glm::vec3 cell = boardPos(static_cast<float>(c), static_cast<float>(row)) + glm::vec3(0.f, 0.f, 0.7f);
                    if (!assets_.brSmoke.empty()) bursts_.push_back({cell, 0.f, 0.6f, 0.011f, &assets_.brSmoke, glm::vec4(1.f, 0.95f, 0.9f, 0.8f)});
                    if (!assets_.brSparks.empty() && (c / 2) % 2 == 0) bursts_.push_back({cell, 0.f, 0.45f, 0.008f, &assets_.brSparks});
                }
            }
            static const char* names[5] = {"", "SINGLE", "DOUBLE", "TRIPLE", "TETRIS"};
            std::string label = names[std::clamp(ev.a, 0, 4)];
            if (game_->chain() > 0) label = "CHAIN X" + std::to_string(game_->chain() + 1) + "  " + label;
            else if (game_->combo() > 1) label = "COMBO X" + std::to_string(game_->combo()) + "  " + label;
            glm::vec4 col = ev.a >= 4 ? glm::vec4(1.f, 0.9f, 0.3f, 1.f) : (game_->chain() > 0 ? glm::vec4(1.f, 0.5f, 0.2f, 1.f) : glm::vec4(1.f));
            announce(label + "  +" + std::to_string(ev.b), col, ev.a >= 4 || game_->chain() > 0 ? 1.6f : 1.2f);
            if (ev.a >= 4) trophy("tetris");
            if (game_->combo() >= 3) trophy("combo3");
            if (game_->chain() > 0) trophy("chain");
            rumble(0.2f * ev.a, 0.15f * ev.a, 120);
            {
                const core::Prizes& pr = game_->lastClearPrizes();
                std::string prize;
                if (pr.bonusHealth > 0.f) prize += "+" + std::to_string(static_cast<int>(pr.bonusHealth)) + " HEALTH  ";
                if (pr.shield > 0.f) prize += "+" + std::to_string(static_cast<int>(pr.shield)) + " ARMOR  ";
                if (pr.invulnChance > 0.f) prize += "+" + std::to_string(static_cast<int>(pr.invulnChance * 100.f)) + "% INVULN";
                if (!prize.empty()) announce(prize, glm::vec4(0.6f, 0.9f, 1.f, 1.f), 0.9f);
            }
            break;
        }
        case core::EventType::RedCellFell: play("lock", 0.4f, 1.6f, 90); break;
        case core::EventType::LevelUp:
            if (ev.a >= 5) trophy("level5");
            if (ev.a >= 10) trophy("level10");
            break;   // the level card handles the celebration
        case core::EventType::CellCorrupting: play("redline", 0.35f, 1.6f, 250); break;
        case core::EventType::CellTurnedRed: play("lock", 0.9f, 0.55f, 120); shakeT_ = std::max(shakeT_, 0.1f); evilJolt_ = 1.f; break;
        case core::EventType::EvilSpawning: play("redline", 0.5f, 0.7f, 250); break;
        case core::EventType::EvilSpawned: play("explode", 0.5f, 1.4f); shakeT_ = std::max(shakeT_, 0.15f); announce("EVIL SPAWNED", glm::vec4(1.f, 0.3f, 0.2f, 1.f), 1.f); break;
        default: break;
        }
    }
}

void App::handleFpsEvents() {
    for (const FpsEvent& ev : fps_.drainEvents()) {
        const EnemyArt& art = assets_.enemies[std::clamp(ev.kind, 0, kEnemyKinds - 1)];
        switch (ev.type) {
        case FpsEvent::Type::Shoot:
            ++run_.shots[std::clamp(ev.a, 0, kRunWeapons - 1)];
            if (brutalActive() && assets_.brWeaponSounds) play(std::string("br_") + assets_.weapons[std::clamp(ev.a, 0, kWeaponArt - 1)].fireSound, 1.f);
            else play(assets_.weapons[std::clamp(ev.a, 0, kWeaponArt - 1)].fireSound, 1.f);
            muzzleLight_ = 1.f;
            viewKick_ += ev.a == kRocketLauncher ? 0.045f : ev.a == kShotgun ? 0.03f : ev.a == kChaingun ? 0.006f : 0.004f;
            shakeT_ = ev.a == kRocketLauncher ? 0.2f : (ev.a == kShotgun ? 0.12f : 0.04f);
            rumble(ev.a == kChaingun ? 0.15f : 0.3f, ev.a == kRocketLauncher ? 0.8f : 0.5f, ev.a == kChaingun ? 40 : 90);
            break;
        case FpsEvent::Type::Pickup: {
            static const char* kinds[] = {"stim", "medikit", "bullets", "rockets", "cells", "chaingun", "rocket launcher", "plasma gun"};
            std::fprintf(stderr, "[fps] pickup %s (health %d)\n", kinds[std::clamp(ev.a, 0, 7)], static_cast<int>(fps_.health()));
            play(ev.a >= static_cast<int>(PickupKind::Chaingun) ? "pickup_weapon" : "pickup_item", 0.9f);
            ++gamePickups_;
            if (gamePickups_ >= 20) trophy("collector");
            if (fps_.weapon(kChaingun).owned && fps_.weapon(kRocketLauncher).owned && fps_.weapon(kPlasmaRifle).owned) trophy("arsenal");
            break;
        }
        case FpsEvent::Type::WeaponSwitch: std::fprintf(stderr, "[fps] weapon -> %s\n", assets_.weapons[std::clamp(ev.a, 0, kWeaponArt - 1)].name.c_str()); play("menu", 0.5f, 1.3f); break;
        case FpsEvent::Type::RocketBlast:
            if (brutalActive() && assets_.brExplodeSounds) play("br_explode" + std::to_string(1 + rng_() % static_cast<unsigned>(assets_.brExplodeSounds)), 1.f);
            else play("rocket_hit", 1.f);
            shakeT_ = 0.35f; rumble(0.8f, 0.6f, 300);
            fightStats_.blocks += ev.a; gameBlocks_ += ev.a;
            if (gameBlocks_ >= 50) trophy("demolition");
            break;
        case FpsEvent::Type::BlockBroken:
            play("lock", 0.7f, 0.8f, 100); shakeT_ = std::max(shakeT_, 0.08f);
            if (!assets_.brSparks.empty()) {
                bursts_.push_back({ev.pos, 0.f, 0.5f, 0.012f, &assets_.brSparks});
                if (assets_.brSparkSounds) play("sparks" + std::to_string(1 + rng_() % static_cast<unsigned>(assets_.brSparkSounds)), 0.5f, 1.f, 80);
            }
            break;
        case FpsEvent::Type::Score:
            announce(art.name + "  +" + std::to_string(ev.a), ev.tier >= 4 ? glm::vec4(1.f, 0.9f, 0.3f, 1.f) : glm::vec4(1.f), ev.tier >= 4 ? 1.5f : 1.1f);
            ++fightStats_.kills;
            ++run_.killsByKind[std::clamp(ev.kind, 0, kRunKinds - 1)];
            trophy("first_blood");
            if (ev.tier >= 4) trophy("boss");
            if (ev.tier == 5) trophy("cyber");
            if (ev.tier == 6) trophy("mastermind");
            break;
        case FpsEvent::Type::KilledGrown: trophy("grown"); break;
        case FpsEvent::Type::Absorb:
            std::fprintf(stderr, "[fps] a monster absorbed %d blocks and became a %s\n", ev.a, art.name.c_str());
            play(art.sightSound, 1.f, 0.8f);
            play("explode", 0.6f, 0.6f);
            shakeT_ = 0.4f;
            break;
        case FpsEvent::Type::PlasmaHit: play("fireball_hit", 0.4f, 1.4f, 80); break;
        case FpsEvent::Type::EnemyHit:
            play(art.painSound, 0.8f, 1.f, 120);
            if (brutalActive() && !assets_.brSpray[bloodColorForKind(ev.kind)].empty()) bursts_.push_back({ev.pos, 0.f, 0.42f, 0.007f, &assets_.brSpray[bloodColorForKind(ev.kind)]});
            break;
        case FpsEvent::Type::BulletHole:
            if (!assets_.brSmoke.empty()) bursts_.push_back({ev.pos, 0.f, 0.6f, 0.006f, &assets_.brSmoke, glm::vec4(0.5f, 0.47f, 0.45f, 0.45f)});
            if (ev.a) {   // wall: sparks and a ricochet; floor: a dull thud
                if (!assets_.brSparks.empty()) bursts_.push_back({ev.pos, 0.f, 0.45f, 0.007f, &assets_.brSparks});
                if (assets_.brRicochetSounds) play("ricochet" + std::to_string(1 + rng_() % static_cast<unsigned>(assets_.brRicochetSounds)), 0.35f, 1.f, 60);
            } else if (assets_.brDirtSounds) play("bhit" + std::to_string(1 + rng_() % static_cast<unsigned>(assets_.brDirtSounds)), 0.3f, 1.f, 60);
            break;
        case FpsEvent::Type::CasingBounce:
            if (ev.a && assets_.brShellSounds) play("shell" + std::to_string(1 + rng_() % static_cast<unsigned>(assets_.brShellSounds)), 0.5f, 1.f, 30);
            else if (!ev.a && assets_.brCasingSounds) play("casing" + std::to_string(1 + rng_() % static_cast<unsigned>(assets_.brCasingSounds)), 0.35f, 1.f, 30);
            break;
        case FpsEvent::Type::EnemyDied: if (!ev.a) play(art.deathSound, 1.f); break;   // a = 1: gibbed, the slop plays instead
        case FpsEvent::Type::EnemyGibbed:
            if (assets_.brGibSounds) play("gibdeath" + std::to_string(1 + rng_() % static_cast<unsigned>(assets_.brGibSounds)), 1.f);
            else play("gib", 1.f);
            if (assets_.brBoneSounds) play("bonecr" + std::to_string(1 + rng_() % static_cast<unsigned>(assets_.brBoneSounds)), 0.6f, 1.f, 100);
            shakeT_ = std::max(shakeT_, 0.08f);
            break;
        case FpsEvent::Type::EnemyAttack:
            if (brutalActive() && ev.kind == 1 && assets_.brImpSounds) play("impclaw" + std::to_string(1 + rng_() % static_cast<unsigned>(assets_.brImpSounds)), 0.7f);
            else play(art.attackSound, 0.7f);
            break;
        case FpsEvent::Type::VileFire:
            play(ev.a == 0 ? "vile_flame_start" : "vile_flame", 0.9f);
            break;
        case FpsEvent::Type::VileBlast:   // A_VileAttack plays the barrel explosion
            play("explode", 1.f); shakeT_ = 0.45f; rumble(0.9f, 0.6f, 300);
            // Smoke, not a flash: the default 1.3 white tint left a glowing pale ball hanging
            // in the air for half a second after every fireball had gone out.
            if (!assets_.brSmoke.empty()) bursts_.push_back({ev.pos + glm::vec3(0.f, 0.6f, 0.f), 0.f, 1.1f, 0.028f, &assets_.brSmoke, glm::vec4(0.34f, 0.30f, 0.28f, 0.38f)});
            break;
        case FpsEvent::Type::Explosion:
            play("explode", 1.f); shakeT_ = 0.3f + 0.1f * ev.tier; rumble(0.7f, 0.5f, 250);
            // Smoke, not a flash: the default 1.3 white tint left a glowing pale ball hanging
            // in the air for half a second after every fireball had gone out.
            if (!assets_.brSmoke.empty()) bursts_.push_back({ev.pos + glm::vec3(0.f, 0.6f, 0.f), 0.f, 1.1f, 0.028f, &assets_.brSmoke, glm::vec4(0.34f, 0.30f, 0.28f, 0.38f)});
            fightStats_.blocks += ev.a; gameBlocks_ += ev.a;
            if (gameBlocks_ >= 50) trophy("demolition");
            break;
        case FpsEvent::Type::PlayerHit:
            play(brutalActive() && assets_.brPlayerPain ? "br_pain" : "pain", 1.f); shakeT_ = 0.25f; rumble(0.6f, 0.3f, 200);
            if (brutalActive() && !assets_.blood.empty()) {   // blood on the screen
                std::uniform_real_distribution<float> u(0.f, 1.f);
                VkExtent2D ext = renderer_->extent();
                for (int i = 0; i < 3; ++i)
                    screenBlood_.push_back({u(rng_) * ext.width, u(rng_) * ext.height, 4.f + 5.f * u(rng_), 0.f, 1.4f + 0.8f * u(rng_), static_cast<int>(u(rng_) * 3.f) % 3});
            }
            break;
        case FpsEvent::Type::FireballHit: play("fireball_hit", 0.5f, 1.f, 80); break;
        case FpsEvent::Type::AllClear: play("levelup", 1.f); break;
        case FpsEvent::Type::GateFalls:
            play("explode", 1.f); shakeT_ = 1.2f; rumble(0.9f, 0.7f, 900);
            announce("ARENA CLEARED: THE WALL COMES DOWN", glm::vec4(1.f, 0.4f, 0.3f, 1.f), 1.5f);
            break;
        case FpsEvent::Type::DungeonOpen:
            play("levelup", 1.f, 0.6f); shakeT_ = 0.5f;
            announce("ENTER THE DUNGEON AND KILL THE BOSS", glm::vec4(1.f, 0.25f, 0.2f, 1.f), 1.7f);
            std::fprintf(stderr, "[dungeon] level %d: %dx%d tiles, %zu rooms, %d demons + boss %s (blocks %d)\n", fps_.level(), fps_.dungeon().width(), fps_.dungeon().depth(),
                         fps_.dungeon().rooms().size(), fps_.totalEnemies() - 1, fps_.boss() ? assets_.enemies[std::clamp(fps_.boss()->kind, 0, kEnemyKinds - 1)].name.c_str() : "?", fps_.boardBlocks());
            break;
        case FpsEvent::Type::Wake:
            if (brutalActive() && ev.kind == 0 && assets_.brZombieSight) play("zcsit" + std::to_string(1 + rng_() % static_cast<unsigned>(assets_.brZombieSight)), 0.8f);
            else play(art.sightSound, 0.8f);
            break;
        case FpsEvent::Type::BossSeen:
            announce("BOSS: " + art.name, glm::vec4(1.f, 0.3f, 0.2f, 1.f), 1.8f);
            play(art.sightSound, 1.f, 0.85f);
            break;
        case FpsEvent::Type::KeyFound:
            std::fprintf(stderr, "[dungeon] the skull key is taken\n");
            play("pickup_weapon", 1.f);
            announce("SKULL KEY: THE BOSS HALL IS OPEN", glm::vec4(1.f, 0.5f, 0.4f, 1.f), 1.4f);
            break;
        case FpsEvent::Type::DoorLocked:
            std::fprintf(stderr, "[dungeon] the hall is sealed; the key is elsewhere\n");
            play("menu", 0.7f, 0.6f);
            announce("SEALED: FIND THE SKULL KEY", glm::vec4(1.f, 0.6f, 0.4f, 1.f), 1.2f);
            break;
        case FpsEvent::Type::DoorOpened:
            std::fprintf(stderr, "[dungeon] the hall is open\n");
            play("levelup", 0.9f, 0.8f);
            shakeT_ = std::max(shakeT_, 0.3f);
            announce("THE HALL OPENS", glm::vec4(1.f, 0.8f, 0.4f, 1.f), 1.4f);
            break;
        case FpsEvent::Type::BossDead:
            announce("BOSS SLAIN", glm::vec4(1.f, 0.9f, 0.4f, 1.f), 2.f);
            play("levelup", 1.f, 1.1f);
            trophy("dungeon");
            break;
        case FpsEvent::Type::PlayerDead: diedInFps_ = true; run_.killedBy = ev.kind; break;
        case FpsEvent::Type::EnemySight: {
            // Announce the biggest monster in the room and log the roster.
            if (brutalActive() && ev.kind == 0 && assets_.brZombieSight) play("zcsit" + std::to_string(1 + rng_() % static_cast<unsigned>(assets_.brZombieSight)), 0.9f);
            else play(art.sightSound, 0.9f);
            std::string roster;
            for (const Enemy& en : fps_.enemies()) roster += (roster.empty() ? "" : ", ") + assets_.enemies[std::clamp(en.kind, 0, kEnemyKinds - 1)].name + "(" + std::to_string(en.cells.size()) + ")";
            std::fprintf(stderr, "[fps] level %d roster: %s\n", fps_.level(), roster.c_str());
            if (fps_.startedInvulnerable()) { announce("INVULNERABLE", glm::vec4(1.f, 0.95f, 0.5f, 1.f), 1.8f); play("levelup", 1.f, 0.7f); trophy("invuln"); }
            else if (lastInvulnChance_ > 0.f) announce("INVULNERABILITY ROLL FAILED (" + std::to_string(static_cast<int>(lastInvulnChance_ * 100.f)) + "%)", glm::vec4(1.f, 0.6f, 0.4f, 1.f), 1.0f);
            fightStats_ = {};
            break;
        }
        }
    }
}

void App::update(float dt) {
    modeT_ += dt;
    updateNoticeT_ = (mode_ == Mode::Title && updateAvailable_) ? updateNoticeT_ + dt : 0.f;
    versionAgeT_ += dt;
    if (mode_ == Mode::Title && screen_ == kScreenNone && versionAgeT_ > 600.f && !online_.busy()) { versionAgeT_ = 0.f; checkVersion(); }   // a game left on the title still learns of a release
    {
        // The dungeon lives only through a fight; the static environment follows its stage.
        const bool fightMode = mode_ == Mode::FlyIn || mode_ == Mode::Countdown || mode_ == Mode::Fps || mode_ == Mode::FlyOut
                            || (mode_ == Mode::Paused && (pausedFrom_ == Mode::Fps || pausedFrom_ == Mode::Countdown)) || (mode_ == Mode::GameOver && diedInFps_);
        if (!fightMode && fps_.stage() != FpsMode::Stage::Arena) fps_.closeDungeon();
        if (static_cast<int>(fps_.stage()) != envStage_ || envDungeon_ != !fps_.dungeon().empty() || envDoorOpen_ != fps_.doorOpen()) { buildEnvironment(); buildProps(); }
    }
    if (mode_ == Mode::Blocks || mode_ == Mode::Alert || mode_ == Mode::FlyIn || mode_ == Mode::Countdown || mode_ == Mode::Fps || mode_ == Mode::FlyOut) run_.duration += dt;
    pollOnline();
    if (dialogDone_) {
        std::vector<std::string> files;
        { std::lock_guard<std::mutex> lock(dialogMutex_); files.swap(dialogFiles_); dialogDone_ = false; }
        dialogOpen_ = false;
        if (files.empty()) wadStatus_ = wadMissing_ ? "NO FILE CHOSEN" : "";
        else if (dialogForExtras_) {
            // The soundtrack file only makes sense with an IWAD loaded; re-run the load with it.
            if (wadPath_.empty()) wadStatus_ = "CHOOSE DOOM.WAD FIRST";
            else if (reloadAssets(wadPath_, files.front()) && !assets_.oggMusic.empty()) wadStatus_.clear();
            else wadStatus_ = "NO SOUNDTRACKS IN " + std::filesystem::path(files.front()).filename().string();
        }
        else if (reloadAssets(files.front())) {}
        else wadStatus_ = "NOT A DOOM WAD: " + std::filesystem::path(files.front()).filename().string();
    }
    if (!opts_.reloadWad.empty() && frameCount_ == 30) reloadAssets(opts_.reloadWad);
    for (Announcement& a : announcements_) a.t += dt;
    bfgFlash_ = std::max(0.f, bfgFlash_ - dt * 1.2f);
    // The secret chord: B, F and G held together for two seconds while stacking.
    if (mode_ == Mode::Blocks && !bfgUsed_ && keyB_ && keyF_ && keyG_) {
        if (bfgHoldT_ == 0.f) play("bfg", 1.f);   // the charge-up whine starts with the hold
        bfgHoldT_ += dt;
        if (bfgHoldT_ >= 2.f) {
            int removed = game_->purgeRed();
            bfgUsed_ = true;
            bfgFlash_ = 1.f;
            bfgHoldT_ = 0.f;
            shakeT_ = 0.6f;
            play("rocket_hit", 1.f, 0.8f);   // the blast
            play("explode", 0.8f, 0.7f);
            announce("BFG9000", glm::vec4(0.5f, 1.f, 0.5f, 1.f), 2.2f);
            announce(std::to_string(removed) + " RED BLOCKS ERASED  -  THE STACK FALLS", glm::vec4(0.7f, 1.f, 0.7f, 1.f), 1.1f);
            std::fprintf(stderr, "[app] BFG9000 fired: %d red blocks erased\n", removed);
            trophy("bfg");
            rumble(1.f, 1.f, 600);
        }
    } else {
        bfgHoldT_ = 0.f;
    }
    announcements_.erase(std::remove_if(announcements_.begin(), announcements_.end(), [](const Announcement& a) { return a.t > 2.f; }), announcements_.end());
    shakeT_ = std::max(0.f, shakeT_ - dt);
    muzzleLight_ = std::max(0.f, muzzleLight_ - dt * 8.f);
    viewKick_ *= std::exp(-9.f * dt);
    {   // weapon lag: the gun trails the view when you turn, then catches up
        const float dyaw = fps_.yaw() - lastYaw_, dpitch = fps_.pitch() - lastPitch_;
        lastYaw_ = fps_.yaw(); lastPitch_ = fps_.pitch();
        const float targetX = std::clamp(-dyaw / std::max(dt, 1e-3f) * 4.f, -22.f, 22.f);
        const float targetY = std::clamp(dpitch / std::max(dt, 1e-3f) * 3.f, -14.f, 14.f);
        const float k = 1.f - std::exp(-10.f * dt);
        swayX_ += (targetX - swayX_) * k;
        swayY_ += (targetY - swayY_) * k;
    }
    // Embers drift up from every flame; rings and the clear flash fade.
    emberT_ += dt;
    if (emberT_ > 0.05f) {
        emberT_ = 0.f;
        std::uniform_real_distribution<float> u(-1.f, 1.f);
        for (const Prop& p : props_) {
            const bool flame = p.anim == &assets_.torchRed || p.anim == &assets_.torchBlue || p.anim == &assets_.torchGreen || p.anim == &assets_.candelabra;
            if (!flame || embers_.size() >= 400 || u(rng_) > -0.2f) continue;   // ~40% of ticks per flame
            Ember e;
            e.pos = p.pos + glm::vec3(u(rng_) * 0.08f, p.lightHeight - 0.3f + u(rng_) * 0.1f, u(rng_) * 0.08f);
            e.vel = glm::vec3(u(rng_) * 0.35f, 0.7f + 0.4f * u(rng_), u(rng_) * 0.35f);
            e.life = e.ttl = 0.9f + 0.6f * std::fabs(u(rng_));
            e.size = 0.03f + 0.02f * std::fabs(u(rng_));
            e.col = glm::mix(p.lightColor, glm::vec3(1.f, 0.9f, 0.6f), 0.35f);
            embers_.push_back(e);
        }
    }
    for (size_t i = 0; i < embers_.size();) {
        Ember& e = embers_[i];
        e.ttl -= dt;
        e.vel.x += std::sin(time_ * 7.f + e.pos.y * 9.f) * 0.6f * dt;
        e.vel.z += std::cos(time_ * 6.f + e.pos.x * 9.f) * 0.6f * dt;
        e.pos += e.vel * dt;
        if (e.ttl <= 0.f) { e = embers_.back(); embers_.pop_back(); } else ++i;
    }
    for (size_t i = 0; i < rings_.size();) { rings_[i].t += dt; if (rings_[i].t > 0.55f) { rings_[i] = rings_.back(); rings_.pop_back(); } else ++i; }
    clearFlash_ = std::max(0.f, clearFlash_ - dt * 3.f);
    evilJolt_ = std::max(0.f, evilJolt_ - dt * 4.f);
    // A registration waiting for the email: ask the server every few seconds on the code screen, every half minute otherwise.
    if (!stats_.pendingPoll().empty() && OnlineClient::available() && !stats_.emailVerified()) {
        pollT_ -= dt;
        if (pollT_ <= 0.f && !online_.busy()) { pollT_ = screen_ == kScreenCode ? 4.f : 30.f; online_.pollRegistration(stats_.playerId(), stats_.pendingPoll()); }
    }
    // Trophy toasts: the front card runs its course, then the next one plays its jingle.
    if (!toasts_.empty()) {
        toasts_.front().t += dt;
        if (toasts_.front().t > (toasts_.front().ultimate ? 8.f : 5.f)) {
            toasts_.erase(toasts_.begin());
            if (!toasts_.empty()) { play("pickup_weapon", 1.f, 1.3f); rumble(0.3f, 0.6f, 200); }
        }
    }
    if (celebrateT_ >= 0.f) {
        const float W = static_cast<float>(renderer_->extent().width), H = static_cast<float>(renderer_->extent().height);
        const float s = std::max(1.f, std::round(H / 300.f));
        std::uniform_real_distribution<float> u(0.f, 1.f);
        const float before = celebrateT_;
        celebrateT_ += dt;
        for (float beat : {1.5f, 3.f, 4.5f, 6.f}) if (before < beat && celebrateT_ >= beat) rumble(0.8f, 0.5f, 250);   // pulses with the rockets
        if (celebrateT_ < 6.5f) {
            celebrateNext_ -= dt;
            if (celebrateNext_ <= 0.f) {   // a rocket from the bottom edge
                celebrateNext_ = 0.22f + 0.25f * u(rng_);
                Spark r;
                r.pos = glm::vec2(W * (0.1f + 0.8f * u(rng_)), H + 4.f);
                r.vel = glm::vec2((u(rng_) - 0.5f) * 0.12f * H, -(0.75f + 0.35f * u(rng_)) * H);
                r.life = r.ttl = 0.75f + 0.3f * u(rng_);
                r.size = 2.f * s;
                const float hue = u(rng_);
                r.col = hue < 0.4f ? glm::vec3(1.f, 0.85f, 0.3f) : hue < 0.7f ? glm::vec3(1.f, 0.3f, 0.2f) : hue < 0.85f ? glm::vec3(1.f, 1.f, 1.f) : glm::vec3(0.4f, 0.8f, 1.f);
                r.kind = 0;
                sparks_.push_back(r);
            }
            for (int i = 0; i < 3; ++i) {   // confetti drifting down from the top
                Spark c;
                c.pos = glm::vec2(W * u(rng_), -6.f);
                c.vel = glm::vec2((u(rng_) - 0.5f) * 40.f * s, (35.f + 40.f * u(rng_)) * s);
                c.life = c.ttl = 3.f + 2.f * u(rng_);
                c.size = (1.2f + 1.2f * u(rng_)) * s;
                const float hue = u(rng_);
                c.col = hue < 0.3f ? glm::vec3(1.f, 0.8f, 0.2f) : hue < 0.55f ? glm::vec3(1.f, 0.25f, 0.2f) : hue < 0.75f ? glm::vec3(0.3f, 0.9f, 0.4f) : hue < 0.9f ? glm::vec3(0.4f, 0.6f, 1.f) : glm::vec3(1.f, 1.f, 1.f);
                c.kind = 2;
                sparks_.push_back(c);
            }
        }
        if (celebrateT_ > 9.f && sparks_.empty()) celebrateT_ = -1.f;
    }
    if (!sparks_.empty()) {
        const float H = static_cast<float>(renderer_->extent().height);
        std::uniform_real_distribution<float> u(0.f, 1.f);
        std::vector<Spark> born;
        for (size_t i = 0; i < sparks_.size();) {
            Spark& p = sparks_[i];
            p.ttl -= dt;
            const float g = p.kind == 2 ? 0.f : (p.kind == 0 ? 0.35f : 0.9f) * H;
            p.vel.y += g * dt;
            if (p.kind == 2) p.vel.x = std::sin(p.ttl * 3.f + p.size) * 30.f * (H / 900.f);
            p.pos += p.vel * dt;
            if (p.ttl <= 0.f) {
                if (p.kind == 0) {   // burst
                    play("explode", 0.35f, 1.4f + 0.4f * u(rng_));
                    const int n = 40 + static_cast<int>(30.f * u(rng_));
                    for (int k = 0; k < n; ++k) {
                        Spark q;
                        const float a = 6.2831853f * u(rng_), v = (0.12f + 0.32f * u(rng_)) * H;
                        q.pos = p.pos;
                        q.vel = glm::vec2(std::cos(a) * v, std::sin(a) * v * 0.9f);
                        q.life = q.ttl = 1.f + 0.9f * u(rng_);
                        q.size = p.size * (0.7f + 0.7f * u(rng_));
                        q.col = u(rng_) < 0.8f ? p.col : glm::vec3(1.f);
                        q.kind = 1;
                        born.push_back(q);
                    }
                }
                p = sparks_.back(); sparks_.pop_back();
            } else if (p.pos.y > H + 20.f) { p = sparks_.back(); sparks_.pop_back(); }
            else ++i;
        }
        sparks_.insert(sparks_.end(), born.begin(), born.end());
    }
    if (const char* demo = std::getenv("REDLINE_TROPHY_DEMO"); demo && frameCount_ == std::max(1, std::atoi(demo) > 0 ? std::atoi(demo) : 60)) {   // test knob: the toast and the celebration without earning them, at that frame
        const auto& all = trophyCatalogue();
        showTrophy(all[0], false);
        showTrophy(all.back(), true);
        startCelebration();
    }
    {
        // Blood running off the evil banner: more of it the higher the stack.
        const float H = static_cast<float>(renderer_->extent().height);
        const float s = std::max(1.f, std::round(H / 300.f));
        const float d = (mode_ == Mode::Blocks && game_) ? game_->danger() : 0.f;
        if (d > 0.f && bannerX1_ > bannerX0_) {
            dripT_ += dt * (1.5f + 8.f * d);
            std::uniform_real_distribution<float> u(0.f, 1.f);
            while (dripT_ >= 1.f) {
                dripT_ -= 1.f;
                drips_.push_back({bannerX0_ + u(rng_) * (bannerX1_ - bannerX0_), bannerY_, bannerY_, 0.f, 1.2f + 1.2f * u(rng_), 3.f + 2.5f * u(rng_)});
            }
        } else dripT_ = 0.f;
        for (size_t i = 0; i < drips_.size();) {
            Drip& dr = drips_[i];
            dr.ttl -= dt;
            dr.vy = std::min(dr.vy + dt * 18.f * s, 16.f * s);   // slow, viscous
            dr.y += dr.vy * dt;
            if (dr.ttl <= 0.f || dr.y0 > H) { dr = drips_.back(); drips_.pop_back(); } else ++i;
        }
    }
    if (game_ && game_->active()) { lastPieceCells_.clear(); for (int i = 0; i < 4; ++i) lastPieceCells_.push_back(game_->active()->cells()[i]); }
    for (size_t i = 0; i < bursts_.size();) {
        bursts_[i].t += dt;
        if (bursts_[i].t >= bursts_[i].ttl) { bursts_[i] = bursts_.back(); bursts_.pop_back(); }
        else ++i;
    }
    for (size_t i = 0; i < screenBlood_.size();) {
        screenBlood_[i].t += dt;
        if (screenBlood_[i].t >= screenBlood_[i].ttl) screenBlood_.erase(screenBlood_.begin() + static_cast<std::ptrdiff_t>(i));
        else ++i;
    }
    pollGamepad(dt);
    if (levelCardT_ < kLevelCardTime) levelCardT_ += dt;

    if (mode_ == Mode::Title || mode_ == Mode::Blocks || mode_ == Mode::Alert || (mode_ == Mode::GameOver && !diedInFps_)) {
        ambient_.update(dt);
        // Silent scenery (no sound over the Tetris game), but in Brutal mode the brawl leaves blood.
        for (const BrawlEvent& be : ambient_.drainEvents()) {
            if (!brutalActive()) continue;
            const int c = bloodColorForTier(be.tier);
            if (be.type == BrawlEvent::Type::Pain && !assets_.brSpray[c].empty()) bursts_.push_back({be.pos + glm::vec3(0.f, 0.9f, 0.f), 0.f, 0.4f, 0.006f, &assets_.brSpray[c]});
            if (be.type == BrawlEvent::Type::Death) {
                if (!assets_.brSpray[c].empty()) bursts_.push_back({be.pos + glm::vec3(0.f, 0.8f, 0.f), 0.f, 0.45f, 0.008f, &assets_.brSpray[c]});
                std::uniform_real_distribution<float> u(0.f, 6.2831853f);
                Decal d;
                d.pos = glm::vec3(be.pos.x, 0.f, be.pos.z);
                d.size = 0.5f + 0.12f * static_cast<float>(be.tier);
                d.yaw = u(rng_);
                d.color = c;
                if (brawlDecals_.size() >= 60) brawlDecals_.erase(brawlDecals_.begin());
                brawlDecals_.push_back(d);
            }
        }
        for (size_t i = 0; i < brawlDecals_.size();) {
            brawlDecals_[i].age += dt;
            if (brawlDecals_[i].age > 60.f) brawlDecals_.erase(brawlDecals_.begin() + static_cast<std::ptrdiff_t>(i));
            else ++i;
        }
    }
    switch (mode_) {
    case Mode::Title:
        break;
    case Mode::Blocks:
        updateBlocksInput(dt);
        if (opts_.bot && !(levelCardT_ < kLevelCardTime)) {   // the test bot plays the blocks
            botTetrisT_ += dt;
            if (botTetrisT_ >= 0.12f) { botTetrisT_ = 0.f; tetrisBot_.step(*game_); }
        }
        // While the level card is up the collapse still plays but the next piece waits.
        if (!(levelCardT_ < kLevelCardTime && (game_->phase() == core::Phase::Falling || game_->phase() == core::Phase::Spawning))) game_->tick(dt);
        handleGameEvents();
        if (game_->phase() == core::Phase::RedLine) enterMode(Mode::Alert);
        else if (game_->phase() == core::Phase::GameOver) enterMode(Mode::GameOver);
        break;
    case Mode::Alert:
        if (modeT_ >= kAlertTime) enterMode(Mode::FlyIn);
        break;
    case Mode::FlyIn:
        if (modeT_ >= kFlyInTime) enterMode(Mode::Countdown);
        break;
    case Mode::Countdown: {
        // Monsters rise, the player can look around, nothing shoots yet.
        FpsInput in;
        in.lookDX = fpsIn_.dx;
        in.lookDY = fpsIn_.dy;
        in.warmup = true;
        fps_.update(dt, in, *game_);
        handleFpsEvents();
        int n = static_cast<int>(std::ceil(kCountdownTime - modeT_));
        if (n != countdownLast_) { countdownLast_ = n; play("menu", 0.8f, n <= 0 ? 1.6f : 1.f + 0.1f * static_cast<float>(3 - n)); }
        if (modeT_ >= kCountdownTime) { play("levelup", 0.8f, 1.2f); enterMode(Mode::Fps); }
        break;
    }
    case Mode::Fps: {
        FpsInput in;
        in.moveZ = (fpsIn_.fwd ? 1.f : 0.f) - (fpsIn_.back ? 1.f : 0.f) + fpsIn_.padMoveZ;
        in.moveX = (fpsIn_.right ? 1.f : 0.f) - (fpsIn_.left ? 1.f : 0.f) + fpsIn_.padMoveX;
        in.lookDX = fpsIn_.dx;
        in.lookDY = fpsIn_.dy;
        in.fire = fpsIn_.fire || fpsIn_.padFire;
        in.selectWeapon = fpsIn_.select;
        in.wheel = fpsIn_.wheel;
        in.run = fpsIn_.run || fpsIn_.padRun;
        if (opts_.bot) {
            // Aim at the nearest living enemy's chest; advance when it is far or hidden.
            // Stick with one target for a few seconds. Picking the nearest every frame
            // makes the bot oscillate between two equidistant demons and never reach
            // either, which is exactly what it did in the crypt's long corridors.
            const std::vector<Enemy>& all = fps_.enemies();
            botTargetT_ -= dt;
            const Enemy* held = (botTargetIdx_ >= 0 && botTargetIdx_ < static_cast<int>(all.size()) && all[static_cast<size_t>(botTargetIdx_)].alive())
                              ? &all[static_cast<size_t>(botTargetIdx_)] : nullptr;
            const float heldDist = held ? glm::length(held->pos - fps_.eye()) : 1e9f;
            int nearestIdx = -1;
            float best = 1e9f;
            for (size_t i = 0; i < all.size(); ++i) {
                if (!all[i].alive()) continue;
                const float d = glm::length(all[i].pos - fps_.eye());
                if (d < best) { best = d; nearestIdx = static_cast<int>(i); }
            }
            if (!held || botTargetT_ <= 0.f || best < heldDist * 0.6f) {
                botTargetIdx_ = nearestIdx;
                botTargetT_ = 3.f;
                botHaveWaypoint_ = false;
                botNavT_ = 0.f;
            }
            const Enemy* target = (botTargetIdx_ >= 0 && botTargetIdx_ < static_cast<int>(all.size()) && all[static_cast<size_t>(botTargetIdx_)].alive())
                                ? &all[static_cast<size_t>(botTargetIdx_)] : nullptr;
            // The boss hall is locked: fetch the key first, or the bot bumps the door
            // for the rest of the fight.
            glm::vec3 errand(0.f);
            bool onErrand = false;
            if (fps_.stage() == FpsMode::Stage::Dungeon && !fps_.hasKey())
                for (const Pickup& p : fps_.pickups())
                    if (p.kind == PickupKind::Key) { errand = p.pos; onErrand = true; break; }
            if (onErrand && (!target || glm::length(target->pos - fps_.eye()) > 6.f)) {
                botNavT_ -= dt;
                if (botNavT_ <= 0.f) { botNavT_ = 0.2f; botHaveWaypoint_ = fps_.navNext(fps_.playerPos(), errand, botWaypoint_, *game_); }
                glm::vec3 w = (botHaveWaypoint_ ? botWaypoint_ : errand) - fps_.playerPos();
                w.y = 0.f;
                if (glm::length(w) > 1e-3f) {
                    const float wantYaw = std::atan2(w.x, w.z);
                    const float dyaw = std::remainder(wantYaw - fps_.yaw(), 2.f * kPi);
                    in.lookDX = -dyaw / 0.0022f;
                    in.lookDY = -fps_.pitch() / 0.0022f;
                    in.moveZ = std::fabs(dyaw) < 0.6f ? 1.f : 0.3f;
                    in.run = true;
                }
                botUnstick(in, dt);
                fps_.update(dt, in, *game_);
                handleFpsEvents();
                if (fps_.playerDead()) { if (modeT_ > 0.f && fps_.damageFlash() <= 0.f) enterMode(Mode::GameOver); }
                else if (fps_.finished()) enterMode(Mode::FlyOut);
                break;
            }
            if (target) {
                glm::vec3 chest = target->pos + glm::vec3(0.f, target->height * 0.7f, 0.f);
                glm::vec3 to = chest - fps_.eye();
                float wantYaw = std::atan2(to.x, to.z);
                float wantPitch = std::atan2(to.y, std::sqrt(to.x * to.x + to.z * to.z));
                float len = glm::length(to);
                bool clear = fps_.rayBlockDistance(fps_.eye(), to / len, len, *game_) >= len - 0.01f;
                // Out of sight (behind blocks, or somewhere in the crypt): walk the shortest
                // path towards it instead, facing the way we go.
                botNavT_ -= dt;
                if (!clear && botNavT_ <= 0.f) { botNavT_ = 0.2f; botHaveWaypoint_ = fps_.navNext(fps_.playerPos(), target->pos, botWaypoint_, *game_); }
                if (!clear && botHaveWaypoint_) {
                    glm::vec3 w = botWaypoint_ - fps_.playerPos();
                    w.y = 0.f;
                    if (glm::length(w) > 1e-3f) { wantYaw = std::atan2(w.x, w.z); wantPitch = 0.f; }
                }
                float dyaw = std::remainder(wantYaw - fps_.yaw(), 2.f * kPi);
                in.lookDX = -dyaw / 0.0022f;
                in.lookDY = -(wantPitch - fps_.pitch()) / 0.0022f;
                in.fire = std::fabs(dyaw) < 0.03f && clear;
                // Best owned weapon with ammo: rockets at range, plasma, chaingun, shotgun.
                auto usable = [&](int id) { return fps_.weapon(id).owned && (weaponDef(id).ammoPerPickup == 0 || fps_.weapon(id).ammo > 0); };
                int want = kShotgun;
                if (usable(kChaingun)) want = kChaingun;
                if (usable(kPlasmaRifle)) want = kPlasmaRifle;
                if (usable(kRocketLauncher) && len > 4.5f) want = kRocketLauncher;
                if (want != fps_.currentWeapon()) in.selectWeapon = want;
                if (std::getenv("REDLINE_LOG_BOT") && frameCount_ % 120 == 0)
                    std::fprintf(stderr, "[bot] target %s at %.1fm clear=%d waypoint=%d stuck=%.1f key=%d door=%d\n",
                                 assets_.enemies[std::clamp(target->kind, 0, kEnemyKinds - 1)].name.c_str(), len, clear ? 1 : 0,
                                 botHaveWaypoint_ ? 1 : 0, botStuckT_, fps_.hasKey() ? 1 : 0, fps_.doorOpen() ? 1 : 0);
                if (clear) {
                    botBlockedT_ = 0.f;
                    in.moveZ = len > 7.f ? 1.f : 0.f;
                    in.run = len > 7.f;
                    in.moveX = std::sin(time_ * 1.7f) * 0.6f;
                } else if (botHaveWaypoint_) {
                    in.moveZ = std::fabs(dyaw) < 0.6f ? 1.f : 0.3f;   // turn first, then go
                    in.run = true;
                } else {
                    // Shot blocked by a block: sidestep, flipping direction every so often.
                    botBlockedT_ += dt;
                    if (botBlockedT_ > 1.5f) { botSide_ = -botSide_; botBlockedT_ = 0.f; }
                    in.moveX = botSide_;
                    in.moveZ = 0.4f;
                }
            }
        }
        if (opts_.bot) botUnstick(in, dt);
        fps_.update(dt, in, *game_);
        handleFpsEvents();
        if (fps_.playerDead()) {
            if (modeT_ > 0.f && fps_.damageFlash() <= 0.f) enterMode(Mode::GameOver);
        } else if (fps_.finished()) {
            enterMode(Mode::FlyOut);
        }
        break;
    }
    case Mode::FlyOut:
        if (modeT_ >= kFlyOutTime) enterMode(Mode::Blocks);
        break;
    case Mode::GameOver:
        gameOverT_ += dt;
        break;
    case Mode::Paused:
        break;
    }
}

// ---------------------------------------------------------------------------
// Board geometry: a hinge along the board's bottom edge tips the whole wall
// forward onto the floor for the first-person phase.
float App::boardTilt() const {
    switch (mode_) {
    case Mode::FlyIn: return smoothstep(modeT_ / kFlyInTime);
    case Mode::Fps: case Mode::Countdown: return 1.f;
    case Mode::FlyOut: return 1.f - smoothstep(modeT_ / kFlyOutTime);
    case Mode::GameOver: return diedInFps_ ? 1.f : 0.f;
    case Mode::Paused: return (pausedFrom_ == Mode::Fps || pausedFrom_ == Mode::Countdown) ? 1.f : 0.f;
    default: return 0.f;
    }
}

glm::vec3 App::boardPosH(float x, float h) const {
    float th = boardTilt() * kPi * 0.5f;
    return {x, h * std::cos(th) + 0.5f * std::sin(th), h * std::sin(th)};
}

glm::vec3 App::boardPos(float col, float row) const {
    return boardPosH(col - core::kBoardW * 0.5f + 0.5f, static_cast<float>(core::kBoardH - 1) - row + 0.5f);
}

App::Camera App::blocksCamera() const {
    Camera c;
    c.eye = {0.f, 11.0f, 20.5f};
    c.target = {0.f, 10.2f, 0.f};
    c.fov = 60.f;
    return c;
}

App::Camera App::fpsCamera() const {
    Camera c;
    c.eye = fps_.eye();
    // The view kicks up a little when a gun goes off and settles back.
    const float p = fps_.pitch() + viewKick_, yaw = fps_.yaw();
    c.target = c.eye + glm::normalize(glm::vec3(std::sin(yaw) * std::cos(p), std::sin(p), std::cos(yaw) * std::cos(p)));
    c.fov = 80.f;
    return c;
}

App::Camera App::currentCamera() const {
    auto lerpCam = [&](float t) {
        Camera c;
        c.eye = glm::mix(flyFrom_.eye, flyTo_.eye, t);
        c.target = glm::mix(flyFrom_.target, flyTo_.target, t);
        c.fov = glm::mix(flyFrom_.fov, flyTo_.fov, t);
        return c;
    };
    switch (mode_) {
    case Mode::Fps: case Mode::Countdown: return fpsCamera();
    case Mode::FlyIn: {
        // Swing high over the tipping board, then settle to eye level.
        float t = smoothstep(modeT_ / kFlyInTime);
        Camera c = lerpCam(t);
        c.eye.y += 6.f * std::sin(t * kPi);
        return c;
    }
    case Mode::FlyOut: {
        float t = smoothstep(modeT_ / kFlyOutTime);
        Camera c = lerpCam(t);
        c.eye.y += 6.f * std::sin(t * kPi);
        return c;
    }
    case Mode::Paused:
        return (pausedFrom_ == Mode::Fps || pausedFrom_ == Mode::Countdown) ? fpsCamera() : blocksCamera();
    case Mode::GameOver: {
        if (!diedInFps_) return blocksCamera();
        // Death camera: the view sinks to the floor and rolls over the first second.
        Camera c = fpsCamera();
        float t = std::clamp(gameOverT_ / 1.1f, 0.f, 1.f);
        t = t * t * (3.f - 2.f * t);
        glm::vec3 fwd = glm::normalize(c.target - c.eye);
        c.eye.y = glm::mix(c.eye.y, 0.35f, t);
        c.target = c.eye + fwd;
        float roll = 0.55f * t;
        glm::vec3 right = glm::normalize(glm::cross(fwd, glm::vec3(0.f, 1.f, 0.f)));
        c.up = glm::normalize(glm::vec3(0.f, 1.f, 0.f) * std::cos(roll) + right * std::sin(roll));
        c.fov = glm::mix(80.f, 66.f, t);
        return c;
    }
    case Mode::Alert: {
        Camera c = blocksCamera();
        float t = smoothstep(modeT_ / kAlertTime);
        c.eye = glm::mix(c.eye, glm::vec3(0.f, 7.f, 16.f), t * 0.5f);
        return c;
    }
    default:
        return blocksCamera();
    }
}

// ---------------------------------------------------------------------------
void App::cube(glm::vec3 pos, float scale, glm::vec4 color, const std::string& tex, glm::vec3 emissive, float emissiveStrength, float flags, float phase, float rotX) {
    const render::AtlasRegion& r = assets_.region(tex);
    render::CubeInstance c;
    c.posScale = glm::vec4(pos, scale);
    c.color = color;
    c.emissive = glm::vec4(emissive, emissiveStrength);
    c.uvRect = glm::vec4(r.u0, r.v0, r.u1, r.v1);
    // Blocks and the frame are glossy tiles: smoother, and reflective (flag bit 2) under ray tracing.
    const bool glossy = tex == assets_.block || tex == assets_.redBlock;
    c.params = glm::vec4(glossy ? 0.42f : 0.7f, 0.05f, phase, flags + (glossy ? 2.f : 0.f));
    c.rot = glm::vec4(rotX, 0.f, 0.f, 0.f);
    cubes_.push_back(c);
}

void App::buildEnvironment() {
    envCubes_.clear();
    envRanges_.clear();
    // Floor cubes first, then walls: each group is one draw range with its own material slot.
    std::vector<render::CubeInstance> floorCubes, wallCubes;
    auto push = [&](glm::vec3 pos, const std::string& tex, glm::vec4 color) {
        const render::AtlasRegion& r = assets_.region(tex);
        render::CubeInstance c;
        c.posScale = glm::vec4(pos, 1.f);
        c.color = color;
        c.emissive = glm::vec4(0.f);
        c.uvRect = glm::vec4(r.u0, r.v0, r.u1, r.v1);
        const bool floor = tex == assets_.floor;
        // The floor is the glossy, reflective surface (flag bit 2): polished enough for the
        // ray-traced reflections to read, the roughness map still breaks them up.
        c.params = glm::vec4(floor ? 0.22f : 0.9f, 0.f, 0.f, floor ? 2.f : 0.f);
        c.rot = glm::vec4(0.f);
        (floor ? floorCubes : wallCubes).push_back(c);
    };
    const int halfW = 15;
    const int depth = 23;
    const int height = core::kBoardH + 3;
    // Floor (top face at y = 0) and ceiling
    for (int x = -halfW; x < halfW; ++x)
        for (int z = -2; z < depth; ++z) {
            push({x + 0.5f, -0.5f, z + 0.5f}, assets_.floor, glm::vec4(1.f));
            // No ceiling: the sun has to reach the floor for the shadow map to mean anything.
        }
    // Back wall behind the board, side walls, front wall behind the overview camera.
    // Once the arena is cleared the gate section of the back wall is gone (it falls,
    // drawn by addFpsActors while it does) and the dungeon stands behind it.
    const bool gateOpen = fps_.stage() != FpsMode::Stage::Arena;
    envStage_ = static_cast<int>(fps_.stage());
    envDungeon_ = !fps_.dungeon().empty();
    envDoorOpen_ = fps_.doorOpen();
    for (int y = 0; y < height; ++y) {
        for (int x = -halfW; x < halfW; ++x) {
            if (!(gateOpen && std::fabs(x + 0.5f) < Dungeon::kGateHalf && y < kGateHeight))
                push({x + 0.5f, y + 0.5f, -2.5f}, assets_.wall, glm::vec4(0.85f, 0.85f, 0.85f, 1.f));
            push({x + 0.5f, y + 0.5f, depth + 0.5f}, assets_.wall, glm::vec4(0.7f, 0.7f, 0.7f, 1.f));
        }
        for (int z = -2; z < depth; ++z) {
            push({-halfW - 0.5f, y + 0.5f, z + 0.5f}, assets_.wall, glm::vec4(0.75f, 0.75f, 0.75f, 1.f));
            push({halfW + 0.5f, y + 0.5f, z + 0.5f}, assets_.wall, glm::vec4(0.75f, 0.75f, 0.75f, 1.f));
        }
    }
    if (envDungeon_) {
        // The crypt: floor and ceiling over every floor tile, walls three high wherever
        // rock borders floor. Darker and greener than the arena, a lintel over the gate.
        const Dungeon& d = fps_.dungeon();
        const glm::vec4 floorTint(0.55f, 0.52f, 0.46f, 1.f), wallTint(0.58f, 0.6f, 0.52f, 1.f), ceilTint(0.36f, 0.36f, 0.34f, 1.f);
        for (int j = 0; j < d.depth(); ++j)
            for (int i = 0; i < d.width(); ++i) {
                const glm::vec3 c = d.tileCentre(i, j);
                const int h = d.ceilingAt(i, j);
                if (d.tile(i, j) == Dungeon::Floor) {
                    push({c.x, -0.5f, c.z}, assets_.floor, floorTint);
                    push({c.x, static_cast<float>(h) + 0.5f, c.z}, assets_.wall, ceilTint);
                } else if (d.tile(i, j) == Dungeon::Wall) {
                    for (int y = 0; y < h; ++y) push({c.x, y + 0.5f, c.z}, assets_.wall, wallTint);
                }
            }
        // The way into the boss hall: a slab of rock across the corridor while it is shut.
        if (fps_.stage() == FpsMode::Stage::Dungeon && !fps_.doorOpen())
            for (const auto& [ti, tj] : d.doorTiles()) {
                const glm::vec3 c = d.tileCentre(ti, tj);
                for (int y = 0; y < d.ceilingAt(ti, tj); ++y) push({c.x, y + 0.5f, c.z}, assets_.wall, glm::vec4(0.75f, 0.22f, 0.18f, 1.f));
            }
        // The wall's footprint gets a floor: the passage runs across the rail and through it.
        for (float x = -Dungeon::kGateHalf + 0.5f; x < Dungeon::kGateHalf; x += 1.f) push({x, -0.5f, -2.5f}, assets_.floor, floorTint);
    }
    envRanges_.push_back({0, static_cast<uint32_t>(floorCubes.size()), materialSlot(assets_.floorLump)});
    envRanges_.push_back({static_cast<uint32_t>(floorCubes.size()), static_cast<uint32_t>(wallCubes.size()), materialSlot(assets_.wallLump)});
    envCubes_ = std::move(floorCubes);
    envCubes_.insert(envCubes_.end(), wallCubes.begin(), wallCubes.end());
}

// Torches, lamps and barrels around the arena, each with its own flickering light.
void App::buildProps() {
    props_.clear();
    auto prop = [&](const SpriteAnim& a, glm::vec3 pos, float px, glm::vec3 lc, float lr, float lh) {
        if (a.empty()) return;
        props_.push_back({&a, pos, px, lc, lr, lh, static_cast<float>(props_.size()) * 1.7f});
    };
    const glm::vec3 red(1.f, 0.42f, 0.12f), blue(0.35f, 0.5f, 1.f), green(0.4f, 1.f, 0.45f), warm(1.f, 0.75f, 0.45f), white(0.9f, 0.9f, 1.f);
    // Torches along the dungeon's walls, while there is one.
    for (const DungeonTorch& t : fps_.dungeon().torches()) prop(assets_.torchRed, t.pos, 0.031f, warm, 6.5f, 1.7f);
    // Red torches flank the board against the back wall.
    prop(assets_.torchRed, {-7.5f, 0.f, -1.85f}, 0.031f, red, 7.f, 1.7f);
    prop(assets_.torchRed, {7.5f, 0.f, -1.85f}, 0.031f, red, 7.f, 1.7f);
    prop(assets_.torchRed, {-12.5f, 0.f, -1.85f}, 0.031f, red, 6.f, 1.7f);
    prop(assets_.torchRed, {12.5f, 0.f, -1.85f}, 0.031f, red, 6.f, 1.7f);
    // Blue torches along the side walls, green ones in the far corners.
    for (float z : {4.f, 11.f, 18.f}) {
        prop(assets_.torchBlue, {-14.85f, 0.f, z}, 0.031f, blue, 6.f, 1.7f);
        prop(assets_.torchBlue, {14.85f, 0.f, z}, 0.031f, blue, 6.f, 1.7f);
    }
    prop(assets_.torchGreen, {-9.f, 0.f, 22.85f}, 0.031f, green, 6.f, 1.7f);
    prop(assets_.torchGreen, {9.f, 0.f, 22.85f}, 0.031f, green, 6.f, 1.7f);
    // Column lamps along the front wall: this is the wall at the player's back
    // in the fight, so they light the near end of the arena.
    for (float x : {-11.5f, -4.f, 4.f, 11.5f}) prop(assets_.lamp, {x, 0.f, 22.6f}, 0.031f, white, 14.f, 3.2f);   // tall reach so the floor catches them
    // Candelabras and column lamps on the floor either side of the board; barrels in the corners.
    prop(assets_.candelabra, {-8.5f, 0.f, 2.5f}, 0.031f, warm, 5.f, 1.2f);
    prop(assets_.candelabra, {8.5f, 0.f, 2.5f}, 0.031f, warm, 5.f, 1.2f);
    prop(assets_.lamp, {-11.f, 0.f, 12.f}, 0.031f, white, 6.f, 1.4f);
    prop(assets_.lamp, {11.f, 0.f, 12.f}, 0.031f, white, 6.f, 1.4f);
    prop(assets_.barrel, {-13.5f, 0.f, 21.5f}, 0.031f, green, 2.5f, 0.6f);
    prop(assets_.barrel, {13.8f, 0.f, 21.f}, 0.031f, green, 2.5f, 0.6f);
    prop(assets_.barrel, {-14.f, 0.f, -0.5f}, 0.031f, green, 2.5f, 0.6f);
    prop(assets_.barrel, {14.f, 0.f, 0.f}, 0.031f, green, 2.5f, 0.6f);
}

void App::addDecor() {
    for (const Prop& p : props_) {
        bool flip = false;
        const std::string& key = animFrame(*p.anim, time_ + p.phase, true, &flip);
        // Flames and lamps sit above 1.0 so they bloom; barrels and candelabras stay plain.
        const bool glows = p.anim == &assets_.torchRed || p.anim == &assets_.torchBlue || p.anim == &assets_.torchGreen || p.anim == &assets_.lamp;
        if (!key.empty()) actor(key, p.pos, p.px, glows ? glm::vec4(1.5f, 1.5f, 1.5f, 1.f) : glm::vec4(1.f), true, flip, 0.f);
        // A soft flare disc over each flame, flickering with the light (community pack).
        const int flare = p.anim == &assets_.torchRed ? 0 : p.anim == &assets_.torchBlue ? 2 : p.anim == &assets_.torchGreen ? 3 : p.anim == &assets_.lamp ? 4 : p.anim == &assets_.candelabra ? 1 : -1;
        if (flare >= 0 && !assets_.brFlare[flare].empty()) {
            const float f = 0.8f + 0.2f * std::sin(time_ * 9.f + p.phase * 3.f) * std::sin(time_ * 4.7f + p.phase);
            const float size = (flare == 4 ? 0.024f : flare == 1 ? 0.012f : 0.018f) * f;
            billboard(assets_.brFlare[flare].frames[0], p.pos + glm::vec3(0.f, p.lightHeight - 0.35f, 0.f), size, glm::vec4(1.f, 1.f, 1.f, 0.55f * f), false, false);
        }
    }
}

// The brawlers beside the board, drawn like enemies.
void App::addAmbient() {
    for (const Brawler& b : ambient_.brawlers()) {
        const EnemyArt& art = assets_.enemies[std::clamp(b.tier, 0, kEnemyTiers - 1)];
        const SpriteAnim* anim = &art.walk;
        bool loop = true;
        float t = b.animT;
        glm::vec4 tint = art.tint;
        switch (b.state) {
        case Brawler::State::Emerging: t = std::max(0.f, b.stateT); break;
        case Brawler::State::Idle: break;
        case Brawler::State::Attack: anim = &art.attack; loop = false; t = b.stateT; break;
        case Brawler::State::Pain: anim = &art.pain; loop = false; t = b.stateT; tint *= glm::vec4(1.f, 0.7f, 0.7f, 1.f); break;
        case Brawler::State::Dying: anim = &art.death; loop = false; t = b.stateT; break;
        case Brawler::State::Dead: anim = &art.death; loop = false; t = 100.f; break;
        }
        float scale = 1.f;
        if (b.state == Brawler::State::Dead) {
            float life = 1.6f, fadeStart = life - 0.35f;
            if (b.stateT >= life) continue;
            if (b.stateT > fadeStart) { float k = 1.f - (b.stateT - fadeStart) / 0.35f; scale = 0.15f + 0.85f * k; tint *= glm::vec4(k, k, k, 1.f); }
        }
        bool flip = false;
        const std::string& key = animFrame(*anim, t, loop, &flip);
        // Face the opponent: mirror when it is to the left.
        if (b.facingLeft) flip = !flip;
        float yaw = b.facingLeft ? -glm::half_pi<float>() : glm::half_pi<float>();
        if (b.target >= 0 && b.target < static_cast<int>(ambient_.brawlers().size())) {
            glm::vec3 d = ambient_.brawlers()[static_cast<size_t>(b.target)].pos - b.pos;
            if (std::fabs(d.x) + std::fabs(d.z) > 1e-3f) yaw = std::atan2(d.x, d.z);
        }
        if (!key.empty()) actor(key, b.pos, art.metresPerPixel * scale, tint, true, flip, yaw);
    }
    for (const BrawlProjectile& p : ambient_.projectiles()) {
        bool flip = false;
        const SpriteAnim& anim = assets_.projectile[std::clamp(p.type, 0, kProjectileTypes - 1)];
        const std::string& key = animFrame(anim, p.animT, true, &flip);
        if (!key.empty()) actor(key, p.pos - glm::vec3(0.f, 0.3f, 0.f), 0.031f, glm::vec4(1.6f, 1.6f, 1.6f, 1.f), false, flip, std::atan2(p.vel.x, p.vel.z), glm::vec3(1.f, 0.8f, 0.5f), 0.4f);   // fullbright: pushed past 1.0 so it blooms
    }
    for (const BrawlBlast& bl : ambient_.blasts()) {
        const SpriteAnim& anim = assets_.projectileHit[std::clamp(bl.hitType, 0, kProjectileTypes - 1)];
        int n = static_cast<int>(anim.frames.size());
        if (n == 0) continue;
        int i = std::clamp(static_cast<int>(bl.t / bl.duration * n), 0, n - 1);
        actor(anim.frames[static_cast<size_t>(i)], bl.pos - glm::vec3(0.f, 0.2f, 0.f), 0.031f * 1.2f, glm::vec4(1.f), false, anim.mirrored[static_cast<size_t>(i)], 0.f, glm::vec3(1.f, 0.8f, 0.5f), 0.4f);
    }
}

bool App::projectToScreen(glm::vec3 world, float& x, float& y) const {
    glm::vec4 clip = frame_.proj * frame_.view * glm::vec4(world, 1.f);
    if (clip.w <= 0.05f) return false;
    glm::vec3 ndc = glm::vec3(clip) / clip.w;
    VkExtent2D ext = renderer_->extent();
    x = (ndc.x * 0.5f + 0.5f) * static_cast<float>(ext.width);
    y = (1.f - (ndc.y * 0.5f + 0.5f)) * static_cast<float>(ext.height);   // +Y up in NDC (negative viewport), top-left screen origin
    return ndc.x > -1.2f && ndc.x < 1.2f && ndc.y > -1.2f && ndc.y < 1.2f;
}

// Health bars for the three toughest living monsters: a list in the corner and a bar over each head.
void App::addHealthBars(float W, float H, float s, float lh) {
    (void)H;
    std::vector<const Enemy*> top;
    for (const Enemy& e : fps_.enemies()) if (e.alive()) top.push_back(&e);
    std::sort(top.begin(), top.end(), [](const Enemy* a, const Enemy* b) { return a->tier != b->tier ? a->tier > b->tier : a->maxHp > b->maxHp; });
    if (top.size() > 3) top.resize(3);
    const glm::vec4 dim(0.8f, 0.8f, 0.8f, 1.f);
    float y = 24.f;
    for (size_t i = 0; i < top.size(); ++i) {
        const Enemy& e = *top[i];
        const EnemyArt& art = assets_.enemies[std::clamp(e.kind, 0, kEnemyKinds - 1)];
        float frac = std::clamp(e.hp / std::max(1.f, e.maxHp), 0.f, 1.f);
        glm::vec4 col = frac > 0.5f ? glm::vec4(0.35f, 0.9f, 0.35f, 1.f) : frac > 0.25f ? glm::vec4(1.f, 0.8f, 0.2f, 1.f) : glm::vec4(1.f, 0.25f, 0.2f, 1.f);
        float bw = 150.f * s * 0.5f, bh = 6.f * s * 0.5f;
        // Corner list (right side, under the hints).
        float lx = W - 24.f - bw;
        text(W - 24.f, y, art.name + "  " + std::to_string(static_cast<int>(std::ceil(e.hp))), s * 0.7f, e.tier >= 4 ? glm::vec4(1.f, 0.85f, 0.3f, 1.f) : dim, 2);
        panel(lx, y + lh * 0.75f, bw, bh, glm::vec4(0.f, 0.f, 0.f, 0.7f));
        panel(lx, y + lh * 0.75f, bw * frac, bh, col);
        y += lh * 1.35f;
        // Floating bar over the head.
        float sx, sy;
        if (projectToScreen(e.pos + glm::vec3(0.f, e.height + 0.35f, 0.f), sx, sy)) {
            float fw = 90.f * s * 0.5f, fh = 5.f * s * 0.5f;
            panel(sx - fw * 0.5f - 1.f, sy - 1.f, fw + 2.f, fh + 2.f, glm::vec4(0.f, 0.f, 0.f, 0.75f));
            panel(sx - fw * 0.5f, sy, fw * frac, fh, col);
            text(sx, sy - lh * 0.65f, art.name, s * 0.55f, glm::vec4(1.f, 1.f, 1.f, 0.9f), 1);
        }
    }
}

void App::addBoard() {
    const core::Game& g = *game_;
    const std::vector<int>& clearing = g.clearingRows();
    float flash = 0.5f + 0.5f * std::sin(g.clearProgress() * kPi * 3.f);
    bool alert = (mode_ == Mode::Alert);
    float alertFlash = alert ? (0.5f + 0.5f * std::sin(modeT_ * 18.f)) : 0.f;
    float rotX = boardTilt() * kPi * 0.5f;
    const glm::vec4 frameCol(0.25f, 0.25f, 0.3f, 1.f);

    // Frame: side rails and the far lip, hinged with the board.
    const float halfW = core::kBoardW * 0.5f + 0.5f;
    for (int i = 0; i <= core::kBoardH; ++i) {
        float h = static_cast<float>(i) + 0.5f;
        cube(boardPosH(-halfW, h), 1.f, frameCol, assets_.block, {}, 0.f, 0.f, 0.f, rotX);
        cube(boardPosH(halfW, h), 1.f, frameCol, assets_.block, {}, 0.f, 0.f, 0.f, rotX);
    }
    for (int c = 0; c < core::kBoardW; ++c)
        cube(boardPosH(static_cast<float>(c) - core::kBoardW * 0.5f + 0.5f, core::kBoardH + 0.5f), 1.f, frameCol, assets_.block, {}, 0.f, 0.f, 0.f, rotX);

    auto redCube = [&](glm::vec3 pos, float extraGlow, float phase) {
        cube(pos, 0.96f, glm::vec4(1.f, 0.55f, 0.55f, 1.f), assets_.redBlock, glm::vec3(1.f, 0.12f, 0.05f), 0.8f + extraGlow, 1.f, phase, rotX);
    };

    for (int r = 0; r < core::kBoardH; ++r) {
        bool rowClearing = std::find(clearing.begin(), clearing.end(), r) != clearing.end();
        bool rowRed = g.rowAllRed(r);
        for (int c = 0; c < core::kBoardW; ++c) {
            const core::Cell& cell = g.at(c, r);
            glm::vec3 pos = boardPos(static_cast<float>(c), static_cast<float>(r));
            if (cell.spawning()) {
                // Evil filling a hole: a red block growing out of nothing, flickering faster as it solidifies.
                float left = cell.corrupt / std::max(0.01f, g.rules().evilSpawnTime);   // 1 -> 0
                float grow = 0.25f + 0.7f * (1.f - left);
                float freq = 10.f + 30.f * (1.f - left);
                bool on = std::sin(time_ * freq + r * 0.9f) > -0.3f;
                if (on) cube(pos, 0.96f * grow, glm::vec4(1.f, 0.5f, 0.5f, 1.f), assets_.redBlock, glm::vec3(1.f, 0.12f, 0.05f), 1.2f, 1.f, static_cast<float>(c) + r, rotX);
                continue;
            }
            if (cell.empty()) continue;
            if (cell.red()) {
                redCube(pos, rowRed ? 0.9f * alertFlash : 0.f, static_cast<float>(c) * 0.7f + r);
            } else if (cell.corrupting()) {
                // Flickers between its own colour and red, faster as the switch approaches.
                float left = cell.corrupt / std::max(0.01f, g.rules().corruptionTime);   // 1 -> 0
                float freq = 8.f + 30.f * (1.f - left);
                bool redNow = std::sin(time_ * freq + c * 1.3f) > 0.f;
                glm::vec3 col = redNow ? glm::vec3(1.f, 0.55f, 0.55f) : kPieceColors[cell.color % 7];
                cube(pos, 0.96f, glm::vec4(col, 1.f), redNow ? assets_.redBlock : assets_.block, glm::vec3(1.f, 0.12f, 0.05f), redNow ? 0.9f : 0.f, 0.f, 0.f, rotX);
            } else {
                glm::vec3 col = kPieceColors[cell.color % 7];
                if (rowClearing) col = glm::mix(col, glm::vec3(1.f), flash);
                cube(pos, 0.96f, glm::vec4(col, 1.f), assets_.block, rowClearing ? glm::vec3(1.f) : glm::vec3(0.f), rowClearing ? flash : 0.f, 0.f, 0.f, rotX);
            }
        }
    }
    // While the board tips over, the regions that will become monsters are
    // still shown as red blocks; they sink away as the monsters rise.
    if (mode_ == Mode::FlyIn || mode_ == Mode::Fps || mode_ == Mode::Countdown) {
        for (const Enemy& e : fps_.enemies()) {
            if (mode_ != Mode::FlyIn && !(e.state == Enemy::State::Emerging && e.stateT < 0.45f)) continue;
            float sink = (mode_ != Mode::FlyIn) ? std::clamp(e.stateT / 0.45f, 0.f, 1.f) : 0.f;
            for (auto [c, r] : e.cells) {
                glm::vec3 pos = boardPos(static_cast<float>(c), static_cast<float>(r));
                pos.y -= sink * 1.2f;
                redCube(pos, 0.6f, static_cast<float>(c) * 0.7f + r);
            }
        }
    }
    bool showPiece = (mode_ == Mode::Blocks || mode_ == Mode::Title || (mode_ == Mode::Paused && pausedFrom_ == Mode::Blocks));
    if (showPiece) {
        if (auto ghost = g.ghost()) {
            for (int i = 0; i < 4; ++i) {
                auto [cx, cy] = ghost->cells()[i];
                if (cy < 0) continue;
                glm::vec3 col = ghost->red[i] ? glm::vec3(0.9f, 0.2f, 0.2f) : kPieceColors[static_cast<int>(ghost->shape)];
                cube(boardPos(static_cast<float>(cx), static_cast<float>(cy)), 0.3f, glm::vec4(col * 0.8f, 1.f), assets_.block, col, 0.35f);
            }
        }
        if (const auto& p = g.active()) {
            for (int i = 0; i < 4; ++i) {
                auto [cx, cy] = p->cells()[i];
                if (cy < 0) continue;
                glm::vec3 pos = boardPos(static_cast<float>(cx), static_cast<float>(cy));
                if (p->red[i]) redCube(pos, 0.1f, static_cast<float>(i));
                else cube(pos, 0.96f, glm::vec4(kPieceColors[static_cast<int>(p->shape)], 1.f), assets_.block);
            }
        }
    }
}

void App::billboard(const std::string& key, glm::vec3 feet, float metresPerPixel, glm::vec4 color, bool lit, bool flip) {
    const render::AtlasRegion& r = assets_.region(key);
    render::QuadInstance q;
    q.pos = glm::vec4(feet, 0.f);
    float w = r.w * metresPerPixel, h = r.h * metresPerPixel;
    float ax = r.w > 0 ? static_cast<float>(r.offsetX) / r.w : 0.5f;
    float ay = r.h > 0 ? 1.f - static_cast<float>(r.offsetY) / r.h : 0.f;
    q.size = glm::vec4(w, h, ax, ay);
    q.uvRect = glm::vec4(r.u0, r.v0, r.u1, r.v1);
    q.color = color;
    q.params = glm::vec4(0.f, lit ? 1.f : 0.f, flip ? 1.f : 0.f, 0.f);
    worldQuads_.push_back(q);
}

void App::softBillboard(const std::string& key, glm::vec3 pos, float metresPerPixel, glm::vec4 color) {
    billboard(key, pos, metresPerPixel, color, false, false);
    worldQuads_.back().params.w += 4.f;   // soft alpha (see quad.frag), skipped by the shadow pass
}

void App::actor(const std::string& key, glm::vec3 feet, float metresPerPixel, glm::vec4 color, bool lit, bool flip, float yaw, glm::vec3 emissive, float emissiveStrength) {
    if (useVoxels_) {
        if (const VoxelModel* m = voxels_.get(key)) {
            // Heading (sin yaw, 0, cos yaw): the pack's default AngleOffset of 90 faces +Z here.
            render::MeshInstance mi;
            mi.mesh = m->mesh;
            glm::mat4 M = glm::translate(glm::mat4(1.f), feet);
            M = glm::rotate(M, yaw + glm::radians(m->angleOffset - 90.f), glm::vec3(0.f, 1.f, 0.f));
            M = glm::scale(M, glm::vec3(metresPerPixel * m->scale));
            M = glm::translate(M, -m->pivot);
            mi.model = M;
            mi.color = color;
            mi.emissive = glm::vec4(emissive, emissiveStrength);
            meshes_.push_back(mi);
            return;
        }
    }
    billboard(key, feet, metresPerPixel, color, lit, flip);
}

const std::string& App::animFrame(const SpriteAnim& a, float t, bool loop, bool* flip) const {
    static const std::string empty;
    if (a.empty()) return empty;
    int n = static_cast<int>(a.frames.size());
    int i = static_cast<int>(t * a.fps);
    i = loop ? ((i % n) + n) % n : std::clamp(i, 0, n - 1);
    if (flip) *flip = a.mirrored[static_cast<size_t>(i)];
    return a.frames[static_cast<size_t>(i)];
}

void App::addFpsActors() {
    for (const Enemy& e : fps_.enemies()) {
        const EnemyArt& art = assets_.enemies[std::clamp(e.kind, 0, kEnemyKinds - 1)];
        const SpriteAnim* anim = &art.walk;
        bool loop = true;
        float t = e.animT;
        glm::vec4 tint = art.tint;
        switch (e.state) {
        case Enemy::State::Emerging: t = std::max(0.f, e.stateT); tint *= glm::vec4(1.f, 0.6f, 0.6f, 1.f); break;
        case Enemy::State::Idle: break;
        case Enemy::State::Attack: anim = &art.attack; loop = false; t = e.stateT; break;
        case Enemy::State::Pain: anim = &art.pain; loop = false; t = e.stateT; tint *= glm::vec4(1.f, 0.7f, 0.7f, 1.f); break;
        case Enemy::State::Dying: anim = &art.death; loop = false; t = e.stateT; break;
        case Enemy::State::Dead: anim = &art.death; loop = false; t = 100.f; break;
        }
        // Gibbed: the XDEATH frames where Doom has them (zombies, imps), otherwise the chunks are the corpse.
        // Brutal deaths: the per-weapon animation runs through Dying into Dead (its own pace, then holds).
        float brutalLife = 0.f;
        if (e.gibbed && !e.alive()) {
            if (art.xdeath.empty()) continue;
            anim = &art.xdeath;
        } else if (!e.alive() && brutalActive() && e.deathKind >= 0 && e.deathKind < kDeathKinds && !art.brDeath[e.deathKind].empty()) {
            anim = &art.brDeath[e.deathKind];
            loop = false;
            t = e.state == Enemy::State::Dead ? e.stateT + 0.8f : e.stateT;
            brutalLife = static_cast<float>(anim->frames.size()) / anim->fps;
        }
        // Corpses fade out; cacodemons (a big sprite lying in the way) go quickest.
        float corpseScale = 1.f;
        if (e.state == Enemy::State::Dead) {
            float life = (e.tier == 3) ? 0.35f : (e.tier >= 5 ? 1.2f : 1.6f);
            if (brutalLife > 0.f) life = std::max(life, brutalLife - 0.8f + (e.deathKind == kDeathPlasma ? 0.4f : 1.4f));   // let the animation finish, then fade
            life += 5.f;   // bodies stay around a while before they fade
            float fadeStart = life - 0.35f;
            if (e.stateT >= life) continue;
            if (e.stateT > fadeStart) {
                float k = 1.f - (e.stateT - fadeStart) / 0.35f;
                corpseScale = 0.15f + 0.85f * k;
                tint *= glm::vec4(k, k, k, 1.f);
            }
        }
        if (e.alive() && e.absorbTimer < 4.f && e.tier < kMaxTier) {   // warning: pulsing red as it gets ready to absorb
            float f = 0.5f + 0.5f * std::sin(time_ * (10.f + (4.f - e.absorbTimer) * 4.f));
            tint = glm::mix(tint, glm::vec4(1.f, 0.2f, 0.2f, 1.f), 0.6f * f);
        }
        if (e.growT > 0.f) tint = glm::mix(tint, glm::vec4(1.f, 1.f, 1.f, 1.f), e.growT);
        bool flip = false;
        const std::string& key = animFrame(*anim, t, loop, &flip);
        // The monster's own heading (sprites are camera-facing billboards regardless; voxel
        // models turn with it, so you can get round behind one, and corpses keep it).
        if (!key.empty()) actor(key, e.pos, art.metresPerPixel * corpseScale, tint, true, flip, e.yaw);
    }
    // The arch-vile's flame: a fullbright column on the player's position, brighter as the clasp nears.
    for (const Enemy& e : fps_.enemies()) {
        if (e.state != Enemy::State::Attack || e.fireT < 0.f || assets_.vileFire.empty()) continue;
        bool flip = false;
        const std::string& key = animFrame(assets_.vileFire, e.fireT, true, &flip);
        const float grow = std::min(1.f, 0.55f + 0.45f * e.fireT / 2.1f);
        billboard(key, e.firePos, 0.034f * grow, glm::vec4(2.2f, 1.6f, 1.0f, 0.95f), false, flip);
    }
    for (const Projectile& p : fps_.projectiles()) {
        bool flip = false;
        const SpriteAnim& anim = assets_.projectile[std::clamp(p.type, 0, kProjectileTypes - 1)];
        const std::string& key = animFrame(anim, p.animT, true, &flip);
        if (!key.empty()) actor(key, p.pos - glm::vec3(0.f, 0.3f, 0.f), 0.031f, glm::vec4(1.6f, 1.6f, 1.6f, 1.f), false, flip, std::atan2(p.vel.x, p.vel.z), glm::vec3(1.f, 0.8f, 0.5f), 0.4f);   // fullbright: pushed past 1.0 so it blooms
    }
    for (const Pickup& p : fps_.pickups()) {
        const SpriteAnim& anim = assets_.pickups[std::clamp(static_cast<int>(p.kind), 0, kPickupArt - 1)];
        if (anim.empty()) continue;
        float bob = p.landed ? 0.06f + 0.05f * std::sin(time_ * 4.f + p.pos.x) : 0.f;
        actor(anim.frames[0], p.pos + glm::vec3(0.f, bob, 0.f), 0.031f, glm::vec4(1.f), true, false, time_ * 1.2f + p.pos.x);
    }
    for (const Explosion& ex : fps_.explosions()) {
        float t = ex.t / ex.duration;
        // Blasts are always 2D fireballs (the pack's 25-frame one when present), never voxels.
        const bool blast = ex.hitType < 0;
        const bool packBlast = blast && !assets_.brBlast.empty();
        const bool packPlasma = ex.hitType == kProjPlasma && !assets_.brPlasmaHit.empty();
        const SpriteAnim& anim = blast ? (packBlast ? assets_.brBlast : assets_.explosion) : packPlasma ? assets_.brPlasmaHit : assets_.projectileHit[std::clamp(ex.hitType, 0, kProjectileTypes - 1)];
        int n = static_cast<int>(anim.frames.size());
        if (n == 0) continue;
        int i = std::clamp(static_cast<int>(t * n), 0, n - 1);
        float scale = packBlast ? 0.02f * (1.2f + 0.35f * ex.radius) : blast ? 0.031f * (1.6f + 0.4f * ex.radius) : packPlasma ? 0.011f : 0.031f * 1.2f;
        // Blasts: deeper orange-red, translucent, fading out over the burst; impact puffs stay as drawn.
        const glm::vec4 tint = packBlast ? glm::vec4(1.7f, 1.15f, 0.8f, 0.95f - 0.3f * t) : blast ? glm::vec4(2.0f, 0.6f, 0.25f, 0.9f - 0.45f * t) : glm::vec4(1.6f, 1.6f, 1.6f, 1.f);
        const glm::vec3 glow = blast ? glm::vec3(1.f, 0.32f, 0.1f) : glm::vec3(1.f, 0.7f, 0.4f);
        if (blast) billboard(anim.frames[static_cast<size_t>(i)], ex.pos - glm::vec3(0.f, packBlast ? 0.6f : 0.9f, 0.f), scale, tint, false, anim.mirrored[static_cast<size_t>(i)]);
        else if (packPlasma) billboard(anim.frames[static_cast<size_t>(i)], ex.pos, scale, glm::vec4(1.6f, 1.6f, 1.6f, 1.f), false, false);
        else actor(anim.frames[static_cast<size_t>(i)], ex.pos - glm::vec3(0.f, 0.2f, 0.f), scale, tint, false, anim.mirrored[static_cast<size_t>(i)], 0.f, glow, 0.6f);
    }
    for (const Debris& d : fps_.debris()) {
        float fade = std::min(1.f, d.ttl / 0.4f);
        // A fragment carries the colour of the block it came off; a flat grey one read as a
        // white box against the arena's lighting.
        const glm::vec3 col = d.colorIndex >= 0 ? kPieceColors[d.colorIndex % 7] * 0.8f : d.color;
        if (d.red) cube(d.pos, d.size, glm::vec4(d.color, 1.f), assets_.redBlock, glm::vec3(1.f, 0.1f, 0.05f), 0.6f * fade);
        else cube(d.pos, d.size, glm::vec4(col * fade, 1.f), assets_.block);
    }
    addGore();
    if (fps_.stage() == FpsMode::Stage::GateFalling) {
        // The gate section of the back wall tips over backwards into the crypt, cube by
        // cube, the top rows lagging a little, and sinks into the passage floor.
        const float T = fps_.stageT() / FpsMode::kGateFallTime;
        for (int y = 0; y < kGateHeight; ++y)
            for (float x = -Dungeon::kGateHalf + 0.5f; x < Dungeon::kGateHalf; x += 1.f) {
                const float lag = 0.06f * static_cast<float>(y) + 0.03f * std::fabs(x);
                const float t = std::clamp((T - lag) / 0.7f, 0.f, 1.f);
                const float a = (t * t) * kPi * 0.5f;   // accelerating like a felled tree
                // Pivot on the wall's bottom back edge (y = 0, z = -3): the cube's centre, at
                // (ly, 0.5) from it, swings up and over into -z, then the slab sinks away.
                const float ly = static_cast<float>(y) + 0.5f;
                const float py = ly * std::cos(a) + 0.5f * std::sin(a) - std::max(0.f, T - 0.8f) * 6.f;
                const float pz = -3.f + 0.5f * std::cos(a) - ly * std::sin(a);
                if (py < -1.2f) continue;
                cube(glm::vec3(x, py, pz), 1.f, glm::vec4(0.85f, 0.85f, 0.85f, 1.f), assets_.wall, {}, 0.f, 0.f, 0.f, -a);
            }
    }
}

void App::addLights() {
    frame_.lights.clear();
    glm::vec3 cam = frame_.cameraPos;
    struct Cand { float score; render::PointLight l; };
    std::vector<Cand> cands;
    // The falling piece carries a soft light of its own colour.
    if (game_ && game_->active() && (mode_ == Mode::Blocks || mode_ == Mode::Alert)) {
        const core::Piece& p = *game_->active();
        glm::vec3 centre(0.f);
        bool red = false;
        for (int i = 0; i < 4; ++i) { auto [cx, cy] = p.cells()[i]; centre += boardPos(static_cast<float>(cx), static_cast<float>(cy)); red = red || p.red[i]; }
        centre = centre * 0.25f + glm::vec3(0.f, 0.f, 0.9f);
        cands.push_back({-90.f, {centre, 5.5f, red ? glm::vec3(1.f, 0.25f, 0.1f) : kPieceColors[static_cast<int>(p.shape)], 1.6f}});
    }
    // Decor lights with a torch flicker.
    for (const Prop& p : props_) {
        if (p.lightRadius <= 0.f) continue;
        float f = 0.82f + 0.12f * std::sin(time_ * 9.f + p.phase) + 0.06f * std::sin(time_ * 23.f + p.phase * 3.f);
        // With ray-traced shadows the torches can burn brighter: nothing bleeds through walls any more.
        float boost = (renderer_->rayTracingAvailable() && rtShadows_ >= 2) ? 1.5f : 1.f;
        cands.push_back({glm::length(p.pos - cam) - 2.f, {p.pos + glm::vec3(0.f, p.lightHeight, 0.f), p.lightRadius * 1.3f, p.lightColor, 2.4f * f * boost}});
    }
    // Every red (or turning) cell glows.
    for (int r = 0; r < core::kBoardH; ++r)
        for (int c = 0; c < core::kBoardW; ++c)
            if (game_->at(c, r).red() || game_->at(c, r).corrupting() || game_->at(c, r).spawning()) {
                glm::vec3 p = boardPos(static_cast<float>(c), static_cast<float>(r)) + glm::vec3(0.f, 0.f, 0.8f);
                float pulse = 0.8f + 0.2f * std::sin(time_ * 6.f + c * 0.7f + r);
                cands.push_back({glm::length(p - cam), {p, 3.5f, {1.f, 0.15f, 0.05f}, 1.2f * pulse}});
            }
    for (const BrawlProjectile& p : ambient_.projectiles())
        cands.push_back({glm::length(p.pos - cam) - 4.f, {p.pos, 5.f, {1.f, 0.5f, 0.1f}, 2.2f}});   // the brawlers' fireballs light their corner too
    for (const Brawler& b : ambient_.brawlers())
        if (b.flashT > 0.f) cands.push_back({glm::length(b.pos - cam), {b.pos + glm::vec3(0.f, 1.2f, 0.f), 4.f, {1.f, 0.85f, 0.5f}, 2.f * b.flashT}});
    bool inFps = (mode_ == Mode::Fps || mode_ == Mode::Countdown || mode_ == Mode::FlyIn || mode_ == Mode::FlyOut || (mode_ == Mode::GameOver && diedInFps_));
    if (inFps) {
        for (const Enemy& e : fps_.enemies()) {
            if (e.state == Enemy::State::Dead) continue;
            float glow = (e.tier >= 4) ? 1.4f : 0.9f;
            glm::vec3 col = (e.tier == 4) ? glm::vec3(0.3f, 1.f, 0.3f) : glm::vec3(1.f, 0.2f, 0.05f);
            cands.push_back({glm::length(e.pos - cam) - 5.f, {e.pos + glm::vec3(0.f, 1.f, 0.f), 4.f + e.tier, col, glow}});
            if (e.flashT > 0.f) cands.push_back({-150.f, {e.pos + glm::vec3(0.f, 1.2f, 0.f), 5.f, {1.f, 0.85f, 0.5f}, 2.5f * e.flashT}});
        }
        for (const Explosion& ex : fps_.explosions()) {
            float t = 1.f - ex.t / ex.duration;
            bool big = ex.hitType < 0;
            cands.push_back({-100.f, {ex.pos, big ? 6.f + ex.radius * 2.f : 3.f, {1.f, 0.6f, 0.2f}, (big ? 6.f : 1.5f) * t}});
        }
        for (const Projectile& p : fps_.projectiles()) {
            glm::vec3 col = p.type == kProjBaron ? glm::vec3(0.3f, 1.f, 0.3f) : (p.type == kProjPlasma || p.type == kProjArach) ? glm::vec3(0.4f, 0.6f, 1.f) : p.type == kProjRevenant ? glm::vec3(1.f, 0.75f, 0.4f) : glm::vec3(1.f, 0.5f, 0.1f);
            // Fireballs and rockets light their flight path: a strong light with a wide reach,
            // flickering a little so the glow on the walls moves with the flame.
            const float flicker = 0.9f + 0.1f * std::sin(time_ * 31.f + p.pos.x * 5.f);
            const bool rocket = p.type == kProjRocket || p.type == kProjRevenant;
            cands.push_back({-160.f, {p.pos, rocket ? 8.f : 6.5f, col, (rocket ? 4.2f : 3.2f) * flicker}});
        }
        for (const Pickup& p : fps_.pickups())
            if (p.landed) cands.push_back({glm::length(p.pos - cam), {p.pos + glm::vec3(0.f, 0.4f, 0.f), 1.5f, {0.6f, 0.8f, 1.f}, 0.5f}});
        for (const Enemy& e : fps_.enemies()) {
            if (e.growT > 0.f) cands.push_back({-120.f, {e.pos + glm::vec3(0.f, 1.f, 0.f), 6.f, {1.f, 0.3f, 0.3f}, 4.f * e.growT}});
            if (e.state == Enemy::State::Attack && e.fireT >= 0.f) {   // the vile's flame lights the player's surroundings
                const float flicker = 0.85f + 0.15f * std::sin(time_ * 37.f);
                cands.push_back({-170.f, {e.firePos + glm::vec3(0.f, 0.9f, 0.f), 5.f, {1.f, 0.6f, 0.25f}, (0.8f + 1.0f * std::min(1.f, e.fireT / 2.1f)) * flicker}});
            }
        }
        if (muzzleLight_ > 0.f && mode_ == Mode::Fps) {
            glm::vec3 mc = fps_.currentWeapon() == kPlasmaRifle ? glm::vec3(0.4f, 0.6f, 1.f) : glm::vec3(1.f, 0.8f, 0.4f);
            cands.push_back({-200.f, {fps_.eye() + fps_.forward() * 1.2f, 7.f, mc, 3.f * muzzleLight_}});
            // A small blue disc at the plasma rifle's muzzle (the Doom flash sprites cover the rest).
            const SpriteAnim& fl = assets_.brFlare[2];
            if (fps_.currentWeapon() == kPlasmaRifle && !fl.empty()) {
                glm::vec3 right(-std::cos(fps_.yaw()), 0.f, std::sin(fps_.yaw()));
                billboard(fl.frames[0], fps_.eye() + fps_.forward() * 1.0f + right * 0.18f - glm::vec3(0.f, 0.22f, 0.f), 0.004f * muzzleLight_ + 0.002f, glm::vec4(1.4f, 1.4f, 1.4f, 0.5f * muzzleLight_), false, false);
            }
        }
    }
    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.score < b.score; });
    for (size_t i = 0; i < cands.size() && i < static_cast<size_t>(render::kMaxLights); ++i) frame_.lights.push_back(cands[i].l);
}

// ---------------------------------------------------------------------------
void App::screenSprite(const std::string& key, float x, float y, float scale, glm::vec4 color, float anchorX, float anchorY, bool flip) {
    const render::AtlasRegion& r = assets_.region(key);
    render::QuadInstance q;
    q.pos = glm::vec4(x, y, 0.f, 0.f);
    q.size = glm::vec4(r.w * scale, r.h * scale, anchorX, anchorY);
    q.uvRect = glm::vec4(r.u0, r.v0, r.u1, r.v1);
    q.color = color;
    q.params = glm::vec4(1.f, 0.f, flip ? 1.f : 0.f, hdrQuads_ ? 2.f : 0.f);
    screenQuads_.push_back(q);
}

void App::decal(const std::string& key, glm::vec3 pos, glm::vec3 normal, float size, float yaw, glm::vec4 color) {
    const render::AtlasRegion& r = assets_.region(key);
    render::QuadInstance q;
    const bool floor = normal.y > 0.5f;
    q.pos = glm::vec4(pos, yaw);
    const float aspect = r.w > 0 ? static_cast<float>(r.h) / static_cast<float>(r.w) : 1.f;
    q.size = floor ? glm::vec4(size, size * aspect, 0.5f, 0.5f) : glm::vec4(size, size * aspect, normal.x, normal.z);
    q.uvRect = glm::vec4(r.u0, r.v0, r.u1, r.v1);
    q.color = color;
    q.params = glm::vec4(floor ? 2.f : 3.f, 1.f, 0.f, 0.f);
    worldQuads_.push_back(q);
}

void App::addGore() {
    const bool pack = assets_.brutalPack;
    for (const Gore& g : fps_.gore()) {
        const float fade = std::min(1.f, g.ttl / 0.5f);
        const float age = std::max(0.f, (g.kind == 1 ? 8.f : g.kind == 2 ? 6.f : 3.f) - g.ttl);
        static const glm::vec3 kBlood[3] = {{0.5f, 0.02f, 0.02f}, {0.12f, 0.5f, 0.05f}, {0.1f, 0.2f, 0.75f}};
        const glm::vec3 bc = kBlood[std::clamp(g.color, 0, 2)];
        if (g.kind == 0) {
            cube(g.pos, g.size, glm::vec4(bc * fade, 1.f), assets_.white);
        } else if (g.kind == 1) {
            // Meat: a voxel gib when the packs are there, a sprite chunk from the pack, else a dark cube.
            const float k = g.size / 0.085f;
            const VoxelModel* m = useVoxels_ ? voxels_.get("GIB" + std::to_string(g.variant % 10) + "_" + ((g.variant & 16) ? "A" : "B")) : nullptr;
            const int c = std::clamp(g.color, 0, 2);
            if (m) {
                // The voxel gibs are red meat; green and blue monsters get theirs tinted.
                static const glm::vec3 kMeatTint[3] = {{1.f, 1.f, 1.f}, {0.35f, 1.1f, 0.35f}, {0.4f, 0.55f, 1.3f}};
                render::MeshInstance mi;
                mi.mesh = m->mesh;
                glm::mat4 M = glm::translate(glm::mat4(1.f), g.pos);
                M = glm::rotate(M, g.spin * age, glm::vec3(0.f, 1.f, 0.f));
                M = glm::rotate(M, g.spin * 0.6f * age, glm::vec3(1.f, 0.f, 0.f));
                M = glm::scale(M, glm::vec3(0.010f * k * m->scale));
                M = glm::translate(M, -m->pivot);
                mi.model = M;
                mi.color = glm::vec4(kMeatTint[c] * fade, 1.f);
                meshes_.push_back(mi);
            } else if (pack && !assets_.brChunk[c].empty()) {
                const SpriteAnim& a = (g.variant % 7 == 0 && !assets_.brChunkBig[c].empty()) ? assets_.brChunkBig[c] : assets_.brChunk[c];
                billboard(a.frames[static_cast<size_t>(g.variant) % a.frames.size()], g.pos, 0.013f * k, glm::vec4(fade, fade, fade, 1.f), true, (g.variant & 32) != 0);
            } else {
                cube(g.pos, g.size, glm::vec4(bc * 0.75f * fade, 1.f), assets_.white, glm::vec3(0.f), 0.f, 0.f, 0.f, g.spin * age);
            }
        } else {
            const SpriteAnim& a = (g.variant & 1) ? assets_.brCasingShell : assets_.brCasingBullet;
            if (pack && a.frames.size() >= 13) {
                const size_t frame = g.resting ? 8 + static_cast<size_t>(g.variant / 2) % 5 : static_cast<size_t>(age * 20.f + static_cast<float>(g.variant)) % 8;
                billboard(a.frames[frame], g.pos, 0.0038f, glm::vec4(fade, fade, fade, 1.f), true, false);
            } else {
                cube(g.pos, g.size, glm::vec4(0.85f * fade, 0.68f * fade, 0.22f * fade, 1.f), assets_.white, glm::vec3(0.f), 0.f, 0.f, 0.f, g.spin * age);
            }
        }
    }
    // Decals: oldest first so newer blood lands on top; a tiny lift per decal avoids z-fighting between them.
    size_t i = 0;
    for (const Decal& d : fps_.decals()) drawDecal(d, i++);
}

void App::addBursts() {
    if (std::getenv("REDLINE_LOG_FX") && !bursts_.empty()) std::fprintf(stderr, "[fx] frame %d: %zu bursts, first at %.1f,%.1f,%.1f t=%.2f\n", frameCount_, bursts_.size(), bursts_[0].pos.x, bursts_[0].pos.y, bursts_[0].pos.z, bursts_[0].t);
    // One-shot sprite bursts: blood clouds at hits, smoke at bullet holes and blasts.
    for (const Burst& b : bursts_) {
        const float k = b.t / b.ttl;
        const int n = static_cast<int>(b.anim->frames.size());
        const int i = std::clamp(static_cast<int>(k * static_cast<float>(n)), 0, n - 1);
        const float fade = k > 0.7f ? (1.f - k) / 0.3f : 1.f;
        // Smoke (an alpha under 1) is see-through from the start and thins as it drifts;
        // opaque bursts (sparks, chunks) keep the old flat fade.
        const float alpha = b.tint.a < 0.99f ? b.tint.a * fade * (1.f - 0.55f * k) : b.tint.a * fade;
        softBillboard(b.anim->frames[static_cast<size_t>(i)], b.pos, b.px, glm::vec4(glm::vec3(b.tint) * fade, alpha));
    }
    // Embers: tiny glowing cubes; rings: an expanding, fading shockwave in the board plane.
    for (const Ember& e : embers_) {
        const float k = std::clamp(e.ttl / e.life, 0.f, 1.f);
        cube(e.pos, e.size * (0.5f + 0.5f * k), glm::vec4(e.col * k, 1.f), assets_.white, e.col, 3.5f * k);
    }
    for (const Ring& r : rings_) {
        const float k = r.t / 0.55f;
        decal(assets_.ring, r.pos, glm::vec3(0.f, 0.f, 1.f), 1.f + 11.f * k, 0.f, glm::vec4(1.6f, 1.4f, 0.9f, 1.f - k));
    }
}

void App::drawDecal(const Decal& d, size_t i) {
    const bool pack = assets_.brutalPack;
    {
        const float alpha = d.age > 52.f ? std::max(0.f, 1.f - (d.age - 52.f) / 8.f) : 1.f;
        glm::vec3 pos = d.pos + d.normal * (0.006f + 0.0015f * static_cast<float>(i % 12));
        const int c = std::clamp(d.color, 0, 2);
        static const glm::vec4 kFallbackPool[3] = {{0.65f, 0.55f, 0.55f, 1.f}, {0.3f, 0.7f, 0.25f, 1.f}, {0.3f, 0.35f, 0.9f, 1.f}};
        static const glm::vec4 kFallbackSplat[3] = {{0.75f, 0.2f, 0.2f, 1.f}, {0.2f, 0.75f, 0.2f, 1.f}, {0.2f, 0.3f, 0.9f, 1.f}};
        if (d.kind == 0 && pack && !assets_.brPool[c].empty()) {
            // The pool spreads over half a second, then stays.
            const SpriteAnim& a = assets_.brPool[c];
            const size_t f = std::min(a.frames.size() - 1, static_cast<size_t>(d.age * a.fps));
            decal(a.frames[f], pos, d.normal, d.size * 1.3f, d.yaw, glm::vec4(0.85f, 0.85f, 0.85f, alpha));
        } else if (d.kind == 1 && pack && !assets_.brSplat[c].empty()) {
            // Hits, spreads, then dries dark over most of its life.
            const SpriteAnim& a = assets_.brSplat[c];
            const size_t n = a.frames.size();
            const size_t f = d.age < 1.5f ? std::min<size_t>(19, static_cast<size_t>(d.age * 12.f)) : std::min(n - 1, 19 + static_cast<size_t>((d.age - 1.5f) / 8.f));
            decal(a.frames[f], pos, d.normal, d.size * 1.4f, d.yaw, glm::vec4(0.9f, 0.9f, 0.9f, alpha));
        } else if (d.kind == 0 && !assets_.bloodPool.empty()) decal(assets_.bloodPool, pos, d.normal, d.size, d.yaw, glm::vec4(glm::vec3(kFallbackPool[c]), alpha));
        else if (d.kind == 1 && !assets_.blood.empty()) decal(assets_.blood.frames.back(), pos, d.normal, d.size, d.yaw, glm::vec4(glm::vec3(kFallbackSplat[c]), alpha));
        else if (d.kind == 2 && !assets_.puff.empty()) decal(assets_.puff.frames.back(), pos, d.normal, d.size, d.yaw, glm::vec4(0.25f, 0.25f, 0.25f, alpha));
    }
}

void App::announce(const std::string& text, glm::vec4 color, float scale) {
    announcements_.push_back({text, color, scale, 0.f});
    if (announcements_.size() > 4) announcements_.erase(announcements_.begin());
}

void App::panel(float x, float y, float w, float h, glm::vec4 color) {
    const render::AtlasRegion& r = assets_.region(assets_.white);
    render::QuadInstance q;
    q.pos = glm::vec4(x, y, 0.f, 0.f);
    q.size = glm::vec4(w, h, 0.f, 1.f);
    q.uvRect = glm::vec4(r.u0, r.v0, r.u1, r.v1);
    q.color = color;
    q.params = glm::vec4(1.f, 0.f, 0.f, hdrQuads_ ? 2.f : 0.f);
    screenQuads_.push_back(q);
}

void App::hotText(float x, float y, const std::string& s, float scale, glm::vec4 color, int align, int kind, int index) {
    float w = static_cast<float>(assets_.textWidth(s, scale));
    float left = align == 1 ? x - w * 0.5f : align == 2 ? x - w : x;
    float h = static_cast<float>(assets_.fontHeight) * scale;
    hotspots_.push_back({left - 6.f, y - 4.f, w + 12.f, h + 8.f, kind, index});
    text(x, y, s, scale, color, align);
}

const App::Hotspot* App::hotspotAt(float wx, float wy) const {
    int ww = 1, wh = 1;
    SDL_GetWindowSize(window_, &ww, &wh);
    VkExtent2D ext = renderer_->extent();
    float px = wx * static_cast<float>(ext.width) / static_cast<float>(std::max(1, ww));
    float py = wy * static_cast<float>(ext.height) / static_cast<float>(std::max(1, wh));
    for (const Hotspot& h : hotspots_)
        if (px >= h.x && px <= h.x + h.w && py >= h.y && py <= h.y + h.h) return &h;
    return nullptr;
}

void App::text(float x, float y, const std::string& s, float scale, glm::vec4 color, int align) {
    int w = assets_.textWidth(s, scale);
    if (align == 1) x -= w * 0.5f;
    else if (align == 2) x -= static_cast<float>(w);
    float cx = x;
    for (char ch : s) {
        char c = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        auto it = assets_.font.find(c);
        if (it == assets_.font.end()) { cx += (assets_.fontHeight / 2 + 1) * scale; continue; }
        const render::AtlasRegion& r = assets_.region(it->second);
        float gy = y + (assets_.usingWad() ? -r.offsetY * scale : 0.f);
        // Large text uses the upscaled glyphs (same metrics, smoother edges).
        auto big = scale >= 3.f ? assets_.fontBig.find(c) : assets_.fontBig.end();   // never minified below 3/4
        if (big != assets_.fontBig.end()) screenSprite(big->second, cx, gy, scale / static_cast<float>(assets_.fontBigScale), color, 0.f, 1.f);
        else screenSprite(it->second, cx, gy, scale, color, 0.f, 1.f);
        cx += (r.w + 1) * scale;
    }
}

float App::textFit(float x, float y, const std::string& str, float scale, glm::vec4 color, int align, float maxWidth) {
    const float w = static_cast<float>(assets_.textWidth(str, scale));
    if (w > maxWidth && w > 1.f && maxWidth > 0.f) scale *= maxWidth / w;
    text(x, y, str, scale, color, align);
    return scale;
}

void App::textRot(float cx, float cy, const std::string& s, float scale, glm::vec4 color, float angle) {
    // Every glyph is a screen quad the shader turns about its own anchor (pos.w), so the
    // line is laid out flat about (0, 0), each anchor is turned the same way, and the
    // glyphs end up sharing one rotation about the centre of the text.
    const float w = static_cast<float>(assets_.textWidth(s, scale));
    const float h = static_cast<float>(assets_.fontHeight) * scale;
    const float c = std::cos(angle), sn = std::sin(angle);
    float cx0 = -w * 0.5f;
    for (char ch : s) {
        char up = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        auto it = assets_.font.find(up);
        if (it == assets_.font.end()) { cx0 += (assets_.fontHeight / 2 + 1) * scale; continue; }
        const render::AtlasRegion& r = assets_.region(it->second);
        // Flat layout: the anchor is the glyph's bottom-left, y measured upwards from the text centre.
        const float ax = cx0, ay = -(h * 0.5f) + (assets_.usingWad() ? r.offsetY * scale : 0.f);
        const float rx = ax * c - ay * sn, ry = ax * sn + ay * c;
        render::QuadInstance q;
        auto big = scale >= 3.f ? assets_.fontBig.find(up) : assets_.fontBig.end();
        const render::AtlasRegion& g = big != assets_.fontBig.end() ? assets_.region(big->second) : r;
        const float gs = big != assets_.fontBig.end() ? scale / static_cast<float>(assets_.fontBigScale) : scale;
        q.pos = glm::vec4(cx + rx, cy - ry, 0.f, angle);
        q.size = glm::vec4(g.w * gs, g.h * gs, 0.f, 1.f);
        q.uvRect = glm::vec4(g.u0, g.v0, g.u1, g.v1);
        q.color = color;
        q.params = glm::vec4(1.f, 0.f, 0.f, hdrQuads_ ? 2.f : 0.f);
        screenQuads_.push_back(q);
        cx0 += (r.w + 1) * scale;
    }
}

void App::addHud() {
    hotspots_.clear();
    VkExtent2D ext = renderer_->extent();
    float W = static_cast<float>(ext.width), H = static_cast<float>(ext.height);
    float s = std::max(1.f, std::round(H / 300.f));   // font scale
    glm::vec4 white(1.f), red(1.f, 0.25f, 0.2f, 1.f), dim(0.8f, 0.8f, 0.8f, 1.f), yellow(1.f, 0.9f, 0.3f, 1.f);
    float lh = (assets_.fontHeight + 4) * s;
    bool inFps = (mode_ == Mode::Fps || mode_ == Mode::Countdown || mode_ == Mode::FlyIn || mode_ == Mode::FlyOut || (mode_ == Mode::Paused && (pausedFrom_ == Mode::Fps || pausedFrom_ == Mode::Countdown)) || (mode_ == Mode::GameOver && diedInFps_));

    // A full-screen overlay (players, options, leaderboard...) replaces the page: drawing
    // the block HUD and the key hints behind it only shows through and collides with titles.
    if (!inFps && screen_ == kScreenNone) {
        float x = 24.f, y = 24.f;
        text(x, y, "SCORE", s, dim); y += lh;
        text(x, y, std::to_string(game_->score()), s, white); y += lh * 1.4f;
        text(x, y, "LEVEL " + std::to_string(game_->level()), s, dim); y += lh;
        text(x, y, "LINES " + std::to_string(game_->lines()), s, dim); y += lh;
        text(x, y, "RED LINES " + std::to_string(redLinesSurvived_), s, red); y += lh;
        if (game_->combo() > 1) { text(x, y, "COMBO X" + std::to_string(game_->combo()), s, yellow); }
        y += lh * 1.4f;
        if (game_->prizes().any()) {
            const core::Prizes& pr = game_->prizes();
            text(x, y, "NEXT FIGHT", s * 0.7f, dim); y += lh * 0.8f;
            if (pr.bonusHealth > 0.f) { text(x, y, "+" + std::to_string(static_cast<int>(pr.bonusHealth)) + " HEALTH", s * 0.7f, glm::vec4(0.6f, 0.9f, 1.f, 1.f)); y += lh * 0.8f; }
            if (pr.shield > 0.f) { text(x, y, std::to_string(static_cast<int>(pr.shield)) + " ARMOR", s * 0.7f, glm::vec4(0.6f, 0.9f, 1.f, 1.f)); y += lh * 0.8f; }
            if (pr.invulnChance > 0.f) { text(x, y, std::to_string(static_cast<int>(pr.invulnChance * 100.f)) + "% INVULN", s * 0.7f, yellow); y += lh * 0.8f; }
            y += lh * 0.4f;
        }
        text(x, y, "NEXT", s, dim); y += lh;
        const core::Piece& n = game_->next();
        float cellPx = 12.f * s;
        for (int i = 0; i < 4; ++i) {
            auto [dx, dy] = core::shapeCells(n.shape, 0)[i];
            const std::string& tex = n.red[i] ? assets_.redBlock : assets_.block;
            glm::vec3 col = n.red[i] ? glm::vec3(1.f, 0.6f, 0.6f) : kPieceColors[static_cast<int>(n.shape)];
            const render::AtlasRegion& r = assets_.region(tex);
            render::QuadInstance q;
            q.pos = glm::vec4(x + dx * cellPx, y + dy * cellPx, 0.f, 0.f);
            q.size = glm::vec4(cellPx - 2.f, cellPx - 2.f, 0.f, 1.f);
            q.uvRect = glm::vec4(r.u0, r.v0, r.u1, r.v1);
            q.color = glm::vec4(col, 1.f);
            q.params = glm::vec4(1.f, 0.f, 0.f, 0.f);
            screenQuads_.push_back(q);
        }
        text(W - 24.f, 24.f, "ARROWS/WASD MOVE  UP ROTATE  SPACE DROP", s * 0.6f, dim, 2);
        text(W - 24.f, 24.f + lh, "RED BLOCKS REFUSE TO CLEAR.", s * 0.6f, dim, 2);
        text(W - 24.f, 24.f + lh * 2.f, "A FULL RED ROW TIPS THE BOARD OVER.", s * 0.6f, dim, 2);
        text(W - 24.f, 24.f + lh * 3.f, pad_ ? "PAD: STICK/DPAD MOVE  A/B ROTATE  X DROP  START PAUSE" : "ESC MENU  F12 SCREENSHOT", s * 0.6f, dim, 2);
    }
    if (mode_ == Mode::Blocks && game_->danger() > 0.f) {
        // The stack is high enough for blocks to turn evil: a creeping red edge and a warning.
        float d = game_->danger();
        float f = 0.5f + 0.5f * std::sin(time_ * (3.f + 6.f * d));
        panel(0.f, 0.f, W, lh * 0.5f, glm::vec4(1.f, 0.1f, 0.05f, 0.35f * d * f));
        panel(0.f, H - lh * 0.5f, W, lh * 0.5f, glm::vec4(1.f, 0.1f, 0.05f, 0.35f * d * f));
        // The banner sits in the empty column right of the board, two lines, and
        // behaves like a failing sign: it flickers, jolts when a block turns, and
        // bleeds. In panic it shakes.
        const bool panic = game_->panic();
        const float colL = W * 0.5f + H * 0.3f, cx = (colL + W - 24.f) * 0.5f;
        const char* l1 = panic ? "OVERRUN" : d < 0.5f ? "THE STACK IS" : "EVIL";
        const char* l2 = panic ? "EVIL SURGES" : d < 0.5f ? "TURNING EVIL" : "RISING";
        const float fs = s * (panic ? 1.6f : 1.1f + 0.3f * d) * (1.f + 0.2f * evilJolt_);
        uint32_t hsh = static_cast<uint32_t>(time_ * 19.f) * 2654435761u; hsh ^= hsh >> 13; hsh *= 0x27d4eb2du; hsh ^= hsh >> 15;
        const float flick = (hsh % 100u) < static_cast<uint32_t>(4 + 18 * d) ? 0.3f : 1.f;   // brief dropouts, more of them as it gets worse
        const float shake = panic ? 1.f : evilJolt_;
        const float jx = ((static_cast<float>((hsh >> 8) % 7u) - 3.f) * 0.6f * s) * shake, jy = ((static_cast<float>((hsh >> 16) % 5u) - 2.f) * 0.6f * s) * shake;
        const float y1 = H * 0.36f, y2 = y1 + fs * (assets_.fontHeight + 3);
        for (const Drip& dr : drips_) {   // blood first, so the letters sit on top of where it starts
            const float a = std::min(1.f, dr.ttl / 1.f);
            const float w = dr.w * s, run = std::max(0.f, dr.y - dr.y0);
            // The run thins and dries towards the letter it came from; the head stays a fat bead.
            panel(dr.x - w * 0.2f, dr.y0, w * 0.4f, run, glm::vec4(0.45f, 0.02f, 0.02f, 0.45f * a));
            panel(dr.x - w * 0.35f, dr.y0 + run * 0.5f, w * 0.7f, run * 0.5f, glm::vec4(0.55f, 0.03f, 0.02f, 0.6f * a));
            panel(dr.x - w * 0.5f, dr.y - w * 0.7f, w, w * 1.4f, glm::vec4(0.65f, 0.04f, 0.02f, 0.95f * a));
        }
        const glm::vec4 halo(0.9f, 0.05f, 0.f, (0.08f + 0.1f * f) * (0.5f + 0.5f * d) * flick);
        for (int k = 0; k < 8; ++k) {
            const float ang = static_cast<float>(k) * 0.7854f, ox = std::cos(ang) * s * 1.6f, oy = std::sin(ang) * s * 1.6f;
            text(cx + jx + ox, y1 + jy + oy, l1, fs, halo, 1);
            text(cx + jx + ox, y2 + jy + oy, l2, fs, halo, 1);
        }
        const glm::vec4 col(1.f, 0.3f + 0.4f * f, 0.2f, (0.55f + 0.45f * d) * flick);
        text(cx + jx, y1 + jy, l1, fs, col, 1);
        text(cx + jx, y2 + jy, l2, fs, col, 1);
        const float bw = static_cast<float>(std::max(assets_.textWidth(l1, fs), assets_.textWidth(l2, fs)));
        bannerX0_ = cx - bw * 0.45f; bannerX1_ = cx + bw * 0.45f; bannerY_ = y2 + fs * assets_.fontHeight;
    } else bannerX1_ = bannerX0_ = 0.f;
    if (clearFlash_ > 0.f) panel(0.f, 0.f, W, H, glm::vec4(1.f, 0.95f, 0.8f, 0.22f * clearFlash_));
    if (mode_ == Mode::Alert) {
        float f = 0.5f + 0.5f * std::sin(modeT_ * 16.f);
        text(W * 0.5f, H * 0.42f, "RED LINE", s * 2.2f, glm::vec4(1.f, 0.2f * f, 0.1f * f, 1.f), 1);
        text(W * 0.5f, H * 0.42f + lh * 2.4f, "THE BOARD IS FALLING", s, white, 1);
    }
    if (inFps) {
        if (mode_ == Mode::Fps || mode_ == Mode::Countdown) {
            const WeaponArt& wa = assets_.weapons[std::clamp(fps_.currentWeapon(), 0, kWeaponArt - 1)];
            bool flip = false;
            // Brutal mode with the pack: Brutal Doom's weapon art, flash baked into the fire frames.
            const bool brutalArt = brutalActive() && !wa.brIdle.empty() && !wa.brFire.empty();
            // Firing: the fire frames with the flash on top. Just released (the plasma rifle's
            // vents): the cool-down frame for a moment. Otherwise idle.
            const float coolFor = std::max(0.25f, weaponDef(fps_.currentWeapon()).cycle) + 0.55f;
            const std::string& gun = brutalArt ? (fps_.gunFiring() ? animFrame(wa.brFire, fps_.gunAnimT(), false, &flip) : animFrame(wa.brIdle, 0.f, true, &flip))
                                   : fps_.gunFiring() ? animFrame(wa.fire, fps_.gunAnimT(), false, &flip)
                                   : (!wa.cooldown.empty() && fps_.gunAnimT() < coolFor) ? animFrame(wa.cooldown, 0.f, true, &flip)
                                   : animFrame(wa.idle, 0.f, true, &flip);
            float gs = H / 200.f;
            bool moving = fpsIn_.fwd || fpsIn_.back || fpsIn_.left || fpsIn_.right;
            float bob = std::sin(time_ * 6.f) * 3.f * gs * (moving ? 1.f : 0.15f);
            const float swayX = (std::sin(time_ * 3.f) * 2.5f * (moving ? 1.f : 0.25f) + swayX_) * gs;   // side-to-side while walking, lag when turning
            float recoil = fps_.recoil() * 18.f * gs + swayY_ * gs;
            hdrQuads_ = true;   // the weapon and its flash bloom with the scene; the HUD after it does not
            if (!gun.empty()) {
                // Doom draws weapon sprites with their own patch origin at (161, 32) of a 320x200 screen,
                // and the muzzle flash with the same origin, so the flash lands on the barrel by itself.
                auto originSprite = [&](const std::string& key, float dy, glm::vec4 tint, bool fl) {
                    const render::AtlasRegion& r = assets_.region(key);
                    float ax = r.w > 0 ? static_cast<float>(r.offsetX) / r.w : 0.5f;
                    float ay = r.h > 0 ? 1.f - static_cast<float>(r.offsetY) / r.h : 1.f;
                    // Doom: left edge = 1 - leftoffset in 320-wide space, so the origin sits 1px right of the frame's left edge.
                    screenSprite(key, (W - 320.f * gs) * 0.5f + 1.f * gs + swayX, 32.f * gs + dy, gs, tint, ax, ay, fl);
                };
                float dy = recoil + std::fabs(bob) + (H - 200.f * gs) * 0.5f;   // centre the 320x200 frame vertically... anchored to the bottom
                dy = recoil + std::fabs(bob) + (H - 200.f * gs);                 // keep the weapon at the bottom edge
                if (assets_.usingWad()) {
                    originSprite(gun, dy, brutalArt ? glm::vec4(1.f) : wa.tint, flip);
                    if (!brutalArt && fps_.gunFiring() && fps_.gunAnimT() < wa.flashFor && !wa.flash.empty())
                        originSprite(animFrame(wa.flash, fps_.gunAnimT(), false), dy, glm::vec4(1.8f, 1.8f, 1.8f, 1.f), false);
                } else {
                    const render::AtlasRegion& r = assets_.region(gun);
                    screenSprite(gun, W * 0.5f, H + recoil + std::fabs(bob) - 2.f * gs, gs, wa.tint, 0.5f, 0.f, flip);
                    if (fps_.gunFiring() && fps_.gunAnimT() < wa.flashFor && !wa.flash.empty()) {
                        const std::string& fl = animFrame(wa.flash, fps_.gunAnimT(), false);
                        const render::AtlasRegion& fr = assets_.region(fl);
                        screenSprite(fl, W * 0.5f, H + recoil - (r.h - fr.h) * gs - 6.f * gs, gs, glm::vec4(1.8f, 1.8f, 1.8f, 1.f), 0.5f, 0.f);
                    }
                }
            }
            hdrQuads_ = false;
            // Brutal: blood splashed on the screen slides down and fades.
            for (const ScreenBlood& b : screenBlood_) {
                const float k = b.t / b.ttl;
                const std::string& key = assets_.blood.frames[static_cast<size_t>(b.frame) % assets_.blood.frames.size()];
                screenSprite(key, b.x, b.y + k * k * 60.f * gs, b.scale * gs, glm::vec4(0.7f, 0.05f, 0.05f, 0.85f * (1.f - k)), 0.5f, 0.5f);
            }
            screenSprite(assets_.crosshair, W * 0.5f, H * 0.5f, std::max(1.f, s * 0.7f), glm::vec4(1.f, 1.f, 1.f, 0.85f), 0.5f, 0.5f);
        }
        if (fps_.pickupFlash() > 0.f) panel(0.f, 0.f, W, H, glm::vec4(1.f, 1.f, 0.6f, 0.18f * fps_.pickupFlash()));
        if (fps_.damageFlash() > 0.f) panel(0.f, 0.f, W, H, glm::vec4(1.f, 0.f, 0.f, 0.45f * fps_.damageFlash()));
        if (mode_ == Mode::Fps) addHealthBars(W, H, s, lh);
        panel(0.f, H - lh * 1.6f, W, lh * 1.6f, glm::vec4(0.f, 0.f, 0.f, 0.55f));
        float hy = H - lh * 1.3f;
        std::string hp = "HEALTH " + std::to_string(static_cast<int>(std::ceil(fps_.health()))) + "%";
        if (fps_.shield() > 0.f) hp += "  ARMOR " + std::to_string(static_cast<int>(std::ceil(fps_.shield())));
        text(24.f, hy, hp, s, fps_.health() < 30.f ? red : (fps_.health() > 100.f ? glm::vec4(0.6f, 0.9f, 1.f, 1.f) : white));
        if (fps_.invulnerable() && mode_ == Mode::Fps) {
            float left = fps_.invulnLeft();
            float total = std::max(1.f, game_->rules().invulnSeconds);
            float f = 0.5f + 0.5f * std::sin(time_ * (left < 3.f ? 14.f : 8.f));   // flashes faster as it runs out
            panel(0.f, 0.f, W, H, glm::vec4(1.f, 0.95f, 0.6f, 0.10f + 0.06f * f));
            char buf[48];
            std::snprintf(buf, sizeof buf, "INVULNERABLE  %.1f", left);
            text(W * 0.5f, H * 0.12f, buf, s * 1.3f, glm::vec4(1.f, 0.95f, 0.5f, 0.8f + 0.2f * f), 1);
            float bw = W * 0.3f, bh = lh * 0.45f, bx = W * 0.5f - bw * 0.5f, by = H * 0.12f + lh * 1.5f;
            panel(bx - 2.f, by - 2.f, bw + 4.f, bh + 4.f, glm::vec4(0.f, 0.f, 0.f, 0.6f));
            panel(bx, by, bw * std::clamp(left / total, 0.f, 1.f), bh, left < 3.f ? glm::vec4(1.f, 0.55f, 0.2f, 0.95f) : glm::vec4(1.f, 0.9f, 0.4f, 0.95f));
        }
        text(W * 0.5f, hy, "DEMONS " + std::to_string(fps_.enemiesLeft()) + "/" + std::to_string(fps_.totalEnemies()), s, yellow, 1);
        if (const Enemy* boss = fps_.boss(); boss && fps_.stage() == FpsMode::Stage::Dungeon) {
            // The boss's health, once it has been seen; before that, a hint that it is down
            // there. Both sit clear above the weapon roster, which owns the row at hy - 1.3.
            const float by = hy - lh * 3.6f;
            if (!fps_.bossSeen()) text(W * 0.5f, hy - lh * 2.7f, fps_.doorOpen() ? "ENTER THE DUNGEON AND KILL THE BOSS" : fps_.hasKey() ? "YOU HAVE THE SKULL KEY: OPEN THE HALL" : "THE HALL IS SEALED: FIND THE SKULL KEY", s * 0.7f, glm::vec4(1.f, 0.5f, 0.4f, 0.8f + 0.2f * std::sin(time_ * 3.f)), 1);
            else if (boss->alive()) {
                const std::string& name = assets_.enemies[std::clamp(boss->kind, 0, kEnemyKinds - 1)].name;
                text(W * 0.5f, by, name, s * 0.8f, glm::vec4(1.f, 0.35f, 0.3f, 1.f), 1);
                float bw = W * 0.34f, bh = lh * 0.4f, bx = W * 0.5f - bw * 0.5f, yy = by + lh * 0.95f;
                panel(bx - 2.f, yy - 2.f, bw + 4.f, bh + 4.f, glm::vec4(0.f, 0.f, 0.f, 0.65f));
                panel(bx, yy, bw * std::clamp(boss->hp / boss->maxHp, 0.f, 1.f), bh, glm::vec4(0.9f, 0.12f, 0.1f, 0.95f));
            }
        }
        text(W - 24.f, hy, "LEVEL " + std::to_string(game_->level()) + "   SCORE " + std::to_string(game_->score()), s, white, 2);
        // Weapon roster: owned weapons with ammo, current one highlighted.
        {
            float wy = hy - lh * 1.3f;
            float wx = W - 24.f;
            for (int i = kWeaponCount - 1; i >= 0; --i) {
                const WeaponSlot& slot = fps_.weapon(i);
                if (!slot.owned) continue;
                std::string label = std::to_string(i + 1) + " " + assets_.weapons[i].name + (weaponDef(i).ammoPerPickup ? " " + std::to_string(slot.ammo) : "");
                bool cur = (i == fps_.currentWeapon());
                text(wx, wy, label, s * 0.7f, cur ? yellow : dim, 2);
                wx -= static_cast<float>(assets_.textWidth(label, s * 0.7f)) + 18.f * s;
            }
        }
        if (mode_ == Mode::Countdown && pad_) text(W * 0.5f, H * 0.7f, "LEFT STICK MOVE  RIGHT STICK LOOK  RT FIRE  LT RUN  BUMPERS WEAPONS", s * 0.7f, dim, 1);
        if (mode_ == Mode::Countdown) {
            float remaining = kCountdownTime - modeT_;
            int n = static_cast<int>(std::ceil(remaining));
            float frac = remaining - std::floor(remaining);          // 1 -> 0 within the second
            float pop = 1.f + 0.6f * frac;                            // number shrinks as its second runs out
            text(W * 0.5f, H * 0.28f, "GET READY", s * 1.2f, white, 1);
            text(W * 0.5f, H * 0.38f, std::to_string(std::max(1, n)), s * 4.f * pop, glm::vec4(1.f, 0.25f, 0.2f, 1.f), 1);
            text(W * 0.5f, H * 0.62f, "MOUSE LOOK  WASD MOVE  SHIFT RUN  CLICK FIRE  1-4/WHEEL WEAPONS", s * 0.8f, dim, 1);
            text(W * 0.5f, H * 0.62f + lh * 1.2f, "DEMONS EAT YOUR COVER. LEAVE ONE ALIVE TOO LONG AND IT GROWS.", s * 0.7f, dim, 1);
            if (fps_.invulnerable()) {
                float f = 0.5f + 0.5f * std::sin(time_ * 6.f);
                text(W * 0.5f, H * 0.50f, "YOU ARE INVULNERABLE FOR THE FIRST " + std::to_string(static_cast<int>(std::ceil(fps_.invulnLeft()))) + " SECONDS", s * 1.1f, glm::vec4(1.f, 0.95f, 0.5f, 0.8f + 0.2f * f), 1);
                text(W * 0.5f, H * 0.50f + lh * 1.2f, "THE TIMER STARTS WHEN THE FIGHT DOES - GO ON THE ATTACK", s * 0.75f, glm::vec4(1.f, 0.95f, 0.7f, 0.9f), 1);
            } else if (lastInvulnChance_ > 0.f) {
                text(W * 0.5f, H * 0.50f, "INVULNERABILITY ROLL FAILED (" + std::to_string(static_cast<int>(lastInvulnChance_ * 100.f)) + "%) - NO SHIELD THIS TIME", s * 0.85f, glm::vec4(1.f, 0.6f, 0.4f, 0.9f), 1);
            }
        }
        if (mode_ == Mode::Fps && fps_.elapsed() < 0.7f) {
            float f = 1.f - fps_.elapsed() / 0.7f;
            text(W * 0.5f, H * 0.38f, "FIGHT", s * 3.5f * (1.f + 0.5f * (1.f - f)), glm::vec4(1.f, 0.9f, 0.3f, f), 1);
        }
        if (mode_ == Mode::Fps) {
            bool warning = false;
            for (const Enemy& e : fps_.enemies()) if (e.alive() && e.absorbTimer < 4.f && e.tier < kMaxTier) warning = true;
            if (warning) {
                float f = 0.5f + 0.5f * std::sin(time_ * 12.f);
                text(W * 0.5f, H * 0.2f, "A DEMON IS ABOUT TO GROW", s * 0.9f, glm::vec4(1.f, 0.3f + 0.4f * f, 0.2f, 1.f), 1);
            }
        }
    }

    if (bfgHoldT_ > 0.f) {   // charging: a green glow that builds over the two seconds
        float c = std::min(1.f, bfgHoldT_ / 2.f);
        panel(0.f, 0.f, W, H, glm::vec4(0.4f, 1.f, 0.4f, 0.25f * c * c));
    }
    if (bfgFlash_ > 0.f) panel(0.f, 0.f, W, H, glm::vec4(0.6f, 1.f, 0.6f, 0.5f * bfgFlash_));
    if (bfgUsed_ && !inFps && mode_ != Mode::Title) text(24.f, H - lh * 1.2f, "BFG SPENT", s * 0.7f, glm::vec4(0.5f, 0.8f, 0.5f, 0.8f));

    // Announcements: rise and fade. While stacking they sit in the column right
    // of the board (under the evil banner) so they never cover the play field.
    {
        const bool side = mode_ == Mode::Blocks || mode_ == Mode::Alert;
        const float colL = W * 0.5f + H * 0.3f;
        const float ax = side ? (colL + W - 24.f) * 0.5f : W * 0.5f;
        const float avail = side ? W - 24.f - colL : W - 48.f;
        float y = side ? H * 0.58f : H * 0.24f;
        for (Announcement& a : announcements_) {
            float alpha = a.t < 1.2f ? 1.f : std::max(0.f, 1.f - (a.t - 1.2f) / 0.8f);
            float rise = 30.f * s * std::min(1.f, a.t / 2.f);
            glm::vec4 col = a.color;
            col.a *= alpha;
            float sc = s * a.scale;
            const int tw = assets_.textWidth(a.text, sc);
            if (tw > avail) sc *= avail / static_cast<float>(tw);   // shrink long lines to the column
            text(ax, y - rise, a.text, sc, col, 1);
            y += lh * a.scale;
        }
    }

    // Level-up card ---------------------------------------------------------
    if (levelCardT_ < kLevelCardTime && (mode_ == Mode::FlyOut || mode_ == Mode::Blocks)) {
        float t = levelCardT_;
        float in = std::min(1.f, t / 0.35f), out = std::min(1.f, (kLevelCardTime - t) / 0.5f);
        float a = in * out;
        panel(0.f, H * 0.18f, W, H * 0.5f, glm::vec4(0.f, 0.f, 0.f, 0.6f * a));
        panel(0.f, H * 0.18f, W, lh * 0.25f, glm::vec4(1.f, 0.85f, 0.2f, 0.9f * a));
        panel(0.f, H * 0.68f - lh * 0.25f, W, lh * 0.25f, glm::vec4(1.f, 0.85f, 0.2f, 0.9f * a));
        float pop = 1.f + 0.35f * std::max(0.f, 1.f - t / 0.6f) + 0.05f * std::sin(time_ * 6.f);
        text(W * 0.5f, H * 0.22f, "LEVEL UP", s * 1.4f, glm::vec4(1.f, 0.85f, 0.2f, a), 1);
        text(W * 0.5f, H * 0.30f, "LEVEL " + std::to_string(levelCard_.level), s * 3.6f * pop, glm::vec4(1.f, 0.95f, 0.6f, a), 1);
        float y = H * 0.30f + lh * 4.4f;
        text(W * 0.5f, y, "DEMONS SLAIN " + std::to_string(levelCard_.kills) + "     BLOCKS DESTROYED " + std::to_string(levelCard_.blocks), s * 0.9f, glm::vec4(1.f, 1.f, 1.f, a), 1); y += lh * 1.1f;
        char buf[64];
        std::snprintf(buf, sizeof buf, "FIGHT TIME %.0f S     DAMAGE TAKEN %.0f", levelCard_.seconds, levelCard_.damage);
        text(W * 0.5f, y, buf, s * 0.9f, glm::vec4(1.f, 1.f, 1.f, a), 1); y += lh * 1.1f;
        text(W * 0.5f, y, "FASTER GRAVITY   MORE RED   TOUGHER DEMONS", s * 0.75f, glm::vec4(1.f, 0.5f, 0.4f, a), 1);
    }

    // Menus ----------------------------------------------------------------
    auto drawMenu = [&](float y0) {
        for (size_t i = 0; i < menu_.items.size(); ++i) {
            bool sel = static_cast<int>(i) == menu_.index;
            float f = sel ? (0.7f + 0.3f * std::sin(time_ * 6.f)) : 1.f;
            std::string label = sel ? ("> " + menu_.items[i] + " <") : menu_.items[i];
            hotText(W * 0.5f, y0 + static_cast<float>(i) * lh * 1.5f, label, s * 1.2f, sel ? glm::vec4(1.f, 0.9f * f, 0.3f * f, 1.f) : dim, 1, kHotMenu, static_cast<int>(i));
        }
    };
    if (mode_ == Mode::Title && screen_ == kScreenNone) {   // overlay screens replace the title page entirely
        panel(0.f, 0.f, W, H, glm::vec4(0.f, 0.f, 0.f, 0.55f));
        if (!assets_.title.empty()) screenSprite(assets_.title, W * 0.5f, H * 0.17f, s * 1.1f, glm::vec4(1.f, 0.6f, 0.6f, 1.f), 0.5f, 0.5f);
        text(W * 0.5f, H * 0.28f, "REDLINE", s * 2.8f, glm::vec4(1.f, 0.15f, 0.1f, 1.f), 1);
        float ty = H * 0.28f + lh * 3.1f;
        text(W * 0.5f, ty, "STACK THE BLOCKS. SOME OF THEM ARE RED.", s * 0.85f, white, 1); ty += lh * 0.95f;
        text(W * 0.5f, ty, "WHEN A ROW IS ALL RED THE BOARD TIPS OVER", s * 0.85f, white, 1); ty += lh * 0.95f;
        text(W * 0.5f, ty, "AND EVERY RED CLUSTER BECOMES A DEMON.", s * 0.85f, white, 1); ty += lh * 1.4f;
        {
            // The menu closes up rather than running into the status strip at the bottom
            // (eight items on a short window would otherwise overlap the player line).
            const float room = (H - lh * 3.3f - lh * 0.6f) - ty;
            const float step = std::min(lh * 1.25f, room / static_cast<float>(std::max<size_t>(menu_.items.size(), 1)));
            for (size_t i = 0; i < menu_.items.size(); ++i) {
                bool sel = static_cast<int>(i) == menu_.index;
                float f = sel ? (0.7f + 0.3f * std::sin(time_ * 6.f)) : 1.f;
                std::string label = sel ? ("> " + menu_.items[i] + " <") : menu_.items[i];
                hotText(W * 0.5f, ty + static_cast<float>(i) * step, label, s * 1.1f, sel ? glm::vec4(1.f, 0.9f * f, 0.3f * f, 1.f) : dim, 1, kHotMenu, static_cast<int>(i));
            }
        }
        if (updateAvailable_) {
            // A sticker to the right of the menu, tilted, that zooms in and out three times
            // when the title comes up (and again every so often) so it gets noticed, then
            // breathes gently. Clicking it opens WHAT'S NEW.
            const float burst = std::fmod(updateNoticeT_, 15.f);
            const float zoom = burst < 1.8f ? 1.f + 0.5f * std::fabs(std::sin(burst * kPi / 0.6f)) : 1.f + 0.05f * std::sin(time_ * 3.f);
            const float ang = 0.22f;   // about 12 degrees, rising to the right
            const float cx = W * 0.81f, cy = H * 0.68f;
            const float f = 0.8f + 0.2f * std::sin(time_ * 5.f);
            auto line = [&](float dy, const std::string& t, float sc, glm::vec4 col) {
                // Lines stack along the sticker's own "down" direction.
                textRot(cx + dy * std::sin(ang), cy + dy * std::cos(ang), t, sc, col, ang);
            };
            line(-lh * 1.5f * zoom, "NEW VERSION", s * 1.1f * zoom, glm::vec4(1.f, 0.95f, 0.45f, 1.f));
            line(0.f, latestVersion_, s * 2.6f * zoom, glm::vec4(0.7f, 1.f, 0.7f, f));
            line(lh * 1.5f * zoom, "IS OUT! SEE WHAT'S NEW", s * 0.8f * zoom, dim);
            const float bw = static_cast<float>(assets_.textWidth("IS OUT! SEE WHAT'S NEW", s * 0.8f)) + lh, bh = lh * 4.5f;
            hotRect(cx - bw * 0.5f, cy - bh * 0.5f, bw, bh, kHotMenu, 5);   // the WHAT'S NEW item
        }
        if (!allProfileScores_.empty()) {
            // Right-hand column, top right under the key hints: the best runs of every player on this machine.
            float hy = H * 0.21f;
            text(W - 24.f, hy, "HIGH SCORES", s * 0.8f, yellow, 2);
            hy += lh * 0.9f;
            int shown = 0;
            for (const NamedScore& n : allProfileScores_) {
                if (shown++ >= 8) break;
                std::string line = std::to_string(shown) + ". " + std::to_string(n.score.score) + "  " + n.player + "  LVL " + std::to_string(n.score.level) + "  RED " + std::to_string(n.score.redLines);
                text(W - 24.f, hy, line, s * 0.65f, n.player == profileName_ ? glm::vec4(0.85f, 0.9f, 1.f, 1.f) : dim, 2);
                hy += lh * 0.75f;
            }
        }
        // Status strip, three rows: player / gamepad + music / assets + key hints.
        float rowA = H - lh * 3.3f, rowB = H - lh * 2.4f, rowC = H - lh * 1.5f;
        if (!profileName_.empty()) text(24.f, rowA, "PLAYER " + profileName_ + "   TROPHIES " + std::to_string(trophies_.unlockedCount()) + "/" + std::to_string(trophies_.total()) + (stats_.loaded() ? "   PLAYED " + PlayerStats::formatDuration(stats_.lifetime().total()) + (stats_.machineCount() > 1 ? " (" + std::to_string(stats_.machineCount()) + " MACHINES)" : "") : ""), s * 0.6f, dim);
        text(24.f, rowB, pad_ ? "GAMEPAD: " + padName_ : std::string("NO GAMEPAD"), s * 0.6f, dim);
        text(W - 24.f, rowB, std::string("MUSIC: ") + (music_.trackNames().empty() ? "NONE" : (std::string(musicSetName()) + (musicSet_ == MusicSet::Classic ? std::string(" / ") + music_.backendName() : ""))), s * 0.6f, dim, 2);
        text(24.f, rowC, (assets_.usingWad() ? ("ASSETS: " + assets_.wadName()) : std::string("ASSETS: PROCEDURAL (NO WAD FOUND)")) + (useVoxels_ && voxels_.available() ? "  +  VOXEL DOOM" : ""), s * 0.6f, dim);
        text(W - 24.f, rowC, "M MUTE   N MUSIC SET   T TROPHIES", s * 0.6f, dim, 2);
    }
    // Overlay screens ---------------------------------------------------------
    if (screen_ != kScreenNone) {
        panel(0.f, 0.f, W, H, glm::vec4(0.f, 0.f, 0.f, 0.88f));
        int rowIndex = 0;
        // enabled = false greys the row out (it stays selectable but does nothing); sub-options are drawn smaller with a dash.
        auto row = [&](float y, const std::string& label, const std::string& value, bool sel, bool enabled = true, bool sub = false) {
            const glm::vec4 grey(0.45f, 0.45f, 0.5f, 1.f);
            const float fs = s * (sub ? 0.85f : 1.f);
            glm::vec4 c = !enabled ? grey : sel ? yellow : dim;
            text(W * 0.5f - 20.f, y, std::string(sel ? "> " : "") + (sub ? "- " : "") + label, fs, c, 2);
            text(W * 0.5f + 20.f, y, value, fs, !enabled ? grey : sel ? white : dim, 0);
            float lw = static_cast<float>(assets_.textWidth(label, s)), vw = static_cast<float>(assets_.textWidth(value, s));
            hotRect(W * 0.5f - 20.f - lw - 30.f, y - 4.f, lw + 40.f + vw + 30.f, static_cast<float>(assets_.fontHeight) * s + 8.f, kHotOptionRow, rowIndex++);
        };
        if (screen_ == kScreenOptions) {
            text(W * 0.5f, H * 0.05f, "OPTIONS", s * 1.5f, white, 1);
            char buf[80];
            // The rows first, then the slice of them that fits between the title and
            // the pinned BACK; the list scrolls to the selection (keyboard, pad) or
            // with the wheel.
            struct OptRow { std::string label, value; bool enabled = true, sub = false; };
            std::vector<OptRow> rows;
            rows.push_back({"MUSIC", musicSetName()});
            std::snprintf(buf, sizeof buf, "%d%%", static_cast<int>(std::lround(music_.volume() * 100.f)));
            rows.push_back({"MUSIC VOLUME", buf});
            std::snprintf(buf, sizeof buf, "%d%%", static_cast<int>(std::lround(sfxVolume_ * 100.f)));
            rows.push_back({"SFX VOLUME", buf});
            rows.push_back({"MUSIC ENABLED", music_.enabled() ? "ON" : "OFF"});
            std::snprintf(buf, sizeof buf, "%.1f", padSens_);
            rows.push_back({"STICK SENSITIVITY", buf});
            rows.push_back({"INVERT LOOK", padInvertY_ ? "ON" : "OFF"});
            rows.push_back({"RUMBLE", padRumble_ ? "ON" : "OFF"});
            rows.push_back({"DISPLAY", displayModeName()});
            std::snprintf(buf, sizeof buf, "%d X %d%s", resW_, resH_, displayMode_ == 1 ? "  (DESKTOP SIZE IN BORDERLESS)" : "");
            rows.push_back({"RESOLUTION", buf});
            rows.push_back({"MODELS", voxels_.available() ? (useVoxels_ ? "VOXELS (VOXEL DOOM)" : "SPRITES") : "SPRITES (NO VOXEL PACK FOUND)"});
            rows.push_back({"DOOM WAD", assets_.usingWad() ? assets_.wadName() + "  (ENTER TO CHANGE)" : "NONE - PLACEHOLDER ART  (ENTER)"});
            rows.push_back({"SOUNDTRACK WAD", !assets_.oggMusic.empty() ? std::filesystem::path(assets_.extrasPath).filename().string() + "  (" + std::to_string(assets_.oggMusic.size()) + " TRACKS)" : "NONE - CLASSIC ONLY  (ENTER)"});
            rows.push_back({"PLAYER EMAIL", stats_.email().empty() ? "NOT SET  (OPTIONAL, ENTER)" : stats_.email() + (stats_.emailVerified() ? "  (REGISTERED)" : onlineOn_ ? "  (NOT REGISTERED, ENTER)" : "  (NOT REGISTERED)")});
            rows.push_back({"ONLINE", !OnlineClient::available() ? "NOT IN THIS BUILD" : !onlineOn_ ? "OFF  (SCORES STAY ON THIS MACHINE)" : !stats_.emailVerified() ? "ON  (REGISTER YOUR EMAIL TO POST SCORES)" : "ON  (POSTING SCORES)", OnlineClient::available()});
            rows.push_back({"DOOM ART", doomArtOff_ ? "OFF  (PLACEHOLDER LOOK)" : "ON"});
            rows.push_back({"BRUTAL", !assets_.usingWad() ? "NEEDS DOOM ART" : brutal_ ? (assets_.brutalPack ? "ON  (COMMUNITY GORE PACK)" : "ON  (BLOOD, GIBS, CASINGS)") : "OFF", assets_.usingWad(), true});
            rows.push_back({"DUNGEON", dungeon_ ? "ON  (A CRYPT BEHIND THE WALL AFTER EVERY ARENA)" : "OFF  (ARENA FIGHTS ONLY)"});
            rows.push_back({"RAY TRACING", !renderer_->rayTracingAvailable() ? "NONE  (NO RAY TRACING ON THIS GPU)" : rtShadows_ == 0 ? "OFF  (SHADOW MAP)" : rtShadows_ == 1 ? "SUN SHADOWS" : rtShadows_ == 2 ? "SUN + ALL LIGHTS" : "SUN + ALL LIGHTS + REFLECTIONS"});
            rows.push_back({"ANTI-ALIASING", !renderer_->msaaAvailable() ? "NONE  (NOT SUPPORTED)" : msaa_ ? "4X MSAA" : "OFF"});
            rows.push_back({"BLOOM & HAZE", bloom_ ? "ON" : "OFF"});
            const int nRows = static_cast<int>(rows.size());   // BACK is index nRows (see screenKey)
            const float rowH = lh * 1.08f, top = H * 0.125f;
            const float bottom = H - lh * 1.1f - lh * 1.2f - lh * 1.3f;   // above the hint line, the status line and BACK
            int visible = std::max(3, static_cast<int>((bottom - top) / rowH));
            if (visible >= nRows) { visible = nRows; optionsScroll_ = 0; }
            optionsScroll_ = std::clamp(optionsScroll_, 0, nRows - visible);
            if (optionsFollow_) {
                if (screenIndex_ >= nRows) optionsScroll_ = nRows - visible;   // BACK: show the end of the list above it
                else if (screenIndex_ < optionsScroll_) optionsScroll_ = screenIndex_;
                else if (screenIndex_ >= optionsScroll_ + visible) optionsScroll_ = screenIndex_ - visible + 1;
            }
            const glm::vec4 marker(0.7f, 0.65f, 0.5f, 1.f);
            if (optionsScroll_ > 0) text(W * 0.5f, top - lh * 0.75f, "^  " + std::to_string(optionsScroll_) + " MORE ABOVE  ^", s * 0.6f, marker, 1);
            float y = top;
            for (int i = optionsScroll_; i < optionsScroll_ + visible; ++i) {
                rowIndex = i;
                row(y, rows[static_cast<size_t>(i)].label, rows[static_cast<size_t>(i)].value, screenIndex_ == i, rows[static_cast<size_t>(i)].enabled, rows[static_cast<size_t>(i)].sub);
                y += rowH;
            }
            const int below = nRows - optionsScroll_ - visible;
            if (below > 0) text(W * 0.5f, y - lh * 0.3f, "v  " + std::to_string(below) + " MORE BELOW  v", s * 0.6f, marker, 1);
            y += lh * 0.35f;
            hotText(W * 0.5f, y, screenIndex_ == nRows ? "> BACK <" : "BACK", s, screenIndex_ == nRows ? yellow : dim, 1, kHotBack, 0);
            if (!wadStatus_.empty()) text(W * 0.5f, y + lh * 1.2f, wadStatus_, s * 0.75f, glm::vec4(1.f, 0.8f, 0.4f, 1.f), 1);
            text(W * 0.5f, H - lh * 1.1f, "LEFT/RIGHT CHANGE   ESC OR B BACK   WHEEL OR UP/DOWN SCROLL   ALT+ENTER FULLSCREEN", s * 0.7f, dim, 1);
        } else if (screen_ == kScreenTrophies) {
            text(W * 0.5f, H * 0.12f, "TROPHIES  " + std::to_string(trophies_.unlockedCount()) + "/" + std::to_string(trophies_.total()), s * 1.6f, yellow, 1);
            const auto& all = trophyCatalogue();
            size_t half = (all.size() + 1) / 2;
            for (size_t i = 0; i < all.size(); ++i) {
                bool got = trophies_.unlocked(all[i].id);
                float x = (i < half) ? W * 0.05f : W * 0.53f;
                float y = H * 0.22f + static_cast<float>(i < half ? i : i - half) * lh * 1.55f;
                text(x, y, std::string(got ? "* " : "- ") + all[i].name, s * 0.9f, got ? yellow : glm::vec4(0.5f, 0.5f, 0.5f, 1.f));
                text(x + 20.f * s, y + lh * 0.75f, got ? std::string(all[i].description) + "  " + trophies_.unlockDate(all[i].id) : all[i].description, s * 0.6f, got ? dim : glm::vec4(0.4f, 0.4f, 0.4f, 1.f));
            }
            hotText(W * 0.5f, H - lh * 1.5f, "< BACK   (ESC)", s * 0.85f, yellow, 1, kHotBack, 0);
        } else if (screen_ == kScreenProfiles) {
            text(W * 0.5f, H * 0.10f, profileRequired_ ? "WHO IS PLAYING?" : "PLAYERS", s * 1.8f, white, 1);
            const glm::vec4 warn(1.f, 0.35f, 0.25f, 1.f), grey(0.5f, 0.5f, 0.55f, 1.f);
            // Each player: the name, then a line of what they have done; the selected one
            // shows its actions (also clickable). Long lists get squeezed rather than cut.
            const float rowH = std::min(lh * 2.f, (H * 0.72f - H * 0.20f) / static_cast<float>(std::max<size_t>(profileList_.size() + 1, 1)));
            float y = H * 0.20f;
            for (size_t i = 0; i < profileList_.size(); ++i) {
                const bool sel = static_cast<int>(i) == screenIndex_;
                const bool current = profileList_[i] == profileName_;
                const ProfileInfo& pi = profileInfo_[i];
                // Names on the left of the middle, what they have done on the right; both
                // shrink rather than run off the edge on a narrow window or a long name.
                const float leftW = W * 0.5f - 20.f - 24.f, rightW = W - 24.f - (W * 0.5f + 20.f);
                const std::string nameLine = (sel ? "> " : "") + profileList_[i] + (current ? "  (CURRENT)" : "");
                float nameScale = s;
                if (static_cast<float>(assets_.textWidth(nameLine, nameScale)) > leftW) nameScale *= leftW / static_cast<float>(assets_.textWidth(nameLine, nameScale));
                hotText(W * 0.5f - 20.f, y, nameLine, nameScale, sel ? yellow : dim, 2, kHotScreenItem, static_cast<int>(i));
                std::string info = "BEST " + std::to_string(pi.best) + "   " + std::to_string(pi.trophies) + "/" + std::to_string(trophies_.total()) + " TROPHIES   " + PlayerStats::formatDuration(pi.played) + (pi.email ? "   EMAIL SET" : "");
                textFit(W * 0.5f + 20.f, y + lh * 0.15f, info, s * 0.6f, sel ? white : grey, 0, rightW);
                if (sel && profileConfirmDelete_ == static_cast<int>(i)) {
                    // The confirm gets the whole width rather than the right-hand column:
                    // it is much longer than the row it belongs to.
                    textFit(W * 0.5f, y + lh * 0.85f, "DELETE " + profileList_[i] + " AND ALL ITS SCORES, TROPHIES AND STATS?   ENTER YES   ESC NO", s * 0.6f, warn, 1, W - 48.f);
                } else if (sel) {
                    float ax = W * 0.5f + 20.f;
                    const char* acts[3] = {"RENAME", "EMAIL", "DELETE"};
                    const char* keys[3] = {pad_ ? "X" : "R", pad_ ? "" : "E", pad_ ? "Y" : "DEL"};
                    for (int a = 0; a < 3; ++a) {
                        if (pad_ && a == 1) continue;   // no pad shortcut for the email screen; click or use OPTIONS
                        std::string t = std::string("[") + keys[a] + "] " + acts[a];
                        hotText(ax, y + lh * 0.85f, t, s * 0.6f, a == 2 ? warn : dim, 0, kHotProfileAction, a);
                        ax += static_cast<float>(assets_.textWidth(t, s * 0.6f)) + 24.f * s * 0.6f;
                    }
                }
                y += rowH;
            }
            const bool selNew = screenIndex_ == static_cast<int>(profileList_.size());
            hotText(W * 0.5f, y + lh * 0.2f, selNew ? "> NEW PLAYER <" : "NEW PLAYER", s, selNew ? yellow : dim, 1, kHotScreenItem, static_cast<int>(profileList_.size()));
            if (!profileRequired_) hotText(W * 0.5f, y + lh * 1.8f, "BACK", s, dim, 1, kHotBack, 0);
            if (!wadStatus_.empty()) text(W * 0.5f, H - lh * 3.2f, wadStatus_, s * 0.75f, glm::vec4(1.f, 0.8f, 0.4f, 1.f), 1);
            textFit(W * 0.5f, H - lh * 2.f, "EACH PLAYER KEEPS THEIR OWN SCORES, TROPHIES, SETTINGS AND PLAY TIME", s * 0.7f, dim, 1, W - 48.f);
            textFit(W * 0.5f, H - lh * 1.1f, pad_ ? "A PLAY   X RENAME   Y DELETE   B BACK" : "ENTER PLAY   R RENAME   E EMAIL   DEL DELETE   ESC BACK", s * 0.7f, dim, 1, W - 48.f);
        } else if (screen_ == kScreenCredits) {
            text(W * 0.5f, H * 0.12f, "REDLINE", s * 2.6f, glm::vec4(1.f, 0.15f, 0.1f, 1.f), 1);
            float y = H * 0.12f + lh * 3.2f;
            auto line = [&](const std::string& t, float sc, glm::vec4 c, float gap) { text(W * 0.5f, y, t, s * sc, c, 1); y += lh * gap; };
            line("CREATED BY", 0.75f, dim, 0.9f);
            line("DAVID JANICE", 1.5f, yellow, 1.7f);
            line("SPECIAL THANKS TO", 0.75f, dim, 0.9f);
            line("JORDAN DRACOULIS", 1.3f, yellow, 1.7f);
            line("WRITTEN WITH CLAUDE CODE  -  VULKAN, SDL3, C++20", 0.75f, white, 1.0f);
            line("DOOM ART, SOUNDS AND MUSIC: ID SOFTWARE", 0.7f, dim, 0.85f);
            line("MODERN SOUNDTRACK: ANDREW HULSHULT   SC-55 RECORDINGS: THE DOOM RERELEASE", 0.7f, dim, 0.85f);
            line("LIBVORBIS, FLUIDSYNTH, GLM", 0.7f, dim, 1.4f);
            hotText(W * 0.5f, y, "< BACK   (ESC)", s * 0.85f, yellow, 1, kHotBack, 0);
        } else if (screen_ == kScreenWadSetup) {
            text(W * 0.5f, H * 0.12f, "REDLINE NEEDS DOOM", s * 2.f, glm::vec4(1.f, 0.15f, 0.1f, 1.f), 1);
            float y = H * 0.12f + lh * 3.f;
            auto line = [&](const std::string& t, float sc, glm::vec4 c, float gap) { text(W * 0.5f, y, t, s * sc, c, 1); y += lh * gap; };
            line("THE MONSTERS, WEAPONS, SOUNDS AND MUSIC COME FROM YOUR OWN COPY OF DOOM.", 0.8f, white, 1.0f);
            line("POINT REDLINE AT DOOM.WAD OR DOOM2.WAD (STEAM, GOG, OR THE ORIGINAL DISC).", 0.8f, white, 1.0f);
            line("EXTRAS.WAD FROM THE DOOM + DOOM II RERELEASE, KEPT NEXT TO IT, ADDS THE SOUNDTRACKS.", 0.7f, dim, 1.0f);
            line("NOTHING IS COPIED: THE FILE STAYS WHERE IT IS AND THE CHOICE IS REMEMBERED.", 0.7f, dim, 2.0f);
            const char* items[4] = {"BROWSE FOR THE WAD FILE", "TYPE THE PATH", "PLAY WITH PLACEHOLDER ART FOR NOW", "QUIT"};
            for (int i = 0; i < 4; ++i) {
                bool sel = screenIndex_ == i;
                hotText(W * 0.5f, y, sel ? std::string("> ") + items[i] + " <" : items[i], s * 1.05f, sel ? yellow : dim, 1, kHotScreenItem, i);
                y += lh * 1.35f;
            }
            if (!wadStatus_.empty()) text(W * 0.5f, y + lh * 0.5f, wadStatus_, s * 0.8f, glm::vec4(1.f, 0.8f, 0.4f, 1.f), 1);
            if (!wadMissing_) hotText(W * 0.5f, y + lh * 0.4f, "BACK", s * 1.05f, dim, 1, kHotBack, 0);
            text(W * 0.5f, H - lh * 2.f, "YOU CAN CHANGE THIS LATER UNDER OPTIONS > DOOM WAD", s * 0.7f, dim, 1);
        } else if (screen_ == kScreenEmailEntry) {
            text(W * 0.5f, H * 0.20f, "YOUR EMAIL ADDRESS (OPTIONAL)", s * 1.4f, white, 1);
            float f = 0.5f + 0.5f * std::sin(time_ * 6.f);
            text(W * 0.5f, H * 0.32f, emailEntry_ + (f > 0.5f ? "_" : " "), s * 1.1f, yellow, 1);
            float y = H * 0.32f + lh * 2.2f;
            auto line = [&](const std::string& t, float sc, glm::vec4 c) { text(W * 0.5f, y, t, s * sc, c, 1); y += lh * 1.0f; };
            line("THIS PUTS YOUR SCORES ON THE ONLINE LEADERBOARD. YOU GET AN EMAIL WITH AN", 0.7f, dim);
            line("APPROVE LINK; NOTHING IS POSTED UNTIL YOU CLICK IT. THE ADDRESS IS STORED", 0.7f, dim);
            line("ENCRYPTED AND NEVER SHOWN TO OTHER PLAYERS. LEAVE IT EMPTY TO KEEP SCORES HERE.", 0.7f, dim);
            y += lh * 0.6f;
            hotText(W * 0.5f - 40.f, y, "SAVE", 0.95f * s, yellow, 2, kHotScreenItem, 0);
            hotText(W * 0.5f + 40.f, y, "CANCEL", 0.95f * s, dim, 0, kHotBack, 0);
            y += lh * 1.1f;
            line("ENTER SAVES   LEAVE EMPTY TO SKIP   ESC CANCELS", 0.7f, dim);
            if (!wadStatus_.empty()) text(W * 0.5f, y + lh * 0.5f, wadStatus_, s * 0.9f, glm::vec4(1.f, 0.5f, 0.3f, 1.f), 1);
        } else if (screen_ == kScreenOnlineAsk) {
            text(W * 0.5f, H * 0.14f, "POST YOUR SCORES ONLINE?", s * 1.5f, white, 1);
            float y = H * 0.14f + lh * 2.2f;
            auto line = [&](const std::string& t, float sc, glm::vec4 c, float gap = 1.0f) { text(W * 0.5f, y, t, s * sc, c, 1); y += lh * gap; };
            line("REDLINE HAS A WORLD LEADERBOARD. WITH IT, EVERY FINISHED GAME IS POSTED", 0.72f, dim);
            line("(YOUR PLAYER NAME, THE SCORE AND GAME STATISTICS, AND WHAT THIS MACHINE IS)", 0.72f, dim);
            line("AND YOU SEE YOUR WORLD RANK AFTER EACH GAME.", 0.72f, dim, 1.4f);
            line("IT NEEDS AN EMAIL ADDRESS: YOU GET ONE MESSAGE WITH AN APPROVE LINK, AND NOTHING", 0.72f, dim);
            line("IS POSTED UNTIL YOU CLICK IT. THE ADDRESS IS STORED ENCRYPTED AND NEVER SHOWN.", 0.72f, dim, 1.8f);
            const char* opts[3] = {"YES, SET IT UP", "NOT NOW", "NO, KEEP MY SCORES ON THIS MACHINE"};
            for (int i = 0; i < 3; ++i) {
                const bool sel = screenIndex_ == i;
                hotText(W * 0.5f, y, sel ? std::string("> ") + opts[i] + " <" : opts[i], s * (i == 0 ? 1.1f : 0.95f), sel ? yellow : dim, 1, kHotScreenItem, i);
                y += lh * 1.4f;
            }
            text(W * 0.5f, H - lh * 1.5f, "YOU CAN CHANGE THIS ANY TIME UNDER OPTIONS > ONLINE AND PLAYER EMAIL", s * 0.65f, dim, 1);
        } else if (screen_ == kScreenAdopt) {
            // This address already has players. Offer them, with enough to tell them apart.
            text(W * 0.5f, H * 0.14f, "YOU ALREADY HAVE A PLAYER HERE", s * 1.4f, white, 1);
            float y = H * 0.14f + lh * 2.2f;
            auto line = [&](const std::string& t, float sc, glm::vec4 c, float gap = 1.0f) { text(W * 0.5f, y, t, s * sc, c, 1); y += lh * gap; };
            line("THIS EMAIL ADDRESS IS ALREADY POSTING SCORES FOR THE PLAYER(S) BELOW.", 0.72f, dim);
            line("IF ONE OF THEM IS YOU, CARRY ON AS THEM AND KEEP THAT HISTORY:", 0.72f, dim);
            line("THIS PROFILE'S SCORES JOIN THEIRS AND THERE IS ONE PLAYER, NOT TWO.", 0.72f, dim, 1.6f);
            for (size_t i = 0; i < adoptChoices_.size(); ++i) {
                const AccountPlayer& a = adoptChoices_[i];
                const bool sel = screenIndex_ == static_cast<int>(i);
                const std::string label = "CONTINUE AS " + a.name;
                hotText(W * 0.5f, y, sel ? "> " + label + " <" : label, s * 1.05f, sel ? yellow : dim, 1, kHotScreenItem, static_cast<int>(i));
                y += lh * 0.95f;
                char buf[160];
                std::snprintf(buf, sizeof buf, "%d RUN%s   BEST %d   %d MACHINE%s   SINCE %s", a.runs, a.runs == 1 ? "" : "S", a.bestScore,
                              a.machines, a.machines == 1 ? "" : "S", a.since.c_str());
                text(W * 0.5f, y, buf, s * 0.68f, dim, 1);
                y += lh * 1.25f;
            }
            {
                const bool sel = screenIndex_ == static_cast<int>(adoptChoices_.size());
                hotText(W * 0.5f, y, sel ? "> START FRESH, THIS IS A DIFFERENT PLAYER <" : "START FRESH, THIS IS A DIFFERENT PLAYER", s * 0.95f, sel ? yellow : dim, 1, kHotScreenItem, static_cast<int>(adoptChoices_.size()));
                y += lh * 1.4f;
            }
            if (adoptBusy_) text(W * 0.5f, y, "MERGING...", s * 0.9f, yellow, 1);
            else if (!wadStatus_.empty()) text(W * 0.5f, y, wadStatus_, s * 0.85f, glm::vec4(1.f, 0.5f, 0.3f, 1.f), 1);
            text(W * 0.5f, H - lh * 1.5f, "SEVERAL PEOPLE CAN SHARE ONE ADDRESS: START FRESH KEEPS THEIR SCORES SEPARATE FROM YOURS", s * 0.65f, dim, 1);
        } else if (screen_ == kScreenDeleteOnline) {
            text(W * 0.5f, H * 0.16f, "DELETE " + deleteProfileName_, s * 1.4f, white, 1);
            float y = H * 0.16f + lh * 2.2f;
            auto line = [&](const std::string& t, float sc, glm::vec4 c, float gap = 1.0f) { text(W * 0.5f, y, t, s * sc, c, 1); y += lh * gap; };
            line("THIS PLAYER ALSO HAS A RECORD ON THE LEADERBOARD.", 0.75f, dim);
            if (deleteInfoLoaded_) {
                char buf[160];
                std::snprintf(buf, sizeof buf, "%d RUN%s   BEST %d   %d MACHINE%s   SINCE %s", deleteOnlineInfo_.runs, deleteOnlineInfo_.runs == 1 ? "" : "S",
                              deleteOnlineInfo_.bestScore, deleteOnlineInfo_.machines, deleteOnlineInfo_.machines == 1 ? "" : "S", deleteOnlineInfo_.since.c_str());
                line(buf, 0.75f, yellow, 1.4f);
            } else {
                line("READING IT...", 0.7f, dim, 1.4f);
            }
            line("KEEPING IT MEANS THAT IF YOU MAKE THIS PLAYER AGAIN WITH THE SAME EMAIL", 0.7f, dim);
            line("ADDRESS, THE GAME WILL OFFER TO CARRY ON WHERE THIS ONE LEFT OFF.", 0.7f, dim, 1.8f);
            const char* opts[3] = {"DELETE THE PROFILE, KEEP THE ONLINE RECORD", "DELETE BOTH: THE RECORD AND ITS SCORES ARE GONE FOR GOOD", "CANCEL"};
            for (int i = 0; i < 3; ++i) {
                const bool sel = screenIndex_ == i;
                hotText(W * 0.5f, y, sel ? std::string("> ") + opts[i] + " <" : opts[i], s * (i == 1 ? 0.95f : 1.f), sel ? (i == 1 ? glm::vec4(1.f, 0.5f, 0.35f, 1.f) : yellow) : dim, 1, kHotScreenItem, i);
                y += lh * 1.4f;
            }
        } else if (screen_ == kScreenWhatsNew) {
            text(W * 0.5f, H * 0.05f, updateAvailable_ ? "VERSION " + latestVersion_ + " IS OUT   (YOU HAVE " + std::string(REDLINE_VERSION) + ")" : "WHAT'S NEW   (VERSION " + std::string(REDLINE_VERSION) + ")", s * 1.3f, updateAvailable_ ? yellow : white, 1);
            // Flatten the changelog into lines that fit the width, then show a window of them.
            std::vector<std::pair<std::string, int>> lines;   // text, kind 0 heading / 1 bullet / 2 continuation
            const float fs = s * 0.75f;
            const float maxW = W * 0.8f;
            for (size_t e = 0; e < changelog_.size(); ++e) {
                const Json& entry = changelog_[e];
                lines.push_back({"VERSION " + entry["version"].asString() + (entry["date"].asString().empty() ? "" : "   " + entry["date"].asString()), 0});
                for (size_t k = 0; k < entry["items"].size(); ++k) {
                    std::string rest = entry["items"][k].asString();
                    for (char& c : rest) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                    std::string prefix = "- ";
                    int kind = 1;
                    while (!rest.empty()) {
                        std::string piece = rest;
                        while (static_cast<float>(assets_.textWidth(prefix + piece, fs)) > maxW && piece.find(' ') != std::string::npos) piece.erase(piece.rfind(' '));
                        lines.push_back({prefix + piece, kind});
                        rest = rest.size() > piece.size() ? rest.substr(piece.size() + 1) : "";
                        prefix = "  "; kind = 2;
                    }
                }
                lines.push_back({"", 2});
            }
            const float top = H * 0.05f + lh * 2.2f, rowH = lh * 0.85f;
            const int visible = std::max(3, static_cast<int>((H - lh * 3.2f - top) / rowH));
            if (lines.empty()) text(W * 0.5f, top + lh, OnlineClient::available() && onlineOn_ ? "NO CHANGE LIST YET: THE SERVER HAS NOT ANSWERED" : "TURN ON ONLINE UNDER OPTIONS TO FETCH THE CHANGE LIST", s * 0.85f, dim, 1);
            whatsNewScroll_ = std::clamp(whatsNewScroll_, 0, std::max(0, static_cast<int>(lines.size()) - visible));
            float y = top;
            for (int i = whatsNewScroll_; i < static_cast<int>(lines.size()) && i < whatsNewScroll_ + visible; ++i) {
                const auto& [t, kind] = lines[static_cast<size_t>(i)];
                if (kind == 0) text(W * 0.1f, y, t, s * 0.9f, yellow, 0);
                else text(W * 0.1f, y, t, fs, kind == 1 ? white : dim, 0);
                y += rowH;
            }
            if (static_cast<int>(lines.size()) > whatsNewScroll_ + visible) text(W * 0.5f, y, "v  MORE  v", s * 0.6f, dim, 1);
            hotText(W * 0.5f - 40.f, H - lh * 1.5f, updateAvailable_ ? "DOWNLOAD VERSION " + latestVersion_ : "OPEN THE DOWNLOADS PAGE", s * 0.9f, yellow, 2, kHotScreenItem, 0);
            hotText(W * 0.5f + 40.f, H - lh * 1.5f, "BACK", s * 0.9f, dim, 0, kHotBack, 0);
            text(W * 0.5f, H - lh * 0.8f, "ENTER OPENS THE PAGE IN YOUR BROWSER   UP/DOWN SCROLL   ESC BACK", s * 0.6f, dim, 1);
        } else if (screen_ == kScreenRegister) {
            text(W * 0.5f, H * 0.16f, "REGISTER ONLINE", s * 1.5f, white, 1);
            float y = H * 0.16f + lh * 2.2f;
            auto line = [&](const std::string& t, float sc, glm::vec4 c, float gap = 1.0f) { text(W * 0.5f, y, t, s * sc, c, 1); y += lh * gap; };
            line("A SIX-DIGIT CODE WILL BE EMAILED TO", 0.8f, dim, 1.1f);
            line(registerEmail_, 1.1f, yellow, 1.6f);
            line("CONFIRMING IT ATTACHES THE PLAYER " + profileName_ + " ON THIS MACHINE (" + machineLabel() + ")", 0.7f, dim);
            line("TO THAT ADDRESS. FROM THEN ON, WHEN ONLINE IS ON, EACH FINISHED GAME IS POSTED:", 0.7f, dim);
            line("YOUR PLAYER NAME, THE SCORE AND GAME STATISTICS, AND WHAT THIS MACHINE IS", 0.7f, dim);
            line("(OS, GPU, CORES, MEMORY, GAMEPAD MODEL). THE ADDRESS IS STORED ENCRYPTED AND", 0.7f, dim);
            line("NEVER SHOWN TO OTHER PLAYERS. EACH PLAYER AND EACH MACHINE IS ASKED SEPARATELY.", 0.7f, dim, 1.6f);
            hotText(W * 0.5f - 40.f, y, registerBusy_ ? "SENDING..." : "SEND THE CODE", 0.95f * s, yellow, 2, kHotScreenItem, 0);
            hotText(W * 0.5f + 40.f, y, "NOT NOW", 0.95f * s, dim, 0, kHotBack, 0);
            y += lh * 1.2f;
            if (!wadStatus_.empty()) text(W * 0.5f, y, wadStatus_, s * 0.8f, glm::vec4(1.f, 0.8f, 0.4f, 1.f), 1);
            text(W * 0.5f, H - lh * 1.5f, "ENTER SENDS   ESC NOT NOW   (YOU CAN REGISTER LATER UNDER OPTIONS > PLAYER EMAIL)", s * 0.65f, dim, 1);
        } else if (screen_ == kScreenCode) {
            text(W * 0.5f, H * 0.16f, "CHECK YOUR EMAIL", s * 1.4f, white, 1);
            text(W * 0.5f, H * 0.16f + lh * 1.6f, "SENT TO " + registerEmail_ + "   (CHECK THE SPAM FOLDER TOO)", s * 0.7f, dim, 1);
            text(W * 0.5f, H * 0.16f + lh * 2.6f, "CLICK THE APPROVE LINK IN IT AND THIS SCREEN FINISHES BY ITSELF, OR TYPE THE CODE:", s * 0.7f, dim, 1);
            { const float f2 = 0.5f + 0.5f * std::sin(time_ * 3.f); text(W * 0.5f, H * 0.16f + lh * 3.5f, "WAITING FOR YOUR APPROVAL...", s * 0.65f, glm::vec4(0.7f, 0.9f, 1.f, 0.5f + 0.5f * f2), 1); }
            float f = 0.5f + 0.5f * std::sin(time_ * 6.f);
            std::string shown;
            for (int i = 0; i < 6; ++i) { shown += i < static_cast<int>(codeEntry_.size()) ? codeEntry_[static_cast<size_t>(i)] : (i == static_cast<int>(codeEntry_.size()) && f > 0.5f ? '_' : '-'); shown += ' '; }
            text(W * 0.5f, H * 0.38f, shown, s * 2.4f, yellow, 1);
            if (pad_) text(W * 0.5f, H * 0.52f, "GAMEPAD: UP/DOWN PICK A DIGIT  [" + std::to_string(codeChar_) + "]  A ADD  B DELETE  START DONE", s * 0.75f, dim, 1);
            hotText(W * 0.5f - 40.f, H * 0.60f, registerBusy_ ? "CHECKING..." : "CONFIRM", 0.95f * s, yellow, 2, kHotScreenItem, 0);
            hotText(W * 0.5f + 40.f, H * 0.60f, "CANCEL", 0.95f * s, dim, 0, kHotBack, 0);
            if (!wadStatus_.empty()) text(W * 0.5f, H * 0.60f + lh * 1.3f, wadStatus_, s * 0.85f, glm::vec4(1.f, 0.8f, 0.4f, 1.f), 1);
            text(W * 0.5f, H - lh * 1.5f, "TYPE THE SIX DIGITS AND PRESS ENTER   R SENDS A NEW EMAIL   ESC LATER (THE LINK KEEPS WORKING)", s * 0.65f, dim, 1);
        } else if (screen_ == kScreenLeaderboard) {
            static const char* tabs[] = {"GLOBAL", "THIS WEEK", "FIGHTS", "LEVEL", "DEMONS SLAIN"};
            text(W * 0.5f, H * 0.06f, "LEADERBOARD", s * 1.5f, white, 1);
            float tx = W * 0.5f - 0.f;
            float widths[5]; float total = 0.f;
            for (int i = 0; i < 5; ++i) { widths[i] = static_cast<float>(assets_.textWidth(tabs[i], s * 0.8f)) + 30.f * s * 0.8f; total += widths[i]; }
            tx = W * 0.5f - total * 0.5f;
            for (int i = 0; i < 5; ++i) {
                hotText(tx + widths[i] * 0.5f, H * 0.06f + lh * 1.9f, std::string(i == leaderboardTab_ ? "[ " : "") + tabs[i] + (i == leaderboardTab_ ? " ]" : ""), s * 0.8f, i == leaderboardTab_ ? yellow : dim, 1, kHotScreenItem, i);
                tx += widths[i];
            }
            float y = H * 0.06f + lh * 3.6f;
            const float colRank = W * 0.12f, colName = W * 0.18f, colScore = W * 0.58f, colLevel = W * 0.70f, colTier = W * 0.80f;
            const char* what = leaderboardTab_ == 2 ? "FIGHTS" : leaderboardTab_ == 3 ? "LEVEL" : leaderboardTab_ == 4 ? "DEMONS" : "SCORE";
            text(colRank, y, "#", s * 0.65f, dim, 0); text(colName, y, "PLAYER", s * 0.65f, dim, 0); text(colScore, y, what, s * 0.65f, dim, 2); text(colLevel, y, "LVL", s * 0.65f, dim, 2); text(colTier, y, "RANK", s * 0.65f, dim, 0);
            y += lh * 0.9f;
            if (!OnlineClient::available()) text(W * 0.5f, y + lh, "THIS BUILD HAS NO ONLINE SUPPORT", s * 0.9f, dim, 1);
            else if (!onlineOn_) text(W * 0.5f, y + lh, "TURN ON ONLINE UNDER OPTIONS TO SEE THE BOARD", s * 0.9f, dim, 1);
            else if (leaderboardLoading_ && leaderboard_.isNull()) text(W * 0.5f, y + lh, "LOADING...", s * 0.9f, dim, 1);
            else if (!leaderboardError_.empty() && leaderboard_.isNull()) text(W * 0.5f, y + lh, "COULD NOT REACH THE SERVER: " + leaderboardError_, s * 0.8f, glm::vec4(1.f, 0.6f, 0.4f, 1.f), 1);
            else {
                const Json& rows = leaderboard_["rows"];
                const float rowH = lh * 0.95f;
                const int maxRows = std::max(5, static_cast<int>((H - lh * 4.5f - y) / rowH));
                if (rows.size() == 0) text(W * 0.5f, y + lh, "NOBODY HAS POSTED A SCORE YET. BE THE FIRST.", s * 0.9f, dim, 1);
                for (size_t i = 0; i < rows.size() && static_cast<int>(i) < maxRows; ++i) {
                    const Json& r = rows[i];
                    const bool me = r["player_id"].asString() == stats_.playerId();
                    const glm::vec4 c = me ? yellow : (i < 3 ? white : dim);
                    text(colRank, y, std::to_string(r["rank"].asInt()), s * 0.75f, c, 0);
                    text(colName, y, r["name"].asString("?"), s * 0.75f, c, 0);
                    text(colScore, y, std::to_string(r["value"].asInt()), s * 0.75f, c, 2);
                    text(colLevel, y, std::to_string(r["level"].asInt()), s * 0.75f, c, 2);
                    text(colTier, y, r["tier"].asString(""), s * 0.65f, c, 0);
                    y += rowH;
                }
                const Json& me = leaderboard_["me"];
                if (me.isObject()) {
                    y += lh * 0.3f;
                    text(colRank, y, std::to_string(me["rank"].asInt()), s * 0.75f, yellow, 0);
                    text(colName, y, "YOU  (" + profileName_ + ")", s * 0.75f, yellow, 0);
                    text(colScore, y, std::to_string(me["value"].asInt()), s * 0.75f, yellow, 2);
                    text(colLevel, y, std::to_string(me["level"].asInt()), s * 0.75f, yellow, 2);
                    text(colTier, y, me["tier"].asString(""), s * 0.65f, yellow, 0);
                }
                std::string foot = leaderboardFetched_.empty() ? "" : "FETCHED " + leaderboardFetched_;
                if (leaderboardLoading_) foot = "UPDATING...";
                if (leaderboard_["total"].asInt() > 0) foot += (foot.empty() ? "" : "   ") + std::to_string(leaderboard_["total"].asInt()) + " PLAYERS";
                if (!foot.empty()) text(W * 0.5f, H - lh * 2.6f, foot, s * 0.65f, dim, 1);
            }
            hotText(W * 0.5f, H - lh * 1.5f, "< BACK   (ESC)     LEFT/RIGHT BOARDS     R REFRESH", s * 0.8f, yellow, 1, kHotBack, 0);
        } else if (screen_ == kScreenWadPath) {
            text(W * 0.5f, H * 0.22f, "TYPE THE FULL PATH TO DOOM.WAD", s * 1.4f, white, 1);
            float f = 0.5f + 0.5f * std::sin(time_ * 6.f);
            std::string shown = wadEntry_;
            if (shown.size() > 70) shown = "..." + shown.substr(shown.size() - 67);
            text(W * 0.5f, H * 0.36f, shown + (f > 0.5f ? "_" : " "), s * 0.9f, yellow, 1);
            text(W * 0.5f, H * 0.36f + lh * 1.6f, "(SHOWN IN CAPITALS; THE PATH IS KEPT EXACTLY AS TYPED)", s * 0.65f, dim, 1);
            hotText(W * 0.5f - 40.f, H * 0.50f, "LOAD", s * 0.95f, yellow, 2, kHotScreenItem, 0);
            hotText(W * 0.5f + 40.f, H * 0.50f, "BACK", s * 0.95f, dim, 0, kHotBack, 0);
            if (!wadStatus_.empty()) text(W * 0.5f, H * 0.58f, wadStatus_, s * 0.9f, glm::vec4(1.f, 0.5f, 0.3f, 1.f), 1);
        } else if (screen_ == kScreenNameEntry) {
            static const std::string kLetters = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 ";
            text(W * 0.5f, H * 0.22f, nameRename_ ? "NEW NAME FOR " + renameFrom_ + "?" : "WHAT IS YOUR NAME, MARINE?", s * 1.4f, white, 1);
            if (!wadStatus_.empty()) text(W * 0.5f, H * 0.72f, wadStatus_, s * 0.8f, glm::vec4(1.f, 0.5f, 0.3f, 1.f), 1);
            float f = 0.5f + 0.5f * std::sin(time_ * 6.f);
            text(W * 0.5f, H * 0.36f, nameEntry_ + (f > 0.5f ? "_" : " "), s * 2.2f, yellow, 1);
            if (pad_) {
                std::string pick(1, kLetters[static_cast<size_t>(nameChar_)]);
                text(W * 0.5f, H * 0.50f, "GAMEPAD: UP/DOWN PICK A LETTER  [" + pick + "]  A ADD  B DELETE  START DONE", s * 0.75f, dim, 1);
            }
            text(W * 0.5f, H * 0.58f, "TYPE YOUR NAME AND PRESS ENTER", s * 0.8f, dim, 1);
            if (!nameRequired_ || !profileName_.empty()) hotText(W * 0.5f, H * 0.64f, "CANCEL   (ESC)", s * 0.85f, dim, 1, kHotBack, 0);
        }
    }
    if (mode_ == Mode::Paused && screen_ == kScreenNone) {   // trophies/options opened from here draw on top instead
        panel(0.f, 0.f, W, H, glm::vec4(0.f, 0.f, 0.f, 0.5f));
        text(W * 0.5f, H * 0.22f, "PAUSED", s * 2.f, white, 1);
        // The run so far.
        {
            float y = H * 0.22f + lh * 2.4f;
            std::string line = "SCORE " + std::to_string(game_->score()) + "   LEVEL " + std::to_string(game_->level()) + "   LINES " + std::to_string(game_->lines()) + "   RED LINES " + std::to_string(redLinesSurvived_);
            text(W * 0.5f, y, line, s * 0.85f, glm::vec4(0.6f, 0.9f, 1.f, 1.f), 1);
            y += lh * 0.95f;
            std::string line2 = (profileName_.empty() ? std::string("") : profileName_ + "   ") + "BEST " + std::to_string(std::max(highScores_.best(), game_->score())) + "   TROPHIES " + std::to_string(trophies_.unlockedCount()) + "/" + std::to_string(trophies_.total());
            if (fps_.totalEnemies() > 0 && pausedFrom_ == Mode::Fps) line2 += "   DEMONS LEFT " + std::to_string(fps_.enemiesLeft());
            if (stats_.loaded()) line2 += "   PLAYTIME " + PlayerStats::formatDuration(stats_.lifetime().total());
            text(W * 0.5f, y, line2, s * 0.75f, dim, 1);
        }
        drawMenu(H * 0.22f + lh * 5.2f);
        text(W * 0.5f, H * 0.32f + lh * 3.f + lh * 1.5f * static_cast<float>(menu_.items.size()) + lh, "UP/DOWN SELECT   ENTER CONFIRM   ESC RESUME", s * 0.7f, dim, 1);
    }
    if (mode_ == Mode::GameOver) {
        // Death: a hard red flash that decays into the dark, then the title slams in.
        if (diedInFps_) panel(0.f, 0.f, W, H, glm::vec4(0.6f, 0.f, 0.f, std::max(0.f, 0.75f - gameOverT_ * 0.6f)));
        panel(0.f, 0.f, W, H, glm::vec4(0.f, 0.f, 0.f, std::min(0.65f, gameOverT_ * 0.8f)));
        float slam = 1.f - std::clamp((gameOverT_ - 0.15f) / 0.45f, 0.f, 1.f);
        slam = slam * slam;
        float pulse = 0.85f + 0.15f * std::sin(time_ * 4.f);
        if (gameOverT_ > 0.15f) text(W * 0.5f, H * 0.3f, diedInFps_ ? "YOU DIED" : "GAME OVER", s * (2.4f + 2.5f * slam), glm::vec4(1.f, 0.15f * pulse, 0.1f * pulse, 1.f), 1);
        if (gameOverT_ > 1.2f) {
            float f = 0.5f + 0.5f * std::sin(time_ * 5.f);
            text(W * 0.5f, H * 0.3f - lh * 1.6f, "PRESS ANY KEY TO TRY AGAIN", s * 1.0f, glm::vec4(1.f, 0.95f, 0.6f, 0.55f + 0.45f * f), 1);
        }
        text(W * 0.5f, H * 0.3f + lh * 2.8f, "SCORE " + std::to_string(game_->score()) + "   BEST " + std::to_string(highScore_), s, white, 1);
        text(W * 0.5f, H * 0.3f + lh * 4.f, "LEVEL " + std::to_string(game_->level()) + "   RED LINES SURVIVED " + std::to_string(redLinesSurvived_), s, dim, 1);
        if (lastRank_ > 0) {
            float f = 0.6f + 0.4f * std::sin(time_ * 5.f);
            text(W * 0.5f, H * 0.3f + lh * 5.3f, lastRank_ == 1 ? "NEW HIGH SCORE!" : "HIGH SCORE #" + std::to_string(lastRank_), s * 1.2f, glm::vec4(1.f, 0.9f * f + 0.1f, 0.3f, 1.f), 1);
        }
        if (!onlineStatus_.empty()) text(W * 0.5f, H * 0.3f + lh * 6.2f, onlineStatus_, s * 0.8f, onlineRank_ > 0 ? glm::vec4(0.7f, 0.9f, 1.f, 1.f) : dim, 1);
        drawMenu(H * 0.3f + lh * 6.8f);
        if (!highScores_.entries().empty()) {
            float ty = H * 0.3f + lh * 6.8f + lh * 1.5f * 3.f + lh * 0.6f;
            int shown = 0;
            for (const HighScore& h : highScores_.entries()) {
                if (shown++ >= 5) break;
                bool mine = (shown == lastRank_);
                std::string line = std::to_string(shown) + ".  " + std::to_string(h.score) + "   LVL " + std::to_string(h.level) + "   RED LINES " + std::to_string(h.redLines);
                text(W * 0.5f, ty, line, s * 0.7f, mine ? yellow : dim, 1);
                ty += lh * 0.8f;
            }
        }
    }
    // Trophy toast and the RIP AND TEAR celebration: over everything, menus included.
    if (celebrateT_ >= 0.f) {
        const float ct = celebrateT_;
        const float fade = std::clamp((8.f - ct) / 1.f, 0.f, 1.f);
        // Gold pulse over the whole view for the opening seconds.
        if (ct < 3.f) panel(0.f, 0.f, W, H, glm::vec4(1.f, 0.8f, 0.3f, 0.18f * (1.f - ct / 3.f) * (0.6f + 0.4f * std::sin(time_ * 12.f))));
        // Fireworks and confetti bloom with the scene (HDR pass), everything else on top.
        hdrQuads_ = true;
        for (const Spark& p : sparks_) {
            const float a = std::clamp(p.ttl / std::max(0.05f, p.life * 0.6f), 0.f, 1.f);
            if (p.kind == 2) panel(p.pos.x, p.pos.y, p.size, p.size * 1.6f, glm::vec4(p.col * 1.6f, 0.9f * a * fade));
            else {
                const float sz = p.kind == 0 ? p.size * 1.5f : p.size;
                // A short trail back along the motion, then the bright head.
                const glm::vec2 step = p.vel * (p.kind == 0 ? 0.03f : 0.022f);
                for (int k = 3; k >= 1; --k) {
                    const glm::vec2 q = p.pos - step * static_cast<float>(k);
                    const float ts = sz * (1.f - 0.2f * static_cast<float>(k));
                    panel(q.x - ts * 0.5f, q.y - ts * 0.5f, ts, ts, glm::vec4(p.col * 1.6f, a * fade * (0.55f - 0.12f * static_cast<float>(k))));
                }
                panel(p.pos.x - sz * 0.5f, p.pos.y - sz * 0.5f, sz, sz, glm::vec4(p.col * (p.kind == 0 ? 3.5f : 2.8f), a * fade));
            }
        }
        hdrQuads_ = false;
        if (ct > 0.2f) {
            const float slam = 1.f - std::clamp((ct - 0.2f) / 0.5f, 0.f, 1.f);
            panel(0.f, H * 0.30f - lh * 1.4f, W, lh * 7.2f, glm::vec4(0.f, 0.f, 0.f, 0.62f * fade));   // a band so the words read over anything
            const float pulse = 0.75f + 0.25f * std::sin(time_ * 9.f);
            const float ts = s * (2.6f + 3.f * slam * slam);
            const glm::vec4 col(1.f, 0.25f + 0.6f * pulse, 0.15f, fade);
            for (int k = 0; k < 8; ++k) {   // gold halo
                const float ang = static_cast<float>(k) * 0.7854f;
                text(W * 0.5f + std::cos(ang) * s * 2.f, H * 0.30f + std::sin(ang) * s * 2.f, "RIP AND TEAR!!!", ts, glm::vec4(1.f, 0.8f, 0.2f, 0.25f * fade), 1);
            }
            text(W * 0.5f, H * 0.30f, "RIP AND TEAR!!!", ts, col, 1);
            if (ct > 0.7f) {
                const float in = std::min(1.f, (ct - 0.7f) / 0.3f);
                text(W * 0.5f, H * 0.30f + lh * 3.2f, "EVERY TROPHY EARNED", s * 1.3f, glm::vec4(1.f, 0.9f, 0.4f, in * fade), 1);
                text(W * 0.5f, H * 0.30f + lh * 4.6f, std::to_string(trophies_.total()) + " / " + std::to_string(trophies_.total()) + "   THE CABINET IS FULL", s * 0.85f, glm::vec4(1.f, 1.f, 1.f, in * fade), 1);
            }
        }
    }
    if (!toasts_.empty()) {
        // A card slides down from the top edge, waits, and slides back up.
        const Toast& t = toasts_.front();
        const float hold = t.ultimate ? 8.f : 5.f;
        float slide = t.t < 0.35f ? t.t / 0.35f : t.t > hold - 0.4f ? std::max(0.f, (hold - t.t) / 0.4f) : 1.f;
        slide = slide * slide * (3.f - 2.f * slide);
        const float fs = s * 0.95f;
        const float icon = lh * 2.2f;
        const float nameW = static_cast<float>(assets_.textWidth(t.name, fs)), descW = static_cast<float>(assets_.textWidth(t.desc, s * 0.6f));
        const float cardW = std::max(nameW, std::max(descW, static_cast<float>(assets_.textWidth("TROPHY UNLOCKED   20 / 20", s * 0.55f)))) + icon + lh * 2.2f;
        const float cardH = lh * 3.1f;
        const float x0 = W * 0.5f - cardW * 0.5f, y0 = -cardH - 8.f + slide * (cardH + 8.f + 18.f);
        const glm::vec4 gold = t.ultimate ? glm::vec4(1.f, 0.35f, 0.2f, 1.f) : glm::vec4(1.f, 0.8f, 0.25f, 1.f);
        const float glow = 0.5f + 0.5f * std::sin(time_ * 6.f);
        panel(x0 - 3.f * s * 0.5f, y0 - 3.f * s * 0.5f, cardW + 3.f * s, cardH + 3.f * s, glm::vec4(gold.r, gold.g, gold.b, 0.55f + 0.35f * glow));   // border
        panel(x0, y0, cardW, cardH, glm::vec4(0.03f, 0.02f, 0.04f, 0.94f));
        // Icon: the menu skull, or a plain badge without Doom art.
        const float ix = x0 + lh * 0.7f, iy = y0 + (cardH - icon) * 0.5f;
        panel(ix, iy, icon, icon, glm::vec4(gold.r, gold.g, gold.b, 0.18f));
        if (!assets_.skull.empty()) {
            const render::AtlasRegion& r = assets_.region(assets_.skull);
            const float sc = icon * 0.8f / static_cast<float>(std::max(r.w, r.h));
            screenSprite(assets_.skull, ix + (icon - r.w * sc) * 0.5f, iy + (icon - r.h * sc) * 0.5f, sc, glm::vec4(1.f, 1.f, 1.f, 1.f), 0.f, 1.f);
        }
        const float tx = ix + icon + lh * 0.7f;
        text(tx, y0 + lh * 0.35f, std::string(t.ultimate ? "ULTIMATE TROPHY" : "TROPHY UNLOCKED") + "   " + std::to_string(t.count) + " / " + std::to_string(t.total), s * 0.55f, gold, 0);
        text(tx, y0 + lh * 1.05f, t.name, fs, white, 0);
        text(tx, y0 + lh * 2.25f, t.desc, s * 0.6f, dim, 0);
    }

}

void App::buildScene() {
    cubes_.clear();
    worldQuads_.clear();
    screenQuads_.clear();
    meshes_.clear();
    cubes_.insert(cubes_.end(), envCubes_.begin(), envCubes_.end());

    Camera cam = currentCamera();
    VkExtent2D ext = renderer_->extent();
    float aspect = static_cast<float>(ext.width) / static_cast<float>(std::max(1u, ext.height));
    glm::vec3 eye = cam.eye;
    if (shakeT_ > 0.f) {
        float a = shakeT_ * 0.35f;
        eye += glm::vec3(std::sin(time_ * 90.f) * a, std::cos(time_ * 73.f) * a, 0.f);
    }
    frame_.view = glm::lookAt(eye, cam.target + (eye - cam.eye), cam.up);
    frame_.proj = glm::perspective(glm::radians(cam.fov), aspect, 0.05f, 120.f);
    frame_.cameraPos = eye;
    frame_.time = time_;
    frame_.sunDir = glm::normalize(glm::vec3(0.45f, 0.75f, 0.5f));
    frame_.shadowCenter = glm::vec3(0.f, 8.f, 10.f);
    frame_.shadowRadius = 26.f;
    const bool inDungeon = fps_.stage() == FpsMode::Stage::Dungeon && eye.z < Dungeon::kZTop + 2.f;
    if (inDungeon) frame_.shadowCenter = glm::vec3(eye.x, 8.f, eye.z);   // the sun box follows into the crypt (its ceiling shades it)
    frame_.shadowStrength = 0.85f;
    frame_.rtShadows = renderer_->rayTracingAvailable() ? rtShadows_ : 0;
    frame_.bloom = bloom_ ? 0.4f : 0.f;
    frame_.volumetric = bloom_ ? 1.f : 0.f;   // the BLOOM row also covers the light haze
    frame_.exposure = 1.f;
    float tilt = boardTilt();
    frame_.sunIntensity = glm::mix(0.9f, 0.55f, tilt);
    frame_.ambient = glm::mix(glm::vec3(0.30f, 0.30f, 0.34f), glm::vec3(0.21f, 0.17f, 0.17f), tilt);   // the fight stays moody but readable
    frame_.fogDensity = glm::mix(0.012f, 0.035f, tilt);
    frame_.fogColor = glm::mix(glm::vec3(0.03f, 0.03f, 0.05f), glm::vec3(0.06f, 0.02f, 0.02f), tilt);
    if (inDungeon) {   // torch-lit stone: darker, and the fog goes with the depth
        frame_.ambient = glm::vec3(0.10f, 0.09f, 0.09f);
        frame_.fogDensity = 0.05f;
        frame_.fogColor = glm::vec3(0.02f, 0.015f, 0.012f);
    }
    frame_.clearColor = frame_.fogColor;
    if (mode_ == Mode::Alert) {
        float f = 0.5f + 0.5f * std::sin(modeT_ * 18.f);
        frame_.ambient = glm::mix(frame_.ambient, glm::vec3(0.45f, 0.08f, 0.05f), f * 0.7f);
    }

    addBoard();
    addDecor();
    if (!ambient_.empty() && (mode_ == Mode::Title || mode_ == Mode::Blocks || mode_ == Mode::Alert || (mode_ == Mode::Paused && pausedFrom_ == Mode::Blocks) || (mode_ == Mode::GameOver && !diedInFps_))) {
        addAmbient();
        if (brutalActive()) { size_t i = 0; for (const Decal& d : brawlDecals_) drawDecal(d, i++); }
    }
    addBursts();
    bool actors = (mode_ == Mode::Fps || mode_ == Mode::Countdown || mode_ == Mode::FlyOut || (mode_ == Mode::GameOver && diedInFps_) || (mode_ == Mode::Paused && (pausedFrom_ == Mode::Fps || pausedFrom_ == Mode::Countdown)));
    if (actors) addFpsActors();
    addLights();
    addHud();
}

}  // namespace rl::game
