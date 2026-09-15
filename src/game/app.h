#pragma once
// Top-level game: window, modes, scene assembly, menu and HUD.
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "audio/audio.h"
#include "audio/music.h"
#include "game/highscores.h"
#include "game/trophies.h"
#include "core/tetris.h"
#include "game/assets.h"
#include "game/fps_mode.h"
#include "game/ambient.h"
#include "game/voxels.h"
#include "render/renderer.h"
#include "render/vk_context.h"

struct SDL_Window;
struct SDL_Gamepad;

namespace rl::game {

struct Options {
    std::optional<std::filesystem::path> wad;
    std::string extras;                // rerelease extras.wad (soundtracks); empty = auto
    bool noWad = false;
    int width = 1600, height = 900;
    bool fullscreen = false;
    bool preferIntegrated = false;
    uint32_t seed = 0;                 // 0 = time-based
    std::string scenario = "title";    // title | blocks | redline | fps
    std::string screenshot;            // path; taken after `frames` frames, then exit
    int frames = 0;
    bool bot = false;                  // auto-aim/fire in FPS mode (smoke testing)
    bool mute = false;
    int level = 1;                     // starting level (scenarios)
    float absorbPeriod = 20.f;         // seconds before a monster may absorb blocks and grow
    bool god = false;                  // no damage to the player (testing)
    int arsenal = -1;                  // testing: start the fight holding this weapon with everything owned
    std::string keys;                  // letters to hold from frame keysFrame (testing), e.g. "BFG"
    int keysFrame = 30;
    int keysHoldFrames = 150;          // released this many frames later
    int stackRows = 0;                 // pre-fill this many holey rows of normal blocks (blocks scenario)
    bool noMusic = false;
    float musicVolume = 0.45f;
    std::string musicSet;              // "classic" | "sc55" | "modern" (empty = saved preference, default modern when available)
    std::string profile;               // player profile to use (skips the name prompt)
    std::string reloadWad;             // testing: swap to this WAD after 30 frames
    std::optional<std::filesystem::path> voxelDir;   // KVX pack directory (default: auto-detect)
    int voxels = -1;                   // -1 saved preference, 0 sprites, 1 voxel models
};

class App {
public:
    explicit App(Options opts);
    ~App();
    int run();

private:
    enum class Mode { Title, Blocks, Alert, FlyIn, Countdown, Fps, FlyOut, GameOver, Paused };
    struct Camera { glm::vec3 eye{0.f}; glm::vec3 target{0.f}; float fov = 50.f; };
    struct Menu { std::vector<std::string> items; int index = 0; };

    void newGame();
    void applyScenario();
    void handleEvents();
    void menuKey(int key);
    void menuSelect();
    void update(float dt);
    void updateBlocksInput(float dt);
    void handleGameEvents();
    void handleFpsEvents();
    void enterMode(Mode m);
    Camera blocksCamera() const;
    Camera fpsCamera() const;
    Camera currentCamera() const;
    float boardTilt() const;                        // 0 = standing wall, 1 = flat on the floor
    glm::vec3 boardPos(float col, float row) const; // cell centre for the current tilt
    glm::vec3 boardPosH(float x, float h) const;    // board-plane coords (x, height) -> world

    void buildScene();
    void buildEnvironment();
    void addBoard();
    void addDecor();
    void addAmbient();
    void addHealthBars(float W, float H, float s, float lh);
    bool projectToScreen(glm::vec3 world, float& x, float& y) const;
    void addFpsActors();
    void addHud();
    void addLights();
    void text(float x, float y, const std::string& s, float scale, glm::vec4 color, int align = 0);
    void screenSprite(const std::string& key, float x, float y, float scale, glm::vec4 color, float anchorX, float anchorY, bool flip = false);
    void billboard(const std::string& key, glm::vec3 feet, float metresPerPixel, glm::vec4 color, bool lit, bool flip);
    // A sprite frame as either its voxel model (when the pack is on and has one) or a billboard.
    void actor(const std::string& key, glm::vec3 feet, float metresPerPixel, glm::vec4 color, bool lit, bool flip, float yaw, glm::vec3 emissive = {}, float emissiveStrength = 0.f);
    void cube(glm::vec3 pos, float scale, glm::vec4 color, const std::string& tex, glm::vec3 emissive = {}, float emissiveStrength = 0.f, float flags = 0.f, float phase = 0.f, float rotX = 0.f);
    void panel(float x, float y, float w, float h, glm::vec4 color);
    void announce(const std::string& text, glm::vec4 color, float scale = 1.2f);
    const std::string& animFrame(const SpriteAnim& a, float t, bool loop, bool* flip = nullptr) const;
    void play(const char* name, float gain = 1.f, float pitch = 1.f, int minIntervalMs = 45);
    void updateMusic();
    std::string pickTrack(const std::vector<const char*>& prefs, int index) const;
    std::string resolveTrack(const std::string& classicName) const;   // classic D_ name -> name in the chosen set
    void cycleMusicSet();
    const char* musicSetName() const;
    void loadSettings();
    void saveSettings() const;
    // Doom data: the WAD can be chosen at runtime (first-run screen, OPTIONS) and swapped in live.
    void initMusic();
    bool reloadAssets(const std::filesystem::path& wad, const std::string& extras = "");
    void browseForWad(bool extras = false);
    void saveWadChoice(const std::string& path) const;
    void saveExtrasChoice(const std::string& path) const;
    std::string extrasHint() const;   // --extras, then the saved choice, then redline.cfg
    static void dialogCallback(void* userdata, const char* const* files, int filter);   // SDL_DialogFileCallback
    // Profiles: one directory per player holding settings, scores and trophies.
    std::string profilesRoot() const;
    std::vector<std::string> listProfiles() const;
    void switchProfile(const std::string& name);
    void openScreen(int screen);
    void closeScreen();
    void screenKey(int key, bool fromPad);
    void adjustOption(int dir);
    void trophy(const char* id);
    void openGamepad(uint32_t which);
    void pollGamepad(float dt);
    void rumble(float low, float high, int ms);
    void padButton(int button, bool down);
    void addScreens();
    void beginLevelCard();
    // Display settings are per machine, not per player: <pref>/display.txt.
    void loadDisplaySettings();
    void saveDisplaySettings() const;
    void applyDisplay();
    void buildResolutionList();
    const char* displayModeName() const;
    void play(const std::string& name, float gain = 1.f, float pitch = 1.f, int minIntervalMs = 45) { play(name.c_str(), gain, pitch, minIntervalMs); }

