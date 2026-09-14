#include "game/app.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <stdexcept>

#include <glm/gtc/matrix_transform.hpp>

namespace rl::game {

namespace {
constexpr float kPi = 3.14159265f;
constexpr float kAlertTime = 1.4f;
constexpr float kFlyInTime = 1.8f;
constexpr float kFlyOutTime = 1.4f;
constexpr float kSpritePx = 0.031f;   // metres per Doom sprite pixel (imp = 57px ~ 1.75 m)

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
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) throw std::runtime_error(std::string("SDL_Init: ") + SDL_GetError());
    Uint32 flags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE;
    if (opts_.fullscreen) flags |= SDL_WINDOW_FULLSCREEN;
    window_ = SDL_CreateWindow("REDLINE", opts_.width, opts_.height, flags);
    if (!window_) throw std::runtime_error(std::string("SDL_CreateWindow: ") + SDL_GetError());
    ctx_ = std::make_unique<render::VkContext>(window_, opts_.preferIntegrated);
    renderer_ = std::make_unique<render::Renderer>(*ctx_);

    if (!opts_.mute) audio_.init();
    std::optional<std::filesystem::path> wad;
    if (!opts_.noWad) wad = Assets::findWad(opts_.wad);
    if (!wad && !opts_.noWad) std::fprintf(stderr, "[assets] no Doom WAD found; using procedural art (pass --wad <file> or set REDLINE_WAD)\n");
    if (!assets_.load(wad, audio_)) throw std::runtime_error("asset build failed");
    renderer_->setAtlas(assets_.atlas().image());
    buildEnvironment();

    newGame();
    applyScenario();
}

App::~App() {
    renderer_.reset();
    ctx_.reset();
    if (window_) SDL_DestroyWindow(window_);
    SDL_Quit();
}

void App::play(const char* name, float gain, float pitch) { audio_.play(name, gain, pitch); }

void App::newGame() {
    uint32_t seed = opts_.seed ? opts_.seed : static_cast<uint32_t>(std::chrono::steady_clock::now().time_since_epoch().count() & 0xFFFFFFFFu);
    game_ = std::make_unique<core::Game>(seed);
    fps_ = FpsMode();
    redLinesSurvived_ = 0;
    keys_ = {};
    enterMode(opts_.scenario == "title" ? Mode::Title : Mode::Blocks);
}

void App::applyScenario() {
    if (opts_.scenario == "redline" || opts_.scenario == "fps") {
        // Nine red cells on the floor, an all-red I piece dropped into the gap,
        // and a few normal blocks stacked nearby so the explosions have victims.
        for (int c = 1; c < core::kBoardW; ++c) game_->setCell(c, core::kBoardH - 1, core::Cell{core::CellKind::Red, 0});
        for (int c = 2; c < 8; ++c) game_->setCell(c, core::kBoardH - 2, core::Cell{core::CellKind::Normal, static_cast<uint8_t>(c % 7)});
        for (int c = 3; c < 7; ++c) game_->setCell(c, core::kBoardH - 3, core::Cell{core::CellKind::Normal, static_cast<uint8_t>((c + 2) % 7)});
        game_->setCell(4, core::kBoardH - 4, core::Cell{core::CellKind::Red, 0});
        game_->forcePiece(core::Shape::I, {true, true, true, true});
        game_->spawnNow();
        game_->rotateCW();
        for (int i = 0; i < 6; ++i) game_->moveLeft();
        game_->hardDrop();
        for (int i = 0; i < 200 && game_->phase() != core::Phase::RedLine; ++i) game_->tick(0.05f);
        if (opts_.scenario == "fps") {
            // Skip the alert and the fly-in.
            fps_.begin(*game_, game_->redRows());
            enterMode(Mode::Fps);
        }
    }
}

