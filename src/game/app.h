#pragma once
// Top-level game: window, modes, scene assembly and HUD.
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "audio/audio.h"
#include "core/tetris.h"
#include "game/assets.h"
#include "game/fps_mode.h"
#include "render/renderer.h"
#include "render/vk_context.h"

struct SDL_Window;

namespace rl::game {

struct Options {
    std::optional<std::filesystem::path> wad;
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
};

class App {
public:
    explicit App(Options opts);
    ~App();
    int run();

private:
    enum class Mode { Title, Blocks, Alert, FlyIn, Fps, FlyOut, GameOver, Paused };
    struct Camera { glm::vec3 eye{0.f}; glm::vec3 target{0.f}; float fov = 50.f; };

    void newGame();
    void applyScenario();
    void handleEvents();
    void update(float dt);
    void updateBlocksInput(float dt);
    void handleGameEvents();
    void handleFpsEvents();
    void enterMode(Mode m);
    Camera blocksCamera() const;
    Camera fpsCamera() const;
    Camera currentCamera() const;

    void buildScene();
    void buildEnvironment();
    void addBoard();
    void addFpsActors();
    void addHud();
    void addLights();
    void text(float x, float y, const std::string& s, float scale, glm::vec4 color, int align = 0);
    void screenSprite(const std::string& key, float x, float y, float scale, glm::vec4 color, float anchorX, float anchorY, bool flip = false);
    void billboard(const std::string& key, glm::vec3 feet, float metresPerPixel, glm::vec4 color, bool lit, bool flip);
    void cube(glm::vec3 pos, float scale, glm::vec4 color, const std::string& tex, glm::vec3 emissive = {}, float emissiveStrength = 0.f, float flags = 0.f, float phase = 0.f);
    const std::string& animFrame(const SpriteAnim& a, float t, bool loop, bool* flip = nullptr) const;
    void play(const char* name, float gain = 1.f, float pitch = 1.f);

    Options opts_;
    SDL_Window* window_ = nullptr;
    std::unique_ptr<render::VkContext> ctx_;
    std::unique_ptr<render::Renderer> renderer_;
    audio::Audio audio_;
    Assets assets_;

    std::unique_ptr<core::Game> game_;
    FpsMode fps_;
    Mode mode_ = Mode::Title;
    Mode pausedFrom_ = Mode::Blocks;
    float modeT_ = 0.f;
    float time_ = 0.f;
    float gameOverT_ = 0.f;
    Camera flyFrom_, flyTo_;
    bool running_ = true;
    bool mouseCaptured_ = false;
    int frameCount_ = 0;
    int highScore_ = 0;
    int redLinesSurvived_ = 0;

    // Blocks-mode input state (DAS)
    struct { bool left = false, right = false, down = false; float dasT = 0.f; int dasDir = 0; bool dasActive = false; } keys_;
    struct { float dx = 0.f, dy = 0.f; bool fire = false; bool fwd = false, back = false, left = false, right = false; } fpsIn_;

    // Per-frame draw lists
    std::vector<render::CubeInstance> envCubes_;
    std::vector<render::CubeInstance> cubes_;
    std::vector<render::QuadInstance> worldQuads_;
    std::vector<render::QuadInstance> screenQuads_;
    render::FrameParams frame_;
    float muzzleLight_ = 0.f;
    float shakeT_ = 0.f;
};

}  // namespace rl::game
