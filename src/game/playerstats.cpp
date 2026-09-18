#include "game/playerstats.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <random>

namespace rl::game {

namespace fs = std::filesystem;

std::string newUuid() {
    std::random_device rd;
    std::mt19937_64 gen(static_cast<uint64_t>(rd()) << 32 ^ rd());
    std::uniform_int_distribution<uint32_t> byte(0, 255);
    unsigned char b[16];
    for (auto& x : b) x = static_cast<unsigned char>(byte(gen));
    b[6] = static_cast<unsigned char>((b[6] & 0x0F) | 0x40);   // version 4
    b[8] = static_cast<unsigned char>((b[8] & 0x3F) | 0x80);   // variant
    char out[37];
    std::snprintf(out, sizeof out, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                  b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
    return out;
}

namespace {

std::string today() {
    std::time_t t = std::time(nullptr);
    char buf[16];
    if (std::strftime(buf, sizeof buf, "%Y-%m-%d", std::localtime(&t)) == 0) return "";
    return buf;
}

std::map<std::string, std::string> readKv(const fs::path& p) {
    std::map<std::string, std::string> kv;
    if (std::FILE* f = std::fopen(p.string().c_str(), "r")) {
        char buf[1024];
        while (std::fgets(buf, sizeof buf, f)) {
            std::string line = buf;
            while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
            size_t eq = line.find('=');
            if (eq != std::string::npos) kv[line.substr(0, eq)] = line.substr(eq + 1);
        }
        std::fclose(f);
    }
    return kv;
}

void readCounters(const std::map<std::string, std::string>& kv, StatCounters& c) {
    auto d = [&](const char* k) { auto it = kv.find(k); return it == kv.end() ? 0.0 : std::atof(it->second.c_str()); };
    auto i = [&](const char* k) { auto it = kv.find(k); return it == kv.end() ? 0LL : std::atoll(it->second.c_str()); };
    c.blocksSeconds = d("play_blocks");
    c.fpsSeconds = d("play_fps");
    c.menuSeconds = d("play_menu");
    c.keyboardActions = i("keyboard_actions");
    c.mouseActions = i("mouse_actions");
    c.padActions = i("pad_actions");
    c.sessions = static_cast<int>(i("sessions"));
}

void writeCounters(std::FILE* f, const StatCounters& c) {
    std::fprintf(f, "play_blocks=%.1f\nplay_fps=%.1f\nplay_menu=%.1f\nkeyboard_actions=%lld\nmouse_actions=%lld\npad_actions=%lld\nsessions=%d\n",
                 c.blocksSeconds, c.fpsSeconds, c.menuSeconds, static_cast<long long>(c.keyboardActions),
                 static_cast<long long>(c.mouseActions), static_cast<long long>(c.padActions), c.sessions);
}

}  // namespace

void PlayerStats::load(const std::string& profileDir, const MachineInfo& machine) {
    dir_ = profileDir;
    info_ = machine;
    lifetime_ = {};
    machine_ = {};
    std::error_code ec;
    fs::create_directories(fs::path(dir_) / "machines", ec);

    auto id = readKv(fs::path(dir_) / "identity.txt");
    playerId_ = id.count("player_id") ? id["player_id"] : "";
    if (playerId_.size() != 36) playerId_ = newUuid();
    email_ = id.count("email") ? id["email"] : "";
    emailVerified_ = id.count("email_verified") && id["email_verified"] == "1";
    token_ = id.count("token") ? id["token"] : "";

    auto st = readKv(fs::path(dir_) / "stats.txt");
    readCounters(st, lifetime_);
    firstPlayed_ = st.count("first_played") ? st["first_played"] : today();
    lastPlayed_ = today();

    auto mc = readKv(fs::path(dir_) / "machines" / (info_.installId + ".txt"));
    readCounters(mc, machine_);
    padModel_ = mc.count("pad_model") ? mc["pad_model"] : "";

    ++lifetime_.sessions;
    ++machine_.sessions;
    machineCount_ = 0;
    for (const auto& e : fs::directory_iterator(fs::path(dir_) / "machines", ec)) if (e.path().extension() == ".txt") ++machineCount_;
    if (machine_.sessions == 1) ++machineCount_;   // this one is new and not on disk yet
    save();
}

void PlayerStats::save() const {
    if (dir_.empty()) return;
    if (std::FILE* f = std::fopen((fs::path(dir_) / "identity.txt").string().c_str(), "w")) {
        std::fprintf(f, "player_id=%s\nemail=%s\nemail_verified=%d\ntoken=%s\n", playerId_.c_str(), email_.c_str(), emailVerified_ ? 1 : 0, token_.c_str());
        std::fclose(f);
    }
    if (std::FILE* f = std::fopen((fs::path(dir_) / "stats.txt").string().c_str(), "w")) {
        writeCounters(f, lifetime_);
        std::fprintf(f, "first_played=%s\nlast_played=%s\n", firstPlayed_.c_str(), lastPlayed_.c_str());
        std::fclose(f);
    }
    if (std::FILE* f = std::fopen((fs::path(dir_) / "machines" / (info_.installId + ".txt")).string().c_str(), "w")) {
        writeCounters(f, machine_);
        std::fprintf(f, "platform=%s\nos=%s\ngpu=%s\ncpu_cores=%d\nram_mb=%d\npad_model=%s\nlast_seen=%s\n",
                     info_.platform.c_str(), info_.os.c_str(), info_.gpu.c_str(), info_.cpuCores, info_.ramMb, padModel_.c_str(), lastPlayed_.c_str());
        std::fclose(f);
    }
}

void PlayerStats::addTime(PlayPhase phase, double seconds) {
    auto add = [&](StatCounters& c) {
        switch (phase) {
        case PlayPhase::Blocks: c.blocksSeconds += seconds; break;
        case PlayPhase::Fps: c.fpsSeconds += seconds; break;
        default: c.menuSeconds += seconds; break;
        }
    };
    add(lifetime_);
    add(machine_);
}

void PlayerStats::addInput(InputDevice device) {
    auto add = [&](StatCounters& c) {
        switch (device) {
        case InputDevice::Keyboard: ++c.keyboardActions; break;
        case InputDevice::Mouse: ++c.mouseActions; break;
        case InputDevice::Gamepad: ++c.padActions; break;
        }
    };
    add(lifetime_);
    add(machine_);
}

void PlayerStats::setPadModel(const std::string& model) { padModel_ = model; }

void PlayerStats::setEmail(const std::string& email) {
    if (email == email_) return;
    email_ = email;
    emailVerified_ = false;
        token_.clear();   // a new address has to be verified again (by the future account feature)
    save();
}

void PlayerStats::setVerified(const std::string& token) {
    token_ = token;
    emailVerified_ = !token.empty();
    save();
}

std::string PlayerStats::formatDuration(double seconds) {
    long long s = static_cast<long long>(seconds);
    long long h = s / 3600, m = (s / 60) % 60, sec = s % 60;
    char buf[32];
    if (h > 0) std::snprintf(buf, sizeof buf, "%lldH %02lldM", h, m);
    else std::snprintf(buf, sizeof buf, "%lldM %02lldS", m, sec);
    return buf;
}

}  // namespace rl::game