void App::enterMode(Mode m) {
    static const char* names[] = {"Title", "Blocks", "Alert", "FlyIn", "Fps", "FlyOut", "GameOver", "Paused"};
    std::fprintf(stderr, "[app] mode %s -> %s (frame %d, score %d)\n", names[static_cast<int>(mode_)], names[static_cast<int>(m)], frameCount_, game_ ? game_->score() : 0);
    modeT_ = 0.f;
    Mode prev = mode_;
    mode_ = m;
    bool wantMouse = (m == Mode::Fps);
    if (wantMouse != mouseCaptured_) {
        SDL_SetWindowRelativeMouseMode(window_, wantMouse);
        mouseCaptured_ = wantMouse;
    }
    switch (m) {
    case Mode::Alert:
        play("redline", 1.f);
        break;
    case Mode::FlyIn:
        flyFrom_ = blocksCamera();
        fps_.begin(*game_, game_->redRows());
        flyTo_ = fpsCamera();
        break;
    case Mode::Fps:
        play("enemy_sight", 0.9f);
        fpsIn_ = {};
        break;
    case Mode::FlyOut:
        flyFrom_ = fpsCamera();
        flyTo_ = blocksCamera();
        ++redLinesSurvived_;
        break;
    case Mode::Blocks:
        if (prev == Mode::FlyOut) game_->resumeAfterRedLine();
        break;
    case Mode::GameOver:
        play("gameover", 1.f);
        gameOverT_ = 0.f;
        highScore_ = std::max(highScore_, game_->score());
        break;
    default:
        break;
    }
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

        handleEvents();
        update(dt);
        audio_.update();
        buildScene();
        renderer_->render(frame_, cubes_, worldQuads_, screenQuads_);
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

void App::handleEvents() {
    SDL_Event e;
    fpsIn_.dx = fpsIn_.dy = 0.f;
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
            if (mode_ == Mode::Fps) { fpsIn_.dx += e.motion.xrel; fpsIn_.dy += e.motion.yrel; }
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            if (e.button.button == SDL_BUTTON_LEFT) {
                if (mode_ == Mode::Fps) fpsIn_.fire = true;
                else if (mode_ == Mode::Title) enterMode(Mode::Blocks);
            }
            break;
        case SDL_EVENT_MOUSE_BUTTON_UP:
            if (e.button.button == SDL_BUTTON_LEFT) fpsIn_.fire = false;
            break;
        case SDL_EVENT_KEY_DOWN: {
            SDL_Keycode k = e.key.key;
            if (k == SDLK_F12) {
                renderer_->screenshot("redline-screenshot.png");
                break;
            }
            if (k == SDLK_ESCAPE) {
                if (mode_ == Mode::Blocks || mode_ == Mode::Fps) { pausedFrom_ = mode_; enterMode(Mode::Paused); }
                else if (mode_ == Mode::Paused) enterMode(pausedFrom_);
                else if (mode_ == Mode::Title) running_ = false;
                else if (mode_ == Mode::GameOver) newGame();
                break;
            }
            if (mode_ == Mode::Title && (k == SDLK_RETURN || k == SDLK_SPACE)) { enterMode(Mode::Blocks); break; }
            if (mode_ == Mode::GameOver && (k == SDLK_RETURN || k == SDLK_SPACE)) { newGame(); enterMode(Mode::Blocks); break; }
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
        case core::EventType::PieceMoved: play("move", 0.5f); break;
        case core::EventType::PieceRotated: play("rotate", 0.6f); break;
        case core::EventType::PieceLocked: play("lock", 0.8f); break;
        case core::EventType::HardDrop: shakeT_ = 0.15f; break;
        case core::EventType::LinesCleared: play("clear", 0.9f, ev.a >= 4 ? 1.3f : 1.f); break;
        case core::EventType::RedCellFell: play("lock", 0.4f, 1.6f); break;
        case core::EventType::LevelUp: play("levelup", 1.f); break;
        case core::EventType::RedLine: break;   // handled by the mode switch
        case core::EventType::GameOver: break;
        default: break;
        }
    }
}