    Options opts_;
    SDL_Window* window_ = nullptr;
    std::unique_ptr<render::VkContext> ctx_;
    std::unique_ptr<render::Renderer> renderer_;
    audio::Audio audio_;
    audio::Music music_;
    Assets assets_;
    VoxelModels voxels_;
    bool useVoxels_ = false;
    HighScores highScores_;
    Trophies trophies_;
    int lastRank_ = 0;
    float lastInvulnChance_ = 0.f;   // the prize rolled at the last fight start (for the failure notice)
    enum class MusicSet { Classic, Sc55, Modern };
    MusicSet musicSet_ = MusicSet::Classic;
    std::string settingsPath_;
    std::string profileName_;
    std::string prefDir_, baseDir_;    // SDL pref path (save data) and executable folder (installer config)
    bool wadMissing_ = false;          // running on placeholder art; the setup screen is offered
    std::string wadEntry_;             // typed path on the WAD path screen
    std::string wadStatus_;            // last result line on the WAD screens
    std::mutex dialogMutex_;
    std::vector<std::string> dialogFiles_;
    bool dialogDone_ = false, dialogOpen_ = false, dialogForExtras_ = false;
    std::string wadPath_;              // the IWAD in use ("" on placeholder art)
    // Overlay screens on top of the title / pause menus.
    enum Screen { kScreenNone = 0, kScreenOptions, kScreenTrophies, kScreenProfiles, kScreenNameEntry, kScreenCredits, kScreenWadSetup, kScreenWadPath };
    int screen_ = kScreenNone;
    int screenIndex_ = 0;
    std::string nameEntry_;
    int nameChar_ = 0;                 // gamepad letter picker position
    std::vector<std::string> profileList_;
    bool nameRequired_ = false;        // first launch: no profile yet
    // Per-profile controller settings.
    float padSens_ = 1.f;
    bool padInvertY_ = false;
    bool padRumble_ = true;
    SDL_Gamepad* pad_ = nullptr;
    std::string padName_;
    struct { bool left = false, right = false, down = false, fire = false, run = false; } padHeld_;
    // Decor props: sprite, feet position, light colour/radius (radius 0 = no light).
    struct Prop { const SpriteAnim* anim; glm::vec3 pos; float px; glm::vec3 lightColor; float lightRadius; float lightHeight; float phase; };
    std::vector<Prop> props_;
    void buildProps();
    // Display: 0 windowed, 1 borderless fullscreen, 2 exclusive fullscreen.
    int displayMode_ = 0;
    int resW_ = 1600, resH_ = 900;
    std::vector<std::pair<int, int>> resolutions_;
    // Level-up card and per-game stats for trophies.
    float levelCardT_ = 99.f;
    struct { int level = 0; int kills = 0; int blocks = 0; float seconds = 0.f; float damage = 0.f; int score = 0; } levelCard_;
    struct { int kills = 0; int blocks = 0; int pickups = 0; } fightStats_;
    int gameBlocks_ = 0, gamePickups_ = 0;

    std::unique_ptr<core::Game> game_;
    FpsMode fps_;
    Ambient ambient_;
    Mode mode_ = Mode::Title;
    Mode pausedFrom_ = Mode::Blocks;
    Menu menu_;
    float modeT_ = 0.f;
    float time_ = 0.f;
    float gameOverT_ = 0.f;
    Camera flyFrom_, flyTo_;
    bool running_ = true;
    bool mouseCaptured_ = false;
    int frameCount_ = 0;
    int highScore_ = 0;
    int redLinesSurvived_ = 0;
    bool diedInFps_ = false;

    // Blocks-mode input state (DAS)
    struct { bool left = false, right = false, down = false; float dasT = 0.f; int dasDir = 0; bool dasActive = false; } keys_;
    struct { float dx = 0.f, dy = 0.f; bool fire = false; bool fwd = false, back = false, left = false, right = false; bool run = false; int select = -1; int wheel = 0; float padMoveX = 0.f, padMoveZ = 0.f; bool padFire = false, padRun = false; } fpsIn_;

    // Per-frame draw lists
    std::vector<render::CubeInstance> envCubes_;
    std::vector<render::CubeInstance> cubes_;
    std::vector<render::QuadInstance> worldQuads_;
    std::vector<render::QuadInstance> screenQuads_;
    std::vector<render::MeshInstance> meshes_;
    render::FrameParams frame_;
    float muzzleLight_ = 0.f;
    float shakeT_ = 0.f;
    float botBlockedT_ = 0.f;
    float botSide_ = 1.f;
    int countdownLast_ = -1;
    struct Announcement { std::string text; glm::vec4 color; float scale; float t; };
    std::vector<Announcement> announcements_;
    bool keyB_ = false, keyF_ = false, keyG_ = false;   // the secret chord
    float bfgHoldT_ = 0.f;       // seconds B+F+G have been held together
    bool bfgUsed_ = false;       // the BFG9000 fires once per playthrough
    float bfgFlash_ = 0.f;
};

}  // namespace rl::game
