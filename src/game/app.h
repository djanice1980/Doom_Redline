#pragma once
// Top-level game: window, modes, scene assembly, menu and HUD.
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "audio/audio.h"
#include "audio/music.h"
#include "game/highscores.h"
#include "game/trophies.h"
#include "core/tetris.h"
#include "core/tetris_bot.h"
#include "game/assets.h"
#include "game/fps_mode.h"
#include "game/ambient.h"
#include "game/voxels.h"
#include "game/playerstats.h"
#include "game/online.h"
#include "game/runrecord.h"
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
    std::string record;                // raw RGBA frames appended here every `recordEvery` frames (scripted runs; encode with ffmpeg)
    std::string onlineServer;          // --online-server: overrides the service URL for this run
    int recordEvery = 2;
    bool bot = false;                  // auto-aim/fire in FPS mode (smoke testing)
    bool mute = false;
    int level = 1;                     // starting level (scenarios)
    float absorbPeriod = 20.f;         // seconds before a monster may absorb blocks and grow
    bool god = false;                  // no damage to the player (testing)
    int arsenal = -1;                  // testing: start the fight holding this weapon with everything owned
    std::string keys;                  // letters to hold from frame keysFrame (testing), e.g. "BFG"
    int keysFrame = 30;
    int keysHoldFrames = 150;          // released this many frames later
    struct Click { int x, y, frame; };
    std::vector<Click> clicks;         // testing: left-clicks at window coordinates on given frames
    int stackRows = 0;                 // pre-fill this many holey rows of normal blocks (blocks scenario)
    bool noMusic = false;
    float musicVolume = 0.5f;          // music and effects start at the same level (see sfxVolume_)
    std::string musicSet;              // "classic" | "sc55" | "modern" (empty = saved preference, default modern when available)
    std::string profile;               // player profile to use (skips the name prompt)
    std::string reloadWad;             // testing: swap to this WAD after 30 frames
    std::optional<std::filesystem::path> voxelDir;   // KVX pack directory (default: auto-detect)
    int voxels = -1;                   // -1 saved preference, 0 sprites, 1 voxel models
    int rtShadows = -1;                // -1 saved preference, 0 shadow map, 1 ray-traced sun, 2 all lights, 3 + reflections
    int msaa = -1;                     // -1 saved preference, 0 off, 1 on
    int brutal = -1;                   // -1 saved preference, 0 off, 1 on (gore, gibs, casings)
    int bloom = -1;                    // -1 saved preference, 0 off, 1 on
};

class App {
public:
    explicit App(Options opts);
    ~App();
    int run();

private:
    enum class Mode { Title, Blocks, Alert, FlyIn, Countdown, Fps, FlyOut, GameOver, Paused };
    struct Camera { glm::vec3 eye{0.f}; glm::vec3 target{0.f}; float fov = 50.f; glm::vec3 up{0.f, 1.f, 0.f}; };
    struct Menu { std::vector<std::string> items; int index = 0; };