void App::handleFpsEvents() {
    for (const FpsEvent& ev : fps_.drainEvents()) {
        switch (ev.type) {
        case FpsEvent::Type::Shoot: play("shoot", 1.f); muzzleLight_ = 1.f; shakeT_ = 0.12f; break;
        case FpsEvent::Type::EnemyHit: play("enemy_pain", 0.8f); break;
        case FpsEvent::Type::EnemyDied: play("enemy_die", 1.f); break;
        case FpsEvent::Type::Explosion: play("explode", 1.f); shakeT_ = 0.35f; break;
        case FpsEvent::Type::PlayerHit: play("pain", 1.f); shakeT_ = 0.25f; break;
        case FpsEvent::Type::FireballLaunched: play("fireball", 0.7f); break;
        case FpsEvent::Type::FireballHit: play("fireball_hit", 0.6f); break;
        case FpsEvent::Type::AllClear: play("levelup", 1.f); break;
        case FpsEvent::Type::PlayerDead: break;
        case FpsEvent::Type::EnemySight: break;
        }
    }
}

void App::update(float dt) {
    modeT_ += dt;
    shakeT_ = std::max(0.f, shakeT_ - dt);
    muzzleLight_ = std::max(0.f, muzzleLight_ - dt * 8.f);

    switch (mode_) {
    case Mode::Title:
        break;
    case Mode::Blocks:
        updateBlocksInput(dt);
        game_->tick(dt);
        handleGameEvents();
        if (game_->phase() == core::Phase::RedLine) enterMode(Mode::Alert);
        else if (game_->phase() == core::Phase::GameOver) enterMode(Mode::GameOver);
        break;
    case Mode::Alert:
        if (modeT_ >= kAlertTime) enterMode(Mode::FlyIn);
        break;
    case Mode::FlyIn:
        if (modeT_ >= kFlyInTime) enterMode(Mode::Fps);
        break;
    case Mode::Fps: {
        FpsInput in;
        in.moveZ = (fpsIn_.fwd ? 1.f : 0.f) - (fpsIn_.back ? 1.f : 0.f);
        in.moveX = (fpsIn_.right ? 1.f : 0.f) - (fpsIn_.left ? 1.f : 0.f);
        in.lookDX = fpsIn_.dx;
        in.lookDY = fpsIn_.dy;
        in.fire = fpsIn_.fire;
        if (opts_.bot) {
            // Aim at the nearest living enemy's chest and fire when the gun is ready.
            const Enemy* target = nullptr;
            float best = 1e9f;
            for (const Enemy& en : fps_.enemies()) {
                if (en.state == Enemy::State::Dead || en.state == Enemy::State::Dying) continue;
                float d = glm::length(en.pos - fps_.eye());
                if (d < best) { best = d; target = &en; }
            }
            if (target) {
                glm::vec3 to = (target->pos + glm::vec3(0.f, 0.9f, 0.f)) - fps_.eye();
                float wantYaw = std::atan2(to.x, to.z);
                float wantPitch = std::atan2(to.y, std::sqrt(to.x * to.x + to.z * to.z));
                float dyaw = std::remainder(wantYaw - fps_.yaw(), 2.f * kPi);
                in.lookDX = -dyaw / 0.0022f;
                in.lookDY = -(wantPitch - fps_.pitch()) / 0.0022f;
                in.fire = std::fabs(dyaw) < 0.03f;
                in.moveX = std::sin(time_ * 1.7f);   // strafe to dodge fireballs
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
    switch (mode_) {
    case Mode::Fps: return fpsCamera();
    case Mode::FlyIn: {
        float t = smoothstep(modeT_ / kFlyInTime);
        Camera c;
        c.eye = glm::mix(flyFrom_.eye, flyTo_.eye, t);
        c.target = glm::mix(flyFrom_.target, flyTo_.target, t);
        c.fov = glm::mix(flyFrom_.fov, flyTo_.fov, t);
        return c;
    }
    case Mode::FlyOut: {
        float t = smoothstep(modeT_ / kFlyOutTime);
        Camera c;
        c.eye = glm::mix(flyFrom_.eye, flyTo_.eye, t);
        c.target = glm::mix(flyFrom_.target, flyTo_.target, t);
        c.fov = glm::mix(flyFrom_.fov, flyTo_.fov, t);
        return c;
    }
    case Mode::Paused:
        return pausedFrom_ == Mode::Fps ? fpsCamera() : blocksCamera();
    case Mode::Alert: {
        // Slow push-in towards the red row.
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
void App::cube(glm::vec3 pos, float scale, glm::vec4 color, const std::string& tex, glm::vec3 emissive, float emissiveStrength, float flags, float phase) {
    const render::AtlasRegion& r = assets_.region(tex);
    render::CubeInstance c;
    c.posScale = glm::vec4(pos, scale);
    c.color = color;
    c.emissive = glm::vec4(emissive, emissiveStrength);
    c.uvRect = glm::vec4(r.u0, r.v0, r.u1, r.v1);
    c.params = glm::vec4(0.7f, 0.05f, phase, flags);
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
        envCubes_.push_back(c);
    };
    const int halfW = static_cast<int>(FpsMode::kArenaHalfW) + 1;
    const int depth = static_cast<int>(FpsMode::kArenaDepth) + 1;
    const int height = core::kBoardH + 3;
    // Floor (top face at y = 0) and ceiling
    for (int x = -halfW; x < halfW; ++x)
        for (int z = -2; z < depth; ++z) {
            push({x + 0.5f, -0.5f, z + 0.5f}, assets_.floor, glm::vec4(1.f));
            push({x + 0.5f, height + 0.5f, z + 0.5f}, assets_.ceiling, glm::vec4(0.6f, 0.6f, 0.6f, 1.f));
        }
    // Back wall behind the board, side walls, front wall behind the player.
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
    // Board frame: dark pillars either side and a lip below.
    for (int y = 0; y < core::kBoardH + 1; ++y) {
        push({-core::kBoardW * 0.5f - 0.5f, y + 0.5f, 0.f}, assets_.block, glm::vec4(0.25f, 0.25f, 0.3f, 1.f));
        push({core::kBoardW * 0.5f + 0.5f, y + 0.5f, 0.f}, assets_.block, glm::vec4(0.25f, 0.25f, 0.3f, 1.f));
    }
}

void App::addBoard() {
    const core::Game& g = *game_;
    const std::vector<int>& clearing = g.clearingRows();
    float flash = 0.5f + 0.5f * std::sin(g.clearProgress() * kPi * 3.f);
    bool alert = (mode_ == Mode::Alert);
    float alertFlash = alert ? (0.5f + 0.5f * std::sin(modeT_ * 18.f)) : 0.f;

    for (int r = 0; r < core::kBoardH; ++r) {
        bool rowClearing = std::find(clearing.begin(), clearing.end(), r) != clearing.end();
        bool rowRed = g.rowAllRed(r);
        for (int c = 0; c < core::kBoardW; ++c) {
            const core::Cell& cell = g.at(c, r);
            if (cell.empty()) continue;
            // Cells that have become enemies are not drawn as blocks any more.
            if (cell.red() && (mode_ == Mode::Fps || mode_ == Mode::FlyIn || mode_ == Mode::FlyOut)) {
                bool isEnemy = false;
                for (const Enemy& e : fps_.enemies()) if (e.col == c && e.row == r) isEnemy = true;
                if (isEnemy) continue;
            }
            glm::vec3 pos = cellCentre(c, r);
            if (cell.red()) {
                float e = 0.8f + (rowRed ? 0.9f * alertFlash : 0.f);
                cube(pos, 0.96f, glm::vec4(1.f, 0.55f, 0.55f, 1.f), assets_.redBlock, glm::vec3(1.f, 0.12f, 0.05f), e, 1.f, static_cast<float>(c) * 0.7f + r);
            } else {
                glm::vec3 col = kPieceColors[cell.color % 7];
                if (rowClearing) col = glm::mix(col, glm::vec3(1.f), flash);
                cube(pos, 0.96f, glm::vec4(col, 1.f), assets_.block, rowClearing ? glm::vec3(1.f) : glm::vec3(0.f), rowClearing ? flash : 0.f);
            }
        }
    }
    if (mode_ == Mode::Blocks || mode_ == Mode::Paused || mode_ == Mode::Title) {
        if (auto ghost = g.ghost()) {
            for (int i = 0; i < 4; ++i) {
                auto [cx, cy] = ghost->cells()[i];
                if (cy < 0) continue;
                glm::vec3 col = ghost->red[i] ? glm::vec3(0.9f, 0.2f, 0.2f) : kPieceColors[static_cast<int>(ghost->shape)];
                cube(cellCentre(cx, cy), 0.3f, glm::vec4(col * 0.8f, 1.f), assets_.block, col, 0.35f);
            }
        }
        if (const auto& p = g.active()) {
            for (int i = 0; i < 4; ++i) {
                auto [cx, cy] = p->cells()[i];
                if (cy < 0) continue;
                if (p->red[i])
                    cube(cellCentre(cx, cy), 0.96f, glm::vec4(1.f, 0.55f, 0.55f, 1.f), assets_.redBlock, glm::vec3(1.f, 0.12f, 0.05f), 0.9f, 1.f, static_cast<float>(i));
                else
                    cube(cellCentre(cx, cy), 0.96f, glm::vec4(kPieceColors[static_cast<int>(p->shape)], 1.f), assets_.block);
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

const std::string& App::animFrame(const SpriteAnim& a, float t, bool loop, bool* flip) const {
    static const std::string empty;
    if (a.empty()) return empty;
    int n = static_cast<int>(a.frames.size());
    int i = static_cast<int>(t * a.fps);
    i = loop ? ((i % n) + n) % n : std::clamp(i, 0, n - 1);
    if (flip) *flip = a.mirrored[i];
    return a.frames[i];
}

void App::addFpsActors() {
    for (const Enemy& e : fps_.enemies()) {
        const SpriteAnim* anim = &assets_.enemyIdle;
        bool loop = true;
        float t = e.animT;
        glm::vec4 tint(1.f);
        switch (e.state) {
        case Enemy::State::Emerging: anim = &assets_.enemyIdle; t = e.stateT; tint = glm::vec4(1.f, 0.6f, 0.6f, 1.f); break;
        case Enemy::State::Idle: anim = &assets_.enemyIdle; break;
        case Enemy::State::Attack: anim = &assets_.enemyAttack; loop = false; t = e.stateT; break;
        case Enemy::State::Pain: anim = &assets_.enemyPain; loop = false; t = e.stateT; tint = glm::vec4(1.f, 0.7f, 0.7f, 1.f); break;
        case Enemy::State::Dying: anim = &assets_.enemyDeath; loop = false; t = e.stateT; break;
        case Enemy::State::Dead: anim = &assets_.enemyDeath; loop = false; t = 100.f; break;
        }
        bool flip = false;
        const std::string& key = animFrame(*anim, t, loop, &flip);
        if (!key.empty()) billboard(key, e.pos, kSpritePx, tint, true, flip);
    }
    for (const Projectile& p : fps_.projectiles()) {
        bool flip = false;
        const std::string& key = animFrame(assets_.fireball, p.animT, true, &flip);
        if (!key.empty()) billboard(key, p.pos - glm::vec3(0.f, 0.3f, 0.f), kSpritePx * 1.2f, glm::vec4(1.f), false, flip);
    }
    for (const Explosion& ex : fps_.explosions()) {
        float t = ex.t / ex.duration;
        const SpriteAnim& anim = ex.radius > 1.f ? assets_.explosion : assets_.fireballHit;
        int n = static_cast<int>(anim.frames.size());
        if (n == 0) continue;
        int i = std::clamp(static_cast<int>(t * n), 0, n - 1);
        float scale = ex.radius > 1.f ? kSpritePx * 2.6f : kSpritePx * 1.2f;
        // Explosion sprites have their origin near the middle; place the origin at the blast centre.
        billboard(anim.frames[i], ex.pos - glm::vec3(0.f, ex.radius > 1.f ? 0.9f : 0.2f, 0.f), scale, glm::vec4(1.f, 1.f, 1.f, 1.f), false, anim.mirrored[i]);
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
    // Every red cell glows.
    for (int r = 0; r < core::kBoardH; ++r)
        for (int c = 0; c < core::kBoardW; ++c)
            if (game_->at(c, r).red()) {
                glm::vec3 p = cellCentre(c, r) + glm::vec3(0.f, 0.f, 0.8f);
                float pulse = 0.8f + 0.2f * std::sin(time_ * 6.f + c * 0.7f + r);
                cands.push_back({glm::length(p - cam), {p, 3.5f, {1.f, 0.15f, 0.05f}, 1.2f * pulse}});
            }
    for (const Enemy& e : fps_.enemies())
        if (e.state != Enemy::State::Dead)
            cands.push_back({glm::length(e.pos - cam) - 5.f, {e.pos + glm::vec3(0.f, 1.f, 0.4f), 4.f, {1.f, 0.2f, 0.05f}, 1.0f}});
    for (const Explosion& ex : fps_.explosions()) {
        float t = 1.f - ex.t / ex.duration;
        cands.push_back({-100.f, {ex.pos, ex.radius > 1.f ? 9.f : 3.f, {1.f, 0.6f, 0.2f}, (ex.radius > 1.f ? 6.f : 1.5f) * t}});
    }
    for (const Projectile& p : fps_.projectiles()) cands.push_back({-50.f, {p.pos, 3.f, {1.f, 0.5f, 0.1f}, 1.2f}});
    if (muzzleLight_ > 0.f && (mode_ == Mode::Fps)) cands.push_back({-200.f, {fps_.eye() + fps_.forward() * 1.2f, 7.f, {1.f, 0.8f, 0.4f}, 3.f * muzzleLight_}});
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
        // Doom glyphs carry a top offset so that letters with descenders line up.
        float gy = y + (assets_.usingWad() ? -r.offsetY * scale : 0.f);
        screenSprite(it->second, cx, gy, scale, color, 0.f, 1.f);
        cx += (r.w + 1) * scale;
    }
}

void App::addHud() {
    VkExtent2D ext = renderer_->extent();
    float W = static_cast<float>(ext.width), H = static_cast<float>(ext.height);
    float s = std::max(1.f, std::round(H / 300.f));   // font scale
    glm::vec4 white(1.f), red(1.f, 0.25f, 0.2f, 1.f), dim(0.8f, 0.8f, 0.8f, 1.f), yellow(1.f, 0.9f, 0.3f, 1.f);
    float lh = (assets_.fontHeight + 4) * s;

    auto darkPanel = [&](float x, float y, float w, float h, float a) {
        const render::AtlasRegion& r = assets_.region(assets_.white);
        render::QuadInstance q;
        q.pos = glm::vec4(x, y, 0.f, 0.f);
        q.size = glm::vec4(w, h, 0.f, 1.f);
        q.uvRect = glm::vec4(r.u0, r.v0, r.u1, r.v1);
        q.color = glm::vec4(0.f, 0.f, 0.f, a);
        q.params = glm::vec4(1.f, 0.f, 0.f, 0.f);
        screenQuads_.push_back(q);
    };

    if (mode_ == Mode::Blocks || mode_ == Mode::Paused || mode_ == Mode::Alert || mode_ == Mode::GameOver || mode_ == Mode::Title) {
        float x = 24.f, y = 24.f;
        text(x, y, "SCORE", s, dim); y += lh;
        text(x, y, std::to_string(game_->score()), s, white); y += lh * 1.4f;
        text(x, y, "LEVEL " + std::to_string(game_->level()), s, dim); y += lh;
        text(x, y, "LINES " + std::to_string(game_->lines()), s, dim); y += lh;
        text(x, y, "RED LINES " + std::to_string(redLinesSurvived_), s, red); y += lh * 1.4f;
        // Next piece preview
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
        y += cellPx * 4.5f;
        text(W - 24.f, 24.f, "ARROWS/WASD MOVE  UP ROTATE  SPACE DROP", s * 0.6f, dim, 2);
        text(W - 24.f, 24.f + lh, "RED BLOCKS REFUSE TO CLEAR.", s * 0.6f, dim, 2);
        text(W - 24.f, 24.f + lh * 2.f, "A FULL RED ROW PULLS YOU IN.", s * 0.6f, dim, 2);
        text(W - 24.f, 24.f + lh * 3.f, "F12 SCREENSHOT  ESC PAUSE", s * 0.6f, dim, 2);
    }
    if (mode_ == Mode::Alert) {
        float f = 0.5f + 0.5f * std::sin(modeT_ * 16.f);
        text(W * 0.5f, H * 0.42f, "RED LINE", s * 2.2f, glm::vec4(1.f, 0.2f * f, 0.1f * f, 1.f), 1);
        text(W * 0.5f, H * 0.42f + lh * 2.4f, "THEY REFUSE TO DIE", s, white, 1);
    }
    if (mode_ == Mode::Fps || mode_ == Mode::FlyIn || mode_ == Mode::FlyOut) {
        if (mode_ == Mode::Fps) {
            // Weapon: bottom centre, bobbing with movement and recoiling when fired.
            bool flip = false;
            const std::string& gun = fps_.gunFiring() ? animFrame(assets_.gunFire, fps_.gunAnimT(), false, &flip) : animFrame(assets_.gunIdle, 0.f, true, &flip);
            float gs = H / 200.f;
            float bob = std::sin(time_ * 6.f) * 3.f * gs * ((fpsIn_.fwd || fpsIn_.back || fpsIn_.left || fpsIn_.right) ? 1.f : 0.15f);
            float recoil = fps_.recoil() * 18.f * gs;
            if (!gun.empty()) {
                const render::AtlasRegion& r = assets_.region(gun);
                // Doom weapon sprites are drawn with their origin relative to a 320x200 screen.
                float gx = W * 0.5f + (assets_.usingWad() ? (-r.offsetX + r.w * 0.5f) * gs * 0.f : 0.f);
                screenSprite(gun, gx, H + recoil + std::fabs(bob) - 2.f * gs, gs, glm::vec4(1.f), 0.5f, 0.f, flip);
                if (fps_.gunFiring() && fps_.gunAnimT() < 0.12f && !assets_.gunFlash.empty()) {
                    const std::string& fl = animFrame(assets_.gunFlash, fps_.gunAnimT(), false);
                    const render::AtlasRegion& fr = assets_.region(fl);
                    screenSprite(fl, gx, H + recoil - (r.h - fr.h) * gs - 6.f * gs, gs, glm::vec4(1.f), 0.5f, 0.f);
                }
            }
            screenSprite(assets_.crosshair, W * 0.5f, H * 0.5f, std::max(1.f, s * 0.7f), glm::vec4(1.f, 1.f, 1.f, 0.85f), 0.5f, 0.5f);
        }
        if (fps_.damageFlash() > 0.f) {
            const render::AtlasRegion& r = assets_.region(assets_.white);
            render::QuadInstance q;
            q.pos = glm::vec4(0.f, 0.f, 0.f, 0.f);
            q.size = glm::vec4(W, H, 0.f, 1.f);
            q.uvRect = glm::vec4(r.u0, r.v0, r.u1, r.v1);
            q.color = glm::vec4(1.f, 0.f, 0.f, 0.45f * fps_.damageFlash());
            q.params = glm::vec4(1.f, 0.f, 0.f, 0.f);
            screenQuads_.push_back(q);
        }
        darkPanel(0.f, H - lh * 1.6f, W, lh * 1.6f, 0.55f);
        float hy = H - lh * 1.3f;
        text(24.f, hy, "HEALTH " + std::to_string(static_cast<int>(std::ceil(fps_.health()))) + "%", s, fps_.health() < 30.f ? red : white);
        text(W * 0.5f, hy, "DEMONS " + std::to_string(fps_.enemiesLeft()) + "/" + std::to_string(fps_.totalEnemies()), s, yellow, 1);
        text(W - 24.f, hy, "SCORE " + std::to_string(game_->score()), s, white, 2);
        if (mode_ == Mode::Fps && fps_.elapsed() < 3.f) text(W * 0.5f, H * 0.3f, "MOUSE LOOK  WASD MOVE  CLICK FIRE", s * 0.8f, white, 1);
    }
    if (mode_ == Mode::Title) {
        darkPanel(0.f, 0.f, W, H, 0.55f);
        if (!assets_.title.empty()) screenSprite(assets_.title, W * 0.5f, H * 0.28f, s * 1.2f, glm::vec4(1.f, 0.6f, 0.6f, 1.f), 0.5f, 0.5f);
        float f = 0.6f + 0.4f * std::sin(time_ * 3.f);
        text(W * 0.5f, H * 0.42f, "REDLINE", s * 3.f, glm::vec4(1.f, 0.15f, 0.1f, 1.f), 1);
        text(W * 0.5f, H * 0.42f + lh * 3.4f, "STACK THE BLOCKS. SOME OF THEM ARE RED.", s, white, 1);
        text(W * 0.5f, H * 0.42f + lh * 4.6f, "RED BLOCKS REFUSE TO EXPLODE. WHEN A ROW IS ALL RED...", s, white, 1);
        text(W * 0.5f, H * 0.42f + lh * 6.6f, "PRESS ENTER", s * 1.2f, glm::vec4(1.f, 1.f, 1.f, f), 1);
        text(W * 0.5f, H - lh * 1.5f, assets_.usingWad() ? ("ASSETS: " + assets_.wadName()) : "ASSETS: PROCEDURAL (NO WAD FOUND)", s * 0.7f, dim, 1);
    }
    if (mode_ == Mode::Paused) {
        darkPanel(0.f, 0.f, W, H, 0.5f);
        text(W * 0.5f, H * 0.45f, "PAUSED", s * 2.f, white, 1);
        text(W * 0.5f, H * 0.45f + lh * 2.4f, "ESC TO RESUME", s, dim, 1);
    }
    if (mode_ == Mode::GameOver) {
        darkPanel(0.f, 0.f, W, H, std::min(0.6f, gameOverT_));
        text(W * 0.5f, H * 0.4f, fps_.playerDead() ? "YOU DIED" : "GAME OVER", s * 2.4f, red, 1);
        text(W * 0.5f, H * 0.4f + lh * 2.8f, "SCORE " + std::to_string(game_->score()) + "   BEST " + std::to_string(highScore_), s, white, 1);
        text(W * 0.5f, H * 0.4f + lh * 4.2f, "ENTER TO PLAY AGAIN", s, dim, 1);
    }
}

void App::buildScene() {
    cubes_.clear();
    worldQuads_.clear();
    screenQuads_.clear();
    cubes_.insert(cubes_.end(), envCubes_.begin(), envCubes_.end());

    Camera cam = currentCamera();
    VkExtent2D ext = renderer_->extent();
    float aspect = static_cast<float>(ext.width) / static_cast<float>(std::max(1u, ext.height));
    glm::vec3 eye = cam.eye;
    if (shakeT_ > 0.f) {
        float a = shakeT_ * 0.35f;
        eye += glm::vec3(std::sin(time_ * 90.f) * a, std::cos(time_ * 73.f) * a, 0.f);
    }
    frame_.view = glm::lookAt(eye, cam.target + (eye - cam.eye), glm::vec3(0.f, 1.f, 0.f));
    frame_.proj = glm::perspective(glm::radians(cam.fov), aspect, 0.05f, 120.f);
    frame_.cameraPos = eye;
    frame_.time = time_;
    frame_.sunDir = glm::normalize(glm::vec3(0.35f, 0.8f, 0.6f));
    bool inFps = (mode_ == Mode::Fps || mode_ == Mode::FlyIn || mode_ == Mode::FlyOut);
    frame_.sunIntensity = inFps ? 0.55f : 0.9f;
    frame_.ambient = inFps ? glm::vec3(0.16f, 0.13f, 0.13f) : glm::vec3(0.30f, 0.30f, 0.34f);
    frame_.fogDensity = inFps ? 0.035f : 0.012f;
    frame_.fogColor = inFps ? glm::vec3(0.06f, 0.02f, 0.02f) : glm::vec3(0.03f, 0.03f, 0.05f);
    frame_.clearColor = frame_.fogColor;
    if (mode_ == Mode::Alert) {
        float f = 0.5f + 0.5f * std::sin(modeT_ * 18.f);
        frame_.ambient = glm::mix(frame_.ambient, glm::vec3(0.45f, 0.08f, 0.05f), f * 0.7f);
    }

    addBoard();
    if (inFps || mode_ == Mode::GameOver || (mode_ == Mode::Paused && pausedFrom_ == Mode::Fps)) addFpsActors();
    addLights();
    addHud();
}

}  // namespace rl::game
