#include "game/app.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <cmath>
#include <cstdio>
#include <cstdlib>
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
    Uint32 flags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE;
    loadDisplaySettings();
    if (opts_.fullscreen) displayMode_ = 1;
    if (opts_.width != 1600 || opts_.height != 900) { resW_ = opts_.width; resH_ = opts_.height; }
    window_ = SDL_CreateWindow("REDLINE", resW_, resH_, flags);
    if (!window_) throw std::runtime_error(std::string("SDL_CreateWindow: ") + SDL_GetError());
    buildResolutionList();
    if (displayMode_ != 0) applyDisplay();
    if (opts_.rtShadows >= 0) rtShadows_ = opts_.rtShadows;
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
    if (!assets_.load(wad, audio_, extrasHint())) throw std::runtime_error("asset build failed");
    if (wad && assets_.usingWad()) wadPath_ = wad->string();
    renderer_->setAtlas(assets_.atlas().image());
    buildEnvironment();
    buildProps();
    {
        std::string pref;
        if (char* p = SDL_GetPrefPath("redline", "redline")) { pref = p; SDL_free(p); }
        std::string base;
        if (const char* b = SDL_GetBasePath()) base = b;
        if (auto dir = VoxelModels::findPack(opts_.voxelDir, pref, base)) voxels_.init(*renderer_, *dir);
        else std::fprintf(stderr, "[voxels] no Voxel Doom pack found (set REDLINE_VOXELS or --voxels-dir); sprites only\n");
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
        if (!opts_.profile.empty()) last = opts_.profile;
        if (last.empty() && !profiles.empty()) last = profiles.front();
        if (last.empty() && opts_.scenario != "title") last = "PLAYER";   // scripted runs never see the prompt
        if (!last.empty()) switchProfile(last);
        else nameRequired_ = true;
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
    renderer_->setAtlas(assets_.atlas().image());
    buildEnvironment();
    buildProps();
    initMusic();
    music_.stop(0.f);   // updateMusic() restarts the right track from the new set
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

void App::newGame() {
    uint32_t seed = opts_.seed ? opts_.seed : static_cast<uint32_t>(std::chrono::steady_clock::now().time_since_epoch().count() & 0xFFFFFFFFu);
    game_ = std::make_unique<core::Game>(seed);
    fps_ = FpsMode();
    ambient_.reset(seed, 1);
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
    enterMode(opts_.scenario == "title" ? Mode::Title : Mode::Blocks);
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
    if (opts_.scenario == "redline" || opts_.scenario == "fps") {
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
        if (opts_.scenario == "fps" && opts_.level >= 8) {
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
        if (opts_.scenario == "fps") {
            game_->setLevel(opts_.level);
            fps_.setAbsorbPeriod(opts_.absorbPeriod);
            fps_.setGodMode(opts_.god);
            fps_.begin(*game_, opts_.level);
            if (opts_.arsenal >= 0) fps_.giveArsenal(opts_.arsenal);
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
        menu_.items = {"START", "PLAYER: " + (profileName_.empty() ? std::string("NONE") : profileName_), "OPTIONS", "TROPHIES", "CREDITS", "QUIT"};
        refreshAllScores();
        if (nameRequired_ && profileName_.empty() && screen_ == kScreenNone) openScreen(kScreenNameEntry);
        break;
    case Mode::Alert:
        play("redline", 1.f);
        break;
    case Mode::FlyIn:
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
        menu_.items = {"RESTART", "QUIT"};
        if (mouseCaptured_) { SDL_SetWindowRelativeMouseMode(window_, false); mouseCaptured_ = false; }
        break;
    case Mode::Paused:
        menu_.items = {"RESUME", "TROPHIES", "OPTIONS", "RESTART", "QUIT"};
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
    music_.setEnabled(true);
    music_.setVolume(opts_.musicVolume);
    if (std::FILE* f = std::fopen(settingsPath_.c_str(), "r")) {
        char key[64], val[64];
        while (std::fscanf(f, "%63[^=]=%63s\n", key, val) == 2) {
            std::string k = key, v = val;
            if (k == "music_set") musicSet_ = v == "classic" ? MusicSet::Classic : v == "sc55" ? MusicSet::Sc55 : MusicSet::Modern;
            else if (k == "music_on") music_.setEnabled(v != "0");
            else if (k == "music_volume") music_.setVolume(static_cast<float>(std::atof(v.c_str())));
            else if (k == "pad_sens") padSens_ = std::clamp(static_cast<float>(std::atof(v.c_str())), 0.25f, 3.f);
            else if (k == "pad_invert") padInvertY_ = v != "0";
            else if (k == "pad_rumble") padRumble_ = v != "0";
            else if (k == "voxels") useVoxels_ = v != "0";
            else if (k == "doom_art") doomArtOff_ = v == "0";
        }
        std::fclose(f);
    }
    if (opts_.voxels >= 0) useVoxels_ = opts_.voxels == 1;   // --voxels / --sprites override the saved choice
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

void App::trophy(const char* id) {
    if (!trophies_.unlock(id)) return;
    for (const TrophyDef& d : trophyCatalogue())
        if (std::string(id) == d.id) {
            announce(std::string("TROPHY: ") + d.name, glm::vec4(1.f, 0.85f, 0.2f, 1.f), 1.5f);
            announce(d.description, glm::vec4(1.f, 0.95f, 0.7f, 1.f), 0.8f);
            std::fprintf(stderr, "[app] trophy unlocked: %s\n", d.name);
        }
    play("pickup_weapon", 1.f, 1.3f);
    rumble(0.3f, 0.6f, 200);
    // The ultimate trophy: everything else in the cabinet.
    if (std::string(id) != "rip_and_tear" && !trophies_.unlocked("rip_and_tear") && trophies_.unlockedCount() == trophies_.total() - 1) {
        trophies_.unlock("rip_and_tear");
        announce("RIP AND TEAR!!!", glm::vec4(1.f, 0.2f, 0.1f, 1.f), 2.6f);
        announce("ULTIMATE TROPHY: EVERY TROPHY EARNED", glm::vec4(1.f, 0.85f, 0.2f, 1.f), 1.1f);
        std::fprintf(stderr, "[app] trophy unlocked: RIP AND TEAR!!!\n");
        play("levelup", 1.f, 0.8f);
        play("bfg", 0.8f);
        shakeT_ = 0.8f;
        rumble(1.f, 1.f, 900);
    }
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
    if (screen == kScreenProfiles) profileList_ = listProfiles();
    if (screen == kScreenNameEntry) { nameEntry_.clear(); nameChar_ = 0; SDL_StartTextInput(window_); }
    if (screen == kScreenWadPath) { wadEntry_ = Assets::savedWadPath(prefDir_); wadStatus_.clear(); SDL_StartTextInput(window_); }
    if (screen == kScreenEmailEntry) { emailEntry_ = stats_.email(); wadStatus_.clear(); SDL_StartTextInput(window_); }
    if (screen == kScreenWadSetup) wadStatus_.clear();
}

void App::closeScreen() {
    if (screen_ == kScreenNameEntry || screen_ == kScreenWadPath || screen_ == kScreenEmailEntry) SDL_StopTextInput(window_);
    if (nameRequired_ && profileName_.empty()) { openScreen(kScreenNameEntry); return; }   // no dodging the name
    screen_ = kScreenNone;
}

void App::adjustOption(int dir) {
    switch (screenIndex_) {
    case 0: {   // music set
        for (int i = 0; i < 3; ++i) { cycleMusicSet(); if (dir > 0) break; }   // cycling backwards = two forward steps
        break;
    }
    case 1: music_.setVolume(std::clamp(music_.volume() + 0.05f * static_cast<float>(dir), 0.f, 1.f)); break;
    case 2: music_.setEnabled(!music_.enabled()); break;
    case 3: padSens_ = std::clamp(padSens_ + 0.1f * static_cast<float>(dir), 0.3f, 3.f); break;
    case 4: padInvertY_ = !padInvertY_; break;
    case 5: padRumble_ = !padRumble_; if (padRumble_) rumble(0.5f, 0.5f, 150); break;
    case 6: displayMode_ = (displayMode_ + 3 + dir) % 3; applyDisplay(); break;
    case 8: if (voxels_.available()) useVoxels_ = !useVoxels_; break;
    case 9: if (dir > 0) browseForWad(); break;
    case 10: if (dir > 0) browseForWad(true); break;
    case 11: if (dir > 0) openScreen(kScreenEmailEntry); break;
    case 12: setDoomArt(doomArtOff_); break;   // toggles
    case 13: if (renderer_->rayTracingAvailable()) { rtShadows_ = (rtShadows_ + 3 + dir) % 3; saveDisplaySettings(); } break;
    case 7: {
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
        const int n = 15;   // 14 options + BACK
        if (key == SDLK_UP) { screenIndex_ = (screenIndex_ + n - 1) % n; play("menu", 0.6f); }
        else if (key == SDLK_DOWN) { screenIndex_ = (screenIndex_ + 1) % n; play("menu", 0.6f); }
        else if (key == SDLK_LEFT) { if (screenIndex_ < 14) adjustOption(-1); }
        else if (key == SDLK_RIGHT) { if (screenIndex_ < 14) adjustOption(1); }
        else if (key == SDLK_RETURN || key == SDLK_SPACE) { if (screenIndex_ == 14) closeScreen(); else adjustOption(1); }
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
        }
        else if (key == SDLK_BACKSPACE) { if (!emailEntry_.empty()) emailEntry_.pop_back(); }
        else if (key == SDLK_ESCAPE) closeScreen();
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
        if (key == SDLK_UP) { screenIndex_ = (screenIndex_ + n - 1) % n; play("menu", 0.6f); }
        else if (key == SDLK_DOWN) { screenIndex_ = (screenIndex_ + 1) % n; play("menu", 0.6f); }
        else if (key == SDLK_RETURN || key == SDLK_SPACE) {
            play("menu_select", 0.7f);
            if (screenIndex_ < static_cast<int>(profileList_.size())) { switchProfile(profileList_[static_cast<size_t>(screenIndex_)]); nameRequired_ = false; closeScreen(); }
            else openScreen(kScreenNameEntry);
        }
        else if (key == SDLK_ESCAPE || key == SDLK_BACKSPACE) closeScreen();
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
            else if (key == SDLK_ESCAPE) { if (!nameRequired_ || !profileName_.empty()) { SDL_StopTextInput(window_); screen_ = kScreenNone; } return; }
        }
        if (key == SDLK_KP_ENTER || (!fromPad && key == SDLK_RETURN)) {
            std::string name = nameEntry_;
            while (!name.empty() && name.back() == ' ') name.pop_back();
            if (name.empty()) return;
            for (char& c : name) if (c == ' ') c = '_';
            SDL_StopTextInput(window_);
            screen_ = kScreenNone;
            nameRequired_ = false;
            switchProfile(name);
            play("menu_select", 0.8f);
            announce("WELCOME, " + name, glm::vec4(0.8f, 0.9f, 1.f, 1.f), 1.2f);
            if (mode_ == Mode::Title) menu_.items[1] = "PLAYER: " + name;
            openScreen(kScreenEmailEntry);   // optional; groundwork for the online leaderboard
        } else if (!fromPad && key == SDLK_BACKSPACE) { if (!nameEntry_.empty()) nameEntry_.pop_back(); }
        else if (!fromPad && key == SDLK_ESCAPE) { if (!nameRequired_ || !profileName_.empty()) { SDL_StopTextInput(window_); screen_ = kScreenNone; } }
        break;
    }
    default:
        break;
    }
}

void App::beginLevelCard() {
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
            else if (k == "rt_shadows") rtShadows_ = std::clamp(std::atoi(v.c_str()), 0, 2);
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
        std::fprintf(f, "mode=%d\nwidth=%d\nheight=%d\nrt_shadows=%d\n", displayMode_, resW_, resH_, rtShadows_);
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
        SDL_SetWindowSize(window_, resW_, resH_);
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
    std::fprintf(stderr, "[display] %s %dx%d\n", displayModeName(), resW_, resH_);
    saveDisplaySettings();
}

void App::saveSettings() const {
    if (std::FILE* f = std::fopen(settingsPath_.c_str(), "w")) {
        std::fprintf(f, "music_set=%s\nmusic_on=%d\nmusic_volume=%.2f\npad_sens=%.2f\npad_invert=%d\npad_rumble=%d\nvoxels=%d\ndoom_art=%d\n",
                     musicSet_ == MusicSet::Classic ? "classic" : musicSet_ == MusicSet::Sc55 ? "sc55" : "modern", music_.enabled() ? 1 : 0,
                     music_.volume(), padSens_, padInvertY_ ? 1 : 0, padRumble_ ? 1 : 0, useVoxels_ ? 1 : 0, doomArtOff_ ? 0 : 1);
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
                else if (ch == '~') k = SDLK_RETURN; else if (ch == '`') k = SDLK_ESCAPE; else if (ch == ' ') k = SDLK_SPACE;
                if (k == SDLK_UNKNOWN) continue;
                ev.key.key = k;
                SDL_PushEvent(&ev);
            }
        }
        for (const Options::Click& c : opts_.clicks) {
            if (c.frame != frameCount_) continue;
            SDL_Event mv{}; mv.type = SDL_EVENT_MOUSE_MOTION; mv.motion.x = static_cast<float>(c.x); mv.motion.y = static_cast<float>(c.y); SDL_PushEvent(&mv);
            SDL_Event dn{}; dn.type = SDL_EVENT_MOUSE_BUTTON_DOWN; dn.button.button = SDL_BUTTON_LEFT; dn.button.x = static_cast<float>(c.x); dn.button.y = static_cast<float>(c.y); SDL_PushEvent(&dn);
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
        renderer_->render(frame_, cubes_, worldQuads_, screenQuads_, meshes_);
        ++frameCount_;

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
    else if (item == "CREDITS") openScreen(kScreenCredits);
    else if (item.rfind("PLAYER: ", 0) == 0) openScreen(kScreenProfiles);
    else if (item == "START") { if (profileName_.empty()) openScreen(kScreenNameEntry); else enterMode(Mode::Blocks); }
    else if (item == "RESUME") enterMode(pausedFrom_);
    else if (item == "RESTART") { newGame(); enterMode(Mode::Blocks); }
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
                if (!opts_.clicks.empty()) std::fprintf(stderr, "[ui] click %.0f,%.0f -> %s (hotspots %zu)\n", e.button.x, e.button.y, h ? (std::to_string(h->kind) + "/" + std::to_string(h->index)).c_str() : "none", hotspots_.size());
                if (h && h->kind == kHotMenu && left) { menu_.index = h->index; play("menu_select", 0.7f); menuSelect(); }
                else if (h && h->kind == kHotScreenItem && left) { screenIndex_ = h->index; screenKey(SDLK_RETURN, false); }
                else if (h && h->kind == kHotOptionRow) { screenIndex_ = h->index; adjustOption(left ? 1 : -1); }   // right-click steps back
                else if (h && h->kind == kHotBack && left) { play("menu", 0.6f); screenKey(SDLK_ESCAPE, false); }
                else if (!h && left && mode_ == Mode::GameOver && gameOverT_ > 1.2f && screen_ == kScreenNone) { menu_.index = 0; play("menu_select", 0.7f); menuSelect(); }   // "press any key"
            }
            break;
        case SDL_EVENT_MOUSE_BUTTON_UP:
            if (e.button.button == SDL_BUTTON_LEFT) fpsIn_.fire = false;
            break;
        case SDL_EVENT_MOUSE_WHEEL:
            if (mode_ == Mode::Fps) fpsIn_.wheel += (e.wheel.y > 0.f) ? 1 : (e.wheel.y < 0.f ? -1 : 0);
            break;
        case SDL_EVENT_KEY_DOWN: {
            SDL_Keycode k = e.key.key;
            if (!e.key.repeat) stats_.addInput(InputDevice::Keyboard);
            if (k == SDLK_F12) {
                renderer_->screenshot("redline-screenshot.png");
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
        case core::EventType::PieceLocked: play("lock", 0.8f); break;
        case core::EventType::HardDrop: shakeT_ = 0.15f; break;
        case core::EventType::LinesCleared: {
            play("clear", 0.9f, ev.a >= 4 ? 1.3f : 1.f);
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
        case core::EventType::CellTurnedRed: play("lock", 0.9f, 0.55f, 120); shakeT_ = std::max(shakeT_, 0.1f); break;
        case core::EventType::EvilSpawning: play("redline", 0.5f, 0.7f, 250); break;
        case core::EventType::EvilSpawned: play("explode", 0.5f, 1.4f); shakeT_ = std::max(shakeT_, 0.15f); announce("EVIL SPAWNED", glm::vec4(1.f, 0.3f, 0.2f, 1.f), 1.f); break;
        default: break;
        }
    }
}

void App::handleFpsEvents() {
    for (const FpsEvent& ev : fps_.drainEvents()) {
        const EnemyArt& art = assets_.enemies[std::clamp(ev.tier, 0, kEnemyTiers - 1)];
        switch (ev.type) {
        case FpsEvent::Type::Shoot:
            play(assets_.weapons[std::clamp(ev.a, 0, kWeaponArt - 1)].fireSound, 1.f); muzzleLight_ = 1.f;
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
            play("rocket_hit", 1.f); shakeT_ = 0.35f; rumble(0.8f, 0.6f, 300);
            fightStats_.blocks += ev.a; gameBlocks_ += ev.a;
            if (gameBlocks_ >= 50) trophy("demolition");
            break;
        case FpsEvent::Type::BlockBroken: play("lock", 0.7f, 0.8f, 100); shakeT_ = std::max(shakeT_, 0.08f); break;
        case FpsEvent::Type::Score:
            announce(art.name + "  +" + std::to_string(ev.a), ev.tier >= 4 ? glm::vec4(1.f, 0.9f, 0.3f, 1.f) : glm::vec4(1.f), ev.tier >= 4 ? 1.5f : 1.1f);
            ++fightStats_.kills;
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
        case FpsEvent::Type::EnemyHit: play(art.painSound, 0.8f, 1.f, 120); break;
        case FpsEvent::Type::EnemyDied: play(art.deathSound, 1.f); break;
        case FpsEvent::Type::EnemyAttack: play(art.attackSound, 0.7f); break;
        case FpsEvent::Type::Explosion:
            play("explode", 1.f); shakeT_ = 0.3f + 0.1f * ev.tier; rumble(0.7f, 0.5f, 250);
            fightStats_.blocks += ev.a; gameBlocks_ += ev.a;
            if (gameBlocks_ >= 50) trophy("demolition");
            break;
        case FpsEvent::Type::PlayerHit: play("pain", 1.f); shakeT_ = 0.25f; rumble(0.6f, 0.3f, 200); break;
        case FpsEvent::Type::FireballHit: play("fireball_hit", 0.5f, 1.f, 80); break;
        case FpsEvent::Type::AllClear: play("levelup", 1.f); break;
        case FpsEvent::Type::PlayerDead: diedInFps_ = true; break;
        case FpsEvent::Type::EnemySight: {
            // Announce the biggest monster in the room and log the roster.
            play(art.sightSound, 0.9f);
            std::string roster;
            for (const Enemy& en : fps_.enemies()) roster += (roster.empty() ? "" : ", ") + assets_.enemies[std::clamp(en.tier, 0, kEnemyTiers - 1)].name + "(" + std::to_string(en.cells.size()) + ")";
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
    pollGamepad(dt);
    if (levelCardT_ < kLevelCardTime) levelCardT_ += dt;

    if (mode_ == Mode::Title || mode_ == Mode::Blocks || mode_ == Mode::Alert || (mode_ == Mode::GameOver && !diedInFps_)) {
        ambient_.update(dt);
        ambient_.drainEvents();   // silent scenery: the brawl makes no sound over the Tetris game
    }
    switch (mode_) {
    case Mode::Title:
        break;
    case Mode::Blocks:
        updateBlocksInput(dt);
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
            const Enemy* target = nullptr;
            float best = 1e9f;
            for (const Enemy& en : fps_.enemies()) {
                if (!en.alive()) continue;
                float d = glm::length(en.pos - fps_.eye());
                if (d < best) { best = d; target = &en; }
            }
            if (target) {
                glm::vec3 chest = target->pos + glm::vec3(0.f, target->height * 0.7f, 0.f);
                glm::vec3 to = chest - fps_.eye();
                float wantYaw = std::atan2(to.x, to.z);
                float wantPitch = std::atan2(to.y, std::sqrt(to.x * to.x + to.z * to.z));
                float dyaw = std::remainder(wantYaw - fps_.yaw(), 2.f * kPi);
                in.lookDX = -dyaw / 0.0022f;
                in.lookDY = -(wantPitch - fps_.pitch()) / 0.0022f;
                float len = glm::length(to);
                bool clear = fps_.rayBlockDistance(fps_.eye(), to / len, len, *game_) >= len - 0.01f;
                in.fire = std::fabs(dyaw) < 0.03f && clear;
                // Best owned weapon with ammo: rockets at range, plasma, chaingun, shotgun.
                auto usable = [&](int id) { return fps_.weapon(id).owned && (weaponDef(id).ammoPerPickup == 0 || fps_.weapon(id).ammo > 0); };
                int want = kShotgun;
                if (usable(kChaingun)) want = kChaingun;
                if (usable(kPlasmaRifle)) want = kPlasmaRifle;
                if (usable(kRocketLauncher) && len > 4.5f) want = kRocketLauncher;
                if (want != fps_.currentWeapon()) in.selectWeapon = want;
                if (clear) {
                    botBlockedT_ = 0.f;
                    in.moveZ = len > 7.f ? 1.f : 0.f;
                    in.run = len > 7.f;
                    in.moveX = std::sin(time_ * 1.7f) * 0.6f;
                } else {
                    // Shot blocked by a block: sidestep, flipping direction every so often.
                    botBlockedT_ += dt;
                    if (botBlockedT_ > 1.5f) { botSide_ = -botSide_; botBlockedT_ = 0.f; }
                    in.moveX = botSide_;
                    in.moveZ = 0.4f;
                }
            }
        }
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
    c.target = c.eye + fps_.forward();
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
    c.params = glm::vec4(0.7f, 0.05f, phase, flags);
    c.rot = glm::vec4(rotX, 0.f, 0.f, 0.f);
    cubes_.push_back(c);
}

void App::buildEnvironment() {
    envCubes_.clear();
    auto push = [&](glm::vec3 pos, const std::string& tex, glm::vec4 color) {
        const render::AtlasRegion& r = assets_.region(tex);
        render::CubeInstance c;
        c.posScale = glm::vec4(pos, 1.f);
        c.color = color;
        c.emissive = glm::vec4(0.f);
        c.uvRect = glm::vec4(r.u0, r.v0, r.u1, r.v1);
        c.params = glm::vec4(0.9f, 0.f, 0.f, 0.f);
        c.rot = glm::vec4(0.f);
        envCubes_.push_back(c);
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
    for (int y = 0; y < height; ++y) {
        for (int x = -halfW; x < halfW; ++x) {
            push({x + 0.5f, y + 0.5f, -2.5f}, assets_.wall, glm::vec4(0.85f, 0.85f, 0.85f, 1.f));
            push({x + 0.5f, y + 0.5f, depth + 0.5f}, assets_.wall, glm::vec4(0.7f, 0.7f, 0.7f, 1.f));
        }
        for (int z = -2; z < depth; ++z) {
            push({-halfW - 0.5f, y + 0.5f, z + 0.5f}, assets_.wall, glm::vec4(0.75f, 0.75f, 0.75f, 1.f));
            push({halfW + 0.5f, y + 0.5f, z + 0.5f}, assets_.wall, glm::vec4(0.75f, 0.75f, 0.75f, 1.f));
        }
    }
}

// Torches, lamps and barrels around the arena, each with its own flickering light.
void App::buildProps() {
    props_.clear();
    auto prop = [&](const SpriteAnim& a, glm::vec3 pos, float px, glm::vec3 lc, float lr, float lh) {
        if (a.empty()) return;
        props_.push_back({&a, pos, px, lc, lr, lh, static_cast<float>(props_.size()) * 1.7f});
    };
    const glm::vec3 red(1.f, 0.42f, 0.12f), blue(0.35f, 0.5f, 1.f), green(0.4f, 1.f, 0.45f), warm(1.f, 0.75f, 0.45f), white(0.9f, 0.9f, 1.f);
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
        if (!key.empty()) actor(key, p.pos, p.px, glm::vec4(1.f), true, flip, 0.f);
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
        if (!key.empty()) actor(key, p.pos - glm::vec3(0.f, 0.3f, 0.f), 0.031f, glm::vec4(1.f), false, flip, std::atan2(p.vel.x, p.vel.z), glm::vec3(1.f, 0.8f, 0.5f), 0.4f);
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
        const EnemyArt& art = assets_.enemies[std::clamp(e.tier, 0, kEnemyTiers - 1)];
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
        const EnemyArt& art = assets_.enemies[std::clamp(e.tier, 0, kEnemyTiers - 1)];
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
        // Corpses fade out; cacodemons (a big sprite lying in the way) go quickest.
        float corpseScale = 1.f;
        if (e.state == Enemy::State::Dead) {
            float life = (e.tier == 3) ? 0.35f : (e.tier >= 5 ? 1.2f : 1.6f);
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
        glm::vec3 toPlayer = fps_.eye() - e.pos;
        float yaw = (std::fabs(toPlayer.x) + std::fabs(toPlayer.z) > 1e-3f) ? std::atan2(toPlayer.x, toPlayer.z) : 0.f;
        if (!key.empty()) actor(key, e.pos, art.metresPerPixel * corpseScale, tint, true, flip, yaw);
    }
    for (const Projectile& p : fps_.projectiles()) {
        bool flip = false;
        const SpriteAnim& anim = assets_.projectile[std::clamp(p.type, 0, kProjectileTypes - 1)];
        const std::string& key = animFrame(anim, p.animT, true, &flip);
        if (!key.empty()) actor(key, p.pos - glm::vec3(0.f, 0.3f, 0.f), 0.031f, glm::vec4(1.f), false, flip, std::atan2(p.vel.x, p.vel.z), glm::vec3(1.f, 0.8f, 0.5f), 0.4f);
    }
    for (const Pickup& p : fps_.pickups()) {
        const SpriteAnim& anim = assets_.pickups[std::clamp(static_cast<int>(p.kind), 0, kPickupArt - 1)];
        if (anim.empty()) continue;
        float bob = p.landed ? 0.06f + 0.05f * std::sin(time_ * 4.f + p.pos.x) : 0.f;
        actor(anim.frames[0], p.pos + glm::vec3(0.f, bob, 0.f), 0.031f, glm::vec4(1.f), true, false, time_ * 1.2f + p.pos.x);
    }
    for (const Explosion& ex : fps_.explosions()) {
        float t = ex.t / ex.duration;
        const SpriteAnim& anim = ex.hitType < 0 ? assets_.explosion : assets_.projectileHit[std::clamp(ex.hitType, 0, kProjectileTypes - 1)];
        int n = static_cast<int>(anim.frames.size());
        if (n == 0) continue;
        int i = std::clamp(static_cast<int>(t * n), 0, n - 1);
        float scale = ex.hitType < 0 ? 0.031f * (1.6f + 0.4f * ex.radius) : 0.031f * 1.2f;
        actor(anim.frames[static_cast<size_t>(i)], ex.pos - glm::vec3(0.f, ex.hitType < 0 ? 0.9f : 0.2f, 0.f), scale, glm::vec4(1.f), false, anim.mirrored[static_cast<size_t>(i)], 0.f, glm::vec3(1.f, 0.7f, 0.4f), 0.6f);
    }
    for (const Debris& d : fps_.debris()) {
        float fade = std::min(1.f, d.ttl / 0.4f);
        if (d.red) cube(d.pos, d.size, glm::vec4(d.color, 1.f), assets_.redBlock, glm::vec3(1.f, 0.1f, 0.05f), 0.6f * fade);
        else cube(d.pos, d.size, glm::vec4(d.color * fade, 1.f), assets_.block);
    }
}

void App::addLights() {
    frame_.lights.clear();
    glm::vec3 cam = frame_.cameraPos;
    struct Cand { float score; render::PointLight l; };
    std::vector<Cand> cands;
    // Decor lights with a torch flicker.
    for (const Prop& p : props_) {
        if (p.lightRadius <= 0.f) continue;
        float f = 0.82f + 0.12f * std::sin(time_ * 9.f + p.phase) + 0.06f * std::sin(time_ * 23.f + p.phase * 3.f);
        // With ray-traced shadows the torches can burn brighter: nothing bleeds through walls any more.
        float boost = (renderer_->rayTracingAvailable() && rtShadows_ == 2) ? 1.5f : 1.f;
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
        cands.push_back({glm::length(p.pos - cam), {p.pos, 3.f, {1.f, 0.5f, 0.1f}, 1.f}});
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
            glm::vec3 col = p.type == kProjBaron ? glm::vec3(0.3f, 1.f, 0.3f) : p.type == kProjPlasma ? glm::vec3(0.4f, 0.6f, 1.f) : glm::vec3(1.f, 0.5f, 0.1f);
            cands.push_back({-50.f, {p.pos, p.type == kProjRocket ? 4.f : 3.f, col, 1.2f}});
        }
        for (const Pickup& p : fps_.pickups())
            if (p.landed) cands.push_back({glm::length(p.pos - cam), {p.pos + glm::vec3(0.f, 0.4f, 0.f), 1.5f, {0.6f, 0.8f, 1.f}, 0.5f}});
        for (const Enemy& e : fps_.enemies())
            if (e.growT > 0.f) cands.push_back({-120.f, {e.pos + glm::vec3(0.f, 1.f, 0.f), 6.f, {1.f, 0.3f, 0.3f}, 4.f * e.growT}});
        if (muzzleLight_ > 0.f && mode_ == Mode::Fps) {
            glm::vec3 mc = fps_.currentWeapon() == kPlasmaRifle ? glm::vec3(0.4f, 0.6f, 1.f) : glm::vec3(1.f, 0.8f, 0.4f);
            cands.push_back({-200.f, {fps_.eye() + fps_.forward() * 1.2f, 7.f, mc, 3.f * muzzleLight_}});
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
    q.params = glm::vec4(1.f, 0.f, flip ? 1.f : 0.f, 0.f);
    screenQuads_.push_back(q);
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
    q.params = glm::vec4(1.f, 0.f, 0.f, 0.f);
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
        screenSprite(it->second, cx, gy, scale, color, 0.f, 1.f);
        cx += (r.w + 1) * scale;
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

    if (!inFps) {
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
        const char* banner = game_->panic() ? "OVERRUN  -  EVIL SURGES" : (d < 0.5f ? "THE STACK IS TURNING EVIL" : "EVIL RISING");
        text(W * 0.5f, H * 0.2f, banner, s * (game_->panic() ? 1.2f : 0.9f), glm::vec4(1.f, 0.3f + 0.4f * f, 0.2f, 0.4f + 0.6f * d), 1);
    }
    if (mode_ == Mode::Alert) {
        float f = 0.5f + 0.5f * std::sin(modeT_ * 16.f);
        text(W * 0.5f, H * 0.42f, "RED LINE", s * 2.2f, glm::vec4(1.f, 0.2f * f, 0.1f * f, 1.f), 1);
        text(W * 0.5f, H * 0.42f + lh * 2.4f, "THE BOARD IS FALLING", s, white, 1);
    }
    if (inFps) {
        if (mode_ == Mode::Fps || mode_ == Mode::Countdown) {
            const WeaponArt& wa = assets_.weapons[std::clamp(fps_.currentWeapon(), 0, kWeaponArt - 1)];
            bool flip = false;
            const std::string& gun = fps_.gunFiring() ? animFrame(wa.fire, fps_.gunAnimT(), false, &flip) : animFrame(wa.idle, 0.f, true, &flip);
            float gs = H / 200.f;
            bool moving = fpsIn_.fwd || fpsIn_.back || fpsIn_.left || fpsIn_.right;
            float bob = std::sin(time_ * 6.f) * 3.f * gs * (moving ? 1.f : 0.15f);
            float recoil = fps_.recoil() * 18.f * gs;
            if (!gun.empty()) {
                // Doom draws weapon sprites with their own patch origin at (161, 32) of a 320x200 screen,
                // and the muzzle flash with the same origin, so the flash lands on the barrel by itself.
                auto originSprite = [&](const std::string& key, float dy, glm::vec4 tint, bool fl) {
                    const render::AtlasRegion& r = assets_.region(key);
                    float ax = r.w > 0 ? static_cast<float>(r.offsetX) / r.w : 0.5f;
                    float ay = r.h > 0 ? 1.f - static_cast<float>(r.offsetY) / r.h : 1.f;
                    // Doom: left edge = 1 - leftoffset in 320-wide space, so the origin sits 1px right of the frame's left edge.
                    screenSprite(key, (W - 320.f * gs) * 0.5f + 1.f * gs, 32.f * gs + dy, gs, tint, ax, ay, fl);
                };
                float dy = recoil + std::fabs(bob) + (H - 200.f * gs) * 0.5f;   // centre the 320x200 frame vertically... anchored to the bottom
                dy = recoil + std::fabs(bob) + (H - 200.f * gs);                 // keep the weapon at the bottom edge
                if (assets_.usingWad()) {
                    originSprite(gun, dy, wa.tint, flip);
                    if (fps_.gunFiring() && fps_.gunAnimT() < (fps_.currentWeapon() == kPlasmaRifle ? 0.2f : 0.12f) && !wa.flash.empty())
                        originSprite(animFrame(wa.flash, fps_.gunAnimT(), false), dy, glm::vec4(1.f), false);
                } else {
                    const render::AtlasRegion& r = assets_.region(gun);
                    screenSprite(gun, W * 0.5f, H + recoil + std::fabs(bob) - 2.f * gs, gs, wa.tint, 0.5f, 0.f, flip);
                    if (fps_.gunFiring() && fps_.gunAnimT() < 0.12f && !wa.flash.empty()) {
                        const std::string& fl = animFrame(wa.flash, fps_.gunAnimT(), false);
                        const render::AtlasRegion& fr = assets_.region(fl);
                        screenSprite(fl, W * 0.5f, H + recoil - (r.h - fr.h) * gs - 6.f * gs, gs, glm::vec4(1.f), 0.5f, 0.f);
                    }
                }
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

    // Announcements: rise and fade from just above the centre.
    {
        float y = H * 0.24f;
        for (Announcement& a : announcements_) {
            float alpha = a.t < 1.2f ? 1.f : std::max(0.f, 1.f - (a.t - 1.2f) / 0.8f);
            float rise = 30.f * s * std::min(1.f, a.t / 2.f);
            glm::vec4 col = a.color;
            col.a *= alpha;
            text(W * 0.5f, y - rise, a.text, s * a.scale, col, 1);
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
        for (size_t i = 0; i < menu_.items.size(); ++i) {
            bool sel = static_cast<int>(i) == menu_.index;
            float f = sel ? (0.7f + 0.3f * std::sin(time_ * 6.f)) : 1.f;
            std::string label = sel ? ("> " + menu_.items[i] + " <") : menu_.items[i];
            hotText(W * 0.5f, ty + static_cast<float>(i) * lh * 1.25f, label, s * 1.1f, sel ? glm::vec4(1.f, 0.9f * f, 0.3f * f, 1.f) : dim, 1, kHotMenu, static_cast<int>(i));
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
        auto row = [&](float y, const std::string& label, const std::string& value, bool sel) {
            glm::vec4 c = sel ? yellow : dim;
            text(W * 0.5f - 20.f, y, (sel ? "> " : "") + label, s, c, 2);
            text(W * 0.5f + 20.f, y, value, s, sel ? white : dim, 0);
            float lw = static_cast<float>(assets_.textWidth(label, s)), vw = static_cast<float>(assets_.textWidth(value, s));
            hotRect(W * 0.5f - 20.f - lw - 30.f, y - 4.f, lw + 40.f + vw + 30.f, static_cast<float>(assets_.fontHeight) * s + 8.f, kHotOptionRow, rowIndex++);
        };
        if (screen_ == kScreenOptions) {
            text(W * 0.5f, H * 0.07f, "OPTIONS", s * 1.8f, white, 1);
            float y = H * 0.17f;
            char buf[80];
            row(y, "MUSIC", musicSetName(), screenIndex_ == 0); y += lh * 1.25f;
            std::snprintf(buf, sizeof buf, "%d%%", static_cast<int>(std::lround(music_.volume() * 100.f)));
            row(y, "MUSIC VOLUME", buf, screenIndex_ == 1); y += lh * 1.25f;
            row(y, "MUSIC ENABLED", music_.enabled() ? "ON" : "OFF", screenIndex_ == 2); y += lh * 1.25f;
            std::snprintf(buf, sizeof buf, "%.1f", padSens_);
            row(y, "STICK SENSITIVITY", buf, screenIndex_ == 3); y += lh * 1.25f;
            row(y, "INVERT LOOK", padInvertY_ ? "ON" : "OFF", screenIndex_ == 4); y += lh * 1.25f;
            row(y, "RUMBLE", padRumble_ ? "ON" : "OFF", screenIndex_ == 5); y += lh * 1.25f;
            row(y, "DISPLAY", displayModeName(), screenIndex_ == 6); y += lh * 1.25f;
            std::snprintf(buf, sizeof buf, "%d X %d%s", resW_, resH_, displayMode_ == 1 ? "  (DESKTOP SIZE IN BORDERLESS)" : "");
            row(y, "RESOLUTION", buf, screenIndex_ == 7); y += lh * 1.25f;
            row(y, "MODELS", voxels_.available() ? (useVoxels_ ? "VOXELS (VOXEL DOOM)" : "SPRITES") : "SPRITES (NO VOXEL PACK FOUND)", screenIndex_ == 8); y += lh * 1.25f;
            row(y, "DOOM WAD", assets_.usingWad() ? assets_.wadName() + "  (ENTER TO CHANGE)" : "NONE - PLACEHOLDER ART  (ENTER)", screenIndex_ == 9); y += lh * 1.25f;
            row(y, "SOUNDTRACK WAD", !assets_.oggMusic.empty() ? std::filesystem::path(assets_.extrasPath).filename().string() + "  (" + std::to_string(assets_.oggMusic.size()) + " TRACKS)" : "NONE - CLASSIC ONLY  (ENTER)", screenIndex_ == 10); y += lh * 1.25f;
            row(y, "PLAYER EMAIL", stats_.email().empty() ? "NOT SET  (OPTIONAL, ENTER)" : stats_.email() + (stats_.emailVerified() ? "  (VERIFIED)" : "  (NOT VERIFIED YET)"), screenIndex_ == 11); y += lh * 1.25f;
            row(y, "DOOM ART", doomArtOff_ ? "OFF  (PLACEHOLDER LOOK)" : "ON", screenIndex_ == 12); y += lh * 1.25f;
            row(y, "SHADOWS", !renderer_->rayTracingAvailable() ? "SHADOW MAP  (NO RAY TRACING ON THIS GPU)" : rtShadows_ == 0 ? "SHADOW MAP" : rtShadows_ == 1 ? "RAY TRACED: SUN" : "RAY TRACED: SUN + ALL LIGHTS", screenIndex_ == 13); y += lh * 1.6f;
            hotText(W * 0.5f, y, screenIndex_ == 14 ? "> BACK <" : "BACK", s, screenIndex_ == 14 ? yellow : dim, 1, kHotBack, 0);
            if (!wadStatus_.empty()) text(W * 0.5f, y + lh * 1.2f, wadStatus_, s * 0.75f, glm::vec4(1.f, 0.8f, 0.4f, 1.f), 1);
            text(W * 0.5f, H - lh * 2.f, "LEFT/RIGHT CHANGE   ESC OR B BACK   ALT+ENTER TOGGLES FULLSCREEN", s * 0.7f, dim, 1);
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
            text(W * 0.5f, H * 0.16f, "PLAYERS", s * 2.f, white, 1);
            float y = H * 0.30f;
            for (size_t i = 0; i < profileList_.size(); ++i) {
                bool sel = static_cast<int>(i) == screenIndex_;
                hotText(W * 0.5f, y, (sel ? "> " : "") + profileList_[i] + (profileList_[i] == profileName_ ? "  (CURRENT)" : "") + (sel ? " <" : ""), s, sel ? yellow : dim, 1, kHotScreenItem, static_cast<int>(i));
                y += lh * 1.4f;
            }
            bool selNew = screenIndex_ == static_cast<int>(profileList_.size());
            hotText(W * 0.5f, y, selNew ? "> NEW PLAYER <" : "NEW PLAYER", s, selNew ? yellow : dim, 1, kHotScreenItem, static_cast<int>(profileList_.size()));
            hotText(W * 0.5f, y + lh * 1.6f, "BACK", s, dim, 1, kHotBack, 0);
            text(W * 0.5f, H - lh * 2.f, "EACH PLAYER KEEPS THEIR OWN SCORES, TROPHIES AND SETTINGS", s * 0.7f, dim, 1);
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
            line("THIS IS FOR THE ONLINE LEADERBOARD THAT IS COMING: IT WILL LET YOUR SCORES,", 0.7f, dim);
            line("TROPHIES AND PLAY TIME FOLLOW YOU BETWEEN MACHINES ONCE THE ADDRESS IS VERIFIED.", 0.7f, dim);
            line("IT IS KEPT ON THIS MACHINE ONLY UNTIL THEN, AND IT IS NEVER SHOWN TO OTHER PLAYERS.", 0.7f, dim);
            y += lh * 0.6f;
            hotText(W * 0.5f - 40.f, y, "SAVE", 0.95f * s, yellow, 2, kHotScreenItem, 0);
            hotText(W * 0.5f + 40.f, y, "CANCEL", 0.95f * s, dim, 0, kHotBack, 0);
            y += lh * 1.1f;
            line("ENTER SAVES   LEAVE EMPTY TO SKIP   ESC CANCELS", 0.7f, dim);
            if (!wadStatus_.empty()) text(W * 0.5f, y + lh * 0.5f, wadStatus_, s * 0.9f, glm::vec4(1.f, 0.5f, 0.3f, 1.f), 1);
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
            text(W * 0.5f, H * 0.22f, "WHAT IS YOUR NAME, MARINE?", s * 1.4f, white, 1);
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
        drawMenu(H * 0.3f + lh * 6.8f);
        if (!highScores_.entries().empty()) {
            float ty = H * 0.3f + lh * 6.8f + lh * 1.5f * 2.f + lh * 0.6f;
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
    frame_.shadowStrength = 0.85f;
    frame_.rtShadows = renderer_->rayTracingAvailable() ? rtShadows_ : 0;
    float tilt = boardTilt();
    frame_.sunIntensity = glm::mix(0.9f, 0.55f, tilt);
    frame_.ambient = glm::mix(glm::vec3(0.30f, 0.30f, 0.34f), glm::vec3(0.21f, 0.17f, 0.17f), tilt);   // the fight stays moody but readable
    frame_.fogDensity = glm::mix(0.012f, 0.035f, tilt);
    frame_.fogColor = glm::mix(glm::vec3(0.03f, 0.03f, 0.05f), glm::vec3(0.06f, 0.02f, 0.02f), tilt);
    frame_.clearColor = frame_.fogColor;
    if (mode_ == Mode::Alert) {
        float f = 0.5f + 0.5f * std::sin(modeT_ * 18.f);
        frame_.ambient = glm::mix(frame_.ambient, glm::vec3(0.45f, 0.08f, 0.05f), f * 0.7f);
    }

    addBoard();
    addDecor();
    if (!ambient_.empty() && (mode_ == Mode::Title || mode_ == Mode::Blocks || mode_ == Mode::Alert || (mode_ == Mode::Paused && pausedFrom_ == Mode::Blocks) || (mode_ == Mode::GameOver && !diedInFps_))) addAmbient();
    bool actors = (mode_ == Mode::Fps || mode_ == Mode::Countdown || mode_ == Mode::FlyOut || (mode_ == Mode::GameOver && diedInFps_) || (mode_ == Mode::Paused && (pausedFrom_ == Mode::Fps || pausedFrom_ == Mode::Countdown)));
    if (actors) addFpsActors();
    addLights();
    addHud();
}

}  // namespace rl::game
