#pragma once
// Player identity and play statistics, kept per profile on disk and never sent
// anywhere by this build. This is the groundwork for the online leaderboard
// (docs/online-and-releases.md): a stable random player id, an optional email
// that a future account feature will verify, and play time / input counters
// split per machine (a random install id per save folder).
//
// Files under profiles/<NAME>/:
//   identity.txt              player_id=<uuid>  email=<addr or empty>  email_verified=0|1
//   stats.txt                 lifetime totals
//   machines/<install_id>.txt the same counters for one machine, plus what the machine is
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace rl::game {

std::string newUuid();   // random version-4 UUID, lower case

struct MachineInfo {
    std::string installId;   // random per save folder
    std::string platform;    // "Linux", "Windows" ...
    std::string os;          // distro / version text when known
    std::string gpu;
    int cpuCores = 0;
    int ramMb = 0;
};

struct StatCounters {
    double blocksSeconds = 0, fpsSeconds = 0, menuSeconds = 0;
    int64_t keyboardActions = 0, mouseActions = 0, padActions = 0;
    int sessions = 0;
    double total() const { return blocksSeconds + fpsSeconds + menuSeconds; }
};

enum class PlayPhase { Menu, Blocks, Fps };
enum class InputDevice { Keyboard, Mouse, Gamepad };

class PlayerStats {
public:
    // Loads (or creates) the profile's identity and counters; starts a session
    // on `machine`. `padModel` is recorded when a gamepad is connected.
    void load(const std::string& profileDir, const MachineInfo& machine);
    void save() const;
    bool loaded() const { return !dir_.empty(); }

    void addTime(PlayPhase phase, double seconds);
    void addInput(InputDevice device);
    void setPadModel(const std::string& model);

    const std::string& playerId() const { return playerId_; }
    const std::string& email() const { return email_; }
    bool emailVerified() const { return emailVerified_; }
    void setEmail(const std::string& email);   // resets verification when it changes
    const std::string& token() const { return token_; }   // the online service's per-player secret, set by a confirmed registration
    void setVerified(const std::string& token);
    const std::string& pendingPoll() const { return pendingPoll_; }   // secret for polling a registration that is waiting for the email approval
    void setPendingPoll(const std::string& secret);

    const StatCounters& lifetime() const { return lifetime_; }
    const StatCounters& thisMachine() const { return machine_; }
    int machineCount() const { return machineCount_; }
    const MachineInfo& machineInfo() const { return info_; }

    static std::string formatDuration(double seconds);   // "1H 05M", "12M 03S"

private:
    std::string dir_;
    std::string playerId_, email_, token_, pendingPoll_;
    bool emailVerified_ = false;
    MachineInfo info_;
    std::string padModel_;
    StatCounters lifetime_, machine_;
    int machineCount_ = 0;
    std::string firstPlayed_, lastPlayed_;
};

}  // namespace rl::game