    void newGame(bool enter = true);   // enter = false: reset only, the caller picks the mode
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
    // As text(), but shrunk just enough to fit `maxWidth` (never enlarged). Returns the scale used.
    float textFit(float x, float y, const std::string& s, float scale, glm::vec4 color, int align, float maxWidth);
    // Text turned by `angle` radians (counter-clockwise) about its own centre at (cx, cy).
    void textRot(float cx, float cy, const std::string& s, float scale, glm::vec4 color, float angle);
    // Mouse: every menu item, option row and BACK/CANCEL label registers a hotspot as it is drawn.
    enum { kHotMenu = 0, kHotScreenItem, kHotOptionRow, kHotBack, kHotProfileAction };   // profile action: index 0 rename, 1 email, 2 delete
    struct Hotspot { float x, y, w, h; int kind; int index; };
    std::vector<Hotspot> hotspots_;
    void hotText(float x, float y, const std::string& s, float scale, glm::vec4 color, int align, int kind, int index);
    void hotRect(float x, float y, float w, float h, int kind, int index) { hotspots_.push_back({x, y, w, h, kind, index}); }
    const Hotspot* hotspotAt(float wx, float wy) const;   // window coordinates -> hotspot or null
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
    void applyAssets(Assets&& fresh);           // swap the loaded set in (atlas, environment, props, music)
    void setDoomArt(bool on);                   // OPTIONS > DOOM ART: placeholder art without touching the WAD choice
    void decal(const std::string& key, glm::vec3 pos, glm::vec3 normal, float size, float yaw, glm::vec4 color);
    void addGore();                             // brutal: blood, chunks, casings and decals from the fight
    void addBursts();                           // one-shot sprite effects (blood clouds, smoke, sparks)
    void softBillboard(const std::string& key, glm::vec3 pos, float metresPerPixel, glm::vec4 color);   // no alpha cutoff, no shadow
    void drawDecal(const Decal& d, size_t index);
    void loadMaterials();                       // normal/roughness maps for the arena textures (WAD art only)
    int materialSlot(const std::string& lump) const;
    void browseForWad(bool extras = false);
    void saveWadChoice(const std::string& path) const;
    void saveExtrasChoice(const std::string& path) const;
    std::string extrasHint() const;   // --extras, then the saved choice, then redline.cfg
    static void dialogCallback(void* userdata, const char* const* files, int filter);   // SDL_DialogFileCallback
    // Profiles: one directory per player holding settings, scores and trophies.
    std::string profilesRoot() const;
    std::vector<std::string> listProfiles() const;
    // Top scores across every profile on this machine, with the player's name (title screen).
    struct NamedScore { HighScore score; std::string player; };
    std::vector<NamedScore> allProfileScores_;
    void refreshAllScores();
    void switchProfile(const std::string& name);
    void loadProfileList();
    void profileAction(int action);   // 0 rename, 1 email, 2 delete, on the selected row
    void deleteProfile(const std::string& name);
    bool renameProfile(const std::string& from, const std::string& to);
    void applyAccountPlayers(const Json& existing);      // the "existing" list from a confirm or poll reply
    void queuePlayerDelete(const std::string& playerId, const std::string& token);   // survives the profile folder going away
    void sendPendingDeletes();
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
    float pixelDensity() const;                 // physical pixels per window point (desktop scaling)
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
    bool doomArtOff_ = false;          // play on the placeholder art even though a WAD is known
    bool brutal_ = true;               // OPTIONS > DOOM ART > BRUTAL: blood, gibs, casings, bullet holes, screen blood
    bool dungeon_ = true;              // OPTIONS > DUNGEON: the crypt behind the back wall after every arena
    float sfxVolume_ = 0.5f;           // OPTIONS > SFX VOLUME (master gain on every effect)
    bool brutalActive() const { return brutal_ && assets_.usingWad(); }   // the preference only applies on Doom art
    struct ScreenBlood { float x, y, scale, t, ttl; int frame; };
    std::vector<ScreenBlood> screenBlood_;
    struct Burst { glm::vec3 pos; float t, ttl, px; const SpriteAnim* anim; glm::vec4 tint{1.3f, 1.3f, 1.3f, 1.f}; };   // one-shot sprite effects (blood clouds, smoke, dust)
    struct Ember { glm::vec3 pos, vel; float ttl, life, size; glm::vec3 col; };   // rising sparks from the torches
    std::vector<Ember> embers_;
    float emberT_ = 0.f;
    struct Ring { glm::vec3 pos; float t; };   // line-clear shockwaves
    std::vector<Ring> rings_;
    float clearFlash_ = 0.f;
    std::vector<std::pair<int, int>> lastPieceCells_;   // where the falling piece was last frame (dust when it lands)
    std::vector<Decal> brawlDecals_;   // blood the brawlers beside the board leave behind (brutal)
    std::vector<Burst> bursts_;
    std::mt19937 rng_{1234u};
    HighScores highScores_;
    Trophies trophies_;
    MachineInfo machine_;              // this save folder's install id and hardware facts
    PlayerStats stats_;                // per-profile identity, play time and input counters
    // Online service (docs/online-and-releases.md): opt-in per profile, registration by
    // emailed code, run records uploaded at game over, leaderboard fetches.
    OnlineClient online_;
    bool onlineOn_ = true;             // per-profile setting (settings.txt online=); on by default, nothing is posted until the email is approved
    bool onlineAsked_ = false;         // the "post your scores?" question has been asked for this profile (settings.txt online_asked=)
    float pollT_ = 0.f;                // seconds until the next registration poll
    // Registering an address that already has players: the account's other
    // profiles, offered so a deleted-and-remade profile can carry on as itself
    // instead of leaving its history stranded on a second row.
    struct AccountPlayer { std::string id, name, since; int runs = 0, bestScore = 0, machines = 0; bool sameName = false; };
    std::vector<AccountPlayer> adoptChoices_;
    bool adoptBusy_ = false;
    // Deleting a registered profile: what the online record holds, so the
    // question can say what would be erased.
    std::string deleteProfileName_, deletePlayerId_, deleteToken_;
    AccountPlayer deleteOnlineInfo_;
    bool deleteInfoLoaded_ = false;
    float versionAgeT_ = 0.f;          // seconds since the last version check (re-checked on the title every ten minutes and when WHAT'S NEW opens)
    // Update notice and WHAT'S NEW (the service's /api/version, cached in <pref>/version.json).
    std::string latestVersion_, latestUrl_;
    bool updateAvailable_ = false;
    Json changelog_;
    int whatsNewScroll_ = 0;
    void checkVersion();
    void applyVersionInfo(const Json& info, bool fromNetwork);
    static bool versionNewer(const std::string& a, const std::string& b);   // a > b, "x.y.z"
    std::string onlineServer_;         // <pref>/online.txt server=, REDLINE_ONLINE_URL, --online-server, or the default
    std::string onlineStatus_;         // last upload result for the game-over screen
    int onlineRank_ = 0, onlineTotal_ = 0;
    std::string onlineTier_;
    std::string registerEmail_;        // address being registered
    bool registerBusy_ = false;        // a register/confirm request is in flight
    Json leaderboard_;                 // last reply for the board on screen
    std::string leaderboardFetched_;   // "just now", "3 minutes ago", or "" (never)
    int leaderboardTab_ = 0;
    bool leaderboardLoading_ = false;
    std::string leaderboardError_;
    // Per-run counters for the run record.
    struct RunCounters {
        std::string startedAt; double duration = 0; int pieces = 0, tetrises = 0, bestChain = 0, fights = 0;
        int killsByKind[kRunKinds] = {}; int shots[kRunWeapons] = {}; double damage = 0; int killedBy = -1;
        int64_t kb0 = 0, mouse0 = 0, pad0 = 0; std::vector<std::string> trophies;
    } run_;
    bool runRecorded_ = false;
    void loadOnlineConfig();
    void pollOnline();
    void recordRun();
    void submitPendingRuns();
    void startRegistration();
    void openLeaderboard(int tab);
    std::string runsDir() const;
    std::string machineLabel() const;
    float statsSaveT_ = 0.f;
    std::string emailEntry_;
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
    enum Screen { kScreenNone = 0, kScreenOptions, kScreenTrophies, kScreenProfiles, kScreenNameEntry, kScreenCredits, kScreenWadSetup, kScreenWadPath, kScreenEmailEntry,
                  kScreenRegister, kScreenCode, kScreenLeaderboard, kScreenOnlineAsk, kScreenWhatsNew, kScreenAdopt, kScreenDeleteOnline, kScreenControls };
    int screen_ = kScreenNone;
    int screenIndex_ = 0;
    int optionsScroll_ = 0;        // first option row in view (the list scrolls when the window is short)
    bool optionsFollow_ = true;    // scroll to keep the selected row in view (keyboard/pad); the wheel turns it off
    std::string nameEntry_;
    int nameChar_ = 0;                 // gamepad letter picker position
    std::vector<std::string> profileList_;
    bool nameRequired_ = false;        // first launch: no profile yet
    // Profile manager (the PLAYERS screen): who is playing, plus rename / email / delete.
    struct ProfileInfo { int best = 0, trophies = 0; double played = 0.0; bool email = false; };
    std::vector<ProfileInfo> profileInfo_;   // parallel to profileList_
    bool profileRequired_ = false;     // start-up: the picker stays up until someone is chosen
    int profileConfirmDelete_ = -1;    // row awaiting a second confirmation
    bool nameRename_ = false;          // the name screen renames renameFrom_ instead of creating
    std::string renameFrom_;
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
    int rtShadows_ = 3;                // machine setting (display.txt): 0 map, 1 sun, 2 all lights, 3 + reflections; used when the GPU can
    bool msaa_ = true;                 // machine settings (display.txt): multisampling and bloom
    bool bloom_ = true;
    bool hdrQuads_ = false;            // while set, screen sprites/panels go into the HDR world pass (weapon + flash)
    float viewKick_ = 0.f;             // camera pitch kick from firing (radians), decays
    float lastYaw_ = 0.f, lastPitch_ = 0.f, swayX_ = 0.f, swayY_ = 0.f;   // weapon lag behind the view
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
    std::vector<render::CubeRange> envRanges_;     // env cubes grouped by material slot
    std::vector<render::CubeRange> cubeRanges_;
    std::vector<std::string> materialLumps_;       // slot i+1 = maps for this Doom lump
    std::vector<render::CubeInstance> cubes_;
    std::vector<render::QuadInstance> worldQuads_;
    std::vector<render::QuadInstance> screenQuads_;
    std::vector<render::MeshInstance> meshes_;
    render::FrameParams frame_;
    float muzzleLight_ = 0.f;
    float shakeT_ = 0.f;
    float botBlockedT_ = 0.f;
    float botSide_ = 1.f;
    float updateNoticeT_ = 0.f;
    int envStage_ = 0;             // FpsMode::Stage the environment cubes were built for
    bool envDungeon_ = false;
    bool envDoorOpen_ = false;      // whether they include a dungeon
    glm::vec3 botWaypoint_{0.f};   // the fight bot's next path step (navNext), refreshed a few times a second
    float botNavT_ = 0.f;
    bool botHaveWaypoint_ = false;
    int botTargetIdx_ = -1;         // the bot sticks with one target, or it paths between two and never arrives
    float botTargetT_ = 0.f;
    glm::vec3 botLastPos_{0.f};     // the test bot wedges itself on corners; this shakes it loose
    float botStuckT_ = 0.f;
    void botUnstick(FpsInput& in, float dt);
    core::TetrisBot tetrisBot_;    // --bot: plays the block phase too (one input every 0.12 s, a person's pace)
    float botTetrisT_ = 0.f;   // seconds the NEW VERSION sticker has been on the title (drives its zoom bursts)
    int countdownLast_ = -1;
    struct Announcement { std::string text; glm::vec4 color; float scale; float t; };
    std::vector<Announcement> announcements_;
    // Trophy toasts: a console-style card that slides in at the top, one at a time,
    // and the celebration for the last trophy (fireworks and confetti over everything).
    struct Toast { std::string name, desc; int count = 0, total = 0; float t = 0.f; bool ultimate = false; };
    std::vector<Toast> toasts_;
    struct Spark { glm::vec2 pos, vel; float ttl, life, size; glm::vec3 col; int kind; };   // kind 0 rocket, 1 spark, 2 confetti
    std::vector<Spark> sparks_;
    float celebrateT_ = -1.f;      // >= 0 while the RIP AND TEAR celebration runs
    float celebrateNext_ = 0.f;    // next rocket
    void showTrophy(const TrophyDef& d, bool ultimate);
    void startCelebration();
    // The evil banner beside the board: blood runs off its letters, and each
    // block that turns jolts it.
    struct Drip { float x, y0, y, vy, w, ttl; };   // a run of blood from y0 (the letter) down to its head at y
    std::vector<Drip> drips_;
    float dripT_ = 0.f;
    float evilJolt_ = 0.f;
    float bannerX0_ = 0.f, bannerX1_ = 0.f, bannerY_ = 0.f;   // extent of the last banner drawn (drip origins)
    bool keyB_ = false, keyF_ = false, keyG_ = false;   // the secret chord
    float bfgHoldT_ = 0.f;       // seconds B+F+G have been held together
    bool bfgUsed_ = false;       // the BFG9000 fires once per playthrough
    float bfgFlash_ = 0.f;
};

}  // namespace rl::game
